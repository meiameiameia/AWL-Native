#include "awl/platform.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace awl {

static HWND g_hwnd = nullptr;
static bool g_running = true;
static PlatformExitReason g_exit_reason = PlatformExitReason::None;

// Timing state
static LARGE_INTEGER g_timer_freq;
static LARGE_INTEGER g_time_start;
static LARGE_INTEGER g_time_end;
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
}

void platform_shutdown() {
    AWL_LOG_INFO("Platform shutdown.");
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
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
            }
            return 0;
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
    QueryPerformanceCounter(&g_time_start);
}

void time_end_frame() {
    QueryPerformanceCounter(&g_time_end);
    g_delta_time = static_cast<double>(g_time_end.QuadPart - g_time_start.QuadPart) / g_timer_freq.QuadPart;
    
    // Clamp delta time to prevent huge simulation jumps after breakpoints or dragging the window
    if (g_delta_time > 0.1) {
        g_delta_time = 0.1;
    }
}

double time_get_delta() {
    return g_delta_time;
}

// Subsystem stub implementations for now
void input_init() {
    AWL_LOG_INFO("Input init.");
}
void input_shutdown() {
    AWL_LOG_INFO("Input shutdown.");
}

void input_begin_frame() {
}

void input_end_frame() {
}

void audio_init() {
    AWL_LOG_INFO("Audio init.");
}
void audio_shutdown() {
    AWL_LOG_INFO("Audio shutdown.");
}

} // namespace awl
