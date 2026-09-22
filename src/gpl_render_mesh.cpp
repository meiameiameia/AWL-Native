#include "awl/gpl_render_mesh.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace awl {
namespace {

constexpr uint8_t kGxDrawQuads = 0x80;
constexpr uint8_t kGxDrawQuads2 = 0x88;
constexpr uint8_t kGxDrawTriangles = 0x90;
constexpr uint8_t kGxDrawTriangleStrip = 0x98;
constexpr uint8_t kGxDrawTriangleFan = 0xA0;

bool add_triangle_count(size_t amount, size_t* total) {
    if (!total || amount > std::numeric_limits<size_t>::max() - *total) {
        return false;
    }
    *total += amount;
    return true;
}

} // namespace

bool build_gpl_render_mesh(const GplMeshAnalysis& src, GplRenderMesh& out_mesh) {
    out_mesh.vertices.clear();
    out_mesh.indices.clear();

    if (src.positions.empty() || src.uvs.empty() || src.raw_refs.empty() ||
        src.primitives.empty() || src.positions.size() % 3 != 0 ||
        src.uvs.size() % 2 != 0 ||
        src.raw_refs.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    size_t next_ref = 0;
    size_t triangle_count = 0;
    for (const auto& primitive : src.primitives) {
        if (primitive.first_ref != next_ref || primitive.ref_count == 0 ||
            primitive.ref_count > src.raw_refs.size() - next_ref) {
            return false;
        }

        size_t primitive_triangles = 0;
        switch (primitive.type) {
            case kGxDrawQuads:
            case kGxDrawQuads2:
                if (primitive.ref_count % 4 != 0) {
                    return false;
                }
                primitive_triangles = (primitive.ref_count / 4) * 2;
                break;
            case kGxDrawTriangles:
                if (primitive.ref_count % 3 != 0) {
                    return false;
                }
                primitive_triangles = primitive.ref_count / 3;
                break;
            case kGxDrawTriangleStrip:
            case kGxDrawTriangleFan:
                if (primitive.ref_count < 3) {
                    return false;
                }
                primitive_triangles = primitive.ref_count - 2;
                break;
            default:
                return false;
        }

        if (!add_triangle_count(primitive_triangles, &triangle_count)) {
            return false;
        }
        next_ref += primitive.ref_count;
    }
    if (next_ref != src.raw_refs.size() ||
        triangle_count > std::numeric_limits<size_t>::max() / 3) {
        return false;
    }

    const bool uses_vertex_colors = src.raw_refs.front().has_color;
    if ((!uses_vertex_colors && !src.colors_rgba8.empty()) ||
        (uses_vertex_colors &&
         (src.colors_rgba8.empty() || src.colors_rgba8.size() % 4 != 0))) {
        return false;
    }

    for (const auto& ref : src.raw_refs) {
        if (!ref.has_pos || !ref.has_uv ||
            ref.has_color != uses_vertex_colors ||
            ref.pos_idx >= src.positions.size() / 3 ||
            ref.uv_idx >= src.uvs.size() / 2 ||
            (uses_vertex_colors &&
             ref.color_idx >= src.colors_rgba8.size() / 4)) {
            return false;
        }
    }

    GplRenderMesh built;
    try {
        built.vertices.resize(src.raw_refs.size());
        built.indices.reserve(triangle_count * 3);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }

    for (size_t i = 0; i < src.raw_refs.size(); ++i) {
        const auto& ref = src.raw_refs[i];
        built.vertices[i].x = src.positions[ref.pos_idx * 3];
        built.vertices[i].y = src.positions[ref.pos_idx * 3 + 1];
        built.vertices[i].z = src.positions[ref.pos_idx * 3 + 2];
        built.vertices[i].u = src.uvs[ref.uv_idx * 2];
        built.vertices[i].v = src.uvs[ref.uv_idx * 2 + 1];
        if (uses_vertex_colors) {
            constexpr float kByteToFloat = 1.0f / 255.0f;
            built.vertices[i].r =
                src.colors_rgba8[ref.color_idx * 4] * kByteToFloat;
            built.vertices[i].g =
                src.colors_rgba8[ref.color_idx * 4 + 1] * kByteToFloat;
            built.vertices[i].b =
                src.colors_rgba8[ref.color_idx * 4 + 2] * kByteToFloat;
            built.vertices[i].a =
                src.colors_rgba8[ref.color_idx * 4 + 3] * kByteToFloat;
        } else {
            built.vertices[i].r = 1.0f;
            built.vertices[i].g = 1.0f;
            built.vertices[i].b = 1.0f;
            built.vertices[i].a = 1.0f;
        }
    }

    try {
        for (const auto& primitive : src.primitives) {
            const uint32_t base =
                static_cast<uint32_t>(primitive.first_ref);
            if (primitive.type == kGxDrawTriangles) {
                for (size_t i = 0; i < primitive.ref_count; ++i) {
                    built.indices.push_back(base + static_cast<uint32_t>(i));
                }
            } else if (primitive.type == kGxDrawQuads ||
                       primitive.type == kGxDrawQuads2) {
                for (size_t i = 0; i < primitive.ref_count; i += 4) {
                    const uint32_t quad = base + static_cast<uint32_t>(i);
                    built.indices.push_back(quad);
                    built.indices.push_back(quad + 1);
                    built.indices.push_back(quad + 2);
                    built.indices.push_back(quad);
                    built.indices.push_back(quad + 2);
                    built.indices.push_back(quad + 3);
                }
            } else if (primitive.type == kGxDrawTriangleStrip) {
                for (size_t i = 0; i + 2 < primitive.ref_count; ++i) {
                    const uint32_t a = base + static_cast<uint32_t>(i);
                    const uint32_t b = a + 1;
                    const uint32_t c = a + 2;
                    if ((i & 1) == 0) {
                        built.indices.push_back(a);
                        built.indices.push_back(b);
                    } else {
                        built.indices.push_back(b);
                        built.indices.push_back(a);
                    }
                    built.indices.push_back(c);
                }
            } else {
                for (size_t i = 1; i + 1 < primitive.ref_count; ++i) {
                    built.indices.push_back(base);
                    built.indices.push_back(base + static_cast<uint32_t>(i));
                    built.indices.push_back(base + static_cast<uint32_t>(i + 1));
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }

    out_mesh = std::move(built);
    return true;
}

} // namespace awl
