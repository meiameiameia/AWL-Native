#include "pad_source.h"

#include <windows.h>
#include <cstdlib>
#include <vector>
#include <Xinput.h>

#include "awl/input.h"

namespace awl::hle {

namespace {
PadProvider g_provider = nullptr;
void* g_providerUser = nullptr;

// AWL_PAD_SCRIPT (debug, headless Neutral mode only): "from-to=HEXBUTTONS[,stickX,stickY];..." sets channel 0 for the PADRead calls
// numbered from..to (1-based, one call per game main-loop pass). Used to check which controller bits a menu reads without a window.
struct ScriptStep { uint32_t from, to; uint16_t buttons; int8_t sx, sy; };
const std::vector<ScriptStep>& padScript() {
    static const std::vector<ScriptStep> steps = [] {
        std::vector<ScriptStep> v;
        const char* env = std::getenv("AWL_PAD_SCRIPT");
        if (!env) return v;
        const char* p = env;
        while (*p) {
            ScriptStep s = {0, 0, 0, 0, 0};
            char* end = nullptr;
            s.from = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (*end != '-') break;
            s.to = static_cast<uint32_t>(std::strtoul(end + 1, &end, 10));
            if (*end != '=') break;
            s.buttons = static_cast<uint16_t>(std::strtoul(end + 1, &end, 16));
            if (*end == ',') { s.sx = static_cast<int8_t>(std::strtol(end + 1, &end, 10)); if (*end == ',') s.sy = static_cast<int8_t>(std::strtol(end + 1, &end, 10)); }
            v.push_back(s);
            if (*end != ';') break;
            p = end + 1;
        }
        return v;
    }();
    return steps;
}
}  // namespace

void setPadProvider(PadProvider fn, void* user) {
    g_provider = fn;
    g_providerUser = user;
}

PadReading readPad(PadMode mode) {
    if (g_provider) return g_provider(g_providerUser);
    PadReading out;
    if (mode == PadMode::Neutral) {
        out.connected = true;
        static uint32_t call = 0;
        ++call;
        for (const ScriptStep& s : padScript())
            if (call >= s.from && call <= s.to) { out.buttons = s.buttons; out.stickX = s.sx; out.stickY = s.sy; }
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
