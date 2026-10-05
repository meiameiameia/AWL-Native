#pragma once
#include "awl/world_map_player_skin_work.h"
#include "awl/world_map_animation_channel.h"

namespace awl {
struct WorldMapPlayerNodeFrameInput {
    // Observed 01F8 result: absent means false/use retained default matrix;
    // present is its copied pose output. This does not decode animation tracks.
    std::optional<std::array<uint32_t,13>> sampled_pose;
    std::optional<WorldMapModelMatrix> post_transform; // Supplied node +10, null means absent.
};
struct WorldMapPlayerFrameInput {
    std::array<uint32_t,13> root_pose{}; // Supplied unparented model +1C.
    bool evaluate_nodes = true, request_skin = true; // E438 arguments r5/r4 low bytes.
    std::vector<WorldMapPlayerNodeFrameInput> nodes; // Core traversal order, not source-record order.
};
struct WorldMapPlayerFrame {
    WorldMapModelMatrix root_matrix{};
    std::vector<WorldMapModelMatrix> node_matrices,skin_palette;
    // Setup feature order, not draw order. Absent means no reached write;
    // notably node evaluation off leaves node features unchanged.
    std::vector<std::optional<WorldMapModelMatrix>> feature_matrix_writes;
    std::vector<uint32_t> feature_write_order; // Reached node writes first, root last; not draw order.
    std::vector<uint8_t> vertex_output;
    bool skin_executed = false;
};
enum class WorldMapPlayerFrameStatus {
    Evaluated, RequiresPoseConversion, UnsupportedLayout, UnsupportedNumerics,
    InvalidInput, AllocationFailure,
    RequiresAnimationSampling,
};
// E438 unparented primary matrix path, per-node pose/post/parent composition,
// inverse-bind palette and feature matrices, followed by checked C080 execution.
// Uses retained setup/default matrices and supplied pose observations, not a live
// playback sampler. Attached-model recursion and inherited root flags are open.
// Node evaluation off skips all node inputs; skin off skips inverse-bind reads.
// Previous output may be out->vertex_output. Every failure preserves the frame.
[[nodiscard]] WorldMapPlayerFrameStatus evaluate_world_map_player_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerFrameInput& input,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out);

struct WorldMapPlayerAnimationFrameInput {
    WorldMapAnimationPose root_pose{}; // Unparented model +1C; not sampled from a clip.
    bool evaluate_nodes = true, request_skin = true;
    WorldMapAnimationPlayback playback; // Supplied model +178 snapshot.
    WorldMapAnimationPoseSettings settings;
    // Core traversal order; one known optional node +10 observation per node.
    std::vector<std::optional<WorldMapModelMatrix>> node_post_transforms;
};
struct WorldMapPlayerAnimationFrameResult {
    WorldMapPlayerFrameStatus status = WorldMapPlayerFrameStatus::InvalidInput;
    // Populated only at RequiresAnimationSampling, before this node's conversion.
    std::optional<uint32_t> failed_node;
    std::optional<WorldMapAnimationPoseStatus> sampling_status;
};
// E438 -> 01F8/FF8C -> pose/post/parent matrices -> C080 in one atomic frame.
// Missing node poses use retained defaults. Node-off ignores playback, banks,
// records, settings and post observations. Inputs remain immutable; no time
// advancement, inherited/attached-model recursion, GPU or live ownership.
[[nodiscard]] WorldMapPlayerAnimationFrameResult evaluate_world_map_player_animation_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerAnimationFrameInput& input,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out);
} // namespace awl
