#include "awl/world_map_player_texture_features.h"
#include <cmath>
#include <cstring>

namespace awl {
namespace {
uint32_t word(const uint8_t* p) noexcept {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
}
float real(const uint8_t* p) noexcept { const auto w=word(p); float value; std::memcpy(&value,&w,4); return value; }
bool ordinary(float value) noexcept { return std::isfinite(value) && std::fpclassify(value)!=FP_SUBNORMAL; }
}
WorldMapTextureAnimationSampleStatus sample_world_map_texture_animation(
    const WorldMapPlayerTimerResourceView& resource, float value, WorldMapTextureAnimationSample* out) noexcept {
    using S=WorldMapTextureAnimationSampleStatus;
    if (!out || !resource.data) return S::InvalidInput;
    if (resource.size<8) return S::UnsupportedLayout;
    const auto count=word(resource.data);
    if (!count || uint64_t(count)*8+8>resource.size) return S::UnsupportedLayout;
    uint32_t current=count-1;
    for (uint32_t i=0;i+1<count;++i) {
        const float next_time=real(resource.data+size_t(i)*8+20);
        if (!ordinary(value) || !ordinary(next_time)) return S::UnsupportedNumerics;
        if (value<next_time) { current=i; break; }
    }
    *out={current,current+1<count?current+1:current,word(resource.data+size_t(current)*8+8)};
    return S::Sampled;
}
WorldMapPlayerTextureFeatureResult prepare_world_map_player_texture_features(
    const WorldMapPlayerTextureFeatureInput& input, const WorldMapPlayerModelAssets& assets) noexcept {
    using S=WorldMapPlayerTextureFeatureStatus;
    WorldMapPlayerTextureFeatureResult result;
    if (!input.feature) { result.status=S::RequiresFeature; return result; }
    const auto& feature=*input.feature;
    uint32_t indices[]{feature.type_0,feature.type_4};
    const uint32_t thresholds[]{0x3a,0x24};
    const WorldMapAnimationFeatureTimer* timers[]{&feature.first_14,&feature.second_24};
    for (uint32_t channel=0;channel<2;++channel) {
        const bool sampled=feature.index_8<=INT32_MAX || (indices[channel]<=INT32_MAX && indices[channel]>=thresholds[channel]);
        if (!sampled) continue;
        result.failed_channel=channel;
        if (!input.timers) { result.status=S::RequiresTimerAssets; return result; }
        WorldMapPlayerTimerResourceView resource;
        if (!input.timers->find_resource(channel?WorldMapPlayerTimerBank::Mouth:WorldMapPlayerTimerBank::Eyes,
            timers[channel]->resource_0,&resource)) { result.status=S::RequiresResource; return result; }
        WorldMapTextureAnimationSample sample;
        const auto status=sample_world_map_texture_animation(resource,timers[channel]->value_8,&sample);
        if (status!=WorldMapTextureAnimationSampleStatus::Sampled) {
            result.status=status==WorldMapTextureAnimationSampleStatus::UnsupportedLayout?S::UnsupportedLayout:
                status==WorldMapTextureAnimationSampleStatus::UnsupportedNumerics?S::UnsupportedNumerics:S::InvalidInput;
            return result;
        }
        indices[channel]=sample.texture_index;
    }
    const auto select=[&](uint32_t channel,WorldMapPlayerTextureBank bank,uint32_t index) {
        result.failed_channel=channel;
        if (index>UINT16_MAX || !assets.texture(bank,static_cast<uint16_t>(index))) { result.status=S::InvalidTexture; return false; }
        result.textures[channel]=WorldMapPlayerModelTextureBinding{static_cast<uint8_t>(channel),bank,
            static_cast<uint16_t>(index),assets.texture_bank_identity(bank)};
        return true;
    };
    if (!select(0,WorldMapPlayerTextureBank::Eyes,indices[0]) || !select(1,WorldMapPlayerTextureBank::Mouth,indices[1])) return result;
    result.failed_channel.reset();
    if (!input.source_10) { result.status=S::RequiresSource; return result; }
    if (*input.source_10==0x16) {
        const auto body=indices[0]>=0x37 && indices[0]<0x3a?3u:indices[0]==0xf?2u:1u;
        if (!select(2,WorldMapPlayerTextureBank::Body,body)) return result;
    }
    result.failed_channel.reset(); result.status=S::Selected; return result;
}
bool prepare_world_map_player_texture_cache(const WorldMapPlayerModelSetupPlan& plan,
    const WorldMapPlayerTextureFeatureResult& selected,
    const std::optional<std::array<WorldMapPlayerTextureCacheEntry,4>>& before,
    std::optional<std::array<WorldMapPlayerTextureCacheEntry,4>>* out,uint32_t* writes) noexcept {
    if (!out || !writes || selected.status!=WorldMapPlayerTextureFeatureStatus::Selected ||
        !selected.textures[0] || !selected.textures[1] ||
        before.has_value()!=plan.cache_channels().has_value()) return false;
    for (size_t channel=0;channel<selected.textures.size();++channel) if (selected.textures[channel]) {
        const auto& t=*selected.textures[channel];
        const auto bank=channel==0?WorldMapPlayerTextureBank::Eyes:channel==1?WorldMapPlayerTextureBank::Mouth:WorldMapPlayerTextureBank::Body;
        if (t.channel!=channel || t.bank!=bank || t.bank_identity!=plan.selection().assets->texture_bank_identity(bank) ||
            !plan.selection().assets->texture(bank,t.index)) return false;
    }
    auto cache=before; uint32_t count=0;
    const auto apply=[&](std::optional<uint32_t> node,const WorldMapPlayerModelTextureBinding& texture) {
        for (const auto& feature:plan.features()) if (feature.node==node) {
            for (const auto& command:plan.command_sections()[feature.group].commands) {
                if (command.type!=1 || command.channel!=texture.channel || !cache) continue;
                size_t slot=cache->size();
                for (size_t i=0;i<cache->size();++i) if ((*cache)[i].channel==texture.channel) { slot=i; break; }
                if (slot==cache->size()) for (size_t i=0;i<cache->size();++i)
                    if ((*cache)[i].channel==UINT32_MAX) { slot=i; break; }
                if (slot==cache->size()) continue;
                (*cache)[slot]={texture.channel,texture}; ++count;
            }
        }
    };
    for (const auto& texture:selected.textures) if (texture) {
        apply(std::nullopt,*texture);
        for (const auto node:plan.feature_node_order()) apply(node,*texture);
    }
    *out=cache; *writes=count; return true;
}
} // namespace awl
