#pragma once
// MP2-nx: the OpenGL renderer's shaders (gl_backend.cpp). The same HLSL the Vulkan renderer compiles goes to SPIR-V
// through vk_shader.cpp (glslang, its disk cache), then to GLSL 4.30 through SPIRV-Cross. Vulkan's separate textures
// and samplers become combined samplers, one texture unit per pair the shader uses; Pair says which of the draw's
// textures (t0-t3) and samplers (s0-s3) each unit takes.
#include <cstdint>
#include <string>
#include <vector>

namespace glshader {

struct Pair {
    uint8_t unit = 0;
    uint8_t tex = 0;        // t register
    uint8_t sampler = 0;    // s register, kNoSampler for a texel fetch
    uint8_t shadow = 0;     // sampled with a depth comparison (sampler2DShadow)
};
constexpr uint8_t kNoSampler = 0xFF;

// spirv: a module vk_shader.cpp made (bindings b0 b1 at 0 1, t0-t3 at 2-5, s0-s3 at 6-9); units numbered from
// first_unit. false after logging why
bool ToGlsl(const std::vector<uint32_t>& spirv, bool vertex, uint32_t first_unit, std::string& glsl,
            std::vector<Pair>& pairs);

}  // namespace glshader
