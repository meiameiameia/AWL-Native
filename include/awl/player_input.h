#pragma once

#include "awl/input.h"

#include <cstdint>

namespace awl {

// Pure, DOL-backed steering portion of FUN_8003083C. The supplied-data
// sequence in world_map_movement.h composes this with collision, but no live
// player controller exists yet.
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
// must not treat this as an accepted position; the later world and collision
// operations are only available as caller-supplied, isolated helpers.
[[nodiscard]] WorldMapPosition propose_world_map_position(
    const WorldMapPosition& current_position,
    float camera_yaw_radians,
    const WorldMapSteeringState& steering);

// Isolates the directional-code selection after FUN_8001DE44 receives a
// contact mask. The caller supplies a verified mask; world_map_contact.h
// composes the supported type-1 query for caller-supplied objects.
// Returns false with code 7 for no qualifying direction or unsupported input.
[[nodiscard]] bool classify_world_map_directional_contact(
    const WorldMapPosition& prior_position,
    const WorldMapPosition& proposed_position,
    float contact_axis_x,
    float contact_axis_z,
    uint32_t contact_mask,
    uint8_t* direction_code);

} // namespace awl
