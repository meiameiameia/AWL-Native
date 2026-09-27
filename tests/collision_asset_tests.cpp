#include "awl/collision_asset.h"
#include "awl/world_map_contact.h"
#include "awl/world_map_collision_registry.h"
#include "awl/world_map_actor_step.h"
#include "awl/world_map_movement.h"
#include "awl/world_map_scene_index.h"
#include "awl/world_map_collision_assets.h"
#include "awl/world_map_collision_records.h"
#include "awl/filesystem.h"
#include "awl/memory.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
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

std::vector<uint8_t> make_record_arc(const std::vector<uint8_t>& col) {
    std::vector<uint8_t> arc(0x50 + col.size(), 0);
    put_be32(arc, 0, 0x55AA382Du);
    put_be32(arc, 4, 0x20);
    put_be32(arc, 8, 0x30);
    put_be32(arc, 12, 0x50);
    put_be32(arc, 0x20, 0x01000000u);
    put_be32(arc, 0x28, 2);
    put_be32(arc, 0x2c, 1);
    put_be32(arc, 0x30, 0x50);
    put_be32(arc, 0x34, static_cast<uint32_t>(col.size()));
    const char name[] = "fixture.col";
    std::memcpy(arc.data() + 0x39, name, sizeof(name));
    std::copy(col.begin(), col.end(), arc.begin() + 0x50);
    return arc;
}

std::vector<uint8_t> make_single_record_arc() {
    std::vector<uint8_t> col = make_single_leaf();
    col[5] = 0;
    col[6] = 0;
    put_be_s16(col, 8 + 6, 6);
    put_be_s16(col, 8 + 8, 8);
    return make_record_arc(col);
}

void test_world_map_collision_archive() {
    const auto fixture = make_single_record_arc();
    awl::WorldMapCollisionArchive archive;
    awl::WorldMapCollisionRecordView view;
    expect(archive.parse(fixture) && archive.record_count() == 1 &&
               archive.lookup(0, &view) && view.size == 0x3c &&
               view.analysis != nullptr && view.analysis->header_byte_6 == 0 &&
               std::strcmp(view.name, "fixture.col") == 0 &&
               view.center_local == std::array<float, 3>{3.0f, 4.0f, 0.0f} &&
               view.radius_local == 5.0f,
           "ARC entry one maps to record zero with DOL-derived local bounds");
    expect(!archive.lookup(1, &view) && view.data == nullptr &&
               !archive.lookup(0, nullptr),
           "out-of-range and null record lookups fail without a sentinel");
    awl::WorldMapCollisionRecordPools unloaded;
    std::vector<awl::WorldMapMapseCollisionMatch> unavailable(1);
    expect(!unloaded.find_mapse_matches(2, &unavailable) &&
               unavailable.empty(),
           "unloaded mapse lookup fails and clears stale matches");

    auto invalid = fixture;
    put_be32(invalid, 0x30, static_cast<uint32_t>(invalid.size() - 2));
    expect(!archive.parse(invalid) && archive.record_count() == 0,
           "out-of-bounds ARC entry fails and clears prior records");
    invalid = fixture;
    put_be32(invalid, 0x28, UINT32_MAX);
    expect(!archive.parse(invalid), "overflowing ARC entry count is rejected");
    invalid = fixture;
    put_be32(invalid, 0x2c, 0x00FFFFFFu);
    expect(!archive.parse(invalid), "out-of-bounds ARC name is rejected");
    invalid = fixture;
    invalid[0x39] = 0;
    expect(!archive.parse(invalid), "empty ARC file name is rejected");
    invalid = fixture;
    invalid.resize(0x50 + 8);
    expect(!archive.parse(invalid), "truncated embedded COL is rejected");
    invalid = fixture;
    invalid[0x50 + 6] = 2;
    expect(!archive.parse(invalid), "unsupported embedded COL mode is rejected");
    invalid.assign(0x60 + 0x3c, 0);
    put_be32(invalid, 0, 0x55AA382Du);
    put_be32(invalid, 4, 0x20);
    put_be32(invalid, 8, 0x40);
    put_be32(invalid, 12, 0x60);
    put_be32(invalid, 0x20, 0x01000000u);
    put_be32(invalid, 0x28, 3);
    for (const size_t node : {size_t{0x2c}, size_t{0x38}}) {
        put_be32(invalid, node, 1);
        put_be32(invalid, node + 4, 0x60);
        put_be32(invalid, node + 8, 0x3c);
    }
    std::memcpy(invalid.data() + 0x45, "fixture.col", 12);
    std::copy(fixture.begin() + 0x50, fixture.end(), invalid.begin() + 0x60);
    expect(!archive.parse(invalid), "overlapping ARC payloads are rejected");
    invalid = fixture;
    put_be32(invalid, 0, 0);
    expect(!archive.parse(invalid), "incorrect ARC signature is rejected");
}

void test_world_map_room_collision_mapping() {
    using Status = awl::WorldMapRoomCollisionStatus;
    awl::WorldMapRoomCollisionState state;
    awl::WorldMapRoomCollisionSelection selected;
    constexpr uint32_t expected_three[6] = {40, 45, 46, 47, 47, 47};
    constexpr uint32_t expected_forty[6] = {0, 1, 2, 3, 4, 5};
    for (uint32_t phase = 0; phase < 6; ++phase) {
        state.phase_index = phase;
        expect(awl::select_world_map_room_collision_record(
                   3, state, &selected) == Status::Found &&
                   selected.record_index == expected_three[phase],
               "room ID 3 follows the DOL phase remap");
        expect(awl::select_world_map_room_collision_record(
                   40, state, &selected) == Status::Found &&
                   selected.record_index == expected_forty[phase] &&
                   selected.remapped_id ==
                       (phase == 0 ? 40u : 76u + phase),
               "room ID 40 selects its six phase records");
        expect(awl::select_world_map_room_collision_record(
                   5, state, &selected) == Status::Found &&
                   selected.record_index == (phase < 3 ? 41u : 48u),
               "room ID 5 changes record in later phases");
    }
    state = {};
    expect(awl::select_world_map_room_collision_record(
               7, state, &selected) == Status::Found &&
               selected.remapped_id == 7 && selected.record_index == 52,
           "room ID 7 clear-state record");
    state.state_299a6 = true;
    expect(awl::select_world_map_room_collision_record(
               7, state, &selected) == Status::Found &&
               selected.remapped_id == 50 && selected.record_index == 52,
           "room ID 7 state byte remaps its key");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               10, state, &selected) == Status::Found &&
               selected.record_index == 49,
           "room ID 10 clear-state record");
    state.state_299a5 = true;
    expect(awl::select_world_map_room_collision_record(
               10, state, &selected) == Status::Found &&
               selected.remapped_id == 51 && selected.record_index == 50,
           "room ID 10 state byte selects a different record");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               13, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 13,
           "room ID 13 has no record with clear state");
    state.state_299ae = true;
    expect(awl::select_world_map_room_collision_record(
               13, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 52,
           "room ID 13 remains unmapped after its state remap");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               4, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 4,
           "room ID 4 sentinel returns no record");
    state.phase_index = 3;
    expect(awl::select_world_map_room_collision_record(
               4, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 48,
           "room ID 4 later-phase key is also unmapped");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               82, state, &selected) == Status::UnsupportedInput &&
               selected.remapped_id == 0,
           "room IDs outside the bounded table are rejected");
    state.phase_index = 6;
    expect(awl::select_world_map_room_collision_record(
               40, state, &selected) == Status::UnsupportedInput &&
               selected.remapped_id == 0 &&
               awl::select_world_map_room_collision_record(
                   40, state, nullptr) == Status::UnsupportedInput,
           "invalid phase and null result are rejected");
    awl::WorldMapCollisionRecordPools unloaded;
    awl::WorldMapCollisionRecordView view;
    state = {};
    expect(unloaded.lookup_room_object(40, state, &view) ==
               Status::MissingRecord && view.data == nullptr &&
               unloaded.lookup_room_object(4, state, &view) ==
                   Status::NoMapping &&
               unloaded.lookup_room_object(40, state, nullptr) ==
                   Status::UnsupportedInput,
           "room lookup distinguishes absent data, no mapping, and bad output");
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

void test_world_map_archive_rejects_untranslated_tree_shape() {
    auto tree = make_one_level_tree();
    tree[6] = 0;
    awl::CollisionTreeAnalysis shape;
    expect(awl::analyze_type1_collision_asset(tree.data(), tree.size(), &shape) &&
               shape.node_count == 5 && shape.leaf_count == 4,
           "multi-leaf rejection fixture is independently valid type-1 COL");
    awl::WorldMapCollisionArchive archive;
    expect(!archive.parse(make_record_arc(tree)) && archive.record_count() == 0,
           "multi-leaf archive record waits for translated leaf selection");
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
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f,
           "type-1 mode zero keeps the dynamic vertex response");
    bytes[6] = 2;
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unverified dynamic vertex mode is rejected and clears output");
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
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "type-1 mode zero keeps the dynamic edge response");
    bytes[6] = 2;
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unverified dynamic edge mode is rejected and clears output");
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
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, 0x20u, 0u,
               &adjustment) && adjustment.contact &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f,
           "type-1 mode zero keeps the pinned-leaf narrow-phase response");
    bytes[6] = 2;
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x20u, 0u, &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unverified dynamic narrow-phase mode is rejected");
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
    query.world_to_object = identity;
    bytes[6] = 0;
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "type-1 mode zero supports the transformed object response");
    bytes[6] = 2;
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment),
           "unverified object collision mode is rejected");
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

void test_third_dynamic_object_pass() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject type1;
    type1.identity = 1;
    type1.enabled = true;
    type1.category = 1;
    type1.collision_flags = 2u;
    type1.data = bytes.data();
    type1.size = bytes.size();
    type1.world_to_object = identity;
    type1.object_to_world = identity;
    type1.center_local = {5.0f, 0.0f, 0.0f};
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};
    awl::CollisionDynamicPassAdjustment result;
    expect(awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 0, 0, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result) && result.contact && result.queried_objects == 1 &&
               result.contact_count == 1 &&
               result.contact_flags_after == 5u &&
               result.resolver_contact_bit == 8u &&
               std::fabs(result.position[2] + 1.01f) < 0.0001f,
           "third-list type-1 contact carries flags and reports resolver bit eight");

    awl::CollisionDynamicPassObject self = type1;
    self.identity = 99;
    awl::CollisionDynamicPassObject disabled = type1;
    disabled.identity = 2;
    disabled.enabled = false;
    awl::CollisionDynamicPassObject wrong_category = type1;
    wrong_category.identity = 3;
    wrong_category.category = 7;
    awl::CollisionDynamicPassObject circle = type1;
    circle.identity = 4;
    circle.collision_flags = 1u;
    circle.center_world = {5.0f, 0.0f, 0.0f};
    circle.radius = 0.5f;
    const std::array<awl::CollisionDynamicPassObject, 5> ordered{
        self, disabled, wrong_category, type1, circle};
    expect(awl::resolve_type1_third_dynamic_object_pass(
               ordered.data(), ordered.size(), 99, 0, 1, prior, proposed,
               1.0f, 4u, 0x8u, &result) && result.contact &&
               result.queried_objects == 2 && result.contact_count == 2 &&
               std::fabs(result.position[2] + 1.51f) < 0.0001f,
           "third-list traversal filters entries and gives each contact the prior response");
    expect(awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 0, 2u, 1, prior, proposed, 1.0f, 4u, 0u,
               &result) && !result.contact && result.position == proposed &&
               result.queried_objects == 0,
           "disabled third-list gate leaves the proposal untouched");
    expect(awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 0, 2u, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result) && result.contact &&
               result.resolver_contact_bit == 8u,
           "an absent source selects the ordinary third-list branch");
    expect(!awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 99, 2u, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result) && result.position == std::array<float, 3>{},
           "source flag two selects the still-unsupported alternate branch");
    expect(!awl::resolve_type1_third_dynamic_object_pass(
               nullptr, 1, 0, 0, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result),
           "a nonempty third list requires object data");
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
    std::vector<uint8_t> mode_zero_static = terrain;
    mode_zero_static[6] = 0;
    expect(awl::resolve_type1_category1_static_contact(
               mode_zero_static.data(), mode_zero_static.size(),
               flags, 0x67u, prior, proposed, 0.3f, 4u,
               &static_result) && static_result.slot_present &&
               !static_result.contact &&
               static_result.narrow_phase.used_containment_shortcut &&
               static_result.position == proposed,
           "slot-two mode zero uses the type-1 containment path");
    mode_zero_static[6] = 2;
    expect(!awl::resolve_type1_category1_static_contact(
               mode_zero_static.data(), mode_zero_static.size(),
               flags, 0x67u, prior, proposed, 0.3f, 4u,
               &static_result) &&
               static_result.position == std::array<float, 3>{},
           "unverified slot-two mode is rejected");

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

    mode_zero_static[6] = 0;
    query.static_data = mode_zero_static.data();
    query.static_size = mode_zero_static.size();
    expect(awl::resolve_type1_category1_movement_candidate(
               query, static_prior, static_proposed, &result) &&
               result.static_contact.contact &&
               result.resolver_contact_bits == 1u &&
               result.final_height_resampled &&
               std::fabs(result.position[2] + 0.31f) < 0.0001f,
           "mode-zero slot-two wall contact composes and resamples height");
    query.static_data = terrain.data();
    query.static_size = terrain.size();

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

void test_world_map_packed_saved_conditions() {
    const std::array<uint8_t, 2> three_bit_values{0xD1, 0x6A};
    awl::WorldMapPackedSavedValues packed{
        three_bit_values.data(), three_bit_values.size(), 3};
    uint32_t value = 99;
    expect(awl::read_world_map_packed_saved_value(packed, 0, &value) &&
               value == 1 &&
               awl::read_world_map_packed_saved_value(packed, 1, &value) &&
               value == 2 &&
               awl::read_world_map_packed_saved_value(packed, 2, &value) &&
               value == 3 &&
               awl::read_world_map_packed_saved_value(packed, 3, &value) &&
               value == 5 &&
               awl::read_world_map_packed_saved_value(packed, 4, &value) &&
               value == 6,
           "packed values use low-bit-first order and cross byte boundaries");
    expect(!awl::read_world_map_packed_saved_value(packed, 5, &value) &&
               value == 0,
           "incomplete packed element is rejected and output cleared");
    packed.width_bits = 33;
    expect(!awl::read_world_map_packed_saved_value(packed, 0, &value) &&
               !awl::read_world_map_packed_saved_value(packed, 0, nullptr),
           "unsupported packed width and null output are rejected");
    packed.width_bits = 3;
    expect(!awl::read_world_map_packed_saved_value(
               packed, std::numeric_limits<size_t>::max(), &value),
           "packed index arithmetic cannot wrap");
    const std::array<uint8_t, 4> full_width{0x12, 0x34, 0x56, 0x78};
    const awl::WorldMapPackedSavedValues full{
        full_width.data(), full_width.size(), 32};
    expect(awl::read_world_map_packed_saved_value(full, 0, &value) &&
               value == 0x78563412u,
           "a 32-bit packed element preserves its low-bit-first byte order");
    packed.width_bits = 0;
    expect(!awl::read_world_map_packed_saved_value(packed, 0, &value),
           "zero-width packed input is unsupported");

    std::array<uint8_t, 47> later_saved{};
    later_saved[38] = 0x20; // Index 0x135, bit 5.
    later_saved[46] = 0x01; // Index 0x170, bit 0.
    const std::array<uint8_t, 2> room_saved{0xC1, 0xC7};
    const awl::WorldMapPackedSavedValues later{
        later_saved.data(), later_saved.size(), 1};
    const awl::WorldMapPackedSavedValues room{
        room_saved.data(), room_saved.size(), 1};
    awl::WorldMapRoomConditionInputs inputs;
    inputs.phase_index = 2;
    inputs.state_299a7 = 1;
    expect(awl::decode_world_map_room_saved_conditions(later, room, &inputs) &&
               inputs.phase_index == 2 && inputs.state_299a7 == 1 &&
               inputs.value_120a4_135_nonzero &&
               inputs.value_120a4_170_nonzero,
           "saved-condition decoding preserves other caller-supplied state");
    awl::WorldMapRoomStaticConditions conditions;
    expect(awl::evaluate_world_map_room_static_conditions(inputs, &conditions) &&
               conditions == awl::WorldMapRoomStaticConditions{
                   true, true, true, true, false, true, true,
                   true, true, true, true, true, true, true},
           "packed table bits feed all ten traced room predicates");
    const auto before = inputs;
    const awl::WorldMapPackedSavedValues short_room{room_saved.data(), 1, 1};
    expect(!awl::decode_world_map_room_saved_conditions(
               later, short_room, &inputs) &&
               inputs.values_14ad4_nonzero == before.values_14ad4_nonzero &&
               inputs.value_120a4_135_nonzero == before.value_120a4_135_nonzero &&
               !awl::decode_world_map_room_saved_conditions(later, room,
                                                             nullptr),
           "truncated tables fail without partially changing condition inputs");
}

void test_world_map_room_condition_evaluator() {
    awl::WorldMapRoomConditionInputs inputs;
    awl::WorldMapRoomStaticConditions conditions;
    expect(awl::evaluate_world_map_room_static_conditions(inputs, &conditions) &&
               conditions == awl::WorldMapRoomStaticConditions{
                   true, false, true, true, false, false, false,
                   false, false, false, false, false, false, false},
           "phase zero and clear state produce the traced room predicates");
    inputs.phase_index = 2;
    inputs.state_299af = 1;
    inputs.state_299a7 = 2;
    inputs.counters_118bc_to_118be = {-1, 0, 0};
    inputs.value_120a4_135_nonzero = true;
    expect(awl::evaluate_world_map_room_static_conditions(inputs, &conditions) &&
               !conditions[0] && conditions[1] && !conditions[2] &&
               !conditions[3] && !conditions[4] && conditions[5],
           "later phase, nonzero state byte, and signed nonpositive counters");
    inputs.counters_118bc_to_118be[2] = 1;
    inputs.value_120a4_170_nonzero = true;
    constexpr std::array<size_t, 8> value_indices{
        0, 6, 7, 8, 9, 10, 14, 15};
    for (size_t row = 0; row < value_indices.size(); ++row) {
        inputs.values_14ad4_nonzero[value_indices[row]] = true;
        expect(awl::evaluate_world_map_room_static_conditions(
                   inputs, &conditions) &&
                   conditions[6 + row] && conditions[2] &&
                   conditions[3] && conditions[4],
               "each indexed saved value enables its ordered room predicate");
        inputs.values_14ad4_nonzero[value_indices[row]] = false;
    }
    inputs.phase_index = 6;
    conditions.fill(true);
    expect(!awl::evaluate_world_map_room_static_conditions(inputs,
                                                           &conditions) &&
               conditions == awl::WorldMapRoomStaticConditions{} &&
               !awl::evaluate_world_map_room_static_conditions(inputs,
                                                                nullptr),
           "unsupported phase and null condition output fail cleanly");
}

void test_world_map_room_static_stage() {
    using Status = awl::WorldMapRoomCollisionStatus;
    constexpr std::array<uint32_t, 14> expected_ids{
        0xA1u, 0x9Eu, 0xECu, 0xEBu, 0x8Du, 0x8Au, 0x11u,
        0x17u, 0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Fu, 0x20u};
    expect(awl::world_map_room_static_condition_ids() == expected_ids,
           "room static predicate order matches the DOL table");
    awl::WorldMapRoomStaticConditions conditions{};
    expect(awl::world_map_room_static_surface_mask(conditions, 0) == 0u &&
               awl::world_map_room_static_surface_mask(conditions, 0x200u) ==
                   0x8000u,
           "room static mask starts empty and carries the resolver high bit");
    conditions.fill(true);
    expect(awl::world_map_room_static_surface_mask(conditions, 0) ==
               0x3FFFu &&
               awl::world_map_room_static_surface_mask(conditions, 0x400u) ==
                   0xBFFFu,
           "all 14 DOL condition rows and the resolver high bit compose");
    constexpr std::array<uint32_t, 14> expected_bits{
        0x0001u, 0x0002u, 0x0004u, 0x2000u, 0x1000u,
        0x0800u, 0x0400u, 0x0200u, 0x0040u, 0x0080u,
        0x0010u, 0x0100u, 0x0008u, 0x0020u};
    for (size_t row = 0; row < expected_bits.size(); ++row) {
        conditions = {};
        conditions[row] = true;
        expect(awl::world_map_room_static_surface_mask(conditions, 0) ==
                   expected_bits[row],
               "each room condition row selects its verified DOL bit");
    }
    conditions = {};
    conditions[13] = true; // DOL row 13: condition 0x20 -> surface bit 0x20.
    expect(awl::world_map_room_static_surface_mask(conditions, 0) == 0x20u,
           "the final DOL condition row selects surface bit 0x20");

    std::vector<uint8_t> col = make_sample_leaf();
    col[6] = 0;
    awl::WorldMapCollisionArchive archive;
    awl::WorldMapCollisionRecordView record;
    expect(archive.parse(make_record_arc(col)) && archive.lookup(0, &record),
           "sample room COL is owned by a validated ARC record");
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    awl::WorldMapRoomStaticAdjustment result;
    expect(awl::resolve_type1_room_static_contact(
               40, Status::Found, &record, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               result.record_present && result.contact &&
               result.surface_mask == 0x20u &&
               result.narrow_phase.first_edge_contact &&
               std::fabs(result.position[2] + 1.01f) < 0.0001f,
           "mapped room category applies the selected type-1 edge response");
    expect(awl::resolve_type1_room_static_contact(
               40, Status::Found, &record, conditions, 1u, 1u, 0u,
               prior, proposed, 1.0f, &result) &&
               result.contact && !result.reverted_horizontal_to_prior &&
               std::fabs(result.position[2] + 1.01f) < 0.0001f,
           "room static query clears carried bit one before narrow phase");
    expect(awl::resolve_type1_room_static_contact(
               40, Status::Found, &record, conditions, 1u, 0u, 5u,
               prior, proposed, 1.0f, &result) &&
               result.contact && result.reverted_horizontal_to_prior &&
               result.position[0] == prior[0] &&
               result.position[2] == prior[2],
           "prior contact bits one and four restore horizontal position");
    expect(awl::resolve_type1_room_static_contact(
               4, Status::NoMapping, nullptr, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !result.record_present && !result.contact &&
               result.position == proposed,
           "unmapped room category copies the proposal");
    expect(awl::resolve_type1_room_static_contact(
               40, Status::MissingRecord, nullptr, conditions, 0u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !result.record_present && result.position == proposed,
           "disabled room static gate does not query the record");
    expect(!awl::resolve_type1_room_static_contact(
               40, Status::MissingRecord, nullptr, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   40, Status::Found, nullptr, conditions, 1u, 0u, 0u,
                   prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   1, Status::Found, &record, conditions, 1u, 0u, 0u,
                   prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   40, Status::Found, &record, conditions, 1u, 0u, 0u,
                   prior, proposed, -1.0f, &result),
           "missing records, category one, and invalid radius are rejected");
    expect(!awl::resolve_type1_room_static_contact(
               4, Status::NoMapping, &record, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   40, Status::Found, &record, conditions, 1u, 0u, 0u,
                   prior, proposed, 1.0f, nullptr),
           "an inconsistent lookup result and null output are rejected");
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
    expect(awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               result.matched && result.direction_code == 0 &&
               result.metadata == 42u,
           "type-1 mode zero supports the directional contact path");
    bytes[6] = 2;
    expect(!awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               !result.matched && result.world_position ==
                   std::array<float, 3>{},
           "unverified directional-contact mode is rejected");
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
    awl::WorldMapScenePositionUpdate scene_update;
    expect(awl::plan_world_map_scene_position_update(
               0, 0, result.resolved_position, &scene_update) &&
               scene_update.position == result.resolved_position &&
               scene_update.next_bucket == 0 &&
               !scene_update.relink_required,
           "collision-adjusted position retains the constructor's bucket-zero scene type");

    awl::CollisionDynamicPassObject circle;
    circle.identity = 2;
    circle.enabled = true;
    circle.category = 1;
    circle.collision_flags = 1u;
    circle.center_world = result.proposed_position;
    circle.radius = 0.5f;
    awl::WorldMapCollisionRegistry circle_registry;
    awl::WorldMapRegisteredCollisionObject circle_entry;
    circle_entry.collision = circle;
    expect(circle_registry.register_object(
               awl::WorldMapCollisionList::First, circle_entry),
           "circle entry registers in the first collision list");
    awl::WorldMapRegisteredCollisionObject player_source = circle_entry;
    player_source.collision.identity = 99;
    expect(circle_registry.register_object(
               awl::WorldMapCollisionList::Third, player_source),
           "player source object can register in the separate third list");
    awl::WorldMapCollisionSnapshot circle_snapshot =
        circle_registry.snapshot();
    expect(circle_snapshot.first_resolver.size() == 1 &&
               circle_snapshot.third_objects.size() == 1 &&
               circle_snapshot.third_objects[0].collision.identity == 99,
           "third-list source does not enter the player's first or later passes");
    query.collision.first_objects = circle_snapshot.first_resolver.data();
    query.collision.first_object_count = circle_snapshot.first_resolver.size();
    query.collision.source_identity = 99;
    query.collision.moving_radius = -99.0f;
    query.directional_objects = circle_snapshot.first_directional.data();
    query.directional_object_count = circle_snapshot.first_directional.size();
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               result.movement_enabled && result.collision.first_pass.contact &&
               result.collision.resolver_contact_bits == 4u &&
               result.directional_contact.queried_objects == 0 &&
               std::fabs(result.collision.first_pass.position[2] -
                         result.proposed_position[2] - 0.81f) < 0.0002f,
           "first-list circle uses the player's fixed radius while directional search skips it");
    circle_snapshot.first_directional[0].category = 2;
    expect(!awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled,
           "inconsistent first-list views are rejected before movement work");
    circle_snapshot.first_directional[0].category = 1;

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
    awl::WorldMapCollisionRegistry wall_registry;
    awl::WorldMapRegisteredCollisionObject wall_entry;
    wall_entry.collision = wall;
    wall_entry.world_position = {20.0f, 30.0f, 40.0f};
    wall_entry.heading_axis = {0.0f, 0.0f, -1.0f};
    wall_entry.metadata = 42u;
    expect(wall_registry.register_object(
               awl::WorldMapCollisionList::First, wall_entry),
           "type-one entry registers in the first collision list");
    const awl::WorldMapCollisionSnapshot wall_snapshot =
        wall_registry.snapshot();
    awl::WorldMapMovementQuery wall_query;
    wall_query.pad.stick_y = -80;
    wall_query.current_position = {5.0f, 7.0f, -0.1f};
    wall_query.current_axis = {0.0f, 0.0f, 1.0f};
    wall_query.directional_objects = wall_snapshot.first_directional.data();
    wall_query.directional_object_count =
        wall_snapshot.first_directional.size();
    wall_query.collision.terrain_data = terrain.data();
    wall_query.collision.terrain_size = terrain.size();
    wall_query.collision.first_objects = wall_snapshot.first_resolver.data();
    wall_query.collision.first_object_count =
        wall_snapshot.first_resolver.size();
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

void test_world_map_scene_position_bucket_decision() {
    constexpr float third_x_threshold = 152.97621f;
    uint32_t threshold_bits = 0;
    std::memcpy(&threshold_bits, &third_x_threshold, sizeof(threshold_bits));
    expect(threshold_bits == 0x4318F9E9u,
           "native third X threshold retains the verified DOL float bits");
    constexpr float x_positions[4] = {0.0f, 54.0f, 99.0f, 152.97621f};
    constexpr float z_positions[4] = {0.0f, 64.0f, 130.0f, 194.0f};
    constexpr uint8_t expected[4][4] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16}};
    for (size_t x = 0; x < 4; ++x) {
        for (size_t z = 0; z < 4; ++z) {
            const std::array<float, 3> position{
                x_positions[x], 500.0f, z_positions[z]};
            awl::WorldMapScenePositionUpdate update;
            expect(awl::plan_world_map_scene_position_update(
                       1, expected[x][z], position, &update) &&
                       update.next_bucket == expected[x][z] &&
                       !update.relink_required &&
                       update.position == position,
                   "verified sixteen-entry scene table uses X-major and Z-minor bins");
        }
    }
    for (size_t index = 1; index < 4; ++index) {
        const std::array<float, 3> before_x{
            std::nextafter(x_positions[index],
                           -std::numeric_limits<float>::infinity()),
            0.0f, 0.0f};
        const std::array<float, 3> before_z{
            0.0f, 0.0f,
            std::nextafter(z_positions[index],
                           -std::numeric_limits<float>::infinity())};
        awl::WorldMapScenePositionUpdate boundary;
        expect(awl::plan_world_map_scene_position_update(
                   1, expected[index - 1][0], before_x, &boundary) &&
                   boundary.next_bucket == expected[index - 1][0],
               "one float below each X threshold stays in the lower bin");
        expect(awl::plan_world_map_scene_position_update(
                   1, expected[0][index - 1], before_z, &boundary) &&
                   boundary.next_bucket == expected[0][index - 1],
               "one float below each Z threshold stays in the lower bin");
    }

    awl::WorldMapScenePositionUpdate update;
    const std::array<float, 3> below_third_x{
        std::nextafter(152.97621f, -std::numeric_limits<float>::infinity()),
        -10.0f, 194.0f};
    expect(awl::plan_world_map_scene_position_update(
               2, 12, below_third_x, &update) &&
               update.next_bucket == 12 && !update.relink_required,
           "one float below the third X threshold stays in the preceding bin");
    const std::array<float, 3> crossing{152.97621f, -10.0f, 194.0f};
    expect(awl::plan_world_map_scene_position_update(
               2, 12, crossing, &update) && update.next_bucket == 16 &&
               update.relink_required && update.position == crossing,
           "crossing a threshold plans a relink while retaining the full position");
    expect(awl::plan_world_map_scene_position_update(
               1, 0, crossing, &update) && update.relink_required &&
               update.previous_bucket == 0 && update.next_bucket == 16,
           "switching from bucket zero plans entry to the selected grid bucket");
    expect(awl::plan_world_map_scene_position_update(
               1, -1, crossing, &update) && update.relink_required &&
               update.previous_bucket == -1 && update.next_bucket == 16,
           "constructor key minus one plans first entry into a spatial bucket");
    expect(awl::plan_world_map_scene_position_update(
               0, 0, crossing, &update) && update.next_bucket == 0 &&
               !update.relink_required && update.position == crossing,
           "constructor's initial type zero keeps bucket zero across position bins");
    expect(awl::plan_world_map_scene_position_update(
               0, 16, crossing, &update) && update.next_bucket == 0 &&
               update.relink_required,
           "changing back to scene type zero plans a move to bucket zero");
    for (int32_t type = 3; type <= 44; ++type) {
        expect(awl::plan_world_map_scene_position_update(
                   type, 0, crossing, &update) &&
                   update.next_bucket == static_cast<uint8_t>(type + 14) &&
                   update.relink_required,
               "verified fixed scene-type table maps types three through forty-four");
    }

    const std::array<float, 3> nonfinite{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    expect(!awl::plan_world_map_scene_position_update(
               1, 0, nonfinite, &update) && update.next_bucket == 0,
           "nonfinite scene position is rejected without an update plan");
    expect(!awl::plan_world_map_scene_position_update(
               45, 0, crossing, &update) && update.next_bucket == 0,
           "scene type beyond verified table is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               -1, 0, crossing, &update),
           "negative scene type is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               1, 59, crossing, &update),
           "prior bucket beyond verified range is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               1, -2, crossing, &update),
           "prior bucket below constructor key is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               1, 0, crossing, nullptr),
           "missing scene position result is rejected");
}

void test_world_map_scene_bucket_registry() {
    awl::WorldMapSceneBucketRegistry registry;
    const std::array<float, 3> start{10.0f, 3.0f, 10.0f};
    const std::array<float, 3> same_bucket{20.0f, 4.0f, 20.0f};
    const std::array<float, 3> across_x{54.0f, 5.0f, 10.0f};
    expect(registry.register_object(11, 1, start) &&
               registry.register_object(22, 2, start) &&
               registry.register_object(33, 1, across_x),
           "scene objects register in the constructor's unpositioned bucket");
    expect(registry.size(-1) == 3 && registry.snapshot(-1)[0].identity == 33 &&
               registry.snapshot(-1)[1].identity == 22 &&
               registry.snapshot(-1)[2].identity == 11,
           "constructor bucket insertion keeps newest object first");

    awl::WorldMapScenePositionUpdate update;
    expect(registry.update_position(11, start, &update) &&
               update.relink_required && update.previous_bucket == -1 &&
               update.next_bucket == 1 && registry.size(-1) == 2 &&
               registry.update_position(22, start, &update) &&
               registry.update_position(33, across_x, &update) &&
               registry.size(-1) == 0 &&
               registry.size(1) == 2 && registry.size(5) == 1 &&
               registry.snapshot(1)[0].identity == 22 &&
               registry.snapshot(1)[1].identity == 11,
           "first position writes move objects into computed spatial buckets");

    expect(registry.update_position(11, same_bucket, &update) &&
               !update.relink_required && update.next_bucket == 1 &&
               registry.snapshot(1)[0].identity == 22 &&
               registry.snapshot(1)[1].position == same_bucket,
           "same-bucket position write preserves list order");
    expect(registry.update_position(11, across_x, &update) &&
               update.relink_required && update.previous_bucket == 1 &&
               update.next_bucket == 5 && registry.size(1) == 1 &&
               registry.snapshot(5)[0].identity == 11 &&
               registry.snapshot(5)[1].identity == 33,
           "crossing a bin detaches and inserts at the new bucket head");
    expect(registry.update_position(22, across_x, &update) &&
               registry.snapshot(5)[0].identity == 22 &&
               registry.snapshot(5)[1].identity == 11,
           "a later crossing moves ahead of existing destination entries");

    expect(registry.register_object(44, 0, start) &&
               registry.size(-1) == 1 &&
               registry.update_position(44, across_x, &update) &&
               update.relink_required && update.previous_bucket == -1 &&
               registry.update_position(44, start, &update) &&
               !update.relink_required && registry.size(0) == 1 &&
               registry.snapshot(0)[0].position == start,
           "type zero first enters bucket zero, then retains it across XYZ changes");
    expect(registry.register_object(55, 44, start) &&
               registry.update_position(55, start, &update) &&
               registry.size(58) == 1,
           "fixed scene type forty-four enters the final supported bucket");

    const std::array<float, 3> nonfinite{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    const auto before = registry.snapshot(5);
    expect(!registry.register_object(11, 1, start) &&
               !registry.register_object(0, 1, start) &&
               !registry.register_object(66, 45, start) &&
               !registry.update_position(11, nonfinite, &update) &&
               !registry.update_position(99, start, &update) &&
               !registry.update_position(11, start, nullptr) &&
               registry.snapshot(5).size() == before.size() &&
               registry.snapshot(5)[1].position == before[1].position &&
               update.next_bucket == 0,
           "invalid registration and position updates leave buckets unchanged");
    expect(registry.unregister_object(11) &&
               !registry.unregister_object(11) && registry.size(5) == 2 &&
               registry.snapshot(5)[0].identity == 22,
           "unregister removes exactly one scene node");
    registry.clear();
    expect(registry.size(-1) == 0 && registry.size(0) == 0 &&
               registry.size(1) == 0 &&
               registry.size(5) == 0 && registry.size(58) == 0 &&
               registry.snapshot(59).empty() &&
               registry.snapshot(-2).empty(),
           "clear removes all buckets and unknown bucket queries stay empty");
}

void test_world_map_collision_mode_flags() {
    constexpr uint32_t expected[5] = {
        0x67u, 0xd4u, 0x16fu, 0x16fu, 0x14fu};
    for (int32_t mode = 0; mode < 5; ++mode) {
        uint32_t flags = 0;
        expect(awl::world_map_collision_flags_for_mode(mode, &flags) &&
                   flags == expected[mode] &&
                   ((flags & 0x8u) != 0) == (mode >= 2),
               "verified mode flags select the third-list resolver gate");
    }
    uint32_t flags = 99;
    expect(!awl::world_map_collision_flags_for_mode(-1, &flags) &&
               flags == 0 &&
               !awl::world_map_collision_flags_for_mode(5, &flags) &&
               flags == 0 &&
               !awl::world_map_collision_flags_for_mode(0, nullptr),
           "unsupported modes and missing output are rejected");
}

void test_world_map_scene_first_collision_registration() {
    constexpr std::array<int32_t, 5> expected_ids{
        0x2c, 0x2d, 0x2e, 0x2f, 0x30};
    constexpr std::array<int32_t, 5> expected_constructor_ids{
        0x3d, 0x41, 0x3e, 0x3f, 0x40};
    constexpr std::array<std::array<float, 3>, 5> expected_selected_positions{{
        {263.0f, 4.0f, 179.0f}, {277.0f, 4.0f, 218.0f},
        {185.0f, 4.0f, 237.0f}, {75.0f, 4.0f, 113.5f},
        {279.0f, 27.0f, 116.0f},
    }};
    constexpr std::array<float, 5> expected_radii{
        0.3f, 0.3f, 0.3f, 0.9f, 0.3f};
    const auto initial = awl::world_map_first_actor_selection(std::nullopt);
    bool selections_match = initial.actor_id == expected_ids[0] &&
                            initial.base_constructor_id ==
                                expected_constructor_ids[0];
    for (uint32_t draw = 0; draw < 10; ++draw) {
        const size_t index = draw % 5u;
        const auto selected = awl::world_map_first_actor_selection(draw);
        std::array<float, 3> position{};
        selections_match = selections_match &&
            selected.actor_id == expected_ids[index] &&
            selected.base_constructor_id == expected_constructor_ids[index] &&
            awl::world_map_first_actor_initial_position(selected.actor_id,
                                                        4.0f, &position) &&
            position == expected_selected_positions[index] &&
            awl::world_map_first_actor_circle_spec(selected.actor_id).radius ==
                expected_radii[index];
    }
    const auto wrapped = awl::world_map_first_actor_selection(
        std::numeric_limits<uint32_t>::max() - 1u);
    expect(selections_match && wrapped.actor_id == 0x30 &&
               wrapped.base_constructor_id == 0x40,
           "initial and refreshed actor choices feed five verified spawn/circle paths");

    const auto check_spec = [](int32_t id, int32_t mode, float radius) {
        const auto actual = awl::world_map_first_actor_circle_spec(id);
        return actual.mode == mode && actual.radius == radius;
    };
    expect(check_spec(0x15, 1, 0.6f) && check_spec(0x23, 1, 0.6f) &&
               check_spec(0x25, 1, 0.3f) && check_spec(0x27, 2, 0.3f) &&
               check_spec(0x2c, 2, 0.3f) && check_spec(0x2e, 2, 0.3f) &&
               check_spec(0x2f, 2, 0.9f) && check_spec(0x30, 2, 0.3f) &&
               check_spec(0x31, 1, 0.5f),
           "first actor collision setup follows the DOL ID branches and radii");

    const std::array<std::array<float, 3>, 6> expected_positions{{
        {263.0f, 9.0f, 179.0f}, {277.0f, 9.0f, 218.0f},
        {185.0f, 9.0f, 237.0f}, {75.0f, 9.0f, 113.5f},
        {279.0f, 27.0f, 116.0f}, {3.0f, 9.0f, 3.0f},
    }};
    bool positions_match = true;
    for (size_t i = 0; i < expected_positions.size(); ++i) {
        const int32_t id = i == 5 ? 0x31 : 0x2c + static_cast<int32_t>(i);
        std::array<float, 3> position{};
        positions_match = positions_match &&
            awl::world_map_first_actor_initial_position(id, 9.0f,
                                                        &position) &&
            position == expected_positions[i];
    }
    std::array<float, 3> position{99.0f, 98.0f, 97.0f};
    const auto unchanged = position;
    expect(positions_match &&
               awl::world_map_first_actor_initial_position(0x2b, 9.0f,
                                                           &position) &&
               position == expected_positions[5] &&
               awl::world_map_first_actor_initial_position(0x30,
                                                           std::nullopt,
                                                           &position) &&
               position == expected_positions[4],
           "actor IDs select DOL XZ seeds and the height-query exception");
    position = unchanged;
    expect(!awl::world_map_first_actor_initial_position(0x2f,
                                                        std::nullopt,
                                                        &position) &&
               position == unchanged &&
               !awl::world_map_first_actor_initial_position(
                   0x2f, std::numeric_limits<float>::quiet_NaN(),
                   &position) &&
               position == unchanged &&
               !awl::world_map_first_actor_initial_position(0x2f, 9.0f,
                                                            nullptr),
           "missing or invalid required height leaves the output unchanged");

    awl::WorldMapSceneFirstCollisionObjects scene;
    scene.conditional_a.emplace();
    scene.conditional_a->collision.identity = 101;
    scene.conditional_a->collision.category = 2;
    scene.conditional_a->collision.enabled = true;
    scene.conditional_b.emplace();
    scene.conditional_b->collision.identity = 102;
    scene.conditional_b->collision.category = 1;
    scene.category1_actor.collision.identity = 103;
    scene.category1_actor.collision.category = 99;
    scene.category1_actor.collision.collision_flags = 2u;
    scene.category1_actor.world_position = {5.0f, 4.0f, 0.0f};
    scene.category1_actor_id =
        awl::world_map_first_actor_selection(3u).actor_id;
    scene.category1_actor_height_query_result = 4.0f;
    awl::WorldMapCollisionRegistry registry;
    awl::WorldMapRegisteredCollisionObject unrelated;
    unrelated.collision.identity = 80;
    expect(registry.register_object(awl::WorldMapCollisionList::Later, unrelated) &&
               awl::register_world_map_scene_first_collision_objects(
                   scene, &registry),
           "scene constructor first-list objects register from supplied presence");
    auto snapshot = registry.snapshot();
    const auto& first = snapshot.first_resolver;
    expect(first.size() == 3 && first[0].identity == 103 &&
               first[1].identity == 102 && first[2].identity == 101 &&
               first[0].enabled && first[0].category == 1 &&
               first[0].collision_flags == 1u && first[0].radius == 0.9f &&
               first[0].center_world ==
                   std::array<float, 3>{75.0f, 4.0f, 113.5f} &&
               first[0].data == nullptr && first[0].size == 0 &&
               snapshot.first_directional.size() == 3 &&
               snapshot.later_resolver.size() == 1 &&
               snapshot.later_resolver[0].identity == 80,
           "known category-1 circle heads the first list without altering other lists");
    if (first.size() == 3) {
        awl::CollisionDynamicPassAdjustment contact;
        const std::array<float, 3> prior{75.0f, 7.0f, 115.5f};
        const std::array<float, 3> proposal{75.0f, 7.0f, 114.5f};
        expect(awl::resolve_type1_first_dynamic_object_pass(
                   first.data(), first.size(), 999, 1, prior, proposal,
                   0.3f, 4u, 0x67u, &contact) && contact.contact &&
                   contact.queried_objects == 1 &&
                   std::fabs(contact.position[2] - 114.71f) < 0.0001f,
               "registered category-1 actor contributes a circle contact");
    }

    const auto before = registry.snapshot();
    scene.conditional_b->collision.identity = 101;
    expect(!awl::register_world_map_scene_first_collision_objects(
               scene, &registry) &&
               registry.snapshot().first_resolver.size() ==
                   before.first_resolver.size() &&
               registry.snapshot().first_resolver.front().identity == 103,
           "duplicate scene identities reject without changing list order");
    const std::array<float, 3> moved_position{8.0f, 4.0f, 0.0f};
    const std::array<float, 3> moved_heading{1.0f, 0.0f, 0.0f};
    expect(registry.update_circle_object(awl::WorldMapCollisionList::First,
                                         103, moved_position, moved_heading,
                                         true),
           "linked category-1 circle accepts a supplied live pose");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 3 &&
               snapshot.first_resolver[0].identity == 103 &&
               snapshot.first_resolver[1].identity == 102 &&
               snapshot.first_resolver[2].identity == 101 &&
               snapshot.first_resolver[0].center_world == moved_position &&
               snapshot.first_directional[0].world_position == moved_position &&
               snapshot.first_directional[0].heading_axis == moved_heading,
           "pose refresh changes both views without relinking the list");
    if (snapshot.first_resolver.size() == 3) {
        awl::CollisionDynamicPassAdjustment moved_contact;
        const std::array<float, 3> prior{8.0f, 7.0f, 2.0f};
        const std::array<float, 3> proposal{8.0f, 7.0f, 1.0f};
        expect(awl::resolve_type1_first_dynamic_object_pass(
                   snapshot.first_resolver.data(),
                   snapshot.first_resolver.size(), 999, 1, prior, proposal,
                   0.3f, 4u, 0x67u, &moved_contact) &&
                   moved_contact.contact &&
                   std::fabs(moved_contact.position[2] - 1.21f) < 0.0001f,
               "first-list contact follows the refreshed circle center");
    }
    const auto invalid_pose = std::array<float, 3>{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    expect(!registry.update_circle_object(awl::WorldMapCollisionList::First,
                                           103, invalid_pose, moved_heading,
                                           true) &&
               !registry.update_circle_object(awl::WorldMapCollisionList::First,
                                              101, moved_position,
                                              moved_heading, true) &&
               registry.snapshot().first_resolver[0].center_world ==
                   moved_position,
           "invalid pose or non-circle object leaves the first list intact");
    expect(registry.update_circle_object(awl::WorldMapCollisionList::First,
                                         103, moved_position, moved_heading,
                                         false) &&
               !registry.snapshot().first_resolver[0].enabled,
           "inactive circle remains linked in the first list");
    const auto inactive_snapshot = registry.snapshot();
    awl::CollisionDynamicPassAdjustment inactive_contact;
    const std::array<float, 3> prior{8.0f, 7.0f, 2.0f};
    const std::array<float, 3> proposal{8.0f, 7.0f, 1.0f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               inactive_snapshot.first_resolver.data(),
               inactive_snapshot.first_resolver.size(), 999, 1, prior,
               proposal, 0.3f, 4u, 0x67u, &inactive_contact) &&
               !inactive_contact.contact &&
               inactive_contact.position == proposal,
           "inactive linked circle does not produce contact");
    expect(registry.unregister_object(103) &&
               registry.snapshot().first_resolver.size() == 2 &&
               registry.snapshot().first_resolver[0].identity == 102,
           "actor destruction unlinks the inactive circle");
    scene.conditional_b.reset();
    scene.conditional_a.reset();
    scene.category1_actor.collision.identity = 104;
    scene.category1_actor_id = 0x2c;
    awl::WorldMapCollisionRegistry only_actor;
    expect(awl::register_world_map_scene_first_collision_objects(
               scene, &only_actor) &&
               only_actor.snapshot().first_resolver.size() == 1 &&
               only_actor.snapshot().first_resolver.front().identity == 104 &&
               only_actor.snapshot().first_resolver.front().center_world ==
                   std::array<float, 3>{263.0f, 4.0f, 179.0f} &&
               only_actor.snapshot().first_resolver.front().radius == 0.3f,
           "absent conditional actors leave the required category-1 actor");
    scene.category1_actor.collision.identity = 105;
    scene.category1_actor_id = 0x30;
    scene.category1_actor_height_query_result.reset();
    awl::WorldMapCollisionRegistry fixed_height_actor;
    expect(awl::register_world_map_scene_first_collision_objects(
               scene, &fixed_height_actor) &&
               fixed_height_actor.snapshot().first_resolver.size() == 1 &&
               fixed_height_actor.snapshot().first_resolver.front()
                       .center_world ==
                   std::array<float, 3>{279.0f, 27.0f, 116.0f},
           "ID 0x30 registers at its fixed DOL height without a query result");
    scene.category1_actor_id = 0x2c;
    scene.category1_actor_height_query_result =
        std::numeric_limits<float>::quiet_NaN();
    expect(!awl::register_world_map_scene_first_collision_objects(
               scene, &fixed_height_actor) &&
               fixed_height_actor.snapshot().first_resolver.size() == 1 &&
               fixed_height_actor.snapshot().first_resolver.front().identity ==
                   105 &&
               !awl::register_world_map_scene_first_collision_objects(
                   scene, nullptr),
           "invalid scene height and missing registry are rejected");
}

void test_world_map_first_actor_step() {
    awl::WorldMapFirstActorStepState state;
    state.actor_id = 0x2c;
    state.moving = true;
    state.timer = 17;
    state.proposal = {2.0f, 7.0f, 2.0f};
    state.target = {3.0f, 7.0f, 2.0f};
    state.heading = {1.0f, 0.0f, 0.0f};

    awl::WorldMapFirstActorStepProposal proposed;
    bool speeds_match = true;
    constexpr std::array<float, 5> expected_steps{
        0.03f, 0.06f, 0.04f, 0.02f, 0.01f};
    for (int32_t id = 0x2c; id <= 0x30; ++id) {
        state.actor_id = id;
        speeds_match = speeds_match &&
            awl::propose_world_map_first_actor_step(state, &proposed) &&
            proposed.collision_required && !proposed.target_reached &&
            proposed.state.moving && proposed.state.timer == 17 &&
            std::fabs(proposed.state.proposal[0] -
                      (2.0f + expected_steps[static_cast<size_t>(id - 0x2c)])) <
                0.000001f &&
            proposed.state.proposal[1] == 7.0f &&
            proposed.state.proposal[2] == 2.0f;
    }
    expect(speeds_match,
           "all five actor IDs use their distinct verified speed-table entries");

    state.actor_id = 0x2e;
    state.selector_d4 = 2;
    state.selector_d8 = 2;
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               std::fabs(proposed.state.proposal[0] - 2.06f) < 0.000001f,
           "the paired selector state multiplies the step by 1.5");
    state.selector_d8 = 1;
    state.target = {2.01f, 7.0f, 2.0f};
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               proposed.collision_required && proposed.target_reached &&
               !proposed.state.moving && proposed.state.timer == 0 &&
               proposed.state.proposal == state.proposal,
           "reaching the target clears movement but still requires collision");
    const auto reached = proposed;
    state.target = {2.0f, 7.07f, 2.0f};
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               !proposed.target_reached && proposed.state.moving,
           "target distance includes Y even though contact stopping compares XZ");

    state.target = {3.0f, 7.0f, 2.0f};
    expect(awl::propose_world_map_first_actor_step(state, &proposed),
           "active proposal can be passed to the supplied collision stage");
    auto terrain = make_sample_leaf();
    constexpr uint32_t vertices = 8 + 0x34 + 8;
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        put_be_s16(terrain, vertices + vertex * 8 + 2, 6);
    }
    awl::WorldMapFirstActorStepResult finished;
    expect(awl::finalize_world_map_first_actor_step(
               reached, reached.state.proposal, terrain.data(),
               terrain.size(), &finished) &&
               !finished.state.moving && finished.state.timer == 0 &&
               finished.state.proposal[1] == 6.0f,
           "target-reached branch still resamples the terrain height");
    expect(awl::finalize_world_map_first_actor_step(
               proposed, proposed.state.proposal, terrain.data(),
               terrain.size(), &finished) &&
               !finished.collision_altered_horizontal &&
               finished.state.moving && finished.state.timer == 17 &&
               finished.state.proposal[1] == 6.0f,
           "unchanged collision XZ keeps motion and resamples terrain Y");
    auto adjusted = proposed.state.proposal;
    adjusted[0] += 0.1f;
    expect(awl::finalize_world_map_first_actor_step(
               proposed, adjusted, terrain.data(), terrain.size(),
               &finished) &&
               finished.collision_altered_horizontal &&
               !finished.state.moving && finished.state.timer == 0 &&
               finished.state.proposal[0] == adjusted[0] &&
               finished.state.proposal[1] == 6.0f,
           "horizontal collision change stops motion after terrain resampling");

    const auto prior = finished;
    expect(!awl::finalize_world_map_first_actor_step(
               proposed, adjusted, nullptr, 0, &finished) &&
               finished.state.proposal == prior.state.proposal &&
               finished.state.timer == prior.state.timer &&
               !awl::finalize_world_map_first_actor_step(
                   proposed,
                   {std::numeric_limits<float>::quiet_NaN(), 7.0f, 2.0f},
                   terrain.data(), terrain.size(), &finished) &&
               finished.state.proposal == prior.state.proposal &&
               !awl::finalize_world_map_first_actor_step(
                   proposed, adjusted, terrain.data(), terrain.size(),
                   nullptr),
           "missing terrain, invalid collision, or output rejects atomically");
    state.actor_id = 0x31;
    const auto prior_proposal = proposed;
    expect(!awl::propose_world_map_first_actor_step(state, &proposed) &&
               proposed.state.proposal == prior_proposal.state.proposal &&
               proposed.state.timer == prior_proposal.state.timer,
           "unsupported constructor ID leaves the proposal unchanged");
    state.actor_id = 0x2c;
    state.target[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::propose_world_map_first_actor_step(state, &proposed) &&
               proposed.state.proposal == prior_proposal.state.proposal &&
               !awl::propose_world_map_first_actor_step(state, nullptr),
           "invalid target and missing output reject atomically");
    state.moving = false;
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               !proposed.collision_required &&
               awl::finalize_world_map_first_actor_step(
                   proposed, {}, nullptr, 0, &finished) &&
               !finished.state.moving && finished.state.timer == state.timer &&
               finished.state.proposal == state.proposal,
           "inactive moving branch preserves actor state without terrain access");
}

void test_world_map_first_actor_resolved_step() {
    auto terrain = make_sample_leaf();
    constexpr uint32_t vertices = 8 + 0x34 + 8;
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        put_be_s16(terrain, vertices + vertex * 8 + 2, 6);
    }
    auto static_col = terrain;
    static_col[6] = 0;
    awl::CollisionCategory1MovementQuery query;
    query.terrain_data = terrain.data();
    query.terrain_size = terrain.size();
    query.static_data = static_col.data();
    query.static_size = static_col.size();
    query.source_identity = 10;
    awl::CollisionDynamicPassObject source;
    source.identity = 10;
    source.enabled = true;
    source.category = 1;
    source.collision_flags = 1u;
    source.center_world = {2.0f, 6.0f, 2.0f};
    source.radius = 0.3f;
    query.first_objects = &source;
    query.first_object_count = 1;
    awl::CollisionDynamicPassObject third = source;
    third.identity = 20;
    third.center_world = {2.5f, 6.0f, 2.0f};
    third.radius = 0.4f;
    query.third_objects = &third;
    query.third_object_count = 1;

    awl::WorldMapFirstActorStepState actor;
    actor.actor_id = 0x2c;
    actor.moving = true;
    actor.timer = 17;
    actor.proposal = {2.0f, 7.0f, 2.0f};
    actor.target = {3.0f, 7.0f, 2.0f};
    actor.heading = {1.0f, 0.0f, 0.0f};
    awl::WorldMapFirstActorResolvedStep resolved;
    expect(awl::calculate_world_map_first_actor_step(
               0x2c, actor, query, &resolved) &&
               !resolved.collision.first_pass.contact &&
               resolved.collision.third_pass.contact &&
               resolved.collision.resolver_contact_bits == 8u &&
               resolved.collision.static_contact.surface_mask == 0x4c1u &&
               !resolved.collision.final_height_resampled &&
               resolved.finished.collision_altered_horizontal &&
               !resolved.finished.state.moving &&
               resolved.finished.state.timer == 0 &&
               std::fabs(resolved.finished.state.proposal[0] - 1.79f) <
                   0.0002f &&
               resolved.finished.state.proposal[1] == 6.0f,
           "mode-two actor uses third-list circle contact before terrain and stops");
    query.moving_radius = 0.3f;
    awl::CollisionCategory1MovementAdjustment player_mode;
    expect(awl::resolve_type1_category1_movement_candidate(
               query, actor.proposal, {2.03f, 7.0f, 2.0f},
               &player_mode) &&
               !player_mode.third_pass.contact &&
               (player_mode.resolver_contact_bits & 8u) == 0u &&
               player_mode.static_contact.surface_mask == 0xc1u,
           "player mode zero still skips the actor's third-list branch");

    actor.actor_id = 0x2f;
    expect(awl::calculate_world_map_first_actor_step(
               0x2f, actor, query, &resolved) &&
               std::fabs(resolved.finished.state.proposal[0] - 1.19f) <
                   0.0002f,
           "ID 0x2F selects its larger 0.9 collision radius");
    const auto prior = resolved;
    expect(!awl::calculate_world_map_first_actor_step(
               0x2c, actor, query, &resolved) &&
               resolved.finished.state.proposal ==
                   prior.finished.state.proposal &&
               !awl::calculate_world_map_first_actor_step(
                   0x31, actor, query, &resolved) &&
               !awl::calculate_world_map_first_actor_step(
                   0x2f, actor, query, nullptr),
           "mismatched actor ID and missing output reject without changing state");
    query.third_objects = nullptr;
    expect(!awl::calculate_world_map_first_actor_step(
               0x2f, actor, query, &resolved) &&
               resolved.finished.state.proposal ==
                   prior.finished.state.proposal,
           "missing third-list data rejects the composed actor candidate");
    actor.moving = false;
    query.terrain_data = nullptr;
    expect(awl::calculate_world_map_first_actor_step(
               0x2f, actor, query, &resolved) &&
               !resolved.proposed.collision_required &&
               resolved.finished.state.proposal == actor.proposal &&
               resolved.finished.state.timer == actor.timer,
           "inactive actor bypasses mode-two collision and terrain access");
}

void test_world_map_fixed_collision_registration() {
    const auto keys = awl::world_map_fixed_collision_record_keys();
    constexpr std::array<uint32_t, 25> expected_indices{
        18, 22, 17, 21, 16, 20, 15, 19,
        27, 31, 26, 30, 25, 29, 24, 28,
        35, 39, 34, 38, 33, 37, 32, 36, 23};
    bool mapped = true;
    for (size_t i = 0; i < keys.size(); ++i) {
        mapped = mapped && keys[i].archive_index == expected_indices[i] &&
                 keys[i].category == (i == 24 ? 13 : 7);
    }
    expect(mapped, "fixed objects use the verified roomobj indices and categories");
    awl::WorldMapCollisionRecordPools unloaded;
    std::array<awl::WorldMapRegisteredCollisionObject, 25> unbound{};
    unbound[0].collision.identity = 123;
    unbound[0].collision.category = 9;
    expect(!awl::bind_world_map_fixed_collision_records(unloaded, &unbound) &&
               unbound[0].collision.identity == 123 &&
               unbound[0].collision.category == 9 &&
               !awl::bind_world_map_fixed_collision_records(unloaded, nullptr),
           "missing archive or output leaves fixed objects untouched");

    awl::WorldMapFixedCollisionState state;
    state.direct_enabled = {1, 0, 2, 0, 0, 0, 0, 0};
    state.selected_variant = {0, 1, 2, 0, 1, 2, 0, 1};
    state.state_299ae = 3;
    constexpr std::array<bool, 25> expected{
        true, false, true, false, false, false, false, false,
        true, false, false, true, false, false, true, false,
        false, true, false, false, true, false, false, true,
        true};
    expect(awl::world_map_fixed_collision_activation(state) == expected,
           "fixed later-list activation uses direct bytes and exact variant equality");

    std::array<awl::WorldMapRegisteredCollisionObject, 25> objects{};
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i].collision.identity = 100 + i;
        objects[i].collision.category = 1;
        objects[i].collision.collision_flags = 1u;
    }
    awl::WorldMapCollisionRegistry registry;
    awl::WorldMapRegisteredCollisionObject unrelated;
    unrelated.collision.identity = 50;
    unrelated.collision.enabled = true;
    expect(registry.register_object(awl::WorldMapCollisionList::First,
                                    unrelated),
           "unrelated first-list record is present before fixed registration");
    expect(awl::register_world_map_fixed_collision_objects(
               state, objects, &registry),
           "25 fixed objects register from caller-supplied state and records");
    const auto snapshot = registry.snapshot();
    bool ordered = snapshot.later_resolver.size() == 25;
    for (size_t i = 0; ordered && i < 25; ++i) {
        const auto& record = snapshot.later_resolver[i];
        ordered = record.identity == 124 - i &&
                  record.enabled == expected[24 - i] &&
                  record.collision_flags == 3u;
    }
    expect(ordered && snapshot.first_resolver.size() == 1 &&
               snapshot.first_resolver[0].identity == 50,
           "head insertion reverses fixed construction order and leaves other lists intact");

    awl::WorldMapFixedCollisionState updated;
    updated.selected_variant.fill(2);
    expect(awl::register_world_map_fixed_collision_objects(
               updated, objects, &registry) &&
               registry.snapshot().later_resolver.size() == 25 &&
               registry.snapshot().later_resolver.front().identity == 124 &&
               !registry.snapshot().later_resolver.front().enabled &&
               !registry.snapshot().later_resolver.back().enabled,
           "reloading fixed records updates active state without duplicating nodes");

    const auto before = registry.snapshot();
    objects[13].collision.identity = objects[0].collision.identity;
    expect(!awl::register_world_map_fixed_collision_objects(
               state, objects, &registry) &&
               registry.snapshot().later_resolver.size() ==
                   before.later_resolver.size() &&
               registry.snapshot().later_resolver.front().identity ==
                   before.later_resolver.front().identity,
           "duplicate fixed identities fail without changing the registry");
    objects[13].collision.identity = 0;
    expect(!awl::register_world_map_fixed_collision_objects(
               state, objects, &registry) &&
               !awl::register_world_map_fixed_collision_objects(
                   state, objects, nullptr),
           "missing identity and registry are rejected");
}

void test_world_map_collision_registry() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::WorldMapRegisteredCollisionObject first;
    first.collision.identity = 11;
    first.collision.enabled = true;
    first.collision.category = 1;
    first.collision.collision_flags = 2u;
    first.collision.data = bytes.data();
    first.collision.size = bytes.size();
    first.collision.radius = 4.0f;
    first.collision.center_local = {1.0f, 2.0f, 3.0f};
    first.world_position = {4.0f, 5.0f, 6.0f};
    first.heading_axis = {0.0f, 0.0f, -1.0f};
    first.metadata = 41u;
    awl::WorldMapRegisteredCollisionObject second;
    second.collision.identity = 22;
    second.collision.enabled = true;
    second.collision.category = 1;
    second.collision.collision_flags = 1u;
    awl::WorldMapRegisteredCollisionObject later;
    later.collision.identity = 33;
    later.collision.enabled = true;
    later.collision.category = 1;
    later.collision.collision_flags = 1u;
    awl::WorldMapRegisteredCollisionObject third = later;
    third.collision.identity = 44;
    third.collision.center_world = {5.0f, 0.0f, 0.0f};
    third.collision.radius = 0.5f;
    awl::WorldMapCollisionRegistry registry;
    expect(registry.register_object(awl::WorldMapCollisionList::First, first) &&
               registry.register_object(awl::WorldMapCollisionList::First,
                                        second) &&
               registry.register_object(awl::WorldMapCollisionList::Later,
                                        later) &&
               registry.register_object(awl::WorldMapCollisionList::Third,
                                        third),
           "collision objects register into three distinct lists");
    awl::WorldMapCollisionSnapshot snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 2 &&
               snapshot.first_directional.size() == 2 &&
               snapshot.first_resolver[0].identity == 22 &&
               snapshot.first_resolver[1].identity == 11 &&
               snapshot.later_resolver.size() == 1 &&
               snapshot.later_resolver[0].identity == 33 &&
               snapshot.third_resolver.size() == 1 &&
               snapshot.third_resolver[0].identity == 44 &&
               snapshot.third_objects.size() == 1 &&
               snapshot.third_objects[0].collision.identity == 44,
           "first, later, and third lists retain separate traversal order");
    awl::CollisionDynamicPassAdjustment third_contact;
    expect(awl::resolve_type1_third_dynamic_object_pass(
               snapshot.third_resolver.data(), snapshot.third_resolver.size(),
               99, 0, 1, {5.0f, 7.0f, -2.0f}, {5.0f, 9.0f, -0.5f},
               1.0f, 4u, 0x8u, &third_contact) && third_contact.contact &&
               third_contact.resolver_contact_bit == 8u &&
               std::fabs(third_contact.position[2] + 1.51f) < 0.0001f,
           "the third-list snapshot supplies its own ordered contact pass");
    expect(snapshot.first_directional[1].metadata == 41u &&
               snapshot.first_directional[1].world_position ==
                   first.world_position &&
               snapshot.first_directional[1].contact_query.object_radius ==
                   first.collision.radius &&
               snapshot.first_directional[1].data == bytes.data(),
           "one registered entry supplies matching directional and resolver views");

    first.metadata = 42u;
    expect(registry.register_object(awl::WorldMapCollisionList::First, first),
           "registering an existing object moves its node to the front");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 2 &&
               snapshot.first_resolver[0].identity == 11 &&
               snapshot.first_directional[0].metadata == 42u,
           "re-registration replaces the record without duplicating it");

    expect(registry.register_object(awl::WorldMapCollisionList::Third, second),
           "one node can move from first to third list");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 1 &&
               snapshot.third_resolver.size() == 2 &&
               snapshot.third_resolver[0].identity == 22 &&
               snapshot.third_objects.size() == 2 &&
               snapshot.third_objects[0].collision.identity == 22 &&
               snapshot.third_objects[1].collision.identity == 44,
           "cross-list move removes prior membership and leads the third list");
    expect(registry.register_object(awl::WorldMapCollisionList::Later, second),
           "the same node can move from third to later list");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 1 &&
               snapshot.first_resolver[0].identity == 11 &&
               snapshot.later_resolver.size() == 2 &&
               snapshot.later_resolver[0].identity == 22 &&
               snapshot.later_resolver[1].identity == 33,
           "cross-list move removes the old membership and inserts at the new front");
    expect(registry.unregister_object(11) &&
               !registry.unregister_object(11) &&
               registry.size(awl::WorldMapCollisionList::First) == 0,
           "unregister removes one known node only once");
    registry.clear(awl::WorldMapCollisionList::Later);
    expect(registry.size(awl::WorldMapCollisionList::Later) == 0 &&
               registry.snapshot().later_resolver.empty(),
           "clearing a list removes every member");
    registry.clear(awl::WorldMapCollisionList::Third);
    expect(registry.size(awl::WorldMapCollisionList::Third) == 0 &&
               registry.snapshot().third_objects.empty() &&
               registry.snapshot().third_resolver.empty(),
           "clearing the third list does not restore moved nodes");
    first.collision.identity = 0;
    expect(!registry.register_object(awl::WorldMapCollisionList::First, first) &&
               !registry.register_object(
                   static_cast<awl::WorldMapCollisionList>(3), second) &&
               registry.size(awl::WorldMapCollisionList::First) == 0,
           "null identity and unknown list are rejected without mutation");
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

void test_world_map_collision_asset_provider() {
    constexpr const char* expected_static[6] = {
        "/files/mapobj.col", "/files/mapobj2.col",
        "/files/mapobj3.col", "/files/mapobj4.col",
        "/files/mapobj5.col", "/files/mapobj5.col"};
    awl::WorldMapCollisionAssetPaths paths;
    for (uint32_t phase = 0; phase < 6; ++phase) {
        expect(awl::select_world_map_collision_asset_paths(
                   phase, false, &paths) &&
                   std::strcmp(paths.terrain, "/files/jimen-move.col") == 0 &&
                   std::strcmp(paths.static_objects, expected_static[phase]) == 0,
               "phase selects the DOL terrain and static COL paths");
        expect(awl::select_world_map_collision_asset_paths(
                   phase, true, &paths) &&
                   std::strcmp(paths.terrain, "/files/jimen1-move.col") == 0,
               "terrain switch selects the alternate movement COL");
    }
    expect(!awl::select_world_map_collision_asset_paths(6, false, &paths) &&
               paths.terrain == nullptr && paths.static_objects == nullptr,
           "unsupported phase is rejected and clears output paths");
    expect(!awl::select_world_map_collision_asset_paths(0, false, nullptr),
           "null selector output is rejected");

    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("awl-collision-provider-" + std::to_string(nonce));
    std::error_code error;
    const bool created = fs::create_directory(root, error);
    expect(created && !error, "collision fixture directory is created");
    if (!created || error) {
        return;
    }
    const fs::path files = root / "files";
    fs::create_directory(files, error);
    expect(!error, "collision fixture files directory is created");
    if (error) {
        fs::remove(root, error);
        return;
    }
    const auto write = [](const fs::path& path,
                          const std::vector<uint8_t>& bytes) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    };
    const fs::path terrain_path = files / "jimen-move.col";
    const fs::path alternate_terrain_path = files / "jimen1-move.col";
    const fs::path static_path = files / "mapobj.col";
    const std::vector<uint8_t> terrain = make_sample_leaf();
    std::vector<uint8_t> spawn_terrain = make_sample_leaf();
    constexpr uint32_t spawn_payload = 8 + 0x34;
    initialize_sample_payload(spawn_terrain, 8, spawn_payload, 70, 110);
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        put_be_s16(spawn_terrain, spawn_payload + 8 + vertex * 8 + 2, 6);
    }
    std::vector<uint8_t> static_asset = terrain;
    static_asset[6] = 0;
    expect(write(terrain_path, terrain) &&
               write(alternate_terrain_path, spawn_terrain) &&
               write(static_path, static_asset),
           "collision fixtures are written");

    awl_memory_init();
    awl::filesystem_init();
    const std::string native_root = root.string();
    const bool mounted = awl::filesystem_mount("/", native_root.c_str());
    expect(mounted, "collision fixture mount succeeds");
    if (mounted) {
        awl::WorldMapCollisionAssets assets;
        awl::CollisionCategory1MovementQuery query;
        query.moving_radius = 0.3f;
        expect(assets.load(0, false) && assets.bind(&query) &&
                   query.terrain_data == assets.terrain_bytes().data() &&
                   query.terrain_size == terrain.size() &&
                   query.static_data == assets.static_bytes().data() &&
                   query.static_size == static_asset.size() &&
                   query.moving_radius == 0.3f &&
                   assets.terrain_analysis().header_byte_6 == 1 &&
                   assets.static_analysis().header_byte_6 == 0,
               "provider owns and binds both validated collision assets");
        awl::CollisionCategory1MovementAdjustment result;
        expect(awl::resolve_type1_category1_movement_candidate(
                   query, {2.0f, 7.0f, 2.0f}, {2.0f, 50.0f, 2.0f},
                   &result) && result.static_contact.slot_present &&
                   !result.static_contact.contact &&
                   result.position == std::array<float, 3>{2.0f, 6.0f, 2.0f},
               "bound mode-zero static data yields a clear full candidate");
        expect(!assets.bind(nullptr), "null query binding is rejected");

        awl::WorldMapSceneFirstCollisionObjects scene;
        scene.category1_actor.collision.identity = 103;
        scene.category1_actor_id =
            awl::world_map_first_actor_selection(3u).actor_id;
        scene.category1_actor_height_query_result = 99.0f;
        awl::WorldMapCollisionRegistry scene_registry;
        expect(assets.load(0, true) &&
                   awl::register_world_map_scene_first_collision_objects_from_assets(
                       scene, assets, &scene_registry) &&
                   scene_registry.snapshot().first_resolver.size() == 1 &&
                   scene_registry.snapshot().first_resolver[0].center_world ==
                       std::array<float, 3>{75.0f, 6.0f, 113.5f} &&
                   scene_registry.snapshot().first_resolver[0].radius == 0.9f,
               "validated slot-10 terrain supplies the actor's spawn height");
        scene.category1_actor.collision.identity = 104;
        scene.category1_actor_id = 0x30;
        expect(awl::register_world_map_scene_first_collision_objects_from_assets(
                   scene, assets, &scene_registry) &&
                   scene_registry.snapshot().first_resolver.size() == 2 &&
                   scene_registry.snapshot().first_resolver[0].center_world ==
                       std::array<float, 3>{279.0f, 27.0f, 116.0f},
               "fixed-height actor skips the terrain height query");
        scene.category1_actor_id = 0x2f;
        expect(write(alternate_terrain_path, make_single_leaf()) &&
                   assets.load(0, true) &&
                   !awl::register_world_map_scene_first_collision_objects_from_assets(
                       scene, assets, &scene_registry) &&
                   scene_registry.snapshot().first_resolver.size() == 2 &&
                   !awl::register_world_map_scene_first_collision_objects_from_assets(
                       scene, assets, nullptr),
               "empty selected leaf rejects spawn without changing the list");
        assets.clear();
        expect(!awl::register_world_map_scene_first_collision_objects_from_assets(
                   scene, assets, &scene_registry) &&
                   assets.load(0, false),
               "unloaded terrain cannot supply a spawn height");

        fs::remove(static_path, error);
        expect(!assets.load(0, false) && !assets.bind(&query) &&
                   assets.paths().terrain == nullptr &&
                   query.terrain_data == nullptr && query.terrain_size == 0 &&
                   query.static_data == nullptr && query.static_size == 0,
               "missing static asset fails without retaining a partial pair");
        expect(write(static_path, {0, 1, 2}),
               "malformed static fixture is written");
        expect(!assets.load(0, false) && !assets.bind(&query),
               "malformed static asset is rejected");
        expect(write(static_path, terrain),
               "wrong-mode static fixture is written");
        expect(!assets.load(0, false) && !assets.bind(&query),
               "wrong static collision mode is rejected");
        expect(write(static_path, static_asset),
               "valid static fixture is restored");
        fs::remove(terrain_path, error);
        expect(!assets.load(0, false) && !assets.bind(&query),
               "missing terrain asset is rejected");
        expect(!assets.load(6, false) && !assets.bind(&query),
               "unsupported phase cannot bind stale bytes");
    }
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    fs::remove(terrain_path, error);
    fs::remove(alternate_terrain_path, error);
    fs::remove(static_path, error);
    fs::remove(files, error);
    fs::remove(root, error);
    expect(!error, "collision fixture directory is cleaned up");
}

bool probe_local_static_wall(const awl::WorldMapCollisionAssets& assets,
                             awl::CollisionCategory1StaticAdjustment* result) {
    const auto& bytes = assets.static_bytes();
    const auto& analysis = assets.static_analysis();
    awl::CollisionEdgeSample edge;
    const float center_z =
        (analysis.root_min[2] + analysis.root_max[2]) * 0.5f;
    if (!awl::project_type1_collision_to_edge(
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
    if (!std::isfinite(length) || length == 0.0f) {
        return false;
    }
    // FUN_8017C918 uses (0, -1, 0) for this edge plane. Construct a
    // positive-side prior point and a candidate just across the plane.
    const float nx = dz / length;
    const float nz = -dx / length;
    const float mid_x = (x0 + x1) * 0.5f;
    const float mid_z = (z0 + z1) * 0.5f;
    const std::array<float, 3> prior{mid_x + nx, 0.0f, mid_z + nz};
    const std::array<float, 3> proposed{
        mid_x - nx * 0.1f, 0.0f, mid_z - nz * 0.1f};
    const awl::CollisionCategory1StaticFlags flags;
    if (!awl::resolve_type1_category1_static_contact(
            bytes.data(), bytes.size(), flags, 0x67u, prior, proposed,
            0.3f, 0u, result) || !result->slot_present || !result->contact ||
        !result->narrow_phase.first_edge_contact ||
        !std::isfinite(result->position[0]) ||
        !std::isfinite(result->position[1]) ||
        !std::isfinite(result->position[2])) {
        return false;
    }
    const float signed_distance =
        (result->position[0] - mid_x) * nx +
        (result->position[2] - mid_z) * nz;
    if (signed_distance < 0.309f) {
        return false;
    }
    awl::CollisionCategory1MovementQuery query;
    query.moving_radius = 0.3f;
    if (!assets.bind(&query)) {
        return false;
    }
    awl::CollisionCategory1MovementAdjustment composed;
    if (!awl::resolve_type1_category1_movement_candidate(
            query, prior, proposed, &composed) ||
        !composed.static_contact.contact ||
        (composed.resolver_contact_bits & 1u) == 0 ||
        !composed.final_height_resampled ||
        !std::isfinite(composed.position[0]) ||
        !std::isfinite(composed.position[1]) ||
        !std::isfinite(composed.position[2])) {
        return false;
    }
    awl::CollisionSurfaceSample terrain_surface;
    if (awl::sample_type1_collision_surface(
            assets.terrain_bytes().data(), assets.terrain_bytes().size(),
            composed.position[0], composed.position[2], &terrain_surface)) {
        return std::fabs(composed.position[1] - terrain_surface.height) <
               0.0002f;
    }
    awl::CollisionEdgeSample terrain_edge;
    return awl::project_type1_collision_to_edge(
               assets.terrain_bytes().data(), assets.terrain_bytes().size(),
               composed.position[0], composed.position[2], &terrain_edge) &&
           std::fabs(composed.position[1] - terrain_edge.position[1]) <
               0.0002f;
}

bool inspect_local_catalog(const char* disc_root) {
    awl_memory_init();
    awl::filesystem_init();
    bool valid = awl::filesystem_mount("/", disc_root);
    if (valid) {
        awl::WorldMapCollisionRecordPools pools;
        valid = pools.load() && pools.record_count(0) == 53 &&
                pools.record_count(2) == 21 && pools.record_count(1) == 54;
        if (valid) {
            std::array<awl::WorldMapRegisteredCollisionObject, 25> fixed{};
            const auto keys = awl::world_map_fixed_collision_record_keys();
            valid = awl::bind_world_map_fixed_collision_records(pools, &fixed);
            for (size_t i = 0; valid && i < fixed.size(); ++i) {
                awl::WorldMapCollisionRecordView record;
                valid = pools.lookup(1, keys[i].archive_index, &record) &&
                        fixed[i].collision.category == keys[i].category &&
                        fixed[i].collision.data == record.data &&
                        fixed[i].collision.size == record.size &&
                        fixed[i].collision.center_local == record.center_local;
            }
        }
        if (valid) {
            std::vector<awl::WorldMapMapseCollisionMatch> matches;
            valid = pools.find_mapse_matches(2, &matches) &&
                    matches.size() == 6;
            for (size_t row = 0; valid && row < matches.size(); ++row) {
                valid = matches[row].record_index == row + 2 &&
                        matches[row].table_flag == 1 &&
                        matches[row].record.data != nullptr;
            }
            valid = valid && pools.find_mapse_matches(11, &matches) &&
                    matches.size() == 1 && matches[0].record_index == 16 &&
                    matches[0].table_flag == 2 &&
                    pools.find_mapse_matches(0, &matches) &&
                    matches.size() == 1 && matches[0].record_index == 0 &&
                    matches[0].table_flag == 3 &&
                    pools.find_mapse_matches(16, &matches) && matches.empty() &&
                    !pools.find_mapse_matches(0, nullptr);
        }
        for (uint32_t phase = 0; valid && phase < 6; ++phase) {
            awl::WorldMapRoomCollisionState state;
            state.phase_index = phase;
            awl::WorldMapCollisionRecordView room;
            valid = pools.lookup_room_object(40, state, &room) ==
                        awl::WorldMapRoomCollisionStatus::Found &&
                    room.data != nullptr && room.analysis != nullptr &&
                    room.analysis->header_byte_6 == 0;
            if (valid) {
                awl::WorldMapRoomConditionInputs inputs;
                inputs.phase_index = phase;
                awl::WorldMapRoomStaticConditions conditions;
                awl::WorldMapRoomStaticAdjustment result;
                const std::array<float, 3> point = room.center_local;
                valid = awl::evaluate_world_map_room_static_conditions(
                            inputs, &conditions) &&
                        awl::resolve_type1_room_static_contact(
                            40, awl::WorldMapRoomCollisionStatus::Found,
                            &room, conditions, 1u, 0u, 0u, point, point, 0.3f,
                            &result) &&
                        result.record_present &&
                        std::isfinite(result.position[0]) &&
                        std::isfinite(result.position[1]) &&
                        std::isfinite(result.position[2]);
            }
        }
        if (valid) {
            awl::WorldMapRoomCollisionState state;
            awl::WorldMapCollisionRecordView room;
            valid = pools.lookup_room_object(4, state, &room) ==
                        awl::WorldMapRoomCollisionStatus::NoMapping &&
                    room.data == nullptr;
        }
        for (const int group : {0, 1, 2}) {
            for (uint32_t index = 0;
                 valid && index < pools.record_count(group); ++index) {
                awl::WorldMapCollisionRecordView view;
                awl::CollisionTreeAnalysis checked;
                valid = pools.lookup(group, index, &view) &&
                        view.data != nullptr && view.analysis != nullptr &&
                        view.analysis->node_count == 1 &&
                        std::isfinite(view.center_local[0]) &&
                        std::isfinite(view.center_local[1]) &&
                        std::isfinite(view.center_local[2]) &&
                        std::isfinite(view.radius_local) &&
                        view.radius_local > 0.0f &&
                        awl::analyze_type1_collision_asset(
                            view.data, view.size, &checked) &&
                        checked.header_byte_6 == 0 &&
                        checked.node_count == view.analysis->node_count;
            }
            awl::WorldMapCollisionRecordView missing;
            valid = valid && !pools.lookup(
                group, static_cast<uint32_t>(pools.record_count(group)),
                &missing) && missing.data == nullptr;
        }
        if (!valid) {
            std::fprintf(stderr, "COL archive catalog validation failed\n");
        } else {
            std::printf("COL archive records: maperase=%zu mapse=%zu roomobj=%zu\n",
                        pools.record_count(0), pools.record_count(2),
                        pools.record_count(1));
        }
    }
    for (uint32_t phase = 0; valid && phase < 6; ++phase) {
        for (int alternate = 0; alternate < 2; ++alternate) {
            awl::WorldMapCollisionAssets assets;
            valid = assets.load(phase, alternate != 0);
            if (!valid) {
                std::fprintf(stderr, "COL catalog failed at phase %u terrain %d\n",
                             phase, alternate);
                break;
            }
            awl::CollisionSurfaceSample surface;
            awl::CollisionCategory1MovementQuery query;
            query.moving_radius = 0.3f;
            awl::CollisionCategory1MovementAdjustment result;
            const bool sampled = awl::sample_type1_collision_surface(
                assets.terrain_bytes().data(), assets.terrain_bytes().size(),
                120.0f, 168.0f, &surface);
            valid = sampled && assets.bind(&query) &&
                awl::resolve_type1_category1_movement_candidate(
                    query, {120.0f, surface.height, 168.0f},
                    {120.1f, surface.height, 168.0f}, &result) &&
                std::isfinite(result.position[0]) &&
                std::isfinite(result.position[1]) &&
                std::isfinite(result.position[2]);
            if (!valid) {
                std::fprintf(stderr,
                             "COL movement candidate failed at phase %u terrain %d\n",
                             phase, alternate);
                break;
            }
            if (phase == 0) {
                constexpr std::array<std::array<float, 2>, 5> seed_xz{{
                    {263.0f, 179.0f}, {277.0f, 218.0f},
                    {185.0f, 237.0f}, {75.0f, 113.5f},
                    {279.0f, 116.0f},
                }};
                for (uint32_t draw = 0; valid && draw < 5; ++draw) {
                    awl::WorldMapSceneFirstCollisionObjects scene;
                    scene.category1_actor.collision.identity = 1;
                    scene.category1_actor_id =
                        awl::world_map_first_actor_selection(draw).actor_id;
                    awl::WorldMapCollisionRegistry registry;
                    valid = awl::register_world_map_scene_first_collision_objects_from_assets(
                                scene, assets, &registry) &&
                            registry.snapshot().first_resolver.size() == 1;
                    if (valid) {
                        const auto center =
                            registry.snapshot().first_resolver[0].center_world;
                        valid = center[0] == seed_xz[draw][0] &&
                                std::isfinite(center[1]) &&
                                center[2] == seed_xz[draw][1];
                        awl::WorldMapFirstActorStepState actor;
                        actor.actor_id = scene.category1_actor_id;
                        actor.moving = true;
                        actor.timer = 1;
                        actor.proposal = center;
                        actor.target = {center[0] + 1.0f, center[1], center[2]};
                        actor.heading = {1.0f, 0.0f, 0.0f};
                        const auto actor_snapshot = registry.snapshot();
                        query.source_identity = 1;
                        query.first_objects = actor_snapshot.first_resolver.data();
                        query.first_object_count =
                            actor_snapshot.first_resolver.size();
                        awl::WorldMapFirstActorResolvedStep finished;
                        valid = valid &&
                            awl::calculate_world_map_first_actor_step(
                                scene.category1_actor_id, actor, query,
                                &finished) &&
                            finished.collision.first_pass.queried_objects == 0 &&
                            finished.collision.third_pass.queried_objects == 0 &&
                            finished.collision.static_contact.slot_present &&
                            std::isfinite(finished.finished.state.proposal[0]) &&
                            std::isfinite(finished.finished.state.proposal[1]) &&
                            std::isfinite(finished.finished.state.proposal[2]);
                        std::printf("COL terrain %d actor ID 0x%02X spawn probe: y=%.3f\n",
                                    alternate, scene.category1_actor_id,
                                    center[1]);
                    }
                }
                if (!valid) {
                    std::fprintf(stderr,
                                 "COL actor spawn probes failed at terrain %d\n",
                                 alternate);
                    break;
                }
            }
            awl::CollisionCategory1StaticAdjustment wall;
            valid = probe_local_static_wall(assets, &wall);
            if (!valid) {
                std::fprintf(stderr,
                             "COL static wall probe failed at phase %u terrain %d\n",
                             phase, alternate);
                break;
            }
            std::printf("COL phase %u terrain %d: %s (%zu bytes), %s (%zu bytes), candidate=(%.3f, %.3f, %.3f), static wall contact=%d\n",
                        phase, alternate, assets.paths().terrain,
                        assets.terrain_bytes().size(),
                        assets.paths().static_objects,
                        assets.static_bytes().size(), result.position[0],
                        result.position[1], result.position[2],
                        wall.contact ? 1 : 0);
        }
    }
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    return valid;
}

} // namespace

int main(int argc, char** argv) {
    test_world_map_collision_archive();
    test_world_map_room_collision_mapping();
    test_world_map_packed_saved_conditions();
    test_world_map_room_condition_evaluator();
    test_world_map_room_static_stage();
    test_world_map_archive_rejects_untranslated_tree_shape();
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
    test_third_dynamic_object_pass();
    test_category1_static_and_movement_candidate();
    test_world_map_directional_contact_search();
    test_world_map_movement_candidate_sequence();
    test_world_map_scene_position_bucket_decision();
    test_world_map_scene_bucket_registry();
    test_world_map_collision_mode_flags();
    test_world_map_scene_first_collision_registration();
    test_world_map_first_actor_step();
    test_world_map_first_actor_resolved_step();
    test_world_map_fixed_collision_registration();
    test_world_map_collision_registry();
    test_radius_vertex_adjustment();
    test_radius_edge_adjustment();
    test_radius_pass_sequence();
    test_terrain_radius_adjustment();
    test_world_map_collision_asset_provider();

    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--catalog-local") == 0) {
            if (++index >= argc || !inspect_local_catalog(argv[index])) {
                ++failures;
            }
        } else if (!inspect_local_asset(argv[index])) {
            ++failures;
        }
    }
    if (failures == 0) {
        std::puts("Collision asset tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
