#pragma once

#include "awl/collision_asset.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace awl {

// Caller-supplied view of one FUN_8001DE44 list entry. The game owns the
// actual list, virtual providers, and nested metadata record.
struct WorldMapContactObject {
    bool enabled = false;
    int32_t category = 0;
    uint32_t collision_flags = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    CollisionDynamicObjectQuery contact_query{};
    std::array<float, 3> world_position{};
    std::array<float, 3> heading_axis{};
    uint32_t metadata = 0;
};

struct WorldMapContactResult {
    bool matched = false;
    uint8_t direction_code = 7;
    uint32_t metadata = 0;
    std::array<float, 3> world_position{};
    std::array<float, 3> heading_axis{};
    size_t queried_objects = 0;
    size_t contacting_objects = 0;
};

// Composes FUN_8001DE44's ordered contact/heading search and FUN_8003083C's
// miss fallback. Only caller-supplied type-1 objects are supported; this
// neither discovers runtime objects nor accepts a player position.
[[nodiscard]] bool query_world_map_directional_contact(
    const WorldMapContactObject* objects,
    size_t object_count,
    int32_t required_category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    const std::array<float, 3>& fallback_axis,
    WorldMapContactResult* result);

} // namespace awl
