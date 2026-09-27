#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace awl {

// The FUN_80159A28 actor's fields used by the moving branch of
// FUN_8015C398. Values come from the caller's actor state; this is not a
// runtime owner or target-selection implementation.
struct WorldMapFirstActorStepState {
    int32_t base_constructor_id = 0;
    int32_t selector_d4 = 0;
    int32_t selector_d8 = 0;
    bool moving = false;
    uint32_t timer = 0;
    std::array<float, 3> proposal{};
    std::array<float, 3> target{};
    std::array<float, 3> heading{};
};

struct WorldMapFirstActorStepProposal {
    WorldMapFirstActorStepState state{};
    bool collision_required = false;
    bool target_reached = false;
};

struct WorldMapFirstActorStepResult {
    WorldMapFirstActorStepState state{};
    bool collision_altered_horizontal = false;
};

// Isolates FUN_8015C398 through its call to FUN_80152724. For constructor
// IDs 0x3D..0x41, the verified speed table supplies 1.95, divided by 30.
// No target acquisition, heading update, collision, or scene position write
// occurs here. Failure leaves the output unchanged.
[[nodiscard]] bool propose_world_map_first_actor_step(
    const WorldMapFirstActorStepState& state,
    WorldMapFirstActorStepProposal* proposal);

// Continues the same function after FUN_80152724. The caller supplies that
// actor-specific resolver's position. This replaces Y through FUN_8001CB54's
// verified type-1 height route and clears movement when collision changed X/Z.
// The raw terrain bytes must already be validated; failure is atomic.
[[nodiscard]] bool finalize_world_map_first_actor_step(
    const WorldMapFirstActorStepProposal& proposal,
    const std::array<float, 3>& collision_position,
    const uint8_t* terrain_data, size_t terrain_size,
    WorldMapFirstActorStepResult* result);

} // namespace awl
