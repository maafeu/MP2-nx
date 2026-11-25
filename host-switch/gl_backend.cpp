// MP2-nx: the title's Direct3D 8 calls drawn with OpenGL 4.3 core (PORT_RENDERER=gl). The Switch build without NVK:
// mesa-switch's nouveau GL driver through EGL on the applet's NWindow (devkitPro's switch-mesa, libdrm_nouveau). The
// PC build runs the same code on WGL (MP2_GL=1) to check it against the Vulkan renderer.
//
// It follows vulkan.cpp draw for draw: the same HLSL (gpu/shaders.cpp) through glslang to SPIR-V (vk_shader.cpp) and
// SPIRV-Cross to GLSL (gl_shader.cpp), the same targets (render scale, CPU writes latched, read-backs at the console's
// size), the same state mapping. Clip space stays Vulkan's (glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)): a target's
// row 0 is the console's top row, as in guest memory and in vulkan.cpp, and only the present flips. The console's
// draw helpers below the marker are vulkan.cpp's own, copied by tools/make_gl_backend.py.
//
// Draws run on the title's thread (XBE_RENDER_THREAD=0), where the context is current.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif
#include <GL/glcorearb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
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
#include "vk_shader.h"

#ifdef _WIN32
// the PC build: a "shot" file beside the exe saves the next frame's back buffer (host-pc/main.cpp)
namespace host {
bool TakeShot();
void SaveShot(const uint8_t* bgra, uint32_t w, uint32_t h);
}  // namespace host
#endif

namespace gpu {

namespace {

// ---- the GL entry points, loaded at Init

#define GL_FUNCS(X)                                                       \
    X(PFNGLGETSTRINGPROC, glGetString)                                    \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                \
    X(PFNGLGETFLOATVPROC, glGetFloatv)                                    \
    X(PFNGLGETERRORPROC, glGetError)                                      \
    X(PFNGLENABLEPROC, glEnable)                                          \
    X(PFNGLDISABLEPROC, glDisable)                                        \
    X(PFNGLVIEWPORTPROC, glViewport)                                      \
    X(PFNGLSCISSORPROC, glScissor)                                        \
    X(PFNGLCLEARPROC, glClear)                                            \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                  \
    X(PFNGLCOLORMASKPROC, glColorMask)                                    \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                    \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                    \
    X(PFNGLCULLFACEPROC, glCullFace)                                      \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                    \
    X(PFNGLPOLYGONMODEPROC, glPolygonMode)                                \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                            \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                  \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                          \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                            \
    X(PFNGLTEXSUBIMAGE3DPROC, glTexSubImage3D)                            \
    X(PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC, glCompressedTexSubImage2D)        \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)                              \
    X(PFNGLTEXSTORAGE3DPROC, glTexStorage3D)                              \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                            \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                  \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex)          \
    X(PFNGLFLUSHPROC, glFlush)                                            \
    X(PFNGLFINISHPROC, glFinish)                                          \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                    \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)            \
    X(PFNGLBLENDCOLORPROC, glBlendColor)                                  \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc)                                \
    X(PFNGLSTENCILOPPROC, glStencilOp)                                    \
    X(PFNGLSTENCILMASKPROC, glStencilMask)                                \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                  \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                  \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                  \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                            \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                        \
    X(PFNGLCREATESHADERPROC, glCreateShader)                              \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                              \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                            \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                      \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                              \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                            \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                              \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                              \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                    \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                  \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                        \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                        \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)        \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)      \
    X(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat)                  \
    X(PFNGLVERTEXATTRIBIFORMATPROC, glVertexAttribIFormat)                \
    X(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding)                \
    X(PFNGLBINDVERTEXBUFFERPROC, glBindVertexBuffer)                      \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                        \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                  \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                        \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)              \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)          \
    X(PFNGLDRAWBUFFERPROC, glDrawBuffer)                                  \
    X(PFNGLREADBUFFERPROC, glReadBuffer)                                  \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers)                                \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                    \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)                    \
    X(PFNGLSAMPLERPARAMETERFVPROC, glSamplerParameterfv)                  \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData)                      \
    X(PFNGLCLIPCONTROLPROC, glClipControl)                                \
    X(PFNGLPROVOKINGVERTEXPROC, glProvokingVertex)                        \
    X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv)                            \
    X(PFNGLCLEARBUFFERIVPROC, glClearBufferiv)                            \
    X(PFNGLCLEARBUFFERFIPROC, glClearBufferfi)

// MP2-nx: optional entry points. Persistent mapped upload rings (ARB_buffer_storage, GL 4.4) with fences, and timer
// queries for the GPU's lag; without them the orphaned buffers and no GPU figure
#define GL_OPT_FUNCS(X)                                                   \
    X(PFNGLBUFFERSTORAGEPROC, glBufferStorage)                            \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange)                          \
    X(PFNGLFENCESYNCPROC, glFenceSync)                                    \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)                          \
    X(PFNGLDELETESYNCPROC, glDeleteSync)                                  \
    X(PFNGLGENQUERIESPROC, glGenQueries)                                  \
    X(PFNGLQUERYCOUNTERPROC, glQueryCounter)                              \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)                \
    X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv)                      \
    X(PFNGLGETINTEGER64VPROC, glGetInteger64v)

#define GL_DECLARE(type, name) type name = nullptr;
GL_FUNCS(GL_DECLARE)
GL_OPT_FUNCS(GL_DECLARE)
#undef GL_DECLARE

constexpr GLsizeiptr kStreamSize = 32 << 20;   // vertices and indices, orphaned when full
constexpr GLsizeiptr kUniformSize = 8 << 20;   // b0 and b1 per draw
// MP2-nx: the persistent rings' segments. Each ring is written front to back; leaving a segment puts a fence after
// the draws that read it, and entering one waits for its fence from the last lap (long signalled at 4 segments).
// Orphaning instead (glBufferData) made Mesa allocate a new 32 MB buffer every 30-40 frames on the console: 15-40 ms
// each, the once-a-second long frame (run 3, 2026-11-22: "gl: long frame" lines, all in the renderer's "rest").
constexpr int kRingSegments = 4;
constexpr int kGpuQueries = 4;  // frames of timestamp queries in flight
constexpr GLsizeiptr kVsConstants = 192 * 16;  // b0: the vertex shader's c registers
constexpr GLenum kAnisotropy = 0x84FE;         // GL_TEXTURE_MAX_ANISOTROPY
constexpr GLenum kMaxAnisotropy = 0x84FF;
constexpr GLenum kDxt1 = 0x83F1, kDxt3 = 0x83F2, kDxt5 = 0x83F3;  // EXT_texture_compression_s3tc

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

// ---- what vulkan.cpp's helpers below take, in GL's terms

// a console surface the renderer draws into
struct Target {
    GLuint tex = 0;  // GL_RGBA8 (BGRA uploads and reads) or GL_DEPTH24_STENCIL8
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    GLuint fb = 0;                // copy read by draws that sample the target they render to
};

struct Tex {
    GLuint img = 0;
    GLenum target = GL_TEXTURE_2D;
    GLenum ifmt = GL_RGBA8;
    TextureDesc d;
    bool decode = false;  // DXT decoded on the CPU (volume textures: GL has no compressed 3D S3TC)
    bool r32f = false;
    uint32_t w = 1, h = 1, depth = 1, levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;
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
    GLuint tex[4] = {};
    GLenum target[4] = {GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D};
    bool r32f[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
};

struct Module {
    GLuint shader = 0;
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

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DCMP_NEVER 0x200 to ALWAYS 0x207: GL_NEVER to GL_ALWAYS are the same numbers
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

// ---- BEGIN vulkan.cpp helpers (tools/make_gl_backend.py copies them; edit vulkan.cpp, not here)
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

// MP2-nx: a texture signature's hash. gpu::Hash is one chain, each step waiting on the last 64 x 64 -> 128 bit
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

// ---- END vulkan.cpp helpers

// ---- the console's values as GL's

GLenum Cmp(uint32_t v) { return GLenum(GL_NEVER + CmpIndex(v)); }

GLenum Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return GL_ZERO;
        case 1: return GL_ONE;
        case 0x300: return alpha ? GL_SRC_ALPHA : GL_SRC_COLOR;
        case 0x301: return alpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
        case 0x302: return GL_SRC_ALPHA;
        case 0x303: return GL_ONE_MINUS_SRC_ALPHA;
        case 0x304: return GL_DST_ALPHA;
        case 0x305: return GL_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? GL_DST_ALPHA : GL_DST_COLOR;
        case 0x307: return alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? GL_ONE : GL_SRC_ALPHA_SATURATE;
        case 0x8001: return GL_CONSTANT_COLOR;
        case 0x8002: return GL_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return GL_CONSTANT_ALPHA;
        case 0x8004: return GL_ONE_MINUS_CONSTANT_ALPHA;
        default: return GL_ONE;
    }
}

GLenum BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return GL_FUNC_SUBTRACT;
        case 0x800B: return GL_FUNC_REVERSE_SUBTRACT;
        case 0x8007: return GL_MIN;
        case 0x8008: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

GLenum StencilOp(uint32_t v) {
    switch (v) {
        case 0: return GL_ZERO;
        case 0x1E01: return GL_REPLACE;
        case 0x1E02: return GL_INCR;
        case 0x1E03: return GL_DECR;
        case 0x150A: return GL_INVERT;
        case 0x8507: return GL_INCR_WRAP;
        case 0x8508: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

GLenum Address(uint32_t v) {
    switch (v) {
        case 2: return GL_MIRRORED_REPEAT;
        case 3: case 5: return GL_CLAMP_TO_EDGE;
        case 4: return GL_CLAMP_TO_BORDER;
        default: return GL_REPEAT;
    }
}

// the input assembler's formats the SDK's vertex.cpp names
struct AttribFormat {
    GLint size;
    GLenum type;
    bool norm, integer;
};
AttribFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return {4, GL_FLOAT, false, false};
        case DXGI_FORMAT_R32G32B32_FLOAT: return {3, GL_FLOAT, false, false};
        case DXGI_FORMAT_R32G32_FLOAT: return {2, GL_FLOAT, false, false};
        case DXGI_FORMAT_R32_FLOAT: return {1, GL_FLOAT, false, false};
        case DXGI_FORMAT_R32_UINT: return {1, GL_UNSIGNED_INT, false, true};
        case DXGI_FORMAT_R16G16B16A16_SNORM: return {4, GL_SHORT, true, false};
        case DXGI_FORMAT_R16G16B16A16_SINT: return {4, GL_SHORT, false, true};
        case DXGI_FORMAT_R16G16_SNORM: return {2, GL_SHORT, true, false};
        case DXGI_FORMAT_R16G16_SINT: return {2, GL_SHORT, false, true};
        case DXGI_FORMAT_R16_SNORM: return {1, GL_SHORT, true, false};
        case DXGI_FORMAT_R16_SINT: return {1, GL_SHORT, false, true};
        case DXGI_FORMAT_R8G8B8A8_UNORM: return {4, GL_UNSIGNED_BYTE, true, false};
        case DXGI_FORMAT_R8G8_UNORM: return {2, GL_UNSIGNED_BYTE, true, false};
        case DXGI_FORMAT_R8_UNORM: return {1, GL_UNSIGNED_BYTE, true, false};
        case DXGI_FORMAT_B8G8R8A8_UNORM: return {GL_BGRA, GL_UNSIGNED_BYTE, true, false};
        default: return {4, GL_FLOAT, false, false};
    }
}

// fixed border colours only, as vulkan.cpp: the console's nearest. 0 transparent black, 1 opaque black, 2 white
uint32_t Border(uint32_t argb) {
    if ((argb >> 24) < 128) return 0;
    uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    return sum >= 384 ? 2 : 1;
}

struct SamplerKey {
    uint32_t min, mag, wrap[3], border, compare, func;
    float bias, min_lod, max_lod, aniso;
};

struct Program {
    GLuint id = 0;
    std::vector<glshader::Pair> pairs;
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

// the renderer's CPU time on the title's thread: draws, clears, copies, read-backs and the present without its swap
struct CpuTimer {
    uint64_t& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CpuTimer() {
        acc += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
};

class GlBackend final : public Backend {
public:
    ~GlBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override {
        if (Current()) glFlush();
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
    // context
    bool CreateContext();
    void* Proc(const char* name);
    void SwapWindow();
    void WindowSize(uint32_t& w, uint32_t& h);
    bool Current();

    // objects
    GLuint NewTexture(GLenum target, GLenum ifmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels);
    GLuint Fbo(GLuint color, GLuint depth);
    void BindFbo(GLuint color, GLuint depth);
    void DropFbos(GLuint tex);
    GLintptr Stream(const void* data, GLsizeiptr n, GLsizeiptr align);
    GLintptr Uniforms(const void* data, GLsizeiptr n);
    GLuint Sampler(const SamplerKey& k);
    GLuint StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool compare);
    GLuint FixedSampler(bool linear);

    // shaders
    Module* EnsureModule(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    Program* EnsureProgram(uint64_t vh, Module* vs, uint64_t ph, Module* ps);
    Program* FullScreen(const char* hlsl, const char* ps_entry);
    void UseProgram(GLuint id);
    void BindPairs(const Program& p, const GLuint tex[4], const GLenum target[4], const GLuint samplers[4]);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);

    // fixed function state
    void ApplyState(const PsoKey& k);
    void PlainState(bool over, bool depth_write);
    void SetAttribs(uint32_t mask);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void UploadTarget(Target& t, const Surface& s);
    void Blit(GLuint dst, uint32_t dw, uint32_t dh, int32_t x, int32_t y, int32_t w, int32_t h, GLuint src,
              const float uvrect[4], const BlitOptions& o);
    GLuint Feedback(Target& t);
    bool DepthCopy(const State& s);
    GLuint CopySource(Target& t, GLuint& down);
    bool ReadBack(GLuint tex, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    // draws
    bool BindStageTextures(const State& s, DrawTextures& stages);
    GLuint BindTexture(const State& s, int stage, DrawTextures& stages);
    GLuint BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box, DrawTextures& stages);
    GLuint BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
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
    void RenderUi();
    void SetAttrib(uint32_t r, uint32_t dxgi, uint32_t offset, uint32_t buffer);
    void Capture(GLuint tex, uint32_t w, uint32_t h);
    void WriteBmp(const char* name, const std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool top_down);

#ifdef _WIN32
    HDC hdc_ = nullptr;
    HGLRC ctx_ = nullptr;
    HMODULE gl_dll_ = nullptr;
    using SwapIntervalFn = BOOL(WINAPI*)(int);
    SwapIntervalFn swap_interval_ = nullptr;
#else
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
#endif
    std::thread::id thread_;
    bool ok_ = false;
    bool vsync_ = true;
    bool face_swap_ = false;  // MP2_GL_FACE=1: the other winding sense, for checking a driver
    bool scissor_ = true;     // gl_flags.txt "noscissor": the scissor test off, for checking a driver
    std::string flags_;       // gl_flags.txt as read
    GLint ubo_align_ = 256;
    float max_aniso_ = 1;

    GLuint vao_ = 0, stream_ = 0, ubo_ = 0;
    // state already set: an unchanged one costs no driver call (Mesa's per-call cost runs on the Switch's CPU)
    struct UnitState {
        GLuint tex[3] = {};  // 2D, 3D, cube
        GLuint smp = ~0u;
    };
    static constexpr GLuint kUnits = 16;
    UnitState units_[kUnits];
    GLuint active_unit_ = ~0u;
    int32_t vp_[4] = {-1, -1, -1, -1}, sc_[4] = {-1, -1, -1, -1};
    float blend_color_[4] = {-1, -1, -1, -1};
    uint32_t stencil_func_[3] = {~0u, ~0u, ~0u};
    struct AttribState {
        uint32_t format = ~0u, offset = 0, buffer = ~0u;
    };
    AttribState attrib_[16];
    struct UboBinding {
        GLintptr off = -1;
        GLsizeiptr size = 0;
    };
    UboBinding ubo_bind_[2];
    // the last b0 / b1 uploaded, reused while the game's constants stay the same
    std::array<std::array<float, 4>, 192> last_vc_{};
    // MP2-nx: the last draw constants uploaded, in a fixed buffer (a std::vector's assign per changed draw was 0.9% of
    // the title's thread on the console, run 9)
    alignas(16) uint8_t last_dc_[sizeof(DrawConstants)] = {};
    bool last_dc_valid_ = false;
    uint32_t last_vsc_gen_ = ~0u;  // State::vsc_gen at the last vertex constants upload or comparison
    GLintptr last_vc_off_ = -1, last_dc_off_ = -1;
    uint64_t ubo_gen_ = 0, last_vc_gen_ = ~0ull, last_dc_gen_ = ~0ull;  // ubo_gen_: the uniform ring's orphanings
    // over the 300 frames between log lines: bytes uploaded, state calls made, calls skipped as unchanged
    uint64_t up_bytes_ = 0, calls_ = 0, skipped_ = 0;
    uint64_t cpu_ns_ = 0;  // CpuTimer's total over the same frames
    uint64_t syncs_ = 0, sync_ns_ = 0;  // the game's read-backs (Sync) over the same frames, and their time
    // MP2-nx, for the 60 fps work: parts of the draws' time (vertex assembly, the stages' textures: hashing, decoding
    // and uploads, and the GL calls from the framebuffer bind to the draw), over the same frames; and one frame's
    // figures at the previous present, for the long-frame lines
    uint64_t asm_ns_ = 0, tex_ns_ = 0, gl_ns_ = 0;
    uint64_t frame_cpu0_ = 0, frame_asm0_ = 0, frame_tex0_ = 0, frame_gl0_ = 0, frame_up0_ = 0, frame_draws0_ = 0;
    uint32_t long_logged_ = 0;
    // MP2-nx: the persistent rings (kRingSegments): mapped pointers, the segment being written and each segment's
    // fence; waits on a fence (should stay 0) over the 300 frames
    bool persistent_ = false;
    uint8_t* stream_ptr_ = nullptr;
    uint8_t* ubo_ptr_ = nullptr;
    int stream_seg_ = 0, ubo_seg_ = 0;
    GLsync stream_fence_[kRingSegments] = {}, ubo_fence_[kRingSegments] = {};
    uint64_t ring_waits_ = 0, ring_wait_ns_ = 0;
    void NextSegment(bool ubo);
    void EnsureStream(GLsizeiptr need);
    void EnsureUniforms(GLsizeiptr need);
    // MP2-nx: the GPU's lag at the end of each frame (a timestamp query after the frame's last command against the GL
    // time when it was issued), read kGpuQueries frames later; summed over the 300 frames
    GLuint gpu_q_[kGpuQueries] = {};
    GLint64 gpu_issue_[kGpuQueries] = {};
    bool gpu_q_used_[kGpuQueries] = {};
    uint64_t gpu_lag_ns_ = 0, gpu_lag_n_ = 0, gpu_lag_max_ns_ = 0;
    void ActiveUnit(GLuint u);
    void BindUnit(GLuint unit, GLenum target, GLuint tex);
    void BindUnitSampler(GLuint unit, GLuint smp);
    void DeleteTexture(GLuint& t);
    void Viewport(int32_t x, int32_t y, int32_t w, int32_t h);
    void Scissor(int32_t x, int32_t y, int32_t w, int32_t h);
    void UniformRange(GLuint index, GLintptr off, GLsizeiptr size);
    void OrphanStream();
    void OrphanUniforms();
    GLsizeiptr stream_used_ = 0, ubo_used_ = 0;
    uint32_t attribs_ = 0;  // enabled attribute arrays
    GLuint program_ = 0;
    GLuint fbo_ = ~0u;
    PsoKey state_{};
    bool state_valid_ = false;

    std::unordered_map<uint64_t, Module> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, Program> programs_;
    std::unordered_map<uint64_t, GLuint> samplers_;
    std::unordered_map<uint64_t, GLuint> fbos_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    GLuint lut_ = 0;
    GLuint dummy_[3] = {};  // 2D, 3D, cube: black
    GLuint overlay_ = 0;
    uint32_t overlay_tw_ = 0, overlay_th_ = 0;
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
    float scale_ = 1.0f;     // the guest targets' scale: vertical (the output's height over the guest's)
    float scale_x_ = 1.0f;   // and horizontal: scale_, or the output's full width with render_wide
    void WantScale(float& y, float& x);
    Assembled asm_;
    // UploadTexture's buffers, kept: a film frame (a 640x480 texture every frame) allocated and zeroed 2.4 MB a frame
    std::vector<uint8_t> up_px_, up_decoded_, up_packed_;
    uint64_t presents_ = 0;
    std::chrono::steady_clock::time_point last_shot_{};
    uint32_t shots_ = 0;
    uint64_t draws_ = 0;
    // the options panel (ImGui), drawn over the presented frame
    bool ui_ = false;
    GLuint ui_font_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

// ---- context

#ifdef _WIN32
constexpr int kWglMajor = 0x2091, kWglMinor = 0x2092, kWglProfile = 0x9126, kWglCore = 0x1, kWglFlags = 0x2094;
using CreateContextAttribsFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

bool GlBackend::CreateContext() {
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!hwnd) {
        LOGE("gl: no window");
        return false;
    }
    hdc_ = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    int pf = ChoosePixelFormat(hdc_, &pfd);
    if (!pf || !SetPixelFormat(hdc_, pf, &pfd)) {
        LOGE("gl: no pixel format (%lu)", GetLastError());
        return false;
    }
    HGLRC legacy = wglCreateContext(hdc_);
    if (!legacy || !wglMakeCurrent(hdc_, legacy)) {
        LOGE("gl: no context (%lu)", GetLastError());
        return false;
    }
    auto create = reinterpret_cast<CreateContextAttribsFn>(wglGetProcAddress("wglCreateContextAttribsARB"));
    const int attrs[] = {kWglMajor, 4, kWglMinor, 3, kWglProfile, kWglCore, 0};
    ctx_ = create ? create(hdc_, nullptr, attrs) : nullptr;
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (!ctx_ || !wglMakeCurrent(hdc_, ctx_)) {
        LOGE("gl: no 4.3 core context");
        return false;
    }
    gl_dll_ = GetModuleHandleA("opengl32.dll");
    swap_interval_ = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"));
    if (swap_interval_) swap_interval_(vsync_ ? 1 : 0);
    return true;
}

void* GlBackend::Proc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    intptr_t v = reinterpret_cast<intptr_t>(p);
    if (v >= -1 && v <= 3) p = reinterpret_cast<void*>(GetProcAddress(gl_dll_, name));
    return p;
}

void GlBackend::SwapWindow() { SwapBuffers(hdc_); }

void GlBackend::WindowSize(uint32_t& w, uint32_t& h) {
    RECT r{};
    GetClientRect(static_cast<HWND>(app::Window()), &r);
    w = uint32_t(std::max<LONG>(r.right - r.left, 1));
    h = uint32_t(std::max<LONG>(r.bottom - r.top, 1));
}
#else
// the Switch's output, docked or not: the system scales 720p to the TV. 1280x720 is the goal (30 fps)
constexpr uint32_t kSurfaceW = 1280, kSurfaceH = 720;

bool GlBackend::CreateContext() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        LOGE("gl: EGL display failed (%x)", eglGetError());
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOGE("gl: no desktop GL API (%x)", eglGetError());
        return false;
    }
    const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(display_, cfg_attrs, &config, 1, &n) || n == 0) {
        LOGE("gl: no EGL config (%x)", eglGetError());
        return false;
    }
    NWindow* win = nwindowGetDefault();
    nwindowSetDimensions(win, kSurfaceW, kSurfaceH);
    surface_ = eglCreateWindowSurface(display_, config, win, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("gl: no window surface (%x)", eglGetError());
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attrs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("gl: no 4.3 core context (%x)", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("gl: make current failed (%x)", eglGetError());
        return false;
    }
    eglSwapInterval(display_, vsync_ ? 1 : 0);
    return true;
}

void* GlBackend::Proc(const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

// MP2-nx: with vsync on, the swap interval follows the frame cap: 2 vblanks a frame at 30, 1 at 60 (app::Pace then
// adds no timer of its own, so a frame always lands on a vblank)
void GlBackend::SwapWindow() {
    if (vsync_) {
        static int interval = 1;
        int want = app::FrameCapNow() == 30 ? 2 : 1;
        if (want != interval) {
            eglSwapInterval(display_, want);
            interval = want;
            LOGI("gl: swap interval %d (%u fps)", want, app::FrameCapNow());
        }
    }
    eglSwapBuffers(display_, surface_);
}

void GlBackend::WindowSize(uint32_t& w, uint32_t& h) {
    w = kSurfaceW;
    h = kSurfaceH;
}
#endif

// the context is current on the thread that made it: a call from another thread is logged and dropped
bool GlBackend::Current() {
    if (!ok_) return false;
    if (std::this_thread::get_id() == thread_) return true;
    LOG_FIRST(8, "gl: a call from another thread dropped (the context is the title thread's)");
    return false;
}

#ifdef _WIN32
void APIENTRY DebugMessage(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    LOG_FIRST(64, "gl: debug %s: %s", type == GL_DEBUG_TYPE_ERROR ? "error" : "note", msg);
}
#endif

bool GlBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    vsync_ = app::Get().vsync;
    face_swap_ = std::getenv("MP2_GL_FACE") != nullptr;
    // test switches for emulator and driver runs: gl_flags.txt beside the executable ("noscissor")
    {
        std::string flags;
        if (FILE* f = std::fopen((app::ExeDir() / "gl_flags.txt").string().c_str(), "rb")) {
            char buf[256] = {};
            flags.assign(buf, std::fread(buf, 1, sizeof(buf) - 1, f));
            std::fclose(f);
        }
        scissor_ = flags.find("noscissor") == std::string::npos;
        flags_ = flags;
        if (!flags.empty()) LOGI("gl: test flags: %s", flags.c_str());
    }
    if (!CreateContext()) return false;
    thread_ = std::this_thread::get_id();
    bool missing = false;
#define GL_LOAD(type, name)                                      \
    name = reinterpret_cast<type>(Proc(#name));                  \
    if (!name) {                                                 \
        LOGE("gl: %s missing", #name);                           \
        missing = true;                                          \
    }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    if (missing) return false;
#define GL_LOAD_OPT(type, name) name = reinterpret_cast<type>(Proc(#name));
    GL_OPT_FUNCS(GL_LOAD_OPT)
#undef GL_LOAD_OPT
    LOGI("gl: %s, %s, %s", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_VENDOR));
#ifdef _WIN32
    if (auto cb = reinterpret_cast<PFNGLDEBUGMESSAGECALLBACKPROC>(Proc("glDebugMessageCallback"))) {
        glEnable(GL_DEBUG_OUTPUT);
        cb(DebugMessage, nullptr);
    }
#endif
    vkshader::Init(app::ExeDir() / "shadercache");
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align_);
    ubo_align_ = std::max<GLint>(ubo_align_, 16);
    glGetFloatv(kMaxAnisotropy, &max_aniso_);
    glGetError();
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    // a flat shaded triangle takes its first vertex's colour, as on the console and in Vulkan (GL: the last)
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    glDisable(GL_DITHER);
    if (scissor_) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    // MP2-nx: the upload rings persistent and mapped where the driver can (gl_flags.txt "nopersist" keeps the
    // orphaned buffers, for an A/B)
    if (glBufferStorage && glMapBufferRange && glFenceSync && glClientWaitSync && glDeleteSync &&
        flags_.find("nopersist") == std::string::npos) {
        const GLbitfield f = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glGetError();
        glGenBuffers(1, &stream_);
        glBindBuffer(GL_ARRAY_BUFFER, stream_);
        glBufferStorage(GL_ARRAY_BUFFER, kStreamSize, nullptr, f);
        void* sp = glMapBufferRange(GL_ARRAY_BUFFER, 0, kStreamSize, f);
        glGenBuffers(1, &ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferStorage(GL_UNIFORM_BUFFER, kUniformSize, nullptr, f);
        void* up = glMapBufferRange(GL_UNIFORM_BUFFER, 0, kUniformSize, f);
        if (sp && up && glGetError() == GL_NO_ERROR) {
            persistent_ = true;
            stream_ptr_ = static_cast<uint8_t*>(sp);
            ubo_ptr_ = static_cast<uint8_t*>(up);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_);
        } else {
            LOGW("gl: persistent upload rings refused (stream %p, uniforms %p): orphaned buffers", sp, up);
        }
    }
    if (!persistent_) {
        glGenBuffers(1, &stream_);
        glBindBuffer(GL_ARRAY_BUFFER, stream_);
        glBufferData(GL_ARRAY_BUFFER, kStreamSize, nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_);
        glGenBuffers(1, &ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferData(GL_UNIFORM_BUFFER, kUniformSize, nullptr, GL_STREAM_DRAW);
    }
    if (glGenQueries && glQueryCounter && glGetQueryObjectui64v && glGetQueryObjectiv && glGetInteger64v)
        glGenQueries(kGpuQueries, gpu_q_);
    LOGI("gl: upload rings %s; GPU lag %s", persistent_ ? "persistent and mapped, 4 fenced segments each" : "orphaned",
         gpu_q_[0] ? "measured" : "not measured");
    // the stages without a texture read black ones of their kind
    const uint32_t black[6] = {};
    dummy_[0] = NewTexture(GL_TEXTURE_2D, GL_RGBA8, 1, 1, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, black);
    dummy_[1] = NewTexture(GL_TEXTURE_3D, GL_RGBA8, 1, 1, 1, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, black);
    dummy_[2] = NewTexture(GL_TEXTURE_CUBE_MAP, GL_RGBA8, 1, 1, 1, 1);
    for (int f = 0; f < 6; f++)
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, black);
    lut_ = NewTexture(GL_TEXTURE_2D, GL_RGBA8, 256, 1, 1, 1);
    for (int i = 0; i < 256; i++) ramp_[i] = ramp_[256 + i] = ramp_[512 + i] = uint8_t(i);
    WindowSize(out_w_, out_h_);
    WantScale(scale_, scale_x_);
    ok_ = true;
    if (flags_.find("selftest") != std::string::npos) SelfTest();
    ui_ = InitUi();
    LOGI("gl: %ux%u guest, %ux%u output, render scale %.3f x %.3f, %s", width, height, out_w_, out_h_, scale_x_,
         scale_, vsync_ ? "vsync" : "no vsync");
    return true;
}

// ---- objects

GLuint GlBackend::NewTexture(GLenum target, GLenum ifmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels) {
    GLuint t = 0;
    glGenTextures(1, &t);
    ActiveUnit(0);  // uploads go to unit 0: a skipped bind leaves another unit active
    BindUnit(0, target, t);
    if (target == GL_TEXTURE_3D)
        glTexStorage3D(target, GLsizei(levels), ifmt, GLsizei(w), GLsizei(h), GLsizei(d));
    else
        glTexStorage2D(target, GLsizei(levels), ifmt, GLsizei(w), GLsizei(h));
    return t;
}

GLuint GlBackend::Fbo(GLuint color, GLuint depth) {
    uint64_t key = uint64_t(color) << 32 | depth;
    if (auto it = fbos_.find(key); it != fbos_.end()) return it->second;
    GLuint f = 0;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    fbo_ = f;
    if (color) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    } else {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) LOG_FIRST(8, "gl: framebuffer %u/%u incomplete (%x)", color, depth, st);
    fbos_[key] = f;
    return f;
}

void GlBackend::BindFbo(GLuint color, GLuint depth) {
    GLuint f = Fbo(color, depth);
    if (f != fbo_) {
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        fbo_ = f;
    }
}

void GlBackend::DropFbos(GLuint tex) {
    for (auto it = fbos_.begin(); it != fbos_.end();) {
        if (uint32_t(it->first >> 32) == tex || uint32_t(it->first) == tex) {
            if (fbo_ == it->second) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                fbo_ = 0;
            }
            glDeleteFramebuffers(1, &it->second);
            it = fbos_.erase(it);
        } else {
            ++it;
        }
    }
}

// the rings are orphaned when full: the driver keeps the old storage for draws still reading it. A draw's uploads
// must all land in one storage (Draw reserves them first): one made before an orphaning is lost to the draw after it
void GlBackend::OrphanStream() {
    glBindBuffer(GL_COPY_WRITE_BUFFER, stream_);
    glBufferData(GL_COPY_WRITE_BUFFER, kStreamSize, nullptr, GL_STREAM_DRAW);
    stream_used_ = 0;
}

void GlBackend::OrphanUniforms() {
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    glBufferData(GL_UNIFORM_BUFFER, kUniformSize, nullptr, GL_STREAM_DRAW);
    ubo_used_ = 0;
    ubo_gen_++;
}

// MP2-nx: the persistent rings. Leaving segment i fences the draws so far (they are the last to read it); entering
// segment j waits for j's fence from the previous lap, then writes from its start. A uniform segment change also moves
// ubo_gen_, so no draw reuses a constant block's offset across it.
void GlBackend::NextSegment(bool ubo) {
    GLsync* fences = ubo ? ubo_fence_ : stream_fence_;
    int& seg = ubo ? ubo_seg_ : stream_seg_;
    GLsizeiptr& used = ubo ? ubo_used_ : stream_used_;
    const GLsizeiptr size = (ubo ? kUniformSize : kStreamSize) / kRingSegments;
    if (fences[seg]) glDeleteSync(fences[seg]);
    fences[seg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    seg = (seg + 1) % kRingSegments;
    if (fences[seg]) {
        auto t0 = std::chrono::steady_clock::now();
        GLenum r = glClientWaitSync(fences[seg], GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
        if (r != GL_ALREADY_SIGNALED) {
            ring_waits_++;
            ring_wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - t0).count());
        }
        glDeleteSync(fences[seg]);
        fences[seg] = nullptr;
    }
    used = GLsizeiptr(seg) * size;
    if (ubo) ubo_gen_++;
}

// room for need bytes in one storage (one segment, or the whole orphaned buffer): a draw's uploads stay together
void GlBackend::EnsureStream(GLsizeiptr need) {
    if (persistent_) {
        const GLsizeiptr size = kStreamSize / kRingSegments;
        if (need <= size && stream_used_ + need > GLsizeiptr(stream_seg_ + 1) * size) NextSegment(false);
    } else if (stream_used_ + need > kStreamSize && need <= kStreamSize) {
        OrphanStream();
    }
}

void GlBackend::EnsureUniforms(GLsizeiptr need) {
    if (persistent_) {
        const GLsizeiptr size = kUniformSize / kRingSegments;
        if (ubo_used_ + need > GLsizeiptr(ubo_seg_ + 1) * size) NextSegment(true);
    } else if (ubo_used_ + need > kUniformSize) {
        OrphanUniforms();
    }
}

GLintptr GlBackend::Stream(const void* data, GLsizeiptr n, GLsizeiptr align) {
    stream_used_ = (stream_used_ + align - 1) / align * align;
    if (persistent_) {
        const GLsizeiptr size = kStreamSize / kRingSegments;
        if (stream_used_ + n > GLsizeiptr(stream_seg_ + 1) * size) {
            NextSegment(false);
            if (n > size) {
                LOGW("gl: an upload of %lld bytes is larger than a ring segment: left out", (long long)n);
                return GLintptr(stream_used_);
            }
        }
        GLintptr at = stream_used_;
        if (data && n) {
            std::memcpy(stream_ptr_ + at, data, size_t(n));
            up_bytes_ += uint64_t(n);
        }
        stream_used_ += std::max<GLsizeiptr>(n, 4);
        return at;
    }
    if (stream_used_ + n > kStreamSize) OrphanStream();
    GLintptr at = stream_used_;
    if (data && n) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, stream_);
        glBufferSubData(GL_COPY_WRITE_BUFFER, at, n, data);
        up_bytes_ += uint64_t(n);
    }
    stream_used_ += std::max<GLsizeiptr>(n, 4);
    return at;
}

GLintptr GlBackend::Uniforms(const void* data, GLsizeiptr n) {
    ubo_used_ = (ubo_used_ + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    if (persistent_) {
        const GLsizeiptr size = kUniformSize / kRingSegments;
        if (ubo_used_ + n > GLsizeiptr(ubo_seg_ + 1) * size) {
            NextSegment(true);
            ubo_used_ = (ubo_used_ + ubo_align_ - 1) / ubo_align_ * ubo_align_;
        }
        GLintptr at = ubo_used_;
        if (data && n) {
            std::memcpy(ubo_ptr_ + at, data, size_t(n));
            up_bytes_ += uint64_t(n);
        }
        ubo_used_ += std::max<GLsizeiptr>(n, 16);
        return at;
    }
    if (ubo_used_ + n > kUniformSize) OrphanUniforms();
    GLintptr at = ubo_used_;
    if (data && n) {
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, at, n, data);
        up_bytes_ += uint64_t(n);
    }
    ubo_used_ += std::max<GLsizeiptr>(n, 16);
    return at;
}

void GlBackend::ActiveUnit(GLuint u) {
    if (u == active_unit_) return;
    glActiveTexture(GL_TEXTURE0 + u);
    active_unit_ = u;
    calls_++;
}

int TargetSlot(GLenum target) { return target == GL_TEXTURE_3D ? 1 : target == GL_TEXTURE_CUBE_MAP ? 2 : 0; }

void GlBackend::BindUnit(GLuint unit, GLenum target, GLuint tex) {
    if (unit < kUnits && units_[unit].tex[TargetSlot(target)] == tex) {
        skipped_++;
        return;
    }
    ActiveUnit(unit);
    glBindTexture(target, tex);
    if (unit < kUnits) units_[unit].tex[TargetSlot(target)] = tex;
    calls_++;
}

void GlBackend::BindUnitSampler(GLuint unit, GLuint smp) {
    if (unit < kUnits && units_[unit].smp == smp) {
        skipped_++;
        return;
    }
    glBindSampler(unit, smp);
    if (unit < kUnits) units_[unit].smp = smp;
    calls_++;
}

// a deleted texture is unbound everywhere, and its name may come back for a new one: forgotten here too
void GlBackend::DeleteTexture(GLuint& t) {
    if (!t) return;
    for (UnitState& u : units_)
        for (GLuint& b : u.tex)
            if (b == t) b = 0;
    glDeleteTextures(1, &t);
    t = 0;
}

void GlBackend::Viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (vp_[0] == x && vp_[1] == y && vp_[2] == w && vp_[3] == h) {
        skipped_++;
        return;
    }
    glViewport(x, y, w, h);
    vp_[0] = x, vp_[1] = y, vp_[2] = w, vp_[3] = h;
    calls_++;
}

void GlBackend::Scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (sc_[0] == x && sc_[1] == y && sc_[2] == w && sc_[3] == h) {
        skipped_++;
        return;
    }
    glScissor(x, y, w, h);
    sc_[0] = x, sc_[1] = y, sc_[2] = w, sc_[3] = h;
    calls_++;
}

void GlBackend::UniformRange(GLuint index, GLintptr off, GLsizeiptr size) {
    if (index < 2 && ubo_bind_[index].off == off && ubo_bind_[index].size == size) {
        skipped_++;
        return;
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, index, ubo_, off, size);
    if (index < 2) ubo_bind_[index] = {off, size};
    calls_++;
}

GLuint GlBackend::Sampler(const SamplerKey& k) {
    uint64_t key = Hash(&k, sizeof(k));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    GLuint s = 0;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GLint(k.min));
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GLint(k.mag));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GLint(k.wrap[0]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GLint(k.wrap[1]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, GLint(k.wrap[2]));
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, k.bias);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, k.min_lod);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, k.max_lod);
    if (k.aniso > 1) glSamplerParameterf(s, kAnisotropy, k.aniso);
    static const float borders[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, borders[k.border]);
    if (k.compare) {
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, GLint(k.func));
    }
    samplers_[key] = s;
    return s;
}

GLuint GlBackend::FixedSampler(bool linear) {
    SamplerKey k{};
    k.min = k.mag = linear ? GL_LINEAR : GL_NEAREST;
    k.wrap[0] = k.wrap[1] = k.wrap[2] = GL_CLAMP_TO_EDGE;
    k.max_lod = 1000;
    return Sampler(k);
}

GLuint GlBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool compare) {
    SamplerKey k{};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    bool mip_lin = mip == kTexfLinear;
    bool min_lin = lin(min), mag_lin = lin(mag);
    k.min = min_lin ? (mip_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                    : (mip_lin ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    k.mag = mag_lin ? GL_LINEAR : GL_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t aniso = app::Get().aniso;
    bool mipped = mip && stages.levels[i] > 1;
    if (aniso && min_lin && mipped && !stages.info[i].shadow) an = aniso;
    if (an > 1) k.aniso = std::min(float(an), max_aniso_);
    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    k.wrap[0] = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    k.wrap[1] = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    k.wrap[2] = Address(t[kTssAddressW]);
    k.bias = std::clamp(Bits(t[kTssMipLodBias]), -15.0f, 15.0f);
    k.min_lod = std::min(float(t[kTssMaxMipLevel]), float(stages.levels[i] - 1));
    k.max_lod = mip ? 1000.0f : k.min_lod;
    k.border = Border(t[kTssBorderColor]);
    if (compare) {
        // the console tests texel OP r, GL r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        bool linear = lin(min) || lin(mag);
        k.min = linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        k.mag = linear ? GL_LINEAR : GL_NEAREST;
        k.compare = 1;
        k.func = GL_NEVER + swap[CmpIndex(s.rs[kRsShadowFunc])];
        k.aniso = 0;
        // a depth texture's border is a depth: 0000FFFF is the far plane of a 16 bit buffer (vulkan.cpp)
        k.border = (t[kTssBorderColor] & 0xFFFFFF) >= 0x8000 ? 2 : 1;
    }
    return Sampler(k);
}

// ---- shaders

Module* GlBackend::EnsureModule(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    bool vertex = stage == vkshader::Stage::Vertex;
    uint64_t seed = Hash(entry, std::strlen(entry), vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = modules_.find(hash); it != modules_.end()) return it->second.shader ? &it->second : nullptr;
    Module& m = modules_[hash];
    std::vector<uint32_t> spirv;
    std::string glsl;
    // the vertex stage's textures (none in the SDK's shaders) after the pixel stage's 8 units
    if (!vkshader::Compile(src, entry, stage, hash, spirv) || !glshader::ToGlsl(spirv, vertex, vertex ? 8 : 0, glsl, m.pairs))
        return nullptr;
    GLuint sh = glCreateShader(vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    const char* text = glsl.c_str();
    glShaderSource(sh, 1, &text, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        LOGE("gl: %s %s does not compile: %s", vertex ? "vertex" : "pixel", entry, log);
        LOGE("%s", glsl.c_str());
        glDeleteShader(sh);
        return nullptr;
    }
    m.shader = sh;
    return &m;
}

Program* GlBackend::EnsureProgram(uint64_t vh, Module* vs, uint64_t ph, Module* ps) {
    uint64_t key = Hash(&ph, sizeof(ph), vh);
    if (auto it = programs_.find(key); it != programs_.end()) return it->second.id ? &it->second : nullptr;
    Program& p = programs_[key];
    GLuint id = glCreateProgram();
    glAttachShader(id, vs->shader);
    glAttachShader(id, ps->shader);
    glLinkProgram(id);
    GLint ok = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
        LOGE("gl: program %016llx/%016llx does not link: %s", (unsigned long long)vh, (unsigned long long)ph, log);
        return nullptr;
    }
    p.id = id;
    p.pairs = vs->pairs;
    p.pairs.insert(p.pairs.end(), ps->pairs.begin(), ps->pairs.end());
    return &p;
}

Program* GlBackend::FullScreen(const char* hlsl, const char* ps_entry) {
    uint64_t hv, hp;
    Module* vs = EnsureModule(hlsl, "vs", vkshader::Stage::Vertex, hv);
    Module* ps = EnsureModule(hlsl, ps_entry, vkshader::Stage::Pixel, hp);
    return vs && ps ? EnsureProgram(hv, vs, hp, ps) : nullptr;
}

void GlBackend::UseProgram(GLuint id) {
    if (id != program_) {
        glUseProgram(id);
        program_ = id;
    }
}

void GlBackend::BindPairs(const Program& p, const GLuint tex[4], const GLenum target[4], const GLuint samplers[4]) {
    for (const glshader::Pair& pr : p.pairs) {
        GLuint t = pr.tex < 4 ? tex[pr.tex] : 0;
        GLenum tg = pr.tex < 4 ? target[pr.tex] : GL_TEXTURE_2D;
        BindUnit(pr.unit, tg, t ? t : dummy_[0]);
        BindUnitSampler(pr.unit, pr.sampler < 4 ? samplers[pr.sampler] : 0);
    }
}

void GlBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        auto m = modules_.find(sh.vh);
        sh.vs = m != modules_.end() && m->second.shader ? &m->second : nullptr;
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

void GlBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt); it != shader_src_.end()) {
        sh.ph = it->second;
        auto m = modules_.find(sh.ph);
        sh.ps = m != modules_.end() && m->second.shader ? &m->second : nullptr;
        return;
    }
    sh.ps = EnsureModule(PixelHlsl(pk), "main", vkshader::Stage::Pixel, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

// ---- fixed function state

void GlBackend::ApplyState(const PsoKey& k) {
    if (state_valid_ && !std::memcmp(&k, &state_, sizeof(k))) return;
    state_ = k;
    state_valid_ = true;
    if (k.blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(Blend(k.src, false), Blend(k.dst, false), Blend(k.src, true), Blend(k.dst, true));
        glBlendEquationSeparate(BlendOp(k.op), BlendOp(k.op));
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(k.wmask & 1, (k.wmask >> 1) & 1, (k.wmask >> 2) & 1, (k.wmask >> 3) & 1);
    if (k.has_ds && k.zen) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(Cmp(k.zfunc));
        glDepthMask(k.zwrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (k.has_ds && k.sten) {
        glEnable(GL_STENCIL_TEST);
        glStencilOp(StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass));
        glStencilMask(k.swmask);
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
    }
    if (k.cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    // vulkan.cpp's front face, the clip origin's flip taken into account (ARB_clip_control keeps facing as before it)
    bool ccw = k.ccw != 0;
    if (face_swap_) ccw = !ccw;
    glFrontFace(ccw ? GL_CCW : GL_CW);
    constexpr uint32_t kFillWireframe = 0x1B01;
    glPolygonMode(GL_FRONT_AND_BACK, k.fill == kFillWireframe ? GL_LINE : GL_FILL);
    if (k.zbias != 0 || k.zslope != 0) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(k.zslope, float(k.zbias));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_POLYGON_OFFSET_LINE);
    }
}

// a full screen pass: no blend (or premultiplied over), no tests, every channel; depth_write: a depth only pass
void GlBackend::PlainState(bool over, bool depth_write) {
    state_valid_ = false;
    if (over) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (depth_write) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
}

void GlBackend::SetAttribs(uint32_t mask) {
    uint32_t diff = mask ^ attribs_;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((diff >> r) & 1)) continue;
        if ((mask >> r) & 1)
            glEnableVertexAttribArray(r);
        else
            glDisableVertexAttribArray(r);
    }
    attribs_ = mask;
}

// ---- targets

Target* GlBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth, gw, gh));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void GlBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    Target& t = it->second;
    if (draw_rt_ == &t) draw_rt_ = nullptr;
    if (draw_ds_ == &t) draw_ds_ = nullptr;
    for (GLuint* p : {&t.tex, &t.fb}) {
        if (!*p) continue;
        DropFbos(*p);
        DeleteTexture(*p);
    }
    targets_.erase(it);
}

Target* GlBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
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
    t.tex = NewTexture(GL_TEXTURE_2D, depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8, t.w, t.h, 1, 1);
    // a new target starts cleared
    BindFbo(depth ? 0 : t.tex, depth ? t.tex : 0);
    Scissor(0, 0, GLsizei(t.w), GLsizei(t.h));
    if (depth) {
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
    } else {
        const float black[4] = {0, 0, 0, 0};
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearBufferfv(GL_COLOR, 0, black);
    }
    state_valid_ = false;
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

// src drawn into the rect of dst (a w x h colour texture) through the blit shader
void GlBackend::Blit(GLuint dst, uint32_t dw, uint32_t dh, int32_t x, int32_t y, int32_t w, int32_t h, GLuint src,
                     const float uvrect[4], const BlitOptions& o) {
    Program* p = FullScreen(kBlitHlsl, "psmain");
    if (!p) return;
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    GLintptr at = Uniforms(c, sizeof(c));
    UniformRange(1, at, sizeof(c));
    if (dst) BindFbo(dst, 0);
    PlainState(o.over, false);
    UseProgram(p->id);
    const GLuint tex[4] = {src, lut_, 0, 0};
    const GLenum tg[4] = {GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D};
    const GLuint smp[4] = {FixedSampler(true), FixedSampler(false), FixedSampler(false), FixedSampler(false)};
    BindPairs(*p, tex, tg, smp);
    SetAttribs(0);
    Viewport(x, y, w, h);
    Scissor(0, 0, GLsizei(dw), GLsizei(dh));
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// CPU pixels of a colour surface into its target, scaled; after a Sync only the changed ones, over the picture
void GlBackend::UploadTarget(Target& t, const Surface& s) {
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
    GLuint tex = NewTexture(GL_TEXTURE_2D, GL_RGBA8, d.width, d.height, 1, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / 4));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(d.width), GLsizei(d.height), GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    float uv[4] = {0, 0, 1, 1};
    BlitOptions o;
    o.alpha = true;
    o.over = over;
    Blit(t.tex, t.w, t.h, 0, 0, int32_t(t.w), int32_t(t.h), tex, uv, o);
    DeleteTexture(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// a copy of the target to read while drawing to it
GLuint GlBackend::Feedback(Target& t) {
    if (!t.fb) t.fb = NewTexture(GL_TEXTURE_2D, t.depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8, t.w, t.h, 1, 1);
    glCopyImageSubData(t.tex, GL_TEXTURE_2D, 0, 0, 0, 0, t.fb, GL_TEXTURE_2D, 0, 0, 0, 0, GLsizei(t.w), GLsizei(t.h), 1);
    return t.fb;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool GlBackend::DepthCopy(const State& s) {
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
    BindFbo(0, dst.tex);
    PlainState(false, true);
    UseProgram(p->id);
    const GLuint tex[4] = {src.tex, 0, 0, 0};
    const GLenum tg[4] = {GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D};
    const GLuint smp[4] = {FixedSampler(true), FixedSampler(false), FixedSampler(false), FixedSampler(false)};
    BindPairs(*p, tex, tg, smp);
    SetAttribs(0);
    Viewport(0, 0, GLsizei(dst.w), GLsizei(dst.h));
    Scissor(0, 0, GLsizei(dst.w), GLsizei(dst.h));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    dst.drawn = true;
    return true;
}

// the target as a guest sized BGRA texture: itself, or shrunk into down (made here, the caller deletes it)
GLuint GlBackend::CopySource(Target& t, GLuint& down) {
    down = 0;
    if (t.depth) return 0;
    if (t.w == t.gw && t.h == t.gh) return t.tex;
    uint32_t taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
    down = NewTexture(GL_TEXTURE_2D, GL_RGBA8, t.gw, t.gh, 1, 1);
    float uv[4] = {0, 0, 1, 1};
    BlitOptions o;
    o.alpha = true;
    o.taps = std::max(taps, 1u);
    Blit(down, t.gw, t.gh, 0, 0, int32_t(t.gw), int32_t(t.gh), t.tex, uv, o);
    return down;
}

// read as RGBA and swapped here: GL_BGRA reads came back a quarter wide under Ryujinx (nouveau's BGRA copy path)
void ReadPixelsBgra(uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    out.resize(size_t(w) * h * 4);
    glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
}

bool GlBackend::ReadBack(GLuint tex, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    BindFbo(tex, 0);
    ReadPixelsBgra(w, h, out);
    return true;
}

// ---- draws

GLuint GlBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                    DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return 0;
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
    stages.target[stage] = GL_TEXTURE_2D;
    if (feedback) return Feedback(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    return t.tex;
}

bool GlBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    DeleteTexture(tx.img);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    tx.decode = bc && d.depth > 1 && !d.cube;
    switch (fi.host) {
        case Host::BC1: tx.ifmt = kDxt1; break;
        case Host::BC2: tx.ifmt = kDxt3; break;
        case Host::BC3: tx.ifmt = kDxt5; break;
        case Host::R32F: tx.ifmt = GL_R32F; tx.r32f = true; break;
        default: tx.ifmt = GL_RGBA8; break;
    }
    if (tx.decode) tx.ifmt = GL_RGBA8;
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    tx.target = d.cube ? GL_TEXTURE_CUBE_MAP : d.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    tx.w = w;
    tx.h = h;
    tx.depth = d.cube ? 1 : d.depth;
    tx.img = NewTexture(tx.target, tx.ifmt, w, h, tx.depth, tx.levels);
    return tx.img != 0;
}

void GlBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                              const uint32_t* colours) {
    ActiveUnit(0);  // uploads go to unit 0: a skipped bind leaves another unit active
    BindUnit(0, tx.target, tx.img);
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
                // already packed: the rows below would copy it byte for byte
                data = src->data();
            } else {
                packed.assign(bytes, 0);
                for (uint32_t z = 0; z < md; z++)
                    for (uint32_t y = 0; y < rows && y < src_rows; y++)
                        std::memcpy(&packed[(size_t(z) * rows + y) * row], &(*src)[(size_t(z) * src_rows + y) * pitch],
                                    std::min(pitch, row));
                data = packed.data();
            }
            GLenum face = tx.target == GL_TEXTURE_CUBE_MAP ? GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f) : tx.target;
            GLenum fmt = tx.r32f ? GL_RED : GL_BGRA, type = tx.r32f ? GL_FLOAT : GL_UNSIGNED_BYTE;
            if (tx.target == GL_TEXTURE_3D)
                glTexSubImage3D(GL_TEXTURE_3D, GLint(l), 0, 0, 0, GLsizei(mw), GLsizei(mh), GLsizei(md), fmt, type, data);
            else if (bc)
                glCompressedTexSubImage2D(face, GLint(l), 0, 0, GLsizei(mw), GLsizei(mh), tx.ifmt, GLsizei(bytes), data);
            else
                glTexSubImage2D(face, GLint(l), 0, 0, GLsizei(mw), GLsizei(mh), fmt, type, data);
        }
}

GLuint GlBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                   DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& t = textures_[key];
    bool same = SameTexture(t, d);
    if (!same && !CreateTexture(t, d, fi)) {
        textures_.erase(key);
        return 0;
    }
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (sig != t.sig || !same) {
            t.sig = sig;
            UploadTexture(t, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette.va ? palette.colours : nullptr);
        }
    }
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.target[stage] = t.target;
    stages.r32f[stage] = t.r32f;
    stages.levels[stage] = t.levels;
    return t.img;
}

GLuint GlBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.target[stage] = GL_TEXTURE_2D;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return 0;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool GlBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) stages.tex[i] = BindTexture(s, i, stages);
    // a stage without a texture reads a black one of its kind
    for (int i = 0; i < 4; i++) {
        if (stages.tex[i]) continue;
        uint8_t kind = stages.info[i].kind;
        int k = kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0;
        stages.tex[i] = dummy_[k];
        stages.target[i] = k == 1 ? GL_TEXTURE_3D : k == 2 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    return true;
}

void GlBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
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

void GlBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
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
    if (!prog) return;

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
    // every upload of this draw in one storage of each ring (see OrphanStream)
    GLsizeiptr need = 64 + (a.index_count ? GLsizeiptr(a.index_count) * 4 + 16 : 0);
    for (uint32_t i = 0; i < a.buffer_count; i++) need += GLsizeiptr(a.buffers[i].bytes) + 32;
    EnsureStream(need);
    EnsureUniforms(GLsizeiptr(2 * (kVsConstants + sizeof(c) + 2 * ubo_align_)));
    GLintptr vc, dc;
    // MP2-nx: nothing wrote the constants since the last upload (State::vsc_gen): no 3 KB comparison (memcmp was 1.3%
    // of the title's thread on the console, run 9); written with the same values: compared, as before
    if (s.vsc_gen_tracked && last_vc_gen_ == ubo_gen_ && last_vsc_gen_ == s.vsc_gen) {
        vc = last_vc_off_;
        skipped_++;
    } else if (last_vc_gen_ == ubo_gen_ && !std::memcmp(s.vsc.data(), last_vc_.data(), kVsConstants)) {
        vc = last_vc_off_;
        skipped_++;
        last_vsc_gen_ = s.vsc_gen;
    } else {
        vc = Uniforms(s.vsc.data(), kVsConstants);
        std::memcpy(last_vc_.data(), s.vsc.data(), kVsConstants);
        last_vc_off_ = vc;
        last_vc_gen_ = ubo_gen_;
        last_vsc_gen_ = s.vsc_gen;
    }
    if (last_dc_gen_ == ubo_gen_ && last_dc_valid_ && !std::memcmp(&c, last_dc_, sizeof(c))) {
        dc = last_dc_off_;
        skipped_++;
    } else {
        dc = Uniforms(&c, sizeof(c));
        std::memcpy(last_dc_, &c, sizeof(c));
        last_dc_valid_ = true;
        last_dc_off_ = dc;
        last_dc_gen_ = ubo_gen_;
    }
    GLintptr voffs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        voffs[i] = Stream(b.src, b.bytes, 16);
    }
    GLintptr ioff = 0;
    if (a.index_count) ioff = Stream(a.index_src, GLsizeiptr(a.index_count) * (a.index16 ? 2 : 4), 4);
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    GLuint samplers[4];
    for (int i = 0; i < 4; i++) {
        bool compare = false;
        for (const glshader::Pair& p : prog->pairs)
            if (p.sampler == i && p.shadow) compare = true;
        samplers[i] = StageSampler(s, i, stages, clamp, compare);
    }

    // the rendering
    CpuTimer gl_part{gl_ns_};
    BindFbo(draw_rt_ ? draw_rt_->tex : 0, draw_ds_ ? draw_ds_->tex : 0);
    if (draw_rt_) draw_rt_->drawn = true;
    if (draw_ds_) draw_ds_->drawn = true;
    ApplyState(k);
    if (k.has_ds && k.sten) {
        uint32_t sf[3] = {Cmp(k.sfunc), SimpleState(s, kMStencilRef) & 0xFF, k.smask};
        if (std::memcmp(sf, stencil_func_, sizeof(sf))) {
            glStencilFunc(sf[0], GLint(sf[1]), sf[2]);
            std::memcpy(stencil_func_, sf, sizeof(sf));
            calls_++;
        }
    }
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    if (std::memcmp(bf, blend_color_, sizeof(bf))) {
        glBlendColor(bf[0], bf[1], bf[2], bf[3]);
        std::memcpy(blend_color_, bf, sizeof(bf));
        calls_++;
    }
    UseProgram(prog->id);
    UniformRange(0, vc, kVsConstants);
    UniformRange(1, dc, sizeof(c));
    // a sampler slot a pair names with a comparison takes the comparison sampler, the others the plain one
    for (const glshader::Pair& p : prog->pairs) {
        GLuint t = p.tex < 4 ? stages.tex[p.tex] : dummy_[0];
        BindUnit(p.unit, p.tex < 4 ? stages.target[p.tex] : GL_TEXTURE_2D, t);
        GLuint smp = 0;
        if (p.sampler < 4) smp = p.shadow ? samplers[p.sampler] : StageSampler(s, p.sampler, stages, clamp, false);
        BindUnitSampler(p.unit, smp);
    }
    Target& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    Viewport(0, 0, GLsizei(bt.w), GLsizei(bt.h));
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
    Scissor(x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0));
    uint32_t mask = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        SetAttrib(r, e.format, e.offset, e.buffer);
        mask |= 1u << r;
    }
    SetAttribs(mask);
    for (uint32_t i = 0; i < a.buffer_count; i++) glBindVertexBuffer(i, stream_, voffs[i], GLsizei(a.buffers[i].stride));
    Topology topo = a.topology;
    GLenum mode = topo == Topology::Points ? GL_POINTS
                  : topo == Topology::Lines ? (a.strip ? GL_LINE_STRIP : GL_LINES)
                                            : (a.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES);
    if (a.index_count)
        glDrawElementsBaseVertex(mode, GLsizei(a.index_count), a.index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                 reinterpret_cast<const void*>(ioff), a.base_vertex);
    else
        glDrawArrays(mode, 0, GLsizei(a.draw_count));
    draws_++;
}

void GlBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
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
    state_valid_ = false;
    if (t.depth) {
        bool zc = flags & kClearZ, sc_ = flags & kClearStencil;
        if (!zc && !sc_) return;
        BindFbo(0, t.tex);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        float zv = std::clamp(z, 0.0f, 1.0f);
        GLint st = GLint(stencil & 0xFF);
        for (const R& r : rs) {
            Scissor(r.x, r.y, r.w, r.h);
            if (zc && sc_) glClearBufferfi(GL_DEPTH_STENCIL, 0, zv, st);
            else if (zc) glClearBufferfv(GL_DEPTH, 0, &zv);
            else glClearBufferiv(GL_STENCIL, 0, &st);
        }
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        BindFbo(t.tex, 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        float c[4];
        Color(color, c);
        for (const R& r : rs) {
            Scissor(r.x, r.y, r.w, r.h);
            glClearBufferfv(GL_COLOR, 0, c);
        }
    }
}

void GlBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
}

void GlBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "gl: CopyRects %08X to %08X skipped", src.data, dst.data);
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
        glCopyImageSubData(src_t->tex, GL_TEXTURE_2D, 0, GLint(l), GLint(t), 0, dst_t->tex, GL_TEXTURE_2D, 0, GLint(dx),
                           GLint(dy), 0, GLsizei(r - l), GLsizei(b - t), 1);
    }
    dst_t->drawn = true;
}

// ---- read-backs

void GlBackend::Sync(uint32_t data, uint32_t) {
    if (!Current()) return;
    CpuTimer timer{cpu_ns_};
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    CpuTimer sync_timer{sync_ns_};
    syncs_++;
    GLuint down = 0;
    GLuint src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(src, t.gw, t.gh, px);
    if (down) {
        DropFbos(down);
        DeleteTexture(down);
    }
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void GlBackend::SetAttrib(uint32_t r, uint32_t dxgi, uint32_t offset, uint32_t buffer) {
    AttribState& st = attrib_[r];
    if (st.format != dxgi || st.offset != offset) {
        AttribFormat f = VertexFormat(dxgi);
        if (f.integer)
            glVertexAttribIFormat(r, f.size, f.type, offset);
        else
            glVertexAttribFormat(r, f.size, f.type, f.norm ? GL_TRUE : GL_FALSE, offset);
        st.format = dxgi;
        st.offset = offset;
        calls_++;
    } else {
        skipped_++;
    }
    if (st.buffer != buffer) {
        glVertexAttribBinding(r, buffer);
        st.buffer = buffer;
        calls_++;
    }
}

// ---- the options panel: the hosts made the ImGui context (main.cpp); here its font atlas and its draw lists

bool GlBackend::InitUi() {
    std::lock_guard lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "gl";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!px || w <= 0 || h <= 0) return false;
    ui_font_ = NewTexture(GL_TEXTURE_2D, GL_RGBA8, uint32_t(w), uint32_t(h), 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID(ImTextureID(intptr_t(ui_font_)));
    LOGI("gl: options panel ready (font atlas %dx%d)", w, h);
    return true;
}

// on the window's framebuffer, its lower left origin set: ImGui's y down becomes clip space's y up
void GlBackend::RenderUi() {
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
    GLsizeiptr need = 64;
    for (const ImDrawList* l : dd->CmdLists)
        need += GLsizeiptr(l->VtxBuffer.Size) * GLsizeiptr(sizeof(ImDrawVert)) + GLsizeiptr(l->IdxBuffer.Size) * 2 + 32;
    EnsureStream(need);
    PlainState(false, false);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    UseProgram(p->id);
    const float W = float(out_w_), H = float(out_h_);
    float xform[4] = {2.0f / dd->DisplaySize.x, -2.0f / dd->DisplaySize.y, -1.0f - 2.0f * dd->DisplayPos.x / dd->DisplaySize.x,
                      1.0f + 2.0f * dd->DisplayPos.y / dd->DisplaySize.y};
    GLintptr at = Uniforms(xform, sizeof(xform));
    UniformRange(1, at, sizeof(xform));
    Viewport(0, 0, int32_t(out_w_), int32_t(out_h_));
    SetAttrib(0, DXGI_FORMAT_R32G32_FLOAT, offsetof(ImDrawVert, pos), 0);
    SetAttrib(1, DXGI_FORMAT_R32G32_FLOAT, offsetof(ImDrawVert, uv), 0);
    SetAttrib(2, DXGI_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col), 0);
    SetAttribs(0x7);
    const GLenum tg[4] = {GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D};
    const GLuint smp[4] = {FixedSampler(true), FixedSampler(true), FixedSampler(true), FixedSampler(true)};
    for (const ImDrawList* l : dd->CmdLists) {
        GLintptr voff = Stream(l->VtxBuffer.Data, GLsizeiptr(l->VtxBuffer.Size) * GLsizeiptr(sizeof(ImDrawVert)), 16);
        GLintptr ioff = Stream(l->IdxBuffer.Data, GLsizeiptr(l->IdxBuffer.Size) * GLsizeiptr(sizeof(ImDrawIdx)), 4);
        glBindVertexBuffer(0, stream_, voff, GLsizei(sizeof(ImDrawVert)));
        for (const ImDrawCmd& c : l->CmdBuffer) {
            if (c.UserCallback || !c.ElemCount) continue;
            float x0 = std::max(c.ClipRect.x - dd->DisplayPos.x, 0.0f), y0 = std::max(c.ClipRect.y - dd->DisplayPos.y, 0.0f);
            float x1 = std::min(c.ClipRect.z - dd->DisplayPos.x, W), y1 = std::min(c.ClipRect.w - dd->DisplayPos.y, H);
            if (x1 <= x0 || y1 <= y0) continue;
            // the window's rows count up from its bottom
            Scissor(int32_t(x0), int32_t(H - y1), int32_t(x1 - x0), int32_t(y1 - y0));
            const GLuint tex[4] = {GLuint(intptr_t(c.GetTexID())), 0, 0, 0};
            BindPairs(*p, tex, tg, smp);
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(c.ElemCount), sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                     reinterpret_cast<const void*>(ioff + GLintptr(c.IdxOffset * sizeof(ImDrawIdx))),
                                     GLint(c.VtxOffset));
        }
    }
    state_valid_ = false;
}

// ---- present

void GlBackend::DrawOverlay(Target& t) {
    if (!overlay_on_) return;
    if (!overlay_ || overlay_tw_ != overlay_w_ || overlay_th_ != overlay_h_) {
        if (overlay_) {
            DropFbos(overlay_);
            DeleteTexture(overlay_);
        }
        overlay_ = NewTexture(GL_TEXTURE_2D, GL_RGBA8, overlay_w_, overlay_h_, 1, 1);
        overlay_tw_ = overlay_w_;
        overlay_th_ = overlay_h_;
        overlay_new_ = true;
    }
    if (overlay_new_) {
        ActiveUnit(0);  // uploads go to unit 0: a skipped bind leaves another unit active
        BindUnit(0, GL_TEXTURE_2D, overlay_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(overlay_w_), GLsizei(overlay_h_), GL_BGRA, GL_UNSIGNED_BYTE,
                        overlay_px_.data());
        overlay_new_ = false;
    }
    float sx = float(t.w) / std::max(t.gw, 1u), sy = float(t.h) / std::max(t.gh, 1u);
    int32_t l = int32_t(overlay_rect_[0] * sx), tp = int32_t(overlay_rect_[1] * sy);
    int32_t r = int32_t(overlay_rect_[2] * sx), b = int32_t(overlay_rect_[3] * sy);
    if (r <= l || b <= tp) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.tex, t.w, t.h, l, tp, r - l, b - tp, overlay_, uv, {});
}

void GlBackend::UploadGammaRamp() {
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
    ActiveUnit(0);  // uploads go to unit 0: a skipped bind leaves another unit active
    BindUnit(0, GL_TEXTURE_2D, lut_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_BGRA, GL_UNSIGNED_BYTE, px);
}

void GlBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            DeleteTexture(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void GlBackend::ApplySettings() {
    WindowSize(out_w_, out_h_);
    float want, want_x;
    WantScale(want, want_x);
    if (std::fabs(want - scale_) <= 1e-4f && std::fabs(want_x - scale_x_) <= 1e-4f) return;
    while (!targets_.empty()) ReleaseTarget(targets_.begin());
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    scale_x_ = want_x;
    LOGI("gl: render scale %.3f x %.3f", scale_x_, scale_);
}

// the output's height over the guest's (render_percent of it); render_wide: horizontally, the share of the output's
// width the frame is shown on over the guest's width (MP2 widescreen: 1280 / 640 = 2 against 720 / 480 = 1.5)
void GlBackend::WantScale(float& y, float& x) {
    y = std::max(out_h_ * (app::Get().render_percent / 100.0f) / guest_h_, 0.1f);
    x = y;
    if (game::Opt().render_wide && guest_h_) {
        float shown = std::min(float(out_w_), out_h_ * game::Aspect());
        x = std::max(y, shown * (app::Get().render_percent / 100.0f) / guest_w_);
    }
}

// emulator and console test runs: a "capture" file beside the NRO saves the presented frame as shots/NNNN.bmp (24
// bit, the guest's size), as the soft renderer does. The file's text is the interval in seconds: empty for 15, "0"
// (or "off") for none, so a console run can switch captures off without deleting the file (each shot stalls the
// game while it is written: about a second on the console's SD since the one-write BMP below, 10-20 s before).
bool GlBackend::CaptureDue() {
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
        LOGI("gl: capture file: %s", s > 0 ? (std::to_string(s) + " s").c_str() : "off");
        return s;
    }();
    if (every <= 0) return false;
    auto now = std::chrono::steady_clock::now();
    if (shots_ && now - last_shot_ < std::chrono::seconds(every)) return false;
    last_shot_ = now;
    return true;
}

void GlBackend::Capture(GLuint tex, uint32_t w, uint32_t h) {
    std::vector<uint8_t> px;
    if (!ReadBack(tex, w, h, px)) return;
    char name[32];
    std::snprintf(name, sizeof(name), "%04u.bmp", shots_++);
    WriteBmp(name, px, w, h, true);
}

// a 24 bit BMP in shots/; top_down: px's first row is the picture's top (guest targets), else its bottom (the window)
void GlBackend::WriteBmp(const char* name, const std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool top_down) {
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
    // the whole file in one write: row by row it took 10-20 s a shot on the console's SD
    std::vector<uint8_t> file(size, 0);
    std::memcpy(file.data(), hd, 54);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* line = file.data() + 54 + size_t(y) * row;
        for (uint32_t x = 0; x < w; x++) std::memcpy(line + x * 3, &px[(size_t(y) * w + x) * 4], 3);
    }
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    LOGI("gl: shot %s", name);
}

// gl_flags.txt "selftest": clears and full-viewport draws read back, how much of a row each covered (for checking a
// driver or an emulator: under Ryujinx every draw covered only the left quarter of its target)
void GlBackend::SelfTest() {
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
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float red[4] = {1, 0, 0, 1};
    std::vector<uint8_t> px;
    uint32_t last = 0;
    GLuint green = NewTexture(GL_TEXTURE_2D, GL_RGBA8, 1, 1, 1, 1);
    const uint8_t g[4] = {0, 255, 0, 255};  // BGRA
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, g);
    const uint32_t sizes[2][2] = {{256, 64}, {960, 720}};
    for (const auto& sz : sizes) {
        uint32_t W = sz[0], H = sz[1];
        GLuint tex = NewTexture(GL_TEXTURE_2D, GL_RGBA8, W, H, 1, 1);
        BindFbo(tex, 0);
        glClearBufferfv(GL_COLOR, 0, red);
        ReadPixelsBgra(W, H, px);
        uint32_t n = covered(px, W, H / 2, 2, last);
        LOGI("gl: selftest %ux%u clear: %u of %u red in the middle row, last at %u", W, H, n, W, last);
        for (int origin = 0; origin < 2; origin++) {
            BindFbo(tex, 0);
            glClearBufferfv(GL_COLOR, 0, red);
            glClipControl(origin ? GL_LOWER_LEFT : GL_UPPER_LEFT, GL_ZERO_TO_ONE);
            float uv[4] = {0, 0, 1, 1};
            Blit(tex, W, H, 0, 0, int32_t(W), int32_t(H), green, uv, {});
            BindFbo(tex, 0);
            ReadPixelsBgra(W, H, px);
            n = covered(px, W, H / 2, 1, last);
            LOGI("gl: selftest %ux%u draw, %s origin: %u of %u green in the middle row, last at %u", W, H,
                 origin ? "lower-left" : "upper-left", n, W, last);
        }
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        DropFbos(tex);
        DeleteTexture(tex);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_ = 0;
    glClearBufferfv(GL_COLOR, 0, red);
    ReadPixelsBgra(out_w_, out_h_, px);
    uint32_t n = covered(px, out_w_, out_h_ / 2, 2, last);
    LOGI("gl: selftest window clear %ux%u: %u of %u red in the middle row, last at %u", out_w_, out_h_, n, out_w_, last);
    DeleteTexture(green);
    if (scissor_) glEnable(GL_SCISSOR_TEST);
    state_valid_ = false;
}

void GlBackend::Present(const Surface& front) {
    if (!Current()) return;
    auto present_t0 = std::chrono::steady_clock::now();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) DrawOverlay(*t);
#ifdef _WIN32
    if (t && host::TakeShot()) {
        GLuint down = 0;
        GLuint src = CopySource(*t, down);
        std::vector<uint8_t> px;
        if (src && ReadBack(src, t->gw, t->gh, px)) host::SaveShot(px.data(), t->gw, t->gh);
        if (down) {
            DropFbos(down);
            DeleteTexture(down);
        }
    }
#endif
    const bool shot = t && CaptureDue();
    const uint32_t shot_index = shots_;
    if (shot) {
        GLuint down = 0;
        GLuint src = CopySource(*t, down);
        if (src) Capture(src, t->gw, t->gh);
        if (down) {
            DropFbos(down);
            DeleteTexture(down);
        }
    }
    if (screen::Gamma() != lift_) ramp_dirty_ = true;
    if (ramp_dirty_) UploadGammaRamp();
    // the frame letterboxed to the game's aspect over black, on the window's own (lower left) origin
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_ = 0;
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    float aspect = game::Aspect();
    float w = float(out_w_), h = float(out_h_);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    Scissor(0, 0, GLsizei(out_w_), GLsizei(out_h_));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float black[4] = {0, 0, 0, 1};
    glClearBufferfv(GL_COLOR, 0, black);
    if (t) {
        float uv[4] = {0, 0, 1, 1};
        BlitOptions o;
        o.gamma = gamma_;
        Blit(0, out_w_, out_h_, int32_t(std::lround((w - vw) / 2)), int32_t(std::lround((h - vh) / 2)),
             int32_t(std::lround(vw)), int32_t(std::lround(vh)), t->tex, uv, o);
    }
    RenderUi();
    if (shot) {
        // what the screen shows, beside the guest target's shot: shots/NNNN_screen.bmp
        std::vector<uint8_t> px;
        ReadPixelsBgra(out_w_, out_h_, px);
        char name[40];
        std::snprintf(name, sizeof(name), "%04u_screen.bmp", shot_index);
        WriteBmp(name, px, out_w_, out_h_, false);
    }
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    cpu_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - present_t0).count());
    // a frame whose renderer time is long (over 20 ms, the first 40 and then every 64th): its parts, to tell the
    // renderer's share of the once-a-second long frames the console shows from the game's
    if (uint64_t f = cpu_ns_ - frame_cpu0_; presents_ > 600 && f > 20000000ull) {
        if (long_logged_ < 40 || long_logged_ % 64 == 0)
            LOGI("gl: long frame %llu: renderer %.1f ms (assembly %.1f, textures %.1f, GL calls %.1f), %llu KB "
                 "uploaded, %llu draws, %zu textures",
                 (unsigned long long)presents_, double(f) / 1e6, double(asm_ns_ - frame_asm0_) / 1e6,
                 double(tex_ns_ - frame_tex0_) / 1e6, double(gl_ns_ - frame_gl0_) / 1e6,
                 (unsigned long long)((up_bytes_ - frame_up0_) / 1024), (unsigned long long)(draws_ - frame_draws0_),
                 textures_.size());
        long_logged_++;
    }
    // the GPU's lag: a timestamp after the frame's last command, against the GL time it was issued at; the result
    // read kGpuQueries frames later (skipped while not available)
    if (gpu_q_[0]) {
        int slot = int(presents_ % kGpuQueries);
        if (gpu_q_used_[slot]) {
            GLint avail = 0;
            glGetQueryObjectiv(gpu_q_[slot], GL_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                GLuint64 done = 0;
                glGetQueryObjectui64v(gpu_q_[slot], GL_QUERY_RESULT, &done);
                int64_t lag = int64_t(done) - int64_t(gpu_issue_[slot]);
                if (lag > 0) {
                    gpu_lag_ns_ += uint64_t(lag);
                    gpu_lag_max_ns_ = std::max(gpu_lag_max_ns_, uint64_t(lag));
                }
                gpu_lag_n_++;
            }
        }
        glQueryCounter(gpu_q_[slot], GL_TIMESTAMP);
        glGetInteger64v(GL_TIMESTAMP, &gpu_issue_[slot]);
        gpu_q_used_[slot] = true;
    }
    SwapWindow();
    presents_++;
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
    if (presents_ % 300 == 1)
    {
        LOGI("gl: present %llu, %llu draws, %zu textures, %zu targets, %zu programs; a frame: %llu KB uploaded, "
             "%llu state calls, %llu skipped as unchanged, renderer %.2f ms on the title's thread; %llu read-backs "
             "(%.1f ms) in 300 frames",
             (unsigned long long)presents_, (unsigned long long)draws_, textures_.size(), targets_.size(), programs_.size(),
             (unsigned long long)(up_bytes_ / 300 / 1024), (unsigned long long)(calls_ / 300),
             (unsigned long long)(skipped_ / 300), double(cpu_ns_) / 300 / 1e6, (unsigned long long)syncs_,
             double(sync_ns_) / 1e6);
        LOGI("gl: renderer parts a frame: assembly %.2f, textures %.2f, GL calls %.2f, the rest %.2f ms; ring fence "
             "waits %llu (%.1f ms); GPU lag at the frame's end %.2f ms (longest %.2f, %llu frames read)",
             double(asm_ns_) / 300 / 1e6, double(tex_ns_) / 300 / 1e6, double(gl_ns_) / 300 / 1e6,
             double(cpu_ns_ - std::min(cpu_ns_, asm_ns_ + tex_ns_ + gl_ns_)) / 300 / 1e6,
             (unsigned long long)ring_waits_, double(ring_wait_ns_) / 1e6,
             gpu_lag_n_ ? double(gpu_lag_ns_) / double(gpu_lag_n_) / 1e6 : 0.0, double(gpu_lag_max_ns_) / 1e6,
             (unsigned long long)gpu_lag_n_);
        up_bytes_ = calls_ = skipped_ = cpu_ns_ = syncs_ = sync_ns_ = asm_ns_ = tex_ns_ = gl_ns_ = 0;
        ring_waits_ = ring_wait_ns_ = gpu_lag_ns_ = gpu_lag_n_ = gpu_lag_max_ns_ = 0;
#ifdef __SWITCH__
        // the title's thread, to find it in the thread census (host-switch/clocks.cpp, "threads:" lines)
        LOGI("gl: the title's thread is handle %X, on core %u now", unsigned(threadGetCurHandle()),
             unsigned(svcGetCurrentProcessorNumber()));
#endif
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

GlBackend* g_gl = nullptr;

}  // namespace

#ifdef GL_BACKEND_DEFAULT
std::unique_ptr<Backend> CreateBackend() {
#else
std::unique_ptr<Backend> CreateGlBackend() {
#endif
    auto backend = std::make_unique<GlBackend>();
    g_gl = backend.get();
    return backend;
}

#ifdef GL_BACKEND_DEFAULT
// the swap with vsync paces the frames: nothing to wait for
void WaitGpuFrame(uint64_t) {}
#endif

}  // namespace gpu
