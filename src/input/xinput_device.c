/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* DualShock 4 / DualSense pads, read over HID (hid_pads.c). XInput pads keep
 * their own ports; the Sony pads fill the ports XInput leaves empty, in
 * order. */
extern int  hidpad_count(void);
extern int  hidpad_get(int n, XBOX_INPUT_STATE *st);
extern void hidpad_rumble(int n, WORD left, WORD right);

static int hid_for_port(DWORD port)
{
    DWORD q;
    int k = 0;
    for (q = 0; q < port; q++)
        k += !g_controller_connected[q];
    return k < hidpad_count() ? k : -1;
}

/* Which kind of pad is in use: what the button prompts should show.
 * 0 an Xbox pad (or none yet), 1 a PlayStation pad, 2 keyboard and mouse --
 * whichever was used last.
 *
 * Steam Input and DS4Windows present a Sony pad as an XInput pad too, so the
 * same press can arrive from both; the PlayStation pad wins whenever it was
 * active within two seconds of the Xbox one, or the prompts would flip with
 * every press. */
static volatile LONG g_xi_active_ms, g_hid_active_ms, g_kb_active_ms;

static int later(LONG a, LONG b)          /* tick a after tick b (wraps) */
{
    return a && (!b || (LONG)((DWORD)a - (DWORD)b) > 0);
}

static BOOL keyboard_enabled(void);
void xbox_InputInit(void);

int xbox_InputActiveKind(void)
{
    LONG xi = g_xi_active_ms, hid = g_hid_active_ms, kb = g_kb_active_ms;
    if (later(kb, xi) && later(kb, hid))
        return 2;
    if (!xi && !hid) {
        /* Nothing pressed yet (the title opens its files at boot, and the
         * prompts' words come with them: kernel_path.c): a PlayStation pad
         * if one is there, else an Xbox one, else the keyboard. */
        static LONG probed;
        int i;
        if (hidpad_count() > 0)
            return 1;
        if (!InterlockedExchange(&probed, 1))
            xbox_InputInit();
        for (i = 0; i < XBOX_MAX_CONTROLLERS; i++)
            if (g_controller_connected[i])
                return 0;
        return keyboard_enabled() ? 2 : 0;
    }
    return hid && (LONG)((DWORD)xi - (DWORD)hid) <= 2000;
}

/* kind: as xbox_InputActiveKind returns. */
static void note_activity(const XBOX_INPUT_STATE *st, int kind)
{
    const XBOX_GAMEPAD *g = &st->Gamepad;
    int i, active = g->wButtons != 0 || g->sThumbLX > 16000 || g->sThumbLX < -16000
                 || g->sThumbLY > 16000 || g->sThumbLY < -16000;
    for (i = 0; i < 8 && !active; i++)
        active = g->bAnalogButtons[i] > 64;
    if (active)
        InterlockedExchange(kind == 2 ? &g_kb_active_ms : kind ? &g_hid_active_ms
                            : &g_xi_active_ms, (LONG)(GetTickCount() | 1));
}

/* Host XInput calls that took a millisecond or more, and the longest, for
 * the audio recorder's report: a wireless pad can block in these, and the
 * title calls them from its own threads. */
static volatile LONG g_xi_slow, g_xi_max_us;

static void xi_timed(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1, f;
    LONG us;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    if (us >= 1000)
        InterlockedIncrement(&g_xi_slow);
    if (us > g_xi_max_us)
        g_xi_max_us = us;
}

void xbox_InputTakeSlowStats(long *slow, long *max_us)
{
    *slow = InterlockedExchange(&g_xi_slow, 0);
    *max_us = InterlockedExchange(&g_xi_max_us, 0);
}

/* ---- keyboard and mouse ---------------------------------------------------
 *
 * Played as player 1's pad: merged on top of a pad on port 0, so either
 * works at any time. On unless RECOMP_KEYBOARD=0.
 *
 * Each pad control has keys (and mouse buttons) bound to it; the defaults
 * below are for Spartan, in a PC action game's layout -- WASD to move, the
 * mouse buttons for the two attacks -- and every one can be rebound in the
 * settings: RECOMP_BIND_<CONTROL>=<key>[,<key>...], key names as in
 * k_keys (A-Z, 0-9, SPACE, SHIFT, MOUSE1, F5, NUM4, ...).
 *
 *   W A S D   move (left stick)      mouse     re-target the bow (right stick)
 *   Mouse 1   X  centred attack      Space     A  jump
 *   Mouse 2   Y  sweeping attack     E         B  use
 *   Shift     left trigger: shield   Q         right trigger: power
 *   F         left stick click: rage R         white: bow
 *   C         black                  Mouse 3   right stick click
 *   Esc       Start (pause)          Tab       Back
 *   arrows    d-pad (menus)          Enter, Backspace   A, B (menus)
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    extern int xbox_setting_int(const char *key, int def);
    return xbox_setting_int("RECOMP_KEYBOARD", 1) != 0;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int  xbox_FramebufferKeyDown(int vk);
extern void xbox_FramebufferMouseDelta(int *dx, int *dy);

static const struct { const char *name; int vk; } k_keys[] = {
    { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "ESC", VK_ESCAPE },
    { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK }, { "SHIFT", VK_SHIFT },
    { "CTRL", VK_CONTROL }, { "ALT", VK_MENU }, { "CAPSLOCK", VK_CAPITAL },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME },
    { "END", VK_END }, { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT },
    { "MOUSE1", VK_LBUTTON }, { "MOUSE2", VK_RBUTTON }, { "MOUSE3", VK_MBUTTON },
    { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 },
    { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 },
    { "F7", VK_F7 }, { "F8", VK_F8 }, { "F11", VK_F11 },
    { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 },
    { "NUM3", VK_NUMPAD3 }, { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 },
    { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 }, { "NUM8", VK_NUMPAD8 },
    { "NUM9", VK_NUMPAD9 },
    { ",", VK_OEM_COMMA }, { ".", VK_OEM_PERIOD }, { ";", VK_OEM_1 }, { "/", VK_OEM_2 },
};

enum {
    C_A, C_B, C_X, C_Y, C_BLACK, C_WHITE, C_LT, C_RT,
    C_START, C_BACK, C_LS, C_RS, C_UP, C_DOWN, C_LEFT, C_RIGHT,
    C_MOVE_UP, C_MOVE_DOWN, C_MOVE_LEFT, C_MOVE_RIGHT, C_COUNT
};
#define MAX_BIND 3

/* A generic layout; a title sets its own with xbox_InputSetDefaultBinding
 * (Spartan's: spartan/src/settings_menu.c). */
static const struct { const char *name, *keys; } k_controls[C_COUNT] = {
    { "A", "SPACE,ENTER" }, { "B", "BACKSPACE" }, { "X", "MOUSE1" },
    { "Y", "MOUSE2" }, { "BLACK", "C" }, { "WHITE", "R" },
    { "LT", "Q" }, { "RT", "E" }, { "START", "ESC" }, { "BACK", "TAB" },
    { "LS", "SHIFT" }, { "RS", "MOUSE3" },
    { "UP", "UP" }, { "DOWN", "DOWN" }, { "LEFT", "LEFT" }, { "RIGHT", "RIGHT" },
    { "MOVE_UP", "W" }, { "MOVE_DOWN", "S" }, { "MOVE_LEFT", "A" }, { "MOVE_RIGHT", "D" },
};

static int s_bind[C_COUNT][MAX_BIND];
static const char *s_default[C_COUNT];          /* the title's, over k_controls */
static volatile LONG s_default_gen;

static const char *default_keys(int c)
{
    return s_default[c] ? s_default[c] : k_controls[c].keys;
}

/* A title's own default keys for a control ("A", "LT", "MOVE_UP", ...; keys
 * as the settings write them, "MOUSE1,ENTER"). The string must stay valid. */
void xbox_InputSetDefaultBinding(const char *control, const char *keys)
{
    int c;
    for (c = 0; c < C_COUNT; c++)
        if (!_stricmp(control, k_controls[c].name)) {
            s_default[c] = keys;
            InterlockedIncrement(&s_default_gen);
            return;
        }
}

static int key_code(const char *name)
{
    size_t i;
    if (name[0] && !name[1]
            && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (!_stricmp(name, k_keys[i].name))
            return k_keys[i].vk;
    return 0;
}

/* The bindings, from the settings over the defaults; again when they change. */
static void bindings_update(void)
{
    extern const char *xbox_setting_get(const char *key);
    extern long xbox_settings_gen(void);
    static long gen = -1, dgen = -1;
    int c, n;
    if (gen == xbox_settings_gen() && dgen == s_default_gen)
        return;
    gen = xbox_settings_gen();
    dgen = s_default_gen;
    for (c = 0; c < C_COUNT; c++) {
        char key[40], list[96], *tok, *ctx = NULL;
        const char *v;
        snprintf(key, sizeof key, "RECOMP_BIND_%s", k_controls[c].name);
        v = xbox_setting_get(key);
        strncpy(list, v && *v ? v : default_keys(c), sizeof list - 1);
        list[sizeof list - 1] = 0;
        _strupr(list);
        memset(s_bind[c], 0, sizeof s_bind[c]);
        n = 0;
        for (tok = strtok_s(list, ", ", &ctx); tok && n < MAX_BIND;
             tok = strtok_s(NULL, ", ", &ctx))
            if ((s_bind[c][n] = key_code(tok)) != 0)
                n++;
    }
}

/* ---- the bindings, for a settings screen ----------------------------------
 * Controls are listed in k_controls order. A title may name them for its
 * own actions ("Jump", ...); otherwise the pad control's name is shown. */
static const char *const *s_labels;

void xbox_InputSetBindLabels(const char *const *labels)
{
    s_labels = labels;
}

int xbox_InputBindCount(void)
{
    return C_COUNT;
}

/* The setting that holds control i's keys, and its label. */
const char *xbox_InputBindSetting(int i, char *buf, int n)
{
    snprintf(buf, (size_t)n, "RECOMP_BIND_%s", k_controls[i].name);
    return buf;
}

const char *xbox_InputBindLabel(int i)
{
    return s_labels && s_labels[i] ? s_labels[i] : k_controls[i].name;
}

const char *xbox_InputBindDefault(int i)
{
    return default_keys(i);
}

/* A key's name as the bindings write it, or NULL for one they cannot. */
const char *xbox_InputKeyName(int vk)
{
    static char one[2];
    size_t i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        one[0] = (char)vk;
        return one;
    }
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].vk == vk)
            return k_keys[i].name;
    return NULL;
}

/* The title screen is up (the game says so: settings_menu.c). Spartan's
 * takes only Start, so there the confirm keys stand in for it. */
static volatile LONG s_title_screen;

void xbox_InputSetTitleScreen(int on)
{
    InterlockedExchange(&s_title_screen, on != 0);
}

static BOOL control_down(int c)
{
    int i;
    for (i = 0; i < MAX_BIND && s_bind[c][i]; i++)
        if (xbox_FramebufferKeyDown(s_bind[c][i]))
            return TRUE;
    return FALSE;
}

/* A thumb axis from two controls, at the full deflection a key implies. */
static SHORT axis_from(int negative, int positive)
{
    int v = 0;
    if (control_down(negative)) v -= 32767;
    if (control_down(positive)) v += 32767;
    return (SHORT)v;
}

/* Mouse movement as the right stick: RECOMP_MOUSE_SENS (default 1) counts
 * per poll for full deflection is 25 / sensitivity. */
static SHORT mouse_axis(int d)
{
    extern float xbox_setting_float(const char *key, float def);
    float sens = xbox_setting_float("RECOMP_MOUSE_SENS", 1.0f);
    float v = (float)d * sens * (32767.0f / 25.0f);
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    WORD b = 0;
    int dx, dy;
    /* At the title the confirm keys are Start, alone: with A held too the
     * press is not taken. */
    BOOL title = s_title_screen != 0;
    /* ...and that press is the title's alone: held on, it would also be
     * taken by the menu that follows, so it counts for nothing until the
     * key is let go. */
    static BOOL title_press;
    BOOL confirm = control_down(C_A);
    if (title && confirm)
        title_press = TRUE;
    else if (!confirm)
        title_press = FALSE;

    bindings_update();
    memset(pState, 0, sizeof(*pState));
    if (control_down(C_UP))    b |= XBOX_GAMEPAD_DPAD_UP;
    if (control_down(C_DOWN))  b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (control_down(C_LEFT))  b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (control_down(C_RIGHT)) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (control_down(C_START) || (title && confirm))
        b |= XBOX_GAMEPAD_START;
    if (control_down(C_BACK))  b |= XBOX_GAMEPAD_BACK;
    if (control_down(C_LS))    b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (control_down(C_RS))    b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = confirm && !title_press ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = control_down(C_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = control_down(C_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = control_down(C_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = control_down(C_WHITE) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = control_down(C_BLACK) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = control_down(C_LT) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = control_down(C_RT) ? 255 : 0;

    pState->Gamepad.sThumbLX = axis_from(C_MOVE_LEFT, C_MOVE_RIGHT);
    pState->Gamepad.sThumbLY = axis_from(C_MOVE_DOWN, C_MOVE_UP);
    note_activity(pState, 2);           /* the mouse moving alone does not count */
    xbox_FramebufferMouseDelta(&dx, &dy);
    pState->Gamepad.sThumbRX = mouse_axis(dx);
    {
        /* RECOMP_MOUSE_INVERT=1: mouse up looks down, as a flight stick. */
        extern int xbox_setting_int(const char *key, int def);
        pState->Gamepad.sThumbRY = mouse_axis(xbox_setting_int("RECOMP_MOUSE_INVERT", 0) ? dy : -dy);
    }

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* The keyboard and mouse on top of a pad's state: buttons and triggers
 * added, sticks the keyboard's where it has a deflection. One merge for
 * every kind of pad -- a Sony pad's used to leave the sticks out, so WASD
 * did nothing with a DualSense plugged in. */
static void merge_keyboard(XBOX_INPUT_STATE *st)
{
    XBOX_INPUT_STATE kb;
    int i;
    keyboard_state(&kb);
    st->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > st->Gamepad.bAnalogButtons[i])
            st->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) st->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) st->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) st->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) st->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is read
     * as the same state and a change from the keyboard never happens. */
    st->dwPacketNumber = kb.dwPacketNumber;
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* The settings overlay has the pad while it is open: the title sees it
     * connected and at rest. */
    {
        extern int xbox_overlay_visible(void);
        static DWORD packet = 0x40000000u;
        if (xbox_overlay_visible() && g_controller_connected[dwPort]) {
            memset(pState, 0, sizeof *pState);
            pState->dwPacketNumber = ++packet;
            return ERROR_SUCCESS;
        }
    }

    {
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        result = XInputGetState(dwPort, &xi_state);
        xi_timed(t0);
    }
    if (result != ERROR_SUCCESS) {
        int h;
        g_controller_connected[dwPort] = FALSE;
        if ((h = hid_for_port(dwPort)) >= 0 && hidpad_get(h, pState)) {
            note_activity(pState, 1);
            /* The packet number is the pad's own; offset so a port that
             * changes source still sees it move. */
            pState->dwPacketNumber += 0x10000000u;
            if (dwPort == 0 && keyboard_enabled())
                merge_keyboard(pState);
            return ERROR_SUCCESS;
        }
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;
    note_activity(pState, 0);

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    if (dwPort == 0 && keyboard_enabled())
        merge_keyboard(pState);

    return ERROR_SUCCESS;
}

/* Rumble goes out on its own thread. XInputSetState can block for
 * milliseconds on a wireless pad, and titles call XInputSetState from the
 * game loop -- often every frame, with unchanged values. The caller only
 * records the latest speeds; the thread sends a port's speeds when they
 * change. */
static volatile LONG g_rumble[XBOX_MAX_CONTROLLERS];     /* left << 16 | right */
static HANDLE g_rumble_event;

/* RECOMP_RUMBLE: vibration intensity in percent, 0 (off) to 100 (as the
 * title asks), read live -- the thread also wakes a few times a second, so
 * a change applies to a rumble already running. */
static ULONG rumble_scaled(LONG v)
{
    extern int xbox_setting_int(const char *key, int def);
    int pct = xbox_setting_int("RECOMP_RUMBLE", 100);
    ULONG l = (ULONG)v >> 16, r = (ULONG)v & 0xFFFF;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* RECOMP_RUMBLE_CURVE (default 1): the speed squared. The console's
     * pads (Duke, S) have heavy motors that barely turn at low and middle
     * speeds, and titles were tuned to that; a modern pad's motors answer
     * from the bottom of the range, so every light rumble came out strong.
     * Squared, light ones stay light and a full one is still full. 0 sends
     * the title's speeds as they are. */
    if (xbox_setting_int("RECOMP_RUMBLE_CURVE", 1)) {
        l = l * l / 0xFFFF;
        r = r * r / 0xFFFF;
    }
    l = l * (ULONG)pct / 100;
    r = r * (ULONG)pct / 100;
    return l << 16 | r;
}

/* Title quirk rumble_fade_ms: the longest a rumble is felt. Each motor's
 * speed starts an envelope when it rises (a new effect) that falls from
 * there to 0 over that many milliseconds; the motor gets the lower of the
 * envelope and what the title asks. A title whose effects fade out over
 * seconds -- tuned to the console pads' heavy motors, which stop turning
 * long before the speed reaches 0 -- buzzes through the whole fade on a
 * modern pad. 0 (the default) leaves the title's timing alone. */
typedef struct { ULONG last, peak; DWORD start; } RumbleEnv;

static ULONG rumble_env(RumbleEnv *e, ULONG raw, DWORD now, DWORD fade_ms,
                        int *active)
{
    ULONG env;
    DWORD t;
    if (raw > e->last) {
        e->peak = raw;
        e->start = now;
    }
    e->last = raw;
    if (!fade_ms || !raw)
        return raw;
    t = now - e->start;
    env = t >= fade_ms ? 0 : (ULONG)((ULONGLONG)e->peak * (fade_ms - t) / fade_ms);
    if (env)
        *active = 1;
    return raw < env ? raw : env;
}

static DWORD WINAPI rumble_thread(LPVOID unused)
{
    extern float xbox_Quirk(const char *name);
    LONG sent[XBOX_MAX_CONTROLLERS];
    RumbleEnv env[XBOX_MAX_CONTROLLERS][2];
    DWORD p, wait = 250;
    (void)unused;
    memset(env, 0, sizeof env);
    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        sent[p] = -1;
    for (;;) {
        DWORD now, fade_ms;
        int active = 0;
        WaitForSingleObject(g_rumble_event, wait);
        now = GetTickCount();
        fade_ms = (DWORD)xbox_Quirk("rumble_fade_ms");
        for (p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
            ULONG raw = (ULONG)g_rumble[p];
            ULONG l = rumble_env(&env[p][0], raw >> 16, now, fade_ms, &active);
            ULONG r = rumble_env(&env[p][1], raw & 0xFFFF, now, fade_ms, &active);
            LONG v = (LONG)rumble_scaled((LONG)(l << 16 | r));
            if (v != sent[p]) {
                XINPUT_VIBRATION xi_vib;
                LARGE_INTEGER t0;
                int h;
                xi_vib.wLeftMotorSpeed = (WORD)((ULONG)v >> 16);
                xi_vib.wRightMotorSpeed = (WORD)v;
                QueryPerformanceCounter(&t0);
                if (!g_controller_connected[p] && (h = hid_for_port(p)) >= 0)
                    hidpad_rumble(h, xi_vib.wLeftMotorSpeed, xi_vib.wRightMotorSpeed);
                else
                    XInputSetState(p, &xi_vib);
                xi_timed(t0);
                {
                    static int trace = -1;
                    if (trace < 0)
                        trace = getenv("RECOMP_RUMBLE_TRACE") != NULL;
                    if (trace)
                        fprintf(stderr, "[RUMBLE] %lu ms: sent %5u %5u\n",
                                (unsigned long)now, xi_vib.wLeftMotorSpeed,
                                xi_vib.wRightMotorSpeed);
                }
                sent[p] = v;
            }
        }
        wait = active ? 16 : 250;       /* a fade in progress steps itself */
    }
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    static volatile LONG started;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!InterlockedExchange(&started, 1)) {
        g_rumble_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, rumble_thread, NULL, 0, NULL));
    }
    if (!g_rumble_event)
        return ERROR_DEVICE_NOT_CONNECTED;
    InterlockedExchange(&g_rumble[dwPort],
                        (LONG)(((ULONG)pVibration->wLeftMotorSpeed << 16)
                               | pVibration->wRightMotorSpeed));
    SetEvent(g_rumble_event);
    return g_controller_connected[dwPort] || hid_for_port(dwPort) >= 0
         ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    /* Live, not just what xbox_InputInit saw: nothing in the runtime calls
     * InputInit, so the flag read FALSE forever and a title asking "is a pad
     * there" (XGetDevices / XGetDeviceChanges) never opened one. Probed at
     * most once a second per port -- XInputGetState on an empty slot is slow.
     * The keyboard counts as pad 0, matching xbox_InputGetState. */
    static DWORD last_probe[XBOX_MAX_CONTROLLERS];
    DWORD now;

    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    now = GetTickCount();
    if (!last_probe[dwPort] || now - last_probe[dwPort] >= 1000) {
        XINPUT_STATE state;
        last_probe[dwPort] = now ? now : 1;
        g_controller_connected[dwPort] = XInputGetState(dwPort, &state) == ERROR_SUCCESS;
    }
    if (dwPort == 0 && keyboard_enabled())
        return TRUE;
    return g_controller_connected[dwPort] || hid_for_port(dwPort) >= 0;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) {
        if (hid_for_port(dwPort) < 0)
            return result;
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = 1;        /* XINPUT_DEVTYPE_GAMEPAD */
        pCaps->SubType = 1;     /* XINPUT_DEVSUBTYPE_GAMEPAD */
        return ERROR_SUCCESS;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
