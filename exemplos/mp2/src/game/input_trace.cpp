// Diagnostic (MP2_INPUT_TRACE=1 in the environment, PC build): what the game's controller object asks of the pad.
// 0x1A2C0D(id, buffer), thiscall, returns a float in st0: the value of control id (0 D-pad up, 1 down, 2 left,
// 3 right, ...) from the current (+0x44) or the previous (+0x48) XINPUT_STATE copy; the object's +4 set means "no
// controller" and every id reads 0. Logs each id the first time it is asked for, and every non-zero answer (capped).
#include <atomic>
#include <cstdlib>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
const bool g_on = std::getenv("MP2_INPUT_TRACE") != nullptr;
std::atomic<uint32_t> g_seen_lo{0}, g_seen_hi{0};
std::atomic<int> g_lines{0};
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001A2C0D);
X86_FUNC(sub_001A2C0D) {
    if (!g_on) {
        __imp__sub_001A2C0D(ctx, base);
        return;
    }
    uint32_t self = ctx.ecx, id = R32(ctx.esp + 4), buf = R32(ctx.esp + 8), ret = R32(ctx.esp);
    uint32_t disabled = R32(self + 4), pad = R32(self + 0x40);
    __imp__sub_001A2C0D(ctx, base);
    double v = X87_ST(0);
    uint32_t bit = 1u << (id & 31);
    std::atomic<uint32_t>& seen = id < 32 ? g_seen_lo : g_seen_hi;
    bool first = !(seen.fetch_or(bit) & bit);
    if ((first || v != 0) && g_lines.fetch_add(1) < 400)
        LOGI("input: id %u buffer %u -> %g (object %08X disabled %u pad %d) from %08X%s", id, buf, v, self, disabled,
             int32_t(pad), ret, first ? " [first]" : "");
}
