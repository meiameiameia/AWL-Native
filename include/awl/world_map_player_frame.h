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
    RequiresAnimationSampling, RequiresHierarchy,
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

// E438 with known null +C (no skin). Resource/core are supplied prepared node
// layout snapshots; links/playback come exclusively from input, not core's
// constructor/link fields. Feature indices in the returned frame are CORE NODE
// indices, whose +8 keys identify the borrowed feature. No root +14 write,
// inverse-bind read, skin execution or vertex-buffer requirement occurs.
[[nodiscard]] WorldMapPlayerAnimationFrameResult evaluate_world_map_model_animation_frame(
    const WorldMapPreparedModelResource& resource,const WorldMapModelCore& core,
    const WorldMapPlayerAnimationFrameInput& input,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out);

struct WorldMapPlayerOwnedFrameInput {
    // Absent reuses the owner's last successfully published root pose.
    // Present is a supplied placement observation, not translated actor input.
    std::optional<WorldMapAnimationPose> root_pose;
    bool evaluate_nodes = true, request_skin = true;
    WorldMapAnimationPartialPlayback playback;
    WorldMapAnimationPoseSettings settings;
    std::vector<std::optional<WorldMapModelMatrix>> node_post_transforms;
    WorldMapPlayerFrameLinks links;
};
enum class WorldMapPlayerFrameOwnerStatus { PreparedCpuState, RequiresSkinWork, InvalidInput, AllocationFailure };
struct WorldMapPlayerFrameOwnerResult {
    WorldMapPlayerFrameOwnerStatus status = WorldMapPlayerFrameOwnerStatus::InvalidInput;
    std::optional<WorldMapPlayerSkinWorkStatus> skin_work_status;
};
// Persistent primary CPU frame state. Owns checked skin work/providers,
// last vertex bytes, supplied root pose and initialized feature matrices.
// No compiled GX commands, original CD50 wrapper, holder, playback/channel
// owner, live actor or GPU object is constructed by PreparedCpuState.
class WorldMapPlayerFrameOwner {
public:
    WorldMapPlayerFrameOwner(const WorldMapPlayerFrameOwner&) = delete;
    WorldMapPlayerFrameOwner& operator=(const WorldMapPlayerFrameOwner&) = delete;
    const WorldMapPlayerSkinWork& work() const { return *work_; }
    const WorldMapAnimationPose& root_pose() const { return root_pose_; }
    const std::vector<WorldMapModelMatrix>& feature_matrices() const { return features_; }
    // No frame is fabricated at preparation; node/palette matrices in the
    // last frame are diagnostic temporary results, not persistent DOL fields.
    const std::optional<WorldMapPlayerFrame>& frame() const { return frame_; }
    const std::vector<uint8_t>& vertex_output() const { return frame_?frame_->vertex_output:work_->initial_output(); }
    // E438's sampled primary CPU work followed by persistent feature/skin
    // publication. Unwritten features and skipped skin bytes stay unchanged.
    // Every failure preserves the complete owner; unknown partial fields
    // remain unknown. Records/banks/settings/links are borrowed observations.
    // Nonnull child slots require a complete hierarchy transaction and return
    // RequiresHierarchy before any publication. No child effect is accepted.
    [[nodiscard]] WorldMapPlayerAnimationFrameResult advance(
        const WorldMapPlayerOwnedFrameInput& input,
        const std::vector<WorldMapAnimationPartialPlaybackRecord>& records,
        const std::vector<const WorldMapAnimationBank*>& banks);
private:
    WorldMapPlayerFrameOwner(std::unique_ptr<WorldMapPlayerSkinWork> work,std::vector<WorldMapModelMatrix> features);
    friend WorldMapPlayerFrameOwnerResult prepare_world_map_player_frame_owner(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,std::unique_ptr<WorldMapPlayerFrameOwner>*);
    std::unique_ptr<WorldMapPlayerSkinWork> work_;
    WorldMapAnimationPose root_pose_{}; // D330 initializes flag byte only; other zero scratch words are unread until supplied.
    std::vector<WorldMapModelMatrix> features_;
    std::optional<WorldMapPlayerFrame> frame_;
};
// Keeps immutable selected providers live. Failure preserves the previous
// owner/output. Successful replacement destroys only that prior CPU owner.
[[nodiscard]] WorldMapPlayerFrameOwnerResult prepare_world_map_player_frame_owner(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,std::unique_ptr<WorldMapPlayerFrameOwner>* out);

struct WorldMapModelFrameSource {
    // Exactly one primary work OR prepared no-skin resource/core views OR
    // native secondary owner. Owner identity/links/playback/layout are authoritative;
    // its input identity/parent/flags/children/playback observations are ignored.
    // Borrowed for this call; identities never dereference host/PPC pointers.
    const WorldMapPlayerSkinWork* primary = nullptr;
    const WorldMapPreparedModelResource* resource = nullptr;
    const WorldMapModelCore* core = nullptr;
    const WorldMapNativeModel* secondary = nullptr;
    WorldMapPlayerAnimationFrameInput input; // links.identity is this record's key.
    WorldMapPlayerFrame previous_frame;
};
struct WorldMapModelFrameState {
    uint64_t identity = 0;
    WorldMapAnimationPose root_pose{};
    WorldMapPlayerInheritedRoot inherited;
    WorldMapPlayerFrame frame; // Retained previous frame if never evaluated.
};
struct WorldMapModelHierarchyFrame {
    std::vector<WorldMapModelFrameState> models; // Supplied source order.
    std::vector<uint64_t> evaluation_order; // Ordered depth-first visits, including aliases.
};
enum class WorldMapModelHierarchyFrameStatus {
    Evaluated, InvalidInput, RequiresModel, FrameFailure, Cycle, EvaluationLimit, AllocationFailure,
};
struct WorldMapModelHierarchyFrameResult {
    WorldMapModelHierarchyFrameStatus status = WorldMapModelHierarchyFrameStatus::InvalidInput;
    uint64_t required_model = 0; // Missing/failing/blocked stable key when known.
    std::optional<WorldMapPlayerAnimationFrameResult> frame_failure;
};
// Complete E438 child evaluation on supplied snapshots. Apply every parent's
// slot propagation before its first child visit; bit 0x01 gates recursion only.
// Root flag mutations and repeated skin output are retained between alias visits.
// Unreached source payloads remain opaque. Duplicate/zero keys reject; missing
// reached children, active-path cycles or the caller's visit bound fail atomically.
// The two root controls propagate unchanged to every child. No clocks, owner
// mutation/registry, GPU drawing or actor acceptance. Every failure preserves
// *out; sources, records and banks remain immutable.
// Owned sources use retained prepared views and banks, merge authoritative
// partial playback keys with external complete records, and preserve unknown
// fields. External records cannot duplicate an owned playback key. Identical
// bank snapshots deduplicate; conflicting bytes under a reached key reject.
// Root pose, inherited producer/matrix, settings, node post observations and
// previous frame remain supplied. No root/feature/actor state is published to owners.
[[nodiscard]] WorldMapModelHierarchyFrameResult evaluate_world_map_model_hierarchy_frame(
    const std::vector<WorldMapModelFrameSource>& sources,uint64_t root,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    size_t maximum_evaluations,WorldMapModelHierarchyFrame* out);
} // namespace awl
