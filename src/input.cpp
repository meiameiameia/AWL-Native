#include "awl/input.h"

namespace awl {

void NativeInputAccumulator::reset(bool focused) {
    *this = NativeInputAccumulator{};
    focused_ = focused;
    frame_.focused = focused;
}

void NativeInputAccumulator::set_focused(bool focused) {
    if (focused_ == focused) {
        return;
    }
    focused_ = focused;
    if (!focused) {
        keyboard_released_ |= keyboard_held_;
        keyboard_held_ = 0;
        set_gamepad(NativeGamepadState{});
        keyboard_pressed_ = 0;
        gamepad_pressed_ = 0;
    }
}

void NativeInputAccumulator::set_key(NativeKey key, bool down) {
    if (!focused_ || static_cast<uint8_t>(key) >=
                         static_cast<uint8_t>(NativeKey::Count)) {
        return;
    }
    const uint32_t mask = native_key_mask(key);
    if (down) {
        if ((keyboard_held_ & mask) == 0) {
            keyboard_pressed_ |= mask;
            keyboard_held_ |= mask;
        }
    } else if ((keyboard_held_ & mask) != 0) {
        keyboard_released_ |= mask;
        keyboard_held_ &= ~mask;
    }
}

void NativeInputAccumulator::set_gamepad(const NativeGamepadState& gamepad) {
    const NativeGamepadState next = focused_ && gamepad.connected
        ? gamepad : NativeGamepadState{};
    const uint16_t previous_buttons = gamepad_.buttons;
    gamepad_pressed_ |= static_cast<uint16_t>(
        next.buttons & static_cast<uint16_t>(~previous_buttons));
    gamepad_released_ |= static_cast<uint16_t>(
        previous_buttons & static_cast<uint16_t>(~next.buttons));
    gamepad_ = next;
}

void NativeInputAccumulator::begin_frame() {
    frame_.focused = focused_;
    frame_.keyboard_held = keyboard_held_;
    frame_.keyboard_pressed = keyboard_pressed_;
    frame_.keyboard_released = keyboard_released_;
    frame_.gamepad = gamepad_;
    frame_.gamepad_pressed = gamepad_pressed_;
    frame_.gamepad_released = gamepad_released_;
    keyboard_pressed_ = 0;
    keyboard_released_ = 0;
    gamepad_pressed_ = 0;
    gamepad_released_ = 0;
}

} // namespace awl
