// convert_exact.cpp: ConvertRun (the converted vertex path's per-element loops, with its division tables) against
// Convert (the per-vertex switch it replaced, kept in vertex.cpp as the reference), bit for bit, on every D3DVSDT type
// byte, random vertex bytes plus extremes (-32768, 32767, the packed minimums, NaN, infinities), both the sequential
// and the index path. Both functions are vertex.cpp's own lines, which convert_exact.sh extracts to conv_extract.inc,
// so this tests the shipped source. Exit status 0 when every run matches.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace gpu {
namespace {
#include "conv_extract.inc"
}  // namespace
}  // namespace gpu

int main() {
    std::mt19937 rng(12345);
    const uint32_t kStride = 20, kCount = 4096;
    std::vector<uint8_t> src(kStride * (kCount + 8));
    std::vector<uint32_t> ids(kCount);
    std::vector<float> a(kCount * 4), b(kCount * 4);
    uint64_t checked = 0, bad = 0;
    for (int round = 0; round < 8; round++) {
        for (auto& c : src) c = uint8_t(rng());
        for (uint32_t v = 0; v < 64; v++) {
            uint8_t* p = &src[v * kStride];
            const uint32_t pat[] = {0x80008000u, 0x7FFF7FFFu, 0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x80000000u,
                                    0x00000400u, 0x00200400u, 0x80100400u, 0xFFFFFFFFu, 0u, 0x00000001u};
            for (uint32_t k = 0; k < 5; k++) std::memcpy(p + k * 4, &pat[(v + k) % 12], 4);
        }
        for (auto& i : ids) i = rng() % (kCount + 8);
        for (uint32_t type = 0; type < 256; type++) {
            for (int path = 0; path < 2; path++) {
                const uint32_t first = path ? 0 : 3;
                const uint32_t* idp = path ? ids.data() : nullptr;
                std::fill(a.begin(), a.end(), -7.0f);
                std::fill(b.begin(), b.end(), -7.0f);
                gpu::ConvertRun(type, src.data(), kStride, first, idp, kCount - 8, a.data(), 4);
                for (uint32_t v = 0; v < kCount - 8; v++) {
                    const uint32_t vert = idp ? idp[v] : first + v;
                    gpu::Convert(type, src.data() + size_t(vert) * kStride, b.data() + v * 4);
                }
                checked++;
                if (std::memcmp(a.data(), b.data(), a.size() * 4) != 0 && bad++ < 10)
                    std::printf("MISMATCH type 0x%02X path %d\n", type, path);
            }
        }
    }
    // every 11 and 10 bit field of NORMPACKED3 and every byte, not left to the random draw
    for (uint32_t v = 0; v < 2048; v++) {
        const uint32_t word = v | (v << 11) | ((v & 1023) << 22);
        float x[4], y[4];
        gpu::ConvertRun(0x16, reinterpret_cast<const uint8_t*>(&word), 4, 0, nullptr, 1, x, 4);
        gpu::Convert(0x16, reinterpret_cast<const uint8_t*>(&word), y);
        const uint32_t bytes = v * 0x01010101u;
        float bx[4], by[4];
        gpu::ConvertRun(0x44, reinterpret_cast<const uint8_t*>(&bytes), 4, 0, nullptr, 1, bx, 4);
        gpu::Convert(0x44, reinterpret_cast<const uint8_t*>(&bytes), by);
        checked++;
        if ((std::memcmp(x, y, 16) || std::memcmp(bx, by, 16)) && bad++ < 10) std::printf("MISMATCH field %u\n", v);
    }
    std::printf("%llu runs, %llu mismatching\n", (unsigned long long)checked, (unsigned long long)bad);
    return bad ? 1 : 0;
}
