#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace awl {

struct CollisionTreeAnalysis {
    uint8_t format = 0;
    uint8_t header_byte_5 = 0;
    uint8_t header_byte_6 = 0;
    uint8_t header_byte_7 = 0;
    size_t node_count = 0;
    size_t leaf_count = 0;
    size_t triangle_count = 0;
    size_t vertex_count = 0;
    size_t max_triangles_per_leaf = 0;
    size_t max_vertices_per_leaf = 0;
    uint32_t max_depth = 0;
    float coordinate_scale = 0.0f;
    std::array<float, 3> root_min{};
    std::array<float, 3> root_max{};
};

struct CollisionSurfaceSample {
    float height = 0.0f;
    uint16_t surface_flags = 0;
    uint32_t leaf_offset = 0;
    uint32_t triangle_index = 0;
};

struct CollisionEdgeSample {
    std::array<float, 3> position{};
    uint16_t surface_flags = 0;
    uint32_t leaf_offset = 0;
    uint32_t triangle_index = 0;
    uint8_t edge_index = 0;
    float distance_squared_xz = 0.0f;
};

struct CollisionTerrainAdjustment {
    std::array<float, 3> position{};
    uint16_t surface_flags = 0;
    bool used_edge_fallback = false;
};

// Validates the relocatable type-1 tree structure used by the verified
// movement collision assets, including their indexed triangle payloads.
[[nodiscard]] bool analyze_type1_collision_asset(
    const uint8_t* data,
    size_t size,
    CollisionTreeAnalysis* analysis);

// Selects the target leaf and returns the first triangle containing (x, z),
// matching the primary terrain-height branch observed in the target. The
// target's nearest-edge fallback and full movement resolver are not included.
[[nodiscard]] bool sample_type1_collision_surface(
    const uint8_t* data,
    size_t size,
    float x,
    float z,
    CollisionSurfaceSample* sample);

// Projects (x, z) onto the nearest triangle edge in the selected leaf and
// evaluates the selected triangle's height there. This is the target's
// FUN_801917FC type-1 fallback, not a movement collision resolver.
[[nodiscard]] bool project_type1_collision_to_edge(
    const uint8_t* data,
    size_t size,
    float x,
    float z,
    CollisionEdgeSample* sample);

// Isolates the height-enabled terrain lookup in FUN_8002009C: use the first
// containing triangle, or project to the nearest edge in the selected leaf.
// This does not apply the function's radius-aware passes or accept movement.
[[nodiscard]] bool adjust_type1_collision_terrain_height(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    CollisionTerrainAdjustment* adjustment);

} // namespace awl
