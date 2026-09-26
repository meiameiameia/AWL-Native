#include "awl/collision_asset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

namespace awl {

namespace {

constexpr uint32_t kCollisionFileMarker = 0xE7E3F1F4u;
constexpr uint8_t kSupportedFormat = 1;
constexpr uint32_t kRootOffset = 8;
constexpr uint32_t kNodeSize = 0x34;
constexpr uint32_t kTriangleStride = 8;
constexpr uint32_t kVertexStride = 8;
constexpr std::array<uint32_t, 4> kChildFields = {0x0c, 0x10, 0x14, 0x18};
constexpr std::array<uint32_t, 3> kNodeRelativeFields = {0x28, 0x2c, 0x30};

uint16_t read_be16(const uint8_t* bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                 static_cast<uint16_t>(bytes[1]));
}

int16_t read_be_s16(const uint8_t* bytes) {
    const uint16_t raw = read_be16(bytes);
    const int32_t value = raw < 0x8000u ? static_cast<int32_t>(raw)
                                        : static_cast<int32_t>(raw) - 0x10000;
    return static_cast<int16_t>(value);
}

uint32_t read_be32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

bool contains_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

bool add_count(size_t& total, uint32_t count) {
    if (count > std::numeric_limits<size_t>::max() - total) {
        return false;
    }
    total += count;
    return true;
}

bool valid_bounds(const uint8_t* node) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (read_be_s16(node + axis * 2) >
            read_be_s16(node + 6 + axis * 2)) {
            return false;
        }
    }
    return true;
}

float decode_coordinate(const uint8_t* bytes, float scale) {
    return static_cast<float>(read_be_s16(bytes)) * scale;
}

struct DecodedVertex {
    float x;
    float y;
    float z;
};

DecodedVertex decode_vertex(const uint8_t* vertex, float scale) {
    return {decode_coordinate(vertex, scale),
            decode_coordinate(vertex + 2, scale),
            decode_coordinate(vertex + 4, scale)};
}

bool contains_xz(const std::array<DecodedVertex, 3>& triangle,
                 float x,
                 float z) {
    const float edge0 = (triangle[1].x - x) * (triangle[2].z - z) -
                        (triangle[1].z - z) * (triangle[2].x - x);
    const float edge1 = (triangle[2].x - x) * (triangle[0].z - z) -
                        (triangle[2].z - z) * (triangle[0].x - x);
    const float edge2 = (triangle[0].x - x) * (triangle[1].z - z) -
                        (triangle[0].z - z) * (triangle[1].x - x);
    return (edge0 >= 0.0f && edge1 >= 0.0f && edge2 >= 0.0f) ||
           (edge0 <= 0.0f && edge1 <= 0.0f && edge2 <= 0.0f);
}

bool interpolate_height(const std::array<DecodedVertex, 3>& triangle,
                        float x,
                        float z,
                        float& height) {
    const float x01 = triangle[0].x - triangle[1].x;
    const float x12 = triangle[1].x - triangle[2].x;
    const float y01 = triangle[0].y - triangle[1].y;
    const float y12 = triangle[1].y - triangle[2].y;
    const float z01 = triangle[0].z - triangle[1].z;
    const float z12 = triangle[1].z - triangle[2].z;
    const float denominator = z01 * x12 - x01 * z12;
    if (denominator == 0.0f) {
        return false;
    }
    height = triangle[0].y +
             ((triangle[0].x - x) * (y01 * z12 - z01 * y12) +
              (triangle[0].z - z) * (x01 * y12 - y01 * x12)) /
                 denominator;
    return std::isfinite(height);
}

uint32_t select_leaf(const uint8_t* data,
                     float scale,
                     float x,
                     float z) {
    uint32_t node_offset = kRootOffset;
    while (read_be32(data + node_offset + kChildFields.front()) != 0) {
        const uint8_t* node = data + node_offset;
        const float split_x =
            static_cast<float>(static_cast<int32_t>(read_be_s16(node)) +
                               static_cast<int32_t>(read_be_s16(node + 6))) *
            scale * 0.5f;
        const float split_z =
            static_cast<float>(static_cast<int32_t>(read_be_s16(node + 4)) +
                               static_cast<int32_t>(read_be_s16(node + 10))) *
            scale * 0.5f;
        const size_t child = (x >= split_x ? 1u : 0u) +
                             (z >= split_z ? 2u : 0u);
        node_offset = read_be32(node + kChildFields[child]);
    }
    return node_offset;
}

std::array<DecodedVertex, 3> decode_triangle(const uint8_t* data,
                                             size_t vertex_offset,
                                             const uint8_t* record,
                                             float scale) {
    std::array<DecodedVertex, 3> triangle;
    for (size_t vertex = 0; vertex < triangle.size(); ++vertex) {
        const uint16_t vertex_index = read_be16(record + 2 + vertex * 2);
        triangle[vertex] =
            decode_vertex(data + vertex_offset +
                              static_cast<size_t>(vertex_index) * kVertexStride,
                          scale);
    }
    return triangle;
}

struct LeafPayload {
    size_t triangles;
    size_t vertices;
    uint32_t triangle_count;
};

LeafPayload leaf_payload(const uint8_t* data, uint32_t node_offset) {
    const uint8_t* node = data + node_offset;
    return {static_cast<size_t>(node_offset) + read_be32(node + 0x28),
            static_cast<size_t>(node_offset) + read_be32(node + 0x2c),
            read_be32(node + 0x1c)};
}

struct EdgeProjection {
    float x;
    float z;
    float distance_squared;
};

EdgeProjection project_to_edge(const DecodedVertex& start,
                               const DecodedVertex& end,
                               float x,
                               float z) {
    const float dx = end.x - start.x;
    const float dz = end.z - start.z;
    const float length_squared = dx * dx + dz * dz;
    float projected_x = start.x;
    float projected_z = start.z;
    if (length_squared != 0.0f) {
        const float along = std::clamp(
            (dx * (x - start.x) + dz * (z - start.z)) / length_squared,
            0.0f, 1.0f);
        projected_x = start.x + dx * along;
        projected_z = start.z + dz * along;
    }
    const float offset_x = projected_x - x;
    const float offset_z = projected_z - z;
    return {projected_x, projected_z,
            offset_x * offset_x + offset_z * offset_z};
}

} // namespace

bool analyze_type1_collision_asset(const uint8_t* data,
                                   size_t size,
                                   CollisionTreeAnalysis* analysis) {
    if (analysis != nullptr) {
        *analysis = {};
    }
    if (data == nullptr || analysis == nullptr ||
        !contains_range(size, kRootOffset, kNodeSize) ||
        read_be32(data) != kCollisionFileMarker ||
        data[4] != kSupportedFormat || data[5] > 30) {
        return false;
    }

    struct PendingNode {
        uint32_t offset;
        uint32_t depth;
    };

    std::vector<PendingNode> pending{{kRootOffset, 0}};
    std::unordered_set<uint32_t> visited;
    size_t leaf_count = 0;
    size_t triangle_total = 0;
    size_t vertex_total = 0;
    size_t max_triangles = 0;
    size_t max_vertices = 0;
    uint32_t max_depth = 0;

    while (!pending.empty()) {
        const PendingNode node = pending.back();
        pending.pop_back();

        if ((node.offset & 3u) != 0 ||
            !contains_range(size, node.offset, kNodeSize) ||
            !visited.insert(node.offset).second ||
            !valid_bounds(data + node.offset)) {
            return false;
        }
        if (node.depth > max_depth) {
            max_depth = node.depth;
        }

        for (const uint32_t field : kNodeRelativeFields) {
            const uint32_t relative = read_be32(data + node.offset + field);
            const uint64_t target = static_cast<uint64_t>(node.offset) + relative;
            // One-past-the-end is valid for an empty payload range.
            if (target > size) {
                return false;
            }
        }

        const uint32_t first_child =
            read_be32(data + node.offset + kChildFields.front());
        if (first_child == 0) {
            const uint32_t triangle_count =
                read_be32(data + node.offset + 0x1c);
            const uint32_t vertex_count =
                read_be32(data + node.offset + 0x20);
            const uint32_t unsupported_count =
                read_be32(data + node.offset + 0x24);
            const uint64_t triangles =
                static_cast<uint64_t>(node.offset) +
                read_be32(data + node.offset + 0x28);
            const uint64_t vertices =
                static_cast<uint64_t>(node.offset) +
                read_be32(data + node.offset + 0x2c);
            if (unsupported_count != 0 ||
                !contains_range(size, triangles,
                                static_cast<uint64_t>(triangle_count) *
                                    kTriangleStride) ||
                !contains_range(size, vertices,
                                static_cast<uint64_t>(vertex_count) *
                                    kVertexStride) ||
                !add_count(triangle_total, triangle_count) ||
                !add_count(vertex_total, vertex_count)) {
                return false;
            }
            max_triangles =
                (std::max)(max_triangles, static_cast<size_t>(triangle_count));
            max_vertices =
                (std::max)(max_vertices, static_cast<size_t>(vertex_count));
            for (uint32_t index = 0; index < triangle_count; ++index) {
                const uint8_t* triangle =
                    data + static_cast<size_t>(triangles) +
                    static_cast<size_t>(index) * kTriangleStride;
                if (read_be16(triangle + 2) >= vertex_count ||
                    read_be16(triangle + 4) >= vertex_count ||
                    read_be16(triangle + 6) >= vertex_count) {
                    return false;
                }
            }
            ++leaf_count;
            continue;
        }

        if (node.depth == UINT32_MAX) {
            return false;
        }
        for (const uint32_t field : kChildFields) {
            const uint32_t child = read_be32(data + node.offset + field);
            if (child == 0 || (child & 3u) != 0 ||
                !contains_range(size, child, kNodeSize)) {
                return false;
            }
            pending.push_back({child, node.depth + 1});
        }
    }

    analysis->format = data[4];
    analysis->header_byte_5 = data[5];
    analysis->header_byte_6 = data[6];
    analysis->header_byte_7 = data[7];
    analysis->node_count = visited.size();
    analysis->leaf_count = leaf_count;
    analysis->triangle_count = triangle_total;
    analysis->vertex_count = vertex_total;
    analysis->max_triangles_per_leaf = max_triangles;
    analysis->max_vertices_per_leaf = max_vertices;
    analysis->max_depth = max_depth;
    analysis->coordinate_scale =
        1.0f / static_cast<float>(uint32_t{1} << data[5]);
    for (size_t axis = 0; axis < 3; ++axis) {
        analysis->root_min[axis] =
            decode_coordinate(data + kRootOffset + axis * 2,
                              analysis->coordinate_scale);
        analysis->root_max[axis] =
            decode_coordinate(data + kRootOffset + 6 + axis * 2,
                              analysis->coordinate_scale);
    }
    return true;
}

bool sample_type1_collision_surface(const uint8_t* data,
                                    size_t size,
                                    float x,
                                    float z,
                                    CollisionSurfaceSample* sample) {
    if (sample != nullptr) {
        *sample = {};
    }
    CollisionTreeAnalysis analysis;
    if (sample == nullptr || !std::isfinite(x) || !std::isfinite(z) ||
        !analyze_type1_collision_asset(data, size, &analysis)) {
        return false;
    }

    const uint32_t node_offset =
        select_leaf(data, analysis.coordinate_scale, x, z);
    const LeafPayload leaf = leaf_payload(data, node_offset);
    for (uint32_t index = 0; index < leaf.triangle_count; ++index) {
        const uint8_t* record =
            data + leaf.triangles + static_cast<size_t>(index) * kTriangleStride;
        const auto triangle = decode_triangle(
            data, leaf.vertices, record, analysis.coordinate_scale);
        if (!contains_xz(triangle, x, z)) {
            continue;
        }
        float height = 0.0f;
        if (!interpolate_height(triangle, x, z, height)) {
            continue;
        }
        sample->height = height;
        sample->surface_flags = read_be16(record);
        sample->leaf_offset = node_offset;
        sample->triangle_index = index;
        return true;
    }
    return false;
}

bool project_type1_collision_to_edge(const uint8_t* data,
                                     size_t size,
                                     float x,
                                     float z,
                                     CollisionEdgeSample* sample) {
    if (sample != nullptr) {
        *sample = {};
    }
    CollisionTreeAnalysis analysis;
    if (sample == nullptr || !std::isfinite(x) || !std::isfinite(z) ||
        !analyze_type1_collision_asset(data, size, &analysis)) {
        return false;
    }

    const uint32_t node_offset =
        select_leaf(data, analysis.coordinate_scale, x, z);
    const LeafPayload leaf = leaf_payload(data, node_offset);
    bool found = false;
    uint32_t selected_triangle = 0;
    uint8_t selected_edge = 0;
    EdgeProjection closest{};
    for (uint32_t index = 0; index < leaf.triangle_count; ++index) {
        const uint8_t* record =
            data + leaf.triangles + static_cast<size_t>(index) * kTriangleStride;
        const auto triangle = decode_triangle(
            data, leaf.vertices, record, analysis.coordinate_scale);
        for (uint8_t edge = 0; edge < 3; ++edge) {
            const EdgeProjection candidate =
                project_to_edge(triangle[edge], triangle[(edge + 1) % 3], x, z);
            if (!std::isfinite(candidate.distance_squared)) {
                return false;
            }
            if (!found || candidate.distance_squared < closest.distance_squared) {
                found = true;
                closest = candidate;
                selected_triangle = index;
                selected_edge = edge;
            }
        }
    }
    if (!found) {
        return false;
    }

    const uint8_t* record = data + leaf.triangles +
                            static_cast<size_t>(selected_triangle) *
                                kTriangleStride;
    const auto triangle = decode_triangle(
        data, leaf.vertices, record, analysis.coordinate_scale);
    float height = 0.0f;
    if (!interpolate_height(triangle, closest.x, closest.z, height)) {
        return false;
    }
    sample->position = {closest.x, height, closest.z};
    sample->surface_flags = read_be16(record);
    sample->leaf_offset = node_offset;
    sample->triangle_index = selected_triangle;
    sample->edge_index = selected_edge;
    sample->distance_squared_xz = closest.distance_squared;
    return true;
}

bool adjust_type1_collision_terrain_height(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    CollisionTerrainAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    if (adjustment == nullptr || !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2])) {
        return false;
    }

    CollisionSurfaceSample surface;
    if (sample_type1_collision_surface(data, size, proposed_position[0],
                                       proposed_position[2], &surface)) {
        adjustment->position = {proposed_position[0], surface.height,
                                proposed_position[2]};
        adjustment->surface_flags = surface.surface_flags;
        return true;
    }

    CollisionEdgeSample edge;
    if (!project_type1_collision_to_edge(data, size, proposed_position[0],
                                         proposed_position[2], &edge)) {
        return false;
    }
    adjustment->position = edge.position;
    adjustment->surface_flags = edge.surface_flags;
    adjustment->used_edge_fallback = true;
    return true;
}

} // namespace awl
