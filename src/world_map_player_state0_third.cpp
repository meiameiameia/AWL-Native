#include "awl/world_map_player_state0_third.h"
#include <utility>

namespace awl {
WorldMapPlayerState0ThirdResult prepare_world_map_player_state0_third(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerContextUpdateSources& sources,
    const WorldMapPlayerState0ThirdState& input,const WorldMapPlayerState0ThirdInput& observations,
    WorldMapPlayerState0ThirdState* out) noexcept {
    using S=WorldMapPlayerState0ThirdStatus;WorldMapPlayerState0ThirdResult result;if(!out)return result;
    const auto dispatch=inputs.dispatch(0x5001);
    if(dispatch.status==WorldMapDispatchStatus::RequiresControl){result.status=S::RequiresControl;return result;}
    if(dispatch.status==WorldMapDispatchStatus::Filtered){*out=input;result.status=S::Filtered;return result;}
    if(dispatch.status!=WorldMapDispatchStatus::Ready || !dispatch.control_word)return result;
    if(!observations.guards){result.status=S::RequiresGuards;return result;}
    auto next=input;next.control_word_1384=*dispatch.control_word;
    bool skip=observations.guards->word_680!=-1;
    if(!skip){if(!observations.guards->word_58c){result.status=S::RequiresGuardWord58c;return result;}
        skip=*observations.guards->word_58c!=0;}
    if(skip || (*dispatch.control_word&0x800u)){*out=std::move(next);result.status=S::SkippedByGuard;return result;}
    if(!observations.context){result.status=S::RequiresContextInput;return result;}
    const auto& context=*observations.context;
    if(inputs.scene().scene_type_68==1 && context.action!=0 && context.action!=2){
        // 1CD18's scene != 1 return does not read any bound constants.
        if(context.scene_type!=1){result.status=S::RequiresPrefixAction;return result;}
        if(!sources.queries || !sources.queries->search_profile() || !sources.queries->search_profile()->prefix_region_xz){
            result.status=S::RequiresPrefixProfile;return result;}
        auto profile=*sources.queries->search_profile();profile.region_xz=*profile.prefix_region_xz;
        if(!world_map_player_context_region(profile,context.scene_type,context.position)){result.status=S::RequiresPrefixAction;return result;}
    }
    if(!next.context){result.status=S::RequiresContextState;return result;}
    auto bound=sources;
    WorldMapPlayerContextIconSources icons;
    if(sources.icons){icons=*sources.icons;icons.clock=inputs.game_clock();bound.icons=&icons;}
    result.context=prepare_world_map_player_context_update(bound,*next.context,context,&*next.context);
    if(result.context->status!=WorldMapPlayerContextUpdateStatus::Prepared){result.status=S::ContextIncomplete;return result;}
    const auto* pad=inputs.pad_for_loop();if(!pad){result.status=S::RequiresPad;return result;}
    result.pressed=pad->frame.pressed;
    if(pad->frame.pressed&0x100u){result.required_button=0x100;result.status=S::RequiresButtonAction;return result;}
    if(pad->frame.pressed&0x800u){result.required_button=0x800;result.status=S::RequiresButtonAction;return result;}
    *out=std::move(next);result.status=S::Prepared;return result;
}
} // namespace awl
