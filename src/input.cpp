#include "awl/input.h"

#include <windows.h>
#include <Xinput.h>
#include <cmath>

namespace awl {

namespace {

uint16_t mapped_buttons(uint32_t keys, uint16_t gamepad_buttons) {
    uint16_t result = 0;
    const auto add = [&result](bool active, PadButton button) {
        if (active) {
            result |= pad_button_mask(button);
        }
    };
    const auto key = [keys](NativeKey value) {
        return (keys & native_key_mask(value)) != 0;
    };
    const auto gamepad = [gamepad_buttons](uint16_t value) {
        return (gamepad_buttons & value) != 0;
    };

    // Keyboard defaults: arrows=D-pad, WASD=main stick, Space=A,
    // Backspace=B, X=X, Tab=Y, Z=Z, Q=L, E=R, Enter=Start.
    add(key(NativeKey::Left) || gamepad(XINPUT_GAMEPAD_DPAD_LEFT), PadButton::Left);
    add(key(NativeKey::Right) || gamepad(XINPUT_GAMEPAD_DPAD_RIGHT), PadButton::Right);
    add(key(NativeKey::Down) || gamepad(XINPUT_GAMEPAD_DPAD_DOWN), PadButton::Down);
    add(key(NativeKey::Up) || gamepad(XINPUT_GAMEPAD_DPAD_UP), PadButton::Up);
    add(key(NativeKey::Z) || gamepad(XINPUT_GAMEPAD_BACK), PadButton::Z);
    add(key(NativeKey::E) || gamepad(XINPUT_GAMEPAD_RIGHT_SHOULDER), PadButton::R);
    add(key(NativeKey::Q) || gamepad(XINPUT_GAMEPAD_LEFT_SHOULDER), PadButton::L);
    add(key(NativeKey::Space) || gamepad(XINPUT_GAMEPAD_A), PadButton::A);
    add(key(NativeKey::Backspace) || gamepad(XINPUT_GAMEPAD_B), PadButton::B);
    add(key(NativeKey::X) || gamepad(XINPUT_GAMEPAD_X), PadButton::X);
    add(key(NativeKey::Tab) || gamepad(XINPUT_GAMEPAD_Y), PadButton::Y);
    add(key(NativeKey::Enter) || gamepad(XINPUT_GAMEPAD_START), PadButton::Start);
    return result;
}

int8_t scale_stick(int16_t value) {
    return static_cast<int8_t>(value / 256);
}

int8_t keyboard_axis(uint32_t keys, NativeKey negative, NativeKey positive) {
    const bool minus = (keys & native_key_mask(negative)) != 0;
    const bool plus = (keys & native_key_mask(positive)) != 0;
    return minus == plus ? 0 : static_cast<int8_t>(plus ? 127 : -127);
}

float stick_radius(int8_t x, int8_t y) {
    const int horizontal = static_cast<int>(x);
    const int vertical = static_cast<int>(y);
    return std::sqrt(static_cast<float>(horizontal * horizontal +
                                             vertical * vertical));
}

void filter_stick(int8_t& x, int8_t& y) {
    // FUN_802129CC: startup configuration at 0x8034144C..4F applies the
    // same radial dead zone (10), cap (82), and offset (10) to both sticks.
    float radius = stick_radius(x, y);
    if (radius < 10.0f) {
        x = 0;
        y = 0;
        return;
    }
    if (radius > 82.0f) {
        x = static_cast<int8_t>(static_cast<float>(x) * 82.0f / radius);
        y = static_cast<int8_t>(static_cast<float>(y) * 82.0f / radius);
        radius = stick_radius(x, y);
    }
    if (radius > 1.0e-10f) {
        x = static_cast<int8_t>(static_cast<float>(x) -
                                static_cast<float>(x) * 10.0f / radius);
        y = static_cast<int8_t>(static_cast<float>(y) -
                                static_cast<float>(y) * 10.0f / radius);
    }
}

uint32_t stick_direction_bits(int8_t x, int8_t y, unsigned shift) {
    // FUN_80213180: startup threshold is 30, angular margin is zero.
    if (stick_radius(x, y) < 30.0f) return 0;
    const float angle = static_cast<float>(std::atan2(
        static_cast<double>(y), static_cast<double>(x)));
    // Verified r2 constants at 0x8034C3C8..E0, in radians.
    constexpr double first = -2.356194490192345;
    constexpr double second = -0.7853981633974483;
    constexpr double third = 0.7853981633974483;
    constexpr double fourth = 2.356194490192345;
    uint32_t bit = 0;
    if (angle < first || angle > fourth) bit = 0x00040000;       // left
    else if (angle <= second) bit = 0x00020000;                  // down
    else if (angle < third) bit = 0x00080000;                    // right
    else bit = 0x00010000;                                      // up
    return bit << shift;
}

uint8_t filter_trigger(uint8_t raw) {
    // FUN_802129CC; FUN_8000AD40 configures dead zone 10 and subtraction.
    return raw < 10 ? 0 : static_cast<uint8_t>(raw - 10);
}

} // namespace

void PadAdapter::reset() {
    frame_ = PadFrame{};
}

void PadAdapter::begin_frame(const NativeInputFrame& native) {
    const uint16_t previous = frame_.sample.buttons;
    PadSample sample;
    sample.connected = native.focused; // Keyboard acts as port 0 while focused.
    if (native.focused) {
        const NativeGamepadState& pad = native.gamepad;
        const uint16_t gamepad_buttons = pad.connected ? pad.buttons : 0;
        sample.buttons = mapped_buttons(native.keyboard_held, gamepad_buttons);
        if (pad.connected) {
            sample.stick_x = scale_stick(pad.left_x);
            sample.stick_y = scale_stick(pad.left_y);
            sample.substick_x = scale_stick(pad.right_x);
            sample.substick_y = scale_stick(pad.right_y);
            sample.trigger_l = pad.left_trigger;
            sample.trigger_r = pad.right_trigger;
        }
        if ((native.keyboard_held & native_key_mask(NativeKey::Q)) != 0 ||
            (gamepad_buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0) {
            sample.trigger_l = 255;
        }
        if ((native.keyboard_held & native_key_mask(NativeKey::E)) != 0 ||
            (gamepad_buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0) {
            sample.trigger_r = 255;
        }
        // Keyboard overrides only the main-stick axis it is using.
        const uint32_t horizontal = native_key_mask(NativeKey::A) |
                                    native_key_mask(NativeKey::D);
        const uint32_t vertical = native_key_mask(NativeKey::S) |
                                  native_key_mask(NativeKey::W);
        if ((native.keyboard_held & horizontal) != 0) {
            sample.stick_x = keyboard_axis(native.keyboard_held,
                                           NativeKey::A, NativeKey::D);
        }
        if ((native.keyboard_held & vertical) != 0) {
            sample.stick_y = keyboard_axis(native.keyboard_held,
                                           NativeKey::S, NativeKey::W);
        }
        // XInput triggers are analog; near-full travel emulates the GC click.
        if (sample.trigger_l >= 200) {
            sample.buttons |= pad_button_mask(PadButton::L);
        }
        if (sample.trigger_r >= 200) {
            sample.buttons |= pad_button_mask(PadButton::R);
        }
    }

    const uint16_t current = sample.buttons;
    const uint16_t event_pressed = native.focused
        ? mapped_buttons(native.keyboard_pressed, native.gamepad_pressed) : 0;
    const uint16_t event_released = native.focused
        ? mapped_buttons(native.keyboard_released, native.gamepad_released) : 0;
    frame_.pressed = static_cast<uint16_t>((current & ~previous) |
                                           (event_pressed & ~previous));
    frame_.released = static_cast<uint16_t>((previous & ~current) |
                                            (event_released & ~current));
    frame_.sample = sample;
}

void HsdPadFilter::reset() {
    frame_ = HsdPadFrame{};
    initial_delay_ = 15;
    interval_ = 2;
    countdown_ = initial_delay_;
}

void HsdPadFilter::set_repeat_timing(uint32_t initial_delay,
                                     uint32_t interval) {
    // FUN_800126FC clamps both scene-dependent values to at least one.
    initial_delay_ = initial_delay == 0 ? 1 : initial_delay;
    interval_ = interval == 0 ? 1 : interval;
}

void HsdPadFilter::begin_frame(const PadSample& sample) {
    frame_.previous = frame_.current;
    uint32_t current = 0;
    if (sample.connected) {
        current = sample.buttons;
        frame_.stick_x = sample.stick_x;
        frame_.stick_y = sample.stick_y;
        frame_.substick_x = sample.substick_x;
        frame_.substick_y = sample.substick_y;
        filter_stick(frame_.stick_x, frame_.stick_y);
        filter_stick(frame_.substick_x, frame_.substick_y);
        current |= stick_direction_bits(frame_.stick_x, frame_.stick_y, 0);
        current |= stick_direction_bits(frame_.substick_x, frame_.substick_y, 4);
        frame_.trigger_l = filter_trigger(sample.trigger_l);
        frame_.trigger_r = filter_trigger(sample.trigger_r);
        if (frame_.trigger_l > 120) current |= 0x01000000;
        if (frame_.trigger_r > 120) current |= 0x02000000;
    } else {
        frame_.stick_x = frame_.stick_y = 0;
        frame_.substick_x = frame_.substick_y = 0;
        frame_.trigger_l = frame_.trigger_r = 0;
    }
    frame_.current = current;
    frame_.pressed = current & (frame_.previous ^ current);
    frame_.released = frame_.previous & (frame_.previous ^ current);
    if (current != frame_.previous) {
        frame_.repeated = frame_.pressed;
        countdown_ = initial_delay_;
    } else if (--countdown_ == 0) {
        frame_.repeated = current;
        countdown_ = interval_;
    } else {
        frame_.repeated = 0;
    }
}

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
