#pragma once

#include "awl/world_map_player_model_setup.h"
#include <variant>

namespace awl {
struct WorldMapPlayerVertexFormat {
    uint8_t attribute = 0, components = 0, type = 0, fraction = 0;
};
struct WorldMapPlayerArrayBinding {
    uint8_t attribute = 0, stride = 0;
    uint16_t count = 0;
    WorldMapModelResourceReference data;
    uint32_t bounded_size = 0; // Last addressed element, including interleaved strides.
};
struct WorldMapPlayerVertexDescriptor { uint8_t attribute = 0, mode = 0; };
struct WorldMapPlayerTextureParameters {
    WorldMapPlayerModelTextureBinding binding;
    uint8_t unit = 0;
    uint32_t wrap_s = 0, wrap_t = 0, min_filter = 0, mag_filter = 0, anisotropy = 0;
    bool mipmap = false, bias_clamp = false;
    uint8_t edge_lod = 0;
    float min_lod = 0, max_lod = 0, lod_bias = 0;
};
struct WorldMapPlayerTevParameters {
    // Supported configuration word 1: one stage modulates texture/raster.
    std::array<uint8_t,4> color_inputs{15,8,10,15}, alpha_inputs{7,4,5,7};
    uint8_t operation = 0, bias = 0, scale = 0, clamp = 1, output_register = 0;
    uint8_t stage = 0, coordinate = 0, map = 0, raster_channel = 4;
    uint8_t texgen = 0, texgen_type = 1, texgen_source = 4;
    uint8_t normalize = 0, post_matrix = 125;
    uint8_t texgens = 1, color_channels = 1, stages = 1;
};
using WorldMapPlayerDrawState = std::variant<WorldMapPlayerTextureParameters,
    std::vector<WorldMapPlayerVertexDescriptor>,WorldMapPlayerTevParameters>;
struct WorldMapPlayerDrawSection {
    std::vector<WorldMapPlayerArrayBinding> arrays; // Original GXSetArray call order.
    std::vector<WorldMapPlayerVertexFormat> formats; // Excludes terminator FF.
    std::optional<std::array<uint8_t,4>> constant_color; // GX channel 4, count-one branch.
    std::array<uint32_t,3> vat_words{}; // Original CP registers 70/80/90.
    std::array<uint8_t,9> matrix_indices{0,60,60,60,60,60,60,60,60};
    std::vector<WorldMapPlayerDrawState> commands; // Same indices as the retained setup plan.
};
enum class WorldMapPlayerDrawStatus {
    PreparedParameters, RequiresAssets, RequiresModelSetup, RequiresNormalFallback,
    RequiresTexture, UnsupportedLayout, InvalidInput, AllocationFailure,
};
// Immutable CPU drawing parameters with retained resource ownership. NOT a
// compiled GPU packet, live model, skinned geometry or accepted draw sequence.
// Display lists remain opaque in setup(); runtime ordering/submission is pending.
class WorldMapPlayerDrawCommands {
public:
    WorldMapPlayerDrawCommands(const WorldMapPlayerDrawCommands&) = delete;
    WorldMapPlayerDrawCommands& operator=(const WorldMapPlayerDrawCommands&) = delete;
    const WorldMapPlayerModelSetupPlan& setup() const { return *setup_; }
    const std::vector<WorldMapPlayerDrawSection>& sections() const { return sections_; }
private:
    WorldMapPlayerDrawCommands() = default;
    friend WorldMapPlayerDrawStatus prepare_world_map_player_draw_commands(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,std::unique_ptr<WorldMapPlayerDrawCommands>*);
    std::unique_ptr<WorldMapPlayerModelSetupPlan> setup_;
    std::vector<WorldMapPlayerDrawSection> sections_;
};
// 4C94/4350 array/VAT setup, 5950 VCD, 57D4 explicit selected texture
// parameters, and 50A0 configuration word 1. Null/default normals, palettes,
// other TEV words and unsafe extents stop explicitly. Failures preserve out.
[[nodiscard]] WorldMapPlayerDrawStatus prepare_world_map_player_draw_commands(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,
    std::unique_ptr<WorldMapPlayerDrawCommands>* out);
} // namespace awl
