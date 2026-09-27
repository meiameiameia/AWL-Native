#pragma once

#include "awl/world_map_contact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

enum class WorldMapCollisionList : uint8_t { First, Later };

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
};

// Native supplied-data equivalent of the two ordered collision lists used by
// FUN_8001DE44 and FUN_8001E498. Registration inserts at the front; moving
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
    std::vector<WorldMapRegisteredCollisionObject> first_;
    std::vector<WorldMapRegisteredCollisionObject> later_;
};

} // namespace awl
