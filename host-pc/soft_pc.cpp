// The PC build with the Switch soft build's renderer (MP2_SOFT=1 in the environment): the SDK's CPU reference
// renderer instead of Vulkan, to find the soft renderer's faults with the PC's crash handler and symbols. The frame
// is not shown; every 150th present is saved as shots/soft_NNNN.tga beside the exe.
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "reference_renderer.h"

namespace gpu {

namespace {

class SoftPcBackend : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        LOGI("soft (PC): CPU reference renderer, %ux%u, the window stays black", w, h);
        return true;
    }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        reference::Clear(s, count, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        reference::Draw(s, prim, start, count, indices);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t a, uint32_t b) override {
        reference::Copy(src, dst, count, a, b);
    }
    void Present(const Surface& front) override {
        if (presents_++ % 150 == 0) {
            char name[64];
            std::snprintf(name, sizeof(name), "soft_%04u.tga", unsigned(presents_ / 150));
            reference::WriteTga(front, (app::ExeDir() / "shots" / name).string().c_str());
        }
        reference::BeginFrame();
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    uint64_t presents_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateSoftBackend() { return std::make_unique<SoftPcBackend>(); }

}  // namespace gpu
