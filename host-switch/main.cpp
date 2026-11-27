// The Switch host: the applet's loop, the settings and folders under sdmc:/switch/<PORT_NAME>, the screen size; the
// game is described by app::kGame (src/common/game_info.cpp). The Android host's main.cpp with libnx in place of SDL.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <ctime>
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

namespace {

constexpr const char* kDir = "sdmc:/switch/" PORT_NAME;

std::atomic<uint32_t> g_surface_epoch{0};
AppletOperationMode g_op_mode = AppletOperationMode_Handheld;

app::Mode ScreenMode() {
    return g_op_mode == AppletOperationMode_Console ? app::Mode{1920, 1080} : app::Mode{1280, 720};
}

}  // namespace

namespace host {

void* Window() { return nwindowGetDefault(); }
bool Foreground() { return true; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }
void WaitForeground() {}

void WindowPixels(int& w, int& h) {
    app::Mode m = ScreenMode();
    w = int(m.width);
    h = int(m.height);
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
                  {"join_ip", s.join_ip},
                  {"cpu_clock", int64_t(s.cpu_clock)},
                  {"gpu_clock", int64_t(s.gpu_clock)},
                  {"mem_clock", int64_t(s.mem_clock)},
                  {"thread_cores", int64_t(s.thread_cores)},
                  {"fps_adaptive", int64_t(s.fps_adaptive)},
                  {"fps_adaptive_ms", int64_t(s.fps_adaptive_ms)},
                  {"render_thread", int64_t(s.render_thread)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// MPayne2-nx (2026-11-22, the user's target "60 fps locked"): 60 frames a second, locked to 30 while the console falls
// well short of 60 (fps_adaptive, fps_adaptive_ms), the CPU at 1581 MHz and the memory at 1600 for the run (cpu_clock,
// mem_clock; the old clocks back at exit; a clock manager on the console, such as Horizon-OC, may hold them lower);
// vsync on, the frame at the screen's height. MPayne2-nx_30.toml in the package: 30 fps at the system's clock (cooler,
// longer on battery). The clocks (2026-11-23, the console survey): 1581 MHz with the memory at 1600 (the docked rate)
// is the lowest pair measured that holds 60 in every level tested; 1428 or 1581 alone, or 1428 with memory 1600, left
// the heaviest fights at 57-58.7 fps.
Settings DefaultSettings() {
    Settings s;
    s.render_percent = 100;
    s.fps_cap = 60;
    s.fps_adaptive = 1;
    s.cpu_clock = 1581;
    s.mem_clock = 1600;
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
    s.cpu_clock = t["cpu_clock"].value_or(s.cpu_clock);
    s.gpu_clock = t["gpu_clock"].value_or(s.gpu_clock);
    s.mem_clock = t["mem_clock"].value_or(s.mem_clock);
    s.thread_cores = t["thread_cores"].value_or(s.thread_cores);
    s.fps_adaptive = t["fps_adaptive"].value_or(s.fps_adaptive);
    s.fps_adaptive_ms = t["fps_adaptive_ms"].value_or(s.fps_adaptive_ms);
    s.render_thread = t["render_thread"].value_or(s.render_thread);
    s.profile = t["profile"].value_or(s.profile);  // test aids: read, never written back (Table leaves them out)
    s.profile_after = t["profile_after"].value_or(s.profile_after);
    s.guest_quantum_ms = t["guest_quantum_ms"].value_or(s.guest_quantum_ms);
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
    LOGR("%ls (%s), %s, build %s, Switch", kGame.title, IMAGE_XBE, date, PORT_BUILD);
    LOGR("Path: %s", ExeDir().c_str());
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    LOGR("Applet: %s, memory %llu MB (%llu MB used)",
         appletGetAppletType() == AppletType_Application ? "application (full memory)" : "applet",
         (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
    Mode desk = Desktop();
    LOGR("Display: %ux%u (%s)", desk.width, desk.height, g_op_mode == AppletOperationMode_Console ? "docked" : "handheld");
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() { return ScreenMode(); }

Mode Output() { return Desktop(); }

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

std::filesystem::path ExeDir() { return std::filesystem::path(kDir); }

int Instance() { return 0; }

void* Window() { return nwindowGetDefault(); }

// no splash and no relaunch into another executable on the Switch
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

// MPayne2-nx: the cap the last Pace used (after fps_adaptive and HoldFrameRate), for the swap interval
std::atomic<uint32_t> g_cap_now{0};
uint32_t FrameCapNow() { return g_cap_now; }

// MPayne2-nx: the render thread's handle (render_thread = 1), whose CPU time the adaptive cap weighs with the title's
std::atomic<uint32_t> g_render_thread{0};
void SetRenderThread(uint32_t handle) { g_render_thread = handle; }

namespace {
// a thread's CPU time in ms since the last call with the same last value, 0 when the kernel cannot tell (before 13.0.0)
double ThreadWorkMs(Handle h, uint64_t& last) {
    u64 ticks = 0;
    if (!h || R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) return -1.0;
    double ms = last && ticks >= last ? double(ticks - last) * 1000.0 / double(armGetSystemTickFreq()) : 0.0;
    last = ticks;
    return ms;
}
}  // namespace

void HoldFrameRate(uint32_t fps) {
    g_hold_fps = fps;
    g_hold_until = (std::chrono::steady_clock::now() + std::chrono::milliseconds(250)).time_since_epoch().count();
}

// MPayne2-nx (2026-11-23): when the last frame was paced (system ticks), for the clock keeper's stall report
// (host-switch/clocks.cpp): a level change hung twice on the console with the title's thread busy and no frame
std::atomic<uint64_t> g_last_frame_tick{0};
uint64_t LastFrameTick() { return g_last_frame_tick.load(std::memory_order_relaxed); }

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    static clock::time_point last_end = clock::now();
    g_last_frame_tick.store(armGetSystemTick(), std::memory_order_relaxed);
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    // MPayne2-nx: adaptive 60. The frame's work is the title's thread's CPU time since the last Pace; while its average
    // stays over 16 ms (60 cannot be held, vsync would show 40-57 fps with uneven frames) the cap drops to 30, and it
    // comes back to 60 once the average has stayed under 14.5 ms for 3 s. The game times itself by the clock, so its speed is the
    // same either way. Console figures (2026-11-22): chapter 1 about 17.5 ms a frame at 1020 MHz and 12 at 1785; level
    // 27 about 25 ms at 1020 (GL renderer). With the deko3d renderer chapter 1 takes about 13 ms at 1020 MHz (run dk2):
    // the way back was 13 ms, which it would not have met after a dip; 14.5 leaves 2 ms of the 16.7 ms frame.
    // MPayne2-nx (2026-11-23, the user's choice "let them run free"): the line is fps_adaptive_ms, 20 by default, and the
    // way back 1.5 ms under it (16 gives the old 16 / 14.5). With the render thread the heaviest scenes at 1020 MHz take
    // 17-18 ms (55-58 fps free) and the worst 20-23 (43-50 fps; survey3): under 20 they run free with a repeated frame
    // now and then, past it the frames would be uneven enough that a steady 30 reads better.
    if (cap == 60 && Get().fps_adaptive) {
        const double drop = std::clamp(double(Get().fps_adaptive_ms), 10.0, 33.0), back = drop - 1.5;
        static double avg = 0;
        static int over = 0, under = 0;
        static bool at30 = false;
        // the title's thread's own CPU time since the last Pace (the kernel's tick count for this thread, 13.0.0+): the
        // wait in the swap, which paces the frame at 30 with swap interval 2, is not work (wall time stayed at 33 ms
        // there and never let the cap come back to 60; run 14); wall time where the tick count is not available
        // MPayne2-nx: with the renderer on its own thread (render_thread), a frame is as long as the busier of the two
        static uint64_t last_ticks = 0, last_render_ticks = 0;
        double work = ThreadWorkMs(threadGetCurHandle(), last_ticks);
        if (work < 0) work = std::chrono::duration<double, std::milli>(now - last_end).count();
        else work = std::max(work, ThreadWorkMs(Handle(g_render_thread.load()), last_render_ticks));
        if (work > 0 && work < 200) avg = avg ? avg * 0.9 + work * 0.1 : work;  // a load or a pause is not a frame
        if (!at30) {
            over = avg > drop ? over + 1 : 0;
            if (over >= 30) {
                at30 = true;
                over = under = 0;
                LOGI("app: frames take %.1f ms on average (over %.1f): 30 fps until they take under %.1f", avg, drop,
                     back);
            }
        } else {
            under = avg < back ? under + 1 : 0;
            if (under >= 90) {
                at30 = false;
                over = under = 0;
                LOGI("app: frames take %.1f ms on average: back to 60 fps", avg);
            }
        }
        if (at30) cap = 30;
    }
    if (uint32_t hold = g_hold_fps; hold && now.time_since_epoch().count() < g_hold_until) cap = hold;
    g_cap_now = cap;
    // MPayne2-nx: at 60 or 30 with vsync on, the swap interval paces (1 or 2 vblanks, gl_backend.cpp SwapWindow): a timer
    // on top of vsync beat against it at 30 (frames of 40-50 ms when it released just after a vblank; run 13)
    if (!cap || (Get().vsync && (cap == 60 || cap == 30))) {
        next = now;
        last_end = clock::now();
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
    last_end = clock::now();
}

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "switch_libnx";
}

}  // namespace

// libnx calls this for any CPU exception: the report goes to the log, then the process ends, so the log's last lines
// say where. pc - main + main's ELF address (aarch64-none-elf-nm <PORT_NAME>.elf) is the ELF address;
// ferramentas/endereco_crash.sh does the sum.
int main(int, char**);

extern "C" {
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    uint64_t far = ctx->far.x;
    LOGE("crash: exception %u at pc %016llX lr %016llX sp %016llX, address %016llX", ctx->error_desc,
         (unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x,
         (unsigned long long)far);
    LOGE("crash: main at %p; guest base %p%s", reinterpret_cast<void*>(&main), kernel::g_base,
         base && far >= base && far < base + (1ull << 32) ? "" : " (address outside guest memory)");
    if (base && far >= base && far < base + (1ull << 32)) LOGE("crash: guest address %08X", uint32_t(far - base));
    for (int i = 0; i < 28; i += 4)
        LOGE("crash: x%-2d %016llX %016llX %016llX %016llX", i, (unsigned long long)ctx->cpu_gprs[i].x,
             (unsigned long long)ctx->cpu_gprs[i + 1].x, (unsigned long long)ctx->cpu_gprs[i + 2].x,
             (unsigned long long)ctx->cpu_gprs[i + 3].x);
    // cpu_gprs holds x0-x28; x29 is fp
    LOGE("crash: x28 %016llX fp %016llX", (unsigned long long)ctx->cpu_gprs[28].x, (unsigned long long)ctx->fp.x);
    // on a guest thread: its registers and the return addresses on its stack place the guest code
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash: on guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    }
    logx::Flush();
    svcExitProcess();
}
}

// the driver (Mesa) ends the process with abort() or a failed assert, not a CPU exception: say where first
extern "C" [[noreturn]] void abort() {
    LOGE("crash: abort() called from %p (main at %p)", __builtin_return_address(0), reinterpret_cast<void*>(&main));
    logx::Flush();
    std::fflush(stderr);
    svcExitProcess();
    __builtin_unreachable();
}

extern "C" [[noreturn]] void __assert_func(const char* file, int line, const char* func, const char* expr) {
    LOGE("crash: assert %s failed in %s, %s:%d", expr, func ? func : "?", file, line);
    abort();
}

namespace {

// a message to the player before the game runs: the console's error applet, then the title ends
void Fatal(const char* text) {
    LOGE("%s", text);
    logx::Flush();
    ErrorApplicationConfig c;
    errorApplicationCreate(&c, text, nullptr);
    errorApplicationShow(&c);
}

}  // namespace

namespace clocks {
void Apply(uint32_t cpu_mhz, uint32_t gpu_mhz, uint32_t mem_mhz);  // clocks.cpp
void Log();
void StartQuantum(uint32_t ms);
}  // namespace clocks
namespace prof {
void Start(uint32_t after_seconds);  // prof.cpp
}  // namespace prof
namespace pgo {
void Start();  // pgo.cpp: the profile-guided build's training run (nothing in a normal build)
}  // namespace pgo

int main(int, char**) {
    // an uncaught C++ exception or std::abort ends the process without a CPU exception: say so in the log first
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
        svcExitProcess();
    });
    appletLockExit();
    g_op_mode = appletGetOperationMode();
    std::filesystem::create_directories(std::filesystem::path(kDir) / "logs");
    // what the driver prints goes to stderr: a file beside the logs, written as it comes
    std::freopen((std::string(kDir) + "/logs/stderr.log").c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();
    clocks::Apply(app::Get().cpu_clock, app::Get().gpu_clock, app::Get().mem_clock);
    clocks::Log();
    kernel::g_thread_cores = int(app::Get().thread_cores);
    LOGI("app: guest threads on %s", kernel::g_thread_cores ? "the application's three cores" : "the process's core");
    if (app::Get().profile) prof::Start(app::Get().profile_after);
    clocks::StartQuantum(app::Get().guest_quantum_ms);
    pgo::Start();

    app::GameFiles game;
    if (!app::FindGame(game)) {
        Fatal(PORT_TITLE ": the game files were not found or are another release. Put the files of your disc in "
              "sdmc:/switch/" PORT_NAME "/game (default.xbe with its folders).");
        appletUnlockExit();
        return 1;
    }

    LOGI("app: ImGui");
    InitUi();
    LOGI("app: controllers");
    input::Init();
    LOGI("app: game window hook");
    if (app::kGame.window) app::kGame.window(nwindowGetDefault());
    LOGI("app: booting the image");
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        Fatal(PORT_TITLE ": the game failed to start, see the log in sdmc:/switch/" PORT_NAME "/logs.");
        appletUnlockExit();
        return 1;
    }

    // the applet's messages (HOME, docking, exit) on this thread; the game runs on its own
    auto alive = std::chrono::steady_clock::now();
    while (appletMainLoop() && !kernel::QuitAsked()) {
        // a line every 5 seconds: a log that stops with these still coming is a hang, not an exit
        if (std::chrono::steady_clock::now() - alive > std::chrono::seconds(5)) {
            alive = std::chrono::steady_clock::now();
            LOGI("app: alive");
            logx::Flush();  // the lines since the last one reach the card (the log is buffered, core/log.cpp)
        }
        AppletOperationMode m = appletGetOperationMode();
        if (m != g_op_mode) {
            g_op_mode = m;
            g_surface_epoch++;
            LOGI("app: %s", m == AppletOperationMode_Console ? "docked" : "handheld");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (kernel::QuitAsked()) kernel::FinishQuit();
    logx::Flush();
    kernel::Shutdown(0);
    appletUnlockExit();
    return 0;
}
