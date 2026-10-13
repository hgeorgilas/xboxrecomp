/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifndef XBOX_CONTIG_SIZE
#define XBOX_CONTIG_SIZE (64u * 1024u * 1024u)   /* as kernel.h */
#endif
#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

static void note(uint32_t subch, uint32_t method)
{
    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].method == method && s_seen[i].subch == subch) {
            s_seen[i].count++;
            return;
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;
    const uint32_t lo = XBOX_CONTIG_BASE, hi = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;

    /* Stay inside the contiguous window. A CALL is followed for up to 4 MB
     * looking for its RETURN, and a stale or overwritten target has none:
     * the walk ran off the end of guest RAM and faulted the executor
     * (Spartan, loading its first level). */
    if (end_va > hi || end_va < va)
        end_va = hi;
    while (va < end_va && words < 0x100000u) {
        uint32_t w;
        if (va < lo || va + 4 > hi) {
            static int said;
            if (!said++)
                fprintf(stderr, "  [PB] walk left the contiguous window at "
                                "0x%08X; segment abandoned\n", va);
            break;
        }
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            if (end_va > hi || end_va < va)
                end_va = hi;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    static int scan = -1;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* Run the pushbuffer the way the DMA engine does: from GET, following every
 * JUMP, CALL and RETURN, until GET reaches PUT. Takes and returns physical
 * addresses, like the registers.
 *
 * Walking [last PUT, PUT) as a flat range was an approximation of this that
 * broke wherever the stream does not run in address order: a JUMP ended the
 * segment (the ring's wrap, and any jump the title placed mid-ring), and
 * whatever came after it -- fence releases included -- was never executed.
 * A title blocking on one of those fences then waited forever (Spartan, a
 * few minutes into its first level).
 *
 * If the stream never reaches PUT (a corrupt or stale word sent it astray)
 * it is abandoned at PUT after a bound, which is what the flat walk did for
 * every segment anyway. */
static struct { uint32_t g, p, words; unsigned long long t; } s_rl[8];
static unsigned s_rl_n;

uint32_t nv2a_pb_run(uint32_t get, uint32_t put, volatile uint32_t *get_reg)
{
    /* GET is published as the walk goes, as the hardware's is. The title
     * reads it to decide how much of the ring it may overwrite, and a GET
     * that only moved once a whole run was done -- a run can be most of a
     * lap -- let it wrap round and overwrite commands still waiting to be
     * read (XDK D3D waits on a fence, which the executor had passed, then
     * checks GET, which still said where the run started).
     *
     * Only while inside the ring: in a pushbuffer the title JUMPed or CALLed
     * to, the last ring position stays, which is behind and so safe. The
     * ring is the range PUT has been seen in. */
    static uint32_t ring_lo = 0xFFFFFFFFu, ring_hi;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t ret = 0, words = 0, start = get;
    int in_call = 0;
    /* The last few control transfers, for when the stream is lost. */
    struct { uint32_t at, w; } hist[16];
    unsigned nh = 0;

    static int scan = -1;
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    get &= 0x0FFFFFFCu;
    put &= 0x0FFFFFFCu;
    if (put < ring_lo) ring_lo = put;
    if (put > ring_hi) ring_hi = put;
    while (get != put) {
        uint32_t w;
        if (get + 4 > XBOX_CONTIG_SIZE || ++words > 0x400000u) {
            static int said;
            if (said++ < 4) {
                extern uint32_t xbox_Nv2aDevice(void);
                uint32_t dev = xbox_Nv2aDevice();
                if (dev)
                    fprintf(stderr, "  [PB] ring 0x%08X..0x%08X lap %u put 0x%08X"
                            " issued %u completed %u\n",
                            *(const uint32_t *)(mem + dev + 0x24),
                            *(const uint32_t *)(mem + dev + 0x28),
                            *(const uint32_t *)(mem + dev + 0x40),
                            *(const uint32_t *)(mem + dev),
                            *(const uint32_t *)(mem + dev + 0x2C),
                            *(const uint32_t *)(mem + 0x80002000u));
                fprintf(stderr, "  [PB] previous runs:\n");
                for (unsigned k = 0; k < 8; k++) {
                    unsigned j = (s_rl_n + k) & 7;
                    fprintf(stderr, "  [PB]   0x%08X -> 0x%08X  %u words  t=%llu ms\n",
                            s_rl[j].g, s_rl[j].p, s_rl[j].words,
                            (unsigned long long)s_rl[j].t);
                }
                fprintf(stderr, "  [PB] stream lost at 0x%08X (%u words, from"
                                " 0x%08X); resyncing to PUT 0x%08X\n",
                        get, words, start, put);
                if (nh >= 2) {
                    uint32_t at = hist[(nh - 1) & 15].at & ~0x1Fu;
                    for (uint32_t a = at - 0x40; a < at + 0x40; a += 16)
                        fprintf(stderr, "  [PB]   %08X: %08X %08X %08X %08X%c", a,
                            *(const uint32_t *)(mem + XBOX_CONTIG_BASE + a),
                            *(const uint32_t *)(mem + XBOX_CONTIG_BASE + a + 4),
                            *(const uint32_t *)(mem + XBOX_CONTIG_BASE + a + 8),
                            *(const uint32_t *)(mem + XBOX_CONTIG_BASE + a + 12), 10);
                }
                for (unsigned k = nh > 16 ? nh - 16 : 0; k < nh; k++)
                    fprintf(stderr, "  [PB]   0x%08X: %08X\n",
                            hist[k & 15].at, hist[k & 15].w);
            }
            return put;
        }
        w = *(const uint32_t *)(mem + XBOX_CONTIG_BASE + get);
        if ((w & 3u) || (w & 0xE0000003u) == 0x20000000u
                     || (w & 0xFFFF0003u) == 0x00020000u) {
            hist[nh & 15].at = get; hist[nh & 15].w = w; nh++;
        }
        get += 4;

        if ((w & 3u) == 1u) {                         /* JUMP */
            get = w & 0x0FFFFFFCu;
            s_tot_jumps++;
            continue;
        }
        if ((w & 0xE0000003u) == 0x20000000u) {       /* old-style JUMP */
            get = w & 0x0FFFFFFCu;
            s_tot_jumps++;
            continue;
        }
        if ((w & 3u) == 2u) {                         /* CALL */
            if (!in_call) {
                ret = get;
                in_call = 1;
                s_tot_calls++;
            }
            get = w & 0x0FFFFFFCu;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {       /* RETURN */
            if (in_call) {
                get = ret;
                in_call = 0;
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;
            if (get_reg && get - 4 >= ring_lo && get - 4 <= ring_hi)
                *get_reg = get - 4;          /* this command, not yet run */
            for (uint32_t i = 0; i < count; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                if (get + 4 > XBOX_CONTIG_SIZE)
                    break;
                if (scan)
                    note(subch, m);
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                        *(const uint32_t *)(mem + XBOX_CONTIG_BASE + get));
                get += 4;
                words++;
            }
            continue;
        }
        s_tot_unknown++;
    }
    s_tot_words += words;
    s_tot_segments++;
    s_rl[s_rl_n & 7].g = start; s_rl[s_rl_n & 7].p = put;
    s_rl[s_rl_n & 7].words = words; s_rl[s_rl_n & 7].t = GetTickCount64();
    s_rl_n++;
    return get;
}
