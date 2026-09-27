#pragma once

#include "awl/player_input.h"
#include "awl/world_map_contact.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace awl {

// Caller-supplied state for the supported category-1 path through
// FUN_8003083C. The directional and resolver views of the first object list
// must refer to the same runtime entries in the same order.
struct WorldMapMovementQuery {
    int32_t state_680 = -1;
    int32_t state_58c = 0;
    HsdPadFrame pad{};
    float camera_yaw_radians = 0.0f;
    std::array<float, 3> current_position{};
    std::array<float, 3> current_axis{};
    WorldMapSteeringState steering{};
    const WorldMapContactObject* directional_objects = nullptr;
    size_t directional_object_count = 0;
    CollisionCategory1MovementQuery collision{};
};

struct WorldMapMovementCandidate {
    bool movement_enabled = false;
    WorldMapSteeringState steering{};
    std::array<float, 3> proposed_position{};
    std::array<float, 3> resolved_position{};
    WorldMapContactResult directional_contact{};
    CollisionCategory1MovementAdjustment collision{};
};

// Composes steering, the pre-collision proposal, directional contact, and the
// supported type-1 collision resolver in DOL call order. The result stops
// before FUN_800107A4's scene-object position/lookup update, camera changes,
// animation, or any actual gameplay state mutation.
[[nodiscard]] bool calculate_world_map_movement_candidate(
    const WorldMapMovementQuery& query,
    WorldMapMovementCandidate* candidate);

} // namespace awl
