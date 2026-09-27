#pragma once

#include "awl/world_map_collision_assets.h"

#include <array>

namespace awl {

// Data-derived fixture for a local, validated static edge. This is not
// game-owned player spawn or scene selection.
struct DevelopmentWallRoute {
    std::array<float, 3> start{};
    float mid_x = 0.0f;
    float mid_z = 0.0f;
    float normal_x = 0.0f;
    float normal_z = 0.0f;

    [[nodiscard]] float signed_distance(const std::array<float, 3>& p) const {
        return (p[0] - mid_x) * normal_x +
               (p[2] - mid_z) * normal_z;
    }
};

[[nodiscard]] bool derive_development_wall_route(
    const WorldMapCollisionAssets& assets, DevelopmentWallRoute* route);

} // namespace awl
