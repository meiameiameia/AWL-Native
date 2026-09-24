#include "awl/collision_asset.h"

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
    return true;
}

} // namespace

int main(int argc, char** argv) {
    test_valid_structures();
    test_rejects_unsupported_or_truncated_files();
    test_rejects_invalid_offsets();
    test_primary_surface_sample();
    test_nearest_edge_fallback();

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
