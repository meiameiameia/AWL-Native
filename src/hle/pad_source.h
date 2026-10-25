// Controller input for the PAD replacement. Neutral input is the deterministic default; the XInput mode reads the first
// controller through the project's PadAdapter (src/input.cpp), which maps it to SDK-compatible button bits and 8-bit sticks.
#pragma once

#include <cstdint>

namespace awl::hle {

struct PadReading {
    bool connected = false;
    uint16_t buttons = 0;          // SDK PAD button bits
    int8_t stickX = 0, stickY = 0, substickX = 0, substickY = 0;
    uint8_t triggerL = 0, triggerR = 0;
};

enum class PadMode { Neutral, XInput };

// A host that owns a window supplies channel 0 itself (keyboard and focus-gated controller): while a provider is set, readPad
// returns what it returns whatever the mode. Neutral stays the default when no provider is set. One provider per process.
using PadProvider = PadReading (*)(void* user);
void setPadProvider(PadProvider fn, void* user);   // null removes it

// Channel 0 only. In XInput mode every call polls the controller and advances the adapter by one frame.
PadReading readPad(PadMode mode);

}  // namespace awl::hle
