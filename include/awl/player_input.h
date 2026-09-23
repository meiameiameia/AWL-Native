#pragma once

#include "awl/input.h"

namespace awl {

// Pure, DOL-backed front half of FUN_8003083C. This is not a complete player
// controller: position updates, collision, animation, and state guards remain
// outside this boundary.
struct WorldMapSteeringState {
    float direction_x = 0.0f;
    float direction_z = 0.0f;
    float facing_x = 0.0f;
    float facing_z = 0.0f;
    float target_speed = 0.0f;
    float current_speed = 0.0f;
    float intensity = 0.0f;
};

struct WorldMapPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

void update_world_map_steering(const HsdPadFrame& pad,
                               float camera_yaw_radians,
                               WorldMapSteeringState& state);

// Reproduces the pre-collision position proposal in FUN_8003083C. The caller
// must not treat this as an accepted position until the untranslated world and
// collision operations have run.
[[nodiscard]] WorldMapPosition propose_world_map_position(
    const WorldMapPosition& current_position,
    float camera_yaw_radians,
    const WorldMapSteeringState& steering);

} // namespace awl
