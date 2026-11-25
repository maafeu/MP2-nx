#pragma once
// The options panel's two chords on player 1's pad, for the hosts' input Poll (host-switch/input.cpp,
// host-pc/input.cpp), before the pad is hidden from the game while the panel is open or a chord is held. MP2-nx,
// 2026-11-24:
// - Back + R3 opens and closes the panel (the PC's F10 sends both). The games checked it in their own pad hook
//   (common/game_info.cpp), on the pad Poll had already emptied while the panel was open, so it opened the panel but
//   never closed it. Here it toggles before the pad is emptied; a game's own check then sees an empty pad and does
//   nothing.
// - Both sticks pressed in together: the Switch opens and closes the panel on them (host-switch/ui.cpp, from the
//   controller itself).
// A person, or sys-botbase, rarely presses a chord's two buttons in the same frame, and the first one alone reached
// the game for a frame or two: in Max Payne 2 on the console, L3 crouched Max as the panel opened, and Back opened
// the game's own menu behind it. So Back, L3 or R3 pressed alone reaches the game kChordWait late, and its chord's
// other button pressed within that time makes it the chord, which the game never sees; a tap shorter than the wait
// still reaches the game, at its release, for kTapTime. A button held longer reaches the game as it is (only its first
// kChordWait is late).
// Returns true while either chord is held: the caller hides the pad from the game then.
#include <chrono>
#include <cstdint>

namespace input {

inline bool PanelChords(uint16_t& buttons, void (*toggle)()) {
    using clock = std::chrono::steady_clock;
    constexpr auto kChordWait = std::chrono::milliseconds(80);
    constexpr auto kTapTime = std::chrono::milliseconds(50);
    // Pad::buttons: dpad, start, back, left thumb, right thumb
    constexpr uint16_t kBack = 1u << 5, kL3 = 1u << 6, kR3 = 1u << 7;
    constexpr uint16_t kMembers[3] = {kBack, kL3, kR3};
    constexpr uint16_t kMenu = kBack | kR3, kSticks = kL3 | kR3;
    enum State { kUp, kWaiting, kPassing };
    static bool sticks = false, menu = false;
    static State state[3] = {kUp, kUp, kUp};
    static clock::time_point since[3], tap_until[3];
    auto now = clock::now();
    const bool menu_down = (buttons & kMenu) == kMenu;
    if (menu_down && !menu && toggle) toggle();
    menu = menu_down || (menu && (buttons & kMenu));  // hidden until both are let go
    sticks = (buttons & kSticks) == kSticks || (sticks && (buttons & kSticks));
    if (menu || sticks) {  // hidden by the caller; nothing of it reaches the game afterwards either
        for (int i = 0; i < 3; i++) {
            state[i] = kUp;
            tap_until[i] = {};
        }
        return true;
    }
    for (int i = 0; i < 3; i++) {
        const uint16_t bit = kMembers[i];
        bool down = buttons & bit;
        switch (state[i]) {
            case kUp:
                if (down) {
                    state[i] = kWaiting;
                    since[i] = now;
                    buttons &= uint16_t(~bit);
                } else if (now < tap_until[i]) {
                    buttons |= bit;
                }
                break;
            case kWaiting:
                if (!down) {  // a tap: the game gets it now
                    state[i] = kUp;
                    tap_until[i] = now + kTapTime;
                    buttons |= bit;
                } else if (now - since[i] >= kChordWait) {
                    state[i] = kPassing;
                } else {
                    buttons &= uint16_t(~bit);
                }
                break;
            case kPassing:
                if (!down) state[i] = kUp;
                break;
        }
    }
    return false;
}

}  // namespace input
