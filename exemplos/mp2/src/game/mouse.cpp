// Mouse look on the PC and gyro aiming on the Switch (2026-11-24): input::TakeMouse's counts (the PC host's raw mouse,
// host-switch/gyro.cpp's turn of the controller), kDegrees each, right and down positive, added to the player's turn
// for the frame.
//
// sub_00045B36 is the update of the player's input source (vtable 0031C8F0; found on the PC with a guest memory scan
// and a hardware write watch, docs/MP2-notes.md): from the right stick it sets the frame's turn, as radians, at +0xD
// (yaw: the stick's value times 100.8 degrees a second, 0031C9D0, times the frame's time) and +0x11 (pitch, 63
// degrees a second, 0031C9CC). The controller (sub_0015003E) takes them through the vtable's +0x78 and +0x7C and turns
// Max (the yaw kept in 0..2 pi at 00148A1F) and his aim; enemies have other input sources. A count turns kDegrees
// whatever the frame's time, so the mouse aims at the same rate in bullet time, as a mouse should.
#include <cstring>

#include "common/mouse_keyboard.h"
#include "cpu/x86_ops.h"

namespace {

constexpr uint32_t kPlayerInput = 0x0031C8F0;  // the player input source's vtable
constexpr float kDegrees = 0.05f;              // host-switch/gyro.cpp's kDegreesPerCount
constexpr float kRadians = kDegrees * 3.14159265f / 180.0f;

// what the hook last left in the two fields, and what of it was the mouse's: a frame where the game did not set
// them again (a path of the update that skips the stick) must not get the mouse's turn twice
float g_left[2] = {};
float g_added[2] = {};

void Add(uint8_t* field, int i, float add) {
    float v;
    std::memcpy(&v, field, 4);
    if (std::memcmp(&v, &g_left[i], 4) == 0) v -= g_added[i];
    g_added[i] = add;
    g_left[i] = v + add;
    std::memcpy(field, &g_left[i], 4);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00045B36);
X86_FUNC(sub_00045B36) {
    const uint32_t self = ctx.ecx;
    __imp__sub_00045B36(ctx, base);
    if (X86_MEM32(self) != kPlayerInput) return;
    float dx = 0, dy = 0;
    if (!input::TakeMouse(dx, dy)) dx = dy = 0;
    // yaw: right is positive, as the stick's. Pitch: positive tilts the view down (the stick pushed up does that, the
    // game's default being the inverted look), so the mouse's down (dy positive) adds: the mouse up looks up, as on a
    // PC (measured: tools/scripts/mp2_mouse_test.txt)
    Add(base + self + 0xD, 0, dx * kRadians);
    Add(base + self + 0x11, 1, dy * kRadians);
}
