#pragma once

#include <vector>
#include <cstdint>
#include "awl/gpl.h"

namespace awl {

struct GplRenderVertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
};

struct GplRenderMesh {
    std::vector<GplRenderVertex> vertices;
    std::vector<uint32_t> indices;
};

/**
 * Builds a clean CPU render mesh from a GplMeshAnalysis source.
 *
 * Builder contract:
 * - Clears out_mesh and leaves it empty on any failure.
 * - Requires nonempty positions/UVs in src, and that every ref has has_pos and has_uv.
 * - Bounds-checks every pos_idx, uv_idx, and any present color_idx.
 * - Uses white for layouts with a constant material color; otherwise requires
 *   COLOR0 on every reference and converts decoded RGBA8 to normalized floats.
 * - Supports GX QUADS, TRIANGLES, TRIANGLE_STRIP, and TRIANGLE_FAN records.
 * - Requires each primitive range to cover raw_refs exactly once and validates
 *   its minimum/group size.
 * - Preserves raw_refs order as output vertex order.
 * - Converts every supported primitive to a DX11 triangle list.
 * - Rejects unsupported/malformed data.
 * - Never synthesizes UVs or topology.
 */
bool build_gpl_render_mesh(const GplMeshAnalysis& src, GplRenderMesh& out_mesh);

} // namespace awl
