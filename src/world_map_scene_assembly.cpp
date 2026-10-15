#include "awl/world_map_scene_assembly.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace awl {
namespace {
int32_t signed_word(uint32_t v) noexcept {int32_t out;std::memcpy(&out,&v,sizeof(out));return out;}
template<size_t N> bool finite(const std::array<float,N>& values) noexcept {
    for(float f:values)if(!std::isfinite(f))return false;
    return true;
}
bool register_scene(const WorldMapPreparedSceneObject& object,WorldMapSceneBucketRegistry& scene){
    if(object.scene.scene_type<0 || object.scene.scene_type>44 || !finite(object.scene.position))return false;
    if(!scene.register_object(object.scene.identity,object.scene.scene_type,object.scene.position))return false;
    WorldMapScenePositionUpdate update;
    if(object.positioned && !scene.update_position(object.scene.identity,object.scene.position,&update))return false;
    return scene.set_context_metadata(object.scene.identity,object.scene.context);
}
bool prepared_actor(const WorldMapPreparedSceneActor& actor){
    const auto& c=actor.collision.collision;
    return actor.object.positioned && actor.object.scene.identity==c.identity && c.identity &&
        c.category==actor.object.scene.scene_type && c.collision_flags==1u && !c.data && !c.size &&
        finite(actor.collision.world_position) && finite(actor.collision.heading_axis) && std::isfinite(c.radius) && c.radius>=0 &&
        actor.object.scene.position==actor.collision.world_position && c.center_world==actor.collision.world_position;
}
}
WorldMapSceneAssemblyResult WorldMapSceneAssembly::create(const WorldMapPlayerStartupProviders& providers,
    const WorldMapSceneAssemblyQuery& query,std::unique_ptr<WorldMapSceneAssembly>* out){
    using S=WorldMapSceneAssemblyStatus;
    WorldMapSceneAssemblyResult result;
    const auto stop=[&](S s){result.status=s;return result;};
    if(!out || !providers.clock || !providers.starts || !query.collision || !query.control.phase ||
        !query.control.phase->raw_word_0 || !query.player.fields.terrain_variant_299a4 ||
        !query.player.fields.static_flags || query.player.movement.pose.scene_type!=1)return result;
    if(!query.collision->pools_)return stop(S::RequiresRecordPools);
    WorldMapCollisionSelection selection;
    if(!derive_world_map_collision_selection({*query.control.phase->raw_word_0,*query.player.fields.terrain_variant_299a4},&selection))return result;
    if(selection.phase_index!=query.collision->selection().phase_index || selection.alternate_terrain!=query.collision->selection().alternate_terrain)
        return stop(S::WrongCollisionSelection);
    if(!query.saved_actor_a_29a64 || !query.saved_actor_b_29a60)return stop(S::RequiresActorPresence);
    const bool a=*query.saved_actor_a_29a64>=0x29 && *query.saved_actor_a_29a64<=0x2b;
    const bool b=*query.saved_actor_b_29a60==0x27 || *query.saved_actor_b_29a60==0x28;
    if(a!=query.actor_a.has_value() || b!=query.actor_b.has_value())return stop(S::RequiresPreparedActor);
    if(!query.player.fields.guards || query.player.fields.guards->word_680!=-1 || query.player.fields.guards->word_58c!=0)
        return stop(S::UnsupportedEntryGate);
    // This bounded entry has no interpreter request. Never pretend to execute
    // 10AAC4 and its potentially changed guards/phase/fade fields.
    if(!query.previous_mode)return stop(S::RequiresEntrySources);
    if(*query.previous_mode==4 || *query.previous_mode==6){
        if(!query.sequence_request_byte)return stop(S::RequiresEntrySources);
        if(*query.sequence_request_byte)return stop(S::SequenceRequired);
    }else if(*query.previous_mode==0x28 || *query.previous_mode==0x29 || *query.previous_mode==0x30)return stop(S::SequenceRequired);
    try {
        auto next=std::unique_ptr<WorldMapSceneAssembly>(new WorldMapSceneAssembly(providers.clock));
        auto seed=query.player;
        if(query.status_mode==WorldMapPlayerSourcesInitialization::Fresh){
            if(seed.updates.status || seed.updates.saved_selection)return result;
            WorldMapPlayerStatusState status;
            if(providers.starts->prepare_fresh_status(query.control.phase->raw_word_0,&status)!=WorldMapPlayerStatusCode::Prepared)return stop(S::RequiresStatus);
            seed.updates.status=status;seed.updates.saved_selection=WorldMapPlayerSavedSelection{0u,0u};
        }else if(query.status_mode!=WorldMapPlayerSourcesInitialization::Observed)return result;
        if(!seed.updates.status || !seed.updates.status->byte_14c || !seed.updates.saved_selection ||
            !seed.updates.saved_selection->item_144 || !seed.updates.saved_selection->action_148)return stop(S::RequiresStatus);
        // All identities must be distinct even across separate native lists.
        // Original addresses are opaque native keys, never reconstructed.
        std::vector<uint64_t> keys{seed.movement.scene_identity};
        const auto add_key=[&](uint64_t key){if(!key || std::find(keys.begin(),keys.end(),key)!=keys.end())return false;keys.push_back(key);return true;};
        if(!keys[0])return result;
        auto fixed=query.fixed_objects;
        for(const auto& object:fixed){const auto& c=object.collision;
            if(c.data || c.size || !add_key(c.identity) || !finite(c.world_to_object) || !finite(c.object_to_world) ||
                !finite(c.center_world) || !std::isfinite(c.radius) || c.radius<0)return result;
        }
        if(!bind_world_map_fixed_collision_records(*query.collision->pools_,&fixed))return result;
        WorldMapCollisionRegistry objects;
        if(!register_world_map_fixed_collision_objects(query.fixed_state,fixed,&objects))return result;
        WorldMapSceneBucketRegistry scene;
        for(const auto* actor:{a?&*query.actor_a:nullptr,b?&*query.actor_b:nullptr})if(actor){
            auto object=actor->object;object.scene.context.facing_28=actor->collision.heading_axis;
            if(!prepared_actor(*actor) || !add_key(object.scene.identity) || !register_scene(object,scene) ||
                !objects.register_object(WorldMapCollisionList::First,actor->collision))return result;
        }
        // 141D8/2FDF8 placement sees fixed + A/B, never the later mandatory actor.
        const auto before_player=objects.snapshot();
        WorldMapPlayerStartQuery placement;
        placement.saved_pose=seed.movement.pose;placement.placement.static_flags=*seed.fields.static_flags;
        placement.placement.static_flags.state_299a4=*seed.fields.terrain_variant_299a4!=0;
        const auto& terrain=query.collision->movement().terrain_bytes();const auto& statics=query.collision->movement().static_bytes();
        placement.placement.terrain_data=terrain.data();placement.placement.terrain_size=terrain.size();
        placement.placement.static_data=statics.data();placement.placement.static_size=statics.size();
        placement.placement.list_92a0=before_player.later_resolver.data();placement.placement.list_92a0_count=before_player.later_resolver.size();
        placement.placement.list_92d8=before_player.first_resolver.data();placement.placement.list_92d8_count=before_player.first_resolver.size();
        result.placement=prepare_world_map_player_start(placement,&next->placement_);
        if(*result.placement!=WorldMapPlayerStartStatus::Ready)return stop(S::PlacementIncomplete);
        auto startup=query.startup;
        startup.initial.phase=selection.phase_index;startup.tail.inputs.start=placement;
        startup.saved_subobject_byte_14c=*seed.updates.status->byte_14c;startup.saved_subobject_word_20=signed_word(seed.updates.status->word_20);
        startup.item_144_after_setup=*seed.updates.saved_selection->item_144;
        startup.tail.action_148_after_setup=signed_word(*seed.updates.saved_selection->action_148);
        startup.audio_byte_90=seed.fields.audio_byte_90;startup.item_type=seed.fields.item_type;
        result.player=construct_world_map_player_startup(providers,startup,&next->player_);
        if(result.player->status!=WorldMapPlayerStartupStatus::ConstructedCpuStartup)return stop(S::PlayerIncomplete);
        // Do not retain transient snapshot pointers in any scene/owner state.
        seed.movement={seed.movement.scene_identity,next->placement_.pose,next->placement_.steering,{},{},{}};
        seed.fields.scene_child_word_4=0;
        next->inputs_.observe_control_sources(query.control);
        next->inputs_.scene_.previous_mode_5c=*query.previous_mode;
        next->inputs_.request_scene_mode(4,0);
        if(next->inputs_.capture_control()!=WorldMapControlStatus::Captured)return stop(S::RequiresEntrySources);
        WorldMapSceneModeCommit committed;
        if(next->inputs_.commit_scene_fields(0,uint8_t{0},&committed)!=WorldMapSceneFilterStatus::CommittedFields)return result;
        if(next->sources_.initialize(*next->player_,seed,WorldMapPlayerSourcesInitialization::Observed)!=WorldMapPlayerSourcesStatus::Initialized)return stop(S::AllocationFailure);
        WorldMapPreparedSceneObject player_object;player_object.scene.identity=seed.movement.scene_identity;
        player_object.scene.scene_type=1;player_object.scene.position=next->placement_.pose.position;player_object.positioned=true;
        if(!register_scene(player_object,scene))return result;
        WorldMapScenePositionUpdate player_positioned;
        if(!scene.update_pose(seed.movement.scene_identity,1,next->placement_.pose.position,next->placement_.pose.heading,&player_positioned))return result;
        WorldMapRegisteredCollisionObject self;self.collision.identity=seed.movement.scene_identity;self.collision.enabled=true;
        self.collision.category=1;self.collision.collision_flags=1;self.collision.radius=0.3f;
        self.world_position=self.collision.center_world=next->placement_.pose.position;self.heading_axis=next->placement_.pose.heading;
        if(!objects.register_object(WorldMapCollisionList::Third,self))return result;
        // The five other spatial owners precede 14358's mandatory actor.
        for(size_t i=0;i<query.spatial_objects.size();++i){const auto& object=query.spatial_objects[i];
            if(object.positioned!=(i!=0) || !add_key(object.scene.identity) || !register_scene(object,scene))return result;
        }
        if(query.mandatory_actor_id<0x2c || query.mandatory_actor_id>0x30 || !add_key(query.mandatory_actor.object.scene.identity))return result;
        WorldMapSceneFirstCollisionObjects mandatory;mandatory.category1_actor_id=query.mandatory_actor_id;
        mandatory.category1_actor=query.mandatory_actor.collision;
        if(!finite(mandatory.category1_actor.heading_axis) || mandatory.category1_actor.collision.identity!=query.mandatory_actor.object.scene.identity ||
            mandatory.category1_actor.collision.data || mandatory.category1_actor.collision.size)return result;
        if(!register_world_map_scene_first_collision_objects_from_assets(mandatory,query.collision->movement(),&objects))return result;
        const auto after_player=objects.snapshot();
        auto actor=query.mandatory_actor.object;actor.positioned=true;actor.scene.scene_type=1;
        actor.scene.position=after_player.first_resolver.front().center_world;
        actor.scene.context.facing_28=mandatory.category1_actor.heading_axis;
        if(!register_scene(actor,scene))return result;
        next->sources_.scene_=std::move(scene);next->sources_.objects_=std::move(objects);
        next->sources_.collision_=query.collision;next->sources_.probe_present_=true;next->camera_=query.camera;
        if(next->fade_.start_world_map_entry()!=WorldMapFadeStatus::Started)return result;
        auto control=query.control;control.phase->byte_8=uint8_t{0};next->inputs_.observe_control_sources(control);
        *out=std::move(next);return stop(S::AssembledCpuScene);
    }catch(const std::bad_alloc&){return stop(S::AllocationFailure);}
}
WorldMapCollisionSnapshot WorldMapSceneAssembly::collision_snapshot() const {
    auto snapshot=sources_.objects_->snapshot();
    for(auto& c:snapshot.third_resolver)if(c.identity==sources_.movement_.scene_identity)c.center_world=sources_.movement_.pose.position;
    for(auto& o:snapshot.third_objects)if(o.collision.identity==sources_.movement_.scene_identity){
        o.collision.center_world=o.world_position=sources_.movement_.pose.position;o.heading_axis=sources_.movement_.pose.heading;
    }
    return snapshot;
}
WorldMapPlayerSourcesStatus WorldMapSceneAssembly::observe_startup_camera(const WorldMapCameraViewQuery& view) noexcept {
    if(player_->camera())return WorldMapPlayerSourcesStatus::Busy;
    camera_=view;return WorldMapPlayerSourcesStatus::Completed;
}
WorldMapPlayerStartupTickResult WorldMapSceneAssembly::advance_startup(const WorldMapPlayerStartupTickInput& observations,WorldMapPlayerActorQueue* render){
    try {
    if(!player_->camera() && !sources_.collision_)return {WorldMapPlayerStartupTickStatus::InvalidInput};
    const auto& receipt=player_->primary_loop();const auto& clock=inputs_.game_clock()->state();
    const bool reused=receipt && receipt->valid && receipt->loop_count==clock.loop_count && receipt->raw_time==clock.raw_time && receipt->queued_completed;
    auto input=reused?WorldMapPlayerStartupTickInput{}:observations;
    input.parent.guards=sources_.fields_.guards;input.parent.scene_child_word_4=sources_.fields_.scene_child_word_4;
    input.parent.pose=sources_.movement_.pose;input.parent.player_node_identity=player_->update_node_identity();
    input.parent.action_148=sources_.updates_.saved_selection && sources_.updates_.saved_selection->action_148?
        std::optional<int32_t>{signed_word(*sources_.updates_.saved_selection->action_148)}:std::nullopt;
    input.parent.first_camera=player_->camera()?std::nullopt:std::optional<WorldMapPlayerStartupCameraSeed>{{sources_.collision_->camera(),camera_}};
    input.secondary.guards=sources_.fields_.guards;input.tertiary_guards=sources_.fields_.guards;
    input.secondary.saved_byte_14d35=sources_.fields_.saved_remap_byte_14d35;
    input.secondary.scene_mode_60=inputs_.scene().prior_mode_60;
    input.actor.actor.frame.actor_placement=WorldMapPlayerActorPlacement{sources_.movement_.pose.position,sources_.movement_.pose.heading};
    return player_->advance_state29_startup_tick(inputs_,input,&fade_,&sources_.updates_,render);
    }catch(const std::bad_alloc&){return {WorldMapPlayerStartupTickStatus::AllocationFailure};}
}
WorldMapPlayerState0TransitionResult WorldMapSceneAssembly::advance_state0_transition(std::optional<bool> item_binding,std::optional<bool> action_binding){
    WorldMapPlayerState0TransitionInput input;input.saved_byte_14d35=sources_.fields_.saved_remap_byte_14d35;
    input.secondary_byte_3f1=sources_.fields_.secondary_byte_3f1;input.scene_type_68=sources_.movement_.pose.scene_type;
    input.audio_byte_90=sources_.fields_.audio_byte_90;
    if(sources_.updates_.status)input.saved_subobject_byte_14c=sources_.updates_.status->byte_14c;
    if(sources_.updates_.saved_selection){input.item_144=sources_.updates_.saved_selection->item_144;
        if(sources_.updates_.saved_selection->action_148)input.action_148=signed_word(*sources_.updates_.saved_selection->action_148);}
    input.item_binding_1454=item_binding;input.action_binding_3ec=action_binding;
    return player_->advance_pending_state0_transition(inputs_,input,&fade_,&sources_.updates_);
}
WorldMapSceneEntryStatus WorldMapSceneAssembly::advance_entry(std::optional<uint8_t> secondary) noexcept {
    if(!entry_pending_)return WorldMapSceneEntryStatus::Completed;
    if(inputs_.queue_control().status!=WorldMapDispatchStatus::Ready)return WorldMapSceneEntryStatus::RequiresControl;
    if(!fade_.loop() || !fade_.loop()->valid)return WorldMapSceneEntryStatus::RequiresFade;
    const auto& clock=inputs_.game_clock()->state();
    if(fade_.state().mode_60==1){
        if(fade_.loop()->loop_count!=clock.loop_count || fade_.loop()->raw_time!=clock.raw_time)return WorldMapSceneEntryStatus::RequiresFade;
        return WorldMapSceneEntryStatus::WaitingForFade;
    }
    // Completed entry fade is no longer registered. Its accepted completion
    // remains evidence when the scene continuation runs in a later loop.
    if(!secondary)return WorldMapSceneEntryStatus::RequiresSecondaryByte;
    auto control=inputs_.sources();if(!control.phase)return WorldMapSceneEntryStatus::RequiresFade;
    if(!*secondary){control.phase->byte_8=uint8_t{1};inputs_.observe_control_sources(control);}
    entry_pending_=false;return *secondary?WorldMapSceneEntryStatus::Completed:WorldMapSceneEntryStatus::Released;
}
WorldMapPlayerSourcesStatus WorldMapSceneAssembly::observe_actor(uint64_t key,const std::array<float,3>& position,
    const std::array<float,3>& heading,bool enabled,const WorldMapPlayerContextMetadata& metadata){
    using S=WorldMapPlayerSourcesStatus;
    if(key==sources_.movement_.scene_identity || !sources_.scene_.object(key))return S::InvalidInput;
    try {
        auto objects=*sources_.objects_;auto scene=sources_.scene_;WorldMapScenePositionUpdate update;
        auto bound=metadata;bound.facing_28=heading;
        if(!objects.update_circle_object(WorldMapCollisionList::First,key,position,heading,enabled) ||
            !scene.update_position(key,position,&update) || !scene.set_context_metadata(key,bound))return S::InvalidInput;
        *sources_.objects_=std::move(objects);sources_.scene_=std::move(scene);return S::Completed;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
} // namespace awl
