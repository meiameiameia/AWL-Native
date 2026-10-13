#include "awl/world_map_player_secondary.h"

#include <new>
#include <utility>

namespace awl {
WorldMapAnimationChannelStatus WorldMapPlayerSecondaryOwner::progress(WorldMapAnimationChannelProgressStep* out) {
    return channel_->progress(model_.get(),out);
}

WorldMapPlayerSecondaryResult construct_world_map_player_secondary(
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    uint32_t word_0, uint32_t word_4, uint32_t loop, float rate,
    std::unique_ptr<WorldMapPlayerSecondaryOwner>* out) {
    using S=WorldMapPlayerSecondaryStatus;using C=WorldMapAnimationChannelStatus;
    WorldMapPlayerSecondaryResult result;
    if (!out) return result;
    result.selected_index=(word_4>>11)&127u;
    if (result.selected_index==127) {result.status=S::RequiresSelection;return result;}
    WorldMapPlayerAnimationGroupBinding group;
    const auto bound=bind_world_map_player_animation_group(word_0,assets,&group);
    if (bound==WorldMapPlayerAnimationAssetsStatus::RequiresGroup) {result.status=S::RequiresGroup;return result;}
    if (bound==WorldMapPlayerAnimationAssetsStatus::RequiresAssets) {result.status=S::RequiresAssets;return result;}
    if (bound!=WorldMapPlayerAnimationAssetsStatus::Bound) return result;
    try {
        const auto& models=group.assets->secondary_models();
        WorldMapSecondarySetupStep setup;
        result.setup_status=prepare_world_map_secondary_model_setup(result.selected_index,group.secondary.model_bank_identity,
            &models,0,std::nullopt,{},uint64_t{0},&setup);
        result.required_record=setup.required_record;
        if (*result.setup_status==WorldMapSecondarySetupStatus::InvalidInput) return result;
        if (*result.setup_status!=WorldMapSecondarySetupStatus::RequiresConstruction) {result.status=S::SetupIncomplete;return result;}
        auto owner=std::unique_ptr<WorldMapPlayerSecondaryOwner>(new WorldMapPlayerSecondaryOwner);
        const auto channel=construct_world_map_animation_channel(&owner->channel_);
        if (channel==WorldMapAnimationChannelConstructionStatus::AllocationFailure) {result.status=S::AllocationFailure;return result;}
        if (channel!=WorldMapAnimationChannelConstructionStatus::Constructed) return result;
        result.construction=construct_world_map_secondary_model(models,result.selected_index,setup,&owner->model_);
        if (result.construction->status==WorldMapModelConstructionStatus::AllocationFailure) {result.status=S::AllocationFailure;return result;}
        if (result.construction->status==WorldMapModelConstructionStatus::InvalidInput) return result;
        if (result.construction->status!=WorldMapModelConstructionStatus::Constructed) {result.status=S::ConstructionIncomplete;return result;}
        WorldMapAnimationPartialChannelStep prepared;
        const auto observe_channel=[&](C status) {
            result.channel_status=status;result.required_record=prepared.required_record;result.required_fields=prepared.required_fields;
            if (status==C::AllocationFailure) result.status=S::AllocationFailure;
            else result.status=status==C::InvalidInput?S::InvalidInput:S::ChannelIncomplete;
        };
        const auto setup_channel=owner->channel_->setup_secondary(owner->model_.get(),word_4,
            group.secondary_animation_bank_identity,&group.assets->secondary_animations(),&prepared);
        observe_channel(setup_channel);
        if (setup_channel!=C::Advanced) return result;
        result.applying_settings=true;
        const auto settings=owner->channel_->apply_settings(owner->model_.get(),loop,rate,&prepared);
        observe_channel(settings);
        if (settings!=C::Advanced) return result;
        owner->selected_index_=result.selected_index;
        *out=std::move(owner);result.status=S::ConstructedCpuSecondary;return result;
    } catch (const std::bad_alloc&) {result.status=S::AllocationFailure;return result;}
}
} // namespace awl
