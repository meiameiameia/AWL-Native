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

} // namespace awl
