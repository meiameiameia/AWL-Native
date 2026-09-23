#pragma once

#include <cstdint>

namespace awl {

// Native device state; PadAdapter below supplies a raw GameCube-style sample.
// Game actions still require separate DOL-backed translation.
enum class NativeKey : uint8_t {
    W, A, S, D, Q, E, Z, X,
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

// SDK-compatible GameCube PAD button bits. This is a raw controller sample,
// not the game's later HSD_Pad filtering, repeat state, or action mapping.
enum class PadButton : uint16_t {
    Left = 0x0001, Right = 0x0002, Down = 0x0004, Up = 0x0008,
    Z = 0x0010, R = 0x0020, L = 0x0040,
    A = 0x0100, B = 0x0200, X = 0x0400, Y = 0x0800,
    Start = 0x1000
};

constexpr uint16_t pad_button_mask(PadButton button) {
    return static_cast<uint16_t>(button);
}

struct PadSample {
    bool connected = false;
    uint16_t buttons = 0;
    int8_t stick_x = 0;
    int8_t stick_y = 0;
    int8_t substick_x = 0;
    int8_t substick_y = 0;
    uint8_t trigger_l = 0;
    uint8_t trigger_r = 0;
};

struct PadFrame {
    PadSample sample;
    uint16_t pressed = 0;
    uint16_t released = 0;
};

class PadAdapter {
public:
    void reset();
    void begin_frame(const NativeInputFrame& native);
    const PadFrame& frame() const { return frame_; }

private:
    PadFrame frame_;
};

// Verified button-only subset of the game's HSD PAD consumer state. Analog
// stick processing and its synthesized direction bits are not represented.
struct HsdButtonFrame {
    uint32_t current = 0;
    uint32_t previous = 0;
    uint32_t pressed = 0;
    uint32_t repeated = 0;
    uint32_t released = 0;
};

class HsdButtonFilter {
public:
    void reset();
    void set_repeat_timing(uint32_t initial_delay, uint32_t interval);
    void begin_frame(const PadSample& sample);
    const HsdButtonFrame& frame() const { return frame_; }

private:
    HsdButtonFrame frame_;
    uint32_t initial_delay_ = 15;
    uint32_t interval_ = 2;
    uint32_t countdown_ = 15;
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
