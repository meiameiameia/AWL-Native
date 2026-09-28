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
    bool yaw_adjustment_called = false; // FUN_80085FD0
    bool yaw_adjustment_written = false; // prior flag +0x98 was nonzero
};

// Isolates FUN_80030E18's direct writes, region selection, and the conditional
// FUN_80085FD0 yaw adjustment from signed PAD byte 0x8034158E. The adjustment
// checks the *previous* +0x98 flag before the outside branch sets it to one.
// FUN_80085884 and FUN_80085998 run afterward in the DOL and remain unknown.
// On invalid input, returns false without changing output.
[[nodiscard]] bool plan_world_map_camera_followup(
    const WorldMapCameraFollowupState& previous, int32_t category,
    const std::array<float, 3>& resolved_position,
    uint8_t global_byte_3f1, int8_t pad_byte_8e,
    WorldMapCameraFollowup* output);

// Inputs read by FUN_8017B908 from the camera object. The region follow-up
// supplies position, pitch (+0x18), yaw (+0x1C), flag, and bounds; the other
// fields remain caller supplied until the camera constructor is translated.
struct WorldMapCameraTargetQuery {
    WorldMapCameraFollowupState camera{};
    std::array<float, 3> origin_offset_0c{};
    float distance_30 = 0.0f;
    float pitch_offset_8c = 0.0f;
    float yaw_offset_90 = 0.0f;
};

struct WorldMapCameraTarget {
    std::array<float, 3> raw{};
    std::array<float, 3> bounded{};
    bool clamped = false;
};

// Isolates FUN_8017B834's target calculation (FUN_8017B908) and optional
// per-axis bounds (FUN_8017B9C4). The later target offsets, up vector, view
// matrix, and terrain adjustment remain untranslated. Failure is atomic.
[[nodiscard]] bool calculate_world_map_camera_target(
    const WorldMapCameraTargetQuery& query, WorldMapCameraTarget* output);

struct WorldMapCameraViewQuery {
    WorldMapCameraTargetQuery target{};
    std::array<float, 3> up_vector_24{};
    float pitch_offset_44 = 0.0f;
    float yaw_offset_48 = 0.0f;
};

struct WorldMapCameraView {
    WorldMapCameraTarget target{};
    std::array<float, 3> second_point{};
    std::array<float, 3> rotated_up{};
    std::array<float, 12> matrix_50{}; // three rows of four floats
};

// Completes the supplied-data FUN_8017B834 view calculation after the target
// clamp. The DOL normalizes two vectors without zero guards; this native
// helper rejects degenerate input and leaves output unchanged on failure.
[[nodiscard]] bool calculate_world_map_camera_view(
    const WorldMapCameraViewQuery& query, WorldMapCameraView* output);

struct WorldMapCameraPlane {
    std::array<float, 3> normal{}; // camera +0x174..+0x17C
    float constant = 0.0f; // camera +0x184
};

using WorldMapCameraHeightSampler = bool (*)(
    const std::array<float, 3>& target, float* height, void* context);

struct WorldMapCameraPostUpdate {
    WorldMapCameraView first_view{}; // FUN_80085884
    WorldMapCameraPlane first_plane{};
    float temporary_pitch_offset_8c = 0.0f;
    WorldMapCameraView pitched_view{}; // FUN_80085998, before terrain clamp
    WorldMapCameraPlane final_plane{};
    std::array<float, 3> final_target{};
    std::array<float, 12> final_matrix_50{};
    bool terrain_clamped = false;
};

// Runs the bounded FUN_80085884 -> FUN_80085998 sequence. The sampler is
// called first at first_view.target, then at pitched_view.target. It replaces
// the unresolved runtime FUN_8001CC20 asset owner; a failed/nonfinite sample
// rejects the result. Camera fields +0x8C/+0x90/+0x94 end at zero in the DOL.
// Output is unchanged on failure; the caller owns sampler side effects.
[[nodiscard]] bool calculate_world_map_camera_post_update(
    const WorldMapCameraViewQuery& query,
    WorldMapCameraHeightSampler sample_height, void* sample_context,
    WorldMapCameraPostUpdate* output);

} // namespace awl
