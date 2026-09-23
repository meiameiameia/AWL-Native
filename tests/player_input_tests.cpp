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

void update(const awl::HsdPadFrame& pad, awl::WorldMapSteeringState& state,
            float camera_yaw = 0.0f) {
    awl::update_world_map_steering(pad, camera_yaw, state);
}

} // namespace

int main() {
    awl::WorldMapSteeringState state;
    state.direction_x = 0.25f;
    state.direction_z = -0.75f;
    state.facing_x = -0.5f;
    state.facing_z = 0.5f;
    update(stick(0, 0), state, 1.0f);
    expect(state.target_speed == 0.0f && state.current_speed == 0.0f &&
           state.intensity == 0.0f && state.direction_x == 0.25f &&
           state.direction_z == -0.75f && state.facing_x == -0.5f &&
           state.facing_z == 0.5f,
           "neutral input retains direction and facing with zero speed");

    state = {};
    update(stick(30, 0), state);
    expect(near(state.target_speed, 0.06f) &&
           near(state.current_speed, 0.03f) &&
           near(state.intensity, 1.0f / 6.0f) &&
           near(state.direction_x, 1.0f) && near(state.direction_z, 0.0f) &&
           near(state.facing_x, 1.0f) && near(state.facing_z, 0.0f),
           "low stick magnitude selects the verified low speed tier");

    state = {};
    update(stick(40, 0), state);
    expect(near(state.target_speed, 0.09f) && near(state.current_speed, 0.03f),
           "medium stick magnitude selects the verified medium speed tier");

    state = {};
    update(stick(69, 0), state);
    expect(near(state.target_speed, 0.18f) && near(state.current_speed, 0.03f),
           "high stick magnitude selects the verified high speed tier");

    state = {};
    update(stick(30, 40), state);
    expect(near(state.direction_x, 0.6f) && near(state.direction_z, -0.8f),
           "stick direction is normalized and GameCube Y becomes steering-plane Z");

    state = {};
    update(stick(32, 0), state);
    expect(near(state.target_speed, 0.06f),
           "squared magnitude 1024 remains below the medium threshold");
    state = {};
    update(stick(33, 0), state);
    expect(near(state.target_speed, 0.09f),
           "squared magnitude 1089 crosses the medium threshold");
    state = {};
    update(stick(68, 0), state);
    expect(near(state.target_speed, 0.09f),
           "squared magnitude 4624 remains below the high threshold");

    state = {};
    for (int frame = 0; frame < 6; ++frame) {
        update(stick(69, 0), state);
        expect(near(state.current_speed, 0.03f * static_cast<float>(frame + 1)),
               "speed accelerates by the verified per-frame step");
    }
    expect(near(state.current_speed, 0.18f) && near(state.intensity, 1.0f),
           "high-speed acceleration saturates at unit intensity");

    update(stick(40, 0), state);
    expect(near(state.target_speed, 0.09f) && near(state.current_speed, 0.15f),
           "lower speed tier decelerates one step without snapping");
    update(stick(40, 0), state);
    update(stick(40, 0), state);
    expect(near(state.current_speed, 0.09f),
           "deceleration reaches the medium tier in verified steps");

    const float prior_x = state.direction_x;
    const float prior_z = state.direction_z;
    for (int frame = 0; frame < 3; ++frame) {
        update(stick(0, 0), state);
    }
    expect(near(state.current_speed, 0.0f) && near(state.intensity, 0.0f) &&
           near(state.direction_x, prior_x) && near(state.direction_z, prior_z),
           "neutral input decelerates to zero while retaining direction");

    constexpr float half_pi = 1.5707963267948966f;
    state = {};
    update(stick(0, -40), state);
    expect(near(state.direction_x, 0.0f) && near(state.direction_z, 1.0f) &&
           near(state.facing_x, 0.0f) && near(state.facing_z, 1.0f),
           "zero camera yaw preserves the forward steering direction");
    update(stick(0, -40), state, half_pi);
    expect(near(state.facing_x, 1.0f) && near(state.facing_z, 0.0f),
           "positive quarter-turn camera yaw rotates forward toward positive X");
    update(stick(40, 0), state, half_pi);
    expect(near(state.facing_x, 0.0f) && near(state.facing_z, -1.0f),
           "camera yaw composes with the stick-derived angle");

    if (failures == 0) {
        std::puts("Player input tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
