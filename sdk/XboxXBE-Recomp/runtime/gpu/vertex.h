#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// MaxPayne2-nx: an allocator whose resize leaves the new elements uninitialised: the converted vertices, the rebuilt index
// lists and the gathered vertices are each written in full right after their resize, and the zero fill before it
// (std::vector's value initialisation) was 1% of the title's thread at Max Payne 2's level 27 on the console
template <typename T>
struct NoInitAllocator : std::allocator<T> {
    template <typename U>
    struct rebind {
        using other = NoInitAllocator<U>;
    };
    NoInitAllocator() noexcept = default;
    template <typename U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}
    template <typename U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(p)) U;
    }
    template <typename U, typename... A>
    void construct(U* p, A&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<A>(args)...);
    }
};
template <typename T>
using NoInitVector = std::vector<T, NoInitAllocator<T>>;

enum class Topology : uint8_t { Points, Lines, Triangles };

// how the vertex shader finishes an element the input assembler reads raw, 4 bits per register in Assembled::decode
enum VertexDecode : uint8_t {
    kDecodeNone,
    kDecodeW1,       // 3 components read as 4: w = 1
    kDecodeInt,      // SHORT: integers to float
    kDecodeIntW1,    // SHORT3
    kDecodePacked,   // NORMPACKED3, 11:11:10 signed in a uint
    kDecodeFloat2H,  // x y w as 3 floats
};

// a buffer the draw reads: guest vertices as they are, or the float4 copy converted on the CPU
struct VertexBuffer {
    const uint8_t* src = nullptr;
    uint32_t bytes = 0, stride = 0;
};

// where input register r comes from: host vertex format (a DXGI_FORMAT), buffer, byte offset in the vertex
struct VertexElement {
    uint8_t format = 0, buffer = 0;
    uint16_t offset = 0;
};

// one draw's vertices and indices as the input assembler takes them
struct Assembled {
    NoInitVector<float> vertices;    // converted path: vertex_count * regs * 4
    NoInitVector<uint32_t> indices;  // converted path, or the list rebuilt for fans, quads and loops
    uint16_t regs = 0;              // input registers present, bit per v register
    uint32_t vertex_count = 0;
    Topology topology = Topology::Triangles;
    bool strip = false;             // line or triangle strip, drawn as a strip
    bool pretransformed = false;    // FVF XYZRHW
    uint64_t decode = 0;            // VertexDecode per register, 4 bits each
    std::array<VertexElement, 16> elements{};
    std::array<VertexBuffer, 16> buffers{};
    uint32_t buffer_count = 0;
    const void* index_src = nullptr;  // the title's 16 bit indices, or indices
    bool index16 = false;
    uint32_t index_count = 0;         // 0: an unindexed draw of draw_count vertices
    uint32_t draw_count = 0;
    int32_t base_vertex = 0;
    std::array<NoInitVector<uint8_t>, 16> gathered;  // raw vertices copied in draw order when the indices are sparse
    NoInitVector<uint32_t> seq;  // converted path's scratch: the draw's vertices in draw order (MaxPayne2-nx: a member, not
                                 // a thread_local, whose every access was a __aarch64_read_tp call under -mtp=soft)
};

// the FVF bits translated to the register layout the XDK's declarations use
void FvfInputs(uint32_t fvf, VertexShader& out);

// false when there is nothing to draw
// indices: the title's 16 bit indices, null for an unindexed draw
// converted: always the float4 copy (Assembled::vertices), never the raw path, for a consumer that reads the vertices
// itself (harness/reference_renderer.cpp)
bool Assemble(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, Assembled& out,
              bool converted = false);

}  // namespace gpu
