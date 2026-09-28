#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
// FUN_80085884 and FUN_80085998 run afterward in the DOL; their supplied
// camera and height path is modeled separately below.
// On invalid input, returns false without changing output.
[[nodiscard]] bool plan_world_map_camera_followup(
    const WorldMapCameraFollowupState& previous, int32_t category,
    const std::array<float, 3>& resolved_position,
    uint8_t global_byte_3f1, int8_t pad_byte_8e,
    WorldMapCameraFollowup* output);

// Inputs read by FUN_8017B908 from the camera object. The region follow-up
// supplies position, pitch (+0x18), yaw (+0x1C), flag, and bounds; the other
// fields remain caller supplied except when using the bounded constructor
// profile helper below.
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
// per-axis bounds (FUN_8017B9C4). The later view and terrain stages are
// modeled below for supplied data. Failure is atomic.
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

struct WorldMapCameraInitialProfile {
    WorldMapCameraViewQuery view_query{};
    WorldMapCameraView initial_view{};
    float field_34 = 0.0f; // aspect input to FUN_801B8930
    float field_38 = 0.0f; // angle input to FUN_801B8930
    float field_3c = 0.0f; // near input to FUN_801B8930
    float field_40 = 0.0f; // far input to FUN_801B8930
    uint32_t mode_104 = 0;
};

// Bounded FUN_8008558C constructor path: FUN_8017B5F0 defaults, then the
// world-map profile copied by FUN_8017B6D4, initial view rebuild, and the
// mode-zero write in FUN_80085D18. It does not configure DX11 projection or
// own the game camera. On failure, leaves output unchanged.
[[nodiscard]] bool make_world_map_camera_initial_profile(
    WorldMapCameraInitialProfile* output);

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

struct WorldMapPlayerCameraPlacementQuery {
    WorldMapCameraViewQuery initial_camera{};
    std::array<float, 3> player_position{}; // player +0x4C
    int32_t collision_category = 0; // player +0x64
    int32_t scene_mode = 0; // state +0x60
    float heading_x = 0.0f; // player +0x58
    float heading_z = 0.0f; // player +0x60
    float fallback_yaw = 0.0f; // state +0x570
    uint8_t global_byte_3f1 = 0;
    int8_t pad_byte_8e = 0;
};

struct WorldMapPlayerCameraPlacement {
    WorldMapCameraFollowup first_followup{};
    WorldMapCameraPostUpdate first_update{};
    WorldMapCameraViewQuery final_camera{};
    WorldMapCameraPostUpdate final_update{};
    bool second_update_called = false;
    bool second_yaw_written = false;
};

// Bounded camera part of player constructor FUN_8002FDF8. The first
// FUN_80030E18 update always runs; a negative region result triggers a
// second position/yaw write and post-update. State and height source remain
// caller supplied. On failure, output is unchanged (sampler effects remain).
[[nodiscard]] bool calculate_world_map_player_camera_placement(
    const WorldMapPlayerCameraPlacementQuery& query,
    WorldMapCameraHeightSampler sample_height, void* sample_context,
    WorldMapPlayerCameraPlacement* output);

// FUN_8001CADC loads this fixed slot-1 asset before camera updates.
inline constexpr const char* kWorldMapCameraCollisionPath =
    "/files/jimen-camera.col";

// Applies the post-update using a caller-owned, validated type-1 camera COL
// (header byte 6 = 0). This is still a supplied-data path, not live camera
// or scene ownership. Failure leaves output unchanged.
[[nodiscard]] bool calculate_world_map_camera_post_update_from_collision(
    const WorldMapCameraViewQuery& query, const uint8_t* camera_col,
    size_t camera_col_size, WorldMapCameraPostUpdate* output);

// Native owner for FUN_8001CADC's fixed slot-1 camera COL. The caller first
// mounts its verified disc extraction and still supplies camera state. A
// failed reload clears the previous bytes; no live camera is updated here.
class WorldMapCameraCollisionAsset {
public:
    [[nodiscard]] bool load();
    void clear();
    [[nodiscard]] bool loaded() const { return !bytes_.empty(); }
    [[nodiscard]] size_t size() const { return bytes_.size(); }
    [[nodiscard]] bool calculate_post_update(
        const WorldMapCameraViewQuery& query,
        WorldMapCameraPostUpdate* output) const;
    [[nodiscard]] bool calculate_player_placement(
        const WorldMapPlayerCameraPlacementQuery& query,
        WorldMapPlayerCameraPlacement* output) const;

private:
    std::vector<uint8_t> bytes_;
};

} // namespace awl
