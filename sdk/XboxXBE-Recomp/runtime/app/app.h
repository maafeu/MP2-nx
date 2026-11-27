#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // MPayne2-nx, the Switch: the console's clocks for the run in MHz, 0 the system's (host-switch/clocks.cpp)
    uint32_t cpu_clock = 0, gpu_clock = 0, mem_clock = 0;
    // MPayne2-nx, the Switch: 1 lets every guest thread run on the application's three cores (kernel/threads.cpp)
    uint32_t thread_cores = 0;
    // MPayne2-nx, the Switch: with fps_cap 60, 1 locks to 30 while frames take over fps_adaptive_ms on average and goes
    // back to 60 once they take 1.5 ms less (host-switch/main.cpp, app::Pace); 0 leaves it free under 60. 20 ms (the
    // user's choice, 2026-11-23): scenes of 50-60 fps run free, slower ones lock to 30; 16 locks as soon as 60 slips
    uint32_t fps_adaptive = 1;
    uint32_t fps_adaptive_ms = 20;
    // MPayne2-nx, the Switch: 1 runs the renderer on a thread of its own (the title's thread records each call and goes
    // on, gpu/render_thread.cpp), where the renderer allows it (the deko3d build; GL's context is the title thread's)
    uint32_t render_thread = 1;
    // MPayne2-nx, the Switch, test aids read but never written back: 1 starts the sampling profiler (host-switch/prof.cpp),
    // from profile_after seconds
    uint32_t profile = 0, profile_after = 0;
    // MPayne2-nx, the Switch, a test aid read but never written back (2026-11-23): every guest_quantum_ms a guest thread
    // that used its whole core is taken off it for a moment (host-switch/clocks.cpp), as the Xbox's kernel shares a core
    // between threads of one priority by quantum and Horizon does not; 0 off
    uint32_t guest_quantum_ms = 0;
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();
// the kernel's XC_VIDEO_FLAGS say 480p; a game whose menus the flag changes clears it in its init (Cold Fear)
inline bool g_video_480p = true;

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();
// a frame rate the game needs for now, over the fps cap (a film made for 30 frames a second); 0 none. The game
// sets it each frame it holds: it lapses on its own a moment after the game stops
void HoldFrameRate(uint32_t fps);
// MPayne2-nx, the Switch host: the frame cap the last Pace used (after fps_adaptive and HoldFrameRate); 0 none yet
uint32_t FrameCapNow();
// MPayne2-nx, the Switch host: the render thread's handle (render_thread), whose CPU time Pace's adaptive cap weighs too
void SetRenderThread(uint32_t handle);

}  // namespace app
