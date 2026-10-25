#include "pad_source.h"

#include <windows.h>
#include <Xinput.h>

#include "awl/input.h"

namespace awl::hle {

PadReading readPad(PadMode mode) {
    PadReading out;
    if (mode == PadMode::Neutral) {
        out.connected = true;
        return out;
    }
    static PadAdapter adapter;
    NativeInputFrame frame;
    XINPUT_STATE state = {};
    if (XInputGetState(0, &state) == ERROR_SUCCESS) {
        frame.focused = true;                       // a console host has no window: a connected controller is the active input
        frame.gamepad.connected = true;
        frame.gamepad.buttons = state.Gamepad.wButtons;
        frame.gamepad.left_trigger = state.Gamepad.bLeftTrigger;
        frame.gamepad.right_trigger = state.Gamepad.bRightTrigger;
        frame.gamepad.left_x = state.Gamepad.sThumbLX;
        frame.gamepad.left_y = state.Gamepad.sThumbLY;
        frame.gamepad.right_x = state.Gamepad.sThumbRX;
        frame.gamepad.right_y = state.Gamepad.sThumbRY;
    }
    adapter.begin_frame(frame);
    const PadSample& s = adapter.frame().sample;
    out.connected = s.connected;
    out.buttons = s.buttons;
    out.stickX = s.stick_x; out.stickY = s.stick_y;
    out.substickX = s.substick_x; out.substickY = s.substick_y;
    out.triggerL = s.trigger_l; out.triggerR = s.trigger_r;
    return out;
}

}  // namespace awl::hle
