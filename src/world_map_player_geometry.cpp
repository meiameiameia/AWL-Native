#include "awl/world_map_player_geometry.h"
#include "world_map_flat_archive.h"
#include <cmath>
#include <new>
#include <utility>

namespace awl {
namespace {
using Status=WorldMapPlayerGeometryStatus;
float component(const uint8_t* p,uint8_t fraction) {
    const uint32_t raw=(uint32_t(p[0])<<8)|p[1];
    const int value=raw<0x8000?int(raw):int(raw)-0x10000;
    return std::ldexp(float(value),-int(fraction));
}
} // namespace
WorldMapPlayerGeometryResult WorldMapPlayerGeometry::decode(
    const WorldMapPlayerSkinWork& work,const std::vector<uint8_t>& vertex_output) {
    std::vector<std::vector<WorldMapPlayerVertex>> staged;
    const auto result = stage_decode(work, vertex_output, &staged);
    if (result.status == Status::DecodedVertices) vertices_.swap(staged);
    return result;
}
WorldMapPlayerGeometryResult WorldMapPlayerGeometry::stage_decode(
    const WorldMapPlayerSkinWork& work,const std::vector<uint8_t>& vertex_output,
    std::vector<std::vector<WorldMapPlayerVertex>>* out) const {
    if (work.drawing().setup().selection().assets!=topology_.assets || vertex_output.size()!=output_size_) return {};
    WorldMapPlayerGeometryResult result;
    try {
        std::vector<std::vector<WorldMapPlayerVertex>> staged;
        staged.reserve(topology_.batches.size());
        for (const auto& batch:topology_.batches) {
            result.section=batch.section;result.command=batch.command;
            const auto& bindings=bindings_[batch.section];
            std::vector<WorldMapPlayerVertex> vertices;vertices.reserve(batch.references.size());
            for (const auto& reference:batch.references) {
                // GX's maximal position index denotes a skipped vertex; that
                // connectivity change is outside this retained topology path.
                if (((batch.descriptor_word>>2)&3u)==2 && reference.position==255) {
                    result.status=Status::UnsupportedLayout;return result;
                }
                const uint16_t indices[]{reference.position,reference.normal,reference.uv};
                WorldMapPlayerVertex vertex;
                for (size_t a=0;a<3;++a) {
                    const auto& binding=bindings[a];const uint32_t lanes=a==2?2u:3u;
                    const uint64_t offset=uint64_t(binding.offset)+uint64_t(indices[a])*binding.stride;
                    const uint64_t size=binding.skinned?vertex_output.size():gpl_.size;
                    if (indices[a]>=binding.count || !detail::archive_range(size,offset,lanes*2)) return result;
                    const auto* data=(binding.skinned?vertex_output.data():gpl_.data)+offset;
                    for (uint32_t lane=0;lane<lanes;++lane) {
                        const float value=component(data+lane*2,binding.fraction);
                        if (a==0) vertex.position[lane]=value;
                        else if (a==1) vertex.normal[lane]=value;
                        else vertex.uv[lane]=value;
                    }
                }
                vertices.push_back(vertex);
            }
            staged.push_back(std::move(vertices));
        }
        *out = std::move(staged);return {Status::DecodedVertices};
    } catch (const std::bad_alloc&) {result.status=Status::AllocationFailure;return result;}
}
WorldMapPlayerGeometryResult prepare_world_map_player_geometry(
    const WorldMapPlayerSkinWork& work,std::unique_ptr<WorldMapPlayerGeometry>* out) {
    if (!out) return {};
    WorldMapPlayerGeometryResult result;
    try {
        auto geometry=std::unique_ptr<WorldMapPlayerGeometry>(new WorldMapPlayerGeometry);
        const auto& drawing=work.drawing();const auto& plan=drawing.setup();
        const auto topology=prepare_world_map_player_topology(drawing,&geometry->topology_);
        if (topology.status!=WorldMapPlayerTopologyStatus::PreparedTopology) {
            return {topology.status==WorldMapPlayerTopologyStatus::AllocationFailure?Status::AllocationFailure:Status::RequiresTopology,
                topology.section,topology.command,topology.status};
        }
        geometry->gpl_=plan.selection().gpl;
        const auto& target=plan.auxiliary().skin_target();
        if (!target || target->data.bank_identity!=geometry->gpl_.reference.bank_identity ||
            target->data.offset<geometry->gpl_.reference.offset || target->size!=work.initial_output().size()) return result;
        const uint32_t start=target->data.offset-geometry->gpl_.reference.offset;
        const uint64_t end=uint64_t(start)+target->size;
        geometry->output_size_=target->size;
        for (size_t s=0;s<drawing.sections().size();++s) {
            result.section=static_cast<uint32_t>(s);result.status=Status::UnsupportedLayout;
            const auto& section=drawing.sections()[s];
            if (section.arrays.size()!=3 || section.formats.size()!=3) return result;
            std::array<WorldMapPlayerGeometry::Binding,3> bindings{};
            constexpr uint8_t attributes[]{9,10,13};
            for (size_t a=0;a<3;++a) {
                const WorldMapPlayerArrayBinding* array=nullptr;const WorldMapPlayerVertexFormat* format=nullptr;
                for (const auto& item:section.arrays) if (item.attribute==attributes[a]) {if (array) return result;array=&item;}
                for (const auto& item:section.formats) if (item.attribute==attributes[a]) {if (format) return result;format=&item;}
                const uint32_t lanes=a==2?2u:3u;
                if (!array || !format || format->type!=3 || format->components!=(a==1?0:1) ||
                    format->fraction>15 || array->stride<lanes*2 || array->data.bank_identity!=geometry->gpl_.reference.bank_identity ||
                    array->data.offset<geometry->gpl_.reference.offset) return result;
                const uint32_t offset=array->data.offset-geometry->gpl_.reference.offset;
                if (!detail::archive_range(geometry->gpl_.size,offset,array->bounded_size)) return result;
                const uint64_t array_end=uint64_t(offset)+array->bounded_size;
                const bool overlaps=offset<end && start<array_end;
                if (overlaps && (s!=target->section_index || a==2 || offset<start || array_end>end)) return result;
                bindings[a]={overlaps?offset-start:offset,array->stride,static_cast<uint8_t>(a==1?14:format->fraction),array->count,overlaps};
            }
            if (s==target->section_index && (!bindings[0].skinned || !bindings[1].skinned || bindings[0].offset!=0 ||
                bindings[1].offset!=6 || bindings[0].stride!=12 || bindings[1].stride!=12 || bindings[0].count!=bindings[1].count)) return result;
            geometry->bindings_.push_back(bindings);
        }
        const auto decoded=geometry->decode(work,work.initial_output());
        if (decoded.status!=Status::DecodedVertices) return decoded;
        *out=std::move(geometry);return {Status::PreparedGeometry};
    } catch (const std::bad_alloc&) {result.status=Status::AllocationFailure;return result;}
}
} // namespace awl
