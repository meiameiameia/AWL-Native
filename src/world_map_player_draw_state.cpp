#include "awl/world_map_player_draw_state.h"
#include "world_map_matrix_math.h"
#include <cfenv>
#include <utility>

namespace awl {
using Status=WorldMapPlayerDrawStateStatus;
WorldMapPlayerDrawStateStatus prepare_world_map_model_channel_controls(
    uint32_t normal_capable,uint8_t material_source,const WorldMapModelLightingSnapshot& lighting,
    WorldMapModelChannelControls* out) {
    if (!out || material_source>1) return Status::InvalidInput;
    if ((normal_capable&255u) && !lighting.enabled) return Status::RequiresLightingState;
    WorldMapModelChannelControls staged;
    if (!(normal_capable&255u) || !*lighting.enabled) {
        staged.count=1;staged.calls[0]={4,0,0,material_source,0,0,2};
    } else {
        if (!lighting.color_mask || !lighting.alpha_mode) return Status::RequiresLightingState;
        staged.count=2;staged.calls[0]={0,1,0,material_source,*lighting.color_mask,2,1};
        const bool alpha=*lighting.alpha_mode==1 || *lighting.alpha_mode==2;
        if (alpha && !lighting.alpha_mask) return Status::RequiresLightingState;
        staged.calls[1]={2,static_cast<uint8_t>(alpha),0,material_source,alpha?*lighting.alpha_mask:0u,2,1};
    }
    *out=staged;return Status::PreparedChannels;
}
WorldMapPlayerDrawStateStatus prepare_world_map_player_feature_draw_state(
    const WorldMapPlayerFrameOwner& owner,const WorldMapPlayerGeometry& geometry,
    const WorldMapPlayerFeatureDrawInput& input,WorldMapPlayerFeatureDrawState* out) {
    if (!out) return Status::InvalidInput;
    WorldMapPlayerFeatureDrawState staged;
    if (!input.feature) {*out=std::move(staged);return Status::SkippedFeature;}
    const auto& plan=owner.work().drawing().setup();const uint32_t feature=*input.feature;
    if (plan.selection().assets!=geometry.topology().assets || feature>=plan.features().size() ||
        feature>=owner.feature_matrices().size()) return Status::InvalidInput;
    const auto& retained=plan.features()[feature];
    const auto raster=input.raster.value_or(WorldMapPlayerFeatureRasterSnapshot{retained.enabled_3c,retained.byte_3d,{}});
    if (!raster.enabled_3c) {*out=std::move(staged);return Status::SkippedFeature;}
    if (!input.view) return Status::RequiresView;
    if (std::fegetround()!=FE_TONEAREST) return Status::UnsupportedNumerics;
    staged.assets=plan.selection().assets;staged.feature=feature;staged.section=retained.group;
    staged.feature_matrix=owner.feature_matrices()[feature];
    if (!detail::concatenate(*input.view,staged.feature_matrix,&staged.model_view)) return Status::UnsupportedNumerics;
    const auto& section=plan.auxiliary().gpl().sections[retained.group];
    const bool normal_capable=section.sub_offsets[3] && section.sub_offsets[1];
    if (normal_capable) {
        staged.normal_matrix=std::array<float,9>{staged.model_view[0],staged.model_view[1],staged.model_view[2],
            staged.model_view[4],staged.model_view[5],staged.model_view[6],staged.model_view[8],staged.model_view[9],staged.model_view[10]};
    }
    uint8_t material_source=1;
    if (raster.override_3d) material_source=0;
    else if (section.sub_offsets[1]) {
        const auto* header=plan.selection().gpl.data+section.sub_offsets[1];
        if (((uint16_t(header[4])<<8)|header[5])==1) material_source=0;
    }
    const auto controls=prepare_world_map_model_channel_controls(normal_capable?1u:0u,material_source,input.lighting,&staged.channels);
    if (controls!=Status::PreparedChannels) return controls;
    if (!input.array_override || *input.array_override) return Status::RequiresArrayBinding;
    if (raster.override_3d) {
        if (!raster.color_3e) return Status::RequiresMaterialColor;
        staged.material_color=raster.color_3e;
    } else staged.material_color=owner.work().drawing().sections()[retained.group].constant_color;
    *out=std::move(staged);return Status::PreparedFeatureState;
}
} // namespace awl
