#include "awl/collision_asset.h"
#include "awl/world_map_contact.h"
#include "awl/world_map_movement.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void put_be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

void put_be16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<uint8_t>(value);
}

void put_be_s16(std::vector<uint8_t>& bytes, size_t offset, int16_t value) {
    put_be16(bytes, offset, static_cast<uint16_t>(value));
}

void initialize_header(std::vector<uint8_t>& bytes) {
    put_be32(bytes, 0, 0xE7E3F1F4u);
    bytes[4] = 1;
    bytes[5] = 6;
    bytes[6] = 1;
}

void initialize_leaf(std::vector<uint8_t>& bytes, uint32_t offset) {
    constexpr uint32_t node_size = 0x34;
    put_be32(bytes, offset + 0x28, node_size);
    put_be32(bytes, offset + 0x2c, node_size);
    put_be32(bytes, offset + 0x30, node_size);
}

std::vector<uint8_t> make_single_leaf() {
    std::vector<uint8_t> bytes(8 + 0x34, 0);
    initialize_header(bytes);
    initialize_leaf(bytes, 8);
    return bytes;
}

std::vector<uint8_t> make_one_level_tree() {
    constexpr uint32_t root = 8;
    constexpr uint32_t node_size = 0x34;
    std::vector<uint8_t> bytes(root + node_size * 5, 0);
    initialize_header(bytes);
    for (uint32_t index = 0; index < 4; ++index) {
        const uint32_t child = root + node_size * (index + 1);
        put_be32(bytes, root + 0x0c + index * 4, child);
        initialize_leaf(bytes, child);
    }
    put_be32(bytes, root + 0x28, node_size);
    put_be32(bytes, root + 0x2c, node_size);
    put_be32(bytes, root + 0x30, 0);
    return bytes;
}

void initialize_sample_payload(std::vector<uint8_t>& bytes,
                               uint32_t node,
                               uint32_t payload,
                               int16_t origin_x,
                               int16_t origin_z) {
    put_be_s16(bytes, node, origin_x);
    put_be_s16(bytes, node + 2, 0);
    put_be_s16(bytes, node + 4, origin_z);
    put_be_s16(bytes, node + 6, static_cast<int16_t>(origin_x + 10));
    put_be_s16(bytes, node + 8, 20);
    put_be_s16(bytes, node + 10, static_cast<int16_t>(origin_z + 10));
    put_be32(bytes, node + 0x1c, 1);
    put_be32(bytes, node + 0x20, 3);
    put_be32(bytes, node + 0x24, 0);
    put_be32(bytes, node + 0x28, payload - node);
    put_be32(bytes, node + 0x2c, payload + 8 - node);
    put_be32(bytes, node + 0x30, 0);

    put_be16(bytes, payload, 0x20);
    put_be16(bytes, payload + 2, 0);
    put_be16(bytes, payload + 4, 1);
    put_be16(bytes, payload + 6, 2);

    const uint32_t vertices = payload + 8;
    put_be_s16(bytes, vertices, origin_x);
    put_be_s16(bytes, vertices + 2, 0);
    put_be_s16(bytes, vertices + 4, origin_z);
    put_be_s16(bytes, vertices + 8, static_cast<int16_t>(origin_x + 10));
    put_be_s16(bytes, vertices + 10, 10);
    put_be_s16(bytes, vertices + 12, origin_z);
    put_be_s16(bytes, vertices + 16, origin_x);
    put_be_s16(bytes, vertices + 18, 20);
    put_be_s16(bytes, vertices + 20, static_cast<int16_t>(origin_z + 10));
}

std::vector<uint8_t> make_sample_leaf() {
    constexpr uint32_t node = 8;
    constexpr uint32_t payload = node + 0x34;
    std::vector<uint8_t> bytes(payload + 8 + 3 * 8, 0);
    initialize_header(bytes);
    bytes[5] = 0;
    initialize_sample_payload(bytes, node, payload, 0, 0);
    return bytes;
}

std::vector<uint8_t> make_one_level_sample_tree() {
    constexpr uint32_t root = 8;
    constexpr uint32_t node_size = 0x34;
    constexpr uint32_t selected_leaf = root + node_size * 4;
    constexpr uint32_t payload = root + node_size * 5;
    std::vector<uint8_t> bytes(payload + 8 + 3 * 8, 0);
    initialize_header(bytes);
    bytes[5] = 0;
    put_be_s16(bytes, root, 0);
    put_be_s16(bytes, root + 2, 0);
    put_be_s16(bytes, root + 4, 0);
    put_be_s16(bytes, root + 6, 20);
    put_be_s16(bytes, root + 8, 20);
    put_be_s16(bytes, root + 10, 20);
    for (uint32_t index = 0; index < 4; ++index) {
        const uint32_t child = root + node_size * (index + 1);
        put_be32(bytes, root + 0x0c + index * 4, child);
        initialize_leaf(bytes, child);
    }
    put_be32(bytes, root + 0x28, node_size);
    put_be32(bytes, root + 0x2c, node_size);
    initialize_sample_payload(bytes, selected_leaf, payload, 10, 10);
    return bytes;
}

bool analyze(const std::vector<uint8_t>& bytes,
             awl::CollisionTreeAnalysis& analysis) {
    return awl::analyze_type1_collision_asset(bytes.data(), bytes.size(),
                                              &analysis);
}

void test_valid_structures() {
    awl::CollisionTreeAnalysis analysis;
    std::vector<uint8_t> bytes = make_single_leaf();
    expect(analyze(bytes, analysis), "single-leaf collision tree is valid");
    expect(analysis.format == 1 && analysis.header_byte_5 == 6 &&
               analysis.header_byte_6 == 1 && analysis.header_byte_7 == 0,
           "collision header bytes are preserved without guessed semantics");
    expect(analysis.node_count == 1 && analysis.leaf_count == 1 &&
               analysis.triangle_count == 0 && analysis.vertex_count == 0 &&
               analysis.max_depth == 0,
           "single-leaf collision metadata is exact");

    bytes = make_one_level_tree();
    expect(analyze(bytes, analysis), "one-level collision tree is valid");
    expect(analysis.node_count == 5 && analysis.leaf_count == 4 &&
               analysis.max_depth == 1,
           "one-level collision metadata is exact");

    bytes = make_sample_leaf();
    expect(analyze(bytes, analysis), "indexed triangle payload is valid");
    expect(analysis.triangle_count == 1 && analysis.vertex_count == 3 &&
               analysis.max_triangles_per_leaf == 1 &&
               analysis.max_vertices_per_leaf == 3 &&
               analysis.coordinate_scale == 1.0f &&
               analysis.root_min[0] == 0.0f &&
               analysis.root_max[1] == 20.0f,
           "indexed payload metadata and root bounds are exact");
}

void test_rejects_unsupported_or_truncated_files() {
    awl::CollisionTreeAnalysis analysis;
    std::vector<uint8_t> bytes = make_single_leaf();
    bytes[0] = 0;
    expect(!analyze(bytes, analysis), "wrong collision marker is rejected");
    expect(analysis.node_count == 0, "failed analysis clears its output");

    bytes = make_single_leaf();
    bytes[4] = 0;
    expect(!analyze(bytes, analysis), "unsupported collision format is rejected");

    bytes = make_single_leaf();
    bytes.pop_back();
    expect(!analyze(bytes, analysis), "truncated collision root is rejected");
    expect(!awl::analyze_type1_collision_asset(nullptr, 0, &analysis),
           "null collision data is rejected");
    expect(!awl::analyze_type1_collision_asset(bytes.data(), bytes.size(), nullptr),
           "null collision output is rejected");
}

void test_rejects_invalid_offsets() {
    awl::CollisionTreeAnalysis analysis;
    std::vector<uint8_t> bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x10, 0);
    expect(!analyze(bytes, analysis), "partial child sets are rejected");

    bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x10, static_cast<uint32_t>(bytes.size()));
    expect(!analyze(bytes, analysis), "out-of-range child nodes are rejected");

    bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x10, 8 + 0x34);
    expect(!analyze(bytes, analysis), "shared child nodes are rejected");

    bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x0c, 8);
    expect(!analyze(bytes, analysis), "collision tree cycles are rejected");

    bytes = make_single_leaf();
    put_be32(bytes, 8 + 0x28, 0xFFFFFFFFu);
    expect(!analyze(bytes, analysis), "out-of-range leaf payloads are rejected");

    bytes = make_sample_leaf();
    put_be16(bytes, 8 + 0x34 + 2, 3);
    expect(!analyze(bytes, analysis), "out-of-range vertex indices are rejected");

    bytes = make_sample_leaf();
    put_be32(bytes, 8 + 0x24, 1);
    expect(!analyze(bytes, analysis), "unsupported third payloads are rejected");

    bytes = make_sample_leaf();
    put_be_s16(bytes, 8, 11);
    expect(!analyze(bytes, analysis), "inverted node bounds are rejected");

    bytes = make_sample_leaf();
    bytes[5] = 31;
    expect(!analyze(bytes, analysis), "unsafe coordinate exponents are rejected");
}

void test_primary_surface_sample() {
    awl::CollisionSurfaceSample sample;
    std::vector<uint8_t> bytes = make_sample_leaf();
    expect(awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                               2.0f, 2.0f, &sample),
           "point inside a leaf triangle produces a surface sample");
    expect(sample.height == 6.0f && sample.surface_flags == 0x20 &&
               sample.leaf_offset == 8 && sample.triangle_index == 0,
           "surface height, flags, leaf, and triangle are exact");

    put_be16(bytes, 8 + 0x34 + 4, 2);
    put_be16(bytes, 8 + 0x34 + 6, 1);
    expect(awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                               2.0f, 2.0f, &sample) &&
               sample.height == 6.0f,
           "surface containment accepts the opposite winding");

    expect(!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                                20.0f, 20.0f, &sample),
           "point outside the leaf triangle has no primary sample");
    expect(sample.height == 0.0f && sample.surface_flags == 0,
           "failed surface sampling clears its output");
    expect(!awl::sample_type1_collision_surface(
               bytes.data(), bytes.size(),
               std::numeric_limits<float>::quiet_NaN(), 0.0f, &sample),
           "non-finite surface coordinates are rejected");
    expect(!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                                0.0f, 0.0f, nullptr),
           "null surface output is rejected");

    bytes = make_one_level_sample_tree();
    expect(awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                               12.0f, 12.0f, &sample),
           "quadtree traversal selects the matching populated leaf");
    expect(sample.height == 6.0f && sample.leaf_offset == 8 + 0x34 * 4,
           "quadtree surface sample preserves the selected leaf");
    expect(!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                                2.0f, 2.0f, &sample),
           "quadtree traversal does not search unrelated leaves");
}

void test_nearest_edge_fallback() {
    awl::CollisionEdgeSample sample;
    std::vector<uint8_t> bytes = make_sample_leaf();
    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 6.0f, -3.0f, &sample),
           "outside point projects onto the nearest triangle edge");
    expect(sample.position[0] == 6.0f && sample.position[1] == 6.0f &&
               sample.position[2] == 0.0f &&
               sample.distance_squared_xz == 9.0f &&
               sample.surface_flags == 0x20 && sample.leaf_offset == 8 &&
               sample.triangle_index == 0 && sample.edge_index == 0,
           "edge projection and plane height match the triangle geometry");

    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 12.0f, -3.0f, &sample),
           "projection clamps to the nearest edge endpoint");
    expect(sample.position[0] == 10.0f && sample.position[1] == 10.0f &&
               sample.position[2] == 0.0f &&
               sample.distance_squared_xz == 13.0f && sample.edge_index == 0,
           "endpoint ties keep the first edge in serialized order");

    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 8.0f, 8.0f, &sample),
           "fallback selects the sloped triangle edge");
    expect(sample.position[0] == 5.0f && sample.position[1] == 15.0f &&
               sample.position[2] == 5.0f &&
               sample.distance_squared_xz == 18.0f && sample.edge_index == 1,
           "sloped edge projection has the expected height and distance");

    expect(!awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(),
               std::numeric_limits<float>::infinity(), 0.0f, &sample),
           "non-finite edge query is rejected");
    expect(sample.position[0] == 0.0f && sample.surface_flags == 0,
           "failed edge projection clears its output");

    bytes = make_single_leaf();
    expect(!awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 0.0f, 0.0f, &sample),
           "empty leaf has no edge projection");

    bytes = make_one_level_sample_tree();
    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 18.0f, 18.0f, &sample) &&
               sample.leaf_offset == 8 + 0x34 * 4,
           "edge fallback uses the selected leaf");
    expect(!awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 2.0f, 2.0f, &sample),
           "edge fallback does not search other leaves");
}

void test_terrain_height_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionTerrainAdjustment adjustment;
    expect(awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f}, &adjustment),
           "terrain adjustment samples a containing triangle");
    expect(adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               adjustment.surface_flags == 0x20 &&
               !adjustment.used_edge_fallback,
           "inside terrain replaces only height and reports no fallback");

    expect(awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {6.0f, 50.0f, -3.0f}, &adjustment),
           "terrain adjustment handles a point outside the triangle");
    expect(adjustment.position == std::array<float, 3>{6.0f, 6.0f, 0.0f} &&
               adjustment.surface_flags == 0x20 &&
               adjustment.used_edge_fallback,
           "outside terrain uses the nearest edge position and height");

    expect(!awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(),
               {0.0f, std::numeric_limits<float>::infinity(), 0.0f},
               &adjustment),
           "non-finite proposed height is rejected");
    expect(adjustment.position == std::array<float, 3>{} &&
               !adjustment.used_edge_fallback,
           "failed terrain adjustment clears its output");
    expect(!awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {2.0f, 0.0f, 2.0f}, nullptr),
           "null terrain adjustment output is rejected");

    bytes = make_single_leaf();
    expect(!awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {0.0f, 0.0f, 0.0f}, &adjustment),
           "empty selected leaf cannot provide terrain adjustment");
}

void test_resolver_height_resampling() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionResolverHeightAdjustment adjustment;
    expect(awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f},
               &adjustment) &&
               adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               adjustment.surface_flags == 0x20 &&
               adjustment.leaf_offset == 8 &&
               !adjustment.used_edge_fallback,
           "resolver height samples the final position's containing triangle");
    expect(awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {6.0f, 50.0f, -3.0f},
               &adjustment) &&
               adjustment.position == std::array<float, 3>{6.0f, 6.0f, -3.0f} &&
               adjustment.surface_flags == 0x20 &&
               adjustment.used_edge_fallback,
           "resolver fallback replaces Y without moving the final X/Z");

    bytes = make_one_level_sample_tree();
    constexpr uint32_t populated_leaf = 8 + 0x34 * 4;
    expect(awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {12.0f, 50.0f, 12.0f},
               &adjustment) && adjustment.leaf_offset == populated_leaf &&
               adjustment.position == std::array<float, 3>{12.0f, 6.0f, 12.0f},
           "resolver height queries the populated final-position leaf");
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f},
               &adjustment) &&
               adjustment.position == std::array<float, 3>{} &&
               adjustment.leaf_offset == 0,
           "crossing into an empty leaf does not reuse the prior leaf");

    bytes = make_sample_leaf();
    bytes[6] = 0;
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f},
               &adjustment),
           "unverified resolver height mode is rejected");
    bytes[6] = 1;
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(),
               {2.0f, std::numeric_limits<float>::infinity(), 2.0f},
               &adjustment),
           "nonfinite resolver position is rejected");
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f}, nullptr),
           "null resolver height output is rejected");
}

void test_dynamic_contact_broad_phase() {
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    bool may_contact = false;
    expect(awl::evaluate_dynamic_contact_broad_phase(
               identity, {3.0f, 100.0f, 4.0f}, {0.0f, -50.0f, 0.0f},
               2.0f, 3.0f, &may_contact) && may_contact,
           "exact combined-radius boundary reaches narrow phase in X/Z");
    expect(awl::evaluate_dynamic_contact_broad_phase(
               identity, {3.0f, 100.0f, 4.0f}, {0.0f, 0.0f, 0.0f},
               2.0f, 2.0f, &may_contact) && !may_contact,
           "candidate beyond the combined radius skips narrow phase");

    awl::CollisionAffineTransform translated = identity;
    translated[3] = 10.0f;
    translated[11] = -5.0f;
    translated[1] = 1000.0f;
    translated[9] = 1000.0f;
    expect(awl::evaluate_dynamic_contact_broad_phase(
               translated, {3.0f, 100.0f, 4.0f}, {13.0f, -50.0f, -1.0f},
               0.0f, 0.0f, &may_contact) && may_contact,
           "world-to-object translation applies after the query Y is cleared");

    awl::CollisionAffineTransform rotated = identity;
    rotated[0] = 0.0f;
    rotated[2] = 1.0f;
    rotated[8] = -1.0f;
    rotated[10] = 0.0f;
    expect(awl::evaluate_dynamic_contact_broad_phase(
               rotated, {3.0f, 0.0f, 4.0f}, {4.0f, 0.0f, -3.0f},
               0.0f, 0.0f, &may_contact) && may_contact,
           "world-to-object rotation affects the local X/Z distance");

    expect(!awl::evaluate_dynamic_contact_broad_phase(
               identity, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
               -1.0f, 0.0f, &may_contact) && !may_contact,
           "negative collision radius is rejected and output is cleared");
    translated[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::evaluate_dynamic_contact_broad_phase(
               translated, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
               1.0f, 1.0f, &may_contact) && !may_contact,
           "nonfinite transform is rejected");
    expect(!awl::evaluate_dynamic_contact_broad_phase(
               identity, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
               1.0f, 1.0f, nullptr),
           "null broad-phase output is rejected");
}

void test_radius_vertex_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusVertexAdjustment adjustment;
    const std::array<float, 3> query{0.5f, 7.0f, 0.0f};
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment),
           "supported type-1 vertex radius query succeeds");
    expect(!adjustment.contact && adjustment.position == query,
           "unmarked triangle edges do not produce vertex contact");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment),
           "marked triangle edge participates in vertex radius query");
    expect(adjustment.contact && adjustment.triangle_index == 0 &&
               adjustment.vertex_index == 0 &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               adjustment.position[1] == 0.0f &&
               adjustment.position[2] == 0.0f,
           "nearest incident vertex pushes X/Z to radius plus 0.01 and uses vertex Y");

    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 0.0f}, 1.0f,
               &adjustment) &&
               adjustment.contact &&
               adjustment.position == std::array<float, 3>{0.0f, 7.0f, 0.0f},
           "exact vertex contact preserves the original point");
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), {1.0f, 7.0f, 0.0f}, 1.0f,
               &adjustment) && !adjustment.contact,
           "point exactly at radius has no vertex contact");

    put_be16(bytes, 8 + 0x34, 0x4000);
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment) &&
               !adjustment.contact,
           "edge-one bit does not mark the unrelated first vertex");
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 9.5f}, 1.0f,
               &adjustment) &&
               adjustment.contact && adjustment.vertex_index == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f &&
               adjustment.position[1] == 20.0f,
           "edge-one bit marks its second endpoint for radius response");

    bytes[6] = 0;
    expect(!awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment),
           "other collision mode is rejected by the bounded radius helper");
    expect(adjustment.position == std::array<float, 3>{} &&
               !adjustment.contact,
           "failed vertex query clears its output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, -1.0f, &adjustment),
           "negative radius is rejected");
    expect(!awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, nullptr),
           "null vertex query output is rejected");
}

void test_dynamic_contact_vertex_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusVertexAdjustment adjustment;
    const std::array<float, 3> query{0.5f, 7.0f, 0.0f};
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x20u,
               &adjustment) &&
               adjustment.contact && adjustment.triangle_index == 0 &&
               adjustment.vertex_index == 0 &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               adjustment.position[1] == 0.0f &&
               adjustment.position[2] == 0.0f,
           "dynamic vertex pass accepts an unmarked vertex on a matching surface");

    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) &&
               !adjustment.contact && adjustment.position == query,
           "surface mask excludes a nonmatching nonzero triangle flag");
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x10000u,
               &adjustment) && adjustment.contact,
           "surface mask bypass bit accepts the triangle");
    put_be16(bytes, 8 + 0x34, 0);
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && adjustment.contact,
           "zero surface flags pass the mask filter");

    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 0.0f}, 1.0f,
               0x40u, &adjustment) && adjustment.contact &&
               adjustment.position == std::array<float, 3>{0.0f, 7.0f, 0.0f},
           "exact dynamic vertex contact leaves the candidate unchanged");
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), {1.0f, 7.0f, 0.0f}, 1.0f,
               0x40u, &adjustment) && !adjustment.contact,
           "dynamic vertex radius boundary is strict");
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), {5.0f, 7.0f, 0.0f}, 6.0f,
               0x40u, &adjustment) && adjustment.contact &&
               adjustment.vertex_index == 0 &&
               std::fabs(adjustment.position[0] - 6.01f) < 0.0001f,
           "equal-distance vertices keep the first serialized vertex");

    bytes[6] = 0;
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unsupported collision mode rejects and clears dynamic vertex output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, -1.0f, 0x40u,
               &adjustment),
           "negative dynamic vertex radius is rejected");
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u, nullptr),
           "null dynamic vertex output is rejected");
}

void test_dynamic_contact_edge_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusEdgeAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x20u,
               &adjustment) && adjustment.contact &&
               adjustment.surface_flags == 0x20 &&
               adjustment.triangle_index == 0 && adjustment.edge_index == 0 &&
               adjustment.position[0] == 5.0f &&
               adjustment.position[1] == 7.0f &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "dynamic edge pass checks an unmarked edge on a matching surface");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == proposed,
           "dynamic edge pass excludes a nonmatching surface");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x10000u, &adjustment) && adjustment.contact,
           "dynamic edge pass accepts the surface-mask bypass bit");
    put_be16(bytes, 8 + 0x34, 0);
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && adjustment.contact,
           "zero surface flags remain eligible for dynamic edge response");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, {5.0f, 7.0f, 0.5f},
               1.0f, 0x40u, &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "dynamic edge response handles a candidate crossing the plane");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), {5.0f, 7.0f, 0.0f}, proposed,
               1.0f, 0x40u, &adjustment) && !adjustment.contact,
           "prior point on the edge plane does not produce dynamic contact");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, {10.0f, 7.0f, -0.5f},
               1.0f, 0x40u, &adjustment) && !adjustment.contact,
           "dynamic edge excludes its final endpoint");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 0.5f, 0x40u,
               &adjustment) && !adjustment.contact,
           "dynamic edge contact is strict at the radius boundary");

    bytes[6] = 0;
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unsupported collision mode rejects and clears dynamic edge output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, -1.0f, 0x40u,
               &adjustment),
           "negative dynamic edge radius is rejected");
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               nullptr),
           "null dynamic edge output is rejected");
}

void test_dynamic_contact_narrow_phase() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionDynamicContactAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, prior, 1.0f, 0x20u,
               0u, &adjustment) && !adjustment.contact &&
               adjustment.pass_count == 1 && adjustment.position == prior,
           "clear dynamic narrow phase ends after one edge-vertex pass");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x20u,
               0u, &adjustment) && adjustment.contact &&
               adjustment.first_edge_contact &&
               adjustment.first_edge_surface_flags == 0x20 &&
               !adjustment.reverted_to_prior && adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "first dynamic edge contact clears on the second pinned-leaf pass");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x20u,
               1u, &adjustment) && adjustment.contact &&
               adjustment.reverted_to_prior && adjustment.pass_count == 1 &&
               adjustment.position == prior,
           "contact flag one restores the prior position after the first hit");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 2.0f},
               {0.0f, 7.0f, 0.0f}, 1.0f, 0x20u, 0u, &adjustment) &&
               adjustment.contact && adjustment.reverted_to_prior &&
               adjustment.pass_count == 3 &&
               adjustment.position == std::array<float, 3>{0.0f, 7.0f, 2.0f},
           "persistent dynamic vertex contact restores the prior position");

    const std::array<float, 3> inside_prior{2.0f, 7.0f, 2.0f};
    const std::array<float, 3> inside_proposed{3.0f, 7.0f, 2.0f};
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), inside_prior, inside_proposed,
               1.0f, 0x20u, 2u, &adjustment) &&
               adjustment.used_containment_shortcut && adjustment.contact &&
               adjustment.reverted_to_prior && adjustment.pass_count == 0 &&
               adjustment.position == inside_prior,
           "matching prior triangle with flag two reports contact and prior point");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), inside_prior, inside_proposed,
               1.0f, 0x20u, 4u, &adjustment) &&
               adjustment.used_containment_shortcut && !adjustment.contact &&
               !adjustment.reverted_to_prior && adjustment.pass_count == 0 &&
               adjustment.position == inside_proposed,
           "matching prior triangle with flag four keeps the proposed point");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), inside_prior, inside_proposed,
               1.0f, 0x40u, 2u, &adjustment) &&
               !adjustment.used_containment_shortcut && !adjustment.contact &&
               adjustment.pass_count == 1 &&
               adjustment.position == inside_proposed,
           "nonmatching surface mask bypasses the containment shortcut");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, {5.0f, 7.0f, 0.5f},
               1.0f, 0x20u, 2u, &adjustment) &&
               !adjustment.used_containment_shortcut && adjustment.contact,
           "containment shortcut tests the prior point, not the proposal");

    bytes = make_one_level_sample_tree();
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, 0x20u, 0u,
               &adjustment) && adjustment.contact &&
               !adjustment.reverted_to_prior && adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f,
           "dynamic response keeps its selected leaf after crossing a boundary");
    bytes[6] = 0;
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x20u, 0u, &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unsupported dynamic collision mode rejects and clears output");
    bytes[6] = 1;
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               0x20u, 0u, &adjustment),
           "negative dynamic narrow-phase radius is rejected");
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x20u, 0u, nullptr),
           "null dynamic narrow-phase output is rejected");
}

void test_dynamic_object_contact() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicObjectQuery query;
    query.world_to_object = identity;
    query.object_to_world = identity;
    query.object_center_local = {5.0f, 0.0f, 0.0f};
    query.moving_radius = 1.0f;
    query.surface_mask = 0x20u;
    awl::CollisionDynamicObjectContactAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && adjustment.broad_phase_passed &&
               adjustment.contact && adjustment.local_narrow_phase.contact &&
               adjustment.position[0] == 5.0f &&
               adjustment.position[1] == 0.0f &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "contacted object response clears local Y before world transform");

    query.object_center_local = {100.0f, 0.0f, 0.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && !adjustment.broad_phase_passed &&
               !adjustment.contact && adjustment.position == proposed,
           "broad-phase miss preserves the original world proposal");
    query.object_center_local = {5.0f, 0.0f, -2.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, prior,
               &adjustment) && adjustment.broad_phase_passed &&
               !adjustment.contact &&
               adjustment.local_narrow_phase.pass_count == 1 &&
               adjustment.position == prior,
           "narrow-phase miss also preserves the original world proposal");

    query.world_to_object = identity;
    query.object_to_world = identity;
    query.world_to_object[1] = 1000.0f;
    query.world_to_object[9] = 1000.0f;
    query.world_to_object[3] = 10.0f;
    query.world_to_object[7] = -42.0f;
    query.world_to_object[11] = -5.0f;
    query.object_to_world[1] = 1000.0f;
    query.object_to_world[9] = 1000.0f;
    query.object_to_world[3] = -10.0f;
    query.object_to_world[7] = 42.0f;
    query.object_to_world[11] = 5.0f;
    query.object_center_local = {5.0f, 0.0f, 0.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, {-5.0f, 7.0f, 3.0f},
               {-5.0f, 9.0f, 4.5f}, &adjustment) &&
               adjustment.contact && adjustment.position[0] == -5.0f &&
               adjustment.position[1] == 42.0f &&
               std::fabs(adjustment.position[2] - 3.99f) < 0.0001f,
           "translation uses horizontal inputs and transforms contacted local output");

    query.world_to_object = {
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f, 0.0f};
    query.object_to_world = {
        0.0f, 0.0f, -1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, {2.0f, 7.0f, 5.0f},
               {0.5f, 9.0f, 5.0f}, &adjustment) &&
               adjustment.contact &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               adjustment.position[1] == 0.0f &&
               adjustment.position[2] == 5.0f,
           "rotation takes the local contact result back to world X/Z");

    query.object_to_world[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "nonfinite output transform is rejected and clears output");
    query.object_to_world = identity;
    bytes[6] = 0;
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment),
           "unsupported object collision mode is rejected");
    bytes[6] = 1;
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               nullptr),
           "null object contact output is rejected");
}

void test_first_dynamic_object_pass() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject object;
    object.identity = 1;
    object.enabled = true;
    object.category = 1;
    object.collision_flags = 2u;
    object.data = bytes.data();
    object.size = bytes.size();
    object.world_to_object = identity;
    object.object_to_world = identity;
    object.center_local = {5.0f, 0.0f, 0.0f};
    awl::CollisionDynamicPassAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};

    expect(awl::resolve_type1_first_dynamic_object_pass(
               &object, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 1 &&
               adjustment.contact_count == 1 &&
               adjustment.contact_flags_after == 1u &&
               adjustment.resolver_contact_bit == 4u &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "first list pass applies one type-1 contact and sets sticky bit one");

    awl::CollisionDynamicPassObject skipped_self = object;
    skipped_self.identity = 99;
    awl::CollisionDynamicPassObject skipped_disabled = object;
    skipped_disabled.identity = 2;
    skipped_disabled.enabled = false;
    awl::CollisionDynamicPassObject skipped_category = object;
    skipped_category.identity = 3;
    skipped_category.category = 7;
    awl::CollisionDynamicPassObject skipped_flags = object;
    skipped_flags.identity = 4;
    skipped_flags.collision_flags = 0;
    const std::array<awl::CollisionDynamicPassObject, 5> filtered{
        skipped_self, skipped_disabled, skipped_category, skipped_flags,
        object};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               filtered.data(), filtered.size(), 99, 1, prior, proposed,
               1.0f, 0u, 4u, &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 1 &&
               adjustment.contact_count == 1,
           "list pass skips self, disabled, wrong-category, and inert objects");

    awl::CollisionDynamicPassObject second = object;
    second.identity = 2;
    second.world_to_object[11] = 1.0f;
    second.object_to_world[11] = -1.0f;
    const std::array<awl::CollisionDynamicPassObject, 2> ordered{object, second};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               ordered.data(), ordered.size(), 0, 1, prior, proposed,
               1.0f, 0u, 4u, &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 2 &&
               adjustment.contact_count == 2 &&
               adjustment.contact_flags_after == 1u &&
               adjustment.position[0] == prior[0] &&
               adjustment.position[2] == prior[2],
           "second object sees the first response and sticky contact flag");

    expect(awl::resolve_type1_first_dynamic_object_pass(
               ordered.data(), ordered.size(), 0, 1, prior, proposed,
               1.0f, 0x20u, 0u, &adjustment) && !adjustment.contact &&
               adjustment.queried_objects == 0 &&
               adjustment.contact_flags_after == 0x20u &&
               adjustment.position == proposed,
           "disabled resolver pass leaves the proposal and flags unchanged");
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               ordered.data(), ordered.size(), 0, 1, prior, proposed,
               1.0f, 0u, 0x14u, &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "alternate flag-0x10 branch is rejected by this bounded pass");
    awl::CollisionDynamicPassObject circle = object;
    circle.collision_flags = 1u;
    circle.center_world = {5.0f, 20.0f, 0.0f};
    circle.radius = 0.5f;
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 1 &&
               adjustment.contact_count == 1 &&
               adjustment.contact_flags_after == 1u &&
               adjustment.resolver_contact_bit == 4u &&
               std::fabs(adjustment.position[2] + 1.51f) < 0.0001f &&
               adjustment.position[1] == proposed[1],
           "circle overlap pushes in X/Z and preserves the proposed height");
    const std::array<float, 3> circle_boundary{5.0f, 9.0f, -1.5f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, circle_boundary, 1.0f, 0u, 4u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == circle_boundary &&
               adjustment.queried_objects == 1,
           "circle contact uses a strict radius boundary");
    const std::array<float, 3> circle_outside{5.0f, 9.0f, -2.0f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, circle_outside, 1.0f, 0u, 4u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == circle_outside,
           "separated circles leave the candidate unchanged");
    const std::array<float, 3> circle_center{5.0f, 9.0f, 0.0f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, circle_center, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               adjustment.position[0] == circle_center[0] &&
               std::fabs(adjustment.position[2] - 1.51f) < 0.0001f &&
               adjustment.position[1] == circle_center[1],
           "coincident circle centers use the DOL's positive-Z fallback");
    circle.center_world = {5.0f, 0.0f, -1.0f};
    const std::array<awl::CollisionDynamicPassObject, 2> mixed{object, circle};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               mixed.data(), mixed.size(), 0, 1, prior, proposed, 1.0f,
               0u, 4u, &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 2 &&
               adjustment.contact_count == 2 &&
               std::fabs(adjustment.position[2] + 2.51f) < 0.0001f &&
               adjustment.position[1] == 0.0f,
           "circle response uses the type-1 result in ordered traversal");
    circle.collision_flags = 3u;
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "type-1 path takes priority when both collision bits are set");
    circle.collision_flags = 1u;
    circle.radius = -0.5f;
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment),
           "invalid circle radius is rejected");
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               nullptr, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment),
           "missing nonempty object list is rejected");
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               &object, 1, 0, 1, prior, proposed, -1.0f, 0u, 4u,
               &adjustment),
           "negative moving radius is rejected");
}

void test_later_dynamic_object_pass() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject first;
    first.identity = 1;
    first.enabled = true;
    first.category = 1;
    first.collision_flags = 2u;
    first.data = bytes.data();
    first.size = bytes.size();
    first.world_to_object = identity;
    first.object_to_world = identity;
    first.center_local = {5.0f, 0.0f, 0.0f};
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};
    awl::CollisionDynamicPassAdjustment first_result;
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &first, 1, 0, 1, prior, proposed, 1.0f, 0u, 0x67u,
               &first_result) && first_result.contact &&
               first_result.resolver_contact_bit == 4u,
           "movement flags enter the pre-terrain object pass");

    awl::CollisionDynamicPassObject later;
    later.identity = 1;
    later.enabled = true;
    later.category = 1;
    later.collision_flags = 1u;
    later.center_world = {5.0f, 100.0f, -1.0f};
    later.radius = 0.5f;
    std::array<float, 3> after_terrain = first_result.position;
    after_terrain[1] = 4.5f;
    awl::CollisionDynamicPassAdjustment later_result;
    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               first_result.contact_flags_after, 0x67u,
               &later_result) && later_result.contact &&
               later_result.queried_objects == 1 &&
               later_result.contact_count == 1 &&
               later_result.resolver_contact_bit == 2u &&
               later_result.contact_flags_after == 1u &&
               std::fabs(later_result.position[2] + 2.51f) < 0.0001f &&
               later_result.position[1] == after_terrain[1],
           "later pass consumes the post-terrain candidate and reports bit two");

    later.center_world[2] = 100.0f;
    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               0x21u, 0x67u, &later_result) && !later_result.contact &&
               later_result.contact_flags_after == 0x20u &&
               later_result.resolver_contact_bit == 0u &&
               later_result.position == after_terrain,
           "later pass clears inherited contact bit one before traversal");

    expect(awl::resolve_type1_later_dynamic_object_pass(
               &first, 1, 1, prior, proposed, 1.0f,
               1u, 0x67u, &later_result) && later_result.contact &&
               later_result.resolver_contact_bit == 2u &&
               std::fabs(later_result.position[2] + 1.01f) < 0.0001f,
           "later type-1 contact does not inherit first-pass reversion");

    awl::CollisionDynamicPassObject null_entry = later;
    null_entry.identity = 0;
    null_entry.center_world = {5.0f, 0.0f, -1.0f};
    const std::array<awl::CollisionDynamicPassObject, 2> entries{
        null_entry, later};
    expect(awl::resolve_type1_later_dynamic_object_pass(
               entries.data(), entries.size(), 1, prior, after_terrain,
               1.0f, 0u, 0x67u, &later_result) && !later_result.contact &&
               later_result.queried_objects == 1 &&
               later_result.position == after_terrain,
           "later list skips null object entries");

    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               0x21u, 4u, &later_result) && !later_result.contact &&
               later_result.queried_objects == 0 &&
               later_result.contact_flags_after == 0x21u,
           "later pass does not clear flags when its gate is disabled");

    later.center_world = {5.0f, 0.0f, -1.0f};
    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               0u, 0x12u, &later_result) && later_result.contact &&
               later_result.resolver_contact_bit == 2u,
           "first-pass alternate bit does not block the later pass");
}

void test_category1_static_and_movement_candidate() {
    std::vector<uint8_t> terrain = make_sample_leaf();
    const std::array<float, 3> prior{2.0f, 7.0f, 2.0f};
    const std::array<float, 3> proposed{2.0f, 50.0f, 2.0f};
    awl::CollisionCategory1StaticFlags flags;
    expect(awl::category1_static_surface_mask(0x67u, flags) == 0xC1u,
           "movement flags and clear runtime bytes produce the traced mask");
    flags.state_299a4 = true;
    flags.state_299a5 = true;
    flags.state_299a6 = true;
    flags.state_299a8 = true;
    flags.state_299a9 = true;
    flags.state_299af = true;
    flags.secondary_3f3 = true;
    flags.secondary_3f1 = true;
    expect(awl::category1_static_surface_mask(0x340u, flags) == 0x1FBFu,
           "each observed runtime byte contributes its DOL surface-mask bit");

    awl::CollisionCategory1StaticAdjustment static_result;
    expect(awl::resolve_type1_category1_static_contact(
               nullptr, 0, flags, 0x67u, prior, proposed, 0.3f, 0u,
               &static_result) && !static_result.slot_present &&
               !static_result.contact && static_result.position == proposed,
           "absent slot two copies the proposal without static contact");
    expect(!awl::resolve_type1_category1_static_contact(
               nullptr, terrain.size(), flags, 0x67u, prior, proposed,
               0.3f, 0u, &static_result),
           "nonempty missing static asset is rejected");
    std::vector<uint8_t> unsupported_static = terrain;
    unsupported_static[6] = 0;
    expect(!awl::resolve_type1_category1_static_contact(
               unsupported_static.data(), unsupported_static.size(),
               flags, 0x67u, prior, proposed, 0.3f, 0u,
               &static_result) &&
               static_result.position == std::array<float, 3>{},
           "unsupported slot-two collision mode is rejected");

    awl::CollisionCategory1MovementQuery query;
    query.terrain_data = terrain.data();
    query.terrain_size = terrain.size();
    query.moving_radius = 0.3f;
    awl::CollisionCategory1MovementAdjustment result;
    expect(awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               result.resolver_contact_bits == 0u &&
               !result.final_height_resampled &&
               !result.static_contact.slot_present,
           "clear movement candidate composes terrain and absent object stages");

    awl::CollisionDynamicPassObject first;
    first.identity = 1;
    first.enabled = true;
    first.category = 1;
    first.collision_flags = 1u;
    first.center_world = {2.0f, 0.0f, 2.0f};
    first.radius = 0.5f;
    awl::CollisionDynamicPassObject later = first;
    later.identity = 2;
    later.center_world[2] = 2.8f;
    query.first_objects = &first;
    query.first_object_count = 1;
    query.later_objects = &later;
    query.later_object_count = 1;
    expect(awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.first_pass.contact && result.later_pass.contact &&
               result.resolver_contact_bits == 6u &&
               result.final_height_resampled &&
               std::fabs(result.position[2] - 3.61f) < 0.0002f,
           "two object lists run around terrain and trigger final height lookup");
    awl::CollisionSurfaceSample final_surface;
    expect(awl::sample_type1_collision_surface(
               terrain.data(), terrain.size(), result.position[0],
               result.position[2], &final_surface) &&
               std::fabs(result.position[1] - final_surface.height) < 0.0001f,
           "final candidate height matches an independent surface query");

    query.first_objects = nullptr;
    query.first_object_count = 0;
    query.later_objects = nullptr;
    query.later_object_count = 0;
    query.static_data = terrain.data();
    query.static_size = terrain.size();
    query.static_flags.state_299a9 = true;
    const std::array<float, 3> static_prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> static_proposed{5.0f, 50.0f, 0.5f};
    expect(awl::resolve_type1_category1_movement_candidate(
               query, static_prior, static_proposed, &result) &&
               result.static_contact.slot_present &&
               result.static_contact.contact &&
               result.resolver_contact_bits == 1u &&
               result.final_height_resampled &&
               std::fabs(result.position[2] + 0.31f) < 0.0001f,
           "slot-two static contact sets bit one and resamples final height");
    awl::CollisionEdgeSample final_edge;
    expect(awl::project_type1_collision_to_edge(
               terrain.data(), terrain.size(), result.position[0],
               result.position[2], &final_edge) &&
               std::fabs(result.position[1] - final_edge.position[1]) < 0.0001f,
           "static response outside terrain gets edge-projected final height");

    const std::array<float, 3> inside_static_prior{5.0f, 7.0f, 2.0f};
    expect(awl::resolve_type1_category1_movement_candidate(
               query, inside_static_prior, static_proposed, &result) &&
               !result.static_contact.contact &&
               result.static_contact.narrow_phase.used_containment_shortcut &&
               result.resolver_contact_bits == 0u &&
               !result.final_height_resampled,
           "player contact flag four keeps a matching prior surface clear");

    terrain[6] = 0;
    expect(!awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.position == std::array<float, 3>{},
           "unsupported terrain mode rejects the composed candidate");
    terrain[6] = 1;
    expect(!awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, nullptr),
           "null composed result is rejected");
    query.terrain_data = terrain.data();
    query.moving_radius = -0.3f;
    expect(!awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.position == std::array<float, 3>{},
           "invalid moving radius rejects the full candidate");
}

void test_world_map_directional_contact_search() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    put_be16(bytes, 8 + 0x34, 1u);
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::WorldMapContactObject object;
    object.enabled = true;
    object.category = 1;
    object.collision_flags = 2u;
    object.data = bytes.data();
    object.size = bytes.size();
    object.contact_query.world_to_object = identity;
    object.contact_query.object_to_world = identity;
    object.contact_query.object_center_local = {5.0f, 0.0f, 0.0f};
    object.contact_query.object_radius = 10.0f;
    object.world_position = {20.0f, 30.0f, 40.0f};
    object.heading_axis = {0.0f, 0.0f, -1.0f};
    object.metadata = 42u;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.1f};
    const std::array<float, 3> fallback_axis{0.0f, 0.0f, 1.0f};
    awl::WorldMapContactResult result;
    expect(awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               result.matched && result.direction_code == 0 &&
               result.metadata == 42u &&
               result.world_position == object.world_position &&
               result.heading_axis == object.heading_axis &&
               result.queried_objects == 1 &&
               result.contacting_objects == 1,
           "first qualifying type-1 edge returns its direction and metadata");

    const std::array<float, 3> vertical_only{5.0f, 20.0f, -2.0f};
    expect(awl::query_world_map_directional_contact(
               &object, 1, 1, prior, vertical_only, fallback_axis,
               &result) && !result.matched && result.direction_code == 7 &&
               result.queried_objects == 0 &&
               result.world_position == vertical_only &&
               result.heading_axis == fallback_axis,
           "zero horizontal motion returns the caller's fallback without search");

    awl::WorldMapContactObject disabled = object;
    disabled.enabled = false;
    awl::WorldMapContactObject wrong_category = object;
    wrong_category.category = 7;
    awl::WorldMapContactObject circle_only = object;
    circle_only.collision_flags = 1u;
    const std::array<awl::WorldMapContactObject, 4> filtered{
        disabled, wrong_category, circle_only, object};
    expect(awl::query_world_map_directional_contact(
               filtered.data(), filtered.size(), 1, prior, proposed,
               fallback_axis, &result) && result.matched &&
               result.queried_objects == 1 &&
               result.contacting_objects == 1,
           "search skips disabled, wrong-category, and circle-only objects");

    awl::WorldMapContactObject wrong_heading = object;
    wrong_heading.heading_axis = {0.0f, 0.0f, 1.0f};
    wrong_heading.metadata = 99u;
    const std::array<awl::WorldMapContactObject, 2> ordered{
        wrong_heading, object};
    expect(awl::query_world_map_directional_contact(
               ordered.data(), ordered.size(), 1, prior, proposed,
               fallback_axis, &result) && result.matched &&
               result.metadata == 42u && result.queried_objects == 2 &&
               result.contacting_objects == 2,
           "a contact without matching heading leaves the next object eligible");

    expect(awl::query_world_map_directional_contact(
               &wrong_heading, 1, 1, prior, proposed, fallback_axis,
               &result) && !result.matched && result.direction_code == 7 &&
               result.metadata == 0u && result.world_position == proposed &&
               result.heading_axis == fallback_axis,
           "no qualifying heading uses the caller's code-seven fallback");

    bytes[6] = 0;
    expect(!awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               !result.matched && result.world_position ==
                   std::array<float, 3>{},
           "unsupported contact COL mode rejects the search");
    bytes[6] = 1;
    expect(!awl::query_world_map_directional_contact(
               nullptr, 1, 1, prior, proposed, fallback_axis, &result),
           "missing nonempty object list is rejected");
}

void test_world_map_movement_candidate_sequence() {
    std::vector<uint8_t> terrain = make_sample_leaf();
    awl::WorldMapMovementQuery query;
    query.pad.stick_x = 80;
    query.current_position = {2.0f, 7.0f, 2.0f};
    query.current_axis = {0.0f, 0.0f, 1.0f};
    query.collision.terrain_data = terrain.data();
    query.collision.terrain_size = terrain.size();
    awl::WorldMapMovementCandidate result;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               result.movement_enabled &&
               std::fabs(result.steering.current_speed - 0.03f) < 0.0001f &&
               result.proposed_position[0] > query.current_position[0] &&
               result.proposed_position[1] == query.current_position[1] &&
               !result.directional_contact.matched &&
               result.directional_contact.direction_code == 7 &&
               result.directional_contact.world_position ==
                   result.proposed_position &&
               result.resolved_position[1] != result.proposed_position[1] &&
               result.collision.resolver_contact_bits == 0u,
           "enabled movement runs steering, proposal, contact fallback, and terrain in order");
    awl::CollisionSurfaceSample surface;
    expect(awl::sample_type1_collision_surface(
               terrain.data(), terrain.size(), result.resolved_position[0],
               result.resolved_position[2], &surface) &&
               std::fabs(result.resolved_position[1] - surface.height) <
                   0.0001f,
           "composed movement candidate height matches independent terrain sampling");

    awl::CollisionDynamicPassObject circle;
    circle.identity = 2;
    circle.enabled = true;
    circle.category = 1;
    circle.collision_flags = 1u;
    circle.center_world = result.proposed_position;
    circle.radius = 0.5f;
    awl::WorldMapContactObject directional;
    directional.enabled = true;
    directional.category = 1;
    directional.collision_flags = 1u;
    query.collision.first_objects = &circle;
    query.collision.first_object_count = 1;
    query.collision.moving_radius = -99.0f;
    query.directional_objects = &directional;
    query.directional_object_count = 1;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               result.movement_enabled && result.collision.first_pass.contact &&
               result.collision.resolver_contact_bits == 4u &&
               result.directional_contact.queried_objects == 0 &&
               std::fabs(result.collision.first_pass.position[2] -
                         result.proposed_position[2] - 0.81f) < 0.0002f,
           "first-list circle uses the player's fixed radius while directional search skips it");
    directional.category = 2;
    expect(!awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled,
           "inconsistent first-list views are rejected before movement work");
    directional.category = 1;

    put_be16(terrain, 8 + 0x34, 1u);
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject wall;
    wall.identity = 3;
    wall.enabled = true;
    wall.category = 1;
    wall.collision_flags = 2u;
    wall.data = terrain.data();
    wall.size = terrain.size();
    wall.world_to_object = identity;
    wall.object_to_world = identity;
    wall.center_local = {5.0f, 0.0f, 0.0f};
    wall.radius = 10.0f;
    awl::WorldMapContactObject wall_direction;
    wall_direction.enabled = wall.enabled;
    wall_direction.category = wall.category;
    wall_direction.collision_flags = wall.collision_flags;
    wall_direction.data = wall.data;
    wall_direction.size = wall.size;
    wall_direction.contact_query.world_to_object = wall.world_to_object;
    wall_direction.contact_query.object_to_world = wall.object_to_world;
    wall_direction.contact_query.object_center_local = wall.center_local;
    wall_direction.contact_query.object_radius = wall.radius;
    wall_direction.world_position = {20.0f, 30.0f, 40.0f};
    wall_direction.heading_axis = {0.0f, 0.0f, -1.0f};
    wall_direction.metadata = 42u;
    awl::WorldMapMovementQuery wall_query;
    wall_query.pad.stick_y = -80;
    wall_query.current_position = {5.0f, 7.0f, -0.1f};
    wall_query.current_axis = {0.0f, 0.0f, 1.0f};
    wall_query.directional_objects = &wall_direction;
    wall_query.directional_object_count = 1;
    wall_query.collision.terrain_data = terrain.data();
    wall_query.collision.terrain_size = terrain.size();
    wall_query.collision.first_objects = &wall;
    wall_query.collision.first_object_count = 1;
    expect(awl::calculate_world_map_movement_candidate(
               wall_query, &result) && result.movement_enabled &&
               result.directional_contact.matched &&
               result.directional_contact.direction_code == 0 &&
               result.directional_contact.metadata == 42u &&
               result.collision.first_pass.queried_objects == 1,
           "same supplied type-one entry reaches direction metadata and collision pass");

    query.state_680 = 0;
    query.collision.terrain_data = nullptr;
    query.collision.terrain_size = 0;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled &&
               result.resolved_position == query.current_position &&
               result.proposed_position == query.current_position &&
               result.steering.current_speed == 0.0f &&
               result.directional_contact.queried_objects == 0,
           "blocked world-map state avoids steering and collision queries");

    query.state_680 = -1;
    query.state_58c = 1;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled &&
               result.resolved_position == query.current_position,
           "nonzero second state guard also blocks the movement sequence");

    query.state_58c = 0;
    expect(!awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled &&
               result.resolved_position == std::array<float, 3>{},
           "unsupported enabled collision input rejects the whole candidate");
    expect(!awl::calculate_world_map_movement_candidate(query, nullptr),
           "missing movement result is rejected");
}

void test_radius_edge_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusEdgeAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               !adjustment.contact && adjustment.position == proposed,
           "unmarked triangle edge does not produce radius contact");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment),
           "marked edge radius query succeeds");
    expect(adjustment.contact && adjustment.surface_flags == 0x2000 &&
               adjustment.triangle_index == 0 && adjustment.edge_index == 0 &&
               adjustment.position[0] == 5.0f &&
               adjustment.position[1] == 7.0f &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "edge pushes the candidate to radius plus 0.01 on its allowed side");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, {5.0f, 7.0f, 0.5f},
               1.0f, &adjustment) &&
               adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "edge response handles a candidate that crossed the plane");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), {5.0f, 7.0f, 2.0f}, proposed,
               1.0f, &adjustment) && !adjustment.contact,
           "prior point on the other side does not produce edge contact");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, {10.0f, 7.0f, -0.5f},
               1.0f, &adjustment) && !adjustment.contact,
           "edge segment excludes its final endpoint");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 0.5f,
               &adjustment) && !adjustment.contact,
           "candidate exactly at radius has no edge contact");

    bytes[6] = 0;
    expect(!awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment),
           "other collision mode is rejected by the bounded edge helper");
    expect(adjustment.position == std::array<float, 3>{} &&
               !adjustment.contact,
           "failed edge query clears its output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative edge radius is rejected");
    expect(!awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null edge query output is rejected");
}

void test_radius_pass_sequence() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusPassesAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               !adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 1 && adjustment.position == proposed,
           "no radius contact stops after one edge-vertex pass");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "one edge contact is followed by a clear second pass");

    const std::array<float, 3> blocked_prior{0.0f, 7.0f, 2.0f};
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), blocked_prior,
               {0.0f, 7.0f, 0.0f}, 1.0f, &adjustment) &&
               adjustment.contact && adjustment.reverted_to_prior &&
               adjustment.pass_count == 3 &&
               adjustment.position == blocked_prior,
           "persistent vertex contact restores the prior position after three passes");

    bytes = make_one_level_sample_tree();
    constexpr uint32_t payload = 8 + 0x34 * 5;
    put_be16(bytes, payload, 0x2000);
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, &adjustment) &&
               adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f,
           "edge response crossing into an empty leaf keeps the initial leaf for the next pass");

    put_be_s16(bytes, payload + 8, 11);
    put_be_s16(bytes, payload + 12, 11);
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), {10.1f, 7.0f, 11.0f},
               {10.1f, 7.0f, 11.0f}, 1.0f, &adjustment) &&
               adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[0] - 9.99f) < 0.0001f,
           "vertex response crossing into an empty leaf keeps the initial leaf for the next pass");
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative sequence radius is rejected");
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null sequence output is rejected");
}

void test_terrain_radius_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionTerrainRadiusAdjustment adjustment;
    const std::array<float, 3> prior{2.0f, 7.0f, 2.0f};
    const std::array<float, 3> proposed{2.0f, 50.0f, 2.0f};
    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 0.0f,
               &adjustment) &&
               adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               adjustment.surface_flags == 0x20 &&
               !adjustment.terrain_contact &&
               !adjustment.initial_edge_fallback &&
               !adjustment.final_edge_fallback &&
               !adjustment.radius_contact && adjustment.radius_pass_count == 0,
           "zero radius keeps the primary height and skips radius passes");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               !adjustment.terrain_contact && !adjustment.radius_contact &&
               adjustment.radius_pass_count == 1,
           "clear radius pass keeps the first sampled height");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, {6.0f, 50.0f, -3.0f},
               0.0f, &adjustment) &&
               adjustment.position == std::array<float, 3>{6.0f, 6.0f, 0.0f} &&
               adjustment.terrain_contact && !adjustment.radius_contact &&
               adjustment.initial_edge_fallback &&
               !adjustment.final_edge_fallback,
           "initial miss projects onto the selected leaf edge");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, {6.0f, 50.0f, -3.0f},
               1.0f, &adjustment) && adjustment.terrain_contact &&
               adjustment.initial_edge_fallback && !adjustment.radius_contact &&
               adjustment.radius_pass_count == 1,
           "initial edge fallback remains terrain contact after a clear radius pass");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), {0.5f, 7.0f, 0.0f},
               {0.5f, 50.0f, 0.0f}, 1.0f, &adjustment) &&
               adjustment.terrain_contact && adjustment.radius_contact &&
               !adjustment.reverted_to_prior &&
               adjustment.radius_pass_count == 2 &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               std::fabs(adjustment.position[1] - 1.01f) < 0.0001f &&
               !adjustment.final_edge_fallback,
           "vertex contact resamples height at the adjusted X/Z");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 2.0f},
               {0.0f, 50.0f, 0.0f}, 1.0f, &adjustment) &&
               adjustment.terrain_contact && adjustment.radius_contact &&
               adjustment.reverted_to_prior &&
               adjustment.radius_pass_count == 3 &&
               adjustment.position == std::array<float, 3>{0.0f, 4.0f, 2.0f},
           "third-pass revert resamples terrain height at the prior X/Z");

    bytes = make_one_level_sample_tree();
    constexpr uint32_t payload = 8 + 0x34 * 5;
    put_be16(bytes, payload, 0x2000);
    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 50.0f, 10.5f}, 1.0f, &adjustment) &&
               adjustment.terrain_contact && adjustment.radius_contact &&
               adjustment.radius_pass_count == 2 &&
               adjustment.final_edge_fallback &&
               adjustment.position == std::array<float, 3>{15.0f, 5.0f, 10.0f},
           "post-contact height fallback stays in the original leaf after crossing");

    bytes = make_single_leaf();
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               adjustment.position == std::array<float, 3>{} &&
               !adjustment.terrain_contact,
           "empty selected leaf fails and clears its output");
    bytes = make_sample_leaf();
    bytes[6] = 0;
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment),
           "untranslated radius mode is rejected");
    bytes[6] = 1;
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative radius is rejected");
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null terrain radius output is rejected");
}

bool inspect_local_asset(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "Unable to open collision asset: %s\n", path);
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    awl::CollisionTreeAnalysis analysis;
    if (!analyze(bytes, analysis)) {
        std::fprintf(stderr, "Collision asset validation failed: %s\n", path);
        return false;
    }
    std::printf("Collision asset validated: %s size=%zu nodes=%zu leaves=%zu "
                "triangles=%zu vertices=%zu depth=%u header=%u/%u/%u/%u\n",
                path, bytes.size(), analysis.node_count, analysis.leaf_count,
                analysis.triangle_count, analysis.vertex_count,
                analysis.max_depth, analysis.format, analysis.header_byte_5,
                analysis.header_byte_6, analysis.header_byte_7);
    const float sample_x = (analysis.root_min[0] + analysis.root_max[0]) * 0.5f;
    const float sample_z = (analysis.root_min[2] + analysis.root_max[2]) * 0.5f;
    awl::CollisionSurfaceSample sample;
    if (awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                            sample_x, sample_z, &sample)) {
        std::printf("Primary surface sample: x=%.3f z=%.3f y=%.3f flags=0x%04X "
                    "leaf=%u triangle=%u\n",
                    sample_x, sample_z, sample.height, sample.surface_flags,
                    sample.leaf_offset, sample.triangle_index);
    } else {
        std::printf("No primary surface sample at root center: x=%.3f z=%.3f\n",
                    sample_x, sample_z);
    }
    awl::CollisionEdgeSample edge_sample;
    const float outside_x = analysis.root_min[0] - 1.0f;
    if (!awl::project_type1_collision_to_edge(
            bytes.data(), bytes.size(), outside_x, sample_z, &edge_sample)) {
        std::fprintf(stderr, "No edge fallback near root boundary: %s\n", path);
        return false;
    }
    std::printf("Edge fallback sample: query=(%.3f, %.3f) "
                "position=(%.3f, %.3f, %.3f) flags=0x%04X "
                "leaf=%u triangle=%u edge=%u\n",
                outside_x, sample_z, edge_sample.position[0],
                edge_sample.position[1], edge_sample.position[2],
                edge_sample.surface_flags, edge_sample.leaf_offset,
                edge_sample.triangle_index, edge_sample.edge_index);
    awl::CollisionTerrainAdjustment adjustment;
    if (!awl::adjust_type1_collision_terrain_height(
            bytes.data(), bytes.size(), {outside_x, 100.0f, sample_z},
            &adjustment) ||
        !adjustment.used_edge_fallback ||
        adjustment.position != edge_sample.position ||
        adjustment.surface_flags != edge_sample.surface_flags) {
        std::fprintf(stderr, "Terrain height adjustment failed: %s\n", path);
        return false;
    }
    awl::CollisionResolverHeightAdjustment resolver_height;
    if (!awl::resample_type1_collision_resolver_height(
            bytes.data(), bytes.size(),
            {outside_x, 100.0f, sample_z}, &resolver_height) ||
        !resolver_height.used_edge_fallback ||
        resolver_height.position !=
            std::array<float, 3>{outside_x, edge_sample.position[1], sample_z}) {
        std::fprintf(stderr, "Resolver height resample failed: %s\n", path);
        return false;
    }
    awl::CollisionRadiusVertexAdjustment vertex_adjustment;
    if (!awl::adjust_type1_collision_radius_vertex(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z}, 0.3f,
            &vertex_adjustment) ||
        !std::isfinite(vertex_adjustment.position[0]) ||
        !std::isfinite(vertex_adjustment.position[1]) ||
        !std::isfinite(vertex_adjustment.position[2])) {
        std::fprintf(stderr, "Radius vertex query failed: %s\n", path);
        return false;
    }
    awl::CollisionRadiusEdgeAdjustment edge_adjustment;
    if (!awl::adjust_type1_collision_radius_edge(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z - 1.0f},
            {sample_x, sample.height, sample_z}, 0.3f,
            &edge_adjustment) ||
        !std::isfinite(edge_adjustment.position[0]) ||
        !std::isfinite(edge_adjustment.position[1]) ||
        !std::isfinite(edge_adjustment.position[2])) {
        std::fprintf(stderr, "Radius edge query failed: %s\n", path);
        return false;
    }
    awl::CollisionRadiusPassesAdjustment passes;
    if (!awl::adjust_type1_collision_radius_passes(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z - 1.0f},
            {sample_x, sample.height, sample_z}, 0.3f,
            &passes) ||
        !std::isfinite(passes.position[0]) ||
        !std::isfinite(passes.position[1]) ||
        !std::isfinite(passes.position[2])) {
        std::fprintf(stderr, "Radius pass sequence failed: %s\n", path);
        return false;
    }
    awl::CollisionTerrainRadiusAdjustment terrain_radius;
    if (!awl::adjust_type1_collision_terrain_with_radius(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z},
            {sample_x, sample.height, sample_z}, 0.3f,
            &terrain_radius) ||
        !std::isfinite(terrain_radius.position[0]) ||
        !std::isfinite(terrain_radius.position[1]) ||
        !std::isfinite(terrain_radius.position[2])) {
        std::fprintf(stderr, "Terrain radius branch failed: %s\n", path);
        return false;
    }
    awl::CollisionCategory1MovementQuery movement_query;
    movement_query.terrain_data = bytes.data();
    movement_query.terrain_size = bytes.size();
    movement_query.moving_radius = 0.3f;
    awl::CollisionCategory1MovementAdjustment movement;
    const std::array<float, 3> route_point{
        sample_x, sample.height, sample_z};
    if (!awl::resolve_type1_category1_movement_candidate(
            movement_query, route_point, route_point, &movement) ||
        movement.position != terrain_radius.position ||
        movement.resolver_contact_bits !=
            (terrain_radius.terrain_contact ? 1u : 0u)) {
        std::fprintf(stderr, "Category-1 candidate composition failed: %s\n",
                     path);
        return false;
    }
    awl::WorldMapMovementQuery sequence;
    sequence.pad.stick_x = 80;
    sequence.current_position = route_point;
    sequence.current_axis = {0.0f, 0.0f, 1.0f};
    sequence.collision.terrain_data = bytes.data();
    sequence.collision.terrain_size = bytes.size();
    awl::WorldMapMovementCandidate candidate;
    awl::CollisionSurfaceSample moved_surface;
    if (!awl::calculate_world_map_movement_candidate(
            sequence, &candidate) || !candidate.movement_enabled ||
        candidate.proposed_position[0] <= route_point[0] ||
        !awl::sample_type1_collision_surface(
            bytes.data(), bytes.size(), candidate.resolved_position[0],
            candidate.resolved_position[2], &moved_surface) ||
        std::fabs(candidate.resolved_position[1] - moved_surface.height) >
            0.0001f) {
        std::fprintf(stderr, "Movement sequence at local sample failed: %s\n",
                     path);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    test_valid_structures();
    test_rejects_unsupported_or_truncated_files();
    test_rejects_invalid_offsets();
    test_primary_surface_sample();
    test_nearest_edge_fallback();
    test_terrain_height_adjustment();
    test_resolver_height_resampling();
    test_dynamic_contact_broad_phase();
    test_dynamic_contact_edge_adjustment();
    test_dynamic_contact_vertex_adjustment();
    test_dynamic_contact_narrow_phase();
    test_dynamic_object_contact();
    test_first_dynamic_object_pass();
    test_later_dynamic_object_pass();
    test_category1_static_and_movement_candidate();
    test_world_map_directional_contact_search();
    test_world_map_movement_candidate_sequence();
    test_radius_vertex_adjustment();
    test_radius_edge_adjustment();
    test_radius_pass_sequence();
    test_terrain_radius_adjustment();

    for (int index = 1; index < argc; ++index) {
        if (!inspect_local_asset(argv[index])) {
            ++failures;
        }
    }
    if (failures == 0) {
        std::puts("Collision asset tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
