// Which level the game loads (always logged), and test_level (a test aid, common/options.h): a test run reaches any of
// the 30 levels without playing up to it. test_level is off (0) unless the settings file sets it, and it is never
// written to that file.
//
// The level init, sub_00023B05 (a method of the game mode, thiscall: the level's name as a C string, then two flags),
// is called from 000252DF and from 000255F2 inside the load-from-stream at 000254AE, which New Game uses: the stream
// gives the name and then the game's state for that level (MP_GM_LOAD1..6). Passing another level's name there
// loads that level and then crashes on the rest of 03_First_Hospital's state (2026-11-22, levels 2, 13 and 14:
// a memmove of 0x3F3504F0 bytes from the stream's next string). So the level is changed the way the game's own
// developer scripts do it: sub_0001B917 runs a script line on the game mode (its cheat routine 0001B952 calls it
// with "player->C_SetInfiniteAmmo(...)" lines), and sub_0001EFE2, the game mode's update once a frame (it counts its
// timer at +0xF84 down by the frame time and itself runs a script at 0001F049), is where the line is run: once,
// 90 updates after New Game's level started, "maxpayne_gamemode-><test_level_command>(<level NN's name>);" (the
// command GM_InitSingleLevel unless the settings file names another; GM_Init takes a name too).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

#ifdef __SWITCH__
namespace clocks {
void TestGpu(uint32_t mhz);  // host-switch/clocks.cpp: test_gpu_clocks' step (0: the system's own rate)
}
#endif

namespace {

const char* const kLevels[] = {
    nullptr, "03_First_Hospital", "04_Warehouse", "05_First_Vodka", "06_First_Dream", "07_Maxs_Apartment_split1",
    "07_Maxs_Apartment_split2", "07_Maxs_Apartment_B", "08_First_Address_Unknown", "09_Upper_East_Side_Split1",
    "09_Upper_East_Side_Split2", "10_Police_Station", "11_Second_Address_Unknown", "12_Condemned_Building_SP1",
    "12_Condemned_Building_SP2", "12_Condemned_Building_B", "13_Construction_Site", "13_Construction_Site_B",
    "13_Construction_Site_C", "14_Second_Dream", "15_Second_Hospital", "16_Second_Vodka", "17_Crackhouse_split1",
    "17_Crackhouse_Split2", "18_Gognittis_Lot", "19_Third_Address_Unknown", "20_Third_Dream", "21_The_Manor_Split1",
    "21_The_Manor_Split2", "21_The_Manor_B_split1", "21_The_Manor_B_split2"};

constexpr uint32_t kRunScript = 0x0001B917;  // the game mode's script line runner (thiscall, the line)
constexpr uint32_t kDelayUpdates = 90;

bool g_level_started = false;
uint32_t g_updates = 0;
bool g_sent = false;
std::chrono::steady_clock::time_point g_started;  // when New Game's level started (test_level_after)

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00023B05);
X86_FUNC(sub_00023B05) {
    const uint32_t name = X86_MEM32(ctx.esp + 4);
    char shown[64] = "?";
    if (name) std::snprintf(shown, sizeof(shown), "%s", reinterpret_cast<const char*>(base + name));
    LOGI("game: level init %s", shown);
    if (!g_level_started) g_started = std::chrono::steady_clock::now();
    g_level_started = true;
    __imp__sub_00023B05(ctx, base);
}

namespace {

// the levels to change to, in order: test_levels ("2,8,11"), or test_level alone
std::vector<uint32_t> Levels() {
    std::vector<uint32_t> out;
    const std::string& list = game::Opt().test_levels;
    for (size_t i = 0; i < list.size();) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        uint32_t n = uint32_t(std::strtoul(list.substr(i, j - i).c_str(), nullptr, 10));
        if (n >= 2 && n <= 30) out.push_back(n);
        i = j + 1;
    }
    if (out.empty() && game::Opt().test_level >= 2 && game::Opt().test_level <= 30) out.push_back(game::Opt().test_level);
    return out;
}

// test_gpu_clocks ("0,384,461"): a GPU clock in MHz for each change in turn; an empty list or a missing entry leaves
// the clock as it is
std::vector<uint32_t> GpuClocks() {
    std::vector<uint32_t> out;
    const std::string& list = game::Opt().test_gpu_clocks;
    for (size_t i = 0; i < list.size();) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        out.push_back(uint32_t(std::strtoul(list.substr(i, j - i).c_str(), nullptr, 10)));
        i = j + 1;
    }
    return out;
}

size_t g_next = 0;                                // the next of Levels() to change to
std::chrono::steady_clock::time_point g_last;     // when the last change was sent

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0001EFE2);
X86_FUNC(sub_0001EFE2) {
    const uint32_t self = ctx.ecx;
    __imp__sub_0001EFE2(ctx, base);
    static const std::vector<uint32_t> levels = Levels();
    const std::string& script = game::Opt().test_script;
    if (g_sent || (levels.empty() && script.empty()) || !g_level_started || ++g_updates < kDelayUpdates) return;
    const auto now = std::chrono::steady_clock::now();
    // test_level_after: chapter 1 for that many seconds first (a run that measures both); test_levels: each next
    // level test_level_every seconds after the last change
    if (g_next == 0 && game::Opt().test_level_after &&
        now - g_started < std::chrono::seconds(game::Opt().test_level_after))
        return;
    if (g_next > 0 && now - g_last < std::chrono::seconds(game::Opt().test_level_every)) return;
    const uint32_t n = levels.empty() ? 0 : levels[g_next];
#ifdef __SWITCH__
    static const std::vector<uint32_t> gpu = GpuClocks();
    if (g_next < gpu.size()) {
        LOGI("test: GPU clock step %u MHz for change %zu (test_gpu_clocks)", gpu[g_next], g_next + 1);
        clocks::TestGpu(gpu[g_next]);
    }
#endif
    g_last = now;
    if (!script.empty() || ++g_next >= levels.size()) g_sent = true;
    char line[200];
    // test_script (a raw script line, e.g. "maxpayne_gamemode->GM_SendEndOfGameMessages();") wins over test_level
    if (!script.empty())
        std::snprintf(line, sizeof(line), "%s", script.c_str());
    else
        std::snprintf(line, sizeof(line), "maxpayne_gamemode->%s(%s);", game::Opt().test_level_command.c_str(), kLevels[n]);
    const uint32_t keep_esp = ctx.esp, keep_ecx = ctx.ecx, keep_eax = ctx.eax;
    ctx.esp = (ctx.esp - 256) & ~15u;  // the line on the guest stack, below what the game uses
    std::memcpy(base + ctx.esp, line, std::strlen(line) + 1);
    const uint32_t str = ctx.esp;
    ctx.ecx = self;
    LOGI("test: running %s (test_level %u%s)", line, n, script.empty() ? "" : ", test_script");
    kernel::CallGuest(ctx, kRunScript, {str});
    ctx.esp = keep_esp;
    ctx.ecx = keep_ecx;
    ctx.eax = keep_eax;
}
