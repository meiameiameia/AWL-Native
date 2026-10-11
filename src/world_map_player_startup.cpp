#include "awl/world_map_player_startup.h"
#include <new>
#include <utility>

namespace awl {
WorldMapPlayerState29CameraStep WorldMapPlayerStartup::prepare_state29_camera(
    const WorldMapPlayerStartupState29Input& input) const {
    using Status = WorldMapPlayerState29CameraStatus;
    if (state_.state_1364 != 0x29) return {Status::UnsupportedState, {}, {}, {}};
    if (camera_ && input.first_camera) return {};
    if (!camera_ && (!input.first_camera || !input.first_camera->collision))
        return {Status::RequiresCamera, {}, {}, {}};
    const auto collision = camera_ ? camera_->collision : input.first_camera->collision;
    WorldMapPlayerState29CameraQuery query;
    query.previous_camera = camera_ ? camera_->camera : input.first_camera->camera;
    query.player_position = input.player_position;
    query.counter_1368 = state_.counter_1368;
    query.action_148 = input.action_148;
    try {
        return collision->calculate_player_state29_update(query);
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure, {}, {}, {}}; }
}

WorldMapPlayerState29CameraStep WorldMapPlayerStartup::advance_state29_camera(
    const WorldMapPlayerStartupState29Input& input) {
    auto step = prepare_state29_camera(input);
    if (step.status != WorldMapPlayerState29CameraStatus::Prepared) return step;
    const auto collision = camera_ ? camera_->collision : input.first_camera->collision;
    camera_ = WorldMapPlayerStartupCameraState{collision, *step.final_camera, *step.update};
    state_.counter_1368 = *step.counter_after;
    step.status = WorldMapPlayerState29CameraStatus::Advanced;
    return step;
}

WorldMapPlayerStartupUpdateResult WorldMapPlayerStartup::advance_state29_parent(
    const WorldMapPlayerStartupUpdateInput& input, WorldMapPlayerUpdateOwners* owners) {
    using S = WorldMapPlayerUpdateStatus;
    if (state_.state_1364 != 0x29) return {S::UnsupportedState, {}};
    WorldMapPlayerStartupUpdateResult result;
    result.status = prepare_world_map_player_state29_update_entry(input.entry);
    if (result.status != S::Prepared) return result;
    result.callback = prepare_state29_camera({input.pose.position, input.action_148, input.first_camera});
    if (result.callback->status != WorldMapPlayerState29CameraStatus::Prepared) {
        result.status = result.callback->status == WorldMapPlayerState29CameraStatus::AllocationFailure ?
            S::AllocationFailure : S::CameraIncomplete;
        return result;
    }
    // The original reaches these calls only after the state callback.
    if ((*input.entry.control_word & 0x800u) == 0) { result.status = S::RequiresStatusUpdate; return result; }
    if (!owners) { result.status = S::RequiresEffectOwners; return result; }
    WorldMapPlayerUpdateOwners next;
    result.status = prepare_world_map_player_state29_update_tail(*owners, input.pose, input.player_node_identity, &next);
    if (result.status != S::Prepared) return result;
    const auto collision = camera_ ? camera_->collision : input.first_camera->collision;
    camera_ = WorldMapPlayerStartupCameraState{collision, *result.callback->final_camera, *result.callback->update};
    state_.counter_1368 = *result.callback->counter_after;
    state_.control_word_1384 = *input.entry.control_word;
    *owners = std::move(next);
    result.callback->status = WorldMapPlayerState29CameraStatus::Advanced;
    result.status = S::Advanced;
    return result;
}

WorldMapPlayerStartupResult construct_world_map_player_startup(
    const WorldMapPlayerStartupProviders& providers, const WorldMapPlayerStartupQuery& query,
    std::unique_ptr<WorldMapPlayerStartup>* out) {
    using Status = WorldMapPlayerStartupStatus;
    if (!out) return {};
    WorldMapPlayerStartupResult result;
    try {
        auto next = std::unique_ptr<WorldMapPlayerStartup>(new WorldMapPlayerStartup);
        result.initial_model = construct_world_map_player_initial_model_with_clock(providers.models,
            providers.animations, providers.initial, providers.timers, query.initial, providers.clock, &next->model_);
        if (result.initial_model.status != WorldMapPlayerTimedInitialHolderStatus::ConstructedCpuHolder) {
            result.status = result.initial_model.status == WorldMapPlayerTimedInitialHolderStatus::AllocationFailure ?
                Status::AllocationFailure : Status::InitialModelIncomplete;
            return result;
        }
        WorldMapPlayerConstructorTail tail;
        result.tail_status = plan_world_map_player_constructor_tail(query.tail, &tail);
        if (*result.tail_status != WorldMapPlayerConstructorTailStatus::Ready) {
            result.status = Status::ConstructorTailIncomplete; return result;
        }
        result.tail = tail;
        if (tail.message_prepared) { result.status = Status::RequiresMessageDelivery; return result; }
        if (tail.requested_state != 0x29) { result.status = Status::UnsupportedConstructorState; return result; }
        // 31D7C's audio dependency precedes state/model/callback work.
        if (query.tail.inputs.start.saved_pose.scene_type == 1) {
            if (!query.audio_byte_90) { result.status = Status::RequiresAudioByte; return result; }
            if (*query.audio_byte_90 != 0) { result.status = Status::RequiresAudioStop; return result; }
        }
        const auto selector = select_world_map_player_model_type(query.saved_subobject_byte_14c, query.saved_subobject_word_20);
        WorldMapPlayerModelTypeRow row;
        if (!providers.initial->select_model_type(query.initial.phase, selector, &row)) {
            result.status = Status::RequiresModelTypeInputs; return result;
        }
        result.model_row = row;
        auto& holder = *next->model_;
        WorldMapAnimationInitializerObservations observations;
        observations.clock = holder.game_clock()->state().raw_time;
        observations.fallback_table_38 = holder.timer_assets()->table_identity(WorldMapPlayerTimerBank::Eyes);
        observations.fallback_table_24 = holder.timer_assets()->table_identity(WorldMapPlayerTimerBank::Mouth);
        // Let F3E8 report its reached dependencies. Equal/below-threshold types
        // do not read a timer row. At most two distinct resets can be reached.
        WorldMapAnimationFeatureStep model;
        for (size_t attempt = 0; ; ++attempt) {
            const auto status = prepare_world_map_actor_model_type(*holder.state_.feature_38, row.type_0, row.type_4, observations, &model);
            if (status == WorldMapAnimationFeatureStatus::Prepared) break;
            if (status != WorldMapAnimationFeatureStatus::RequiresRow || !model.required_row || attempt >= 2) {
                result.status = Status::InvalidInput; return result;
            }
            result.required_row = model.required_row;
            const auto& required = *model.required_row;
            WorldMapPlayerTimerBank bank;
            if (required.table_identity == *observations.fallback_table_38) bank = WorldMapPlayerTimerBank::Eyes;
            else if (required.table_identity == *observations.fallback_table_24) bank = WorldMapPlayerTimerBank::Mouth;
            else { result.status = Status::InvalidInput; return result; }
            WorldMapPlayerTimerResourceView resource;
            if (!holder.timer_assets()->resource(bank, required.index, required.column, &resource)) {
                result.status = Status::RequiresTimerRow; return result;
            }
            observations.rows.push_back({required.table_identity, required.index, required.column, resource.identity});
        }
        result.required_row.reset();
        holder.state_.feature_38 = model.after;
        // 349A4 normal callback: original constructor variant 35, selector 1,
        // pointed +144/+148 and mode 4. Counter reset follows even a same-base
        // D660 no-op. Reached secondary/feature dependencies still stop.
        result.animation = holder.start(providers.starts, {1,0x35,query.item_144_after_setup,
            static_cast<uint32_t>(query.tail.action_148_after_setup),4}, query.item_type, observations);
        const auto started = result.animation->status;
        if (started != WorldMapPlayerStartAnimationStatus::Advanced && started != WorldMapPlayerStartAnimationStatus::Unchanged) {
            result.status = started == WorldMapPlayerStartAnimationStatus::AllocationFailure ? Status::AllocationFailure : Status::AnimationIncomplete;
            return result;
        }
        next->starts_ = providers.starts;
        // Only complete callback results establish these parent/secondary words.
        next->state_ = {0x29,0,1,{}};
        *out = std::move(next); result.status = Status::ConstructedCpuStartup; return result;
    } catch (const std::bad_alloc&) { result.status = Status::AllocationFailure; return result; }
}
} // namespace awl
