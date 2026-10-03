#include "awl/world_map_model_initialization.h"
#include "awl/world_map_animation_initializer.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace awl {
namespace {
bool finite(const WorldMapModelMatrix& matrix) {
    return std::all_of(matrix.begin(),matrix.end(),[](float v){return std::isfinite(v);});
}
WorldMapModelMatrix concatenate(const WorldMapModelMatrix& a,const WorldMapModelMatrix& b) {
    WorldMapModelMatrix result{};
    for(size_t row=0;row<3;++row)for(size_t column=0;column<4;++column) {
        float sum=a[row*4]*b[column];
        sum=std::fma(a[row*4+1],b[4+column],sum);
        sum=std::fma(a[row*4+2],b[8+column],sum);
        if(column>=2)sum=std::fma(a[row*4+3],column==3?1.0f:0.0f,sum);
        result[row*4+column]=sum;
    }
    return result;
}
WorldMapModelCoreStatus invert(const WorldMapModelMatrix& m,WorldMapModelMatrix* out) {
    // FUN_801B7E88 cofactor pairs and determinant, preserving product/FMA
    // order. Host division replaces fres + Newton correction only.
    const float c00=std::fma(m[5],m[10],-(m[9]*m[6]));
    const float c10=std::fma(m[6],m[8],-(m[10]*m[4]));
    const float c01=std::fma(m[9],m[2],-(m[1]*m[10]));
    const float c11=std::fma(m[10],m[0],-(m[2]*m[8]));
    const float c02=std::fma(m[1],m[6],-(m[5]*m[2]));
    const float c12=std::fma(m[2],m[4],-(m[6]*m[0]));
    const float c20=std::fma(m[4],m[9],-(m[5]*m[8]));
    const float c21=std::fma(m[1],m[8],-(m[0]*m[9]));
    const float c22=std::fma(m[0],m[5],-(m[1]*m[4]));
    float determinant=m[0]*c00;
    determinant=std::fma(m[4],c01,determinant);
    determinant=std::fma(m[8],c02,determinant);
    if(!std::isfinite(determinant))return WorldMapModelCoreStatus::InvalidInput;
    if(determinant==0)return WorldMapModelCoreStatus::SingularMatrix;
    const float reciprocal=1.0f/determinant;
    if(!std::isfinite(reciprocal))return WorldMapModelCoreStatus::InvalidInput;
    WorldMapModelMatrix inverse{c00*reciprocal,c01*reciprocal,c02*reciprocal,0,
        c10*reciprocal,c11*reciprocal,c12*reciprocal,0,c20*reciprocal,c21*reciprocal,c22*reciprocal,0};
    for(size_t row=0;row<3;++row) {
        float sum=inverse[row*4]*m[3];
        sum=std::fma(inverse[row*4+1],m[7],sum);
        inverse[row*4+3]=-std::fma(inverse[row*4+2],m[11],sum);
    }
    if(!finite(inverse))return WorldMapModelCoreStatus::InvalidInput;
    *out=inverse;return WorldMapModelCoreStatus::Prepared;
}
} // namespace
WorldMapModelCoreStatus prepare_world_map_model_core(
    const WorldMapPreparedModelResource& resource,WorldMapModelCoreInitialization* out) {
    using Status=WorldMapModelCoreStatus;
    if(out==nullptr)return Status::InvalidInput;
    const auto& meta=resource.metadata;
    const bool extended=meta.field_14!=0xffff;
    const uint32_t count=meta.count_6,table_end=0x20u+count*0x1cu;
    if(meta.reference.bank_identity==0 || resource.records.size()!=count || table_end>meta.size ||
        uint64_t(meta.reference.offset)+meta.size>uint64_t(UINT32_MAX)+1 ||
        meta.core_storage_size!=0x198u+count*(extended?0x78u:0x18u) ||
        meta.allocation_size!=meta.core_storage_size+0x20u)return Status::InvalidInput;
    auto record_index=[&](const WorldMapModelResourceReference& ref,uint32_t* index) {
        if(ref.bank_identity!=meta.reference.bank_identity || ref.offset<meta.reference.offset)return false;
        const uint32_t relative=ref.offset-meta.reference.offset;
        if(relative<0x20 || relative>=table_end || (relative-0x20)%0x1c!=0)return false;
        *index=(relative-0x20)/0x1c;return true;
    };
    WorldMapModelCore core;core.resource=meta;
    auto allocation=[&](uint32_t size) {core.allocations.push_back({core.consumed_size,size});core.consumed_size+=size;};
    allocation(0x17c);allocation(0x1c);allocation(count*0x18u);
    if(extended)allocation(count*0x30u);
    struct Visit { uint32_t record; uint16_t parent; };
    std::vector<Visit> stack;
    if(resource.pointer_c) {
        uint32_t index;if(!record_index(*resource.pointer_c,&index))return Status::InvalidInput;
        stack.push_back({index,0xffff});
    }
    core.nodes.reserve(count);
    while(!stack.empty()) {
        const auto visit=stack.back();stack.pop_back();
        if(core.nodes.size()>=count)return Status::InvalidInput; // Also bounds cycles before counter wrap/write overflow.
        const auto& source=resource.records[visit.record];
        const auto index=static_cast<uint16_t>(core.nodes.size());
        core.nodes.push_back({visit.record,static_cast<uint8_t>(source.word_18>>24),
            static_cast<uint8_t>(source.word_18>>16),visit.parent,source.word_14>>16,0,source.pointers[0],0});
        if(source.pointers[2]) {
            uint32_t next;if(!record_index(*source.pointers[2],&next))return Status::InvalidInput;
            stack.push_back({next,visit.parent});
        }
        if(source.pointers[4]) {
            uint32_t child;if(!record_index(*source.pointers[4],&child))return Status::InvalidInput;
            stack.push_back({child,index});
        }
    }
    if(core.nodes.size()!=count)return Status::InvalidInput;
    if(extended) {
        std::vector<WorldMapModelMatrix> forward;forward.reserve(count);
        for(const auto& node:core.nodes) {
            const auto& source=resource.records[node.source_record_index];
            if(!source.pointers[0] || !source.matrix || !finite(*source.matrix) ||
                source.pointers[0]->bank_identity!=meta.reference.bank_identity ||
                source.pointers[0]->offset<meta.reference.offset ||
                uint64_t(source.pointers[0]->offset)+0x34>uint64_t(meta.reference.offset)+meta.size)return Status::InvalidInput;
            auto matrix=*source.matrix;
            if(node.type_0==1 && node.parent_2!=0xffff) {
                if(node.parent_2>=forward.size())return Status::InvalidInput;
                matrix=concatenate(forward[node.parent_2],matrix);
            }
            if(!finite(matrix))return Status::InvalidInput;
            forward.push_back(matrix);
        }
        core.inverse_initial_matrices.reserve(count);
        for(uint32_t i=0;i<count;++i) {
            WorldMapModelMatrix inverse;
            const auto status=invert(forward[i],&inverse);
            if(status==Status::SingularMatrix) {*out={std::nullopt,i};return status;}
            if(status!=Status::Prepared)return status;
            core.inverse_initial_matrices.push_back(inverse);
        }
    }
    *out={std::move(core),0};return Status::Prepared;
}

WorldMapNativeModel::WorldMapNativeModel(std::unique_ptr<const WorldMapModelBank> bank,
    WorldMapModelCore core, std::optional<WorldMapAnimationPlayback> playback)
    : bank_(std::move(bank)), core_(std::move(core)), storage_requests_(core_.allocations),
      playback_(std::move(playback)) {
    // D0FC -> E7C4 -> DD44 reaches no feature calls for this fresh core.
    // The final buffer is unused here; retain the target cursor request as
    // layout evidence without allocating a dummy packed PPC byte buffer.
    storage_requests_.push_back({core_.consumed_size,0x20});
    if(playback_)set_playback(partial_world_map_animation_playback(*playback_));
}
WorldMapAnimationPartialPlayback WorldMapNativeModel::partial_playback() const noexcept {
    const auto& p=core_.playback;
    return {p.position_0,p.rate_4,p.word_8,p.limit_c,p.clip_10,p.link_14,p.value_18};
}
void WorldMapNativeModel::set_playback(const WorldMapAnimationPartialPlayback& p) noexcept {
    core_.playback.position_0=p.position_0;core_.playback.rate_4=p.rate_4;
    core_.playback.clip_10=p.clip_10;core_.playback.link_14=p.link_14;
    core_.playback.word_8=p.word_8;core_.playback.limit_c=p.limit_c;core_.playback.value_18=p.value_18;
    playback_=p.complete();
}
const WorldMapAnimationBank* WorldMapNativeModel::animation_bank(uint64_t identity) const {
    for(const auto& bank:animation_banks_)if(bank->identity()==identity)return bank.get();
    return nullptr;
}
WorldMapAnimationModelBinding WorldMapNativeModel::binding() const {
    return {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&playback_))};
}
WorldMapSecondaryModelRecord WorldMapNativeModel::record() const {
    const auto keys=binding();
    return {keys.model_identity,static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&core_.resource)),
        core_.resource.count_6,core_.auxiliary_c,core_.allocation_10,core_.feature_14,flags_174(),keys.playback_178};
}
WorldMapModelLinkNode WorldMapNativeModel::model_links() const {
    return {binding().model_identity,core_.parent_150,core_.flags_158,core_.children_15c,core_.attachments_16c};
}
WorldMapModelConstructionResult construct_world_map_secondary_model(
    const WorldMapModelBank& bank,uint32_t index,const WorldMapSecondarySetupStep& setup,
    std::unique_ptr<WorldMapNativeModel>* out) {
    using Status=WorldMapModelConstructionStatus;
    const WorldMapModelConstructionResult invalid{Status::InvalidInput};
    if(out==nullptr || !setup.resource || !setup.construction ||
        setup.retain_playback!=setup.saved_playback.has_value())return invalid;
    const auto& request=*setup.construction;
    WorldMapModelResource resource;
    if(!bank.resolve(index,&resource))return invalid;
    auto matches=[&](const WorldMapModelResource& r) {
        return r.reference.bank_identity==resource.reference.bank_identity && r.reference.offset==resource.reference.offset &&
            r.size==resource.size && r.count_6==resource.count_6 && r.field_14==resource.field_14 &&
            r.core_storage_size==resource.core_storage_size && r.allocation_size==resource.allocation_size;
    };
    if(!matches(*setup.resource) || !matches(request.resource) || request.argument_5!=0)return invalid;
    if(setup.saved_playback) {
        const auto& p=*setup.saved_playback;
        if(!std::isfinite(p.position_0) || !std::isfinite(p.rate_4) || !std::isfinite(p.limit_c) ||
            !std::isfinite(p.value_18))return invalid;
    }
    if(request.arena_identity!=0) {
        if(request.allocation_size || request.result_flags_174!=0)return invalid;
        return {Status::RequiresArenaBinding,0,request.arena_identity};
    }
    if(!request.allocation_size || *request.allocation_size!=resource.allocation_size ||
        request.result_flags_174!=4)return invalid;
    try {
        // A private snapshot prevents a caller's later clear/reparse from
        // invalidating this model. No copied fixup proposal is trusted as a
        // substitute for owned bytes; prepare the snapshot again.
        auto owned_bank=std::make_unique<const WorldMapModelBank>(bank);
        WorldMapModelPreparationStep prepared;
        const auto preparation=owned_bank->prepare(index,&prepared);
        if(preparation!=request.preparation_status)return invalid;
        if(preparation==WorldMapModelPreparationStatus::InvalidInput)return invalid;
        if(preparation!=WorldMapModelPreparationStatus::Prepared)
            return {Status::RequiresResourcePreparation,prepared.required_record_index};
        if(!prepared.prepared)return invalid;
        WorldMapModelCoreInitialization initialized;
        const auto status=prepare_world_map_model_core(*prepared.prepared,&initialized);
        if(status==WorldMapModelCoreStatus::SingularMatrix)return {Status::SingularMatrix,initialized.required_node_index};
        if(status!=WorldMapModelCoreStatus::Prepared || !initialized.core ||
            initialized.core->consumed_size+0x20u>resource.allocation_size)return invalid;
        auto model=std::unique_ptr<WorldMapNativeModel>(new WorldMapNativeModel(
            std::move(owned_bank),std::move(*initialized.core),setup.saved_playback));
        *out=std::move(model);
        return {Status::Constructed};
    } catch(const std::bad_alloc&) {
        return {Status::AllocationFailure};
    }
}
WorldMapAnimationChannelStatus advance_world_map_native_secondary_channel(
    WorldMapNativeModel* model,WorldMapAnimationChannelState* channel,
    std::vector<WorldMapAnimationPartialPlaybackRecord>* records,
    uint32_t descriptor_word,uint64_t bank_identity,const WorldMapAnimationBank* bank,
    WorldMapAnimationPartialChannelStep* out) {
    using Status=WorldMapAnimationChannelStatus;
    if(model==nullptr || channel==nullptr || records==nullptr || out==nullptr ||
        channel==&out->after || records==&out->records_after || bank_identity==0)return Status::InvalidInput;
    const uint32_t index=(descriptor_word>>11)&127u;
    if(index==127)return Status::InvalidInput; // Absent-secondary caller uses release, not this call.
    WorldMapModelResource resource;
    if(!model->bank().resolve(index,&resource) || resource.reference.offset!=model->core_.resource.reference.offset)
        return Status::InvalidInput;
    const auto binding=model->binding();
    for(const auto& record:*records)if(record.identity==binding.playback_178)return Status::InvalidInput;
    uint32_t blend=(descriptor_word>>6)&31u;if(blend==31)blend=10;
    const WorldMapActorAnimationSetup setup{binding.model_identity,bank_identity,(index-1u)&0xffffu,blend,0,0.0f};
    try {
        const auto* retained=model->animation_bank(bank_identity);
        const auto* selected_bank=bank?bank:retained;
        auto pool=*records;pool.push_back({binding.playback_178,model->partial_playback()});
        WorldMapAnimationPartialChannelStep prepared;
        const auto status=prepare_world_map_partial_animation_channel(*channel,pool,setup,binding,selected_bank,&prepared);
        if(status==Status::InvalidInput)return status;
        if(status!=Status::Prepared){*out=std::move(prepared);return status;}
        if(retained && !retained->same_contents(*selected_bank))return Status::InvalidInput;
        std::vector<std::shared_ptr<const WorldMapAnimationBank>> banks_after;
        if(!retained){
            banks_after=model->animation_banks_;
            banks_after.push_back(std::make_shared<const WorldMapAnimationBank>(*selected_bank));
        }
        // All potentially throwing data allocations finish before publication.
        auto external=prepared.records_after;const auto model_after=external.back().state;external.pop_back();
        *out=std::move(prepared);
        if(!retained)model->animation_banks_.swap(banks_after);
        model->set_playback(model_after);*channel=out->after;records->swap(external);
        return Status::Advanced;
    } catch(const std::bad_alloc&) {
        return Status::AllocationFailure;
    }
}
WorldMapAnimationChannelStatus apply_world_map_native_animation_channel_settings(
    WorldMapNativeModel* model,WorldMapAnimationChannelState* channel,
    std::vector<WorldMapAnimationPartialPlaybackRecord>* records,
    uint32_t loop,float rate,WorldMapAnimationPartialChannelStep* out) {
    using Status=WorldMapAnimationChannelStatus;
    if(model==nullptr || channel==nullptr || records==nullptr || out==nullptr ||
        channel==&out->after || records==&out->records_after)return Status::InvalidInput;
    const auto binding=model->binding();
    for(const auto& record:*records)if(record.identity==binding.playback_178)return Status::InvalidInput;
    try {
        auto pool=*records;pool.push_back({binding.playback_178,model->partial_playback()});
        WorldMapAnimationPartialChannelStep prepared;
        const auto status=prepare_world_map_partial_animation_channel_settings(*channel,pool,
            binding.model_identity,binding,loop,rate,&prepared);
        if(status==Status::InvalidInput)return status;
        if(status!=Status::Prepared){*out=std::move(prepared);return status;}
        auto external=prepared.records_after;const auto model_after=external.back().state;external.pop_back();
        *out=std::move(prepared);
        model->set_playback(model_after);*channel=out->after;records->swap(external);
        return Status::Advanced;
    } catch(const std::bad_alloc&) {
        return Status::AllocationFailure;
    }
}
WorldMapModelAttachmentStatus apply_world_map_native_model_attachments(
    const std::vector<WorldMapNativeModel*>& owners,
    const WorldMapModelAttachmentRequest& request,WorldMapModelAttachmentStep* out) {
    using Status=WorldMapModelAttachmentStatus;
    if(out==nullptr)return Status::InvalidInput;
    for(size_t i=0;i<owners.size();++i) {
        if(owners[i]==nullptr)return Status::InvalidInput;
        for(size_t j=0;j<i;++j)if(owners[i]==owners[j])return Status::InvalidInput;
    }
    try {
        WorldMapModelLinkState state;std::vector<WorldMapModelAttachmentBinding> bindings;
        for(const auto* owner:owners) {
            state.nodes.push_back(owner->model_links());
            uint16_t first=0;
            for(size_t i=0;i<owner->core_.nodes.size();++i)if(owner->core_.nodes[i].value_4!=0xffff) {
                first=static_cast<uint16_t>(i);break;
            }
            uint16_t index=0;std::optional<uint16_t> attachment;
            if(owner->bank().resolve_attachment_index(owner->core_.resource.reference,&index))attachment=index;
            bindings.push_back({owner->binding().model_identity,first,attachment});
        }
        WorldMapModelAttachmentStep prepared;
        const auto status=prepare_world_map_model_attachments(state,request,bindings,&prepared);
        if(status==Status::InvalidInput)return status;
        if(status!=Status::Prepared){*out=std::move(prepared);return status;}
        *out=std::move(prepared); // All allocating work ends before publication.
        for(size_t i=0;i<owners.size();++i) {
            const auto& node=out->after.nodes[i];auto& core=owners[i]->core_;
            core.parent_150=node.parent_150;core.flags_158=node.flags_158;
            core.children_15c=node.children_15c;core.attachments_16c=node.attachments_16c;
        }
        return Status::Advanced;
    } catch(const std::bad_alloc&) {return Status::AllocationFailure;}
}
} // namespace awl
