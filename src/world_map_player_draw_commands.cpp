#include "awl/world_map_player_draw_commands.h"
#include "world_map_flat_archive.h"
#include <algorithm>
#include <new>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapPlayerDrawStatus;
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
bool overlap(uint64_t a,uint64_t size,uint64_t b,uint64_t length) { return size && length && a < b+length && b < a+size; }
uint32_t scalar_width(uint8_t type) { return type < 2 ? 1u : type < 4 ? 2u : type == 4 ? 4u : 0u; }
uint32_t color_width(uint8_t type) { return type == 0 || type == 3 ? 2u : type == 1 || type == 4 ? 3u : type == 2 || type == 5 ? 4u : 0u; }

std::array<uint32_t,3> vat(const std::vector<WorldMapPlayerVertexFormat>& formats) {
    // 4350 starts with float XYZ/normal/ST, RGBA8 colors, byte dequant
    // enabled and the VAT-B bit 31 set; absent formats keep those defaults.
    std::array<WorldMapPlayerVertexFormat,12> f{};
    for (size_t i=0;i<f.size();++i) f[i]={static_cast<uint8_t>(9+i),1,4,0};
    f[1].components=0; f[2].type=5; f[3].type=5;
    bool nbt3=false;
    for (const auto& item:formats) {
        const uint8_t attr=item.attribute == 25 ? 10 : item.attribute;
        if (attr < 9 || attr > 20) continue;
        f[attr-9]=item;
        if (attr == 10 && item.components == 2) { f[1].components=1; nbt3=true; }
    }
    const auto bits=[&](size_t i) { return uint32_t(f[i].components) | (uint32_t(f[i].type)<<1) | (uint32_t(f[i].fraction)<<4); };
    const uint32_t a=bits(0) | (uint32_t(f[1].components)<<9) | (uint32_t(f[1].type)<<10) |
        (uint32_t(f[2].components)<<13) | (uint32_t(f[2].type)<<14) |
        (uint32_t(f[3].components)<<17) | (uint32_t(f[3].type)<<18) |
        (bits(4)<<21) | (1u<<30) | (uint32_t(nbt3)<<31);
    const uint32_t b=bits(5) | (bits(6)<<9) | (bits(7)<<18) |
        (uint32_t(f[8].components)<<27) | (uint32_t(f[8].type)<<28) | (1u<<31);
    const uint32_t c=f[8].fraction | (bits(9)<<5) | (bits(10)<<14) | (bits(11)<<23);
    return {a,b,c};
}

Status section_setup(const WorldMapPlayerModelSetupPlan& plan,size_t index,WorldMapPlayerDrawSection& out) {
    const auto& gpl=plan.auxiliary().gpl(); const auto& section=gpl.sections[index];
    const auto& view=plan.selection().gpl;
    uint32_t end=view.size;
    for (const auto& other:gpl.sections) {
        end=(std::min)(end,other.name_offset);
        if (other.offset>section.offset) end=(std::min)(end,other.offset);
    }
    const uint8_t uv_count=section.sub_offsets[2] ? view.data[section.offset+20] : 0;
    if (uv_count>8 || !section.position) return Status::UnsupportedLayout;
    // Relocation writes these headers in the original. Array payloads may
    // interleave with each other, but must not alias those writable regions.
    const auto safe=[&](uint32_t data,uint64_t size) {
        if (data<section.offset || !detail::archive_range(end,data,size)) return false;
        if (overlap(data,size,section.offset,section.sub_offsets[2]?21:20) ||
            overlap(data,size,section.material_offset,12)) return false;
        const uint32_t commands=detail::archive_be32(view.data+section.material_offset+4)+section.offset;
        if (overlap(data,size,commands,uint64_t(section.commands.size())*16)) return false;
        for (size_t i=0;i<4;++i) if (section.sub_offsets[i] &&
            overlap(data,size,section.sub_offsets[i],i==2?uint64_t(uv_count)*16:8)) return false;
        return true;
    };
    const auto bind=[&](uint32_t header,uint8_t attr,uint8_t components,uint32_t element_components,bool color) {
        const uint32_t relative=detail::archive_be32(view.data+header);
        const uint16_t count=be16(view.data+header+4);
        const uint8_t format=view.data[header+6],type=format>>4;
        const uint32_t width=color?color_width(type):scalar_width(type);
        if (!width || !relative || !count) return false;
        const uint8_t stride=static_cast<uint8_t>(color?width:uint32_t(view.data[header+7])*width);
        const uint64_t span=uint64_t(count-1)*stride+(color?width:element_components*width);
        const uint64_t data=uint64_t(section.offset)+relative;
        if (data>UINT32_MAX || !safe(static_cast<uint32_t>(data),span)) return false;
        out.arrays.push_back({attr,stride,count,{view.reference.bank_identity,view.reference.offset+static_cast<uint32_t>(data)},static_cast<uint32_t>(span)});
        out.formats.push_back({attr,components,type,static_cast<uint8_t>(color?0:format&15)});
        return true;
    };
    if (!bind(section.sub_offsets[0],9,1,3,false)) return Status::UnsupportedLayout;
    if (const auto header=section.sub_offsets[1]) {
        if (be16(view.data+header+4)==1) {
            const uint8_t type=view.data[header+6]>>4;
            if (type>5) return Status::UnsupportedLayout;
            const uint64_t data=uint64_t(section.offset)+detail::archive_be32(view.data+header);
            const uint32_t length=type==0 || type==3 || type==4?2u:4u;
            if (!detail::archive_be32(view.data+header) || data>UINT32_MAX || !safe(static_cast<uint32_t>(data),length)) return Status::UnsupportedLayout;
            const auto* p=view.data+data; const uint32_t v=be16(p);
            std::array<uint8_t,4> c{};
            if (type==0) c={uint8_t(((v>>11)<<3)|(v>>13)),uint8_t((((v>>5)&63)<<2)|((v>>9)&3)),uint8_t(((v&31)<<3)|((v>>2)&7)),255};
            else if (type==3) c={uint8_t(((v>>12)&15)*17),uint8_t(((v>>8)&15)*17),uint8_t(((v>>4)&15)*17),uint8_t((v&15)*17)};
            else if (type==4) c={0,uint8_t((v>>10)&0x3c),uint8_t(((v>>4)&0xfc)|((v>>10)&3)),uint8_t(((v<<2)&0xfc)|((v>>4)&3))};
            else c={p[0],p[1],p[2],type==5?p[3]:uint8_t(255)};
            out.constant_color=c;
        } else if (!bind(header,11,view.data[header+7]==3?0u:1u,0,true)) return Status::UnsupportedLayout;
    }
    for (uint8_t i=0;i<uv_count;++i) if (!bind(section.sub_offsets[2]+uint32_t(i)*16,static_cast<uint8_t>(13+i),1,2,false)) return Status::UnsupportedLayout;
    // Preserve 4C94's color-provider guard, even on the constant-color path.
    if (section.sub_offsets[3] && section.sub_offsets[1]) {
        const uint32_t header=section.sub_offsets[3];
        if (!detail::archive_be32(view.data+header)) return Status::RequiresNormalFallback;
        const uint8_t components=view.data[header+7];
        if (components==2) {
            // NBT3's array stride is THREE scalars, not header component 2.
            const uint8_t type=view.data[header+6]>>4; const uint32_t width=scalar_width(type);
            const uint16_t count=be16(view.data+header+4);
            const uint64_t data=uint64_t(section.offset)+detail::archive_be32(view.data+header);
            const uint64_t span=uint64_t(count)*3*width;
            if (!width || !count || data>UINT32_MAX || !safe(static_cast<uint32_t>(data),span)) return Status::UnsupportedLayout;
            out.arrays.push_back({25,static_cast<uint8_t>(3*width),count,{view.reference.bank_identity,view.reference.offset+static_cast<uint32_t>(data)},static_cast<uint32_t>(span)});
            out.formats.push_back({25,2,type,static_cast<uint8_t>(view.data[header+6]&15)});
        } else if ((components!=3 && components!=6) || !bind(header,10,0,3,false)) return Status::UnsupportedLayout;
    }
    out.vat_words=vat(out.formats); return Status::PreparedParameters;
}
} // namespace

WorldMapPlayerDrawStatus prepare_world_map_player_draw_commands(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,std::unique_ptr<WorldMapPlayerDrawCommands>* out) {
    if (!out) return Status::InvalidInput;
    if (!assets) return Status::RequiresAssets;
    try {
        auto result=std::unique_ptr<WorldMapPlayerDrawCommands>(new WorldMapPlayerDrawCommands);
        const auto setup=prepare_world_map_player_model_setup(assets,&result->setup_);
        if (setup==WorldMapPlayerModelSetupStatus::AllocationFailure) return Status::AllocationFailure;
        if (setup!=WorldMapPlayerModelSetupStatus::PreparedPlan) return Status::RequiresModelSetup;
        for (size_t i=0;i<result->setup_->command_sections().size();++i) {
            WorldMapPlayerDrawSection section;
            const auto prepared=section_setup(*result->setup_,i,section);
            if (prepared!=Status::PreparedParameters) return prepared;
            for (const auto& command:result->setup_->command_sections()[i].commands) {
                if (command.type==1) {
                    if (!command.texture) return Status::RequiresTexture;
                    const auto& binding=*command.texture;
                    const auto* texture=result->setup_->selection().assets->texture(binding.bank,binding.index);
                    if (!texture || texture->palette_header_offset) return Status::UnsupportedLayout;
                    const auto& t=texture->header;
                    section.commands.emplace_back(WorldMapPlayerTextureParameters{binding,static_cast<uint8_t>((command.word_4>>13)&7),
                        t.wrap_s,t.wrap_t,t.min_filter,t.mag_filter,0,t.min_lod!=t.max_lod,false,t.edge_lod,
                        float(t.min_lod),float(t.max_lod),t.lod_bias});
                } else if (command.type==2) {
                    std::vector<WorldMapPlayerVertexDescriptor> descriptors;
                    const auto append=[&](uint8_t attr,uint32_t mode) { if (mode) descriptors.push_back({attr,static_cast<uint8_t>(mode)}); };
                    append(0,command.word_4&3); append(25,(command.word_4>>26)&3);
                    for (uint8_t j=0;j<12;++j) append(static_cast<uint8_t>(9+j),(command.word_4>>(2+2*j))&3);
                    section.commands.emplace_back(std::move(descriptors));
                } else {
                    if (command.word_4!=1) return Status::UnsupportedLayout;
                    section.commands.emplace_back(WorldMapPlayerTevParameters{});
                }
            }
            result->sections_.push_back(std::move(section));
        }
        *out=std::move(result); return Status::PreparedParameters;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
