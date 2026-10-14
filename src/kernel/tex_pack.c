/*
 * Texture packs: dump the title's textures, replace them with others.
 *
 * A texture is named by what it looks like, not where it lives: a 64-bit
 * hash of its decoded pixels, with its format and size, so the same image
 * gets the same name in every run whatever address it was loaded to and
 * whatever palette arrangement produced it. Files are
 *
 *     <hash>_<format>_<w>x<h>.png
 *
 * RECOMP_TEXPACK_DUMP=1 writes each texture seen, once, to
 * textures/dump/; whatever is in textures/replace/ under the same hash (any
 * size, the rest of the name is free) is used in its place, unless
 * RECOMP_TEXPACK=0. Both directories sit beside the working directory.
 *
 * Neither path may stall the frame: PNG encoding and decoding run on their
 * own threads. A replacement not yet decoded is reported pending; the
 * renderer shows the original meanwhile and asks again next frame. Decoded
 * pixels are handed over once and freed by the caller after the upload.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

extern int xbox_setting_int(const char *key, int def);

#define DUMP_DIR    "textures\\dump\\"
#define REPL_DIR    "textures\\replace\\"

/* Button prompts: one icon set per device, drawn onto the textures that draw
 * controller buttons as they load (texpack_prompt, below).
 * RECOMP_BUTTON_PROMPTS picks the set: "auto" (the default) follows the
 * device last used -- the title's own art for an Xbox pad, the PlayStation
 * set for a DualShock/DualSense, the keyboard's for keys and mouse;
 * "original", "xbox", "playstation" and "keyboard" fix it. Applies with the
 * HD pack on or off. (Ready-made replacements under a per-set folder, salted
 * keys, were the first way; s_prompt_dir lists none now.) */
enum { PROMPT_ORIGINAL, PROMPT_XBOX, PROMPT_PLAYSTATION, PROMPT_KEYBOARD, PROMPT_STYLES };
static const char *const s_prompt_dir[PROMPT_STYLES] = { NULL, NULL, NULL, NULL };
#define PROMPT_KEY(k, style) ((k) ^ (0xB077011CDA5E0000ull * (uint64_t)(style)))
static int s_prompt;            /* the style in force */

/* ---- a small open-addressing set of 64-bit keys ---------------------------- */

typedef struct {
    uint64_t key;          /* 0: empty */
    int      state;        /* replace: 1 on disk, 2 loading, 3 ready, 4 failed */
    char    *path;
    uint32_t *px;          /* ready: BGRA, handed to the renderer */
    uint32_t w, h;
} Slot;

typedef struct { Slot *s; uint32_t cap, n; } Set;

static Slot *set_find(Set *t, uint64_t key, int add)
{
    uint32_t i, mask;
    if (!key)
        key = 1;
    if (add && (t->n + 1) * 2 > t->cap) {
        Set g;
        uint32_t j;
        g.cap = t->cap ? t->cap * 2 : 4096;
        g.n = 0;
        g.s = (Slot *)calloc(g.cap, sizeof(Slot));
        if (!g.s)
            return NULL;
        for (j = 0; j < t->cap; j++)
            if (t->s[j].key) {
                Slot *d;
                for (i = (uint32_t)(t->s[j].key * 0x9E3779B97F4A7C15ull >> 40) & (g.cap - 1);
                     g.s[i].key; i = (i + 1) & (g.cap - 1))
                    ;
                d = &g.s[i];
                *d = t->s[j];
                g.n++;
            }
        free(t->s);
        *t = g;
    }
    if (!t->cap)
        return NULL;
    mask = t->cap - 1;
    for (i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & mask; t->s[i].key; i = (i + 1) & mask)
        if (t->s[i].key == key)
            return &t->s[i];
    if (!add)
        return NULL;
    t->s[i].key = key;
    t->n++;
    return &t->s[i];
}

/* ---- state ------------------------------------------------------------------ */

static SRWLOCK s_lock = SRWLOCK_INIT;
static Set s_dumped, s_repl;
static int s_dump = -1, s_use = -1;

typedef struct Job {
    struct Job *next;
    int dump;              /* 1: write px to path; 0: load slot key */
    uint64_t key;
    char path[MAX_PATH];
    uint32_t *px, w, h;
} Job;

static Job *s_head, *s_tail;
static HANDLE s_event;
static volatile LONG s_dumped_count, s_loaded_count, s_inflight;

static void push(Job *j)
{
    InterlockedIncrement(&s_inflight);
    AcquireSRWLockExclusive(&s_lock);
    j->next = NULL;
    if (s_tail) s_tail->next = j; else s_head = j;
    s_tail = j;
    ReleaseSRWLockExclusive(&s_lock);
    SetEvent(s_event);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Job *j;
        WaitForSingleObject(s_event, INFINITE);
        for (;;) {
            AcquireSRWLockExclusive(&s_lock);
            j = s_head;
            if (j && !(s_head = j->next))
                s_tail = NULL;
            ReleaseSRWLockExclusive(&s_lock);
            if (!j)
                break;
            if (j->dump) {
                /* BGRA -> RGBA in place; the buffer is ours. */
                uint32_t i, n = j->w * j->h;
                for (i = 0; i < n; i++) {
                    uint32_t c = j->px[i];
                    j->px[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
                }
                if (stbi_write_png(j->path, (int)j->w, (int)j->h, 4, j->px, (int)j->w * 4))
                    InterlockedIncrement(&s_dumped_count);
                free(j->px);
            } else {
                int w = 0, h = 0, n = 0;
                Slot *sl;
                char path[MAX_PATH];
                AcquireSRWLockShared(&s_lock);
                sl = set_find(&s_repl, j->key, 0);
                path[0] = 0;
                if (sl && sl->path)
                    strncpy(path, sl->path, sizeof path - 1);
                ReleaseSRWLockShared(&s_lock);
                {
                    stbi_uc *rgba = path[0] ? stbi_load(path, &w, &h, &n, 4) : NULL;
                    uint32_t *px = (uint32_t *)rgba, i;
                    if (px)
                        for (i = 0; i < (uint32_t)(w * h); i++) {
                            uint32_t c = px[i];
                            px[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
                        }
                    AcquireSRWLockExclusive(&s_lock);
                    sl = set_find(&s_repl, j->key, 0);
                    if (sl) {
                        if (px && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
                            sl->px = px; sl->w = (uint32_t)w; sl->h = (uint32_t)h;
                            sl->state = 3;
                            px = NULL;
                            InterlockedIncrement(&s_loaded_count);
                        } else {
                            sl->state = 4;
                            fprintf(stderr, "  [TEXPACK] could not read %s\n", path);
                        }
                    }
                    ReleaseSRWLockExclusive(&s_lock);
                    if (px)
                        stbi_image_free(px);
                }
            }
            free(j);
            InterlockedDecrement(&s_inflight);
        }
    }
}

static void init(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int n = 0;

    if (s_dump >= 0)
        return;
    s_dump = xbox_setting_int("RECOMP_TEXPACK_DUMP", 0) != 0;
    s_use = xbox_setting_int("RECOMP_TEXPACK", 1) != 0;
    s_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    /* What an earlier run already dumped is not written again. */
    if ((f = FindFirstFileA(DUMP_DIR "*.png", &fd)) != INVALID_HANDLE_VALUE) {
        do {
            uint64_t k = _strtoui64(fd.cFileName, NULL, 16);
            if (k)
                set_find(&s_dumped, k, 1);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    if (s_dump) {
        CreateDirectoryA("textures", NULL);
        CreateDirectoryA(DUMP_DIR, NULL);
        fprintf(stderr, "  [TEXPACK] dumping new textures to %s (%u already there)\n",
                DUMP_DIR, s_dumped.n);
    }
    /* Listed even when off: the overlay can turn the pack on later. */
    {
        int style;
        for (style = 0; style < PROMPT_STYLES; style++) {
            const char *dir = style ? s_prompt_dir[style] : REPL_DIR;
            char pat[MAX_PATH];
            if (!dir)
                continue;
            n = 0;
            snprintf(pat, sizeof pat, "%s*.png", dir);
            if ((f = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
                continue;
            do {
                uint64_t k = _strtoui64(fd.cFileName, NULL, 16);
                Slot *sl;
                if (!k || !(sl = set_find(&s_repl, style ? PROMPT_KEY(k, style) : k, 1)))
                    continue;
                if (!sl->path) {
                    size_t len = strlen(dir) + strlen(fd.cFileName) + 1;
                    sl->path = (char *)malloc(len);
                    if (sl->path)
                        snprintf(sl->path, len, "%s%s", dir, fd.cFileName);
                    sl->state = 1;
                    n++;
                }
            } while (FindNextFileA(f, &fd));
            FindClose(f);
            if (n)
                fprintf(stderr, "  [TEXPACK] %d replacement texture(s) in %s\n", n, dir);
        }
    }
}

/* The prompt style RECOMP_BUTTON_PROMPTS asks for now. */
static int prompt_style(void)
{
    extern const char *xbox_setting_get(const char *key);
    extern int xbox_InputActiveKind(void);
    const char *v = xbox_setting_get("RECOMP_BUTTON_PROMPTS");
    if (v && !_stricmp(v, "original"))
        return PROMPT_ORIGINAL;
    if (v && !_stricmp(v, "xbox"))
        return PROMPT_XBOX;
    if (v && !_stricmp(v, "playstation"))
        return PROMPT_PLAYSTATION;
    if (v && !_stricmp(v, "keyboard"))
        return PROMPT_KEYBOARD;
    switch (xbox_InputActiveKind()) {
    case 1: return PROMPT_PLAYSTATION;
    case 2: return PROMPT_KEYBOARD;
    }
    return PROMPT_ORIGINAL;
}

/* The texture's name: its decoded pixels, format and size. */
uint64_t texpack_key(const uint32_t *px, uint32_t w, uint32_t h, uint32_t fmt)
{
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)fmt << 40) ^ ((uint64_t)w << 20) ^ h;
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i++) {
        x ^= px[i];
        x *= 0x100000001B3ull;
        x ^= x >> 29;
    }
    return x ? x : 1;
}

int texpack_dump_enabled(void);

/* Queue one texture for textures/dump/, unless it has been already. */
static void dump_queue(uint64_t key, const uint32_t *px, uint32_t w, uint32_t h, uint32_t fmt)
{
    Slot *sl;
    Job *j;
    AcquireSRWLockExclusive(&s_lock);
    sl = set_find(&s_dumped, key, 0);
    if (!sl)
        set_find(&s_dumped, key, 1);
    ReleaseSRWLockExclusive(&s_lock);
    if (sl || !(j = (Job *)calloc(1, sizeof *j)))
        return;
    if (!(j->px = (uint32_t *)malloc((size_t)w * h * 4))) {
        free(j);
        return;
    }
    memcpy(j->px, px, (size_t)w * h * 4);
    j->dump = 1;
    j->w = w; j->h = h;
    snprintf(j->path, sizeof j->path, DUMP_DIR "%016llX_%02X_%ux%u.png",
             (unsigned long long)key, fmt, w, h);
    push(j);
}

/* For the offline extractor: dump regardless of the setting. */
void texpack_dump_force(uint64_t key, const uint32_t *px, uint32_t w, uint32_t h, uint32_t fmt)
{
    init();
    dump_queue(key, px, w, h, fmt);
}

/* Wait until every queued PNG is written. */
void texpack_flush(void)
{
    while (InterlockedCompareExchange(&s_inflight, 0, 0))
        Sleep(20);
}

/* A texture as it is drawn: dumped when dumping while playing is on. */
void texpack_dump(uint64_t key, const uint32_t *px, uint32_t w, uint32_t h, uint32_t fmt)
{
    if (texpack_dump_enabled())
        dump_queue(key, px, w, h, fmt);
}

/* 1: *px (BGRA, caller frees with texpack_free) is the replacement;
 * -1: there is one, being decoded -- ask again; 0: none. */
int texpack_lookup(uint64_t key, uint32_t **px, uint32_t *w, uint32_t *h)
{
    Slot *sl;
    int r = 0, load = 0;
    init();
    if (!s_repl.n)
        return 0;
    AcquireSRWLockExclusive(&s_lock);
    sl = NULL;
    if (s_prompt && (sl = set_find(&s_repl, PROMPT_KEY(key, s_prompt), 0)) != NULL)
        key = PROMPT_KEY(key, s_prompt);
    else if (s_use)
        sl = set_find(&s_repl, key, 0);
    if (sl) {
        switch (sl->state) {
        case 1:                       /* on disk: start decoding */
            sl->state = 2;
            load = 1;
            r = -1;
            break;
        case 2:
            r = -1;
            break;
        case 3:                       /* ready: hand over, back to on-disk */
            *px = sl->px; *w = sl->w; *h = sl->h;
            sl->px = NULL;
            sl->state = 1;
            r = 1;
            break;
        default:
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (load) {
        Job *j = (Job *)calloc(1, sizeof *j);
        if (j) {
            j->key = key;
            push(j);
        }
    }
    return r;
}

/* ---- button prompts, drawn onto the title's textures as they load ---------
 *
 * prompts\layout.txt (beside the working directory) names the textures
 * that draw controller buttons, by their pack hash, and where each button
 * sits in them; prompts\<set>\<button>.png are the icons, and
 * prompts\<set>\<HASH>_<button>.png one texture's own (the menus' OK and
 * Back are Enter and Backspace on the keyboard). Each sprite is found by the
 * original's alpha in its search box, cleared, and the icon drawn centred on
 * it, scaled uniformly to the sprite's height and at most its width plus the
 * layout's extra pixels. Onto the HD pack's version when there is one, else
 * the original at four times its size, so the icons are sharp either way.
 * Nothing of the title's is shipped: only the icons and where they go. */

#define PROMPT_ICONS "prompts\\"

typedef struct { char button[8]; int l, t, r, b, grow; } PSprite;
typedef struct { uint64_t key; int n; PSprite s[16]; } PTex;
static PTex s_ptex[8];
static int s_nptex = -1;
static const char *const s_prompt_set[PROMPT_STYLES] = { NULL, "xbox", "playstation", "keyboard" };

static void prompt_layout(void)
{
    FILE *f;
    char line[256];
    PTex *cur = NULL;
    s_nptex = 0;
    if (!(f = fopen(PROMPT_ICONS "layout.txt", "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long k;
        PSprite sp;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "texture %llx", &k) == 1) {
            cur = s_nptex < 8 ? &s_ptex[s_nptex++] : NULL;
            if (cur) {
                cur->key = k;
                cur->n = 0;
            }
        } else if (cur && cur->n < 16 && sscanf(line, "%7s %d %d %d %d %d", sp.button,
                                                &sp.l, &sp.t, &sp.r, &sp.b, &sp.grow) == 6) {
            cur->s[cur->n++] = sp;
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXPACK] button prompts: %d texture(s) in %slayout.txt\n",
            s_nptex, PROMPT_ICONS);
}

/* RGBA bytes at (x, y) in pixel units, bilinear, premultiplied. */
static void icon_sample(const uint8_t *img, int w, int h, float x, float y, float o[4])
{
    int x0 = (int)floorf(x - 0.5f), y0 = (int)floorf(y - 0.5f), dx, dy;
    float fx = x - 0.5f - (float)x0, fy = y - 0.5f - (float)y0;
    o[0] = o[1] = o[2] = o[3] = 0;
    for (dy = 0; dy < 2; dy++)
        for (dx = 0; dx < 2; dx++) {
            int xi = x0 + dx, yi = y0 + dy;
            float wt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy), a;
            const uint8_t *p;
            xi = xi < 0 ? 0 : xi >= w ? w - 1 : xi;
            yi = yi < 0 ? 0 : yi >= h ? h - 1 : yi;
            p = img + ((size_t)yi * w + xi) * 4;
            a = p[3] / 255.0f * wt;
            o[0] += p[0] * a; o[1] += p[1] * a; o[2] += p[2] * a; o[3] += a;
        }
}

/* The icon over dst (BGRA, straight alpha) at x0, y0, scaled to dw x dh:
 * 4x4 samples a pixel, alpha "over". */
static void icon_draw(uint32_t *dst, uint32_t W, uint32_t H, const uint8_t *img, int iw, int ih,
                      int x0, int y0, int dw, int dh)
{
    int i, j, s, t;
    for (j = 0; j < dh; j++) {
        int y = y0 + j;
        if (y < 0 || y >= (int)H)
            continue;
        for (i = 0; i < dw; i++) {
            int x = x0 + i;
            float c[4] = { 0, 0, 0, 0 }, o[4], da, oa, r, g, b;
            uint32_t d;
            if (x < 0 || x >= (int)W)
                continue;
            for (t = 0; t < 4; t++)
                for (s = 0; s < 4; s++) {
                    icon_sample(img, iw, ih, ((float)i + (s + 0.5f) / 4) * iw / dw,
                                ((float)j + (t + 0.5f) / 4) * ih / dh, o);
                    c[0] += o[0]; c[1] += o[1]; c[2] += o[2]; c[3] += o[3];
                }
            c[0] /= 16; c[1] /= 16; c[2] /= 16; c[3] /= 16;
            if (c[3] <= 0)
                continue;
            d = dst[(size_t)y * W + x];
            da = (d >> 24) / 255.0f;
            oa = c[3] + da * (1 - c[3]);
            r = (c[0] + ((d >> 16) & 0xFF) * da * (1 - c[3])) / oa;
            g = (c[1] + ((d >> 8) & 0xFF) * da * (1 - c[3])) / oa;
            b = (c[2] + (d & 0xFF) * da * (1 - c[3])) / oa;
            dst[(size_t)y * W + x] = ((uint32_t)(oa * 255 + 0.5f) << 24)
                | ((uint32_t)(r > 255 ? 255 : r + 0.5f) << 16)
                | ((uint32_t)(g > 255 ? 255 : g + 0.5f) << 8) | (uint32_t)(b > 255 ? 255 : b + 0.5f);
        }
    }
}

/* 1: *px (BGRA, free with texpack_free) is the texture with the buttons of
 * the set in use drawn on; 0: not a button texture, or the title's own
 * buttons are wanted. orig: the texture as decoded (BGRA). */
int texpack_prompt(uint64_t key, const uint32_t *orig, uint32_t w, uint32_t h,
                   uint32_t **px, uint32_t *ow, uint32_t *oh)
{
    const PTex *pt = NULL;
    uint32_t *base = NULL, bw = 0, bh = 0, x, y;
    const char *set;
    float k;
    int i;

    init();
    if (!s_prompt || !(set = s_prompt_set[s_prompt]))
        return 0;
    if (s_nptex < 0)
        prompt_layout();
    for (i = 0; i < s_nptex; i++)
        if (s_ptex[i].key == key)
            pt = &s_ptex[i];
    if (!pt)
        return 0;

    /* The HD pack's version, else the original at 4x. */
    if (s_use) {
        char path[MAX_PATH];
        Slot *sl;
        path[0] = 0;
        AcquireSRWLockShared(&s_lock);
        if ((sl = set_find(&s_repl, key, 0)) != NULL && sl->path)
            strncpy(path, sl->path, sizeof path - 1);
        ReleaseSRWLockShared(&s_lock);
        if (path[0]) {
            int iw, ih, n;
            uint32_t *p = (uint32_t *)stbi_load(path, &iw, &ih, &n, 4);
            if (p && iw >= (int)w && ih >= (int)h) {
                for (x = 0; x < (uint32_t)(iw * ih); x++) {
                    uint32_t c = p[x];
                    p[x] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
                }
                base = p; bw = (uint32_t)iw; bh = (uint32_t)ih;
            } else if (p) {
                stbi_image_free(p);
            }
        }
    }
    if (!base) {
        bw = w * 4; bh = h * 4;
        if (!(base = (uint32_t *)malloc((size_t)bw * bh * 4)))
            return 0;
        for (y = 0; y < bh; y++)
            for (x = 0; x < bw; x++) {
                /* Bilinear on the straight BGRA channels. */
                float sx = (x + 0.5f) / 4 - 0.5f, sy = (y + 0.5f) / 4 - 0.5f, v[4] = { 0, 0, 0, 0 };
                int x0 = (int)floorf(sx), y0 = (int)floorf(sy), dx, dy, ch;
                float fx = sx - x0, fy = sy - y0;
                for (dy = 0; dy < 2; dy++)
                    for (dx = 0; dx < 2; dx++) {
                        int xi = x0 + dx, yi = y0 + dy;
                        float wt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                        uint32_t c;
                        xi = xi < 0 ? 0 : xi >= (int)w ? (int)w - 1 : xi;
                        yi = yi < 0 ? 0 : yi >= (int)h ? (int)h - 1 : yi;
                        c = orig[(size_t)yi * w + xi];
                        for (ch = 0; ch < 4; ch++)
                            v[ch] += ((c >> (8 * ch)) & 0xFF) * wt;
                    }
                base[(size_t)y * bw + x] = (uint32_t)(v[0] + 0.5f) | ((uint32_t)(v[1] + 0.5f) << 8)
                    | ((uint32_t)(v[2] + 0.5f) << 16) | ((uint32_t)(v[3] + 0.5f) << 24);
            }
    }
    k = (float)bw / (float)w;

    for (i = 0; i < pt->n; i++) {
        const PSprite *sp = &pt->s[i];
        int l = 1 << 30, t = 1 << 30, r = -1, b = -1, iw, ih, n, dw, dh, sx, sy;
        char path[MAX_PATH];
        uint8_t *img;
        float fit, cx, cy;
        /* The sprite: what is opaque in its search box. */
        for (sy = sp->t; sy < sp->b && sy < (int)h; sy++)
            for (sx = sp->l; sx < sp->r && sx < (int)w; sx++)
                if ((orig[(size_t)sy * w + sx] >> 24) > 40) {
                    if (sx < l) l = sx;
                    if (sy < t) t = sy;
                    if (sx + 1 > r) r = sx + 1;
                    if (sy + 1 > b) b = sy + 1;
                }
        if (r < 0)
            continue;
        snprintf(path, sizeof path, PROMPT_ICONS "%s\\%016llX_%s.png", set,
                 (unsigned long long)key, sp->button);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            snprintf(path, sizeof path, PROMPT_ICONS "%s\\%s.png", set, sp->button);
        if (!(img = stbi_load(path, &iw, &ih, &n, 4)))
            continue;
        /* Clear the old sprite: a round icon on an oval gem would leave
         * the gem's ends showing. */
        for (sy = (int)(t * k + 0.5f); sy < (int)(b * k + 0.5f) && sy < (int)bh; sy++)
            for (sx = (int)(l * k + 0.5f); sx < (int)(r * k + 0.5f) && sx < (int)bw; sx++)
                base[(size_t)sy * bw + sx] = 0;
        fit = (r - l + sp->grow) * k / iw;
        if ((b - t) * k / ih < fit)
            fit = (b - t) * k / ih;
        dw = (int)(iw * fit + 0.5f);
        dh = (int)(ih * fit + 0.5f);
        cx = (l + r) / 2.0f * k;
        cy = (t + b) / 2.0f * k;
        if (dw > 0 && dh > 0)
            icon_draw(base, bw, bh, img, iw, ih, (int)floorf(cx - dw / 2.0f + 0.5f),
                      (int)floorf(cy - dh / 2.0f + 0.5f), dw, dh);
        stbi_image_free(img);
    }
    *px = base; *ow = bw; *oh = bh;
    return 1;
}

/* The prompt style as a folder name under mods\ (kernel_path.c), where a
 * title's files that name the buttons in words have that device's words;
 * NULL for the console's own. */
const char *xbox_prompt_device_dir(void)
{
    switch (prompt_style()) {
    case PROMPT_PLAYSTATION: return "playstation";
    case PROMPT_KEYBOARD:    return "keyboard";
    }
    return NULL;
}

/* RECOMP_TEXPACK and the button prompt style, live: nonzero when either
 * changed since the last call, so the renderer re-uploads its textures --
 * replaced or original. Called once a frame. */
int texpack_use_changed(void)
{
    static long gen;
    static DWORD last_check;
    extern long xbox_settings_gen(void);
    int was, now;
    init();
    /* The pad in use can change at any time; a few times a second is
     * plenty to notice. */
    if (GetTickCount() - last_check >= 250) {
        int style = prompt_style();
        last_check = GetTickCount();
        if (style != s_prompt) {
            fprintf(stderr, "  [TEXPACK] button prompts: %s\n",
                    style == PROMPT_PLAYSTATION ? "PlayStation" : style == PROMPT_XBOX ? "Xbox"
                    : style == PROMPT_KEYBOARD ? "keyboard" : "original");
            s_prompt = style;
            return 1;
        }
    }
    if (gen == xbox_settings_gen())
        return 0;
    gen = xbox_settings_gen();
    was = s_use;
    now = xbox_setting_int("RECOMP_TEXPACK", 1) != 0;
    s_use = now;
    if (was != now)
        fprintf(stderr, "  [TEXPACK] HD textures %s\n", now ? "on" : "off");
    return was != now;
}

void texpack_free(uint32_t *px)
{
    stbi_image_free(px);
}

/* Dumping while playing, live (overlay); returns the setting. */
int texpack_dump_enabled(void)
{
    static long gen;
    extern long xbox_settings_gen(void);
    init();
    if (gen != xbox_settings_gen()) {
        gen = xbox_settings_gen();
        s_dump = xbox_setting_int("RECOMP_TEXPACK_DUMP", 0) != 0;
        if (s_dump) {
            CreateDirectoryA("textures", NULL);
            CreateDirectoryA(DUMP_DIR, NULL);
        }
    }
    return s_dump;
}

long texpack_dumped_count(void) { return s_dumped_count; }
long texpack_loaded_count(void) { return s_loaded_count; }

/* Every texture in memory, not only those drawn so far: the title loads a
 * level's textures with it, each behind a D3D resource header --
 *
 *     Common  refcount | type (bits 16-18: 4 = texture)
 *     Data    texel offset, as SET_TEXTURE_OFFSET takes it
 *     Lock    0
 *     Format  as SET_TEXTURE_FORMAT: DMA 1/2, dimensions 2, colour format,
 *             mip levels, log2 width/height
 *     Size    0 for a swizzled or compressed texture
 *
 * -- so a scan of RAM for that shape finds them all, and decoding each the
 * way a draw would (nv2a_tex_decode) gives each its texture-pack name.
 * Loading a level and pressing the button covers it without playing it.
 * Returns how many new textures were queued. */
int texpack_scan_memory(void)
{
    extern uintptr_t g_xbox_mem_offset;
    extern int nv2a_tex_decode(uint32_t data, uint32_t format, uint32_t **out,
                               uint32_t *w, uint32_t *h, uint32_t *color);
    uint32_t va;
    int seen = 0, queued = 0, diag_n = 0;
    static uint32_t diag[256][2];
    extern int nv2a_recent_textures(uint32_t (*out)[2], int max);
    if (getenv("RECOMP_TEXPACK_SCAN_DIAG")) {
        int r;
        diag_n = nv2a_recent_textures(diag, 256);
        /* The first bytes of each drawn texture, to find it in the files. */
        for (r = 0; r < diag_n; r++) {
            extern const uint8_t *nv2a_tex_bytes(uint32_t offset_param);
            const uint8_t *t = nv2a_tex_bytes(diag[r][0]);
            int b;
            fprintf(stderr, "  [TEXBYTES] %08X %08X ", diag[r][0], diag[r][1]);
            for (b = 0; b < 48; b++)
                fprintf(stderr, "%02X", t[b]);
            fprintf(stderr, "\n");
        }
        diag_n = 0;
    }

    init();
    CreateDirectoryA("textures", NULL);
    CreateDirectoryA(DUMP_DIR, NULL);
    for (va = 0x10000; va < 0x04000000; ) {
        MEMORY_BASIC_INFORMATION mi;
        const uint8_t *p = (const uint8_t *)(g_xbox_mem_offset + va);
        uint32_t len, k;
        if (!VirtualQuery(p, &mi, sizeof mi))
            break;
        len = (uint32_t)((const uint8_t *)mi.BaseAddress + mi.RegionSize - p);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (k = 0; k + 20 <= len; k += 4) {
                const uint32_t *d = (const uint32_t *)(p + k);
                uint32_t common = d[0], data = d[1], fmt = d[3];
                uint32_t ul, vl, lv, *px = NULL, w, h, color;
                if (diag_n) {
                    /* Where the title keeps what it draws textures with. */
                    static int shown;
                    int r;
                    for (r = 0; r < diag_n; r++)
                        if (d[0] == diag[r][0] && shown < 120) {
                            const uint32_t *q = d - 4;
                            int has_fmt = 0, z;
                            for (z = 1; z <= 6; z++)
                                if (d[z] == diag[r][1])
                                    has_fmt = z;
                            if (k < 16)
                                continue;
                            shown++;
                            fprintf(stderr, "  [TEXSCAN] off %08X fmt %08X at %08X (+%d):"
                                    " %08X %08X %08X %08X [%08X] %08X %08X %08X %08X\n",
                                    diag[r][0], diag[r][1], va + k, has_fmt * 4,
                                    q[0], q[1], q[2], q[3], d[0], d[1], d[2], d[3], d[4]);
                        }
                }
                if ((common & 0x00070000u) != 0x00040000u || !(common & 0xFFFF)
                        || (common & 0xFFFF) > 0x400 || d[2] != 0 || d[4] != 0)
                    continue;
                if ((fmt & 3) == 0 || (fmt & 3) == 3 || ((fmt >> 4) & 0xF) != 2)
                    continue;
                ul = (fmt >> 20) & 0xF; vl = (fmt >> 24) & 0xF; lv = (fmt >> 16) & 0xF;
                if (ul < 3 || vl < 3 || ul > 11 || vl > 11 || !lv
                        || lv > (ul > vl ? ul : vl) + 1 || data >= 0x04000000u)
                    continue;
                color = (fmt >> 8) & 0xFF;
                if (color == 0x16 || color == 0x17 || color == 0x28 || color == 0x29)
                    continue;
                if (!nv2a_tex_decode(data, fmt, &px, &w, &h, &color))
                    continue;
                seen++;
                {
                    uint64_t key = texpack_key(px, w, h, color);
                    int had;
                    AcquireSRWLockShared(&s_lock);
                    had = set_find(&s_dumped, key, 0) != NULL;
                    ReleaseSRWLockShared(&s_lock);
                    if (!had) {
                        dump_queue(key, px, w, h, color);
                        queued++;
                    }
                }
                free(px);
            }
        va += len;
    }
    fprintf(stderr, "  [TEXPACK] memory scan: %d texture(s) found, %d new queued for %s\n",
            seen, queued, DUMP_DIR);
    return queued;
}
