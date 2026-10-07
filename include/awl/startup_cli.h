#pragma once
#include <string>

namespace awl {
enum class StartupMode {
    Default, Help, VerifyDisc, ReplayMovement,
    TargetSmoke, PreviewSmoke, SceneSmoke,
    Movement, MovementSmoke, Wall, WallSmoke, Actor, ActorSmoke
};
struct StartupOptions {
    StartupMode mode = StartupMode::Default;
    std::string recording_path;
    std::string replay_path;
};
// Strict, single-mode parsing. Errors preserve output and explain the failure.
[[nodiscard]] bool parse_startup_options(int argc, const char* const* argv,
                                         StartupOptions* output, std::string* error);
// Recordings are local evidence: a plain build/<name>.awlr filename only.
[[nodiscard]] bool valid_movement_recording_path(const std::string& path);
const char* startup_usage();
} // namespace awl
