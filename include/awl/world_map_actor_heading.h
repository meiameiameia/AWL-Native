#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace awl {

// Supplied fields of the first world-map actor used by FUN_8015C058.
struct WorldMapFirstActorHeadingState {
    int32_t actor_id = 0;
    int32_t action_code = 0; // actor +0xD8
    std::array<float, 3> position{}; // actor +0xFC
    std::array<float, 3> target{}; // actor +0x108
    std::array<float, 3> heading{}; // actor +0xF0
    int32_t facing_degrees = 0; // actor +0xEC
    bool moving = false; // actor +0xC1
};

struct WorldMapFirstActorHeadingResult {
    WorldMapFirstActorHeadingState state{};
    int32_t previous_facing_degrees = 0; // actor +0x120
    bool target_rebuilt = false;
    bool used_random_angle = false;
};

// Isolates FUN_8015C058's action gate, bounded destination, and heading
// update. A caller RNG word is required only when position is within 15
// three-dimensional units of the actor-specific anchor. The actor, action,
// position, and old state remain caller supplied; no scene object is changed.
// Native trigonometry can differ at exact angular boundaries. Failure is
// atomic and does not guess an RNG word or invalid vector.
[[nodiscard]] bool build_world_map_first_actor_heading(
    const WorldMapFirstActorHeadingState& state,
    std::optional<uint32_t> angle_rng_word,
    WorldMapFirstActorHeadingResult* result);

} // namespace awl
