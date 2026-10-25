// Input mapping for the game window: virtual keys, XInput values, focus rules. No window, no controller needed.
// Expected pad values are the GameCube SDK constants (PAD_BUTTON_*), written out here independently of the code under test.
#include <windows.h>

#include <cstdio>

#include "gfx/gfx_input.h"

using namespace awl;
using namespace awl::gfx;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// SDK values: PAD_BUTTON_LEFT 0x0001, RIGHT 0x0002, DOWN 0x0004, UP 0x0008, TRIGGER_Z 0x0010, TRIGGER_R 0x0020, TRIGGER_L 0x0040,
// A 0x0100, B 0x0200, X 0x0400, Y 0x0800, START 0x1000.
enum : uint16_t { kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kZ = 0x0010, kR = 0x0020, kL = 0x0040,
                  kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kStart = 0x1000 };

static uint16_t buttonsFor(uint32_t vk) {
    WindowInput in;
    in.enableXInputPolling(false);
    in.setFocus(true);
    in.onKey(vk, true);
    return in.sample().buttons;
}

static void testKeyTable() {
    struct { uint32_t vk; uint16_t pad; } map[] = {
        {VK_LEFT, kLeft}, {VK_RIGHT, kRight}, {VK_DOWN, kDown}, {VK_UP, kUp},
        {'Z', kZ}, {'E', kR}, {'Q', kL},
        {VK_SPACE, kA}, {VK_BACK, kB}, {'X', kX}, {VK_TAB, kY}, {VK_RETURN, kStart},
    };
    for (const auto& m : map) CHECK(buttonsFor(m.vk) == m.pad);
    for (const auto& m : map) { NativeKey k; CHECK(keyFromVirtualKey(m.vk, k)); }
    // keys with no pad meaning, and the window's own keys, are not mapped
    NativeKey k;
    CHECK(!keyFromVirtualKey(VK_ESCAPE, k));
    CHECK(!keyFromVirtualKey(VK_F11, k));
    CHECK(!keyFromVirtualKey(VK_F4, k));
    CHECK(!keyFromVirtualKey('P', k));
    // stick keys do not press buttons
    CHECK(buttonsFor('W') == 0 && buttonsFor('A') == 0 && buttonsFor('S') == 0 && buttonsFor('D') == 0);
}

static void testSticksAndTriggers() {
    WindowInput in;
    in.enableXInputPolling(false);
    in.setFocus(true);
    in.onKey('D', true);
    CHECK(in.sample().stick_x == 127 && in.sample().stick_y == 0);
    in.onKey('A', true);                         // opposite keys cancel
    CHECK(in.sample().stick_x == 0);
    in.onKey('D', false);
    CHECK(in.sample().stick_x == -127);
    in.onKey('A', false);
    in.onKey('W', true);
    CHECK(in.sample().stick_y == 127);           // W = stick up = +Y
    in.onKey('W', false);
    in.onKey('S', true);
    CHECK(in.sample().stick_y == -127);
    in.onKey('S', false);
    CHECK(in.sample().stick_x == 0 && in.sample().stick_y == 0);
    in.onKey('Q', true);                         // L: analog full travel plus the digital L bit
    CHECK(in.sample().trigger_l == 255 && (in.sample().buttons & kL));
    in.onKey('Q', false);
    CHECK(in.sample().trigger_l == 0 && !(in.sample().buttons & kL));
}

static void testGamepad() {
    WindowInput in;
    in.enableXInputPolling(false);
    in.setFocus(true);
    // XINPUT_GAMEPAD_A 0x1000, START 0x0010, DPAD_UP 0x0001, B 0x2000
    in.setGamepad(gamepadFromXInput(0x1000 | 0x0010 | 0x0001, 0, 0, 16384, -32768, 0, 0));
    const PadSample& s = in.sample();
    CHECK(s.connected);
    CHECK((s.buttons & (kA | kStart | kUp)) == (kA | kStart | kUp));
    CHECK(!(s.buttons & kB));
    CHECK(s.stick_x == 64);                      // 16384 / 256
    CHECK(s.stick_y == -128);                    // -32768 / 256
    // a full analog trigger registers as the digital shoulder button as well
    in.setGamepad(gamepadFromXInput(0, 0, 255, 0, 0, 0, 0));
    CHECK(in.sample().trigger_r == 255 && (in.sample().buttons & kR));
    // keyboard and controller combine
    in.setGamepad(gamepadFromXInput(0x1000, 0, 0, 0, 0, 0, 0));
    in.onKey(VK_BACK, true);
    CHECK((in.sample().buttons & (kA | kB)) == (kA | kB));
}

static void testFocus() {
    WindowInput in;
    in.enableXInputPolling(false);
    // not focused: nothing gets through, the controller reads as absent
    in.onKey(VK_SPACE, true);
    in.setGamepad(gamepadFromXInput(0x1000, 0, 0, 0, 0, 0, 0));
    CHECK(!in.sample().connected && in.sample().buttons == 0);
    // focused: keys count
    in.setFocus(true);
    in.onKey(VK_SPACE, true);
    CHECK(in.sample().connected && in.sample().buttons == kA);
    // focus lost: everything is released and cleared
    in.setFocus(false);
    CHECK(!in.sample().connected && in.sample().buttons == 0);
    in.onKey(VK_RETURN, true);                   // key events while unfocused are dropped
    // focus back: the key held across the loss is not held any more, the new press is dropped, input starts clear
    in.setFocus(true);
    CHECK(in.sample().connected && in.sample().buttons == 0);
    in.onKey(VK_RETURN, true);
    CHECK(in.sample().buttons == kStart);
    // clear() is the pause path
    in.clear();
    CHECK(!in.sample().connected && in.sample().buttons == 0);
    in.setFocus(true);
    CHECK(in.sample().buttons == 0);
}

int main() {
    testKeyTable();
    testSticksAndTriggers();
    testGamepad();
    testFocus();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    std::printf("gfx_input_tests: all checks passed\n");
    return 0;
}
