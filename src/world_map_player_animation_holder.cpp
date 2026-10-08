#include "awl/world_map_player_animation_holder.h"
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
WorldMapNativeAnimationInitializerMetadata supplied_metadata(const WorldMapPlayerAnimationHolderState& s) {
    WorldMapNativeAnimationInitializerMetadata result;
    auto& a = result.animation;
    a.current_descriptor_0 = s.current_descriptor_0; a.base_descriptor_4 = s.base_descriptor_4;
    a.speed_8 = s.speed_8; a.default_duration_c = s.default_duration_c; a.default_count_14 = s.default_count_14;
    // DB28's supported modes never read these prior words: mode 2 overwrites
    // +10; mode 3 overwrites +18/+1C; D69C overwrites +20/+21 when changed.
    // Private placeholders adapt the existing supplied-state helper. They are
    // never exposed as observations or used to establish preserved fields.
    a.deadline_10 = s.deadline_10.value_or(0); a.count_18 = s.count_18.value_or(0);
    a.completed_count_1c = s.completed_count_1c.value_or(0);
    a.completed_20 = s.completed_20.value_or(0); a.flag_21 = s.flag_21.value_or(0);
    a.restart_deadline_24 = s.restart_deadline_24; a.restart_limit_28 = s.restart_limit_28; a.restart_count_2c = s.restart_count_2c;
    a.model_identity_30 = s.model_identity_30;
    result.has_optional_bindings = true; result.feature_38 = s.feature_38; result.feature_3c = s.feature_3c;
    result.secondary_model_c0 = s.secondary_model_c0; return result;
}
void publish_known_fields(WorldMapPlayerAnimationHolderState& state,
    const WorldMapNativeAnimationInitializerMetadata& metadata, const WorldMapActorAnimationDescriptor& descriptor) noexcept {
    const auto& a = metadata.animation;
    state.current_descriptor_0 = a.current_descriptor_0; state.base_descriptor_4 = a.base_descriptor_4;
    state.completed_20 = a.completed_20; state.flag_21 = a.flag_21; state.restart_count_2c = a.restart_count_2c;
    const auto mode = (descriptor.word_0 >> 10) & 3u;
    if (mode == 2) state.deadline_10 = a.deadline_10;
    if (mode == 3) { state.count_18 = a.count_18; state.completed_count_1c = a.completed_count_1c; }
    state.feature_38 = metadata.feature_38; state.feature_3c = metadata.feature_3c;
}
template<class Result> void diagnostics(Result& out, const WorldMapNativeAnimationInitializerStep& initialized) noexcept {
    out.initializer_status = initialized.initializer_status;
    if (initialized.initializer) {
        out.secondary_index = initialized.initializer->secondary_index;
        out.required_row = initialized.initializer->required_row;
        const auto& step = *initialized.initializer;
        for (const auto* channel : {&step.setup, &step.settings}) if (*channel &&
            ((*channel)->required_record || (*channel)->required_fields)) {
            out.channel_dependency = WorldMapPlayerAnimationHolderChannelDependency{
                channel == &step.settings, (*channel)->required_record, (*channel)->required_fields};
        }
    }
}
} // namespace
WorldMapPlayerAnimationHolderResult construct_world_map_player_animation_holder(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const WorldMapActorAnimationDescriptor& initial_descriptor,
    const std::optional<WorldMapAnimationFeature38>& feature_38,
    const WorldMapAnimationInitializerObservations& observations,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerAnimationHolderStatus;
    using Native = WorldMapNativeAnimationInitializerStatus;
    if (!out || !initial_descriptor.identity) return {};
    if (!model_assets) return {Status::RequiresModelAssets};
    if (!animation_assets) return {Status::RequiresAnimationAssets};
    try {
        WorldMapPlayerAnimationGroupBinding group;
        const auto bound = bind_world_map_player_animation_group(initial_descriptor.word_0, animation_assets, &group);
        if (bound == WorldMapPlayerAnimationAssetsStatus::RequiresGroup) return {Status::RequiresGroup};
        if (bound != WorldMapPlayerAnimationAssetsStatus::Bound) return {};
        auto holder = std::unique_ptr<WorldMapPlayerAnimationHolder>(new WorldMapPlayerAnimationHolder);
        for (auto* channel : {&holder->primary_channel_, &holder->secondary_channel_})
            if (construct_world_map_animation_channel(channel) != WorldMapAnimationChannelConstructionStatus::Constructed) return {Status::AllocationFailure};
        const auto primary = construct_world_map_player_primary(model_assets, &holder->primary_);
        if (primary.status == WorldMapPlayerPrimaryStatus::AllocationFailure) return {Status::AllocationFailure, primary};
        if (primary.status != WorldMapPlayerPrimaryStatus::ConstructedCpuPrimary) return {Status::RequiresPrimary, primary};
        holder->animations_ = animation_assets;
        holder->state_.model_identity_30 = holder->primary_->model().binding().model_identity;
        // Stable native owner key for the retained group table equivalent.
        holder->state_.group_identity_34 = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(animation_assets.get()));
        holder->state_.feature_38 = feature_38;
        holder->models_.push_back(&holder->primary_->model());
        auto metadata = supplied_metadata(holder->state_);
        WorldMapNativeAnimationInitializerStep initialized;
        // Fresh D560 base is null and requested is nonnull. D69C changes it,
        // so the existing helper executes D610's unconditional DB28 call.
        // This does not implement equal-base D610 rebinding on an old holder.
        const auto status = advance_world_map_native_animation_initializer(&metadata, holder->primary_channel_.get(),
            holder->models_, initial_descriptor.identity, initial_descriptor, group.primary,
            &group.assets->primary_animations(), observations, &initialized);
        if (status == Native::AllocationFailure) return {Status::AllocationFailure};
        if (status == Native::InvalidInput || status == Native::Unchanged) return {};
        WorldMapPlayerAnimationHolderResult result;
        diagnostics(result, initialized);
        if (status != Native::Advanced) { result.status = Status::InitializerIncomplete; return result; }
        publish_known_fields(holder->state_, metadata, initial_descriptor); holder->descriptor_ = initial_descriptor;
        *out = std::move(holder); result.status = Status::ConstructedCpuHolder; return result;
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapPlayerAnimationHolderStartResult WorldMapPlayerAnimationHolder::start(
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
    const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type,
    const WorldMapAnimationInitializerObservations& observations) {
    using Status = WorldMapPlayerStartAnimationStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerAnimationHolderState>);
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerAnimationHolderStartResult>);
    try {
        auto metadata = supplied_metadata(state_);
        WorldMapPlayerStartNativeAnimationStep initialized;
        WorldMapPlayerAnimationHolderStartResult result;
        result.status = advance_world_map_player_start_animation(tables, command, item_type, &metadata,
            primary_channel_.get(), models_, animations_, observations, &initialized);
        result.selection = initialized.selection; diagnostics(result, initialized.initializer);
        if (result.status == Status::Advanced) {
            publish_known_fields(state_, metadata, initialized.selection->descriptor);
            descriptor_ = initialized.selection->descriptor; tables_ = tables;
        }
        return result;
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapPlayerInitialHolderResult construct_world_map_player_initial_animation_holder(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const WorldMapPlayerInitialAnimationQuery& query,
    const WorldMapAnimationInitializerObservations& observations,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerInitialHolderStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerInitialHolderResult>);
    if (!out) return {};
    WorldMapPlayerInitialHolderResult result;
    result.initial = prepare_world_map_player_initial_animation(inputs, query, observations);
    if (result.initial.status != WorldMapPlayerInitialAnimationStatus::Prepared) {
        result.status = result.initial.status == WorldMapPlayerInitialAnimationStatus::InvalidInput ?
            Status::InvalidInput : Status::InitialInputsIncomplete;
        return result;
    }
    if (!model_assets) { result.status = Status::RequiresModelAssets; return result; }
    constexpr uint32_t variants[]{0,0,0,1,1,2};
    if (model_assets->variant() != variants[query.phase]) { result.status = Status::PhaseMismatch; return result; }
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    result.holder = construct_world_map_player_animation_holder(model_assets, animation_assets,
        result.initial.selection->descriptor, result.initial.feature, observations, &staging);
    if (result.holder->status == WorldMapPlayerAnimationHolderStatus::AllocationFailure) {
        result.status = Status::AllocationFailure; return result;
    }
    if (result.holder->status != WorldMapPlayerAnimationHolderStatus::ConstructedCpuHolder) {
        result.status = Status::HolderIncomplete; return result;
    }
    staging->initial_inputs_ = inputs;
    staging->state_.feature_source_10 = result.initial.selection->feature_source_10;
    staging->state_.feature_flag_34 = result.initial.selection->feature_flag_34;
    *out = std::move(staging); result.status = Status::ConstructedCpuHolder; return result;
}
WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_timers(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::optional<uint32_t>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerTimedInitialHolderStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerTimedInitialHolderResult>);
    if (!out) return {};
    WorldMapPlayerTimedInitialHolderResult result;
    if (!inputs) { result.holder.initial.status = WorldMapPlayerInitialAnimationStatus::RequiresInputs; result.status = Status::InitialInputsIncomplete; return result; }
    result.holder.initial = inputs->select(query);
    if (result.holder.initial.status != WorldMapPlayerInitialAnimationStatus::Selected) { result.status = Status::InitialInputsIncomplete; return result; }
    const auto& selection = *result.holder.initial.selection;
    WorldMapPlayerTimerBinding binding;
    result.timer = bind_world_map_player_timer_assets(timers, selection.type_0, selection.type_4, clock, &binding);
    if (result.timer.status != WorldMapPlayerTimerAssetsStatus::Bound) {
        result.status = result.timer.status == WorldMapPlayerTimerAssetsStatus::AllocationFailure ? Status::AllocationFailure :
            result.timer.status == WorldMapPlayerTimerAssetsStatus::InvalidInput ? Status::InvalidInput : Status::TimerInputsIncomplete;
        return result;
    }
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    result.holder = construct_world_map_player_initial_animation_holder(model_assets, animation_assets, inputs, query, binding.observations, &staging);
    if (result.holder.status != WorldMapPlayerInitialHolderStatus::ConstructedCpuHolder) {
        result.status = result.holder.status == WorldMapPlayerInitialHolderStatus::AllocationFailure ? Status::AllocationFailure : Status::HolderIncomplete;
        return result;
    }
    staging->timer_assets_ = binding.assets;
    *out = std::move(staging); result.status = Status::ConstructedCpuHolder; return result;
}
WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_clock(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::shared_ptr<GameClock>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    if (!out) return {};
    const auto snapshot = clock ? std::optional<uint32_t>(clock->state().raw_time) : std::nullopt;
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    auto result = construct_world_map_player_initial_holder_with_timers(model_assets, animation_assets,
        inputs, timers, query, snapshot, &staging);
    if (result.status == WorldMapPlayerTimedInitialHolderStatus::ConstructedCpuHolder) {
        staging->game_clock_ = clock;
        *out = std::move(staging);
    }
    return result;
}
bool WorldMapPlayerAnimationHolder::finish_initial_model(float scale,
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets) {
    if (!assets || !state_.feature_38 || initial_model_state_) return false;
    WorldMapPlayerInitialModelState next;
    if (assets->file_count() >= 7) {
        WorldMapPlayerModelAssetView view;
        if (!assets->resource(7, &view)) return false;
        next.private_texture_animation = view;
    }
    // B8F8 copies the primary wrapper's private TAM after D610. Its stable
    // raw-byte key replaces the relocated PPC pointer; no TAM body is read.
    const auto table = next.private_texture_animation ?
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(next.private_texture_animation->data)) : 0;
    state_.feature_38->table_c = table;
    if (!primary_->initialize_root_scale(scale)) return false;
    next.initialized_c = 1; // Last supported wrapper write, after all dependencies.
    initial_model_state_ = next;
    return true;
}
WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_model_with_clock(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::shared_ptr<GameClock>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerTimedInitialHolderStatus;
    if (!out) return {};
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    auto result = construct_world_map_player_initial_holder_with_clock(model_assets, animation_assets,
        inputs, timers, query, clock, &staging);
    if (result.status != Status::ConstructedCpuHolder) return result;
    if (!staging->finish_initial_model(result.holder.initial.selection->scale, model_assets)) {
        result.status = Status::HolderIncomplete; return result;
    }
    *out = std::move(staging); return result;
}
} // namespace awl
