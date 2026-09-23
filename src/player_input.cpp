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
    return proposed;
}

} // namespace awl
