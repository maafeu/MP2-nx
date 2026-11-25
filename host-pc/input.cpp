// The PC build's controllers: XInput pads, the keyboard as player 1, and the "cmd" file beside the exe for scripted
// presses. Everything goes through the Switch's button bits (HidNpadButton_*, host-pc/compat/switch.h), so the
// mapping to the Xbox pad, the options panel's navigation and Minus + R3 are the Switch build's.
//
// keyboard: arrows = D-pad, WASD = left stick, IJKL = right stick, Enter = A, Backspace = B, Q = X, E = Y,
// Tab = Plus (Start), Esc = Minus (Back), 1 = L (white), 2 = R (black), Ctrl = ZL, Shift = ZR, F/G = L3/R3,
// F10 = Minus + R3 (the options panel)
// cmd file: "A", "PLUS", "DOWN+A 10", ... buttons joined by +, held for that many frames (8 without)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "core/panel_chords.h"
#include "host.h"
#include "kernel/kernel.h"

namespace input {

namespace {

std::mutex g_lock;

// MP2-nx (2026-11-24): the mouse, while the window has it (host-pc/main.cpp): raw counts for the game's mouse look
// (input::TakeMouse, src/game/mouse.cpp), the buttons and the wheel as the pad's
std::mutex g_mouse_lock;
int g_mouse_x = 0, g_mouse_y = 0;
std::chrono::steady_clock::time_point g_mouse_take;
std::atomic<bool> g_mouse_on{false};
std::atomic<bool> g_mouse_swallow{false};  // the click that gave the window the mouse is not a shot
std::atomic<int> g_wheel{0};               // notches not yet pressed as the D-pad
std::chrono::steady_clock::time_point g_wheel_until;
uint64_t g_wheel_button = 0;

struct Raw {
    bool connected = false;
    uint64_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }

void Shape(int16_t& x, int16_t& y, uint32_t deadzone, float sensitivity) {
    if (!deadzone && sensitivity == 1.0f) return;
    constexpr float kFull = 32767.0f;
    float fx = float(x) / kFull, fy = float(y) / kFull;
    float m = std::sqrt(fx * fx + fy * fy);
    float dz = std::min(float(deadzone), 90.0f) / 100.0f;
    if (m <= dz) {
        x = y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz) * sensitivity / m;
    x = Axis(fx * k);
    y = Axis(fy * k);
}

// the scripted press from the cmd file, counted in polls of port 0
struct Script {
    uint64_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int left = 0;
    // MP2-nx: held for the frames as time (60 Hz) as well as for that many polls: a title that polls its pads
    // several times a frame saw an 8 poll press for under one frame
    std::chrono::steady_clock::time_point until{};
} g_script;

bool ScriptActive() { return g_script.left > 0 || std::chrono::steady_clock::now() < g_script.until; }

uint64_t ButtonByName(const std::string& n) {
    static const struct {
        const char* name;
        uint64_t bit;
    } kNames[] = {{"A", HidNpadButton_A},         {"B", HidNpadButton_B},         {"X", HidNpadButton_X},
                  {"Y", HidNpadButton_Y},         {"L3", HidNpadButton_StickL},   {"R3", HidNpadButton_StickR},
                  {"L", HidNpadButton_L},         {"R", HidNpadButton_R},         {"ZL", HidNpadButton_ZL},
                  {"ZR", HidNpadButton_ZR},       {"PLUS", HidNpadButton_Plus},   {"START", HidNpadButton_Plus},
                  {"MINUS", HidNpadButton_Minus}, {"BACK", HidNpadButton_Minus},  {"LEFT", HidNpadButton_Left},
                  {"UP", HidNpadButton_Up},       {"RIGHT", HidNpadButton_Right}, {"DOWN", HidNpadButton_Down}};
    for (auto& k : kNames)
        if (n == k.name) return k.bit;
    return 0;
}

// MP2-nx test aid (2026-11-24): "SCAN" in the cmd file writes the guest's readable memory below 64 MB to
// scans/NNN.bin beside the exe (records: guest address and length as two 32-bit words, then the bytes), for finding
// where the game keeps a value (tools/scan_diff.py)
void DumpGuest() {
    static int n = 0;
    std::error_code ec;
    std::filesystem::path dir = app::ExeDir() / "scans";
    std::filesystem::create_directories(dir, ec);
    char name[32];
    std::snprintf(name, sizeof(name), "%03d.bin", n++);
    FILE* f = _wfopen((dir / name).c_str(), L"wb");
    if (!f) return;
    constexpr uint64_t kTop = 0x4000000;
    uint64_t total = 0;
    for (uint64_t a = 0; a < kTop;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(kernel::g_base + a, &mi, sizeof(mi))) break;
        uint64_t end = std::min<uint64_t>(uint64_t(static_cast<uint8_t*>(mi.BaseAddress) - kernel::g_base) + mi.RegionSize, kTop);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            uint32_t head[2] = {uint32_t(a), uint32_t(end - a)};
            std::fwrite(head, 4, 2, f);
            std::fwrite(kernel::g_base + a, 1, size_t(end - a), f);
            total += end - a;
        }
        a = end;
    }
    std::fclose(f);
    LOGI("pc: scan %s, %llu bytes of guest memory", name, (unsigned long long)total);
}

// MP2-nx test aid (2026-11-24): "WATCH=XXXXXXXX" in the cmd file puts a hardware write watch (debug register 0, 4
// bytes, or 1 at an address not 4-aligned) on that guest address in every thread of the process; each write logs the host instruction after it and the
// recompiled function it is in (from the PDB), the first 4 times for each instruction. "WATCH=0" takes it off.
std::atomic<int> g_watch_hits{0};

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> any{0};
    if (any++ < 3) LOGI("pc: single step at %p, Dr6 %llX", e->ExceptionRecord->ExceptionAddress, (unsigned long long)e->ContextRecord->Dr6);
    e->ContextRecord->Dr6 = 0;
    static std::mutex m;
    static std::vector<std::pair<uintptr_t, int>> seen;
    uintptr_t pc = uintptr_t(e->ExceptionRecord->ExceptionAddress);
    std::lock_guard lock(m);
    int* count = nullptr;
    for (auto& s : seen)
        if (s.first == pc) count = &s.second;
    if (!count) {
        seen.push_back({pc, 0});
        count = &seen.back().second;
    }
    if ((*count)++ < 4 && g_watch_hits++ < 400) {
        HANDLE proc = GetCurrentProcess();
        static bool sym = SymInitialize(proc, nullptr, TRUE);
        alignas(8) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char* name = sym && SymFromAddr(proc, pc, &disp, si) ? si->Name : "?";
        LOGI("pc: watch write before %p (%s+%llX), thread %lu", e->ExceptionRecord->ExceptionAddress, name,
             (unsigned long long)disp, GetCurrentThreadId());
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetWatchFrom(uint32_t guest, DWORD self);

// from a helper thread, so the calling thread (the game's own, which reads the cmd file) gets the watch too
void SetWatch(uint32_t guest) {
    static PVOID handler = AddVectoredExceptionHandler(1, WatchHandler);
    (void)handler;
    std::thread([guest] { SetWatchFrom(guest, GetCurrentThreadId()); }).join();
}

void SetWatchFrom(uint32_t guest, DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = guest ? uintptr_t(kernel::g_base + guest) : 0;
            // DR0 on, break on writes (01), 4 bytes (11), or 1 byte (00) at an address not 4-aligned
            const uint64_t len = (guest & 3) ? 0ull : 3ull;
            c.Dr7 = guest ? (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (len << 18) : (c.Dr7 & ~0xF0003ull);
            if (SetThreadContext(t, &c)) n++;
            CONTEXT r{};
            r.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (n == 1 && GetThreadContext(t, &r))
                LOGI("pc: watch: thread %lu reads back Dr0 %llX Dr7 %llX", te.th32ThreadID, (unsigned long long)r.Dr0, (unsigned long long)r.Dr7);
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    LOGI("pc: write watch on guest %08X in %d threads", guest, n);
}

void ReadCmd() {
    std::filesystem::path p = app::ExeDir() / "cmd";
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return;
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) return;
    char b[128] = {};
    int frames = 8;
    int got = std::fscanf(f, "%127s %d", b, &frames);
    std::fclose(f);
    std::filesystem::remove(p, ec);
    if (got < 1) return;
    if (!std::strcmp(b, "SCAN")) {
        DumpGuest();
        return;
    }
    if (!std::strncmp(b, "WATCH=", 6)) {
        SetWatch(uint32_t(std::strtoul(b + 6, nullptr, 16)));
        return;
    }
    Script s;
    std::string all = b;
    size_t at = 0;
    while (at <= all.size()) {
        size_t e = all.find('+', at);
        if (e == std::string::npos) e = all.size();
        std::string n = all.substr(at, e - at);
        at = e + 1;
        if (n == "LS") s.ly = 32767;
        else if (n == "LSD") s.ly = -32767;
        else if (n == "LSL") s.lx = -32767;
        else if (n == "LSR") s.lx = 32767;
        else if (n == "RS") s.ry = 32767;
        else if (n == "RSD") s.ry = -32767;
        else if (n == "RSL") s.rx = -32767;
        else if (n == "RSR") s.rx = 32767;
        // MP2-nx (2026-11-24): a stick at part of its travel, -1..1 (RX=0.25): the camera's response measured
        else if (n.rfind("RX=", 0) == 0) s.rx = Axis(std::strtof(n.c_str() + 3, nullptr));
        else if (n.rfind("RY=", 0) == 0) s.ry = Axis(std::strtof(n.c_str() + 3, nullptr));
        else if (n.rfind("LX=", 0) == 0) s.lx = Axis(std::strtof(n.c_str() + 3, nullptr));
        else if (n.rfind("LY=", 0) == 0) s.ly = Axis(std::strtof(n.c_str() + 3, nullptr));
        // MP2-nx (2026-11-24): mouse counts, once (MX=200: right, MY=-100: up), as the raw mouse gives them
        else if (n.rfind("MX=", 0) == 0 || n.rfind("MY=", 0) == 0) {
            std::lock_guard lock(g_mouse_lock);
            (n[1] == 'X' ? g_mouse_x : g_mouse_y) += int(std::strtol(n.c_str() + 3, nullptr, 10));
            g_mouse_take = std::chrono::steady_clock::now();
        }
        else s.b |= ButtonByName(n);
    }
    s.left = std::max(frames, 1);
    s.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.left * 1000 / 60);
    g_script = s;
    LOGI("pc: cmd %s for %d polls", b, frames);
}

bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }


bool Focused() { return GetForegroundWindow() == static_cast<HWND>(app::Window()); }

Raw ReadKeyboard() {
    Raw r;
    if (!Focused()) return r;
    struct {
        int vk;
        uint64_t bit;
    } const kKeys[] = {{VK_UP, HidNpadButton_Up},        {VK_DOWN, HidNpadButton_Down},   {VK_LEFT, HidNpadButton_Left},
                       {VK_RIGHT, HidNpadButton_Right},  {VK_RETURN, HidNpadButton_A},    {VK_BACK, HidNpadButton_B},
                       {'Q', HidNpadButton_X},           {'E', HidNpadButton_Y},          {VK_TAB, HidNpadButton_Plus},
                       {VK_ESCAPE, HidNpadButton_Minus}, {'1', HidNpadButton_L},          {'2', HidNpadButton_R},
                       {VK_CONTROL, HidNpadButton_ZL},   {VK_SHIFT, HidNpadButton_ZR},    {'F', HidNpadButton_StickL},
                       {'G', HidNpadButton_StickR},      {VK_F10, HidNpadButton_Minus | HidNpadButton_StickR},
                       // MP2-nx (2026-11-24), for play with the mouse: Space jumps (A), R reloads (X), C crouches
                       {VK_SPACE, HidNpadButton_A},      {'R', HidNpadButton_X},          {'C', HidNpadButton_StickL}};
    // Alt+Enter switches the window (host-pc/main.cpp), so Enter held with Alt is not A
    const bool alt = Key(VK_MENU);
    for (auto& k : kKeys)
        if (Key(k.vk) && !(alt && k.vk == VK_RETURN)) r.b |= k.bit;
    if (Key('A')) r.lx = -32767;
    if (Key('D')) r.lx = 32767;
    if (Key('W')) r.ly = 32767;
    if (Key('S')) r.ly = -32767;
    if (Key('J')) r.rx = -32767;
    if (Key('L')) r.rx = 32767;
    if (Key('I')) r.ry = 32767;
    if (Key('K')) r.ry = -32767;
    // the mouse's buttons: left the right trigger (shoot), right the left trigger (shootdodge), middle B (bullet
    // time); the wheel a D-pad press up or down (the weapons) for each notch
    if (g_mouse_on) {
        const bool left = Key(VK_LBUTTON);
        if (g_mouse_swallow && !left) g_mouse_swallow = false;
        if (left && !g_mouse_swallow) r.b |= HidNpadButton_ZR;
        if (Key(VK_RBUTTON)) r.b |= HidNpadButton_ZL;
        if (Key(VK_MBUTTON)) r.b |= HidNpadButton_B;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_wheel_until) {
        g_wheel_button = 0;
        if (int w = g_wheel.load(); w) {
            g_wheel -= w > 0 ? 1 : -1;
            g_wheel_button = w > 0 ? HidNpadButton_Up : HidNpadButton_Down;
            g_wheel_until = now + std::chrono::milliseconds(120);
        }
    } else if (now >= g_wheel_until - std::chrono::milliseconds(40)) {
        r.b &= ~g_wheel_button;  // a gap between notches, so two read as two presses
    } else {
        r.b |= g_wheel_button;
    }
    r.connected = r.b || r.lx || r.ly || r.rx || r.ry;
    return r;
}

Raw ReadXInput(int port) {
    Raw r;
    XINPUT_STATE st{};
    if (XInputGetState(DWORD(port), &st) != ERROR_SUCCESS) return r;
    r.connected = true;
    const XINPUT_GAMEPAD& g = st.Gamepad;
    // positions as on the Switch: A is the Xbox A (bottom), B the right one
    struct {
        WORD x;
        uint64_t bit;
    } const kMap[] = {{XINPUT_GAMEPAD_DPAD_UP, HidNpadButton_Up},       {XINPUT_GAMEPAD_DPAD_DOWN, HidNpadButton_Down},
                      {XINPUT_GAMEPAD_DPAD_LEFT, HidNpadButton_Left},   {XINPUT_GAMEPAD_DPAD_RIGHT, HidNpadButton_Right},
                      {XINPUT_GAMEPAD_START, HidNpadButton_Plus},       {XINPUT_GAMEPAD_BACK, HidNpadButton_Minus},
                      {XINPUT_GAMEPAD_LEFT_THUMB, HidNpadButton_StickL}, {XINPUT_GAMEPAD_RIGHT_THUMB, HidNpadButton_StickR},
                      {XINPUT_GAMEPAD_LEFT_SHOULDER, HidNpadButton_L},  {XINPUT_GAMEPAD_RIGHT_SHOULDER, HidNpadButton_R},
                      {XINPUT_GAMEPAD_A, HidNpadButton_A},              {XINPUT_GAMEPAD_B, HidNpadButton_B},
                      {XINPUT_GAMEPAD_X, HidNpadButton_X},              {XINPUT_GAMEPAD_Y, HidNpadButton_Y}};
    for (auto& m : kMap)
        if (g.wButtons & m.x) r.b |= m.bit;
    if (g.bLeftTrigger > 64) r.b |= HidNpadButton_ZL;
    if (g.bRightTrigger > 64) r.b |= HidNpadButton_ZR;
    auto dz = [](SHORT v) { return int16_t(std::abs(v) < 7000 ? 0 : std::max<int>(v, -32767)); };
    r.lx = dz(g.sThumbLX);
    r.ly = dz(g.sThumbLY);
    r.rx = dz(g.sThumbRX);
    r.ry = dz(g.sThumbRY);
    return r;
}

Raw Read(int port) {
    Raw r = ReadXInput(port);
    if (port == 0) {
        Raw k = ReadKeyboard();
        r.connected = true;  // player 1 always has the keyboard
        r.b |= k.b;
        if (k.lx) r.lx = k.lx;
        if (k.ly) r.ly = k.ly;
        if (k.rx) r.rx = k.rx;
        if (k.ry) r.ry = k.ry;
        if (ScriptActive()) {
            r.b |= g_script.b;
            if (g_script.lx) r.lx = g_script.lx;
            if (g_script.ly) r.ly = g_script.ly;
            if (g_script.rx) r.rx = g_script.rx;
            if (g_script.ry) r.ry = g_script.ry;
        }
    }
    return r;
}

Raw g_last0;

}  // namespace

void Init() { LOGI("input: XInput and the keyboard (PC build), cmd file for scripted presses"); }

Pad Poll(int port) {
    Pad p;
    if (port < 0 || port > 3) return p;
    Raw r;
    {
        std::lock_guard lock(g_lock);
        if (port == 0) {
            if (g_script.left > 0) g_script.left--;
            else ReadCmd();
        }
        r = Read(port);
        if (port == 0) g_last0 = r;
    }
    if (!r.connected) return p;
    p.connected = true;
    constexpr uint64_t kButtons[8] = {HidNpadButton_Up,   HidNpadButton_Down,  HidNpadButton_Left,     HidNpadButton_Right,
                                      HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_StickL, HidNpadButton_StickR};
    for (int i = 0; i < 8; i++)
        if (r.b & kButtons[i]) p.buttons |= uint16_t(1u << i);
    constexpr uint64_t kAnalog[8] = {HidNpadButton_A, HidNpadButton_B, HidNpadButton_X,  HidNpadButton_Y,
                                     HidNpadButton_R, HidNpadButton_L, HidNpadButton_ZL, HidNpadButton_ZR};
    for (int i = 0; i < 8; i++) p.analog[i] = (r.b & kAnalog[i]) ? 0xFF : 0;
    p.lx = r.lx;
    p.ly = r.ly;
    p.rx = r.rx;
    p.ry = r.ry;
    // the options panel takes the controllers while it is open, and the game sees nothing of the chords that open and
    // close it (Back + R3, both sticks), not even the first stick of the two (core/panel_chords.h)
    static bool held = false;
    if (port == 0) held = input::PanelChords(p.buttons, app::ToggleMenu);
    if (app::MenuOpen() || held) {
        bool connected = p.connected;
        p = Pad{};
        p.connected = connected;
    }
    const game::Options& o = game::Opt();
    Shape(p.lx, p.ly, o.left_deadzone, o.left_sensitivity);
    Shape(p.rx, p.ry, o.right_deadzone, o.right_sensitivity);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    if (port < 0 || port > 3) return;
    XINPUT_VIBRATION v{left, right};
    XInputSetState(DWORD(port), &v);
}

uint64_t NavButtons() {
    std::lock_guard lock(g_lock);
    return Read(0).b;
}

void NavStick(float& x, float& y) {
    std::lock_guard lock(g_lock);
    Raw r = Read(0);
    x = r.lx / 32767.0f;
    y = r.ly / 32767.0f;
}

// MP2-nx (2026-11-24): the mouse, from the window (host-pc/main.cpp): raw counts while the window has the mouse, the
// wheel's notches, and whether it has the mouse (and whether the click that gave it is still held)
void PcMouseMove(int dx, int dy) {
    std::lock_guard lock(g_mouse_lock);
    g_mouse_x += dx;
    g_mouse_y += dy;
}

void PcMouseWheel(int notches) { g_wheel += notches; }

void PcMouseCaptured(bool on, bool by_click) {
    g_mouse_on = on;
    g_mouse_swallow = on && by_click;
    std::lock_guard lock(g_mouse_lock);
    g_mouse_x = g_mouse_y = 0;
}

// the game's mouse look (src/game/mouse.cpp), once a frame in play: the counts since the last call times the
// settings' mouse_sensitivity. After a pause (a menu, a film, the game not asking for 0.25 s) the counts piled up are
// dropped, not turned all at once
bool TakeMouse(float& dx, float& dy) {
    std::lock_guard lock(g_mouse_lock);
    const auto now = std::chrono::steady_clock::now();
    const bool stale = now - g_mouse_take > std::chrono::milliseconds(250);
    g_mouse_take = now;
    const int x = g_mouse_x, y = g_mouse_y;
    g_mouse_x = g_mouse_y = 0;
    if (stale || app::MenuOpen() || (!x && !y)) return false;
    const float s = game::Opt().mouse_sensitivity;
    dx = float(x) * s;
    dy = float(y) * s;
    return true;
}

}  // namespace input
