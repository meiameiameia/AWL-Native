#include "awl/world_map_camera.h"

#include <cmath>
#include <cstddef>
#include <utility>

namespace awl {

namespace {

struct CameraRegion {
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    float yaw;
    std::array<float, 3> first_corner;
    std::array<float, 3> second_corner;
};

// FUN_8001D0E8's ordered, inclusive rectangles (r2-0x7E64..-0x7E3C)
// and FUN_80030E18's profile rows at 0x8029FE10, stride 0x1C.
constexpr CameraRegion kRegions[] = {
    {171.0f, 180.0f, 111.0f, 118.0f, 0.0f,
     {173.5f, 0.0f, 120.5f}, {177.6f, 100.0f, 122.3f}},
    {192.0f, 199.0f, 111.0f, 120.0f, 0.0f,
     {194.5f, 0.0f, 119.6f}, {196.6f, 100.0f, 123.6f}},
    {216.0f, 229.0f, 112.0f, 130.0f, 4.71238899f,
     {211.1f, 0.0f, 115.2f}, {219.6f, 100.0f, 125.8f}},
};

bool finite_vector(const std::array<float, 3>& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool finite_state(const WorldMapCameraFollowupState& value) {
    return finite_vector(value.position) && std::isfinite(value.field_18) &&
           std::isfinite(value.yaw) && finite_vector(value.bounds_min) &&
           finite_vector(value.bounds_max);
}

} // namespace

bool plan_world_map_camera_followup(
    const WorldMapCameraFollowupState& previous, int32_t category,
    const std::array<float, 3>& resolved_position,
    uint8_t global_byte_3f1, int8_t pad_byte_8e,
    WorldMapCameraFollowup* output) {
    if (output == nullptr || !finite_state(previous) ||
        !finite_vector(resolved_position)) {
        return false;
    }
    WorldMapCameraFollowup next;
    next.state = previous;
    next.state.position = resolved_position;
    if (category == 1) {
        for (int32_t index = 0; index < 3; ++index) {
            const CameraRegion& region = kRegions[index];
            if (resolved_position[0] < region.min_x ||
                resolved_position[0] > region.max_x ||
                resolved_position[2] < region.min_z ||
                resolved_position[2] > region.max_z) {
                continue;
            }
            next.region_index = index;
            next.state.field_18 = -1.012291f; // r2-0x7CA8
            next.state.yaw = region.yaw;
            next.state.bounds_min = region.first_corner;
            next.state.bounds_max = region.second_corner;
            for (size_t axis = 0; axis < 3; ++axis) {
                if (next.state.bounds_min[axis] > next.state.bounds_max[axis]) {
                    std::swap(next.state.bounds_min[axis],
                              next.state.bounds_max[axis]);
                }
            }
            next.state.flag_99 = true;
            next.state.flag_98 = false;
            *output = next;
            return true;
        }
        if (global_byte_3f1 == 0) {
            next.yaw_adjustment_called = true;
            if (previous.flag_98) {
                // FUN_80085FD0: signed PAD byte at 0x8034158E, scaled by
                // r2-0x77D8 (1/256); square and restore its sign against
                // r2-0x77FC (zero) before adding to camera +0x1C.
                const float scaled =
                    static_cast<float>(pad_byte_8e) * 0.00390625f;
                const float square = scaled * scaled;
                const float delta = scaled < 0.0f ? -square : square;
                next.state.yaw = previous.yaw + delta;
                if (!std::isfinite(next.state.yaw)) {
                    return false;
                }
                next.yaw_adjustment_written = true;
            }
        }
        next.state.field_18 = -0.14835298f; // r2-0x7CA4
        next.state.flag_99 = false;
        next.state.flag_98 = true;
    }
    *output = next;
    return true;
}

bool calculate_world_map_camera_target(
    const WorldMapCameraTargetQuery& query, WorldMapCameraTarget* output) {
    if (output == nullptr || !finite_state(query.camera) ||
        !finite_vector(query.origin_offset_0c) ||
        !std::isfinite(query.distance_30) ||
        !std::isfinite(query.pitch_offset_8c) ||
        !std::isfinite(query.yaw_offset_90)) {
        return false;
    }
    const float pitch = query.camera.field_18 + query.pitch_offset_8c;
    const float yaw = query.camera.yaw + query.yaw_offset_90;
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }

    // FUN_8017B908 rotates (0, 0, +0x30) by the X matrix at pitch and then
    // the Y matrix at yaw, before adding camera +0x00 and +0x0C.
    const float horizontal = query.distance_30 * std::cos(pitch);
    const std::array<float, 3> rotated{
        horizontal * std::sin(yaw),
        -query.distance_30 * std::sin(pitch),
        horizontal * std::cos(yaw)};
    WorldMapCameraTarget next;
    for (size_t axis = 0; axis < 3; ++axis) {
        next.raw[axis] = query.camera.position[axis] +
                         query.origin_offset_0c[axis] + rotated[axis];
    }
    if (!finite_vector(next.raw)) {
        return false;
    }
    next.bounded = next.raw;
    if (query.camera.flag_99) {
        // FUN_8017B9C4 compares lower and then upper bound on each axis.
        for (size_t axis = 0; axis < 3; ++axis) {
            if (next.bounded[axis] < query.camera.bounds_min[axis]) {
                next.bounded[axis] = query.camera.bounds_min[axis];
            }
            if (next.bounded[axis] > query.camera.bounds_max[axis]) {
                next.bounded[axis] = query.camera.bounds_max[axis];
            }
        }
        next.clamped = next.bounded != next.raw;
    }
    *output = next;
    return true;
}

} // namespace awl
