/**
 * Sony pads (DualShock 4, DualSense) through HID, as Xbox pads.
 *
 * XInput only sees Xbox-protocol pads; a DualShock or DualSense needs Steam
 * Input or DS4Windows in between, or it is invisible. Both speak plain HID,
 * over USB and Bluetooth alike, and their reports are fixed layouts -- the
 * same ones SDL's HIDAPI drivers read -- so they are read here directly,
 * with nothing to install.
 *
 * A discovery thread looks for new pads every two seconds; each open pad has
 * a reader thread that keeps its latest state. Bluetooth pads start in a
 * reduced report mode and switch to the full one when their calibration
 * feature report is read, which is done on open.
 *
 *   report     layout from byte 'base'
 *   DS4  0x01  USB, and Bluetooth before the switch: LX LY RX RY b0 b1 b2 L2 R2
 *   DS4  0x11  Bluetooth, full: the same, two bytes on
 *   DS5  0x01  USB: LX LY RX RY L2 R2 counter b0 b1 b2
 *   DS5  0x31  Bluetooth, full: the same, one byte on
 *   DS5  0x01  Bluetooth before the switch (10 bytes): the DS4 layout
 *
 *   b0: hat (0 = up, clockwise, 8 = none) | square 0x10 cross 0x20
 *       circle 0x40 triangle 0x80
 *   b1: L1 0x01 R1 0x02 L2 0x04 R2 0x08 share/create 0x10 options 0x20
 *       L3 0x40 R3 0x80
 *
 * Rumble goes out as each pad's effects output report; over Bluetooth that
 * report ends in a CRC32 of 0xA2 and the report.
 */

#if defined(_WIN32)

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <string.h>
#include "xinput_xbox.h"

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#define MAX_PADS 4

enum { KIND_NONE, KIND_DS4, KIND_DS5 };

typedef struct {
    volatile LONG live;          /* reader running and reports arriving */
    int kind, bt;
    HANDLE h;
    WCHAR path[260];
    USHORT in_len, out_len, feat_len;
    SRWLOCK lock;
    XBOX_INPUT_STATE st;         /* latest, under lock */
    DWORD packet;
} Pad;

static Pad s_pads[MAX_PADS];
static volatile LONG s_started;

static int sony_kind(USHORT vid, USHORT pid)
{
    if (vid != 0x054C)
        return KIND_NONE;
    switch (pid) {
    case 0x05C4: case 0x09CC: case 0x0BA0:   /* DualShock 4 v1, v2, dongle */
        return KIND_DS4;
    case 0x0CE6: case 0x0DF2:                /* DualSense, DualSense Edge */
        return KIND_DS5;
    }
    return KIND_NONE;
}

static SHORT stick(BYTE v, int invert)
{
    int x = ((int)v - 128) * 258;
    if (invert)
        x = -x;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (SHORT)x;
}

/* One report -> the Xbox pad. Returns 0 for a report that carries no state. */
static int parse(Pad *p, const BYTE *r, DWORD n, XBOX_INPUT_STATE *out)
{
    const BYTE *s;
    BYTE lx, ly, rx, ry, b0, b1, l2, r2;
    WORD w = 0;
    int base, ds5 = 0;

    if (p->kind == KIND_DS4 && r[0] == 0x01 && n >= 10)
        base = 1;
    else if (p->kind == KIND_DS4 && r[0] == 0x11 && n >= 12)
        base = 3;
    else if (p->kind == KIND_DS5 && r[0] == 0x01 && !p->bt && n >= 11)
        base = 1, ds5 = 1;
    else if (p->kind == KIND_DS5 && r[0] == 0x31 && n >= 12)
        base = 2, ds5 = 1;
    else if (p->kind == KIND_DS5 && r[0] == 0x01 && n >= 10)
        base = 1;       /* Bluetooth, before the switch */
    else
        return 0;
    s = r + base;
    lx = s[0]; ly = s[1]; rx = s[2]; ry = s[3];
    if (ds5) {
        l2 = s[4]; r2 = s[5]; b0 = s[7]; b1 = s[8];
    } else {
        b0 = s[4]; b1 = s[5]; l2 = s[7]; r2 = s[8];
    }

    memset(out, 0, sizeof *out);
    switch (b0 & 0x0F) {
    case 0: w |= XBOX_GAMEPAD_DPAD_UP; break;
    case 1: w |= XBOX_GAMEPAD_DPAD_UP | XBOX_GAMEPAD_DPAD_RIGHT; break;
    case 2: w |= XBOX_GAMEPAD_DPAD_RIGHT; break;
    case 3: w |= XBOX_GAMEPAD_DPAD_DOWN | XBOX_GAMEPAD_DPAD_RIGHT; break;
    case 4: w |= XBOX_GAMEPAD_DPAD_DOWN; break;
    case 5: w |= XBOX_GAMEPAD_DPAD_DOWN | XBOX_GAMEPAD_DPAD_LEFT; break;
    case 6: w |= XBOX_GAMEPAD_DPAD_LEFT; break;
    case 7: w |= XBOX_GAMEPAD_DPAD_UP | XBOX_GAMEPAD_DPAD_LEFT; break;
    }
    if (b1 & 0x20) w |= XBOX_GAMEPAD_START;        /* options */
    if (b1 & 0x10) w |= XBOX_GAMEPAD_BACK;         /* share / create */
    if (b1 & 0x40) w |= XBOX_GAMEPAD_LEFT_THUMB;
    if (b1 & 0x80) w |= XBOX_GAMEPAD_RIGHT_THUMB;
    out->Gamepad.wButtons = w;
    /* The same seats XInput pads get in xinput_device.c: shoulders on the
     * black and white buttons, triggers analog. */
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = (b0 & 0x20) ? 255 : 0;   /* cross */
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = (b0 & 0x40) ? 255 : 0;   /* circle */
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = (b0 & 0x10) ? 255 : 0;   /* square */
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = (b0 & 0x80) ? 255 : 0;   /* triangle */
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = (b1 & 0x01) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = (b1 & 0x02) ? 255 : 0;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = l2;
    out->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = r2;
    out->Gamepad.sThumbLX = stick(lx, 0);
    out->Gamepad.sThumbLY = stick(ly, 1);      /* HID y is down */
    out->Gamepad.sThumbRX = stick(rx, 0);
    out->Gamepad.sThumbRY = stick(ry, 1);
    return 1;
}

static void close_pad(Pad *p)
{
    InterlockedExchange(&p->live, 0);
    if (p->h != INVALID_HANDLE_VALUE && p->h)
        CloseHandle(p->h);
    p->h = NULL;
    p->kind = KIND_NONE;
    p->path[0] = 0;
}

static DWORD WINAPI reader(LPVOID arg)
{
    Pad *p = (Pad *)arg;
    BYTE buf[1024];
    OVERLAPPED ov;
    XBOX_INPUT_STATE st, last;

    memset(&last, 0, sizeof last);
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (;;) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(p->h, buf, p->in_len, &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING)
                break;
            if (WaitForSingleObject(ov.hEvent, 1000) != WAIT_OBJECT_0) {
                CancelIo(p->h);
                /* An idle Bluetooth pad can go quiet; a gone one fails. */
                if (!GetOverlappedResult(p->h, &ov, &n, TRUE)
                        && GetLastError() != ERROR_OPERATION_ABORTED)
                    break;
                continue;
            }
            if (!GetOverlappedResult(p->h, &ov, &n, FALSE))
                break;
        }
        if (!parse(p, buf, n, &st))
            continue;
        AcquireSRWLockExclusive(&p->lock);
        /* The title finds presses by packet number, so it moves only when
         * the state does. */
        if (memcmp(&st.Gamepad, &last.Gamepad, sizeof st.Gamepad))
            p->packet++;
        last = st;
        st.dwPacketNumber = p->packet;
        p->st = st;
        InterlockedExchange(&p->live, 1);
        ReleaseSRWLockExclusive(&p->lock);
    }
    CloseHandle(ov.hEvent);
    fprintf(stderr, "  [PAD] %s disconnected\n", p->kind == KIND_DS5 ? "DualSense" : "DualShock 4");
    close_pad(p);
    return 0;
}

static int already_open(const WCHAR *path)
{
    int i;
    for (i = 0; i < MAX_PADS; i++)
        if (s_pads[i].kind != KIND_NONE && !_wcsicmp(s_pads[i].path, path))
            return 1;
    return 0;
}

static void try_open(const WCHAR *path)
{
    HANDLE h;
    HIDD_ATTRIBUTES a;
    PHIDP_PREPARSED_DATA pp;
    HIDP_CAPS caps;
    int kind, i;
    Pad *p = NULL;

    if (already_open(path))
        return;
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    a.Size = sizeof a;
    if (!HidD_GetAttributes(h, &a) || !(kind = sony_kind(a.VendorID, a.ProductID))
            || !HidD_GetPreparsedData(h, &pp)) {
        CloseHandle(h);
        return;
    }
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != 0x01
            || (caps.Usage != 0x05 && caps.Usage != 0x04)) {
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        return;
    }
    HidD_FreePreparsedData(pp);
    for (i = 0; i < MAX_PADS; i++)
        if (s_pads[i].kind == KIND_NONE) {
            p = &s_pads[i];
            break;
        }
    if (!p) {
        CloseHandle(h);
        return;
    }
    p->h = h;
    p->kind = kind;
    if (caps.InputReportByteLength > 1024 || caps.OutputReportByteLength > 1024) {
        CloseHandle(h);
        return;
    }
    p->in_len = caps.InputReportByteLength;
    p->out_len = caps.OutputReportByteLength;
    p->feat_len = caps.FeatureReportByteLength;
    /* USB pads report 64 bytes, Bluetooth ones 78 (the full-mode size). */
    p->bt = caps.InputReportByteLength > 64;
    wcsncpy_s(p->path, 260, path, _TRUNCATE);
    if (p->bt) {
        /* Reading the calibration feature report switches the pad to its
         * full report: 0x05 on a DualSense, 0x02 on a DualShock 4. */
        BYTE f[128];
        memset(f, 0, sizeof f);
        f[0] = kind == KIND_DS5 ? 0x05 : 0x02;
        HidD_GetFeature(h, f, p->feat_len < sizeof f ? p->feat_len : sizeof f);
    }
    fprintf(stderr, "  [PAD] %s over %s (pad %d)\n",
            kind == KIND_DS5 ? "DualSense" : "DualShock 4", p->bt ? "Bluetooth" : "USB",
            (int)(p - s_pads));
    CloseHandle(CreateThread(NULL, 0, reader, p, 0, NULL));
}

static void scan(void)
{
    GUID g;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;

    HidD_GetHidGuid(&g);
    set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return;
    ifd.cbSize = sizeof ifd;
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        BYTE buf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 520];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        d->cbSize = sizeof *d;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, d, sizeof buf, NULL, NULL))
            continue;
        /* Sony's vendor id is in the interface path; skip the others
         * without opening them. */
        if (!wcsstr(d->DevicePath, L"054c") && !wcsstr(d->DevicePath, L"054C"))
            continue;
        try_open(d->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
}

static DWORD WINAPI discovery(LPVOID unused)
{
    (void)unused;
    for (;;) {
        scan();
        Sleep(2000);
    }
}

/* RECOMP_HID_PADS=0 leaves Sony pads alone -- for Steam Input or DS4Windows,
 * which present them as XInput pads already. Read at start. */
static int enabled(void)
{
    extern int xbox_setting_int(const char *key, int def);
    static int on = -1;
    if (on < 0)
        on = xbox_setting_int("RECOMP_HID_PADS", 1) != 0;
    return on;
}

static void start(void)
{
    int i;
    if (!enabled() || InterlockedExchange(&s_started, 1))
        return;
    for (i = 0; i < MAX_PADS; i++)
        InitializeSRWLock(&s_pads[i].lock);
    scan();         /* pads already plugged in are there from the first poll */
    CloseHandle(CreateThread(NULL, 0, discovery, NULL, 0, NULL));
}

/* The n-th live pad (in slot order), or NULL. */
static Pad *nth(int n)
{
    int i;
    start();
    for (i = 0; i < MAX_PADS; i++)
        if (s_pads[i].live && n-- == 0)
            return &s_pads[i];
    return NULL;
}

int hidpad_count(void)
{
    int i, n = 0;
    start();
    for (i = 0; i < MAX_PADS; i++)
        n += s_pads[i].live != 0;
    return n;
}

int hidpad_get(int n, XBOX_INPUT_STATE *st)
{
    Pad *p = nth(n);
    if (!p)
        return 0;
    AcquireSRWLockShared(&p->lock);
    *st = p->st;
    ReleaseSRWLockShared(&p->lock);
    return 1;
}

static DWORD crc32(DWORD crc, const BYTE *d, size_t n)
{
    size_t i;
    int k;
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* Motor speeds as XInput has them: left the low-frequency (heavy) motor,
 * right the high-frequency one. Blocking; call from the rumble thread. */
void hidpad_rumble(int n, WORD left, WORD right)
{
    Pad *p = nth(n);
    BYTE r[1024], hi = (BYTE)(right >> 8), lo = (BYTE)(left >> 8);
    DWORD len, w = 0;
    OVERLAPPED ov;

    if (!p || !p->out_len)
        return;
    memset(r, 0, sizeof r);
    if (p->kind == KIND_DS4 && !p->bt) {
        r[0] = 0x05; r[1] = 0x01;           /* rumble only: leave the light */
        r[4] = hi; r[5] = lo;
        len = 32;
    } else if (p->kind == KIND_DS4) {
        r[0] = 0x11; r[1] = 0xC0 | 0x04; r[3] = 0x01;
        r[6] = hi; r[7] = lo;
        len = 78;
    } else if (!p->bt) {
        r[0] = 0x02; r[1] = 0x03;           /* compatible rumble + haptics */
        r[3] = hi; r[4] = lo;
        len = 48;
    } else {
        r[0] = 0x31; r[1] = 0x02; r[2] = 0x03;
        r[4] = hi; r[5] = lo;
        len = 78;
    }
    if (p->bt) {
        BYTE seed = 0xA2;
        DWORD c = crc32(crc32(0, &seed, 1), r, 74);
        memcpy(r + 74, &c, 4);
    }
    /* Windows takes a HID write only at the full output report length;
     * the pad ignores what follows its report. */
    if (len > p->out_len)
        return;
    len = p->out_len;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!WriteFile(p->h, r, len, &w, &ov) && GetLastError() == ERROR_IO_PENDING
            && WaitForSingleObject(ov.hEvent, 100) != WAIT_OBJECT_0)
        CancelIo(p->h);
    CloseHandle(ov.hEvent);
}

#endif /* _WIN32 */
