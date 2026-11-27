// The PC host (Windows): the same game and renderer as the Switch build (host-switch/vulkan.cpp on the PC's Vulkan),
// in a Win32 window, to find and fix the Switch's bugs on the PC. The game is described by app::kGame.
//
// MPayne2-nx_pc.exe [game folder] [--docked] [--windowed]
//   game folder: the disc's files (default.xbe and its folders); without it <exe>/game or the env XBE_GAME_DIR
//   --docked: a 1920x1080 window, the Switch docked; 1280x720 (handheld) without it
//   --windowed: a window even when the settings ask for borderless (F11 / Alt+Enter switch either way)
// Files beside the exe while it runs (for scripted tests):
//   cmd   "BUTTONS [frames]" presses those buttons (input.cpp), then is deleted
//   shot  saves the next frame's back buffer as shots/NNNN.bmp, then is deleted
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "renderdoc_app.h"

namespace {

HWND g_hwnd = nullptr;
bool g_docked = false;
bool g_windowed_arg = false;  // --windowed: a window whatever the settings say (the scripted test runs)
std::atomic<uint32_t> g_surface_epoch{0};
std::filesystem::path g_exe_dir;
std::filesystem::path g_game_arg;
// MPayne2-nx (2026-11-24): the window is a resizable window or a borderless one over its whole screen (display_mode;
// "fullscreen" is taken as borderless), F11 or Alt+Enter switching; the frame is drawn at the output's size
// (ScreenMode) and scaled to the window, letterboxed to the game's aspect, by the renderers' last pass
std::atomic<bool> g_borderless{false};
RECT g_window_rect{};                  // the window's place while borderless, to go back to
std::atomic<int> g_client_w{0}, g_client_h{0};  // the last client size that was not empty (a minimized window has none)
constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;

// the size the game's frame is drawn at: 1280x720 (the target; 1920x1080 with --docked), or the settings' width and
// height when both are set (a sharper picture, the player's choice)
app::Mode ScreenMode() {
    const app::Settings& s = app::Get();
    if (s.width && s.height) return app::Mode{s.width, s.height};
    return g_docked ? app::Mode{1920, 1080} : app::Mode{1280, 720};
}

std::filesystem::path FindExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

}  // namespace

namespace host {

void* Window() { return g_hwnd; }
bool Foreground() { return true; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }
void WaitForeground() {}

// the window's client size (the swap chain's); while minimized, the last one it had
void WindowPixels(int& w, int& h) {
    RECT r{};
    if (g_hwnd && !IsIconic(g_hwnd) && GetClientRect(g_hwnd, &r) && r.right > 0 && r.bottom > 0) {
        g_client_w = int(r.right);
        g_client_h = int(r.bottom);
    }
    w = g_client_w;
    h = g_client_h;
    if (w <= 0 || h <= 0) {
        app::Mode m = ScreenMode();
        w = int(m.width);
        h = int(m.height);
    }
}

const std::filesystem::path& GameArg() { return g_game_arg; }

// "shot" beside the exe: the next frame is saved
bool TakeShot() {
    std::error_code ec;
    std::filesystem::path p = g_exe_dir / "shot";
    if (!std::filesystem::exists(p, ec)) return false;
    std::filesystem::remove(p, ec);
    return true;
}

void SaveShot(const uint8_t* bgra, uint32_t w, uint32_t h) {
    static uint32_t n = 0;
    std::error_code ec;
    std::filesystem::create_directories(g_exe_dir / "shots", ec);
    char name[32];
    std::snprintf(name, sizeof(name), "%04u.bmp", n++);
    std::ofstream f(g_exe_dir / "shots" / name, std::ios::binary);
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    uint32_t bytes = w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + bytes;
    ih.biSize = sizeof(ih);
    ih.biWidth = LONG(w);
    ih.biHeight = -LONG(h);  // top down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    // opaque: the guest's alpha is not the picture's
    std::vector<uint8_t> px(bgra, bgra + bytes);
    for (uint32_t i = 3; i < bytes; i += 4) px[i] = 0xFF;
    f.write(reinterpret_cast<const char*>(px.data()), bytes);
    LOGI("pc: shot %s (%ux%u)", name, w, h);
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// as the Switch: 60 frames a second (the user's target since 2026-11-22; 30 before), vsync on, the frame at the
// screen's height. The game times itself by the clock, so it plays at the same speed at any rate.
Settings DefaultSettings() {
    Settings s;
    s.render_percent = 100;
    s.fps_cap = 60;
    s.vsync = true;
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s, PC", kGame.title, IMAGE_XBE, date, PORT_BUILD);
    LOGR("Path: %s", ExeDir().string().c_str());
    Mode desk = Desktop();
    LOGR("Display: %ux%u (%s)", desk.width, desk.height, g_docked ? "as docked" : "as handheld");
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() { return ScreenMode(); }

Mode Output() { return Desktop(); }

bool Widescreen() {
    if (std::getenv("MP2_43")) return false;  // MPayne2-nx test: the game told the TV is 4:3
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

std::filesystem::path ExeDir() { return g_exe_dir; }

int Instance() { return 0; }

void* Window() { return g_hwnd; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

namespace {
std::atomic<uint32_t> g_hold_fps{0};
std::atomic<int64_t> g_hold_until{0};  // steady clock ticks
}  // namespace

void HoldFrameRate(uint32_t fps) {
    g_hold_fps = fps;
    g_hold_until = (std::chrono::steady_clock::now() + std::chrono::milliseconds(250)).time_since_epoch().count();
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (uint32_t hold = g_hold_fps; hold && now.time_since_epoch().count() < g_hold_until) cap = hold;
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "pc_win32";
}

// a crash: where on the host, and the guest's stack when it happened on a guest thread
LONG WINAPI Crash(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    HMODULE exe = GetModuleHandleW(nullptr);
    uintptr_t pc = uintptr_t(er->ExceptionAddress);
    uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    uint64_t fault = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    LOGE("crash: exception %08lX at %p (exe+%llX), address %016llX", er->ExceptionCode, er->ExceptionAddress,
         (unsigned long long)(pc - uintptr_t(exe)), (unsigned long long)fault);
    // the function's name from the PDB, when lld wrote one
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(proc, nullptr, TRUE)) {
        alignas(8) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, pc, &disp, si)) LOGE("crash: in %s+%llX", si->Name, (unsigned long long)disp);
    }
    if (base && fault >= base && fault < base + (1ull << 32)) LOGE("crash: guest address %08X", uint32_t(fault - base));
    LOGE("crash: rax %016llX rbx %016llX rcx %016llX rdx %016llX", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOGE("crash: rsi %016llX rdi %016llX rsp %016llX rbp %016llX", c->Rsi, c->Rdi, c->Rsp, c->Rbp);
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash: on guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    }
    logx::Flush();
    TerminateProcess(proc, 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

namespace input {
// host-pc/input.cpp (MPayne2-nx, 2026-11-24): the mouse for the game's mouse look and the pad's buttons
void PcMouseMove(int dx, int dy);
void PcMouseWheel(int notches);
void PcMouseCaptured(bool on, bool by_click);
}  // namespace input

namespace {

// MPayne2-nx (2026-11-24): the window has the mouse while it is the active window and either borderless or clicked into:
// the pointer hidden and kept inside it, raw counts to the game's mouse look. Losing the focus (Alt+Tab) gives it back
std::atomic<bool> g_captured{false};

void ClipToClient() {
    RECT r{};
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);
}

void Capture(bool on, bool by_click = false) {
    if (on == g_captured) return;
    g_captured = on;
    if (on) ClipToClient();
    else ClipCursor(nullptr);
    input::PcMouseCaptured(on, by_click);
    LOGI("pc: mouse %s", on ? "taken for the game" : "given back");
}

// borderless over the window's whole screen, or back to the window it was (main thread: the window's own)
void SetBorderless(bool on) {
    if (!g_hwnd || on == g_borderless) return;
    if (on) {
        GetWindowRect(g_hwnd, &g_window_rect);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& s = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, LONG_PTR(WS_POPUP | WS_VISIBLE));
        SetWindowPos(g_hwnd, HWND_TOP, s.left, s.top, s.right - s.left, s.bottom - s.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    } else {
        const RECT& r = g_window_rect;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, LONG_PTR(kWindowedStyle | WS_VISIBLE));
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    g_borderless = on;
    RECT c{};
    GetClientRect(g_hwnd, &c);
    LOGI("pc: %s, %ldx%ld", on ? "borderless over the screen" : "a window", c.right, c.bottom);
    if (g_captured) ClipToClient();
    else if (on && GetForegroundWindow() == g_hwnd) Capture(true);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        LOGI("pc: window closed");
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_CHAR:
        if (ImGui::GetCurrentContext() && wp < 0x10000) {
            std::lock_guard lock(app::UiLock());
            ImGui::GetIO().AddInputCharacter(unsigned(wp));
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) {  // not the key's repeats
            SetBorderless(!g_borderless);
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
            if (!(lp & (1 << 30))) SetBorderless(!g_borderless);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;  // no beep for Alt+Enter
        break;
    case WM_SETCURSOR:
        // no pointer over the game while it has the mouse, or borderless
        if ((g_captured || g_borderless) && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) Capture(false);
        else if (g_borderless) Capture(true, LOWORD(wp) == WA_CLICKACTIVE);  // that click is not a shot
        break;
    case WM_LBUTTONDOWN:
        if (!g_captured) Capture(true, true);
        return 0;
    case WM_SIZE:
    case WM_MOVE:
        if (g_captured) ClipToClient();
        break;
    case WM_INPUT: {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (g_captured && GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            (ri.data.mouse.lLastX || ri.data.mouse.lLastY))
            input::PcMouseMove(int(ri.data.mouse.lLastX), int(ri.data.mouse.lLastY));
        break;  // DefWindowProc cleans up after WM_INPUT
    }
    case WM_MOUSEWHEEL:
        if (g_captured) input::PcMouseWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND MakeWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));  // the port's icon (host-pc/CMakeLists.txt), else none
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"XboxSwitchPortKitPC";
    RegisterClassW(&wc);
    app::Mode m = ScreenMode();
    RECT r{0, 0, LONG(m.width), LONG(m.height)};
    AdjustWindowRect(&r, kWindowedStyle, FALSE);
    std::wstring title = std::wstring(app::kGame.title) + L" (MPayne2-nx PC, build " + std::wstring(PORT_BUILD, PORT_BUILD + std::strlen(PORT_BUILD)) + L")";
    HWND h = CreateWindowExW(0, wc.lpszClassName, title.c_str(), kWindowedStyle, 40, 40, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    // in front for a player; the scripted test runs (--windowed) stay behind whatever the user is doing
    ShowWindow(h, g_windowed_arg ? SW_SHOWNOACTIVATE : SW_SHOW);
    // the mouse's raw counts (WM_INPUT), only while the window is in front
    RAWINPUTDEVICE rid{0x01, 0x02, 0, h};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) LOGE("pc: raw mouse input not registered (%lu)", GetLastError());
    return h;
}

void Fatal(const char* text) {
    LOGE("%s", text);
    logx::Flush();
    MessageBoxA(nullptr, text, PORT_TITLE, MB_OK | MB_ICONERROR);
}

}  // namespace

// MPayne2-nx test aid: the sampling profiler (host-pc/profiler.cpp, MP2_PROF=START:SECONDS)
void StartProfiler(const std::filesystem::path& exe_dir);

int main(int argc, char** argv) {
    // the window's pixels are the screen's (a scaled desktop would blur the frame); Windows 10 1703 and later
    using DpiFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto set = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_exe_dir = FindExeDir();
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--docked")) g_docked = true;
        else if (!std::strcmp(argv[i], "--windowed")) g_windowed_arg = true;
        else g_game_arg = argv[i];
    }
    SetUnhandledExceptionFilter(Crash);
    std::set_terminate([] {
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
            LOGE("crash: std::terminate (abort, no exception)");
        } catch (const std::system_error& e) {
            LOGE("crash: uncaught std::system_error %d: %s", e.code().value(), e.what());
        } catch (const std::exception& e) {
            LOGE("crash: uncaught exception: %s", e.what());
        } catch (...) {
            LOGE("crash: uncaught exception of an unknown type");
        }
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), 3);
    });
    timeBeginPeriod(1);
    std::error_code ec;
    std::filesystem::create_directories(g_exe_dir / "logs", ec);
    std::freopen((g_exe_dir / "logs" / "stderr.log").string().c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    logx::Init((g_exe_dir / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();
    StartProfiler(g_exe_dir);

    app::GameFiles game;
    if (!app::FindGame(game)) {
        Fatal(PORT_TITLE ": the game files were not found. Pass the folder with default.xbe on the command line.");
        return 1;
    }

    g_hwnd = MakeWindow();
    if (app::Get().display_mode != app::DisplayMode::Windowed && !g_windowed_arg) SetBorderless(true);
    LOGI("app: ImGui");
    InitUi();
    LOGI("app: controllers");
    input::Init();
    LOGI("app: game window hook");
    if (app::kGame.window) app::kGame.window(g_hwnd);
    LOGI("app: booting the image");
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        Fatal(PORT_TITLE ": the game failed to start, see the log.");
        return 1;
    }

    // MPayne2-nx: started under RenderDoc (renderdoccmd capture), an "rdc" file beside the exe captures the next frame
    // (tools/run_pc.ps1 -RenderDoc, script step RDC)
    RENDERDOC_API_1_1_2* rdc = nullptr;
    if (HMODULE m = GetModuleHandleA("renderdoc.dll")) {
        auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(m, "RENDERDOC_GetAPI"));
        if (get && get(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&rdc)) == 1) LOGI("app: RenderDoc attached");
        else rdc = nullptr;
    }
    auto alive = std::chrono::steady_clock::now();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (rdc) {
            std::error_code rec;
            if (std::filesystem::remove(g_exe_dir / "rdc", rec)) {
                rdc->TriggerCapture();
                LOGI("app: RenderDoc capture triggered");
            }
        }
        if (std::chrono::steady_clock::now() - alive > std::chrono::seconds(5)) {
            alive = std::chrono::steady_clock::now();
            LOGI("app: alive");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
