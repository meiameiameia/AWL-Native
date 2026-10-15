#include "awl/world_map_player_sources.h"
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using S=WorldMapPlayerSourcesStatus;
uint64_t identity(const void* p) noexcept {return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));}
int32_t signed_word(uint32_t v) noexcept {int32_t out;std::memcpy(&out,&v,sizeof(out));return out;}
bool current(const WorldMapPlayerPrimaryLoop& receipt,const GameClockState& clock) noexcept {
    return receipt.valid && receipt.loop_count==clock.loop_count && receipt.raw_time==clock.raw_time;
}
bool primary_complete(WorldMapPlayerPrimarySliceStatus status) noexcept {return status==WorldMapPlayerPrimarySliceStatus::Completed;}
}
S WorldMapPlayerCollisionSources::load(const WorldMapCollisionSourceState& source,
    std::shared_ptr<const WorldMapCameraCollisionAsset> camera,bool load_objects,
    std::shared_ptr<const WorldMapPlayerCollisionSources>* out){
    if(!out)return S::InvalidInput;
    WorldMapCollisionSelection selection;
    if(!derive_world_map_collision_selection(source,&selection))return S::InvalidInput;
    if(!camera || !camera->loaded())return S::RequiresCamera;
    try {
        auto next=std::shared_ptr<WorldMapPlayerCollisionSources>(new WorldMapPlayerCollisionSources);
        next->selection_=selection;next->camera_=std::move(camera);
        if(!next->movement_.load_from_source_state(source) || !next->triggers_.load() || !next->probe_.load())return S::AssetFailure;
        if(load_objects){next->pools_.emplace();if(!next->pools_->load())return S::AssetFailure;}
        *out=std::move(next);return S::Completed;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
S WorldMapPlayerSources::initialize(const WorldMapPlayerStartup& player,const WorldMapPlayerSourcesSeed& seed,WorldMapPlayerSourcesInitialization mode){
    const bool fresh=mode==WorldMapPlayerSourcesInitialization::Fresh;
    if(!fresh && mode!=WorldMapPlayerSourcesInitialization::Observed)return S::InvalidInput;
    if(player_identity_)return S::AlreadyInitialized;
    if(!inputs_.game_clock() || inputs_.game_clock()!=player.model().game_clock())return S::ForeignPlayer;
    if(seed.movement.scene_identity==0 || seed.movement.pose.scene_type!=1)return S::InvalidInput;
    for(const auto& v:{seed.movement.pose.position,seed.movement.pose.heading})for(float f:v)if(!std::isfinite(f))return S::InvalidInput;
    if(fresh && (seed.updates.status || seed.updates.saved_selection))return S::InvalidInput;
    if(!fresh && !seed.updates.saved_selection)return S::InvalidInput;
    try {
        auto updates=seed.updates;auto movement=seed.movement;auto fields=seed.fields;
        WorldMapSceneBucketRegistry scene;WorldMapScenePositionUpdate placed;
        if(!scene.register_object(movement.scene_identity,movement.pose.scene_type,movement.pose.position) ||
            !scene.update_pose(movement.scene_identity,movement.pose.scene_type,movement.pose.position,movement.pose.heading,&placed))return S::InvalidInput;
        if(fresh){
            const auto phase=inputs_.sources().phase?inputs_.sources().phase->raw_word_0:std::nullopt;
            WorldMapPlayerStatusState status;
            if(player.starts()->prepare_fresh_status(phase,&status)!=WorldMapPlayerStatusCode::Prepared)return S::RequiresPhase;
            updates.status=status;updates.saved_selection=WorldMapPlayerSavedSelection{0u,0u};
            fields.guards=WorldMapPlayerUpdateGuards{-1,0};
        }
        static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerUpdateOwners>);
        static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerMovementState>);
        static_assert(std::is_nothrow_move_assignable_v<WorldMapSceneBucketRegistry>);
        updates_=std::move(updates);movement_=std::move(movement);scene_=std::move(scene);fields_=fields;
        player_identity_=identity(&player);return S::Initialized;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
S WorldMapPlayerSources::check(const WorldMapPlayerStartup& player) const noexcept {
    if(!player_identity_)return S::RequiresInitialization;
    if(player_identity_!=identity(&player) || inputs_.game_clock()!=player.model().game_clock())return S::ForeignPlayer;
    if(inputs_.queue_control().status!=WorldMapDispatchStatus::Ready)return S::RequiresControl;
    if(player.state().state_1364!=0 && player.state().state_1364!=1)return S::UnsupportedState;
    return S::Completed;
}
bool WorldMapPlayerSources::collision_busy() const noexcept {
    if(!collision_loop_ || !inputs_.game_clock())return false;
    const auto& clock=inputs_.game_clock()->state();
    return collision_loop_->first==clock.loop_count && collision_loop_->second==clock.raw_time;
}
void WorldMapPlayerSources::retain_collision_use() noexcept {
    const auto& clock=inputs_.game_clock()->state();collision_loop_=std::make_pair(clock.loop_count,clock.raw_time);
}
S WorldMapPlayerSources::install_collision(std::shared_ptr<const WorldMapPlayerCollisionSources> resources){
    if(!resources)return S::InvalidInput;
    if(collision_busy())return S::Busy;
    if(objects_ && (objects_->size(WorldMapCollisionList::First) || objects_->size(WorldMapCollisionList::Later) ||
        objects_->size(WorldMapCollisionList::Third)))return S::Busy;
    collision_=std::move(resources);probe_present_=true;return S::Completed;
}
S WorldMapPlayerSources::clear_collision() noexcept {
    if(collision_busy())return S::Busy;
    if(objects_ && (objects_->size(WorldMapCollisionList::First) || objects_->size(WorldMapCollisionList::Later) ||
        objects_->size(WorldMapCollisionList::Third)))return S::Busy;
    collision_.reset();probe_present_=false;return S::Completed;
}
bool WorldMapPlayerSources::bind_probe() noexcept {
    if(!collision_)return false;probe_present_=true;return true;
}
S WorldMapPlayerSources::register_collision_object(WorldMapCollisionList list,
    const WorldMapRegisteredCollisionObject& input,std::optional<std::pair<int32_t,uint32_t>> record){
    if(!objects_)return S::RequiresCollisionLists;
    if(input.collision.data || input.collision.size)return S::InvalidInput;
    auto object=input;
    if(object.collision.collision_flags&2u){
        if(!collision_ || !collision_->pools_ || !record)return S::RequiresRecordPool;
        WorldMapCollisionRecordView view;
        if(!collision_->pools_->lookup(record->first,record->second,&view))return S::InvalidInput;
        object.collision.data=view.data;object.collision.size=view.size;object.collision.center_local=view.center_local;
    }else if(record)return S::InvalidInput;
    try {
        auto next=*objects_;if(!next.register_object(list,object))return S::InvalidInput;
        *objects_=std::move(next);return S::Completed;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
bool WorldMapPlayerSources::unregister_collision_object(uint64_t key) noexcept {return objects_ && objects_->unregister_object(key);}
S WorldMapPlayerSources::prepare_movement(const WorldMapPlayerStartup& player,WorldMapCollisionSnapshot& snapshot,
    WorldMapMovementQuery& query,WorldMapPlayerMovementInput& input) const {
    if(!collision_)return S::RequiresCollision;
    if(!inputs_.sources().phase || !inputs_.sources().phase->raw_word_0)return S::RequiresPhase;
    if(!fields_.terrain_variant_299a4)return S::RequiresVariant;
    WorldMapCollisionSelection selection;
    if(!derive_world_map_collision_selection({*inputs_.sources().phase->raw_word_0,*fields_.terrain_variant_299a4},&selection))return S::InvalidInput;
    if(selection.phase_index!=collision_->selection_.phase_index || selection.alternate_terrain!=collision_->selection_.alternate_terrain)return S::WrongCollisionSelection;
    if(!player.camera() || player.camera()->collision!=collision_->camera_ || !collision_->camera_->loaded())return S::RequiresCamera;
    if(!fields_.static_flags)return S::RequiresStaticFlags;
    if(!objects_)return S::RequiresCollisionLists;
    snapshot=objects_->snapshot();
    // Linked player views read the current actor virtually; the native
    // registration record must not freeze its constructor position.
    for(auto& c:snapshot.third_resolver)if(c.identity==movement_.scene_identity)c.center_world=movement_.pose.position;
    for(auto& o:snapshot.third_objects)if(o.collision.identity==movement_.scene_identity){
        o.collision.center_world=o.world_position=movement_.pose.position;o.heading_axis=movement_.pose.heading;
    }
    if(!collision_->movement_.bind(&query.collision))return S::RequiresCollision;
    query.collision.static_flags=*fields_.static_flags;query.collision.static_flags.state_299a4=*fields_.terrain_variant_299a4!=0;
    query.collision.source_identity=movement_.scene_identity;
    query.collision.first_objects=snapshot.first_resolver.data();query.collision.first_object_count=snapshot.first_resolver.size();
    query.collision.later_objects=snapshot.later_resolver.data();query.collision.later_object_count=snapshot.later_resolver.size();
    query.collision.third_objects=snapshot.third_resolver.data();query.collision.third_object_count=snapshot.third_resolver.size();
    query.directional_objects=snapshot.first_directional.data();query.directional_object_count=snapshot.first_directional.size();
    input.dependencies=&query;input.triggers=&collision_->triggers_;input.guards=fields_.guards;input.secondary_byte_3f1=fields_.secondary_byte_3f1;
    return S::Completed;
}
WorldMapPlayerSourcesResult WorldMapPlayerSources::advance_primary(WorldMapPlayerStartup& player){
    WorldMapPlayerSourcesResult result;result.status=check(player);if(result.status!=S::Completed)return result;
    try {
        if(player.state().state_1364==0){
            WorldMapPlayerState0ParentInput input;input.guards=fields_.guards;input.scene_child_word_4=fields_.scene_child_word_4;
            input.pose=movement_.pose;if(updates_.status)input.saved_subobject_byte_14c=updates_.status->byte_14c;
            result.state0_primary=player.advance_state0_primary_slice(inputs_,input,&updates_);
            result.status=primary_complete(result.state0_primary->status)?S::Completed:S::CallbackIncomplete;
        }else{
            WorldMapPlayerState1ParentInput input;input.guards=fields_.guards;input.scene_child_word_4=fields_.scene_child_word_4;
            if(updates_.status)input.saved_subobject_byte_14c=updates_.status->byte_14c;
            if(updates_.saved_selection){input.item_144=updates_.saved_selection->item_144;input.action_148=updates_.saved_selection->action_148;}
            input.item_type=fields_.item_type;
            WorldMapCollisionSnapshot snapshot;WorldMapMovementQuery query;WorldMapPlayerMovementInput movement;
            const auto control=inputs_.dispatch(0x5001);
            const bool reused=player.primary_loop() && current(*player.primary_loop(),inputs_.game_clock()->state());
            if(!reused && control.status==WorldMapDispatchStatus::Ready && control.control_word && !(*control.control_word&0x800u) &&
                prepare_world_map_player_normal_update_entry({control.control_word,fields_.guards,fields_.scene_child_word_4})==WorldMapPlayerUpdateStatus::Prepared){
                result.status=prepare_movement(player,snapshot,query,movement);if(result.status!=S::Completed)return result;input.movement=&movement;
            }
            result.state1_primary=player.advance_state1_primary_slice(inputs_,input,&updates_,&movement_,&scene_);
            const auto& parent=result.state1_primary->parent;
            if(parent && parent->status==WorldMapPlayerUpdateStatus::Advanced && parent->movement)retain_collision_use();
            result.status=primary_complete(result.state1_primary->status)?S::Completed:S::CallbackIncomplete;
        }
    }catch(const std::bad_alloc&){result.status=S::AllocationFailure;}
    return result;
}
WorldMapPlayerSourcesResult WorldMapPlayerSources::advance_secondary(WorldMapPlayerStartup& player){
    WorldMapPlayerSourcesResult result;result.status=check(player);if(result.status!=S::Completed)return result;
    try {
        WorldMapPlayerState1SecondaryInput body;body.guards=fields_.guards;body.terrain_slot_present=probe_present_;
        body.terrain=collision_?&collision_->probe_:nullptr;body.secondary_byte_3f1=fields_.secondary_byte_3f1;
        if(player.state().state_1364==0){
            WorldMapPlayerState0SecondaryInput input;input.body=body;
            if(updates_.saved_selection){input.item_144=updates_.saved_selection->item_144;input.action_148=updates_.saved_selection->action_148;}
            const auto prefix=player.advance_state0_secondary(inputs_,fields_.guards);
            WorldMapCollisionSnapshot snapshot;WorldMapMovementQuery query;WorldMapPlayerMovementInput movement;
            if(prefix.status==WorldMapPlayerState0SecondaryStatus::RequiresMovement){
                result.status=prepare_movement(player,snapshot,query,movement);if(result.status!=S::Completed)return result;input.movement=&movement;
            }
            result.state0_secondary=player.advance_state0_secondary(inputs_,input,&updates_,&movement_,&scene_);
            const auto status=result.state0_secondary->status;
            const bool done=status==WorldMapPlayerState0SecondaryStatus::RetainedState || status==WorldMapPlayerState0SecondaryStatus::SkippedByGuard ||
                status==WorldMapPlayerState0SecondaryStatus::AlreadyCompleted || status==WorldMapPlayerState0SecondaryStatus::Filtered;
            if(result.state0_secondary->movement && (done || status==WorldMapPlayerState0SecondaryStatus::TransitionRequired))retain_collision_use();
            result.status=done?S::Completed:S::CallbackIncomplete;
        }else{
            result.state1_secondary=player.advance_state1_secondary(inputs_,body,&updates_,&movement_);
            const auto status=result.state1_secondary->status;
            result.status=status==WorldMapPlayerState1SecondaryStatus::Completed || status==WorldMapPlayerState1SecondaryStatus::AlreadyCompleted ||
                status==WorldMapPlayerState1SecondaryStatus::Filtered?S::Completed:S::CallbackIncomplete;
        }
    }catch(const std::bad_alloc&){result.status=S::AllocationFailure;}
    return result;
}
WorldMapPlayerSourcesResult WorldMapPlayerSources::advance_transition(WorldMapPlayerStartup& player){
    WorldMapPlayerSourcesResult result;result.status=check(player);if(result.status!=S::Completed)return result;
    WorldMapPlayerState1TransitionInput input;input.saved_byte_14d35=fields_.saved_remap_byte_14d35;
    input.secondary_byte_3f1=fields_.secondary_byte_3f1;input.audio_byte_90=fields_.audio_byte_90;
    if(updates_.status)input.saved_subobject_byte_14c=updates_.status->byte_14c;
    if(updates_.saved_selection && updates_.saved_selection->action_148)input.action_148=signed_word(*updates_.saved_selection->action_148);
    result.transition=player.advance_state1_transition(inputs_,input,&updates_);
    const auto status=result.transition->status;result.status=status==WorldMapPlayerState1TransitionStatus::Advanced ||
        status==WorldMapPlayerState1TransitionStatus::AlreadyAdvanced?S::Completed:S::CallbackIncomplete;return result;
}
WorldMapPlayerSourcesResult WorldMapPlayerSources::advance_third(WorldMapPlayerStartup& player,
    const WorldMapPlayerContextUpdateSources& sources,WorldMapPlayerState0ThirdInput input){
    WorldMapPlayerSourcesResult result;result.status=check(player);if(result.status!=S::Completed)return result;
    input.guards=fields_.guards;
    const auto control=inputs_.dispatch(0x5001);
    const bool reached=control.status==WorldMapDispatchStatus::Ready && control.control_word && !(*control.control_word&0x800u) &&
        fields_.guards && fields_.guards->word_680==-1 && fields_.guards->word_58c==0;
    const auto& receipt=player.state0_third_loop();const auto& clock=inputs_.game_clock()->state();
    const bool reused=receipt && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time;
    if(!reused && reached && input.context){
        if(!updates_.saved_selection || !updates_.saved_selection->item_144 || !updates_.saved_selection->action_148){result.status=S::InvalidInput;return result;}
        input.context->held_item=signed_word(*updates_.saved_selection->item_144);input.context->action=signed_word(*updates_.saved_selection->action_148);
        input.context->scene_type=movement_.pose.scene_type;input.context->position=movement_.pose.position;input.context->facing=movement_.pose.heading;
        input.context->tracked_14ac=player.state().target_14ac;input.context->secondary_byte_3f1=fields_.secondary_byte_3f1;
    }
    auto bound=sources;bound.registry=&scene_;
    result.third=player.state().state_1364==0?player.advance_state0_third(inputs_,bound,input):player.advance_state1_third(inputs_,bound,input);
    const auto status=result.third->status;result.status=status==WorldMapPlayerState0ThirdStatus::Prepared || status==WorldMapPlayerState0ThirdStatus::SkippedByGuard ||
        status==WorldMapPlayerState0ThirdStatus::Filtered || status==WorldMapPlayerState0ThirdStatus::AlreadyCompleted?S::Completed:S::CallbackIncomplete;return result;
}
WorldMapPlayerSourcesResult WorldMapPlayerSources::consume_queued(WorldMapPlayerStartup& player,
    const WorldMapPlayerStartupQueuedActorInput& observations,WorldMapPlayerActorQueue* render){
    WorldMapPlayerSourcesResult result;result.status=check(player);if(result.status!=S::Completed)return result;
    try {
    const auto& receipt=player.primary_loop();
    const bool reused=receipt && current(*receipt,inputs_.game_clock()->state()) && receipt->queued_completed;
    auto input=reused?WorldMapPlayerStartupQueuedActorInput{}:observations;
    input.actor.frame.actor_placement=WorldMapPlayerActorPlacement{movement_.pose.position,movement_.pose.heading};
    result.queued=player.state().state_1364==0?player.consume_state0_update_after_third(inputs_,input,&updates_,render):player.consume_state1_update_after_third(inputs_,input,&updates_,render);
    const auto status=result.queued->status;result.status=status==WorldMapPlayerStartupQueueStatus::Completed || status==WorldMapPlayerStartupQueueStatus::Empty ||
        status==WorldMapPlayerStartupQueueStatus::AlreadyCompleted?S::Completed:S::CallbackIncomplete;
    }catch(const std::bad_alloc&){result.status=S::AllocationFailure;}
    return result;
}
} // namespace awl
