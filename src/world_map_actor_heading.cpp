#include "awl/world_map_actor_heading.h"

#include <cmath>

namespace awl {

namespace {

struct Anchor {
    std::array<float, 3> point;
    float radius;
};

// 0x802558A8 (12-byte point rows) and 0x80255948 (radius rows).
constexpr std::array<Anchor, 5> kAnchors{{
    {{263.0f, 20.0f, 179.0f}, 4.0f},
    {{277.0f, 20.0f, 218.0f}, 2.0f},
    {{185.0f, 13.0f, 237.0f}, 3.0f},
    {{75.0f, 0.0f, 113.5f}, 1.0f},
    {{279.0f, 27.0f, 116.0f}, 1.0f}}};

bool finite_vector(const std::array<float, 3>& vector) {
    return std::isfinite(vector[0]) && std::isfinite(vector[1]) &&
           std::isfinite(vector[2]);
}

int32_t wrap_degrees(int32_t degrees) {
    const int32_t wrapped = degrees % 360;
    return wrapped < 0 ? wrapped + 360 : wrapped;
}

} // namespace

bool build_world_map_first_actor_heading(
    const WorldMapFirstActorHeadingState& state,
    std::optional<uint32_t> angle_rng_word,
    WorldMapFirstActorHeadingResult* result) {
    if (result == nullptr || state.actor_id < 0x2c || state.actor_id > 0x30 ||
        !finite_vector(state.position) || !finite_vector(state.target) ||
        !finite_vector(state.heading)) {
        return false;
    }
    WorldMapFirstActorHeadingResult next;
    next.state = state;
    next.previous_facing_degrees = state.facing_degrees;
    next.state.moving = false;

    const int32_t variant = state.actor_id - 0x2c;
    const bool rebuild = variant == 2 ?
        (state.action_code == 1 || state.action_code == 2) :
        (variant < 4 && state.action_code == 1);
    if (!rebuild) {
        *result = next;
        return true;
    }
    const Anchor& anchor = kAnchors[static_cast<size_t>(variant)];
    const double dx = static_cast<double>(anchor.point[0]) - state.position[0];
    const double dy = static_cast<double>(anchor.point[1]) - state.position[1];
    const double dz = static_cast<double>(anchor.point[2]) - state.position[2];
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance)) {
        return false;
    }
    int32_t angle_degrees = 0;
    if (distance <= 15.0) { // DOL r2-0x66BC, 0x8034B784.
        if (!angle_rng_word) {
            return false;
        }
        angle_degrees = static_cast<int32_t>(*angle_rng_word % 360u);
        next.used_random_angle = true;
    } else {
        constexpr float kDegreesPerRadian = 57.2957802f; // 0x8034B780
        const float radians = static_cast<float>(std::atan2(dx, dz));
        angle_degrees = wrap_degrees(static_cast<int32_t>(
            kDegreesPerRadian * radians));
    }

    constexpr float kRadiansPerDegree = 0.0174532924f; // 0x8034B774
    const float radians = kRadiansPerDegree * angle_degrees;
    const float direction_x = std::sin(radians);
    const float direction_z = std::cos(radians);
    next.state.target = {
        state.position[0] + anchor.radius * direction_x,
        state.position[1],
        state.position[2] + anchor.radius * direction_z};
    if (!finite_vector(next.state.target)) {
        return false;
    }
    const double heading_x = static_cast<double>(next.state.target[0]) -
                             state.position[0];
    const double heading_z = static_cast<double>(next.state.target[2]) -
                             state.position[2];
    const double length = std::sqrt(heading_x * heading_x +
                                    heading_z * heading_z);
    if (!std::isfinite(length) || length == 0.0) {
        return false;
    }
    next.state.heading = {
        static_cast<float>(heading_x / length), 0.0f,
        static_cast<float>(heading_z / length)};
    const float bearing = static_cast<float>(std::atan2(
        next.state.heading[0], next.state.heading[2]));
    constexpr float kDegreesPerRadian = 57.2957802f;
    next.state.facing_degrees = wrap_degrees(static_cast<int32_t>(
        kDegreesPerRadian * bearing));
    next.state.moving = true;
    next.target_rebuilt = true;
    *result = next;
    return true;
}

} // namespace awl
