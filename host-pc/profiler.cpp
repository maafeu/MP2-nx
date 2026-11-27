// MPayne2-nx test aid (PC build only): a sampling profiler. MP2_PROF=START:SECONDS (for example 60:60) samples from START
// seconds after launch for SECONDS seconds. About every millisecond it stops each thread that used the CPU since its
// last sample, walks the thread's stack (the recompiled code's host frames follow the guest's calls), and lets it go.
// At the end it writes logs/prof.txt: per thread how often it was working, then the functions by self time (the
// innermost frame) and by inclusive time (anywhere on the stack) for the guest threads together and for the busiest
// thread, and the commonest call chains of the top functions.
// Not working: a thread that used no CPU since its last sample (not walked), or one stopped inside a wait or sleep
// call (its innermost frame is one of ntdll's wait calls). Neither counts in the percentages.
// Nothing is allocated while a thread is stopped (it may hold the heap's lock): the walk fills a fixed buffer.
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.h"

namespace {

constexpr int kMaxDepth = 256;
// a thread that used fewer cycles than this since its last sample (about 0.05 ms at 3 GHz) was not working
constexpr ULONG64 kRunningCycles = 150000;

struct StackHash {
    size_t operator()(const std::vector<uint64_t>& v) const {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t x : v) h = (h ^ x) * 1099511628211ull;
        return size_t(h);
    }
};

struct Target {
    DWORD tid = 0;
    HANDLE h = nullptr;
    std::string name;
    ULONG64 cycles = 0;
    uint64_t walked = 0, idle = 0, failed = 0;
    // innermost first, each frame by its function's start (or the address itself without unwind data)
    std::unordered_map<std::vector<uint64_t>, uint32_t, StackHash> stacks;
    bool seen = false;
};

// SEH only here (no C++ objects to unwind): a bad stack value ends the walk instead of the game
int Walk(CONTEXT* c, uint64_t* key) {
    int n = 0;
    __try {
        while (n < kMaxDepth && c->Rip) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c->Rip, &base, nullptr);
            key[n++] = f ? base + f->BeginAddress : c->Rip;
            if (!f) {
                c->Rip = *reinterpret_cast<DWORD64*>(c->Rsp);
                c->Rsp += 8;
            } else {
                void* handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &handler_data, &frame, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

std::string ThreadName(HANDLE h, DWORD tid) {
    PWSTR w = nullptr;
    std::string s;
    if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
        for (PWSTR p = w; *p; p++) s += char(*p < 128 ? *p : '?');
        LocalFree(w);
    }
    if (s.empty()) s = "tid " + std::to_string(tid);
    return s;
}

void Refresh(std::vector<Target>& targets, DWORD self_tid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (auto& t : targets) t.seen = false;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) continue;
        auto it = std::find_if(targets.begin(), targets.end(), [&](const Target& t) { return t.tid == te.th32ThreadID; });
        if (it != targets.end()) {
            it->seen = true;
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        Target t;
        t.tid = te.th32ThreadID;
        t.h = h;
        t.name = ThreadName(h, t.tid);
        QueryThreadCycleTime(h, &t.cycles);
        t.seen = true;
        targets.push_back(std::move(t));
    }
    CloseHandle(snap);
    // a thread's name is set after it starts: read it again while it is young
    for (auto& t : targets)
        if (t.seen && t.walked + t.idle < 2000) t.name = ThreadName(t.h, t.tid);
}

// function names, resolved once per function start
struct Names {
    std::vector<std::string> list;        // "module!name"
    std::vector<bool> wait;               // innermost frame of a stopped, waiting thread
    std::vector<bool> recompiled;         // the game's recompiled code (sub_XXXXXXXX)
    std::unordered_map<uint64_t, uint32_t> by_key;
    std::unordered_map<std::string, uint32_t> by_name;

    uint32_t Of(uint64_t a) {
        auto it = by_key.find(a);
        if (it != by_key.end()) return it->second;
        HANDLE p = GetCurrentProcess();
        std::string mod = "?", name;
        IMAGEHLP_MODULE64 m{};
        m.SizeOfStruct = sizeof(m);
        if (SymGetModuleInfo64(p, a, &m)) mod = m.ModuleName;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(p, a, &disp, s)) {
            name.assign(s->Name, s->NameLen);
        } else {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)a);
            name = hex;
        }
        // lld's names for a recompiled function that the port can replace: "_imp__sub_X" (the default body) and
        // "weak.sub_X.default.__imp__sub_Y" (folded with another): keep the sub_ part that names the guest address
        size_t at = name.find("weak.sub_");
        if (at != std::string::npos) name = name.substr(at + 5, 12) + " (folded)";
        else if (name.rfind("_imp__sub_", 0) == 0) name = name.substr(6);
        std::string full = mod + "!" + name;
        uint32_t id;
        auto nit = by_name.find(full);
        if (nit != by_name.end()) {
            id = nit->second;
        } else {
            id = uint32_t(list.size());
            list.push_back(full);
            bool w = mod == "ntdll" && (name.rfind("ZwWait", 0) == 0 || name.rfind("NtWait", 0) == 0 ||
                                        name.rfind("ZwDelay", 0) == 0 || name.rfind("NtDelay", 0) == 0 ||
                                        name.rfind("ZwRemoveIoCompletion", 0) == 0 || name.rfind("ZwYield", 0) == 0 ||
                                        name.rfind("NtYield", 0) == 0);
            wait.push_back(w);
            recompiled.push_back(name.rfind("sub_", 0) == 0);
            by_name[full] = id;
        }
        by_key[a] = id;
        return id;
    }
};

struct Tally {
    uint64_t working = 0, waiting = 0;
    std::vector<uint64_t> self, incl;
    // the commonest stacks by name ids (innermost first) for the report's call chains
    std::unordered_map<std::vector<uint64_t>, uint64_t, StackHash> chains;

    void Add(Names& names, const std::vector<uint64_t>& stack, uint32_t count) {
        std::vector<uint64_t> ids;
        ids.reserve(stack.size());
        for (uint64_t k : stack) ids.push_back(names.Of(k));
        if (self.size() < names.list.size()) {
            self.resize(names.list.size());
            incl.resize(names.list.size());
        }
        if (names.wait[ids[0]]) {
            waiting += count;
            return;
        }
        working += count;
        self[ids[0]] += count;
        for (size_t i = 0; i < ids.size(); i++) {
            bool dup = false;
            for (size_t j = 0; j < i && !dup; j++) dup = ids[j] == ids[i];
            if (!dup) incl[ids[i]] += count;
        }
        // a chain is the first 12 distinct frames: enough to see who calls the innermost function
        std::vector<uint64_t> chain;
        for (size_t i = 0; i < ids.size() && chain.size() < 12; i++)
            if (chain.empty() || chain.back() != ids[i]) chain.push_back(ids[i]);
        chains[chain] += count;
    }
};

void Table(FILE* f, const char* title, const Names& names, const Tally& t, bool by_self, size_t rows) {
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < t.self.size(); i++)
        if (t.incl[i]) order.push_back(i);
    const auto& v = by_self ? t.self : t.incl;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return v[a] > v[b]; });
    std::fprintf(f, "\n-- %s (of %llu working samples)\n   self%%  incl%%  function\n", title,
                 (unsigned long long)t.working);
    double total = double(t.working ? t.working : 1);
    for (size_t i = 0; i < order.size() && i < rows; i++)
        std::fprintf(f, "  %5.1f  %5.1f  %s\n", 100.0 * double(t.self[order[i]]) / total,
                     100.0 * double(t.incl[order[i]]) / total, names.list[order[i]].c_str());
}

void Chains(FILE* f, const Names& names, const Tally& t, size_t funcs, size_t per) {
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < t.self.size(); i++)
        if (t.self[i]) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return t.self[a] > t.self[b]; });
    std::fprintf(f, "\n-- call chains of the top functions by self time (innermost first)\n");
    double total = double(t.working ? t.working : 1);
    for (size_t i = 0; i < order.size() && i < funcs; i++) {
        uint32_t id = order[i];
        std::vector<std::pair<uint64_t, const std::vector<uint64_t>*>> mine;
        for (const auto& [c, n] : t.chains)
            if (c[0] == id) mine.push_back({n, &c});
        std::sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::fprintf(f, "\n%5.1f%% %s\n", 100.0 * double(t.self[id]) / total, names.list[id].c_str());
        for (size_t j = 0; j < mine.size() && j < per; j++) {
            std::fprintf(f, "  %5.1f%%", 100.0 * double(mine[j].first) / total);
            const auto& c = *mine[j].second;
            for (size_t k = 1; k < c.size(); k++) {
                const std::string& s = names.list[c[k]];
                size_t bang = s.find('!');
                std::fprintf(f, " < %s", bang == std::string::npos ? s.c_str() : s.c_str() + bang + 1);
            }
            std::fprintf(f, "\n");
        }
    }
}

void Run(std::filesystem::path dir, double start_s, double len_s) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(start_s));
    LOGI("prof: sampling for %.0f s", len_s);
    auto end = clock::now() + std::chrono::duration<double>(len_s);
    std::vector<Target> targets;
    DWORD self_tid = GetCurrentThreadId();
    static uint64_t key[kMaxDepth];
    uint64_t rounds = 0;
    auto next_refresh = clock::now();
    while (clock::now() < end) {
        if (clock::now() >= next_refresh) {
            Refresh(targets, self_tid);
            next_refresh = clock::now() + std::chrono::milliseconds(500);
        }
        for (auto& t : targets) {
            if (!t.seen) continue;
            ULONG64 cyc = 0;
            if (!QueryThreadCycleTime(t.h, &cyc)) continue;
            bool ran = cyc - t.cycles >= kRunningCycles;
            t.cycles = cyc;
            if (!ran) {
                t.idle++;
                continue;
            }
            if (SuspendThread(t.h) == DWORD(-1)) {
                t.failed++;
                continue;
            }
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            int n = GetThreadContext(t.h, &c) ? Walk(&c, key) : 0;
            ResumeThread(t.h);
            if (n == 0) {
                t.failed++;
                continue;
            }
            t.walked++;
            t.stacks[std::vector<uint64_t>(key, key + n)]++;
        }
        rounds++;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    double secs = std::chrono::duration<double>(clock::now() - t0).count() - start_s;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitializeW(GetCurrentProcess(), dir.wstring().c_str(), TRUE);
    std::filesystem::path out = dir / "logs" / "prof.txt";
    FILE* f = _wfopen(out.wstring().c_str(), L"w");
    if (!f) {
        LOGE("prof: %s could not be written", out.string().c_str());
        return;
    }
    Names names;
    Tally guest;
    std::vector<Tally> per(targets.size());
    for (size_t i = 0; i < targets.size(); i++) {
        bool is_guest = targets[i].name.rfind("guest", 0) == 0;
        for (const auto& [s, n] : targets[i].stacks) {
            per[i].Add(names, s, n);
            if (is_guest) guest.Add(names, s, n);
        }
    }
    for (auto& t : per) {
        t.self.resize(names.list.size());
        t.incl.resize(names.list.size());
    }
    guest.self.resize(names.list.size());
    guest.incl.resize(names.list.size());

    std::fprintf(f, "prof: %.1f s from %.0f s after launch, %llu rounds (one every %.2f ms)\n", secs, start_s,
                 (unsigned long long)rounds, rounds ? 1000.0 * secs / double(rounds) : 0.0);
    std::vector<size_t> order(targets.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return per[a].working > per[b].working; });
    std::fprintf(f, "\nthreads: working = the share of rounds it was found working (not waiting); top module by self time\n");
    for (size_t i : order) {
        const auto& t = targets[i];
        uint64_t all = t.walked + t.idle;
        if (!per[i].working) continue;
        std::map<std::string, uint64_t> mods;
        for (uint32_t id = 0; id < per[i].self.size(); id++)
            if (per[i].self[id]) mods[names.list[id].substr(0, names.list[id].find('!'))] += per[i].self[id];
        auto top = std::max_element(mods.begin(), mods.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        std::fprintf(f, "  %-12s working %5.1f%% (%llu samples), waiting %llu, idle %llu; %s %.0f%%\n", t.name.c_str(),
                     all ? 100.0 * double(per[i].working) / double(all) : 0.0, (unsigned long long)per[i].working,
                     (unsigned long long)per[i].waiting, (unsigned long long)t.idle,
                     top == mods.end() ? "-" : top->first.c_str(),
                     top == mods.end() ? 0.0 : 100.0 * double(top->second) / double(per[i].working));
    }
    std::map<std::string, uint64_t> mods;
    uint64_t recompiled = 0;
    for (uint32_t id = 0; id < guest.self.size(); id++) {
        mods[names.list[id].substr(0, names.list[id].find('!'))] += guest.self[id];
        if (names.recompiled[id]) recompiled += guest.self[id];
    }
    double gt = double(guest.working ? guest.working : 1);
    std::fprintf(f, "\nguest threads, self time by module:\n");
    for (const auto& [m, s] : mods)
        if (s) std::fprintf(f, "  %5.1f%%  %s\n", 100.0 * double(s) / gt, m.c_str());
    std::fprintf(f, "  (the game's recompiled code, sub_*: %.1f%%)\n", 100.0 * double(recompiled) / gt);
    Table(f, "guest threads by self time", names, guest, true, 80);
    Table(f, "guest threads by inclusive time", names, guest, false, 120);
    if (!order.empty() && per[order[0]].working) {
        std::string title = "busiest thread (" + targets[order[0]].name + ") by inclusive time";
        Table(f, title.c_str(), names, per[order[0]], false, 60);
    }
    Chains(f, names, guest, 30, 3);
    std::fclose(f);
    SymCleanup(GetCurrentProcess());
    for (auto& t : targets) CloseHandle(t.h);
    LOGI("prof: %llu working guest samples written to %s", (unsigned long long)guest.working, out.string().c_str());
}

}  // namespace

void StartProfiler(const std::filesystem::path& exe_dir) {
    const char* e = std::getenv("MP2_PROF");
    if (!e || !*e) return;
    double start = 60, len = 60;
    if (std::sscanf(e, "%lf:%lf", &start, &len) < 1) return;
    LOGI("prof: MP2_PROF=%s (start %.0f s, %.0f s long)", e, start, len);
    std::thread(Run, exe_dir, start, len).detach();
}
