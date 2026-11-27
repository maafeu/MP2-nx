// MPayne2-nx: a sampling profiler for the console, ported from the COLD-FEAR-NX-PORT base's (src/switch_nx.c,
// RECOMP_NX_PROFILE). The console has no profiler and the emulator's and the PC's costs are not the console's. A thread
// above the game's wakes every millisecond, pauses each busy guest thread (svcSetThreadActivity), reads its pc, lr and
// up to five return addresses up the x29 chain (svcGetThreadContext3) and lets it run again. "Busy" is decided every
// 100 ms from the threads' tick counts: over 10% of a core. Samples go every 10 s to logs/prof_NNN.bin beside the log
// of the same number (a new file each run: nothing on the card is removed), 32-byte records (thread slot, time, pc,
// lr, 5 return addresses) as offsets into the NRO; tools/prof_report.py turns the file into per-function tables with
// the run's ELF. Nothing is resolved on the console. Costs a few percent of one core while on.
//
// On with `profile = 1` in MPayne2-nx.toml (a test aid, never written back); `profile_after = <s>` starts it later.
#include <switch.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"

extern "C" void _start(void);  // the NRO's first instruction: ELF address 0

namespace prof {
namespace {

struct Sample {
    uint16_t slot, when;  // when: 10 ms units since the profiler started
    uint32_t pc, lr, ret[5];
};
static_assert(sizeof(Sample) == 32, "32-byte records");

constexpr unsigned kMax = 120000;
constexpr int kFrames = 5;
Sample g_samples[kMax];
unsigned g_n = 0;
std::string g_path;
uint64_t g_after = 0, g_t0 = 0;

uint32_t Off(uint64_t a) {
    const uint64_t b = reinterpret_cast<uintptr_t>(&_start);
    return a >= b && a - b < 0xFFFFFFFFull ? uint32_t(a - b) : 0xFFFFFFFFu;
}

void Flush() {
    if (!g_n) return;
    if (FILE* f = std::fopen(g_path.c_str(), "ab")) {
        std::fwrite(g_samples, sizeof(Sample), g_n, f);
        std::fclose(f);
    }
    LOGI("prof: %u samples written to %s", g_n, g_path.c_str());
    g_n = 0;
}

void Thread_(void*) {
    constexpr int kMaxT = 64;
    std::vector<uint32_t> hs;
    uint64_t last[kMaxT] = {};
    bool busy[kMaxT] = {};
    const uint64_t freq = armGetSystemTickFreq();
    uint64_t t_list = 0, t_flush = armGetSystemTick();
    for (;;) {
        svcSleepThread(1000000ull);
        const uint64_t now = armGetSystemTick();
        if (now - g_t0 < g_after) {
            t_flush = now;
            continue;
        }
        if (now - t_list > freq / 10) {  // who is busy
            const uint64_t span = now - t_list;
            hs = kernel::HostThreadHandles();
            if (hs.size() > size_t(kMaxT)) hs.resize(kMaxT);
            for (size_t i = 0; i < hs.size(); i++) {
                u64 t = 0;
                if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, hs[i], UINT64_MAX))) t = 0;
                busy[i] = t_list && (t - last[i]) * 10 > span;  // thread ticks count at 19.2 MHz, like the system tick
                last[i] = t;
            }
            t_list = now;
        }
        for (size_t i = 0; i < hs.size() && g_n < kMax; i++) {
            if (!busy[i] || R_FAILED(svcSetThreadActivity(hs[i], ThreadActivity_Paused))) continue;
            ThreadContext ctx;
            if (R_SUCCEEDED(svcGetThreadContext3(&ctx, hs[i]))) {
                Sample& o = g_samples[g_n++];
                o.slot = uint16_t(i);
                o.when = uint16_t((armGetSystemTick() - g_t0) / (freq / 100));
                o.pc = Off(ctx.pc.x);
                o.lr = Off(ctx.lr);
                std::memset(o.ret, 0xFF, sizeof(o.ret));
                // the x29 chain within the stack's own mapping: the thread is paused and its stack is our memory
                MemoryInfo mi;
                u32 pi;
                if ((ctx.fp & 7) == 0 && R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.fp)) && (mi.perm & Perm_R)) {
                    uint64_t fp = ctx.fp, lo = mi.addr, hi = mi.addr + mi.size;
                    for (int k = 0; k < kFrames && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
                        const uint64_t* fr = reinterpret_cast<const uint64_t*>(uintptr_t(fp));
                        o.ret[k] = Off(fr[1]);
                        if (fr[0] <= fp) break;
                        fp = fr[0];
                    }
                }
            }
            svcSetThreadActivity(hs[i], ThreadActivity_Runnable);
        }
        if (now - t_flush > freq * 10) {
            std::string line = "prof: slots";
            for (size_t i = 0; i < hs.size(); i++) {
                char b[32];
                std::snprintf(b, sizeof(b), " %zu=%X", i, unsigned(hs[i]));
                line += b;
            }
            LOGI("%s", line.c_str());
            Flush();
            t_flush = armGetSystemTick();
        }
    }
}

}  // namespace

// at boot, after the settings: on with profile = 1 (host-switch/main.cpp)
void Start(uint32_t after_seconds) {
    static ::Thread t;
    g_t0 = armGetSystemTick();
    g_after = uint64_t(after_seconds) * armGetSystemTickFreq();
    // beside the run's log: logs/mp2_NNN.log -> logs/prof_NNN.bin
    g_path = logx::Path();
    size_t us = g_path.rfind('_'), dot = g_path.rfind('.');
    size_t slash = g_path.find_last_of('/');
    if (us != std::string::npos && dot != std::string::npos && dot > us && slash != std::string::npos)
        g_path = g_path.substr(0, slash + 1) + "prof" + g_path.substr(us, dot - us) + ".bin";
    else
        g_path += ".prof.bin";
    // 0x1E: above the guest threads and the clock keeper, so it samples on time
    if (R_FAILED(threadCreate(&t, Thread_, nullptr, nullptr, 0x4000, 0x1E, -2)) || R_FAILED(threadStart(&t))) {
        LOGW("prof: could not start the profiler thread");
        return;
    }
    LOGI("prof: sampling busy guest threads at 1 kHz into %s (NRO base %p), from %u s", g_path.c_str(),
         reinterpret_cast<void*>(&_start), after_seconds);
}

}  // namespace prof
