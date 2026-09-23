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

void update_world_map_steering(const HsdPadFrame& pad,
                               float camera_yaw_radians,
                               WorldMapSteeringState& state);

} // namespace awl
