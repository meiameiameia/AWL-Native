#include "awl/world_map_scene_index.h"

#include <cmath>

namespace awl {

namespace {

uint8_t bin_coordinate(float coordinate,
                       float first,
                       float second,
                       float third) {
    if (coordinate < first) {
        return 0;
    }
    if (coordinate < second) {
        return 1;
    }
    if (coordinate < third) {
        return 2;
    }
    return 3;
}

} // namespace

bool plan_world_map_scene_position_update(
    int32_t scene_type,
    uint8_t previous_bucket,
    const std::array<float, 3>& resolved_position,
    WorldMapScenePositionUpdate* update) {
    if (update != nullptr) {
        *update = {};
    }
    if (update == nullptr || scene_type < 0 || scene_type > 44 ||
        previous_bucket > 58 || !std::isfinite(resolved_position[0]) ||
        !std::isfinite(resolved_position[1]) ||
        !std::isfinite(resolved_position[2])) {
        return false;
    }

    WorldMapScenePositionUpdate candidate;
    candidate.position = resolved_position;
    candidate.previous_bucket = previous_bucket;
    if (scene_type == 1 || scene_type == 2) {
        // FUN_8001153C uses >= at each threshold. The third X constant has
        // DOL float bits 0x4318F9E9; the 4x4 table is 1..16.
        const uint8_t x_bin = bin_coordinate(
            resolved_position[0], 54.0f, 99.0f, 152.97621f);
        const uint8_t z_bin = bin_coordinate(
            resolved_position[2], 64.0f, 130.0f, 194.0f);
        candidate.next_bucket = static_cast<uint8_t>(1 + x_bin * 4 + z_bin);
    } else if (scene_type >= 3) {
        // The verified 42-pair table at 0x8023DFE8 maps 3..44 to 17..58.
        candidate.next_bucket = static_cast<uint8_t>(scene_type + 14);
    }
    candidate.relink_required = candidate.next_bucket != previous_bucket;
    *update = candidate;
    return true;
}

} // namespace awl
