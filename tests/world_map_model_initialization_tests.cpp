#include "awl/world_map_model_initialization.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

// Fail real native allocations at each construction stage. This affects
// this test executable only, never the production allocator or other tests.
namespace allocation_probe {
bool enabled=false;
size_t remaining=0, live=0;
}
void* operator new(size_t size) {
    if(allocation_probe::enabled && allocation_probe::remaining--==0)throw std::bad_alloc();
    if(void* pointer=std::malloc(size==0?1:size)){++allocation_probe::live;return pointer;}
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
    if(pointer){--allocation_probe::live;std::free(pointer);}
}
void* operator new[](size_t size){return ::operator new(size);}
void operator delete[](void* pointer) noexcept {::operator delete(pointer);}
void operator delete(void* pointer,size_t) noexcept {::operator delete(pointer);}
void operator delete[](void* pointer,size_t) noexcept {::operator delete(pointer);}

namespace {
using Resource=awl::WorldMapPreparedModelResource;
using Matrix=awl::WorldMapModelMatrix;
using Step=awl::WorldMapModelCoreInitialization;
using Status=awl::WorldMapModelCoreStatus;
int failures=0;
void expect(bool yes,const char* message){if(!yes){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
uint32_t bits(float v){uint32_t w;std::memcpy(&w,&v,4);return w;}
float value(uint32_t w){float v;std::memcpy(&v,&w,4);return v;}
Matrix identity(){return {1,0,0,0,0,1,0,0,0,0,1,0};}
awl::WorldMapModelResourceReference reference(uint32_t offset){return {0x100000003ull,0x400u+offset};}
Resource resource(uint16_t count,bool extended=true){
    Resource r;r.metadata.reference=reference(0);r.metadata.count_6=count;r.metadata.field_14=extended?0:0xffff;
    r.metadata.size=32u+uint32_t(count)*80u;r.metadata.core_storage_size=0x198u+uint32_t(count)*(extended?0x78u:0x18u);
    r.metadata.allocation_size=r.metadata.core_storage_size+32;r.records.resize(count);
    if(count)r.pointer_c=reference(32);
    for(uint32_t i=0;i<count;++i){auto& n=r.records[i];n.pointers[0]=reference(32u+uint32_t(count)*28u+i*52u);
        n.matrix=identity();n.word_18=0x01000000u;n.word_14=(i+7)<<16;}
    return r;
}
void next(Resource& r,uint32_t from,uint32_t to){r.records[from].pointers[2]=reference(32+to*28);}
void child(Resource& r,uint32_t from,uint32_t to){r.records[from].pointers[4]=reference(32+to*28);}
Resource tree(uint32_t seed){
    const uint16_t count=static_cast<uint16_t>(16+seed%17);auto r=resource(count,(seed&1)!=0);
    std::vector<std::vector<uint32_t>> children(count);uint32_t random=seed;
    for(uint32_t i=1;i<count;++i){random=random*1664525u+1013904223u;children[(random>>4)%i].push_back(i);}
    for(uint32_t i=0;i<count;++i){
        if(!children[i].empty())child(r,i,children[i][0]);
        for(size_t j=1;j<children[i].size();++j)next(r,children[i][j-1],children[i][j]);
        auto& n=r.records[i];n.word_18=(((seed+i)%3)<<24)|(((seed+i*7)&255)<<16)|0xa55a;
        n.word_14=((seed+i*17)&0xffff)<<16|0x1357;
        (*n.matrix)[3]=static_cast<float>(i%7+1);
        (*n.matrix)[7]=static_cast<float>(static_cast<int>(i%3)-1);
    }
    return r;
}
void test_core(){
    auto r=resource(4);child(r,0,1);next(r,1,2);child(r,1,3);Step step;
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared && step.core,"core initialization prepares");
    if(!step.core)return;
    const auto& c=*step.core;
    expect(c.nodes.size()==4 && c.nodes[0].source_record_index==0 && c.nodes[1].source_record_index==1 &&
        c.nodes[2].source_record_index==3 && c.nodes[3].source_record_index==2 && c.nodes[0].parent_2==0xffff &&
        c.nodes[1].parent_2==0 && c.nodes[2].parent_2==1 && c.nodes[3].parent_2==0,
        "child-before-sibling traversal emits preorder with traversal parent indices");
    expect(c.allocations.size()==4 && c.allocations[0].size==0x17c && c.allocations[1].offset==0x17c &&
        c.allocations[2].offset==0x198 && c.allocations[2].size==96 && c.allocations[3].size==192 &&
        c.consumed_size==696 && c.resource.core_storage_size==888,
        "observed cursor consumption differs from conservative D2C0 sizing and is not conflated");
    expect(c.playback.position_0==0 && c.playback.rate_4==1 && !c.playback.clip_10 && c.playback.link_14==0 &&
        c.nodes[2].value_4==10 && c.nodes[2].feature_8==0 && c.nodes[2].word_10==0 &&
        c.children_15c==std::array<uint64_t,4>{} && c.attachments_16c==std::array<uint16_t,4>{0xffff,0xffff,0xffff,0xffff},
        "only supported playback/node/model fields initialize; unknown playback fields have no fabricated values");
    r=resource(2);child(r,0,1);r.records[0].matrix=Matrix{0,-1,0,10,1,0,0,20,0,0,1,30};
    r.records[1].matrix=Matrix{1,0,0,2,0,1,0,3,0,0,1,4};
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared &&
        std::abs(step.core->inverse_initial_matrices[1][3]+22)<1e-6f &&
        std::abs(step.core->inverse_initial_matrices[1][7]-7)<1e-6f &&
        std::abs(step.core->inverse_initial_matrices[1][11]+34)<1e-6f,
        "type-one child combines known quarter-turn/translation before inversion");
    r.records[1].word_18=0x02000000;
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared && step.core->inverse_initial_matrices[1][3]==-2,
        "non-type-one child copies its local matrix independently of its parent");
    r=resource(4,false);child(r,0,1);next(r,0,2);child(r,2,1);
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared && step.core->nodes[3].source_record_index==1 &&
        step.core->nodes[3].parent_2==2,"shared source is revisited with a distinct traversal parent; unreachable spare record is opaque");
    r.records[3].pointers[4]=reference(UINT32_MAX);r.records[3].matrix=Matrix{};
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared,"unreached source fields stay opaque");
    r=resource(0);expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared && step.core->allocations.size()==4 &&
        step.core->allocations[2].size==0 && step.core->allocations[3].size==0 && step.core->consumed_size==0x198,
        "zero-count extended model retains zero-size cursor requests");
    r=resource(1,false);r.records[0].pointers[0].reset();r.records[0].matrix.reset();
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared && step.core->allocations.size()==3 &&
        step.core->inverse_initial_matrices.empty(),"compact model skips pose reads and matrix allocation/evaluation");
    for(unsigned fault=0;fault<10;++fault){
        r=resource(2);child(r,0,1);step.required_node_index=777;
        switch(fault){
        case 0:r.pointer_c.reset();break;case 1:next(r,1,0);break;
        case 2:r.records[0].pointers[4]=reference(33);break;case 3:r.pointer_c->bank_identity=9;break;
        case 4:r.records.pop_back();break;case 5:r.metadata.size=40;break;
        case 6:r.records[1].matrix.reset();break;case 7:(*r.records[1].matrix)[0]=std::numeric_limits<float>::infinity();break;
        case 8:r.records[1].pointers[0]->bank_identity=9;break;case 9:r.metadata.core_storage_size=0;break;
        }
        expect(awl::prepare_world_map_model_core(r,&step)==Status::InvalidInput && step.required_node_index==777,
            "count/cycle/link/owner/extent/matrix/metadata failure preserves entire output");
    }
    r=resource(2);child(r,0,1);r.records[1].matrix=Matrix{};
    expect(awl::prepare_world_map_model_core(r,&step)==Status::SingularMatrix && !step.core && step.required_node_index==1,
        "singular later inverse returns an unaccepted stop without partial core");
    expect(awl::prepare_world_map_model_core(r,nullptr)==Status::InvalidInput,"null core output rejects");
    r=resource(4096,false);for(uint32_t i=1;i<4096;++i)child(r,i-1,i);
    expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared && step.core->nodes.back().parent_2==4094,
        "iterative traversal safely crosses original unroll/recursive depth");
}
void test_inverse(){
    for(uint32_t seed=0;seed<1024;++seed){
        auto r=resource(1);Matrix m{1+float(seed%7)/8,0.125f,-0.0625f,float(seed%17),
            -0.125f,1+float((seed>>3)%7)/8,0.0625f,-float(seed%13),
            0.0625f,-0.0625f,1+float((seed>>6)%7)/8,float(seed%11)};
        r.records[0].matrix=m;Step step;expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared,"nonsingular invented matrix prepares");
        if(!step.core)continue;const auto& inverse=step.core->inverse_initial_matrices[0];
        for(const std::array<double,3> point: {std::array<double,3>{0,0,0},{1,2,3},{-4,5,-6}}){
            std::array<double,3> transformed{};
            for(size_t row=0;row<3;++row){transformed[row]=m[row*4+3];for(size_t col=0;col<3;++col)transformed[row]+=m[row*4+col]*point[col];}
            for(size_t row=0;row<3;++row){double restored=inverse[row*4+3];for(size_t col=0;col<3;++col)restored+=inverse[row*4+col]*transformed[col];
                expect(std::abs(restored-point[row])<1e-5,"inverse independently restores three points through original affine transform");}
        }
    }
}
void matrix_digest(){
    uint64_t digest=14695981039346656037ull;
    auto hash=[&](uint32_t w){for(unsigned i=0;i<4;++i){digest^=(w>>(24-i*8))&255;digest*=1099511628211ull;}};
    for(uint32_t seed=0;seed<512;++seed){const auto r=tree(seed);Step step;
        expect(awl::prepare_world_map_model_core(r,&step)==Status::Prepared,"invented hierarchy matrix prepares");if(!step.core)continue;
        const auto& c=*step.core;hash(seed);hash(c.consumed_size);hash(static_cast<uint32_t>(c.allocations.size()));
        for(const auto& a:c.allocations){hash(a.offset);hash(a.size);}hash(bits(c.playback.position_0));hash(bits(c.playback.rate_4));
        hash(c.playback.clip_10?1u:0u);hash(static_cast<uint32_t>(c.playback.link_14));
        for(const auto& n:c.nodes){hash(n.source_record_index);hash(n.type_0);hash(n.order_1);hash(n.parent_2);hash(n.value_4);hash(static_cast<uint32_t>(n.feature_8));hash(n.word_10);}
    }
    std::cout<<"MODEL_CORE_LAYOUT_MATRIX 512 "<<std::hex<<digest<<std::dec<<'\n';
    // Filled from the independently executed mapped constructor instructions.
    expect(digest==0xe9a1e6e889dd2022ull,"model core layout and ordered node fields agree with original mapped instructions");
}
using ConstructionStatus=awl::WorldMapModelConstructionStatus;
using Setup=awl::WorldMapSecondarySetupStep;
using Owner=std::unique_ptr<awl::WorldMapNativeModel>;
void put(std::vector<uint8_t>& b,size_t at,uint32_t word){for(unsigned i=0;i<4;++i)b[at+i]=static_cast<uint8_t>(word>>(24-i*8));}
awl::WorldMapModelBank bank_fixture(uint32_t seed=3,unsigned fault=0){
    const uint32_t count=seed%64,table=32+count*28,size=table+count*52;
    const bool extended=(seed&64)!=0;
    std::vector<uint8_t> bytes(64+size);
    put(bytes,0,0x55aa382d);put(bytes,4,32);put(bytes,8,32);put(bytes,12,64);
    put(bytes,32,0x01000000);put(bytes,40,2);put(bytes,44,1);put(bytes,48,64);put(bytes,52,size);bytes[57]='m';
    const size_t base=64;put(bytes,base,0x007b7960);put(bytes,base+4,count);put(bytes,base+12,count?32:0);
    put(bytes,base+20,extended?0:0xffff0000u);
    for(uint32_t i=0;i<count;++i){const size_t record=base+32+i*28,pose=base+table+i*52;
        put(bytes,record,table+i*52);put(bytes,record+8,i+1<count?32+(i+1)*28:0);
        put(bytes,record+20,(((seed+i*17)&65535)<<16)|0x1357);
        put(bytes,record+24,0x01000000|(((seed+i*7)&255)<<16)|0xa55a);
        put(bytes,pose,0); // Independently obvious identity, no quaternion error.
        if(fault==1 && i==1){put(bytes,pose,0x02000000);put(bytes,pose+16,bits(90));}
        if(fault==2 && i==1)put(bytes,pose,0x10000000); // Explicit zero/singular matrix.
        if(fault==3 && i+1==count)put(bytes,record+8,32); // Cycle back to root.
        if(fault==4 && i==1)put(bytes,record,table); // Shared in-place pose span.
    }
    awl::WorldMapModelBank bank;expect(bank.parse(0x100000003ull,std::move(bytes)),"invented native-owner bank parses");return bank;
}
awl::WorldMapAnimationPlayback saved_playback(uint32_t seed){
    return {float(seed)/4,-1.25f,0xdead0000u+seed,7.5f,
        awl::WorldMapAnimationClipReference{0x200000003ull,0x80+seed},0x300000003ull+seed,-4.25f};
}
Setup setup_fixture(const awl::WorldMapModelBank& bank,uint32_t seed=0){
    Setup setup;awl::WorldMapModelResource resource;
    expect(bank.resolve(1,&resource),"invented model metadata resolves");
    const bool retain=(seed&128)!=0;
    const auto saved=saved_playback(seed);
    const std::vector<awl::WorldMapAnimationPlaybackRecord> records{{700,saved}};
    const awl::WorldMapSecondaryModelRecord model{300,0,retain?resource.count_6:resource.count_6+1u,0,0,0,0,700};
    const auto status=awl::prepare_world_map_secondary_model_setup(1,bank.identity(),&bank,300,model,records,uint64_t(0),&setup);
    expect(status==awl::WorldMapSecondarySetupStatus::RequiresConstruction ||
        status==awl::WorldMapSecondarySetupStatus::RequiresResourcePreparation,"supplied setup reaches construction or explicit resource stop");
    return setup;
}
void test_construction(){
    auto bank=bank_fixture(67);auto setup=setup_fixture(bank);Owner model;
    expect(awl::construct_world_map_secondary_model(bank,1,setup,&model).status==ConstructionStatus::Constructed && model,
        "null-arena/null-secondary constructor creates a real native owner");
    if(!model)return;
    const auto record=model->record();const auto keys=model->binding();
    expect(model->flags_174()==4 && record.identity==keys.model_identity && record.playback_178==keys.playback_178 &&
        record.identity!=0 && record.playback_178!=0 && record.identity!=record.playback_178 && record.count_4==3 &&
        record.resource_0!=0 && record.auxiliary_c==0 && record.allocation_10==0 && record.feature_14==0,
        "owned model exposes stable distinct native keys, count and supported zero-feature state");
    expect(!model->playback() && model->core().playback.rate_4==1 && model->storage_requests().size()==5 &&
        model->storage_requests().back().size==32 && model->storage_requests().back().offset==model->core().consumed_size &&
        model->consumed_size()==656 && model->core().resource.allocation_size==800,
        "fresh playback stays partial; D0FC final cursor request and conservative estimate remain distinct");
    const auto* previous=model.get();
    for(unsigned fault=0;fault<15;++fault){auto bad=setup;
        switch(fault){
        case 0:bad.resource.reset();break;case 1:bad.construction.reset();break;
        case 2:bad.resource->reference.bank_identity=9;break;case 3:++bad.construction->resource.count_6;break;
        case 4:bad.construction->argument_5=1;break;case 5:bad.construction->allocation_size.reset();break;
        case 6:++*bad.construction->allocation_size;break;case 7:bad.construction->result_flags_174=0;break;
        case 8:bad.retain_playback=true;break;case 9:bad.saved_playback=saved_playback(0);break;
        case 10:bad.retain_playback=true;bad.saved_playback=saved_playback(0);bad.saved_playback->limit_c=std::numeric_limits<float>::infinity();break;
        case 11:bad.construction->preparation_status=awl::WorldMapModelPreparationStatus::RequiresEulerRotation;break;
        case 12:bad.construction->arena_identity=99;break; // Contradictory owned-size proposal.
        case 13:++bad.resource->allocation_size;break;case 14:++bad.construction->resource.reference.offset;break;
        }
        expect(awl::construct_world_map_secondary_model(bank,1,bad,&model).status==ConstructionStatus::InvalidInput &&
            model.get()==previous && model->binding().model_identity==keys.model_identity,
            "malformed supplied proposals preserve the existing native owner and binding");
    }
    setup.construction->arena_identity=0x400000003ull;setup.construction->allocation_size.reset();setup.construction->result_flags_174=0;
    const auto blocked=awl::construct_world_map_secondary_model(bank,1,setup,&model);
    expect(blocked.status==ConstructionStatus::RequiresArenaBinding && blocked.required_arena==0x400000003ull &&
        model.get()==previous,"external arena is an explicit unaccepted stop and cannot replace a native owner");
    setup=setup_fixture(bank,128);const auto saved=*setup.saved_playback;
    expect(awl::construct_world_map_secondary_model(bank,1,setup,&model).status==ConstructionStatus::Constructed && model.get()!=previous &&
        model->playback() && model->playback()->position_0==saved.position_0 && model->playback()->rate_4==saved.rate_4 &&
        model->playback()->word_8==saved.word_8 && model->playback()->limit_c==saved.limit_c &&
        model->playback()->clip_10->bank_identity==saved.clip_10->bank_identity && model->playback()->clip_10->offset==saved.clip_10->offset &&
        model->playback()->link_14==saved.link_14 && model->playback()->value_18==saved.value_18 &&
        model->core().playback.link_14==saved.link_14,
        "compatible replacement restores all seven fields, retaining blend identity/weight rather than FECC resets");
    previous=model.get();
    for(unsigned fault=1;fault<=4;++fault){const auto bad_bank=bank_fixture(67,fault);const auto bad_setup=setup_fixture(bad_bank);
        const auto stopped=awl::construct_world_map_secondary_model(bad_bank,1,bad_setup,&model);
        const auto expected=fault==1 || fault==4?ConstructionStatus::RequiresResourcePreparation:
            fault==2?ConstructionStatus::SingularMatrix:ConstructionStatus::InvalidInput;
        expect(stopped.status==expected && (fault>2 || stopped.required_index==1) && model.get()==previous,
            "Euler, singular, cyclic and shared-pose boundaries preserve an existing native owner without partial acceptance");
    }
    // Construction uses bank bytes, not a caller-editable prepared-view copy.
    setup.construction->preparation.prepared->records.clear();
    expect(awl::construct_world_map_secondary_model(bank,1,setup,&model).status==ConstructionStatus::Constructed &&
        model->core().nodes.size()==3,"native construction re-prepares owned bytes instead of trusting copied fixup state");
    bank.clear();awl::WorldMapModelResource resource;
    expect(!bank.loaded() && model->bank().resolve(1,&resource) && resource.count_6==3 && model->core().nodes.size()==3,
        "clearing source bank leaves the owner's private resource bytes and hierarchy valid");
    bank=bank_fixture(70);expect(model->bank().resolve(1,&resource) && resource.count_6==3 && bank.resolve(1,&resource) && resource.count_6==6,
        "reusing a source identity cannot change an already constructed model's resource generation");
    expect(awl::construct_world_map_secondary_model(bank,1,setup,nullptr).status==ConstructionStatus::InvalidInput,
        "null native owner output rejects");
    setup=setup_fixture(bank,128);
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto baseline=allocation_probe::live;previous=model.get();size_t rejected=0;
    for(size_t fail_at=0;fail_at<128;++fail_at){
        allocation_probe::remaining=fail_at;allocation_probe::enabled=true;
        const auto result=awl::construct_world_map_secondary_model(bank,1,setup,&model);
        allocation_probe::enabled=false;
        if(result.status==ConstructionStatus::Constructed)break;
        ++rejected;expect(result.status==ConstructionStatus::AllocationFailure && model.get()==previous &&
            allocation_probe::live==baseline,"allocation failure at every reached stage releases partial ownership and preserves prior model");
    }
    expect(rejected>5 && model.get()!=previous,"allocation sweep reaches late constructor allocations and eventual successful replacement");
    std::cout<<"NATIVE_MODEL_ALLOCATION_FAILURES "<<rejected<<'\n';
#else
    // MSVC's checked vector default/move constructors allocate iterator
    // proxies inside noexcept functions. Injecting failure there terminates
    // in the STL before a caller can catch bad_alloc. Keep those checks on;
    // the complete allocation-failure sweep runs in the Release build.
    std::cout<<"NATIVE_MODEL_ALLOCATION_FAILURES checked in Release only\n";
#endif
}
void construction_digest(){
    uint64_t digest=14695981039346656037ull;
    const auto baseline=allocation_probe::live;
    auto hash=[&](uint32_t w){for(unsigned i=0;i<4;++i){digest^=(w>>(24-i*8))&255;digest*=1099511628211ull;}};
    for(uint32_t seed=0;seed<512;++seed){auto bank=bank_fixture(seed);const auto setup=setup_fixture(bank,seed);Owner model;
        expect(awl::construct_world_map_secondary_model(bank,1,setup,&model).status==ConstructionStatus::Constructed,
            "supplied constructor/restore matrix creates a native owner");if(!model)continue;
        const auto& c=model->core();hash(seed);hash(c.resource.allocation_size);hash(model->consumed_size());hash(model->flags_174());
        hash(static_cast<uint32_t>(model->storage_requests().size()));
        for(const auto& allocation:model->storage_requests()){hash(allocation.offset);hash(allocation.size);}
        hash(static_cast<uint32_t>(c.nodes.size()));hash(static_cast<uint32_t>(c.inverse_initial_matrices.size()));
        hash(bits(c.playback.position_0));hash(bits(c.playback.rate_4));
        hash(c.playback.clip_10?c.playback.clip_10->offset:0);hash(static_cast<uint32_t>(c.playback.link_14));
        hash(model->playback()?1u:0u);
        if(model->playback()){const auto& p=*model->playback();hash(p.word_8);hash(bits(p.limit_c));hash(bits(p.value_18));}
        for(const auto& n:c.nodes){hash(n.source_record_index);hash(n.type_0);hash(n.order_1);hash(n.parent_2);hash(n.value_4);}
    }
    expect(allocation_probe::live==baseline,"all 512 owned-bank/core/playback lifetimes release their native allocations");
    std::cout<<"MODEL_CONSTRUCTION_MATRIX 512 "<<std::hex<<digest<<std::dec<<'\n';
    // Set only after comparison with mapped CD50/D0FC/cursor/040C instructions.
    expect(digest==0xdc747cc02b29f6b3ull,"mapped constructor and compatible restore matrix agrees");
}
void local_core(const std::filesystem::path& disc,const std::filesystem::path& comparison){
    std::ifstream input(disc/"files"/"boy_0.arc",std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    awl::WorldMapModelBank bank;awl::WorldMapModelPreparationStep prepared;Step step;
    expect(bank.parse(300,std::move(bytes)) && bank.prepare(1,&prepared)==awl::WorldMapModelPreparationStatus::Prepared &&
        awl::prepare_world_map_model_core(*prepared.prepared,&step)==Status::Prepared,"local diagnostic ACT prepares its complete supported model core");
    if(!step.core)return;
    const auto& c=*step.core;expect(c.nodes.size()==55 && c.inverse_initial_matrices.size()==55,"local core includes 55 nodes and inverse matrices");
    std::cout<<"LOCAL_MODEL_CORE "<<c.nodes.size()<<' '<<c.consumed_size<<'\n';
    if(!comparison.empty()){
        std::ifstream expected(comparison);uint32_t index,parent,type,order,param,source,word;unsigned cases=0;float maximum=0;
        while(expected>>std::dec>>index>>parent>>type>>order>>param>>source){
            Matrix matrix{};bool complete=true;for(auto& v:matrix){if(!(expected>>std::hex>>word))complete=false;else v=value(word);}
            expect(complete && index<c.nodes.size(),"mapped local core comparison has full payload and bounded index");if(!complete || index>=c.nodes.size())break;
            const auto& node=c.nodes[index];expect(node.parent_2==parent && node.type_0==type && node.order_1==order && node.value_4==param &&
                node.source_record_index==source,"native local preorder/parents/fields match original mapped constructor");
            for(size_t i=0;i<12;++i){const float error=std::abs(c.inverse_initial_matrices[index][i]-matrix[i])/std::max(1.0f,std::abs(matrix[i]));
                expect(std::isfinite(matrix[i]) && std::isfinite(error) && error<1e-5f,"native local inverse matrix agrees within declared reciprocal tolerance");maximum=std::max(maximum,error);}
            ++cases;
        }
        std::cout<<"LOCAL_MODEL_CORE_MAPPED "<<cases<<" maximum normalized error "<<maximum<<'\n';expect(cases==165,"all 55 local nodes compared under three reciprocal estimates");
    }
    auto setup=setup_fixture(bank,128);Owner owner;
    expect(awl::construct_world_map_secondary_model(bank,1,setup,&owner).status==ConstructionStatus::Constructed && owner &&
        owner->core().nodes.size()==55 && owner->consumed_size()==4400 && owner->flags_174()==4 && owner->playback() &&
        owner->playback()->link_14==setup.saved_playback->link_14,"local 55-node ACT constructs a native owner and restores supplied compatible playback");
    bank.clear();expect(owner && owner->bank().loaded() && owner->core().inverse_initial_matrices.size()==55,
        "local native model survives release of its source bank");
}
} // namespace
int main(int argc,char** argv){
#if defined(_MSC_VER) && defined(_DEBUG)
    // Report assertions to the test runner, without an interactive dialog.
    for(int kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}){
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE);_CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
#endif
    test_core();test_inverse();matrix_digest();test_construction();construction_digest();
    if((argc==3 || argc==4) && std::string(argv[1])=="--model-core-local")local_core(argv[2],argc==4?argv[3]:"");
    else if(argc!=1)expect(false,"usage: --model-core-local <disc> [ignored comparison]");
    return failures==0?0:1;
}
