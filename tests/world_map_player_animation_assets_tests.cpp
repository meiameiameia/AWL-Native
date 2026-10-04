#include "awl/world_map_player_animation_assets.h"
#include "awl/world_map_model_initialization.h"
#include "awl/filesystem.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace allocation_probe { bool enabled=false; size_t remaining=0,live=0; }
void* operator new(size_t size) {
    if(allocation_probe::enabled && allocation_probe::remaining--==0)throw std::bad_alloc();
    if(void* p=std::malloc(size?size:1)){++allocation_probe::live;return p;}throw std::bad_alloc();
}
void operator delete(void* p) noexcept {if(p){--allocation_probe::live;std::free(p);}}
void* operator new[](size_t s){return ::operator new(s);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,size_t) noexcept {::operator delete(p);}

namespace {
using Status=awl::WorldMapPlayerAnimationAssetsStatus;
using Assets=awl::WorldMapPlayerAnimationAssets;
int failures=0;
void expect(bool yes,const char* why){if(!yes){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
void word(std::vector<uint8_t>& b,size_t p,uint32_t w){for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(w>>(24-i*8));}
uint32_t bits(float v){uint32_t w;std::memcpy(&w,&v,4);return w;}
void hash(uint64_t& h,uint32_t w){for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}

std::vector<uint8_t> clip(float limit){
    std::vector<uint8_t> b(24);word(b,0,0x00ac7472);word(b,4,0x00010000);word(b,8,bits(limit));return b;
}
std::vector<uint8_t> model(){
    std::vector<uint8_t> b(118);word(b,0,0x007b7960);word(b,4,1);word(b,12,32);
    word(b,20,0xffff0000);word(b,24,1);word(b,28,116);word(b,32,64);
    word(b,32+20,0x00010000);word(b,32+24,0x01000000);b[117]=7;return b;
}
std::vector<uint8_t> archive(const std::vector<std::vector<uint8_t>>& files){
    const size_t count=files.size()+1,metadata=count*12+1+files.size()*6;
    const size_t data=(32+metadata+31)&~size_t(31);
    std::vector<uint8_t> b(data);word(b,0,0x55aa382d);word(b,4,32);
    word(b,8,uint32_t(metadata));word(b,12,uint32_t(data));word(b,32,0x01000000);word(b,40,uint32_t(count));
    for(size_t i=0;i<files.size();++i){
        const size_t node=44+i*12,name=32+count*12+1+i*6;
        constexpr char text[]="entry";std::memcpy(b.data()+name,text,sizeof(text));
        word(b,node,uint32_t(1+i*6));word(b,node+4,uint32_t(b.size()));word(b,node+8,uint32_t(files[i].size()));
        b.insert(b.end(),files[i].begin(),files[i].end());b.resize((b.size()+31)&~size_t(31));
    }
    return b;
}
struct Fixture {
    std::filesystem::path root;
    Fixture(){
        root=std::filesystem::temp_directory_path()/std::filesystem::path("awl-player-banks-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(root))throw std::runtime_error("Fixture directory already exists");
        std::filesystem::create_directory(root/"files");
        expect(awl::filesystem_mount("/",root.string().c_str()),"synthetic mount succeeds");
    }
    ~Fixture(){
        std::error_code e;
        for(const char* n:{"boy_0.anm.arc","boy_0_subanm.arc","boy_0_subact.arc"})std::filesystem::remove(root/"files"/n,e);
        std::filesystem::remove(root/"files",e);std::filesystem::remove(root,e);
    }
    void write(const char* name,const std::vector<uint8_t>& bytes){
        std::ofstream f(root/"files"/name,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        if(!f)throw std::runtime_error("Fixture write failed");
    }
    void complete(){write("boy_0.anm.arc",archive({clip(3),clip(7)}));
        write("boy_0_subanm.arc",archive({clip(11),clip(13)}));write("boy_0_subact.arc",archive({model(),model()}));}
};

void synthetic(){
    Fixture f;std::shared_ptr<const Assets> assets;awl::WorldMapPlayerAnimationGroupBinding binding;
    binding.secondary_animation_bank_identity=123;
    expect(awl::load_world_map_player_animation_assets(nullptr)==Status::InvalidInput,"null output rejects without I/O");
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::ReadFailure && !assets,"missing primary publishes nothing");
    f.write("boy_0.anm.arc",clip(3));
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::UnsupportedLayout && !assets,"raw primary clip cannot replace indexed ARC lookup");
    f.write("boy_0.anm.arc",archive({clip(3),clip(7)}));
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::ReadFailure && !assets,"missing secondary animation follows primary validation");
    f.write("boy_0_subanm.arc",clip(11));
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::UnsupportedLayout && !assets,"raw secondary animation is unsupported");
    f.write("boy_0_subanm.arc",archive({clip(11),clip(13)}));
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::ReadFailure && !assets,"missing final model bank rejects the complete bundle");
    f.write("boy_0_subact.arc",std::vector<uint8_t>(32));
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::UnsupportedLayout && !assets,"invalid final archive publishes no prefix");
    expect(awl::bind_world_map_player_animation_group(0,nullptr,&binding)==Status::RequiresAssets &&
        binding.secondary_animation_bank_identity==123,"missing reached assets preserve output");
    f.complete();expect(awl::load_world_map_player_animation_assets(&assets)==Status::Loaded && assets,"three complete archives publish one immutable owner");if(!assets)return;
    const auto* retained=assets.get();f.write("boy_0.anm.arc",{});
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::Loaded && assets.get()==retained,"complete bundle reuses without file reads");f.complete();
    for(uint32_t group=0;group<128;++group){
        const auto old=binding.secondary_animation_bank_identity;
        const auto status=awl::bind_world_map_player_animation_group((group<<25)|0x01ffffff,assets,&binding);
        expect(group==0?status==Status::Bound:status==Status::RequiresGroup && binding.secondary_animation_bank_identity==old,
            "all descriptor group values preserve the verified mask and explicit unsupported-group stop");
    }
    expect(binding.primary.group_index==0 && binding.secondary.group_index==0 && binding.assets==assets &&
        binding.primary.bank_identity==assets->primary_animations().identity() &&
        binding.secondary.model_bank_identity==assets->secondary_models().identity() &&
        binding.secondary_animation_bank_identity==assets->secondary_animations().identity() &&
        binding.primary.bank_identity!=binding.secondary_animation_bank_identity,"distinct row slots retain correct stable bank identities");
    expect(awl::bind_world_map_player_animation_group(0,assets,nullptr)==Status::InvalidInput,"null binding output rejects");
    expect(awl::bind_world_map_player_animation_group(0,binding.assets,&binding)==Status::Bound,"provider/output alias remains safe");
    awl::WorldMapActorAnimationState state;state.model_identity_30=9;
    awl::WorldMapActorAnimationStep start;
    const awl::WorldMapActorAnimationDescriptor descriptor{99,1,(2u<<11)|(31u<<6),0};
    expect(awl::prepare_world_map_actor_animation_start(state,99,descriptor,binding.primary,&start)==
        awl::WorldMapActorAnimationStatus::RequiresModelSetup && start.primary_setup && start.fields &&
        start.primary_setup->clip_index==1 && start.primary_setup->bank_identity==binding.primary.bank_identity &&
        start.fields->secondary_index==2u,"descriptor fields select owned primary group and separate full secondary node index");
    awl::WorldMapAnimationClip c;expect(assets->primary_animations().resolve(1,&c) && c.parameter_zero==7 &&
        assets->secondary_animations().resolve(1,&c) && c.parameter_zero==13,"primary and secondary clip lookup do not conflate banks");
    awl::WorldMapModelResource r;uint16_t attachment=0;
    expect(assets->secondary_models().resolve(2,&r) && assets->secondary_models().resolve_attachment_index(r.reference,&attachment) &&
        r.count_6==1 && attachment==7,"model lookup preserves full node index and attachment metadata");
    expect(!assets->secondary_models().resolve(0,&r) && !assets->secondary_models().resolve(3,&r),"unavailable model nodes reject");
    std::weak_ptr<const Assets> lifetime=assets;assets.reset();
    expect(!lifetime.expired() && binding.assets->secondary_animations().resolve(0,&c),"binding owns all banks after external release");
    binding={};expect(lifetime.expired(),"last binding release destroys the immutable bundle");
// MSVC Debug's noexcept iterator-proxy allocations are not injected; retain
// iterator checking and sweep catchable allocations in Release.
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t rejected=0;bool loaded=false;
    for(size_t fail=0;fail<128;++fail){
        allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::load_world_map_player_animation_assets(&assets);allocation_probe::enabled=false;
        if(status==Status::Loaded){loaded=true;break;}++rejected;
        expect(status==Status::AllocationFailure && !assets && allocation_probe::live==live,
            "every caught allocation failure releases pending banks and leaves output empty");
    }
    expect(loaded && rejected>10,"allocation sweep reaches publication");
    std::cout<<"PLAYER_BANK_LOAD_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}

bool read(const char* path,std::vector<uint8_t>* out){
    void* data=nullptr;size_t size=0;if(!awl::filesystem_read_entire_file(path,&data,&size))return false;
    const std::unique_ptr<void,void(*)(void*)> guard(data,awl::filesystem_free_file_data);
    const auto* p=static_cast<const uint8_t*>(data);out->assign(p,p+size);return true;
}
void local(const char* disc){
    expect(awl::filesystem_mount("/",disc),"local disc mount succeeds");std::shared_ptr<const Assets> assets;
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::Loaded,"actual player group archives load");if(!assets)return;
    awl::WorldMapPlayerAnimationGroupBinding group;
    expect(awl::bind_world_map_player_animation_group(0,assets,&group)==Status::Bound,"actual group zero binds");
    expect(assets->primary_animations().clip_count()==126 && assets->secondary_models().file_count()==96 &&
        assets->secondary_animations().clip_count()==96,"all observed group-zero archive counts agree");
    uint64_t digest=14695981039346656037ull;hash(digest,126);hash(digest,96);hash(digest,96);
    for(uint32_t i=0;i<126;++i){awl::WorldMapAnimationClip c;
        const bool ok=assets->primary_animations().resolve(i,&c);expect(ok,"every primary clip resolves");if(!ok)return;
        for(uint32_t w:{i,c.reference.offset,c.size,bits(c.parameter_zero)})hash(digest,w);
    }
    std::vector<uint8_t> primary_bytes;
    if(!read("/files/boy_0.arc",&primary_bytes)){expect(false,"diagnostic primary model file reads");return;}
    awl::WorldMapModelBank primary_bank;
    expect(primary_bank.parse(111,std::move(primary_bytes)),"diagnostic primary ACT bank parses");
    awl::WorldMapSecondarySetupStep primary_setup;
    expect(awl::prepare_world_map_secondary_model_setup(1,111,&primary_bank,0,{}, {},uint64_t{0},&primary_setup)==
        awl::WorldMapSecondarySetupStatus::RequiresConstruction,"diagnostic primary construction prepares");
    std::unique_ptr<awl::WorldMapNativeModel> primary;
    expect(awl::construct_world_map_secondary_model(primary_bank,1,primary_setup,&primary).status==
        awl::WorldMapModelConstructionStatus::Constructed,"diagnostic primary owner constructs");if(!primary)return;
    size_t constructed=0,attached=0;
    for(uint32_t index=1;index<=96;++index){
        awl::WorldMapModelResource r;awl::WorldMapAnimationClip c;uint16_t attachment=0;
        const bool ok=assets->secondary_models().resolve(index,&r) && assets->secondary_models().resolve_attachment_index(r.reference,&attachment) &&
            assets->secondary_animations().resolve(index-1,&c);expect(ok,"paired secondary model/animation metadata resolves");if(!ok)return;
        for(uint32_t w:{index,r.reference.offset,r.size,uint32_t(r.count_6),uint32_t(r.field_14),r.core_storage_size,r.allocation_size,
            uint32_t(attachment),c.reference.offset,c.size,bits(c.parameter_zero)})hash(digest,w);
        awl::WorldMapSecondarySetupStep setup;
        expect(awl::prepare_world_map_secondary_model_setup(index,group.secondary.model_bank_identity,&assets->secondary_models(),0,{}, {},
            uint64_t{0},&setup)==awl::WorldMapSecondarySetupStatus::RequiresConstruction,"selected actual secondary ACT prepares construction");
        std::unique_ptr<awl::WorldMapNativeModel> model;
        const auto result=awl::construct_world_map_secondary_model(assets->secondary_models(),index,setup,&model);
        expect(result.status==awl::WorldMapModelConstructionStatus::Constructed,"selected actual secondary ACT constructs");if(!model)return;++constructed;
        awl::WorldMapAnimationChannelState channel;std::vector<awl::WorldMapAnimationPartialPlaybackRecord> pool;
        awl::WorldMapAnimationPartialChannelStep channel_step;
        if(index==1)expect(awl::advance_world_map_native_secondary_channel(model.get(),&channel,&pool,(index<<11)|(31u<<6),
            group.secondary_animation_bank_identity,&assets->secondary_animations(),&channel_step)==awl::WorldMapAnimationChannelStatus::InvalidInput &&
            !model->partial_playback().clip_10,"missing channel playback identities reject without inventing records");
        // Explicit diagnostic records, not translated embedded-channel construction.
        channel.target_8=2;channel.previous_c=3;channel.older_10=4;
        for(uint64_t key:{2ull,3ull,4ull})pool.push_back({key,awl::partial_world_map_animation_playback({0,1,0,0,std::nullopt,0,0})});
        expect(awl::advance_world_map_native_secondary_channel(model.get(),&channel,&pool,(index<<11)|(31u<<6),
            group.secondary_animation_bank_identity,&assets->secondary_animations(),&channel_step)==awl::WorldMapAnimationChannelStatus::Advanced,
            "descriptor index selects matching owned secondary clip with default blend");
        expect(awl::apply_world_map_native_animation_channel_settings(model.get(),&channel,&pool,1,1,&channel_step)==
            awl::WorldMapAnimationChannelStatus::Advanced && model->playback() && model->playback()->clip_10 &&
            model->playback()->clip_10->offset==c.reference.offset,
            "supported settings copy actual selected clip metadata into native playback");
        const uint64_t p=primary->binding().model_identity,s=model->binding().model_identity;
        awl::WorldMapModelAttachmentStep step;
        const auto attachment_status=awl::apply_world_map_native_model_attachments({primary.get(),model.get()},{p,s,42,0},&step);
        expect(attachment_status==
            awl::WorldMapModelAttachmentStatus::Advanced && primary->core().children_15c[0]==s &&
            primary->core().attachments_16c[0]==attachment && model->core().parent_150==p,
            "supplied diagnostic feature key attaches actual secondary through its real resource halfword");
        if(attachment_status==awl::WorldMapModelAttachmentStatus::Advanced)++attached;
        // Clear while the borrowed secondary is still alive, before destruction.
        expect(awl::apply_world_map_native_model_attachments({primary.get(),model.get()},{p,0,0,0},&step)==
            awl::WorldMapModelAttachmentStatus::Advanced && primary->core().children_15c[0]==0,
            "clearing releases the borrowed graph link before model destruction");
    }
    std::cout<<"LOCAL_PLAYER_GROUP_METADATA "<<std::hex<<digest<<std::dec<<" models "<<constructed<<" attachments "<<attached<<'\n';
    expect(digest==0x43131709f1de94c9ull,"all selected archive metadata agrees with independent DOL sizing/fixup/section lookup");
    expect(constructed==96 && attached==96,"all actual selected secondary paths complete the bounded native checks");
}
} // namespace
int main(int argc,char** argv){
#if defined(_MSC_VER) && defined(_DEBUG)
    for(int kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}){
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE);_CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
#endif
    synthetic();awl::filesystem_shutdown();
    if(argc==3 && std::string(argv[1])=="--player-banks-local")local(argv[2]);
    else if(argc!=1)expect(false,"usage: --player-banks-local <disc>");
    awl::filesystem_shutdown();return failures?1:0;
}
