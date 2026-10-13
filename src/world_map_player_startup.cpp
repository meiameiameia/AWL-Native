#include "awl/world_map_player_startup.h"
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
WorldMapPlayerStatusCode WorldMapPlayerStartup::initialize_fresh_status(
    const WorldMapUpdateInputs& inputs, WorldMapPlayerUpdateOwners* owners) const noexcept {
    using S = WorldMapPlayerStatusCode;
    if (!owners || inputs.game_clock() != model_->game_clock()) return S::InvalidInput;
    if (owners->status) return S::AlreadyInitialized;
    const auto phase = inputs.sources().phase ? inputs.sources().phase->raw_word_0 : std::optional<uint32_t>{};
    WorldMapPlayerStatusState next;
    const auto status = starts_->prepare_fresh_status(phase, &next);
    if (status == S::Prepared) owners->status = next;
    return status;
}

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
    if (state_.state_1364 != 0x29) return {S::UnsupportedState, {}, {}};
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
    if ((*input.entry.control_word & 0x800u) == 0) {
        if (!owners || !owners->status) { result.status = S::RequiresStatusUpdate; return result; }
        result.status_update = prepare_world_map_player_status(*owners->status, model_->descriptor().word_8, input.status_inputs);
        if (result.status_update->status != WorldMapPlayerStatusCode::Prepared) {
            result.status = S::RequiresStatusUpdate; return result;
        }
    }
    if (!owners) { result.status = S::RequiresEffectOwners; return result; }
    WorldMapPlayerUpdateOwners next;
    result.status = prepare_world_map_player_state29_update_tail(*owners, input.pose, input.player_node_identity, &next);
    if (result.status != S::Prepared) return result;
    if (result.status_update) next.status = result.status_update->after;
    const auto collision = camera_ ? camera_->collision : input.first_camera->collision;
    camera_ = WorldMapPlayerStartupCameraState{collision, *result.callback->final_camera, *result.callback->update};
    state_.counter_1368 = *result.callback->counter_after;
    state_.control_word_1384 = *input.entry.control_word;
    *owners = std::move(next);
    result.callback->status = WorldMapPlayerState29CameraStatus::Advanced;
    result.status = S::Advanced;
    return result;
}

WorldMapPlayerStartupUpdateResult WorldMapPlayerStartup::dispatch_state29_parent(
    const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupDispatchInput& input,
    WorldMapPlayerUpdateOwners* owners) {
    using S = WorldMapPlayerUpdateStatus;
    const auto dispatch = inputs.dispatch(0x5001u);
    if (dispatch.status == WorldMapDispatchStatus::RequiresControl) return {S::RequiresDispatchControl, {}, {}};
    if (dispatch.status == WorldMapDispatchStatus::Filtered) return {S::SkippedByDispatchFilter, {}, {}};
    if (dispatch.status != WorldMapDispatchStatus::Ready || !dispatch.control_word ||
        inputs.game_clock() != model_->game_clock()) return {S::InvalidInput, {}, {}};
    WorldMapPlayerStartupUpdateInput update;
    update.entry = {dispatch.control_word, input.guards, input.scene_child_word_4};
    update.pose = input.pose;
    update.action_148 = input.action_148;
    update.first_camera = input.first_camera;
    update.player_node_identity = input.player_node_identity;
    if ((*dispatch.control_word & 0x800u) == 0) {
        const auto factors = input.status_factors_override ? *input.status_factors_override :
            starts_->status_factors(model_->descriptor());
        update.status_inputs.globals = factors.globals;
        update.status_inputs.rate = factors.rate;
    }
    // CDB8 rereads this owner at callback time; do not substitute GameClock
    // or let a supplied rate/global observation introduce another phase owner.
    if (inputs.sources().phase) update.status_inputs.phase_raw = inputs.sources().phase->raw_word_0;
    return advance_state29_parent(update, owners);
}

WorldMapPlayerStartupActorFrameResult WorldMapPlayerStartup::prepare_actor_frame(
    const WorldMapPlayerStartupActorFrameInput& input) const {
    using S = WorldMapPlayerStartupActorFrameStatus;
    if (state_.state_1364 != 0x29) return {S::UnsupportedState};
    WorldMapPlayerStartupActorFrameResult result;
    result.gate = prepare_world_map_player_state29_actor_gate(input.saved_byte_29a01, input.script);
    if (*result.gate != WorldMapPlayerActorGateStatus::Ready) { result.status = S::GateStopped; return result; }
    auto sphere = input.sphere;
    if (input.derive_normal_visibility) {
        if (sphere) { result.status = S::InvalidInput; return result; }
        if (!input.frame.actor_placement) { result.status = S::RequiresActorPlacement; return result; }
        WorldMapPlayerActorBounds bounds;
        result.bounds_status = prepare_world_map_player_normal_actor_bounds(input.frame.actor_placement->position,&bounds);
        if (*result.bounds_status != WorldMapPlayerActorBoundsStatus::Prepared) { result.status = S::BoundsIncomplete; return result; }
        result.bounds = bounds; sphere = bounds.sphere;
    }
    if (!sphere) { result.status = S::RequiresSphere; return result; }
    if (!input.visibility) { result.status = S::RequiresVisibilityOwner; return result; }
    result.visibility = prepare_world_map_player_actor_visibility(*sphere, *input.visibility);
    if (result.visibility->status != WorldMapPlayerActorVisibilityStatus::Visible) {
        result.status = S::VisibilityStopped; return result;
    }
    if (input.derive_normal_visibility) {
        if (!input.screen) { result.status = S::RequiresScreenOwner; return result; }
        result.screen = prepare_world_map_player_actor_screen(*sphere,*input.screen);
        const auto screen = result.screen->status;
        if (screen != WorldMapPlayerActorScreenStatus::Inside && screen != WorldMapPlayerActorScreenStatus::Outside) {
            result.status = S::ScreenIncomplete; return result;
        }
        result.proposed_effect_flag_15f = screen == WorldMapPlayerActorScreenStatus::Inside ? uint8_t(1) : uint8_t(0);
    }
    if (!input.frame.actor_placement) { result.status = S::RequiresActorPlacement; return result; }
    if (input.frame.root_pose) { result.status = S::InvalidInput; return result; }
    result.status = S::Prepared;
    return result;
}
WorldMapPlayerStartupActorFrameResult WorldMapPlayerStartup::advance_gated_actor_frame(
    const WorldMapPlayerStartupActorFrameInput& input) {
    using S = WorldMapPlayerStartupActorFrameStatus;
    auto result = prepare_actor_frame(input);
    if (result.status != S::Prepared) return result;
    result.frame = input.derive_normal_visibility ? model_->advance_actor_frame(input.frame) : model_->advance(input.frame);
    result.status = result.frame->status == WorldMapPlayerPrimaryUpdateStatus::Advanced ? S::FrameAdvanced : S::FrameIncomplete;
    return result;
}

WorldMapPlayerStartupActorCallbackResult WorldMapPlayerStartup::advance_state29_actor_callback(
    const WorldMapPlayerStartupActorCallbackInput& input, WorldMapPlayerActorQueue* queue) {
    return advance_actor_callback(input.control_word,input.actor,input.effect_gate,queue);
}
WorldMapPlayerStartupActorCallbackResult WorldMapPlayerStartup::advance_actor_callback(
    std::optional<uint32_t> control, const WorldMapPlayerStartupActorFrameInput& input,
    std::optional<uint8_t> effect_gate, WorldMapPlayerActorQueue* queue) {
    using S = WorldMapPlayerStartupActorCallbackStatus;
    using F = WorldMapPlayerStartupActorFrameStatus;
    if (state_.state_1364 != 0x29) return {S::UnsupportedState};
    if (!control) return {S::RequiresControl};
    WorldMapPlayerStartupActorCallbackResult result;
    result.actor = prepare_actor_frame(input);
    auto& actor = *result.actor;
    if (actor.gate == WorldMapPlayerActorGateStatus::BlockedByScript) {
        state_.control_word_1384 = *control;
        result.status = S::ScriptBlocked;
        return result;
    }
    const bool culled = actor.visibility && actor.visibility->status == WorldMapPlayerActorVisibilityStatus::Culled;
    if (actor.status != F::Prepared && !culled) { result.status = S::ActorIncomplete; return result; }
    if (!input.derive_normal_visibility) { result.status = S::InvalidInput; return result; }
    auto& primary = *model_->primary_;
    WorldMapPlayerPrimaryOwner::PreparedFrame staged;
    if (!culled) {
        const WorldMapPlayerTextureFeatureInput features{model_->state_.feature_38,model_->state_.feature_source_10,model_->timer_assets_.get()};
        actor.frame = primary.stage(input.frame,model_->primary_channel_.get(),&features,&staged);
        if (actor.frame->status != WorldMapPlayerPrimaryUpdateStatus::Advanced) {
            actor.status = F::FrameIncomplete; result.status = S::ActorIncomplete; return result;
        }
    }
    // Original order reaches the effect after frame/texture work, and the
    // player enqueue even when BBA0 culled. Do not validate these early.
    if (!queue) { result.status = S::InvalidInput; return result; }
    WorldMapPlayerActorSchedule schedule;
    result.schedule_status = prepare_world_map_player_actor_schedule(actor_metadata_,*queue,
        {actor.proposed_effect_flag_15f,effect_gate,actor_wrapper_identity(),effect_node_identity(),render_node_identity()},&schedule);
    if (*result.schedule_status != WorldMapPlayerActorScheduleStatus::Prepared) {
        result.status = S::ScheduleIncomplete; return result;
    }
    // Every operation below is nonallocating. A missing effect gate or failed
    // queue allocation cannot leave the new frame with old scheduling state.
    if (!culled) { primary.publish(std::move(staged)); actor.status = F::FrameAdvanced; }
    actor_metadata_ = schedule.metadata; queue->swap(schedule.queue);
    state_.control_word_1384 = *control;
    result.effect_scheduled = schedule.effect_scheduled;
    result.status = S::Completed;
    return result;
}

WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state29_update(
    const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates, WorldMapPlayerActorQueue* render_queue) {
    return consume_update(inputs,input,updates,render_queue,false);
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state29_update_after_animation(
    const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates, WorldMapPlayerActorQueue* render_queue) {
    return consume_update(inputs,input,updates,render_queue,true);
}
WorldMapPlayerAnimationLoopResult WorldMapPlayerStartup::advance_animation_for_loop(const WorldMapUpdateInputs& inputs) {
    using S=WorldMapPlayerAnimationLoopStatus;
    if (!model_->game_clock() || inputs.game_clock()!=model_->game_clock()) return {S::InvalidInput};
    const auto control=inputs.queue_control();
    if (control.status!=WorldMapDispatchStatus::Ready || !control.control_word) return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    if (animation_loop_ && animation_loop_->loop_count==clock.loop_count && animation_loop_->raw_time==clock.raw_time)
        return {animation_loop_->valid?S::AlreadyAdvanced:S::RequiresNextLoop};
    // ED7C calls virtual +0C only on initialized slots, then reaches +90's
    // separate feature update. Preflight this untranslated tail so it cannot
    // leave an accepted holder update behind an unsupported pool operation.
    if (!model_->initial_model_state() || !model_->initial_model_state()->initialized_c || model_->state().feature_3c)
        return {S::RequiresModelTail};
    WorldMapPlayerAnimationLoopResult result;result.animation=model_->advance_animation();
    if (result.animation->status!=WorldMapPlayerHolderProgressStatus::Advanced) {result.status=S::AnimationIncomplete;return result;}
    animation_loop_=WorldMapPlayerAnimationLoop{clock.loop_count,clock.raw_time};
    result.status=S::Advanced;return result;
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_update(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue,bool require_animation) {
    using S = WorldMapPlayerStartupQueueStatus;
    if (!updates) return {S::InvalidInput};
    const auto& queue = updates->queue;
    for (size_t i = 0; i < queue.size(); ++i) {
        if (!queue[i].node_identity || (i && queue[i-1].priority > queue[i].priority)) return {S::InvalidQueue};
        for (size_t j = 0; j < i; ++j) if (queue[j].node_identity == queue[i].node_identity) return {S::InvalidQueue};
    }
    if (queue.empty()) return {S::Empty};
    const auto key = update_node_identity();
    if (queue.front().node_identity != key) return {S::UnsupportedNode};
    if (queue.front().priority != 0x1010) return {S::UnsupportedPriority};
    if (inputs.game_clock() != model_->game_clock()) return {S::InvalidInput};
    const auto control = inputs.queue_control();
    if (control.status != WorldMapDispatchStatus::Ready || !control.control_word) return {S::RequiresControl};
    if (require_animation && (!animation_loop_ || !animation_loop_->valid || animation_loop_->loop_count!=model_->game_clock()->state().loop_count ||
        animation_loop_->raw_time!=model_->game_clock()->state().raw_time)) return {S::RequiresAnimation};
    WorldMapPlayerStartupQueueResult result;
    // Borrow observations without copying/allocating before the script gate.
    result.callback = advance_actor_callback(control.control_word,input.actor,input.effect_gate,render_queue);
    const auto handled = result.callback->status;
    if (handled != WorldMapPlayerStartupActorCallbackStatus::Completed &&
        handled != WorldMapPlayerStartupActorCallbackStatus::ScriptBlocked) {
        result.status = S::CallbackIncomplete; return result;
    }
    // 989C removes links/membership without destroying the owner. Typed erase
    // preserves every remaining key/priority and cannot fail after publication.
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerUpdateQueueEntry>);
    updates->queue.erase(updates->queue.begin());
    result.completed_node = key;
    result.status = S::Completed;
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
