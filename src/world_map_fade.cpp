#include "awl/world_map_fade.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <new>
#include <utility>

namespace awl {
WorldMapFadeStatus prepare_world_map_fade_start(const WorldMapFadeState& before,
    float seconds,const WorldMapFadeColor& from,const WorldMapFadeColor& to,
    uint8_t hold,uint8_t front,std::optional<uint32_t> now,WorldMapFadeState* out) noexcept {
    using S=WorldMapFadeStatus;
    if (!out || !std::isfinite(seconds) || seconds<0) return S::InvalidInput;
    if (std::fegetround()!=FE_TONEAREST) return S::UnsupportedNumerics;
    const float duration=1000.0f*seconds;
    if (!std::isfinite(duration)) return S::InvalidInput;
    if (!now) return S::RequiresClock;
    auto next=before;next.from_4c=from;next.to_50=to;next.duration_58=duration;
    next.started_5c=*now;next.mode_60=1;next.hold_64=hold;
    next.render_priority_68=front?0xdfff:0xcffe;next.registered=true;
    *out=next;return S::Prepared;
}
WorldMapFadeStatus prepare_world_map_fade_update(const WorldMapFadeState& before,
    std::optional<uint32_t> now,WorldMapFadeStep* out) noexcept {
    using S=WorldMapFadeStatus;
    if (!out) return S::InvalidInput;
    if (before.mode_60==0) {*out={before,false};return S::Idle;}
    if (before.mode_60!=1 && before.mode_60!=2) return S::UnsupportedMode;
    if (std::fegetround()!=FE_TONEAREST) return S::UnsupportedNumerics;
    auto next=before;float fraction=1.0f;
    if (before.mode_60==1) {
        if (!std::isfinite(before.duration_58) || before.duration_58<0) return S::InvalidInput;
        // Zero duration takes the original 1.1 branch and never reads +5C.
        if (before.duration_58==0) fraction=1.1f;
        else {
            if (!now) return S::RequiresClock;
            if (!before.started_5c) return S::RequiresStartTime;
            const float elapsed=static_cast<float>(*now-*before.started_5c);
            fraction=elapsed/before.duration_58;
        }
        if (fraction>1.0f) {
            if (!before.hold_64) return S::RequiresHold;
            next.mode_60=*before.hold_64?2:0;
            if (next.mode_60==0) next.registered=false; // 9508 -> 9904 removes primary node
        }
    }
    fraction=std::min(fraction,1.0f);
    const float remaining=1.0f-fraction;
    WorldMapFadeColor color;
    for(size_t i=0;i<color.size();++i) {
        const float first=static_cast<float>(before.from_4c[i])*remaining;
        const float second=static_cast<float>(before.to_50[i])*fraction;
        const float mixed=first+second; // separate fmuls/fmuls/fadds; no FMA
        color[i]=static_cast<uint8_t>(static_cast<uint32_t>(mixed));
    }
    next.color_54=color;*out={next,true};return S::Prepared;
}
WorldMapFadeStatus WorldMapFade::start(float seconds,const WorldMapFadeColor& from,
    const WorldMapFadeColor& to,uint8_t hold,uint8_t front) noexcept {
    using S=WorldMapFadeStatus;
    WorldMapFadeState next;
    const auto status=prepare_world_map_fade_start(state_,seconds,from,to,hold,front,
        clock_?std::optional<uint32_t>{clock_->state().raw_time}:std::nullopt,&next);
    if (status!=S::Prepared) return status;
    state_=next;
    if (loop_) loop_->valid=false;
    return S::Started;
}
WorldMapFadeStatus WorldMapFade::advance_for_loop(
    const WorldMapUpdateInputs& inputs,WorldMapPlayerActorQueue* render_queue) {
    using S=WorldMapFadeStatus;
    if (!clock_ || inputs.game_clock()!=clock_) return S::InvalidInput;
    const auto clock=clock_->state();
    const bool same=loop_ && loop_->loop_count==clock.loop_count && loop_->raw_time==clock.raw_time;
    if (!state_.registered && !same) return S::Idle;
    const auto control=inputs.queue_control();
    if (control.status!=WorldMapDispatchStatus::Ready) return S::RequiresControl;
    if (same) {
        if (!loop_->valid) return S::RequiresNextLoop;
        if (loop_->inputs_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs))) return S::InvalidInput;
        if (render_queue && loop_->render_owner!=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(render_queue))) return S::InvalidInput;
        return S::AlreadyAdvanced;
    }
    const auto dispatch=inputs.dispatch(0x10000);
    if (dispatch.status==WorldMapDispatchStatus::Filtered) return S::Filtered;
    if (dispatch.status!=WorldMapDispatchStatus::Ready) return S::InvalidInput;
    WorldMapFadeStep step;
    const auto status=prepare_world_map_fade_update(state_,clock.raw_time,&step);
    if (status!=S::Prepared && status!=S::Idle) return status;
    if (!step.schedule_render) return S::Idle;
    if (!render_queue) return S::RequiresRenderQueue;
    const auto key=render_node_identity();
    for(size_t i=0;i<render_queue->size();++i) {
        const auto& entry=(*render_queue)[i];
        if (!entry.node_identity || (i && (*render_queue)[i-1].priority>entry.priority)) return S::InvalidQueue;
        for(size_t j=0;j<i;++j) if ((*render_queue)[j].node_identity==entry.node_identity) return S::InvalidQueue;
        if (entry.node_identity==key && entry.priority!=step.after.render_priority_68) return S::UnsupportedPriorityChange;
    }
    try {
        auto queue=*render_queue;
        const auto existing=std::find_if(queue.begin(),queue.end(),[key](const auto& entry){return entry.node_identity==key;});
        if (existing!=queue.end()) queue.erase(existing);
        const auto at=std::lower_bound(queue.begin(),queue.end(),step.after.render_priority_68,
            [](const auto& entry,uint32_t priority){return entry.priority<priority;});
        queue.insert(at,{key,step.after.render_priority_68});
        *render_queue=std::move(queue);state_=step.after;
        loop_=WorldMapFadeLoop{clock.loop_count,clock.raw_time,true,static_cast<uint64_t>(reinterpret_cast<uintptr_t>(render_queue))};
        loop_->inputs_owner=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&inputs));
        return S::Advanced;
    } catch(const std::bad_alloc&) {return S::AllocationFailure;}
}
} // namespace awl
