#include "awl/render_validation.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                       \
            std::cerr << "Assertion failed: " << #condition << " at "           \
                      << __FILE__ << ':' << __LINE__ << std::endl;                \
            return false;                                                        \
        }                                                                        \
    } while (0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))

namespace {

constexpr uint32_t kD3d11MaxTextureDimension = 16384;

bool test_texture_upload_validation() {
    size_t required_bytes = 999;
    ASSERT_TRUE(awl::validate_rgba8_texture_upload(
        2, 2, 16, kD3d11MaxTextureDimension, &required_bytes));
    ASSERT_TRUE(required_bytes == 16);

    ASSERT_FALSE(awl::validate_rgba8_texture_upload(
        0, 2, 16, kD3d11MaxTextureDimension, &required_bytes));
    ASSERT_FALSE(awl::validate_rgba8_texture_upload(
        2, 2, 15, kD3d11MaxTextureDimension, &required_bytes));
    ASSERT_FALSE(awl::validate_rgba8_texture_upload(
        kD3d11MaxTextureDimension + 1, 1,
        (std::numeric_limits<size_t>::max)(), kD3d11MaxTextureDimension,
        &required_bytes));
    ASSERT_FALSE(awl::validate_rgba8_texture_upload(
        (std::numeric_limits<uint32_t>::max)(),
        (std::numeric_limits<uint32_t>::max)(),
        (std::numeric_limits<size_t>::max)(),
        (std::numeric_limits<uint32_t>::max)(), &required_bytes));
    ASSERT_FALSE(awl::validate_rgba8_texture_upload(
        2, 2, 16, kD3d11MaxTextureDimension, nullptr));
    return true;
}

bool test_mesh_upload_validation() {
    const awl::DebugTexturedVertex vertices[] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };
    const uint32_t indices[] = {0, 1, 2};
    size_t vertex_bytes = 0;
    size_t index_bytes = 0;
    ASSERT_TRUE(awl::validate_debug_mesh_upload(
        vertices, 3, indices, 3, &vertex_bytes, &index_bytes));
    ASSERT_TRUE(vertex_bytes == sizeof(vertices));
    ASSERT_TRUE(index_bytes == sizeof(indices));

    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        nullptr, 3, indices, 3, &vertex_bytes, &index_bytes));
    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        vertices, 3, nullptr, 3, &vertex_bytes, &index_bytes));
    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        vertices, 3, indices, 2, &vertex_bytes, &index_bytes));

    uint32_t bad_indices[] = {0, 1, 3};
    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        vertices, 3, bad_indices, 3, &vertex_bytes, &index_bytes));

    awl::DebugTexturedVertex bad_vertices[] = {vertices[0], vertices[1], vertices[2]};
    bad_vertices[1].u = std::numeric_limits<float>::quiet_NaN();
    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        bad_vertices, 3, indices, 3, &vertex_bytes, &index_bytes));
    bad_vertices[1] = vertices[1];
    bad_vertices[2].z = std::numeric_limits<float>::infinity();
    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        bad_vertices, 3, indices, 3, &vertex_bytes, &index_bytes));
    bad_vertices[2] = vertices[2];
    bad_vertices[0].a = std::numeric_limits<float>::quiet_NaN();
    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        bad_vertices, 3, indices, 3, &vertex_bytes, &index_bytes));

    ASSERT_FALSE(awl::validate_debug_mesh_upload(
        vertices, (std::numeric_limits<uint32_t>::max)(), indices, 3,
        &vertex_bytes, &index_bytes));
    return true;
}

bool test_draw_batch_validation() {
    const awl::DebugDrawBatch batches[] = {
        {0, 6, 0},
        {6, 3, 1},
    };
    ASSERT_TRUE(awl::validate_debug_draw_batches(batches, 2, 9, 2));
    ASSERT_FALSE(awl::validate_debug_draw_batches(nullptr, 2, 9, 2));
    ASSERT_FALSE(awl::validate_debug_draw_batches(batches, 0, 9, 2));
    ASSERT_FALSE(awl::validate_debug_draw_batches(batches, 2, 8, 2));
    ASSERT_FALSE(awl::validate_debug_draw_batches(batches, 2, 9, 1));

    awl::DebugDrawBatch malformed[] = {batches[0], batches[1]};
    malformed[1].first_index = 3;
    ASSERT_FALSE(awl::validate_debug_draw_batches(malformed, 2, 9, 2));
    malformed[1] = batches[1];
    malformed[0].index_count = 5;
    ASSERT_FALSE(awl::validate_debug_draw_batches(malformed, 2, 9, 2));
    return true;
}

} // namespace

int main() {
    int failed = 0;
    if (!test_texture_upload_validation()) {
        ++failed;
    }
    if (!test_mesh_upload_validation()) {
        ++failed;
    }
    if (!test_draw_batch_validation()) {
        ++failed;
    }
    if (failed != 0) {
        std::cerr << "[TEST RESULT] " << failed << " test(s) failed." << std::endl;
        return 1;
    }
    std::cout << "[TEST RESULT] Renderer upload validation passed." << std::endl;
    return 0;
}
