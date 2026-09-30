#include "awl/world_map_secondary_model.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {
using Bank=awl::WorldMapModelBank;
using Model=awl::WorldMapSecondaryModelRecord;
using Feature=awl::WorldMapSecondaryFeatureRecord;
using Setup=awl::WorldMapSecondarySetupStep;
using SStatus=awl::WorldMapSecondarySetupStatus;
using Release=awl::WorldMapSecondaryReleaseStep;
using RStatus=awl::WorldMapSecondaryReleaseStatus;
using Kind=awl::WorldMapSecondaryReleaseKind;
int failures=0;
void expect(bool yes,const char* message) {if(!yes){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
void put(std::vector<uint8_t>& b,size_t o,uint32_t v) {for(unsigned i=0;i<4;++i)b[o+i]=static_cast<uint8_t>(v>>(24-i*8));}
uint32_t bits(float v) {uint32_t w;std::memcpy(&w,&v,4);return w;}
void hash(uint64_t& h,uint32_t w) {for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}
std::vector<uint8_t> archive(uint16_t count=2,uint16_t field=0xffff) {
    // Invented flat U8 containing one raw ACT metadata fixture and one opaque
    // non-model file. No game names, matrices or byte payloads are included.
    const uint32_t size=0x20u+uint32_t(count)*0x1cu, second=96+size;
    std::vector<uint8_t> b(second+32);
    put(b,0,0x55aa382d);put(b,4,32);put(b,8,48);put(b,12,96);
    put(b,32,0x01000000);put(b,40,3);put(b,44,1);put(b,48,96);put(b,52,size);
    put(b,56,3);put(b,60,second);put(b,64,32);b[69]='a';b[71]='b';
    put(b,96,0x007b7960);put(b,100,count);put(b,116,uint32_t(field)<<16);
    return b;
}
Bank bank(uint16_t count=2,uint16_t field=0xffff) {Bank b;expect(b.parse(300,archive(count,field)),"synthetic model bank parses");return b;}
awl::WorldMapAnimationPlayback playback(uint32_t seed) {
    awl::WorldMapAnimationPlayback p;
    p.position_0=seed==0 ? -0.0f : static_cast<float>(seed)+0.5f;p.rate_4=-2;p.word_8=0xdeadbeefu+seed;p.limit_c=3;
    if(seed==1)p.clip_10=awl::WorldMapAnimationClipReference{77,seed+44};
    p.link_14=seed+500;p.value_18=-4.25f;return p;
}
void hash_playback(uint64_t& h,const awl::WorldMapAnimationPlayback& p) {
    for(uint32_t w:{bits(p.position_0),bits(p.rate_4),p.word_8,bits(p.limit_c),
        p.clip_10?static_cast<uint32_t>(p.clip_10->bank_identity):0,p.clip_10?p.clip_10->offset:0,
        static_cast<uint32_t>(p.link_14),bits(p.value_18)}) hash(h,w);
}
void test_bank() {
    Bank b=bank();awl::WorldMapModelResource r;
    expect(b.file_count()==2 && b.resolve(1,&r) && r.count_6==2 && r.field_14==0xffff &&
        r.core_storage_size==456 && r.allocation_size==488,"full node one reads bounded ACT metadata and compact sizing");
    const auto saved=r;
    for(uint32_t index:{0u,2u,3u,0x10001u,UINT32_MAX}) expect(!b.resolve(index,&r) && r.reference.offset==saved.reference.offset,
        "root, heterogeneous non-model, invalid and high full indices reject without low-word aliasing or output mutation");
    expect(!b.resolve(1,nullptr),"null model metadata output rejects");
    auto bytes=archive();put(bytes,96,UINT32_MAX);
    expect(b.parse(300,bytes) && !b.resolve(1,&r),"relocated ACT marker is unsupported without mutating owner bytes");
    bytes=archive();put(bytes,100,3);
    expect(b.parse(300,bytes) && !b.resolve(1,&r),"truncated reached record table rejects at lookup");
    for(unsigned fault=0;fault<8;++fault) {
        bytes=archive();
        switch(fault){
        case 0:put(bytes,40,UINT32_MAX);break;case 1:put(bytes,44,0x01000001);break;
        case 2:put(bytes,48,80);break;case 3:put(bytes,52,UINT32_MAX);break;
        case 4:put(bytes,60,100);break;case 5:put(bytes,44,100);break;
        case 6:for(size_t i=70;i<80;++i)bytes[i]='z';break;case 7:put(bytes,8,12);break;
        }
        expect(!b.parse(300,bytes) && !b.loaded(),"shared flat-U8 bounds/type/name/overlap failure clears owner");
    }
    expect(!b.parse(0,archive()) && !b.parse(300,std::vector<uint8_t>(31)),"null identity and truncated archive reject");
    b=bank(0,0);
    expect(b.resolve(1,&r) && r.core_storage_size==408 && r.allocation_size==440,"zero count retains fixed storage size");
    b=bank(0xffff,0);
    expect(b.resolve(1,&r) && r.core_storage_size==7864608 && r.allocation_size==7864640,
        "maximum halfword count stays bounded and sizing has no native word overflow");
}
void test_setup() {
    const auto b=bank();const std::vector<awl::WorldMapAnimationPlaybackRecord> records{{8,playback(1)}};
    Model model;model.identity=9;model.count_4=2;model.playback_178=8;Setup step;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,records,0,&step)==SStatus::RequiresConstruction &&
        step.retain_playback && step.saved_playback && step.saved_playback->word_8==0xdeadbef0 &&
        step.saved_playback->clip_10->bank_identity==77 && step.saved_playback->link_14==501 && step.saved_playback->value_18==-4.25f &&
        step.construction->allocation_size==488u && step.construction->result_flags_174==4 && step.construction->argument_5==0,
        "matching count saves all seven playback fields without blend reset, stopping at construction");
    model.count_4=0x10002;model.playback_178=0;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,{},700,&step)==SStatus::RequiresConstruction &&
        !step.retain_playback && !step.saved_playback && !step.construction->allocation_size && step.construction->result_flags_174==0,
        "full word count equality is required; mismatched model skips playback and uses observed external arena");
    expect(awl::prepare_world_map_secondary_model_setup(1,300,nullptr,9,std::nullopt,{},std::nullopt,&step)==SStatus::RequiresBank &&
        !step.resource,"owned bank lookup precedes old-model and arena evidence");
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,std::nullopt,{},std::nullopt,&step)==SStatus::RequiresModel &&
        step.required_record==9,"old model snapshot is required only for nonnull wrapper");
    model.count_4=2;model.playback_178=8;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,{},std::nullopt,&step)==SStatus::RequiresPlayback &&
        step.required_record==8,"matching count requires playback before arena binding");
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,records,std::nullopt,&step)==SStatus::RequiresArena &&
        step.saved_playback && !step.construction,"missing arena preserves saved prefix without construction success");
    step.required_record=444;auto invalid=records;invalid[0].state.value_18=std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,invalid,0,&step)==SStatus::InvalidInput &&
        step.required_record==444,"reached nonfinite blend weight fails without overwriting output");
    model.count_4=3;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,invalid,0,&step)==SStatus::RequiresConstruction,
        "mismatched count ignores unread nonfinite playback");
    model.count_4=2;invalid=records;invalid.push_back(records[0]);
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,invalid,0,&step)==SStatus::InvalidInput,
        "ambiguous reached playback identity rejects");
    model.identity=10;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,9,model,records,0,&step)==SStatus::InvalidInput &&
        awl::prepare_world_map_secondary_model_setup(1,301,&b,0,std::nullopt,{},0,&step)==SStatus::InvalidInput &&
        awl::prepare_world_map_secondary_model_setup(1,0,&b,0,std::nullopt,{},0,&step)==SStatus::InvalidInput &&
        awl::prepare_world_map_secondary_model_setup(1,300,&b,0,std::nullopt,{},0,nullptr)==SStatus::InvalidInput,
        "wrong model/bank keys, null bank identity and output reject");
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,0,model,invalid,0,&step)==SStatus::RequiresConstruction &&
        !step.saved_playback,"null old model ignores every unused snapshot and playback value");
    const auto high=playback(1);auto high_record=high;high_record.link_14=0x1000001f5ull;
    model.identity=0x100000009ull;model.count_4=2;model.playback_178=0x200000008ull;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&b,model.identity,model,{{model.playback_178,high_record}},
        0x3000002bcull,&step)==SStatus::RequiresConstruction && step.saved_playback->link_14==high_record.link_14 &&
        step.construction->arena_identity==0x3000002bcull,"model, playback, retained link and arena identities keep all native bits");
}
void test_release() {
    Model m; m.identity=1;m.resource_0=10;m.auxiliary_c=20;m.allocation_10=30;m.feature_14=40;m.flags_174=7;
    Feature f{40,50,1};Release step;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::RequiresBackend &&
        step.required_call->kind==Kind::Asset && step.required_call->resource_identity==10 && step.wrapper_after==1,
        "asset ownership bit two takes priority without clearing wrapper");
    m.flags_174=5;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::RequiresBackend &&
        step.required_call->kind==Kind::Auxiliary && step.required_call->argument_offset==0xc,"auxiliary bit one precedes feature and heap ownership");
    m.flags_174=4;
    expect(awl::prepare_world_map_secondary_model_release(1,m,std::nullopt,&step)==RStatus::RequiresFeature &&
        step.required_record==40,"reached feature record is required before heap release");
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::RequiresBackend &&
        step.required_call->owner_identity==40 && step.required_call->resource_identity==50,"feature-owned buffer free precedes model allocation free");
    f.flags_38=0;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::RequiresBackend &&
        step.required_call->resource_identity==30 && step.feature_after->buffer_34==50,
        "blocked heap release rolls back earlier borrowed-feature buffer clear");
    m.allocation_10=0;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::RequiresBackend &&
        step.required_call->resource_identity==1,"owned model itself still needs free when allocation ten is null");
    m.flags_174=0;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::Prepared && step.wrapper_after==0 &&
        step.feature_after->buffer_34==0 && step.model_after->resource_0==10 && f.buffer_34==50,
        "borrowed resources need no free; complete proposal clears wrapper and feature buffer only");
    step.required_record=444;f.identity=41;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::InvalidInput && step.required_record==444,
        "wrong reached feature identity preserves output");
    m.flags_174=2;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::RequiresBackend,
        "earlier asset stop ignores later invalid feature evidence");
    expect(awl::prepare_world_map_secondary_model_release(0,m,f,&step)==RStatus::Prepared && step.wrapper_after==0 &&
        !step.required_call,"null wrapper ignores model/feature observations");
    expect(awl::prepare_world_map_secondary_model_release(1,std::nullopt,std::nullopt,&step)==RStatus::RequiresModel &&
        step.required_record==1,"missing model core is explicit");
    m.identity=2;
    expect(awl::prepare_world_map_secondary_model_release(1,m,f,&step)==RStatus::InvalidInput &&
        awl::prepare_world_map_secondary_model_release(1,m,f,nullptr)==RStatus::InvalidInput &&
        awl::prepare_world_map_secondary_model_release(1,step.model_after,f,&step)==RStatus::InvalidInput &&
        awl::prepare_world_map_secondary_model_release(1,m,step.feature_after,&step)==RStatus::InvalidInput,
        "model mismatch, null output and snapshot output aliases reject");
}
void test_setup_matrix() {
    uint64_t digest=14695981039346656037ull;unsigned cases=0;
    for(uint32_t count:{0u,1u,2u,55u,65535u}) for(uint32_t old:{0u,1u,2u,55u,65535u,65536u,UINT32_MAX}) {
        for(uint32_t present:{0u,1u}) for(uint32_t seed:{0u,1u,2u}) for(uint64_t arena:{0ull,700ull}) {
            const auto b=bank(static_cast<uint16_t>(count),seed==0?uint16_t(0xffff):static_cast<uint16_t>(seed-1));
            Model m;m.identity=9;m.count_4=old;m.playback_178=8;Setup step;
            const auto status=awl::prepare_world_map_secondary_model_setup(1,300,&b,present?9:0,m,{{8,playback(seed)}},arena,&step);
            expect(status==SStatus::RequiresConstruction,"all supplied setup matrix cases reach construction stop");
            if(status!=SStatus::RequiresConstruction)continue;
            for(uint32_t w:{count,old,present,seed,static_cast<uint32_t>(arena),step.resource->core_storage_size,
                step.resource->allocation_size,static_cast<uint32_t>(step.retain_playback),step.construction->result_flags_174,
                step.construction->allocation_size.value_or(0)})hash(digest,w);
            if(step.saved_playback)hash_playback(digest,*step.saved_playback);
            ++cases;
        }
    }
    std::cout<<"SECONDARY_SETUP_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==420 && digest==0x93aba495bb7ba1c1ull,"setup decisions, seven-field saves and isolated sizing match independent PPC walk");
}
void test_release_matrix() {
    uint64_t digest=14695981039346656037ull;unsigned cases=0;
    for(uint32_t flags=0;flags<16;++flags)for(uint32_t mask=0;mask<16;++mask)for(uint32_t feature_flags=0;feature_flags<4;++feature_flags) {
        for(uint32_t buffer:{0u,50u})for(uint32_t present:{0u,1u}) {
            const Model m{1,(mask&1)?10u:0u,0,(mask&2)?20u:0u,(mask&4)?30u:0u,(mask&8)?40u:0u,flags,0};
            const Feature f{40,buffer,feature_flags};Release step;
            const auto status=awl::prepare_world_map_secondary_model_release(present?1:0,m,f,&step);
            expect(status==RStatus::Prepared||status==RStatus::RequiresBackend,"all supplied release cases prepare or stop before free");
            for(uint32_t w:{flags,mask,feature_flags,buffer,present,static_cast<uint32_t>(step.wrapper_after),
                static_cast<uint32_t>(step.feature_after->buffer_34),static_cast<uint32_t>(bool(step.required_call))})hash(digest,w);
            if(step.required_call)for(uint32_t w:{static_cast<uint32_t>(step.required_call->kind),
                static_cast<uint32_t>(step.required_call->owner_identity),static_cast<uint32_t>(step.required_call->resource_identity),
                step.required_call->argument_offset})hash(digest,w);
            ++cases;
        }
    }
    std::cout<<"SECONDARY_RELEASE_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==4096 && digest==0xf98e3a87a827ef25ull,"release branch ordering and atomic proposal match independent PPC walk");
}
void local_bank(const std::filesystem::path& disc) {
    std::ifstream input(disc/"files"/"boy_0.arc",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});Bank b;
    expect(b.parse(300,std::move(bytes)) && b.file_count()==7,"local diagnostic heterogeneous model bank validates");
    if(!b.loaded())return;
    uint64_t digest=14695981039346656037ull;uint32_t supported=0;
    for(uint32_t i=1;i<=b.file_count();++i){
        awl::WorldMapModelResource r;const bool ok=b.resolve(i,&r);hash(digest,i);hash(digest,ok?1:0);
        if(ok){++supported;for(uint32_t w:{r.reference.offset,r.size,uint32_t(r.count_6),uint32_t(r.field_14),r.core_storage_size,r.allocation_size})hash(digest,w);}
    }
    std::cout<<"LOCAL_MODEL_BANK "<<b.file_count()<<' '<<supported<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(supported==1 && digest==0x5d4507bdb774fc18ull,"local supported and rejected full nodes match independent metadata walk");
}
} // namespace
int main(int argc,char** argv){
    test_bank();test_setup();test_release();test_setup_matrix();test_release_matrix();
    if(argc==3 && std::string(argv[1])=="--model-bank-local")local_bank(argv[2]);
    else if(argc!=1)expect(false,"usage: --model-bank-local <disc>");
    return failures==0?0:1;
}
