#include "awl/render_validation.h"

#include <cmath>
#include <limits>

namespace awl {
namespace {

bool checked_multiply(size_t lhs, size_t rhs, size_t* result) {
    if (!result || (lhs != 0 && rhs > (std::numeric_limits<size_t>::max)() / lhs)) {
        return false;
    }
    *result = lhs * rhs;
    return true;
}

} // namespace

bool validate_rgba8_texture_upload(uint32_t width, uint32_t height,
                                   size_t pixel_bytes, uint32_t max_dimension,
                                   size_t* out_required_bytes) {
    if (out_required_bytes) {
        *out_required_bytes = 0;
    }
    if (!out_required_bytes || width == 0 || height == 0 || max_dimension == 0 ||
        width > max_dimension || height > max_dimension) {
        return false;
    }

    size_t pixel_count = 0;
    size_t required_bytes = 0;
    if (!checked_multiply(width, height, &pixel_count) ||
        !checked_multiply(pixel_count, size_t{4}, &required_bytes) ||
        required_bytes > (std::numeric_limits<uint32_t>::max)() ||
        pixel_bytes < required_bytes) {
        return false;
    }

    *out_required_bytes = required_bytes;
    return true;
}

bool validate_debug_mesh_upload(const DebugTexturedVertex* vertices,
                                uint32_t vertex_count,
                                const uint32_t* indices,
                                uint32_t index_count,
                                size_t* out_vertex_bytes,
                                size_t* out_index_bytes) {
    if (out_vertex_bytes) {
        *out_vertex_bytes = 0;
    }
    if (out_index_bytes) {
        *out_index_bytes = 0;
    }
    if (!vertices || !indices || !out_vertex_bytes || !out_index_bytes ||
        vertex_count == 0 || index_count == 0 || index_count % 3 != 0) {
        return false;
    }

    size_t vertex_bytes = 0;
    size_t index_bytes = 0;
    if (!checked_multiply(vertex_count, sizeof(DebugTexturedVertex), &vertex_bytes) ||
        !checked_multiply(index_count, sizeof(uint32_t), &index_bytes) ||
        vertex_bytes > (std::numeric_limits<uint32_t>::max)() ||
        index_bytes > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }

    for (uint32_t i = 0; i < vertex_count; ++i) {
        const DebugTexturedVertex& vertex = vertices[i];
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z) || !std::isfinite(vertex.u) ||
            !std::isfinite(vertex.v) || !std::isfinite(vertex.r) ||
            !std::isfinite(vertex.g) || !std::isfinite(vertex.b) ||
            !std::isfinite(vertex.a)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < index_count; ++i) {
        if (indices[i] >= vertex_count) {
            return false;
        }
    }

    *out_vertex_bytes = vertex_bytes;
    *out_index_bytes = index_bytes;
    return true;
}

bool validate_debug_draw_batches(const DebugDrawBatch* batches,
                                 uint32_t batch_count,
                                 uint32_t total_index_count,
                                 uint32_t texture_count) {
    if (!batches || batch_count == 0 || total_index_count == 0 ||
        texture_count == 0) {
        return false;
    }

    uint32_t next_index = 0;
    for (uint32_t index = 0; index < batch_count; ++index) {
        const DebugDrawBatch& batch = batches[index];
        if (batch.first_index != next_index || batch.index_count == 0 ||
            batch.index_count % 3 != 0 ||
            batch.index_count > total_index_count - next_index ||
            batch.texture_slot >= texture_count) {
            return false;
        }
        next_index += batch.index_count;
    }
    return next_index == total_index_count;
}

} // namespace awl
