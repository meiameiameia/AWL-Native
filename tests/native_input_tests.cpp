#include "awl/input.h"

#include <windows.h>
#include <Xinput.h>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    awl::NativeInputAccumulator input;
    input.reset(true);
    input.begin_frame();
    expect(input.frame().focused, "initial focus is reported");
    expect(input.frame().keyboard_held == 0, "initial keyboard is clear");

    const uint32_t w = awl::native_key_mask(awl::NativeKey::W);
    const uint32_t a = awl::native_key_mask(awl::NativeKey::A);
    input.set_key(awl::NativeKey::W, true);
    input.set_key(awl::NativeKey::W, true); // autorepeat is not a new press
    input.begin_frame();
    expect(input.frame().keyboard_held == w, "key is held");
    expect(input.frame().keyboard_pressed == w, "key press is reported once");
    input.begin_frame();
    expect(input.frame().keyboard_held == w, "hold persists");
    expect(input.frame().keyboard_pressed == 0, "press does not repeat");

    input.set_key(awl::NativeKey::W, false);
    input.set_key(awl::NativeKey::A, true);
    input.set_key(awl::NativeKey::A, false); // complete tap between frames
    input.begin_frame();
    expect(input.frame().keyboard_held == 0, "released keys are not held");
    expect(input.frame().keyboard_pressed == a, "short tap press survives");
    expect(input.frame().keyboard_released == (w | a),
           "release and short tap are reported");

    awl::NativeGamepadState pad;
    pad.connected = true;
    pad.buttons = 0x1000;
    pad.left_x = 1234;
    input.set_gamepad(pad);
    input.begin_frame();
    expect(input.frame().gamepad.connected, "gamepad connects");
    expect(input.frame().gamepad.left_x == 1234, "raw stick is preserved");
    expect(input.frame().gamepad_pressed == 0x1000, "button press reported");
    input.begin_frame();
    expect(input.frame().gamepad_pressed == 0, "button press does not repeat");

    input.set_key(awl::NativeKey::W, true);
    input.set_focused(false);
    input.begin_frame();
    expect(!input.frame().focused, "focus loss is reported");
    expect(input.frame().keyboard_held == 0, "focus loss clears held keys");
    expect(input.frame().keyboard_released == w, "focus loss releases keys");
    expect(!input.frame().gamepad.connected, "focus loss disconnects gamepad state");
    expect(input.frame().gamepad_released == 0x1000,
           "focus loss releases gamepad buttons");

    input.set_key(awl::NativeKey::W, true);
    input.set_gamepad(pad);
    input.begin_frame();
    expect(input.frame().keyboard_held == 0,
           "unfocused keyboard event is ignored");
    expect(!input.frame().gamepad.connected,
           "unfocused gamepad sample is ignored");

    input.set_focused(true);
    input.set_gamepad(pad);
    input.begin_frame();
    expect(input.frame().gamepad_pressed == 0x1000,
           "gamepad button becomes pressed after refocus");

    input.set_gamepad(awl::NativeGamepadState{});
    input.begin_frame();
    expect(input.frame().gamepad_released == 0x1000,
           "disconnect releases gamepad button");

    input.set_key(awl::NativeKey::W, true);
    input.set_gamepad(pad);
    input.set_focused(false); // blur before either press is delivered
    input.begin_frame();
    expect(input.frame().keyboard_pressed == 0 &&
           input.frame().gamepad_pressed == 0,
           "focus loss cancels undelivered presses");

    awl::PadAdapter bridge;
    bridge.reset();
    input.reset(true);
    input.set_key(awl::NativeKey::Space, true);
    input.set_key(awl::NativeKey::W, true);
    input.set_key(awl::NativeKey::Up, true);
    input.begin_frame();
    bridge.begin_frame(input.frame());
    const uint16_t pad_a = awl::pad_button_mask(awl::PadButton::A);
    const uint16_t pad_up = awl::pad_button_mask(awl::PadButton::Up);
    expect(bridge.frame().sample.connected, "focused keyboard supplies PAD port 0");
    expect(bridge.frame().sample.buttons == (pad_a | pad_up),
           "keyboard face and D-pad buttons map to GC masks");
    expect(bridge.frame().sample.stick_y == 127,
           "W maps to positive main-stick Y");
    expect(bridge.frame().pressed == (pad_a | pad_up),
           "mapped buttons have rising edges");
    input.begin_frame();
    bridge.begin_frame(input.frame());
    expect(bridge.frame().pressed == 0 && bridge.frame().sample.buttons == (pad_a | pad_up),
           "mapped holds persist without a repeated press");

    input.set_key(awl::NativeKey::Q, true);
    input.set_key(awl::NativeKey::E, true);
    input.set_key(awl::NativeKey::Z, true);
    input.set_key(awl::NativeKey::X, true);
    input.begin_frame();
    bridge.begin_frame(input.frame());
    expect(bridge.frame().sample.buttons ==
               (pad_a | pad_up |
                awl::pad_button_mask(awl::PadButton::L) |
                awl::pad_button_mask(awl::PadButton::R) |
                awl::pad_button_mask(awl::PadButton::Z) |
                awl::pad_button_mask(awl::PadButton::X)) &&
           bridge.frame().sample.trigger_l == 255 &&
           bridge.frame().sample.trigger_r == 255,
           "keyboard supplies the remaining digital buttons and full trigger clicks");

    input.set_key(awl::NativeKey::Space, false);
    input.set_key(awl::NativeKey::W, false);
    input.set_key(awl::NativeKey::Up, false);
    input.set_key(awl::NativeKey::Q, false);
    input.set_key(awl::NativeKey::E, false);
    input.set_key(awl::NativeKey::Z, false);
    input.set_key(awl::NativeKey::X, false);
    pad = {};
    pad.connected = true;
    pad.buttons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X |
                  XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_DPAD_LEFT;
    pad.left_x = -32768;
    pad.left_y = 32767;
    pad.right_x = 256;
    pad.right_y = -256;
    pad.left_trigger = 199;
    pad.right_trigger = 200;
    input.set_gamepad(pad);
    input.begin_frame();
    bridge.begin_frame(input.frame());
    const uint16_t expected_gamepad = pad_a |
        awl::pad_button_mask(awl::PadButton::X) |
        awl::pad_button_mask(awl::PadButton::Z) |
        awl::pad_button_mask(awl::PadButton::Left) |
        awl::pad_button_mask(awl::PadButton::R);
    expect(bridge.frame().sample.buttons == expected_gamepad,
           "XInput face, Back, D-pad, and trigger map to GC bits");
    expect(bridge.frame().released ==
               (pad_up | awl::pad_button_mask(awl::PadButton::L)) &&
           (bridge.frame().pressed & pad_a) == 0,
           "switching A from keyboard to controller does not retrigger it");
    expect(bridge.frame().sample.stick_x == -128 &&
           bridge.frame().sample.stick_y == 127 &&
           bridge.frame().sample.substick_x == 1 &&
           bridge.frame().sample.substick_y == -1,
           "XInput stick endpoints and C-stick scaling are preserved");
    expect(bridge.frame().sample.trigger_l == 199 &&
           bridge.frame().sample.trigger_r == 200 &&
           (bridge.frame().sample.buttons &
            awl::pad_button_mask(awl::PadButton::L)) == 0,
           "analog triggers retain pressure and click at threshold");

    input.set_key(awl::NativeKey::A, true);
    input.set_key(awl::NativeKey::D, true);
    input.begin_frame();
    bridge.begin_frame(input.frame());
    expect(bridge.frame().sample.stick_x == 0,
           "opposed keyboard directions cancel the gamepad axis");

    input.set_focused(false);
    input.begin_frame();
    bridge.begin_frame(input.frame());
    expect(!bridge.frame().sample.connected &&
           bridge.frame().sample.buttons == 0 &&
           bridge.frame().sample.stick_y == 0 &&
           bridge.frame().released == expected_gamepad,
           "focus loss neutralizes and releases the PAD sample");

    input.reset(true);
    bridge.reset();
    input.set_key(awl::NativeKey::Space, true);
    input.set_key(awl::NativeKey::Space, false);
    input.begin_frame();
    bridge.begin_frame(input.frame());
    expect(bridge.frame().sample.buttons == 0 &&
           bridge.frame().pressed == pad_a && bridge.frame().released == pad_a,
           "short keyboard taps survive a frame boundary without a false hold");

    struct ButtonCase {
        uint16_t native;
        awl::PadButton expected;
    };
    const ButtonCase gamepad_cases[] = {
        {XINPUT_GAMEPAD_DPAD_LEFT, awl::PadButton::Left},
        {XINPUT_GAMEPAD_DPAD_RIGHT, awl::PadButton::Right},
        {XINPUT_GAMEPAD_DPAD_DOWN, awl::PadButton::Down},
        {XINPUT_GAMEPAD_DPAD_UP, awl::PadButton::Up},
        {XINPUT_GAMEPAD_BACK, awl::PadButton::Z},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, awl::PadButton::R},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, awl::PadButton::L},
        {XINPUT_GAMEPAD_A, awl::PadButton::A},
        {XINPUT_GAMEPAD_B, awl::PadButton::B},
        {XINPUT_GAMEPAD_X, awl::PadButton::X},
        {XINPUT_GAMEPAD_Y, awl::PadButton::Y},
        {XINPUT_GAMEPAD_START, awl::PadButton::Start},
    };
    for (const ButtonCase& button_case : gamepad_cases) {
        awl::NativeInputFrame frame;
        frame.focused = true;
        frame.gamepad.connected = true;
        frame.gamepad.buttons = button_case.native;
        bridge.reset();
        bridge.begin_frame(frame);
        expect(bridge.frame().sample.buttons ==
                   awl::pad_button_mask(button_case.expected),
               "each XInput button maps to its selected GC PAD bit");
    }

    struct KeyCase {
        awl::NativeKey native;
        awl::PadButton expected;
    };
    const KeyCase keyboard_cases[] = {
        {awl::NativeKey::Left, awl::PadButton::Left},
        {awl::NativeKey::Right, awl::PadButton::Right},
        {awl::NativeKey::Down, awl::PadButton::Down},
        {awl::NativeKey::Up, awl::PadButton::Up},
        {awl::NativeKey::Z, awl::PadButton::Z},
        {awl::NativeKey::E, awl::PadButton::R},
        {awl::NativeKey::Q, awl::PadButton::L},
        {awl::NativeKey::Space, awl::PadButton::A},
        {awl::NativeKey::Backspace, awl::PadButton::B},
        {awl::NativeKey::X, awl::PadButton::X},
        {awl::NativeKey::Tab, awl::PadButton::Y},
        {awl::NativeKey::Enter, awl::PadButton::Start},
    };
    for (const KeyCase& key_case : keyboard_cases) {
        awl::NativeInputFrame frame;
        frame.focused = true;
        frame.keyboard_held = awl::native_key_mask(key_case.native);
        bridge.reset();
        bridge.begin_frame(frame);
        expect(bridge.frame().sample.buttons ==
                   awl::pad_button_mask(key_case.expected),
               "each keyboard button maps to its selected GC PAD bit");
    }

    // Expected frame transitions come from the verified DOL's
    // FUN_80213B44; 15/2 and the strict 120 processed-trigger threshold
    // are runtime overrides in FUN_8000AD40, not initial template values.
    // FUN_802129CC subtracts the runtime dead zone of 10 first.
    awl::HsdPadFilter hsd;
    hsd.reset();
    awl::PadSample sample;
    sample.connected = true;
    sample.buttons = pad_a;
    sample.trigger_l = 130;
    sample.trigger_r = 131;
    hsd.begin_frame(sample);
    expect(hsd.frame().current == (pad_a | 0x02000000) &&
           hsd.frame().previous == 0 &&
           hsd.frame().pressed == (pad_a | 0x02000000) &&
           hsd.frame().repeated == hsd.frame().pressed &&
           hsd.frame().released == 0,
           "HSD first press and strict trigger threshold match the DOL");
    for (int hold = 1; hold < 15; ++hold) {
        hsd.begin_frame(sample);
        expect(hsd.frame().pressed == 0 && hsd.frame().repeated == 0,
               "HSD does not repeat before the 15-frame initial delay");
    }
    hsd.begin_frame(sample);
    expect(hsd.frame().repeated == hsd.frame().current,
           "HSD repeats after the initial delay");
    hsd.begin_frame(sample);
    expect(hsd.frame().repeated == 0,
           "HSD waits one frame between two-frame repeat pulses");
    hsd.begin_frame(sample);
    expect(hsd.frame().repeated == hsd.frame().current,
           "HSD repeats at the runtime interval");

    sample.buttons = pad_up;
    sample.trigger_l = 131;
    sample.trigger_r = 130;
    hsd.begin_frame(sample);
    expect(hsd.frame().current == (pad_up | 0x01000000) &&
           hsd.frame().pressed == (pad_up | 0x01000000) &&
           hsd.frame().released == (pad_a | 0x02000000) &&
           hsd.frame().repeated == hsd.frame().pressed,
           "HSD simultaneous press and release resets repeat delay");
    sample.connected = false;
    hsd.begin_frame(sample);
    expect(hsd.frame().current == 0 && hsd.frame().pressed == 0 &&
           hsd.frame().released == (pad_up | 0x01000000),
           "HSD disconnect clears current state and emits releases");

    hsd.reset();
    hsd.set_repeat_timing(1, 1);
    hsd.begin_frame(sample);
    sample.connected = true;
    sample.buttons = pad_a;
    sample.trigger_l = 0;
    sample.trigger_r = 0;
    hsd.begin_frame(sample);
    hsd.begin_frame(sample);
    expect(hsd.frame().repeated == pad_a,
           "HSD repeat timing can follow a scene-specific update");

    struct StickCase {
        int8_t raw_x;
        int8_t raw_y;
        int8_t filtered_x;
        int8_t filtered_y;
        uint32_t direction;
    };
    const StickCase stick_cases[] = {
        {9, 0, 0, 0, 0},           // Below the DOL's radial dead zone.
        {10, 0, 0, 0, 0},          // Offset consumes the exact boundary.
        {39, 0, 29, 0, 0},         // Below direction threshold after offset.
        {40, 0, 30, 0, 0x80000},   // Right at the direction threshold.
        {-40, 0, -30, 0, 0x40000}, // Left.
        {0, 40, 0, 30, 0x10000},   // Up.
        {0, -40, 0, -30, 0x20000}, // Down.
        {127, 0, 72, 0, 0x80000}, // Radius capped to 82 then offset by 10.
        {-128, 0, -72, 0, 0x40000},
    };
    for (const StickCase& stick_case : stick_cases) {
        hsd.reset();
        sample = {};
        sample.connected = true;
        sample.stick_x = stick_case.raw_x;
        sample.stick_y = stick_case.raw_y;
        hsd.begin_frame(sample);
        expect(hsd.frame().stick_x == stick_case.filtered_x &&
               hsd.frame().stick_y == stick_case.filtered_y &&
               hsd.frame().current == stick_case.direction,
               "HSD cardinal stick boundaries follow DOL radial and angular constants");
    }

    hsd.reset();
    sample = {};
    sample.connected = true;
    sample.stick_x = 70;
    sample.stick_y = 30;
    sample.substick_y = -40;
    hsd.begin_frame(sample);
    expect((hsd.frame().current & 0x00ff0000) == (0x80000 | 0x200000) &&
           hsd.frame().substick_x == 0 && hsd.frame().substick_y == -30,
           "HSD applies direction sectors and radial filtering to both sticks");
    sample.stick_x = -40;
    sample.stick_y = 0;
    sample.substick_y = 0;
    hsd.begin_frame(sample);
    expect(hsd.frame().current == 0x40000 &&
           hsd.frame().pressed == 0x40000 &&
           hsd.frame().released == (0x80000 | 0x200000),
           "HSD direction changes produce synthesized press and release edges");
    sample.stick_x = -39;
    hsd.begin_frame(sample);
    expect(hsd.frame().current == 0 && hsd.frame().released == 0x40000,
           "HSD leaving the direction threshold releases the synthesized bit");

    hsd.reset();
    sample = {};
    sample.connected = true;
    sample.trigger_l = 9;
    sample.trigger_r = 130;
    hsd.begin_frame(sample);
    expect(hsd.frame().trigger_l == 0 && hsd.frame().trigger_r == 120 &&
           hsd.frame().current == 0,
           "HSD trigger dead zone and strict synthesized-bit boundary");
    sample.trigger_l = 255;
    sample.trigger_r = 131;
    hsd.begin_frame(sample);
    expect(hsd.frame().trigger_l == 245 && hsd.frame().trigger_r == 121 &&
           hsd.frame().current == 0x03000000,
           "HSD processed trigger bytes feed synthesized button bits");
    sample.connected = false;
    hsd.begin_frame(sample);
    expect(hsd.frame().stick_x == 0 && hsd.frame().substick_y == 0 &&
           hsd.frame().trigger_l == 0 && hsd.frame().trigger_r == 0 &&
           hsd.frame().released == 0x03000000,
           "HSD disconnect clears processed axes and releases synthesized bits");

    if (failures == 0) {
        std::puts("Native input tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
