#pragma once
#include "awl/world_map_player_primary_owner.h"
#include "awl/world_map_player_start_animation.h"

namespace awl {
// D560's stores, with its unwritten words/bytes explicitly absent. These are
// native semantic fields, not the original packed holder/vtable layout.
struct WorldMapPlayerAnimationHolderState {
    uint64_t current_descriptor_0 = 0, base_descriptor_4 = 0;
    float speed_8 = 1;
    uint32_t default_duration_c = 1000;
    std::optional<uint32_t> deadline_10;
    uint32_t default_count_14 = 1;
    std::optional<uint32_t> count_18, completed_count_1c;
    std::optional<uint8_t> completed_20, flag_21;
    uint32_t restart_deadline_24 = 0, restart_limit_28 = 0, restart_count_2c = 0;
    uint64_t model_identity_30 = 0, group_identity_34 = 0;
    std::optional<WorldMapAnimationFeature38> feature_38;
    std::optional<WorldMapAnimationFeature3c> feature_3c;
    uint64_t secondary_model_c0 = 0, feature_c4 = 0, auxiliary_model_c8 = 0, arena_cc = 0;
};
enum class WorldMapPlayerAnimationHolderStatus { ConstructedCpuHolder, RequiresModelAssets,
    RequiresAnimationAssets, RequiresPrimary, RequiresGroup, InitializerIncomplete, InvalidInput, AllocationFailure };
struct WorldMapPlayerAnimationHolderChannelDependency {
    bool settings = false;
    uint64_t required_record = 0;
    // Record +8/+C mask 1/2; record zero with mask 1 means channel +14.
    uint32_t required_fields = 0;
};
struct WorldMapPlayerAnimationHolderResult {
    WorldMapPlayerAnimationHolderStatus status = WorldMapPlayerAnimationHolderStatus::InvalidInput;
    std::optional<WorldMapPlayerPrimaryResult> primary_failure;
    std::optional<WorldMapAnimationInitializerStatus> initializer_status;
    std::optional<uint32_t> secondary_index;
    std::optional<WorldMapAnimationFeatureRow> required_row;
    std::optional<WorldMapPlayerAnimationHolderChannelDependency> channel_dependency;
};
struct WorldMapPlayerAnimationHolderStartResult {
    WorldMapPlayerStartAnimationStatus status = WorldMapPlayerStartAnimationStatus::InvalidInput;
    std::optional<WorldMapPlayerStartAnimationSelection> selection;
    std::optional<WorldMapAnimationInitializerStatus> initializer_status;
    std::optional<uint32_t> secondary_index;
    std::optional<WorldMapAnimationFeatureRow> required_row;
    std::optional<WorldMapPlayerAnimationHolderChannelDependency> channel_dependency;
};

// D4B4's two owned channels, D560 state, actual CPU primary and retained
// group-zero providers. Fresh D610 binding executes initialization before
// publication. Existing-holder D610 rebind/reset, secondary construction,
// descriptor sequences, holder clocks, actor acknowledgement and GPU draws
// remain unsupported. Optional feature state is a supplied complete snapshot.
class WorldMapPlayerAnimationHolder {
public:
    WorldMapPlayerAnimationHolder(const WorldMapPlayerAnimationHolder&) = delete;
    WorldMapPlayerAnimationHolder& operator=(const WorldMapPlayerAnimationHolder&) = delete;
    const WorldMapPlayerAnimationHolderState& state() const { return state_; }
    const WorldMapNativeAnimationChannel& primary_channel() const { return *primary_channel_; }
    const WorldMapNativeAnimationChannel& secondary_channel() const { return *secondary_channel_; }
    const WorldMapPlayerPrimaryOwner& primary() const { return *primary_; }
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animations() const { return animations_; }
    const WorldMapActorAnimationDescriptor& descriptor() const { return descriptor_; }
    // Subsequent selector-1 starts use D660's conditional D69C -> DB28 path,
    // not D610's unconditional DB28 rebind. Every stop preserves all owners.
    [[nodiscard]] WorldMapPlayerAnimationHolderStartResult start(
        const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
        const WorldMapPlayerStartAnimationCommand& command,
        const std::optional<WorldMapPlayerStartItemType>& item_type,
        const WorldMapAnimationInitializerObservations& observations);
    [[nodiscard]] WorldMapPlayerPrimaryUpdateResult advance(const WorldMapPlayerPrimaryFrameInput& input) {
        return primary_->advance(input, primary_channel_.get());
    }
private:
    WorldMapPlayerAnimationHolder() = default;
    friend WorldMapPlayerAnimationHolderResult construct_world_map_player_animation_holder(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&, const WorldMapActorAnimationDescriptor&,
        const std::optional<WorldMapAnimationFeature38>&, const WorldMapAnimationInitializerObservations&,
        std::unique_ptr<WorldMapPlayerAnimationHolder>*);
    // Declared before the model: it dies before the channels it borrows.
    std::unique_ptr<WorldMapNativeAnimationChannel> primary_channel_, secondary_channel_;
    std::shared_ptr<const WorldMapPlayerAnimationAssets> animations_;
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> tables_;
    std::unique_ptr<WorldMapPlayerPrimaryOwner> primary_;
    std::vector<WorldMapNativeModel*> models_; // Borrowed registry, fixed after fresh construction.
    WorldMapPlayerAnimationHolderState state_;
    WorldMapActorAnimationDescriptor descriptor_;
};
// Fresh construction only. Retains all providers and the first descriptor
// snapshot. Failures release staging and preserve *out; release external
// borrowers before successful replacement. No parent state-29 acceptance.
[[nodiscard]] WorldMapPlayerAnimationHolderResult construct_world_map_player_animation_holder(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const WorldMapActorAnimationDescriptor& initial_descriptor,
    const std::optional<WorldMapAnimationFeature38>& feature_38,
    const WorldMapAnimationInitializerObservations& observations,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out);
} // namespace awl
