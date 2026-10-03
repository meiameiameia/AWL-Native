#include "awl/world_map_model_feature.h"

#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
WorldMapModelMatrix identity_matrix() { return {1,0,0,0,0,1,0,0,0,0,1,0}; }
}
WorldMapModelFeatureStatus prepare_world_map_held_item_feature(
    const WorldMapModelFeatureObjectState& state,int32_t item_id,
    const WorldMapHeldItemFeatureObservations& observations,WorldMapHeldItemFeatureStep* out) {
    using Status=WorldMapModelFeatureStatus;
    if(out==nullptr || &state==&out->after || item_id<0 || uint32_t(item_id)>UINT32_MAX/0x24u ||
        state.cache_48.size()>UINT32_MAX/8u)return Status::InvalidInput;
    WorldMapHeldItemFeatureStep step;step.after=state;
    auto stop=[&](Status status){step.after=state;step.writes.clear();*out=std::move(step);return status;};
    uint64_t resource=0,metadata=0;
    if(item_id!=0) {
        if(!observations.row)return stop(Status::RequiresRow);
        const auto& row=*observations.row;if(row.item_id!=item_id)return Status::InvalidInput;
        WorldMapHeldItemFeatureRoute route;
        const bool split=row.type_0==1 || row.type_0==2 || row.type_0==4 || item_id==0x4ff;
        if(split) {
            if(!observations.null_item_alternate_group)return stop(Status::RequiresBaseline);
            if(row.alternate_group_3!=*observations.null_item_alternate_group)route.bank=WorldMapHeldItemFeatureBank::Alternate;
        }
        const bool alternate=route.bank==WorldMapHeldItemFeatureBank::Alternate;
        route.group=alternate?row.alternate_group_3:row.group_2;
        route.textures=alternate?std::array<uint16_t,2>{row.alternate_texture_8,row.alternate_texture_a}:
            std::array<uint16_t,2>{row.texture_4,row.texture_6};step.route=route;
        if(!observations.resource)return stop(Status::RequiresResource);
        const auto& binding=*observations.resource;
        if(binding.bank!=route.bank || binding.group!=route.group)return Status::InvalidInput;
        resource=binding.resource_0;metadata=binding.metadata_44;
        // 12030 computes each texture reference before scanning any commands.
        if(!observations.texture_bank_identity)return stop(Status::RequiresTextureBank);
        if(*observations.texture_bank_identity==0)return Status::InvalidInput;
        // 2B74 reads metadata +8 even for zero commands. Null is unsafe.
        if(metadata==0)return Status::InvalidInput;
        if(!binding.commands)return stop(Status::RequiresCommands);
        if(binding.commands->size()>UINT32_MAX/0x14u)return Status::InvalidInput;
    }
    auto store=[&](uint32_t offset,uint64_t value){step.writes.push_back({offset,value,std::nullopt,std::nullopt});};
    step.after.resource_0=resource;store(0,resource);
    step.after.buffer_34=0;store(0x34,0);
    step.after.matrix_4=identity_matrix();
    // 7D0C uses paired stores at matrix +8,+18,+20,+10,+0,+28.
    for(uint32_t offset:{0xcu,0x10u,0x1cu,0x20u,0x24u,0x28u,0x14u,0x18u,0x4u,0x8u,0x2cu,0x30u}) {
        const auto index=(offset-4u)/4u;
        store(offset,step.after.matrix_4[index]==1?0x3f800000u:0u);
    }
    step.after.enabled_3c=1;store(0x3c,1);step.after.byte_3d=0;store(0x3d,0);
    step.after.metadata_44=metadata;store(0x44,metadata);
    if(step.route) {
        for(uint32_t channel=0;channel<2;++channel) {
            const WorldMapModelFeatureTexture texture{*observations.texture_bank_identity,step.route->textures[channel]};
            for(const auto& command:*observations.resource->commands) {
                if(command.type_0!=1 || command.channel_1!=channel)continue;
                size_t selected=step.after.cache_48.size();
                for(size_t i=0;i<step.after.cache_48.size();++i)if(step.after.cache_48[i].channel==channel){selected=i;break;}
                if(selected==step.after.cache_48.size())for(size_t i=0;i<step.after.cache_48.size();++i)
                    if(step.after.cache_48[i].channel==UINT32_MAX){selected=i;break;}
                if(selected==step.after.cache_48.size())continue;
                auto& entry=step.after.cache_48[selected];entry.channel=channel;entry.texture=texture;
                step.writes.push_back({0,channel,static_cast<uint32_t>(selected),std::nullopt});
                step.writes.push_back({4,0,static_cast<uint32_t>(selected),texture});
            }
        }
    }
    *out=std::move(step);return Status::Prepared;
}
WorldMapNativeModelFeature::WorldMapNativeModelFeature(WorldMapModelFeatureObjectState state) noexcept:state_(std::move(state)) {}
uint64_t WorldMapNativeModelFeature::identity() const noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)); }
std::optional<WorldMapModelSourceChange> WorldMapNativeModelFeature::attachment_source() const noexcept {
    if(state_.resource_0==0)return std::nullopt;
    return WorldMapModelSourceChange{WorldMapModelSourceKind::Feature,identity()};
}
WorldMapModelFeatureStatus construct_world_map_native_model_feature(
    uint32_t capacity,std::unique_ptr<WorldMapNativeModelFeature>* out) {
    using Status=WorldMapModelFeatureStatus;
    if(out==nullptr || capacity>(UINT32_MAX-0x50u)/8u)return Status::InvalidInput;
    try {
        WorldMapModelFeatureObjectState state;state.matrix_4=identity_matrix();state.cache_48.resize(capacity);
        auto feature=std::unique_ptr<WorldMapNativeModelFeature>(new WorldMapNativeModelFeature(std::move(state)));
        *out=std::move(feature);return Status::Constructed;
    } catch(const std::bad_alloc&) {return Status::AllocationFailure;}
}
WorldMapModelFeatureStatus advance_world_map_native_held_item_feature(
    WorldMapNativeModelFeature* feature,int32_t item_id,
    const WorldMapHeldItemFeatureObservations& observations,WorldMapHeldItemFeatureStep* out) {
    using Status=WorldMapModelFeatureStatus;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapModelFeatureObjectState>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapHeldItemFeatureStep>);
    if(feature==nullptr || out==nullptr || &feature->state_==&out->after)return Status::InvalidInput;
    try {
        WorldMapHeldItemFeatureStep prepared;
        const auto status=prepare_world_map_held_item_feature(feature->state_,item_id,observations,&prepared);
        if(status==Status::InvalidInput)return status;
        if(status!=Status::Prepared){*out=std::move(prepared);return status;}
        auto after=prepared.after; // Last allocating copy precedes publication.
        *out=std::move(prepared);feature->state_=std::move(after);return Status::Advanced;
    } catch(const std::bad_alloc&) {return Status::AllocationFailure;}
}
} // namespace awl
