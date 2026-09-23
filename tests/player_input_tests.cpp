#include "awl/player_input.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool near(float actual, float expected, float tolerance = 0.00001f) {
    return std::fabs(actual - expected) <= tolerance;
}

awl::HsdPadFrame stick(int8_t x, int8_t y) {
    awl::HsdPadFrame frame;
    frame.stick_x = x;
    frame.stick_y = y;
    return frame;
}

} // namespace

int main() {
    awl::WorldMapSteeringState state;
    state.direction_x = 0.25f;
    state.direction_z = -0.75f;
    awl::update_world_map_steering(stick(0, 0), state);
    expect(state.target_speed == 0.0f && state.current_speed == 0.0f &&
           state.intensity == 0.0f && state.direction_x == 0.25f &&
           state.direction_z == -0.75f,
           "neutral input retains direction and has zero speed");

    state = {};
    awl::update_world_map_steering(stick(30, 0), state);
    expect(near(state.target_speed, 0.06f) &&
           near(state.current_speed, 0.03f) &&
           near(state.intensity, 1.0f / 6.0f) &&
           near(state.direction_x, 1.0f) && near(state.direction_z, 0.0f),
           "low stick magnitude selects the verified low speed tier");

    state = {};
    awl::update_world_map_steering(stick(40, 0), state);
    expect(near(state.target_speed, 0.09f) && near(state.current_speed, 0.03f),
           "medium stick magnitude selects the verified medium speed tier");

    state = {};
    awl::update_world_map_steering(stick(69, 0), state);
    expect(near(state.target_speed, 0.18f) && near(state.current_speed, 0.03f),
           "high stick magnitude selects the verified high speed tier");

    state = {};
    awl::update_world_map_steering(stick(30, 40), state);
    expect(near(state.direction_x, 0.6f) && near(state.direction_z, -0.8f),
           "stick direction is normalized and GameCube Y becomes steering-plane Z");

    state = {};
    awl::update_world_map_steering(stick(32, 0), state);
    expect(near(state.target_speed, 0.06f),
           "squared magnitude 1024 remains below the medium threshold");
    state = {};
    awl::update_world_map_steering(stick(33, 0), state);
    expect(near(state.target_speed, 0.09f),
           "squared magnitude 1089 crosses the medium threshold");
    state = {};
    awl::update_world_map_steering(stick(68, 0), state);
    expect(near(state.target_speed, 0.09f),
           "squared magnitude 4624 remains below the high threshold");

    state = {};
    for (int frame = 0; frame < 6; ++frame) {
        awl::update_world_map_steering(stick(69, 0), state);
        expect(near(state.current_speed, 0.03f * static_cast<float>(frame + 1)),
               "speed accelerates by the verified per-frame step");
    }
    expect(near(state.current_speed, 0.18f) && near(state.intensity, 1.0f),
           "high-speed acceleration saturates at unit intensity");

    awl::update_world_map_steering(stick(40, 0), state);
    expect(near(state.target_speed, 0.09f) && near(state.current_speed, 0.15f),
           "lower speed tier decelerates one step without snapping");
    awl::update_world_map_steering(stick(40, 0), state);
    awl::update_world_map_steering(stick(40, 0), state);
    expect(near(state.current_speed, 0.09f),
           "deceleration reaches the medium tier in verified steps");

    const float prior_x = state.direction_x;
    const float prior_z = state.direction_z;
    for (int frame = 0; frame < 3; ++frame) {
        awl::update_world_map_steering(stick(0, 0), state);
    }
    expect(near(state.current_speed, 0.0f) && near(state.intensity, 0.0f) &&
           near(state.direction_x, prior_x) && near(state.direction_z, prior_z),
           "neutral input decelerates to zero while retaining direction");

    if (failures == 0) {
        std::puts("Player input tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
