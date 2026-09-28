#include "awl/player_input.h"

#include <algorithm>
#include <cmath>

namespace awl {

namespace {

// Runtime constants read by FUN_8003083C from the verified target DOL.
constexpr float kHighMagnitudeSquared = 4678.5596f;
constexpr float kMediumMagnitudeSquared = 1049.76f;
constexpr float kHighSpeed = 0.18f;
constexpr float kMediumSpeed = 0.09f;
constexpr float kLowSpeed = 0.06f;
constexpr float kSpeedStep = 0.03f;
constexpr float kRadiansPerDegree = 0.017453292f;
constexpr float kTurnRateDegrees = 4.0f;
constexpr float kContactDirectionThreshold = 0.8660254f;
constexpr float kContactAngleDegrees[4] = {0.0f, 180.0f, 90.0f, -90.0f};

} // namespace

void update_world_map_steering(const HsdPadFrame& pad,
                               float camera_yaw_radians,
                               WorldMapSteeringState& state) {
    const float x = static_cast<float>(pad.stick_x);
    const float z = -static_cast<float>(pad.stick_y);
    const float magnitude_squared = x * x + z * z;

    if (magnitude_squared == 0.0f) {
        // The original retains the last direction while decelerating.
        state.target_speed = 0.0f;
    } else {
        const float inverse_magnitude = 1.0f / std::sqrt(magnitude_squared);
        state.direction_x = x * inverse_magnitude;
        state.direction_z = z * inverse_magnitude;
        // FUN_8003083C calls atan2(x, z), adds camera yaw, constructs the
        // verified Y-axis rotation, and transforms the unit-forward vector.
        const float angle = std::atan2(state.direction_x, state.direction_z) +
                            camera_yaw_radians;
        state.facing_x = std::sin(angle);
        state.facing_z = std::cos(angle);
        if (magnitude_squared >= kHighMagnitudeSquared) {
            state.target_speed = kHighSpeed;
        } else if (magnitude_squared >= kMediumMagnitudeSquared) {
            state.target_speed = kMediumSpeed;
        } else {
            state.target_speed = kLowSpeed;
        }
    }

    if (state.current_speed > state.target_speed) {
        state.current_speed = std::max(0.0f, state.current_speed - kSpeedStep);
    } else if (state.current_speed < state.target_speed) {
        state.current_speed = std::min(state.target_speed,
                                       state.current_speed + kSpeedStep);
    }

    if (state.current_speed <= 0.0f) {
        state.intensity = 0.0f;
    } else if (state.current_speed >= kHighSpeed) {
        state.intensity = 1.0f;
    } else {
        state.intensity = state.current_speed / kHighSpeed;
    }
}

WorldMapPosition propose_world_map_position(
    const WorldMapPosition& current_position,
    float camera_yaw_radians,
    const WorldMapSteeringState& steering) {
    return propose_world_map_position_with_camera(
               current_position, camera_yaw_radians, steering, false)
        .position;
}

WorldMapPositionProposal propose_world_map_position_with_camera(
    const WorldMapPosition& current_position,
    float camera_yaw_radians,
    const WorldMapSteeringState& steering,
    bool camera_yaw_commit_enabled) {
    const float scaled_x = steering.direction_x * steering.current_speed;
    const float scaled_z = steering.direction_z * steering.current_speed;
    const float yaw_correction =
        kRadiansPerDegree * (scaled_x * kTurnRateDegrees);
    const float x_component_yaw = camera_yaw_radians + yaw_correction;
    const float z_component_yaw = camera_yaw_radians - yaw_correction;

    // FUN_8003083C rotates (scaled_x, 0, 0) and (0, 0, scaled_z)
    // separately before adding both vectors to the current position.
    WorldMapPosition proposed = current_position;
    proposed.x += std::cos(x_component_yaw) * scaled_x;
    proposed.z -= std::sin(x_component_yaw) * scaled_x;
    proposed.x += std::sin(z_component_yaw) * scaled_z;
    proposed.z += std::cos(z_component_yaw) * scaled_z;
    return {proposed,
            camera_yaw_commit_enabled ? z_component_yaw : camera_yaw_radians,
            camera_yaw_commit_enabled};
}

bool classify_world_map_directional_contact(
    const WorldMapPosition& prior_position,
    const WorldMapPosition& proposed_position,
    float contact_axis_x,
    float contact_axis_z,
    uint32_t contact_mask,
    uint8_t* direction_code) {
    if (direction_code == nullptr) {
        return false;
    }
    *direction_code = 7;
    const float delta_x = proposed_position.x - prior_position.x;
    const float delta_z = proposed_position.z - prior_position.z;
    if (!std::isfinite(delta_x) || !std::isfinite(delta_z) ||
        !std::isfinite(contact_axis_x) || !std::isfinite(contact_axis_z) ||
        (contact_axis_x == 0.0f && contact_axis_z == 0.0f) ||
        (contact_mask & ~0xFu) != 0) {
        return false;
    }
    const float distance = std::sqrt(delta_x * delta_x + delta_z * delta_z);
    if (distance == 0.0f || !std::isfinite(distance)) {
        return false;
    }
    const float movement_x = delta_x / distance;
    const float movement_z = delta_z / distance;
    const float contact_angle = std::atan2(contact_axis_x, contact_axis_z);
    for (uint8_t index = 0; index < 4; ++index) {
        if ((contact_mask & (1u << index)) == 0) {
            continue;
        }
        const float angle = contact_angle +
                            kContactAngleDegrees[index] * kRadiansPerDegree;
        const float dot = std::sin(angle) * movement_x +
                          std::cos(angle) * movement_z;
        if (-dot >= kContactDirectionThreshold) {
            if (index < 2) {
                *direction_code = index;
            } else {
                uint8_t code = index == 2 ? 3 : 5;
                if ((contact_mask & 1u) != 0) {
                    --code;
                }
                *direction_code = code;
            }
            return true;
        }
    }
    return false;
}

} // namespace awl
