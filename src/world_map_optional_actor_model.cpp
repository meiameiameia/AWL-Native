#include "awl/world_map_optional_actor_model.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include "world_map_dol_view.h"
#include <cmath>
#include <cstring>
#include <new>

namespace awl {
namespace {using S=WorldMapOptionalActorModelStatus;}
S WorldMapOptionalActorModelTables::load(std::shared_ptr<const WorldMapOptionalActorModelTables>* out) {
    if(!out)return S::InvalidInput;if(*out)return S::Loaded;
    try {
        void* data=nullptr;size_t size=0;
        if(!filesystem_read_entire_file("/sys/main.dol",&data,&size))return S::ReadFailure;
        const std::unique_ptr<void,void(*)(void*)> guard(data,filesystem_free_file_data);
        Sha1 sha;if(size>detail::max_dol_size || !sha1_bytes(static_cast<const uint8_t*>(data),size,&sha))return S::ReadFailure;
        if(sha!=kTargetDolSha1)return S::WrongDol;
        detail::DolView dol;if(!dol.initialize(static_cast<const uint8_t*>(data),size))return S::UnsupportedLayout;
        auto next=std::shared_ptr<WorldMapOptionalActorModelTables>(new WorldMapOptionalActorModelTables);
        constexpr uint32_t descriptors[]{0x8024131c,0x8024143c,0x80241fd0,0x80242000,0x802420f0,0x80242078};
        for(size_t i=0;i<6;++i){const auto* p=dol.read(descriptors[i],12);if(!p)return S::UnsupportedLayout;
            next->descriptors_[i]={descriptors[i],detail::dol_word(p),detail::dol_word(p+4),detail::dol_word(p+8)};
            if(next->descriptors_[i].word_0>>25!=(i<2?66u:65u))return S::UnsupportedLayout;}
        for(uint32_t i=0;i<5;++i){const bool a=i>=2;
            const auto* pointer=dol.read((a?0x8029fc10u:0x8029fbf0u)+(i+0x27)*4,4);if(!pointer)return S::UnsupportedLayout;
            const auto base=detail::dol_word(pointer);
            for(uint32_t choice=0;choice<2;++choice){const auto* p=dol.read(base+choice*4*(a?8:4),a?8:4);if(!p)return S::UnsupportedLayout;
                for(uint32_t j=0;j<(a?2u:1u);++j){const auto word=detail::dol_word(p+j*4);std::memcpy(&next->appearance_[i][choice][j],&word,4);}}
        }
        *out=std::move(next);return S::Loaded;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
bool WorldMapOptionalActorModelTables::descriptor(bool a,uint32_t clip,uint32_t variant,WorldMapActorAnimationDescriptor* out) const noexcept {
    if(!out)return false;size_t index=6;
    if(a){if(clip==0 && variant==0)index=0;else if(clip==10 && variant==1)index=1;}
    else if(variant==0){if(clip==0)index=2;else if(clip==2)index=3;else if(clip==11)index=4;else if(clip==6)index=5;}
    if(index==6)return false;*out=descriptors_[index];return true;
}
bool WorldMapOptionalActorModelTables::appearance(int32_t id,uint32_t choice,std::array<int32_t,2>* out) const noexcept {
    if(!out || id<0x27 || id>0x2b || (choice!=0 && choice!=4))return false;
    *out=appearance_[id-0x27][choice==4?1:0];return true;
}
S WorldMapOptionalActorModel::apply_appearance(const WorldMapOptionalActorModelTables& tables,uint32_t choice) {
    std::array<int32_t,2> types;if(!tables.appearance(actor_id_,choice,&types))return S::UnsupportedCommand;
    // FA0C/F8CC reset reached common timers even if the choice is unchanged.
    for(size_t i=0;i<(actor_id_>=0x29?2u:1u);++i)if(types[i]>=20){
        if(!clock_)return S::RequiresClock;
        WorldMapPlayerTimerResourceView resource;
        if(!timer_assets_ || !timer_assets_->resource(i?WorldMapPlayerTimerBank::Mouth:WorldMapPlayerTimerBank::Eyes,
            uint32_t(types[i]-20),0,&resource))return S::RequiresTimer;
        timers_[i]=WorldMapAnimationFeatureTimer{resource.identity,clock_->state().raw_time,0,1};
    }
    appearance_types_=types;appearance_choice_=choice;return S::Constructed;
}
WorldMapOptionalActorModelResult WorldMapOptionalActorModel::animate(const WorldMapOptionalActorModelTables& tables,
    const WorldMapOptionalModelCommand& command) {
    WorldMapActorAnimationDescriptor descriptor;
    if(!tables.descriptor(actor_id_>=0x29,command.index,command.variant,&descriptor) || !std::isfinite(command.rate))return {S::UnsupportedCommand};
    WorldMapPlayerAnimationGroupBinding group;
    if(bind_world_map_player_animation_group(descriptor.word_0,holder_->animations_,&group)!=WorldMapPlayerAnimationAssetsStatus::Bound)return {S::AnimationIncomplete};
    WorldMapPlayerAnimationHolder::StartPreparation staged;holder_->seed_start(staged);
    // C870/D660 equal-base skips DB28; D74C still follows and updates speed.
    if(staged.state.base_descriptor_4!=descriptor.identity){
        holder_->stage_initialization(staged.initialization);
        WorldMapAnimationInitializerObservations observations;if(clock_)observations.clock=clock_->state().raw_time;
        WorldMapPartialAnimationInitializerStep prepared;WorldMapAnimationInitializerStatus status;
        const auto initialized=holder_->prepare_owned_initializer(staged.state,descriptor,group,observations,false,staged.initialization,&prepared,status);
        if(initialized==WorldMapNativeAnimationInitializerStatus::AllocationFailure)return {S::AllocationFailure,{},status};
        if(initialized!=WorldMapNativeAnimationInitializerStatus::Advanced)return {S::AnimationIncomplete,{},status};
        staged.initialization_staged=true;staged.descriptor=descriptor;
    }
    if(holder_->prepare_speed(staged,command.rate)!=WorldMapAnimationChannelStatus::Prepared)return {S::AnimationIncomplete};
    holder_->publish_start(staged);return {S::Constructed};
}
WorldMapOptionalActorModelResult WorldMapOptionalActorModel::create(
    const std::shared_ptr<const WorldMapOptionalActorModelTables>& tables,const WorldMapOptionalActionPlan& action,
    float scale,const std::shared_ptr<GameClock>& clock,std::unique_ptr<WorldMapOptionalActorModel>* out) {
    if(!out || !action.actor_identity || action.actor_id<0x27 || action.actor_id>0x2b ||
        (action.kind!=WorldMapOptionalActionKind::AExplicit && action.kind!=WorldMapOptionalActionKind::BExplicit && action.kind!=WorldMapOptionalActionKind::BTable) ||
        (action.kind==WorldMapOptionalActionKind::AExplicit)!=(action.actor_id>=0x29) ||
        action.model_command_count>action.model_commands.size() || !std::isfinite(scale) || scale<=0)return {};
    if(!tables)return {S::RequiresTables};
    try {
        auto next=std::unique_ptr<WorldMapOptionalActorModel>(new WorldMapOptionalActorModel);
        next->actor_id_=action.actor_id;next->actor_identity_=action.actor_identity;next->clock_=clock;
        const auto loaded=load_world_map_optional_model_assets(action.actor_id,&next->assets_);
        if(loaded!=WorldMapPlayerModelAssetsStatus::Loaded)return {loaded==WorldMapPlayerModelAssetsStatus::AllocationFailure?S::AllocationFailure:S::ModelIncomplete};
        const auto timer=load_world_map_player_timer_assets(&next->timer_assets_);
        if(timer!=WorldMapPlayerTimerAssetsStatus::Loaded)return {timer==WorldMapPlayerTimerAssetsStatus::AllocationFailure?S::AllocationFailure:S::RequiresTimer};
        auto appearance=next->apply_appearance(*tables,0);if(appearance!=S::Constructed)return {appearance};
        std::shared_ptr<const WorldMapPlayerAnimationAssets> animations;
        const auto animation=load_world_map_optional_animation_assets(action.actor_id>=0x29?66:65,&animations);
        if(animation!=WorldMapPlayerAnimationAssetsStatus::Loaded)return {animation==WorldMapPlayerAnimationAssetsStatus::AllocationFailure?S::AllocationFailure:S::AnimationIncomplete};
        WorldMapActorAnimationDescriptor initial;if(!tables->descriptor(action.actor_id>=0x29,0,0,&initial))return {S::UnsupportedLayout};
        WorldMapAnimationInitializerObservations observations;if(clock)observations.clock=clock->state().raw_time;
        const auto constructed=construct_world_map_player_animation_holder(next->assets_,animations,initial,std::nullopt,observations,&next->holder_);
        if(constructed.status!=WorldMapPlayerAnimationHolderStatus::ConstructedCpuHolder)return {
            constructed.status==WorldMapPlayerAnimationHolderStatus::AllocationFailure?S::AllocationFailure:S::ModelIncomplete,constructed};
        if(!next->holder_->primary_->initialize_root_scale(scale))return {S::ModelIncomplete};
        for(uint32_t i=0;i<action.model_command_count;++i){const auto& command=action.model_commands[i];
            WorldMapOptionalActorModelResult result{S::Constructed};
            switch(command.operation){
            case WorldMapOptionalModelOperation::Appearance:result.status=next->apply_appearance(*tables,command.index);break;
            case WorldMapOptionalModelOperation::Animation:result=next->animate(*tables,command);break;
            case WorldMapOptionalModelOperation::BindSecondary:next->secondary_binding_=next->holder_->primary().model().binding().model_identity;break;
            case WorldMapOptionalModelOperation::SetFlag15c:next->flag_15c_=static_cast<uint8_t>(command.index);break;
            default:result.status=S::UnsupportedCommand;break;
            }
            if(result.status!=S::Constructed){result.command=i;return result;}
        }
        *out=std::move(next);return {S::Constructed};
    }catch(const std::bad_alloc&){return {S::AllocationFailure};}
}
} // namespace awl
