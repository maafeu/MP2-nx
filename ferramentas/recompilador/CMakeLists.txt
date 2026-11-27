# The x86 to C++ recompiler (xbe_recomp) and the XDK symbol finder (xbe_symbols), for ferramentas/recompilar.ps1:
# Clang (clang-cl) with the MSVC headers and linker, so the build needs Visual Studio's C++ tools and a Clang
#
# MPayne2-nx: nothing is fetched. The x86 decoder is tools/zydis-lite (Zydis's API, written for this port) and toml++ is
# third_party/tomlplusplus (a local copy). -DXBE_RECOMP_FETCH=ON brings back the kit's FetchContent of Zydis v4.1.1 and
# toml++ v3.4.0 from GitHub.

cmake_minimum_required(VERSION 3.22)
project(xbe_recomp_tool C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the kit's copy of the SDK, the one with the Switch changes
get_filename_component(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/XboxXBE-Recomp ABSOLUTE)
get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
file(WRITE ${CMAKE_BINARY_DIR}/sdk_dir.txt "${SDK}\n")

option(XBE_RECOMP_FETCH "fetch Zydis and toml++ from GitHub as the kit does" OFF)
if(XBE_RECOMP_FETCH)
    include(FetchContent)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
    FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(zydis tomlplusplus)
    set(DECODER Zydis)
    set(TOML tomlplusplus::tomlplusplus)
else()
    add_library(zydis_lite STATIC ${ROOT}/tools/zydis-lite/src/decoder.cpp)
    target_include_directories(zydis_lite PUBLIC ${ROOT}/tools/zydis-lite/include)
    target_compile_definitions(zydis_lite PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_library(toml_local INTERFACE)
    target_include_directories(toml_local INTERFACE ${ROOT}/third_party/tomlplusplus/include)
    set(DECODER zydis_lite)
    set(TOML toml_local)
endif()

file(GLOB RECOMP_SOURCES ${SDK}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${SDK}/runtime ${SDK}/tools/recomp)
target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_recomp PRIVATE ${DECODER} ${TOML})

add_executable(xbe_symbols ${SDK}/tools/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${SDK}/runtime)
target_compile_definitions(xbe_symbols PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_symbols PRIVATE ${DECODER} ${TOML})
