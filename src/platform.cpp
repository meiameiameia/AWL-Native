#include "awl/platform.h"
#include <windows.h>
#include <Xinput.h>
#include <cstdio>
#include <cstdarg>

namespace awl {

static HWND g_hwnd = nullptr;
static bool g_running = true;
static PlatformExitReason g_exit_reason = PlatformExitReason::None;
static NativeInputAccumulator g_input;
static PadAdapter g_pad;

static bool key_from_virtual_key(WPARAM virtual_key, NativeKey* key) {
    switch (virtual_key) {
        case 'W': *key = NativeKey::W; return true;
        case 'A': *key = NativeKey::A; return true;
        case 'S': *key = NativeKey::S; return true;
        case 'D': *key = NativeKey::D; return true;
        case 'Q': *key = NativeKey::Q; return true;
        case 'E': *key = NativeKey::E; return true;
        case 'Z': *key = NativeKey::Z; return true;
        case 'X': *key = NativeKey::X; return true;
        case VK_UP: *key = NativeKey::Up; return true;
        case VK_DOWN: *key = NativeKey::Down; return true;
        case VK_LEFT: *key = NativeKey::Left; return true;
        case VK_RIGHT: *key = NativeKey::Right; return true;
        case VK_SPACE: *key = NativeKey::Space; return true;
        case VK_RETURN: *key = NativeKey::Enter; return true;
        case VK_BACK: *key = NativeKey::Backspace; return true;
        case VK_TAB: *key = NativeKey::Tab; return true;
        default: return false;
    }
}

// Timing state
static LARGE_INTEGER g_timer_freq;
static LARGE_INTEGER g_time_start = {};
static double g_delta_time = 0.0;

void logging_init() {
    // Console is typically already attached if built as a Console App.
    // We could allocate a console here if built as a Windowed app.
    printf("--- AWL Native Port ---\n");
}

void log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[INFO] ");
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

void log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void platform_init() {
    QueryPerformanceFrequency(&g_timer_freq);
    g_time_start.QuadPart = 0;
    g_delta_time = 0.0;
}

void platform_shutdown() {
    AWL_LOG_INFO("Platform shutdown.");
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_SETFOCUS:
            g_input.set_focused(true);
            return 0;
        case WM_KILLFOCUS:
            g_input.set_focused(false);
            return 0;
        case WM_CLOSE:
            g_running = false;
            if (g_exit_reason == PlatformExitReason::None) g_exit_reason = PlatformExitReason::WmClose;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_running = false;
            if (g_exit_reason == PlatformExitReason::None) g_exit_reason = PlatformExitReason::WmDestroy;
            if (hwnd == g_hwnd) {
                g_hwnd = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_running = false;
                if (g_exit_reason == PlatformExitReason::None) g_exit_reason = PlatformExitReason::Escape;
                DestroyWindow(hwnd);
                return 0;
            }
            [[fallthrough]];
        case WM_SYSKEYDOWN: {
            NativeKey key;
            if (key_from_virtual_key(wParam, &key)) {
                g_input.set_key(key, true);
                return 0;
            }
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            NativeKey key;
            if (key_from_virtual_key(wParam, &key)) {
                g_input.set_key(key, false);
                return 0;
            }
            break;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void window_init() {
    const char* class_name = "AWL_DECOMP_WINDOW";

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(
        0,
        class_name,
        "Harvest Moon: A Wonderful Life",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_SHOW);
    } else {
        AWL_LOG_ERROR("Failed to create window.");
    }
}

void window_shutdown() {
    AWL_LOG_INFO("Window shutdown.");
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

void* platform_get_window_handle() {
    return g_hwnd;
}

bool platform_pump_messages() {
    if (!g_running) return false;

    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_running = false;
            if (g_exit_reason == PlatformExitReason::None) g_exit_reason = PlatformExitReason::WmQuit;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Sleep briefly if minimized to prevent CPU burn
    if (g_hwnd && IsIconic(g_hwnd)) {
        Sleep(16);
    }
    if (!g_running && g_exit_reason == PlatformExitReason::None) {
        g_exit_reason = PlatformExitReason::Unknown;
    }
    
    if (!g_running) {
        const char* reason_str = "Unknown";
        switch (g_exit_reason) {
            case PlatformExitReason::WmClose: reason_str = "WM_CLOSE"; break;
            case PlatformExitReason::WmDestroy: reason_str = "WM_DESTROY"; break;
            case PlatformExitReason::WmQuit: reason_str = "WM_QUIT"; break;
            case PlatformExitReason::Escape: reason_str = "Escape"; break;
            default: break;
        }
        AWL_LOG_INFO("platform_pump_messages exiting. Reason: %s", reason_str);
    }

    return g_running;
}

PlatformExitReason platform_get_exit_reason() {
    return g_exit_reason;
}

void time_begin_frame() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_time_start.QuadPart != 0 && g_timer_freq.QuadPart > 0) {
        g_delta_time = static_cast<double>(now.QuadPart - g_time_start.QuadPart) /
                       static_cast<double>(g_timer_freq.QuadPart);
    }
    g_time_start = now;
    if (g_delta_time < 0.0) {
        g_delta_time = 0.0;
    }
    // Clamp pauses and window drags before a future game-state update uses dt.
    if (g_delta_time > 0.1) {
        g_delta_time = 0.1;
    }
}

double time_get_delta() {
    return g_delta_time;
}

void input_init() {
    g_input.reset(g_hwnd && GetForegroundWindow() == g_hwnd);
    g_pad.reset();
    AWL_LOG_INFO("Native keyboard and XInput capture initialized.");
}
void input_shutdown() {
    g_input.reset(false);
    g_pad.reset();
    AWL_LOG_INFO("Native input capture shut down.");
}

void input_begin_frame() {
    g_input.set_focused(g_hwnd && GetForegroundWindow() == g_hwnd);
    NativeGamepadState gamepad;
    if (g_input.focused()) {
        XINPUT_STATE state = {};
        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            gamepad.connected = true;
            gamepad.buttons = state.Gamepad.wButtons;
            gamepad.left_trigger = state.Gamepad.bLeftTrigger;
            gamepad.right_trigger = state.Gamepad.bRightTrigger;
            gamepad.left_x = state.Gamepad.sThumbLX;
            gamepad.left_y = state.Gamepad.sThumbLY;
            gamepad.right_x = state.Gamepad.sThumbRX;
            gamepad.right_y = state.Gamepad.sThumbRY;
        }
    }
    g_input.set_gamepad(gamepad);
    g_input.begin_frame();
    g_pad.begin_frame(g_input.frame());
}

const NativeInputFrame& input_frame() {
    return g_input.frame();
}

const PadFrame& pad_frame() {
    return g_pad.frame();
}

void audio_init() {
    AWL_LOG_INFO("Audio init.");
}
void audio_shutdown() {
    AWL_LOG_INFO("Audio shutdown.");
}

} // namespace awl
