# The PC test build of a kit port: the same game sources and the same Vulkan renderer as the Switch build
# (host-switch/vulkan.cpp, vk_shader.cpp, ui.cpp, game_host.cpp), on Windows with the PC's Vulkan driver, a Win32
# window, XInput and XAudio2 (this folder). To find and fix the Switch's bugs on the PC; the NRO is still the release.
#
# ferramentas/build_pc.ps1 <game folder> runs this with the Visual Studio's Clang and Ninja:
# cmake -S host-pc -B build/<name>-pc -G Ninja -DPORT_GAME_DIR=<game folder> -DCMAKE_BUILD_TYPE=Release

cmake_minimum_required(VERSION 3.22)
project(xbe_pc_port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(KIT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(HOST ${CMAKE_CURRENT_SOURCE_DIR})
set(SWITCH_HOST ${KIT}/host-switch)
set(SDK ${KIT}/sdk/XboxXBE-Recomp)
set(THIRD ${KIT}/third_party)
set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}" CACHE PATH "the LunarG Vulkan SDK (Include/, Lib/vulkan-1.lib)")
if(NOT VULKAN_SDK_DIR OR NOT EXISTS ${VULKAN_SDK_DIR}/Lib/vulkan-1.lib)
    file(GLOB found LIST_DIRECTORIES true C:/VulkanSDK/*)
    list(SORT found)
    list(REVERSE found)
    list(GET found 0 VULKAN_SDK_DIR)
endif()
message(STATUS "Vulkan SDK: ${VULKAN_SDK_DIR}")

set(PORT_GAME_DIR "" CACHE PATH "the game's folder: src/, config/game.toml, assets/, generated/, port.cmake")
get_filename_component(PORT_GAME_DIR ${PORT_GAME_DIR} ABSOLUTE)
set(GAME_DIR ${PORT_GAME_DIR})
set(GAME ${GAME_DIR}/src)
set(PORT_NAME "")
set(PORT_TITLE "")
set(PORT_AUTHOR "XboxXBE-Recomp")
set(PORT_VERSION "0.1.0")
include(${GAME_DIR}/port.cmake)

set(GEN ${GAME_DIR}/generated/game)
set(XBE ${GAME_DIR}/assets/default.xbe)
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
     "#define PORT_BUILD \"pc-${PORT_VERSION}\"\n#define PORT_NAME \"${PORT_NAME}\"\n#define PORT_TITLE \"${PORT_TITLE}\"\n")

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
# glslang before 12 defines glslang-default-resource-limits only with its binaries: build its source here
if(NOT TARGET glslang-default-resource-limits)
    add_library(glslang-default-resource-limits STATIC ${THIRD}/glslang/StandAlone/ResourceLimits.cpp)
    target_include_directories(glslang-default-resource-limits PUBLIC ${THIRD}/glslang)
endif()
add_subdirectory(${THIRD}/tomlplusplus ${CMAKE_CURRENT_BINARY_DIR}/tomlplusplus EXCLUDE_FROM_ALL)
set(IMGUI_SOURCES ${THIRD}/imgui/imgui.cpp ${THIRD}/imgui/imgui_draw.cpp ${THIRD}/imgui/imgui_tables.cpp
    ${THIRD}/imgui/imgui_widgets.cpp ${THIRD}/imgui/backends/imgui_impl_vulkan.cpp)

# as the Switch build: the SDK's runtime without its Windows app and renderer, the game's src/ replacing dsound/d3d8
file(GLOB RUNTIME_SOURCES ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|render_thread)\\.cpp$|/audio/xaudio2\\.cpp$|/xdk/(dsound|d3d8)\\.cpp$")
file(GLOB GAME_HOST_SOURCES CONFIGURE_DEPENDS ${GAME}/*/*.cpp)
file(GLOB PC_SOURCES CONFIGURE_DEPENDS ${HOST}/*.cpp)
set(SHARED_SWITCH_SOURCES ${SWITCH_HOST}/vulkan.cpp ${SWITCH_HOST}/vk_shader.cpp ${SWITCH_HOST}/ui.cpp
    ${SWITCH_HOST}/game_host.cpp ${SDK}/harness/reference_renderer.cpp)
# MPayne2-nx: the Switch's OpenGL renderer (PORT_RENDERER=gl), run on the PC with MP2_GL=1 against the Vulkan one
set(SPIRV_CROSS_SOURCES ${THIRD}/spirv-cross/spirv_cross.cpp ${THIRD}/spirv-cross/spirv_parser.cpp
    ${THIRD}/spirv-cross/spirv_cross_parsed_ir.cpp ${THIRD}/spirv-cross/spirv_cfg.cpp ${THIRD}/spirv-cross/spirv_glsl.cpp)
list(APPEND SHARED_SWITCH_SOURCES ${SWITCH_HOST}/gl_backend.cpp ${SWITCH_HOST}/gl_shader.cpp ${SPIRV_CROSS_SOURCES})
set(HOST_SOURCES ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${PC_SOURCES} ${SHARED_SWITCH_SOURCES})
set(HOST_INCLUDES ${HOST}/compat ${SWITCH_HOST} ${GAME} ${SDK}/runtime ${SDK}/harness ${GEN} ${CMAKE_CURRENT_BINARY_DIR}
    ${THIRD}/glslang ${VULKAN_SDK_DIR}/Include ${THIRD}/spirv-cross ${THIRD}/gl)

add_executable(${PORT_NAME}_pc ${HOST_SOURCES} ${GEN_SOURCES} ${IMGUI_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp)
target_include_directories(${PORT_NAME}_pc PRIVATE ${HOST_INCLUDES})
target_include_directories(${PORT_NAME}_pc SYSTEM PRIVATE ${THIRD}/imgui)
target_compile_options(${PORT_NAME}_pc PRIVATE -fno-strict-aliasing -fsigned-char -msse4.1 -Wno-everything)
target_compile_definitions(${PORT_NAME}_pc PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-w;-fno-strict-aliasing")
# every log line, as on the Switch (core/log.h); not on the generated code, so changing it rebuilds only the host
set_property(SOURCE ${HOST_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS XBE_PC_VERBOSE)
set_property(SOURCE ${SWITCH_HOST}/vk_shader.cpp APPEND PROPERTY COMPILE_DEFINITIONS ENABLE_HLSL)

add_library(host_check OBJECT EXCLUDE_FROM_ALL ${HOST_SOURCES} ${IMGUI_SOURCES})
target_include_directories(host_check PRIVATE ${HOST_INCLUDES})
target_include_directories(host_check SYSTEM PRIVATE ${THIRD}/imgui)
target_compile_options(host_check PRIVATE -fno-strict-aliasing -fsigned-char -msse4.1 -Wno-everything)
target_compile_definitions(host_check PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_link_libraries(host_check PRIVATE tomlplusplus::tomlplusplus)

target_link_libraries(${PORT_NAME}_pc PRIVATE tomlplusplus::tomlplusplus glslang $<$<TARGET_EXISTS:SPIRV>:SPIRV> glslang-default-resource-limits
    ${VULKAN_SDK_DIR}/Lib/vulkan-1.lib xinput xaudio2 ole32 dbghelp winmm ws2_32 user32 gdi32 opengl32)
# lld keeps the recompiled weak functions overridable by the host ones. MPayne2-nx (2026-11-24): a window program, no
# console beside it (the log is in logs/ beside the exe, stderr.log unbuffered), so the PC build is a release too
target_link_options(${PORT_NAME}_pc PRIVATE -fuse-ld=lld -g -Wl,/subsystem:windows -Wl,/entry:mainCRTStartup)
# MPayne2-nx (2026-11-24): the port's icon (icon.jpg, the NRO's) on the exe and its window, made into an .ico at configure
# time when Python has Pillow; without either the exe keeps Windows' default icon
set(PORT_ICON ${GAME_DIR}/icon.jpg)
if(EXISTS ${PORT_ICON})
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(ico_result 1)
    if(Python3_FOUND)
        execute_process(COMMAND ${Python3_EXECUTABLE} -I -c
            "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGBA').save(sys.argv[2], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])"
            ${PORT_ICON} ${CMAKE_CURRENT_BINARY_DIR}/icon.ico RESULT_VARIABLE ico_result OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(ico_result EQUAL 0)
        enable_language(RC)
        file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon.rc CONTENT "1 ICON \"${CMAKE_CURRENT_BINARY_DIR}/icon.ico\"
")
        target_sources(${PORT_NAME}_pc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/icon.rc)
        message(STATUS "icon: ${PORT_ICON}")
    else()
        message(STATUS "icon: not made (Python with Pillow needed)")
    endif()
endif()
# MPayne2-nx: line numbers for the CPU renderer's faults (MP2_SOFT=1)
set_property(SOURCE ${SDK}/harness/reference_renderer.cpp APPEND PROPERTY COMPILE_OPTIONS -g -gcodeview)
# MPayne2-nx: RenderDoc's in-app API header (a capture trigger, host-pc/main.cpp); the installed RenderDoc's, else none
if(EXISTS "C:/Program Files/RenderDoc/renderdoc_app.h")
    target_include_directories(${PORT_NAME}_pc PRIVATE "C:/Program Files/RenderDoc")
else()
    message(FATAL_ERROR "renderdoc_app.h not found: install RenderDoc or remove the capture trigger in host-pc/main.cpp")
endif()
