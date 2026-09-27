#pragma once

#include <array>
#include <cstdint>

namespace awl {

struct WorldMapScenePositionUpdate {
    std::array<float, 3> position{};
    uint8_t previous_bucket = 0;
    uint8_t next_bucket = 0;
    bool relink_required = false;
};

// Isolates FUN_800107A4's position and bucket decision for verified scene
// types 0..44. Type 0 uses bucket 0, types 1/2 use position bins 1..16,
// and types 3..44 use fixed buckets 17..58. This returns a plan only: it
// neither mutates an object nor performs the target's list operations.
[[nodiscard]] bool plan_world_map_scene_position_update(
    int32_t scene_type,
    uint8_t previous_bucket,
    const std::array<float, 3>& resolved_position,
    WorldMapScenePositionUpdate* update);

} // namespace awl
