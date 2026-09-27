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

namespace {

bool sample_surface_in_leaf(const uint8_t* data,
                            const CollisionTreeAnalysis& analysis,
                            uint32_t node_offset,
                            float x,
                            float z,
                            CollisionSurfaceSample* sample) {
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

} // namespace

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
    return sample_surface_in_leaf(data, analysis, node_offset, x, z, sample);
}

namespace {

bool project_edge_in_leaf(const uint8_t* data,
                          const CollisionTreeAnalysis& analysis,
                          uint32_t node_offset,
                          float x,
                          float z,
                          CollisionEdgeSample* sample) {
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

} // namespace

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
    return project_edge_in_leaf(data, analysis, node_offset, x, z, sample);
}

bool resample_type1_collision_resolver_height(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& position,
    CollisionResolverHeightAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(position[0]) ||
        !std::isfinite(position[1]) || !std::isfinite(position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, position[0], position[2]);
    CollisionSurfaceSample surface;
    if (sample_surface_in_leaf(data, analysis, node_offset, position[0],
                               position[2], &surface)) {
        adjustment->position = {position[0], surface.height, position[2]};
        adjustment->surface_flags = surface.surface_flags;
        adjustment->leaf_offset = node_offset;
        return true;
    }

    CollisionEdgeSample edge;
    if (!project_edge_in_leaf(data, analysis, node_offset, position[0],
                              position[2], &edge)) {
        return false;
    }
    adjustment->position = {position[0], edge.position[1], position[2]};
    adjustment->surface_flags = edge.surface_flags;
    adjustment->leaf_offset = node_offset;
    adjustment->used_edge_fallback = true;
    return true;
}

bool evaluate_dynamic_contact_broad_phase(
    const CollisionAffineTransform& world_to_object,
    const std::array<float, 3>& proposed_position,
    const std::array<float, 3>& object_center_local,
    float object_radius,
    float moving_radius,
    bool* may_contact) {
    if (may_contact == nullptr) {
        return false;
    }
    *may_contact = false;
    for (float element : world_to_object) {
        if (!std::isfinite(element)) {
            return false;
        }
    }
    for (float element : proposed_position) {
        if (!std::isfinite(element)) {
            return false;
        }
    }
    for (float element : object_center_local) {
        if (!std::isfinite(element)) {
            return false;
        }
    }
    if (!std::isfinite(object_radius) || object_radius < 0.0f ||
        !std::isfinite(moving_radius) || moving_radius < 0.0f) {
        return false;
    }

    const float x = proposed_position[0];
    const float z = proposed_position[2];
    const float local_x = world_to_object[0] * x +
                          world_to_object[2] * z + world_to_object[3];
    const float local_z = world_to_object[8] * x +
                          world_to_object[10] * z + world_to_object[11];
    const float delta_x = object_center_local[0] - local_x;
    const float delta_z = object_center_local[2] - local_z;
    const float distance = std::sqrt(delta_x * delta_x + delta_z * delta_z);
    const float combined_radius = object_radius + moving_radius;
    if (!std::isfinite(distance) || !std::isfinite(combined_radius)) {
        return false;
    }
    *may_contact = distance <= combined_radius;
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

namespace {

bool adjust_vertex_in_leaf(
    const uint8_t* data,
    const CollisionTreeAnalysis& analysis,
    uint32_t node_offset,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    bool require_incident_edge,
    CollisionRadiusVertexAdjustment* adjustment) {
    adjustment->position = proposed_position;
    const LeafPayload leaf = leaf_payload(data, node_offset);
    float nearest_distance = radius;
    DecodedVertex nearest_vertex{};
    for (uint32_t index = 0; index < leaf.triangle_count; ++index) {
        const uint8_t* record = data + leaf.triangles +
                                static_cast<size_t>(index) * kTriangleStride;
        const uint16_t flags = read_be16(record);
        if ((surface_mask & 0x10000u) == 0 &&
            (flags & surface_mask) == 0 && flags != 0) {
            continue;
        }
        const auto triangle = decode_triangle(
            data, leaf.vertices, record, analysis.coordinate_scale);
        for (uint8_t vertex = 0; vertex < 3; ++vertex) {
            const uint16_t incident_edges = static_cast<uint16_t>(
                (1u << (13 + vertex)) | (1u << (13 + (vertex + 2) % 3)));
            if (require_incident_edge && (flags & incident_edges) == 0) {
                continue;
            }
            const float dx = proposed_position[0] - triangle[vertex].x;
            const float dz = proposed_position[2] - triangle[vertex].z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (!std::isfinite(distance)) {
                *adjustment = {};
                return false;
            }
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_vertex = triangle[vertex];
                adjustment->contact = true;
                adjustment->triangle_index = index;
                adjustment->vertex_index = vertex;
            }
        }
    }
    if (adjustment->contact && nearest_distance != 0.0f) {
        const float scale = (radius + 0.01f) / nearest_distance;
        adjustment->position = {
            nearest_vertex.x +
                (proposed_position[0] - nearest_vertex.x) * scale,
            nearest_vertex.y,
            nearest_vertex.z +
                (proposed_position[2] - nearest_vertex.z) * scale};
        if (!std::isfinite(adjustment->position[0]) ||
            !std::isfinite(adjustment->position[2])) {
            *adjustment = {};
            return false;
        }
    }
    return true;
}

} // namespace

bool adjust_type1_collision_radius_vertex(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusVertexAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    return adjust_vertex_in_leaf(data, analysis, node_offset,
                                 proposed_position, radius, 0x10000u, true,
                                 adjustment);
}

bool adjust_type1_dynamic_contact_vertex(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    CollisionRadiusVertexAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    return adjust_vertex_in_leaf(data, analysis, node_offset,
                                 proposed_position, radius, surface_mask,
                                 false, adjustment);
}

namespace {

bool adjust_edge_in_leaf(
    const uint8_t* data,
    const CollisionTreeAnalysis& analysis,
    uint32_t node_offset,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    bool require_marked_edge,
    CollisionRadiusEdgeAdjustment* adjustment) {
    adjustment->position = proposed_position;
    const LeafPayload leaf = leaf_payload(data, node_offset);
    float nearest_distance = 0.0f;
    float selected_normal_x = 0.0f;
    float selected_normal_z = 0.0f;
    for (uint32_t index = 0; index < leaf.triangle_count; ++index) {
        const uint8_t* record = data + leaf.triangles +
                                static_cast<size_t>(index) * kTriangleStride;
        const uint16_t flags = read_be16(record);
        if ((surface_mask & 0x10000u) == 0 &&
            (flags & surface_mask) == 0 && flags != 0) {
            continue;
        }
        const auto triangle = decode_triangle(
            data, leaf.vertices, record, analysis.coordinate_scale);
        for (uint8_t edge = 0; edge < 3; ++edge) {
            if (require_marked_edge &&
                (flags & (1u << (13 + edge))) == 0) {
                continue;
            }
            const DecodedVertex& start = triangle[edge];
            const DecodedVertex& end = triangle[(edge + 1) % 3];
            const float dx = end.x - start.x;
            const float dz = end.z - start.z;
            const float length_squared = dx * dx + dz * dz;
            if (!std::isfinite(length_squared)) {
                *adjustment = {};
                return false;
            }
            if (length_squared == 0.0f) {
                continue;
            }
            const float length = std::sqrt(length_squared);
            // FUN_8017C918 crosses the edge with the verified (0, -1, 0)
            // vector at 0x8026AA84, producing this X/Z plane normal.
            const float normal_x = dz / length;
            const float normal_z = -dx / length;
            const float prior_distance =
                (prior_position[0] - start.x) * normal_x +
                (prior_position[2] - start.z) * normal_z;
            if (!std::isfinite(prior_distance)) {
                *adjustment = {};
                return false;
            }
            if (prior_distance <= 0.0f) {
                continue;
            }
            const float proposed_dx = proposed_position[0] - start.x;
            const float proposed_dz = proposed_position[2] - start.z;
            const float along =
                (proposed_dx * dx + proposed_dz * dz) / length_squared;
            if (!std::isfinite(along)) {
                *adjustment = {};
                return false;
            }
            if (along < 0.0f || along >= 1.0f) {
                continue;
            }
            const float distance =
                proposed_dx * normal_x + proposed_dz * normal_z;
            if (!std::isfinite(distance)) {
                *adjustment = {};
                return false;
            }
            if (distance >= radius ||
                (adjustment->contact && distance >= nearest_distance)) {
                continue;
            }
            nearest_distance = distance;
            selected_normal_x = normal_x;
            selected_normal_z = normal_z;
            adjustment->contact = true;
            adjustment->surface_flags = flags;
            adjustment->triangle_index = index;
            adjustment->edge_index = edge;
        }
    }
    if (adjustment->contact) {
        const float push = radius - nearest_distance + 0.01f;
        adjustment->position[0] += selected_normal_x * push;
        adjustment->position[2] += selected_normal_z * push;
        if (!std::isfinite(adjustment->position[0]) ||
            !std::isfinite(adjustment->position[2])) {
            *adjustment = {};
            return false;
        }
    }
    return true;
}

} // namespace

bool adjust_type1_collision_radius_edge(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusEdgeAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(prior_position[0]) ||
        !std::isfinite(prior_position[1]) ||
        !std::isfinite(prior_position[2]) ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    return adjust_edge_in_leaf(data, analysis, node_offset,
                               prior_position, proposed_position,
                               radius, 0x10000u, true, adjustment);
}

bool adjust_type1_dynamic_contact_edge(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    CollisionRadiusEdgeAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(prior_position[0]) ||
        !std::isfinite(prior_position[1]) ||
        !std::isfinite(prior_position[2]) ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    return adjust_edge_in_leaf(data, analysis, node_offset,
                               prior_position, proposed_position, radius,
                               surface_mask, false, adjustment);
}

namespace {

bool first_containing_flags_in_leaf(
    const uint8_t* data,
    const CollisionTreeAnalysis& analysis,
    uint32_t node_offset,
    const std::array<float, 3>& position,
    uint16_t& surface_flags) {
    const LeafPayload leaf = leaf_payload(data, node_offset);
    for (uint32_t index = 0; index < leaf.triangle_count; ++index) {
        const uint8_t* record = data + leaf.triangles +
                                static_cast<size_t>(index) * kTriangleStride;
        const auto triangle = decode_triangle(
            data, leaf.vertices, record, analysis.coordinate_scale);
        if (contains_xz(triangle, position[0], position[2])) {
            surface_flags = read_be16(record);
            return true;
        }
    }
    return false;
}

bool resolve_dynamic_contact_in_leaf(
    const uint8_t* data,
    const CollisionTreeAnalysis& analysis,
    uint32_t node_offset,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    uint32_t contact_flags,
    CollisionDynamicContactAdjustment* adjustment) {
    if ((contact_flags & 6u) != 0) {
        uint16_t flags = 0;
        if (first_containing_flags_in_leaf(data, analysis, node_offset,
                                           prior_position, flags) &&
            ((surface_mask & 0x10000u) != 0 ||
             (flags & surface_mask) != 0 || flags == 0)) {
            adjustment->used_containment_shortcut = true;
            adjustment->contact = (contact_flags & 2u) != 0;
            adjustment->reverted_to_prior = adjustment->contact;
            adjustment->position = adjustment->contact ? prior_position
                                                        : proposed_position;
            return true;
        }
    }

    std::array<float, 3> candidate = proposed_position;
    for (uint8_t pass = 1; pass <= 3; ++pass) {
        CollisionRadiusEdgeAdjustment edge;
        if (!adjust_edge_in_leaf(data, analysis, node_offset,
                                 prior_position, candidate, radius,
                                 surface_mask, false, &edge)) {
            return false;
        }
        candidate = edge.position;
        CollisionRadiusVertexAdjustment vertex;
        if (!adjust_vertex_in_leaf(data, analysis, node_offset,
                                   candidate, radius, surface_mask, false,
                                   &vertex)) {
            return false;
        }
        candidate = vertex.position;
        const bool contact = edge.contact || vertex.contact;
        if (pass == 1) {
            adjustment->contact = contact;
            adjustment->first_edge_contact = edge.contact;
            if (edge.contact) {
                adjustment->first_edge_surface_flags = edge.surface_flags;
            }
        }
        if (!contact || (contact_flags & 1u) != 0 || pass == 3) {
            adjustment->reverted_to_prior =
                contact && ((contact_flags & 1u) != 0 || pass == 3);
            adjustment->position = adjustment->reverted_to_prior
                                       ? prior_position : candidate;
            adjustment->pass_count = pass;
            return true;
        }
    }
    return false;
}

} // namespace

bool resolve_type1_dynamic_contact_narrow_phase(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    uint32_t contact_flags,
    CollisionDynamicContactAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(prior_position[0]) ||
        !std::isfinite(prior_position[1]) ||
        !std::isfinite(prior_position[2]) ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    CollisionDynamicContactAdjustment result;
    if (!resolve_dynamic_contact_in_leaf(data, analysis, node_offset,
                                         prior_position, proposed_position,
                                         radius, surface_mask, contact_flags,
                                         &result)) {
        return false;
    }
    *adjustment = result;
    return true;
}

namespace {

std::array<float, 3> transform_horizontal_point(
    const CollisionAffineTransform& transform,
    const std::array<float, 3>& position) {
    const float x = position[0];
    const float z = position[2];
    return {
        transform[0] * x + transform[2] * z + transform[3],
        transform[4] * x + transform[6] * z + transform[7],
        transform[8] * x + transform[10] * z + transform[11]};
}

bool finite_position(const std::array<float, 3>& position) {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

bool resolve_circle_contact(const std::array<float, 3>& center,
                            float combined_radius,
                            std::array<float, 3>* candidate,
                            bool* contact) {
    if (!finite_position(center) || !std::isfinite(combined_radius) ||
        combined_radius < 0.0f || candidate == nullptr || contact == nullptr) {
        return false;
    }
    *contact = false;
    const float dx = (*candidate)[0] - center[0];
    const float dz = (*candidate)[2] - center[2];
    const float distance = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(distance)) {
        return false;
    }
    if (distance >= combined_radius) {
        return true;
    }
    const float pushed_radius = combined_radius + 0.01f;
    if (!std::isfinite(pushed_radius)) {
        return false;
    }
    const float scale = distance == 0.0f ? 0.0f : pushed_radius / distance;
    (*candidate)[0] = center[0] + dx * scale;
    (*candidate)[2] = center[2] + (distance == 0.0f ? pushed_radius : dz * scale);
    if (!finite_position(*candidate)) {
        return false;
    }
    *contact = true;
    return true;
}

} // namespace

bool resolve_type1_dynamic_object_contact(
    const uint8_t* data,
    size_t size,
    const CollisionDynamicObjectQuery& query,
    const std::array<float, 3>& prior_world_position,
    const std::array<float, 3>& proposed_world_position,
    CollisionDynamicObjectContactAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    if (adjustment == nullptr || !finite_position(prior_world_position) ||
        !finite_position(proposed_world_position)) {
        return false;
    }
    for (const float element : query.object_to_world) {
        if (!std::isfinite(element)) {
            return false;
        }
    }
    CollisionTreeAnalysis analysis;
    if (!analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    bool may_contact = false;
    if (!evaluate_dynamic_contact_broad_phase(
            query.world_to_object, proposed_world_position,
            query.object_center_local, query.object_radius,
            query.moving_radius, &may_contact)) {
        return false;
    }
    CollisionDynamicObjectContactAdjustment result;
    result.broad_phase_passed = may_contact;
    result.position = proposed_world_position;
    if (!may_contact) {
        *adjustment = result;
        return true;
    }

    const auto local_prior = transform_horizontal_point(
        query.world_to_object, prior_world_position);
    const auto local_proposed = transform_horizontal_point(
        query.world_to_object, proposed_world_position);
    if (!finite_position(local_prior) || !finite_position(local_proposed)) {
        return false;
    }
    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, local_proposed[0],
        local_proposed[2]);
    if (!resolve_dynamic_contact_in_leaf(
            data, analysis, node_offset, local_prior, local_proposed,
            query.moving_radius, query.surface_mask, query.contact_flags,
            &result.local_narrow_phase)) {
        return false;
    }
    result.contact = result.local_narrow_phase.contact;
    if (result.contact) {
        result.position = transform_horizontal_point(
            query.object_to_world, result.local_narrow_phase.position);
        if (!finite_position(result.position)) {
            return false;
        }
    }
    *adjustment = result;
    return true;
}

namespace {

bool resolve_type1_ordered_dynamic_object_pass(
    const CollisionDynamicPassObject* objects,
    size_t object_count,
    uint64_t source_identity,
    int32_t category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float moving_radius,
    uint32_t initial_contact_flags,
    uint32_t resolver_flags,
    bool later_pass,
    CollisionDynamicPassAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    if (adjustment == nullptr ||
        (object_count != 0 && objects == nullptr) ||
        !finite_position(prior_position) ||
        !finite_position(proposed_position) ||
        !std::isfinite(moving_radius) || moving_radius < 0.0f) {
        return false;
    }

    CollisionDynamicPassAdjustment result;
    result.position = proposed_position;
    result.contact_flags_after = initial_contact_flags;
    const uint32_t gate = later_pass ? 2u : 4u;
    if ((resolver_flags & gate) == 0) {
        *adjustment = result;
        return true;
    }
    if (!later_pass && (resolver_flags & 0x10u) != 0) {
        return false;
    }
    if (later_pass) {
        result.contact_flags_after &= ~1u;
    }

    for (size_t index = 0; index < object_count; ++index) {
        const CollisionDynamicPassObject& object = objects[index];
        if ((later_pass ? object.identity == 0 :
                          object.identity == source_identity) || !object.enabled ||
            object.category != category) {
            continue;
        }
        if ((object.collision_flags & 2u) != 0) {
            CollisionDynamicObjectQuery query;
            query.world_to_object = object.world_to_object;
            query.object_to_world = object.object_to_world;
            query.object_center_local = object.center_local;
            query.object_radius = object.radius;
            query.moving_radius = moving_radius;
            query.surface_mask = 0x10000u;
            query.contact_flags = result.contact_flags_after;
            CollisionDynamicObjectContactAdjustment contact;
            if (!resolve_type1_dynamic_object_contact(
                    object.data, object.size, query, prior_position,
                    result.position, &contact)) {
                return false;
            }
            result.position = contact.position;
            ++result.queried_objects;
            if (contact.contact) {
                result.contact = true;
                ++result.contact_count;
                result.contact_flags_after |= 1u;
            }
        } else if ((object.collision_flags & 1u) != 0) {
            const float combined_radius = moving_radius + object.radius;
            bool contact = false;
            if (!std::isfinite(object.radius) || object.radius < 0.0f ||
                !resolve_circle_contact(object.center_world, combined_radius,
                                        &result.position, &contact)) {
                return false;
            }
            ++result.queried_objects;
            if (contact) {
                result.contact = true;
                ++result.contact_count;
                result.contact_flags_after |= 1u;
            }
        }
    }
    if (result.contact) {
        result.resolver_contact_bit = gate;
    }
    *adjustment = result;
    return true;
}

} // namespace

bool resolve_type1_first_dynamic_object_pass(
    const CollisionDynamicPassObject* objects,
    size_t object_count,
    uint64_t source_identity,
    int32_t category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float moving_radius,
    uint32_t initial_contact_flags,
    uint32_t resolver_flags,
    CollisionDynamicPassAdjustment* adjustment) {
    return resolve_type1_ordered_dynamic_object_pass(
        objects, object_count, source_identity, category, prior_position,
        proposed_position, moving_radius, initial_contact_flags,
        resolver_flags, false, adjustment);
}

bool resolve_type1_later_dynamic_object_pass(
    const CollisionDynamicPassObject* objects,
    size_t object_count,
    int32_t category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float moving_radius,
    uint32_t initial_contact_flags,
    uint32_t resolver_flags,
    CollisionDynamicPassAdjustment* adjustment) {
    return resolve_type1_ordered_dynamic_object_pass(
        objects, object_count, 0, category, prior_position,
        proposed_position, moving_radius, initial_contact_flags,
        resolver_flags, true, adjustment);
}

namespace {

bool adjust_radius_passes_in_leaf(
    const uint8_t* data,
    const CollisionTreeAnalysis& analysis,
    uint32_t node_offset,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusPassesAdjustment* adjustment) {
    std::array<float, 3> candidate = proposed_position;
    bool first_pass_contact = false;
    for (uint8_t pass = 1; pass <= 3; ++pass) {
        CollisionRadiusEdgeAdjustment edge;
        if (!adjust_edge_in_leaf(data, analysis, node_offset,
                                 prior_position, candidate, radius,
                                 0x10000u, true, &edge)) {
            return false;
        }
        candidate = edge.position;
        CollisionRadiusVertexAdjustment vertex;
        if (!adjust_vertex_in_leaf(data, analysis, node_offset,
                                   candidate, radius, 0x10000u, true,
                                   &vertex)) {
            return false;
        }
        candidate = vertex.position;
        const bool contact = edge.contact || vertex.contact;
        if (pass == 1) {
            first_pass_contact = contact;
        }
        if (!contact || pass == 3) {
            adjustment->position = contact ? prior_position : candidate;
            adjustment->contact = first_pass_contact;
            adjustment->reverted_to_prior = contact;
            adjustment->pass_count = pass;
            return true;
        }
    }
    return false;
}

} // namespace

bool adjust_type1_collision_radius_passes(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusPassesAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(prior_position[0]) ||
        !std::isfinite(prior_position[1]) ||
        !std::isfinite(prior_position[2]) ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    return adjust_radius_passes_in_leaf(data, analysis, node_offset,
                                        prior_position, proposed_position,
                                        radius, adjustment);
}

namespace {

bool query_height_in_leaf(const uint8_t* data,
                          const CollisionTreeAnalysis& analysis,
                          uint32_t node_offset,
                          std::array<float, 3>& candidate,
                          float& height,
                          uint16_t& surface_flags,
                          bool& used_edge_fallback) {
    CollisionSurfaceSample surface;
    if (sample_surface_in_leaf(data, analysis, node_offset, candidate[0],
                               candidate[2], &surface)) {
        height = surface.height;
        surface_flags = surface.surface_flags;
        used_edge_fallback = false;
        return true;
    }

    CollisionEdgeSample edge;
    if (!project_edge_in_leaf(data, analysis, node_offset, candidate[0],
                              candidate[2], &edge)) {
        return false;
    }
    candidate = edge.position;
    height = edge.position[1];
    surface_flags = edge.surface_flags;
    used_edge_fallback = true;
    return true;
}

} // namespace

bool adjust_type1_collision_terrain_with_radius(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionTerrainRadiusAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    CollisionTreeAnalysis analysis;
    if (adjustment == nullptr || !std::isfinite(radius) || radius < 0.0f ||
        !std::isfinite(prior_position[0]) ||
        !std::isfinite(prior_position[1]) ||
        !std::isfinite(prior_position[2]) ||
        !std::isfinite(proposed_position[0]) ||
        !std::isfinite(proposed_position[1]) ||
        !std::isfinite(proposed_position[2]) ||
        !analyze_type1_collision_asset(data, size, &analysis) ||
        analysis.header_byte_6 != 1) {
        return false;
    }

    const uint32_t node_offset = select_leaf(
        data, analysis.coordinate_scale, proposed_position[0],
        proposed_position[2]);
    std::array<float, 3> candidate = proposed_position;
    float height = 0.0f;
    uint16_t surface_flags = 0;
    bool initial_fallback = false;
    if (!query_height_in_leaf(data, analysis, node_offset, candidate, height,
                              surface_flags, initial_fallback)) {
        return false;
    }

    CollisionRadiusPassesAdjustment radius_adjustment;
    bool final_fallback = false;
    if (radius > 0.0f) {
        if (!adjust_radius_passes_in_leaf(data, analysis, node_offset,
                                          prior_position, candidate, radius,
                                          &radius_adjustment)) {
            return false;
        }
        candidate = radius_adjustment.position;
        if (radius_adjustment.contact &&
            !query_height_in_leaf(data, analysis, node_offset, candidate,
                                  height, surface_flags, final_fallback)) {
            return false;
        }
    }

    candidate[1] = height;
    adjustment->position = candidate;
    adjustment->surface_flags = surface_flags;
    adjustment->terrain_contact = initial_fallback || radius_adjustment.contact;
    adjustment->initial_edge_fallback = initial_fallback;
    adjustment->final_edge_fallback = final_fallback;
    adjustment->radius_contact = radius_adjustment.contact;
    adjustment->reverted_to_prior = radius_adjustment.reverted_to_prior;
    adjustment->radius_pass_count = radius_adjustment.pass_count;
    return true;
}

uint32_t category1_static_surface_mask(
    uint32_t resolver_flags,
    const CollisionCategory1StaticFlags& runtime_flags) {
    uint32_t mask = 1u;
    if ((resolver_flags & 0x40u) != 0) {
        mask |= 0x80u;
        if (runtime_flags.state_299a4) {
            mask |= 2u;
        }
    }
    if ((resolver_flags & 0x100u) != 0) {
        mask |= 0x400u;
    }
    if ((resolver_flags & 0x200u) != 0) {
        mask |= 0x1000u;
    }
    if (runtime_flags.state_299a5) {
        mask |= 4u;
    }
    if (runtime_flags.state_299a6) {
        mask |= 8u;
    } else {
        mask |= 0x40u;
    }
    if (runtime_flags.state_299a8) {
        mask |= 0x10u;
    }
    if (runtime_flags.state_299a9) {
        mask |= 0x20u;
    }
    if (runtime_flags.secondary_3f3) {
        mask |= 0x100u;
    }
    if (runtime_flags.secondary_3f1) {
        mask |= 0x200u;
    }
    if (runtime_flags.state_299af) {
        mask |= 0x800u;
    }
    return mask;
}

bool resolve_type1_category1_static_contact(
    const uint8_t* data,
    size_t size,
    const CollisionCategory1StaticFlags& runtime_flags,
    uint32_t resolver_flags,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float moving_radius,
    uint32_t initial_contact_flags,
    CollisionCategory1StaticAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    if (adjustment == nullptr || !finite_position(prior_position) ||
        !finite_position(proposed_position) ||
        !std::isfinite(moving_radius) || moving_radius < 0.0f ||
        (data == nullptr && size != 0)) {
        return false;
    }
    CollisionCategory1StaticAdjustment result;
    result.position = proposed_position;
    if (data == nullptr) {
        *adjustment = result;
        return true;
    }
    result.slot_present = true;
    result.surface_mask = category1_static_surface_mask(
        resolver_flags, runtime_flags);
    if (!resolve_type1_dynamic_contact_narrow_phase(
            data, size, prior_position, proposed_position, moving_radius,
            result.surface_mask, initial_contact_flags,
            &result.narrow_phase)) {
        return false;
    }
    result.position = result.narrow_phase.position;
    result.contact = result.narrow_phase.contact;
    *adjustment = result;
    return true;
}

bool resolve_type1_category1_movement_candidate(
    const CollisionCategory1MovementQuery& query,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    CollisionCategory1MovementAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    if (adjustment == nullptr) {
        return false;
    }

    constexpr uint32_t movement_resolver_flags = 0x67u;
    CollisionCategory1MovementAdjustment result;
    if (!resolve_type1_first_dynamic_object_pass(
            query.first_objects, query.first_object_count,
            query.source_identity, 1, prior_position, proposed_position,
            query.moving_radius, query.initial_contact_flags,
            movement_resolver_flags, &result.first_pass)) {
        return false;
    }
    if (!adjust_type1_collision_terrain_with_radius(
            query.terrain_data, query.terrain_size, prior_position,
            result.first_pass.position, query.moving_radius,
            &result.terrain)) {
        return false;
    }
    if (!resolve_type1_category1_static_contact(
            query.static_data, query.static_size, query.static_flags,
            movement_resolver_flags, prior_position, result.terrain.position,
            query.moving_radius, result.first_pass.contact_flags_after & ~1u,
            &result.static_contact)) {
        return false;
    }
    if (!resolve_type1_later_dynamic_object_pass(
            query.later_objects, query.later_object_count, 1, prior_position,
            result.static_contact.position, query.moving_radius,
            result.first_pass.contact_flags_after, movement_resolver_flags,
            &result.later_pass)) {
        return false;
    }

    result.position = result.later_pass.position;
    result.resolver_contact_bits = result.first_pass.resolver_contact_bit |
        (result.terrain.terrain_contact || result.static_contact.contact ?
             1u : 0u) | result.later_pass.resolver_contact_bit;
    if (result.static_contact.contact || result.later_pass.contact) {
        CollisionResolverHeightAdjustment height;
        if (!resample_type1_collision_resolver_height(
                query.terrain_data, query.terrain_size, result.position,
                &height)) {
            return false;
        }
        result.position = height.position;
        result.final_height_resampled = true;
    }
    *adjustment = result;
    return true;
}

} // namespace awl
