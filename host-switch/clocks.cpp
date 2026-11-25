// MP2-nx: the console's CPU, GPU and memory clocks for the run, from the settings file (cpu_clock, gpu_clock,
// mem_clock in MHz; 0 leaves the system's). Ported from the COLD-FEAR-NX-PORT base's clock code (src/switch_nx.c):
// through clkrst (8.0.0+) or pcv, as sys-clk does; the closest rate the hardware lists, never above the request, capped
// at 1785 / 921.6 / 1600 MHz; the old rates come back at exit. The system resets clocks on dock, undock and after
// sleep, so a thread sets them again every second while the game has focus.
//
// Why: the console's first runs (2026-11-22) put chapter 1 at about 20 ms of CPU a frame on the title's thread at the
// system's 1020 MHz (the recompiled game about 11 ms, the GL renderer and Mesa about 9 ms). 60 fps needs under 16.7 ms.
// More heat and battery: the settings decide (docs/MP2-notes.md, "60 fps").
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"

extern "C" void _start(void);  // the NRO's first instruction: ELF address 0 (as host-switch/prof.cpp)
namespace app {
uint64_t LastFrameTick();  // host-switch/main.cpp: when Pace last ran, in system ticks (0: no frame yet)
}

namespace clocks {
namespace {

struct Clock {
    const char* name;
    PcvModule mod;
    PcvModuleId id;
    uint32_t cap_hz;
    uint32_t old_hz = 0, set_hz = 0;
    bool open = false;
    ClkrstSession s{};
};

Clock g_clk[3] = {
    {"CPU", PcvModule_CpuBus, PcvModuleId_CpuBus, 1785000000u},
    {"GPU", PcvModule_GPU, PcvModuleId_GPU, 921600000u},
    {"memory", PcvModule_EMC, PcvModuleId_EMC, 1600000000u},
};
int g_rst = -1;  // 1 clkrst, 0 pcv, -1 not up
std::atomic<bool> g_stop{false};
Thread g_thread;
bool g_thread_up = false;
uint32_t g_quantum_ms = 0;  // guest_quantum_ms: the quantum thread (Quantum, below)
Thread g_qthread;
bool g_qthread_up = false;

Result Get(int i, uint32_t* hz) {
    return g_rst ? clkrstGetClockRate(&g_clk[i].s, hz) : pcvGetClockRate(g_clk[i].mod, hz);
}

Result Set(int i, uint32_t hz) {
    return g_rst ? clkrstSetClockRate(&g_clk[i].s, hz) : pcvSetClockRate(g_clk[i].mod, hz);
}

// the highest listed rate not above want (want itself when there is no list)
uint32_t Pick(int i, uint32_t want) {
    uint32_t rates[64], best = 0;
    s32 n = 0;
    PcvClockRatesListType type;
    Result rc = g_rst ? clkrstGetPossibleClockRates(&g_clk[i].s, rates, 64, &type, &n)
                      : pcvGetPossibleClockRates(g_clk[i].mod, rates, 64, &type, &n);
    if (R_FAILED(rc) || n <= 0 || type != PcvClockRatesListType_Discrete) return want;
    for (s32 k = 0; k < n; k++)
        if (rates[k] <= want && rates[k] > best) best = rates[k];
    return best ? best : want;
}

void Restore() {
    if (g_rst < 0) return;
    g_stop = true;
    if (g_thread_up) {
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread_up = false;
    }
    if (g_qthread_up) {
        threadWaitForExit(&g_qthread);
        threadClose(&g_qthread);
        g_qthread_up = false;
    }
    for (int i = 0; i < 3; i++)
        if (g_clk[i].set_hz && g_clk[i].old_hz) Set(i, g_clk[i].old_hz);
    for (int i = 0; i < 3; i++)
        if (g_clk[i].open) clkrstCloseSession(&g_clk[i].s);
    if (g_rst) clkrstExit();
    else pcvExit();
    g_rst = -1;
}

Handle g_main = 0;  // the host's main thread, for the census
double MeasureMhzOwnTicks();

// MP2-nx: every 10 s while in focus, the guest threads' CPU (the kernel's ticks for each, 13.0.0+) with their preferred
// core and core mask, the main thread's, and the CPU's clock measured on this thread's own ticks (so a busy core
// cannot skew it). For the 60 fps work: which threads share which core, and whether the clock stays where it was set.
void Census() {
    static std::map<uint32_t, uint64_t> last;
    static uint64_t last_tick = 0;
    const uint64_t now = armGetSystemTick();
    const double span = last_tick ? double(now - last_tick) : 0.0;
    last_tick = now;
    std::vector<uint32_t> hs = kernel::HostThreadHandles();
    if (g_main) hs.insert(hs.begin(), g_main);
    char line[512];
    int n = 0;
    line[0] = 0;
    for (size_t i = 0; i < hs.size(); i++) {
        u64 ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, hs[i], UINT64_MAX))) continue;
        uint64_t& prev = last[hs[i]];
        double pct = span > 0 && prev ? 100.0 * double(ticks - prev) / span : -1.0;
        prev = ticks;
        if (pct < 2.0) continue;
        s32 pref = -1;
        u64 mask = 0;
        svcGetThreadCoreMask(&pref, &mask, hs[i]);
        n += std::snprintf(line + n, sizeof(line) - size_t(n), "%s%s%zu (%X) core %d/%llX %.0f%%", n ? ", " : "",
                           g_main && i == 0 ? "main " : "#", g_main ? i : i + 1, unsigned(hs[i]), int(pref),
                           (unsigned long long)mask, pct);
        if (n > int(sizeof(line)) - 64) break;
    }
    if (span <= 0) return;
    const double mhz = MeasureMhzOwnTicks();
    LOGI("threads: %s; CPU at %.0f MHz", n ? line : "none over 2%", mhz);
    // the clock asked for but not running (clkrst reads back what was asked): asked again
    if (g_clk[0].set_hz && mhz > 0 && mhz < 0.9 * double(g_clk[0].set_hz) / 1e6) {
        Set(0, g_clk[0].set_hz);
        LOGI("clock: CPU measured at %.0f MHz under the %u asked: set again", mhz, g_clk[0].set_hz / 1000000u);
    }
}

uint32_t NroOff(uint64_t a) {
    const uint64_t b = reinterpret_cast<uintptr_t>(&_start);
    return a >= b && a - b < 0xFFFFFFFFull ? uint32_t(a - b) : 0xFFFFFFFFu;
}

// MP2-nx (2026-11-23): a stall report. Two level changes on the console (sweeps A and B, logs mp2_049 / mp2_050) hung
// with the title's thread at 100% and no frame drawn, which neither the PC nor the PC with the render thread ever did.
// Every second this notes which guest threads used over half a core; once no frame has been paced for 10 s it pauses
// each busy one for a moment (svcSetThreadActivity, as the profiler does), reads its pc, lr and five return addresses
// up the x29 chain, and logs them as offsets into the NRO (tools/prof_report.py's ELF names them): five reports 2 s
// apart, then one a minute. Nothing changes while frames come; a long load is a few lines.
void StallCheck() {
    static std::map<uint32_t, uint64_t> last;
    static uint64_t last_tick = 0, next_report = 0;
    static unsigned reports = 0;
    const uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
    const double span = last_tick ? double(now - last_tick) : 0.0;
    last_tick = now;
    std::vector<uint32_t> hs = kernel::HostThreadHandles();
    std::vector<size_t> busy;
    for (size_t i = 0; i < hs.size(); i++) {
        u64 t = 0;
        if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, hs[i], UINT64_MAX))) continue;
        uint64_t& prev = last[hs[i]];
        if (span > 0 && prev && double(t - prev) > 0.5 * span) busy.push_back(i);
        prev = t;
    }
    // a frame paced after `now` was read is newer than it: no stall (the unsigned difference once read as 960 billion
    // seconds, Ryujinx run reap2)
    const uint64_t frame = app::LastFrameTick();
    if (!frame || frame >= now || now - frame < 10 * freq) {
        reports = 0;
        next_report = 0;
        return;
    }
    if (now < next_report) return;
    next_report = now + (reports < 5 ? 2 : 60) * freq;
    reports++;
    const double s = double(now - frame) / double(freq);
    if (busy.empty()) {
        LOGW("stall: no frame for %.0f s, no guest thread busy", s);
        return;
    }
    for (size_t i : busy) {
        const Handle h = hs[i];
        uint32_t pc = ~0u, lr = ~0u, ret[5];
        std::fill(ret, ret + 5, ~0u);
        if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
        ThreadContext ctx;
        if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h))) {
            pc = NroOff(ctx.pc.x);
            lr = NroOff(ctx.lr);
            MemoryInfo mi;
            u32 pi;
            if ((ctx.fp & 7) == 0 && R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.fp)) && (mi.perm & Perm_R)) {
                uint64_t fp = ctx.fp, lo = mi.addr, hi = mi.addr + mi.size;
                for (int k = 0; k < 5 && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
                    const uint64_t* fr = reinterpret_cast<const uint64_t*>(uintptr_t(fp));
                    ret[k] = NroOff(fr[1]);
                    if (fr[0] <= fp) break;
                    fp = fr[0];
                }
            }
        }
        svcSetThreadActivity(h, ThreadActivity_Runnable);
        // numbered as the census numbers them (the main thread first)
        LOGW("stall: no frame for %.0f s; thread #%zu (%X) busy at pc %X lr %X, returns %X %X %X %X %X (NRO offsets)", s,
             i + 1, unsigned(h), pc, lr, ret[0], ret[1], ret[2], ret[3], ret[4]);
    }
}

// MP2-nx (2026-11-23), guest_quantum_ms (a test aid): the Xbox's kernel shares a core between threads of one priority
// by quantum; Horizon lets a thread keep its core until it waits or yields, and the guest threads all have one priority
// (on one core with thread_cores = 0). A guest thread that spins waiting for another guest thread on its core would then
// spin for good. Every guest_quantum_ms this takes each guest thread that used its whole core off it for a moment
// (paused until its context is read, then runnable again, at the back of its priority's queue), so another guest thread
// waiting for that core gets it until it waits in turn.
void Quantum(void*) {
    std::map<uint32_t, uint64_t> last;
    const uint64_t freq = armGetSystemTickFreq();
    uint64_t last_tick = armGetSystemTick(), t_log = last_tick;
    unsigned taken = 0;
    while (!g_stop) {
        svcSleepThread(uint64_t(g_quantum_ms) * 1000000ull);
        const uint64_t now = armGetSystemTick();
        const double span = double(now - last_tick);
        last_tick = now;
        for (uint32_t h : kernel::HostThreadHandles()) {
            u64 t = 0;
            if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h, UINT64_MAX))) continue;
            uint64_t& prev = last[h];
            const bool whole = prev && double(t - prev) > 0.9 * span;
            prev = t;
            if (!whole || R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
            ThreadContext ctx;
            svcGetThreadContext3(&ctx, h);  // returns once the thread is off its core
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            taken++;
        }
        if (now - t_log > 60 * freq) {
            LOGI("quantum: %u times a guest thread was taken off its core in the last minute", taken);
            taken = 0;
            t_log = now;
        }
    }
}

void Keeper(void*) {
    unsigned logged = 0;
    unsigned seconds = 0;
    while (!g_stop) {
        svcSleepThread(1000000000ull);
        if (g_stop || appletGetFocusState() != AppletFocusState_InFocus) continue;
        StallCheck();
        if (++seconds % 10 == 0) Census();
        for (int i = 0; i < 3; i++) {
            uint32_t hz = 0;
            if (!g_clk[i].set_hz || R_FAILED(Get(i, &hz)) || hz == g_clk[i].set_hz) continue;
            Set(i, g_clk[i].set_hz);
            if (logged++ < 8)
                LOGI("clock: %s was reset to %u MHz, back to %u MHz", g_clk[i].name, hz / 1000000u,
                     g_clk[i].set_hz / 1000000u);
        }
    }
}

bool Up() {
    if (g_rst >= 0) return true;
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) g_rst = 1;
    else if (R_SUCCEEDED(pcvInitialize())) g_rst = 0;
    else {
        LOGW("clock: neither clkrst nor pcv is available: clocks unchanged");
        return false;
    }
    std::atexit(Restore);
    // above the guest threads (0x20; the base's 0x3F never ran on the console while the game played: run 5 logged no
    // census, and a clock the system pulled back stayed back). It sleeps a second at a time.
    if (R_SUCCEEDED(threadCreate(&g_thread, Keeper, nullptr, nullptr, 0x4000, 0x20, -2)) &&
        R_SUCCEEDED(threadStart(&g_thread)))
        g_thread_up = true;
    return true;
}

void Want(int i, uint32_t want_hz) {
    if (want_hz > g_clk[i].cap_hz) want_hz = g_clk[i].cap_hz;
    if (!Up()) return;
    if (g_rst && !g_clk[i].open) {
        if (Result rc = clkrstOpenSession(&g_clk[i].s, g_clk[i].id, 3); R_FAILED(rc)) {
            LOGW("clock: %s: no clkrst session (0x%X)", g_clk[i].name, rc);
            return;
        }
        g_clk[i].open = true;
    }
    if (!g_clk[i].old_hz) Get(i, &g_clk[i].old_hz);
    uint32_t hz = Pick(i, want_hz), now = 0;
    Result rc = Set(i, hz);
    Get(i, &now);
    if (R_SUCCEEDED(rc)) g_clk[i].set_hz = now ? now : hz;
    LOGI("clock: %s %u -> %u MHz (asked %u)%s", g_clk[i].name, g_clk[i].old_hz / 1000000u, now / 1000000u,
         want_hz / 1000000u, R_FAILED(rc) ? " -- refused" : "");
}

// the CPU's real clock on this core: a chain of dependent adds (one a cycle on the Cortex-A57), eight an iteration
// so the loop's own count and branch run beside them, timed by the system tick. The clock clkrst reports is what was
// asked for; run 3 (2026-11-22) showed CPU-only work no faster at a reported 1785 MHz, so the log says both.
double MeasureMhz() {
    constexpr uint64_t kIters = 4000000;  // 32 million cycles: about 31 ms at 1020 MHz
    uint64_t x = 0;
    const uint64_t t0 = armGetSystemTick();
    for (uint64_t i = 0; i < kIters; i++) {
        asm volatile(
            "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
            "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1"
            : "+r"(x));
    }
    const uint64_t t1 = armGetSystemTick();
    const double s = double(t1 - t0) / double(armGetSystemTickFreq());
    return s > 0 ? double(kIters) * 8.0 / s / 1e6 : 0.0;
}

// the same chain timed by this thread's own CPU ticks (13.0.0+): right on a busy core too
double MeasureMhzOwnTicks() {
    constexpr uint64_t kIters = 250000;  // 2 million cycles: about 1-2 ms every 10 s
    const Handle me = threadGetCurHandle();
    u64 a = 0, b = 0;
    if (R_FAILED(svcGetInfo(&a, InfoType_ThreadTickCount, me, UINT64_MAX))) return 0.0;
    uint64_t x = 0;
    for (uint64_t i = 0; i < kIters; i++) {
        asm volatile(
            "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
            "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1"
            : "+r"(x));
    }
    if (R_FAILED(svcGetInfo(&b, InfoType_ThreadTickCount, me, UINT64_MAX)) || b <= a) return 0.0;
    const double s = double(b - a) / double(armGetSystemTickFreq());
    return double(kIters) * 8.0 / s / 1e6;
}

}  // namespace

// at boot, after the settings are read: each clock not 0 is set for the run
void Apply(uint32_t cpu_mhz, uint32_t gpu_mhz, uint32_t mem_mhz) {
    const uint32_t mhz[3] = {cpu_mhz, gpu_mhz, mem_mhz};
    g_main = threadGetCurHandle();
    Up();  // the keeper thread: the clocks kept, and the thread census every 10 s
    LOGI("clock: CPU measured at %.0f MHz before any change", MeasureMhz());
    bool any = false;
    for (int i = 0; i < 3; i++)
        if (mhz[i]) {
            Want(i, mhz[i] * 1000000u);
            any = true;
        }
    if (any) {
        svcSleepThread(50000000ull);  // the regulator's settle
        LOGI("clock: CPU measured at %.0f MHz after", MeasureMhz());
    }
}

// test_gpu_clocks (a test aid, src/game/test_level.cpp, 2026-11-23): the GPU at mhz from now on, kept by the keeper
// like gpu_clock; 0 puts back the rate it had before the run changed it (or leaves the system's) and stops keeping
// it. Either way the rate is read back and logged, so a step's frames can be read against it.
void TestGpu(uint32_t mhz) {
    if (!Up()) return;
    Clock& c = g_clk[1];
    if (mhz) {
        Want(1, mhz * 1000000u);
    } else {
        if (g_rst && !c.open) {
            if (Result rc = clkrstOpenSession(&c.s, c.id, 3); R_FAILED(rc)) {
                LOGW("clock: GPU: no clkrst session (0x%X)", rc);
                return;
            }
            c.open = true;
        }
        if (c.set_hz && c.old_hz) Set(1, c.old_hz);
        c.set_hz = 0;
    }
    uint32_t hz = 0;
    if (!g_rst || c.open) Get(1, &hz);
    LOGI("clock: GPU now %u.%u MHz (test_gpu_clocks step %u)", hz / 1000000u, (hz / 100000u) % 10u, mhz);
}

// guest_quantum_ms (a test aid, 0 off): the quantum thread above, beside the keeper
void StartQuantum(uint32_t ms) {
    if (!ms) return;
    g_quantum_ms = std::clamp<uint32_t>(ms, 1, 1000);
    if (R_SUCCEEDED(threadCreate(&g_qthread, Quantum, nullptr, nullptr, 0x4000, 0x20, -2)) &&
        R_SUCCEEDED(threadStart(&g_qthread))) {
        g_qthread_up = true;
        LOGI("quantum: a guest thread that uses its whole core is taken off it every %u ms", g_quantum_ms);
    }
}

// the clocks now, for the log
void Log() {
    if (g_rst < 0) return;
    uint32_t hz[3] = {};
    for (int i = 0; i < 3; i++)
        if (!g_rst || g_clk[i].open) Get(i, &hz[i]);
    LOGI("clock: now CPU %u, GPU %u, memory %u MHz (0: not opened)", hz[0] / 1000000u, hz[1] / 1000000u,
         hz[2] / 1000000u);
}

}  // namespace clocks
