#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

// GPL file header
struct GplHeader {
    uint32_t magic = 0;     // 0x005BBC61
    uint32_t unknown1 = 0;  // Likely a version or flags
    uint32_t unknown2 = 0;  // Likely related to data size or flags
    uint32_t section_count = 0; // Number of sections inside this GPL
    uint32_t constant = 0;  // Always 0x14? Needs verification
    std::vector<uint32_t> section_offsets;
};

// Known GX Component Types
enum class GXCompType : uint8_t {
    U8  = 0,
    S8  = 1,
    U16 = 2,
    S16 = 3,
    F32 = 4,
    Unknown = 0xFF
};

// Embedded VAT attribute header
struct GplAttrHeader {
    uint32_t data_offset = 0;
    uint16_t element_count = 0;
    uint8_t type_and_frac = 0;
    uint8_t comp_count = 0;
};

// GPL section descriptor
struct GplSection {
    uint32_t offset = 0;
    std::vector<uint32_t> sub_offsets;
    // Pointers into the raw file data
    const uint8_t* raw_data = nullptr;
    uint32_t raw_size = 0;
};

// GPL file container
struct GplFile {
    GplFile() = default;
    ~GplFile();
    GplFile(const GplFile&) = delete;
    GplFile& operator=(const GplFile&) = delete;
    GplFile(GplFile&& other) noexcept;
    GplFile& operator=(GplFile&& other) noexcept;

    void* raw_file_data = nullptr;
    size_t raw_file_size = 0;

    GplHeader header;
    std::vector<GplSection> sections;
};

// Loads a GPL file via the logical filesystem
bool gpl_load_from_file(const char* logical_path, GplFile* out_gpl);

// Frees the underlying raw memory
void gpl_free(GplFile* gpl);

// Dumps structured metadata to the logger
bool gpl_dump_metadata(const GplFile& gpl);

// Deep hex payload analysis and float/GX primitive interpretation
bool gpl_analyze_payload(const GplFile& Gpl);

struct GplMeshAnalysis {
    // Flat array of extracted positions (X, Y, Z)
    std::vector<float> positions;

    // Flat array of extracted UVs (U, V)
    std::vector<float> uvs;

    // Flat array of decoded per-vertex colors (R, G, B, A), when COLOR0 is
    // indexed by the verified VCD. Constant register colors remain material
    // state and leave this array empty.
    std::vector<uint8_t> colors_rgba8;

    // Render indices for DX11 drawing
    // Each triangle is 3 consecutive indices pointing to combined vertex data
    std::vector<uint32_t> indices;

    // Diagnostic raw arrays (for OBJ export)
    struct VertexRef {
        uint32_t pos_idx = 0;
        uint32_t normal_idx = 0;
        uint32_t color_idx = 0;
        uint32_t uv_idx = 0;
        bool has_pos = false;
        bool has_normal = false;
        bool has_color = false;
        bool has_uv = false;
    };
    std::vector<VertexRef> raw_refs;

    struct Primitive {
        // GX primitive opcode with the VAT selector bits removed.
        uint8_t type = 0;
        size_t first_ref = 0;
        size_t ref_count = 0;
    };
    std::vector<Primitive> primitives;
};

struct GplTextureCommand {
    uint32_t command_index = 0;
    uint32_t image_index = 0;
    uint8_t state_selector = 0;
    uint8_t texture_unit = 0;
};

struct GplTargetMaterial {
    uint32_t command_index = 0;
    uint32_t tev_mode_word = 0;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0;
    bool uses_vertex_color = false;
};

struct GplDrawBatchAnalysis {
    uint32_t command_index = 0;
    uint32_t texture_image_index = 0;
    uint32_t vertex_descriptor = 0;
    GplMeshAnalysis mesh;
};

struct GplDrawSequenceAnalysis {
    std::vector<GplDrawBatchAnalysis> batches;
};

// Parses type-1 material commands from the first GPL section. The serialized
// layout and bit extraction are verified against GYWE41 FUN_801A3DB8,
// FUN_801A4078, FUN_801A50E8, and FUN_801A5F6C. Unsupported or malformed
// layouts fail closed.
bool gpl_parse_texture_commands_for_analysis(
    const GplFile& gpl, std::vector<GplTextureCommand>* out_commands);

// Evidence-limited material parser for the verified unlit ground layouts.
// GYWE41 FUN_801A4C94 selects a register color when Sub[1] has one element and
// COLOR0 when it has multiple elements. FUN_801A59C0 decodes the supported
// RGB565/RGBA4 values, while FUN_801A714C maps type-3 word 1 to texture RGBA
// multiplied by raster RGBA. Other layouts and TEV words fail closed.
bool gpl_parse_target_material_for_analysis(
    const GplFile& gpl, GplTargetMaterial* out_material);

// Replays the verified ground command subset in serialized order. Type-1
// attached ranges draw with the previously active texture before the command's
// new texture is installed, matching GYWE41 FUN_801A3000. Type 2 installs the
// VCD before drawing and type 3 installs verified TEV word 1 before drawing.
// Unknown state, missing prerequisites, malformed ranges, and unsupported
// command types fail closed.
bool gpl_parse_draw_sequence_for_analysis(
    const GplFile& gpl, const char* debug_name,
    GplDrawSequenceAnalysis* out_sequence);

// Evidence-limited parser for the first verified GPL section and its GX layouts:
// position/normal/UV INDEX8 (VCD 0x828), position/normal/COLOR0/UV INDEX8
// (VCD 0x8A8), and position INDEX16 with the remaining attributes INDEX8
// (VCD 0x8AC). VCD 0x8EC additionally changes COLOR0 to INDEX16. The
// VCD 0xCEC layout also changes Tex0 to INDEX16. The serialized type-2 command
// supplies the initial VCD word, display-list pointer, and exact byte length.
// Commands with additional attached draw ranges fail closed until the complete
// ordered command stream is modeled.
bool gpl_parse_display_list_for_analysis(const GplFile& gpl, const char* debug_name, GplMeshAnalysis* out_mesh);

} // namespace awl
