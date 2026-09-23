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

} // namespace

void update_world_map_steering(const HsdPadFrame& pad,
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

} // namespace awl
