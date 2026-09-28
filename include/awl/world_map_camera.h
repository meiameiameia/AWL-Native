#pragma once

#include <array>
#include <cstdint>

namespace awl {

// Fields observed in FUN_80030E18. The caller owns the camera and supplies
// the resolved position; this structure is not a native runtime camera.
struct WorldMapCameraFollowupState {
    std::array<float, 3> position{}; // camera +0x00
    float field_18 = 0.0f;
    float yaw = 0.0f; // +0x1C
    std::array<float, 3> bounds_min{}; // +0x9C
    std::array<float, 3> bounds_max{}; // +0xA8
    bool flag_98 = false;
    bool flag_99 = false;
};

struct WorldMapCameraFollowup {
    WorldMapCameraFollowupState state{};
    int32_t region_index = -1;
    bool reset_call_requested = false; // FUN_80085FD0, not translated here
};

// Isolates the direct field writes and region selection in FUN_80030E18.
// FUN_80085884 and FUN_80085998 run afterward in the DOL and remain unknown.
// On invalid input, returns false without changing output.
[[nodiscard]] bool plan_world_map_camera_followup(
    const WorldMapCameraFollowupState& previous, int32_t category,
    const std::array<float, 3>& resolved_position,
    uint8_t global_byte_3f1, WorldMapCameraFollowup* output);

} // namespace awl
