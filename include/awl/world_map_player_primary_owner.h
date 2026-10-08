#pragma once
#include "awl/world_map_player_frame.h"
#include "awl/world_map_player_geometry.h"
#include "awl/world_map_animation_channel_owner.h"

namespace awl {
enum class WorldMapPlayerPrimaryStatus { ConstructedCpuPrimary, RequiresAssets, RequiresFrameState,
    RequiresGeometry, RequiresResourcePreparation, UnsupportedLayout, InvalidInput, AllocationFailure };
struct WorldMapPlayerPrimaryResult {
    WorldMapPlayerPrimaryStatus status = WorldMapPlayerPrimaryStatus::InvalidInput;
    std::optional<WorldMapPlayerFrameOwnerResult> frame_failure;
    std::optional<WorldMapPlayerGeometryResult> geometry_failure;
};
struct WorldMapPlayerPrimaryFrameInput {
    std::optional<WorldMapAnimationPose> root_pose; // Supplied actor placement; absent retains accepted pose.
    bool evaluate_nodes = true, request_skin = true;
    WorldMapAnimationPoseSettings settings;
    std::vector<std::optional<WorldMapModelMatrix>> node_post_transforms;
};
enum class WorldMapPlayerPrimaryUpdateStatus { Advanced, RequiresHierarchy, RequiresFeatureBindings, FrameFailure,
    GeometryFailure, AllocationFailure };
struct WorldMapPlayerPrimaryUpdateResult {
    WorldMapPlayerPrimaryUpdateStatus status = WorldMapPlayerPrimaryUpdateStatus::FrameFailure;
    std::optional<WorldMapPlayerAnimationFrameResult> frame_failure;
    std::optional<WorldMapPlayerGeometryResult> geometry_failure;
};
// Owns actual phase-selected ACT/GPL/SKN/texture providers, CPU skin/frame/mesh
// storage and a native model with D7B0 feature bindings, E7C4 list order and
// fresh partial playback. Checked CPU draw parameters replace GX packets;
// GPU execution/draw scheduling and live actor/state29 startup remain open.
// Release external borrowers before replacing this owner; models die before
// channels whose record keys they borrow. No original heap/packed pointer ABI.
class WorldMapPlayerPrimaryOwner {
public:
    WorldMapPlayerPrimaryOwner(const WorldMapPlayerPrimaryOwner&) = delete;
    WorldMapPlayerPrimaryOwner& operator=(const WorldMapPlayerPrimaryOwner&) = delete;
    WorldMapNativeModel& model() { return *model_; }
    const WorldMapNativeModel& model() const { return *model_; }
    const WorldMapPlayerFrameOwner& cpu() const { return *frame_; }
    const WorldMapPlayerGeometry& geometry() const { return *geometry_; }
    const WorldMapPlayerModelSetupPlan& setup() const { return frame_->work().drawing().setup(); }
    // Stage authoritative playback sampling, skin/features AND mesh before
    // publication. Attached/inherited models require a full hierarchy and stop.
    // No channel/animation clock, actor movement, rendering or acknowledgement.
    [[nodiscard]] WorldMapPlayerPrimaryUpdateResult advance(
        const WorldMapPlayerPrimaryFrameInput& input, const WorldMapNativeAnimationChannel* channel);
private:
    WorldMapPlayerPrimaryOwner() = default;
    friend WorldMapPlayerPrimaryResult construct_world_map_player_primary(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&, std::unique_ptr<WorldMapPlayerPrimaryOwner>*);
    // Model keys refer to providers retained by frame_; destroy model first.
    std::unique_ptr<WorldMapPlayerFrameOwner> frame_;
    std::unique_ptr<WorldMapPlayerGeometry> geometry_;
    std::unique_ptr<WorldMapNativeModel> model_;
};
// Rebuild supported providers from immutable assets. All failures preserve
// an existing output owner; no null-auxiliary secondary substitute is used.
[[nodiscard]] WorldMapPlayerPrimaryResult construct_world_map_player_primary(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets, std::unique_ptr<WorldMapPlayerPrimaryOwner>* out);
} // namespace awl
