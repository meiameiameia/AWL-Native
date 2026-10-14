#include "awl/world_map_player_movement.h"
#include "awl/world_map_trigger_asset.h"
#include <cfenv>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
WorldMapPlayerMovementResult advance_world_map_player_movement(
    const WorldMapUpdateInputs& inputs,const WorldMapPlayerMovementInput& input,
    WorldMapPlayerMovementState* owner,WorldMapSceneBucketRegistry* scene) {
    using S=WorldMapPlayerMovementStatus;
    static_assert(std::is_nothrow_copy_constructible_v<WorldMapPlayerMovementResult>);
    WorldMapPlayerMovementResult result;const auto stop=[&](S status){result.status=status;return result;};
    const auto outputs=[&](uint32_t metadata,uint32_t direction,const std::array<float,3>& position,const std::array<float,3>& axis){
        if(input.outputs.metadata)result.metadata=metadata;
        if(input.outputs.direction)result.direction=direction;
        if(input.outputs.position)result.position=position;
        if(input.outputs.axis)result.axis=axis;
    };
    if(!input.guards)return stop(S::RequiresGuards);
    bool skip=input.guards->word_680!=-1;
    if(!skip){if(!input.guards->word_58c)return stop(S::RequiresGuardWord58c);skip=*input.guards->word_58c!=0;}
    if(skip){outputs(0,7,{0,0,0},{0,0,0});result.intensity=0.0f;return stop(S::SkippedByGuard);}
    if(!owner || !scene)return stop(S::RequiresOwners);
    if(owner->pose.scene_type!=1)return stop(S::UnsupportedCategory);
    const auto* node=scene->object(owner->scene_identity);
    if(!node || node->scene_type!=owner->pose.scene_type ||
        std::memcmp(node->position.data(),owner->pose.position.data(),sizeof(float)*3)!=0 ||
        (node->context.facing_28 && std::memcmp(node->context.facing_28->data(),owner->pose.heading.data(),sizeof(float)*3)!=0))return stop(S::InvalidInput);
    const auto* pad=inputs.pad_for_loop();if(!pad)return stop(S::RequiresPad);
    if(std::fegetround()!=FE_TONEAREST)return stop(S::UnsupportedNumerics);
    if(!owner->camera)return stop(S::RequiresCamera);
    if(!input.dependencies)return stop(S::RequiresCollision);
    try {
        auto query=*input.dependencies;query.state_680=-1;query.state_58c=0;query.pad=pad->frame;
        query.current_position=owner->pose.position;query.current_axis=owner->pose.heading;query.steering=owner->steering;
        // Facing +58/+60 is the same entry field represented by pose.heading.
        query.steering.facing_x=owner->pose.heading[0];query.steering.facing_z=owner->pose.heading[2];
        query.camera_yaw_radians=owner->camera->target.camera.yaw;query.camera_yaw_commit_enabled=owner->camera->target.camera.flag_98;
        WorldMapMovementCandidate movement;
        if(!calculate_world_map_movement_candidate(query,&movement))return stop(S::CollisionIncomplete);
        result.movement=movement;
        auto registry=*scene;auto next=*owner;
        WorldMapScenePositionUpdate position;
        if(!registry.update_pose(owner->scene_identity,owner->pose.scene_type,movement.resolved_position,movement.heading_after,&position))return stop(S::SceneIncomplete);
        result.scene=position;
        const auto& contact=movement.directional_contact;
        outputs(contact.metadata,contact.direction_code,contact.world_position,contact.heading_axis);
        WorldMapMovementContactTail tail;tail.recorded_category=1;tail.recorded_prior=owner->pose.position;tail.recorded_resolved=movement.resolved_position;
        result.tail=tail;
        if(!input.triggers)return stop(S::RequiresTriggerOwner);
        for(int32_t slot=0;slot<2;++slot){
            bool contacted=false;result.stopped_slot=slot;
            if(!input.triggers->query_category1_contact(size_t(slot),tail.recorded_prior,tail.recorded_resolved,&contacted))return stop(S::TriggerIncomplete);
            ++tail.polygon_queries;result.tail=tail;
            if(contacted)return stop(S::RequiresActionRequest);
        }
        result.stopped_slot=-1;
        // +3F1 is reached by the pressed-bit branch or outside 30E18's
        // fixed regions. Known-region/no-press paths skip this byte entirely.
        if(!input.secondary_byte_3f1){
            if(pad->frame.pressed&0x01000000u)return stop(S::RequiresSecondaryByte);
            WorldMapCameraFollowup region;
            if(!plan_world_map_camera_followup(owner->camera->target.camera,1,movement.resolved_position,1,pad->frame.substick_x,&region))return stop(S::CameraIncomplete);
            if(region.region_index<0)return stop(S::RequiresSecondaryByte);
        }
        if(!input.sample_height)return stop(S::RequiresHeightSource);
        WorldMapPlayerMovementCameraQuery camera;camera.previous_camera=*owner->camera;
        camera.previous_camera.target.camera.yaw=movement.camera_yaw_after_proposal;
        camera.resolved_position=movement.resolved_position;camera.collision_category=1;
        camera.heading_x=movement.heading_after[0];camera.heading_z=movement.heading_after[2];
        camera.global_control_word_8034157c=pad->frame.pressed;camera.global_byte_3f1=input.secondary_byte_3f1.value_or(0);
        camera.pad_byte_8e=pad->frame.substick_x;
        WorldMapPlayerMovementCameraResult updated;
        if(!calculate_world_map_player_movement_camera_update(camera,input.sample_height,input.sample_context,&updated))return stop(S::CameraIncomplete);
        result.camera=updated;
        next.pose.position=movement.resolved_position;next.pose.heading=movement.heading_after;next.steering=movement.steering;
        next.shared_contact=tail;next.camera=updated.final_camera;next.camera_update=updated;
        static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerMovementState>);
        static_assert(std::is_nothrow_move_assignable_v<WorldMapSceneBucketRegistry>);
        *owner=std::move(next);*scene=std::move(registry);
        result.intensity=movement.steering.intensity;return stop(S::Advanced);
    }catch(const std::bad_alloc&){return stop(S::AllocationFailure);}
}
} // namespace awl
