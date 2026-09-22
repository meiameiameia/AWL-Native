#include "awl/tpl.h"
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

constexpr uint32_t kTplMagic = 0x0020AF30;
constexpr uint32_t kMaxTextureCount = 65536;
constexpr size_t kTplDescriptorSize = 8;
constexpr size_t kTplImageHeaderSize = 36;

struct TplBlockLayout {
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
};

struct RgbaColor {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
};

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

bool checked_add(size_t lhs, size_t rhs, size_t* result) {
    if (!result || rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    *result = lhs + rhs;
    return true;
}

bool get_block_layout(GXTexFmt format, TplBlockLayout* layout) {
    if (!layout) {
        return false;
    }

    switch (format) {
        case GXTexFmt::I4:
        case GXTexFmt::CI4:
        case GXTexFmt::CMPR:
            *layout = {8, 8, 32};
            return true;
        case GXTexFmt::I8:
        case GXTexFmt::IA4:
        case GXTexFmt::CI8:
            *layout = {8, 4, 32};
            return true;
        case GXTexFmt::IA8:
        case GXTexFmt::RGB565:
        case GXTexFmt::RGB5A3:
        case GXTexFmt::CI14X2:
            *layout = {4, 4, 32};
            return true;
        case GXTexFmt::RGBA8:
            *layout = {4, 4, 64};
            return true;
        default:
            return false;
    }
}

bool calculate_texture_data_size(const TplTextureHeader& header, size_t* data_size) {
    if (!data_size || header.width == 0 || header.height == 0) {
        return false;
    }

    TplBlockLayout layout = {};
    if (!get_block_layout(static_cast<GXTexFmt>(header.format), &layout)) {
        return false;
    }

    const size_t block_count_x =
        (static_cast<size_t>(header.width) + layout.width - 1) / layout.width;
    const size_t block_count_y =
        (static_cast<size_t>(header.height) + layout.height - 1) / layout.height;

    size_t block_count = 0;
    if (!checked_multiply(block_count_x, block_count_y, &block_count) ||
        !checked_multiply(block_count, layout.bytes, data_size)) {
        return false;
    }
    return true;
}

uint32_t maximum_mip_level_count(uint32_t width, uint32_t height) {
    uint32_t count = 1;
    while (width > 1 || height > 1) {
        width = (std::max)(1U, width / 2);
        height = (std::max)(1U, height / 2);
        ++count;
    }
    return count;
}

void write_rgba(std::vector<uint8_t>& output, uint32_t width, uint32_t x, uint32_t y,
                uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    const size_t index =
        (static_cast<size_t>(y) * static_cast<size_t>(width) + x) * 4;
    output[index + 0] = red;
    output[index + 1] = green;
    output[index + 2] = blue;
    output[index + 3] = alpha;
}

RgbaColor decode_rgb565(uint16_t value) {
    const uint8_t red5 = static_cast<uint8_t>((value >> 11) & 0x1F);
    const uint8_t green6 = static_cast<uint8_t>((value >> 5) & 0x3F);
    const uint8_t blue5 = static_cast<uint8_t>(value & 0x1F);
    return {
        static_cast<uint8_t>((red5 << 3) | (red5 >> 2)),
        static_cast<uint8_t>((green6 << 2) | (green6 >> 4)),
        static_cast<uint8_t>((blue5 << 3) | (blue5 >> 2)),
        255,
    };
}

uint8_t blend_cmpr_5_3(uint8_t weighted_five, uint8_t weighted_three) {
    // GameCube CMPR uses 5/8 and 3/8 interpolation, not DXT1 thirds.
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(weighted_five) * 5 +
         static_cast<uint32_t>(weighted_three) * 3) >> 3);
}

void decode_cmpr_sub_block(std::vector<uint8_t>& output, uint32_t image_width,
                           uint32_t image_height, uint32_t block_x, uint32_t block_y,
                           const uint8_t* block) {
    const uint16_t endpoint0 = read_be16(block);
    const uint16_t endpoint1 = read_be16(block + 2);
    RgbaColor colors[4] = {decode_rgb565(endpoint0), decode_rgb565(endpoint1), {}, {}};

    if (endpoint0 > endpoint1) {
        colors[2] = {
            blend_cmpr_5_3(colors[0].red, colors[1].red),
            blend_cmpr_5_3(colors[0].green, colors[1].green),
            blend_cmpr_5_3(colors[0].blue, colors[1].blue),
            255,
        };
        colors[3] = {
            blend_cmpr_5_3(colors[1].red, colors[0].red),
            blend_cmpr_5_3(colors[1].green, colors[0].green),
            blend_cmpr_5_3(colors[1].blue, colors[0].blue),
            255,
        };
    } else {
        colors[2] = {
            static_cast<uint8_t>((static_cast<uint32_t>(colors[0].red) + colors[1].red) / 2),
            static_cast<uint8_t>((static_cast<uint32_t>(colors[0].green) + colors[1].green) / 2),
            static_cast<uint8_t>((static_cast<uint32_t>(colors[0].blue) + colors[1].blue) / 2),
            255,
        };
        colors[3] = colors[2];
        colors[3].alpha = 0;
    }

    for (uint32_t y = 0; y < 4; ++y) {
        const uint8_t selectors = block[4 + y];
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t pixel_x = block_x + x;
            const uint32_t pixel_y = block_y + y;
            if (pixel_x >= image_width || pixel_y >= image_height) {
                continue;
            }
            const uint8_t selector =
                static_cast<uint8_t>((selectors >> (6 - x * 2)) & 0x03);
            const RgbaColor& color = colors[selector];
            write_rgba(output, image_width, pixel_x, pixel_y,
                       color.red, color.green, color.blue, color.alpha);
        }
    }
}

std::vector<uint8_t> decode_level_to_rgba8(const uint8_t* raw_data,
                                           uint32_t width, uint32_t height,
                                           GXTexFmt format,
                                           size_t encoded_size) {
    TplTextureHeader level_header = {};
    level_header.width = static_cast<uint16_t>(width);
    level_header.height = static_cast<uint16_t>(height);
    level_header.format = static_cast<uint32_t>(format);

    size_t required_data_size = 0;
    if (!raw_data || !calculate_texture_data_size(level_header, &required_data_size) ||
        required_data_size > encoded_size) {
        AWL_LOG_ERROR("TPL: Texture level data is smaller than its format requires");
        return {};
    }

    size_t pixel_count = 0;
    size_t output_size = 0;
    if (!checked_multiply(width, height, &pixel_count) ||
        !checked_multiply(pixel_count, 4, &output_size)) {
        AWL_LOG_ERROR("TPL: Decoded texture dimensions overflow output size");
        return {};
    }

    std::vector<uint8_t> output;
    try {
        output.assign(output_size, 0);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("TPL: Unable to allocate %zu decoded bytes", output_size);
        return {};
    } catch (const std::length_error&) {
        AWL_LOG_ERROR("TPL: Decoded output size is invalid");
        return {};
    }

    size_t source_offset = 0;
    if (format == GXTexFmt::I4) {
        for (uint32_t block_y = 0; block_y < height; block_y += 8) {
            for (uint32_t block_x = 0; block_x < width; block_x += 8) {
                for (uint32_t y = 0; y < 8; ++y) {
                    for (uint32_t x = 0; x < 8; x += 2) {
                        const uint8_t packed = raw_data[source_offset++];
                        const uint8_t high = static_cast<uint8_t>(packed >> 4);
                        const uint8_t low = static_cast<uint8_t>(packed & 0x0F);
                        const uint32_t pixel_x = block_x + x;
                        const uint32_t pixel_y = block_y + y;
                        if (pixel_x < width && pixel_y < height) {
                            const uint8_t intensity = static_cast<uint8_t>((high << 4) | high);
                            write_rgba(output, width, pixel_x, pixel_y,
                                       intensity, intensity, intensity, intensity);
                        }
                        if (pixel_x + 1 < width && pixel_y < height) {
                            const uint8_t intensity = static_cast<uint8_t>((low << 4) | low);
                            write_rgba(output, width, pixel_x + 1, pixel_y,
                                       intensity, intensity, intensity, intensity);
                        }
                    }
                }
            }
        }
    } else if (format == GXTexFmt::I8) {
        for (uint32_t block_y = 0; block_y < height; block_y += 4) {
            for (uint32_t block_x = 0; block_x < width; block_x += 8) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 8; ++x) {
                        const uint8_t intensity = raw_data[source_offset++];
                        const uint32_t pixel_x = block_x + x;
                        const uint32_t pixel_y = block_y + y;
                        if (pixel_x < width && pixel_y < height) {
                            write_rgba(output, width, pixel_x, pixel_y,
                                       intensity, intensity, intensity, intensity);
                        }
                    }
                }
            }
        }
    } else if (format == GXTexFmt::RGB5A3) {
        for (uint32_t block_y = 0; block_y < height; block_y += 4) {
            for (uint32_t block_x = 0; block_x < width; block_x += 4) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint16_t texel = read_be16(raw_data + source_offset);
                        source_offset += 2;

                        const uint32_t pixel_x = block_x + x;
                        const uint32_t pixel_y = block_y + y;
                        if (pixel_x >= width || pixel_y >= height) {
                            continue;
                        }

                        uint8_t red = 0;
                        uint8_t green = 0;
                        uint8_t blue = 0;
                        uint8_t alpha = 0;
                        if ((texel & 0x8000) != 0) {
                            const uint8_t red5 = static_cast<uint8_t>((texel >> 10) & 0x1F);
                            const uint8_t green5 = static_cast<uint8_t>((texel >> 5) & 0x1F);
                            const uint8_t blue5 = static_cast<uint8_t>(texel & 0x1F);
                            red = static_cast<uint8_t>((red5 << 3) | (red5 >> 2));
                            green = static_cast<uint8_t>((green5 << 3) | (green5 >> 2));
                            blue = static_cast<uint8_t>((blue5 << 3) | (blue5 >> 2));
                            alpha = 255;
                        } else {
                            const uint8_t alpha3 = static_cast<uint8_t>((texel >> 12) & 0x07);
                            const uint8_t red4 = static_cast<uint8_t>((texel >> 8) & 0x0F);
                            const uint8_t green4 = static_cast<uint8_t>((texel >> 4) & 0x0F);
                            const uint8_t blue4 = static_cast<uint8_t>(texel & 0x0F);
                            alpha = static_cast<uint8_t>((alpha3 << 5) | (alpha3 << 2) |
                                                         (alpha3 >> 1));
                            red = static_cast<uint8_t>((red4 << 4) | red4);
                            green = static_cast<uint8_t>((green4 << 4) | green4);
                            blue = static_cast<uint8_t>((blue4 << 4) | blue4);
                        }
                        write_rgba(output, width, pixel_x, pixel_y,
                                   red, green, blue, alpha);
                    }
                }
            }
        }
    } else if (format == GXTexFmt::CMPR) {
        for (uint32_t block_y = 0; block_y < height; block_y += 8) {
            for (uint32_t block_x = 0; block_x < width; block_x += 8) {
                const uint8_t* block = raw_data + source_offset;
                source_offset += 32;
                for (uint32_t sub_block = 0; sub_block < 4; ++sub_block) {
                    const uint32_t sub_x = block_x + (sub_block & 1) * 4;
                    const uint32_t sub_y = block_y + (sub_block >> 1) * 4;
                    decode_cmpr_sub_block(output, width, height, sub_x, sub_y,
                                          block + sub_block * 8);
                }
            }
        }
    } else if (format == GXTexFmt::RGBA8) {
        for (uint32_t block_y = 0; block_y < height; block_y += 4) {
            for (uint32_t block_x = 0; block_x < width; block_x += 4) {
                const uint8_t* block = raw_data + source_offset;
                source_offset += 64;
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        const size_t texel_index = static_cast<size_t>(y * 4 + x) * 2;
                        const uint32_t pixel_x = block_x + x;
                        const uint32_t pixel_y = block_y + y;
                        if (pixel_x < width && pixel_y < height) {
                            write_rgba(output, width, pixel_x, pixel_y,
                                       block[texel_index + 1], block[texel_index + 32],
                                       block[texel_index + 33], block[texel_index]);
                        }
                    }
                }
            }
        }
    } else {
        AWL_LOG_ERROR("TPL: Unsupported format %u for RGBA8 conversion",
                      static_cast<uint32_t>(format));
        return {};
    }

    return output;
}

} // namespace

TplFile::~TplFile() {
    tpl_free(this);
}

TplFile::TplFile(TplFile&& other) noexcept
    : header(std::move(other.header)),
      textures(std::move(other.textures)),
      raw_file_data(other.raw_file_data),
      raw_file_size(other.raw_file_size) {
    other.raw_file_data = nullptr;
    other.raw_file_size = 0;
    other.header = {};
    other.textures.clear();
}

TplFile& TplFile::operator=(TplFile&& other) noexcept {
    if (this != &other) {
        tpl_free(this);
        header = std::move(other.header);
        textures = std::move(other.textures);
        raw_file_data = other.raw_file_data;
        raw_file_size = other.raw_file_size;
        other.raw_file_data = nullptr;
        other.raw_file_size = 0;
        other.header = {};
        other.textures.clear();
    }
    return *this;
}

bool tpl_load_from_file(const char* logical_path, TplFile* out_tpl) {
    if (!logical_path || !out_tpl) {
        return false;
    }

    tpl_free(out_tpl);

    void* file_data = nullptr;
    size_t file_size = 0;
    if (!filesystem_read_entire_file(logical_path, &file_data, &file_size)) {
        AWL_LOG_ERROR("TPL: Failed to read file %s", logical_path);
        return false;
    }

    TplFile parsed;
    parsed.raw_file_data = file_data;
    parsed.raw_file_size = file_size;

    const auto fail = [&parsed]() {
        tpl_free(&parsed);
        return false;
    };

    if (!checked_range(0, 12, file_size)) {
        AWL_LOG_ERROR("TPL: File %s too small (size %zu)", logical_path, file_size);
        return fail();
    }

    const uint8_t* buffer = static_cast<const uint8_t*>(file_data);
    parsed.header.magic = read_be32(buffer + 0);
    parsed.header.texture_count = read_be32(buffer + 4);
    parsed.header.descriptor_table_offset = read_be32(buffer + 8);

    if (parsed.header.magic != kTplMagic) {
        AWL_LOG_ERROR("TPL: Invalid magic number 0x%08X in %s", parsed.header.magic,
                      logical_path);
        return fail();
    }

    if (parsed.header.texture_count > kMaxTextureCount) {
        AWL_LOG_ERROR("TPL: Texture count %u exceeds safety limit in %s",
                      parsed.header.texture_count, logical_path);
        return fail();
    }

    const size_t table_offset = parsed.header.descriptor_table_offset;
    if (table_offset > file_size ||
        parsed.header.texture_count >
            (file_size - table_offset) / kTplDescriptorSize) {
        AWL_LOG_ERROR("TPL: Descriptor table out of bounds in %s", logical_path);
        return fail();
    }

    try {
        parsed.textures.resize(parsed.header.texture_count);
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("TPL: Unable to allocate texture descriptors for %s", logical_path);
        return fail();
    } catch (const std::length_error&) {
        AWL_LOG_ERROR("TPL: Invalid texture descriptor count in %s", logical_path);
        return fail();
    }

    for (uint32_t i = 0; i < parsed.header.texture_count; ++i) {
        TplTexture& texture = parsed.textures[i];
        const size_t descriptor_offset =
            table_offset + static_cast<size_t>(i) * kTplDescriptorSize;
        texture.image_header_offset = read_be32(buffer + descriptor_offset);
        texture.palette_header_offset = read_be32(buffer + descriptor_offset + 4);

        if (texture.image_header_offset == 0) {
            continue;
        }

        const size_t image_header_offset = texture.image_header_offset;
        if (!checked_range(image_header_offset, kTplImageHeaderSize, file_size)) {
            AWL_LOG_ERROR("TPL: Image header %u out of bounds in %s", i, logical_path);
            return fail();
        }

        const uint8_t* image_header = buffer + image_header_offset;
        texture.header.height = read_be16(image_header + 0);
        texture.header.width = read_be16(image_header + 2);
        texture.header.format = read_be32(image_header + 4);
        texture.header.data_offset = read_be32(image_header + 8);
        texture.header.wrap_s = read_be32(image_header + 12);
        texture.header.wrap_t = read_be32(image_header + 16);
        texture.header.min_filter = read_be32(image_header + 20);
        texture.header.mag_filter = read_be32(image_header + 24);
        texture.header.lod_bias = read_be32f(image_header + 28);
        texture.header.edge_lod = image_header[32];
        texture.header.min_lod = image_header[33];
        texture.header.max_lod = image_header[34];
        texture.header.unpacked = image_header[35];

        if (texture.header.min_lod > texture.header.max_lod ||
            static_cast<uint32_t>(texture.header.max_lod) + 1 >
                maximum_mip_level_count(texture.header.width,
                                        texture.header.height)) {
            AWL_LOG_ERROR("TPL: Image %u has an invalid LOD range %u..%u",
                          i, texture.header.min_lod, texture.header.max_lod);
            return fail();
        }

        const uint32_t mip_level_count =
            static_cast<uint32_t>(texture.header.max_lod) + 1;
        uint32_t mip_width = texture.header.width;
        uint32_t mip_height = texture.header.height;
        size_t encoded_data_size = 0;
        try {
            texture.mip_levels.reserve(mip_level_count);
        } catch (const std::bad_alloc&) {
            AWL_LOG_ERROR("TPL: Unable to allocate mip descriptors for image %u", i);
            return fail();
        } catch (const std::length_error&) {
            AWL_LOG_ERROR("TPL: Invalid mip descriptor count for image %u", i);
            return fail();
        }

        for (uint32_t level = 0; level < mip_level_count; ++level) {
            TplTextureHeader level_header = texture.header;
            level_header.width = static_cast<uint16_t>(mip_width);
            level_header.height = static_cast<uint16_t>(mip_height);
            size_t level_size = 0;
            if (!calculate_texture_data_size(level_header, &level_size) ||
                level_size > std::numeric_limits<uint32_t>::max()) {
                AWL_LOG_ERROR(
                    "TPL: Image %u mip %u has invalid dimensions or format %u",
                    i, level, texture.header.format);
                return fail();
            }
            if (!checked_add(encoded_data_size, level_size, &encoded_data_size)) {
                AWL_LOG_ERROR("TPL: Image %u mip chain size overflowed", i);
                return fail();
            }

            TplMipLevel mip = {};
            mip.width = static_cast<uint16_t>(mip_width);
            mip.height = static_cast<uint16_t>(mip_height);
            mip.encoded_size = static_cast<uint32_t>(level_size);
            texture.mip_levels.push_back(mip);
            mip_width = (std::max)(1U, mip_width / 2);
            mip_height = (std::max)(1U, mip_height / 2);
        }

        if (encoded_data_size > std::numeric_limits<uint32_t>::max()) {
            AWL_LOG_ERROR("TPL: Image %u has invalid dimensions or unsupported format %u",
                          i, texture.header.format);
            return fail();
        }
        const size_t data_offset = texture.header.data_offset;
        if (!checked_range(data_offset, encoded_data_size, file_size)) {
            AWL_LOG_ERROR(
                "TPL: Image %u data bounds check failed (offset=%u, size=%zu, file_size=%zu)",
                i, texture.header.data_offset, encoded_data_size, file_size);
            return fail();
        }
        texture.encoded_data_size = static_cast<uint32_t>(encoded_data_size);
        texture.raw_data = buffer + data_offset;
        size_t mip_offset = 0;
        for (TplMipLevel& mip : texture.mip_levels) {
            mip.raw_data = texture.raw_data + mip_offset;
            mip_offset += mip.encoded_size;
        }
    }

    *out_tpl = std::move(parsed);
    parsed.raw_file_data = nullptr;
    parsed.raw_file_size = 0;
    return true;
}

void tpl_free(TplFile* tpl) {
    if (!tpl) {
        return;
    }
    if (tpl->raw_file_data) {
        filesystem_free_file_data(tpl->raw_file_data);
    }
    tpl->raw_file_data = nullptr;
    tpl->raw_file_size = 0;
    tpl->textures.clear();
    tpl->header = {};
}

bool tpl_dump_metadata(const TplFile& tpl) {
    AWL_LOG_INFO("--- TPL Metadata Dump ---");
    AWL_LOG_INFO("  File size: %zu bytes", tpl.raw_file_size);
    AWL_LOG_INFO("  Magic: 0x%08X", tpl.header.magic);
    AWL_LOG_INFO("  Texture/Image count: %u", tpl.header.texture_count);
    AWL_LOG_INFO("  Descriptor table offset: %u", tpl.header.descriptor_table_offset);

    for (size_t i = 0; i < tpl.textures.size(); ++i) {
        const TplTexture& texture = tpl.textures[i];
        AWL_LOG_INFO("  Image %zu:", i);
        AWL_LOG_INFO("    Descriptor - Image Header Offset: %u",
                     texture.image_header_offset);
        AWL_LOG_INFO("    Descriptor - Palette Header Offset: %u",
                     texture.palette_header_offset);
        if (texture.image_header_offset != 0) {
            AWL_LOG_INFO("    Width: %u", texture.header.width);
            AWL_LOG_INFO("    Height: %u", texture.header.height);
            AWL_LOG_INFO("    Format ID: %u", texture.header.format);
            AWL_LOG_INFO("    Data Offset: %u", texture.header.data_offset);
            AWL_LOG_INFO("    Wrap S: %u", texture.header.wrap_s);
            AWL_LOG_INFO("    Wrap T: %u", texture.header.wrap_t);
            AWL_LOG_INFO("    Min Filter: %u", texture.header.min_filter);
            AWL_LOG_INFO("    Mag Filter: %u", texture.header.mag_filter);
            AWL_LOG_INFO("    LOD Range: %u..%u", texture.header.min_lod,
                         texture.header.max_lod);
            AWL_LOG_INFO("    LOD Bias: %.3f", texture.header.lod_bias);
            AWL_LOG_INFO("    Mip Levels: %zu", texture.mip_levels.size());
            AWL_LOG_INFO("    Encoded Data Size: %u bytes",
                         texture.encoded_data_size);
        }
    }
    AWL_LOG_INFO("-------------------------");
    return true;
}

std::vector<uint8_t> tpl_convert_to_rgba8(const TplTexture& texture) {
    if (texture.mip_levels.empty()) {
        return {};
    }
    const TplMipLevel& level = texture.mip_levels.front();
    return decode_level_to_rgba8(
        level.raw_data, level.width, level.height,
        static_cast<GXTexFmt>(texture.header.format), level.encoded_size);
}

bool tpl_decode_mip_chain_to_rgba8(const TplTexture& texture,
                                   TplDecodedTexture* out_texture) {
    if (!out_texture || texture.mip_levels.empty()) {
        return false;
    }

    TplDecodedTexture decoded;
    decoded.header = texture.header;
    try {
        decoded.mip_levels.reserve(texture.mip_levels.size());
        for (const TplMipLevel& encoded_level : texture.mip_levels) {
            TplDecodedMipLevel level;
            level.width = encoded_level.width;
            level.height = encoded_level.height;
            level.rgba8 = decode_level_to_rgba8(
                encoded_level.raw_data, encoded_level.width, encoded_level.height,
                static_cast<GXTexFmt>(texture.header.format),
                encoded_level.encoded_size);
            if (level.rgba8.empty()) {
                return false;
            }
            decoded.mip_levels.push_back(std::move(level));
        }
    } catch (const std::bad_alloc&) {
        AWL_LOG_ERROR("TPL: Unable to allocate decoded mip chain");
        return false;
    } catch (const std::length_error&) {
        AWL_LOG_ERROR("TPL: Decoded mip chain size is invalid");
        return false;
    }

    *out_texture = std::move(decoded);
    return true;
}

} // namespace awl
