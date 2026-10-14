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

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

extern int xbox_setting_int(const char *key, int def);

#define DUMP_DIR    "textures\\dump\\"
#define REPL_DIR    "textures\\replace\\"

/* Button prompts: textures/prompts/<style>/ holds replacements, named like
 * the pack's, for the textures that draw controller buttons -- one set per
 * pad family. RECOMP_BUTTON_PROMPTS picks the set: "auto" (the default)
 * follows the pad last used, the title's own art for an Xbox pad and the
 * PlayStation set for a DualShock/DualSense; "original", "xbox" and
 * "playstation" fix it. They sit in the replacement set under the texture's
 * key salted with the style, and apply with the HD pack on or off. */
enum { PROMPT_ORIGINAL, PROMPT_XBOX, PROMPT_PLAYSTATION, PROMPT_KEYBOARD, PROMPT_STYLES };
static const char *const s_prompt_dir[PROMPT_STYLES] = {
    NULL, "textures\\prompts\\xbox\\", "textures\\prompts\\playstation\\",
    "textures\\prompts\\keyboard\\" };
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
