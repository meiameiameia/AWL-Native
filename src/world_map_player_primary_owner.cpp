#include "awl/world_map_player_primary_owner.h"
#include "awl/world_map_player_initial_animation.h"
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
bool WorldMapPlayerPrimaryOwner::initialize_root_scale(float scale) {
    if (frame_->frame_) return false; // Fresh construction only.
    WorldMapAnimationPose next;
    if (prepare_world_map_player_initial_root_pose(frame_->root_pose_, scale, &next) !=
        WorldMapPlayerInitialAnimationStatus::Prepared) return false;
    frame_->root_pose_ = next;
    model_->core_.byte_1c = static_cast<uint8_t>(next[0] >> 24);
    return true;
}
WorldMapPlayerPrimaryResult construct_world_map_player_primary(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets, std::unique_ptr<WorldMapPlayerPrimaryOwner>* out) {
    using Status = WorldMapPlayerPrimaryStatus;
    if (!out) return {};
    if (!assets) return {Status::RequiresAssets};
    try {
        auto owner = std::unique_ptr<WorldMapPlayerPrimaryOwner>(new WorldMapPlayerPrimaryOwner);
        const auto frame = prepare_world_map_player_frame_owner(assets, &owner->frame_);
        if (frame.status == WorldMapPlayerFrameOwnerStatus::AllocationFailure) return {Status::AllocationFailure, frame};
        if (frame.status != WorldMapPlayerFrameOwnerStatus::PreparedCpuState) return {Status::RequiresFrameState, frame};
        const auto geometry = prepare_world_map_player_geometry(owner->frame_->work(), &owner->geometry_);
        if (geometry.status == WorldMapPlayerGeometryStatus::AllocationFailure) return {Status::AllocationFailure, {}, geometry};
        if (geometry.status != WorldMapPlayerGeometryStatus::PreparedGeometry) return {Status::RequiresGeometry, {}, geometry};
        const auto& plan = owner->setup();
        auto bank = std::make_unique<const WorldMapModelBank>(assets->models());
        WorldMapModelPreparationStep prepared;
        const auto preparation = bank->prepare(1, &prepared);
        if (preparation != WorldMapModelPreparationStatus::Prepared || !prepared.prepared) return {Status::RequiresResourcePreparation};
        auto core = plan.core_before_features();
        core.auxiliary_c = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&plan.auxiliary()));
        for (const auto& feature : plan.features()) {
            const auto key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&feature));
            if (feature.node) {
                if (*feature.node >= core.nodes.size() || core.nodes[*feature.node].feature_8) return {Status::UnsupportedLayout};
                core.nodes[*feature.node].feature_8 = key;
            } else {
                if (core.feature_14) return {Status::UnsupportedLayout};
                core.feature_14 = key;
            }
        }
        const auto& order = plan.feature_node_order();
        core.head_50 = order.empty() ? 0u : order.front() + 1u;
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] >= core.nodes.size() || !core.nodes[order[i]].feature_8) return {Status::UnsupportedLayout};
            core.nodes[order[i]].next_feature_14 = i + 1 < order.size() ? order[i + 1] + 1u : 0u;
        }
        const auto& storage = plan.storage_requests();
        if (storage.empty()) return {Status::UnsupportedLayout};
        const uint64_t consumed = uint64_t(storage.back().offset) + storage.back().size;
        if (consumed > plan.model_allocation_size()) return {Status::UnsupportedLayout};
        owner->model_ = std::unique_ptr<WorldMapNativeModel>(new WorldMapNativeModel(std::move(bank),
            std::move(*prepared.prepared), std::move(core), storage, static_cast<uint32_t>(consumed)));
        *out = std::move(owner); return {Status::ConstructedCpuPrimary};
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapPlayerPrimaryUpdateResult WorldMapPlayerPrimaryOwner::advance(
    const WorldMapPlayerPrimaryFrameInput& input, const WorldMapNativeAnimationChannel* channel) {
    using Status = WorldMapPlayerPrimaryUpdateStatus;
    static_assert(std::is_nothrow_move_constructible_v<WorldMapPlayerFrame>);
    if (model_->core().parent_150) return {Status::RequiresHierarchy};
    for (uint64_t child : model_->core().children_15c) if (child) return {Status::RequiresHierarchy};
    // An attachment/source operation may replace a feature key. This CPU
    // owner only executes its retained providers, never an external feature.
    const auto expected_feature = [&](std::optional<uint32_t> node) -> uint64_t {
        for (const auto& feature : setup().features()) if (feature.node == node)
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&feature));
        return 0;
    };
    if (model_->core().feature_14 != expected_feature(std::nullopt)) return {Status::RequiresFeatureBindings};
    for (uint32_t i = 0; i < model_->core().nodes.size(); ++i)
        if (model_->core().nodes[i].feature_8 != expected_feature(i)) return {Status::RequiresFeatureBindings};
    try {
        WorldMapPlayerOwnedFrameInput supplied;
        supplied.root_pose = input.root_pose; supplied.evaluate_nodes = input.evaluate_nodes; supplied.request_skin = input.request_skin;
        supplied.settings = input.settings; supplied.node_post_transforms = input.node_post_transforms;
        supplied.playback = model_->partial_playback(); supplied.links.identity = model_->binding().model_identity;
        std::vector<WorldMapAnimationPartialPlaybackRecord> records;
        if (channel) records = channel->records();
        const auto model_key = model_->binding().playback_178;
        for (const auto& record : records) if (record.identity == model_key) return {Status::FrameFailure};
        records.push_back({model_key, model_->partial_playback()});
        std::vector<const WorldMapAnimationBank*> banks;
        for (const auto& bank : model_->animation_banks()) banks.push_back(bank.get());
        WorldMapPlayerFrame staged; std::vector<WorldMapModelMatrix> features;
        const auto evaluated = frame_->stage(supplied, records, banks, &staged, &features);
        if (evaluated.status == WorldMapPlayerFrameStatus::AllocationFailure) return {Status::AllocationFailure, evaluated};
        if (evaluated.status != WorldMapPlayerFrameStatus::Evaluated) return {Status::FrameFailure, evaluated};
        std::vector<std::vector<WorldMapPlayerVertex>> vertices;
        const auto decoded = geometry_->stage_decode(frame_->work(), staged.vertex_output, &vertices);
        if (decoded.status == WorldMapPlayerGeometryStatus::AllocationFailure) return {Status::AllocationFailure, {}, decoded};
        if (decoded.status != WorldMapPlayerGeometryStatus::DecodedVertices) return {Status::GeometryFailure, {}, decoded};
        // No allocating work follows; persistent pose/features/skin/mesh publish together.
        frame_->root_pose_ = staged.root_pose_after; frame_->features_.swap(features);
        model_->core_.byte_1c = static_cast<uint8_t>(staged.root_pose_after[0] >> 24);
        frame_->frame_.emplace(std::move(staged)); geometry_->vertices_.swap(vertices);
        return {Status::Advanced};
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
} // namespace awl
