#pragma once

#include <cstdint>

namespace awl {

// Native device state only. Mapping these controls to GameCube PAD and game
// actions requires a separate DOL-backed translation.
enum class NativeKey : uint8_t {
    W, A, S, D,
    Up, Down, Left, Right,
    Space, Enter, Backspace, Tab,
    Count
};

constexpr uint32_t native_key_mask(NativeKey key) {
    return uint32_t{1} << static_cast<uint8_t>(key);
}

struct NativeGamepadState {
    bool connected = false;
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t left_x = 0;
    int16_t left_y = 0;
    int16_t right_x = 0;
    int16_t right_y = 0;
};

struct NativeInputFrame {
    bool focused = false;
    uint32_t keyboard_held = 0;
    uint32_t keyboard_pressed = 0;
    uint32_t keyboard_released = 0;
    NativeGamepadState gamepad;
    uint16_t gamepad_pressed = 0;
    uint16_t gamepad_released = 0;
};

class NativeInputAccumulator {
public:
    void reset(bool focused);
    void set_focused(bool focused);
    void set_key(NativeKey key, bool down);
    void set_gamepad(const NativeGamepadState& gamepad);
    void begin_frame();
    const NativeInputFrame& frame() const { return frame_; }
    bool focused() const { return focused_; }

private:
    bool focused_ = false;
    uint32_t keyboard_held_ = 0;
    uint32_t keyboard_pressed_ = 0;
    uint32_t keyboard_released_ = 0;
    NativeGamepadState gamepad_;
    uint16_t gamepad_pressed_ = 0;
    uint16_t gamepad_released_ = 0;
    NativeInputFrame frame_;
};

} // namespace awl
