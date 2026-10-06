#include "awl/world_map_player_topology.h"
#include "world_map_flat_archive.h"
#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace awl {
namespace {
using Status=WorldMapPlayerTopologyStatus;
uint16_t be16(const uint8_t* p) {return uint16_t((uint16_t(p[0])<<8)|p[1]);}
bool overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m) {return n && m && a<b+m && b<a+n;}
Status decode(const uint8_t* data,uint32_t size,const WorldMapPlayerDrawSection& section,
    const std::vector<WorldMapPlayerVertexDescriptor>& descriptors,WorldMapPlayerTopologyBatch& batch) {
    constexpr uint8_t attributes[]{9,10,13};
    if (descriptors.size()!=3) return Status::UnsupportedLayout;
    std::array<uint8_t,3> widths{};std::array<uint16_t,3> counts{};
    uint32_t width=0;
    for (size_t i=0;i<3;++i) {
        if (descriptors[i].attribute!=attributes[i] || (descriptors[i].mode!=2 && descriptors[i].mode!=3)) return Status::UnsupportedLayout;
        widths[i]=descriptors[i].mode==2?1u:2u;width+=widths[i];
        bool found=false;
        for (const auto& array:section.arrays) if (array.attribute==attributes[i]) {
            if (found || !array.count) return Status::UnsupportedLayout;
            found=true;counts[i]=array.count;
        }
        if (!found) return Status::UnsupportedLayout;
    }
    uint32_t cursor=0;
    while (cursor<size) {
        const uint8_t op=data[cursor++];
        if (!op) {++batch.nop_bytes;continue;}
        if ((op&7u) || (op!=0x80 && op!=0x88 && op!=0x90 && op!=0x98 && op!=0xa0)) return Status::UnsupportedLayout;
        if (!detail::archive_range(size,cursor,2)) return Status::InvalidInput;
        const uint32_t count=be16(data+cursor);cursor+=2;
        if (!count || ((op==0x80 || op==0x88)?count%4:op==0x90?count%3:count<3)) return Status::UnsupportedLayout;
        if (!detail::archive_range(size,cursor,uint64_t(count)*width) || batch.references.size()>UINT32_MAX-count) return Status::InvalidInput;
        const auto first=static_cast<uint32_t>(batch.references.size());
        batch.primitives.push_back({op,first,count});
        for (uint32_t vertex=0;vertex<count;++vertex) {
            std::array<uint16_t,3> indices{};
            for (size_t i=0;i<3;++i) {
                indices[i]=widths[i]==1?data[cursor]:be16(data+cursor);cursor+=widths[i];
                if (indices[i]>=counts[i]) return Status::InvalidInput;
            }
            batch.references.push_back({indices[0],indices[1],indices[2]});
        }
        const auto triangle=[&](uint32_t a,uint32_t b,uint32_t c) {
            batch.triangle_indices.push_back(first+a);batch.triangle_indices.push_back(first+b);batch.triangle_indices.push_back(first+c);
        };
        if (op==0x90) for (uint32_t i=0;i<count;i+=3) triangle(i,i+1,i+2);
        else if (op==0x80 || op==0x88) for (uint32_t i=0;i<count;i+=4) {triangle(i,i+1,i+2);triangle(i,i+2,i+3);}
        else if (op==0x98) for (uint32_t i=0;i+2<count;++i) {if (i&1u) triangle(i+1,i,i+2);else triangle(i,i+1,i+2);}
        else for (uint32_t i=1;i+1<count;++i) triangle(0,i,i+1);
    }
    return batch.primitives.empty()?Status::UnsupportedLayout:Status::PreparedTopology;
}
} // namespace
WorldMapPlayerTopologyResult prepare_world_map_player_topology(
    const WorldMapPlayerDrawCommands& drawing,WorldMapPlayerTopology* out) {
    if (!out) return {};
    const auto& plan=drawing.setup();const auto& view=plan.selection().gpl;
    const auto& metadata=plan.auxiliary().gpl();
    if (!view.data || metadata.sections.size()!=drawing.sections().size() || metadata.sections.size()!=plan.command_sections().size()) return {};
    WorldMapPlayerTopologyResult result;
    try {
        WorldMapPlayerTopology staged;staged.assets=plan.selection().assets;
        for (size_t i=0;i<metadata.sections.size();++i) {
            result.section=static_cast<uint32_t>(i);result.command=0;
            const auto& source=metadata.sections[i];const auto& section=drawing.sections()[i];const auto& commands=plan.command_sections()[i].commands;
            if (section.commands.size()!=commands.size()) return result;
            uint32_t end=view.size;
            for (const auto& other:metadata.sections) {end=(std::min)(end,other.name_offset);if (other.offset>source.offset) end=(std::min)(end,other.offset);}
            if (source.offset>=end || !detail::archive_range(end,source.material_offset,12)) return result;
            const uint32_t first_range=detail::archive_be32(view.data+source.material_offset);
            std::optional<WorldMapPlayerTextureParameters> texture;
            const std::vector<WorldMapPlayerVertexDescriptor>* descriptors=nullptr;
            bool tev=false,has_range=false;uint32_t descriptor_word=0;
            for (size_t j=0;j<commands.size();++j) {
                result.command=static_cast<uint32_t>(j);const auto& command=commands[j];const auto& state=section.commands[j];
                if (command.type==2) {descriptors=std::get_if<std::vector<WorldMapPlayerVertexDescriptor>>(&state);descriptor_word=command.word_4;if (!descriptors) return result;}
                else if (command.type==3) {if (!std::get_if<WorldMapPlayerTevParameters>(&state)) return result;tev=true;}
                else if (command.type!=1) {result.status=Status::UnsupportedLayout;return result;}
                if (command.display_list.has_value()!=(command.display_list_size!=0)) return result;
                if (command.display_list) {
                    if (!descriptors) {result.status=Status::RequiresDescriptor;return result;}
                    if (!texture) {result.status=Status::RequiresTexture;return result;}
                    if (!tev) {result.status=Status::RequiresTev;return result;}
                    const auto& ref=*command.display_list;
                    if (ref.bank_identity!=view.reference.bank_identity || ref.offset<view.reference.offset) return result;
                    const uint32_t start=ref.offset-view.reference.offset,length=command.display_list_size;
                    if (start<source.offset || !detail::archive_range(end,start,length) || (!has_range && uint64_t(source.offset)+first_range!=start)) return result;
                    // Retain the shared immutable reader's non-aliasing policy.
                    if (overlaps(start,length,source.offset,source.sub_offsets[2]?21u:20u) || overlaps(start,length,source.material_offset,12)) return result;
                    const uint64_t table=uint64_t(source.offset)+detail::archive_be32(view.data+source.material_offset+4);
                    if (overlaps(start,length,table,uint64_t(commands.size())*16)) return result;
                    for (size_t sub=0;sub<4;++sub) if (source.sub_offsets[sub]) {
                        const uint32_t size=sub==2?uint32_t(view.data[source.offset+20])*16u:8u;
                        if (overlaps(start,length,source.sub_offsets[sub],size)) return result;
                    }
                    for (const auto& array:section.arrays) {
                        if (array.data.bank_identity!=view.reference.bank_identity || array.data.offset<view.reference.offset ||
                            overlaps(start,length,array.data.offset-view.reference.offset,array.bounded_size)) return result;
                    }
                    WorldMapPlayerTopologyBatch batch;batch.section=result.section;batch.command=result.command;batch.display_list=ref;
                    batch.display_list_size=length;batch.texture=*texture;batch.descriptor_word=descriptor_word;
                    const auto status=decode(view.data+start,length,section,*descriptors,batch);
                    if (status!=Status::PreparedTopology) {result.status=status;return result;}
                    staged.batches.push_back(std::move(batch));has_range=true;
                }
                if (command.type==1) {const auto* next=std::get_if<WorldMapPlayerTextureParameters>(&state);if (!next) return result;texture=*next;}
            }
        }
        if (staged.batches.empty()) {result.status=Status::UnsupportedLayout;return result;}
        *out=std::move(staged);return {Status::PreparedTopology};
    } catch (const std::bad_alloc&) {result.status=Status::AllocationFailure;return result;}
}
} // namespace awl
