#include "awl/collision_asset.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
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
               analysis.max_depth == 0,
           "single-leaf collision metadata is exact");

    bytes = make_one_level_tree();
    expect(analyze(bytes, analysis), "one-level collision tree is valid");
    expect(analysis.node_count == 5 && analysis.leaf_count == 4 &&
               analysis.max_depth == 1,
           "one-level collision metadata is exact");
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
    std::printf("Collision asset validated: %s size=%zu nodes=%zu leaves=%zu depth=%u "
                "header=%u/%u/%u/%u\n",
                path, bytes.size(), analysis.node_count, analysis.leaf_count,
                analysis.max_depth, analysis.format, analysis.header_byte_5,
                analysis.header_byte_6, analysis.header_byte_7);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    test_valid_structures();
    test_rejects_unsupported_or_truncated_files();
    test_rejects_invalid_offsets();

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
