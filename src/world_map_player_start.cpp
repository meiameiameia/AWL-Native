#include "awl/world_map_player_start.h"
#include <cmath>
#include <cstring>

namespace awl {
namespace {
bool valid_pose(const WorldMapPlayerScenePose& pose) {
    if (pose.scene_type < 0 || pose.scene_type > 44) return false;
    for (float value : pose.position) if (!std::isfinite(value)) return false;
    for (float value : pose.heading) if (!std::isfinite(value)) return false;
    return true;
}

float read_float(const uint8_t* bytes) {
    const uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}
} // namespace

bool decode_world_map_player_start_pose(const uint8_t* bytes, size_t size,
    int32_t scene_type, WorldMapPlayerScenePose* output) {
    constexpr size_t position_offset = 0x29C78;
    constexpr size_t heading_offset = 0x29F84;
    if (!bytes || !output || size < heading_offset + 12) return false;
    WorldMapPlayerScenePose next;
    next.scene_type = scene_type;
    for (size_t i = 0; i < 3; ++i) {
        next.position[i] = read_float(bytes + position_offset + i * 4);
        next.heading[i] = read_float(bytes + heading_offset + i * 4);
    }
    if (!valid_pose(next)) return false;
    *output = next;
    return true;
}

bool make_world_map_phase_entry_pose(int32_t scene_type,
    WorldMapPlayerScenePose* output) {
    // 8023E398: BF800000 00000000 C0A66666; 80251130: +Z.
    const WorldMapPlayerScenePose next{scene_type,
        {-1.0f, 0.0f, -0x1.4cccccp+2f}, {0.0f, 0.0f, 1.0f}};
    if (!output || !valid_pose(next)) return false;
    *output = next;
    return true;
}

WorldMapPlayerStartStatus prepare_world_map_player_start(
    const WorldMapPlayerStartQuery& query, WorldMapPlayerStart* output) {
    if (!output || !valid_pose(query.saved_pose))
        return WorldMapPlayerStartStatus::InvalidInput;
    if (query.secondary_byte_3f3 != 0)
        return WorldMapPlayerStartStatus::UnsupportedRelocation;
    if (query.placement.static_flags.secondary_3f3)
        return WorldMapPlayerStartStatus::InvalidInput;
    WorldMapPlayerStart next;
    next.pose = query.saved_pose;
    // Target/current speed and direction are the constructor's zero words;
    // facing comes from the saved heading, not the last movement direction.
    next.steering.facing_x = next.pose.heading[0];
    next.steering.facing_z = next.pose.heading[2];
    if (query.state_680 == -1 && query.state_58c == 0) {
        if (!resolve_type1_world_map_initial_placement(query.placement,
                next.pose.scene_type, next.pose.position, &next.placement))
            return WorldMapPlayerStartStatus::PlacementFailed;
        next.placement_called = true;
        next.pose.position = next.placement.position;
    }
    *output = next;
    return WorldMapPlayerStartStatus::Ready;
}
} // namespace awl
