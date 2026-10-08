#pragma once
#include "awl/world_map_player_primary_owner.h"
#include "awl/world_map_player_start_animation.h"
#include "awl/world_map_player_initial_animation.h"
#include "awl/world_map_player_timer_assets.h"
#include "awl/game_clock.h"

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
    // Known only through the supported actual F39C initial-input path.
    std::optional<uint32_t> feature_source_10;
    std::optional<uint8_t> feature_flag_34;
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
enum class WorldMapPlayerInitialHolderStatus {
    ConstructedCpuHolder, InitialInputsIncomplete, RequiresModelAssets,
    PhaseMismatch, HolderIncomplete, InvalidInput, AllocationFailure
};
struct WorldMapPlayerInitialHolderResult {
    WorldMapPlayerInitialHolderStatus status = WorldMapPlayerInitialHolderStatus::InvalidInput;
    WorldMapPlayerInitialAnimationStep initial;
    std::optional<WorldMapPlayerAnimationHolderResult> holder;
};
enum class WorldMapPlayerTimedInitialHolderStatus {
    ConstructedCpuHolder, InitialInputsIncomplete, TimerInputsIncomplete,
    HolderIncomplete, InvalidInput, AllocationFailure
};
struct WorldMapPlayerTimedInitialHolderResult {
    WorldMapPlayerTimedInitialHolderStatus status = WorldMapPlayerTimedInitialHolderStatus::InvalidInput;
    WorldMapPlayerTimerBindingResult timer;
    WorldMapPlayerInitialHolderResult holder;
};

// B8F8's supported slot-zero, alternate -1 wrapper writes. A source address
// is a symbolic reference ONLY: no host pointer or runtime table contents.
// Later consumers of +120 require a translated runtime table owner.
struct WorldMapPlayerInitialModelState {
    uint32_t model_slot_4 = 0, alternate_8 = UINT32_MAX;
    uint32_t runtime_entry_address_120 = 0x802ED6C0;
    // Raw private TAM node 7; bytes borrow the primary's retained assets.
    // Its presence/binding does not establish decoding or TAM execution.
    std::optional<WorldMapPlayerModelAssetView> private_texture_animation;
    uint8_t initialized_c = 0;
};

// D4B4's two owned channels, D560 state, actual CPU primary and retained
// group-zero providers. Fresh D610 binding executes initialization before
// publication. Existing-holder D610 rebind/reset, secondary construction,
// descriptor sequences, holder clocks, actor acknowledgement and GPU draws
// remain unsupported. General construction accepts a supplied complete
// feature snapshot; the initial-input wrapper prepares it from static inputs
// and reached runtime table/clock evidence.
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
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& initial_inputs() const { return initial_inputs_; }
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timer_assets() const { return timer_assets_; }
    const std::shared_ptr<GameClock>& game_clock() const { return game_clock_; }
    const std::optional<WorldMapPlayerInitialModelState>& initial_model_state() const { return initial_model_state_; }
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
    bool finish_initial_model(float scale, const std::shared_ptr<const WorldMapPlayerModelAssets>& assets);
    friend WorldMapPlayerAnimationHolderResult construct_world_map_player_animation_holder(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&, const WorldMapActorAnimationDescriptor&,
        const std::optional<WorldMapAnimationFeature38>&, const WorldMapAnimationInitializerObservations&,
        std::unique_ptr<WorldMapPlayerAnimationHolder>*);
    friend WorldMapPlayerInitialHolderResult construct_world_map_player_initial_animation_holder(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&,
        const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>&, const WorldMapPlayerInitialAnimationQuery&,
        const WorldMapAnimationInitializerObservations&, std::unique_ptr<WorldMapPlayerAnimationHolder>*);
    friend WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_timers(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&,
        const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>&,
        const std::shared_ptr<const WorldMapPlayerTimerAssets>&, const WorldMapPlayerInitialAnimationQuery&,
        const std::optional<uint32_t>&, std::unique_ptr<WorldMapPlayerAnimationHolder>*);
    friend WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_clock(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&,
        const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>&,
        const std::shared_ptr<const WorldMapPlayerTimerAssets>&, const WorldMapPlayerInitialAnimationQuery&,
        const std::shared_ptr<GameClock>&, std::unique_ptr<WorldMapPlayerAnimationHolder>*);
    friend WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_model_with_clock(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&,
        const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>&,
        const std::shared_ptr<const WorldMapPlayerTimerAssets>&, const WorldMapPlayerInitialAnimationQuery&,
        const std::shared_ptr<GameClock>&, std::unique_ptr<WorldMapPlayerAnimationHolder>*);
    // Declared before the model: it dies before the channels it borrows.
    std::unique_ptr<WorldMapNativeAnimationChannel> primary_channel_, secondary_channel_;
    std::shared_ptr<const WorldMapPlayerAnimationAssets> animations_;
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> tables_;
    std::shared_ptr<const WorldMapPlayerInitialAnimationInputs> initial_inputs_;
    std::shared_ptr<const WorldMapPlayerTimerAssets> timer_assets_;
    std::shared_ptr<GameClock> game_clock_;
    std::unique_ptr<WorldMapPlayerPrimaryOwner> primary_;
    std::vector<WorldMapNativeModel*> models_; // Borrowed registry, fixed after fresh construction.
    WorldMapPlayerAnimationHolderState state_;
    WorldMapActorAnimationDescriptor descriptor_;
    std::optional<WorldMapPlayerInitialModelState> initial_model_state_;
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
// Slot-zero, alternate -1 B8F8 inputs: exact static descriptor/types and
// supplied runtime timer table/clock evidence, then fresh CPU construction.
// Phase must match the retained primary asset variant. No actor final flag,
// scale/message/state callback or live startup acknowledgement is performed.
// All stops release staging and preserve *out, including late DB28 stops.
[[nodiscard]] WorldMapPlayerInitialHolderResult construct_world_map_player_initial_animation_holder(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const WorldMapPlayerInitialAnimationQuery& query,
    const WorldMapAnimationInitializerObservations& observations,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out);
// Own actual eye/mouth timer resources through slot-zero fresh CPU startup.
// The raw clock is an explicit snapshot, not a frame count or scheduler time.
// Retains all timer bytes before publication; every stop preserves *out.
[[nodiscard]] WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_timers(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::optional<uint32_t>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out);
// Same fresh startup from a shared GameClock's raw snapshot, with clock
// lifetime retained before publication. First-row/clock/second-row stops and
// late holder failures preserve *out and never advance the shared clock.
// Timer clock fields remain initial snapshots; TAM sampling is unsupported.
[[nodiscard]] WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_clock(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::shared_ptr<GameClock>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out);
// Adds B8F8's private-TAM table_C copy, flag/scale writes and final initialized
// byte to fresh CPU startup. Symbolic +120 reference is retained, not resolved.
// Never advances time, evaluates a frame or acknowledges parent state 29.
// Every failure preserves *out and shared clock, including late dependencies.
[[nodiscard]] WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_model_with_clock(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::shared_ptr<GameClock>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out);
} // namespace awl
