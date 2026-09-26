#include "awl/collision_asset.h"

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
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, &adjustment),
           "candidate crossing the initially selected leaf is rejected");
    expect(adjustment.position == std::array<float, 3>{} &&
               adjustment.pass_count == 0,
           "unsupported leaf crossing clears the sequence output");
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative sequence radius is rejected");
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null sequence output is rejected");
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
    test_radius_vertex_adjustment();
    test_radius_edge_adjustment();
    test_radius_pass_sequence();

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
