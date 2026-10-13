/**
 * D3D11 back end for the pushbuffer executor (nv2a_pb_exec.c).
 *
 * The executor decodes the NV097 command stream and runs vertex programs on
 * the CPU (nv2a_vsh_interp.c); what it cannot do fast is the pixels -- every
 * one through the register combiners in software, a few frames a second on a
 * title that composites full-screen glow passes. With RECOMP_PB_GPU=1 the
 * transformed triangles are handed here instead and drawn by D3D11:
 *
 *   - vertices arrive in the executor's screen space (the XDK's viewport
 *     epilogue already applied, w kept) and are taken back to clip space in
 *     a fixed vertex shader, so the GPU clips and interpolates them;
 *   - each register-combiner setup becomes a generated pixel shader that
 *     mirrors nv2a_rc_eval term for term, including the stage texture modes,
 *     fog, clip planes and the alpha test;
 *   - colour surfaces are render targets keyed by guest address, so a pass
 *     that samples an earlier pass (glow, blur, composite) reads it on the
 *     GPU; textures are decoded with the executor's own texel fetch;
 *   - at a flip the presented surface is copied back into guest memory, so
 *     the framebuffer window, the frame dumps and anything else that reads
 *     the framebuffer keep working unchanged.
 *
 * Portable interface, D3D11 implementation; without RECOMP_PB_GPU nothing
 * here runs and the software rasteriser is untouched.
 */
#ifndef XBOXRECOMP_NV2A_PB_GPU_H
#define XBOXRECOMP_NV2A_PB_GPU_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* One transformed vertex: the executor's Nv2aVshOutput minus what the pixel
 * stage does not read, plus the per-vertex fog factor it computes. */
typedef struct {
    float pos[4];             /* screen x, y, z (depth units), clip w */
    float d0[4], d1[4];       /* r, g, b, a */
    float tex[4][4];          /* per texture stage, as the program wrote it */
    float fog;                /* fog factor, 1 = no fog */
    float pad[3];
} PbGpuVertex;

/* Reads one texel of a texture as a D3DCOLOR (0xAARRGGBB); the executor's
 * sample_tex. `face_offset` selects a cube face. Returns 0 if unusable. */
typedef int (*PbGpuTexelFn)(const void *tex, uint32_t face_offset,
                            uint32_t u, uint32_t v, uint32_t *argb);

typedef struct {
    int      valid;
    uint32_t addr;            /* guest address of texel (0,0) */
    uint32_t color;           /* NV097 colour format */
    uint32_t width, height, pitch, levels, palette;
    int      cube;
    int      normalized;      /* coordinates arrive 0..1 (swizzled / DXT) */
    int      no_alpha;        /* X8 formats: alpha reads 1 */
    uint32_t addr_u, addr_v, filter;
    uint32_t bytes;           /* guest bytes the texture (all faces) spans */
    uint32_t face_stride;
    const void   *src;        /* executor's Texture, for `fetch` */
    PbGpuTexelFn  fetch;
} PbGpuTexture;

typedef struct {
    /* Colour surface: guest address (resolved), pitch, bytes per pixel. */
    uint32_t surf_addr, surf_pitch, surf_bpp;
    uint32_t clip_x, clip_y, clip_w, clip_h;
    /* Depth: guest address (0 = none), 1 = Z16, 2 = Z24S8. */
    uint32_t zeta_addr, zeta_format;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t blend_enable, blend_sfactor, blend_dfactor, blend_equation;
    uint32_t blend_color, color_mask;
    uint32_t alpha_test, alpha_func, alpha_ref;
    int      use_rc;          /* combiners programmed; else d0 * texture 0 */
    Nv2aCombiner rc;
    uint32_t clip_plane_mode;
    float    fog_color[4];    /* r, g, b, a */
    PbGpuTexture tex[4];
} PbGpuState;

/* RECOMP_PB_GPU set and a D3D11 device came up. Cheap after the first call. */
int  pbgpu_enabled(void);

/* Triangle list, three vertices per triangle. */
void pbgpu_draw(const PbGpuState *st, const PbGpuVertex *v, uint32_t count);

/* NV097_CLEAR_SURFACE: `flags` as the method's parameter (Z 0x01,
 * stencil 0x02, R G B A 0x10..0x80). `color` is the surface-format clear
 * value, `depth` 0..1, `stencil` 0..255. Honours the clip rectangle. */
void pbgpu_clear(const PbGpuState *st, uint32_t flags, uint32_t color,
                 float depth, uint32_t stencil);

/* Copy the GPU's copy of the surface at `addr` back into guest memory, if
 * it has one. Called at a flip for the presented surface. */
void pbgpu_readback(uint32_t addr);

/* A flip happened: ages render-target aliases and texture checks. */
void pbgpu_flip(void);

#endif
