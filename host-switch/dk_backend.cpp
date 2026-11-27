// MPayne2-nx: the title's Direct3D 8 calls drawn with deko3d, the Switch GPU's own API (PORT_RENDERER=dk). No Mesa between
// the renderer and the GPU: a draw is a few words written into a command buffer, where the OpenGL renderer
// (gl_backend.cpp) spent 3 ms of a level 27 frame in Mesa's driver on the console's CPU (run 12, 2026-11-22).
//
// Draw for draw what gl_backend.cpp does (and through it vulkan.cpp): the same HLSL (gpu/shaders.cpp) through glslang
// to SPIR-V (vk_shader.cpp) and SPIRV-Cross to GLSL (gl_shader.cpp), the same targets (render scale, CPU writes
// latched, read-backs at the console's size), the same state mapping. The GLSL is compiled to deko3d's DKSH on the
// console by uam (third_party/uam, from the ultimate base: devkitPro's uam as a library), on a thread of its own with
// the 8 MB of stack Mesa's GLSL front end needs, and kept in shadercache/ beside the SPIR-V. Clip space and the
// viewport follow vulkan.cpp (deko3d's defaults are Vulkan's: depth 0 to 1, origin upper left; a negative viewport
// height turns clip space the D3D way up). The console's draw helpers between the markers are gl_backend.cpp's own.
//
// What deko3d leaves to the renderer, done here: memory (rings for vertices, indices, constants and uploads, an image
// heap, shader code), the GPU's caches and ordering (barriers where a target drawn is read, or a copy is involved),
// and the swapchain. Draws run on the title's thread (XBE_RENDER_THREAD=0), as the GL renderer's do.
#include <switch.h>
#include <deko3d.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dxgiformat.h>
#include <imgui.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/log.h"
#include "d3d/gpu_frame.h"
#include "gl_shader.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "uam_api.h"
#include "vk_shader.h"

namespace gpu {

namespace {

// ---- sizes

constexpr uint32_t kStreamSize = 64u << 20;   // vertices, indices and texture uploads: a ring of 4 fenced segments
constexpr uint32_t kUniformSize = 16u << 20;  // b0 and b1 per draw: a ring of 4 fenced segments
constexpr int kRingSegments = 4;
constexpr uint32_t kCodeSize = 32u << 20;     // shader code, never freed (a few hundred shaders a game)
constexpr uint32_t kCmdSize = 4u << 20;       // a frame's commands (a level 27 frame is about 160 draws)
constexpr int kFrameSlots = 3;                // frames in flight: their command memory, fence and deferred frees
constexpr uint32_t kMaxImages = 8192;         // image descriptors
constexpr uint32_t kMaxSamplers = 1024;       // sampler descriptors
constexpr uint32_t kHeapChunk = 64u << 20;    // the image heap grows by this
constexpr uint32_t kSwapImages = 3;
constexpr uint32_t kVsConstants = 192 * 16;   // b0: the vertex shader's c registers
constexpr uint32_t kDkshCacheVersion = 3;     // shadercache/<hash>_dk<version>.bin: bumped when the GLSL or uam changes

// the present's and the copies' full screen pass, as vulkan.cpp has it
const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// the kit's options panel and frame counter (ImGui): position, uv, colour per vertex, the font atlas at t0
const char* kUiHlsl = R"(
cbuffer B : register(b1) { float4 xform; };
Texture2D img : register(t0);
SamplerState ls : register(s0);
struct VIn {
    float2 v0 : ATTR0;
    float2 v1 : ATTR1;
    float4 v2 : ATTR2;
};
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };
O vs(VIn i) {
    O o;
    o.p = float4(i.v0 * xform.xy + xform.zw, 0, 1);
    o.uv = i.v1;
    o.c = i.v2;
    return o;
}
float4 psmain(O i) : SV_Target { return img.Sample(ls, i.uv) * i.c; }
)";

// ---- what the GL renderer's helpers below take, in deko3d's terms

// a range of the image heap
struct Alloc {
    int chunk = -1;
    uint32_t offset = 0, size = 0;
};

// an image and what the renderer knows about it: its descriptor (for sampling) and when it was last written and read,
// for the barriers (stamps: DkBackend::stamp_ when it happened)
struct Img {
    DkImage image;
    DkImageView view;
    DkImageFormat format = DkImageFormat_None;
    DkImageType type = DkImageType_2D;
    uint32_t w = 1, h = 1, d = 1, levels = 1;
    Alloc mem;
    uint32_t desc = 0;      // image descriptor index, 0 for none
    bool owned = true;      // the swapchain's images are the swapchain's
    uint64_t w_stamp = 0, r_stamp = 0;
    bool w_copy = false, r_copy = false;  // the last write / read was a copy (the copy engine: a full barrier)
};

// a console surface the renderer draws into
struct Target {
    Img* img = nullptr;  // BGRA8 (the guest's byte order: uploads and reads as they are) or Z24S8
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    Img* fb = nullptr;            // copy read by draws that sample the target they render to
};

struct Tex {
    Img* img = nullptr;
    TextureDesc d;
    bool decode = false;  // DXT decoded on the CPU for volume textures, as the GL renderer does (the same texels)
    bool r32f = false;
    uint32_t w = 1, h = 1, depth = 1, levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;
    uint64_t epoch = 0;   // DirtyEpoch at the last upload
};

struct PsoKey {
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t has_ds;
};

struct DrawTextures {
    TexInfo info[4];
    Img* tex[4] = {};
    bool r32f[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
};

struct Module {
    DkShader shader;
    bool ok = false;
    std::vector<glshader::Pair> pairs;
};

struct DrawShaders {
    bool prog = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    Module* vs = nullptr;
    Module* ps = nullptr;
};

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// MPayne2-nx (deko3d renderer): byte / 255.0f from a table of the same quotients (a draw converts about 20 colours, 80
// divisions; Color was 5.7% of the draw path's own time at level 27, console run dk4)
const std::array<float, 256> kByteUnit = [] {
    std::array<float, 256> t{};
    for (uint32_t i = 0; i < 256; i++) t[i] = i / 255.0f;
    return t;
}();

void Color(uint32_t argb, float* o) {
    o[0] = kByteUnit[(argb >> 16) & 255];
    o[1] = kByteUnit[(argb >> 8) & 255];
    o[2] = kByteUnit[argb & 255];
    o[3] = kByteUnit[argb >> 24];
}

// D3DCMP_NEVER 0x200 to ALWAYS 0x207: GL_NEVER to GL_ALWAYS are the same numbers
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

// ---- BEGIN the console's draw helpers, as gl_backend.cpp has them (its copy of vulkan.cpp's)
// ---- DXT on the CPU, for GPUs without BC textures (most phones)

void Rgb565(uint16_t c, uint8_t* bgr) {
    bgr[2] = uint8_t((c >> 11) * 255 / 31);
    bgr[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    bgr[0] = uint8_t((c & 31) * 255 / 31);
}

// one 4x4 block's colours, BGRA, out 16 x 4 bytes; dxt1 alpha for the 3 colour mode
void ColourBlock(const uint8_t* b, bool dxt1, uint8_t* out) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int i = 0; i < 4; i++) pal[i][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out + i * 4, pal[(bits >> (i * 2)) & 3], 4);
}

// a level of DXT1, 3 or 5 blocks (bw x bh per slice, row_pitch bytes a block row) into w x h BGRA texels
void DecodeDxt(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h, uint32_t z,
               std::vector<uint8_t>& out) {
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    out.assign(size_t(w) * h * z * 4, 0);
    uint8_t px[64];
    for (uint32_t s = 0; s < z; s++)
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t at = (size_t(s) * bh + by) * row_pitch + size_t(bx) * block;
                if (at + block > in.size()) continue;
                const uint8_t* b = &in[at];
                ColourBlock(host == Host::BC1 ? b : b + 8, host == Host::BC1, px);
                if (host == Host::BC2) {
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                } else if (host == Host::BC3) {
                    uint8_t a[8] = {b[0], b[1]};
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                    } else {
                        for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                        std::memcpy(&out[((size_t(s) * h + by * 4 + y) * w + bx * 4 + x) * 4], px + (y * 4 + x) * 4, 4);
            }
}

struct GuestExtent {
    uint32_t w, h;
};

// ---- what the console's state asks of a draw

// the game's 2D layer leaves oPos.w at 0: screen space, drawn with w = 1 as XYZRHW is
std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    const std::string clamp = "    p.w = magclamp(p.w);\n";
    const std::string screen_space = "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n";
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    size_t at = src.find(clamp);
    if (at != std::string::npos) src.replace(at, clamp.size(), screen_space);
    return src;
}

// a point list writes its size: Vulkan leaves it undefined otherwise; empty when the source has no main to wrap
std::string PointsHlsl(const std::string& src) {
    size_t at = src.find("VOut main(");
    if (at == std::string::npos) return {};
    size_t p = at + 10, sp = src.find(' ', p);
    std::string type = src.substr(p, sp - p);
    // the module's entry point is named main: the program's own main goes by another name
    std::string out = src;
    out.replace(at, 10, "VOut vs_body(");
    return out + "\nVOut main_points(" + type + " i, out float psize : PSIZE) { psize = 1; return vs_body(i); }\n";
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f, kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

// MPayne2-nx: a texture signature's hash. gpu::Hash is one chain, each step waiting on the last 64 x 64 -> 128 bit
// multiply: on the console it was 1.25 ms of the renderer's frame in chapter 1 (run 7); four lanes of that mix were
// still 3.6% of the title's thread (run 9), the A57's multiplies being slow. So four independent CRC32C lanes over the
// 8-byte words of each 32-byte block (the A57's CRC instructions, SSE4.2's on the PC: the same polynomial), folded
// through the mix at the end. A change inside one block is missed only if that lane's 32-bit CRC collides (never for
// a change of 32 bits or less). Signatures are only compared within a run, never stored.
#if defined(__aarch64__)
#define SIG_CRC 1
#define SIG_TARGET __attribute__((target("+crc")))
SIG_TARGET inline uint32_t SigCrc(uint32_t c, uint64_t v) { return __builtin_aarch64_crc32cx(c, v); }
#elif defined(__x86_64__) || defined(_M_X64)
#define SIG_CRC 1
#define SIG_TARGET __attribute__((target("sse4.2")))
SIG_TARGET inline uint32_t SigCrc(uint32_t c, uint64_t v) { return uint32_t(__builtin_ia32_crc32di(c, v)); }
#else
#define SIG_CRC 0
#define SIG_TARGET
#endif

SIG_TARGET uint64_t SigHash(const void* p, size_t n, uint64_t h) {
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    auto mix = [](uint64_t a, uint64_t b) {
        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
        return uint64_t(r) ^ uint64_t(r >> 64);
    };
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
#if SIG_CRC
    uint32_t c0 = uint32_t(h), c1 = uint32_t(h >> 32), c2 = ~uint32_t(h), c3 = ~uint32_t(h >> 32);
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        std::memcpy(w, b + i, 32);
        c0 = SigCrc(c0, w[0]);
        c1 = SigCrc(c1, w[1]);
        c2 = SigCrc(c2, w[2]);
        c3 = SigCrc(c3, w[3]);
    }
    uint64_t sig = mix(uint64_t(c0) | uint64_t(c1) << 32, (uint64_t(c2) | uint64_t(c3) << 32) ^ uint64_t(n) ^ k0);
#else
    uint64_t s0 = h ^ k0, s1 = h ^ k1, s2 = ~h ^ k0, s3 = ~h ^ k1;
    for (; i + 64 <= n; i += 64) {
        uint64_t w[8];
        std::memcpy(w, b + i, 64);
        s0 = mix(w[0] ^ k1, w[1] ^ s0);
        s1 = mix(w[2] ^ k1, w[3] ^ s1);
        s2 = mix(w[4] ^ k1, w[5] ^ s2);
        s3 = mix(w[6] ^ k1, w[7] ^ s3);
    }
    uint64_t sig = mix(s0 ^ s2, s1 ^ s3 ^ uint64_t(n));
#endif
    return i < n ? Hash(b + i, n - i, sig) : sig;
}

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return SigHash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = SigHash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4, pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    return px;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kPointsSalt = 0x9017;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (a.topology == Topology::Points) vkey ^= kPointsSalt;
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgram(*s.vs, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to Vulkan's RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

constexpr float kAlphaTestAlways = 7.0f;

void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6;

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, the host half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++)
        if ((fk.types >> (i * 2)) & 3) LightToView(s.lights[size_t(i)], vm, c.lt[i]);
}

struct GuestRect {
    int32_t left, top, right, bottom;
};
GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}
void ReadGuestPoint(uint32_t points, uint32_t i, int32_t& x, int32_t& y) {
    x = int32_t(kernel::R32(points + i * 8));
    y = int32_t(kernel::R32(points + i * 8 + 4));
}
uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

// a depth buffer is kept per size too: a title can draw shadow buffers of several sizes into the same memory (Cold
// Fear's lamps at 1024, 512 and 256) and samples the one its texture's size names
uint64_t TargetMapKey(uint32_t data, bool depth, uint32_t gw = 0, uint32_t gh = 0) {
    uint64_t k = uint64_t(data) << 1 | (depth ? 1 : 0);
    if (depth) k |= uint64_t(gw & 0x3FFF) << 33 | uint64_t(gh & 0x3FFF) << 47;
    return k;
}

// ---- END the console's draw helpers

// ---- the console's values as deko3d's (the GL renderer's mapping, GL's names swapped for deko3d's)

DkCompareOp Cmp(uint32_t v) { return DkCompareOp(DkCompareOp_Never + CmpIndex(v)); }

DkBlendFactor BlendFactor(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return DkBlendFactor_Zero;
        case 1: return DkBlendFactor_One;
        case 0x300: return alpha ? DkBlendFactor_SrcAlpha : DkBlendFactor_SrcColor;
        case 0x301: return alpha ? DkBlendFactor_InvSrcAlpha : DkBlendFactor_InvSrcColor;
        case 0x302: return DkBlendFactor_SrcAlpha;
        case 0x303: return DkBlendFactor_InvSrcAlpha;
        case 0x304: return DkBlendFactor_DstAlpha;
        case 0x305: return DkBlendFactor_InvDstAlpha;
        case 0x306: return alpha ? DkBlendFactor_DstAlpha : DkBlendFactor_DstColor;
        case 0x307: return alpha ? DkBlendFactor_InvDstAlpha : DkBlendFactor_InvDstColor;
        case 0x308: return alpha ? DkBlendFactor_One : DkBlendFactor_SrcAlphaSaturate;
        case 0x8001: return DkBlendFactor_ConstColor;
        case 0x8002: return DkBlendFactor_InvConstColor;
        case 0x8003: return DkBlendFactor_ConstAlpha;
        case 0x8004: return DkBlendFactor_InvConstAlpha;
        default: return DkBlendFactor_One;
    }
}

DkBlendOp BlendOperation(uint32_t v) {
    switch (v) {
        case 0x800A: return DkBlendOp_Sub;
        case 0x800B: return DkBlendOp_RevSub;
        case 0x8007: return DkBlendOp_Min;
        case 0x8008: return DkBlendOp_Max;
        default: return DkBlendOp_Add;
    }
}

DkStencilOp StencilOperation(uint32_t v) {
    switch (v) {
        case 0: return DkStencilOp_Zero;
        case 0x1E01: return DkStencilOp_Replace;
        case 0x1E02: return DkStencilOp_Incr;
        case 0x1E03: return DkStencilOp_Decr;
        case 0x150A: return DkStencilOp_Invert;
        case 0x8507: return DkStencilOp_IncrWrap;
        case 0x8508: return DkStencilOp_DecrWrap;
        default: return DkStencilOp_Keep;
    }
}

DkWrapMode Address(uint32_t v) {
    switch (v) {
        case 2: return DkWrapMode_MirroredRepeat;
        case 3: case 5: return DkWrapMode_ClampToEdge;
        case 4: return DkWrapMode_ClampToBorder;
        default: return DkWrapMode_Repeat;
    }
}

// the input assembler's formats the SDK's vertex.cpp names
struct AttribFormat {
    DkVtxAttribSize size;
    DkVtxAttribType type;
    bool bgra;
};
AttribFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return {DkVtxAttribSize_4x32, DkVtxAttribType_Float, false};
        case DXGI_FORMAT_R32G32B32_FLOAT: return {DkVtxAttribSize_3x32, DkVtxAttribType_Float, false};
        case DXGI_FORMAT_R32G32_FLOAT: return {DkVtxAttribSize_2x32, DkVtxAttribType_Float, false};
        case DXGI_FORMAT_R32_FLOAT: return {DkVtxAttribSize_1x32, DkVtxAttribType_Float, false};
        case DXGI_FORMAT_R32_UINT: return {DkVtxAttribSize_1x32, DkVtxAttribType_Uint, false};
        case DXGI_FORMAT_R16G16B16A16_SNORM: return {DkVtxAttribSize_4x16, DkVtxAttribType_Snorm, false};
        case DXGI_FORMAT_R16G16B16A16_SINT: return {DkVtxAttribSize_4x16, DkVtxAttribType_Sint, false};
        case DXGI_FORMAT_R16G16_SNORM: return {DkVtxAttribSize_2x16, DkVtxAttribType_Snorm, false};
        case DXGI_FORMAT_R16G16_SINT: return {DkVtxAttribSize_2x16, DkVtxAttribType_Sint, false};
        case DXGI_FORMAT_R16_SNORM: return {DkVtxAttribSize_1x16, DkVtxAttribType_Snorm, false};
        case DXGI_FORMAT_R16_SINT: return {DkVtxAttribSize_1x16, DkVtxAttribType_Sint, false};
        case DXGI_FORMAT_R8G8B8A8_UNORM: return {DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, false};
        case DXGI_FORMAT_R8G8_UNORM: return {DkVtxAttribSize_2x8, DkVtxAttribType_Unorm, false};
        case DXGI_FORMAT_R8_UNORM: return {DkVtxAttribSize_1x8, DkVtxAttribType_Unorm, false};
        case DXGI_FORMAT_B8G8R8A8_UNORM: return {DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, true};
        default: return {DkVtxAttribSize_4x32, DkVtxAttribType_Float, false};
    }
}

// fixed border colours only, as vulkan.cpp: the console's nearest. 0 transparent black, 1 opaque black, 2 white
uint32_t Border(uint32_t argb) {
    if ((argb >> 24) < 128) return 0;
    uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    return sum >= 384 ? 2 : 1;
}

// GL's sampler parameters as the GL renderer sets them: min and mag linear or nearest, the mip filter (none for the
// fixed samplers, which GL made with a plain GL_LINEAR / GL_NEAREST), the LOD clamps and bias, a border, a comparison
struct SamplerKey {
    uint32_t min, mag, mip, wrap[3], border, compare, func;
    float bias, min_lod, max_lod, aniso;
};

struct Program {
    Module* vs = nullptr;
    Module* ps = nullptr;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    bool over = false;  // premultiplied over the target, which keeps its alpha
};

constexpr float kDepth24Max = 16777215.0f;
constexpr float kUpscaledMin = 1.01f;

// the renderer's CPU time on the title's thread: draws, clears, copies, read-backs and the present without its waits
struct CpuTimer {
    uint64_t& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CpuTimer() {
        acc += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
};

uint64_t NsSince(std::chrono::steady_clock::time_point t0) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// a ring in CPU-written, GPU-read memory, in kRingSegments segments written front to back. Leaving a segment signals
// a fence after the commands that read it; entering one waits for its fence from the last lap (long signalled).
struct Ring {
    DkMemBlock mem = nullptr;
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    uint32_t size = 0, used = 0;
    int seg = 0;
    DkFence fence[kRingSegments] = {};
    bool fence_set[kRingSegments] = {};
    uint64_t fence_submit[kRingSegments] = {};  // the submission that carries the fence
    uint64_t gen = 0;                           // moved at each segment change: no draw reuses an offset across it
    uint32_t dirty_from = 0;                    // written since the last flush of the CPU's cache: [dirty_from, used)
    uint32_t SegSize() const { return size / kRingSegments; }
};

// what the DKSH file starts with (deko3d's examples define it the same way)
struct DkshHeader {
    uint32_t magic;  // 'DKSH'
    uint32_t header_sz;
    uint32_t control_sz;
    uint32_t code_sz;
    uint32_t programs_off;
    uint32_t num_programs;
};
constexpr uint32_t kDkshMagic = 0x48534B44;

// deko3d's fixed function state, bound only when it changes
struct DkStates {
    DkRasterizerState rs;
    DkColorState cs;
    DkColorWriteState cw;
    DkBlendState bs;
    DkDepthStencilState ds;
    float bias_const, bias_slope;
};

class DkBackend final : public Backend {
public:
    ~DkBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    // render_thread: the worker takes the renderer over after Init (deko3d objects are not tied to a thread; one
    // thread at a time records)
    void Adopt() override {
        thread_ = std::this_thread::get_id();
        render_thread_ = true;
        app::SetRenderThread(uint32_t(threadGetCurHandle()));
        kernel::AddHostThread(uint32_t(threadGetCurHandle()));
        LOGI("dk: the renderer runs on its own thread (handle %X)", unsigned(threadGetCurHandle()));
    }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override {
        if (Current()) Submit();
    }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void SetOverlay(const uint8_t* bgra, uint32_t w, uint32_t h, const int32_t* rect) override {
        overlay_on_ = bgra && w && h;
        if (!overlay_on_) return;
        overlay_px_.assign(bgra, bgra + size_t(w) * h * 4);
        overlay_w_ = w;
        overlay_h_ = h;
        std::memcpy(overlay_rect_, rect, sizeof(overlay_rect_));
        overlay_new_ = true;
    }

private:
    // device, queue, frames
    bool CreateDevice();
    bool Current();
    void Submit();
    void BeginFrame();
    static void AddMem(void* user, DkCmdBuf cb, size_t min);
    DkMemBlock NewBlock(uint32_t size, uint32_t flags);

    // the image heap and descriptors
    bool AllocImage(uint32_t size, uint32_t align, Alloc& out);
    Img* NewImage(DkImageType type, DkImageFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels,
                  uint32_t flags);
    void DeleteImage(Img*& img);
    void DestroyImage(Img* img);
    uint32_t SamplerId(const SamplerKey& k);
    uint32_t SamplerIdSlow(const SamplerKey& k);
    uint32_t StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool compare);
    uint32_t FixedSampler(bool linear);

    // rings
    bool InitRing(Ring& r, uint32_t size);
    void FlushRing(Ring& r);
    void NextSegment(Ring& r);
    void EnsureRoom(Ring& r, uint32_t need);
    uint32_t Put(Ring& r, const void* data, uint32_t n, uint32_t align);

    // barriers
    void Barrier(DkBarrier kind);
    void BeforeSample(Img* img);
    void BeforeRender(Img* img);
    void BeforeCopyTo(Img* img);
    void BeforeCopyFrom(Img* img);
    void FlushPendingBarrier();
    void PrepareDraw();
    DkBarrier pending_barrier_ = DkBarrier_None;
    Img* reads_[64] = {};  // the images the next draw samples, stamped once its barrier is recorded
    uint32_t nreads_ = 0;

    // shaders
    Module* EnsureModule(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    bool CompileDksh(bool vertex, const std::string& glsl, std::vector<uint8_t>& dksh, std::string& log);
    bool LoadShader(const std::vector<uint8_t>& dksh, DkShader& out);
    bool LoadCached(uint64_t hash, std::vector<glshader::Pair>& pairs, std::vector<uint8_t>& dksh);
    void StoreCached(uint64_t hash, const std::vector<glshader::Pair>& pairs, const std::vector<uint8_t>& dksh);
    static void* UamMain(void* self);
    Program* EnsureProgram(uint64_t vh, Module* vs, uint64_t ph, Module* ps);
    Program* FullScreen(const char* hlsl, const char* ps_entry);
    void BindProgram(const Program& p);
    void BindTextureUnit(DkStage stage, uint32_t unit, Img* img, uint32_t sampler);
    void BindFixedTextures(const Program& p, Img* const tex[4], const uint32_t samplers[4]);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsureVertexShaderSlow(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void EnsurePixelShaderSlow(const PixelKey& pk, DrawShaders& sh);
    uint64_t last_vkey_ = 0, last_vh_ = 0, last_pkey_ = 0, last_ph_ = 0;  // the last draw's shaders
    Module* last_vs_ = nullptr;
    Module* last_ps_ = nullptr;

    // state
    void BindStates(const DkStates& st);
    void ApplyState(const PsoKey& k);
    void PlainState(bool over, bool depth_write);
    void BindTargets(Img* color, Img* depth);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x, int32_t y, int32_t w, int32_t h);
    void BindUniforms(uint32_t index, uint32_t offset, uint32_t size);
    void BindAttribs(const DkVtxAttribState* attribs, uint32_t n);
    void BindStrides(const DkVtxBufferState* strides, uint32_t n);
    void ResetStateCache();

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    Target* EnsureTargetSlow(const Surface& s, bool depth);
    struct TargetHit {
        Surface s;
        Target* t = nullptr;
    };
    TargetHit target_hit_[2];  // EnsureTarget's last colour and depth targets
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void UploadTarget(Target& t, const Surface& s);
    void Upload2D(Img* img, const uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h);
    void CopyRect(Img* src, const DkImageRect& sr, Img* dst, const DkImageRect& dr);
    void Blit(Img* dst, uint32_t dw, uint32_t dh, int32_t x, int32_t y, int32_t w, int32_t h, Img* src,
              const float uvrect[4], const BlitOptions& o);
    Img* Feedback(Target& t);
    bool DepthCopy(const State& s);
    Img* CopySource(Target& t, Img*& down);
    bool ReadBack(Img* img, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    // draws
    bool BindStageTextures(const State& s, DrawTextures& stages);
    Img* BindTexture(const State& s, int stage, DrawTextures& stages);
    Img* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box, DrawTextures& stages);
    Img* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    void ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil);

    // present
    void DrawOverlay(Target& t);
    void UploadGammaRamp();
    void SelfTest();
    void ReleaseOldTextures();
    void ApplySettings();
    bool CaptureDue();
    bool InitUi();
    void RenderUi(Img* fb);
    void WriteBmp(const char* name, const std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool top_down);
    void WantScale(float& y, float& x);

    // ---- deko3d's objects
    DkDevice device_ = nullptr;
    DkQueue queue_ = nullptr;
    DkCmdBuf cmd_ = nullptr;
    DkSwapchain swapchain_ = nullptr;
    DkMemBlock swap_mem_ = nullptr;
    Img swap_[kSwapImages];
    DkMemBlock code_mem_ = nullptr;
    uint8_t* code_cpu_ = nullptr;
    uint32_t code_used_ = 0;
    DkMemBlock desc_mem_ = nullptr;
    uint8_t* desc_cpu_ = nullptr;
    DkGpuAddr desc_gpu_ = 0;
    bool desc_dirty_ = false;    // descriptors written since the last invalidation of the GPU's descriptor cache
    bool code_dirty_ = false;    // shader code loaded since the last invalidation of the GPU's shader cache
    uint32_t next_image_desc_ = 1, next_sampler_ = 0;
    std::vector<uint32_t> free_image_desc_;
    struct Chunk {
        DkMemBlock mem = nullptr;
        uint32_t size = 0;
        std::vector<std::pair<uint32_t, uint32_t>> free;  // offset, size; sorted by offset
    };
    std::vector<Chunk> heap_;
    Ring stream_, ubo_;
    struct FrameSlot {
        DkMemBlock cmd = nullptr;
        DkFence fence = {};
        bool pending = false;
        std::vector<DkMemBlock> extra;  // command memory added when a frame outgrew kCmdSize
        std::vector<Img*> trash;        // images deleted during the frame, destroyed once its fence is signalled
    };
    FrameSlot slots_[kFrameSlots];
    int slot_ = -1;
    uint64_t submits_ = 0;
    DkMemBlock ts_mem_ = nullptr;  // per slot: timestamps at the frame's start and end (16 bytes each)
    DkMemBlock rb_mem_ = nullptr;  // read-backs
    uint32_t rb_size_ = 0;
    DkFence rb_fence_ = {};

    // the uam thread: one compile at a time, the title's thread waiting for it
    pthread_t uam_thread_{};
    bool uam_started_ = false;
    std::mutex uam_m_;
    std::condition_variable uam_cv_;
    const std::string* uam_glsl_ = nullptr;
    bool uam_vertex_ = false, uam_done_ = false;
    uam::Result uam_result_;

    // state already bound: an unchanged one costs no command
    DkStates st_{};
    bool st_valid_ = false;
    Img* bound_color_ = nullptr;
    Img* bound_depth_ = nullptr;
    bool targets_valid_ = false;
    const Program* bound_prog_ = nullptr;
    DkResHandle bound_tex_[2][32] = {};  // vertex, fragment
    bool tex_valid_[2][32] = {};
    uint32_t ubo_bound_[2] = {~0u, ~0u}, ubo_bound_size_[2] = {};
    float vp_[4] = {-1, -1, -1, -1};
    int32_t sc_[4] = {-1, -1, -1, -1};
    float blend_color_[4] = {-1, -1, -1, -1};
    uint32_t stencil_[3] = {~0u, ~0u, ~0u};
    DkVtxAttribState attribs_[16] = {};
    uint32_t attribs_n_ = ~0u;
    DkVtxBufferState strides_[16] = {};
    uint32_t strides_n_ = ~0u;
    uint64_t stamp_ = 1, full_stamp_ = 1;  // hazard barriers so far; the stamp after the last full one

    // the last b0 / b1 uploaded, reused while the game's constants stay the same
    std::array<std::array<float, 4>, 192> last_vc_{};
    alignas(16) uint8_t last_dc_[sizeof(DrawConstants)] = {};
    bool last_dc_valid_ = false;
    uint32_t last_vsc_gen_ = ~0u;
    uint32_t last_vc_off_ = 0, last_dc_off_ = 0;
    uint64_t last_vc_gen_ = ~0ull, last_dc_gen_ = ~0ull;

    // figures over the 300 frames between log lines
    uint64_t up_bytes_ = 0, calls_ = 0, skipped_ = 0;
    uint64_t cpu_ns_ = 0, syncs_ = 0, sync_ns_ = 0;
    uint64_t asm_ns_ = 0, tex_ns_ = 0, gl_ns_ = 0;
    uint64_t frame_cpu0_ = 0, frame_asm0_ = 0, frame_tex0_ = 0, frame_gl0_ = 0, frame_up0_ = 0, frame_draws0_ = 0;
    uint32_t long_logged_ = 0;
    uint64_t ring_waits_ = 0, ring_wait_ns_ = 0;
    uint64_t barriers_frag_ = 0, barriers_full_ = 0;
    uint64_t gpu_ns_ = 0, gpu_n_ = 0, gpu_max_ns_ = 0;      // the GPU's time from a frame's first command to its last
    uint64_t gpu_lag_ns_ = 0, gpu_lag_max_ns_ = 0;          // the frame's end on the GPU after its submission
    uint64_t gpu_issue_[kFrameSlots] = {};
    uint64_t acquire_ns_ = 0, frame_wait_ns_ = 0;           // waits in the present: for a swapchain image, a frame slot
    uint64_t compiles_ = 0, compile_ns_ = 0, cache_loads_ = 0;
    uint64_t sig_only_ = 0, sig_only_total_ = 0;  // texture changes seen by the signature alone (no lock)
    uint64_t asm_dense_ = 0, asm_gathered_ = 0, asm_converted_ = 0;  // draws by how their vertices were taken
    uint64_t asm_dense_bytes_ = 0, asm_gathered_bytes_ = 0, asm_converted_bytes_ = 0;

    std::thread::id thread_;
    bool render_thread_ = false;  // Adopt: calls come from render_thread.cpp's worker
    bool ok_ = false;
    bool vsync_ = true;
    bool face_swap_ = false;  // dk_flags.txt "faceswap": the other winding sense, for checking
    bool scissor_ = true;     // dk_flags.txt "noscissor"
    std::string flags_;
    uint32_t interval_ = 1;

    std::unordered_map<uint64_t, Module> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, Program> programs_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    Img* lut_ = nullptr;
    Img* dummy_[3] = {};  // 2D, 3D, cube: black
    Img* overlay_ = nullptr;
    std::vector<uint8_t> overlay_px_;
    uint32_t overlay_w_ = 0, overlay_h_ = 0;
    int32_t overlay_rect_[4] = {};
    bool overlay_on_ = false, overlay_new_ = false;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = true;
    bool gamma_ = false;
    float lift_ = 1.0f;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    uint32_t out_w_ = 1280, out_h_ = 720;
    float scale_ = 1.0f, scale_x_ = 1.0f;
    Assembled asm_;
    std::vector<uint8_t> up_px_, up_decoded_, up_packed_;
    uint64_t presents_ = 0;
    std::chrono::steady_clock::time_point last_shot_{};
    uint32_t shots_ = 0;
    uint64_t draws_ = 0;
    bool ui_ = false;
    Img* ui_font_ = nullptr;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

// ---- device, queue, frames

constexpr uint32_t kSurfaceW = 1280, kSurfaceH = 720;

void DebugMessage(void*, const char* context, DkResult result, const char* message) {
    LOG_FIRST(64, "dk: %s: %s (%d)", context ? context : "?", message ? message : "", int(result));
}

DkMemBlock DkBackend::NewBlock(uint32_t size, uint32_t flags) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, device_, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    DkMemBlock b = dkMemBlockCreate(&m);
    if (!b) LOGE("dk: a memory block of %u KB (flags %x) refused", size >> 10, flags);
    return b;
}

// a frame that outgrew its command memory: another block for it, freed with the frame
void DkBackend::AddMem(void* user, DkCmdBuf cb, size_t min) {
    auto* self = static_cast<DkBackend*>(user);
    uint32_t size = std::max<uint32_t>(AlignUp(uint32_t(min), DK_MEMBLOCK_ALIGNMENT), 1u << 20);
    DkMemBlock b = self->NewBlock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!b) return;
    self->slots_[self->slot_ < 0 ? 0 : self->slot_].extra.push_back(b);
    dkCmdBufAddMemory(cb, b, 0, size);
    LOG_FIRST(8, "dk: a frame's commands outgrew %u KB: %u KB more", kCmdSize >> 10, size >> 10);
}

bool DkBackend::CreateDevice() {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = DebugMessage;
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
    device_ = dkDeviceCreate(&dm);
    if (!device_) {
        LOGE("dk: no device");
        return false;
    }
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device_);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_EnableZcull;
    qm.commandMemorySize = 1u << 20;
    qm.flushThreshold = qm.commandMemorySize / 8;
    // local memory for shaders that spill or index arrays: 16 KB a warp
    qm.perWarpScratchMemorySize = 32 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT;
    queue_ = dkQueueCreate(&qm);
    if (!queue_) {
        LOGE("dk: no queue");
        return false;
    }
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device_);
    cm.userData = this;
    cm.cbAddMem = AddMem;
    cmd_ = dkCmdBufCreate(&cm);
    for (FrameSlot& f : slots_) f.cmd = NewBlock(kCmdSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    code_mem_ = NewBlock(kCodeSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    desc_mem_ = NewBlock((kMaxImages + kMaxSamplers) * 32, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    ts_mem_ = NewBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!cmd_ || !code_mem_ || !desc_mem_ || !ts_mem_ || !slots_[kFrameSlots - 1].cmd) return false;
    code_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(code_mem_));
    desc_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(desc_mem_));
    desc_gpu_ = dkMemBlockGetGpuAddr(desc_mem_);
    std::memset(dkMemBlockGetCpuAddr(ts_mem_), 0, DK_MEMBLOCK_ALIGNMENT);
    if (!InitRing(stream_, kStreamSize) || !InitRing(ubo_, kUniformSize)) return false;

    // the swapchain: three 1280x720 images on the applet's window, the system scaling them to the TV when docked
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = kSurfaceW;
    lm.dimensions[1] = kSurfaceH;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    uint32_t size = AlignUp(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    swap_mem_ = NewBlock(size * kSwapImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!swap_mem_) return false;
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        Img& im = swap_[i];
        dkImageInitialize(&im.image, &layout, swap_mem_, i * size);
        dkImageViewDefaults(&im.view, &im.image);
        im.format = lm.format;
        im.w = kSurfaceW;
        im.h = kSurfaceH;
        im.owned = false;
        images[i] = &im.image;
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device_, nwindowGetDefault(), images, kSwapImages);
    swapchain_ = dkSwapchainCreate(&sm);
    if (!swapchain_) {
        LOGE("dk: no swapchain");
        return false;
    }
    dkSwapchainSetSwapInterval(swapchain_, vsync_ ? 1 : 0);
    interval_ = vsync_ ? 1 : 0;
    return true;
}

// the renderer is the title thread's: a call from another thread is logged and dropped
bool DkBackend::Current() {
    if (!ok_) return false;
    if (std::this_thread::get_id() == thread_) return true;
    LOG_FIRST(8, "dk: a call from another thread dropped (the renderer is the title thread's)");
    return false;
}

// what is recorded so far to the GPU (it starts on it at once), the rings' new bytes out of the CPU's cache first
void DkBackend::Submit() {
    FlushPendingBarrier();
    FlushRing(stream_);
    FlushRing(ubo_);
    dkQueueSubmitCommands(queue_, dkCmdBufFinishList(cmd_));
    dkQueueFlush(queue_);
    submits_++;
}

void DkBackend::ResetStateCache() {
    st_valid_ = false;
    targets_valid_ = false;
    bound_prog_ = nullptr;
    std::memset(tex_valid_, 0, sizeof(tex_valid_));
    ubo_bound_[0] = ubo_bound_[1] = ~0u;
    for (float& v : vp_) v = -1;
    for (int32_t& v : sc_) v = -1;
    for (float& v : blend_color_) v = -1;
    for (uint32_t& v : stencil_) v = ~0u;
    attribs_n_ = strides_n_ = ~0u;
}

// the next frame slot: the frame that used it last finished (its fence), what it deleted destroyed, its command memory
// the command buffer's; the frame's state bound from scratch
void DkBackend::BeginFrame() {
    slot_ = (slot_ + 1) % kFrameSlots;
    FrameSlot& f = slots_[slot_];
    if (f.pending) {
        auto t0 = std::chrono::steady_clock::now();
        dkFenceWait(&f.fence, -1);
        frame_wait_ns_ += NsSince(t0);
        f.pending = false;
        // the GPU's busy time for that frame: from its first command to the game's last, and from the swapchain image's
        // arrival to the frame's end (16 bytes a report: the counter, then the timestamp)
        const uint64_t* ts = static_cast<const uint64_t*>(dkMemBlockGetCpuAddr(ts_mem_)) + slot_ * 8;
        uint64_t b = ts[1], m1 = ts[3], m2 = ts[5], e = ts[7];
        if (e > b && m1 >= b && e >= m2) {
            uint64_t ns = dkTimestampToNs((m1 - b) + (e - m2));
            gpu_ns_ += ns;
            gpu_max_ns_ = std::max(gpu_max_ns_, ns);
            gpu_n_++;
            if (e > gpu_issue_[slot_]) {
                uint64_t lag = dkTimestampToNs(e - gpu_issue_[slot_]);
                gpu_lag_ns_ += lag;
                gpu_lag_max_ns_ = std::max(gpu_lag_max_ns_, lag);
            }
        }
    }
    for (Img* img : f.trash) DestroyImage(img);
    f.trash.clear();
    dkCmdBufClear(cmd_);
    for (DkMemBlock b : f.extra) dkMemBlockDestroy(b);
    f.extra.clear();
    dkCmdBufAddMemory(cmd_, f.cmd, 0, kCmdSize);
    dkCmdBufBindImageDescriptorSet(cmd_, desc_gpu_, kMaxImages);
    dkCmdBufBindSamplerDescriptorSet(cmd_, desc_gpu_ + kMaxImages * 32, kMaxSamplers);
    ResetStateCache();
    dkCmdBufReportCounter(cmd_, DkCounter_Timestamp, dkMemBlockGetGpuAddr(ts_mem_) + slot_ * 64);
}

// ---- memory

// the rings are written at the CPU cache's speed (uncached memory took 3.8% of the title's thread in Put at level 27,
// console run dk4) and their new bytes flushed to memory before each submission (the ultimate base's plume layer does
// the same for its uploads)
bool DkBackend::InitRing(Ring& r, uint32_t size) {
    r.mem = NewBlock(size, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
    if (!r.mem) return false;
    r.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(r.mem));
    r.gpu = dkMemBlockGetGpuAddr(r.mem);
    r.size = size;
    return true;
}

void DkBackend::FlushRing(Ring& r) {
    if (r.used > r.dirty_from) dkMemBlockFlushCpuCache(r.mem, r.dirty_from, r.used - r.dirty_from);
    r.dirty_from = r.used;
}

// leaving segment i fences the commands so far (they are the last to read it); entering segment j waits for j's fence
// from the previous lap, sending the commands first if that fence is still among them
void DkBackend::NextSegment(Ring& r) {
    FlushRing(r);
    dkCmdBufSignalFence(cmd_, &r.fence[r.seg], false);
    r.fence_set[r.seg] = true;
    r.fence_submit[r.seg] = submits_;
    r.seg = (r.seg + 1) % kRingSegments;
    if (r.fence_set[r.seg]) {
        if (r.fence_submit[r.seg] == submits_) Submit();
        auto t0 = std::chrono::steady_clock::now();
        if (dkFenceWait(&r.fence[r.seg], 0) == DkResult_Timeout) {
            dkFenceWait(&r.fence[r.seg], -1);
            ring_waits_++;
            ring_wait_ns_ += NsSince(t0);
        }
        r.fence_set[r.seg] = false;
    }
    r.used = uint32_t(r.seg) * r.SegSize();
    r.dirty_from = r.used;
    r.gen++;
    // constants and vertices at addresses the GPU read a lap ago: none of it from its caches
    dkCmdBufBarrier(cmd_, DkBarrier_None, DkInvalidateFlags_Shader);
}

// room for need bytes in the segment being written: a draw's uploads stay together
void DkBackend::EnsureRoom(Ring& r, uint32_t need) {
    if (need <= r.SegSize() && r.used + need > uint32_t(r.seg + 1) * r.SegSize()) NextSegment(r);
}

uint32_t DkBackend::Put(Ring& r, const void* data, uint32_t n, uint32_t align) {
    r.used = AlignUp(r.used, align);
    if (r.used + n > uint32_t(r.seg + 1) * r.SegSize()) {
        NextSegment(r);
        if (n > r.SegSize()) {
            LOGW("dk: an upload of %u bytes is larger than a ring segment: left out", n);
            return r.used;
        }
    }
    uint32_t at = r.used;
    if (data && n) {
        std::memcpy(r.cpu + at, data, n);
        up_bytes_ += n;
    }
    r.used += std::max<uint32_t>(n, 4);
    return at;
}

// first fit in the heap's chunks, a new chunk when none has room
bool DkBackend::AllocImage(uint32_t size, uint32_t align, Alloc& out) {
    size = AlignUp(size, align);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t c = 0; c < heap_.size(); c++) {
            auto& fr = heap_[c].free;
            for (size_t i = 0; i < fr.size(); i++) {
                uint32_t start = AlignUp(fr[i].first, align), end = fr[i].first + fr[i].second;
                if (start + size > end) continue;
                uint32_t before = start - fr[i].first, after = end - (start + size);
                uint32_t first = fr[i].first;
                fr.erase(fr.begin() + long(i));
                if (after) fr.insert(fr.begin() + long(i), {start + size, after});
                if (before) fr.insert(fr.begin() + long(i), {first, before});
                out = {int(c), start, size};
                return true;
            }
        }
        if (pass) break;
        Chunk ch;
        ch.size = std::max(kHeapChunk, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
        ch.mem = NewBlock(ch.size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
        if (!ch.mem) return false;
        ch.free.push_back({0, ch.size});
        heap_.push_back(std::move(ch));
        LOGI("dk: image heap %zu chunks, %u MB", heap_.size(), uint32_t(heap_.size()) * (kHeapChunk >> 20));
    }
    return false;
}

bool CompressedFormat(DkImageFormat f) {
    return f == DkImageFormat_RGBA_BC1 || f == DkImageFormat_RGBA_BC2 || f == DkImageFormat_RGBA_BC3;
}

bool DepthFormat(DkImageFormat f) { return f == DkImageFormat_Z24S8; }

// deko3d 0.5.0's tile height bug (found by SwitchWakerHD, its memory.cpp image_tile_size_fix; the ultimate base's plume
// layer has it too): dkImageLayoutInitialize picks the block-linear tile from 1.5 x the height and its layout shrinks
// it for level 0, while the copy engines and the image descriptor use the unshrunk one; where the two differ an
// upload lands in the wrong rows. The shrunk tile is given as a custom tile size there.
void TileSizeFix(DkImageLayoutMaker& m, uint32_t rows) {
    if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) return;
    if (m.type == DkImageType_3D) return;
    const uint32_t gobs = (rows + rows / 2 + 7) / 8;
    uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
    uint32_t shrunk = tile;
    while (shrunk && (8u << (shrunk - 1)) >= rows) shrunk--;
    if (shrunk == tile) return;
    m.flags |= DkImageFlags_CustomTileSize;
    m.tileSize = DkTileSize(shrunk);
}

Img* DkBackend::NewImage(DkImageType type, DkImageFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels,
                         uint32_t flags) {
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.type = type;
    // uncompressed colour images are the 2D engine's too (the ultimate base's plume layer does the same)
    if (type != DkImageType_3D && !CompressedFormat(fmt) && !DepthFormat(fmt)) flags |= DkImageFlags_Usage2DEngine;
    lm.flags = flags;
    lm.format = fmt;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    lm.dimensions[2] = type == DkImageType_3D ? d : type == DkImageType_Cubemap ? 6 : 0;
    lm.mipLevels = levels;
    TileSizeFix(lm, CompressedFormat(fmt) ? (h + 3) / 4 : h);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    Img* img = new Img;
    if (!AllocImage(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout), img->mem)) {
        delete img;
        return nullptr;
    }
    dkImageInitialize(&img->image, &layout, heap_[size_t(img->mem.chunk)].mem, img->mem.offset);
    dkImageViewDefaults(&img->view, &img->image);
    img->format = fmt;
    img->type = type;
    img->w = w;
    img->h = h;
    img->d = d;
    img->levels = levels;
    if (!free_image_desc_.empty()) {
        img->desc = free_image_desc_.back();
        free_image_desc_.pop_back();
    } else if (next_image_desc_ < kMaxImages) {
        img->desc = next_image_desc_++;
    } else {
        LOG_FIRST(4, "dk: out of image descriptors");
    }
    if (img->desc) {
        DkImageDescriptor desc;
        dkImageDescriptorInitialize(&desc, &img->view, false, false);
        std::memcpy(desc_cpu_ + size_t(img->desc) * 32, &desc, sizeof(desc));
        desc_dirty_ = true;
    }
    return img;
}

// the image goes when the frame that deleted it has finished on the GPU
void DkBackend::DeleteImage(Img*& img) {
    if (!img) return;
    if (draw_rt_ && draw_rt_->img == img) draw_rt_ = nullptr;
    if (bound_color_ == img || bound_depth_ == img) targets_valid_ = false;
    slots_[slot_].trash.push_back(img);
    img = nullptr;
}

void DkBackend::DestroyImage(Img* img) {
    if (!img->owned) return;
    if (img->desc) free_image_desc_.push_back(img->desc);
    Chunk& ch = heap_[size_t(img->mem.chunk)];
    auto& fr = ch.free;
    auto it = std::lower_bound(fr.begin(), fr.end(), std::make_pair(img->mem.offset, 0u));
    it = fr.insert(it, {img->mem.offset, img->mem.size});
    // merged with its neighbours
    if (it + 1 != fr.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        fr.erase(it + 1);
    }
    if (it != fr.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        fr.erase(it);
    }
    delete img;
}

// a small key's hash: CRC32C over its 8-byte words in two lanes (the A57's CRC instructions), for the sampler cache,
// which compares the whole key behind it
SIG_TARGET uint32_t KeyHash(const void* p, size_t n) {
#if SIG_CRC
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint32_t c0 = 0x12345678u, c1 = 0x9ABCDEF0u;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t w[2];
        std::memcpy(w, b + i, 16);
        c0 = SigCrc(c0, w[0]);
        c1 = SigCrc(c1, w[1]);
    }
    uint64_t t[2] = {};
    std::memcpy(t, b + i, n - i);
    c0 = SigCrc(c0, t[0]);
    c1 = SigCrc(c1, t[1] ^ n);
    return c0 ^ (c1 * 0x9E3779B1u);
#else
    return uint32_t(Hash(p, n));
#endif
}

uint32_t DkBackend::SamplerId(const SamplerKey& k) {
    // a direct mapped cache in front of the map: a draw asks for 6-8 samplers (gpu::Hash's byte at a time FNV and the
    // map's lookup were 1.3% of the title's thread at level 27, console run dk4)
    struct Cached {
        SamplerKey key;
        uint32_t id;
        bool valid;
    };
    static Cached cache[256];
    Cached& ce = cache[KeyHash(&k, sizeof(k)) & 255];
    if (ce.valid && !std::memcmp(&ce.key, &k, sizeof(k))) return ce.id;
    uint32_t id = SamplerIdSlow(k);
    ce.key = k;
    ce.id = id;
    ce.valid = true;
    return id;
}

uint32_t DkBackend::SamplerIdSlow(const SamplerKey& k) {
    uint64_t key = Hash(&k, sizeof(k));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    if (next_sampler_ >= kMaxSamplers) {
        LOG_FIRST(4, "dk: out of sampler descriptors");
        return 0;
    }
    DkSampler s;
    dkSamplerDefaults(&s);
    s.minFilter = DkFilter(k.min);
    s.magFilter = DkFilter(k.mag);
    s.mipFilter = DkMipFilter(k.mip);
    for (int i = 0; i < 3; i++) s.wrapMode[i] = DkWrapMode(k.wrap[i]);
    s.lodBias = k.bias;
    s.lodClampMin = k.min_lod;
    s.lodClampMax = k.max_lod;
    s.maxAnisotropy = k.aniso > 1 ? k.aniso : 1.0f;
    static const float borders[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    for (int i = 0; i < 4; i++) s.borderColor[i].value_f = borders[k.border][i];
    if (k.compare) {
        s.compareEnable = true;
        s.compareOp = DkCompareOp(k.func);
    }
    DkSamplerDescriptor desc;
    dkSamplerDescriptorInitialize(&desc, &s);
    uint32_t id = next_sampler_++;
    std::memcpy(desc_cpu_ + size_t(kMaxImages) * 32 + size_t(id) * 32, &desc, sizeof(desc));
    desc_dirty_ = true;
    samplers_[key] = id;
    return id;
}

uint32_t DkBackend::FixedSampler(bool linear) {
    SamplerKey k{};
    k.min = k.mag = linear ? DkFilter_Linear : DkFilter_Nearest;
    k.mip = DkMipFilter_None;
    k.wrap[0] = k.wrap[1] = k.wrap[2] = DkWrapMode_ClampToEdge;
    k.max_lod = 1000;
    return SamplerId(k);
}

uint32_t DkBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool compare) {
    SamplerKey k{};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    bool mip_lin = mip == kTexfLinear;
    bool min_lin = lin(min), mag_lin = lin(mag);
    k.min = min_lin ? DkFilter_Linear : DkFilter_Nearest;
    k.mip = mip_lin ? DkMipFilter_Linear : DkMipFilter_Nearest;
    k.mag = mag_lin ? DkFilter_Linear : DkFilter_Nearest;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t aniso = app::Get().aniso;
    bool mipped = mip && stages.levels[i] > 1;
    if (aniso && min_lin && mipped && !stages.info[i].shadow) an = aniso;
    if (an > 1) k.aniso = std::min(float(an), 16.0f);
    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    k.wrap[0] = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    k.wrap[1] = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    k.wrap[2] = Address(t[kTssAddressW]);
    k.bias = std::clamp(Bits(t[kTssMipLodBias]), -15.0f, 15.0f);
    k.min_lod = std::min(float(t[kTssMaxMipLevel]), float(stages.levels[i] - 1));
    k.max_lod = mip ? 1000.0f : k.min_lod;
    k.border = Border(t[kTssBorderColor]);
    if (compare) {
        // the console tests texel OP r, deko3d (as GL) r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        bool linear = lin(min) || lin(mag);
        k.min = linear ? DkFilter_Linear : DkFilter_Nearest;
        k.mip = DkMipFilter_Nearest;
        k.mag = linear ? DkFilter_Linear : DkFilter_Nearest;
        k.compare = 1;
        k.func = uint32_t(DkCompareOp_Never) + swap[CmpIndex(s.rs[kRsShadowFunc])];
        k.aniso = 0;
        // a depth texture's border is a depth: 0000FFFF is the far plane of a 16 bit buffer (vulkan.cpp)
        k.border = (t[kTssBorderColor] & 0xFFFFFF) >= 0x8000 ? 2 : 1;
    }
    return SamplerId(k);
}

// ---- barriers: a target drawn and then read waits for the drawing (fragments) and drops the texture cache; a copy
// (the copy engine) before or after waits for everything (full)

void DkBackend::Barrier(DkBarrier kind) {
    dkCmdBufBarrier(cmd_, kind, DkInvalidateFlags_Image);
    stamp_++;
    if (kind == DkBarrier_Full) {
        full_stamp_ = stamp_;
        barriers_full_++;
    } else {
        barriers_frag_++;
    }
    pending_barrier_ = DkBarrier_None;
}

// the barriers a draw needs are gathered while its textures and targets are bound, and recorded once before it
void DkBackend::FlushPendingBarrier() {
    if (pending_barrier_ != DkBarrier_None) Barrier(pending_barrier_);
}

void DkBackend::BeforeSample(Img* img) {
    if (!img) return;
    bool pending = img->w_copy ? img->w_stamp >= full_stamp_ : img->w_stamp == stamp_;
    if (pending) pending_barrier_ = std::max(pending_barrier_, img->w_copy ? DkBarrier_Full : DkBarrier_Fragments);
    if (nreads_ < std::size(reads_)) reads_[nreads_++] = img;
}

// right before a draw: its barrier, the descriptor cache dropped if descriptors were written, its reads stamped with
// the stamp after the barrier (a later write to one of them must wait for this draw)
void DkBackend::PrepareDraw() {
    FlushPendingBarrier();
    if (desc_dirty_) {
        dkCmdBufBarrier(cmd_, DkBarrier_None, DkInvalidateFlags_Descriptors);
        desc_dirty_ = false;
    }
    for (uint32_t i = 0; i < nreads_; i++) {
        reads_[i]->r_stamp = stamp_;
        reads_[i]->r_copy = false;
    }
    nreads_ = 0;
}

void DkBackend::BeforeRender(Img* img) {
    if (!img) return;
    if ((img->r_copy && img->r_stamp >= full_stamp_) || (img->w_copy && img->w_stamp >= full_stamp_))
        pending_barrier_ = DkBarrier_Full;
    else if (img->r_stamp == stamp_)
        pending_barrier_ = std::max(pending_barrier_, DkBarrier_Fragments);
}

void DkBackend::BeforeCopyTo(Img* img) {
    if (img->w_stamp >= full_stamp_ || img->r_stamp >= full_stamp_) pending_barrier_ = DkBarrier_Full;
    FlushPendingBarrier();
}

void DkBackend::BeforeCopyFrom(Img* img) {
    if (img->w_stamp >= full_stamp_) pending_barrier_ = DkBarrier_Full;
    FlushPendingBarrier();
}

// ---- shaders

// one compile at a time, on a thread with the stack Mesa's GLSL front end and nv50_ir need
void* DkBackend::UamMain(void* p) {
    auto* self = static_cast<DkBackend*>(p);
    // uam as devkitPro ships it, with the ultimate base's fixes: none of banjo-nx's optional code generation passes
    uam::setBanjoFlags(0);
    uam::init(true);
    for (;;) {
        std::unique_lock lock(self->uam_m_);
        self->uam_cv_.wait(lock, [self] { return self->uam_glsl_ != nullptr; });
        const std::string* glsl = self->uam_glsl_;
        bool vertex = self->uam_vertex_;
        lock.unlock();
        uam::Result r = uam::compile(vertex ? uam::Stage::Vertex : uam::Stage::Fragment, glsl->c_str());
        lock.lock();
        self->uam_result_ = std::move(r);
        self->uam_glsl_ = nullptr;
        self->uam_done_ = true;
        lock.unlock();
        self->uam_cv_.notify_all();
    }
    return nullptr;
}

bool DkBackend::CompileDksh(bool vertex, const std::string& glsl, std::vector<uint8_t>& dksh, std::string& log) {
    if (!uam_started_) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        int e = pthread_create(&uam_thread_, &attr, UamMain, this);
        pthread_attr_destroy(&attr);
        if (e) {
            LOGE("dk: the shader compiler's thread did not start (%d)", e);
            return false;
        }
        uam_started_ = true;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::unique_lock lock(uam_m_);
    uam_vertex_ = vertex;
    uam_done_ = false;
    uam_glsl_ = &glsl;
    uam_cv_.notify_all();
    uam_cv_.wait(lock, [this] { return uam_done_; });
    compiles_++;
    compile_ns_ += NsSince(t0);
    log = uam_result_.log;
    if (!uam_result_.ok) return false;
    dksh = std::move(uam_result_.dksh);
    return true;
}

// the DKSH's control section stays with the shader object, its code goes to the code memory
bool DkBackend::LoadShader(const std::vector<uint8_t>& dksh, DkShader& out) {
    DkshHeader h;
    if (dksh.size() < sizeof(h)) return false;
    std::memcpy(&h, dksh.data(), sizeof(h));
    if (h.magic != kDkshMagic || uint64_t(h.control_sz) + h.code_sz > dksh.size()) return false;
    uint32_t at = AlignUp(code_used_, DK_SHADER_CODE_ALIGNMENT);
    if (at + h.code_sz > kCodeSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
        LOG_FIRST(4, "dk: shader code memory full");
        return false;
    }
    std::memcpy(code_cpu_ + at, dksh.data() + h.control_sz, h.code_sz);
    code_used_ = at + h.code_sz;
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, code_mem_, at);
    m.control = dksh.data();
    m.programId = 0;
    dkShaderInitialize(&out, &m);
    code_dirty_ = true;
    return dkShaderIsValid(&out);
}

// shadercache/<hash>_dk<version>.bin: "MPDK", the pairs (4 bytes each), the DKSH
std::filesystem::path DkCachePath(uint64_t hash) {
    char name[48];
    std::snprintf(name, sizeof(name), "%016llx_dk%u.bin", (unsigned long long)hash, kDkshCacheVersion);
    return app::ExeDir() / "shadercache" / name;
}

bool DkBackend::LoadCached(uint64_t hash, std::vector<glshader::Pair>& pairs, std::vector<uint8_t>& dksh) {
    FILE* f = std::fopen(DkCachePath(hash).string().c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> all;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 8 && n < (16 << 20)) {
        all.resize(size_t(n));
        if (std::fread(all.data(), 1, all.size(), f) != all.size()) all.clear();
    }
    std::fclose(f);
    if (all.size() < 8 || std::memcmp(all.data(), "MPDK", 4)) return false;
    uint32_t np;
    std::memcpy(&np, all.data() + 4, 4);
    size_t at = 8 + size_t(np) * 4;
    if (np > 64 || at > all.size()) return false;
    pairs.resize(np);
    for (uint32_t i = 0; i < np; i++) {
        const uint8_t* p = all.data() + 8 + i * 4;
        pairs[i].unit = p[0];
        pairs[i].tex = p[1];
        pairs[i].sampler = p[2];
        pairs[i].shadow = p[3];
    }
    dksh.assign(all.begin() + long(at), all.end());
    cache_loads_++;
    return true;
}

void DkBackend::StoreCached(uint64_t hash, const std::vector<glshader::Pair>& pairs, const std::vector<uint8_t>& dksh) {
    std::vector<uint8_t> all(8 + pairs.size() * 4);
    std::memcpy(all.data(), "MPDK", 4);
    uint32_t np = uint32_t(pairs.size());
    std::memcpy(all.data() + 4, &np, 4);
    for (size_t i = 0; i < pairs.size(); i++) {
        uint8_t* p = all.data() + 8 + i * 4;
        p[0] = pairs[i].unit;
        p[1] = pairs[i].tex;
        p[2] = pairs[i].sampler;
        p[3] = pairs[i].shadow;
    }
    all.insert(all.end(), dksh.begin(), dksh.end());
    std::filesystem::path path = DkCachePath(hash), tmp = path;
    tmp += ".tmp";
    if (FILE* f = std::fopen(tmp.string().c_str(), "wb")) {
        bool ok = std::fwrite(all.data(), 1, all.size(), f) == all.size();
        std::fclose(f);
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, path, ec);
    }
}

Module* DkBackend::EnsureModule(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    bool vertex = stage == vkshader::Stage::Vertex;
    uint64_t seed = Hash(entry, std::strlen(entry), vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = modules_.find(hash); it != modules_.end()) return it->second.ok ? &it->second : nullptr;
    Module& m = modules_[hash];
    std::vector<uint8_t> dksh;
    if (!LoadCached(hash, m.pairs, dksh)) {
        std::vector<uint32_t> spirv;
        std::string glsl, log;
        // each stage's textures from unit 0: deko3d binds them per stage
        if (!vkshader::Compile(src, entry, stage, hash, spirv) || !glshader::ToGlsl(spirv, vertex, 0, glsl, m.pairs))
            return nullptr;
        if (!CompileDksh(vertex, glsl, dksh, log)) {
            LOGE("dk: %s %s does not compile: %s", vertex ? "vertex" : "pixel", entry, log.c_str());
            LOGE("%s", glsl.c_str());
            return nullptr;
        }
        if (!log.empty()) LOG_FIRST(16, "dk: %s %s compiled with notes: %s", vertex ? "vertex" : "pixel", entry, log.c_str());
        StoreCached(hash, m.pairs, dksh);
    }
    if (!LoadShader(dksh, m.shader)) {
        LOGE("dk: %s shader %016llx did not load", vertex ? "vertex" : "pixel", (unsigned long long)hash);
        return nullptr;
    }
    m.ok = true;
    return &m;
}

Program* DkBackend::EnsureProgram(uint64_t vh, Module* vs, uint64_t ph, Module* ps) {
    uint64_t key = Hash(&ph, sizeof(ph), vh);
    if (auto it = programs_.find(key); it != programs_.end()) return &it->second;
    Program& p = programs_[key];
    p.vs = vs;
    p.ps = ps;
    return &p;
}

Program* DkBackend::FullScreen(const char* hlsl, const char* ps_entry) {
    uint64_t hv, hp;
    Module* vs = EnsureModule(hlsl, "vs", vkshader::Stage::Vertex, hv);
    Module* ps = EnsureModule(hlsl, ps_entry, vkshader::Stage::Pixel, hp);
    return vs && ps ? EnsureProgram(hv, vs, hp, ps) : nullptr;
}

void DkBackend::BindProgram(const Program& p) {
    if (code_dirty_) {
        dkCmdBufBarrier(cmd_, DkBarrier_None, DkInvalidateFlags_Shader);
        code_dirty_ = false;
    }
    if (bound_prog_ == &p) {
        skipped_++;
        return;
    }
    const DkShader* shaders[2] = {&p.vs->shader, &p.ps->shader};
    dkCmdBufBindShaders(cmd_, DkStageFlag_GraphicsMask, shaders, 2);
    bound_prog_ = &p;
    calls_++;
}

void DkBackend::BindTextureUnit(DkStage stage, uint32_t unit, Img* img, uint32_t sampler) {
    if (unit >= 32) return;
    int si = stage == DkStage_Vertex ? 0 : 1;
    DkResHandle h = dkMakeTextureHandle(img ? img->desc : 0, sampler);
    if (tex_valid_[si][unit] && bound_tex_[si][unit] == h) {
        skipped_++;
        return;
    }
    dkCmdBufBindTexture(cmd_, stage, unit, h);
    bound_tex_[si][unit] = h;
    tex_valid_[si][unit] = true;
    calls_++;
}

// a full screen pass's or the panel's textures: t registers to the images, s registers to the samplers
void DkBackend::BindFixedTextures(const Program& p, Img* const tex[4], const uint32_t samplers[4]) {
    for (int stage = 0; stage < 2; stage++) {
        const Module* m = stage ? p.ps : p.vs;
        for (const glshader::Pair& pr : m->pairs) {
            Img* t = pr.tex < 4 && tex[pr.tex] ? tex[pr.tex] : dummy_[0];
            BeforeSample(t);
            BindTextureUnit(stage ? DkStage_Fragment : DkStage_Vertex, pr.unit, t,
                            pr.sampler < 4 ? samplers[pr.sampler] : FixedSampler(false));
        }
    }
}

void DkBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    // the draw before's shader, most often the same (two map lookups saved)
    if (last_vs_ && sh.vkey == last_vkey_) {
        sh.vh = last_vh_;
        sh.vs = last_vs_;
        return;
    }
    EnsureVertexShaderSlow(s, a, sh);
    if (sh.vs) {
        last_vkey_ = sh.vkey;
        last_vh_ = sh.vh;
        last_vs_ = sh.vs;
    }
}

void DkBackend::EnsureVertexShaderSlow(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        auto m = modules_.find(sh.vh);
        sh.vs = m != modules_.end() && m->second.ok ? &m->second : nullptr;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    const char* entry = "main";
    if (a.topology == Topology::Points) {
        std::string points = PointsHlsl(src);
        if (!points.empty()) {
            src = std::move(points);
            entry = "main_points";
        }
    }
    sh.vs = EnsureModule(src, entry, vkshader::Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

void DkBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    if (last_ps_ && sh.pkey == last_pkey_) {
        sh.ph = last_ph_;
        sh.ps = last_ps_;
        return;
    }
    EnsurePixelShaderSlow(pk, sh);
    if (sh.ps) {
        last_pkey_ = sh.pkey;
        last_ph_ = sh.ph;
        last_ps_ = sh.ps;
    }
}

void DkBackend::EnsurePixelShaderSlow(const PixelKey& pk, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt); it != shader_src_.end()) {
        sh.ph = it->second;
        auto m = modules_.find(sh.ph);
        sh.ps = m != modules_.end() && m->second.ok ? &m->second : nullptr;
        return;
    }
    sh.ps = EnsureModule(PixelHlsl(pk), "main", vkshader::Stage::Pixel, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

// ---- state

void DkBackend::BindStates(const DkStates& st) {
    if (st_valid_ && !std::memcmp(&st, &st_, sizeof(st))) {
        skipped_++;
        return;
    }
    // each part on its own: a blend change does not send the depth state again
    if (!st_valid_ || std::memcmp(&st.rs, &st_.rs, sizeof(st.rs))) dkCmdBufBindRasterizerState(cmd_, &st.rs);
    if (!st_valid_ || std::memcmp(&st.cs, &st_.cs, sizeof(st.cs))) dkCmdBufBindColorState(cmd_, &st.cs);
    if (!st_valid_ || std::memcmp(&st.cw, &st_.cw, sizeof(st.cw))) dkCmdBufBindColorWriteState(cmd_, &st.cw);
    if (!st_valid_ || std::memcmp(&st.bs, &st_.bs, sizeof(st.bs))) dkCmdBufBindBlendState(cmd_, 0, &st.bs);
    if (!st_valid_ || std::memcmp(&st.ds, &st_.ds, sizeof(st.ds))) dkCmdBufBindDepthStencilState(cmd_, &st.ds);
    if (!st_valid_ || st.bias_const != st_.bias_const || st.bias_slope != st_.bias_slope)
        dkCmdBufSetDepthBias(cmd_, st.bias_const, 0.0f, st.bias_slope);
    st_ = st;
    st_valid_ = true;
    calls_++;
}

void DkBackend::ApplyState(const PsoKey& k) {
    DkStates st;
    std::memset(&st, 0, sizeof(st));
    dkRasterizerStateDefaults(&st.rs);
    dkColorStateDefaults(&st.cs);
    dkColorWriteStateDefaults(&st.cw);
    dkBlendStateDefaults(&st.bs);
    dkDepthStencilStateDefaults(&st.ds);
    if (k.blend) {
        dkColorStateSetBlendEnable(&st.cs, 0, true);
        dkBlendStateSetFactors(&st.bs, BlendFactor(k.src, false), BlendFactor(k.dst, false), BlendFactor(k.src, true),
                               BlendFactor(k.dst, true));
        dkBlendStateSetOps(&st.bs, BlendOperation(k.op), BlendOperation(k.op));
    }
    dkColorWriteStateSetMask(&st.cw, 0, k.wmask & 0xF);
    st.ds.depthTestEnable = k.has_ds && k.zen;
    st.ds.depthWriteEnable = k.has_ds && k.zen && k.zwrite;
    st.ds.depthCompareOp = k.has_ds && k.zen ? Cmp(k.zfunc) : DkCompareOp_Always;
    st.ds.stencilTestEnable = k.has_ds && k.sten;
    if (k.has_ds && k.sten) {
        st.ds.stencilFrontFailOp = st.ds.stencilBackFailOp = StencilOperation(k.sfail);
        st.ds.stencilFrontDepthFailOp = st.ds.stencilBackDepthFailOp = StencilOperation(k.szfail);
        st.ds.stencilFrontPassOp = st.ds.stencilBackPassOp = StencilOperation(k.spass);
        st.ds.stencilFrontCompareOp = st.ds.stencilBackCompareOp = Cmp(k.sfunc);
    }
    st.rs.cullMode = k.cull ? DkFace_Back : DkFace_None;
    // the GL renderer's front face, with its clip origin (SetViewport). dk_flags.txt "faceswap" turns it, for checking
    bool ccw = k.ccw != 0;
    if (face_swap_) ccw = !ccw;
    st.rs.frontFace = ccw ? DkFrontFace_CCW : DkFrontFace_CW;
    // a flat shaded triangle takes its first vertex's colour, as on the console
    st.rs.provokingVertex = DkProvokingVertex_First;
    constexpr uint32_t kFillWireframe = 0x1B01;
    st.rs.polygonModeFront = st.rs.polygonModeBack = k.fill == kFillWireframe ? DkPolygonMode_Line : DkPolygonMode_Fill;
    if (k.zbias != 0 || k.zslope != 0) {
        st.rs.depthBiasEnableMask = DkPolygonFlag_Fill | DkPolygonFlag_Line;
        st.bias_const = float(k.zbias);  // deko3d doubles it as nouveau does GL's units: the GL renderer's offset
        st.bias_slope = k.zslope;
    }
    BindStates(st);
}

// a full screen pass: no blend (or premultiplied over), no tests, every channel; depth_write: a depth only pass
void DkBackend::PlainState(bool over, bool depth_write) {
    DkStates st;
    std::memset(&st, 0, sizeof(st));
    dkRasterizerStateDefaults(&st.rs);
    dkColorStateDefaults(&st.cs);
    dkColorWriteStateDefaults(&st.cw);
    dkBlendStateDefaults(&st.bs);
    dkDepthStencilStateDefaults(&st.ds);
    if (over) {
        dkColorStateSetBlendEnable(&st.cs, 0, true);
        dkBlendStateSetFactors(&st.bs, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha, DkBlendFactor_Zero, DkBlendFactor_One);
        dkBlendStateSetOps(&st.bs, DkBlendOp_Add, DkBlendOp_Add);
    }
    dkColorWriteStateSetMask(&st.cw, 0, DkColorMask_RGBA);
    st.ds.depthTestEnable = depth_write;
    st.ds.depthWriteEnable = depth_write;
    st.ds.depthCompareOp = DkCompareOp_Always;
    st.ds.stencilTestEnable = false;
    st.rs.cullMode = DkFace_None;
    st.rs.provokingVertex = DkProvokingVertex_First;
    BindStates(st);
}

void DkBackend::BindTargets(Img* color, Img* depth) {
    BeforeRender(color);
    BeforeRender(depth);
    if (targets_valid_ && bound_color_ == color && bound_depth_ == depth) {
        skipped_++;
        return;
    }
    const DkImageView* colors[1] = {color ? &color->view : nullptr};
    dkCmdBufBindRenderTargets(cmd_, colors, color ? 1 : 0, depth ? &depth->view : nullptr);
    bound_color_ = color;
    bound_depth_ = depth;
    targets_valid_ = true;
    calls_++;
}

// in the D3D convention, y down from the top: deko3d's DkDeviceFlags_OriginUpperLeft is GL's glClipControl(
// GL_UPPER_LEFT), the GL renderer's setting (clip space +1 at row 0), so the viewport is GL's, positive. (vulkan.cpp's
// negative height turned the picture upside down here, which the present's blit turned back, and on the console it
// emptied the GPU's viewport clip rectangle, which deko3d derives from it: every draw clipped away, run dk1.)
void DkBackend::SetViewport(float x, float y, float w, float h) {
    if (vp_[0] == x && vp_[1] == y && vp_[2] == w && vp_[3] == h) {
        skipped_++;
        return;
    }
    DkViewport v = {x, y, w, h, 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd_, 0, &v, 1);
    vp_[0] = x, vp_[1] = y, vp_[2] = w, vp_[3] = h;
    calls_++;
}

void DkBackend::SetScissor(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!scissor_) x = y = 0, w = h = 16384;
    if (sc_[0] == x && sc_[1] == y && sc_[2] == w && sc_[3] == h) {
        skipped_++;
        return;
    }
    DkScissor r = {uint32_t(std::max(x, 0)), uint32_t(std::max(y, 0)), uint32_t(std::max(w, 0)), uint32_t(std::max(h, 0))};
    dkCmdBufSetScissors(cmd_, 0, &r, 1);
    sc_[0] = x, sc_[1] = y, sc_[2] = w, sc_[3] = h;
    calls_++;
}

// b0 / b1 for both stages: the vertex shader reads both, the pixel shader b1
void DkBackend::BindUniforms(uint32_t index, uint32_t offset, uint32_t size) {
    size = AlignUp(size, 16);
    if (ubo_bound_[index] == offset && ubo_bound_size_[index] == size) {
        skipped_++;
        return;
    }
    DkGpuAddr a = ubo_.gpu + offset;
    dkCmdBufBindUniformBuffer(cmd_, DkStage_Vertex, index, a, size);
    dkCmdBufBindUniformBuffer(cmd_, DkStage_Fragment, index, a, size);
    ubo_bound_[index] = offset;
    ubo_bound_size_[index] = size;
    calls_++;
}

void DkBackend::BindAttribs(const DkVtxAttribState* attribs, uint32_t n) {
    if (attribs_n_ == n && !std::memcmp(attribs, attribs_, sizeof(DkVtxAttribState) * n)) {
        skipped_++;
        return;
    }
    dkCmdBufBindVtxAttribState(cmd_, attribs, n);
    std::memcpy(attribs_, attribs, sizeof(DkVtxAttribState) * n);
    attribs_n_ = n;
    calls_++;
}

void DkBackend::BindStrides(const DkVtxBufferState* strides, uint32_t n) {
    if (strides_n_ == n && !std::memcmp(strides, strides_, sizeof(DkVtxBufferState) * n)) {
        skipped_++;
        return;
    }
    dkCmdBufBindVtxBufferState(cmd_, strides, n);
    std::memcpy(strides_, strides, sizeof(DkVtxBufferState) * n);
    strides_n_ = n;
    calls_++;
}

// ---- targets

Target* DkBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth, gw, gh));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void DkBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    Target& t = it->second;
    target_hit_[0].t = target_hit_[1].t = nullptr;
    if (draw_rt_ == &t) draw_rt_ = nullptr;
    if (draw_ds_ == &t) draw_ds_ = nullptr;
    DeleteImage(t.img);
    DeleteImage(t.fb);
    targets_.erase(it);
}

Target* DkBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the last target asked for, of each kind: a draw asks for the same one as the draw before it (two map lookups
    // and the surface's description saved); any release forgets them
    TargetHit& hit = target_hit_[depth ? 1 : 0];
    if (hit.t && hit.s.header == s.header && hit.s.data == s.data && hit.s.format == s.format && hit.s.size == s.size)
        return hit.t;
    Target* t = EnsureTargetSlow(s, depth);
    hit.s = s;
    hit.t = t;
    return t;
}

Target* DkBackend::EnsureTargetSlow(const Surface& s, bool depth) {
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetMapKey(data, depth, gw, gh);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) ReleaseTarget(it);
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * scale_x_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * scale_)));
    t.img = NewImage(DkImageType_2D, depth ? DkImageFormat_Z24S8 : DkImageFormat_BGRA8_Unorm, t.w, t.h, 1, 1,
                     DkImageFlags_UsageRender | (depth ? 0u : uint32_t(DkImageFlags_Usage2DEngine)));
    if (!t.img) return nullptr;
    // a new target starts cleared
    if (depth) BindTargets(nullptr, t.img);
    else BindTargets(t.img, nullptr);
    FlushPendingBarrier();
    SetScissor(0, 0, int32_t(t.w), int32_t(t.h));
    if (depth) {
        dkCmdBufClearDepthStencil(cmd_, true, 1.0f, 0xFF, 0);
    } else {
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0, 0, 0, 0);
    }
    t.img->w_stamp = stamp_;
    t.img->w_copy = false;
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

// src drawn into the rect of dst (a dw x dh colour image) through the blit shader
void DkBackend::Blit(Img* dst, uint32_t dw, uint32_t dh, int32_t x, int32_t y, int32_t w, int32_t h, Img* src,
                     const float uvrect[4], const BlitOptions& o) {
    Program* p = FullScreen(kBlitHlsl, "psmain");
    if (!p || !dst) return;
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    EnsureRoom(ubo_, 512);
    uint32_t at = Put(ubo_, c, sizeof(c), DK_UNIFORM_BUF_ALIGNMENT);
    BindUniforms(1, at, sizeof(c));
    BindTargets(dst, nullptr);
    PlainState(o.over, false);
    BindProgram(*p);
    Img* tex[4] = {src, lut_, nullptr, nullptr};
    const uint32_t smp[4] = {FixedSampler(true), FixedSampler(false), FixedSampler(false), FixedSampler(false)};
    BindFixedTextures(*p, tex, smp);
    BindAttribs(nullptr, 0);
    SetViewport(float(x), float(y), float(w), float(h));
    SetScissor(0, 0, int32_t(dw), int32_t(dh));
    PrepareDraw();
    dkCmdBufDraw(cmd_, DkPrimitive_Triangles, 3, 1, 0, 0);
    dst->w_stamp = stamp_;
    dst->w_copy = false;
}

// pixels into a 2D image's level 0 through the upload ring and the copy engine
void DkBackend::Upload2D(Img* img, const uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h) {
    BeforeCopyTo(img);
    uint32_t at = Put(stream_, px, pitch * h, 256);
    DkCopyBuf src = {stream_.gpu + at, pitch, pitch * h};
    DkImageRect r = {0, 0, 0, w, h, 1};
    dkCmdBufCopyBufferToImage(cmd_, &src, &img->view, &r, 0);
    img->w_stamp = stamp_;
    img->w_copy = true;
}

// CPU pixels of a colour surface into its target, scaled; after a Sync only the changed ones, over the picture
void DkBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!DescribeSurface(s, d) && !Describe(s.header, d)) return;
    if (Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    if (px.empty()) return;
    Img* tex = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, d.width, d.height, 1, 1, 0);
    if (!tex) return;
    Upload2D(tex, px.data(), pitch, d.width, d.height);
    float uv[4] = {0, 0, 1, 1};
    BlitOptions o;
    o.alpha = true;
    o.over = over;
    Blit(t.img, t.w, t.h, 0, 0, int32_t(t.w), int32_t(t.h), tex, uv, o);
    DeleteImage(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// an image's rect into another's, the same format (the 2D engine)
void DkBackend::CopyRect(Img* src, const DkImageRect& sr, Img* dst, const DkImageRect& dr) {
    BeforeCopyFrom(src);
    BeforeCopyTo(dst);
    dkCmdBufCopyImage(cmd_, &src->view, &sr, &dst->view, &dr, 0);
    src->r_stamp = stamp_;
    src->r_copy = true;
    dst->w_stamp = stamp_;
    dst->w_copy = true;
}

// a copy of the target to read while drawing to it
Img* DkBackend::Feedback(Target& t) {
    if (!t.fb)
        t.fb = NewImage(DkImageType_2D, t.depth ? DkImageFormat_Z24S8 : DkImageFormat_BGRA8_Unorm, t.w, t.h, 1, 1,
                        t.depth ? 0u : uint32_t(DkImageFlags_Usage2DEngine));
    if (!t.fb) return nullptr;
    DkImageRect r = {0, 0, 0, t.w, t.h, 1};
    CopyRect(t.img, r, t.fb, r);
    return t.fb;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool DkBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1)
        return false;
    uint32_t rw, rh;
    SurfaceSize(s.rt, rw, rh);
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true, rw, rh));
    auto si = targets_.find(TargetMapKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    Program* p = FullScreen(kDepthHlsl, "psmain");
    if (!p) return false;
    BindTargets(nullptr, dst.img);
    PlainState(false, true);
    BindProgram(*p);
    Img* tex[4] = {src.img, nullptr, nullptr, nullptr};
    const uint32_t smp[4] = {FixedSampler(true), FixedSampler(false), FixedSampler(false), FixedSampler(false)};
    BindFixedTextures(*p, tex, smp);
    BindAttribs(nullptr, 0);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    PrepareDraw();
    dkCmdBufDraw(cmd_, DkPrimitive_Triangles, 3, 1, 0, 0);
    dst.img->w_stamp = stamp_;
    dst.img->w_copy = false;
    dst.drawn = true;
    return true;
}

// the target as a guest sized BGRA image: itself, or shrunk into down (made here, the caller deletes it)
Img* DkBackend::CopySource(Target& t, Img*& down) {
    down = nullptr;
    if (t.depth) return nullptr;
    if (t.w == t.gw && t.h == t.gh) return t.img;
    uint32_t taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
    down = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, t.gw, t.gh, 1, 1, DkImageFlags_UsageRender);
    if (!down) return nullptr;
    float uv[4] = {0, 0, 1, 1};
    BlitOptions o;
    o.alpha = true;
    o.taps = std::max(taps, 1u);
    Blit(down, t.gw, t.gh, 0, 0, int32_t(t.gw), int32_t(t.gh), t.img, uv, o);
    return down;
}

// a colour image's pixels as its memory has them (BGRA for the targets), waiting for the GPU
bool DkBackend::ReadBack(Img* img, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    uint32_t bytes = w * h * 4;
    if (bytes > rb_size_) {
        if (rb_mem_) {
            Submit();
            dkQueueWaitIdle(queue_);
            dkMemBlockDestroy(rb_mem_);
        }
        rb_size_ = AlignUp(std::max(bytes, 4u << 20), DK_MEMBLOCK_ALIGNMENT);
        rb_mem_ = NewBlock(rb_size_, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
        if (!rb_mem_) {
            rb_size_ = 0;
            return false;
        }
    }
    // no line of the buffer dirty in the CPU's cache, to be written back over the GPU's pixels
    dkMemBlockFlushCpuCache(rb_mem_, 0, bytes);
    pending_barrier_ = DkBarrier_Full;
    FlushPendingBarrier();
    DkImageRect r = {0, 0, 0, w, h, 1};
    DkCopyBuf dst = {dkMemBlockGetGpuAddr(rb_mem_), w * 4, bytes};
    dkCmdBufCopyImageToBuffer(cmd_, &img->view, &r, &dst, 0);
    // the copy engine's writes out of the GPU's L2 before the CPU reads them
    dkCmdBufBarrier(cmd_, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    img->r_stamp = stamp_;
    img->r_copy = true;
    dkCmdBufSignalFence(cmd_, &rb_fence_, true);
    Submit();
    dkFenceWait(&rb_fence_, -1);
    dkMemBlockFlushCpuCache(rb_mem_, 0, bytes);
    const uint8_t* p = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(rb_mem_));
    out.assign(p, p + bytes);
    return true;
}

// ---- draws

Img* DkBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                  DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    Target* rt = draw_rt_;
    float k = float(t.w) / (t.gw * stages.k[stage]);
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    if (feedback) return Feedback(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    return t.img;
}

bool DkBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    DeleteImage(tx.img);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    tx.decode = bc && d.depth > 1 && !d.cube;
    DkImageFormat fmt;
    switch (fi.host) {
        case Host::BC1: fmt = DkImageFormat_RGBA_BC1; break;
        case Host::BC2: fmt = DkImageFormat_RGBA_BC2; break;
        case Host::BC3: fmt = DkImageFormat_RGBA_BC3; break;
        case Host::R32F: fmt = DkImageFormat_R32_Float; tx.r32f = true; break;
        default: fmt = DkImageFormat_BGRA8_Unorm; break;
    }
    if (tx.decode) fmt = DkImageFormat_BGRA8_Unorm;
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    DkImageType type = d.cube ? DkImageType_Cubemap : d.depth > 1 ? DkImageType_3D : DkImageType_2D;
    tx.w = w;
    tx.h = h;
    tx.depth = d.cube ? 1 : d.depth;
    tx.img = NewImage(type, fmt, w, h, tx.depth, tx.levels, 0);
    return tx.img != nullptr;
}

void DkBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                              const uint32_t* colours) {
    BeforeCopyTo(tx.img);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t>& px = up_px_;
    std::vector<uint8_t>& decoded = up_decoded_;
    std::vector<uint8_t>& packed = up_packed_;
    bool bc = fi.compressed && !tx.decode;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            // DecodeLevel writes every byte of a 2D level; a volume's linear, depth and YUY2 paths fill only the
            // first slice, so a volume starts from zeros as a new buffer did
            if (d.depth > 1) px.clear();
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t mw = std::max(tx.w >> l, 1u), mh = std::max(tx.h >> l, 1u), md = std::max(tx.depth >> l, 1u);
            const std::vector<uint8_t>* src = &px;
            if (tx.decode) {
                DecodeDxt(fi.host, px, pitch, mw, mh, md, decoded);
                pitch = mw * 4;
                src = &decoded;
            }
            // rows packed: block rows for DXT, texel rows else
            uint32_t row = bc ? std::max(1u, (mw + 3) / 4) * block : mw * 4;
            uint32_t rows = bc ? std::max(1u, (mh + 3) / 4) : mh;
            size_t src_rows = pitch ? src->size() / pitch / md : 0;
            const size_t bytes = size_t(row) * rows * md;
            const uint8_t* data;
            if (pitch == row && src_rows == rows) {
                data = src->data();
            } else {
                packed.assign(bytes, 0);
                for (uint32_t z = 0; z < md; z++)
                    for (uint32_t y = 0; y < rows && y < src_rows; y++)
                        std::memcpy(&packed[(size_t(z) * rows + y) * row], &(*src)[(size_t(z) * src_rows + y) * pitch],
                                    std::min(pitch, row));
                data = packed.data();
            }
            uint32_t at = Put(stream_, data, uint32_t(bytes), 256);
            DkCopyBuf cb = {stream_.gpu + at, row, row * rows};
            DkImageView v = tx.img->view;
            v.mipLevelOffset = uint8_t(l);
            v.mipLevelCount = 1;
            if (d.cube) {
                v.layerOffset = uint16_t(f);
                v.layerCount = 1;
            }
            DkImageRect r = {0, 0, 0, mw, mh, md};
            dkCmdBufCopyBufferToImage(cmd_, &cb, &v, &r, 0);
        }
    tx.img->w_stamp = stamp_;
    tx.img->w_copy = true;
}

Img* DkBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                 DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& t = textures_[key];
    bool same = SameTexture(t, d);
    if (!same && !CreateTexture(t, d, fi)) {
        textures_.erase(key);
        return nullptr;
    }
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        bool palette_same = t.palette == palette.hash;
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        uint64_t epoch = DirtyEpoch(d.data);
        // a lock (the epoch) or a new palette re-uploads at once; the memory signature, for a write without a lock,
        // is taken on every 4th frame of a texture (staggered by its address). It was 4.5% of the title's thread at
        // level 27 (console run dk4) and saw no change a lock had not already shown in a run through the films,
        // menus, novel, chapter 1 and level 27 (run dk5).
        bool sig_due = !same || epoch != t.epoch || !palette_same || ((presents_ + (d.data >> 8)) & 3) == 0;
        uint64_t sig = sig_due ? MemSig(d.data, TotalSize(d)) ^ epoch ^ palette.hash : t.sig;
        if (sig != t.sig || !same) {
            // a change only the signature saw: the game wrote the texture without a lock (counted, for whether the
            // signature's cost is needed in this game)
            if (same && t.sig && epoch == t.epoch && palette_same) sig_only_++;
            t.sig = sig;
            t.epoch = epoch;
            UploadTexture(t, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette.va ? palette.colours : nullptr);
        }
    }
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.r32f[stage] = t.r32f;
    stages.levels[stage] = t.levels;
    return t.img;
}

Img* DkBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool DkBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) stages.tex[i] = BindTexture(s, i, stages);
    // a stage without a texture reads a black one of its kind
    for (int i = 0; i < 4; i++) {
        if (stages.tex[i]) continue;
        uint8_t kind = stages.info[i].kind;
        int k = kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0;
        stages.tex[i] = dummy_[k];
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    return true;
}

void DkBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (draw_ds_ ? float(draw_ds_->w) / draw_ds_->gw : 1.0f);
    }
    // vulkan.cpp: a blended pass over geometry already drawn moves a little towards the camera
    constexpr uint32_t kEqual = 0x202, kLequal = 0x203;
    if (k.zen && !k.zwrite && (k.zfunc == kEqual || k.zfunc == kLequal) && SimpleState(s, kMBlendEnable) &&
        a.topology == Topology::Triangles) {
        k.zfunc = kLequal;
        k.zbias -= 64;
        k.zslope -= 1.0f;
    }
}

void DkBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    bool assembled;
    {
        CpuTimer part{asm_ns_};
        assembled = Assemble(s, prim, start, count, indices, a);
    }
    if (!assembled) return;
    // how the input assembler took the draw's vertices (for the log): converted on the CPU, gathered (sparse indices:
    // copied vertex by vertex), or the guest's own read in place
    if (!a.vertices.empty()) {
        asm_converted_++;
        asm_converted_bytes_ += a.vertices.size() * 4;
    } else {
        bool gathered = false;
        for (uint32_t i = 0; i < a.buffer_count; i++)
            if (!a.gathered[i].empty() && a.buffers[i].src == a.gathered[i].data()) {
                gathered = true;
                asm_gathered_bytes_ += a.buffers[i].bytes;
            } else {
                asm_dense_bytes_ += a.buffers[i].bytes;
            }
        (gathered ? asm_gathered_ : asm_dense_)++;
    }

    // the targets, CPU writes to them first
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) {
        // a colour surface without memory is a depth only pass's stand-in (a shadow buffer): the depth is the target
        if (!s.rt.data) rt = nullptr;
        else ds = nullptr;
    }
    if (!rt && !ds) return;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    draw_rt_ = rt;
    draw_ds_ = ds;

    DrawShaders sh = ClassifyShaders(s, a);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    bool bound;
    {
        CpuTimer part{tex_ns_};
        bound = BindStageTextures(s, stages);
    }
    if (!bound || (!draw_rt_ && !draw_ds_)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs || !sh.ps) return;
    Program* prog = EnsureProgram(sh.vh, sh.vs, sh.ph, sh.ps);

    PsoKey k;
    std::memset(&k, 0, sizeof(k));
    KeyBlend(s, k);
    k.has_ds = draw_ds_ != nullptr;
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
    if (!draw_rt_) k.wmask = 0;

    GuestExtent size;
    SurfaceSize(draw_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    // every upload of this draw in one segment of each ring
    uint32_t need = 64 + (a.index_count ? a.index_count * 4 + 16 : 0);
    for (uint32_t i = 0; i < a.buffer_count; i++) need += a.buffers[i].bytes + 32;
    EnsureRoom(stream_, need);
    EnsureRoom(ubo_, 2 * (kVsConstants + uint32_t(sizeof(c)) + 2 * DK_UNIFORM_BUF_ALIGNMENT));
    uint32_t vc, dc;
    // nothing wrote the constants since the last upload (State::vsc_gen): no 3 KB comparison; written with the same
    // values: compared
    if (s.vsc_gen_tracked && last_vc_gen_ == ubo_.gen && last_vsc_gen_ == s.vsc_gen) {
        vc = last_vc_off_;
        skipped_++;
    } else if (last_vc_gen_ == ubo_.gen && !std::memcmp(s.vsc.data(), last_vc_.data(), kVsConstants)) {
        vc = last_vc_off_;
        skipped_++;
        last_vsc_gen_ = s.vsc_gen;
    } else {
        vc = Put(ubo_, s.vsc.data(), kVsConstants, DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(last_vc_.data(), s.vsc.data(), kVsConstants);
        last_vc_off_ = vc;
        last_vc_gen_ = ubo_.gen;
        last_vsc_gen_ = s.vsc_gen;
    }
    if (last_dc_gen_ == ubo_.gen && last_dc_valid_ && !std::memcmp(&c, last_dc_, sizeof(c))) {
        dc = last_dc_off_;
        skipped_++;
    } else {
        dc = Put(ubo_, &c, uint32_t(sizeof(c)), DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(last_dc_, &c, sizeof(c));
        last_dc_valid_ = true;
        last_dc_off_ = dc;
        last_dc_gen_ = ubo_.gen;
    }
    uint32_t voffs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        voffs[i] = Put(stream_, b.src, b.bytes, 16);
    }
    uint32_t ioff = 0;
    if (a.index_count) ioff = Put(stream_, a.index_src, a.index_count * (a.index16 ? 2 : 4), 4);
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t samplers[4];
    for (int i = 0; i < 4; i++) {
        bool compare = false;
        for (const glshader::Pair& p : sh.ps->pairs)
            if (p.sampler == i && p.shadow) compare = true;
        samplers[i] = StageSampler(s, i, stages, clamp, compare);
    }

    // the rendering
    CpuTimer gl_part{gl_ns_};
    BindTargets(draw_rt_ ? draw_rt_->img : nullptr, draw_ds_ ? draw_ds_->img : nullptr);
    if (draw_rt_) draw_rt_->drawn = true;
    if (draw_ds_) draw_ds_->drawn = true;
    ApplyState(k);
    if (k.has_ds && k.sten) {
        uint32_t sf[3] = {k.swmask, SimpleState(s, kMStencilRef) & 0xFF, k.smask};
        if (std::memcmp(sf, stencil_, sizeof(sf))) {
            dkCmdBufSetStencil(cmd_, DkFace_FrontAndBack, uint8_t(sf[0]), uint8_t(sf[1]), uint8_t(sf[2]));
            std::memcpy(stencil_, sf, sizeof(sf));
            calls_++;
        }
    }
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    if (std::memcmp(bf, blend_color_, sizeof(bf))) {
        dkCmdBufSetBlendConst(cmd_, bf[0], bf[1], bf[2], bf[3]);
        std::memcpy(blend_color_, bf, sizeof(bf));
        calls_++;
    }
    BindProgram(*prog);
    BindUniforms(0, vc, kVsConstants);
    BindUniforms(1, dc, uint32_t(sizeof(c)));
    // a sampler slot a pair names with a comparison takes the comparison sampler, the others the plain one
    for (int stage = 0; stage < 2; stage++) {
        const Module* m = stage ? sh.ps : sh.vs;
        for (const glshader::Pair& p : m->pairs) {
            Img* t = p.tex < 4 ? stages.tex[p.tex] : dummy_[0];
            if (!t) t = dummy_[0];
            BeforeSample(t);
            uint32_t smp = FixedSampler(false);
            if (p.sampler < 4) smp = p.shadow ? samplers[p.sampler] : StageSampler(s, p.sampler, stages, clamp, false);
            BindTextureUnit(stage ? DkStage_Fragment : DkStage_Vertex, p.unit, t, smp);
        }
    }
    Target& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    SetViewport(0, 0, W, H);
    float sc = W / size.w, scy = H / size.h;  // render_wide: the two differ
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * scy);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * scy);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * scy));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * scy));
    }
    x0 = std::clamp(x0, 0, int32_t(W));
    y0 = std::clamp(y0, 0, int32_t(H));
    x1 = std::clamp(x1, 0, int32_t(W));
    y1 = std::clamp(y1, 0, int32_t(H));
    SetScissor(x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0));
    DkVtxAttribState attribs[16];
    std::memset(attribs, 0, sizeof(attribs));
    uint32_t na = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            AttribFormat f = VertexFormat(e.format);
            attribs[r].bufferId = e.buffer;
            attribs[r].offset = e.offset;
            attribs[r].size = f.size;
            attribs[r].type = f.type;
            attribs[r].isBgra = f.bgra;
            na = r + 1;
        } else {
            // a register the draw does not supply (and its shader does not read)
            attribs[r].isFixed = 1;
            attribs[r].size = DkVtxAttribSize_1x32;
            attribs[r].type = DkVtxAttribType_Float;
        }
    }
    BindAttribs(attribs, na);
    DkVtxBufferState strides[16];
    DkBufExtents bufs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        strides[i] = {a.buffers[i].stride, 0};
        bufs[i] = {stream_.gpu + voffs[i], stream_.size - voffs[i]};
    }
    BindStrides(strides, a.buffer_count);
    if (a.buffer_count) dkCmdBufBindVtxBuffers(cmd_, 0, bufs, a.buffer_count);
    PrepareDraw();
    if (draw_rt_) {
        draw_rt_->img->w_stamp = stamp_;
        draw_rt_->img->w_copy = false;
    }
    if (draw_ds_) {
        draw_ds_->img->w_stamp = stamp_;
        draw_ds_->img->w_copy = false;
    }
    Topology topo = a.topology;
    DkPrimitive mode = topo == Topology::Points ? DkPrimitive_Points
                       : topo == Topology::Lines ? (a.strip ? DkPrimitive_LineStrip : DkPrimitive_Lines)
                                                 : (a.strip ? DkPrimitive_TriangleStrip : DkPrimitive_Triangles);
    if (a.index_count) {
        dkCmdBufBindIdxBuffer(cmd_, a.index16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, stream_.gpu + ioff);
        dkCmdBufDrawIndexed(cmd_, mode, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        dkCmdBufDraw(cmd_, mode, a.draw_count, 1, 0, 0);
    }
    draws_++;
}

void DkBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                            uint32_t color, float z, uint32_t stencil) {
    struct R {
        int32_t x, y, w, h;
    };
    std::vector<R> rs;
    float sc = float(t.w) / t.gw, scy = float(t.h) / t.gh;
    auto add = [&](int x0, int y0, int x1, int y1) {
        int32_t l = std::clamp(int32_t(x0 * sc), 0, int32_t(t.w)), tp = std::clamp(int32_t(y0 * scy), 0, int32_t(t.h));
        int32_t r = std::clamp(int32_t(x1 * sc), 0, int32_t(t.w)), b = std::clamp(int32_t(y1 * scy), 0, int32_t(t.h));
        if (r > l && b > tp) rs.push_back({l, tp, r - l, b - tp});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        const auto& v = s.viewport;
        add(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
    }
    t.drawn = true;
    if (rs.empty()) return;
    if (t.depth) {
        bool zc = flags & kClearZ, stc = flags & kClearStencil;
        if (!zc && !stc) return;
        BindTargets(nullptr, t.img);
        FlushPendingBarrier();
        float zv = std::clamp(z, 0.0f, 1.0f);
        for (const R& r : rs) {
            SetScissor(r.x, r.y, r.w, r.h);
            dkCmdBufClearDepthStencil(cmd_, zc, zv, stc ? 0xFF : 0, uint8_t(stencil & 0xFF));
        }
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        BindTargets(t.img, nullptr);
        FlushPendingBarrier();
        float c[4];
        Color(color, c);
        for (const R& r : rs) {
            SetScissor(r.x, r.y, r.w, r.h);
            dkCmdBufClearColor(cmd_, 0, DkColorMask_RGBA, c);
        }
    }
    t.img->w_stamp = stamp_;
    t.img->w_copy = false;
}

void DkBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
}

void DkBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "dk: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    src_t = FindTarget(src, false);
    if (!dst_t || !src_t || src_t == dst_t) return;
    float dst_scale = float(dst_t->w) / dst_t->gw, dst_scale_y = float(dst_t->h) / dst_t->gh;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h), dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = ScaleUnsigned(g.left, dst_scale);
            t = ScaleUnsigned(g.top, dst_scale_y);
            r = std::min(ScaleUnsigned(g.right, dst_scale), src_t->w);
            b = std::min(ScaleUnsigned(g.bottom, dst_scale_y), src_t->h);
            dx = l;
            dy = t;
            if (dst_points) {
                int32_t px, py;
                ReadGuestPoint(dst_points, i, px, py);
                dx = ScaleUnsigned(px, dst_scale);
                dy = ScaleUnsigned(py, dst_scale_y);
            }
        }
        if (r <= l || b <= t || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, t + dst_t->h - dy);
        DkImageRect sr = {l, t, 0, r - l, b - t, 1}, dr = {dx, dy, 0, r - l, b - t, 1};
        CopyRect(src_t->img, sr, dst_t->img, dr);
    }
    dst_t->drawn = true;
}

// ---- read-backs

void DkBackend::Sync(uint32_t data, uint32_t) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    CpuTimer sync_timer{sync_ns_};
    syncs_++;
    Img* down = nullptr;
    Img* src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(src, t.gw, t.gh, px);
    if (down) DeleteImage(down);
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// ---- the options panel: the hosts made the ImGui context (main.cpp); here its font atlas and its draw lists

bool DkBackend::InitUi() {
    std::lock_guard lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "deko3d";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!px || w <= 0 || h <= 0) return false;
    ui_font_ = NewImage(DkImageType_2D, DkImageFormat_RGBA8_Unorm, uint32_t(w), uint32_t(h), 1, 1, 0);
    if (!ui_font_) return false;
    Upload2D(ui_font_, px, uint32_t(w) * 4, uint32_t(w), uint32_t(h));
    io.Fonts->SetTexID(ImTextureID(intptr_t(ui_font_)));
    LOGI("dk: options panel ready (font atlas %dx%d)", w, h);
    return true;
}

void DkBackend::RenderUi(Img* fb) {
    if (!ui_) return;
    std::lock_guard lock(app::UiLock());
    ui::NewFrame(float(out_w_), float(out_h_));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    touch::Draw();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0 || dd->TotalVtxCount == 0) return;
    Program* p = FullScreen(kUiHlsl, "psmain");
    if (!p) return;
    uint32_t need = 64;
    for (const ImDrawList* l : dd->CmdLists)
        need += uint32_t(l->VtxBuffer.Size) * uint32_t(sizeof(ImDrawVert)) + uint32_t(l->IdxBuffer.Size) * 2 + 32;
    EnsureRoom(stream_, need);
    BindTargets(fb, nullptr);
    DkStates st;
    std::memset(&st, 0, sizeof(st));
    dkRasterizerStateDefaults(&st.rs);
    dkColorStateDefaults(&st.cs);
    dkColorWriteStateDefaults(&st.cw);
    dkBlendStateDefaults(&st.bs);
    dkDepthStencilStateDefaults(&st.ds);
    st.rs.cullMode = DkFace_None;
    st.rs.provokingVertex = DkProvokingVertex_First;
    dkColorStateSetBlendEnable(&st.cs, 0, true);
    dkBlendStateSetFactors(&st.bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
    dkColorWriteStateSetMask(&st.cw, 0, DkColorMask_RGBA);
    st.ds.depthTestEnable = st.ds.depthWriteEnable = false;
    BindStates(st);
    BindProgram(*p);
    const float W = float(out_w_), H = float(out_h_);
    float xform[4] = {2.0f / dd->DisplaySize.x, -2.0f / dd->DisplaySize.y, -1.0f - 2.0f * dd->DisplayPos.x / dd->DisplaySize.x,
                      1.0f + 2.0f * dd->DisplayPos.y / dd->DisplaySize.y};
    EnsureRoom(ubo_, 512);
    uint32_t at = Put(ubo_, xform, sizeof(xform), DK_UNIFORM_BUF_ALIGNMENT);
    BindUniforms(1, at, sizeof(xform));
    SetViewport(0, 0, W, H);
    DkVtxAttribState attribs[3];
    std::memset(attribs, 0, sizeof(attribs));
    attribs[0] = {0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0};
    attribs[1] = {0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0};
    attribs[2] = {0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0};
    BindAttribs(attribs, 3);
    DkVtxBufferState stride = {uint32_t(sizeof(ImDrawVert)), 0};
    BindStrides(&stride, 1);
    const uint32_t smp[4] = {FixedSampler(true), FixedSampler(true), FixedSampler(true), FixedSampler(true)};
    for (const ImDrawList* l : dd->CmdLists) {
        uint32_t voff = Put(stream_, l->VtxBuffer.Data, uint32_t(l->VtxBuffer.Size) * uint32_t(sizeof(ImDrawVert)), 16);
        uint32_t ioff = Put(stream_, l->IdxBuffer.Data, uint32_t(l->IdxBuffer.Size) * uint32_t(sizeof(ImDrawIdx)), 4);
        dkCmdBufBindVtxBuffer(cmd_, 0, stream_.gpu + voff, stream_.size - voff);
        dkCmdBufBindIdxBuffer(cmd_, sizeof(ImDrawIdx) == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, stream_.gpu + ioff);
        for (const ImDrawCmd& c : l->CmdBuffer) {
            if (c.UserCallback || !c.ElemCount) continue;
            float x0 = std::max(c.ClipRect.x - dd->DisplayPos.x, 0.0f), y0 = std::max(c.ClipRect.y - dd->DisplayPos.y, 0.0f);
            float x1 = std::min(c.ClipRect.z - dd->DisplayPos.x, W), y1 = std::min(c.ClipRect.w - dd->DisplayPos.y, H);
            if (x1 <= x0 || y1 <= y0) continue;
            SetScissor(int32_t(x0), int32_t(y0), int32_t(x1 - x0), int32_t(y1 - y0));
            Img* tex[4] = {reinterpret_cast<Img*>(intptr_t(c.GetTexID())), nullptr, nullptr, nullptr};
            BindFixedTextures(*p, tex, smp);
            PrepareDraw();
            dkCmdBufDrawIndexed(cmd_, DkPrimitive_Triangles, c.ElemCount, 1, c.IdxOffset, int32_t(c.VtxOffset), 0);
        }
    }
    fb->w_stamp = stamp_;
    fb->w_copy = false;
}

// ---- present

void DkBackend::DrawOverlay(Target& t) {
    if (!overlay_on_) return;
    if (!overlay_ || overlay_->w != overlay_w_ || overlay_->h != overlay_h_) {
        DeleteImage(overlay_);
        overlay_ = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, overlay_w_, overlay_h_, 1, 1, 0);
        overlay_new_ = true;
        if (!overlay_) return;
    }
    if (overlay_new_) {
        Upload2D(overlay_, overlay_px_.data(), overlay_w_ * 4, overlay_w_, overlay_h_);
        overlay_new_ = false;
    }
    float sx = float(t.w) / std::max(t.gw, 1u), sy = float(t.h) / std::max(t.gh, 1u);
    int32_t l = int32_t(overlay_rect_[0] * sx), tp = int32_t(overlay_rect_[1] * sy);
    int32_t r = int32_t(overlay_rect_[2] * sx), b = int32_t(overlay_rect_[3] * sy);
    if (r <= l || b <= tp) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.img, t.w, t.h, l, tp, r - l, b - tp, overlay_, uv, {});
}

void DkBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    uint8_t px[256 * 4];
    bool identity = true;
    lift_ = screen::Gamma();
    auto lift = [this](uint8_t v) {
        return lift_ == 1.0f ? v : uint8_t(std::lround(255.0f * std::pow(v / 255.0f, lift_)));
    };
    for (int i = 0; i < 256; i++) {
        uint8_t* p = px + i * 4;
        p[0] = lift(ramp_[512 + i]);
        p[1] = lift(ramp_[256 + i]);
        p[2] = lift(ramp_[i]);
        p[3] = 255;
        identity &= p[0] == i && p[1] == i && p[2] == i;
    }
    gamma_ = !identity;
    Upload2D(lut_, px, sizeof(px), 256, 1);
}

void DkBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            DeleteImage(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void DkBackend::ApplySettings() {
    float want, want_x;
    WantScale(want, want_x);
    if (std::fabs(want - scale_) <= 1e-4f && std::fabs(want_x - scale_x_) <= 1e-4f) return;
    while (!targets_.empty()) ReleaseTarget(targets_.begin());
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    scale_x_ = want_x;
    LOGI("dk: render scale %.3f x %.3f", scale_x_, scale_);
}

// the output's height over the guest's (render_percent of it); render_wide: horizontally, the share of the output's
// width the frame is shown on over the guest's width (MP2 widescreen: 1280 / 640 = 2 against 720 / 480 = 1.5)
void DkBackend::WantScale(float& y, float& x) {
    y = std::max(out_h_ * (app::Get().render_percent / 100.0f) / guest_h_, 0.1f);
    x = y;
    if (game::Opt().render_wide && guest_h_) {
        float shown = std::min(float(out_w_), out_h_ * game::Aspect());
        x = std::max(y, shown * (app::Get().render_percent / 100.0f) / guest_w_);
    }
}

// as the GL renderer: a "capture" file beside the NRO saves the presented frame to shots/ every N seconds (its text)
bool DkBackend::CaptureDue() {
    static const int every = [] {
        std::error_code ec;
        if (!std::filesystem::exists(app::ExeDir() / "capture", ec)) return 0;
        int s = 15;
        if (FILE* f = std::fopen((app::ExeDir() / "capture").string().c_str(), "rb")) {
            char buf[32] = {};
            size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            std::string text(buf, n);
            if (text.find("off") != std::string::npos) s = 0;
            else if (text.find_first_of("0123456789") != std::string::npos) s = std::atoi(text.c_str() + text.find_first_of("0123456789"));
        }
        if (s > 0) std::filesystem::create_directories(app::ExeDir() / "shots", ec);
        LOGI("dk: capture file: %s", s > 0 ? (std::to_string(s) + " s").c_str() : "off");
        return s;
    }();
    if (every <= 0) return false;
    auto now = std::chrono::steady_clock::now();
    if (shots_ && now - last_shot_ < std::chrono::seconds(every)) return false;
    last_shot_ = now;
    return true;
}

// a 24 bit BMP in shots/; top_down: px's first row is the picture's top
void DkBackend::WriteBmp(const char* name, const std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool top_down) {
    FILE* f = std::fopen((app::ExeDir() / "shots" / name).string().c_str(), "wb");
    if (!f) return;
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hd[54] = {'B', 'M'};
    auto put32 = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, top_down ? uint32_t(-int32_t(h)) : h);
    hd[26] = 1;
    hd[28] = 24;
    std::vector<uint8_t> file(size, 0);
    std::memcpy(file.data(), hd, 54);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* line = file.data() + 54 + size_t(y) * row;
        for (uint32_t x = 0; x < w; x++) std::memcpy(line + x * 3, &px[(size_t(y) * w + x) * 4], 3);
    }
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    LOGI("dk: shot %s", name);
}

// dk_flags.txt "selftest": clears, a blit and a texture's orientation read back (for checking the renderer on an
// emulator or the console): how much of a row each covered, and which colour the top and bottom rows came out
void DkBackend::SelfTest() {
    auto covered = [](const std::vector<uint8_t>& px, uint32_t w, uint32_t row, int ch, uint32_t& last) {
        uint32_t n = 0;
        last = 0;
        for (uint32_t x = 0; x < w; x++)
            if (px[(size_t(row) * w + x) * 4 + ch] > 128) {
                n++;
                last = x + 1;
            }
        return n;
    };
    std::vector<uint8_t> px;
    uint32_t last = 0;
    // 2x2 BGRA: the top row red, the bottom row green
    const uint8_t quad[16] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    Img* tex = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, 2, 2, 1, 1, 0);
    if (!tex) return;
    Upload2D(tex, quad, 8, 2, 2);
    if (ReadBack(tex, 2, 2, px))
        LOGI("dk: selftest the 2x2 texture read back: %02X%02X%02X%02X %02X%02X%02X%02X / %02X%02X%02X%02X %02X%02X%02X%02X "
             "(BGRA; red above, green below)",
             px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7], px[8], px[9], px[10], px[11], px[12], px[13], px[14],
             px[15]);
    const uint32_t sizes[2][2] = {{256, 64}, {960, 720}};
    for (const auto& sz : sizes) {
        uint32_t W = sz[0], H = sz[1];
        Img* t = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, W, H, 1, 1,
                          DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine);
        if (!t) break;
        BindTargets(t, nullptr);
        FlushPendingBarrier();
        SetScissor(0, 0, int32_t(W), int32_t(H));
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0, 0, 1, 1);
        t->w_stamp = stamp_;
        ReadBack(t, W, H, px);
        uint32_t n = covered(px, W, H / 2, 0, last);
        LOGI("dk: selftest %ux%u clear: %u of %u blue in the middle row, last at %u", W, H, n, W, last);
        // a clear inside a scissor: the left half red
        SetScissor(0, 0, int32_t(W / 2), int32_t(H));
        dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 1, 0, 0, 1);
        t->w_stamp = stamp_;
        ReadBack(t, W, H, px);
        n = covered(px, W, H / 2, 2, last);
        LOGI("dk: selftest %ux%u clear in the left half's scissor: %u of %u red in the middle row, last at %u", W, H, n,
             W, last);
        float uv[4] = {0, 0, 1, 1};
        BlitOptions o;
        o.alpha = true;
        Blit(t, W, H, 0, 0, int32_t(W), int32_t(H), tex, uv, o);
        ReadBack(t, W, H, px);
        uint32_t top_red = covered(px, W, 0, 2, last), bottom_green = covered(px, W, H - 1, 1, last);
        n = covered(px, W, H / 2, 0, last);
        auto at = [&](uint32_t x, uint32_t y) {
            const uint8_t* p = &px[(size_t(y) * W + x) * 4];
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        };
        LOGI("dk: selftest %ux%u blit of the 2x2 texture: top row %u of %u red, bottom row %u green, %u blue left in "
             "the middle row; BGRA at the top left %08X, the middle %08X, the bottom left %08X",
             W, H, top_red, W, bottom_green, n, at(0, 0), at(W / 2, H / 2), at(0, H - 1));
        DeleteImage(t);
    }
    DeleteImage(tex);
}

bool DkBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    vsync_ = app::Get().vsync;
    // test switches for emulator and console runs: dk_flags.txt beside the NRO
    {
        std::string flags;
        if (FILE* f = std::fopen((app::ExeDir() / "dk_flags.txt").string().c_str(), "rb")) {
            char buf[256] = {};
            flags.assign(buf, std::fread(buf, 1, sizeof(buf) - 1, f));
            std::fclose(f);
        }
        scissor_ = flags.find("noscissor") == std::string::npos;
        face_swap_ = flags.find("faceswap") != std::string::npos;
        flags_ = flags;
        if (!flags.empty()) LOGI("dk: test flags: %s", flags.c_str());
    }
    if (!CreateDevice()) return false;
    thread_ = std::this_thread::get_id();
    vkshader::Init(app::ExeDir() / "shadercache");
    BeginFrame();
    // the stages without a texture read black ones of their kind
    const uint8_t black[4 * 6] = {};
    dummy_[0] = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, 1, 1, 1, 1, 0);
    dummy_[1] = NewImage(DkImageType_3D, DkImageFormat_BGRA8_Unorm, 1, 1, 1, 1, 0);
    dummy_[2] = NewImage(DkImageType_Cubemap, DkImageFormat_BGRA8_Unorm, 1, 1, 1, 1, 0);
    lut_ = NewImage(DkImageType_2D, DkImageFormat_BGRA8_Unorm, 256, 1, 1, 1, 0);
    if (!dummy_[0] || !dummy_[1] || !dummy_[2] || !lut_) return false;
    Upload2D(dummy_[0], black, 4, 1, 1);
    {
        BeforeCopyTo(dummy_[1]);
        uint32_t at = Put(stream_, black, 4, 256);
        DkCopyBuf cb = {stream_.gpu + at, 4, 4};
        DkImageRect r = {0, 0, 0, 1, 1, 1};
        dkCmdBufCopyBufferToImage(cmd_, &cb, &dummy_[1]->view, &r, 0);
        for (uint16_t f = 0; f < 6; f++) {
            DkImageView v = dummy_[2]->view;
            v.layerOffset = f;
            v.layerCount = 1;
            dkCmdBufCopyBufferToImage(cmd_, &cb, &v, &r, 0);
        }
        dummy_[1]->w_stamp = dummy_[2]->w_stamp = stamp_;
        dummy_[1]->w_copy = dummy_[2]->w_copy = true;
    }
    for (int i = 0; i < 256; i++) ramp_[i] = ramp_[256 + i] = ramp_[512 + i] = uint8_t(i);
    out_w_ = kSurfaceW;
    out_h_ = kSurfaceH;
    WantScale(scale_, scale_x_);
    ok_ = true;
    if (flags_.find("selftest") != std::string::npos) SelfTest();
    ui_ = InitUi();
    LOGI("dk: deko3d renderer: %ux%u guest, %ux%u output, render scale %.3f x %.3f, %s; rings %u MB + %u MB, %u swapchain "
         "images, shaders compiled by uam on the console (kept in shadercache/)",
         width, height, out_w_, out_h_, scale_x_, scale_, vsync_ ? "vsync" : "no vsync", kStreamSize >> 20,
         kUniformSize >> 20, kSwapImages);
    return true;
}

void DkBackend::Present(const Surface& front) {
    if (!Current()) return;
    auto present_t0 = std::chrono::steady_clock::now();
    uint64_t waits0 = acquire_ns_ + frame_wait_ns_;
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) DrawOverlay(*t);
    const bool shot = t && CaptureDue();
    const uint32_t shot_index = shots_;
    if (shot) {
        Img* down = nullptr;
        Img* src = CopySource(*t, down);
        std::vector<uint8_t> px;
        if (src && ReadBack(src, t->gw, t->gh, px)) {
            char name[32];
            std::snprintf(name, sizeof(name), "%04u.bmp", shots_++);
            WriteBmp(name, px, t->gw, t->gh, true);
        }
        if (down) DeleteImage(down);
    }
    if (screen::Gamma() != lift_) ramp_dirty_ = true;
    if (ramp_dirty_) UploadGammaRamp();
    // the game's frame to the GPU now, which works on it while a swapchain image is waited for
    FlushPendingBarrier();
    dkCmdBufReportCounter(cmd_, DkCounter_Timestamp, dkMemBlockGetGpuAddr(ts_mem_) + slot_ * 64 + 16);
    Submit();
    auto acq_t0 = std::chrono::steady_clock::now();
    int slot = dkQueueAcquireImage(queue_, swapchain_);
    acquire_ns_ += NsSince(acq_t0);
    dkCmdBufReportCounter(cmd_, DkCounter_Timestamp, dkMemBlockGetGpuAddr(ts_mem_) + slot_ * 64 + 32);
    Img* fb = &swap_[std::clamp(slot, 0, int(kSwapImages) - 1)];
    // the frame letterboxed to the game's aspect over black
    BindTargets(fb, nullptr);
    FlushPendingBarrier();
    SetScissor(0, 0, int32_t(out_w_), int32_t(out_h_));
    dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    fb->w_stamp = stamp_;
    fb->w_copy = false;
    float aspect = game::Aspect();
    float w = float(out_w_), h = float(out_h_);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    if (t) {
        float uv[4] = {0, 0, 1, 1};
        BlitOptions o;
        o.gamma = gamma_;
        Blit(fb, out_w_, out_h_, int32_t(std::lround((w - vw) / 2)), int32_t(std::lround((h - vh) / 2)),
             int32_t(std::lround(vw)), int32_t(std::lround(vh)), t->img, uv, o);
    }
    RenderUi(fb);
    if (shot) {
        // what the screen shows, beside the guest target's shot: shots/NNNN_screen.bmp (RGBA swapped to BGRA)
        std::vector<uint8_t> px;
        if (ReadBack(fb, out_w_, out_h_, px)) {
            for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
            char name[40];
            std::snprintf(name, sizeof(name), "%04u_screen.bmp", shot_index);
            WriteBmp(name, px, out_w_, out_h_, true);
        }
    }
    // the frame's last command: its timestamp and its slot's fence
    FlushPendingBarrier();
    dkCmdBufReportCounter(cmd_, DkCounter_Timestamp, dkMemBlockGetGpuAddr(ts_mem_) + slot_ * 64 + 48);
    dkCmdBufSignalFence(cmd_, &slots_[slot_].fence, false);
    slots_[slot_].pending = true;
    if (vsync_) {
        // the swap interval follows the frame cap: 2 vblanks a frame at 30, 1 at 60 (app::Pace adds no timer then)
        uint32_t want = app::FrameCapNow() == 30 ? 2 : 1;
        if (want != interval_) {
            dkSwapchainSetSwapInterval(swapchain_, want);
            interval_ = want;
            LOGI("dk: swap interval %u (%u fps)", want, app::FrameCapNow());
        }
    }
    Submit();
    gpu_issue_[slot_] = dkDeviceGetCurrentTimestamp(device_);
    dkQueuePresentImage(queue_, swapchain_, slot);
    presents_++;
    BeginFrame();
    uint64_t waits = acquire_ns_ + frame_wait_ns_ - waits0;
    uint64_t spent = NsSince(present_t0);
    cpu_ns_ += spent > waits ? spent - waits : 0;
    // a frame whose renderer time is long (over 20 ms, the first 40 and then every 64th): its parts
    if (uint64_t f = cpu_ns_ - frame_cpu0_; presents_ > 600 && f > 20000000ull) {
        if (long_logged_ < 40 || long_logged_ % 64 == 0)
            LOGI("dk: long frame %llu: renderer %.1f ms (assembly %.1f, textures %.1f, commands %.1f), %llu KB "
                 "uploaded, %llu draws, %zu textures",
                 (unsigned long long)presents_, double(f) / 1e6, double(asm_ns_ - frame_asm0_) / 1e6,
                 double(tex_ns_ - frame_tex0_) / 1e6, double(gl_ns_ - frame_gl0_) / 1e6,
                 (unsigned long long)((up_bytes_ - frame_up0_) / 1024), (unsigned long long)(draws_ - frame_draws0_),
                 textures_.size());
        long_logged_++;
    }
    fps_n_++;
    {
        auto now = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(now - fps_t_).count();
        if (el >= 0.5) {
            fps_ = double(fps_n_) / el;
            fps_n_ = 0;
            fps_t_ = now;
        }
    }
    if (presents_ % 300 == 1) {
        LOGI("dk: present %llu, %llu draws, %zu textures, %zu targets, %zu programs; a frame: %llu KB uploaded, "
             "%llu state calls, %llu skipped as unchanged, renderer %.2f ms on the %s thread; %llu read-backs "
             "(%.1f ms) in 300 frames",
             (unsigned long long)presents_, (unsigned long long)draws_, textures_.size(), targets_.size(), programs_.size(),
             (unsigned long long)(up_bytes_ / 300 / 1024), (unsigned long long)(calls_ / 300),
             (unsigned long long)(skipped_ / 300), double(cpu_ns_) / 300 / 1e6, render_thread_ ? "render" : "title's",
             (unsigned long long)syncs_, double(sync_ns_) / 1e6);
        LOGI("dk: renderer parts a frame: assembly %.2f, textures %.2f, commands %.2f, the rest %.2f ms; ring fence "
             "waits %llu (%.1f ms); barriers a frame %.1f fragments, %.1f full; GPU %.2f ms a frame (longest %.2f, %llu "
             "frames), done %.2f ms after its submission (longest %.2f); waits a frame: swapchain %.2f ms, frame slot "
             "%.2f ms; shaders: %llu compiled (%.0f ms), %llu from the cache; textures changed without a lock %llu (%llu "
             "in the run)",
             double(asm_ns_) / 300 / 1e6, double(tex_ns_) / 300 / 1e6, double(gl_ns_) / 300 / 1e6,
             double(cpu_ns_ - std::min(cpu_ns_, asm_ns_ + tex_ns_ + gl_ns_)) / 300 / 1e6,
             (unsigned long long)ring_waits_, double(ring_wait_ns_) / 1e6, double(barriers_frag_) / 300,
             double(barriers_full_) / 300, gpu_n_ ? double(gpu_ns_) / double(gpu_n_) / 1e6 : 0.0,
             double(gpu_max_ns_) / 1e6, (unsigned long long)gpu_n_, gpu_n_ ? double(gpu_lag_ns_) / double(gpu_n_) / 1e6 : 0.0,
             double(gpu_lag_max_ns_) / 1e6, double(acquire_ns_) / 300 / 1e6, double(frame_wait_ns_) / 300 / 1e6,
             (unsigned long long)compiles_, double(compile_ns_) / 1e6, (unsigned long long)cache_loads_,
             (unsigned long long)sig_only_, (unsigned long long)(sig_only_total_ + sig_only_));
        sig_only_total_ += sig_only_;
        sig_only_ = 0;
        LOGI("dk: vertices a frame: %.0f draws in place (%.0f KB), %.0f gathered (%.0f KB), %.0f converted (%.0f KB)",
             double(asm_dense_) / 300, double(asm_dense_bytes_) / 300 / 1024, double(asm_gathered_) / 300,
             double(asm_gathered_bytes_) / 300 / 1024, double(asm_converted_) / 300,
             double(asm_converted_bytes_) / 300 / 1024);
        asm_dense_ = asm_gathered_ = asm_converted_ = asm_dense_bytes_ = asm_gathered_bytes_ = asm_converted_bytes_ = 0;
        up_bytes_ = calls_ = skipped_ = cpu_ns_ = syncs_ = sync_ns_ = asm_ns_ = tex_ns_ = gl_ns_ = 0;
        ring_waits_ = ring_wait_ns_ = barriers_frag_ = barriers_full_ = 0;
        gpu_ns_ = gpu_n_ = gpu_max_ns_ = gpu_lag_ns_ = gpu_lag_max_ns_ = acquire_ns_ = frame_wait_ns_ = 0;
        compiles_ = compile_ns_ = cache_loads_ = 0;
        LOGI("dk: the %s thread is handle %X, on core %u now", render_thread_ ? "render" : "title's",
             unsigned(threadGetCurHandle()), unsigned(svcGetCurrentProcessorNumber()));
        // render_thread: what the hand-over costs the title's thread (render_thread.cpp's counters, backend.h)
        if (render_thread_) {
            auto ms = [](std::atomic<uint64_t>& v) { return double(v.exchange(0)) / 300 / 1e6; };
            double rec = ms(g_perf_capture_ns), wait = ms(g_perf_wait_ns);
            double rb = ms(g_perf_wait_kind_ns[0]), res = ms(g_perf_wait_kind_ns[1]), fence = ms(g_perf_wait_kind_ns[2]);
            double flush = ms(g_perf_wait_kind_ns[3]), copy = ms(g_perf_wait_kind_ns[4]), frame = ms(g_perf_wait_kind_ns[5]);
            LOGI("dk: the title's thread a frame: recording %.2f ms, waiting %.2f ms (read-back %.2f, resource %.2f, "
                 "fence %.2f, flush %.2f, copy %.2f, the frame before %.2f), %.1f named waits; read-backs %llu late, "
                 "%llu blocking in 300 frames",
                 rec, wait, rb, res, fence, flush, copy, frame, double(g_perf_waits.exchange(0)) / 300,
                 (unsigned long long)g_perf_sync_late.exchange(0), (unsigned long long)g_perf_sync_block.exchange(0));
        }
    }
    frame_cpu0_ = cpu_ns_;
    frame_asm0_ = asm_ns_;
    frame_tex0_ = tex_ns_;
    frame_gl0_ = gl_ns_;
    frame_up0_ = up_bytes_;
    frame_draws0_ = draws_;
    ReleaseOldTextures();
    ApplySettings();
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<DkBackend>(); }

// the swap with vsync paces the frames: nothing to wait for
void WaitGpuFrame(uint64_t) {}

}  // namespace gpu
