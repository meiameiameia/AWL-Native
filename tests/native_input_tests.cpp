#include "awl/input.h"

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

    if (failures == 0) {
        std::puts("Native input tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
