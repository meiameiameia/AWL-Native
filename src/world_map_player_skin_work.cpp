#include "awl/world_map_player_skin_work.h"
#include "world_map_flat_archive.h"
#include <cmath>
#include <cfenv>
#include <cstring>
#include <new>
#include <utility>

namespace awl {
namespace {
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0])<<8)|p[1]); }
uint32_t round32(uint32_t size) { return (size+31u)&~31u; }
bool overlap(uint64_t a,uint64_t size,uint64_t b,uint64_t length) { return size && length && a<b+length && b<a+size; }
bool supported(float value) { return std::isfinite(value) && std::fpclassify(value)!=FP_SUBNORMAL; }
float vertex_component(const uint8_t* p,int scale) {
    const uint16_t raw=be16(p);
    return std::ldexp(float(raw<0x8000?int(raw):int(raw)-0x10000),-scale);
}
// Each ps_madd rounds once to single precision; separating multiply/add changes
// quantized results. Only the six stored lanes are evaluated on the CPU.
bool transform(const WorldMapModelMatrix& matrix,const std::array<float,6>& input,std::array<float,6>* result) {
    for (size_t axis=0;axis<3;++axis) {
        const size_t row=axis*4;
        float position=std::fma(matrix[row],input[0],matrix[row+3]);
        float normal=matrix[row]*input[3];
        if (!supported(position) || !supported(normal)) return false;
        position=std::fma(matrix[row+1],input[1],position);
        normal=std::fma(matrix[row+1],input[4],normal);
        if (!supported(position) || !supported(normal)) return false;
        position=std::fma(matrix[row+2],input[2],position);
        normal=std::fma(matrix[row+2],input[5],normal);
        if (!supported(position) || !supported(normal)) return false;
        (*result)[axis]=position; (*result)[axis+3]=normal;
    }
    return true;
}
bool store_vertex(uint8_t* destination,const std::array<float,6>& value,int scale) {
    for (size_t i=0;i<6;++i) {
        if (!supported(value[i])) return false;
        // Power-of-two scaling is exact unless it overflows; saturation handles
        // that case before integer conversion. Integer conversion truncates.
        const double scaled=std::ldexp(double(value[i]),scale);
        const int encoded=scaled>=32767?32767:scaled<=-32768?-32768:int(scaled);
        const uint16_t raw=uint16_t(encoded);
        destination[i*2]=uint8_t(raw>>8); destination[i*2+1]=uint8_t(raw);
    }
    return true;
}
} // namespace
bool reorder_world_map_player_skin_matrix(const WorldMapModelMatrix& frame_matrix,WorldMapModelMatrix* out) {
    if (!out) return false;
    for (float value:frame_matrix) if (!std::isfinite(value)) return false;
    const WorldMapModelMatrix reordered{frame_matrix[0],frame_matrix[4],frame_matrix[8],frame_matrix[1],frame_matrix[5],frame_matrix[9],
        frame_matrix[2],frame_matrix[6],frame_matrix[10],frame_matrix[3],frame_matrix[7],frame_matrix[11]};
    *out=reordered; return true;
}
WorldMapPlayerSkinWorkStatus prepare_world_map_player_skin_work(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,std::unique_ptr<WorldMapPlayerSkinWork>* out) {
    using Status=WorldMapPlayerSkinWorkStatus;
    if (!out) return Status::InvalidInput;
    if (!assets) return Status::RequiresAssets;
    try {
        auto work=std::unique_ptr<WorldMapPlayerSkinWork>(new WorldMapPlayerSkinWork);
        const auto drawing=prepare_world_map_player_draw_commands(assets,&work->drawing_);
        if (drawing==WorldMapPlayerDrawStatus::AllocationFailure) return Status::AllocationFailure;
        if (drawing!=WorldMapPlayerDrawStatus::PreparedParameters) return Status::RequiresDrawingPreparation;
        const auto& setup=work->drawing_->setup(); const auto& auxiliary=setup.auxiliary();
        const auto& target=auxiliary.skin_target(); const auto& metadata=auxiliary.skin();
        const auto& view=setup.selection().skin;
        if (!target || !target->size) return Status::RequiresSkinTarget;
        if (target->data.offset%32) return Status::UnsupportedLayout;
        // GQR scales are six-bit signed fields; admit the positive SKN scales
        // only. Other encodings need their numerical behavior translated first.
        if (metadata.control_6>31) return Status::UnsupportedLayout;
        const uint32_t scale=metadata.control_6;
        work->vertex_quantization_=((scale<<8)|7u)*0x10001u;
        const uint32_t header_size=metadata.counts[2]?36u:32u;
        if (view.size<header_size) return Status::UnsupportedLayout;
        constexpr uint32_t strides[]{0x40,0x74,0x44};
        const auto safe=[&](uint32_t offset,uint32_t size) {
            if (!detail::archive_range(view.size,offset,size) || overlap(offset,size,0,header_size)) return false;
            for (size_t i=0;i<3;++i) if (metadata.tables[i] &&
                overlap(offset,size,*metadata.tables[i],uint64_t(metadata.counts[i])*strides[i])) return false;
            return true;
        };
        const auto word=[&](uint32_t at) { return detail::archive_be32(view.data+at); };
        const auto output=[&](uint32_t offset,uint32_t size) { return detail::archive_range(target->size,offset,size); };
        for (size_t kind=0;kind<3;++kind) {
            if (metadata.tables[kind] && metadata.counts[kind] && *metadata.tables[kind]<header_size) return Status::UnsupportedLayout;
            for (uint32_t i=0;i<metadata.counts[kind];++i) {
                const uint32_t at=*metadata.tables[kind]+i*strides[kind];
                WorldMapPlayerSkinJob job; job.kind=static_cast<WorldMapPlayerSkinJobKind>(kind); job.record_offset=at;
                const uint32_t count_at=kind==0?0x3a:kind==1?0x70:0x42;
                const uint32_t index_at=kind==0?0x38:kind==1?0x6c:0x40;
                job.vertex_count=be16(view.data+at+count_at);
                job.prefix=kind==2?0:view.data[at+count_at+2];
                // C734/C840 use count-1 in a do/while CTR loop: counts <2
                // are unsafe, whereas CA60's direct CTR loop admits one.
                if (job.vertex_count<(kind==2?1:2) || job.prefix%4 || job.prefix>28) return Status::UnsupportedLayout;
                for (uint32_t m=0;m<(kind==1?2u:1u);++m) {
                    const uint16_t index=be16(view.data+at+index_at+m*2);
                    if (index>=setup.core_before_features().nodes.size()) return Status::UnsupportedLayout;
                    job.matrix_indices.push_back(index);
                }
                const uint32_t source_at=kind==1?0x60:0x30;
                const uint32_t bytes=uint32_t(job.vertex_count)*12u+job.prefix;
                job.source={word(at+source_at),round32(bytes)};
                if (job.source.size>0x1000 || job.source.offset%32 || !safe(job.source.offset,job.source.size)) return Status::UnsupportedLayout;
                if (kind==1 || kind==2) {
                    const uint32_t weights_at=kind==1?0x64:0x3c;
                    job.weights=WorldMapPlayerSkinSpan{word(at+weights_at),round32(uint32_t(job.vertex_count)*(kind==1?2u:1u))};
                    if (job.weights->size>(kind==1?0x1000u:0x800u) || job.weights->offset%32 || !safe(job.weights->offset,job.weights->size)) return Status::UnsupportedLayout;
                }
                const uint32_t output_at=kind==0?0x34:kind==1?0x68:0x38;
                job.output={word(at+output_at),kind==2?0u:job.source.size};
                if (!output(job.output.offset,job.output.size)) return Status::UnsupportedLayout;
                if (kind!=2) {
                    // Native CPU jobs preserve complete DMA blocks. Unaligned
                    // starts require a separate original alignment trace.
                    if (job.source.offset%32 || job.output.offset%32) return Status::UnsupportedLayout;
                } else {
                    job.indices=WorldMapPlayerSkinSpan{word(at+0x34),round32(uint32_t(job.vertex_count)*2u)};
                    if (job.indices->size>0x800 || job.indices->offset%32 || !safe(job.indices->offset,job.indices->size)) return Status::UnsupportedLayout;
                    for (uint32_t v=0;v<job.vertex_count;++v) {
                        const uint16_t index=be16(view.data+job.indices->offset+v*2);
                        if (!output(job.output.offset,uint32_t(index)*12u+12u)) return Status::UnsupportedLayout;
                        job.scatter_indices.push_back(index);
                    }
                }
                work->jobs_.push_back(std::move(job));
            }
        }
        if (metadata.counts[2]) {
            if (metadata.word_18) {
                // C718's dcbz clears count rounded UP blocks at a cache-line
                // aligned address. Actual offset 21672 aligns DOWN to 21664.
                // Adding size to the unaligned offset would falsely overrun.
                if (metadata.word_18>UINT32_MAX-31u) return Status::UnsupportedLayout;
                const WorldMapPlayerSkinSpan clear{metadata.word_14&~31u,round32(metadata.word_18)};
                if (!output(clear.offset,clear.size)) return Status::UnsupportedLayout;
                work->clear_=clear;
            }
            const uint32_t count=word(32);
            if (count) {
                if (!metadata.pointer_1c || count>view.size/2 || !safe(*metadata.pointer_1c,count*2)) return Status::UnsupportedLayout;
                const auto& first=work->jobs_[size_t(metadata.counts[0])+metadata.counts[1]];
                for (uint32_t i=0;i<count;++i) {
                    const uint16_t index=be16(view.data+*metadata.pointer_1c+i*2);
                    if (!output(first.output.offset,uint32_t(index)*12u+12u)) return Status::UnsupportedLayout;
                    work->flush_indices_.push_back(index);
                }
            }
        }
        const auto& gpl=setup.selection().gpl;
        const uint32_t offset=target->data.offset-gpl.reference.offset;
        if (!detail::archive_range(gpl.size,offset,target->size)) return Status::UnsupportedLayout;
        work->output_.assign(gpl.data+offset,gpl.data+offset+target->size);
        work->workspace_.resize(WorldMapPlayerSkinWork::workspace_size+31u);
        work->workspace_offset_=(32u-(reinterpret_cast<uintptr_t>(work->workspace_.data())&31u))&31u;
        *out=std::move(work); return Status::PreparedWork;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
WorldMapPlayerSkinExecutionStatus execute_world_map_player_skin_work(
    const WorldMapPlayerSkinWork& work,const std::vector<WorldMapModelMatrix>& frame_palette,
    const std::vector<uint8_t>& prior_output,std::vector<uint8_t>* out) {
    using Status=WorldMapPlayerSkinExecutionStatus;
    if (!out || prior_output.size()!=work.initial_output().size() ||
        frame_palette.size()!=work.drawing().setup().core_before_features().nodes.size()) return Status::InvalidInput;
    if (std::fegetround()!=FE_TONEAREST) return Status::UnsupportedNumerics;
    for (const auto& matrix:frame_palette) for (float value:matrix) if (!supported(value)) return Status::UnsupportedNumerics;
    try {
        auto staged=prior_output;
        std::array<uint8_t,0x1000> block{};
        const int scale=int((work.vertex_quantization()>>8)&31u);
        const auto* skin=work.drawing().setup().selection().skin.data;
        bool additive_started=false;
        for (const auto& job:work.jobs()) {
            if (job.kind==WorldMapPlayerSkinJobKind::Accumulate && !additive_started) {
                additive_started=true;
                if (work.clear_before_accumulation()) {
                    const auto clear=*work.clear_before_accumulation();
                    std::memset(staged.data()+clear.offset,0,clear.size);
                }
            }
            // Preserve the original rounded copy's prefix and padding bytes.
            std::memcpy(block.data(),skin+job.source.offset,job.source.size);
            for (size_t vertex=0;vertex<job.vertex_count;++vertex) {
                std::array<float,6> input{},value{},second{};
                const size_t offset=job.prefix+vertex*12;
                for (size_t i=0;i<6;++i) input[i]=vertex_component(block.data()+offset+i*2,scale);
                if (!transform(frame_palette[job.matrix_indices[0]],input,&value)) return Status::UnsupportedNumerics;
                uint8_t* destination=block.data()+offset;
                if (job.kind==WorldMapPlayerSkinJobKind::BlendTwo) {
                    if (!transform(frame_palette[job.matrix_indices[1]],input,&second)) return Status::UnsupportedNumerics;
                    const float first_weight=float(skin[job.weights->offset+vertex*2])/256.0f;
                    const float second_weight=float(skin[job.weights->offset+vertex*2+1])/256.0f;
                    for (size_t i=0;i<6;++i) {
                        const float weighted=value[i]*first_weight;
                        if (!supported(weighted)) return Status::UnsupportedNumerics;
                        value[i]=std::fma(second[i],second_weight,weighted);
                    }
                } else if (job.kind==WorldMapPlayerSkinJobKind::Accumulate) {
                    destination=staged.data()+job.output.offset+size_t(job.scatter_indices[vertex])*12;
                    const float weight=float(skin[job.weights->offset+vertex])/256.0f;
                    for (size_t i=0;i<6;++i) value[i]=std::fma(value[i],weight,vertex_component(destination+i*2,scale));
                }
                if (!store_vertex(destination,value,scale)) return Status::UnsupportedNumerics;
            }
            if (job.kind!=WorldMapPlayerSkinJobKind::Accumulate)
                std::memcpy(staged.data()+job.output.offset,block.data(),job.output.size);
        }
        *out=std::move(staged); return Status::Executed;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
