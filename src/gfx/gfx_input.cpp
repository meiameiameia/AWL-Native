#include "gfx/gfx_input.h"

#include <windows.h>
#include <Xinput.h>

namespace awl::gfx {

bool keyFromVirtualKey(uint32_t vk, NativeKey& out) {
    switch (vk) {
        case 'W': out = NativeKey::W; return true;
        case 'A': out = NativeKey::A; return true;
        case 'S': out = NativeKey::S; return true;
        case 'D': out = NativeKey::D; return true;
        case 'Q': out = NativeKey::Q; return true;
        case 'E': out = NativeKey::E; return true;
        case 'Z': out = NativeKey::Z; return true;
        case 'X': out = NativeKey::X; return true;
        case VK_UP: out = NativeKey::Up; return true;
        case VK_DOWN: out = NativeKey::Down; return true;
        case VK_LEFT: out = NativeKey::Left; return true;
        case VK_RIGHT: out = NativeKey::Right; return true;
        case VK_SPACE: out = NativeKey::Space; return true;
        case VK_RETURN: out = NativeKey::Enter; return true;
        case VK_BACK: out = NativeKey::Backspace; return true;
        case VK_TAB: out = NativeKey::Tab; return true;
        default: return false;
    }
}

NativeGamepadState gamepadFromXInput(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    NativeGamepadState g;
    g.connected = true;
    g.buttons = buttons;
    g.left_trigger = lt;
    g.right_trigger = rt;
    g.left_x = lx; g.left_y = ly;
    g.right_x = rx; g.right_y = ry;
    return g;
}

void WindowInput::setFocus(bool focused) {
    if (!focused) { acc_.set_focused(false); adapter_.reset(); return; }
    acc_.set_focused(true);
}

void WindowInput::onKey(uint32_t vk, bool down) {
    NativeKey key;
    if (keyFromVirtualKey(vk, key)) acc_.set_key(key, down);
}

void WindowInput::pollGamepad() {
    if (!poll_ || !acc_.focused()) return;
    XINPUT_STATE s = {};
    if (XInputGetState(0, &s) == ERROR_SUCCESS)
        acc_.set_gamepad(gamepadFromXInput(s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX,
                                           s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY));
    else
        acc_.set_gamepad(NativeGamepadState{});
}

const PadSample& WindowInput::sample() {
    pollGamepad();
    acc_.begin_frame();
    adapter_.begin_frame(acc_.frame());
    return adapter_.frame().sample;
}

void WindowInput::clear() {
    acc_.set_focused(false);
    adapter_.reset();
}

}  // namespace awl::gfx
