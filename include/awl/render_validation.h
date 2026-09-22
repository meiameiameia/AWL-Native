#pragma once

#include <cstddef>
#include <cstdint>

namespace awl {

struct DebugTexturedVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct DebugDrawBatch {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t texture_slot = 0;
    uint8_t material_red = 0xFF;
    uint8_t material_green = 0xFF;
    uint8_t material_blue = 0xFF;
    uint8_t material_alpha = 0xFF;
};

// Pure validation helpers used before mutating Direct3D resources.
bool validate_rgba8_texture_upload(uint32_t width, uint32_t height,
                                   size_t pixel_bytes, uint32_t max_dimension,
                                   size_t* out_required_bytes);

bool validate_debug_mesh_upload(const DebugTexturedVertex* vertices,
                                uint32_t vertex_count,
                                const uint32_t* indices,
                                uint32_t index_count,
                                size_t* out_vertex_bytes,
                                size_t* out_index_bytes);

bool validate_debug_draw_batches(const DebugDrawBatch* batches,
                                 uint32_t batch_count,
                                 uint32_t total_index_count,
                                 uint32_t texture_count);

} // namespace awl
