#include "awl/gpl_render_mesh.h"

#include <cmath>
#include <cstdint>
#include <iostream>

#define ASSERT_TRUE(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ \
                      << ":" << __LINE__ << std::endl;                          \
            return false;                                                        \
        }                                                                        \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

static bool float_near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

static awl::GplMeshAnalysis make_source(size_t ref_count) {
    awl::GplMeshAnalysis src;
    for (size_t i = 0; i < ref_count; ++i) {
        src.positions.push_back(static_cast<float>(i));
        src.positions.push_back(static_cast<float>(i % 2));
        src.positions.push_back(0.0f);
        src.uvs.push_back(static_cast<float>(i));
        src.uvs.push_back(static_cast<float>(i % 2));

        awl::GplMeshAnalysis::VertexRef ref;
        ref.pos_idx = static_cast<uint32_t>(i);
        ref.normal_idx = static_cast<uint32_t>(i);
        ref.uv_idx = static_cast<uint32_t>(i);
        ref.has_pos = true;
        ref.has_normal = true;
        ref.has_uv = true;
        src.raw_refs.push_back(ref);
    }
    return src;
}

static void add_primitive(awl::GplMeshAnalysis& src, uint8_t type,
                          size_t first_ref, size_t ref_count) {
    awl::GplMeshAnalysis::Primitive primitive;
    primitive.type = type;
    primitive.first_ref = first_ref;
    primitive.ref_count = ref_count;
    src.primitives.push_back(primitive);
}

static bool test_valid_triangle() {
    awl::GplMeshAnalysis src = make_source(3);
    add_primitive(src, 0x90, 0, 3);

    awl::GplRenderMesh out;
    ASSERT_TRUE(awl::build_gpl_render_mesh(src, out));
    ASSERT_TRUE(out.vertices.size() == 3);
    ASSERT_TRUE(out.indices.size() == 3);
    ASSERT_TRUE(out.indices[0] == 0 && out.indices[1] == 1 &&
                out.indices[2] == 2);
    ASSERT_TRUE(float_near(out.vertices[1].x, 1.0f));
    ASSERT_TRUE(float_near(out.vertices[2].u, 2.0f));
    ASSERT_TRUE(float_near(out.vertices[0].r, 1.0f));
    ASSERT_TRUE(float_near(out.vertices[0].g, 1.0f));
    ASSERT_TRUE(float_near(out.vertices[0].b, 1.0f));
    ASSERT_TRUE(float_near(out.vertices[0].a, 1.0f));
    return true;
}

static bool test_indexed_vertex_colors() {
    awl::GplMeshAnalysis src = make_source(3);
    src.colors_rgba8 = {
        255, 0, 0, 255,
        0, 128, 0, 64,
        0, 0, 255, 0,
    };
    for (size_t i = 0; i < src.raw_refs.size(); ++i) {
        src.raw_refs[i].has_color = true;
        src.raw_refs[i].color_idx = static_cast<uint32_t>(i);
    }
    add_primitive(src, 0x90, 0, 3);

    awl::GplRenderMesh out;
    ASSERT_TRUE(awl::build_gpl_render_mesh(src, out));
    ASSERT_TRUE(float_near(out.vertices[0].r, 1.0f));
    ASSERT_TRUE(float_near(out.vertices[0].g, 0.0f));
    ASSERT_TRUE(float_near(out.vertices[1].g, 128.0f / 255.0f));
    ASSERT_TRUE(float_near(out.vertices[1].a, 64.0f / 255.0f));
    ASSERT_TRUE(float_near(out.vertices[2].b, 1.0f));
    ASSERT_TRUE(float_near(out.vertices[2].a, 0.0f));
    return true;
}

static bool test_valid_quad_triangulation() {
    awl::GplMeshAnalysis src = make_source(8);
    add_primitive(src, 0x80, 0, 8);

    awl::GplRenderMesh out;
    ASSERT_TRUE(awl::build_gpl_render_mesh(src, out));
    const uint32_t expected[] = {0, 1, 2, 0, 2, 3,
                                 4, 5, 6, 4, 6, 7};
    ASSERT_TRUE(out.indices.size() == std::size(expected));
    for (size_t i = 0; i < std::size(expected); ++i) {
        ASSERT_TRUE(out.indices[i] == expected[i]);
    }
    return true;
}

static bool test_strip_fan_and_multiple_primitives() {
    awl::GplMeshAnalysis src = make_source(8);
    add_primitive(src, 0x98, 0, 4);
    add_primitive(src, 0xA0, 4, 4);

    awl::GplRenderMesh out;
    ASSERT_TRUE(awl::build_gpl_render_mesh(src, out));
    const uint32_t expected[] = {0, 1, 2, 2, 1, 3,
                                 4, 5, 6, 4, 6, 7};
    ASSERT_TRUE(out.indices.size() == std::size(expected));
    for (size_t i = 0; i < std::size(expected); ++i) {
        ASSERT_TRUE(out.indices[i] == expected[i]);
    }
    return true;
}

static bool test_bad_attribute_indices() {
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.raw_refs[2].pos_idx = 3;
        awl::GplRenderMesh out;
        out.vertices.resize(1);
        out.indices.resize(1);
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
        ASSERT_TRUE(out.vertices.empty() && out.indices.empty());
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.raw_refs[1].uv_idx = 3;
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
        ASSERT_TRUE(out.vertices.empty() && out.indices.empty());
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        src.colors_rgba8 = {255, 255, 255, 255};
        for (auto& ref : src.raw_refs) {
            ref.has_color = true;
        }
        src.raw_refs[1].color_idx = 1;
        add_primitive(src, 0x90, 0, 3);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
        ASSERT_TRUE(out.vertices.empty() && out.indices.empty());
    }
    return true;
}

static bool test_missing_required_data() {
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.raw_refs[1].has_uv = false;
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.raw_refs[0].has_pos = false;
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.uvs.clear();
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    return true;
}

static bool test_unsupported_or_malformed_primitives() {
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0xA8, 0, 3);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(4);
        add_primitive(src, 0x90, 0, 4);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(5);
        add_primitive(src, 0x80, 0, 5);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    return true;
}

static bool test_primitive_ranges_must_cover_refs() {
    {
        awl::GplMeshAnalysis src = make_source(6);
        add_primitive(src, 0x90, 0, 3);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(6);
        add_primitive(src, 0x90, 0, 3);
        add_primitive(src, 0x90, 4, 2);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    return true;
}

static bool test_malformed_array_shapes() {
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.positions.pop_back();
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        add_primitive(src, 0x90, 0, 3);
        src.uvs.pop_back();
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        src.colors_rgba8 = {255, 255, 255};
        for (auto& ref : src.raw_refs) {
            ref.has_color = true;
        }
        add_primitive(src, 0x90, 0, 3);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        src.colors_rgba8 = {255, 255, 255, 255};
        add_primitive(src, 0x90, 0, 3);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    {
        awl::GplMeshAnalysis src = make_source(3);
        src.colors_rgba8 = {255, 255, 255, 255};
        for (auto& ref : src.raw_refs) {
            ref.has_color = true;
        }
        src.raw_refs[2].has_color = false;
        add_primitive(src, 0x90, 0, 3);
        awl::GplRenderMesh out;
        ASSERT_FALSE(awl::build_gpl_render_mesh(src, out));
    }
    return true;
}

int main() {
    int failed = 0;
    const auto run = [&failed](const char* name, bool (*test)()) {
        std::cout << "[RUNNING] " << name << std::endl;
        if (test()) {
            std::cout << "[PASSED]" << std::endl;
        } else {
            std::cout << "[FAILED]" << std::endl;
            ++failed;
        }
    };

    run("test_valid_triangle", test_valid_triangle);
    run("test_indexed_vertex_colors", test_indexed_vertex_colors);
    run("test_valid_quad_triangulation", test_valid_quad_triangulation);
    run("test_strip_fan_and_multiple_primitives",
        test_strip_fan_and_multiple_primitives);
    run("test_bad_attribute_indices", test_bad_attribute_indices);
    run("test_missing_required_data", test_missing_required_data);
    run("test_unsupported_or_malformed_primitives",
        test_unsupported_or_malformed_primitives);
    run("test_primitive_ranges_must_cover_refs",
        test_primitive_ranges_must_cover_refs);
    run("test_malformed_array_shapes", test_malformed_array_shapes);

    if (failed != 0) {
        std::cerr << "[TEST RESULT] " << failed << " test(s) failed."
                  << std::endl;
        return 1;
    }
    std::cout << "[TEST RESULT] All tests passed!" << std::endl;
    return 0;
}
