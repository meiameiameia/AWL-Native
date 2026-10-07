#include "awl/startup_cli.h"
#include <cctype>
#include <cstring>
#include <new>
#include <utility>

namespace awl {
bool valid_movement_recording_path(const std::string& path) {
    if (path.size() < 12 || path.size() > 140 ||
        (path.compare(0, 6, "build/") != 0 && path.compare(0, 6, "build\\") != 0) ||
        path.compare(path.size() - 5, 5, ".awlr") != 0) return false;
    std::string base = path.substr(6, path.size() - 11);
    for (char& c : base) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
    if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 ||
                             base.compare(0, 3, "LPT") == 0) &&
        base[3] >= '1' && base[3] <= '9') return false;
    return true;
}

bool parse_startup_options(int argc, const char* const* argv,
                           StartupOptions* output, std::string* error) {
    if (!output || !error || argc < 1 || !argv) return false;
    try {
        StartupOptions next;
        bool selected = false;
        for (int i = 1; i < argc; ++i) {
            if (!argv[i]) { *error = "Null argument."; return false; }
            const std::string argument = argv[i];
            if (argument == "--record-movement") {
                if (!next.recording_path.empty() || i + 1 >= argc || !argv[i + 1]) {
                    *error = "--record-movement requires one unused output path."; return false;
                }
                next.recording_path = argv[++i];
                if (!valid_movement_recording_path(next.recording_path)) {
                    *error = "Recording path must be build/<name>.awlr (letters, digits, - or _).";
                    return false;
                }
                continue;
            }
            struct ModeName { const char* name; StartupMode mode; };
            constexpr ModeName modes[] = {
                {"--help", StartupMode::Help}, {"--verify-disc", StartupMode::VerifyDisc},
                {"--replay-movement", StartupMode::ReplayMovement},
                {"--target-smoke", StartupMode::TargetSmoke},
                {"--preview-smoke", StartupMode::PreviewSmoke},
                {"--scene-smoke", StartupMode::SceneSmoke},
                {"--movement-rehearsal", StartupMode::Movement},
                {"--movement-rehearsal-smoke", StartupMode::MovementSmoke},
                {"--movement-wall-rehearsal", StartupMode::Wall},
                {"--movement-wall-rehearsal-smoke", StartupMode::WallSmoke},
                {"--movement-actor-rehearsal", StartupMode::Actor},
                {"--movement-actor-rehearsal-smoke", StartupMode::ActorSmoke}
            };
            bool found = false;
            for (const auto& mode : modes) {
                if (argument != mode.name) continue;
                if (selected) { *error = "Select exactly one mode; duplicates also reject."; return false; }
                next.mode = mode.mode;
                selected = found = true;
                break;
            }
            if (!found) { *error = "Unknown argument: " + argument; return false; }
            if (next.mode == StartupMode::ReplayMovement) {
                if (i + 1 >= argc || !argv[i + 1]) {
                    *error = "--replay-movement requires an input path."; return false;
                }
                next.replay_path = argv[++i];
                if (next.replay_path.empty() || next.replay_path[0] == '-') {
                    *error = "Invalid replay input path."; return false;
                }
            }
        }
        if (!next.recording_path.empty() && next.mode != StartupMode::Movement &&
            next.mode != StartupMode::MovementSmoke) {
            *error = "Recording requires --movement-rehearsal or its smoke (seam profile).";
            return false;
        }
        *output = std::move(next);
        error->clear();
        return true;
    } catch (const std::bad_alloc&) {
        *error = "Could not allocate startup options.";
        return false;
    }
}

const char* startup_usage() {
    return "AWL Native development harness. Run from the repository root.\n"
           "One mode: --help | --verify-disc | --target-smoke | --preview-smoke |\n"
           "  --scene-smoke | --movement-rehearsal[-smoke] |\n"
           "  --movement-wall-rehearsal[-smoke] | --movement-actor-rehearsal[-smoke]\n"
           "Record: --movement-rehearsal[-smoke] --record-movement build/<name>.awlr\n"
           "Replay/check (no window): --replay-movement <file.awlr>\n"
           "Default opens the ground harness. --verify-disc never opens a window.\n";
}
} // namespace awl
