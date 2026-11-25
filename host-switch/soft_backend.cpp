// The Switch build's software renderer (PORT_RENDERER=soft, host-switch/CMakeLists.txt): the SDK's CPU reference
// renderer (harness/reference_renderer.cpp) draws the title's calls into its surfaces in guest memory, as the
// console's GPU would, and each present scales the front buffer into libnx's framebuffer (1280x720, the game's aspect:
// 16:9 when the game draws widescreen, as Max Payne 2 does when told the TV is). No GPU and no Vulkan: for bringing a port up on the Switch and in an emulator when
// no NVK build is at hand (MP2-nx docs/MP2-notes.md, "the Switch renderer"). Slow; the real renderer is a GPU one.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "app/app.h"
#include "common/aspect.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "host.h"
#include "reference_renderer.h"

namespace gpu {

namespace {

constexpr uint32_t kScreenW = 1280, kScreenH = 720;

class SoftBackend : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        w_ = width;
        h_ = height;
        Result rc = framebufferCreate(&fb_, nwindowGetDefault(), kScreenW, kScreenH, PIXEL_FORMAT_RGBA_8888, 2);
        if (R_FAILED(rc)) {
            LOGE("soft: framebufferCreate failed %08X", rc);
            return false;
        }
        framebufferMakeLinear(&fb_);
        made_ = true;
        LOGI("soft: CPU renderer, %ux%u guest, %ux%u framebuffer", width, height, kScreenW, kScreenH);
        for (int i = 0; i < 256; i++) ramp_[0][i] = ramp_[1][i] = ramp_[2][i] = uint8_t(i);
        return true;
    }
    ~SoftBackend() override {
        if (made_) framebufferClose(&fb_);
    }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        reference::Clear(s, count, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        reference::Draw(s, prim, start, count, indices);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
              uint32_t dst_points_va) override {
        reference::Copy(src, dst, count, src_rects_va, dst_points_va);
    }
    void Present(const Surface& front) override {
        uint32_t sw = 0, sh = 0;
        bool ok = reference::ReadPixels(front, px_, sw, sh) && sw && sh;
        u32 stride = 0;
        uint32_t* out = static_cast<uint32_t*>(framebufferBegin(&fb_, &stride));
        uint32_t pitch = stride / 4;
        // the guest frame at the game's aspect over black, as vulkan.cpp's present: the output's shape (16:9 here),
        // which the kit reports to the game as a widescreen TV; Max Payne 2 then draws anamorphic 16:9 frames
        float aspect = game::Aspect();
        uint32_t dh = kScreenH, dw = std::min<uint32_t>(kScreenW, uint32_t(kScreenH * aspect + 0.5f));
        uint32_t x0 = (kScreenW - dw) / 2;
        for (uint32_t y = 0; y < kScreenH; y++) {
            uint32_t* row = out + size_t(y) * pitch;
            std::memset(row, 0, kScreenW * 4);
            if (!ok) continue;
            const uint8_t* src = &px_[size_t(y * sh / dh) * sw * 4];
            for (uint32_t x = 0; x < dw; x++) {
                const uint8_t* c = src + size_t(x * sw / dw) * 4;  // BGRA
                row[x0 + x] = 0xFF000000u | uint32_t(ramp_[2][c[0]]) << 16 | uint32_t(ramp_[1][c[1]]) << 8 | ramp_[0][c[2]];
            }
        }
        framebufferEnd(&fb_);
        if (ok) Capture(sw, sh);
        reference::BeginFrame();
        if (++presents_ % 300 == 1) LOGI("soft: present %llu (%ux%u)", (unsigned long long)presents_, sw, sh);
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t* ramp) override {
        if (!ramp) return;
        for (int c = 0; c < 3; c++) std::memcpy(ramp_[c], ramp + c * 256, 256);
    }

private:
    // emulator and console test runs: a "capture" file beside the NRO saves the presented frame every 15 s as
    // shots/NNNN.bmp (24 bit, the guest's size)
    void Capture(uint32_t w, uint32_t h) {
        using clock = std::chrono::steady_clock;
        static const bool on = [] {
            std::error_code ec;
            bool b = std::filesystem::exists(app::ExeDir() / "capture", ec);
            if (b) std::filesystem::create_directories(app::ExeDir() / "shots", ec);
            return b;
        }();
        if (!on) return;
        auto now = clock::now();
        if (shots_ && now - last_shot_ < std::chrono::seconds(15)) return;
        last_shot_ = now;
        char name[32];
        std::snprintf(name, sizeof(name), "%04u.bmp", shots_++);
        FILE* f = std::fopen((app::ExeDir() / "shots" / name).string().c_str(), "wb");
        if (!f) return;
        uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
        uint8_t hd[54] = {'B', 'M'};
        auto put32 = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, uint32_t(-int32_t(h)));  // top row first
        hd[26] = 1;
        hd[28] = 24;
        std::fwrite(hd, 1, 54, f);
        std::vector<uint8_t> line(row, 0);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) std::memcpy(&line[x * 3], &px_[(size_t(y) * w + x) * 4], 3);
            std::fwrite(line.data(), 1, row, f);
        }
        std::fclose(f);
        LOGI("soft: shot %s", name);
    }

    std::chrono::steady_clock::time_point last_shot_{};
    uint32_t shots_ = 0;
    Framebuffer fb_{};
    bool made_ = false;
    uint32_t w_ = 640, h_ = 480;
    uint64_t presents_ = 0;
    std::vector<uint8_t> px_;
    uint8_t ramp_[3][256];
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<SoftBackend>(); }

// the software renderer finishes each draw before returning
void WaitGpuFrame(uint64_t) {}

}  // namespace gpu
