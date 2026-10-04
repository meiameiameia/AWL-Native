#pragma once
#include "awl/world_map_player_skin_work.h"

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
} // namespace awl
