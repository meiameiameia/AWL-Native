#include "awl/world_map_player_context_update.h"
#include <utility>

namespace awl {
bool world_map_player_context_region(const WorldMapPlayerContextSearchProfile& profile,int32_t scene,
    const WorldMapPlayerContextVector& p) noexcept {
    // bge tests !LT, including unordered comparisons. Preserve the original
    // NaN behavior and X-before-Z short circuit instead of replacing !LT by >=.
    return scene==1 && !(p[0]<profile.region_xz[0] || profile.region_xz[1]<p[0]) &&
        !(p[2]<profile.region_xz[2] || profile.region_xz[3]<p[2]);
}
WorldMapPlayerContextUpdateStep prepare_world_map_player_context_update(
    const WorldMapPlayerContextUpdateSources& sources,const WorldMapPlayerContextUpdateState& input,
    const WorldMapPlayerContextUpdateInput& observations,WorldMapPlayerContextUpdateState* out) noexcept {
    using S=WorldMapPlayerContextUpdateStatus;WorldMapPlayerContextUpdateStep step;if(!out)return step;
    if(observations.held_item<0)return step;
    if(!observations.held_row){step.status=S::RequiresHeldRow;return step;}
    if(observations.held_row->item_id!=observations.held_item)return step;
    if(!observations.held_row->type_0){step.status=S::RequiresHeldType;return step;}
    if(!sources.queries || !sources.queries->search_profile()){step.status=S::RequiresSearchProfile;return step;}
    const auto& profile=*sources.queries->search_profile();const uint8_t type=*observations.held_row->type_0;
    const bool first=type==1 || type==2 || type==3 || type==9 || type==16;
    WorldMapPlayerContextQuery query;query.current_key=observations.current_key;query.origin=observations.position;
    query.forward=observations.facing;query.angle_radians=profile.angle_radians;
    query.min_distance=first?profile.first_min_distance:profile.other_min_distance;
    query.max_distance=first?profile.first_max_distance:profile.other_max_distance;
    query.excluded_kinds=first?(type==2?0xfe6u:0xff6u):0x18u;
    WorldMapPlayerContextResult target;
    for(uint32_t pass=0;pass<2;++pass){
        step.query_masks[pass]=query.excluded_kinds;++step.query_count;
        step.query=query_world_map_player_scene_context(sources.queries,sources.registry,query,&target);
        if(step.query->status!=WorldMapPlayerContextStatus::Prepared){step.status=S::QueryIncomplete;return step;}
        if(target.found || pass==1)break;
        query.min_distance=profile.other_min_distance;query.max_distance=profile.other_max_distance;
        query.excluded_kinds=first?0x18u:0x10u;
    }
    if(!target.found && observations.action!=0)target.record.owner_code=static_cast<uint32_t>(observations.action);
    step.target=target;
    if(!observations.tracked_14ac){step.status=S::RequiresTrackedTarget;return step;}
    if(*observations.tracked_14ac){
        if(!observations.tracked_comparison[0] || !observations.tracked_comparison[1]){step.status=S::RequiresTrackedComparison;return step;}
        if(*observations.tracked_comparison[0]!=*observations.tracked_comparison[1] && target.record.payload[1]!=UINT32_MAX){
            step.requested_state=0x99;step.transition_context_138c=2;step.status=S::RequiresTransition;return step;
        }
    }
    auto label_input=observations.label_observations;
    label_input.source=observations.action?WorldMapPlayerContextLabelSource::Action:WorldMapPlayerContextLabelSource::HeldItem;
    label_input.scene_type=observations.scene_type;label_input.position=observations.position;label_input.facing=observations.facing;
    label_input.value=observations.action?observations.action:observations.held_item;label_input.query_mode=0;
    if(!observations.action)label_input.item_row=observations.held_row;
    WorldMapPlayerContextLabels labels;
    step.labels=prepare_world_map_player_context_labels_from_target(sources.labels,target,label_input,&labels);
    if(step.labels->status!=WorldMapPlayerContextLabelStatus::Prepared){step.status=S::LabelsIncomplete;return step;}
    auto next=input;next.secondary_byte_574=0;
    if(observations.scene_type==1){
        if(!observations.scene_predicate_1728e4){step.status=S::RequiresScenePredicate;return step;}
        if(*observations.scene_predicate_1728e4){next.secondary_byte_574=1;if(type==4)labels.labels[0]=0x30;}
    }
    if(!target.found && observations.scene_type==1){
        if(!observations.secondary_byte_3f1){step.status=S::RequiresSecondaryByte;return step;}
        // Original 1CF3C precedes the held/action-zero tests. Its complete
        // predicate is pure, so no missing observation is bypassed here.
        if(!*observations.secondary_byte_3f1 && world_map_player_context_region(profile,observations.scene_type,observations.position) &&
            observations.held_item==0 && observations.action==0)labels.labels[0]=0x31;
    }
    next.labels_128={labels.labels[0],0,labels.labels[2],labels.labels[1]};
    step.icons=prepare_world_map_player_context_icons(sources.labels,next.ui,labels,&next.ui,sources.icons);
    if(step.icons->status!=WorldMapPlayerContextIconStatus::Prepared){step.status=S::IconsIncomplete;return step;}
    next.byte_1350=0;
    // cmpwi is signed; wrapping/signed high-bit owner codes are outside.
    if(target.found && target.record.owner_code>=0x1a5 && target.record.owner_code<=0x277){
        step.requested_action_kind=0x2d;step.status=S::RequiresContextAction;return step;
    }
    next.secondary_word_564=target.found?target.record.payload[0]:0x35;
    next.target_1388={target.record.owner_code,target.record.payload[0],target.record.payload[1]};
    *out=std::move(next);step.status=S::Prepared;return step;
}
} // namespace awl
