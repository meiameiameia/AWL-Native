#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

// GameCube GX texture formats
enum class GXTexFmt : uint32_t {
    I4 = 0x0,
    I8 = 0x1,
    IA4 = 0x2,
    IA8 = 0x3,
    RGB565 = 0x4,
    RGB5A3 = 0x5,
    RGBA8 = 0x6,
    CI4 = 0x8,
    CI8 = 0x9,
    CI14X2 = 0xA,
    CMPR = 0xE
};

struct TplTextureHeader {
    uint16_t height = 0;
    uint16_t width = 0;
    uint32_t format = 0;
    uint32_t data_offset = 0;
    uint32_t wrap_s = 0;
    uint32_t wrap_t = 0;
    uint32_t min_filter = 0;
    uint32_t mag_filter = 0;
    float lod_bias = 0.0f;
    uint8_t edge_lod = 0;
    uint8_t min_lod = 0;
    uint8_t max_lod = 0;
    uint8_t unpacked = 0;
};

struct TplMipLevel {
    uint16_t height = 0;
    uint16_t width = 0;
    uint32_t encoded_size = 0;
    const uint8_t* raw_data = nullptr;
};

// Texture descriptor
struct TplTexture {
    uint32_t image_header_offset = 0;
    uint32_t palette_header_offset = 0;

    TplTextureHeader header;
    uint32_t encoded_data_size = 0;
    std::vector<TplMipLevel> mip_levels;

    // Pointers into the raw file buffer
    const uint8_t* raw_data = nullptr;
};

struct TplDecodedMipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba8;
};

struct TplDecodedTexture {
    TplTextureHeader header;
    std::vector<TplDecodedMipLevel> mip_levels;
};

// TPL file header
struct TplHeader {
    uint32_t magic = 0; // 0x0020AF30
    uint32_t texture_count = 0;
    uint32_t descriptor_table_offset = 0;
};

// TPL file container
struct TplFile {
    TplFile() = default;
    ~TplFile();
    TplFile(const TplFile&) = delete;
    TplFile& operator=(const TplFile&) = delete;
    TplFile(TplFile&& other) noexcept;
    TplFile& operator=(TplFile&& other) noexcept;

    TplHeader header;
    std::vector<TplTexture> textures;

    // The raw file buffer read from the filesystem
    void* raw_file_data = nullptr;
    size_t raw_file_size = 0;
};

// Loads a TPL file from a logical path using the filesystem layer
bool tpl_load_from_file(const char* logical_path, TplFile* out_tpl);

// Frees the resources associated with a TPL file
void tpl_free(TplFile* tpl);

// Safely dumps the metadata for the loaded TPL file
bool tpl_dump_metadata(const TplFile& tpl);

// Attempt to convert the given texture to RGBA8 format.
// Returns an empty vector if the format is not supported.
std::vector<uint8_t> tpl_convert_to_rgba8(const TplTexture& texture);

// Decodes every TPL mip level declared by max_lod. The level data is laid out
// consecutively from the image data pointer, as consumed by the original GX
// texture setup path. Returns false without partial output on malformed data.
bool tpl_decode_mip_chain_to_rgba8(const TplTexture& texture,
                                   TplDecodedTexture* out_texture);

} // namespace awl
