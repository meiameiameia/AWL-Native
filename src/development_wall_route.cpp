#include "awl/development_wall_route.h"

#include <cmath>
#include <cstdint>

namespace awl {

bool derive_development_wall_route(const WorldMapCollisionAssets& assets,
                                   DevelopmentWallRoute* route) {
    if (route == nullptr) {
        return false;
    }
    const auto& bytes = assets.static_bytes();
    const auto& analysis = assets.static_analysis();
    CollisionEdgeSample edge;
    const float center_z = (analysis.root_min[2] + analysis.root_max[2]) * 0.5f;
    if (!project_type1_collision_to_edge(
            bytes.data(), bytes.size(), analysis.root_min[0] - 1.0f,
            center_z, &edge) || (edge.surface_flags & 0xC1u) == 0 ||
        edge.edge_index >= 3 || edge.leaf_offset > bytes.size() ||
        bytes.size() - edge.leaf_offset < 0x34) {
        return false;
    }
    const auto be16 = [&bytes](size_t offset) {
        return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
                                     bytes[offset + 1]);
    };
    const auto be32 = [&bytes](size_t offset) {
        return (static_cast<uint32_t>(bytes[offset]) << 24) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
               bytes[offset + 3];
    };
    const size_t triangles = edge.leaf_offset + be32(edge.leaf_offset + 0x28);
    const size_t vertices = edge.leaf_offset + be32(edge.leaf_offset + 0x2c);
    if (triangles > bytes.size() ||
        edge.triangle_index >= (bytes.size() - triangles) / 8 ||
        vertices > bytes.size()) {
        return false;
    }
    const size_t record = triangles + static_cast<size_t>(edge.triangle_index) * 8;
    const uint16_t start_index = be16(record + 2 + edge.edge_index * 2);
    const uint16_t end_index = be16(record + 2 + ((edge.edge_index + 1) % 3) * 2);
    if (start_index >= (bytes.size() - vertices) / 8 ||
        end_index >= (bytes.size() - vertices) / 8) {
        return false;
    }
    const auto signed_coordinate = [&be16](size_t offset) {
        const int32_t raw = be16(offset);
        return raw >= 32768 ? raw - 65536 : raw;
    };
    const size_t start = vertices + static_cast<size_t>(start_index) * 8;
    const size_t end = vertices + static_cast<size_t>(end_index) * 8;
    const float x0 = signed_coordinate(start) * analysis.coordinate_scale;
    const float z0 = signed_coordinate(start + 4) * analysis.coordinate_scale;
    const float x1 = signed_coordinate(end) * analysis.coordinate_scale;
    const float z1 = signed_coordinate(end + 4) * analysis.coordinate_scale;
    const float dx = x1 - x0;
    const float dz = z1 - z0;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(length) || length <= 0.0f) {
        return false;
    }
    DevelopmentWallRoute derived;
    derived.mid_x = (x0 + x1) * 0.5f;
    derived.mid_z = (z0 + z1) * 0.5f;
    // Verified FUN_8017C918 edge-plane orientation for this mode-zero edge.
    derived.normal_x = dz / length;
    derived.normal_z = -dx / length;
    derived.start = {derived.mid_x + derived.normal_x, 0.0f,
                     derived.mid_z + derived.normal_z};
    CollisionSurfaceSample surface;
    if (sample_type1_collision_surface(
            assets.terrain_bytes().data(), assets.terrain_bytes().size(),
            derived.start[0], derived.start[2], &surface)) {
        derived.start[1] = surface.height;
    } else {
        CollisionEdgeSample terrain_edge;
        if (!project_type1_collision_to_edge(
                assets.terrain_bytes().data(), assets.terrain_bytes().size(),
                derived.start[0], derived.start[2], &terrain_edge)) {
            return false;
        }
        derived.start[1] = terrain_edge.position[1];
    }
    if (!std::isfinite(derived.start[1])) {
        return false;
    }
    *route = derived;
    return true;
}

} // namespace awl
