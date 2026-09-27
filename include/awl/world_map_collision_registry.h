#pragma once

#include "awl/world_map_contact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

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
    [[nodiscard]] bool unregister_object(uint64_t identity);
    void clear(WorldMapCollisionList list);
    [[nodiscard]] size_t size(WorldMapCollisionList list) const;
    [[nodiscard]] WorldMapCollisionSnapshot snapshot() const;

private:
    std::array<std::vector<WorldMapRegisteredCollisionObject>, 3> lists_{};
};

// FUN_80021440 reads eight records at state +0x123D0. The direct byte is at
// record +0x14 and the variant word is at +0x18. Callers decode those words
// and supply the separate state byte +0x299AE.
struct WorldMapFixedCollisionState {
    std::array<uint8_t, 8> direct_enabled{};
    std::array<uint32_t, 8> selected_variant{};
    uint8_t state_299ae = 0;
};

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
