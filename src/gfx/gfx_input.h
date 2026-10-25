// Keyboard and XInput input for the game window, feeding the project's PadAdapter (src/input.cpp). Input is active only
// while the window has focus; losing focus releases every key and clears the gamepad.
#pragma once

#include <cstdint>

#include "awl/input.h"

namespace awl::gfx {

// Windows virtual-key code -> pad key. Keys: W/A/S/D main stick, arrows D-pad, Space=A, Backspace=B, X=X, Tab=Y,
// Z=Z, Q=L, E=R, Enter=Start (PadAdapter defaults). Esc, F11 and Alt+Enter belong to the window and are not mapped.
bool keyFromVirtualKey(uint32_t vk, NativeKey& out);

// Raw XInput state (XINPUT_GAMEPAD field values) -> native gamepad state.
NativeGamepadState gamepadFromXInput(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger, int16_t lx, int16_t ly, int16_t rx, int16_t ry);

class WindowInput {
public:
    WindowInput() { acc_.reset(false); }

    void setFocus(bool focused);
    bool focused() const { return acc_.focused(); }
    void onKey(uint32_t vk, bool down);
    void setGamepad(const NativeGamepadState& pad) { acc_.set_gamepad(pad); }
    // Polls XInput controller 0 into the accumulator (no-op while unfocused or when polling is disabled, as in tests).
    void pollGamepad();
    void enableXInputPolling(bool enable) { poll_ = enable; }
    // Advances one guest poll: returns the PAD sample the game sees now. connected is false while unfocused.
    const PadSample& sample();
    // Releases everything (focus loss, pause).
    void clear();

private:
    NativeInputAccumulator acc_;
    PadAdapter adapter_;
    bool poll_ = true;
};

}  // namespace awl::gfx
