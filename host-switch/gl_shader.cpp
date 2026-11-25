// MP2-nx: SPIR-V to GLSL for the OpenGL renderer, through SPIRV-Cross (third_party/spirv-cross, Apache-2.0)
#include "gl_shader.h"

#include <exception>

#include "core/log.h"
#include "spirv_glsl.hpp"
#include "vk_shader.h"

namespace glshader {

bool ToGlsl(const std::vector<uint32_t>& spirv, bool vertex, uint32_t first_unit, std::string& glsl,
            std::vector<Pair>& pairs) {
    pairs.clear();
    try {
        spirv_cross::CompilerGLSL c(spirv);
        spirv_cross::CompilerGLSL::Options o = c.get_common_options();
        o.version = 430;
        o.es = false;
        o.vulkan_semantics = false;
        o.enable_420pack_extension = true;
        // the clip space stays Vulkan's: the renderer sets glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)
        o.vertex.fixup_clipspace = false;
        o.vertex.flip_vert_y = false;
        c.set_common_options(o);
        spirv_cross::VariableID dummy = c.build_dummy_sampler_for_combined_images();
        c.build_combined_image_samplers();
        uint32_t unit = first_unit;
        for (const auto& cs : c.get_combined_image_samplers()) {
            Pair p;
            p.unit = uint8_t(unit);
            p.tex = uint8_t(c.get_decoration(cs.image_id, spv::DecorationBinding) - vkshader::kBindingTextures);
            p.sampler = cs.sampler_id == dummy && dummy != 0
                            ? kNoSampler
                            : uint8_t(c.get_decoration(cs.sampler_id, spv::DecorationBinding) - vkshader::kBindingSamplers);
            c.set_decoration(cs.combined_id, spv::DecorationBinding, unit);
            c.unset_decoration(cs.combined_id, spv::DecorationDescriptorSet);
            c.set_name(cs.combined_id, "u_tex" + std::to_string(unit));
            pairs.push_back(p);
            unit++;
        }
        spirv_cross::ShaderResources res = c.get_shader_resources();
        for (const auto& u : res.uniform_buffers) c.unset_decoration(u.id, spv::DecorationDescriptorSet);
        glsl = c.compile();
    } catch (const std::exception& e) {
        LOGE("gl: %s shader to GLSL failed: %s", vertex ? "vertex" : "pixel", e.what());
        return false;
    }
    // which units sample with a comparison: their declaration says so
    for (Pair& p : pairs) {
        std::string name = "u_tex" + std::to_string(p.unit) + ";";
        size_t at = glsl.find(name);
        if (at == std::string::npos) continue;
        size_t line = glsl.rfind('\n', at);
        std::string decl = glsl.substr(line == std::string::npos ? 0 : line, at - (line == std::string::npos ? 0 : line));
        p.shadow = decl.find("Shadow") != std::string::npos;
    }
    return true;
}

}  // namespace glshader
