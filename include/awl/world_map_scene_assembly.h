#pragma once
#include "awl/world_map_player_sources.h"
#include "awl/world_map_scene_constructors.h"

namespace awl {
// Prepared observations after untranslated actor/model constructors. Presence
// comes separately from saved IDs; a missing reached constructor is a stop.
struct WorldMapPreparedSceneObject {
    WorldMapSceneObject scene;
    bool positioned = false;
};
struct WorldMapPreparedSceneActor {
    WorldMapPreparedSceneObject object;
    WorldMapRegisteredCollisionObject collision;
};
struct WorldMapSceneAssemblyQuery {
    WorldMapControlSources control;
    WorldMapPlayerSourcesSeed player;
    WorldMapPlayerSourcesInitialization status_mode = WorldMapPlayerSourcesInitialization::Observed;
    WorldMapPlayerStartupQuery startup;
    std::shared_ptr<const WorldMapPlayerCollisionSources> collision;
    WorldMapCameraViewQuery camera;
    WorldMapFixedCollisionState fixed_state;
    // Identities/transforms/centers/radii are prepared observations. Bytes and
    // categories bind exclusively to the retained roomobj pool in DOL order.
    std::array<WorldMapRegisteredCollisionObject,25> fixed_objects{};
    std::optional<int32_t> saved_actor_a_29a64,saved_actor_b_29a60;
    std::optional<WorldMapPreparedSceneActor> actor_a,actor_b;
    // Scene +2C/+30/+34/+38/+3C, in order. The first remains unpositioned
    // at construction; the other four require their prepared final positions.
    std::array<WorldMapPreparedSceneObject,5> spatial_objects{};
    int32_t mandatory_actor_id = 0x2c;
    WorldMapPreparedSceneActor mandatory_actor;
    // 1456C's sequence route, sampled after object construction. Reached
    // sequence startup remains unsupported; no script completion is invented.
    std::optional<int32_t> previous_mode;
    std::optional<uint8_t> sequence_request_byte;
};
enum class WorldMapSceneAssemblyStatus {
    AssembledCpuScene,InvalidInput,RequiresStatus,RequiresActorPresence,
    RequiresPreparedActor,RequiresRecordPools,WrongCollisionSelection,
    PlacementIncomplete,PlayerIncomplete,RequiresEntrySources,SequenceRequired,
    UnsupportedEntryGate,AllocationFailure,RequiresConstructorSources,ConstructorIncomplete,RequiresSceneCommit,RequiresControl
};
struct WorldMapSceneAssemblyResult {
    WorldMapSceneAssemblyStatus status = WorldMapSceneAssemblyStatus::InvalidInput;
    std::optional<WorldMapPlayerStartStatus> placement;
    std::optional<WorldMapPlayerStartupResult> player;
    std::optional<WorldMapSceneConstructorStatus> constructors;
};
enum class WorldMapSceneEntryStatus { WaitingForFade,RequiresSecondaryByte,Released,Completed,RequiresFade,RequiresControl };

// Bounded CPU assembly of 13F60, not original cold boot or scene activation.
// Captures the supplied entry control once, before scene commit/fade clear.
// Owns stable input/status/queue storage through 29 -> 0 -> 1. External
// producers still supply scripts, visibility, context metadata and actor AI.
// The caller keeps a borrowed render queue alive through callback completion
// and drains this scene's scheduled render nodes before replacing/destroying it.
class WorldMapSceneAssembly {
public:
    [[nodiscard]] static WorldMapSceneAssemblyResult create(
        const WorldMapPlayerStartupProviders&,const WorldMapSceneAssemblyQuery&,
        std::unique_ptr<WorldMapSceneAssembly>* out);
    // Source-driven CPU adapter: borrow the existing captured/
    // committed manager, derive all object results from DOL/terrain/current
    // saved fields, and allocate scene-owned opaque keys. No supplied object
    // transforms, spatial metadata, camera result or previous-mode override.
    // Inputs must outlive the scene. The state owner stays current and retained.
    [[nodiscard]] static WorldMapSceneAssemblyResult create_from_sources(
        const WorldMapPlayerStartupProviders&,const WorldMapPlayerSourcesSeed&,
        const WorldMapPlayerStartupQuery&,WorldMapUpdateInputs&,
        std::shared_ptr<const WorldMapSceneConstructorTables>,
        std::shared_ptr<WorldMapSceneConstructorState>,
        std::shared_ptr<const WorldMapPlayerCollisionSources>,
        std::unique_ptr<WorldMapSceneAssembly>* out);
    WorldMapUpdateInputs& inputs() noexcept {return inputs_;}
    WorldMapPlayerSources& sources() noexcept {return sources_;}
    const WorldMapPlayerSources& sources() const noexcept {return sources_;}
    WorldMapPlayerStartup& player() noexcept {return *player_;}
    const WorldMapPlayerStartup& player() const noexcept {return *player_;}
    const WorldMapFade& fade() const noexcept {return fade_;}
    const WorldMapPlayerStart& initial_placement() const noexcept {return placement_;}
    [[nodiscard]] WorldMapCollisionSnapshot collision_snapshot() const;
    // Supplied secondary observations are sampled afresh. Owned guards,
    // selection, scene mode, pose and camera always supersede replacements.
    [[nodiscard]] WorldMapPlayerStartupTickResult advance_startup(
        const WorldMapPlayerStartupTickInput&,WorldMapPlayerActorQueue*);
    // Correct an untranslated initial camera observation before it is bound.
    [[nodiscard]] WorldMapPlayerSourcesStatus observe_startup_camera(const WorldMapCameraViewQuery&) noexcept;
    [[nodiscard]] WorldMapPlayerState0TransitionResult advance_state0_transition(
        std::optional<bool> item_binding,std::optional<bool> action_binding);
    // Invoke at the scene's own continuation point. A change to phase +8
    // never replaces control already captured for this loop. +49 can block
    // enable, but still consumes the original one-shot continuation.
    [[nodiscard]] WorldMapSceneEntryStatus advance_entry(std::optional<uint8_t> secondary_49) noexcept;
    [[nodiscard]] WorldMapSceneEntryStatus advance_entry() noexcept;
    // Refresh both linked virtual views without re-registering/reordering.
    [[nodiscard]] WorldMapPlayerSourcesStatus observe_actor(uint64_t identity,
        const std::array<float,3>& position,const std::array<float,3>& heading,bool enabled,
        const WorldMapPlayerContextMetadata&);
private:
    explicit WorldMapSceneAssembly(std::shared_ptr<GameClock> clock)
        : owned_inputs_(std::make_unique<WorldMapUpdateInputs>(clock)),inputs_(*owned_inputs_),sources_(inputs_),fade_(std::move(clock)) {}
    explicit WorldMapSceneAssembly(WorldMapUpdateInputs& inputs) noexcept
        : inputs_(inputs),sources_(inputs_),fade_(inputs.game_clock()) {}
    static WorldMapSceneAssemblyResult assemble(const WorldMapPlayerStartupProviders&,
        const WorldMapSceneAssemblyQuery&,std::unique_ptr<WorldMapSceneAssembly>,bool borrowed,
        std::unique_ptr<WorldMapSceneAssembly>*);
    std::unique_ptr<WorldMapUpdateInputs> owned_inputs_;
    WorldMapUpdateInputs& inputs_;
    std::unique_ptr<WorldMapPlayerStartup> player_;
    WorldMapPlayerSources sources_;
    WorldMapFade fade_;
    WorldMapPlayerStart placement_;
    WorldMapCameraViewQuery camera_;
    bool entry_pending_ = true;
    std::array<uint8_t,33> constructor_keys_{};
    std::shared_ptr<const WorldMapSceneConstructorTables> constructor_tables_;
    std::shared_ptr<WorldMapSceneConstructorState> constructor_state_;
};
} // namespace awl
