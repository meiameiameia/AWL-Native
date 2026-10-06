#pragma once
#include "awl/world_map_player_geometry.h"
#include "awl/world_map_player_frame.h"

namespace awl {
struct WorldMapModelLightingSnapshot {
    std::optional<uint8_t> enabled; // r13-5460, not inferred from model normals.
    std::optional<uint32_t> color_mask,alpha_mask; // r13-5474 / -546C.
    std::optional<int32_t> alpha_mode; // r13-545C; only modes 1/2 enable alpha.
};
struct WorldMapModelChannelControl {
    uint8_t channel=0,enabled=0,ambient_source=0,material_source=0;
    uint32_t light_mask=0;
    uint8_t diffuse=0,attenuation=0;
};
struct WorldMapModelChannelControls {
    std::array<WorldMapModelChannelControl,2> calls{};
    uint8_t count=0; // Ordered GXSetChanCtrl operands, not a lighting result.
};
enum class WorldMapPlayerDrawStateStatus {
    PreparedChannels,PreparedFeatureState,SkippedFeature,RequiresView,
    RequiresLightingState,RequiresArrayBinding,RequiresMaterialColor,
    InvalidInput,UnsupportedNumerics,
};
// Complete 7BA4 channel-control selection. normal_capable's low byte gates
// lighting; only reached snapshot fields are required. Sources 0/1 only.
// Other alpha modes disable alpha, preserving diffuse/attenuation operands.
// Every stop preserves out; light objects, ambient colors and XF arithmetic
// are not evaluated by these call operands.
[[nodiscard]] WorldMapPlayerDrawStateStatus prepare_world_map_model_channel_controls(
    uint32_t normal_capable,uint8_t material_source,const WorldMapModelLightingSnapshot& lighting,
    WorldMapModelChannelControls* out);

struct WorldMapPlayerFeatureRasterSnapshot {
    uint8_t enabled_3c=1,override_3d=0;
    std::optional<std::array<uint8_t,4>> color_3e;
};
struct WorldMapPlayerFeatureDrawInput {
    std::optional<uint32_t> feature; // Known null skips without other observations.
    std::optional<WorldMapPlayerFeatureRasterSnapshot> raster; // Absent uses retained constructor values.
    std::optional<WorldMapModelMatrix> view;
    WorldMapModelLightingSnapshot lighting;
    // 369C reads r13-5490, independently of feature +34. A known null
    // keeps compiled source arrays; nonnull requires an untranslated binding.
    std::optional<uint64_t> array_override;
};
struct WorldMapPlayerFeatureDrawState {
    std::shared_ptr<const WorldMapPlayerModelAssets> assets;
    uint32_t feature=0,section=0;
    WorldMapModelMatrix feature_matrix{},model_view{};
    std::optional<std::array<float,9>> normal_matrix;
    uint8_t matrix_index=0;
    WorldMapModelChannelControls channels;
    std::optional<std::array<uint8_t,4>> material_color;
};
// Isolates 2CFC's enabled/resource gates, feature/view composition, normal
// matrix and channel selection, known-null array override and material color.
// Uses retained owner features/providers and geometry; no feature ordering,
// compiled packets, global bookkeeping, light payloads, GPU or live draw gates.
// Does not apply an inverse transpose or normalize normals: the reached DOL
// loads the combined matrix's 3x3 directly. Only this feature's section is selected.
// Disabled/null features ignore view/lighting/color/array observations.
// Every failure preserves out. PreparedFeatureState is a CPU proposal only.
[[nodiscard]] WorldMapPlayerDrawStateStatus prepare_world_map_player_feature_draw_state(
    const WorldMapPlayerFrameOwner& owner,const WorldMapPlayerGeometry& geometry,
    const WorldMapPlayerFeatureDrawInput& input,WorldMapPlayerFeatureDrawState* out);
} // namespace awl
