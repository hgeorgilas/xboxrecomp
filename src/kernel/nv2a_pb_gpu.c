/**
 * D3D11 back end for the pushbuffer executor. See nv2a_pb_gpu.h.
 *
 * Everything here mirrors a rule the software rasteriser in nv2a_pb_exec.c
 * already gets right; where the two could differ, the comment names the
 * function in that file this follows.
 */
#include "nv2a_pb_gpu.h"
#include "nv2a_vsh_interp.h"

#if defined(_WIN32)

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static CRITICAL_SECTION     s_lock;
static pD3DCompile          s_compile;
static uint32_t             s_flip;        /* flips seen, for ageing */

/* Settings (xbox_settings.c): read live, re-read when the generation moves. */
extern long xbox_settings_gen(void);
extern int  xbox_setting_int(const char *key, int def);

/* ---- small helpers ----------------------------------------------------- */

/* Guest addresses arrive through the contiguous (0x80000000) or tiled
 * (0xF0000000) mirrors as well as directly; key everything on physical. */
static uint32_t phys(uint32_t a) { return a & 0x0FFFFFFFu; }

static uint8_t *guest(uint32_t a)
{
    return (uint8_t *)xbox_GetMemoryOffset() + a;
}

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint32_t *w = (const uint32_t *)p;
    size_t i, nw = n / 4;
    for (i = 0; i < nw; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    return h;
}

static ID3DBlob *compile(const char *src, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    if (!s_compile)
        return NULL;
    hr = s_compile(src, strlen(src), NULL, NULL, NULL, "main", target,
                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [PBGPU] shader compile failed: %s\n%s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        fflush(stderr);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* ---- fixed pipeline objects -------------------------------------------- */

static ID3D11VertexShader *s_vs;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer       *s_vb, *s_vs_cb, *s_ps_cb;
static uint32_t            s_vb_pos;
static ID3D11RasterizerState *s_rs;
static ID3D11Texture2D       *s_white_tex;
static ID3D11ShaderResourceView *s_white;
static ID3D11PixelShader  *s_clear_ps;
static ID3D11VertexShader *s_clear_vs;

#define VB_BYTES (16u << 20)

/* Back to clip space: the executor's vertices are already divided by w and
 * viewport-mapped (the XDK epilogue: c[58] scale, c[59] offset), so undo the
 * divide with w and map the surface's pixel grid onto NDC. Pixel centres sit
 * at +0.5 in both conventions (xf_rows samples px + 0.5). */
static const char k_vs[] =
"cbuffer V : register(b0) { float4 vp; };\n"
"struct VI { float4 p:POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
"  float4 t3:TEXCOORD3; float f:TEXCOORD4; };\n"
"struct VO { float4 p:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
"  float4 t3:TEXCOORD3; float f:TEXCOORD4; };\n"
"VO main(VI i) {\n"
/* A screen-space pass can carry w = inf (Spartan's depth-of-field quad:
 * a reciprocal of 0). Rebuilding clip space as screen * w then makes inf/inf
 * = NaN and the quad rasterised into one corner. Equal w at every vertex
 * changes nothing about the result, so any finite stand-in is exact. */
"  VO o; float w = i.p.w; if (!(w > 0 && w < 1e30)) w = 1;\n"
"  float x = i.p.x * vp.x - 1.0, y = 1.0 - i.p.y * vp.y, z = i.p.z * vp.z;\n"
"  o.p = float4(x * w, y * w, z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2;\n"
"  o.t3 = i.t3; o.f = i.f; return o; }\n";

static const char k_clear_vs[] =
"float4 main(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);\n"
"  return float4(p, 0, 1); }\n";
static const char k_clear_ps[] =
"cbuffer P : register(b0) { float4 c; };\n"
"float4 main() : SV_Target { return c; }\n";

typedef struct {
    float fac0[8][4], fac1[8][4];
    float fc0[4], fc1[4], fogc[4];
    float texsize[4][4];          /* 1/w, 1/h for texel-coordinate stages */
    float misc[4];                /* x: alpha reference 0..255 */
    float bump[4][4];             /* per stage: BUMP_ENV_MAT 00 01 10 11 */
    float bumpls[4][4];           /* per stage: x luminance scale, y offset */
} PsConsts;

static int init_device(void)
{
    static const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0 };
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    D3D11_INPUT_ELEMENT_DESC ie[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT,          0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *b;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    uint32_t white = 0xFFFFFFFFu;
    HRESULT hr;

    if (dc)
        s_compile = (pD3DCompile)GetProcAddress(dc, "D3DCompile");
    if (!s_compile) {
        fprintf(stderr, "  [PBGPU] d3dcompiler_47.dll not available\n");
        return 0;
    }
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 1,
                           D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [PBGPU] D3D11CreateDevice failed 0x%08lX\n", hr);
        return 0;
    }

    if (!(b = compile(k_vs, "vs_5_0")))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &s_vs);
    ID3D11Device_CreateInputLayout(s_dev, ie, 8, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), &s_layout);
    ID3D10Blob_Release(b);
    if (!(b = compile(k_clear_vs, "vs_5_0")))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &s_clear_vs);
    ID3D10Blob_Release(b);
    if (!(b = compile(k_clear_ps, "ps_5_0")))
        return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &s_clear_ps);
    ID3D10Blob_Release(b);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb);
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vs_cb);
    bd.ByteWidth = (sizeof(PsConsts) + 15) & ~15u;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ps_cb);

    /* No culling: the software rasteriser draws both windings (raster_triangle
     * and xf_rows accept either sign). No depth clip: the NV2A clamps. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);

    /* An unusable stage reads white (rc_texel). */
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &white; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 4;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &s_white_tex);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_white_tex,
                                          NULL, &s_white);
    fprintf(stderr, "  [PBGPU] D3D11 back end up\n");
    fflush(stderr);
    return 1;
}

int pbgpu_enabled(void)
{
    static int state = -1;
    if (state < 0) {
        state = 0;
        if (getenv("RECOMP_PB_GPU")) {
            InitializeCriticalSection(&s_lock);
            state = init_device();
        }
    }
    return state;
}

/* ---- colour and depth surfaces ----------------------------------------- */

typedef struct {
    uint32_t key, addr, pitch, bpp, w, h;   /* the guest's (logical) size */
    uint32_t pw, ph;                        /* the texture's: logical x scale */
    float sx, sy;                           /* that scale */
    ID3D11Texture2D *tex, *staging, *down;  /* staging and down: logical size */
    ID3D11RenderTargetView *rtv, *down_rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t drawn_flip;           /* last flip it was drawn in */
    int drawn;                     /* drawn since the last readback */
} Surface;

typedef struct {
    uint32_t key, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} Zeta;

#define MAX_SURF 32
#define MAX_ZETA 8
static Surface s_surf[MAX_SURF];
static Zeta    s_zeta[MAX_ZETA];
static int     s_surf_next, s_zeta_next;

static void surf_release(Surface *s)
{
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    if (s->staging) ID3D11Texture2D_Release(s->staging);
    if (s->down_rtv) ID3D11RenderTargetView_Release(s->down_rtv);
    if (s->down) ID3D11Texture2D_Release(s->down);
    memset(s, 0, sizeof *s);
}

/* Internal resolution. Render targets are drawn at their guest size times
 * RECOMP_RES_SCALE -- and, with a display aspect wider than the buffer's,
 * wider still, so the stretched picture has square pixels. The title never
 * sees it: positions reach the viewport in normalised form, texture
 * coordinates are fractions, and what goes back to guest memory is filtered
 * down to the guest size (surf_logical). A change re-creates the surfaces. */
extern float xbox_DisplayAspect(void);
extern float xbox_setting_float(const char *key, float def);
static float s_res_sx = 1, s_res_sy = 1;

static void res_update(void)
{
    static long gen;
    float sc, a, sx, sy;
    int i;
    if (gen == xbox_settings_gen())
        return;
    gen = xbox_settings_gen();
    sc = xbox_setting_float("RECOMP_RES_SCALE", 1.0f);
    sc = sc < 1 ? 1 : sc > 6 ? 6 : sc;
    a = xbox_DisplayAspect();
    sx = sc * (a > 4.0f / 3.0f ? a * 0.75f : 1.0f);
    sy = sc;
    if (sx == s_res_sx && sy == s_res_sy)
        return;
    s_res_sx = sx;
    s_res_sy = sy;
    for (i = 0; i < MAX_SURF; i++)
        if (s_surf[i].tex)
            surf_release(&s_surf[i]);
    for (i = 0; i < MAX_ZETA; i++)
        if (s_zeta[i].tex) {
            ID3D11DepthStencilView_Release(s_zeta[i].dsv);
            ID3D11Texture2D_Release(s_zeta[i].tex);
            memset(&s_zeta[i], 0, sizeof s_zeta[i]);
        }
    fprintf(stderr, "  [PBGPU] internal resolution x%.2f (%.0fx%.0f for 640x480)\n",
            sc, 640 * sx, 480 * sy);
}

static ID3D11Texture2D *surf_logical(Surface *s);

/* Guest pixels (32 or 16 bpp) to B8G8R8A8 rows, and back. */
static void to_bgra(const uint8_t *src, uint32_t bpp, uint32_t w, uint32_t *dst)
{
    uint32_t x;
    if (bpp == 4) {
        memcpy(dst, src, (size_t)w * 4);
        return;
    }
    for (x = 0; x < w; x++) {
        uint32_t t = ((const uint16_t *)src)[x];
        dst[x] = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
               | ((t & 0x001Fu) << 3);
    }
}

static void from_bgra(const uint32_t *src, uint32_t bpp, uint32_t w, uint8_t *dst)
{
    uint32_t x;
    if (bpp == 4) {
        memcpy(dst, src, (size_t)w * 4);
        return;
    }
    for (x = 0; x < w; x++) {
        uint32_t c = src[x];
        ((uint16_t *)dst)[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                        | ((c >> 3) & 0x001F));
    }
}

static Surface *surface_get(uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t need_h)
{
    uint32_t key = phys(addr), w = bpp ? pitch / bpp : 0, h, y;
    D3D11_TEXTURE2D_DESC td;
    Surface *s = NULL;
    uint32_t *row;
    int i;

    if (!w || !need_h)
        return NULL;
    for (i = 0; i < MAX_SURF; i++)
        if (s_surf[i].tex && s_surf[i].key == key) {
            s = &s_surf[i];
            if (s->pitch == pitch && s->bpp == bpp && s->h >= need_h)
                return s;
            surf_release(s);       /* reshaped: start again from guest memory */
            break;
        }
    if (!s) {
        for (i = 0; i < MAX_SURF && s_surf[i].tex; i++)
            ;
        if (i == MAX_SURF) {
            i = s_surf_next++ % MAX_SURF;
            surf_release(&s_surf[i]);
        }
        s = &s_surf[i];
    }
    h = need_h < 480 ? 480 : need_h;
    s->sx = s_res_sx;
    s->sy = s_res_sy;
    s->pw = (uint32_t)((float)w * s->sx + 0.5f);
    s->ph = (uint32_t)((float)h * s->sy + 0.5f);
    memset(&td, 0, sizeof td);
    td.Width = s->pw; td.Height = s->ph;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->tex)))
        return NULL;
    td.Width = w; td.Height = h;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->staging);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    s->key = key; s->addr = addr; s->pitch = pitch; s->bpp = bpp;
    s->w = w; s->h = h;

    /* Start from what is in guest memory: a title may have written the
     * surface with the CPU, and a first draw that only covers part of it
     * must not leave the rest black. */
    row = (uint32_t *)malloc((size_t)w * h * 4);
    if (row) {
        for (y = 0; y < h; y++)
            to_bgra(guest(addr) + (size_t)y * pitch, bpp, w, row + (size_t)y * w);
        if (s->pw == w && s->ph == h) {
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s->tex, 0,
                                                  NULL, row, w * 4, 0);
        } else {
            /* Scaled: the guest's pixels, each repeated over its area. */
            uint32_t *big = (uint32_t *)malloc((size_t)s->pw * s->ph * 4), x;
            if (big) {
                for (y = 0; y < s->ph; y++) {
                    const uint32_t *src = row + (size_t)(y * h / s->ph) * w;
                    uint32_t *dst = big + (size_t)y * s->pw;
                    for (x = 0; x < s->pw; x++)
                        dst[x] = src[x * w / s->pw];
                }
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s->tex, 0,
                                                      NULL, big, s->pw * 4, 0);
                free(big);
            }
        }
        free(row);
    }
    return s;
}

static Surface *surface_find(uint32_t addr)
{
    uint32_t key = phys(addr);
    int i;
    for (i = 0; i < MAX_SURF; i++)
        if (s_surf[i].tex && s_surf[i].key == key)
            return &s_surf[i];
    return NULL;
}

static Zeta *zeta_get(uint32_t addr, uint32_t w, uint32_t h)
{
    uint32_t key = phys(addr);
    D3D11_TEXTURE2D_DESC td;
    Zeta *z = NULL;
    int i;

    for (i = 0; i < MAX_ZETA; i++)
        if (s_zeta[i].tex && s_zeta[i].key == key) {
            z = &s_zeta[i];
            if (z->w == w && z->h == h)
                return z;
            ID3D11DepthStencilView_Release(z->dsv);
            ID3D11Texture2D_Release(z->tex);
            memset(z, 0, sizeof *z);
            break;
        }
    if (!z) {
        for (i = 0; i < MAX_ZETA && s_zeta[i].tex; i++)
            ;
        if (i == MAX_ZETA) {
            i = s_zeta_next++ % MAX_ZETA;
            ID3D11DepthStencilView_Release(s_zeta[i].dsv);
            ID3D11Texture2D_Release(s_zeta[i].tex);
            memset(&s_zeta[i], 0, sizeof s_zeta[i]);
        }
        z = &s_zeta[i];
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &z->tex)))
        return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex, NULL, &z->dsv);
    /* The software depth buffer starts at "infinitely far" (zbuf_current). */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    z->key = key; z->w = w; z->h = h;
    return z;
}

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t addr, color, w, h, pitch, palette, cube, bytes;
    uint64_t hash;
    uint32_t checked_flip, used_flip;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint64_t pack_key;             /* texture-pack name (tex_pack.c), 0: none */
    uint32_t changes;              /* re-uploads with new contents */
    int pack_pending;              /* a replacement is being decoded */
} TexEntry;

/* tex_pack.c */
extern uint64_t texpack_key(const uint32_t *px, uint32_t w, uint32_t h, uint32_t fmt);
extern void texpack_dump(uint64_t key, const uint32_t *px, uint32_t w, uint32_t h, uint32_t fmt);
extern int  texpack_lookup(uint64_t key, uint32_t **px, uint32_t *w, uint32_t *h);
extern void texpack_free(uint32_t *px);

#define MAX_TEX 512
static TexEntry s_tex[MAX_TEX];

static uint64_t tex_hash(const PbGpuTexture *t)
{
    uint64_t h = 14695981039346656037ull;
    if (t->bytes && t->bytes < (64u << 20))
        h = fnv64(guest(t->addr), t->bytes, h);
    if (t->palette)
        h = fnv64(guest(t->palette), 1024, h);
    return h;
}

static int tex_create(TexEntry *e, const uint32_t *px, uint32_t w, uint32_t h,
                      uint32_t faces, int mips, int cube);

static int tex_upload(TexEntry *e, const PbGpuTexture *t)
{
    uint32_t faces = t->cube ? 6 : 1, f, u, v, w = t->width, h = t->height;
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * faces * 4);

    if (!px)
        return 0;
    for (f = 0; f < faces; f++)
        for (v = 0; v < h; v++)
            for (u = 0; u < w; u++) {
                uint32_t c;
                if (!t->fetch(t->src, f * t->face_stride, u, v, &c))
                    c = 0xFFFFFFFFu;
                if (t->no_alpha)
                    c |= 0xFF000000u;
                px[((size_t)f * h + v) * w + u] = c;
            }
    {
        /* RECOMP_TEX_DUMP=<dir>: each uploaded texture of a format named in
         * RECOMP_TEX_DUMP_FMT (hex), as decoded, once per address. */
        const char *dir = getenv("RECOMP_TEX_DUMP"), *fm = getenv("RECOMP_TEX_DUMP_FMT");
        if (dir && fm && strtoul(fm, NULL, 16) == t->color && !t->cube) {
            char path[512];
            FILE *fp;
            uint32_t hdr[13];
            snprintf(path, sizeof path, "%stex_%08X_%02X.bmp", dir, t->addr, t->color);
            if ((fp = fopen(path, "wb")) != NULL) {
                memset(hdr, 0, sizeof hdr);
                fwrite("BM", 1, 2, fp);
                hdr[0] = 54 + w * h * 4; hdr[2] = 54; hdr[3] = 40;
                hdr[4] = w; hdr[5] = (uint32_t)-(int32_t)h; hdr[6] = 1 | (32 << 16);
                fwrite(hdr, 4, 13, fp);
                fwrite(px, 4, (size_t)w * h, fp);
                fclose(fp);
            }
        }
    }
    /* Texture packs: name it, dump it, and use a replacement if there is
     * one. Not for cube maps, specks, or a texture whose contents keep
     * changing (a movie, an animated surface): those would flood the dump
     * and no pack replaces them. */
    e->pack_key = 0;
    e->pack_pending = 0;
    /* R8B8/G8B8 are data, not pictures -- Spartan's colour grade is an R8B8
     * table read through BUMPENVMAP -- and are never named or replaced. */
    if (!t->cube && w * h >= 64 && e->changes < 4 && t->color != 0x16
            && t->color != 0x17 && t->color != 0x28 && t->color != 0x29) {
        uint32_t *rpx = NULL, rw = 0, rh = 0;
        int r;
        e->pack_key = texpack_key(px, w, h, t->color);
        texpack_dump(e->pack_key, px, w, h, t->color);
        r = texpack_lookup(e->pack_key, &rpx, &rw, &rh);
        if (r == 1) {
            int ok = tex_create(e, rpx, rw, rh, 1, t->normalized, 0);
            texpack_free(rpx);
            free(px);
            return ok;
        }
        e->pack_pending = r < 0;
    }
    {
        int ok = tex_create(e, px, w, h, faces,
                            t->levels > 1 && !t->cube && t->normalized, t->cube);
        free(px);
        return ok;
    }
}

/* A replacement decoded since the texture went up with its original. */
static void tex_pack_poll(TexEntry *e)
{
    uint32_t *rpx = NULL, rw = 0, rh = 0;
    int r = texpack_lookup(e->pack_key, &rpx, &rw, &rh);
    if (r == 1) {
        tex_create(e, rpx, rw, rh, 1, 1, 0);
        texpack_free(rpx);
    }
    e->pack_pending = r < 0;
}

/* The D3D texture for decoded pixels; mips: a full chain generated on the
 * GPU from level 0. */
static int tex_create(TexEntry *e, const uint32_t *px, uint32_t w, uint32_t h,
                      uint32_t faces, int mips, int cube)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[6];
    uint32_t f;

    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    e->srv = NULL; e->tex = NULL;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = faces;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (f = 0; f < faces; f++) {
        sd[f].pSysMem = px + (size_t)f * w * h;
        sd[f].SysMemPitch = w * 4;
        sd[f].SysMemSlicePitch = w * h * 4;
    }
    if (mips) {
        /* A mipmapped texture gets a full mip chain, generated on the GPU
         * from level 0. Uploading level 0 alone left minified textures
         * sampled at full resolution -- the ground and walls shimmered as
         * the camera moved. The title's own levels sit in guest memory
         * after level 0; generated ones are the same box filter in
         * practice and need no per-format decode. */
        td.MipLevels = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex))) {
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0,
                                                  NULL, px, w * 4, 0);
            if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev,
                              (ID3D11Resource *)e->tex, NULL, &e->srv)))
                ID3D11DeviceContext_GenerateMips(s_ctx, e->srv);
        }
    } else if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &e->tex)))
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex,
                                              NULL, &e->srv);
    return e->srv != NULL;
}

/* A copy of the surface being drawn into, for a pass that samples its own
 * target: D3D cannot read and write one resource in the same draw. */
static ID3D11Texture2D          *s_copy;
static ID3D11ShaderResourceView *s_copy_srv;

static ID3D11ShaderResourceView *self_copy(const Surface *rt)
{
    D3D11_TEXTURE2D_DESC want, have;
    ID3D11Texture2D_GetDesc(rt->tex, &want);
    want.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (s_copy) {
        ID3D11Texture2D_GetDesc(s_copy, &have);
        if (have.Width != want.Width || have.Height != want.Height) {
            ID3D11ShaderResourceView_Release(s_copy_srv);
            ID3D11Texture2D_Release(s_copy);
            s_copy = NULL; s_copy_srv = NULL;
        }
    }
    if (!s_copy) {
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &want, NULL, &s_copy)))
            return s_white;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_copy,
                                              NULL, &s_copy_srv);
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_copy,
                                     (ID3D11Resource *)rt->tex);
    return s_copy_srv ? s_copy_srv : s_white;
}

static ID3D11ShaderResourceView *tex_get(const PbGpuTexture *t, Surface *target)
{
    TexEntry *e = NULL, *oldest = NULL;
    Surface *rt;
    int i;

    if (!t->valid || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return s_white;

    /* A surface drawn in the last few frames is read on the GPU: that is a
     * render-to-texture pass (glow, blur, composite), and its guest memory is
     * only written back at flips. Older aliases are stale -- the memory has
     * likely been reused for an ordinary texture. */
    rt = !t->cube ? surface_find(t->addr) : NULL;
    if (rt && rt->drawn_flip + 4 >= s_flip)
        return rt != target ? rt->srv : self_copy(rt);

    for (i = 0; i < MAX_TEX; i++) {
        TexEntry *c = &s_tex[i];
        if (c->srv && c->addr == t->addr && c->color == t->color && c->w == t->width
            && c->h == t->height && c->pitch == t->pitch && c->palette == t->palette
            && c->cube == (uint32_t)t->cube) {
            e = c;
            break;
        }
        if (!oldest || c->used_flip < oldest->used_flip || !c->srv)
            if (!oldest || oldest->srv)
                oldest = c;
    }
    if (e) {
        if (e->checked_flip != s_flip) {
            uint64_t h = tex_hash(t);
            e->checked_flip = s_flip;
            if (h != e->hash) {
                e->hash = h;
                e->changes++;
                tex_upload(e, t);
            } else if (e->pack_pending) {
                tex_pack_poll(e);
            }
        }
    } else {
        e = oldest;
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
        e->addr = t->addr; e->color = t->color; e->w = t->width; e->h = t->height;
        e->pitch = t->pitch; e->palette = t->palette; e->cube = (uint32_t)t->cube;
        e->bytes = t->bytes;
        e->hash = tex_hash(t);
        e->checked_flip = s_flip;
        if (!tex_upload(e, t)) {
            memset(e, 0, sizeof *e);
            return s_white;
        }
    }
    e->used_flip = s_flip;
    return e->srv ? e->srv : s_white;
}

/* ---- pipeline state caches ---------------------------------------------- */

static D3D11_BLEND blend_map(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

typedef struct { uint64_t key; void *obj; } Cached;
#define MAX_STATE 256
static Cached s_blend[MAX_STATE], s_depth[MAX_STATE], s_samp[MAX_STATE];

static void *cache_find(Cached *c, uint64_t key)
{
    int i;
    for (i = 0; i < MAX_STATE && c[i].obj; i++)
        if (c[i].key == key)
            return c[i].obj;
    return NULL;
}

static void cache_put(Cached *c, uint64_t key, void *obj)
{
    int i;
    for (i = 0; i < MAX_STATE && c[i].obj; i++)
        ;
    if (i < MAX_STATE) {
        c[i].key = key;
        c[i].obj = obj;
    }
}

static ID3D11BlendState *blend_get(const PbGpuState *st, uint32_t color_mask)
{
    /* put_pixel: blending is on unless the pair is ONE, ZERO. */
    int on = st->blend_enable && !(st->blend_sfactor == 1 && st->blend_dfactor == 0);
    uint64_t key = ((uint64_t)on << 63) | ((uint64_t)(st->blend_sfactor & 0xFFFF) << 40)
                 | ((uint64_t)(st->blend_dfactor & 0xFFFF) << 24)
                 | ((uint64_t)(st->blend_equation & 0xFFFF) << 8)
                 | ((color_mask & 0x01000000u) ? 8 : 0) | ((color_mask & 0x00010000u) ? 4 : 0)
                 | ((color_mask & 0x00000100u) ? 2 : 0) | ((color_mask & 0x00000001u) ? 1 : 0);
    ID3D11BlendState *b = (ID3D11BlendState *)cache_find(s_blend, key);
    D3D11_BLEND_DESC bd;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];

    if (b)
        return b;
    memset(&bd, 0, sizeof bd);
    t->BlendEnable = on;
    t->SrcBlend = blend_map(st->blend_sfactor, 0);
    t->DestBlend = blend_map(st->blend_dfactor, 0);
    t->SrcBlendAlpha = blend_map(st->blend_sfactor, 1);
    t->DestBlendAlpha = blend_map(st->blend_dfactor, 1);
    t->BlendOp = t->BlendOpAlpha = blend_op(st->blend_equation);
    t->RenderTargetWriteMask = (UINT8)(((color_mask & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                            | ((color_mask & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                            | ((color_mask & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                            | ((color_mask & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &b)))
        return NULL;
    cache_put(s_blend, key, b);
    return b;
}

/* NV097 stencil ops are GL enums. */
static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;          /* 0x1E00 KEEP */
    }
}

/* Culling and polygon offset, one rasterizer state per combination.
 *
 * Winding: the executor hands over screen-space positions (y down) and the
 * vertex shader flips y into NDC, which D3D11 flips back through the
 * viewport -- so a triangle's winding in the render target is its winding
 * in NV2A screen space, and the NV2A's CW/CCW is in that same y-down space
 * (checked on Spartan: the swapped reading culled every floor and turned
 * characters inside out). RECOMP_CULL=0 turns culling off and
 * RECOMP_CULL_FLIP reverses the winding, should a title prove this wrong. */
static ID3D11RasterizerState *rs_get(const PbGpuState *st)
{
    static int mode = -2;
    static ID3D11RasterizerState *cache[64];
    static uint32_t keys[64];
    static int n;
    D3D11_RASTERIZER_DESC rd;
    D3D11_CULL_MODE cull = D3D11_CULL_NONE;
    BOOL ccw;
    int bias = 0, i;
    float slope = 0;
    uint32_t key;

    if (mode == -2) {
        const char *e = getenv("RECOMP_CULL");
        mode = e ? atoi(e) : 1;
        if (getenv("RECOMP_CULL_FLIP"))
            mode |= 2;
    }
    if ((mode & 1) && st->cull_enable) {
        if (st->cull_face == 0x0404)       cull = D3D11_CULL_FRONT;
        else if (st->cull_face == 0x0405)  cull = D3D11_CULL_BACK;
        /* FRONT_AND_BACK (0x408) draws nothing but lines and points. */
    }
    ccw = st->front_face == 0x0901;               /* GL CCW */
    if (mode & 2)
        ccw = !ccw;
    if (st->poly_offset_fill) {
        /* D3D11's depth bias is in units of the depth buffer's resolution,
         * as GL's polygon-offset units are. */
        bias = (int)st->poly_offset_bias;
        slope = st->poly_offset_scale;
    }
    key = (uint32_t)cull | (uint32_t)ccw << 4 | ((uint32_t)bias & 0xFFFF) << 8
        | ((uint32_t)(slope * 16.0f) & 0xFF) << 24;
    for (i = 0; i < n; i++)
        if (keys[i] == key)
            return cache[i];
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = cull;
    rd.FrontCounterClockwise = ccw;
    rd.DepthBias = bias;
    rd.SlopeScaledDepthBias = slope;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    if (n == 64)
        return s_rs;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &cache[n])))
        return s_rs;
    keys[n] = key;
    return cache[n++];
}

static ID3D11DepthStencilState *depth_get(const PbGpuState *st, int have_z)
{
    uint64_t key = ((uint64_t)!!have_z << 40) | ((uint64_t)!!st->depth_test << 32)
                 | ((uint64_t)!!st->depth_mask << 31) | (st->depth_func & 0xFFFF);
    ID3D11DepthStencilState *d;
    D3D11_DEPTH_STENCIL_DESC dd;
    int stencil = have_z && st->stencil_test;

    if (stencil) {
        /* Fold the stencil setup into the key: func, masks and the three
         * ops. The reference value is passed at bind time, not baked in. */
        uint64_t h = 1469598103934665603ull;
        h = (h ^ st->stencil_func) * 1099511628211ull;
        h = (h ^ (st->stencil_mask & 0xFF)) * 1099511628211ull;
        h = (h ^ (st->stencil_func_mask & 0xFF)) * 1099511628211ull;
        h = (h ^ st->stencil_op_fail) * 1099511628211ull;
        h = (h ^ st->stencil_op_zfail) * 1099511628211ull;
        h = (h ^ st->stencil_op_zpass) * 1099511628211ull;
        key ^= (h & 0xFFFFFF) << 41 | 1ull << 16;
    }
    d = (ID3D11DepthStencilState *)cache_find(s_depth, key);
    if (d)
        return d;
    memset(&dd, 0, sizeof dd);
    /* xf_rows: test when enabled, write whenever the mask allows -- even with
     * the test off, which D3D expresses as a test that always passes. */
    dd.DepthEnable = have_z && (st->depth_test || st->depth_mask);
    dd.DepthWriteMask = st->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (st->depth_test && st->depth_func >= 0x200 && st->depth_func <= 0x207)
        dd.DepthFunc = (D3D11_COMPARISON_FUNC)(st->depth_func - 0x200 + 1);
    if (stencil) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = (UINT8)st->stencil_func_mask;
        dd.StencilWriteMask = (UINT8)st->stencil_mask;
        dd.FrontFace.StencilFunc = (st->stencil_func >= 0x200 && st->stencil_func <= 0x207)
            ? (D3D11_COMPARISON_FUNC)(st->stencil_func - 0x200 + 1)
            : D3D11_COMPARISON_ALWAYS;
        dd.FrontFace.StencilFailOp = stencil_op(st->stencil_op_fail);
        dd.FrontFace.StencilDepthFailOp = stencil_op(st->stencil_op_zfail);
        dd.FrontFace.StencilPassOp = stencil_op(st->stencil_op_zpass);
        dd.BackFace = dd.FrontFace;         /* the NV2A has one-sided stencil */
        /* The stencil lives in the depth buffer; it must be bound even
         * when the depth test itself is off. */
        if (!dd.DepthEnable) {
            dd.DepthEnable = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        }
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &d)))
        return NULL;
    cache_put(s_depth, key, d);
    return d;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_map(uint32_t m)
{
    switch (m) {
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState *sampler_get(const PbGpuTexture *t)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    /* rc_texel: bilinear when either direction asks for TENT. */
    int lin = mag == 2 || min == 2 || min == 4 || min == 6;
    /* RECOMP_ANISO=<2..16>: anisotropic filtering on filtered textures --
     * ground and walls at grazing angles stay sharp. A settings change
     * makes new samplers; the key carries the level. */
    static long gen;
    static int aniso;
    uint64_t key;
    if (gen != xbox_settings_gen()) {
        gen = xbox_settings_gen();
        aniso = xbox_setting_int("RECOMP_ANISO", 0);
        aniso = aniso < 2 ? 0 : aniso > 16 ? 16 : aniso;
    }
    key = ((uint64_t)(lin ? aniso : 0) << 20) | ((uint64_t)lin << 16)
        | ((t->addr_u & 0xFF) << 8) | (t->addr_v & 0xFF);
    ID3D11SamplerState *s = (ID3D11SamplerState *)cache_find(s_samp, key);
    D3D11_SAMPLER_DESC sd;

    if (s)
        return s;
    memset(&sd, 0, sizeof sd);
    sd.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (lin && aniso) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = (UINT)aniso;
    }
    sd.AddressU = addr_map(t->addr_u);
    sd.AddressV = addr_map(t->addr_v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &sd, &s)))
        return NULL;
    cache_put(s_samp, key, s);
    return s;
}

/* ---- generated pixel shaders --------------------------------------------- */

typedef struct {
    Nv2aCombiner rc;           /* factors zeroed: they are constants, not code */
    uint32_t use_rc, alpha_test, alpha_func, clip_plane_mode;
    uint32_t stage_kind[4];    /* 0 none 1 2D 2 cube 3 rect-2D */
    uint32_t no_alpha[4];
    uint32_t input_tex[4];     /* dependent reads: the stage they read */
    uint32_t rt_swizzle[4];    /* render target read as another 32-bit order */
} PsKey;

typedef struct { PsKey key; ID3D11PixelShader *ps; } PsEntry;
#define MAX_PS 1024
static PsEntry s_ps[MAX_PS];
static int s_nps;

typedef struct { char *p; size_t n, cap; } Sb;

static void sb(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (b->n + 512 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 16384;
        b->p = (char *)realloc(b->p, b->cap);
    }
    va_start(ap, fmt);
    k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k > 0)
        b->n += (size_t)k;
}

/* map_in, as an HLSL expression over `x` (the raw value). */
static void map_in(Sb *b, const char *x, uint32_t byte)
{
    switch ((byte >> 5) & 7) {
    case 0:  sb(b, "max(%s,0)", x); break;
    case 1:  sb(b, "(1-saturate(%s))", x); break;
    case 2:  sb(b, "(2*max(%s,0)-1)", x); break;
    case 3:  sb(b, "(-2*max(%s,0)+1)", x); break;
    case 4:  sb(b, "(max(%s,0)-0.5)", x); break;
    case 5:  sb(b, "(-max(%s,0)+0.5)", x); break;
    case 6:  sb(b, "(%s)", x); break;
    default: sb(b, "(-(%s))", x); break;
    }
}

static void read_rgb(Sb *b, uint32_t byte)
{
    char x[32];
    snprintf(x, sizeof x, (byte & 0x10) ? "R[%u].aaa" : "R[%u].rgb", byte & 0xF);
    map_in(b, x, byte);
}

static void read_alpha(Sb *b, uint32_t byte)
{
    char x[32];
    snprintf(x, sizeof x, (byte & 0x10) ? "R[%u].a" : "R[%u].b", byte & 0xF);
    map_in(b, x, byte);
}

static const char *map_out(uint32_t m)
{
    switch (m) {
    case 0x08: return "(%s-0.5)";
    case 0x10: return "(%s*2)";
    case 0x18: return "((%s-0.5)*2)";
    case 0x20: return "(%s*4)";
    case 0x30: return "(%s*0.5)";
    default:   return "(%s)";
    }
}

static void gen_ps(const PsKey *k, Sb *b)
{
    static const char *const cmp[8] = {
        "false", "v<r", "v==r", "v<=r", "v>r", "v!=r", "v>=r", "true" };
    uint32_t st, s, n = k->rc.control & 0xFF, flags = k->rc.control >> 8;

    sb(b, "cbuffer P : register(b0) { float4 fac0[8]; float4 fac1[8];\n"
          "  float4 fc0; float4 fc1; float4 fogc; float4 texsize[4]; float4 misc;\n"
          "  float4 bump[4]; float4 bumpls[4]; };\n"
          "struct VO { float4 p:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1;\n"
          "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
          "  float4 t3:TEXCOORD3; float f:TEXCOORD4; };\n");
    for (st = 0; st < 4; st++) {
        sb(b, "%s tx%u : register(t%u); SamplerState sm%u : register(s%u);\n",
           k->stage_kind[st] == 2 ? "TextureCube" : "Texture2D", st, st, st, st);
    }
    sb(b, "float4 main(VO i) : SV_Target {\n"
          "  float4 R[16]; [unroll] for (int k = 0; k < 16; k++) R[k] = 0;\n"
          "  float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
          "  float4 o;\n");

    if (!k->use_rc) {
        /* xf_rows without combiners: diffuse, times texture 0 if bound. */
        sb(b, "  o = i.d0;\n");
        if (k->stage_kind[0]) {
            sb(b, "  { float2 uv = tc[0].xy");
            if (k->stage_kind[0] == 3) sb(b, " * texsize[0].xy");
            sb(b, "; float4 t = tx0.Sample(sm0, uv);%s o *= t; }\n",
               k->no_alpha[0] ? " t.a = 1;" : "");
        }
    } else {
        sb(b, "  R[3] = float4(fogc.rgb, i.f);\n"
              "  R[4] = i.d0; R[5] = i.d1;\n");
        /* rc_stage_fetch, by SHADER_STAGE_PROGRAM mode. */
        for (st = 0; st < 4; st++) {
            uint32_t mode = (k->rc.stage_program >> (st * 5)) & 0x1F, j;
            sb(b, "  { float4 c = tc[%u]; float q = c.w != 0 ? c.w : 1; float4 t;\n", st);
            switch (mode) {
            case 0:
                sb(b, "    t = float4(0,0,0,1);\n");
                break;
            case 1: case 2:                    /* PROJECT2D / 3D */
                sb(b, "    float2 uv = c.xy / q;\n");
                if (k->stage_kind[st] == 3)    /* texel coordinates */
                    sb(b, "    uv *= texsize[%u].xy;\n", st);
                if (k->stage_kind[st] == 2)
                    sb(b, "    t = tx%u.Sample(sm%u, float3(uv, 0));\n", st, st);
                else
                    sb(b, "    t = tx%u.Sample(sm%u, uv);\n", st, st);
                break;
            case 3:                            /* CUBEMAP */
                if (k->stage_kind[st] == 2) {
                    sb(b, "    t = tx%u.Sample(sm%u, c.xyz);\n", st, st);
                } else {
                    /* Not a cube: rc_stage_fetch reads it as 2D, unprojected. */
                    sb(b, "    float2 uv = c.xy;\n");
                    if (k->stage_kind[st] == 3)
                        sb(b, "    uv *= texsize[%u].xy;\n", st);
                    sb(b, "    t = tx%u.Sample(sm%u, uv);\n", st, st);
                }
                break;
            case 4:
                sb(b, "    t = saturate(c);\n");
                break;
            case 6: case 7:                    /* BUMPENVMAP (_LUMINANCE) */
                /* The input stage's blue and green are du and dv, as signed
                 * bytes (xemu's sign3, over 128).
                 *
                 * The four BUMP_ENV_MAT registers hold 00, 01, 11, 10 --
                 * not the 00 01 10 11 their order suggests (xemu reads them
                 * that way). Spartan's colour grade settles it: it writes
                 * 0.5 0 0.5 0 and samples a table whose u and v carry two
                 * different channels' curves from a texel-centre
                 * coordinate, which only works as the diagonal 0.5: half a
                 * signed byte, wrapped, is the byte unsigned. */
                sb(b, "    float2 ds = round(R[%u].bg * 255);\n"
                      "    ds = (ds >= 128 ? ds - 256 : ds) / 128;\n",
                   8 + k->input_tex[st]);
                sb(b, "    ds = float2(dot(ds, bump[%u].xw), dot(ds, bump[%u].yz));\n",
                   st, st);
                sb(b, "    float2 uv = c.xy + ds;\n");
                if (k->stage_kind[st] == 3)
                    sb(b, "    uv *= texsize[%u].xy;\n", st);
                sb(b, "    t = tx%u.Sample(sm%u, uv);\n", st, st);
                if (mode == 7)                 /* luminance: unsigned red */
                    sb(b, "    t *= R[%u].r * bumpls[%u].x + bumpls[%u].y;\n",
                       8 + k->input_tex[st], st, st);
                break;
            case 5:
                for (j = 0; j < 4; j++) {
                    int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
                    sb(b, "    if (c[%u] %s 0) discard;\n", j, ge ? ">=" : "<");
                }
                sb(b, "    t = 0;\n");
                break;
            default:
                sb(b, "    t = 0;\n");
                break;
            }
            /* A render target is sampled as it was drawn (A8R8G8B8); a
             * title reading it back as another byte order -- Spartan's colour
             * grade reads its downsample as R8G8B8A8 to index a table by
             * green and red -- gets the texel decoder's permutation here. */
            if (k->rt_swizzle[st] == 0x41)
                sb(b, "    t = t.argb;\n");          /* LIN_R8G8B8A8 */
            else if (k->rt_swizzle[st] == 0x40)
                sb(b, "    t = t.grab;\n");          /* LIN_B8G8R8A8 */
            else if (k->rt_swizzle[st] == 0x3F)
                sb(b, "    t = t.bgra;\n");          /* LIN_A8B8G8R8 */
            if (k->no_alpha[st] && (mode == 1 || mode == 2 || mode == 3
                                    || mode == 6 || mode == 7))
                sb(b, "    t.a = 1;\n");
            sb(b, "    R[%u] = t; }\n", 8 + st);
        }
        /* r0.a starts as texture 0's alpha, or 1 with stage 0 off. */
        sb(b, "  R[12].a = %s;\n", (k->rc.stage_program & 0x1F) ? "R[8].a" : "1");

        if (n > 8) n = 8;
        for (s = 0; s < n; s++) {
            uint32_t icw = k->rc.color_icw[s], ocw = k->rc.color_ocw[s];
            uint32_t aicw = k->rc.alpha_icw[s], aocw = k->rc.alpha_ocw[s];
            uint32_t fl = ocw >> 12, afl = aocw >> 12;
            const char *mo = map_out(fl & 0x38), *amo = map_out(afl & 0x38);
            char e[64];

            sb(b, "  { R[1] = fac0[%u]; R[2] = fac1[%u];\n",
               (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
            if (flags & 1)
                sb(b, "    bool mux = R[12].a >= 0.5;\n");
            else
                sb(b, "    bool mux = (((uint)(R[12].a * 255.0)) & 1) != 0;\n");
            sb(b, "    float3 A = "); read_rgb(b, icw >> 24); sb(b, ";\n");
            sb(b, "    float3 B = "); read_rgb(b, (icw >> 16) & 0xFF); sb(b, ";\n");
            sb(b, "    float3 C = "); read_rgb(b, (icw >> 8) & 0xFF); sb(b, ";\n");
            sb(b, "    float3 D = "); read_rgb(b, icw & 0xFF); sb(b, ";\n");
            sb(b, "    float aA = "); read_alpha(b, aicw >> 24); sb(b, ";\n");
            sb(b, "    float aB = "); read_alpha(b, (aicw >> 16) & 0xFF); sb(b, ";\n");
            sb(b, "    float aC = "); read_alpha(b, (aicw >> 8) & 0xFF); sb(b, ";\n");
            sb(b, "    float aD = "); read_alpha(b, aicw & 0xFF); sb(b, ";\n");
            sb(b, "    float3 pab = %s;\n", (fl & 2) ? "dot(A,B).xxx" : "A*B");
            sb(b, "    float3 pcd = %s;\n", (fl & 1) ? "dot(C,D).xxx" : "C*D");
            sb(b, "    float3 sm = %s;\n", (fl & 4) ? "(mux ? pcd : pab)" : "pab + pcd");
            snprintf(e, sizeof e, mo, "pab"); sb(b, "    float3 ab = clamp(%s,-1,1);\n", e);
            snprintf(e, sizeof e, mo, "pcd"); sb(b, "    float3 cd = clamp(%s,-1,1);\n", e);
            snprintf(e, sizeof e, mo, "sm");  sb(b, "    float3 ms = clamp(%s,-1,1);\n", e);
            sb(b, "    float apab = aA*aB, apcd = aC*aD;\n");
            sb(b, "    float asm_ = %s;\n", (afl & 4) ? "(mux ? apcd : apab)" : "apab + apcd");
            snprintf(e, sizeof e, amo, "apab"); sb(b, "    float aab = clamp(%s,-1,1);\n", e);
            snprintf(e, sizeof e, amo, "apcd"); sb(b, "    float acd = clamp(%s,-1,1);\n", e);
            snprintf(e, sizeof e, amo, "asm_"); sb(b, "    float ams = clamp(%s,-1,1);\n", e);
            if ((ocw >> 4) & 0xF) {
                sb(b, "    R[%u].rgb = ab;\n", (ocw >> 4) & 0xF);
                if (fl & 0x80) sb(b, "    R[%u].a = ab.b;\n", (ocw >> 4) & 0xF);
            }
            if (ocw & 0xF) {
                sb(b, "    R[%u].rgb = cd;\n", ocw & 0xF);
                if (fl & 0x40) sb(b, "    R[%u].a = cd.b;\n", ocw & 0xF);
            }
            if ((ocw >> 8) & 0xF)
                sb(b, "    R[%u].rgb = ms;\n", (ocw >> 8) & 0xF);
            if ((aocw >> 4) & 0xF) sb(b, "    R[%u].a = aab;\n", (aocw >> 4) & 0xF);
            if (aocw & 0xF)        sb(b, "    R[%u].a = acd;\n", aocw & 0xF);
            if ((aocw >> 8) & 0xF) sb(b, "    R[%u].a = ams;\n", (aocw >> 8) & 0xF);
            sb(b, "    R[0] = 0; }\n");
        }

        if (k->rc.final0 || k->rc.final1) {
            uint32_t f0 = k->rc.final0, f1 = k->rc.final1, ff = f1 & 0xFF;
            sb(b, "  { R[1] = fc0; R[2] = fc1;\n");
            sb(b, "    float3 sa = %s, sb_ = %s;\n",
               (ff & 0x40) ? "1 - R[5].rgb" : "R[5].rgb",
               (ff & 0x20) ? "1 - R[12].rgb" : "R[12].rgb");
            sb(b, "    R[14] = float4(%s, 0);\n", (ff & 0x80) ? "saturate(sa + sb_)" : "sa + sb_");
            sb(b, "    float3 E = "); read_rgb(b, f1 >> 24); sb(b, ";\n");
            sb(b, "    float3 F = "); read_rgb(b, (f1 >> 16) & 0xFF); sb(b, ";\n");
            sb(b, "    R[15] = float4(E * F, 0);\n");
            sb(b, "    float3 A = "); read_rgb(b, f0 >> 24); sb(b, ";\n");
            sb(b, "    float3 B = "); read_rgb(b, (f0 >> 16) & 0xFF); sb(b, ";\n");
            sb(b, "    float3 C = "); read_rgb(b, (f0 >> 8) & 0xFF); sb(b, ";\n");
            sb(b, "    float3 D = "); read_rgb(b, f0 & 0xFF); sb(b, ";\n");
            sb(b, "    o.rgb = D + C * (1 - A) + B * A;\n");
            sb(b, "    o.a = "); read_alpha(b, (f1 >> 8) & 0xFF); sb(b, "; }\n");
        } else {
            sb(b, "  o = R[12];\n");
        }
    }
    sb(b, "  o = saturate(o);\n");
    if (k->alpha_test) {
        uint32_t f = k->alpha_func;
        sb(b, "  { int v = (int)(o.a * 255.0 + 0.5), r = (int)misc.x;\n"
              "    if (!(%s)) discard; }\n",
           (f >= 0x200 && f <= 0x207) ? cmp[f - 0x200] : "true");
    }
    sb(b, "  return o; }\n");
}

static ID3D11PixelShader *ps_get(const PsKey *k)
{
    int i;
    Sb b = { 0, 0, 0 };
    ID3DBlob *code;
    ID3D11PixelShader *ps = NULL;

    for (i = 0; i < s_nps; i++)
        if (!memcmp(&s_ps[i].key, k, sizeof *k))
            return s_ps[i].ps;
    gen_ps(k, &b);
    code = compile(b.p, "ps_5_0");
    if (getenv("RECOMP_PB_GPU_DUMP"))
        fprintf(stderr, "  [PBGPU] pixel shader %d:\n%s\n", s_nps, b.p);
    free(b.p);
    if (code) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &ps);
        ID3D10Blob_Release(code);
    }
    if (s_nps < MAX_PS) {
        s_ps[s_nps].key = *k;
        s_ps[s_nps].ps = ps;
        s_nps++;
    }
    return ps;
}

/* ---- drawing ------------------------------------------------------------ */

static void unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

/* The surface's anti-aliasing mode (SET_SURFACE_FORMAT bits 12-15), as the
 * factor between the title's coordinates and the surface's pixels.
 *
 * In a supersampled mode the NV2A renders a logical surface onto more
 * samples than it has pixels: CENTER_CORNER_2 doubles x, SQUARE_OFFSET_4
 * doubles both (xemu's pgraph_apply_anti_aliasing_factor). A title then
 * sets its clip rectangle and viewport in the logical size, and the
 * hardware covers the whole buffer. Spartan switches its 640x480 back
 * buffer to SQUARE_OFFSET_4 for its stencil shadows and glow sprites,
 * drawing in 320x240 logical space; read 1:1, every one of them landed
 * in the top-left quarter -- the miniature shadows, the misplaced orbs.
 * Here the samples are the pixels, so the factor scales positions and
 * the clip rectangle onto the surface. */
static void aa_factor(const PbGpuState *st, uint32_t *fx, uint32_t *fy)
{
    switch ((st->surf_format >> 12) & 0xF) {
    case 1:  *fx = 2; *fy = 1; break;      /* CENTER_CORNER_2 */
    case 2:  *fx = 2; *fy = 2; break;      /* SQUARE_OFFSET_4 */
    default: *fx = 1; *fy = 1; break;      /* CENTER_1 */
    }
}

static void set_scissor(const PbGpuState *st, const Surface *s)
{
    D3D11_RECT r;
    uint32_t fx, fy;
    aa_factor(st, &fx, &fy);
    r.left = (LONG)floorf((float)(st->clip_x * fx) * s->sx);
    r.top = (LONG)floorf((float)(st->clip_y * fy) * s->sy);
    r.right = (LONG)ceilf((float)((st->clip_x + st->clip_w) * fx) * s->sx);
    r.bottom = (LONG)ceilf((float)((st->clip_y + st->clip_h) * fy) * s->sy);
    if (r.right > (LONG)s->pw) r.right = (LONG)s->pw;
    if (r.bottom > (LONG)s->ph) r.bottom = (LONG)s->ph;
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
}

/* Frame capture: every render target one frame draws to, in order, with
 * the state it was drawn with, and each target as it stood when the frame
 * moved on from it -- to find a pass landing on the wrong surface or
 * sampling the wrong thing.
 *
 * F12 in the game window captures the next frame into capture\NNN\ under
 * the working directory (trace.txt plus one BMP per pass);
 * RECOMP_RT_TRACE=<flip> captures that flip without a key. */
static struct {
    int active;
    uint32_t flip;
    FILE *log;
    char dir[260];
    Surface *last;
    unsigned n, draws;
} s_cap;

static void rt_dump(Surface *s, const char *name);

static void cap_poll(void)
{
    extern int xbox_FramebufferKeyDown(int vk);
    static int was, env_flip = -2;
    int down = xbox_FramebufferKeyDown(VK_F12);

    if (s_cap.active && s_flip >= s_cap.flip) {
        if (s_cap.last) {
            char name[64];
            snprintf(name, sizeof name, "%02u_%08X_final", s_cap.n, s_cap.last->addr);
            rt_dump(s_cap.last, name);
            fprintf(s_cap.log, "  (%u draws)\n", s_cap.draws);
        }
        fclose(s_cap.log);
        fprintf(stderr, "  [CAPTURE] frame written to %s\n", s_cap.dir);
        s_cap.active = 0;
    }
    if (env_flip == -2) {
        const char *e = getenv("RECOMP_RT_TRACE");
        env_flip = e ? atoi(e) : -1;
    }
    if (!s_cap.active && ((down && !was)
                          || (env_flip >= 0 && s_flip + 1 == (uint32_t)env_flip))) {
        int i;
        CreateDirectoryA("capture", NULL);
        for (i = 0; i < 1000; i++) {
            snprintf(s_cap.dir, sizeof s_cap.dir, "capture\\%03d\\", i);
            if (CreateDirectoryA(s_cap.dir, NULL))
                break;
        }
        {
            char path[300];
            snprintf(path, sizeof path, "%strace.txt", s_cap.dir);
            s_cap.log = fopen(path, "w");
        }
        if (s_cap.log) {
            s_cap.active = 1;
            s_cap.flip = s_flip + 1;
            s_cap.last = NULL;
            s_cap.n = s_cap.draws = 0;
            fprintf(s_cap.log, "capture of flip %u\n", s_cap.flip);
            {
                /* RECOMP_CAP_FIND=<hex dword>[,<hex dword>]: guest RAM
                 * addresses holding that dword (pair), to find which game
                 * variable a value in the trace comes from. */
                const char *f = getenv("RECOMP_CAP_FIND");
                if (f) {
                    extern uintptr_t g_xbox_mem_offset;
                    char *e2;
                    uint32_t a = (uint32_t)strtoul(f, &e2, 16), b = 0, va;
                    int pair = *e2 == ',', hits = 0;
                    if (pair)
                        b = (uint32_t)strtoul(e2 + 1, NULL, 16);
                    for (va = 0x10000; va < 0x04000000 && hits < 64; ) {
                        MEMORY_BASIC_INFORMATION mi;
                        uint8_t *p = (uint8_t *)(g_xbox_mem_offset + va);
                        uint32_t len, k;
                        if (!VirtualQuery(p, &mi, sizeof mi))
                            break;
                        len = (uint32_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - p);
                        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                            for (k = 0; k + 8 <= len && hits < 64; k += 4)
                                if (*(uint32_t *)(p + k) == a
                                        && (!pair || *(uint32_t *)(p + k + 4) == b)) {
                                    const uint32_t *q = (const uint32_t *)(p + k);
                                    fprintf(s_cap.log, "  found %08X at %08X: %08X %08X [%08X] %08X %08X %08X\n",
                                            a, va + k, k >= 8 ? q[-2] : 0, k >= 4 ? q[-1] : 0,
                                            q[0], q[1], q[2], q[3]);
                                    hits++;
                                }
                        va += len;
                    }
                }
            }
            if (env_flip >= 0 && s_cap.flip == (uint32_t)env_flip)
                env_flip = -1;                 /* the env trigger fires once */
        }
    }
    was = down;
}

/* Write a surface as it is on the GPU now to <capture dir><name>.bmp. */
static void rt_dump(Surface *s, const char *name)
{
    const char *dir = s_cap.active ? s_cap.dir : NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[512];
    FILE *fp;
    uint32_t y, w = s->w, h = s->h, hdr[13];

    if (!dir || !s->staging)
        return;
    snprintf(path, sizeof path, "%s%s.bmp", dir, name);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s->staging,
                                     (ID3D11Resource *)surf_logical(s));
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    if ((fp = fopen(path, "wb")) != NULL) {
        memset(hdr, 0, sizeof hdr);
        fwrite("BM", 1, 2, fp);
        hdr[0] = 54 + w * h * 4; hdr[1] = 0; hdr[2] = 54; hdr[3] = 40;
        hdr[4] = w; hdr[5] = (uint32_t)-(int32_t)h; hdr[6] = 1 | (32 << 16);
        fwrite(hdr, 4, 13, fp);
        for (y = 0; y < h; y++)
            fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, w, fp);
        fclose(fp);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s->staging, 0);
}

static Surface *bind_target(const PbGpuState *st, Zeta **zout)
{
    Surface *s;
    Zeta *z = NULL;
    D3D11_VIEWPORT vp;

    if (!st->surf_addr || !st->surf_pitch || (st->surf_bpp != 4 && st->surf_bpp != 2))
        return NULL;
    res_update();
    s = surface_get(st->surf_addr, st->surf_pitch, st->surf_bpp,
                    (st->clip_y + st->clip_h) * (((st->surf_format >> 12) & 0xF) == 2 ? 2u : 1u));
    if (!s)
        return NULL;
    if (st->zeta_addr)
        z = zeta_get(st->zeta_addr, s->pw, s->ph);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->rtv, z ? z->dsv : NULL);
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)s->pw;
    vp.Height = (float)s->ph;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    set_scissor(st, s);
    if (s_cap.active && s_flip == s_cap.flip && s_cap.n < 400) {
        FILE *lg = s_cap.log;
        if (s != s_cap.last) {
            int k;
            if (s_cap.last) {
                char name[64];
                snprintf(name, sizeof name, "%02u_%08X", s_cap.n, s_cap.last->addr);
                rt_dump(s_cap.last, name);
                fprintf(lg, "  (%u draws)\n", s_cap.draws);
            }
            s_cap.n++;
            s_cap.draws = 0;
            fprintf(lg, "%02u -> %08X pitch %u clip %u,%u %ux%u (surf %ux%u) blend %u"
                    " %X/%X eq %X z %08X ztest %u func %X zmask %u cmask %08X\n",
                    s_cap.n, st->surf_addr, st->surf_pitch, st->clip_x, st->clip_y,
                    st->clip_w, st->clip_h, s->w, s->h, st->blend_enable,
                    st->blend_sfactor, st->blend_dfactor, st->blend_equation,
                    st->zeta_addr, st->depth_test, st->depth_func, st->depth_mask,
                    st->color_mask);
            fprintf(lg, "     surface format %08X control0 %08X aa_control %08X\n",
                    st->surf_format, st->control0, st->aa_control);
            fprintf(lg, "     rc %d stages %05X combiners %u fac0 %08X fac1 %08X"
                    " fc0 %08X fc1 %08X alphatest %u\n", st->use_rc,
                    st->rc.stage_program, st->rc.control & 0xF, st->rc.factor0[0],
                    st->rc.factor1[0], st->rc.final_c0, st->rc.final_c1,
                    st->alpha_test);
            for (k = 0; k < 4; k++)
                if (st->tex[k].valid)
                    fprintf(lg, "     t%d %08X %ux%u fmt %02X norm %d addr %u/%u\n", k,
                            st->tex[k].addr, st->tex[k].width, st->tex[k].height,
                            st->tex[k].color, st->tex[k].normalized,
                            st->tex[k].addr_u, st->tex[k].addr_v);
        }
        s_cap.last = s;
        s_cap.draws++;
    }
    s->drawn_flip = s_flip;
    s->drawn = 1;
    *zout = z;
    return s;
}

/* RECOMP capture: one line per screen-space draw (see cap_poll). */
static void cap_log_draw(const PbGpuState *st, const PbGpuVertex *v, uint32_t count)
{
    if (s_cap.active && s_flip == s_cap.flip && s_cap.log) {
        /* One line per draw: its screen extent and the state that decides
         * what it does to the target. */
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        uint32_t k;
        for (k = 0; k < count; k++) {
            if (v[k].pos[0] < x0) x0 = v[k].pos[0];
            if (v[k].pos[0] > x1) x1 = v[k].pos[0];
            if (v[k].pos[1] < y0) y0 = v[k].pos[1];
            if (v[k].pos[1] > y1) y1 = v[k].pos[1];
            if (v[k].pos[2] < z0) z0 = v[k].pos[2];
            if (v[k].pos[2] > z1) z1 = v[k].pos[2];
        }
        fprintf(s_cap.log, "     draw %u verts x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f"
                " w0 %.3f blend %u %X/%X ztest %u zmask %u cmask %08X stages %05X"
                " t0 %08X %ux%u d0 %.2f,%.2f,%.2f,%.2f\n",
                count, x0, x1, y0, y1, z0, z1, count ? v[0].pos[3] : 0.0f,
                st->blend_enable, st->blend_sfactor, st->blend_dfactor,
                st->depth_test, st->depth_mask, st->color_mask,
                st->use_rc ? st->rc.stage_program : 0xFFFFF,
                st->tex[0].valid ? st->tex[0].addr : 0, st->tex[0].width,
                st->tex[0].height, count ? v[0].d0[0] : 0, count ? v[0].d0[1] : 0,
                count ? v[0].d0[2] : 0, count ? v[0].d0[3] : 0);
        {
            extern const float *nv2a_vsh_constant(uint32_t index);
            const float *c58 = nv2a_vsh_constant(58), *c59 = nv2a_vsh_constant(59);
            fprintf(s_cap.log, "          c58 %.1f %.1f %.1f  c59 %.1f %.1f %.1f\n",
                    c58[0], c58[1], c58[2], c59[0], c59[1], c59[2]);
            if (count >= 3) {
                /* Where the bulk of the draw lands: the extent of the
                 * vertices with a sane w (clipped ones run off to huge
                 * values and hide it). */
                float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
                uint32_t sane = 0;
                for (k = 0; k < count; k++)
                    if (v[k].pos[3] > 0.05f && v[k].pos[3] < 1e6f) {
                        sane++;
                        if (v[k].pos[0] < sx0) sx0 = v[k].pos[0];
                        if (v[k].pos[0] > sx1) sx1 = v[k].pos[0];
                        if (v[k].pos[1] < sy0) sy0 = v[k].pos[1];
                        if (v[k].pos[1] > sy1) sy1 = v[k].pos[1];
                    }
                fprintf(s_cap.log, "          sane-w verts %u: x %.1f..%.1f y %.1f..%.1f;"
                        " v0 (%.1f %.1f %.4f %.3f)\n", sane, sx0, sx1, sy0, sy1,
                        v[0].pos[0], v[0].pos[1], v[0].pos[2], v[0].pos[3]);
                fprintf(s_cap.log, "          tex0 v0 (%.3f %.3f %.3f %.3f) v1 (%.3f %.3f %.3f %.3f)"
                        " v2 (%.3f %.3f %.3f %.3f) fmt %02X\n",
                        v[0].tex[0][0], v[0].tex[0][1], v[0].tex[0][2], v[0].tex[0][3],
                        v[1].tex[0][0], v[1].tex[0][1], v[1].tex[0][2], v[1].tex[0][3],
                        v[2].tex[0][0], v[2].tex[0][1], v[2].tex[0][2], v[2].tex[0][3],
                        st->tex[0].color);
            }
        }
    }

}

/* Everything a draw needs except its vertices and vertex shader: target,
 * pixel shader and its constants, textures, blend/depth/stencil/raster state.
 * Shared by the screen-space path (pbgpu_draw) and the vertex-program path
 * (pbgpu_draw_program). Called with s_lock held; 0 if nothing can be drawn. */
static int draw_begin(const PbGpuState *st, Surface **so, Zeta **zo)
{
    Surface *s;
    Zeta *z;
    PsKey key;
    PsConsts pc;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t st_i;
    float bf[4];
    ID3D11PixelShader *ps;
    ID3D11BlendState *bs;

    if (!(s = bind_target(st, &z)))
        return 0;
    *so = s;
    *zo = z;

    /* Pixel shader key: the combiner program's shape, not its constants. */
    memset(&key, 0, sizeof key);
    key.rc = st->rc;
    memset(key.rc.factor0, 0, sizeof key.rc.factor0);
    memset(key.rc.factor1, 0, sizeof key.rc.factor1);
    key.rc.final_c0 = key.rc.final_c1 = 0;
    key.use_rc = (uint32_t)st->use_rc;
    key.alpha_test = st->alpha_test ? 1 : 0;
    key.alpha_func = st->alpha_test ? st->alpha_func : 0;
    if (!st->use_rc)
        memset(&key.rc, 0, sizeof key.rc);
    key.clip_plane_mode = st->clip_plane_mode;

    memset(&pc, 0, sizeof pc);
    for (st_i = 0; st_i < 4; st_i++) {
        const PbGpuTexture *t = &st->tex[st_i];
        uint32_t mode = st->use_rc ? (st->rc.stage_program >> (st_i * 5)) & 0x1F
                                   : (st_i == 0 && t->valid ? 1 : 0);
        srv[st_i] = s_white;
        smp[st_i] = NULL;
        if ((mode >= 1 && mode <= 3) || mode == 6 || mode == 7) {
            if (mode == 6 || mode == 7) {
                uint32_t in = st_i == 1 ? 0
                            : (st->other_stage_input >> (st_i == 2 ? 16 : 20)) & 0xF;
                key.input_tex[st_i] = in < st_i ? in : st_i - 1;
                memcpy(pc.bump[st_i], t->bump_mat, sizeof pc.bump[st_i]);
                pc.bumpls[st_i][0] = t->bump_scale;
                pc.bumpls[st_i][1] = t->bump_offset;
            }
            key.stage_kind[st_i] = (mode == 3 && t->cube) ? 2
                                 : (t->valid && !t->normalized) ? 3 : 1;
            key.no_alpha[st_i] = (uint32_t)t->no_alpha;
            srv[st_i] = tex_get(t, s);
            smp[st_i] = sampler_get(t);
            if (key.stage_kind[st_i] == 2 && srv[st_i] == s_white)
                key.stage_kind[st_i] = 1;      /* no cube to bind */
            if (t->width && t->height) {
                /* Texel coordinates into a render target address the
                 * surface it was drawn to, which is taller than the texture
                 * (surfaces are made at least 480 rows): normalising by the
                 * texture's own height read rows nothing had drawn. */
                const Surface *rt = !t->cube ? surface_find(t->addr) : NULL;
                uint32_t w = t->width, h = t->height;
                if (rt && rt->drawn_flip + 4 >= s_flip
                    && (srv[st_i] == rt->srv || srv[st_i] == s_copy_srv)) {
                    w = rt->w;
                    h = rt->h;
                    if (t->color == 0x41 || t->color == 0x40 || t->color == 0x3F)
                        key.rt_swizzle[st_i] = t->color;
                }
                pc.texsize[st_i][0] = 1.0f / (float)w;
                pc.texsize[st_i][1] = 1.0f / (float)h;
            }
        }
    }
    if (!(ps = ps_get(&key)))
        return 0;

    for (st_i = 0; st_i < 8; st_i++) {
        unpack(st->rc.factor0[st_i], pc.fac0[st_i]);
        unpack(st->rc.factor1[st_i], pc.fac1[st_i]);
    }
    unpack(st->rc.final_c0, pc.fc0);
    unpack(st->rc.final_c1, pc.fc1);
    memcpy(pc.fogc, st->fog_color, sizeof pc.fogc);
    pc.misc[0] = (float)(st->alpha_ref & 0xFF);

    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pc, sizeof pc);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
    }

    bs = blend_get(st, st->color_mask);
    {
        /* CONSTANT_COLOR and CONSTANT_ALPHA share D3D's one blend factor. */
        float c[4];
        unpack(st->blend_color, c);
        if (st->blend_sfactor == 0x8003 || st->blend_sfactor == 0x8004
            || st->blend_dfactor == 0x8003 || st->blend_dfactor == 0x8004)
            bf[0] = bf[1] = bf[2] = bf[3] = c[3];
        else
            memcpy(bf, c, sizeof bf);
    }
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_get(st, z != NULL),
                                               st->stencil_ref & 0xFF);
    ID3D11DeviceContext_RSSetState(s_ctx, rs_get(st));
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    return 1;
}

/* After a draw: let go of the textures (a render target bound as a texture
 * cannot be bound as a target next) and, when capturing, keep a picture. */
static void draw_end(Surface *s)
{
    ID3D11ShaderResourceView *none[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
    if (s_cap.active && s_flip == s_cap.flip && s_cap.draws > 3) {
        /* RECOMP_CAP_DRAWS=a,b,c: also an image after those draw numbers
         * (counted within each pass). */
        static int n = -1;
        static unsigned list[32];
        int i;
        if (n < 0) {
            const char *e = getenv("RECOMP_CAP_DRAWS");
            n = 0;
            while (e && *e && n < 32) {
                char *end;
                list[n++] = (unsigned)strtoul(e, &end, 10);
                e = *end == ',' ? end + 1 : "";
            }
        }
        for (i = 0; i < n; i++)
            if (list[i] == s_cap.draws) {
                char name[64];
                snprintf(name, sizeof name, "%02u_%08X_draw%u", s_cap.n, s->addr, s_cap.draws);
                rt_dump(s, name);
            }
    }
    if (s_cap.active && s_flip == s_cap.flip && s_cap.draws <= 3) {
        /* The first few draws of each pass, one image each: what a single
         * draw did to the target, where the per-pass images only show the
         * sum. */
        char name[64];
        snprintf(name, sizeof name, "%02u_%08X_draw%u", s_cap.n, s->addr, s_cap.draws);
        rt_dump(s, name);
    }
}

/* Screen-space vertices, transformed already (by the CPU interpreter or
 * because the title sent them that way). */
void pbgpu_draw(const PbGpuState *st, const PbGpuVertex *v, uint32_t count)
{
    Surface *s;
    Zeta *z;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t bytes = count * sizeof(PbGpuVertex), zmax;
    float vpc[4];
    UINT stride = sizeof(PbGpuVertex), off;

    if (!count || bytes > VB_BYTES || !pbgpu_enabled())
        return;
    EnterCriticalSection(&s_lock);
    if (!draw_begin(st, &s, &z))
        goto out;
    cap_log_draw(st, v, count);

    if (s_vb_pos + bytes > VB_BYTES)
        s_vb_pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
            s_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        goto out;
    memcpy((uint8_t *)m.pData + s_vb_pos, v, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    off = s_vb_pos;
    s_vb_pos += bytes;

    zmax = st->zeta_format == 1 ? 0xFFFFu : 0xFFFFFFu;
    {
        uint32_t fx, fy;
        aa_factor(st, &fx, &fy);
        vpc[0] = 2.0f * (float)fx / (float)s->w;
        vpc[1] = 2.0f * (float)fy / (float)s->h;
    }
    vpc[2] = 1.0f / (float)zmax;
    vpc[3] = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vs_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, vpc, sizeof vpc);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vs_cb, 0);
    }
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_vs_cb);
    ID3D11DeviceContext_Draw(s_ctx, count, 0);
    draw_end(s);
out:
    LeaveCriticalSection(&s_lock);
}

/* ---- vertex programs on the GPU ------------------------------------------
 *
 * The title's vertex program, translated to HLSL (nv2a_vsh_hlsl.c), runs as
 * the vertex shader. Its raw attributes go up in a structured buffer read by
 * vertex index -- no input layout to match against the NV2A's own vertex
 * formats (packed normals, shorts...), which the executor already decodes
 * to floats -- and its 192 constants in a constant buffer.
 *
 * What the program writes is screen space, as on the console: the XDK
 * appends the viewport transform and the divide by w to every shader. So
 * the epilogue here is the one the screen-space path uses -- back to clip
 * space through w -- and near-plane clipping is a clip distance at the same
 * w the CPU clipper used. */
#define PG_MAX_PROGS   512
#define PG_ATTR_F4     (1u << 21)     /* float4s in the attribute ring: 32 MB */
#define PG_IDX_U32     (1u << 21)     /* indices in the index ring: 8 MB */

typedef struct {
    uint64_t hash;
    ID3D11VertexShader *vs;           /* NULL: translation or compile failed */
    int ui_const;                     /* oPos = r + c[ui_const], or -1 */
} PgEntry;
static PgEntry   s_pg[PG_MAX_PROGS];
static int       s_npg;
static ID3D11Buffer *s_attr_buf, *s_idx_buf, *s_pg_cb, *s_const_cb;
static ID3D11ShaderResourceView *s_attr_srv;
static uint32_t  s_attr_pos, s_idx_pos, s_const_gen_up;

typedef struct {
    float vp[4];                      /* 2/w, 2/h, 1/zmax, - */
    float fog[4];                     /* enable, mode, param x, param y */
    uint32_t info[4];                 /* stride (float4s), first element */
    int32_t  slot[16][4];             /* attribute -> slot in a vertex, or -1 */
    float    vdef[16][4];             /* value of an absent attribute */
    float    ui[4];                   /* safe-area fill: x' = (x - ui.x) * ui.z + ui.w */
    float    ui2[4];                  /* y' = (y - ui2.x) * ui2.z + ui2.w; ui.z 0: off */
} PgConsts;

static const char k_pg_head[] =
"cbuffer P : register(b0) { float4 vp; float4 fogp; uint4 info;\n"
"  int4 slot[16]; float4 vdef[16]; float4 ui; float4 ui2; };\n"
"cbuffer K : register(b1) { float4 C[192]; };\n"
"StructuredBuffer<float4> Attr : register(t0);\n"
"struct VO { float4 p:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2;\n"
"  float4 t3:TEXCOORD3; float f:TEXCOORD4; float cd:SV_ClipDistance0; };\n"
"float4 nv_rcc(float x) { float f = 1.0 / x;\n"
"  if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
"  if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
"  return f.xxxx; }\n"
"float4 nv_exp(float x) { float fl = floor(x);\n"
"  return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
"float4 nv_log(float x) { float ax = abs(x);\n"
"  if (ax == 0) return float4(asfloat(0xFF800000), 1.0, asfloat(0xFF800000), 1.0);\n"
"  float e = floor(log2(ax)); return float4(e, ax / exp2(e), log2(ax), 1.0); }\n"
"float4 nv_lit(float4 c) { float nl = max(c.x, 0), nh = max(c.y, 0);\n"
"  float p = clamp(c.w, -127.9961, 127.9961);\n"
"  return float4(1.0, nl, c.x > 0 ? pow(nh, p) : 0, 1.0); }\n"
"float fogf(float d) { if (fogp.x == 0) return 1.0;\n"
"  float f, px = fogp.z, py = fogp.w; uint mode = (uint)fogp.y;\n"
"  if (mode == 0x800 || mode == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (mode == 0x801 || mode == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return saturate(f); }\n"
"VO main(uint vid : SV_VertexID) {\n"
"  float4 v[16]; int k;\n"
"  [unroll] for (k = 0; k < 16; k++)\n"
"    v[k] = slot[k].x >= 0 ? Attr[info.y + vid * info.x + (uint)slot[k].x] : vdef[k];\n"
"  float4 R[12]; [unroll] for (k = 0; k < 12; k++) R[k] = 0;\n"
"  float4 o[13]; [unroll] for (k = 0; k < 13; k++) o[k] = 0;\n"
"  o[3].w = 1; o[4].w = 1; o[9].w = 1; o[10].w = 1; o[11].w = 1; o[12].w = 1;\n"
"  float4 oPos = 0; int a0 = 0;\n";

static const char k_pg_tail[] =
"  VO r; float w = oPos.w;\n"
"  if (isinf(w) || isnan(w)) w = 1.0;\n"
"  if (ui.z != 0) { oPos.x = (oPos.x - ui.x) * ui.z + ui.w;\n"
"    oPos.y = (oPos.y - ui2.x) * ui2.z + ui2.w; }\n"
"  float x = oPos.x * vp.x - 1.0, y = 1.0 - oPos.y * vp.y, z = oPos.z * vp.z;\n"
"  r.p = float4(x * w, y * w, z * w, w);\n"
"  r.cd = w - 1e-3;\n"
"  r.d0 = o[3]; r.d1 = o[4];\n"
"  r.t0 = o[9]; r.t1 = o[10]; r.t2 = o[11]; r.t3 = o[12];\n"
"  r.f = fogf(o[5].x);\n"
"  return r; }\n";

/* The shape the frame is shown at, width over height: RECOMP_ASPECT, for a
 * title rendering anamorphic widescreen into its 4:3 buffer; 0 shows the
 * buffer at its own shape. */
float xbox_DisplayAspect(void)
{
    static float a = -1;
    if (a < 0) {
        const char *e = getenv("RECOMP_ASPECT");
        a = e ? (float)atof(e) : 0;
        if (a < 0.5f || a > 8)
            a = 0;
    }
    return a;
}

/* The program pg_get last returned. */
static PgEntry *s_pg_cur;

/* A screen-space program -- one whose last position write is a register plus
 * a constant, oPos = r + c[N] -- returns N, else -1. Titles draw their 2D
 * layer (text, HUD, menus) this way, inside a TV title-safe box placed by
 * that constant; see ui_fill. */
static int ui_const_of(const char *body)
{
    const char *p = NULL, *q = body, *blk, *c;
    while ((q = strstr(q, "oPos.")) != NULL)
        p = q++;
    if (!p)
        return -1;
    for (blk = p; blk > body && strncmp(blk, "{ float4 A", 10); blk--)
        ;
    if (strncmp(blk, "{ float4 A", 10))
        return -1;
    {
        size_t n = (size_t)(p - blk);
        char tmp[512];
        if (n >= sizeof tmp)
            return -1;
        memcpy(tmp, blk, n);
        tmp[n] = 0;
        if (!strstr(tmp, "m = A + Cc;") || strstr(tmp, "float4 A = C[")
                || strstr(tmp, "a0") || !(c = strstr(tmp, "float4 Cc = C[")))
            return -1;
        return atoi(c + 14);
    }
}

/* Safe-area fill. A console title keeps its 2D layer inside the TV's title-
 * safe area: Spartan lays text out in a 512x448 box and offsets it into the
 * 640x480 screen with c15 = (63.5, 15.5), which leaves subtitles straddling
 * the edge of the letterbox bar. On a PC display there is no overscan, so the
 * box is scaled (uniformly, keeping glyph shapes) to fill the target and
 * centred -- from whatever the target's size and shape. The offset constant
 * is recognised by its shape: positive half-pixel x/y margins, zero z/w.
 * RECOMP_SAFE_AREA_FILL=0 keeps the console layout. */
static void ui_fill(const PgEntry *e, float W, float H, PgConsts *pg)
{
    static int on;
    static long gen;
    const float *c;
    float ox, oy, bw, bh, sc;
    if (gen != xbox_settings_gen()) {
        gen = xbox_settings_gen();
        on = xbox_setting_int("RECOMP_SAFE_AREA_FILL", 1) != 0;
    }
    if (!on || !e || e->ui_const < 0)
        return;
    c = nv2a_vsh_constant((uint32_t)e->ui_const);
    ox = c[0] + 0.5f;
    oy = c[1] + 0.5f;
    if (c[2] != 0 || c[3] != 0 || ox < 1 || oy < 1
            || ox != floorf(ox) || oy != floorf(oy)
            || ox * 4 > W || oy * 4 > H)
        return;
    bw = W - 2 * ox;
    bh = H - 2 * oy;
    {
        /* Scaled uniformly as displayed: with a display aspect wider than
         * the buffer, a buffer pixel shows wider than it is tall, and the
         * box is narrowed by the same factor to keep its shapes. */
        float wd = xbox_DisplayAspect() > 0 ? H * xbox_DisplayAspect() : W;
        float sx;
        sc = wd / bw < H / bh ? wd / bw : H / bh;
        sx = sc * W / wd;
        pg->ui[0] = c[0];
        pg->ui[2] = sx;
        pg->ui[3] = (W - bw * sx) * 0.5f - 0.5f;
    }
    pg->ui2[0] = c[1];
    pg->ui2[2] = sc;
    pg->ui2[3] = (H - bh * sc) * 0.5f - 0.5f;
}

static ID3D11VertexShader *pg_get(void)
{
    uint64_t h = nv2a_vsh_program_hash();
    int i;
    static char body[96 * 1024];
    static char full[sizeof body + 8192];
    size_t n;
    ID3DBlob *code;
    PgEntry *e;

    for (i = 0; i < s_npg; i++)
        if (s_pg[i].hash == h) {
            s_pg_cur = &s_pg[i];
            return s_pg[i].vs;
        }
    s_pg_cur = NULL;
    if (s_npg == PG_MAX_PROGS)
        return NULL;
    e = &s_pg[s_npg++];
    e->hash = h;
    e->vs = NULL;
    e->ui_const = -1;
    s_pg_cur = e;
    n = nv2a_vsh_hlsl(body, sizeof body, NULL);
    if (!n) {
        fprintf(stderr, "  [PBGPU] vertex program %016llX not translated;"
                " it stays on the CPU\n", (unsigned long long)h);
        return NULL;
    }
    e->ui_const = ui_const_of(body);
    snprintf(full, sizeof full, "%s%s%s", k_pg_head, body, k_pg_tail);
    code = compile(full, "vs_5_0");
    if (code) {
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &e->vs);
        ID3D10Blob_Release(code);
    }
    if (getenv("RECOMP_PB_GPU_DUMP"))
        fprintf(stderr, "  [PBGPU] vertex program %016llX:\n%s\n",
                (unsigned long long)h, body);
    return e->vs;
}

static int pg_init(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;

    if (s_attr_srv)
        return 1;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = PG_ATTR_F4 * 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_attr_buf)))
        return 0;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_UNKNOWN;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = PG_ATTR_F4;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_attr_buf,
                                                    &vd, &s_attr_srv)))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = PG_IDX_U32 * 4;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_idx_buf)))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (sizeof(PgConsts) + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_pg_cb)))
        return 0;
    bd.ByteWidth = NV2A_VSH_CONSTANTS * 16;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_const_cb)))
        return 0;
    return 1;
}

/* Copy n units into a dynamic ring, wrapping (and discarding) when full.
 * Returns the unit offset written at, or -1. */
static long ring_put(ID3D11Buffer *b, uint32_t *pos, uint32_t cap, uint32_t unit,
                     const void *src, uint32_t n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t at;
    if (n > cap)
        return -1;
    if (*pos + n > cap)
        *pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
            *pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return -1;
    memcpy((uint8_t *)m.pData + (size_t)*pos * unit, src, (size_t)n * unit);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    at = *pos;
    *pos += n;
    return (long)at;
}

int pbgpu_draw_program(const PbGpuState *st, const float *attr, uint32_t nverts,
                       uint32_t stride, const int32_t slot[16],
                       const float vdef[16][4], const uint32_t *idx, uint32_t nidx,
                       const float fog[4])
{
    Surface *s;
    Zeta *z;
    ID3D11VertexShader *vs;
    D3D11_MAPPED_SUBRESOURCE m;
    PgConsts pg;
    long aoff, ioff;
    int k, ok = 0;

    if (!nidx || !nverts || !pbgpu_enabled())
        return 1;
    EnterCriticalSection(&s_lock);
    if (!pg_init() || !(vs = pg_get()))
        goto out;                          /* caller keeps it on the CPU */
    ok = 1;
    if (s_cap.active && s_flip == s_cap.flip && s_cap.log)
        fprintf(s_cap.log, "     draw %u indices %u verts on GPU program %016llX"
                " blend %u %X/%X ztest %u stages %05X t0 %08X\n", nidx, nverts,
                (unsigned long long)nv2a_vsh_program_hash(), st->blend_enable,
                st->blend_sfactor, st->blend_dfactor, st->depth_test,
                st->use_rc ? st->rc.stage_program : 0xFFFFF,
                st->tex[0].valid ? st->tex[0].addr : 0);
    if (s_cap.active && s_flip == s_cap.flip && s_cap.log) {
        /* The viewport a program maps to, where Spartan keeps it (c[0]
         * offset, c[1] scale) -- a pass drawn at a different scale shows. */
        const float *c0 = nv2a_vsh_constant(0), *c1 = nv2a_vsh_constant(1);
        fprintf(s_cap.log, "          c0 (%.1f %.1f %.1f) c1 (%.1f %.1f %.1f) clip %u,%u %ux%u"
                " surf %08X pitch %u z %08X fmt %08X\n",
                c0[0], c0[1], c0[2], c1[0], c1[1], c1[2],
                st->clip_x, st->clip_y, st->clip_w, st->clip_h,
                st->surf_addr, st->surf_pitch, st->zeta_addr, st->surf_format);
    }
    if (s_cap.active && s_flip == s_cap.flip && s_cap.log && nverts <= 8) {
        /* Small draws (screen quads): their inputs and the constants a
         * screen-space program usually scales them by. */
        const float *c14 = nv2a_vsh_constant(14), *c15 = nv2a_vsh_constant(15);
        uint32_t k;
        for (k = 0; k < nverts && k < 4; k++) {
            uint32_t a;
            fprintf(s_cap.log, "          vert %u:", k);
            for (a = 0; a < 16; a++)
                if (slot[a] >= 0)
                    fprintf(s_cap.log, " v%u(%.1f %.1f %.1f %.1f)", a,
                            attr[(k * stride + (uint32_t)slot[a]) * 4 + 0],
                            attr[(k * stride + (uint32_t)slot[a]) * 4 + 1],
                            attr[(k * stride + (uint32_t)slot[a]) * 4 + 2],
                            attr[(k * stride + (uint32_t)slot[a]) * 4 + 3]);
            fprintf(s_cap.log, "\n");
        }
        fprintf(s_cap.log, "          c14 (%.3f %.3f %.3f %.3f) c15 (%.3f %.3f %.3f %.3f)\n",
                c14[0], c14[1], c14[2], c14[3], c15[0], c15[1], c15[2], c15[3]);
    }
    if (s_cap.active && s_flip == s_cap.flip && s_cap.log && nverts > 8 && slot[0] >= 0) {
        /* Larger draws: the extent of their positions (text, HUD). */
        float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
        uint32_t k, c;
        for (k = 0; k < nverts; k++)
            for (c = 0; c < 4; c++) {
                float v = attr[(k * stride + (uint32_t)slot[0]) * 4 + c];
                if (v < lo[c]) lo[c] = v;
                if (v > hi[c]) hi[c] = v;
            }
        fprintf(s_cap.log, "          v0 min (%.1f %.1f %.1f %.1f) max (%.1f %.1f %.1f %.1f)\n",
                lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3]);
        {
            /* The program and its constants, once per program per capture. */
            char path[300];
            FILE *fp;
            snprintf(path, sizeof path, "%sprog_%016llX.txt", s_cap.dir,
                     (unsigned long long)nv2a_vsh_program_hash());
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES
                    && (fp = fopen(path, "w")) != NULL) {
                static char body[96 * 1024];
                int i;
                if (nv2a_vsh_hlsl(body, sizeof body, NULL))
                    fputs(body, fp);
                for (i = 0; i < 192; i++) {
                    const float *c = nv2a_vsh_constant(i);
                    if (c[0] || c[1] || c[2] || c[3])
                        fprintf(fp, "c%d = (%g %g %g %g)\n", i, c[0], c[1], c[2], c[3]);
                }
                fclose(fp);
            }
        }
    }
    if (!draw_begin(st, &s, &z))
        goto out;
    aoff = ring_put(s_attr_buf, &s_attr_pos, PG_ATTR_F4, 16, attr, nverts * stride);
    ioff = ring_put(s_idx_buf, &s_idx_pos, PG_IDX_U32, 4, idx, nidx);
    if (aoff < 0 || ioff < 0)
        goto out;

    memset(&pg, 0, sizeof pg);
    {
        uint32_t fx, fy;
        aa_factor(st, &fx, &fy);
        pg.vp[0] = 2.0f * (float)fx / (float)s->w;
        pg.vp[1] = 2.0f * (float)fy / (float)s->h;
    }
    pg.vp[2] = 1.0f / (float)(st->zeta_format == 1 ? 0xFFFFu : 0xFFFFFFu);
    {
        uint32_t fx, fy;
        aa_factor(st, &fx, &fy);
        ui_fill(s_pg_cur, (float)s->w / (float)fx, (float)s->h / (float)fy, &pg);
    }
    memcpy(pg.fog, fog, sizeof pg.fog);
    pg.info[0] = stride;
    pg.info[1] = (uint32_t)aoff;
    for (k = 0; k < 16; k++) {
        pg.slot[k][0] = slot[k];
        memcpy(pg.vdef[k], vdef[k], sizeof pg.vdef[k]);
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_pg_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pg, sizeof pg);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_pg_cb, 0);
    }
    if (s_const_gen_up != nv2a_vsh_constant_gen()
        && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_const_cb, 0,
                                             D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, nv2a_vsh_constant(0), NV2A_VSH_CONSTANTS * 16);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_const_cb, 0);
        s_const_gen_up = nv2a_vsh_constant_gen();
    }
    {
        ID3D11Buffer *cbs[2] = { s_pg_cb, s_const_cb };
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_idx_buf, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, cbs);
        ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &s_attr_srv);
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, (UINT)ioff, 0);
    }
    draw_end(s);
out:
    LeaveCriticalSection(&s_lock);
    return ok;
}

void pbgpu_clear(const PbGpuState *st, uint32_t flags, uint32_t color,
                 float depth, uint32_t stencil)
{
    Surface *s;
    Zeta *z;

    if (!pbgpu_enabled())
        return;
    EnterCriticalSection(&s_lock);
    if (!(s = bind_target(st, &z)))
        goto out;
    if ((flags & 0x03) && z) {
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv, f, depth, (UINT8)stencil);
    }
    if (flags & 0xF0) {
        /* A quad through the clip rectangle and the clear's own channel mask
         * (clear_surface honours both). 16-bit surfaces store R5G6B5. */
        PbGpuState cs = *st;
        D3D11_MAPPED_SUBRESOURCE m;
        float c[4];
        if (st->surf_bpp == 2) {
            uint32_t t = color & 0xFFFF;
            color = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                  | ((t & 0x001Fu) << 3);
        }
        unpack(color, c);
        cs.blend_enable = 0;
        cs.blend_sfactor = 1; cs.blend_dfactor = 0;
        cs.blend_equation = 0x8006;
        cs.color_mask = ((flags & 0x80) ? 0x01000000u : 0) | ((flags & 0x10) ? 0x00010000u : 0)
                      | ((flags & 0x20) ? 0x00000100u : 0) | ((flags & 0x40) ? 0x00000001u : 0);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, c, sizeof c);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
        }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->rtv, NULL);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_get(&cs, cs.color_mask), NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_clear_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_clear_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
        ID3D11DeviceContext_Draw(s_ctx, 4, 0);
    }
out:
    LeaveCriticalSection(&s_lock);
}

void pbgpu_readback(uint32_t addr)
{
    Surface *s;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y;

    if (!pbgpu_enabled())
        return;
    EnterCriticalSection(&s_lock);
    s = surface_find(addr);
    if (s && s->drawn && s->staging) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s->staging,
                                         (ID3D11Resource *)surf_logical(s));
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s->staging,
                                              0, D3D11_MAP_READ, 0, &m))) {
            for (y = 0; y < s->h; y++)
                from_bgra((const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch),
                          s->bpp, s->w, guest(s->addr) + (size_t)y * s->pitch);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s->staging, 0);
        }
        s->drawn = 0;
    }
    LeaveCriticalSection(&s_lock);
}

/* ---- presenting ----------------------------------------------------------
 *
 * The finished frame goes to the window straight from the GPU: a DXGI swap
 * chain on the framebuffer window, the frame's surface drawn into it scaled
 * to the client area at the frame's own aspect ratio.
 *
 * Before this, every flip copied the frame back to guest memory -- a stall
 * until the GPU had finished it -- and the window thread converted and drew
 * it with GDI. The copy back still happens (guest memory should hold what
 * the title rendered: screenshots, dumps, old targets read as textures), but
 * asynchronously, one flip late, so it never waits (pbgpu_readback_async).
 *
 * This is also where a higher internal resolution would be resolved to the
 * window. RECOMP_PRESENT_GDI=1 keeps the old path. */
static IDXGISwapChain1        *s_swap;
static ID3D11RenderTargetView *s_swap_rtv;
static UINT                    s_swap_w, s_swap_h;
static ID3D11VertexShader     *s_blit_vs;
static ID3D11PixelShader      *s_blit_ps;
static ID3D11SamplerState     *s_blit_smp;
static ID3D11Buffer           *s_blit_cb;
static int                     s_present_state;   /* 0 untried, 1 on, -1 off */

static const char k_blit_vs[] =
"struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO main(uint id : SV_VertexID) { VO o;\n"
"  float2 t = float2((id << 1) & 2, id & 2);\n"
"  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }\n";
/* foot.xy: one output pixel's footprint in the source, as uv; foot.z taps
 * per axis averaged over it (1: plain bilinear); foot.w 1 keeps alpha. A
 * scaled frame shown in a smaller window, or filtered down for guest
 * memory, is averaged rather than point-sampled -- no shimmer. */
static const char k_blit_ps[] =
"cbuffer B : register(b0) { float4 uvmax; float4 foot; };\n"
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
"  float2 c = uv * uvmax.xy; int n = (int)foot.z; float4 acc = 0; int i, j;\n"
"  if (n <= 1) acc = t.Sample(s, c);\n"
"  else { for (j = 0; j < n; j++) for (i = 0; i < n; i++)\n"
"      acc += t.Sample(s, c + ((float2(i, j) + 0.5) / n - 0.5) * foot.xy);\n"
"    acc /= n * n; }\n"
"  return float4(acc.rgb, foot.w != 0 ? acc.a : 1); }\n";

/* The blit shaders, sampler and constants, made once, for the present and
 * for surf_logical. */
static int blit_init(void)
{
    D3D11_SAMPLER_DESC smd;
    D3D11_BUFFER_DESC bd;
    ID3DBlob *code;

    if (s_blit_vs && s_blit_ps && s_blit_smp && s_blit_cb)
        return 1;
    if (!s_blit_vs && (code = compile(k_blit_vs, "vs_5_0")) != NULL) {
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &s_blit_vs);
        ID3D10Blob_Release(code);
    }
    if (!s_blit_ps && (code = compile(k_blit_ps, "ps_5_0")) != NULL) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &s_blit_ps);
        ID3D10Blob_Release(code);
    }
    if (!s_blit_smp) {
        memset(&smd, 0, sizeof smd);
        smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &smd, &s_blit_smp);
    }
    if (!s_blit_cb) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_blit_cb);
    }
    return s_blit_vs && s_blit_ps && s_blit_smp && s_blit_cb;
}

static void blit_consts(const float uvmax[2], float fx, float fy, float taps, float alpha)
{
    D3D11_MAPPED_SUBRESOURCE m;
    float c[8];
    c[0] = uvmax[0]; c[1] = uvmax[1]; c[2] = 0; c[3] = 0;
    c[4] = fx; c[5] = fy; c[6] = taps; c[7] = alpha;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_blit_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_blit_cb, 0);
    }
}

/* The surface at its guest size: the texture itself when unscaled, else a
 * box-filtered copy -- for everything that goes back to guest memory. */
static ID3D11Texture2D *surf_logical(Surface *s)
{
    D3D11_VIEWPORT vp;
    D3D11_RECT r;
    float one[2] = { 1, 1 }, taps;
    ID3D11ShaderResourceView *none = NULL;

    if (s->pw == s->w && s->ph == s->h)
        return s->tex;
    if (!blit_init())
        return s->tex;
    if (!s->down) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = s->w; td.Height = s->h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->down)))
            return s->tex;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->down, NULL, &s->down_rtv);
    }
    taps = ceilf(s->sx > s->sy ? s->sx : s->sy);
    if (taps > 8) taps = 8;
    blit_consts(one, 1.0f / (float)s->w, 1.0f / (float)s->h, taps, 1);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->down_rtv, NULL);
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)s->w; vp.Height = (float)s->h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    r.left = r.top = 0; r.right = (LONG)s->w; r.bottom = (LONG)s->h;
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    return s->down;
}

static int present_init(HWND hwnd)
{
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D3D11_SAMPLER_DESC smd;
    D3D11_BUFFER_DESC bd;
    ID3DBlob *code;
    HRESULT hr;

    if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd)))
        return 0;
    hr = IDXGIDevice_GetAdapter(dd, &ad);
    IDXGIDevice_Release(dd);
    if (FAILED(hr))
        return 0;
    hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    IDXGIAdapter_Release(ad);
    if (FAILED(hr))
        return 0;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_NONE;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, hwnd, &sd,
                                              NULL, NULL, &s_swap);
    if (SUCCEEDED(hr))   /* the window's own Alt+Enter is not ours to give */
        IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(fac);
    if (FAILED(hr)) {
        fprintf(stderr, "  [PBGPU] swap chain not created (0x%08lX); presenting"
                        " through GDI\n", hr);
        return 0;
    }
    if (!blit_init())
        return 0;
    fprintf(stderr, "  [PBGPU] presenting through a D3D11 swap chain\n");
    {
        extern void xbox_overlay_init(HWND, ID3D11Device *, ID3D11DeviceContext *);
        xbox_overlay_init(hwnd, s_dev, s_ctx);
    }
    return 1;
}

int pbgpu_present(uint32_t addr, uint32_t width, uint32_t height)
{
    extern void *xbox_FramebufferWindowHwnd(void);
    extern void xbox_FramebufferWindowGpuOwned(int on);
    HWND hwnd;
    RECT rc;
    UINT cw, ch;
    Surface *s;
    int ok = 0;

    if (!pbgpu_enabled() || s_present_state < 0)
        return 0;
    hwnd = (HWND)xbox_FramebufferWindowHwnd();
    if (!hwnd)
        return 0;                          /* window not up yet */
    EnterCriticalSection(&s_lock);
    if (s_present_state == 0) {
        if (getenv("RECOMP_PRESENT_GDI") || !present_init(hwnd)) {
            s_present_state = -1;
            goto out;
        }
        s_present_state = 1;
        xbox_FramebufferWindowGpuOwned(1);
    }
    if (!(s = surface_find(addr)) || !width || !height)
        goto out;
    GetClientRect(hwnd, &rc);
    cw = (UINT)(rc.right - rc.left);
    ch = (UINT)(rc.bottom - rc.top);
    if (!cw || !ch)
        goto out_ok;                       /* minimised: nothing to show */
    if (cw != s_swap_w || ch != s_swap_h || !s_swap_rtv) {
        ID3D11Texture2D *bb = NULL;
        if (s_swap_rtv) {
            ID3D11RenderTargetView_Release(s_swap_rtv);
            s_swap_rtv = NULL;
        }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_swap, 0, cw, ch,
                                                 DXGI_FORMAT_UNKNOWN, 0)))
            goto out;
        if (FAILED(IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D,
                                             (void **)&bb)))
            goto out;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_swap_rtv);
        ID3D11Texture2D_Release(bb);
        s_swap_w = cw;
        s_swap_h = ch;
    }
    {
        static const float black[4] = { 0, 0, 0, 1 };
        D3D11_VIEWPORT vp;
        D3D11_MAPPED_SUBRESOURCE m;
        float uv[4];
        float sw = (float)width, sh = (float)height;
        float aspect = xbox_DisplayAspect() > 0 ? xbox_DisplayAspect() : sw / sh;
        ID3D11Buffer *cb = s_blit_cb;

        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_swap_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_swap_rtv, black);
        /* The largest box of the display aspect in the window. */
        vp.Height = (float)cw / aspect < (float)ch ? (float)cw / aspect : (float)ch;
        vp.Width = vp.Height * aspect;
        vp.TopLeftX = ((float)cw - vp.Width) * 0.5f;
        vp.TopLeftY = ((float)ch - vp.Height) * 0.5f;
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        {
            D3D11_RECT r = { 0, 0, (LONG)cw, (LONG)ch };
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
        }
        uv[0] = sw / (float)s->w;
        uv[1] = sh / (float)s->h;
        uv[2] = uv[3] = 0;
        {
            /* Averaged over each window pixel when the scaled frame is
             * bigger than the window. */
            float over = (sw * s->sx) / vp.Width;
            blit_consts(uv, uv[0] / vp.Width, uv[1] / vp.Height, over > 1.25f ? 4.0f : 1.0f, 0);
        }
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        {
            ID3D11ShaderResourceView *none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        }
        /* The settings overlay, over the frame (full-window viewport). */
        {
            extern void xbox_overlay_render(void);
            D3D11_VIEWPORT full = { 0, 0, (float)cw, (float)ch, 0, 1 };
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &full);
            xbox_overlay_render();
        }
        /* No vsync wait by default: the executor already paces flips to
         * 60 Hz. RECOMP_VSYNC=1 waits for the display's -- no tearing, at
         * the cost of a frame of latency. */
        {
            static long gen;
            static UINT interval;
            if (gen != xbox_settings_gen()) {
                gen = xbox_settings_gen();
                interval = xbox_setting_int("RECOMP_VSYNC", 0) ? 1 : 0;
            }
            IDXGISwapChain1_Present(s_swap, interval, 0);
        }
    }
out_ok:
    ok = 1;
out:
    LeaveCriticalSection(&s_lock);
    return ok;
}

/* The flip's copy back to guest memory, one flip late: copy this frame to
 * staging now, and map the copy started at the previous flip -- which the
 * GPU has long finished -- so nothing waits. */
void pbgpu_readback_async(uint32_t addr)
{
    static Surface *pending;
    D3D11_MAPPED_SUBRESOURCE m;
    Surface *s;
    uint32_t y;

    if (!pbgpu_enabled())
        return;
    EnterCriticalSection(&s_lock);
    if (pending && pending->staging) {
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)pending->staging,
                                              0, D3D11_MAP_READ, 0, &m))) {
            for (y = 0; y < pending->h; y++)
                from_bgra((const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch),
                          pending->bpp, pending->w,
                          guest(pending->addr) + (size_t)y * pending->pitch);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)pending->staging, 0);
        }
        pending = NULL;
    }
    s = surface_find(addr);
    if (s && s->drawn && s->staging) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s->staging,
                                         (ID3D11Resource *)surf_logical(s));
        s->drawn = 0;
        pending = s;
    }
    LeaveCriticalSection(&s_lock);
}

void pbgpu_flip(void)
{
    cap_poll();
    s_flip++;
}

#else  /* !_WIN32: no D3D11; the software rasteriser stays in charge. */

int  pbgpu_enabled(void) { return 0; }
void pbgpu_draw(const PbGpuState *s, const PbGpuVertex *v, uint32_t n) { (void)s; (void)v; (void)n; }
int pbgpu_draw_program(const PbGpuState *st, const float *a, uint32_t nv, uint32_t stride,
                       const int32_t slot[16], const float vdef[16][4],
                       const uint32_t *idx, uint32_t ni, const float fog[4])
{ (void)st; (void)a; (void)nv; (void)stride; (void)slot; (void)vdef; (void)idx; (void)ni; (void)fog; return 0; }
void pbgpu_clear(const PbGpuState *s, uint32_t f, uint32_t c, float d, uint32_t t)
{ (void)s; (void)f; (void)c; (void)d; (void)t; }
void pbgpu_readback(uint32_t a) { (void)a; }
void pbgpu_readback_async(uint32_t a) { (void)a; }
int  pbgpu_present(uint32_t a, uint32_t w, uint32_t h) { (void)a; (void)w; (void)h; return 0; }
void pbgpu_flip(void) {}

#endif
