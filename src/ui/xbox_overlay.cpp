/*
 * Settings overlay: a Dear ImGui panel drawn over the presented frame.
 *
 * F1 opens and closes it; while it is open the title gets no input. It is
 * drawn by the thread that presents (pbgpu_present), on the swap chain the
 * frame went to, while the window's messages arrive on the window thread --
 * so the window procedure only queues them and the presenting thread feeds
 * them to ImGui before each frame. ImGui is not thread-safe, and this keeps
 * all of it on one thread.
 *
 * Values go through xbox_setting_set, which the runtime reads live where it
 * can; the rest say they apply after a restart. Save writes the ini.
 */
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

extern "C" {
const char *xbox_setting_get(const char *key);
int   xbox_setting_int(const char *key, int def);
float xbox_setting_float(const char *key, float def);
float xbox_Quirk(const char *name);
float xbox_FramebufferFps(void);
void  xbox_setting_set(const char *key, const char *val);
void  xbox_FramebufferSetFullscreen(int on);
int   texpack_scan_memory(void);
long  texpack_dumped_count(void);
long  texpack_loaded_count(void);
int   xbox_settings_save(void);
const char *xbox_settings_path(void);
int   hidpad_count(void);
int   xbox_InputActiveKind(void);
int   xbox_InputBindCount(void);
const char *xbox_InputBindSetting(int i, char *buf, int n);
const char *xbox_InputBindLabel(int i);
const char *xbox_InputBindDefault(int i);
const char *xbox_InputKeyName(int vk);
}

namespace {

struct Msg { HWND h; UINT m; WPARAM w; LPARAM l; };

SRWLOCK g_qlock = SRWLOCK_INIT;
Msg     g_q[512];
int     g_qn;
volatile LONG g_visible;
volatile LONG g_capture = -1;  /* binding waiting for a key: control * 2 + slot */
volatile LONG g_captured;      /* the key (or mouse button) pressed for it */
volatile LONG g_show_binds;    /* opened to the key bindings */
volatile LONG g_scan;          /* F9: dump every texture in memory */
int     g_scan_found = -1;
bool    g_ready;
HWND    g_hwnd;
LARGE_INTEGER g_freq, g_last;
float   g_fps;
char    g_status[160];
float   g_boot_aspect;

void set_float(const char *key, float v)
{
    char b[32];
    snprintf(b, sizeof b, "%.4f", v);
    xbox_setting_set(key, b);
}

void set_int(const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    xbox_setting_set(key, b);
}

void bindings();

void panel()
{
    ImGuiIO &io = ImGui::GetIO();
    bool open = true;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Settings", &open,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Display");
    {
        static const char *names[] = { "4:3 (original)", "16:9", "21:9" };
        static const float values[] = { 0.0f, 16.0f / 9.0f, 21.0f / 9.0f };
        float cur = xbox_setting_float("RECOMP_ASPECT", 0.0f);
        int sel = 0;
        for (int i = 1; i < 3; i++)
            if (fabsf(cur - values[i]) < 0.01f)
                sel = i;
        if (ImGui::Combo("Aspect ratio", &sel, names, 3))
            set_float("RECOMP_ASPECT", values[sel]);
        if (fabsf(values[sel] - g_boot_aspect) > 0.01f)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Applies after a restart.");
    }
    {
        static const char *names[] = { "Match window", "854x480 (original)", "1280x720 (HD)",
                                       "1920x1080 (Full HD)", "2560x1440 (2K)",
                                       "3840x2160 (4K)" };
        static const float values[] = { 0.0f, 1.0f, 1.5f, 2.25f, 3.0f, 4.5f };
        float cur = xbox_setting_float("RECOMP_RES_SCALE", 0.0f);
        int sel = 0;
        for (int i = 0; i < 6; i++)
            if (fabsf(cur - values[i]) < 0.01f)
                sel = i;
        if (ImGui::Combo("Resolution", &sel, names, 6))
            set_float("RECOMP_RES_SCALE", values[sel]);
        ImGui::SetItemTooltip("The height the game is rendered at (wider in widescreen)."
                              " Match window renders at the window's own resolution.");
    }
    {
        bool fill = xbox_setting_int("RECOMP_SAFE_AREA_FILL", 1) != 0;
        if (ImGui::Checkbox("HUD and subtitles use the whole screen", &fill))
            set_int("RECOMP_SAFE_AREA_FILL", fill ? 1 : 0);
        ImGui::SetItemTooltip("Off: the console's TV-safe layout, with margins.");
    }

    {
        bool full = xbox_setting_int("RECOMP_FULLSCREEN", 0) != 0;
        if (ImGui::Checkbox("Fullscreen (Alt+Enter)", &full)) {
            set_int("RECOMP_FULLSCREEN", full ? 1 : 0);
            xbox_FramebufferSetFullscreen(full ? 1 : 0);
        }
        bool vsync = xbox_setting_int("RECOMP_VSYNC", 0) != 0;
        if (ImGui::Checkbox("V-sync", &vsync))
            set_int("RECOMP_VSYNC", vsync ? 1 : 0);
        ImGui::SetItemTooltip("Waits for the display: no tearing, a little more latency.");
    }

    if (xbox_setting_int("RECOMP_GAME_HZ_SUPPORTED", 0)) {
        static const char *names[] = { "30 (as on Xbox)", "60", "120", "144" };
        static const int values[] = { 30, 60, 120, 144 };
        int cur = xbox_setting_int("RECOMP_GAME_HZ", 60), sel = 1;
        for (int i = 0; i < 4; i++)
            if (values[i] == cur)
                sel = i;
        if (ImGui::Combo("Frame rate", &sel, names, 4)) {
            set_int("RECOMP_GAME_HZ", values[sel]);
            set_int("RECOMP_FLIP_HZ", values[sel]);
        }
        ImGui::SetItemTooltip("The game steps once per frame at this rate, so game speed"
                              " stays right while your PC holds it; if it cannot, the"
                              " game slows down, as the console did.");
    }

    ImGui::SeparatorText("Controls");
    {
        static const char *names[] = { "Automatic (follows the pad)", "Original (Xbox)",
                                       "Xbox Series", "PlayStation", "Keyboard & Mouse" };
        static const char *values[] = { "auto", "original", "xbox", "playstation", "keyboard" };
        const char *cur = xbox_setting_get("RECOMP_BUTTON_PROMPTS");
        int sel = 0;
        for (int i = 1; i < 5; i++)
            if (cur && !_stricmp(cur, values[i]))
                sel = i;
        if (ImGui::Combo("Button prompts", &sel, names, 5))
            xbox_setting_set("RECOMP_BUTTON_PROMPTS", values[sel]);
        ImGui::SetItemTooltip("Which buttons the game's prompts show. Automatic: the"
                              " original art with an Xbox pad, PlayStation symbols with"
                              " a DualShock or DualSense.");
        int rumble = xbox_setting_int("RECOMP_RUMBLE", 100);
        if (ImGui::SliderInt("Vibration", &rumble, 0, 100, rumble ? "%d%%" : "Off",
                             ImGuiSliderFlags_AlwaysClamp))
            set_int("RECOMP_RUMBLE", rumble);
        ImGui::SetItemTooltip("Controller vibration strength. 0 turns it off.");
        bool hid = xbox_setting_int("RECOMP_HID_PADS", 1) != 0;
        if (ImGui::Checkbox("DualShock / DualSense support", &hid))
            set_int("RECOMP_HID_PADS", hid ? 1 : 0);
        ImGui::SetItemTooltip("Reads PlayStation pads directly, over USB or Bluetooth."
                              " Turn off if Steam Input or DS4Windows already does.");
        ImGui::TextDisabled("%d PlayStation pad(s) connected; using %s", hidpad_count(),
                            xbox_InputActiveKind() == 2 ? "keyboard and mouse"
                            : xbox_InputActiveKind() ? "PlayStation" : "Xbox");
    }

    bool to_binds = InterlockedExchange(&g_show_binds, 0) != 0;
    if (to_binds)
        ImGui::SetNextItemOpen(true);
    if (ImGui::CollapsingHeader("Keyboard and mouse controls")) {
        if (to_binds)
            ImGui::SetScrollHereY(0.0f);    /* the header at the top */
        bindings();
        ImGui::Spacing();
    }

    ImGui::SeparatorText("Anti-aliasing");
    {
        static const char *names[] = { "Off", "FXAA (cheap)", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
        static const int values[] = { 0, 1, 2, 4, 8 };
        int cur = xbox_setting_int("RECOMP_AA", 0), sel = 0;
        for (int i = 0; i < 5; i++)
            if (values[i] == cur)
                sel = i;
        if (ImGui::Combo("Anti-aliasing", &sel, names, 5))
            set_int("RECOMP_AA", values[sel]);
        ImGui::SetItemTooltip("Smooths jagged edges. FXAA is a cheap filter over the whole"
                              " picture; MSAA is sharper but costs more, most at high"
                              " resolutions.");
    }
    if (xbox_Quirk("bloom_composites")) {     /* a title that says where its bloom is */
        int pct = (int)(xbox_setting_float("RECOMP_BLOOM", 1.0f) * 100.0f + 0.5f);
        if (ImGui::SliderInt("Bloom", &pct, 0, 100, "%d%%"))
            set_float("RECOMP_BLOOM", (float)pct / 100.0f);
        ImGui::SetItemTooltip("The glow around bright things. 100%% is the"
                              " game as it shipped.");
    }

    ImGui::SeparatorText("Textures");
    {
        static const char *names[] = { "Off", "2x", "4x", "8x", "16x" };
        static const int values[] = { 0, 2, 4, 8, 16 };
        int cur = xbox_setting_int("RECOMP_ANISO", 0), sel = 0;
        for (int i = 0; i < 5; i++)
            if (values[i] == cur)
                sel = i;
        if (ImGui::Combo("Anisotropic filtering", &sel, names, 5))
            set_int("RECOMP_ANISO", values[sel]);
    }

    ImGui::SeparatorText("HD texture pack");
    {
        bool use = xbox_setting_int("RECOMP_TEXPACK", 1) != 0;
        if (ImGui::Checkbox("HD textures", &use))
            set_int("RECOMP_TEXPACK", use ? 1 : 0);
        ImGui::SetItemTooltip("The upscaled textures in spartan\\textures\\replace.");
        bool dump = xbox_setting_int("RECOMP_TEXPACK_DUMP", 0) != 0;
        if (ImGui::Checkbox("Dump textures while playing", &dump))
            set_int("RECOMP_TEXPACK_DUMP", dump ? 1 : 0);
        if (ImGui::Button("Dump all loaded textures now (F9)"))
            InterlockedExchange(&g_scan, 1);
        ImGui::Text("%ld dumped this run, %ld HD textures loaded", texpack_dumped_count(),
                    texpack_loaded_count());
        if (g_scan_found >= 0)
            ImGui::TextDisabled("Last scan: %d new texture(s)", g_scan_found);
    }

    ImGui::SeparatorText("Info");
    {
        bool fps = xbox_setting_int("RECOMP_SHOW_FPS", 0) != 0;
        if (ImGui::Checkbox("Show frame rate", &fps))
            set_int("RECOMP_SHOW_FPS", fps ? 1 : 0);
        ImGui::Text("%.1f fps", xbox_FramebufferFps());
    }

    ImGui::Separator();
    if (ImGui::Button("Save")) {
        if (xbox_settings_save())
            snprintf(g_status, sizeof g_status, "Saved to %s", xbox_settings_path());
        else
            snprintf(g_status, sizeof g_status, "Could not write %s", xbox_settings_path());
    }
    ImGui::SameLine();
    if (ImGui::Button("Close (F1)"))
        open = false;
    if (g_status[0])
        ImGui::TextDisabled("%s", g_status);
    ImGui::End();
    if (!open)
        InterlockedExchange(&g_visible, 0);
}

/* A control's keys, as up to two names. */
void binds_get(int c, char out[2][24])
{
    char key[40], list[96], *tok, *ctx = NULL;
    const char *v;
    int n = 0;
    out[0][0] = out[1][0] = 0;
    v = xbox_setting_get(xbox_InputBindSetting(c, key, sizeof key));
    snprintf(list, sizeof list, "%s", v && *v ? v : xbox_InputBindDefault(c));
    for (tok = strtok_s(list, ", ", &ctx); tok && n < 2; tok = strtok_s(NULL, ", ", &ctx))
        if (_stricmp(tok, "NONE"))
            snprintf(out[n++], 24, "%s", tok);
}

void binds_set(int c, char keys[2][24])
{
    char key[40], v[64];
    if (keys[0][0] && keys[1][0] && _stricmp(keys[0], keys[1]))
        snprintf(v, sizeof v, "%s,%s", keys[0], keys[1]);
    else
        snprintf(v, sizeof v, "%s", keys[0][0] ? keys[0] : keys[1][0] ? keys[1] : "NONE");
    xbox_setting_set(xbox_InputBindSetting(c, key, sizeof key), v);
}

/* Keyboard and mouse: each control's two keys; click one, press the new. */
void bindings()
{
    LONG cap = g_capture;
    if (cap >= 0) {
        LONG vk = InterlockedExchange(&g_captured, 0);
        if (vk) {
            int c = cap / 2, slot = cap % 2;
            char keys[2][24];
            const char *name = xbox_InputKeyName((int)vk);
            if (vk == VK_ESCAPE) {
                InterlockedExchange(&g_capture, -1);
            } else if (vk == VK_DELETE || name) {
                binds_get(c, keys);
                snprintf(keys[slot], 24, "%s", vk == VK_DELETE ? "" : name);
                binds_set(c, keys);
                InterlockedExchange(&g_capture, -1);
            }
        }
    }
    if (g_capture >= 0)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "Press a key or mouse button for \"%s\" (Esc cancels, Delete clears)",
                           xbox_InputBindLabel((int)(g_capture / 2)));
    else
        ImGui::TextDisabled("Click a key to change it.");
    if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Also", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();
        for (int c = 0; c < xbox_InputBindCount(); c++) {
            char keys[2][24];
            binds_get(c, keys);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(xbox_InputBindLabel(c));
            for (int slot = 0; slot < 2; slot++) {
                char id[48];
                bool waiting = g_capture == c * 2 + slot;
                ImGui::TableNextColumn();
                snprintf(id, sizeof id, "%s##b%d_%d", waiting ? "..." : keys[slot][0] ? keys[slot]
                         : "-", c, slot);
                if (ImGui::Button(id, ImVec2(110.0f, 0))) {
                    InterlockedExchange(&g_captured, 0);
                    InterlockedExchange(&g_capture, c * 2 + slot);
                }
            }
        }
        ImGui::EndTable();
    }
    {
        float sens = xbox_setting_float("RECOMP_MOUSE_SENS", 1.0f);
        if (ImGui::SliderFloat("Mouse sensitivity", &sens, 0.25f, 4.0f, "%.2f"))
            set_float("RECOMP_MOUSE_SENS", sens);
    }
    if (ImGui::Button("Reset to defaults")) {
        for (int c = 0; c < xbox_InputBindCount(); c++) {
            char key[40];
            xbox_setting_set(xbox_InputBindSetting(c, key, sizeof key), "");
        }
        set_float("RECOMP_MOUSE_SENS", 1.0f);
    }
}

void fps_corner()
{
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                 | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing
                 | ImGuiWindowFlags_NoNav);
    ImGui::Text("%.0f fps", xbox_FramebufferFps());
    ImGui::End();
}

} // namespace

extern "C" {

void xbox_overlay_init(HWND hwnd, ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    if (g_ready)
        return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;                   /* our ini, not ImGui's */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    {
        /* A readable size on a TV-distance window; ImGui's built-in font
         * otherwise. */
        char path[MAX_PATH];
        UINT n = GetWindowsDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH - 32) {
            strcat(path, "\\Fonts\\segoeui.ttf");
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
                io.Fonts->AddFontFromFileTTF(path, 20.0f);
        }
    }
    ImGui::GetStyle().ScaleAllSizes(1.25f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(dev, ctx);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    g_boot_aspect = xbox_setting_float("RECOMP_ASPECT", 0.0f);
    if (g_boot_aspect < 0.5f || g_boot_aspect > 8.0f)
        g_boot_aspect = 0.0f;
    g_hwnd = hwnd;
    g_ready = true;
    fprintf(stderr, "[OVERLAY] settings overlay ready: F1\n");
}

/* Window thread. Returns nonzero when the overlay takes the message. */
int xbox_overlay_msg(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_visible && g_capture >= 0) {
        LONG vk = 0;
        switch (m) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: vk = (LONG)w; break;
        case WM_LBUTTONDOWN: vk = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: vk = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: vk = VK_MBUTTON; break;
        case WM_XBUTTONDOWN:
            vk = GET_XBUTTON_WPARAM(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            break;
        }
        if (vk) {
            InterlockedExchange(&g_captured, vk);
            return 1;
        }
    }
    if (m == WM_KEYDOWN && w == VK_F9 && !(l & (1 << 30))) {
        InterlockedExchange(&g_scan, 1);
        return 1;
    }
    if (m == WM_KEYDOWN && w == VK_F1 && !(l & (1 << 30))) {
        InterlockedExchange(&g_visible, !g_visible);
        return 1;
    }
    switch (m) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SETFOCUS:
    case WM_KILLFOCUS: case WM_MOUSELEAVE:
        AcquireSRWLockExclusive(&g_qlock);
        if (g_qn < (int)(sizeof g_q / sizeof g_q[0]))
            g_q[g_qn++] = Msg{ h, m, w, l };
        ReleaseSRWLockExclusive(&g_qlock);
        break;
    default:
        break;
    }
    return g_visible && m != WM_KILLFOCUS && m != WM_SETFOCUS
           && (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR
               || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST));
}

/* Open the overlay at the key bindings (the game's own menu asks for it). */
void xbox_overlay_show_bindings(void)
{
    InterlockedExchange(&g_show_binds, 1);
    InterlockedExchange(&g_visible, 1);
}

int xbox_overlay_visible(void)
{
    return InterlockedCompareExchange(&g_visible, 0, 0) != 0;
}

/* Presenting thread, with the swap chain's target bound. */
void xbox_overlay_render(void)
{
    Msg q[512];
    int n;
    LARGE_INTEGER now;

    if (!g_ready)
        return;
    QueryPerformanceCounter(&now);
    {
        float dt = (float)(now.QuadPart - g_last.QuadPart) / (float)g_freq.QuadPart;
        if (dt > 0)
            g_fps = g_fps ? g_fps * 0.95f + (1.0f / dt) * 0.05f : 1.0f / dt;
        g_last = now;
    }
    AcquireSRWLockExclusive(&g_qlock);
    n = g_qn;
    memcpy(q, g_q, sizeof(Msg) * (size_t)n);
    g_qn = 0;
    ReleaseSRWLockExclusive(&g_qlock);
    for (int i = 0; i < n; i++)
        ImGui_ImplWin32_WndProcHandler(q[i].h, q[i].m, q[i].w, q[i].l);

    if (InterlockedExchange(&g_scan, 0))
        g_scan_found = texpack_scan_memory();

    bool show = xbox_overlay_visible();
    bool fps = xbox_setting_int("RECOMP_SHOW_FPS", 0) != 0;
    if (!show && !fps)
        return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = false;
    if (fps)
        fps_corner();
    if (show)
        panel();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // extern "C"
