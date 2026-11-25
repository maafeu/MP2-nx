# third_party

Not in git (`.gitignore`): local copies of the trees the kit's builds use. The kit's own instructions fetch them from
GitHub; this port copies them from renut-nx's tree on the build PC instead (no downloads):

| Folder | Copied from | Version | Licence |
| --- | --- | --- | --- |
| `tomlplusplus/` | `Renut-Port/banjo-nx/sdk/thirdparty/tomlplusplus` | 3.x | MIT |
| `glslang/` | `Renut-Port/banjo-nx/sdk/thirdparty/glslang` | with HLSL | BSD-3 / others (its LICENSE.txt) |
| `imgui/` | `Renut-Port/banjo-nx/sdk/thirdparty/imgui` | 1.92.5 (the kit was written for 1.91.0) | MIT |
| `vulkan/` | Vulkan headers from renut-nx's plume (`app/thirdparty/plume/contrib`); `Lib/vulkan-1.lib` made from `System32\vulkan-1.dll` with `llvm-dlltool` | 1.3 | Apache-2.0 / MIT |
| `spirv-cross/` | `SSX_Build/downloads/SPIRV-Cross-22b22f56` (16 files: `spirv_cross`, `spirv_parser`, `spirv_cross_parsed_ir`, `spirv_cfg`, `spirv_glsl` .cpp/.hpp, `spirv_common.hpp`, `spirv_cross_containers.hpp`, `spirv_cross_error_handling.hpp`, `spirv.hpp`, `GLSL.std.450.h`, `LICENSE`) | 22b22f56 | Apache-2.0 |
| `gl/` | `GL/glcorearb.h` and `KHR/khrplatform.h` from devkitPro's `portlibs/switch/include` (the PC build's GL headers; the Switch build uses portlibs directly) | Khronos 2018 | Khronos (MIT-style) |
| `uam/` | `renut-ultimate/base/app/thirdparty/uam` (the ultimate base): devkitPro's deko3d shader compiler as a library (`uam_api.h`), with the SwitchWakerHD / banjo-nx patches in its `PATCHES.md`; the deko3d renderer (`PORT_RENDERER=dk`) compiles its shaders with it on the console | uam 1.1.0 | zlib (uam), MIT (`mesa-imported/`) |

The x86 decoder is not here: `tools/zydis-lite` replaces Zydis (docs/MP2-notes.md, Decision 3).
