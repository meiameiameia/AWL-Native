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
    uint8_t global_byte_3f1, WorldMapCameraFollowup* output) {
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
        next.reset_call_requested = global_byte_3f1 == 0;
        next.state.field_18 = -0.14835298f; // r2-0x7CA4
        next.state.flag_99 = false;
        next.state.flag_98 = true;
    }
    *output = next;
    return true;
}

} // namespace awl
