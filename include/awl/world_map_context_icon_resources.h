#pragma once
#include "awl/tpl.h"
#include <array>
#include <memory>

namespace awl {
struct WorldMapContextIconGroup {
    uint32_t offset=0,frame_count=0,frame_table=0;
    uint16_t flags_8=0,value_a=0;
    uint32_t duration_c=0,value_10=0;
    uint32_t mode() const noexcept {return flags_8&0x8000?1u:flags_8&0x4000?2u:0u;}
    uint32_t parameter() const noexcept {return flags_8&0xfffu;}
};
struct WorldMapContextIconFrame {
    uint32_t offset=0,leaf_count=0,leaf_offset=0;
    std::array<int16_t,2> pair{};
};
enum class WorldMapContextIconResourcesStatus {
    Decoded,Loaded,InvalidInput,ReadFailure,UnsupportedAgc,UnsupportedTpl,AllocationFailure
};
// 82AB0/1811A8's immutable AGC + TPL ownership. Only the checked 8/20/12
// metadata hierarchy is interpreted; leaf geometry/texture selection and
// animation progression/drawing remain unsupported. No raw DOL retained.
class WorldMapContextIconResources {
public:
    uint64_t identity() const noexcept {return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));}
    uint32_t root_count() const noexcept;
    [[nodiscard]] bool root(uint32_t index,uint32_t* offset,uint32_t* groups) const noexcept;
    [[nodiscard]] bool group(uint32_t root,uint32_t index,WorldMapContextIconGroup*) const noexcept;
    [[nodiscard]] bool frame(uint32_t root,uint32_t group,uint32_t index,WorldMapContextIconFrame*) const noexcept;
    const TplFile& textures() const noexcept {return tpl_;}
private:
    WorldMapContextIconResources()=default;
    bool parse_agc(const uint8_t*,size_t);
    friend WorldMapContextIconResourcesStatus decode_world_map_context_icon_resources(
        const uint8_t*,size_t,const uint8_t*,size_t,std::shared_ptr<const WorldMapContextIconResources>*);
    std::vector<uint8_t> agc_;
    TplFile tpl_;
};
// Both sources are copied/owned together. Every failure preserves *out.
// Existing TPL parse/allocation stops share UnsupportedTpl and log their cause.
[[nodiscard]] WorldMapContextIconResourcesStatus decode_world_map_context_icon_resources(
    const uint8_t* agc,size_t agc_size,const uint8_t* tpl,size_t tpl_size,
    std::shared_ptr<const WorldMapContextIconResources>* out);
// Native file I/O for 82AB0's opguide.agc/opguide.tpl names; no GPU/window.
[[nodiscard]] WorldMapContextIconResourcesStatus load_world_map_context_icon_resources(
    std::shared_ptr<const WorldMapContextIconResources>* out);
} // namespace awl
