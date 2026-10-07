#pragma once
#include "awl/disc_identity.h"
#include "awl/world_map_movement_runtime.h"
#include <array>
#include <vector>

namespace awl {
inline constexpr size_t kMaxMovementRecordingTicks = 18000; // Ten minutes at 30 Hz.
inline constexpr size_t kMovementStateWords = 23;
using MovementStateWords = std::array<uint32_t, kMovementStateWords>;
struct MovementRecordedTick {
    bool reset_before = false; // Native focus pause before this tick.
    PadSample input;
    MovementStateWords state{};
};
struct MovementRecording {
    // Version 1: seam fixture, phase 0/main terrain, scene type 0, yaw 0,
    // axis +Z, default movement guards, empty dynamic lists, rate 30/1.
    std::array<float, 3> spawn{};
    Sha1 terrain_sha1{};
    Sha1 static_sha1{};
    std::vector<MovementRecordedTick> ticks;
};
// Explicit float bit patterns and integer fields; no struct padding or asset bytes.
MovementStateWords capture_movement_state(const WorldMapMovementRuntime& runtime,
                                          const WorldMapMovementRuntimeStep& step);
const char* movement_state_word_name(size_t index);
// Bounded versioned little-endian native evidence. Failed calls preserve outputs.
[[nodiscard]] bool encode_movement_recording(const MovementRecording& recording,
                                            std::vector<uint8_t>* output);
[[nodiscard]] bool decode_movement_recording(const uint8_t* data, size_t size,
                                            MovementRecording* output);
[[nodiscard]] bool load_movement_recording(const char* path, MovementRecording* output);
// Exclusive create under plain build/: never overwrites a previous recording.
[[nodiscard]] bool save_movement_recording(const char* path,
                                          const MovementRecording& recording);
} // namespace awl
