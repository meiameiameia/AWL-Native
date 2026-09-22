#pragma once

#include "awl/types.h"

namespace awl {

// Logging infrastructure
void logging_init();
void log_info(const char* fmt, ...);
void log_error(const char* fmt, ...);

#define AWL_LOG_INFO(fmt, ...) awl::log_info(fmt, ##__VA_ARGS__)
#define AWL_LOG_ERROR(fmt, ...) awl::log_error(fmt, ##__VA_ARGS__)

// Platform and Window
void platform_init();
void platform_shutdown();

void window_init();
void window_shutdown();
void* platform_get_window_handle();

enum class PlatformExitReason {
    None,
    WmClose,
    WmDestroy,
    WmQuit,
    Escape,
    Unknown
};

// Returns true if the application should keep running, false if a quit was requested
bool platform_pump_messages();

// Returns the reason the platform loop exited
PlatformExitReason platform_get_exit_reason();

// Timing infrastructure
void time_begin_frame();
void time_end_frame();
double time_get_delta(); // Actual delta time, primarily for internal use/metrics

// Input
void input_init();
void input_shutdown();
void input_begin_frame();
void input_end_frame();

// Audio
void audio_init();
void audio_shutdown();

} // namespace awl
