#include "awl/gpl.h"
#include "awl/filesystem.h"
#include "awl/platform.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace awl {
namespace {

constexpr uint32_t kGplMagic = 0x005BBC61;
constexpr uint32_t kMaxSectionCount = 65536;
constexpr size_t kGplBaseHeaderSize = 20;
constexpr size_t kGplSectionHeaderSize = 20;
constexpr size_t kGplSubOffsetCount = 5;
constexpr size_t kGplMaterialHeaderSize = 12;
constexpr size_t kGplMaterialCommandSize = 16;
constexpr uint8_t kGplTextureCommandType = 1;
constexpr uint8_t kGplGeometryCommandType = 2;
constexpr uint8_t kGplTevCommandType = 3;

constexpr uint8_t kGxDrawQuads = 0x80;
constexpr uint8_t kGxDrawQuads2 = 0x88;
constexpr uint8_t kGxDrawTriangles = 0x90;
constexpr uint8_t kGxDrawTriangleStrip = 0x98;
constexpr uint8_t kGxDrawTriangleFan = 0xA0;
constexpr uint8_t kGxPrimitiveTypeMask = 0xF8;
constexpr uint8_t kGxVatMask = 0x07;

// GYWE41 FUN_801A5950 decodes the type-2 command word as two-bit GX
// component modes. 0x828 is Position INDEX8, Normal INDEX8, Tex0 INDEX8;
// 0x8A8 adds Color0 INDEX8 between Normal and Tex0. 0x8AC changes Position
// to INDEX16 while retaining INDEX8 for the remaining attributes. 0x8EC also
// changes Color0 to INDEX16, and 0xCEC additionally changes Tex0 to INDEX16.
constexpr uint32_t kConstantColorVertexDescriptor = 0x00000828;
constexpr uint32_t kIndexedColorVertexDescriptor = 0x000008A8;
constexpr uint32_t kIndexedColorPosition16VertexDescriptor = 0x000008AC;
constexpr uint32_t kIndexedColor16Position16VertexDescriptor = 0x000008EC;
constexpr uint32_t kIndexedColor16Position16Uv16VertexDescriptor = 0x00000CEC;

struct GplVertexLayout {
    bool has_indexed_color = false;
    bool position_index_16 = false;
    bool color_index_16 = false;
    bool uv_index_16 = false;
    size_t reference_size = 0;
    const char* description = nullptr;
};

bool supported_vertex_layout(uint32_t vertex_descriptor,
                             GplVertexLayout* out_layout) {
    if (!out_layout) {
        return false;
    }

    GplVertexLayout layout;
    switch (vertex_descriptor) {
        case kConstantColorVertexDescriptor:
            layout.reference_size = 3;
            layout.description = "position/normal/UV";
            break;
        case kIndexedColorVertexDescriptor:
            layout.has_indexed_color = true;
            layout.reference_size = 4;
            layout.description = "position/normal/COLOR0/UV";
            break;
        case kIndexedColorPosition16VertexDescriptor:
            layout.has_indexed_color = true;
            layout.position_index_16 = true;
            layout.reference_size = 5;
            layout.description = "position16/normal/COLOR0/UV";
            break;
        case kIndexedColor16Position16VertexDescriptor:
            layout.has_indexed_color = true;
            layout.position_index_16 = true;
            layout.color_index_16 = true;
            layout.reference_size = 6;
            layout.description = "position16/normal/COLOR016/UV";
            break;
        case kIndexedColor16Position16Uv16VertexDescriptor:
            layout.has_indexed_color = true;
            layout.position_index_16 = true;
            layout.color_index_16 = true;
            layout.uv_index_16 = true;
            layout.reference_size = 7;
            layout.description = "position16/normal/COLOR016/UV16";
            break;
        default:
            return false;
    }

    *out_layout = layout;
    return true;
}

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

float read_be32f(const uint8_t* data) {
    const uint32_t value = read_be32(data);
    float result = 0.0f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

bool checked_range(size_t offset, size_t length, size_t total_size) {
    return offset <= total_size && length <= total_size - offset;
}

bool checked_multiply(size_t lhs, size_t rhs, size_t* result) {
    if (!result) {
        return false;
    }
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        return false;
    }
    *result = lhs * rhs;
    return true;
}

bool section_range_valid(const GplFile& gpl, const GplSection& section) {
    return checked_range(section.offset, section.raw_size, gpl.raw_file_size);
}

bool supports_single_section_analysis(const GplFile& gpl,
                                      const char* operation) {
    if (!gpl.raw_file_data || gpl.header.section_count != 1 ||
        gpl.sections.size() != 1) {
        AWL_LOG_ERROR(
            "GPL: %s requires exactly one section; complete multi-section assets are not yet supported",
            operation ? operation : "analysis");
        return false;
    }
    return true;
}

struct GplGeometryCommand {
    uint32_t command_index = 0;
    uint32_t vertex_descriptor = 0;
    uint32_t display_list_relative = 0;
    uint32_t display_list_size = 0;
};

bool find_geometry_command(const GplFile& gpl, const GplSection& section,
                           GplGeometryCommand* out_command) {
    if (!out_command || !gpl.raw_file_data ||
        !section_range_valid(gpl, section) ||
        section.sub_offsets.size() < kGplSubOffsetCount ||
        section.sub_offsets[4] == 0 ||
        !checked_range(section.sub_offsets[4], kGplMaterialHeaderSize,
                       section.raw_size)) {
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    const size_t material_header_offset =
        static_cast<size_t>(section.offset) + section.sub_offsets[4];
    if (!checked_range(material_header_offset, kGplMaterialHeaderSize,
                       gpl.raw_file_size)) {
        return false;
    }

    const uint32_t header_display_list_relative =
        read_be32(raw + material_header_offset);
    const uint32_t command_list_relative =
        read_be32(raw + material_header_offset + 4);
    const uint16_t command_count = read_be16(raw + material_header_offset + 8);
    if (command_count == 0 || command_list_relative == 0) {
        return false;
    }

    size_t command_bytes = 0;
    if (!checked_multiply(command_count, kGplMaterialCommandSize,
                          &command_bytes) ||
        !checked_range(command_list_relative, command_bytes,
                       section.raw_size)) {
        return false;
    }
    const size_t command_list_offset =
        static_cast<size_t>(section.offset) + command_list_relative;
    if (!checked_range(command_list_offset, command_bytes,
                       gpl.raw_file_size)) {
        return false;
    }

    bool found = false;
    GplGeometryCommand command;
    for (uint32_t command_index = 0; command_index < command_count;
         ++command_index) {
        const size_t command_offset =
            command_list_offset +
            static_cast<size_t>(command_index) * kGplMaterialCommandSize;
        const uint8_t command_type = raw[command_offset];
        const uint32_t attached_display_list_relative =
            read_be32(raw + command_offset + 8);
        const uint32_t attached_display_list_size =
            read_be32(raw + command_offset + 12);
        if (command_type != kGplGeometryCommandType) {
            // GYWE41 FUN_801A3000 may draw the range attached to type 1, 3,
            // and 0x80 commands after applying their state. Until the native
            // path preserves that full command order, accepting one range
            // would silently render only part of the object.
            if (attached_display_list_relative != 0 ||
                attached_display_list_size != 0) {
                AWL_LOG_ERROR(
                    "GPL: Command %u type 0x%02X has an unsupported attached display-list range",
                    command_index, command_type);
                return false;
            }
            continue;
        }
        if (found) {
            return false;
        }

        command.command_index = command_index;
        command.vertex_descriptor = read_be32(raw + command_offset + 4);
        command.display_list_relative = attached_display_list_relative;
        command.display_list_size = attached_display_list_size;
        found = true;
    }

    if (!found || command.display_list_relative == 0 ||
        command.display_list_size == 0 ||
        header_display_list_relative != command.display_list_relative ||
        !checked_range(command.display_list_relative,
                       command.display_list_size, section.raw_size)) {
        return false;
    }

    const size_t display_list_offset =
        static_cast<size_t>(section.offset) + command.display_list_relative;
    if (!checked_range(display_list_offset, command.display_list_size,
                       gpl.raw_file_size)) {
        return false;
    }

    *out_command = command;
    return true;
}

bool display_list_range(const GplFile& gpl, const GplSection& section,
                         size_t* out_start, size_t* out_end,
                         uint32_t* out_vertex_descriptor = nullptr,
                         uint32_t* out_command_index = nullptr) {
    if (!out_start || !out_end) {
        return false;
    }

    GplGeometryCommand command;
    if (!find_geometry_command(gpl, section, &command)) {
        return false;
    }

    *out_start = static_cast<size_t>(section.offset) +
                 command.display_list_relative;
    *out_end = *out_start + command.display_list_size;
    if (out_vertex_descriptor) {
        *out_vertex_descriptor = command.vertex_descriptor;
    }
    if (out_command_index) {
        *out_command_index = command.command_index;
    }
    return true;
}

size_t component_size(GXCompType type) {
    switch (type) {
        case GXCompType::U8:
        case GXCompType::S8:
            return 1;
        case GXCompType::U16:
        case GXCompType::S16:
            return 2;
        case GXCompType::F32:
            return 4;
        default:
            return 0;
    }
}

bool parse_attribute_header(const GplFile& gpl, const GplSection& section,
                            uint32_t relative_offset, GplAttrHeader* out_header) {
    if (!out_header || relative_offset == 0 || !section_range_valid(gpl, section) ||
        !checked_range(relative_offset, 8, section.raw_size)) {
        return false;
    }

    const size_t absolute_offset =
        static_cast<size_t>(section.offset) + relative_offset;
    if (!checked_range(absolute_offset, 8, gpl.raw_file_size)) {
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    out_header->data_offset = read_be32(raw + absolute_offset);
    out_header->element_count = read_be16(raw + absolute_offset + 4);
    out_header->type_and_frac = raw[absolute_offset + 6];
    out_header->comp_count = raw[absolute_offset + 7];
    return true;
}

bool attribute_data_range(const GplFile& gpl, const GplSection& section,
                          const GplAttrHeader& header, size_t* stride,
                          size_t* data_size) {
    if (!stride || !data_size || header.element_count == 0 ||
        header.comp_count == 0 || !section_range_valid(gpl, section)) {
        return false;
    }

    const GXCompType type = static_cast<GXCompType>(header.type_and_frac >> 4);
    const size_t scalar_size = component_size(type);
    if (scalar_size == 0 ||
        !checked_multiply(header.comp_count, scalar_size, stride) ||
        !checked_multiply(header.element_count, *stride, data_size)) {
        return false;
    }

    if (!checked_range(header.data_offset, *data_size, section.raw_size)) {
        return false;
    }
    const size_t absolute_offset =
        static_cast<size_t>(section.offset) + header.data_offset;
    return checked_range(absolute_offset, *data_size, gpl.raw_file_size);
}

bool extract_attribute(const GplFile& gpl, const GplSection& section,
                       const GplAttrHeader& header, uint8_t required_components,
                       const char* name, std::vector<float>* output) {
    if (!output || header.comp_count != required_components) {
        AWL_LOG_ERROR("GPL: %s attribute has %u components; expected %u", name,
                      header.comp_count, required_components);
        return false;
    }

    size_t stride = 0;
    size_t data_size = 0;
    if (!attribute_data_range(gpl, section, header, &stride, &data_size)) {
        AWL_LOG_ERROR("GPL: %s attribute data is invalid or out of bounds", name);
        return false;
    }

    size_t value_count = 0;
    if (!checked_multiply(header.element_count, header.comp_count, &value_count)) {
        return false;
    }

    output->clear();
    try {
        output->reserve(value_count);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("GPL: Unable to allocate %s attribute output", name);
        return false;
    } catch (const std::length_error&) {
        AWL_LOG_ERROR("GPL: Invalid %s attribute output size", name);
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    const size_t data_offset =
        static_cast<size_t>(section.offset) + header.data_offset;
    const GXCompType type = static_cast<GXCompType>(header.type_and_frac >> 4);
    const uint8_t fractional_bits = header.type_and_frac & 0x0F;
    const float scale = 1.0f / static_cast<float>(1u << fractional_bits);

    for (uint32_t element = 0; element < header.element_count; ++element) {
        const size_t element_offset = data_offset + static_cast<size_t>(element) * stride;
        for (uint32_t component = 0; component < header.comp_count; ++component) {
            float value = 0.0f;
            if (type == GXCompType::F32) {
                value = read_be32f(raw + element_offset + component * 4);
            } else if (type == GXCompType::S16) {
                const int16_t raw_value =
                    static_cast<int16_t>(read_be16(raw + element_offset + component * 2));
                value = static_cast<float>(raw_value) * scale;
            } else if (type == GXCompType::U16) {
                value = static_cast<float>(
                            read_be16(raw + element_offset + component * 2)) *
                        scale;
            } else if (type == GXCompType::S8) {
                value = static_cast<float>(
                            static_cast<int8_t>(raw[element_offset + component])) *
                        scale;
            } else if (type == GXCompType::U8) {
                value = static_cast<float>(raw[element_offset + component]) * scale;
            } else {
                return false;
            }
            output->push_back(value);
        }
    }

    AWL_LOG_INFO("GPL: Decoded %u %s elements", header.element_count, name);
    return true;
}

bool color_attribute_stride(const GplAttrHeader& header, size_t* out_stride) {
    if (!out_stride || header.element_count == 0) {
        return false;
    }

    const uint8_t format = header.type_and_frac >> 4;
    if (format == 0 && header.comp_count == 3) {
        *out_stride = 2; // RGB565
        return true;
    }
    if (format == 3 && header.comp_count == 4) {
        *out_stride = 2; // RGBA4
        return true;
    }
    return false;
}

bool extract_color_attribute(const GplFile& gpl, const GplSection& section,
                             const GplAttrHeader& header,
                             std::vector<uint8_t>* output) {
    if (!output || !section_range_valid(gpl, section)) {
        return false;
    }
    output->clear();

    size_t stride = 0;
    size_t data_size = 0;
    size_t output_size = 0;
    if (!color_attribute_stride(header, &stride) ||
        !checked_multiply(header.element_count, stride, &data_size) ||
        !checked_multiply(header.element_count, size_t{4}, &output_size) ||
        !checked_range(header.data_offset, data_size, section.raw_size)) {
        AWL_LOG_ERROR("GPL: Unsupported or out-of-bounds Sub[1] color array");
        return false;
    }

    const size_t data_offset =
        static_cast<size_t>(section.offset) + header.data_offset;
    if (!checked_range(data_offset, data_size, gpl.raw_file_size)) {
        return false;
    }

    try {
        output->resize(output_size);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("GPL: Unable to allocate decoded color output");
        return false;
    } catch (const std::length_error&) {
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    const uint8_t format = header.type_and_frac >> 4;
    for (size_t element = 0; element < header.element_count; ++element) {
        const uint16_t packed =
            read_be16(raw + data_offset + element * stride);
        const size_t destination = element * 4;
        if (format == 0) {
            const uint8_t red5 = static_cast<uint8_t>((packed >> 11) & 0x1Fu);
            const uint8_t green6 = static_cast<uint8_t>((packed >> 5) & 0x3Fu);
            const uint8_t blue5 = static_cast<uint8_t>(packed & 0x1Fu);
            (*output)[destination] =
                static_cast<uint8_t>((red5 << 3) | (red5 >> 2));
            (*output)[destination + 1] =
                static_cast<uint8_t>((green6 << 2) | (green6 >> 4));
            (*output)[destination + 2] =
                static_cast<uint8_t>((blue5 << 3) | (blue5 >> 2));
            (*output)[destination + 3] = 0xFF;
        } else {
            const uint8_t red4 = static_cast<uint8_t>((packed >> 12) & 0x0Fu);
            const uint8_t green4 = static_cast<uint8_t>((packed >> 8) & 0x0Fu);
            const uint8_t blue4 = static_cast<uint8_t>((packed >> 4) & 0x0Fu);
            const uint8_t alpha4 = static_cast<uint8_t>(packed & 0x0Fu);
            (*output)[destination] =
                static_cast<uint8_t>((red4 << 4) | red4);
            (*output)[destination + 1] =
                static_cast<uint8_t>((green4 << 4) | green4);
            (*output)[destination + 2] =
                static_cast<uint8_t>((blue4 << 4) | blue4);
            (*output)[destination + 3] =
                static_cast<uint8_t>((alpha4 << 4) | alpha4);
        }
    }

    AWL_LOG_INFO("GPL: Decoded %u Sub[1] color elements as RGBA8",
                 header.element_count);
    return true;
}

void clear_mesh(GplMeshAnalysis* mesh) {
    if (mesh) {
        *mesh = {};
    }
}

} // namespace

GplFile::~GplFile() {
    gpl_free(this);
}

GplFile::GplFile(GplFile&& other) noexcept
    : raw_file_data(other.raw_file_data),
      raw_file_size(other.raw_file_size),
      header(std::move(other.header)),
      sections(std::move(other.sections)) {
    other.raw_file_data = nullptr;
    other.raw_file_size = 0;
    other.header = {};
    other.sections.clear();
}

GplFile& GplFile::operator=(GplFile&& other) noexcept {
    if (this != &other) {
        gpl_free(this);
        raw_file_data = other.raw_file_data;
        raw_file_size = other.raw_file_size;
        header = std::move(other.header);
        sections = std::move(other.sections);
        other.raw_file_data = nullptr;
        other.raw_file_size = 0;
        other.header = {};
        other.sections.clear();
    }
    return *this;
}

bool gpl_load_from_file(const char* logical_path, GplFile* out_gpl) {
    if (!logical_path || !out_gpl) {
        return false;
    }

    gpl_free(out_gpl);

    void* file_data = nullptr;
    size_t file_size = 0;
    if (!filesystem_read_entire_file(logical_path, &file_data, &file_size)) {
        AWL_LOG_ERROR("GPL: Failed to read file %s", logical_path);
        return false;
    }

    GplFile parsed;
    parsed.raw_file_data = file_data;
    parsed.raw_file_size = file_size;
    const auto fail = [&parsed]() {
        gpl_free(&parsed);
        return false;
    };

    if (!checked_range(0, kGplBaseHeaderSize, file_size)) {
        AWL_LOG_ERROR("GPL: File %s too small (size %zu)", logical_path, file_size);
        return fail();
    }

    const uint8_t* buffer = static_cast<const uint8_t*>(file_data);
    parsed.header.magic = read_be32(buffer + 0);
    parsed.header.unknown1 = read_be32(buffer + 4);
    parsed.header.unknown2 = read_be32(buffer + 8);
    parsed.header.section_count = read_be32(buffer + 12);
    parsed.header.constant = read_be32(buffer + 16);

    if (parsed.header.magic != kGplMagic) {
        AWL_LOG_ERROR("GPL: Invalid magic number 0x%08X in %s", parsed.header.magic,
                      logical_path);
        return fail();
    }
    if (parsed.header.section_count == 0 ||
        parsed.header.section_count > kMaxSectionCount) {
        AWL_LOG_ERROR("GPL: Section count %u is invalid in %s",
                      parsed.header.section_count, logical_path);
        return fail();
    }

    constexpr size_t section_offset_size = sizeof(uint32_t);
    if (parsed.header.section_count >
        (file_size - kGplBaseHeaderSize) / section_offset_size) {
        AWL_LOG_ERROR("GPL: Section offset table is out of bounds in %s", logical_path);
        return fail();
    }

    try {
        parsed.header.section_offsets.resize(parsed.header.section_count);
        parsed.sections.resize(parsed.header.section_count);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("GPL: Unable to allocate section metadata for %s", logical_path);
        return fail();
    } catch (const std::length_error&) {
        AWL_LOG_ERROR("GPL: Invalid section count in %s", logical_path);
        return fail();
    }

    for (uint32_t i = 0; i < parsed.header.section_count; ++i) {
        const size_t table_entry =
            kGplBaseHeaderSize + static_cast<size_t>(i) * section_offset_size;
        parsed.header.section_offsets[i] = read_be32(buffer + table_entry);
    }

    for (uint32_t i = 0; i < parsed.header.section_count; ++i) {
        const uint32_t section_offset = parsed.header.section_offsets[i];
        if (!checked_range(section_offset, kGplSectionHeaderSize, file_size)) {
            AWL_LOG_ERROR("GPL: Section %u header is out of bounds", i);
            return fail();
        }

        size_t section_end = file_size;
        for (uint32_t j = 0; j < parsed.header.section_count; ++j) {
            const size_t candidate = parsed.header.section_offsets[j];
            if (candidate > section_offset && candidate < section_end) {
                section_end = candidate;
            }
        }
        const size_t section_size = section_end - section_offset;
        if (section_size > std::numeric_limits<uint32_t>::max()) {
            AWL_LOG_ERROR("GPL: Section %u is too large", i);
            return fail();
        }

        GplSection& section = parsed.sections[i];
        section.offset = section_offset;
        section.raw_size = static_cast<uint32_t>(section_size);
        section.raw_data = buffer + section_offset;
        section.sub_offsets.resize(kGplSubOffsetCount);
        for (size_t j = 0; j < kGplSubOffsetCount; ++j) {
            section.sub_offsets[j] =
                read_be32(buffer + section_offset + j * sizeof(uint32_t));
            if (section.sub_offsets[j] != 0 &&
                section.sub_offsets[j] >= section.raw_size) {
                AWL_LOG_ERROR("GPL: Section %u sub-offset %zu is out of bounds", i, j);
                return fail();
            }
        }
    }

    *out_gpl = std::move(parsed);
    parsed.raw_file_data = nullptr;
    parsed.raw_file_size = 0;
    return true;
}

void gpl_free(GplFile* gpl) {
    if (!gpl) {
        return;
    }
    if (gpl->raw_file_data) {
        filesystem_free_file_data(gpl->raw_file_data);
    }
    gpl->raw_file_data = nullptr;
    gpl->raw_file_size = 0;
    gpl->header = {};
    gpl->sections.clear();
}

bool gpl_dump_metadata(const GplFile& gpl) {
    if (!gpl.raw_file_data) {
        return false;
    }

    AWL_LOG_INFO("--- GPL Metadata Dump ---");
    AWL_LOG_INFO("  File size: %zu bytes", gpl.raw_file_size);
    AWL_LOG_INFO("  Magic: 0x%08X", gpl.header.magic);
    AWL_LOG_INFO("  Unknown 1: 0x%08X", gpl.header.unknown1);
    AWL_LOG_INFO("  Unknown 2: 0x%08X", gpl.header.unknown2);
    AWL_LOG_INFO("  Section Count: %u", gpl.header.section_count);
    AWL_LOG_INFO("  Constant: 0x%08X", gpl.header.constant);

    for (size_t i = 0; i < gpl.sections.size(); ++i) {
        const GplSection& section = gpl.sections[i];
        AWL_LOG_INFO("  Section %zu: offset=0x%08X size=%u", i, section.offset,
                     section.raw_size);
        for (size_t j = 0; j < section.sub_offsets.size(); ++j) {
            AWL_LOG_INFO("    Sub[%zu]: relative=0x%08X", j, section.sub_offsets[j]);
        }
    }
    AWL_LOG_INFO("-------------------------");
    return true;
}

bool gpl_analyze_payload(const GplFile& gpl) {
    if (!supports_single_section_analysis(gpl, "payload analysis")) {
        return false;
    }

    const GplSection& section = gpl.sections[0];
    size_t display_start = 0;
    size_t display_end = 0;
    uint32_t vertex_descriptor = 0;
    uint32_t command_index = 0;
    if (!display_list_range(gpl, section, &display_start, &display_end,
                            &vertex_descriptor, &command_index)) {
        return false;
    }

    AWL_LOG_INFO("--- GPL geometry command ---");
    AWL_LOG_INFO("  Command index: %u", command_index);
    AWL_LOG_INFO("  VCD word: 0x%08X", vertex_descriptor);
    AWL_LOG_INFO("  Display-list file range: 0x%zX..0x%zX (%zu bytes)",
                 display_start, display_end, display_end - display_start);
    return true;
}

bool gpl_parse_texture_commands_for_analysis(
    const GplFile& gpl, std::vector<GplTextureCommand>* out_commands) {
    if (!out_commands) {
        return false;
    }
    out_commands->clear();

    if (!supports_single_section_analysis(gpl, "texture-command analysis")) {
        return false;
    }

    const GplSection& section = gpl.sections[0];
    if (!section_range_valid(gpl, section) ||
        section.sub_offsets.size() < kGplSubOffsetCount ||
        section.sub_offsets[4] == 0 ||
        !checked_range(section.sub_offsets[4], kGplMaterialHeaderSize,
                       section.raw_size)) {
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    const size_t material_header_offset =
        static_cast<size_t>(section.offset) + section.sub_offsets[4];
    if (!checked_range(material_header_offset, kGplMaterialHeaderSize,
                       gpl.raw_file_size)) {
        return false;
    }

    const uint32_t command_list_relative =
        read_be32(raw + material_header_offset + 4);
    const uint16_t command_count =
        read_be16(raw + material_header_offset + 8);
    if (command_count == 0) {
        return command_list_relative == 0;
    }
    if (command_list_relative == 0) {
        return false;
    }

    size_t command_bytes = 0;
    if (!checked_multiply(command_count, kGplMaterialCommandSize,
                          &command_bytes) ||
        !checked_range(command_list_relative, command_bytes,
                       section.raw_size)) {
        return false;
    }
    const size_t command_list_offset =
        static_cast<size_t>(section.offset) + command_list_relative;
    if (!checked_range(command_list_offset, command_bytes,
                       gpl.raw_file_size)) {
        return false;
    }

    try {
        out_commands->reserve(command_count);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("GPL: Unable to allocate texture-command output");
        return false;
    } catch (const std::length_error&) {
        return false;
    }

    for (uint32_t command_index = 0; command_index < command_count;
         ++command_index) {
        const size_t command_offset =
            command_list_offset +
            static_cast<size_t>(command_index) * kGplMaterialCommandSize;
        if (raw[command_offset] != kGplTextureCommandType) {
            continue;
        }

        const uint32_t command_word = read_be32(raw + command_offset + 4);
        GplTextureCommand command;
        command.command_index = command_index;
        command.image_index = command_word & 0x1FFFu;
        command.state_selector = raw[command_offset + 1];
        command.texture_unit =
            static_cast<uint8_t>((command_word >> 13) & 0x7u);
        out_commands->push_back(command);
    }
    return true;
}

bool gpl_parse_target_material_for_analysis(
    const GplFile& gpl, GplTargetMaterial* out_material) {
    if (!out_material) {
        return false;
    }
    *out_material = {};

    if (!supports_single_section_analysis(gpl, "material analysis")) {
        return false;
    }

    const GplSection& section = gpl.sections[0];
    if (!section_range_valid(gpl, section) ||
        section.sub_offsets.size() < kGplSubOffsetCount ||
        section.sub_offsets[1] == 0 || section.sub_offsets[3] == 0 ||
        section.sub_offsets[4] == 0) {
        return false;
    }

    GplAttrHeader color_header;
    std::vector<uint8_t> decoded_colors;
    if (!parse_attribute_header(gpl, section, section.sub_offsets[1],
                                &color_header) ||
        !extract_color_attribute(gpl, section, color_header, &decoded_colors)) {
        AWL_LOG_ERROR(
            "GPL: Ground material requires a supported Sub[1] color array");
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    const size_t normal_descriptor_offset =
        static_cast<size_t>(section.offset) + section.sub_offsets[3];
    if (!checked_range(section.sub_offsets[3], sizeof(uint32_t),
                       section.raw_size) ||
        !checked_range(normal_descriptor_offset, sizeof(uint32_t),
                       gpl.raw_file_size) ||
        read_be32(raw + normal_descriptor_offset) != 0) {
        AWL_LOG_ERROR(
            "GPL: Target material requires lighting disabled by a null Sub[3] normal array");
        return false;
    }

    if (!checked_range(section.sub_offsets[4], kGplMaterialHeaderSize,
                       section.raw_size)) {
        return false;
    }
    const size_t material_header_offset =
        static_cast<size_t>(section.offset) + section.sub_offsets[4];
    if (!checked_range(material_header_offset, kGplMaterialHeaderSize,
                       gpl.raw_file_size)) {
        return false;
    }

    const uint32_t command_list_relative =
        read_be32(raw + material_header_offset + 4);
    const uint16_t command_count =
        read_be16(raw + material_header_offset + 8);
    size_t command_bytes = 0;
    if (command_count == 0 || command_list_relative == 0 ||
        !checked_multiply(command_count, kGplMaterialCommandSize,
                          &command_bytes) ||
        !checked_range(command_list_relative, command_bytes,
                       section.raw_size)) {
        return false;
    }
    const size_t command_list_offset =
        static_cast<size_t>(section.offset) + command_list_relative;
    if (!checked_range(command_list_offset, command_bytes,
                       gpl.raw_file_size)) {
        return false;
    }

    bool found_tev = false;
    GplTargetMaterial parsed;
    for (uint32_t command_index = 0; command_index < command_count;
         ++command_index) {
        const size_t command_offset =
            command_list_offset +
            static_cast<size_t>(command_index) * kGplMaterialCommandSize;
        if (raw[command_offset] != kGplTevCommandType) {
            continue;
        }
        if (found_tev) {
            AWL_LOG_ERROR("GPL: Multiple type-3 TEV commands are unsupported");
            return false;
        }
        parsed.command_index = command_index;
        parsed.tev_mode_word = read_be32(raw + command_offset + 4);
        found_tev = true;
    }
    if (!found_tev || parsed.tev_mode_word != 1) {
        AWL_LOG_ERROR("GPL: Unsupported target TEV mode word 0x%08X",
                      parsed.tev_mode_word);
        return false;
    }

    parsed.uses_vertex_color = color_header.element_count != 1;
    if (parsed.uses_vertex_color) {
        parsed.red = 0xFF;
        parsed.green = 0xFF;
        parsed.blue = 0xFF;
        parsed.alpha = 0xFF;
    } else {
        parsed.red = decoded_colors[0];
        parsed.green = decoded_colors[1];
        parsed.blue = decoded_colors[2];
        parsed.alpha = decoded_colors[3];
    }

    *out_material = parsed;
    if (parsed.uses_vertex_color) {
        AWL_LOG_INFO(
            "GPL: Type-3 command %u selects texture x per-vertex COLOR0 RGBA",
            parsed.command_index);
    } else {
        AWL_LOG_INFO(
            "GPL: Type-3 command %u selects texture x raster RGBA; constant raster color=(%u,%u,%u,%u)",
            parsed.command_index, parsed.red, parsed.green, parsed.blue,
            parsed.alpha);
    }
    return true;
}

static bool parse_display_list_range_for_analysis(
    const GplFile& gpl, const char* debug_name,
    uint32_t vertex_descriptor, uint32_t geometry_command_index,
    size_t display_start, size_t display_end, GplMeshAnalysis* out_mesh) {
    if (!out_mesh) {
        return false;
    }
    clear_mesh(out_mesh);

    if (!supports_single_section_analysis(gpl, "display-list analysis")) {
        return false;
    }

    const GplSection& section = gpl.sections[0];
    if (!section_range_valid(gpl, section) ||
        section.sub_offsets.size() < kGplSubOffsetCount ||
        section.sub_offsets[0] == 0 || section.sub_offsets[1] == 0 ||
        section.sub_offsets[2] == 0 || section.sub_offsets[3] == 0 ||
        section.sub_offsets[4] == 0) {
        return false;
    }

    GplMeshAnalysis parsed_mesh;
    GplAttrHeader position_header;
    GplAttrHeader uv_header;
    if (!parse_attribute_header(gpl, section, section.sub_offsets[0],
                                &position_header) ||
        !extract_attribute(gpl, section, position_header, 3, "position",
                           &parsed_mesh.positions) ||
        !parse_attribute_header(gpl, section, section.sub_offsets[2], &uv_header) ||
        !extract_attribute(gpl, section, uv_header, 2, "UV", &parsed_mesh.uvs)) {
        return false;
    }

    GplAttrHeader color_header;
    if (!parse_attribute_header(gpl, section, section.sub_offsets[1],
                                &color_header)) {
        return false;
    }

    const size_t normal_descriptor_offset =
        static_cast<size_t>(section.offset) + section.sub_offsets[3];
    if (!checked_range(section.sub_offsets[3], sizeof(uint32_t),
                       section.raw_size) ||
        !checked_range(normal_descriptor_offset, sizeof(uint32_t),
                       gpl.raw_file_size)) {
        AWL_LOG_ERROR("GPL: Sub[3] normal descriptor is out of bounds");
        return false;
    }

    if (display_start >= display_end ||
        !checked_range(display_start, display_end - display_start,
                       gpl.raw_file_size) ||
        display_start < section.offset ||
        display_end > static_cast<size_t>(section.offset) + section.raw_size) {
        AWL_LOG_ERROR("GPL: Explicit display-list range is invalid or out of bounds");
        return false;
    }
    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);

    GplVertexLayout vertex_layout;
    if (!supported_vertex_layout(vertex_descriptor, &vertex_layout)) {
        AWL_LOG_ERROR(
            "GPL: Geometry command %u uses unsupported VCD word 0x%08X",
            geometry_command_index, vertex_descriptor);
        return false;
    }
    const bool has_indexed_color = vertex_layout.has_indexed_color;
    std::vector<uint8_t> decoded_colors;
    if (!extract_color_attribute(gpl, section, color_header,
                                 &decoded_colors) ||
        (!has_indexed_color && color_header.element_count != 1) ||
        (has_indexed_color && color_header.element_count <= 1)) {
        AWL_LOG_ERROR(
            "GPL: VCD 0x%08X is inconsistent with its Sub[1] color count",
            vertex_descriptor);
        return false;
    }
    if (has_indexed_color) {
        parsed_mesh.colors_rgba8 = std::move(decoded_colors);
    }
    if (read_be32(raw + normal_descriptor_offset) != 0) {
        AWL_LOG_ERROR(
            "GPL: Verified position/normal/UV layout requires the default Sub[3] normal array");
        return false;
    }

    const size_t vertex_reference_size = vertex_layout.reference_size;
    const size_t display_size = display_end - display_start;
    try {
        parsed_mesh.raw_refs.reserve(display_size / vertex_reference_size);
        parsed_mesh.primitives.reserve(display_size / 3);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("GPL: Unable to allocate display-list analysis output");
        return false;
    } catch (const std::length_error&) {
        return false;
    }

    size_t cursor = display_start;
    size_t nop_count = 0;
    while (cursor < display_end) {
        const uint8_t opcode = raw[cursor];
        if (opcode == 0) {
            ++nop_count;
            ++cursor;
            continue;
        }

        if (opcode < 0x80 || opcode > 0xBF ||
            !checked_range(cursor, 3, display_end)) {
            AWL_LOG_ERROR(
                "GPL: Unsupported or truncated GX opcode 0x%02X at display-list offset 0x%zX",
                opcode, cursor - display_start);
            return false;
        }

        const uint8_t primitive_type = opcode & kGxPrimitiveTypeMask;
        const uint8_t vat_index = opcode & kGxVatMask;
        if (vat_index != 0 ||
            (primitive_type != kGxDrawQuads &&
             primitive_type != kGxDrawQuads2 &&
             primitive_type != kGxDrawTriangles &&
             primitive_type != kGxDrawTriangleStrip &&
             primitive_type != kGxDrawTriangleFan)) {
            AWL_LOG_ERROR(
                "GPL: Unsupported GX primitive opcode 0x%02X at display-list offset 0x%zX",
                opcode, cursor - display_start);
            return false;
        }

        const uint16_t vertex_count = read_be16(raw + cursor + 1);
        const bool valid_group_size =
            ((primitive_type == kGxDrawQuads ||
              primitive_type == kGxDrawQuads2) &&
             vertex_count != 0 && vertex_count % 4 == 0) ||
            (primitive_type == kGxDrawTriangles && vertex_count != 0 &&
             vertex_count % 3 == 0) ||
            ((primitive_type == kGxDrawTriangleStrip ||
              primitive_type == kGxDrawTriangleFan) &&
             vertex_count >= 3);
        if (!valid_group_size) {
            AWL_LOG_ERROR("GPL: Primitive vertex count %u is invalid",
                          vertex_count);
            return false;
        }

        size_t reference_bytes = 0;
        if (!checked_multiply(vertex_count, vertex_reference_size,
                              &reference_bytes)) {
            return false;
        }
        const size_t references_offset = cursor + 3;
        if (!checked_range(references_offset, reference_bytes, display_end)) {
            AWL_LOG_ERROR("GPL: Primitive index data is out of bounds");
            return false;
        }

        GplMeshAnalysis::Primitive primitive;
        primitive.type = primitive_type;
        primitive.first_ref = parsed_mesh.raw_refs.size();
        primitive.ref_count = vertex_count;

        try {
            for (uint32_t i = 0; i < vertex_count; ++i) {
                const size_t reference_offset =
                    references_offset +
                    static_cast<size_t>(i) * vertex_reference_size;
                GplMeshAnalysis::VertexRef reference;
                reference.has_pos = true;
                reference.has_normal = true;
                reference.has_color = has_indexed_color;
                reference.has_uv = true;
                size_t field_offset = reference_offset;
                if (vertex_layout.position_index_16) {
                    reference.pos_idx = read_be16(raw + field_offset);
                    field_offset += sizeof(uint16_t);
                } else {
                    reference.pos_idx = raw[field_offset++];
                }
                reference.normal_idx = raw[field_offset++];
                if (has_indexed_color) {
                    if (vertex_layout.color_index_16) {
                        reference.color_idx = read_be16(raw + field_offset);
                        field_offset += sizeof(uint16_t);
                    } else {
                        reference.color_idx = raw[field_offset++];
                    }
                }
                reference.uv_idx = vertex_layout.uv_index_16
                                       ? read_be16(raw + field_offset)
                                       : raw[field_offset];

                if (reference.pos_idx >= position_header.element_count ||
                    reference.uv_idx >= uv_header.element_count ||
                    (has_indexed_color &&
                     reference.color_idx >= color_header.element_count)) {
                    AWL_LOG_ERROR(
                        "GPL: Primitive reference %u is out of bounds", i);
                    return false;
                }
                parsed_mesh.raw_refs.push_back(reference);
            }
            parsed_mesh.primitives.push_back(primitive);
        } catch (const std::bad_alloc&) {
            AWL_LOG_ERROR("GPL: Unable to allocate primitive references");
            return false;
        } catch (const std::length_error&) {
            return false;
        }

        cursor = references_offset + reference_bytes;
    }

    if (parsed_mesh.primitives.empty()) {
        AWL_LOG_ERROR("GPL: Display list contains no supported primitives");
        return false;
    }

    if (has_indexed_color) {
        bool all_alpha_zero = true;
        for (size_t offset = 3; offset < parsed_mesh.colors_rgba8.size();
             offset += 4) {
            if (parsed_mesh.colors_rgba8[offset] != 0) {
                all_alpha_zero = false;
                break;
            }
        }
        if (all_alpha_zero) {
            AWL_LOG_INFO(
                "GPL: Every decoded COLOR0 alpha is zero; texture x COLOR0 is expected to be fully transparent");
        }
    }

    *out_mesh = std::move(parsed_mesh);
    AWL_LOG_INFO(
        "GPL: Parsed %zu primitives and %zu validated %s references%s%s",
        out_mesh->primitives.size(), out_mesh->raw_refs.size(),
        vertex_layout.description,
        debug_name ? " from " : "", debug_name ? debug_name : "");
    if (nop_count != 0) {
        AWL_LOG_INFO("GPL: Consumed %zu GX NOP padding bytes", nop_count);
    }
    return true;
}

bool gpl_parse_display_list_for_analysis(const GplFile& gpl,
                                         const char* debug_name,
                                         GplMeshAnalysis* out_mesh) {
    if (!out_mesh) {
        return false;
    }
    clear_mesh(out_mesh);
    if (!supports_single_section_analysis(gpl, "ordered draw analysis")) {
        return false;
    }

    const GplSection& section = gpl.sections[0];
    size_t display_start = 0;
    size_t display_end = 0;
    uint32_t vertex_descriptor = 0;
    uint32_t geometry_command_index = 0;
    if (!display_list_range(gpl, section, &display_start, &display_end,
                            &vertex_descriptor, &geometry_command_index)) {
        AWL_LOG_ERROR("GPL: Display-list pointer is invalid or out of bounds");
        return false;
    }
    return parse_display_list_range_for_analysis(
        gpl, debug_name, vertex_descriptor, geometry_command_index,
        display_start, display_end, out_mesh);
}

bool gpl_parse_draw_sequence_for_analysis(
    const GplFile& gpl, const char* debug_name,
    GplDrawSequenceAnalysis* out_sequence) {
    if (!out_sequence) {
        return false;
    }
    out_sequence->batches.clear();
    if (!gpl.raw_file_data || gpl.sections.empty()) {
        return false;
    }

    const GplSection& section = gpl.sections[0];
    if (!section_range_valid(gpl, section) ||
        section.sub_offsets.size() < kGplSubOffsetCount ||
        section.sub_offsets[4] == 0 ||
        !checked_range(section.sub_offsets[4], kGplMaterialHeaderSize,
                       section.raw_size)) {
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(gpl.raw_file_data);
    const size_t material_header_offset =
        static_cast<size_t>(section.offset) + section.sub_offsets[4];
    const uint32_t header_display_list_relative =
        read_be32(raw + material_header_offset);
    const uint32_t command_list_relative =
        read_be32(raw + material_header_offset + 4);
    const uint16_t command_count =
        read_be16(raw + material_header_offset + 8);
    size_t command_bytes = 0;
    if (header_display_list_relative == 0 || command_count == 0 ||
        command_list_relative == 0 ||
        !checked_multiply(command_count, kGplMaterialCommandSize,
                          &command_bytes) ||
        !checked_range(command_list_relative, command_bytes,
                       section.raw_size)) {
        return false;
    }
    const size_t command_list_offset =
        static_cast<size_t>(section.offset) + command_list_relative;

    bool has_texture = false;
    bool has_vertex_descriptor = false;
    bool has_target_tev = false;
    bool saw_first_draw = false;
    uint32_t active_texture = 0;
    uint32_t active_vertex_descriptor = 0;
    GplDrawSequenceAnalysis parsed;

    const auto append_draw = [&](uint32_t command_index,
                                 uint32_t display_relative,
                                 uint32_t display_size) -> bool {
        if (!has_texture || !has_vertex_descriptor || !has_target_tev ||
            display_relative == 0 || display_size == 0 ||
            !checked_range(display_relative, display_size,
                           section.raw_size)) {
            AWL_LOG_ERROR(
                "GPL: Draw command %u is missing verified state or has an invalid range",
                command_index);
            return false;
        }
        if (!saw_first_draw &&
            display_relative != header_display_list_relative) {
            AWL_LOG_ERROR(
                "GPL: First ordered draw does not match the material header");
            return false;
        }

        GplDrawBatchAnalysis batch;
        batch.command_index = command_index;
        batch.texture_image_index = active_texture;
        batch.vertex_descriptor = active_vertex_descriptor;
        const size_t display_start =
            static_cast<size_t>(section.offset) + display_relative;
        const size_t display_end = display_start + display_size;
        if (!parse_display_list_range_for_analysis(
                gpl, debug_name, active_vertex_descriptor, command_index,
                display_start, display_end, &batch.mesh)) {
            return false;
        }
        try {
            parsed.batches.push_back(std::move(batch));
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
        saw_first_draw = true;
        return true;
    };

    for (uint32_t command_index = 0; command_index < command_count;
         ++command_index) {
        const size_t command_offset =
            command_list_offset +
            static_cast<size_t>(command_index) * kGplMaterialCommandSize;
        const uint8_t command_type = raw[command_offset];
        const uint32_t command_word = read_be32(raw + command_offset + 4);
        const uint32_t display_relative =
            read_be32(raw + command_offset + 8);
        const uint32_t display_size =
            read_be32(raw + command_offset + 12);
        const bool has_draw = display_relative != 0 || display_size != 0;
        if ((display_relative == 0) != (display_size == 0)) {
            AWL_LOG_ERROR(
                "GPL: Command %u has a partial display-list range",
                command_index);
            return false;
        }

        if (command_type == kGplTextureCommandType) {
            // FUN_801A3000 flushes this command's range before installing the
            // replacement texture.
            if (has_draw && !append_draw(
                                command_index, display_relative,
                                display_size)) {
                return false;
            }
            const uint8_t texture_unit =
                static_cast<uint8_t>((command_word >> 13) & 0x7u);
            if (raw[command_offset + 1] != 0xFF || texture_unit != 0) {
                AWL_LOG_ERROR(
                    "GPL: Command %u uses unsupported texture state",
                    command_index);
                return false;
            }
            active_texture = command_word & 0x1FFFu;
            has_texture = true;
        } else if (command_type == kGplGeometryCommandType) {
            active_vertex_descriptor = command_word;
            GplVertexLayout ignored;
            if (!supported_vertex_layout(active_vertex_descriptor, &ignored)) {
                AWL_LOG_ERROR(
                    "GPL: Command %u uses unsupported VCD 0x%08X",
                    command_index, active_vertex_descriptor);
                return false;
            }
            has_vertex_descriptor = true;
            if (has_draw && !append_draw(
                                command_index, display_relative,
                                display_size)) {
                return false;
            }
        } else if (command_type == kGplTevCommandType) {
            if (command_word != 1) {
                AWL_LOG_ERROR(
                    "GPL: Command %u uses unsupported TEV word 0x%08X",
                    command_index, command_word);
                return false;
            }
            has_target_tev = true;
            if (has_draw && !append_draw(
                                command_index, display_relative,
                                display_size)) {
                return false;
            }
        } else {
            AWL_LOG_ERROR("GPL: Unsupported ordered command type 0x%02X",
                          command_type);
            return false;
        }
    }

    if (parsed.batches.empty()) {
        return false;
    }
    *out_sequence = std::move(parsed);
    AWL_LOG_INFO("GPL: Parsed %zu ordered draw batch(es)%s%s",
                 out_sequence->batches.size(), debug_name ? " from " : "",
                 debug_name ? debug_name : "");
    return true;
}

} // namespace awl
