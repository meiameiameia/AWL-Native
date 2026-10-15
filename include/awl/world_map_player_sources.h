#pragma once
#include "awl/world_map_player_startup.h"
#include "awl/world_map_collision_assets.h"
#include "awl/world_map_collision_records.h"
#include "awl/world_map_collision_registry.h"
#include "awl/world_map_trigger_asset.h"
#include <memory>
#include <optional>
#include <utility>

namespace awl {
enum class WorldMapPlayerSourcesInitialization { Fresh, Observed };
enum class WorldMapPlayerSourcesStatus {
    Completed,Initialized,AlreadyInitialized,CallbackIncomplete,RequiresInitialization,
    RequiresControl,RequiresPhase,RequiresVariant,RequiresCollision,RequiresCollisionLists,
    RequiresStaticFlags,WrongCollisionSelection,RequiresCamera,RequiresRecordPool,
    UnsupportedState,ForeignPlayer,Busy,InvalidInput,AllocationFailure,AssetFailure
};
// Sealed providers: loading stages every resource before replacing *out.
// Immutable native views replace original slot binding/in-place COL relocation.
// The existing retained camera owner must not be reloaded through another alias.
class WorldMapPlayerCollisionSources {
public:
    [[nodiscard]] static WorldMapPlayerSourcesStatus load(
        const WorldMapCollisionSourceState&,std::shared_ptr<const WorldMapCameraCollisionAsset>,
        bool load_object_pools,std::shared_ptr<const WorldMapPlayerCollisionSources>* out);
    const WorldMapCollisionSelection& selection() const noexcept {return selection_;}
    const WorldMapCollisionAssets& movement() const noexcept {return movement_;}
    const std::shared_ptr<const WorldMapCameraCollisionAsset>& camera() const noexcept {return camera_;}
private:
    WorldMapPlayerCollisionSources()=default;
    WorldMapCollisionSelection selection_;
    WorldMapCollisionAssets movement_;
    std::shared_ptr<const WorldMapCameraCollisionAsset> camera_;
    WorldMapTriggerAsset triggers_;
    WorldMapPlayerTerrainProbeAsset probe_;
    std::optional<WorldMapCollisionRecordPools> pools_;
    friend class WorldMapPlayerSources;
    friend class WorldMapSceneAssembly;
};
struct WorldMapPlayerSourceFields {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    std::optional<int32_t> scene_child_word_4;
    std::optional<uint8_t> terrain_variant_299a4,secondary_byte_3f1;
    std::optional<CollisionCategory1StaticFlags> static_flags;
    std::optional<uint8_t> saved_remap_byte_14d35,audio_byte_90;
    std::optional<WorldMapPlayerStartItemType> item_type;
};
struct WorldMapPlayerSourcesSeed {
    WorldMapPlayerUpdateOwners updates;
    WorldMapPlayerMovementState movement;
    WorldMapPlayerSourceFields fields;
};
struct WorldMapPlayerSourcesResult {
    WorldMapPlayerSourcesStatus status=WorldMapPlayerSourcesStatus::InvalidInput;
    std::optional<WorldMapPlayerState0PrimarySliceResult> state0_primary;
    std::optional<WorldMapPlayerState1PrimarySliceResult> state1_primary;
    std::optional<WorldMapPlayerState0SecondarySliceResult> state0_secondary;
    std::optional<WorldMapPlayerState1SecondaryResult> state1_secondary;
    std::optional<WorldMapPlayerState1TransitionResult> transition;
    std::optional<WorldMapPlayerState0ThirdResult> third;
    std::optional<WorldMapPlayerStartupQueueResult> queued;
};
// One nonmovable owner for both normal state paths. Inputs and player must
// outlive use; the existing input owner supplies phase/scene/PAD/control and
// is never copied. No raw pointer from a save or original address is followed.
// Fresh initialization creates only the traced status/selection/guard projection;
// it does not construct a scene, run predecessor scripts, restore a save, choose
// a spawn, capture input, advance time, or activate the executable.
class WorldMapPlayerSources {
public:
    explicit WorldMapPlayerSources(WorldMapUpdateInputs& inputs) noexcept : inputs_(inputs) {}
    WorldMapPlayerSources(const WorldMapPlayerSources&)=delete;
    WorldMapPlayerSources& operator=(const WorldMapPlayerSources&)=delete;
    WorldMapPlayerSources(WorldMapPlayerSources&&)=delete;
    WorldMapPlayerSources& operator=(WorldMapPlayerSources&&)=delete;
    [[nodiscard]] WorldMapPlayerSourcesStatus initialize(
        const WorldMapPlayerStartup&,const WorldMapPlayerSourcesSeed&,WorldMapPlayerSourcesInitialization);
    const WorldMapPlayerUpdateOwners& updates() const noexcept {return updates_;}
    const WorldMapPlayerMovementState& movement() const noexcept {return movement_;}
    const WorldMapSceneBucketRegistry& scene() const noexcept {return scene_;}
    const WorldMapPlayerSourceFields& fields() const noexcept {return fields_;}
    const std::shared_ptr<const WorldMapPlayerCollisionSources>& collision() const noexcept {return collision_;}
    std::optional<bool> probe_present() const noexcept {return probe_present_;}
    // Explicit producer observations. Replaces the entire supplied projection;
    // unknown fields stay unknown, even after an earlier callback read them.
    void observe_fields(const WorldMapPlayerSourceFields& fields) noexcept {fields_=fields;}
    void observe_status(const std::optional<WorldMapPlayerStatusState>& status) noexcept {updates_.status=status;}
    void observe_selection(const WorldMapPlayerSavedSelection& selection) noexcept {updates_.saved_selection=selection;}
    [[nodiscard]] WorldMapPlayerSourcesStatus install_collision(std::shared_ptr<const WorldMapPlayerCollisionSources>);
    [[nodiscard]] WorldMapPlayerSourcesStatus clear_collision() noexcept;
    // Ordinary slot-five clear/bind, independent of retained resource lifetime.
    void clear_probe() noexcept {probe_present_=false;}
    [[nodiscard]] bool bind_probe() noexcept;
    // Explicit reset establishes empty native lists, not a populated original
    // scene. Snapshots live through each synchronous movement transaction.
    void reset_collision_lists() noexcept {objects_.emplace();}
    // External raw geometry is rejected. Type-one objects bind a retained pool
    // record using group/index; circles need no geometry owner.
    [[nodiscard]] WorldMapPlayerSourcesStatus register_collision_object(
        WorldMapCollisionList,const WorldMapRegisteredCollisionObject&,
        std::optional<std::pair<int32_t,uint32_t>> record=std::nullopt);
    [[nodiscard]] bool unregister_collision_object(uint64_t) noexcept;
    [[nodiscard]] WorldMapPlayerSourcesResult advance_primary(WorldMapPlayerStartup&);
    [[nodiscard]] WorldMapPlayerSourcesResult advance_secondary(WorldMapPlayerStartup&);
    [[nodiscard]] WorldMapPlayerSourcesResult advance_transition(WorldMapPlayerStartup&);
    // Context resources/untranslated predicates remain supplied; owned guard,
    // item/action/pose/contact/secondary fields override caller observations.
    [[nodiscard]] WorldMapPlayerSourcesResult advance_third(WorldMapPlayerStartup&,
        const WorldMapPlayerContextUpdateSources&,WorldMapPlayerState0ThirdInput);
    [[nodiscard]] WorldMapPlayerSourcesResult consume_queued(WorldMapPlayerStartup&,
        const WorldMapPlayerStartupQueuedActorInput&,WorldMapPlayerActorQueue*);
private:
    friend class WorldMapSceneAssembly;
    WorldMapPlayerSourcesStatus check(const WorldMapPlayerStartup&) const noexcept;
    WorldMapPlayerSourcesStatus prepare_movement(const WorldMapPlayerStartup&,
        WorldMapCollisionSnapshot&,WorldMapMovementQuery&,WorldMapPlayerMovementInput&) const;
    bool collision_busy() const noexcept;
    void retain_collision_use() noexcept;
    WorldMapUpdateInputs& inputs_;
    uint64_t player_identity_=0;
    WorldMapPlayerUpdateOwners updates_;
    WorldMapPlayerMovementState movement_;
    WorldMapSceneBucketRegistry scene_;
    WorldMapPlayerSourceFields fields_;
    std::shared_ptr<const WorldMapPlayerCollisionSources> collision_;
    std::optional<bool> probe_present_;
    std::optional<WorldMapCollisionRegistry> objects_;
    std::optional<std::pair<uint32_t,uint32_t>> collision_loop_;
};
} // namespace awl
