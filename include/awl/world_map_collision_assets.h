#pragma once

#include "awl/collision_asset.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapCollisionAssetPaths {
    const char* terrain = nullptr;
    const char* static_objects = nullptr;
};

// Raw inputs read by FUN_800209A0 from the state owners at r13-0x57A8
// (first word) and state +0x299A4. Their live ownership is not translated.
struct WorldMapCollisionSourceState {
    uint32_t phase_counter_word = 0;
    uint8_t terrain_variant_byte = 0;
};

struct WorldMapCollisionSelection {
    uint32_t phase_index = 0;
    bool alternate_terrain = false;
};

// FUN_8000FFB0 -> FUN_8000FEE8's bounded phase calculation and the
// terrain-table byte used by FUN_800209A0. Values above 1 for the terrain
// byte are unsupported. Failure clears the output.
[[nodiscard]] bool derive_world_map_collision_selection(
    const WorldMapCollisionSourceState& source,
    WorldMapCollisionSelection* selection);

// FUN_800209A0's supported terrain and slot-2 static COL selectors. The
// phase index comes from FUN_8000FEE8 (0..5); the terrain switch is the
// caller-supplied byte read at runtime state +0x299A4.
[[nodiscard]] bool select_world_map_collision_asset_paths(
    uint32_t phase_index,
    bool alternate_terrain,
    WorldMapCollisionAssetPaths* paths);

// Owns validated type-1 COL bytes read through the existing disc mount.
// A bound query borrows these bytes: clear(), load(), or destruction
// invalidates its pointers. The object-record pools and dynamic lists are separate.
class WorldMapCollisionAssets {
public:
    WorldMapCollisionAssets() = default;
    WorldMapCollisionAssets(const WorldMapCollisionAssets&) = delete;
    WorldMapCollisionAssets& operator=(const WorldMapCollisionAssets&) = delete;
    WorldMapCollisionAssets(WorldMapCollisionAssets&&) = delete;
    WorldMapCollisionAssets& operator=(WorldMapCollisionAssets&&) = delete;

    [[nodiscard]] bool load(uint32_t phase_index, bool alternate_terrain);
    [[nodiscard]] bool load_from_source_state(
        const WorldMapCollisionSourceState& source);
    void clear();
    [[nodiscard]] bool bind(CollisionCategory1MovementQuery* query) const;

    [[nodiscard]] const WorldMapCollisionAssetPaths& paths() const {
        return paths_;
    }
    [[nodiscard]] const std::vector<uint8_t>& terrain_bytes() const {
        return terrain_;
    }
    [[nodiscard]] const std::vector<uint8_t>& static_bytes() const {
        return static_objects_;
    }
    [[nodiscard]] const CollisionTreeAnalysis& terrain_analysis() const {
        return terrain_analysis_;
    }
    [[nodiscard]] const CollisionTreeAnalysis& static_analysis() const {
        return static_analysis_;
    }

private:
    WorldMapCollisionAssetPaths paths_{};
    std::vector<uint8_t> terrain_;
    std::vector<uint8_t> static_objects_;
    CollisionTreeAnalysis terrain_analysis_{};
    CollisionTreeAnalysis static_analysis_{};
};

} // namespace awl
