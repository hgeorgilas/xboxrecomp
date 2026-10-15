/**
 * NV2A vertex programs as HLSL, so a GPU can run them.
 *
 * The interpreter (nv2a_vsh_interp.c) runs a program on the CPU one vertex
 * at a time, and in a 3D game that is most of the executor's time. This
 * turns the same program into straight-line HLSL instead: an NV2A vertex
 * program has no branches, so each instruction becomes one block of
 * statements, in order.
 *
 * API-neutral on purpose. The output is a body of plain HLSL statements that
 * any back end wraps in its own entry point -- D3D11 compiles it with
 * D3DCompile; a Vulkan back end would compile the same text to SPIR-V with
 * DXC. Nothing here knows which.
 *
 * The body expects these names, declared by the wrapper:
 *   float4 v[16];        inputs, filled in before the body runs
 *   float4 R[12];        temporaries, zeroed
 *   float4 oPos;         position output (R12 reads and writes it too)
 *   float4 o[13];        outputs by NV2A output address (3 = diffuse,
 *                        4 = specular, 5 = fog, 9..12 = texture 0..3)
 *   int a0;              address register
 *   float4 C[192];       constants
 *   float4 nv_rcc(float), nv_exp(float), nv_log(float), nv_lit(float4)
 *
 * Decoding is the interpreter's, field for field (the table at the top of
 * nv2a_vsh_interp.c), and so are the semantics: sources are read before
 * anything is written, an ILU op paired with a MAC op writes R1, a temp
 * index past 12 reads R0, ARL floors with the interpreter's small bias.
 * Writes to c[] are dropped: a program that may write constants
 * (PROGRAM_CXT_WRITE_EN) is never sent here, because its vertices depend on
 * each other.
 */
#include "nv2a_vsh_interp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

typedef struct { char *p; size_t n, cap; int overflow; } Out;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (o->overflow)
        return;
    va_start(ap, fmt);
    k = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= o->cap - o->n)
        o->overflow = 1;
    else
        o->n += (size_t)k;
}

/* One source operand, with swizzle and negation, as an expression. */
static void src(Out *o, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    put(o, neg ? "-" : "");
    if (mux == 1) {
        if (reg == 12)
            put(o, "oPos");
        else
            put(o, "R[%u]", reg < 12 ? reg : 0);
    } else if (mux == 2) {
        put(o, "v[%u]", field(ins, 1, 9, 4));
    } else if (field(ins, 3, 1, 1)) {
        put(o, "C[clamp(a0 + %u, 0, 191)]", field(ins, 1, 13, 8));
    } else {
        put(o, "C[%u]", field(ins, 1, 13, 8));
    }
    put(o, ".%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
        sw[(swz >> 2) & 3], sw[swz & 3]);
}

/* "dst.xz = val.xz;" for a write mask (x=8 y=4 z=2 w=1). */
static void masked(Out *o, const char *dst, const char *val, uint32_t mask)
{
    char m[5];
    int n = 0;
    if (!mask)
        return;
    if (mask & 8) m[n++] = 'x';
    if (mask & 4) m[n++] = 'y';
    if (mask & 2) m[n++] = 'z';
    if (mask & 1) m[n++] = 'w';
    m[n] = 0;
    put(o, "    %s.%s = %s.%s;\n", dst, m, val, m);
}

static const char *temp_name(uint32_t t, char *buf)
{
    if (t == 12)
        return "oPos";
    if (t > 12)
        return NULL;
    snprintf(buf, 8, "R[%u]", t);
    return buf;
}

static uint64_t hash_program(const uint32_t (*prog)[4], uint32_t start)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t s;
    int i;
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        for (i = 0; i < 4; i++)
            h = (h ^ prog[s][i]) * 1099511628211ull;
        if (field(prog[s], 3, 0, 1))
            break;
    }
    return h;
}

uint64_t nv2a_vsh_program_hash(void)
{
    uint32_t start;
    const uint32_t (*prog)[4] = nv2a_vsh_program(&start);
    return hash_program(prog, start);
}

size_t nv2a_vsh_hlsl(char *buf, size_t cap, uint64_t *hash)
{
    uint32_t start, s;
    const uint32_t (*prog)[4] = nv2a_vsh_program(&start);
    Out o = { buf, 0, cap, 0 };
    int final = 0;

    if (hash)
        *hash = hash_program(prog, start);
    for (s = start; s < NV2A_VSH_SLOTS && !final; s++) {
        const uint32_t *ins = prog[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        char tb[8];
        const char *t;

        final = (int)field(ins, 3, 0, 1);
        if (mac == 14 || mac == 15)
            return 0;                          /* not an NV2A op we know */

        put(&o, "  { float4 A = "); src(&o, ins, 0);
        put(&o, "; float4 B = ");   src(&o, ins, 1);
        put(&o, "; float4 Cc = ");  src(&o, ins, 2);
        put(&o, ";\n    float4 m = A, i = Cc;\n");

        switch (mac) {
        case 2:  put(&o, "    m = A * B;\n"); break;
        case 3:  put(&o, "    m = A + Cc;\n"); break;
        case 4:  put(&o, "    m = A * B + Cc;\n"); break;
        case 5:  put(&o, "    m = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6:  put(&o, "    m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7:  put(&o, "    m = dot(A, B).xxxx;\n"); break;
        case 8:  put(&o, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  put(&o, "    m = float4(A.x < B.x ? A.x : B.x, A.y < B.y ? A.y : B.y,"
                         " A.z < B.z ? A.z : B.z, A.w < B.w ? A.w : B.w);\n"); break;
        case 10: put(&o, "    m = float4(A.x >= B.x ? A.x : B.x, A.y >= B.y ? A.y : B.y,"
                         " A.z >= B.z ? A.z : B.z, A.w >= B.w ? A.w : B.w);\n"); break;
        case 11: put(&o, "    m = float4(A < B);\n"); break;
        case 12: put(&o, "    m = float4(A >= B);\n"); break;
        default: break;                        /* 0 nop, 1 mov, 13 arl */
        }
        switch (ilu) {
        case 2: put(&o, "    i = (1.0 / Cc.x).xxxx;\n"); break;
        case 3: put(&o, "    i = nv_rcc(Cc.x);\n"); break;
        case 4: put(&o, "    i = rsqrt(abs(Cc.x)).xxxx;\n"); break;
        case 5: put(&o, "    i = nv_exp(Cc.x);\n"); break;
        case 6: put(&o, "    i = nv_log(Cc.x);\n"); break;
        case 7: put(&o, "    i = nv_lit(Cc);\n"); break;
        default: break;                        /* 0 nop, 1 mov */
        }
        if (mac == 13)
            put(&o, "    a0 = (int)floor(A.x + 0.001);\n");

        /* Writes, in the interpreter's order. */
        if (mac && mac != 13 && mac_mask && (t = temp_name(tdst, tb)) != NULL)
            masked(&o, t, "m", mac_mask);
        if (ilu && ilu_mask && (t = temp_name(mac ? 1u : tdst, tb)) != NULL)
            masked(&o, t, "i", ilu_mask);
        if (omask && field(ins, 3, 11, 1)) {
            const char *val = field(ins, 3, 2, 1) ? "i" : "m";
            if (oaddr == 0) {
                masked(&o, "oPos", val, omask);
            } else if (oaddr < 13) {
                char d[8];
                snprintf(d, sizeof d, "o[%u]", oaddr);
                masked(&o, d, val, omask);
                if (oaddr == 5) {
                    /* oFog: the hardware puts the first component the mask
                     * writes into x, which the fog unit reads (xemu's
                     * fog_mask_str). Spartan writes its fog distance as
                     * oFog.z; read from x as written, the distance stayed 0
                     * and nothing was ever fogged. */
                    char c = (omask & 8) ? 'x' : (omask & 4) ? 'y' : (omask & 2) ? 'z' : 'w';
                    put(&o, "    o[5].x = %s.%c;\n", val, c);
                }
            }
        }
        put(&o, "  }\n");
    }
    if (!final || o.overflow)
        return 0;                              /* no FINAL: no program */
    return o.n;
}
