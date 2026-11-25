// A native version of the films' colour conversion, sub_002CC910 (in the Bink library linked into the XBE).
//
// During a film this one function was 83% of the title thread's work on the PC build (host-pc/profiler.cpp,
// 2026-11-22: about 10 ms of each film frame): its MMX code is recompiled into per-lane loops. It converts two rows of
// 4:2:0 YUV to 32-bit pixels, four pixels a step (stdcall, one argument: the steps, at least one is made):
//   globals 0x451DA0 / 0x451DA4  the two rows' destination; left at each row's end (the next call goes on from there)
//           0x451DA8 / 0x451DAC  the two rows' Y (4 bytes a step), left at the end
//           0x451DB0 / 0x451DB4  U and V (2 bytes a step each), shared by both rows, left at the end
//   tables  0x452DC8 [U], 0x4535C8 [U] + 0x4531C8 [V], 0x4539C8 [V]: two 16-bit words a pixel pair, one per channel
//   words   0x34BD88 (Y's offset), 0x34BD90 (Y's scale), 0x34BD98 (the clamp's bias)
// Per pixel: y = pmulhw(psllw(psubusw(Y, offset), 2), scale); each channel c = psubusw(paddsw(table + y, bias),
// bias); the pixel is c1 | c2 << 8 | c3 << 16.
//
// This version does the same 16-bit arithmetic exactly, four lanes at a time in GCC's vector types (SSE on the PC,
// NEON on the Switch), and leaves everything as the original does: the six globals, the destination rows, the guest
// stack below the return address (its four pushes), the registers (eax = the second row's end, edx = its Y, ebx ebp
// esi edi kept, esp past the argument) and mm0..mm7 as its last step leaves them. Flags are not kept across a return
// by the recompiled code. test_shadow (common/options.h) runs both on every call, compares all of that and keeps the
// original's result; test_native false runs only the original.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_002CC910);

namespace {

typedef int32_t v4i __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));

constexpr uint32_t kDst0 = 0x451DA0, kDst1 = 0x451DA4, kY0 = 0x451DA8, kY1 = 0x451DAC, kU = 0x451DB0, kV = 0x451DB4;
constexpr uint32_t kTabA = 0x452DC8, kTabB = 0x4535C8, kTabBV = 0x4531C8, kTabC = 0x4539C8;
constexpr uint32_t kOffset = 0x34BD88, kScale = 0x34BD90, kBias = 0x34BD98;

inline uint32_t M32(const uint8_t* base, uint32_t va) {
    uint32_t v;
    std::memcpy(&v, base + va, 4);
    return v;
}
inline uint64_t M64(const uint8_t* base, uint32_t va) {
    uint64_t v;
    std::memcpy(&v, base + va, 8);
    return v;
}
inline void S32(uint8_t* base, uint32_t va, uint32_t v) { std::memcpy(base + va, &v, 4); }

// the four 16-bit lanes of an MMX value, each as 0..0xFFFF
inline v4i Lanes(uint64_t q) {
    return v4i{int32_t(q & 0xFFFF), int32_t((q >> 16) & 0xFFFF), int32_t((q >> 32) & 0xFFFF), int32_t(q >> 48)};
}
// two dwords of two 16-bit words each, as four lanes (pixel order)
inline v4i Pairs(uint32_t lo, uint32_t hi) {
    return v4i{int32_t(lo & 0xFFFF), int32_t(lo >> 16), int32_t(hi & 0xFFFF), int32_t(hi >> 16)};
}
inline v4i Sext16(v4i x) { return v4i(v4u(x) << 16) >> 16; }
inline v4i Max(v4i a, v4i b) {
    v4i m = a > b;
    return (a & m) | (b & ~m);
}
inline v4i Min(v4i a, v4i b) {
    v4i m = a < b;
    return (a & m) | (b & ~m);
}

struct Consts {
    v4i offset, scale, bias;
    uint64_t mm7;
};

// paddw (16-bit wrap), paddsw bias, psubusw bias
inline v4i Channel(v4i table, v4i y, const Consts& k) {
    const v4i lo = {-32768, -32768, -32768, -32768}, hi = {32767, 32767, 32767, 32767}, zero = {0, 0, 0, 0};
    v4i t = (table + y) & 0xFFFF;
    v4i s = Min(Max(Sext16(t) + Sext16(k.bias), lo), hi);
    return Max((s & 0xFFFF) - k.bias, zero);
}

struct Step {
    v4i c1, c2, c3;
    v4u out;
};

// one step: reads its inputs, returns the four pixels (nothing written)
inline Step Convert(const uint8_t* base, uint32_t y_va, uint32_t u_va, uint32_t v_va, const Consts& k) {
    const v4i zero = {0, 0, 0, 0};
    uint32_t ybytes = M32(base, y_va);
    v4i y = {int32_t(ybytes & 0xFF), int32_t((ybytes >> 8) & 0xFF), int32_t((ybytes >> 16) & 0xFF), int32_t(ybytes >> 24)};
    y = Max(y - k.offset, zero);                    // psubusw
    y = (y << 2) & 0xFFFF;                          // psllw 2
    y = ((Sext16(y) * Sext16(k.scale)) >> 16) & 0xFFFF;  // pmulhw
    uint32_t u0 = base[u_va], u1 = base[u_va + 1], v0 = base[v_va], v1 = base[v_va + 1];
    Step s;
    s.c1 = Channel(Pairs(M32(base, kTabA + u0 * 4), M32(base, kTabA + u1 * 4)), y, k);
    s.c2 = Channel(Pairs(M32(base, kTabB + u0 * 4), M32(base, kTabB + u1 * 4)) +
                       Pairs(M32(base, kTabBV + v0 * 4), M32(base, kTabBV + v1 * 4)),
                   y, k);
    s.c3 = Channel(Pairs(M32(base, kTabC + v0 * 4), M32(base, kTabC + v1 * 4)), y, k);
    s.out = v4u(s.c1) | (v4u(s.c2) << 8) | (v4u(s.c3) << 16);
    return s;
}

inline uint64_t Dwords(uint32_t lo, uint32_t hi) { return uint64_t(lo) | uint64_t(hi) << 32; }

// one row: from [dst_g] (setting it to the row's end) with Y from [y_g] (left at its end), U and V from [kU] and [kV];
// returns the U and V pointers at the end and the last step
uint32_t Row(X86Context& ctx, uint8_t* base, uint32_t dst_g, uint32_t y_g, uint32_t steps, const Consts& k,
             uint32_t& u_end, uint32_t& v_end, Step& last) {
    uint32_t edi = M32(base, dst_g), edx = M32(base, y_g), ebp = M32(base, kU), ebx = M32(base, kV);
    uint32_t end = (steps << 4) + edi;
    S32(base, dst_g, end);
    do {
        last = Convert(base, edx, ebp, ebx, k);
        edx += 4;
        ebp += 2;
        ebx += 2;
        uint32_t px[4] = {last.out[0], last.out[1], last.out[2], last.out[3]};
        std::memcpy(base + edi, px, 8);
        edi += 16;
        end = M32(base, dst_g);  // the original reads the end here, between its two stores
        std::memcpy(base + edi - 8, px + 2, 8);
    } while (edi < end);
    S32(base, y_g, edx);
    u_end = ebp;
    v_end = ebx;
    (void)ctx;
    return end;
}

void Native(X86Context& ctx, uint8_t* base) {
    const uint32_t esp0 = ctx.esp;
    const uint32_t steps = M32(base, esp0 + 4);
    // the original's four pushes (ebx ebp esi edi) stay on the guest stack below the return address
    S32(base, esp0 - 4, ctx.ebx);
    S32(base, esp0 - 8, ctx.ebp);
    S32(base, esp0 - 12, ctx.esi);
    S32(base, esp0 - 16, ctx.edi);
    Consts k;
    k.offset = Lanes(M64(base, kOffset));
    k.scale = Lanes(M64(base, kScale));
    k.mm7 = M64(base, kBias);
    k.bias = Lanes(k.mm7);
    uint32_t u_end = 0, v_end = 0;
    Step last;
    Row(ctx, base, kDst0, kY0, steps, k, u_end, v_end, last);
    // the second row: the same U and V again (4:2:0), then both left at their end
    uint32_t end1 = Row(ctx, base, kDst1, kY1, steps, k, u_end, v_end, last);
    S32(base, kU, u_end);
    S32(base, kV, v_end);
    ctx.eax = end1;
    ctx.edx = M32(base, kY1);
    ctx.esp = esp0 + 8;
    ctx.mm[0] = 0;
    ctx.mm[1] = Dwords(last.out[0], last.out[1]);
    ctx.mm[2] = Dwords(uint32_t(last.c2[0]) << 8, uint32_t(last.c2[1]) << 8);
    ctx.mm[3] = Dwords(uint32_t(last.c3[0]) << 16, uint32_t(last.c3[1]) << 16);
    ctx.mm[4] = Dwords(last.out[2], last.out[3]);
    ctx.mm[5] = Dwords(uint32_t(last.c2[2]) << 8, uint32_t(last.c2[3]) << 8);
    ctx.mm[6] = Dwords(uint32_t(last.c3[2]) << 16, uint32_t(last.c3[3]) << 16);
    ctx.mm[7] = k.mm7;
}

// test_shadow: both versions from the same state, everything either writes compared, the original's result kept
struct Snapshot {
    X86Context ctx;
    std::vector<uint8_t> row0, row1;
    uint8_t globals[24], stack[16];
};

uint64_t g_calls = 0, g_mismatches = 0;

void Take(const X86Context& ctx, const uint8_t* base, uint32_t d0, uint32_t d1, uint32_t bytes, Snapshot& s) {
    s.ctx = ctx;
    s.row0.assign(base + d0, base + d0 + bytes);
    s.row1.assign(base + d1, base + d1 + bytes);
    std::memcpy(s.globals, base + kDst0, 24);
    std::memcpy(s.stack, base + ctx.esp - 16, 16);
}

void Put(X86Context& ctx, uint8_t* base, uint32_t d0, uint32_t d1, const Snapshot& s) {
    ctx = s.ctx;
    std::memcpy(base + d0, s.row0.data(), s.row0.size());
    std::memcpy(base + d1, s.row1.data(), s.row1.size());
    std::memcpy(base + kDst0, s.globals, 24);
    std::memcpy(base + s.ctx.esp - 16, s.stack, 16);
}

void Shadow(X86Context& ctx, uint8_t* base) {
    const uint32_t steps = M32(base, ctx.esp + 4);
    const uint32_t bytes = (steps ? steps : 1) * 16;
    const uint32_t d0 = M32(base, kDst0), d1 = M32(base, kDst1);
    Snapshot before, native;
    Take(ctx, base, d0, d1, bytes, before);
    Native(ctx, base);
    native.ctx = ctx;
    native.row0.assign(base + d0, base + d0 + bytes);
    native.row1.assign(base + d1, base + d1 + bytes);
    std::memcpy(native.globals, base + kDst0, 24);
    std::memcpy(native.stack, base + before.ctx.esp - 16, 16);
    Put(ctx, base, d0, d1, before);
    __imp__sub_002CC910(ctx, base);
    g_calls++;
    const char* what = nullptr;
    if (std::memcmp(native.row0.data(), base + d0, bytes)) what = "row 0";
    else if (std::memcmp(native.row1.data(), base + d1, bytes)) what = "row 1";
    else if (std::memcmp(native.globals, base + kDst0, 24)) what = "globals";
    else if (std::memcmp(native.stack, base + before.ctx.esp - 16, 16)) what = "stack";
    else if (native.ctx.eax != ctx.eax || native.ctx.ecx != ctx.ecx || native.ctx.edx != ctx.edx ||
             native.ctx.ebx != ctx.ebx || native.ctx.esp != ctx.esp || native.ctx.ebp != ctx.ebp ||
             native.ctx.esi != ctx.esi || native.ctx.edi != ctx.edi)
        what = "registers";
    else if (std::memcmp(native.ctx.mm, ctx.mm, sizeof(ctx.mm))) what = "mm registers";
    if (what) {
        if (++g_mismatches <= 10) {
            LOGW("native: sub_002CC910 mismatch in %s (call %llu, %u steps)", what, (unsigned long long)g_calls, steps);
            if (!std::strcmp(what, "row 0") || !std::strcmp(what, "row 1")) {
                const uint8_t* mine = !std::strcmp(what, "row 0") ? native.row0.data() : native.row1.data();
                const uint8_t* theirs = base + (!std::strcmp(what, "row 0") ? d0 : d1);
                for (uint32_t i = 0; i < bytes; i += 4)
                    if (std::memcmp(mine + i, theirs + i, 4)) {
                        LOGW("native:   pixel %u: native %08X, original %08X", i / 4, M32(mine, i), M32(theirs, i));
                        break;
                    }
            }
            if (!std::strcmp(what, "mm registers"))
                for (int i = 0; i < 8; i++)
                    if (native.ctx.mm[i] != ctx.mm[i])
                        LOGW("native:   mm%d native %016llX, original %016llX", i, (unsigned long long)native.ctx.mm[i],
                             (unsigned long long)ctx.mm[i]);
        }
    }
    if ((g_calls & (g_calls - 1)) == 0 && g_calls >= 1024)
        LOGI("native: sub_002CC910 shadow: %llu calls, %llu mismatches", (unsigned long long)g_calls,
             (unsigned long long)g_mismatches);
}

}  // namespace

// the time spent in it, logged every 8192 calls (about 34 film frames of 640x480): the console's figure for the A/B
X86_FUNC(sub_002CC910) {
    static uint64_t calls = 0, ns = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto& o = game::Opt();
    if (o.test_shadow) Shadow(ctx, base);
    else if (!o.test_native) __imp__sub_002CC910(ctx, base);
    else Native(ctx, base);
    ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (++calls % 8192 == 0) {
        LOGI("native: sub_002CC910 (%s) 8192 calls in %.1f ms", o.test_shadow ? "shadow" : o.test_native ? "native" : "original",
             double(ns) / 1e6);
        ns = 0;
    }
}
