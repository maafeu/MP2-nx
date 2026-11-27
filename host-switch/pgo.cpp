// MaxPayne2-nx: profile-guided optimisation, the training run (PORT_PGO=generate, host-switch/CMakeLists.txt). The game's
// recompiled code and the host are built with -fprofile-generate -fprofile-info-section=gcov_info: GCC's counters, and
// a pointer to each object's gcov_info in the section gcov_info instead of a destructor that would write
// <the object's absolute PC path>.gcda (a C:\... name the console's FAT card cannot hold). Here the port writes them
// itself, through libgcov's __gcov_info_to_gcda, into its own folder: pgo/NNNN.gcda and pgo/names.txt (NNNN and the
// name GCC gave the file). tools/pgo_collect.py puts each one back at that name on the PC, where PORT_PGO=use builds
// read them. Nothing else on the card is touched, and nothing is removed.
//
// When: pgo.txt beside the NRO, "<reset> <dump>" in seconds from the start (the counters zeroed at <reset> when it is
// not 0, written at <dump>); without the file, written at 600 s. A normal build (no PORT_PGO=generate) has none of it.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

#ifdef MP2_PGO_GENERATE
// GCC's gcov.h declares libgcov's C functions without extern "C"
extern "C" {
#include <gcov.h>
}

extern "C" const struct gcov_info* const __start_gcov_info[];
extern "C" const struct gcov_info* const __stop_gcov_info[];

namespace {

struct Dump {
    std::string name;
    std::vector<unsigned char> bytes;
};

void OnFilename(const char* name, void* arg) { static_cast<Dump*>(arg)->name = name ? name : ""; }

void OnData(const void* p, unsigned n, void* arg) {
    auto* d = static_cast<Dump*>(arg);
    const auto* b = static_cast<const unsigned char*>(p);
    d->bytes.insert(d->bytes.end(), b, b + n);
}

void* OnAllocate(unsigned n, void*) { return std::malloc(n); }

void WriteAll() {
    // once: at <dump> or at the exit, whichever comes first
    static std::atomic<bool> written{false};
    if (written.exchange(true)) return;
    std::error_code ec;
    std::filesystem::path dir = app::ExeDir() / "pgo";
    std::filesystem::create_directories(dir, ec);
    std::string names;
    unsigned n = 0, bytes = 0;
    const struct gcov_info* const* info = __start_gcov_info;
    const struct gcov_info* const* end = __stop_gcov_info;
    __asm__("" : "+r"(info));  // the section's bounds come from the linker: no assumption about them
    for (; info != end; ++info) {
        Dump d;
        __gcov_info_to_gcda(*info, OnFilename, OnData, OnAllocate, &d);
        if (d.bytes.empty()) continue;
        char file[16];
        std::snprintf(file, sizeof(file), "%04u.gcda", n);
        if (FILE* f = std::fopen((dir / file).string().c_str(), "wb")) {
            std::fwrite(d.bytes.data(), 1, d.bytes.size(), f);
            std::fclose(f);
        }
        names += std::string(file, 4) + " " + d.name + "\n";
        bytes += unsigned(d.bytes.size());
        n++;
    }
    if (FILE* f = std::fopen((dir / "names.txt").string().c_str(), "wb")) {
        std::fwrite(names.data(), 1, names.size(), f);
        std::fclose(f);
    }
    LOGW("pgo: %u profiles written to pgo/ (%u KB)", n, bytes / 1024);
}

}  // namespace

namespace pgo {

void Start() {
    unsigned reset = 0, dump = 600;
    if (FILE* f = std::fopen((app::ExeDir() / "pgo.txt").string().c_str(), "rb")) {
        if (std::fscanf(f, "%u %u", &reset, &dump) < 2) dump = 600;
        std::fclose(f);
    }
    LOGW("pgo: an instrumented build: counters %s, written at %u s or at the exit", reset ? "zeroed later" : "from the start",
         dump);
    kernel::OnExit(WriteAll);
    std::thread([reset, dump] {
        auto t0 = std::chrono::steady_clock::now();
        if (reset) {
            std::this_thread::sleep_until(t0 + std::chrono::seconds(reset));
            __gcov_reset();
            LOGW("pgo: counters zeroed at %u s", reset);
        }
        std::this_thread::sleep_until(t0 + std::chrono::seconds(dump));
        WriteAll();
    }).detach();
}

}  // namespace pgo

#else

namespace pgo {
void Start() {}
}  // namespace pgo

#endif
