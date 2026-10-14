#include "awl/world_map_player_state1_secondary.h"
#include "awl/world_map_camera.h"
#include "awl/filesystem.h"
#include <cfenv>
#include <cmath>
#include <memory>
#include <new>
#include <utility>

namespace awl {
bool WorldMapPlayerTerrainProbeAsset::load_from_bytes(const uint8_t* data,size_t size){
    try {
        CollisionTreeAnalysis analysis;
        if(!analyze_type1_collision_asset(data,size,&analysis) || analysis.header_byte_6!=0){bytes_.clear();return false;}
        std::vector<uint8_t> next(data,data+size);bytes_.swap(next);return true;
    }catch(const std::bad_alloc&){bytes_.clear();return false;}
}
bool WorldMapPlayerTerrainProbeAsset::load(){
    void* raw=nullptr;size_t size=0;
    if(!filesystem_read_entire_file("/files/mapjump.col",&raw,&size)){bytes_.clear();return false;}
    std::unique_ptr<void,decltype(&filesystem_free_file_data)> owner(raw,&filesystem_free_file_data);
    return load_from_bytes(static_cast<const uint8_t*>(raw),size);
}
WorldMapPlayerTerrainProbeResult WorldMapPlayerTerrainProbeAsset::query(const WorldMapPlayerScenePose& pose) const {
    using S=WorldMapPlayerTerrainProbeStatus;WorldMapPlayerTerrainProbeResult result;
    const auto stop=[&](S code){result.status=code;return result;};
    if(bytes_.empty())return stop(S::RequiresAsset);
    if(std::fegetround()!=FE_TONEAREST)return stop(S::UnsupportedNumerics);
    for(float f:pose.position)if(!std::isfinite(f))return stop(S::InvalidInput);
    for(float f:pose.heading)if(!std::isfinite(f))return stop(S::InvalidInput);
    result.code=7;
    if(pose.heading[0]==0 && pose.heading[2]==0)return stop(S::Prepared);
    // 1D848 zeros Y, normalizes XZ, then scales by unit length. Native sqrt
    // and cos replace original reciprocal-estimate/libm routines; boundary
    // equivalence requires further reference calibration.
    const float length=std::sqrt(pose.heading[0]*pose.heading[0]+pose.heading[2]*pose.heading[2]);
    if(!std::isfinite(length) || length==0)return stop(S::UnsupportedNumerics);
    const std::array<float,3> direction{pose.heading[0]/length,0,pose.heading[2]/length};
    const std::array<float,3> end{pose.position[0]+direction[0],pose.position[1],pose.position[2]+direction[2]};
    try {
        CollisionDirectedTriangleSample sample;
        if(!query_type1_collision_directed_triangle(bytes_.data(),bytes_.size(),pose.position,end,&sample))return stop(S::UnsupportedLayout);
        result.triangle=sample;if(!sample.found)return stop(S::Prepared);
        std::array<float,3> a,b,n;for(size_t i=0;i<3;++i){a[i]=sample.triangle[1][i]-sample.triangle[0][i];b[i]=sample.triangle[2][i]-sample.triangle[0][i];}
        n={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        const float norm=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(!std::isfinite(norm) || norm==0)return stop(S::UnsupportedNumerics);
        const float dot=std::fabs(direction[0]*(n[0]/norm)+direction[2]*(n[2]/norm));
        if(!std::isfinite(dot))return stop(S::UnsupportedNumerics);
        if(dot>=static_cast<float>(std::cos(static_cast<double>(0x1.921fb6p-1f))))result.code=sample.surface_flags&0xfffu;
        return stop(S::Prepared);
    }catch(const std::bad_alloc&){return stop(S::AllocationFailure);}
}
WorldMapPlayerState1SecondaryResult prepare_world_map_player_state1_secondary(const WorldMapPlayerState1SecondaryQuery& input){
    using S=WorldMapPlayerState1SecondaryStatus;using C=WorldMapPlayerStatusConditionCode;using T=WorldMapPlayerTerrainProbeStatus;
    WorldMapPlayerState1SecondaryResult result;const auto stop=[&](S status){result.status=status;return result;};
    const auto request=[&](uint32_t state){result.requested_state=state;return stop(S::TransitionRequired);};
    if(!input.control)return stop(S::RequiresControl);if(*input.control&0x800u)return stop(S::Completed);
    if(!input.status)return stop(S::RequiresStatus);if(!input.action)return stop(S::RequiresAction);
    result.conditions=prepare_world_map_player_status_conditions(*input.status,input.factors,input.phase_raw,*input.action);
    if(result.conditions->status==C::TransitionRequired){result.requested_state=result.conditions->requested_state;return stop(S::TransitionRequired);}
    if(result.conditions->status!=C::Prepared)return stop(S::ConditionsIncomplete);
    if(!input.dependencies.terrain_slot_present)return stop(S::RequiresTerrainSlot);
    int32_t terrain_code=7;
    if(*input.dependencies.terrain_slot_present){
        if(!input.dependencies.terrain || !input.dependencies.terrain->loaded())return stop(S::RequiresTerrainAsset);
        if(!input.pose)return stop(S::RequiresMovementOwner);
        result.terrain=input.dependencies.terrain->query(*input.pose);
        if(result.terrain->status!=T::Prepared)return stop(result.terrain->status==T::AllocationFailure?S::AllocationFailure:S::TerrainIncomplete);
        terrain_code=*result.terrain->code;
    }
    if(terrain_code==0 || terrain_code==1)return stop(S::RequiresTerrainHandler);
    if(!input.pressed)return stop(S::RequiresPad);const auto pad=*input.pressed;
    if(pad&0x1000u){result.reset_required=true;return request(0x27);}
    if(pad&0x200u){if(!input.item)return stop(S::RequiresItem);if(*input.item){result.action_setup_required=true;return request(7);}}
    if(pad&0x400u){if(!input.item)return stop(S::RequiresItem);if(!*input.item && !*input.action)return request(3);
        if(!*input.action){result.action_setup_required=true;return request(4);}return stop(S::Completed);}
    if(pad&0x10u){if(!input.manager_scene_type)return stop(S::RequiresSceneType);
        if(*input.manager_scene_type==1){if(!input.pose)return stop(S::RequiresMovementOwner);
            if(input.pose->scene_type==1){if(!input.dependencies.secondary_byte_3f1)return stop(S::RequiresSecondaryByte);if(*input.dependencies.secondary_byte_3f1)return stop(S::Completed);}
            result.scene_cleanup_required=true;return request(2);
        }
    }
    if(pad&0x02000000u){if(!input.item)return stop(S::RequiresItem);if(*input.item || *input.action)return stop(S::Completed);
        if(!input.pose)return stop(S::RequiresMovementOwner);
        if(input.pose->scene_type==1 && classify_world_map_camera_region(1,input.pose->position)<0)return request(0x2b);
        return request(0x2e);
    }
    return stop(S::Completed);
}
} // namespace awl
