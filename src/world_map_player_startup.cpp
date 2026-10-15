#include "awl/world_map_player_startup.h"
#include "awl/world_map_phase.h"
#include <cfenv>
#include <cstring>
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
    if (primary_loop_) primary_loop_->valid=false;
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
    if (primary_loop_) primary_loop_->valid=false;
    result.callback->status = WorldMapPlayerState29CameraStatus::Advanced;
    result.status = S::Advanced;
    return result;
}

WorldMapPlayerStartupUpdateResult WorldMapPlayerStartup::dispatch_state29_parent(
    const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupDispatchInput& input,
    WorldMapPlayerUpdateOwners* owners) {
    using S = WorldMapPlayerUpdateStatus;
    const auto dispatch = inputs.dispatch(primary_registration()[0].priority);
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
    const WorldMapPlayerStartupActorFrameInput& input,bool normal) const {
    using S = WorldMapPlayerStartupActorFrameStatus;
    if (state_.state_1364 != 0x29 && !(normal && (state_.state_1364==0 || state_.state_1364==1))) return {S::UnsupportedState};
    WorldMapPlayerStartupActorFrameResult result;
    result.gate = prepare_world_map_player_normal_actor_gate(input.saved_byte_29a01, input.script);
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
    std::optional<uint8_t> effect_gate, WorldMapPlayerActorQueue* queue,bool normal) {
    using S = WorldMapPlayerStartupActorCallbackStatus;
    using F = WorldMapPlayerStartupActorFrameStatus;
    if (state_.state_1364 != 0x29 && !(normal && (state_.state_1364==0 || state_.state_1364==1))) return {S::UnsupportedState};
    if (!control) return {S::RequiresControl};
    WorldMapPlayerStartupActorCallbackResult result;
    result.actor = prepare_actor_frame(input,normal);
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
    return consume_update(inputs,input,updates,render_queue,QueuePrerequisite::None);
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state29_update_after_animation(
    const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates, WorldMapPlayerActorQueue* render_queue) {
    return consume_update(inputs,input,updates,render_queue,QueuePrerequisite::Animation);
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state29_update_after_primary_slice(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue) {
    return consume_update(inputs,input,updates,render_queue,QueuePrerequisite::Primary);
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
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state29_update_after_primary_passes(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue) {
    return consume_update(inputs,input,updates,render_queue,QueuePrerequisite::Passes);
}
WorldMapPlayerPrimaryPassesResult WorldMapPlayerStartup::complete_state29_primary_passes(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState29SecondaryInput& secondary,
    const std::optional<WorldMapPlayerUpdateGuards>& tertiary_guards) noexcept {
    using S=WorldMapPlayerPrimaryPassesStatus;using F=WorldMapPlayerFollowupStatus;
    if (!model_->game_clock() || inputs.game_clock()!=model_->game_clock()) return {S::InvalidInput};
    const auto control=inputs.queue_control();
    if (control.status!=WorldMapDispatchStatus::Ready || !control.control_word) return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    const auto current=[&](const auto& receipt) {
        return receipt && receipt->valid && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;
    };
    if (!current(primary_loop_)) return {S::RequiresPrimary};
    if (primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs))) return {S::InvalidInput};
    if (!current(animation_loop_)) return {S::RequiresAnimation};
    if (primary_loop_->pending_state_request) {
        WorldMapPlayerPrimaryPassesResult result;result.status=S::TransitionRequired;
        result.secondary=WorldMapPlayerState29SecondaryResult{F::TransitionRequired,primary_loop_->pending_state_request};return result;
    }
    if (primary_loop_->followups_completed) return {S::AlreadyCompleted};
    WorldMapPlayerPrimaryPassesResult result;
    const auto dispatch=inputs.dispatch(primary_registration()[0].priority);
    if (dispatch.status==WorldMapDispatchStatus::Filtered) result.filtered=true;
    else {
        if (dispatch.status!=WorldMapDispatchStatus::Ready || state_.state_1364!=0x29) return {S::InvalidInput};
        result.secondary=prepare_world_map_player_state29_secondary(secondary);
        const auto status=result.secondary->status;
        if (status==F::TransitionRequired) {result.status=S::TransitionRequired;return result;}
        if (status!=F::RetainedState && status!=F::SkippedByGuard) {result.status=S::SecondaryIncomplete;return result;}
        result.tertiary=prepare_world_map_player_state29_tertiary(tertiary_guards);
        if (*result.tertiary!=F::RetainedState && *result.tertiary!=F::SkippedByGuard) {result.status=S::TertiaryIncomplete;return result;}
        state_.control_word_1384=*control.control_word;
    }
    primary_loop_->followups_completed=true;
    result.status=S::Completed;return result;
}
WorldMapPlayerAnimationLoopResult WorldMapPlayerStartup::dispatch_animation_for_loop(const WorldMapUpdateInputs& inputs) {
    using S=WorldMapPlayerAnimationLoopStatus;
    if (!model_->game_clock() || inputs.game_clock()!=model_->game_clock()) return {S::InvalidInput};
    const auto dispatch=inputs.dispatch(primary_registration()[1].priority);
    if (dispatch.status==WorldMapDispatchStatus::RequiresControl) return {S::RequiresControl};
    if (dispatch.status==WorldMapDispatchStatus::Filtered) return {S::Filtered};
    if (dispatch.status!=WorldMapDispatchStatus::Ready) return {S::InvalidInput};
    return advance_animation_for_loop(inputs);
}
WorldMapPlayerPrimarySliceResult WorldMapPlayerStartup::advance_state29_primary_slice(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupDispatchInput& input,WorldMapPlayerUpdateOwners* owners) {
    using S=WorldMapPlayerPrimarySliceStatus;using P=WorldMapPlayerUpdateStatus;using A=WorldMapPlayerAnimationLoopStatus;
    static_assert(std::is_nothrow_move_constructible_v<WorldMapPlayerPrimarySliceResult>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerPrimarySliceResult>);
    if (!model_->game_clock() || inputs.game_clock()!=model_->game_clock()) return {S::InvalidInput};
    if (inputs.queue_control().status!=WorldMapDispatchStatus::Ready) return {S::RequiresControl};
    const auto clock=model_->game_clock()->state();
    const auto same=[&](const auto& receipt) {
        return receipt && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;
    };
    // Never execute the parent retroactively after a separately advanced model
    // or repeat an accepted parent changed by an isolated diagnostic call.
    if ((same(primary_loop_) && !primary_loop_->valid) || (same(animation_loop_) && !same(primary_loop_)))
        return {S::RequiresNextLoop};
    if (same(primary_loop_) && primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs))) return {S::InvalidInput};
    if (same(primary_loop_) && primary_loop_->updates_owner && owners &&
        primary_loop_->updates_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owners))) return {S::InvalidInput};
    WorldMapPlayerPrimarySliceResult result;
    if (same(primary_loop_)) result.parent_reused=true;
    else {
        const auto dispatch=inputs.dispatch(primary_registration()[0].priority);
        if (dispatch.status==WorldMapDispatchStatus::Filtered) result.parent=dispatch_state29_parent(inputs,{},nullptr);
        else {
            if (input.player_node_identity && input.player_node_identity!=update_node_identity()) return {S::InvalidInput};
            auto owned=input;owned.player_node_identity=update_node_identity();
            result.parent=dispatch_state29_parent(inputs,owned,owners);
        }
        if (result.parent->status!=P::Advanced && result.parent->status!=P::SkippedByDispatchFilter) {
            result.status=S::ParentIncomplete;return result;
        }
        primary_loop_=WorldMapPlayerPrimaryLoop{clock.loop_count,clock.raw_time,true,
            result.parent->status==P::Advanced?static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owners)):0};
        primary_loop_->inputs_owner=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs));
    }
    result.animation=dispatch_animation_for_loop(inputs);
    if (result.animation->status==A::RequiresNextLoop) result.status=S::RequiresNextLoop;
    else if (result.animation->status!=A::Advanced && result.animation->status!=A::AlreadyAdvanced) result.status=S::AnimationIncomplete;
    else result.status=S::Completed;
    return result;
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_update(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue,QueuePrerequisite prerequisite) {
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
    const bool needs_primary=prerequisite==QueuePrerequisite::Primary || prerequisite==QueuePrerequisite::Passes || prerequisite==QueuePrerequisite::NormalThird;
    if (needs_primary && (!primary_loop_ || !primary_loop_->valid ||
        primary_loop_->loop_count!=model_->game_clock()->state().loop_count || primary_loop_->raw_time!=model_->game_clock()->state().raw_time))
        return {S::RequiresPrimary};
    if (needs_primary && primary_loop_->updates_owner &&
        primary_loop_->updates_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(updates))) return {S::InvalidInput};
    if (needs_primary && primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs))) return {S::InvalidInput};
    if (prerequisite!=QueuePrerequisite::None && (!animation_loop_ || !animation_loop_->valid || animation_loop_->loop_count!=model_->game_clock()->state().loop_count ||
        animation_loop_->raw_time!=model_->game_clock()->state().raw_time)) return {S::RequiresAnimation};
    if (prerequisite==QueuePrerequisite::Passes && !primary_loop_->followups_completed) return {S::RequiresPrimaryPasses};
    WorldMapPlayerStartupQueueResult result;
    // Borrow observations without copying/allocating before the script gate.
    result.callback = advance_actor_callback(control.control_word,input.actor,input.effect_gate,render_queue,prerequisite==QueuePrerequisite::NormalThird);
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

WorldMapPlayerStartupTickResult WorldMapPlayerStartup::advance_state29_startup_tick(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupTickInput& input,
    WorldMapFade* fade,WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue) {
    using S=WorldMapPlayerStartupTickStatus;using F=WorldMapFadeStatus;using P=WorldMapPlayerPrimaryPassesStatus;
    static_assert(std::is_nothrow_move_constructible_v<WorldMapPlayerStartupTickResult>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerStartupTickResult>);
    if (!fade || !updates || !model_->game_clock() || inputs.game_clock()!=model_->game_clock() || fade->game_clock()!=model_->game_clock())
        return {S::InvalidInput};
    if (!fade->state().started_5c) {
        WorldMapPlayerStartupTickResult result;result.status=S::FadeIncomplete;result.fade=F::RequiresStartTime;return result;
    }
    const auto clock=model_->game_clock()->state();
    const auto fade_key=fade->primary_node_identity();
    const bool same=primary_loop_ && primary_loop_->loop_count==clock.loop_count && primary_loop_->raw_time==clock.raw_time;
    if (same && primary_loop_->fade_owner && primary_loop_->fade_owner!=fade_key) return {S::InvalidInput};
    if (same && primary_loop_->followups_completed && !primary_loop_->fade_owner) return {S::RequiresNextLoop};
    const bool fade_handled=fade->loop() && fade->loop()->loop_count==clock.loop_count && fade->loop()->raw_time==clock.raw_time;
    if (fade_handled && (!same || !primary_loop_->fade_owner)) return {S::RequiresNextLoop};
    WorldMapPlayerStartupTickResult result;
    result.primary=advance_state29_primary_slice(inputs,input.parent,updates);
    if (result.primary->status!=WorldMapPlayerPrimarySliceStatus::Completed) {
        result.status=result.primary->status==WorldMapPlayerPrimarySliceStatus::RequiresNextLoop?S::RequiresNextLoop:S::PrimaryIncomplete;
        return result;
    }
    // Native source binding after the accepted first pass, before fade work.
    primary_loop_->fade_owner=fade_key;
    result.fade=fade->advance_for_loop(inputs,render_queue);
    if (*result.fade!=F::Advanced && *result.fade!=F::AlreadyAdvanced && *result.fade!=F::Idle) {
        result.status=*result.fade==F::RequiresNextLoop?S::RequiresNextLoop:S::FadeIncomplete;return result;
    }
    auto secondary=input.secondary;
    if (!primary_loop_->followups_completed && !primary_loop_->pending_state_request) {
        const auto dispatch=inputs.dispatch(primary_registration()[0].priority);
        // The original guard can skip 34B44, including its mode read. The
        // caller's optional diagnostic override is checked only if reached.
        const bool reaches_mode=dispatch.status==WorldMapDispatchStatus::Ready &&
            prepare_world_map_player_state29_tertiary(secondary.guards)==WorldMapPlayerFollowupStatus::RetainedState;
        if (reaches_mode && secondary.mode_word_60 && *secondary.mode_word_60!=fade->state().mode_60) {
            result.status=S::InvalidInput;return result;
        }
        secondary.mode_word_60=fade->state().mode_60;
    }
    result.passes=complete_state29_primary_passes(inputs,secondary,input.tertiary_guards);
    if (result.passes->status==P::TransitionRequired) {
        primary_loop_->pending_state_request=result.passes->secondary->requested_state;
        result.status=S::TransitionRequired;return result;
    }
    if (result.passes->status!=P::Completed && result.passes->status!=P::AlreadyCompleted) {result.status=S::PassesIncomplete;return result;}
    if (primary_loop_->queued_completed) {result.queue_reused=true;result.status=S::Completed;return result;}
    result.queued=consume_state29_update_after_primary_passes(inputs,input.actor,updates,render_queue);
    const auto queued=result.queued->status;
    result.status=queued==WorldMapPlayerStartupQueueStatus::Completed || queued==WorldMapPlayerStartupQueueStatus::Empty?S::Completed:S::QueueIncomplete;
    if (result.status==S::Completed) {
        primary_loop_->queued_completed=true;
        if (!primary_loop_->updates_owner) primary_loop_->updates_owner=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(updates));
    }
    return result;
}

WorldMapPlayerState0InitializerResult prepare_world_map_player_state0_initializer(
    const WorldMapPlayerState0TransitionInput& input,std::optional<uint8_t> completed_20,
    std::optional<uint8_t> flag_21,uint8_t prior_secondary) noexcept {
    using S=WorldMapPlayerState0TransitionStatus;
    if (!input.item_144) return {S::RequiresItem,{}};
    if (*input.item_144!=0) {
        if (!input.item_binding_1454) return {S::RequiresItemBinding,{}};
        if (*input.item_binding_1454) return {S::RequiresItemAttachment,{}};
    }
    if (!input.action_148) return {S::RequiresAction,{}};
    if (*input.action_148!=0) {
        if (!input.action_binding_3ec) return {S::RequiresActionBinding,{}};
        if (*input.action_binding_3ec) return {S::RequiresActionAttachment,{}};
    }
    if (!completed_20) return {S::RequiresHolderCompleted,{}};
    if (*completed_20) return {S::Prepared,uint8_t{0}};
    if (!flag_21) return {S::RequiresHolderFlag,{}};
    return {S::Prepared,*flag_21?prior_secondary:uint8_t{0}};
}
WorldMapPlayerState0TransitionResult WorldMapPlayerStartup::advance_pending_state0_transition(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState0TransitionInput& input,
    const WorldMapFade* fade,WorldMapPlayerUpdateOwners* owners) {
    using S=WorldMapPlayerState0TransitionStatus;
    static_assert(std::is_nothrow_move_constructible_v<WorldMapPlayerState0TransitionResult>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerState0TransitionResult>);
    if (!fade || !owners || !model_->game_clock() || inputs.game_clock()!=model_->game_clock() || fade->game_clock()!=model_->game_clock()) return {S::InvalidInput};
    const auto control=inputs.queue_control();
    if (control.status!=WorldMapDispatchStatus::Ready || !control.control_word) return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    const auto current=[&](const auto& receipt){return receipt && receipt->valid && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if (!current(primary_loop_)) return {S::RequiresPendingTransition};
    if (primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs)) ||
        primary_loop_->updates_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owners)) || primary_loop_->fade_owner!=fade->primary_node_identity()) return {S::InvalidInput};
    if (!current(animation_loop_)) return {S::RequiresAnimation};
    if (!current(fade->loop())) return {S::RequiresFade};
    if (state_.state_1364==0 && primary_loop_->state0_transition_completed) return {S::AlreadyAdvanced};
    if (state_.state_1364!=0x29 || primary_loop_->pending_state_request!=0) return {S::RequiresPendingTransition};
    WorldMapPlayerState0TransitionResult result;
    const auto stop=[&](S status){result.status=status;return result;};
    if (!input.saved_byte_14d35) return stop(S::RequiresSavedRemapByte);
    if (!input.secondary_byte_3f1) return stop(S::RequiresSecondaryRemapByte);
    result.remapped_state=*input.saved_byte_14d35?0xd9:*input.secondary_byte_3f1?0xed:0;
    if (*result.remapped_state!=0) return stop(S::UnsupportedRemappedState);
    if (!input.scene_type_68) return stop(S::RequiresSceneType);
    if (*input.scene_type_68==1) {
        if (!input.audio_byte_90) return stop(S::RequiresAudioByte);
        if (*input.audio_byte_90) return stop(S::RequiresAudioStop);
    }
    const auto appearance=prepare_state0_appearance(inputs,input.saved_subobject_byte_14c,owners);
    result.model_row=appearance.model_row;result.model=appearance.model;
    if(appearance.status!=S::Prepared)return stop(appearance.status);
    auto& holder=*model_;
    const auto initialized=prepare_world_map_player_state0_initializer(input,holder.state_.completed_20,holder.state_.flag_21,state_.secondary_byte_3f0);
    if (initialized.status!=S::Prepared) return stop(initialized.status);
    holder.state_.feature_38=result.model->after;
    state_.state_1364=0;state_.control_word_1384=*control.control_word;state_.secondary_byte_3f0=*initialized.secondary_byte_3f0;
    primary_loop_->pending_state_request.reset();primary_loop_->state0_transition_completed=true;
    result.status=S::Advanced;return result;
}
WorldMapPlayerState0TransitionResult WorldMapPlayerStartup::prepare_state0_appearance(
    const WorldMapUpdateInputs& inputs,std::optional<uint8_t> byte14c,const WorldMapPlayerUpdateOwners* owners,const WorldMapPlayerAnimationHolderState* staged) const {
    using S=WorldMapPlayerState0TransitionStatus;using F=WorldMapAnimationFeatureStatus;
    WorldMapPlayerState0TransitionResult result;const auto stop=[&](S status){result.status=status;return result;};
    if (!byte14c) return stop(S::RequiresModelByte);
    uint32_t selector=0x24;
    if (*byte14c==0) {
        if (!owners || !owners->status) return stop(S::RequiresStatus);
        if (std::fegetround()!=FE_TONEAREST) return stop(S::UnsupportedNumerics);
        int32_t word20;std::memcpy(&word20,&owners->status->word_20,sizeof(word20));
        selector=select_world_map_player_model_type(0,word20);
    }
    const auto raw=inputs.sources().phase?inputs.sources().phase->raw_word_0:std::nullopt;
    if (!raw) return stop(S::RequiresPhaseRaw);
    auto& holder=*model_;
    const auto& state=staged?*staged:holder.state_;
    WorldMapPlayerModelTypeRow row;
    if (!state.feature_38 || state.feature_source_10!=0u || !holder.initial_inputs() ||
        !holder.initial_inputs()->select_model_type(derive_world_map_phase(*raw),selector,&row)) return stop(S::RequiresModelType);
    result.model_row=row;
    try {
        WorldMapAnimationInitializerObservations observations;observations.clock=holder.game_clock()->state().raw_time;
        if (!holder.timer_assets()) return stop(S::RequiresTimerRow);
        observations.fallback_table_38=holder.timer_assets()->table_identity(WorldMapPlayerTimerBank::Eyes);
        observations.fallback_table_24=holder.timer_assets()->table_identity(WorldMapPlayerTimerBank::Mouth);
        WorldMapAnimationFeatureStep model;
        for(size_t attempt=0;;++attempt) {
            const auto status=prepare_world_map_actor_model_type(*state.feature_38,row.type_0,row.type_4,observations,&model);
            result.model=model;
            if(status==F::Prepared)break;
            if(status!=F::RequiresRow || !model.required_row || attempt>=2)return stop(S::RequiresTimerRow);
            const auto required=*model.required_row;
            WorldMapPlayerTimerBank bank;
            if(required.table_identity==*observations.fallback_table_38)bank=WorldMapPlayerTimerBank::Eyes;
            else if(required.table_identity==*observations.fallback_table_24)bank=WorldMapPlayerTimerBank::Mouth;
            else return stop(S::InvalidInput);
            WorldMapPlayerTimerResourceView resource;
            if(!holder.timer_assets()->resource(bank,required.index,required.column,&resource))return stop(S::RequiresTimerRow);
            observations.rows.push_back({required.table_identity,required.index,required.column,resource.identity});
        }
        result.status=S::Prepared;return result;
    } catch(const std::bad_alloc&) {return stop(S::AllocationFailure);}
}

bool WorldMapPlayerStartup::seed_state0_context(const WorldMapPlayerContextUpdateState& context) noexcept {
    if(state_.state_1364!=0 || state0_context_ || state0_third_loop_)return false;
    state0_context_=context;return true;
}
WorldMapPlayerState0ParentResult WorldMapPlayerStartup::dispatch_state0_parent(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState0ParentInput& input,WorldMapPlayerUpdateOwners* owners) {
    using S=WorldMapPlayerUpdateStatus;using A=WorldMapPlayerState0TransitionStatus;
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    const auto dispatch=inputs.dispatch(primary_registration()[0].priority);
    if(dispatch.status==WorldMapDispatchStatus::RequiresControl)return {S::RequiresDispatchControl};
    if(dispatch.status==WorldMapDispatchStatus::Filtered)return {S::SkippedByDispatchFilter};
    if(dispatch.status!=WorldMapDispatchStatus::Ready || !dispatch.control_word)return {S::InvalidInput};
    if(state_.state_1364!=0)return {S::UnsupportedState};
    WorldMapPlayerState0ParentResult result;
    result.status=prepare_world_map_player_normal_update_entry({dispatch.control_word,input.guards,input.scene_child_word_4});
    if(result.status!=S::Prepared)return result;
    result.appearance=prepare_state0_appearance(inputs,input.saved_subobject_byte_14c,owners);
    if(result.appearance->status!=A::Prepared){
        result.status=result.appearance->status==A::AllocationFailure?S::AllocationFailure:S::AppearanceIncomplete;return result;
    }
    if((*dispatch.control_word&0x800u)==0){
        if(!owners || !owners->status){result.status=S::RequiresStatusUpdate;return result;}
        WorldMapPlayerStatusInputs status;
        const auto factors=input.status_factors_override?*input.status_factors_override:starts_->status_factors(model_->descriptor());
        status.globals=factors.globals;status.rate=factors.rate;
        if(inputs.sources().phase)status.phase_raw=inputs.sources().phase->raw_word_0;
        result.status_update=prepare_world_map_player_status(*owners->status,model_->descriptor().word_8,status);
        if(result.status_update->status!=WorldMapPlayerStatusCode::Prepared){result.status=S::RequiresStatusUpdate;return result;}
    }
    if(!owners){result.status=S::RequiresEffectOwners;return result;}
    WorldMapPlayerUpdateOwners next;
    result.status=prepare_world_map_player_normal_update_tail(*owners,input.pose,update_node_identity(),&next);
    if(result.status!=S::Prepared)return result;
    if(result.status_update)next.status=result.status_update->after;
    model_->state_.feature_38=result.appearance->model->after;
    state_.control_word_1384=*dispatch.control_word;*owners=std::move(next);
    if(primary_loop_)primary_loop_->valid=false;
    result.status=S::Advanced;return result;
}
namespace {
bool state1_camera_height(const std::array<float,3>& point,float* out,void* context){
    return static_cast<const WorldMapCameraCollisionAsset*>(context)->sample_height(point,out);
}
}
WorldMapPlayerState1ParentResult WorldMapPlayerStartup::dispatch_state1_parent(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState1ParentInput& input,WorldMapPlayerUpdateOwners* owners,
    WorldMapPlayerMovementState* movement,WorldMapSceneBucketRegistry* scene){
    using S=WorldMapPlayerUpdateStatus;using A=WorldMapPlayerStartAnimationStatus;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerMovementState>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapSceneBucketRegistry>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerUpdateOwners>);
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerStartupCameraState>);
    static_assert(std::is_nothrow_copy_constructible_v<WorldMapPlayerState1ParentResult>);
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    const auto dispatch=inputs.dispatch(primary_registration()[0].priority);
    if(dispatch.status==WorldMapDispatchStatus::RequiresControl)return {S::RequiresDispatchControl};
    if(dispatch.status==WorldMapDispatchStatus::Filtered)return {S::SkippedByDispatchFilter};
    if(dispatch.status!=WorldMapDispatchStatus::Ready || !dispatch.control_word)return {S::InvalidInput};
    if(state_.state_1364!=1)return {S::UnsupportedState};
    WorldMapPlayerState1ParentResult result;const auto stop=[&](S status){result.status=status;return result;};
    result.status=prepare_world_map_player_normal_update_entry({dispatch.control_word,input.guards,input.scene_child_word_4});
    if(result.status!=S::Prepared)return result;
    if(!owners)return stop(S::RequiresEffectOwners);
    const bool active=(*dispatch.control_word&0x800u)==0;
    try {
        WorldMapPlayerAnimationHolder::StartPreparation prepared;model_->seed_start(prepared);
        std::optional<WorldMapPlayerMovementState> moving;
        std::optional<WorldMapSceneBucketRegistry> registry;
        WorldMapPlayerScenePose pose;
        if(active){
            if(!movement || !scene)return stop(S::RequiresMovementOwners);
            if(!camera_ || !camera_->collision)return stop(S::RequiresMovementCamera);
            if(!input.movement)return stop(S::MovementIncomplete);
            moving=*movement;registry=*scene;moving->camera=camera_->camera;
            auto dependencies=*input.movement;dependencies.guards=input.guards;dependencies.outputs={true,true,false,false};
            dependencies.sample_height=state1_camera_height;
            dependencies.sample_context=const_cast<WorldMapCameraCollisionAsset*>(camera_->collision.get()); // opaque, read-only callback
            result.movement=advance_world_map_player_movement(inputs,dependencies,&*moving,&*registry);
            if(result.movement->status!=WorldMapPlayerMovementStatus::Advanced)return stop(result.movement->status==WorldMapPlayerMovementStatus::AllocationFailure?S::AllocationFailure:S::MovementIncomplete);
            pose=moving->pose;
            result.command=prepare_world_map_player_state1_animation(dispatch.control_word,model_->initial_inputs()->state1_animation_profile(),
                result.movement->intensity,moving->steering.current_speed,input.guards,input.item_144,input.action_148);
            if(result.command->status!=WorldMapPlayerState1AnimationStatus::Prepared)return stop(S::RequiresAnimationCommand);
            if(result.command->command){
                result.animation=model_->prepare_start_with_timers(starts_,*result.command->command,input.item_type,&prepared);
                if(result.animation->status!=A::Advanced && result.animation->status!=A::Unchanged)return stop(result.animation->status==A::AllocationFailure?S::AllocationFailure:S::AnimationIncomplete);
                result.speed=model_->prepare_speed(prepared,*result.command->command_speed);
                if(*result.speed!=WorldMapAnimationChannelStatus::Prepared)return stop(S::AnimationIncomplete);
            }
            if(result.command->final_speed!=result.command->command_speed){
                result.speed=model_->prepare_speed(prepared,*result.command->final_speed);
                if(*result.speed!=WorldMapAnimationChannelStatus::Prepared)return stop(S::AnimationIncomplete);
            }
            result.appearance=prepare_state0_appearance(inputs,input.saved_subobject_byte_14c,owners,&prepared.state);
            if(result.appearance->status!=WorldMapPlayerState0TransitionStatus::Prepared)return stop(result.appearance->status==WorldMapPlayerState0TransitionStatus::AllocationFailure?S::AllocationFailure:S::AppearanceIncomplete);
            prepared.state.feature_38=result.appearance->model->after;
            if(!owners->status)return stop(S::RequiresStatusUpdate);
            WorldMapPlayerStatusInputs status;
            const auto factors=input.status_factors_override?*input.status_factors_override:starts_->status_factors(prepared.descriptor);
            status.globals=factors.globals;status.rate=factors.rate;if(inputs.sources().phase)status.phase_raw=inputs.sources().phase->raw_word_0;
            result.status_update=prepare_world_map_player_status(*owners->status,prepared.descriptor.word_8,status);
            if(result.status_update->status!=WorldMapPlayerStatusCode::Prepared)return stop(S::RequiresStatusUpdate);
        }else{
            if(movement && input.paused_pose)return stop(S::InvalidInput);
            if(movement)pose=movement->pose;else if(input.paused_pose)pose=*input.paused_pose;else return stop(S::RequiresPose);
        }
        WorldMapPlayerUpdateOwners after;
        result.status=prepare_world_map_player_normal_update_tail(*owners,pose,update_node_identity(),&after);
        if(result.status!=S::Prepared)return result;if(result.status_update)after.status=result.status_update->after;
        // All remaining publication is nonallocating. Original parent writes
        // are interleaved; native motion/animation/status/scene stay atomic.
        if(active){
            model_->publish_start(prepared);
            camera_=WorldMapPlayerStartupCameraState{camera_->collision,*moving->camera,moving->camera_update->update};
            state_.target_14ac=result.movement->metadata;state_.direction_14b4=result.movement->direction;
            *movement=std::move(*moving);*scene=std::move(*registry);
        }
        state_.control_word_1384=*dispatch.control_word;*owners=std::move(after);
        if(primary_loop_)primary_loop_->valid=false;result.status=S::Advanced;return result;
    }catch(const std::bad_alloc&){return stop(S::AllocationFailure);}
}
WorldMapPlayerState1PrimarySliceResult WorldMapPlayerStartup::advance_state1_primary_slice(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState1ParentInput& input,WorldMapPlayerUpdateOwners* owners,
    WorldMapPlayerMovementState* movement,WorldMapSceneBucketRegistry* scene){
    using S=WorldMapPlayerPrimarySliceStatus;using P=WorldMapPlayerUpdateStatus;using A=WorldMapPlayerAnimationLoopStatus;
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock() || state_.state_1364!=1)return {S::InvalidInput};
    if(inputs.queue_control().status!=WorldMapDispatchStatus::Ready)return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    const auto same=[&](const auto& receipt){return receipt && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if((same(primary_loop_) && (!primary_loop_->valid || !primary_loop_->state1_parent_completed)) || (same(animation_loop_) && !same(primary_loop_)))return {S::RequiresNextLoop};
    const auto key=[](const auto* ptr){return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));};
    if(same(primary_loop_) && (primary_loop_->inputs_owner!=key(&inputs) || (owners && primary_loop_->updates_owner && primary_loop_->updates_owner!=key(owners)) ||
        (movement && primary_loop_->movement_owner && primary_loop_->movement_owner!=key(movement)) || (scene && primary_loop_->scene_owner && primary_loop_->scene_owner!=key(scene))))return {S::InvalidInput};
    WorldMapPlayerState1PrimarySliceResult result;
    if(same(primary_loop_))result.parent_reused=true;
    else {
        result.parent=dispatch_state1_parent(inputs,input,owners,movement,scene);
        if(result.parent->status!=P::Advanced && result.parent->status!=P::SkippedByDispatchFilter){result.status=S::ParentIncomplete;return result;}
        primary_loop_=WorldMapPlayerPrimaryLoop{clock.loop_count,clock.raw_time,true,result.parent->status==P::Advanced?key(owners):0};
        primary_loop_->inputs_owner=key(&inputs);primary_loop_->state1_parent_completed=true;
        if(result.parent->status==P::Advanced){primary_loop_->movement_owner=key(movement);primary_loop_->scene_owner=key(scene);
            if(result.parent->movement){primary_loop_->item_144=input.item_144;primary_loop_->action_148=input.action_148;}}
    }
    result.animation=dispatch_animation_for_loop(inputs);
    if(result.animation->status==A::RequiresNextLoop)result.status=S::RequiresNextLoop;
    else if(result.animation->status!=A::Advanced && result.animation->status!=A::AlreadyAdvanced)result.status=S::AnimationIncomplete;
    else result.status=S::Completed;return result;
}
WorldMapPlayerState1SecondaryStatus WorldMapPlayerStartup::advance_state1_secondary(
    const WorldMapUpdateInputs& inputs,const std::optional<WorldMapPlayerUpdateGuards>& guards) noexcept {
    using S=WorldMapPlayerState1SecondaryStatus;
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock())return S::InvalidInput;
    const auto control=inputs.queue_control();if(control.status!=WorldMapDispatchStatus::Ready || !control.control_word)return S::RequiresControl;
    const auto& clock=model_->game_clock()->state();const auto current=[&](const auto& r){return r && r->valid && r->loop_count==clock.loop_count && r->raw_time==clock.raw_time;};
    if(state_.state_1364!=1 || !current(primary_loop_) || !primary_loop_->state1_parent_completed)return S::RequiresPrimary;
    if(primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs)))return S::InvalidInput;
    if(!current(animation_loop_))return S::RequiresAnimation;if(primary_loop_->state1_secondary_completed)return S::AlreadyCompleted;
    if(inputs.dispatch(primary_registration()[0].priority).status==WorldMapDispatchStatus::Filtered){primary_loop_->state1_secondary_completed=true;return S::Filtered;}
    if(!guards)return S::RequiresGuards;
    bool skip=guards->word_680!=-1;if(!skip){if(!guards->word_58c)return S::RequiresGuardWord58c;skip=*guards->word_58c!=0;}
    if(!skip && (*control.control_word&0x800u)==0)return S::RequiresBody;
    state_.control_word_1384=*control.control_word;primary_loop_->state1_secondary_completed=true;return S::Completed;
}
WorldMapPlayerState1SecondaryResult WorldMapPlayerStartup::advance_state1_secondary(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState1SecondaryInput& input,
    WorldMapPlayerUpdateOwners* owners,const WorldMapPlayerMovementState* movement){
    using S=WorldMapPlayerState1SecondaryStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerStatusState>);
    static_assert(std::is_nothrow_copy_constructible_v<WorldMapPlayerState1SecondaryResult>);
    const auto key=[](const auto* owner){return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner));};
    const auto prefix=advance_state1_secondary(inputs,input.guards);
    if(prefix==S::AlreadyCompleted && primary_loop_->updates_owner &&
        ((owners && primary_loop_->updates_owner!=key(owners)) || (movement && primary_loop_->movement_owner!=key(movement))))return {S::InvalidInput};
    if(prefix!=S::RequiresBody)return {prefix};
    if(!owners)return {S::RequiresUpdateOwners};
    if(primary_loop_->updates_owner!=key(owners) || (movement && primary_loop_->movement_owner!=key(movement)))return {S::InvalidInput};
    if(movement && (movement->pose.scene_type!=owners->saved_pose.scene_type || movement->pose.position!=owners->saved_pose.position || movement->pose.heading!=owners->saved_pose.heading))return {S::InvalidInput};
    WorldMapPlayerState1SecondaryQuery query;query.control=inputs.queue_control().control_word;
    query.action=primary_loop_->action_148;query.item=primary_loop_->item_144;
    query.status=owners->status?&*owners->status:nullptr;
    query.factors=input.condition_factors_override?*input.condition_factors_override:starts_->condition_factors();
    if(inputs.sources().phase)query.phase_raw=inputs.sources().phase->raw_word_0;
    if(const auto* pad=inputs.pad_for_loop())query.pressed=pad->frame.pressed;
    if(movement)query.pose=movement->pose;query.manager_scene_type=inputs.scene().scene_type_68;query.dependencies=input;
    auto result=prepare_world_map_player_state1_secondary(query);
    if(result.status==S::Completed){
        if(result.conditions && result.conditions->after)owners->status=result.conditions->after;
        state_.control_word_1384=*query.control;primary_loop_->state1_secondary_completed=true;
    }
    return result;
}
WorldMapPlayerState0PrimarySliceResult WorldMapPlayerStartup::advance_state0_primary_slice(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState0ParentInput& input,WorldMapPlayerUpdateOwners* owners) {
    using S=WorldMapPlayerPrimarySliceStatus;using P=WorldMapPlayerUpdateStatus;using A=WorldMapPlayerAnimationLoopStatus;
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    if(inputs.queue_control().status!=WorldMapDispatchStatus::Ready)return {S::RequiresControl};
    if(state_.state_1364!=0)return {S::InvalidInput};
    const auto& clock=model_->game_clock()->state();
    const auto same=[&](const auto& receipt){return receipt && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if((same(primary_loop_) && (!primary_loop_->valid || !primary_loop_->state0_parent_completed)) ||
        (same(animation_loop_) && !same(primary_loop_)))return {S::RequiresNextLoop};
    const auto owner=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs));
    if(same(primary_loop_) && primary_loop_->inputs_owner!=owner)return {S::InvalidInput};
    if(same(primary_loop_) && primary_loop_->updates_owner && owners &&
        primary_loop_->updates_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owners)))return {S::InvalidInput};
    WorldMapPlayerState0PrimarySliceResult result;
    if(same(primary_loop_))result.parent_reused=true;
    else {
        result.parent=dispatch_state0_parent(inputs,input,owners);
        if(result.parent->status!=P::Advanced && result.parent->status!=P::SkippedByDispatchFilter){result.status=S::ParentIncomplete;return result;}
        primary_loop_=WorldMapPlayerPrimaryLoop{clock.loop_count,clock.raw_time,true,
            result.parent->status==P::Advanced?static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owners)):0};
        primary_loop_->inputs_owner=owner;primary_loop_->state0_parent_completed=true;
    }
    result.animation=dispatch_animation_for_loop(inputs);
    if(result.animation->status==A::RequiresNextLoop)result.status=S::RequiresNextLoop;
    else if(result.animation->status!=A::Advanced && result.animation->status!=A::AlreadyAdvanced)result.status=S::AnimationIncomplete;
    else result.status=S::Completed;
    return result;
}
WorldMapPlayerState0SecondaryResult WorldMapPlayerStartup::advance_state0_secondary(
    const WorldMapUpdateInputs& inputs,const std::optional<WorldMapPlayerUpdateGuards>& guards) noexcept {
    using S=WorldMapPlayerState0SecondaryStatus;
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    if(inputs.queue_control().status!=WorldMapDispatchStatus::Ready)return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    const auto current=[&](const auto& receipt){return receipt && receipt->valid && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if(state_.state_1364!=0 || !current(primary_loop_) || !primary_loop_->state0_parent_completed)return {S::RequiresPrimary};
    if(primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs)))return {S::InvalidInput};
    if(!current(animation_loop_))return {S::RequiresAnimation};
    if(primary_loop_->pending_state_request)return {S::TransitionRequired,primary_loop_->pending_state_request};
    if(primary_loop_->state0_secondary_completed)return {S::AlreadyCompleted};
    const auto dispatch=inputs.dispatch(primary_registration()[0].priority);
    WorldMapPlayerState0SecondaryResult result;
    if(dispatch.status==WorldMapDispatchStatus::Filtered)result.status=S::Filtered;
    else {
        if(dispatch.status!=WorldMapDispatchStatus::Ready || !dispatch.control_word)return {S::InvalidInput};
        result=prepare_world_map_player_state0_secondary(guards,model_->state_.completed_20,model_->state_.flag_21);
        if(result.status!=S::RetainedState && result.status!=S::SkippedByGuard)return result;
        state_.control_word_1384=*dispatch.control_word;
    }
    primary_loop_->state0_secondary_completed=true;return result;
}
WorldMapPlayerState0SecondaryResult WorldMapPlayerStartup::request_state1_transition(
    const WorldMapUpdateInputs& inputs,const std::optional<WorldMapPlayerUpdateGuards>& guards) noexcept {
    auto result=advance_state0_secondary(inputs,guards);
    if(result.status==WorldMapPlayerState0SecondaryStatus::TransitionRequired && result.requested_state==1)
        primary_loop_->pending_state_request=1;
    return result;
}
WorldMapPlayerState0SecondarySliceResult WorldMapPlayerStartup::advance_state0_secondary(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState0SecondaryInput& input,
    WorldMapPlayerUpdateOwners* owners,WorldMapPlayerMovementState* movement,WorldMapSceneBucketRegistry* scene){
    using S=WorldMapPlayerState0SecondaryStatus;using M=WorldMapPlayerMovementStatus;using B=WorldMapPlayerState1SecondaryStatus;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerMovementState>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapSceneBucketRegistry>);
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerStartupCameraState>);
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerStatusState>);
    static_assert(std::is_nothrow_copy_constructible_v<WorldMapPlayerState0SecondarySliceResult>);
    const auto key=[](const auto* owner){return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner));};
    const auto prefix=advance_state0_secondary(inputs,input.body.guards);
    if((prefix.status==S::AlreadyCompleted || prefix.status==S::TransitionRequired) && primary_loop_ &&
        ((owners && primary_loop_->updates_owner && primary_loop_->updates_owner!=key(owners)) ||
         (movement && primary_loop_->movement_owner && primary_loop_->movement_owner!=key(movement)) ||
         (scene && primary_loop_->scene_owner && primary_loop_->scene_owner!=key(scene))))return {S::InvalidInput};
    if(prefix.status==S::TransitionRequired){
        // Completed holder and previously accepted movement share this request.
        // No second receipt until the checked initializer has completed.
        primary_loop_->pending_state_request=prefix.requested_state;
        return {prefix.status,prefix.requested_state};
    }
    if(prefix.status!=S::RequiresMovement)return {prefix.status,prefix.requested_state};
    if(!owners)return {S::RequiresUpdateOwners};
    if(primary_loop_->updates_owner!=key(owners))return {S::InvalidInput};
    if(!movement || !scene)return {S::RequiresMovementOwners};
    if(movement->pose.scene_type!=owners->saved_pose.scene_type || movement->pose.position!=owners->saved_pose.position ||
        movement->pose.heading!=owners->saved_pose.heading)return {S::InvalidInput};
    if(!camera_ || !camera_->collision)return {S::RequiresMovementCamera};
    if(!input.movement)return {S::RequiresMovement};
    WorldMapPlayerState0SecondarySliceResult result;
    const auto stop=[&](S status){result.status=status;return result;};
    try {
        auto moving=*movement;auto registry=*scene;moving.camera=camera_->camera;
        auto dependencies=*input.movement;dependencies.guards=input.body.guards;dependencies.outputs={};
        dependencies.sample_height=state1_camera_height;
        dependencies.sample_context=const_cast<WorldMapCameraCollisionAsset*>(camera_->collision.get());
        result.movement=advance_world_map_player_movement(inputs,dependencies,&moving,&registry);
        if(result.movement->status!=M::Advanced)return stop(result.movement->status==M::AllocationFailure?S::AllocationFailure:S::MovementIncomplete);
        const auto route=prepare_world_map_player_state0_secondary(input.body.guards,model_->state_.completed_20,
            model_->state_.flag_21,result.movement->intensity);
        if(route.status==S::RequiresBody){
            WorldMapPlayerState1SecondaryQuery query;query.control=inputs.queue_control().control_word;
            query.action=input.action_148;query.item=input.item_144;query.status=owners->status?&*owners->status:nullptr;
            query.factors=input.body.condition_factors_override?*input.body.condition_factors_override:starts_->condition_factors();
            if(inputs.sources().phase)query.phase_raw=inputs.sources().phase->raw_word_0;
            if(const auto* pad=inputs.pad_for_loop())query.pressed=pad->frame.pressed;
            query.pose=moving.pose;query.manager_scene_type=inputs.scene().scene_type_68;query.dependencies=input.body;
            result.body=prepare_world_map_player_state1_secondary(query);
            if(result.body->status!=B::Completed)return stop(result.body->status==B::AllocationFailure?S::AllocationFailure:S::BodyIncomplete);
        }else if(route.status!=S::TransitionRequired)return stop(route.status);
        // Native publication defers movement until the supported body completes.
        // The original interleaves these writes. First/model remain accepted on
        // a stop; a pending state-one initializer is a separate accepted stage.
        camera_=WorldMapPlayerStartupCameraState{camera_->collision,*moving.camera,moving.camera_update->update};
        *movement=std::move(moving);*scene=std::move(registry);
        if(result.body && result.body->conditions && result.body->conditions->after)owners->status=result.body->conditions->after;
        state_.control_word_1384=*inputs.queue_control().control_word;
        primary_loop_->movement_owner=key(movement);primary_loop_->scene_owner=key(scene);
        if(route.status==S::TransitionRequired){
            result.requested_state=route.requested_state;primary_loop_->pending_state_request=route.requested_state;
            return stop(S::TransitionRequired);
        }
        primary_loop_->state0_secondary_completed=true;return stop(S::RetainedState);
    }catch(const std::bad_alloc&){return stop(S::AllocationFailure);}
}
WorldMapPlayerState1TransitionResult WorldMapPlayerStartup::advance_state1_transition(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerState1TransitionInput& input,WorldMapPlayerUpdateOwners* owners) {
    using S=WorldMapPlayerState1TransitionStatus;using A=WorldMapPlayerState0TransitionStatus;
    if(!owners || !model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    const auto control=inputs.queue_control();if(control.status!=WorldMapDispatchStatus::Ready || !control.control_word)return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    const auto current=[&](const auto& receipt){return receipt && receipt->valid && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if(!current(primary_loop_) || !primary_loop_->state0_parent_completed)return {S::RequiresPrimary};
    if(primary_loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs)) ||
        primary_loop_->updates_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owners)))return {S::InvalidInput};
    if(!current(animation_loop_))return {S::RequiresAnimation};
    if(state_.state_1364==1 && primary_loop_->state1_transition_completed)return {S::AlreadyAdvanced};
    if(state_.state_1364!=0 || primary_loop_->pending_state_request!=1)return {S::RequiresPendingTransition};
    WorldMapPlayerState1TransitionResult result;const auto stop=[&](S status){result.status=status;return result;};
    if(!input.saved_byte_14d35)return stop(S::RequiresSavedRemapByte);
    if(!input.secondary_byte_3f1)return stop(S::RequiresSecondaryRemapByte);
    result.remapped_state=*input.saved_byte_14d35?0xda:*input.secondary_byte_3f1?0xee:1;
    if(*result.remapped_state!=1)return stop(S::UnsupportedRemappedState);
    if(inputs.scene().scene_type_68==1){
        if(!input.audio_byte_90)return stop(S::RequiresAudioByte);
        if(*input.audio_byte_90)return stop(S::RequiresAudioStop);
    }
    result.appearance=prepare_state0_appearance(inputs,input.saved_subobject_byte_14c,owners);
    if(result.appearance->status!=A::Prepared)return stop(result.appearance->status==A::AllocationFailure?S::AllocationFailure:S::AppearanceIncomplete);
    result.initializer=prepare_world_map_player_state1_initializer(input.action_148,model_->initial_inputs()->state1_action_mask(),owners->item_owner.has_value());
    if(result.initializer->status!=WorldMapPlayerState1InitializerStatus::Prepared)return stop(S::InitializerIncomplete);
    // All reached work is prepared. The original publishes state/model before
    // the mask store; native publication defers them until this nonallocating tail.
    model_->state_.feature_38=result.appearance->model->after;owners->item_owner->mask_88=*result.initializer->mask;
    state_.state_1364=1;state_.control_word_1384=*control.control_word;state_.secondary_byte_3f0=0;
    primary_loop_->pending_state_request.reset();primary_loop_->state0_secondary_completed=true;primary_loop_->state1_transition_completed=true;
    result.status=S::Advanced;return result;
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state0_update_after_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue) {
    return consume_normal_update_after_third(inputs,input,updates,render_queue,0);
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_state1_update_after_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue) {
    return consume_normal_update_after_third(inputs,input,updates,render_queue,1);
}
WorldMapPlayerStartupQueueResult WorldMapPlayerStartup::consume_normal_update_after_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerStartupQueuedActorInput& input,
    WorldMapPlayerUpdateOwners* updates,WorldMapPlayerActorQueue* render_queue,int32_t expected_state) {
    using S=WorldMapPlayerStartupQueueStatus;
    if(!updates || !model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    if(inputs.queue_control().status!=WorldMapDispatchStatus::Ready)return {S::RequiresControl};
    const auto& clock=model_->game_clock()->state();
    const auto current=[&](const auto& receipt){return receipt && receipt->valid && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if(state_.state_1364!=expected_state || !current(primary_loop_))return {S::RequiresPrimary};
    const auto owner=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs));
    if(primary_loop_->inputs_owner!=owner || (primary_loop_->updates_owner &&
        primary_loop_->updates_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(updates))))return {S::InvalidInput};
    if(!current(animation_loop_))return {S::RequiresAnimation};
    if(!state0_third_loop_ || state0_third_loop_->loop_count!=clock.loop_count || state0_third_loop_->raw_time!=clock.raw_time ||
        state0_third_loop_->inputs_owner!=owner || state0_third_loop_->state!=expected_state)return {S::RequiresThird};
    if(primary_loop_->queued_completed)return {S::AlreadyCompleted};
    auto result=consume_update(inputs,input,updates,render_queue,QueuePrerequisite::NormalThird);
    if(result.status==S::Completed || result.status==S::Empty)primary_loop_->queued_completed=true;
    return result;
}
WorldMapPlayerState0ThirdResult WorldMapPlayerStartup::advance_state0_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerContextUpdateSources& sources,
    const WorldMapPlayerState0ThirdInput& input) noexcept {
    return advance_normal_third(inputs,sources,input,0);
}
WorldMapPlayerState0ThirdResult WorldMapPlayerStartup::advance_state1_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerContextUpdateSources& sources,
    const WorldMapPlayerState0ThirdInput& input) noexcept {
    return advance_normal_third(inputs,sources,input,1);
}
WorldMapPlayerState0ThirdResult WorldMapPlayerStartup::advance_normal_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerContextUpdateSources& sources,
    const WorldMapPlayerState0ThirdInput& input,int32_t expected_state) noexcept {
    using S=WorldMapPlayerState0ThirdStatus;
    if(!model_->game_clock() || inputs.game_clock()!=model_->game_clock())return {S::InvalidInput};
    if(inputs.queue_control().status!=WorldMapDispatchStatus::Ready)return {S::RequiresControl};
    if(state_.state_1364!=expected_state)return {S::RequiresPrimary};
    const auto& clock=model_->game_clock()->state();
    const auto owner=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs));
    const auto current=[&](const auto& receipt){return receipt && receipt->valid &&
        receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;};
    if(!current(primary_loop_))return {S::RequiresPrimary};
    if(expected_state==0 && !primary_loop_->state0_transition_completed && !primary_loop_->state0_parent_completed)return {S::RequiresPrimary};
    if(expected_state==1 && !primary_loop_->state1_transition_completed && !primary_loop_->state1_parent_completed)return {S::RequiresPrimary};
    if(primary_loop_->inputs_owner!=owner)return {S::InvalidInput};
    if(!current(animation_loop_))return {S::RequiresAnimation};
    if(primary_loop_->state0_parent_completed && !primary_loop_->state0_secondary_completed)return {S::RequiresSecondary};
    if(primary_loop_->state1_parent_completed && !primary_loop_->state1_secondary_completed)return {S::RequiresSecondary};
    if(state0_third_loop_ && state0_third_loop_->loop_count==clock.loop_count &&
        state0_third_loop_->raw_time==clock.raw_time){
        if(state0_third_loop_->inputs_owner!=owner || state0_third_loop_->state!=expected_state)return {S::InvalidInput};
        return {S::AlreadyCompleted};
    }
    WorldMapPlayerState0ThirdState next{state_.control_word_1384,state0_context_};
    auto result=prepare_world_map_player_normal_third(inputs,sources,next,input,&next);
    if(result.status!=S::Prepared && result.status!=S::SkippedByGuard && result.status!=S::Filtered)return result;
    state_.control_word_1384=next.control_word_1384;state0_context_=std::move(next.context);
    state0_third_loop_=WorldMapPlayerState0ThirdLoop{clock.loop_count,clock.raw_time,owner,result.status,expected_state};
    if(primary_loop_->state0_parent_completed || primary_loop_->state1_parent_completed)primary_loop_->followups_completed=true;
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
