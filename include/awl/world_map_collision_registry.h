#pragma once

#include "awl/world_map_contact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {

class WorldMapCollisionRecordPools;
class WorldMapCollisionAssets;

enum class WorldMapCollisionList : uint8_t { First, Later, Third };

// FUN_801527A8's resolver flags for verified object modes 0..4. Other
// modes are rejected even though the DOL returns a generic 0x40 fallback.
[[nodiscard]] bool world_map_collision_flags_for_mode(int32_t mode,
                                                       uint32_t* flags);

// One caller-supplied collision object. The registry owns this record, but
// not the COL bytes referenced by collision.data.
struct WorldMapRegisteredCollisionObject {
    CollisionDynamicPassObject collision{};
    std::array<float, 3> world_position{};
    std::array<float, 3> heading_axis{};
    uint32_t metadata = 0;
};

struct WorldMapCollisionSnapshot {
    // Keep this snapshot alive while a movement query points into its vectors.
    std::vector<CollisionDynamicPassObject> first_resolver;
    std::vector<WorldMapContactObject> first_directional;
    std::vector<CollisionDynamicPassObject> later_resolver;
    // Caller-supplied view for the bounded third-list pass. The alternate
    // source-flag-0x2 branch remains unsupported.
    std::vector<CollisionDynamicPassObject> third_resolver;
    std::vector<WorldMapRegisteredCollisionObject> third_objects;
};

// Native supplied-data equivalent of the three ordered collision lists used
// by FUN_8001DE44 and FUN_8001E498. Registration inserts at the front; moving
// an existing identity detaches its old entry first. No game objects or
// runtime asset providers are discovered here.
class WorldMapCollisionRegistry {
public:
    [[nodiscard]] bool register_object(
        WorldMapCollisionList list,
        const WorldMapRegisteredCollisionObject& object);
    // Refreshes a linked circle's virtual position/heading/active views
    // without moving its node. Callers supply the live owner fields and
    // take a new snapshot for later queries. Type-1 objects use a different
    // world-center transform and are rejected here.
    [[nodiscard]] bool update_circle_object(
        WorldMapCollisionList list, uint64_t identity,
        const std::array<float, 3>& world_position,
        const std::array<float, 3>& heading_axis, bool enabled);
    [[nodiscard]] bool unregister_object(uint64_t identity);
    void clear(WorldMapCollisionList list);
    [[nodiscard]] size_t size(WorldMapCollisionList list) const;
    [[nodiscard]] WorldMapCollisionSnapshot snapshot() const;

private:
    std::array<std::vector<WorldMapRegisteredCollisionObject>, 3> lists_{};
};

// FUN_80152218 configures the circular collision object used by the
// FUN_80159A28 actor. Unknown IDs take the DOL's default branch.
struct WorldMapFirstActorCircleSpec {
    int32_t mode = 1;
    float radius = 0.0f;
};

[[nodiscard]] WorldMapFirstActorCircleSpec
world_map_first_actor_circle_spec(int32_t actor_id);

// The world-map state starts at actor ID 0x2C. FUN_80013AE0 and
// FUN_8001307C refresh it from an already-produced RNG word modulo five.
// FUN_801595E0 maps that ID to the constructor's fourth argument. Neither
// RNG generation nor the game-state owner is connected here.
struct WorldMapFirstActorSelection {
    int32_t actor_id = 0x2c;
    int32_t base_constructor_id = 0x3d;
};

[[nodiscard]] WorldMapFirstActorSelection world_map_first_actor_selection(
    std::optional<uint32_t> refresh_rng_word);

// FUN_80159A28 selects an initial XYZ from the DOL table at 0x802558A8.
// Except for ID 0x30, FUN_8001CB54 replaces its Y. This low-level helper
// accepts that query result; the asset-backed registration below samples it
// from validated terrain. Failure leaves the output unchanged.
[[nodiscard]] bool world_map_first_actor_initial_position(
    int32_t actor_id, std::optional<float> height_query_result,
    std::array<float, 3>* position);

// FUN_80013F60 constructs two conditional first-list actors, then the
// FUN_80159A28 actor whose scene category is 1. Objects and optional-actor
// presence are supplied by the caller; only the final actor's verified
// circle setup and initial position are applied here. The supplied actor
// world_position is replaced by the constructor-derived position. The caller
// supplies the height-query result and actor heading; no live scene state is
// discovered.
struct WorldMapSceneFirstCollisionObjects {
    std::optional<WorldMapRegisteredCollisionObject> conditional_a;
    std::optional<WorldMapRegisteredCollisionObject> conditional_b;
    WorldMapRegisteredCollisionObject category1_actor{};
    int32_t category1_actor_id = 0;
    std::optional<float> category1_actor_height_query_result;
};

[[nodiscard]] bool register_world_map_scene_first_collision_objects(
    const WorldMapSceneFirstCollisionObjects& scene,
    WorldMapCollisionRegistry* registry);

// Uses the already-loaded slot-10 terrain to supply FUN_8001CB54's height
// result for the category-1 actor. The input scene's optional height is
// ignored. ID 0x30 still uses its fixed table Y. Failure is atomic.
[[nodiscard]] bool register_world_map_scene_first_collision_objects_from_assets(
    const WorldMapSceneFirstCollisionObjects& scene,
    const WorldMapCollisionAssets& assets,
    WorldMapCollisionRegistry* registry);

// FUN_80021440 reads eight records at state +0x123D0. The direct byte is at
// record +0x14 and the variant word is at +0x18. Callers decode those words
// and supply the separate state byte +0x299AE.
struct WorldMapFixedCollisionState {
    std::array<uint8_t, 8> direct_enabled{};
    std::array<uint32_t, 8> selected_variant{};
    uint8_t state_299ae = 0;
};

struct WorldMapFixedCollisionRecordKey {
    uint32_t archive_index = 0;
    int32_t category = 0;
};

// FUN_80021440's 25 roomobj.col.arc indices and runtime categories in
// construction order. These categories are 7 or 13, not the player's 1.
[[nodiscard]] std::array<WorldMapFixedCollisionRecordKey, 25>
world_map_fixed_collision_record_keys();

// Binds validated roomobj COL bytes and local centers for supplied objects.
// The pointers borrow the pool's storage. Identities, transforms, world
// centers/radii, and activation remain caller-owned. Failure is atomic.
[[nodiscard]] bool bind_world_map_fixed_collision_records(
    const WorldMapCollisionRecordPools& pools,
    std::array<WorldMapRegisteredCollisionObject, 25>* objects);

// In construction order: eight direct objects, eight variant-0 objects,
// eight variant-1 objects, and one final state-gated object.
[[nodiscard]] std::array<bool, 25> world_map_fixed_collision_activation(
    const WorldMapFixedCollisionState& state);

// Registers supplied fixed objects in FUN_80021440 order. The DOL adds flag
// 0x2 to each object and inserts it at the later-list head. Asset pointers,
// categories, matrices, and unique identities are supplied by the caller.
// Invalid identities leave the registry unchanged; this does not discover
// game-owned objects or connect scene loading.
[[nodiscard]] bool register_world_map_fixed_collision_objects(
    const WorldMapFixedCollisionState& state,
    const std::array<WorldMapRegisteredCollisionObject, 25>& objects,
    WorldMapCollisionRegistry* registry);

} // namespace awl
