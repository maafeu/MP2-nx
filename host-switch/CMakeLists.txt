# An original Xbox game, statically recompiled with XboxXBE-Recomp, as a Switch homebrew (NRO). The game's folder
# (PORT_GAME_DIR) has its sources (src/), its recompiler config (config/game.toml), the xbe (assets/default.xbe), the
# recompiled code (generated/game, from ferramentas/recompilar.ps1) and port.cmake, which names the port. This folder
# is the Switch host; the SDK's portable runtime is in sdk/XboxXBE-Recomp.
#
# ferramentas/build_switch.sh <game folder> runs this:
# cmake -S host-switch -B build/<name> -G Ninja -DPORT_GAME_DIR=<game folder>
#       -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release

cmake_minimum_required(VERSION 3.22)
project(xbe_switch_port LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(KIT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(HOST ${CMAKE_CURRENT_SOURCE_DIR})
set(SDK ${KIT}/sdk/XboxXBE-Recomp)
set(THIRD ${KIT}/third_party)
set(NVK_SDK ${KIT}/nvk CACHE PATH "mesa-switch (NVK): include/ and the static libvulkan")

set(PORT_GAME_DIR "" CACHE PATH "the game's folder: src/, config/game.toml, assets/, generated/, port.cmake")
if(NOT PORT_GAME_DIR OR NOT EXISTS ${PORT_GAME_DIR}/port.cmake)
    message(FATAL_ERROR "PORT_GAME_DIR must be a game folder with port.cmake (exemplos/black has one)")
endif()
get_filename_component(PORT_GAME_DIR ${PORT_GAME_DIR} ABSOLUTE)
set(GAME_DIR ${PORT_GAME_DIR})
set(GAME ${GAME_DIR}/src)

# what port.cmake sets; the defaults are for a game that needs nothing more
set(PORT_NAME "")                       # the NRO's name and its folder on the card, sdmc:/switch/<PORT_NAME>
set(PORT_TITLE "")                      # the title in the menu and in the error messages
set(PORT_AUTHOR "XboxXBE-Recomp")
set(PORT_VERSION "0.1.0")
set(PORT_ICON ${GAME_DIR}/icon.jpg)     # 256x256 jpg, optional
set(PORT_XBE_PATCH "")                  # optional: a patch turning another release's xbe into the recompiled one
set(PORT_GYRO_MODE 0)                   # the gyro before the player picks: 0 off, 1 always, 2 ZR held, 3 ZL held
set(PORT_XBE_PATCH_SHA1 "")             # the SHA-1 of the xbe that patch applies to
include(${GAME_DIR}/port.cmake)
if(NOT PORT_NAME OR NOT PORT_TITLE)
    message(FATAL_ERROR "port.cmake must set PORT_NAME and PORT_TITLE")
endif()

# MaxPayne2-nx: the renderer. nvk = host-switch/vulkan.cpp on the NVK driver in nvk/ (the kit's); gl = gl_backend.cpp, the same
# draws through OpenGL 4.3 on devkitPro's switch-mesa (nouveau, EGL), the GPU renderer when no NVK build is at hand;
# soft = the SDK's CPU reference renderer and libnx's framebuffer (soft_backend.cpp): no GPU, slow, for bring-up;
# dk = dk_backend.cpp, the same draws through deko3d (libnx's, the GPU's own API: no Mesa on the CPU), its shaders
# compiled on the console by uam (third_party/uam)
if(EXISTS ${NVK_SDK}/lib/libvulkan.a)
    set(PORT_RENDERER_DEFAULT nvk)
# MaxPayne2-nx (2026-11-22): deko3d the default once checked on the console (chapter 1 locked at 60 fps at 1020 MHz where
# GL gave 57; docs/MP2-notes.md, "The deko3d renderer")
elseif(EXISTS ${DEVKITPRO}/libnx/lib/libdeko3d.a AND EXISTS ${KIT}/third_party/uam/uam_api.h)
    set(PORT_RENDERER_DEFAULT dk)
elseif(EXISTS ${DEVKITPRO}/portlibs/switch/lib/libEGL.a)
    set(PORT_RENDERER_DEFAULT gl)
else()
    set(PORT_RENDERER_DEFAULT soft)
endif()
set(PORT_RENDERER ${PORT_RENDERER_DEFAULT} CACHE STRING "nvk, gl, dk or soft")
message(STATUS "renderer: ${PORT_RENDERER}")

# the recompiled code and the xbe it came from
set(GEN ${GAME_DIR}/generated/game)
set(XBE ${GAME_DIR}/assets/default.xbe)
if(NOT EXISTS ${GEN}/x86_func_mapping.cpp OR NOT EXISTS ${XBE})
    message(FATAL_ERROR "No recompiled code in ${GEN}: run ferramentas/recompilar.ps1 ${GAME_DIR}")
endif()
file(STRINGS ${GAME_DIR}/config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
math(EXPR last "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
foreach(i RANGE ${last})
    list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
endforeach()
file(SHA1 ${XBE} xbe_sha1)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/build_info.h CONTENT
     "#define PORT_BUILD \"switch-${PORT_VERSION}\"\n#define PORT_NAME \"${PORT_NAME}\"\n#define PORT_TITLE \"${PORT_TITLE}\"\n#define PORT_GYRO_MODE ${PORT_GYRO_MODE}\n")

# glslang with its HLSL front end (the SDK's shaders are HLSL), built here for the Switch
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# its precompiled header is named by an MSYS path (/c/...) the Windows-hosted GCC cannot open
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
# MaxPayne2-nx: with msys2's make, GCC's dependency files (C:/... paths) of targets in a subdirectory become rules make
# cannot parse ("multiple target patterns" in glslang's compiler_depend.make). Off for glslang's directory only (its
# sources never change; build_switch.sh writes the stamps CMake still names); this directory keeps them, so a header
# change rebuilds what includes it (with them off everywhere it did not: depend.make stayed empty)
if(NOT PORT_RENDERER STREQUAL "soft")
set(CMAKE_DEPENDS_USE_COMPILER FALSE)
add_subdirectory(${THIRD}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
set(CMAKE_DEPENDS_USE_COMPILER TRUE)
endif()
# MaxPayne2-nx: glslang adds its OSDependent library for Windows and Unix targets only, and the Switch toolchain is
# neither ("cannot find -lOSDependent"): its pthread version, which libnx's newlib carries
if(NOT PORT_RENDERER STREQUAL "soft" AND NOT TARGET OSDependent)
    add_library(OSDependent STATIC ${THIRD}/glslang/glslang/OSDependent/Unix/ossource.cpp)
    # glslang's own install export names it (nothing is installed)
    install(TARGETS OSDependent EXPORT glslangTargets ARCHIVE DESTINATION lib)
endif()
# glslang before 12 defines glslang-default-resource-limits only with its binaries: build its source here
if(NOT PORT_RENDERER STREQUAL "soft" AND NOT TARGET glslang-default-resource-limits)
    add_library(glslang-default-resource-limits STATIC ${THIRD}/glslang/StandAlone/ResourceLimits.cpp)
    target_include_directories(glslang-default-resource-limits PUBLIC ${THIRD}/glslang)
endif()
add_subdirectory(${THIRD}/tomlplusplus ${CMAKE_CURRENT_BINARY_DIR}/tomlplusplus EXCLUDE_FROM_ALL)
set(IMGUI_SOURCES ${THIRD}/imgui/imgui.cpp ${THIRD}/imgui/imgui_draw.cpp ${THIRD}/imgui/imgui_tables.cpp
    ${THIRD}/imgui/imgui_widgets.cpp)
if(PORT_RENDERER STREQUAL "nvk")
    list(APPEND IMGUI_SOURCES ${THIRD}/imgui/backends/imgui_impl_vulkan.cpp)
endif()

# the SDK's runtime but its Windows parts, and those the game's sources replace (src/sound, src/d3d)
file(GLOB RUNTIME_SOURCES ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|render_thread)\\.cpp$|/audio/xaudio2\\.cpp$|/xdk/(dsound|d3d8)\\.cpp$")
file(GLOB GAME_HOST_SOURCES CONFIGURE_DEPENDS ${GAME}/*/*.cpp)
# the game's own Switch-only code (traces, workarounds): <game>/switch/*.cpp
file(GLOB GAME_SWITCH_SOURCES CONFIGURE_DEPENDS ${GAME_DIR}/switch/*.cpp)
file(GLOB SWITCH_SOURCES CONFIGURE_DEPENDS ${HOST}/*.cpp)
set(SPIRV_CROSS_SOURCES "")
if(PORT_RENDERER STREQUAL "soft")
    list(FILTER SWITCH_SOURCES EXCLUDE REGEX "/(vulkan|vk_shader|gl_backend|gl_shader|dk_backend)\.cpp$")
    list(APPEND SWITCH_SOURCES ${SDK}/harness/reference_renderer.cpp)
elseif(PORT_RENDERER STREQUAL "gl" OR PORT_RENDERER STREQUAL "dk")
    if(PORT_RENDERER STREQUAL "gl")
        list(FILTER SWITCH_SOURCES EXCLUDE REGEX "/(vulkan|soft_backend|dk_backend)\.cpp$")
    else()
        list(FILTER SWITCH_SOURCES EXCLUDE REGEX "/(vulkan|soft_backend|gl_backend)\.cpp$")
    endif()
    set(SPIRV_CROSS_SOURCES ${THIRD}/spirv-cross/spirv_cross.cpp ${THIRD}/spirv-cross/spirv_parser.cpp
        ${THIRD}/spirv-cross/spirv_cross_parsed_ir.cpp ${THIRD}/spirv-cross/spirv_cfg.cpp ${THIRD}/spirv-cross/spirv_glsl.cpp)
else()
    list(FILTER SWITCH_SOURCES EXCLUDE REGEX "/(soft_backend|gl_backend|gl_shader|dk_backend)\.cpp$")
endif()
# MaxPayne2-nx: the deko3d renderer's shader compiler, uam as a library (third_party/uam: its own CMakeLists, uamlib)
if(PORT_RENDERER STREQUAL "dk")
    set(CMAKE_DEPENDS_USE_COMPILER FALSE)
    add_subdirectory(${THIRD}/uam ${CMAKE_CURRENT_BINARY_DIR}/uam EXCLUDE_FROM_ALL)
    set(CMAKE_DEPENDS_USE_COMPILER TRUE)
endif()
set(HOST_SOURCES ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${GAME_SWITCH_SOURCES} ${SWITCH_SOURCES})
set(HOST_INCLUDES ${HOST}/compat ${SDK}/harness/compat ${HOST} ${GAME} ${SDK}/runtime ${GEN}
    ${CMAKE_CURRENT_BINARY_DIR} ${THIRD}/glslang ${NVK_SDK}/include ${SDK}/harness ${THIRD}/spirv-cross)

add_executable(${PORT_NAME} ${HOST_SOURCES} ${GEN_SOURCES} ${IMGUI_SOURCES} ${SPIRV_CROSS_SOURCES}
               ${HOST}/switch_elf_stubs.c ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp)
target_include_directories(${PORT_NAME} PRIVATE ${HOST_INCLUDES})
target_include_directories(${PORT_NAME} SYSTEM PRIVATE ${THIRD}/imgui)
# x86 chars are signed, as the game's code and the runtime were written for
target_compile_options(${PORT_NAME} PRIVATE -fno-strict-aliasing -fsigned-char)
# ImGui's open-in-browser uses fork/exec, which the Switch has not
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_DEFINITIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${HOST}/compat/crt.h")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing;-fsigned-char")
set_source_files_properties(${HOST}/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
# MaxPayne2-nx: profile-guided optimisation of the recompiled code and the host (docs/MP2-notes.md, "The game's own code").
# generate: counters in every object, written by the port itself (host-switch/pgo.cpp) to pgo/ on the card during a
# training run; use: the build reads them back (tools/pgo_collect.py puts them beside this build's objects; the same
# build folder for both). Code the training never ran is optimised as without a profile (-fprofile-partial-training).
set(PORT_PGO "" CACHE STRING "empty, generate or use")
# -fno-profile-values: libgcov's value profilers (the indirect call one) keep their state in thread-local variables,
# and devkitPro's prebuilt libgcov does not agree with libnx's thread-local layout: the first indirect call read
# address 0x2000 (Ryujinx, 2026-11-22). The block and edge counts, PGO's main use, need none.
if(PORT_PGO STREQUAL "generate")
    set_property(SOURCE ${GEN_SOURCES} ${HOST_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
                 -fprofile-generate -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
    set_property(SOURCE ${HOST}/pgo.cpp APPEND PROPERTY COMPILE_DEFINITIONS MP2_PGO_GENERATE)
    target_link_options(${PORT_NAME} PRIVATE -fprofile-generate)
    message(STATUS "PGO: instrumented (training) build")
elseif(PORT_PGO STREQUAL "use")
    set_property(SOURCE ${GEN_SOURCES} ${HOST_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
                 -fprofile-use -fno-profile-values -fprofile-partial-training -fprofile-correction -Wno-missing-profile
                 -Wno-coverage-mismatch)
    message(STATUS "PGO: built with the training run's profile")
endif()
# MaxPayne2-nx: link-time optimisation of the recompiled code and the host (docs/MP2-notes.md, "60 at 1020 MHz"). The
# generated code calls every function through its weak alias (sub_X, which a hand-written replacement may take over),
# and GCC never inlines a call to a weak symbol, not even within one file; at link time the linker plugin tells it which
# definitions prevail, so small game functions can inline into their callers across all the files. The same semantics.
option(PORT_LTO "link-time optimisation of the recompiled code and the host" OFF)
set(PORT_LTO_JOBS 12 CACHE STRING "link-time compile jobs")
if(PORT_LTO)
    set_property(SOURCE ${GEN_SOURCES} ${HOST_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -flto=${PORT_LTO_JOBS})
    target_link_options(${PORT_NAME} PRIVATE -flto=${PORT_LTO_JOBS} -O2)
    message(STATUS "LTO: on, ${PORT_LTO_JOBS} jobs")
endif()
if(PORT_XBE_PATCH)
    if(NOT EXISTS ${PORT_XBE_PATCH} OR NOT PORT_XBE_PATCH_SHA1)
        message(FATAL_ERROR "PORT_XBE_PATCH needs the patch file and PORT_XBE_PATCH_SHA1")
    endif()
    set_source_files_properties(${HOST}/game_files.cpp PROPERTIES
        COMPILE_DEFINITIONS "XBE_PATCH=\"${PORT_XBE_PATCH}\";XBE_PATCH_SHA1=\"${PORT_XBE_PATCH_SHA1}\""
        OBJECT_DEPENDS ${PORT_XBE_PATCH})
endif()

# the host and runtime alone, to find their compile errors before the long build of the recompiled code
add_library(host_check OBJECT EXCLUDE_FROM_ALL ${HOST_SOURCES} ${IMGUI_SOURCES})
target_include_directories(host_check PRIVATE ${HOST_INCLUDES})
target_include_directories(host_check SYSTEM PRIVATE ${THIRD}/imgui)
target_compile_options(host_check PRIVATE -fno-strict-aliasing -fsigned-char)
target_link_libraries(host_check PRIVATE tomlplusplus::tomlplusplus)

# NVK, linked statically; its own (HLSL-less) glslang is left out
if(PORT_RENDERER STREQUAL "soft")
# the CPU renderer draws on the title's thread (render_thread.cpp's batches add nothing to it)
set_property(SOURCE ${GAME}/d3d/d3d8.cpp APPEND PROPERTY COMPILE_DEFINITIONS XBE_RENDER_THREAD=0)
target_link_libraries(${PORT_NAME} PRIVATE tomlplusplus::tomlplusplus -Wl,--start-group nx stdc++ m pthread -Wl,--end-group)
elseif(PORT_RENDERER STREQUAL "gl")
# GL draws on the title's thread too, where its context is current (gl_backend.cpp)
set_property(SOURCE ${GAME}/d3d/d3d8.cpp APPEND PROPERTY COMPILE_DEFINITIONS XBE_RENDER_THREAD=0)
set_property(SOURCE ${HOST}/gl_backend.cpp APPEND PROPERTY COMPILE_DEFINITIONS GL_BACKEND_DEFAULT)
# switch-mesa lives in devkitPro's portlibs, which the toolchain file leaves off the paths
target_include_directories(${PORT_NAME} PRIVATE ${DEVKITPRO}/portlibs/switch/include)
target_include_directories(host_check PRIVATE ${DEVKITPRO}/portlibs/switch/include)
target_link_directories(${PORT_NAME} PRIVATE ${DEVKITPRO}/portlibs/switch/lib)
target_link_libraries(${PORT_NAME} PRIVATE
    tomlplusplus::tomlplusplus glslang $<$<TARGET_EXISTS:SPIRV>:SPIRV> glslang-default-resource-limits
    -Wl,--start-group EGL glapi drm_nouveau nx stdc++ m pthread -Wl,--end-group)
elseif(PORT_RENDERER STREQUAL "dk")
# deko3d may draw on a thread of its own (render_thread in the settings, 1 by default; d3d8.cpp, render_thread.cpp):
# its objects are not tied to a thread, and the worker takes the renderer over after Init (Backend::Adopt)
set_property(SOURCE ${GAME}/d3d/d3d8.cpp APPEND PROPERTY COMPILE_DEFINITIONS XBE_RENDER_THREAD=1)
target_link_libraries(host_check PRIVATE uamlib)
target_link_libraries(${PORT_NAME} PRIVATE
    tomlplusplus::tomlplusplus glslang $<$<TARGET_EXISTS:SPIRV>:SPIRV> glslang-default-resource-limits uamlib
    -Wl,--start-group deko3d nx stdc++ m pthread -Wl,--end-group)
else()
target_link_libraries(${PORT_NAME} PRIVATE
    tomlplusplus::tomlplusplus glslang $<$<TARGET_EXISTS:SPIRV>:SPIRV> glslang-default-resource-limits
    -Wl,--allow-multiple-definition
    -Wl,--start-group
    ${NVK_SDK}/lib/libvulkan.a
    ${NVK_SDK}/lib/liballoc-af82aa609fb0f811.rlib
    ${NVK_SDK}/lib/libcore-8c20f6407cb781b6.rlib
    ${NVK_SDK}/lib/libcompiler_builtins-389209a593b9fef9.rlib
    ${NVK_SDK}/lib/libpanic_abort-810dcfbec4f99ef7.rlib
    zstd expat z nx stdc++ m pthread
    -Wl,--end-group)
endif()

nx_generate_nacp(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PORT_NAME}.nacp
    NAME "${PORT_TITLE}" AUTHOR "${PORT_AUTHOR}" VERSION "${PORT_VERSION}")
if(EXISTS ${PORT_ICON})
    nx_create_nro(${PORT_NAME} NACP ${CMAKE_CURRENT_BINARY_DIR}/${PORT_NAME}.nacp ICON ${PORT_ICON})
else()
    nx_create_nro(${PORT_NAME} NACP ${CMAKE_CURRENT_BINARY_DIR}/${PORT_NAME}.nacp)
endif()
