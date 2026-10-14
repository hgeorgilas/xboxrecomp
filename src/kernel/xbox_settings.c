/*
 * Runtime settings: an ini file of KEY=VALUE lines, applied as environment
 * variables.
 *
 * Everything in the runtime is already configured through RECOMP_* variables,
 * so the file feeds the same mechanism instead of a second one: at startup
 * each line becomes a variable unless the environment already sets it -- a
 * launcher script or a shell still wins, which keeps one-off experiments
 * easy. The settings overlay changes values live with xbox_setting_set and
 * writes the file back with xbox_settings_save. Code that caches a value
 * re-reads it when xbox_settings_gen() moves.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SET_MAX 128

typedef struct {
    char key[64];
    char val[192];
    int  from_env;                  /* the environment overrode the file */
    char file[192];                 /* then the file's value, which Save keeps */
} Setting;

static Setting s_set[SET_MAX];
static int s_n;
static char s_path[MAX_PATH];
static volatile LONG s_gen = 1;
static SRWLOCK s_lock = SRWLOCK_INIT;

static void trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
}

static Setting *find(const char *key)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (!_stricmp(s_set[i].key, key))
            return &s_set[i];
    return NULL;
}

static Setting *add(const char *key)
{
    Setting *s = find(key);
    if (s || s_n == SET_MAX)
        return s;
    s = &s_set[s_n++];
    memset(s, 0, sizeof *s);
    strncpy(s->key, key, sizeof s->key - 1);
    return s;
}

void xbox_settings_load(const char *path)
{
    FILE *f;
    char line[300];

    strncpy(s_path, path, sizeof s_path - 1);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[SETTINGS] %s not found; defaults (it is written on Save)\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq;
        Setting *s;
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        if (!(eq = strchr(line, '=')))
            continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        if (!line[0] || !(s = add(line)))
            continue;
        if (getenv(line)) {
            s->from_env = 1;
            strncpy(s->val, getenv(line), sizeof s->val - 1);
            strncpy(s->file, eq + 1, sizeof s->file - 1);
            fprintf(stderr, "[SETTINGS] %s=%s from the environment (file says %s)\n",
                    line, s->val, eq + 1);
        } else {
            strncpy(s->val, eq + 1, sizeof s->val - 1);
            _putenv_s(line, s->val);
        }
    }
    fclose(f);
    fprintf(stderr, "[SETTINGS] loaded %d setting(s) from %s\n", s_n, path);
}

/* The current value: the variable, which the file and the overlay both set. */
const char *xbox_setting_get(const char *key)
{
    return getenv(key);
}

int xbox_setting_int(const char *key, int def)
{
    const char *v = getenv(key);
    return v && *v ? atoi(v) : def;
}

float xbox_setting_float(const char *key, float def)
{
    const char *v = getenv(key);
    return v && *v ? (float)atof(v) : def;
}

void xbox_setting_set(const char *key, const char *val)
{
    Setting *s;
    AcquireSRWLockExclusive(&s_lock);
    if ((s = add(key)) != NULL) {
        strncpy(s->val, val, sizeof s->val - 1);
        s->val[sizeof s->val - 1] = 0;
        s->from_env = 0;            /* changed here: this is what to save */
    }
    _putenv_s(key, val);
    ReleaseSRWLockExclusive(&s_lock);
    InterlockedIncrement(&s_gen);
}

long xbox_settings_gen(void)
{
    return InterlockedCompareExchange(&s_gen, 0, 0);
}

int xbox_settings_save(void)
{
    FILE *f;
    int i;
    if (!s_path[0])
        return 0;
    if (!(f = fopen(s_path, "w")))
        return 0;
    fprintf(f, "# Settings, written by the in-game settings overlay (F1).\n"
               "# KEY=VALUE; a variable set in the environment overrides a line here.\n");
    AcquireSRWLockShared(&s_lock);
    /* A value the environment overrode, and nothing changed since, goes
     * back as the file had it: a launcher's one-off is not a setting. */
    for (i = 0; i < s_n; i++)
        fprintf(f, "%s=%s\n", s_set[i].key,
                s_set[i].from_env ? s_set[i].file : s_set[i].val);
    ReleaseSRWLockShared(&s_lock);
    fclose(f);
    fprintf(stderr, "[SETTINGS] saved %d setting(s) to %s\n", s_n, s_path);
    return 1;
}

const char *xbox_settings_path(void)
{
    return s_path;
}

/* ---- title quirks ------------------------------------------------------------
 *
 * Behaviour one title needs and another would not want, off unless the game
 * project turns it on (from main, before the title starts):
 *
 *   xbox_SetQuirk("ui_bottom_nudge", 5)    2D drawn wholly in the bottom
 *                                          bar (below 84% of the height) moves
 *                                          down that many lines (at 480), once
 *                                          the 2D layer fills the screen
 *   xbox_SetQuirk("drop_wide_rhw0", 1)     drop a batch of pre-transformed
 *                                          vertices that all have RHW 0 and
 *                                          reach past the right of the target
 *   xbox_SetQuirk("bloom_composites", 1)   RHW-0 composites blended onto the
 *                                          frame (src alpha with one, or with
 *                                          1 - src alpha) are the bloom, which
 *                                          RECOMP_BLOOM (0..1) scales; also
 *                                          shows the overlay's Bloom slider
 *
 * Unknown names are kept too, so a quirk can be read before its user is. */
#define QUIRK_MAX 32

static struct { char name[32]; float value; } s_quirk[QUIRK_MAX];
static int s_nquirk;

void xbox_SetQuirk(const char *name, float value)
{
    int i;
    for (i = 0; i < s_nquirk; i++)
        if (!strcmp(s_quirk[i].name, name)) {
            s_quirk[i].value = value;
            return;
        }
    if (s_nquirk < QUIRK_MAX) {
        strncpy(s_quirk[s_nquirk].name, name, sizeof s_quirk[0].name - 1);
        s_quirk[s_nquirk].value = value;
        s_nquirk++;
        fprintf(stderr, "[SETTINGS] quirk %s = %g\n", name, value);
    }
}

/* The quirk's value, 0 if the title did not set it. */
float xbox_Quirk(const char *name)
{
    int i;
    for (i = 0; i < s_nquirk; i++)
        if (!strcmp(s_quirk[i].name, name))
            return s_quirk[i].value;
    return 0.0f;
}
