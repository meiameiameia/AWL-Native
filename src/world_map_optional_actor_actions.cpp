#include "awl/world_map_optional_actor_actions.h"
#include "awl/game_rng.h"
#include <cfenv>
#include <cmath>

namespace awl {
bool prepare_world_map_optional_b_weighted_selection(const WorldMapOptionalBWeightedRow& row,
    uint32_t upper,uint32_t seed,WorldMapOptionalBWeightedSelection* out) noexcept {
    if(!out || !row.count || row.count>12 || upper==UINT32_MAX)return false;
    const uint32_t word=advance_game_rng(&seed)%(upper+1u);uint32_t sum=0,index=row.count-1;
    const auto signed_greater=[](uint32_t a,uint32_t b){return (a^0x80000000u)>(b^0x80000000u);};
    for(uint32_t i=0;i<row.count;++i)if(row.weights[i]){sum+=row.weights[i];if(signed_greater(sum,word)){index=i;break;}}
    const auto& choice=row.actions[index];*out={choice[0],choice[1],choice[2],seed,index};return true;
}
WorldMapOptionalActionStatus prepare_world_map_optional_actor_action(const WorldMapOptionalActionTables& tables,
    const WorldMapOptionalActionQuery& q,WorldMapOptionalActionPlan* out) noexcept {
    using S=WorldMapOptionalActionStatus;
    if(!out)return S::InvalidInput;
    const bool a=q.kind==WorldMapOptionalActionKind::AExplicit;
    if(!a && q.kind!=WorldMapOptionalActionKind::BExplicit && q.kind!=WorldMapOptionalActionKind::BTable)return S::InvalidInput;
    WorldMapOptionalActionPlan next;next.kind=q.kind;next.actor_id=q.actor_id;next.after=q.before;
    if(q.requested==(a?0x23:0x2b)){*out=next;return S::Prepared;}
    if(q.requested<0 || q.requested>=(a?34:42))return S::UnsupportedAction;
    uint32_t lo=q.minimum,hi=q.maximum;
    if(q.kind==WorldMapOptionalActionKind::BTable){if(!tables.b_duration_known[q.requested])return S::UnsupportedAction;
        lo=tables.b_durations[q.requested][0];hi=tables.b_durations[q.requested][1];}
    if(lo>hi || hi-lo==UINT32_MAX)return S::InvalidInput;
    if(q.kind==WorldMapOptionalActionKind::BTable){
        if(!q.rng_before)return S::RequiresRng;if(!q.phase_raw)return S::RequiresPhase;
    }else {if(!q.phase_raw)return S::RequiresPhase;if(!q.rng_before)return S::RequiresRng;}
    auto seed=*q.rng_before;const auto value=advance_game_rng(&seed);
    next.rng_after=seed;next.after.deadline=*q.phase_raw+(lo+value%(hi-lo+1u))*600u;
    next.after.previous=q.before.current;next.after.current=q.requested;
    if(q.kind!=WorldMapOptionalActionKind::BTable)next.after.restart_latch=0;
    if(a && q.before.current==q.requested){*out=next;return S::Prepared;}
    const auto target=a?tables.a_initializers[q.requested]:tables.b_initializers[q.requested];
    const uint32_t expected=a?(q.requested==0x12?0x80162918u:q.requested==0x13?0x80162ad0u:q.requested==0x14?0x80162b10u:q.requested==0x1d?0x8016312cu:0u):
        (q.requested==2?0x80168bb0u:q.requested==0x11?0x80169e7cu:q.requested==0x25?0x8016b834u:0u);
    if(!expected || target!=expected)return S::UnsupportedInitializer;
    next.initialized=true;
    if(a && q.requested==0x1d){*out=next;return S::Prepared;} // Verified leaf is exactly blr.
    if(a && q.requested==0x13){next.collision_enabled=false;next.bind_secondary_model=true;
        next.model_commands[0]={WorldMapOptionalModelOperation::BindSecondary,0,0,0};
        next.model_commands[1]={WorldMapOptionalModelOperation::SetFlag15c,1,0,0};next.model_command_count=2;
    }else {
        if(q.actor_id<(a?0x29:0x27) || q.actor_id>(a?0x2b:0x28))return S::InvalidInput;
        if(((a && q.requested==0x12) || (!a && q.requested==0x11)) && !q.held_scene_type)return S::RequiresHeldType;
        if(a && q.requested==0x14){if(!q.held_position)return S::RequiresHeldPosition;
            for(float f:*q.held_position)if(!std::isfinite(f))return S::InvalidInput;
            next.record_position=q.held_position;}
        if(std::fegetround()!=FE_TONEAREST)return S::UnsupportedNumerics;
        const std::size_t index=static_cast<std::size_t>(q.actor_id-0x27);const float speed=tables.speeds[index];
        volatile float denominator=tables.rate_divisors[a?0:1]*tables.model_scales[index];
        if(!std::isfinite(speed) || !std::isfinite(denominator) || denominator<=0)return S::UnsupportedNumerics;
        const float rate=speed/denominator;
        if(!std::isfinite(rate))return S::UnsupportedNumerics;
        next.record_enabled=true;
        next.requested_speed=speed;
        next.model_commands[0]={WorldMapOptionalModelOperation::Appearance,(a || q.requested==0x25)?4u:0u,0,0};
        next.model_commands[1]={WorldMapOptionalModelOperation::Animation,a?0xau:q.requested==2?2u:q.requested==0x11?0xbu:6u,a?1u:0u,rate};
        next.model_command_count=2;
        if(a && q.requested==0x14){next.model_commands[0]=next.model_commands[1];next.model_command_count=1;}
        if((a && q.requested==0x12) || (!a && q.requested==0x11)){next.internal_scene_type=q.held_scene_type;next.collision_enabled=false;}
        if(!a && q.requested==0x25){next.special_flag_one=true;next.clear_accumulator=true;
            next.heading_degrees=0; // supported forward (0,0,1), original 16055C
            next.position=tables.night_position;next.heading=tables.forward;next.facing_threshold=tables.night_threshold;
            for(float f:tables.night_position)if(!std::isfinite(f))return S::UnsupportedNumerics;
            if(tables.forward!=std::array<float,3>{0,0,1} || !std::isfinite(tables.night_threshold))return S::UnsupportedNumerics;
        }
    }
    *out=next;return S::Prepared;
}
} // namespace awl
