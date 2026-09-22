#include "awl/filesystem.h"
#include "awl/gpl.h"
#include "awl/memory.h"
#include "awl/tpl.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#define ASSERT_TRUE(condition)                                                        \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::cerr << "Assertion failed: " << #condition << " at " << __FILE__   \
                      << ":" << __LINE__ << std::endl;                               \
            return false;                                                             \
        }                                                                             \
    } while (0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))

static void put_be16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<uint8_t>(value);
}

static void put_be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

static void put_be_float(std::vector<uint8_t>& bytes, size_t offset, float value) {
    uint32_t raw = 0;
    static_assert(sizeof(raw) == sizeof(value), "Unexpected float size");
    std::memcpy(&raw, &value, sizeof(raw));
    put_be32(bytes, offset, raw);
}

static bool write_fixture(const fs::path& root, const std::string& name,
                          const std::vector<uint8_t>& bytes) {
    std::ofstream output(root / name, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

static std::vector<uint8_t> make_tpl(uint16_t width, uint16_t height, uint32_t format,
                                     const std::vector<uint8_t>& texture_data) {
    constexpr size_t descriptor_offset = 12;
    constexpr size_t image_header_offset = 20;
    constexpr size_t data_offset = 56;
    std::vector<uint8_t> bytes(data_offset + texture_data.size(), 0);
    put_be32(bytes, 0, 0x0020AF30);
    put_be32(bytes, 4, 1);
    put_be32(bytes, 8, static_cast<uint32_t>(descriptor_offset));
    put_be32(bytes, descriptor_offset, static_cast<uint32_t>(image_header_offset));
    put_be32(bytes, descriptor_offset + 4, 0);
    put_be16(bytes, image_header_offset + 0, height);
    put_be16(bytes, image_header_offset + 2, width);
    put_be32(bytes, image_header_offset + 4, format);
    put_be32(bytes, image_header_offset + 8, static_cast<uint32_t>(data_offset));
    std::copy(texture_data.begin(), texture_data.end(), bytes.begin() + data_offset);
    return bytes;
}

static std::vector<uint8_t> make_gpl(uint8_t primitive, uint8_t first_position_index,
                                      uint32_t position_data_offset = 28,
                                      uint32_t vertex_descriptor = 0x00000828) {
    constexpr size_t section_offset = 24;
    constexpr size_t position_header_offset = section_offset + 20;
    constexpr size_t position_data_absolute = section_offset + 28;
    constexpr size_t color_header_relative = 64;
    constexpr size_t color_header_offset = section_offset + color_header_relative;
    constexpr size_t color_data_relative = 72;
    constexpr size_t color_data_absolute = section_offset + color_data_relative;
    constexpr size_t uv_header_relative = 76;
    constexpr size_t uv_header_offset = section_offset + uv_header_relative;
    constexpr size_t uv_data_relative = 84;
    constexpr size_t uv_data_absolute = section_offset + uv_data_relative;
    constexpr size_t normal_descriptor_relative = 108;
    constexpr size_t normal_descriptor_offset =
        section_offset + normal_descriptor_relative;
    constexpr size_t material_header_relative = 120;
    constexpr size_t material_header_offset =
        section_offset + material_header_relative;
    constexpr size_t command_list_relative = 132;
    constexpr size_t command_list_offset = section_offset + command_list_relative;
    constexpr size_t display_list_relative = 148;
    constexpr size_t display_list_offset =
        section_offset + display_list_relative;
    constexpr uint32_t display_list_size = 12;

    std::vector<uint8_t> bytes(display_list_offset + display_list_size, 0);
    put_be32(bytes, 0, 0x005BBC61);
    put_be32(bytes, 12, 1);
    put_be32(bytes, 16, 0x14);
    put_be32(bytes, 20, static_cast<uint32_t>(section_offset));

    put_be32(bytes, section_offset + 0, 20);
    put_be32(bytes, section_offset + 4,
             static_cast<uint32_t>(color_header_relative));
    put_be32(bytes, section_offset + 8,
             static_cast<uint32_t>(uv_header_relative));
    put_be32(bytes, section_offset + 12,
             static_cast<uint32_t>(normal_descriptor_relative));
    put_be32(bytes, section_offset + 16,
             static_cast<uint32_t>(material_header_relative));

    put_be32(bytes, position_header_offset, position_data_offset);
    put_be16(bytes, position_header_offset + 4, 3);
    bytes[position_header_offset + 6] = 0x40;
    bytes[position_header_offset + 7] = 3;

    const float positions[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    for (size_t i = 0; i < std::size(positions); ++i) {
        put_be_float(bytes, position_data_absolute + i * sizeof(float), positions[i]);
    }

    put_be32(bytes, color_header_offset,
             static_cast<uint32_t>(color_data_relative));
    put_be16(bytes, color_header_offset + 4, 1);
    bytes[color_header_offset + 6] = 0x00;
    bytes[color_header_offset + 7] = 3;
    bytes[color_data_absolute] = 0xFF;
    bytes[color_data_absolute + 1] = 0xFF;
    bytes[color_data_absolute + 2] = 0xFF;

    put_be32(bytes, uv_header_offset,
             static_cast<uint32_t>(uv_data_relative));
    put_be16(bytes, uv_header_offset + 4, 3);
    bytes[uv_header_offset + 6] = 0x40;
    bytes[uv_header_offset + 7] = 2;

    const float uvs[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < std::size(uvs); ++i) {
        put_be_float(bytes, uv_data_absolute + i * sizeof(float), uvs[i]);
    }

    put_be32(bytes, normal_descriptor_offset, 0);

    put_be32(bytes, material_header_offset + 0,
              static_cast<uint32_t>(display_list_relative));
    put_be32(bytes, material_header_offset + 4,
             static_cast<uint32_t>(command_list_relative));
    put_be16(bytes, material_header_offset + 8, 1);

    bytes[command_list_offset] = 2;
    put_be32(bytes, command_list_offset + 4, vertex_descriptor);
    put_be32(bytes, command_list_offset + 8,
             static_cast<uint32_t>(display_list_relative));
    put_be32(bytes, command_list_offset + 12, display_list_size);

    bytes[display_list_offset + 0] = primitive;
    put_be16(bytes, display_list_offset + 1, 3);
    bytes[display_list_offset + 3] = first_position_index;
    bytes[display_list_offset + 4] = 0;
    bytes[display_list_offset + 5] = 0;
    bytes[display_list_offset + 6] = 1;
    bytes[display_list_offset + 7] = 1;
    bytes[display_list_offset + 8] = 1;
    bytes[display_list_offset + 9] = 2;
    bytes[display_list_offset + 10] = 2;
    bytes[display_list_offset + 11] = 2;
    return bytes;
}

static std::vector<uint8_t> make_indexed_color_gpl(
    bool rgba4, uint8_t first_color_index = 0,
    bool include_target_tev = false,
    uint32_t vertex_descriptor = 0x000008A8) {
    constexpr size_t section_offset = 24;
    constexpr size_t position_header_relative = 20;
    constexpr size_t position_header_offset =
        section_offset + position_header_relative;
    constexpr size_t position_data_relative = 28;
    constexpr size_t position_data_offset =
        section_offset + position_data_relative;
    constexpr size_t color_header_relative = 64;
    constexpr size_t color_header_offset =
        section_offset + color_header_relative;
    constexpr size_t color_data_relative = 72;
    constexpr size_t color_data_offset =
        section_offset + color_data_relative;
    constexpr size_t uv_header_relative = 80;
    constexpr size_t uv_header_offset = section_offset + uv_header_relative;
    constexpr size_t uv_data_relative = 88;
    constexpr size_t uv_data_offset = section_offset + uv_data_relative;
    constexpr size_t normal_descriptor_relative = 112;
    constexpr size_t normal_descriptor_offset =
        section_offset + normal_descriptor_relative;
    constexpr size_t material_header_relative = 124;
    constexpr size_t material_header_offset =
        section_offset + material_header_relative;
    constexpr size_t command_list_relative = 136;
    constexpr size_t command_list_offset =
        section_offset + command_list_relative;
    const size_t geometry_command_relative =
        command_list_relative + (include_target_tev ? 16 : 0);
    const size_t geometry_command_offset =
        section_offset + geometry_command_relative;
    const size_t display_list_relative = geometry_command_relative + 16;
    const size_t display_list_offset =
        section_offset + display_list_relative;
    const bool position_index_16 =
        vertex_descriptor == 0x000008AC || vertex_descriptor == 0x000008EC ||
        vertex_descriptor == 0x00000CEC;
    const bool color_index_16 =
        vertex_descriptor == 0x000008EC || vertex_descriptor == 0x00000CEC;
    const bool uv_index_16 = vertex_descriptor == 0x00000CEC;
    const uint32_t reference_size =
        (position_index_16 ? 2u : 1u) + 1u +
        (color_index_16 ? 2u : 1u) + (uv_index_16 ? 2u : 1u);
    const uint32_t display_list_size = 3u + 3u * reference_size;

    std::vector<uint8_t> bytes(display_list_offset + display_list_size, 0);
    put_be32(bytes, 0, 0x005BBC61);
    put_be32(bytes, 12, 1);
    put_be32(bytes, 16, 0x14);
    put_be32(bytes, 20, static_cast<uint32_t>(section_offset));

    put_be32(bytes, section_offset + 0,
             static_cast<uint32_t>(position_header_relative));
    put_be32(bytes, section_offset + 4,
             static_cast<uint32_t>(color_header_relative));
    put_be32(bytes, section_offset + 8,
             static_cast<uint32_t>(uv_header_relative));
    put_be32(bytes, section_offset + 12,
             static_cast<uint32_t>(normal_descriptor_relative));
    put_be32(bytes, section_offset + 16,
             static_cast<uint32_t>(material_header_relative));

    put_be32(bytes, position_header_offset,
             static_cast<uint32_t>(position_data_relative));
    put_be16(bytes, position_header_offset + 4, 3);
    bytes[position_header_offset + 6] = 0x40;
    bytes[position_header_offset + 7] = 3;
    const float positions[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    for (size_t i = 0; i < std::size(positions); ++i) {
        put_be_float(bytes, position_data_offset + i * sizeof(float),
                     positions[i]);
    }

    put_be32(bytes, color_header_offset,
             static_cast<uint32_t>(color_data_relative));
    put_be16(bytes, color_header_offset + 4, 3);
    bytes[color_header_offset + 6] = rgba4 ? 0x30 : 0x00;
    bytes[color_header_offset + 7] = rgba4 ? 4 : 3;
    const uint16_t colors_rgb565[] = {0xF800, 0x07E0, 0x001F};
    const uint16_t colors_rgba4[] = {0xF00F, 0x0F08, 0x00F4};
    for (size_t i = 0; i < 3; ++i) {
        put_be16(bytes, color_data_offset + i * 2,
                 rgba4 ? colors_rgba4[i] : colors_rgb565[i]);
    }

    put_be32(bytes, uv_header_offset,
             static_cast<uint32_t>(uv_data_relative));
    put_be16(bytes, uv_header_offset + 4, 3);
    bytes[uv_header_offset + 6] = 0x40;
    bytes[uv_header_offset + 7] = 2;
    const float uvs[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < std::size(uvs); ++i) {
        put_be_float(bytes, uv_data_offset + i * sizeof(float), uvs[i]);
    }

    put_be32(bytes, normal_descriptor_offset, 0);
    put_be32(bytes, material_header_offset,
             static_cast<uint32_t>(display_list_relative));
    put_be32(bytes, material_header_offset + 4,
             static_cast<uint32_t>(command_list_relative));
    put_be16(bytes, material_header_offset + 8,
             static_cast<uint16_t>(include_target_tev ? 2 : 1));

    if (include_target_tev) {
        bytes[command_list_offset] = 3;
        put_be32(bytes, command_list_offset + 4, 1);
    }
    bytes[geometry_command_offset] = 2;
    put_be32(bytes, geometry_command_offset + 4, vertex_descriptor);
    put_be32(bytes, geometry_command_offset + 8,
             static_cast<uint32_t>(display_list_relative));
    put_be32(bytes, geometry_command_offset + 12, display_list_size);

    bytes[display_list_offset] = 0x90;
    put_be16(bytes, display_list_offset + 1, 3);
    size_t reference_offset = display_list_offset + 3;
    for (size_t i = 0; i < 3; ++i) {
        if (position_index_16) {
            put_be16(bytes, reference_offset, static_cast<uint16_t>(i));
            reference_offset += 2;
        } else {
            bytes[reference_offset++] = static_cast<uint8_t>(i);
        }
        bytes[reference_offset++] = static_cast<uint8_t>(i);
        const uint16_t color_index =
            i == 0 ? first_color_index : static_cast<uint16_t>(i);
        if (color_index_16) {
            put_be16(bytes, reference_offset, color_index);
            reference_offset += 2;
        } else {
            bytes[reference_offset++] = static_cast<uint8_t>(color_index);
        }
        if (uv_index_16) {
            put_be16(bytes, reference_offset, static_cast<uint16_t>(i));
            reference_offset += 2;
        } else {
            bytes[reference_offset++] = static_cast<uint8_t>(i);
        }
    }
    return bytes;
}

static std::vector<uint8_t> make_gpl_with_texture_command(
    uint32_t command_word, uint8_t command_type = 1,
    bool include_target_tev = false, uint32_t tev_word = 1) {
    constexpr size_t section_offset = 24;
    constexpr size_t material_header_offset = section_offset + 120;
    constexpr size_t command_list_relative = 132;
    constexpr size_t command_list_offset =
        section_offset + command_list_relative;
    constexpr size_t display_list_size = 12;

    const size_t geometry_command_relative =
        command_list_relative + (include_target_tev ? 32 : 16);
    const size_t geometry_command_offset =
        section_offset + geometry_command_relative;
    const size_t display_list_relative = geometry_command_relative + 16;
    const size_t display_list_offset = section_offset + display_list_relative;

    std::vector<uint8_t> bytes = make_gpl(0x90, 0);
    constexpr size_t original_display_list_offset = section_offset + 148;
    const std::vector<uint8_t> primitive(
        bytes.begin() + original_display_list_offset, bytes.end());
    bytes.resize(display_list_offset + primitive.size(), 0);
    std::fill(bytes.begin() + command_list_offset, bytes.end(),
              uint8_t{0});

    put_be32(bytes, material_header_offset + 0,
             static_cast<uint32_t>(display_list_relative));
    put_be32(bytes, material_header_offset + 4,
              static_cast<uint32_t>(command_list_relative));
    put_be16(bytes, material_header_offset + 8,
             static_cast<uint16_t>(include_target_tev ? 3 : 2));

    bytes[command_list_offset + 0] = command_type;
    bytes[command_list_offset + 1] = 0xFF;
    put_be32(bytes, command_list_offset + 4, command_word);

    if (include_target_tev) {
        const size_t tev_command_offset = command_list_offset + 16;
        bytes[tev_command_offset] = 3;
        put_be32(bytes, tev_command_offset + 4, tev_word);
    }

    bytes[geometry_command_offset] = 2;
    put_be32(bytes, geometry_command_offset + 4, 0x00000828);
    put_be32(bytes, geometry_command_offset + 8,
             static_cast<uint32_t>(display_list_relative));
    put_be32(bytes, geometry_command_offset + 12,
             static_cast<uint32_t>(display_list_size));
    std::copy(primitive.begin(), primitive.end(),
              bytes.begin() + display_list_offset);
    return bytes;
}

static std::vector<uint8_t> make_multi_primitive_gpl() {
    constexpr size_t section_offset = 24;
    constexpr size_t geometry_command_offset = section_offset + 132;
    constexpr size_t display_list_offset = section_offset + 148;
    constexpr uint32_t display_list_size = 56;

    std::vector<uint8_t> bytes = make_gpl(0x90, 0);
    bytes.resize(display_list_offset + display_list_size, 0);
    std::fill(bytes.begin() + display_list_offset, bytes.end(), uint8_t{0});
    put_be32(bytes, geometry_command_offset + 12, display_list_size);

    size_t cursor = display_list_offset;
    const auto append_primitive = [&bytes, &cursor](uint8_t opcode,
                                                    uint16_t count) {
        bytes[cursor] = opcode;
        put_be16(bytes, cursor + 1, count);
        cursor += 3;
        for (uint16_t i = 0; i < count; ++i) {
            bytes[cursor++] = static_cast<uint8_t>(i % 3);
            bytes[cursor++] = static_cast<uint8_t>(10 + i);
            bytes[cursor++] = static_cast<uint8_t>(i % 3);
        }
    };
    append_primitive(0x90, 3);
    append_primitive(0x80, 4);
    append_primitive(0x98, 3);
    append_primitive(0xA0, 3);
    return bytes;
}

static std::vector<uint8_t> make_ordered_multi_draw_gpl() {
    constexpr size_t section_offset = 24;
    constexpr size_t material_header_offset = section_offset + 120;
    constexpr size_t command_list_relative = 132;
    constexpr size_t command_list_offset =
        section_offset + command_list_relative;
    constexpr size_t command_count = 5;
    constexpr size_t first_display_relative =
        command_list_relative + command_count * 16;
    constexpr size_t first_display_offset =
        section_offset + first_display_relative;
    constexpr size_t display_size = 12;

    std::vector<uint8_t> bytes = make_gpl(0x90, 0);
    bytes.resize(first_display_offset + display_size * 3, 0);
    std::fill(bytes.begin() + command_list_offset, bytes.end(), uint8_t{0});

    put_be32(bytes, material_header_offset,
             static_cast<uint32_t>(first_display_relative));
    put_be32(bytes, material_header_offset + 4,
             static_cast<uint32_t>(command_list_relative));
    put_be16(bytes, material_header_offset + 8,
             static_cast<uint16_t>(command_count));

    bytes[command_list_offset] = 1;
    bytes[command_list_offset + 1] = 0xFF;
    put_be32(bytes, command_list_offset + 4, 0);

    bytes[command_list_offset + 16] = 3;
    put_be32(bytes, command_list_offset + 20, 1);

    bytes[command_list_offset + 32] = 2;
    put_be32(bytes, command_list_offset + 36, 0x00000828);
    put_be32(bytes, command_list_offset + 40,
             static_cast<uint32_t>(first_display_relative));
    put_be32(bytes, command_list_offset + 44,
             static_cast<uint32_t>(display_size));

    bytes[command_list_offset + 48] = 1;
    bytes[command_list_offset + 49] = 0xFF;
    put_be32(bytes, command_list_offset + 52, 2);
    put_be32(bytes, command_list_offset + 56,
             static_cast<uint32_t>(first_display_relative + display_size));
    put_be32(bytes, command_list_offset + 60,
             static_cast<uint32_t>(display_size));

    bytes[command_list_offset + 64] = 2;
    put_be32(bytes, command_list_offset + 68, 0x00000828);
    put_be32(bytes, command_list_offset + 72,
             static_cast<uint32_t>(first_display_relative + display_size * 2));
    put_be32(bytes, command_list_offset + 76,
             static_cast<uint32_t>(display_size));

    for (size_t draw = 0; draw < 3; ++draw) {
        const size_t display_offset = first_display_offset + draw * display_size;
        bytes[display_offset] = 0x90;
        put_be16(bytes, display_offset + 1, 3);
        for (size_t vertex = 0; vertex < 3; ++vertex) {
            const size_t reference_offset = display_offset + 3 + vertex * 3;
            bytes[reference_offset] = static_cast<uint8_t>(vertex);
            bytes[reference_offset + 1] = static_cast<uint8_t>(vertex);
            bytes[reference_offset + 2] = static_cast<uint8_t>(vertex);
        }
    }
    return bytes;
}

static std::vector<uint8_t> make_multi_section_gpl() {
    std::vector<uint8_t> bytes =
        make_gpl_with_texture_command(0x11110002, 1, true, 1);
    const uint32_t second_section_offset =
        static_cast<uint32_t>(bytes.size() + sizeof(uint32_t));

    // Add room for the second section-table entry. This moves the complete,
    // valid first section from offset 24 to offset 28 without changing any of
    // its section-relative offsets.
    bytes.insert(bytes.begin() + 24, sizeof(uint32_t), uint8_t{0});
    put_be32(bytes, 12, 2);
    put_be32(bytes, 20, 28);
    put_be32(bytes, 24, second_section_offset);
    bytes.resize(static_cast<size_t>(second_section_offset) + 20, 0);
    return bytes;
}

static bool test_valid_rgb5a3(const fs::path& root) {
    std::vector<uint8_t> data(32, 0xFF);
    ASSERT_TRUE(write_fixture(root, "valid_rgb5a3.tpl", make_tpl(4, 4, 5, data)));

    awl::TplFile tpl;
    ASSERT_TRUE(awl::tpl_load_from_file("/valid_rgb5a3.tpl", &tpl));
    ASSERT_TRUE(tpl.textures.size() == 1);
    const std::vector<uint8_t> rgba = awl::tpl_convert_to_rgba8(tpl.textures[0]);
    ASSERT_TRUE(rgba.size() == 64);
    ASSERT_TRUE(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255 &&
                rgba[3] == 255);
    awl::tpl_free(&tpl);
    return true;
}

static bool test_i4_intensity_populates_rgba(const fs::path& root) {
    std::vector<uint8_t> data(32, 0);
    data[0] = 0xA3;
    ASSERT_TRUE(write_fixture(root, "i4.tpl", make_tpl(8, 8, 0, data)));

    awl::TplFile tpl;
    ASSERT_TRUE(awl::tpl_load_from_file("/i4.tpl", &tpl));
    const std::vector<uint8_t> rgba = awl::tpl_convert_to_rgba8(tpl.textures[0]);
    ASSERT_TRUE(rgba.size() == 8 * 8 * 4);
    ASSERT_TRUE(rgba[0] == 170 && rgba[1] == 170 && rgba[2] == 170 &&
                rgba[3] == 170);
    ASSERT_TRUE(rgba[4] == 51 && rgba[5] == 51 && rgba[6] == 51 &&
                rgba[7] == 51);
    awl::tpl_free(&tpl);
    return true;
}

static bool test_tiled_i8(const fs::path& root) {
    std::vector<uint8_t> data(64, 200);
    std::fill(data.begin(), data.begin() + 32, static_cast<uint8_t>(10));
    ASSERT_TRUE(write_fixture(root, "tiled_i8.tpl", make_tpl(16, 4, 1, data)));

    awl::TplFile tpl;
    ASSERT_TRUE(awl::tpl_load_from_file("/tiled_i8.tpl", &tpl));
    const std::vector<uint8_t> rgba = awl::tpl_convert_to_rgba8(tpl.textures[0]);
    ASSERT_TRUE(rgba.size() == 16 * 4 * 4);
    const size_t left_second_row = (16 * 1) * 4;
    const size_t right_first_row = 8 * 4;
    ASSERT_TRUE(rgba[left_second_row] == 10 &&
                rgba[left_second_row + 1] == 10 &&
                rgba[left_second_row + 2] == 10 &&
                rgba[left_second_row + 3] == 10);
    ASSERT_TRUE(rgba[right_first_row] == 200 &&
                rgba[right_first_row + 1] == 200 &&
                rgba[right_first_row + 2] == 200 &&
                rgba[right_first_row + 3] == 200);
    awl::tpl_free(&tpl);
    return true;
}

static bool test_planar_rgba8(const fs::path& root) {
    std::vector<uint8_t> data(64, 0);
    data[0] = 0x44;
    data[1] = 0x11;
    data[32] = 0x22;
    data[33] = 0x33;
    ASSERT_TRUE(write_fixture(root, "planar_rgba8.tpl", make_tpl(4, 4, 6, data)));

    awl::TplFile tpl;
    ASSERT_TRUE(awl::tpl_load_from_file("/planar_rgba8.tpl", &tpl));
    const std::vector<uint8_t> rgba = awl::tpl_convert_to_rgba8(tpl.textures[0]);
    ASSERT_TRUE(rgba.size() == 64);
    ASSERT_TRUE(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 &&
                rgba[3] == 0x44);
    awl::tpl_free(&tpl);
    return true;
}

static bool test_cmpr_block_layout_and_alpha(const fs::path& root) {
    std::vector<uint8_t> data(32, 0);

    // Top-left: exercise all four opaque selectors on the first row.
    put_be16(data, 0, 0xF800);
    put_be16(data, 2, 0x07E0);
    data[4] = 0x1B; // 00, 01, 10, 11

    // Top-right: opaque blue (selector 0).
    put_be16(data, 8, 0x001F);
    put_be16(data, 10, 0x0000);

    // Bottom-left: endpoint average (selector 2, 0b10 repeated).
    put_be16(data, 16, 0x0000);
    put_be16(data, 18, 0xFFFF);
    std::fill(data.begin() + 20, data.begin() + 24, static_cast<uint8_t>(0xAA));

    // Bottom-right: the same average RGB with transparent alpha (selector 3).
    put_be16(data, 24, 0x0000);
    put_be16(data, 26, 0xFFFF);
    std::fill(data.begin() + 28, data.end(), static_cast<uint8_t>(0xFF));

    ASSERT_TRUE(write_fixture(root, "cmpr.tpl", make_tpl(8, 8, 14, data)));
    awl::TplFile tpl;
    ASSERT_TRUE(awl::tpl_load_from_file("/cmpr.tpl", &tpl));
    const std::vector<uint8_t> rgba = awl::tpl_convert_to_rgba8(tpl.textures[0]);
    ASSERT_TRUE(rgba.size() == 8 * 8 * 4);

    const auto pixel = [](size_t x, size_t y) { return (y * 8 + x) * 4; };
    size_t offset = pixel(0, 0);
    ASSERT_TRUE(rgba[offset] == 255 && rgba[offset + 1] == 0 &&
                rgba[offset + 2] == 0 && rgba[offset + 3] == 255);
    offset = pixel(1, 0);
    ASSERT_TRUE(rgba[offset] == 0 && rgba[offset + 1] == 255 &&
                rgba[offset + 2] == 0 && rgba[offset + 3] == 255);
    offset = pixel(2, 0);
    ASSERT_TRUE(rgba[offset] == 159 && rgba[offset + 1] == 95 &&
                rgba[offset + 2] == 0 && rgba[offset + 3] == 255);
    offset = pixel(3, 0);
    ASSERT_TRUE(rgba[offset] == 95 && rgba[offset + 1] == 159 &&
                rgba[offset + 2] == 0 && rgba[offset + 3] == 255);
    offset = pixel(4, 0);
    ASSERT_TRUE(rgba[offset] == 0 && rgba[offset + 1] == 0 &&
                rgba[offset + 2] == 255 && rgba[offset + 3] == 255);
    offset = pixel(0, 4);
    ASSERT_TRUE(rgba[offset] == 127 && rgba[offset + 1] == 127 &&
                rgba[offset + 2] == 127 && rgba[offset + 3] == 255);
    offset = pixel(4, 4);
    ASSERT_TRUE(rgba[offset] == 127 && rgba[offset + 1] == 127 &&
                rgba[offset + 2] == 127 && rgba[offset + 3] == 0);
    return true;
}

static bool test_tpl_decodes_complete_mip_chain(const fs::path& root) {
    // I8 levels are padded to 8x4 blocks: 8x8=64, 4x4=32, 2x2=32.
    std::vector<uint8_t> data(128, 30);
    std::fill(data.begin(), data.begin() + 64, static_cast<uint8_t>(10));
    std::fill(data.begin() + 64, data.begin() + 96, static_cast<uint8_t>(20));
    std::vector<uint8_t> fixture = make_tpl(8, 8, 1, data);
    constexpr size_t image_header_offset = 20;
    put_be32(fixture, image_header_offset + 12, 1); // GX_REPEAT
    put_be32(fixture, image_header_offset + 16, 2); // GX_MIRROR
    put_be32(fixture, image_header_offset + 20, 5); // GX_LIN_MIP_LIN
    put_be32(fixture, image_header_offset + 24, 1); // GX_LINEAR
    put_be_float(fixture, image_header_offset + 28, -1.0f);
    fixture[image_header_offset + 34] = 2;
    ASSERT_TRUE(write_fixture(root, "mip_chain.tpl", fixture));

    awl::TplFile tpl;
    ASSERT_TRUE(awl::tpl_load_from_file("/mip_chain.tpl", &tpl));
    ASSERT_TRUE(tpl.textures.size() == 1);
    const awl::TplTexture& texture = tpl.textures[0];
    ASSERT_TRUE(texture.mip_levels.size() == 3);
    ASSERT_TRUE(texture.encoded_data_size == 128);
    ASSERT_TRUE(texture.mip_levels[0].width == 8 &&
                texture.mip_levels[0].height == 8 &&
                texture.mip_levels[0].encoded_size == 64);
    ASSERT_TRUE(texture.mip_levels[1].width == 4 &&
                texture.mip_levels[1].height == 4 &&
                texture.mip_levels[1].encoded_size == 32);
    ASSERT_TRUE(texture.mip_levels[2].width == 2 &&
                texture.mip_levels[2].height == 2 &&
                texture.mip_levels[2].encoded_size == 32);

    awl::TplDecodedTexture decoded;
    ASSERT_TRUE(awl::tpl_decode_mip_chain_to_rgba8(texture, &decoded));
    ASSERT_TRUE(decoded.mip_levels.size() == 3);
    ASSERT_TRUE(decoded.mip_levels[0].rgba8.size() == 8 * 8 * 4);
    ASSERT_TRUE(decoded.mip_levels[1].rgba8.size() == 4 * 4 * 4);
    ASSERT_TRUE(decoded.mip_levels[2].rgba8.size() == 2 * 2 * 4);
    ASSERT_TRUE(decoded.mip_levels[0].rgba8[0] == 10);
    ASSERT_TRUE(decoded.mip_levels[1].rgba8[0] == 20);
    ASSERT_TRUE(decoded.mip_levels[2].rgba8[0] == 30);
    ASSERT_TRUE(decoded.header.wrap_s == 1 && decoded.header.wrap_t == 2);
    ASSERT_TRUE(decoded.header.min_filter == 5 && decoded.header.mag_filter == 1);
    ASSERT_TRUE(decoded.header.lod_bias == -1.0f);
    awl::tpl_free(&tpl);
    return true;
}

static bool test_tpl_rejects_truncated_or_impossible_mip_chain(
    const fs::path& root) {
    std::vector<uint8_t> base_level(64, 10);
    std::vector<uint8_t> truncated = make_tpl(8, 8, 1, base_level);
    constexpr size_t image_header_offset = 20;
    truncated[image_header_offset + 34] = 2;
    ASSERT_TRUE(write_fixture(root, "truncated_mips.tpl", truncated));

    awl::TplFile tpl;
    ASSERT_FALSE(awl::tpl_load_from_file("/truncated_mips.tpl", &tpl));

    std::vector<uint8_t> impossible = make_tpl(8, 8, 1, base_level);
    impossible[image_header_offset + 34] = 4;
    ASSERT_TRUE(write_fixture(root, "impossible_mips.tpl", impossible));
    ASSERT_FALSE(awl::tpl_load_from_file("/impossible_mips.tpl", &tpl));
    return true;
}

static bool test_tpl_rejects_malformed_bounds(const fs::path& root) {
    std::vector<uint8_t> excessive_count(12, 0);
    put_be32(excessive_count, 0, 0x0020AF30);
    put_be32(excessive_count, 4, 0xFFFFFFFF);
    put_be32(excessive_count, 8, 12);
    ASSERT_TRUE(write_fixture(root, "tpl_bad_count.tpl", excessive_count));

    awl::TplFile tpl;
    ASSERT_FALSE(awl::tpl_load_from_file("/tpl_bad_count.tpl", &tpl));
    ASSERT_TRUE(tpl.raw_file_data == nullptr);
    ASSERT_TRUE(tpl.textures.empty());

    std::vector<uint8_t> invalid_data = make_tpl(4, 4, 5, std::vector<uint8_t>(32));
    put_be32(invalid_data, 20 + 8, 0xFFFFFFF0);
    ASSERT_TRUE(write_fixture(root, "tpl_bad_data.tpl", invalid_data));
    ASSERT_FALSE(awl::tpl_load_from_file("/tpl_bad_data.tpl", &tpl));
    ASSERT_TRUE(tpl.raw_file_data == nullptr);
    return true;
}

static bool test_valid_gpl_triangle(const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "valid_triangle.gpl", make_gpl(0x90, 0)));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/valid_triangle.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.primitives.size() == 1);
    ASSERT_TRUE(mesh.primitives[0].type == 0x90);
    ASSERT_TRUE(mesh.primitives[0].first_ref == 0);
    ASSERT_TRUE(mesh.primitives[0].ref_count == 3);
    ASSERT_TRUE(mesh.positions.size() == 9);
    ASSERT_TRUE(mesh.uvs.size() == 6);
    ASSERT_TRUE(mesh.raw_refs.size() == 3);
    ASSERT_TRUE(mesh.raw_refs[2].normal_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].has_normal);
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_indexed_color_layouts(const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "indexed_rgb565.gpl",
                              make_indexed_color_gpl(false)));
    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/indexed_rgb565.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(
        awl::gpl_parse_display_list_for_analysis(gpl, "RGB565", &mesh));
    ASSERT_TRUE(mesh.colors_rgba8.size() == 12);
    ASSERT_TRUE(mesh.colors_rgba8[0] == 255);
    ASSERT_TRUE(mesh.colors_rgba8[1] == 0);
    ASSERT_TRUE(mesh.colors_rgba8[2] == 0);
    ASSERT_TRUE(mesh.colors_rgba8[3] == 255);
    ASSERT_TRUE(mesh.colors_rgba8[4] == 0);
    ASSERT_TRUE(mesh.colors_rgba8[5] == 255);
    ASSERT_TRUE(mesh.raw_refs.size() == 3);
    ASSERT_TRUE(mesh.raw_refs[2].has_color);
    ASSERT_TRUE(mesh.raw_refs[2].color_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].uv_idx == 2);
    awl::gpl_free(&gpl);

    ASSERT_TRUE(write_fixture(root, "indexed_rgba4.gpl",
                              make_indexed_color_gpl(true, 0, true)));
    ASSERT_TRUE(awl::gpl_load_from_file("/indexed_rgba4.gpl", &gpl));
    ASSERT_TRUE(
        awl::gpl_parse_display_list_for_analysis(gpl, "RGBA4", &mesh));
    ASSERT_TRUE(mesh.colors_rgba8.size() == 12);
    ASSERT_TRUE(mesh.colors_rgba8[0] == 255);
    ASSERT_TRUE(mesh.colors_rgba8[1] == 0);
    ASSERT_TRUE(mesh.colors_rgba8[2] == 0);
    ASSERT_TRUE(mesh.colors_rgba8[3] == 255);
    ASSERT_TRUE(mesh.colors_rgba8[4] == 0);
    ASSERT_TRUE(mesh.colors_rgba8[5] == 255);
    ASSERT_TRUE(mesh.colors_rgba8[7] == 136);

    awl::GplTargetMaterial material;
    ASSERT_TRUE(awl::gpl_parse_target_material_for_analysis(gpl, &material));
    ASSERT_TRUE(material.command_index == 0);
    ASSERT_TRUE(material.tev_mode_word == 1);
    ASSERT_TRUE(material.uses_vertex_color);
    ASSERT_TRUE(material.red == 255 && material.green == 255 &&
                material.blue == 255 && material.alpha == 255);
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_position16_indexed_color_layout(const fs::path& root) {
    ASSERT_TRUE(write_fixture(
        root, "indexed_position16.gpl",
        make_indexed_color_gpl(false, 0, true, 0x000008AC)));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/indexed_position16.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 position", &mesh));
    ASSERT_TRUE(mesh.raw_refs.size() == 3);
    ASSERT_TRUE(mesh.raw_refs[0].pos_idx == 0);
    ASSERT_TRUE(mesh.raw_refs[1].pos_idx == 1);
    ASSERT_TRUE(mesh.raw_refs[2].pos_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].normal_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].color_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].uv_idx == 2);
    awl::gpl_free(&gpl);

    constexpr size_t section_offset = 24;
    constexpr size_t command_list_relative = 136;
    constexpr size_t geometry_command_offset =
        section_offset + command_list_relative;
    constexpr size_t display_list_offset =
        section_offset + command_list_relative + 16;

    std::vector<uint8_t> bytes =
        make_indexed_color_gpl(false, 0, false, 0x000008AC);
    bytes[display_list_offset + 3] = 0x01;
    bytes[display_list_offset + 4] = 0x00;
    ASSERT_TRUE(write_fixture(root, "indexed_position16_bad_ref.gpl", bytes));
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_position16_bad_ref.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 bad position", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);

    bytes = make_indexed_color_gpl(false, 0, false, 0x000008AC);
    put_be32(bytes, geometry_command_offset + 12, 17);
    ASSERT_TRUE(write_fixture(root, "indexed_position16_truncated.gpl", bytes));
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_position16_truncated.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 truncated", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_position16_color16_layout(const fs::path& root) {
    ASSERT_TRUE(write_fixture(
        root, "indexed_position16_color16.gpl",
        make_indexed_color_gpl(false, 0, true, 0x000008EC)));

    awl::GplFile gpl;
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_position16_color16.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 position and color", &mesh));
    ASSERT_TRUE(mesh.raw_refs.size() == 3);
    ASSERT_TRUE(mesh.raw_refs[2].pos_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].normal_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].color_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].uv_idx == 2);
    awl::gpl_free(&gpl);

    constexpr size_t section_offset = 24;
    constexpr size_t command_list_relative = 136;
    constexpr size_t geometry_command_offset =
        section_offset + command_list_relative;
    constexpr size_t display_list_offset =
        section_offset + command_list_relative + 16;

    std::vector<uint8_t> bytes =
        make_indexed_color_gpl(false, 0, false, 0x000008EC);
    bytes[display_list_offset + 6] = 0x01;
    bytes[display_list_offset + 7] = 0x00;
    ASSERT_TRUE(
        write_fixture(root, "indexed_color16_bad_ref.gpl", bytes));
    ASSERT_TRUE(awl::gpl_load_from_file("/indexed_color16_bad_ref.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 bad color", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    ASSERT_TRUE(mesh.colors_rgba8.empty());
    awl::gpl_free(&gpl);

    bytes = make_indexed_color_gpl(false, 0, false, 0x000008EC);
    put_be32(bytes, geometry_command_offset + 12, 20);
    ASSERT_TRUE(
        write_fixture(root, "indexed_color16_truncated.gpl", bytes));
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_color16_truncated.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 color truncated", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    ASSERT_TRUE(mesh.colors_rgba8.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_position16_color16_uv16_layout(const fs::path& root) {
    ASSERT_TRUE(write_fixture(
        root, "indexed_position16_color16_uv16.gpl",
        make_indexed_color_gpl(false, 0, true, 0x00000CEC)));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file(
        "/indexed_position16_color16_uv16.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 position, color, and UV", &mesh));
    ASSERT_TRUE(mesh.raw_refs.size() == 3);
    ASSERT_TRUE(mesh.raw_refs[2].pos_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].normal_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].color_idx == 2);
    ASSERT_TRUE(mesh.raw_refs[2].uv_idx == 2);
    awl::gpl_free(&gpl);

    constexpr size_t section_offset = 24;
    constexpr size_t command_list_relative = 136;
    constexpr size_t geometry_command_offset =
        section_offset + command_list_relative;
    constexpr size_t display_list_offset =
        section_offset + command_list_relative + 16;

    std::vector<uint8_t> bytes =
        make_indexed_color_gpl(false, 0, false, 0x00000CEC);
    bytes[display_list_offset + 8] = 0x01;
    bytes[display_list_offset + 9] = 0x00;
    ASSERT_TRUE(write_fixture(root, "indexed_uv16_bad_ref.gpl", bytes));
    ASSERT_TRUE(awl::gpl_load_from_file("/indexed_uv16_bad_ref.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 bad UV", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    ASSERT_TRUE(mesh.colors_rgba8.empty());
    awl::gpl_free(&gpl);

    bytes = make_indexed_color_gpl(false, 0, false, 0x00000CEC);
    put_be32(bytes, geometry_command_offset + 12, 23);
    ASSERT_TRUE(write_fixture(root, "indexed_uv16_truncated.gpl", bytes));
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_uv16_truncated.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "INDEX16 UV truncated", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    ASSERT_TRUE(mesh.colors_rgba8.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_rejects_bad_indexed_colors(const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "indexed_bad_ref.gpl",
                              make_indexed_color_gpl(false, 3)));
    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/indexed_bad_ref.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_FALSE(
        awl::gpl_parse_display_list_for_analysis(gpl, "bad color ref", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    ASSERT_TRUE(mesh.colors_rgba8.empty());
    awl::gpl_free(&gpl);

    std::vector<uint8_t> bytes = make_indexed_color_gpl(true);
    constexpr size_t color_header_offset = 24 + 64;
    put_be32(bytes, color_header_offset, 0xFFFFFFF0);
    ASSERT_TRUE(write_fixture(root, "indexed_bad_color_bounds.gpl", bytes));
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_bad_color_bounds.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "bad color bounds", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    ASSERT_TRUE(mesh.colors_rgba8.empty());
    awl::gpl_free(&gpl);

    bytes = make_indexed_color_gpl(false);
    bytes[color_header_offset + 6] = 0x50;
    bytes[color_header_offset + 7] = 4;
    ASSERT_TRUE(write_fixture(root, "indexed_unsupported_color.gpl", bytes));
    ASSERT_TRUE(
        awl::gpl_load_from_file("/indexed_unsupported_color.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "unsupported color", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_parses_complete_command_range(const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "multi_primitive.gpl",
                              make_multi_primitive_gpl()));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/multi_primitive.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.primitives.size() == 4);
    ASSERT_TRUE(mesh.raw_refs.size() == 13);
    ASSERT_TRUE(mesh.primitives[0].type == 0x90);
    ASSERT_TRUE(mesh.primitives[1].type == 0x80);
    ASSERT_TRUE(mesh.primitives[2].type == 0x98);
    ASSERT_TRUE(mesh.primitives[3].type == 0xA0);
    ASSERT_TRUE(mesh.primitives[3].first_ref == 10);
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_rejects_malformed_bounds(const fs::path& root) {
    std::vector<uint8_t> excessive_count(20, 0);
    put_be32(excessive_count, 0, 0x005BBC61);
    put_be32(excessive_count, 12, 0xFFFFFFFF);
    put_be32(excessive_count, 16, 0x14);
    ASSERT_TRUE(write_fixture(root, "gpl_bad_count.gpl", excessive_count));

    awl::GplFile gpl;
    ASSERT_FALSE(awl::gpl_load_from_file("/gpl_bad_count.gpl", &gpl));
    ASSERT_TRUE(gpl.raw_file_data == nullptr);

    ASSERT_TRUE(write_fixture(root, "gpl_bad_attr.gpl",
                              make_gpl(0x90, 0, 0xFFFFFFF0)));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_bad_attr.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_reference_and_layout_validation(const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "gpl_bad_ref.gpl", make_gpl(0x90, 3)));
    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_bad_ref.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);

    ASSERT_TRUE(write_fixture(root, "gpl_strip.gpl", make_gpl(0x98, 0)));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_strip.gpl", &gpl));
    ASSERT_TRUE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.primitives.size() == 1);
    ASSERT_TRUE(mesh.primitives[0].type == 0x98);
    awl::gpl_free(&gpl);

    ASSERT_TRUE(write_fixture(root, "gpl_lines.gpl", make_gpl(0xA8, 0)));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_lines.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);

    ASSERT_TRUE(write_fixture(root, "gpl_bad_vcd.gpl",
                              make_gpl(0x90, 0, 28, 0x00000028)));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_bad_vcd.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);

    std::vector<uint8_t> trailing_command = make_gpl(0x90, 0);
    constexpr size_t section_offset = 24;
    constexpr size_t geometry_command_offset = section_offset + 132;
    trailing_command.push_back(0x61);
    put_be32(trailing_command, geometry_command_offset + 12, 13);
    ASSERT_TRUE(write_fixture(root, "gpl_trailing_command.gpl",
                              trailing_command));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_trailing_command.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_texture_command(const fs::path& root) {
    constexpr uint32_t command_word = 0x1111A007;
    ASSERT_TRUE(write_fixture(
        root, "gpl_texture_command.gpl",
        make_gpl_with_texture_command(command_word)));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_texture_command.gpl", &gpl));
    std::vector<awl::GplTextureCommand> commands;
    ASSERT_TRUE(
        awl::gpl_parse_texture_commands_for_analysis(gpl, &commands));
    ASSERT_TRUE(commands.size() == 1);
    ASSERT_TRUE(commands[0].command_index == 0);
    ASSERT_TRUE(commands[0].image_index == 7);
    ASSERT_TRUE(commands[0].texture_unit == 5);
    ASSERT_TRUE(commands[0].state_selector == 0xFF);

    awl::GplMeshAnalysis mesh;
    ASSERT_TRUE(
        awl::gpl_parse_display_list_for_analysis(gpl, "synthetic", &mesh));
    awl::gpl_free(&gpl);

    ASSERT_TRUE(write_fixture(
        root, "gpl_non_texture_command.gpl",
        make_gpl_with_texture_command(command_word, 3)));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_non_texture_command.gpl", &gpl));
    ASSERT_TRUE(
        awl::gpl_parse_texture_commands_for_analysis(gpl, &commands));
    ASSERT_TRUE(commands.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_rejects_additional_command_draw_range(
    const fs::path& root) {
    constexpr size_t section_offset = 24;
    constexpr size_t texture_command_offset = section_offset + 132;
    constexpr uint32_t display_list_relative = 164;
    std::vector<uint8_t> bytes =
        make_gpl_with_texture_command(0x1111A007);
    put_be32(bytes, texture_command_offset + 8, display_list_relative);
    put_be32(bytes, texture_command_offset + 12, 12);
    ASSERT_TRUE(write_fixture(root, "gpl_multiple_draw_ranges.gpl", bytes));

    awl::GplFile gpl;
    ASSERT_TRUE(
        awl::gpl_load_from_file("/gpl_multiple_draw_ranges.gpl", &gpl));
    awl::GplMeshAnalysis mesh;
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "multiple draw ranges", &mesh));
    ASSERT_TRUE(mesh.raw_refs.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_ordered_draw_sequence(const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "gpl_ordered_draws.gpl",
                              make_ordered_multi_draw_gpl()));
    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_ordered_draws.gpl", &gpl));

    awl::GplDrawSequenceAnalysis sequence;
    ASSERT_TRUE(awl::gpl_parse_draw_sequence_for_analysis(
        gpl, "ordered synthetic", &sequence));
    ASSERT_TRUE(sequence.batches.size() == 3);
    ASSERT_TRUE(sequence.batches[0].command_index == 2);
    ASSERT_TRUE(sequence.batches[1].command_index == 3);
    ASSERT_TRUE(sequence.batches[2].command_index == 4);
    ASSERT_TRUE(sequence.batches[0].texture_image_index == 0);
    ASSERT_TRUE(sequence.batches[1].texture_image_index == 0);
    ASSERT_TRUE(sequence.batches[2].texture_image_index == 2);
    for (const auto& batch : sequence.batches) {
        ASSERT_TRUE(batch.vertex_descriptor == 0x00000828);
        ASSERT_TRUE(batch.mesh.primitives.size() == 1);
        ASSERT_TRUE(batch.mesh.raw_refs.size() == 3);
    }

    awl::GplMeshAnalysis single_mesh;
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "strict single range", &single_mesh));
    ASSERT_TRUE(single_mesh.raw_refs.empty());
    awl::gpl_free(&gpl);

    std::vector<uint8_t> bytes = make_ordered_multi_draw_gpl();
    constexpr size_t command_list_offset = 24 + 132;
    bytes[command_list_offset + 49] = 0;
    ASSERT_TRUE(write_fixture(root, "gpl_ordered_bad_texture_state.gpl",
                              bytes));
    ASSERT_TRUE(awl::gpl_load_from_file(
        "/gpl_ordered_bad_texture_state.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_draw_sequence_for_analysis(
        gpl, "bad ordered texture state", &sequence));
    ASSERT_TRUE(sequence.batches.empty());
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_analysis_rejects_multiple_sections(
    const fs::path& root) {
    ASSERT_TRUE(write_fixture(root, "gpl_multiple_sections.gpl",
                              make_multi_section_gpl()));
    awl::GplFile gpl;
    ASSERT_TRUE(
        awl::gpl_load_from_file("/gpl_multiple_sections.gpl", &gpl));
    ASSERT_TRUE(gpl.sections.size() == 2);

    std::vector<awl::GplTextureCommand> commands(1);
    ASSERT_FALSE(
        awl::gpl_parse_texture_commands_for_analysis(gpl, &commands));
    ASSERT_TRUE(commands.empty());

    awl::GplTargetMaterial material;
    material.tev_mode_word = 99;
    ASSERT_FALSE(awl::gpl_parse_target_material_for_analysis(gpl, &material));
    ASSERT_TRUE(material.tev_mode_word == 0);

    awl::GplMeshAnalysis mesh;
    mesh.indices.push_back(1);
    ASSERT_FALSE(awl::gpl_parse_display_list_for_analysis(
        gpl, "multi-section synthetic", &mesh));
    ASSERT_TRUE(mesh.indices.empty());

    awl::GplDrawSequenceAnalysis sequence;
    sequence.batches.emplace_back();
    ASSERT_FALSE(awl::gpl_parse_draw_sequence_for_analysis(
        gpl, "multi-section synthetic", &sequence));
    ASSERT_TRUE(sequence.batches.empty());
    ASSERT_FALSE(awl::gpl_analyze_payload(gpl));

    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_texture_command_rejects_bad_bounds(
    const fs::path& root) {
    constexpr size_t section_offset = 24;
    constexpr size_t material_header_offset = section_offset + 120;
    std::vector<uint8_t> bytes =
        make_gpl_with_texture_command(0x11110002);
    put_be32(bytes, material_header_offset + 4, 0xFFFFFFF0);
    ASSERT_TRUE(
        write_fixture(root, "gpl_bad_command_bounds.gpl", bytes));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file(
        "/gpl_bad_command_bounds.gpl", &gpl));
    std::vector<awl::GplTextureCommand> commands(1);
    ASSERT_FALSE(
        awl::gpl_parse_texture_commands_for_analysis(gpl, &commands));
    ASSERT_TRUE(commands.empty());
    ASSERT_FALSE(
        awl::gpl_parse_texture_commands_for_analysis(gpl, nullptr));
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_target_material(const fs::path& root) {
    constexpr size_t section_offset = 24;
    constexpr size_t color_data_offset = section_offset + 72;
    std::vector<uint8_t> bytes =
        make_gpl_with_texture_command(0x11110002, 1, true, 1);
    bytes[color_data_offset] = 0xB6;
    bytes[color_data_offset + 1] = 0x59;
    ASSERT_TRUE(write_fixture(root, "gpl_target_material.gpl", bytes));

    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_target_material.gpl", &gpl));
    awl::GplTargetMaterial material;
    ASSERT_TRUE(awl::gpl_parse_target_material_for_analysis(gpl, &material));
    ASSERT_TRUE(material.command_index == 1);
    ASSERT_TRUE(material.tev_mode_word == 1);
    ASSERT_TRUE(material.red == 181);
    ASSERT_TRUE(material.green == 203);
    ASSERT_TRUE(material.blue == 206);
    ASSERT_TRUE(material.alpha == 255);
    ASSERT_FALSE(material.uses_vertex_color);
    ASSERT_FALSE(awl::gpl_parse_target_material_for_analysis(gpl, nullptr));
    awl::gpl_free(&gpl);
    return true;
}

static bool test_gpl_target_material_rejects_unverified_state(
    const fs::path& root) {
    constexpr size_t section_offset = 24;
    constexpr size_t normal_descriptor_offset = section_offset + 108;
    constexpr size_t material_header_offset = section_offset + 120;
    constexpr size_t command_list_offset = section_offset + 132;

    std::vector<uint8_t> bytes =
        make_gpl_with_texture_command(0x11110002, 1, true, 2);
    ASSERT_TRUE(write_fixture(root, "gpl_bad_tev_word.gpl", bytes));
    awl::GplFile gpl;
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_bad_tev_word.gpl", &gpl));
    awl::GplTargetMaterial material;
    ASSERT_FALSE(awl::gpl_parse_target_material_for_analysis(gpl, &material));
    ASSERT_TRUE(material.tev_mode_word == 0);
    awl::gpl_free(&gpl);

    bytes = make_gpl_with_texture_command(0x11110002, 1, true, 1);
    put_be32(bytes, normal_descriptor_offset, 1);
    ASSERT_TRUE(write_fixture(root, "gpl_lit_material.gpl", bytes));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_lit_material.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_target_material_for_analysis(gpl, &material));
    awl::gpl_free(&gpl);

    bytes = make_gpl_with_texture_command(0x11110002, 1, true, 1);
    put_be16(bytes, material_header_offset + 8, 4);
    bytes.resize(bytes.size() + 16, 0);
    bytes[command_list_offset + 48] = 3;
    put_be32(bytes, command_list_offset + 52, 1);
    ASSERT_TRUE(write_fixture(root, "gpl_duplicate_tev.gpl", bytes));
    ASSERT_TRUE(awl::gpl_load_from_file("/gpl_duplicate_tev.gpl", &gpl));
    ASSERT_FALSE(awl::gpl_parse_target_material_for_analysis(gpl, &material));
    awl::gpl_free(&gpl);
    return true;
}

int main() {
    const auto unique_value =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path fixture_root =
        fs::temp_directory_path() / ("awl-asset-parser-tests-" +
                                     std::to_string(unique_value));

    std::error_code error;
    fs::create_directories(fixture_root, error);
    if (error) {
        std::cerr << "Unable to create fixture directory: " << error.message() << std::endl;
        return 1;
    }

    awl_memory_init();
    awl::filesystem_init();
    const std::string native_root = fixture_root.string();
    if (!awl::filesystem_mount("/", native_root.c_str())) {
        std::cerr << "Unable to mount fixture directory" << std::endl;
        awl::filesystem_shutdown();
        awl_memory_shutdown();
        fs::remove_all(fixture_root, error);
        return 1;
    }

    int failures = 0;
    const auto run = [&failures](const char* name, bool (*test)(const fs::path&),
                                 const fs::path& root) {
        std::cout << "[RUNNING] " << name << std::endl;
        if (!test(root)) {
            std::cout << "[FAILED] " << name << std::endl;
            ++failures;
        } else {
            std::cout << "[PASSED] " << name << std::endl;
        }
    };

    run("test_valid_rgb5a3", test_valid_rgb5a3, fixture_root);
    run("test_i4_intensity_populates_rgba", test_i4_intensity_populates_rgba,
        fixture_root);
    run("test_tiled_i8", test_tiled_i8, fixture_root);
    run("test_planar_rgba8", test_planar_rgba8, fixture_root);
    run("test_cmpr_block_layout_and_alpha", test_cmpr_block_layout_and_alpha,
        fixture_root);
    run("test_tpl_decodes_complete_mip_chain",
        test_tpl_decodes_complete_mip_chain, fixture_root);
    run("test_tpl_rejects_truncated_or_impossible_mip_chain",
        test_tpl_rejects_truncated_or_impossible_mip_chain, fixture_root);
    run("test_tpl_rejects_malformed_bounds", test_tpl_rejects_malformed_bounds,
        fixture_root);
    run("test_valid_gpl_triangle", test_valid_gpl_triangle, fixture_root);
    run("test_gpl_indexed_color_layouts", test_gpl_indexed_color_layouts,
        fixture_root);
    run("test_gpl_position16_indexed_color_layout",
        test_gpl_position16_indexed_color_layout, fixture_root);
    run("test_gpl_position16_color16_layout",
        test_gpl_position16_color16_layout, fixture_root);
    run("test_gpl_position16_color16_uv16_layout",
        test_gpl_position16_color16_uv16_layout, fixture_root);
    run("test_gpl_rejects_bad_indexed_colors",
        test_gpl_rejects_bad_indexed_colors, fixture_root);
    run("test_gpl_parses_complete_command_range",
        test_gpl_parses_complete_command_range, fixture_root);
    run("test_gpl_rejects_malformed_bounds", test_gpl_rejects_malformed_bounds,
        fixture_root);
    run("test_gpl_reference_and_layout_validation",
        test_gpl_reference_and_layout_validation, fixture_root);
    run("test_gpl_texture_command", test_gpl_texture_command, fixture_root);
    run("test_gpl_rejects_additional_command_draw_range",
        test_gpl_rejects_additional_command_draw_range, fixture_root);
    run("test_gpl_ordered_draw_sequence", test_gpl_ordered_draw_sequence,
        fixture_root);
    run("test_gpl_analysis_rejects_multiple_sections",
        test_gpl_analysis_rejects_multiple_sections, fixture_root);
    run("test_gpl_texture_command_rejects_bad_bounds",
        test_gpl_texture_command_rejects_bad_bounds, fixture_root);
    run("test_gpl_target_material", test_gpl_target_material, fixture_root);
    run("test_gpl_target_material_rejects_unverified_state",
        test_gpl_target_material_rejects_unverified_state, fixture_root);

    awl::filesystem_shutdown();
    awl_memory_shutdown();
    fs::remove_all(fixture_root, error);
    if (error) {
        std::cerr << "Unable to remove fixture directory: " << error.message() << std::endl;
        ++failures;
    }

    if (failures != 0) {
        std::cerr << "[TEST RESULT] " << failures << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "[TEST RESULT] All asset parser tests passed" << std::endl;
    return 0;
}
