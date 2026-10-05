#pragma once
#include "awl/world_map_player_skin_work.h"
#include "awl/world_map_animation_channel.h"

namespace awl {
struct WorldMapPlayerInheritedRoot {
    // Supplied model +150/+154/+158/+120 snapshot, independent of retained setup.
    // A null parent skips all other fields. Identities are native stable keys.
    uint64_t parent = 0, producer = 0;
    std::optional<uint32_t> flags;
    WorldMapModelMatrix matrix{};
};
struct WorldMapPlayerAttachmentInput {
    uint64_t child = 0; // Null slots skip index and flags.
    uint16_t node = 0xffff;
    std::optional<uint32_t> child_flags;
};
struct WorldMapPlayerAttachmentWrite {
    uint64_t child = 0, producer = 0; // child +154 = this model identity.
    WorldMapModelMatrix matrix{}; // child +120.
    bool evaluate_child = false; // child +158 mask 0x01; recursion remains caller work.
};
struct WorldMapPlayerFrameLinks {
    WorldMapPlayerInheritedRoot inherited;
    uint64_t identity = 0; // Required only when a child slot is nonnull.
    std::array<WorldMapPlayerAttachmentInput,4> children{};
};
struct WorldMapPlayerNodeFrameInput {
    // Observed 01F8 result: absent means false/use retained default matrix;
    // present is its copied pose output. This does not decode animation tracks.
    std::optional<std::array<uint32_t,13>> sampled_pose;
    std::optional<WorldMapModelMatrix> post_transform; // Supplied node +10, null means absent.
};
struct WorldMapPlayerFrameInput {
    std::array<uint32_t,13> root_pose{}; // Supplied model +1C.
    bool evaluate_nodes = true, request_skin = true; // E438 arguments r5/r4 low bytes.
    std::vector<WorldMapPlayerNodeFrameInput> nodes; // Core traversal order, not source-record order.
    WorldMapPlayerFrameLinks links;
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
    WorldMapAnimationPose root_pose_after{}; // E4EC masks only the +1C flag byte.
    // E6D4..E748 slot order, including aliases. Apply ALL writes before child
    // evaluation; a shared child sees the last slot's matrix on every visit.
    std::array<std::optional<WorldMapPlayerAttachmentWrite>,4> attachment_writes{};
};
enum class WorldMapPlayerFrameStatus {
    Evaluated, RequiresPoseConversion, UnsupportedLayout, UnsupportedNumerics,
    InvalidInput, AllocationFailure,
    RequiresAnimationSampling,
};
// E438 primary root paths, per-node pose/post/parent composition,
// inverse-bind palette and feature matrices, followed by checked C080 execution.
// Uses retained setup/default matrices and supplied pose observations, not a live
// playback sampler. Prepares inherited root and child matrices/acknowledgements,
// but does not evaluate child frames or publish model state. Invalid inherited
// acknowledgements reject rather than invoking the original assertion reporter.
// Evaluated covers this primary frame and its ordered propagation proposal,
// not completion of the requested child frames or the whole model hierarchy.
// Node-off skips nodes except attachment ancestry (EB80); skin-off skips binds.
// Previous output may be out->vertex_output. Every failure preserves the frame.
[[nodiscard]] WorldMapPlayerFrameStatus evaluate_world_map_player_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerFrameInput& input,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out);

struct WorldMapPlayerAnimationFrameInput {
    WorldMapAnimationPose root_pose{}; // Model +1C; not sampled from a clip.
    bool evaluate_nodes = true, request_skin = true;
    WorldMapAnimationPlayback playback; // Supplied model +178 snapshot.
    WorldMapAnimationPoseSettings settings;
    // Core traversal order; one known optional node +10 observation per node.
    std::vector<std::optional<WorldMapModelMatrix>> node_post_transforms;
    WorldMapPlayerFrameLinks links;
};
struct WorldMapPlayerAnimationFrameResult {
    WorldMapPlayerFrameStatus status = WorldMapPlayerFrameStatus::InvalidInput;
    // Populated only at RequiresAnimationSampling, before this node's conversion.
    std::optional<uint32_t> failed_node;
    std::optional<WorldMapAnimationPoseStatus> sampling_status;
};
// E438 -> 01F8/FF8C -> pose/post/parent matrices -> C080 in one atomic frame.
// Missing poses use defaults. Node-off with children reaches EB80 sampling;
// without children it ignores playback/banks/records/settings/post observations.
// Inputs remain immutable; no time advancement, child recursion, GPU or live ownership.
[[nodiscard]] WorldMapPlayerAnimationFrameResult evaluate_world_map_player_animation_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerAnimationFrameInput& input,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out);
} // namespace awl
