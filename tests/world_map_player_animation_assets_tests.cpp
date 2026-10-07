#include "awl/world_map_player_animation_assets.h"
#include "awl/world_map_player_start_animation.h"
#include "awl/world_map_animation_channel_owner.h"
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

namespace allocation_probe { bool enabled=false; size_t remaining=0,live=0,minimum_size=0; }
void* operator new(size_t size) {
    if(allocation_probe::enabled && size>=allocation_probe::minimum_size && allocation_probe::remaining--==0)throw std::bad_alloc();
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
        std::filesystem::remove(root/"sys"/"main.dol",e);std::filesystem::remove(root/"sys",e);
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

void channel_constructor(){
    using Owner=awl::WorldMapNativeAnimationChannel;
    using CStatus=awl::WorldMapAnimationChannelConstructionStatus;
    std::unique_ptr<Owner> owner,other;
    expect(awl::construct_world_map_animation_channel(nullptr)==CStatus::InvalidInput,"null channel construction output rejects");
    expect(awl::construct_world_map_animation_channel(&owner)==CStatus::Constructed && owner,"channel owns its three records");
    if(!owner)return;
    const auto& state=owner->state();const auto& records=owner->records();
    expect(records.size()==3 && state.elapsed_0==0 && state.duration_4==0 && state.mode_18==0 && !state.blend_14,
        "verified clock/mode writes preserve unknown original blend");
    if(records.size()!=3)return;
    expect(state.older_10==records[0].identity && state.previous_c==records[1].identity && state.target_8==records[2].identity,
        "record creation preserves older/previous/target allocation order");
    for(const auto& record:records)expect(record.identity!=0 && record.state.position_0==0 && record.state.rate_4==1 &&
        !record.state.clip_10 && record.state.link_14==0 && !record.state.word_8 && !record.state.limit_c && !record.state.value_18,
        "FDE8 initializes only reached playback fields without neutral fabricated values");
    expect(awl::construct_world_map_animation_channel(&other)==CStatus::Constructed,"second independent channel constructs");
    if(other)for(const auto& a:records)for(const auto& b:other->records())expect(a.identity!=b.identity,"independent owners have disjoint stable keys");
    const auto* address=owner.get();const auto target=state.target_8;other.reset();
    expect(owner.get()==address && owner->state().target_8==target,"other owner release preserves live channel keys");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t rejected=0;bool constructed=false;
    for(size_t fail=0;fail<16;++fail){
        allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::construct_world_map_animation_channel(&owner);allocation_probe::enabled=false;
        if(status==CStatus::Constructed){constructed=true;break;}++rejected;
        expect(status==CStatus::AllocationFailure && owner.get()==address && owner->state().target_8==target && allocation_probe::live==live,
            "failed channel replacement releases pending records and preserves the old owner/keys");
    }
    expect(constructed && rejected==2,"native channel construction reaches both catchable allocation stages");
    std::cout<<"CHANNEL_CONSTRUCTION_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}

void hash_channel(uint64_t& digest,uint32_t index,uint32_t round,const awl::WorldMapNativeAnimationChannel& owner,
    const awl::WorldMapNativeModel& model,const awl::WorldMapAnimationPartialChannelStep& step){
    const auto& channel=owner.state();
    auto key=[&](uint64_t identity){
        if(identity==0)return 0u;
        for(size_t i=0;i<owner.records().size();++i)if(owner.records()[i].identity==identity)return uint32_t(i+1);
        if(identity==model.binding().playback_178)return 4u;
        expect(false,"channel digest encounters no unsupported borrowed key");return UINT32_MAX;
    };
    expect(step.branch.has_value(),"completed setup records an observed branch");
    for(uint32_t word:{index,round,step.branch?uint32_t(*step.branch):UINT32_MAX,channel.elapsed_0,channel.duration_4,
        key(channel.target_8),key(channel.previous_c),key(channel.older_10),uint32_t(channel.blend_14.has_value()),
        channel.blend_14?bits(*channel.blend_14):0u,channel.mode_18})hash(digest,word);
    auto playback=[&](uint32_t identity,const awl::WorldMapAnimationPartialPlayback& p){
        const uint32_t known=uint32_t(p.word_8.has_value())|(uint32_t(p.limit_c.has_value())<<1)|(uint32_t(p.value_18.has_value())<<2);
        for(uint32_t word:{identity,bits(p.position_0),bits(p.rate_4),known,p.word_8.value_or(0),p.limit_c?bits(*p.limit_c):0u,
            uint32_t(p.clip_10.has_value()),p.clip_10?p.clip_10->offset:0u,key(p.link_14),p.value_18?bits(*p.value_18):0u})hash(digest,word);
    };
    for(size_t i=0;i<owner.records().size();++i)playback(uint32_t(i+1),owner.records()[i].state);
    playback(4,model.partial_playback());
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
    // Declare channels before their models: model blend links borrow record keys.
    std::unique_ptr<awl::WorldMapNativeAnimationChannel> primary_channel;
    expect(awl::construct_world_map_animation_channel(&primary_channel)==awl::WorldMapAnimationChannelConstructionStatus::Constructed,
        "diagnostic primary channel constructs from verified initialization");if(!primary_channel)return;
    std::unique_ptr<awl::WorldMapNativeModel> primary;
    expect(awl::construct_world_map_secondary_model(primary_bank,1,primary_setup,&primary).status==
        awl::WorldMapModelConstructionStatus::Constructed,"diagnostic primary owner constructs");if(!primary)return;
    awl::WorldMapAnimationPartialChannelStep primary_step;
    const awl::WorldMapActorAnimationSetup initial{primary->binding().model_identity,group.primary.bank_identity,0,10,0,0};
    expect(primary_channel->setup(primary.get(),initial,&assets->primary_animations(),&primary_step)==awl::WorldMapAnimationChannelStatus::Advanced &&
        primary_step.branch==awl::WorldMapAnimationChannelBranch::NoClip && !primary_channel->state().blend_14,
        "supplied initial primary clip setup preserves unobserved blend");
    expect(primary_channel->apply_settings(primary.get(),1,1,&primary_step)==awl::WorldMapAnimationChannelStatus::Advanced && primary->playback(),
        "supplied initial primary settings establish complete model playback without inventing record fields");
    size_t constructed=0,attached=0,channel_cases=0;uint64_t channel_digest=14695981039346656037ull;
    for(uint32_t index=1;index<=96;++index){
        awl::WorldMapModelResource r;awl::WorldMapAnimationClip c;uint16_t attachment=0;
        const bool ok=assets->secondary_models().resolve(index,&r) && assets->secondary_models().resolve_attachment_index(r.reference,&attachment) &&
            assets->secondary_animations().resolve(index-1,&c);expect(ok,"paired secondary model/animation metadata resolves");if(!ok)return;
        for(uint32_t w:{index,r.reference.offset,r.size,uint32_t(r.count_6),uint32_t(r.field_14),r.core_storage_size,r.allocation_size,
            uint32_t(attachment),c.reference.offset,c.size,bits(c.parameter_zero)})hash(digest,w);
        awl::WorldMapSecondarySetupStep setup;
        expect(awl::prepare_world_map_secondary_model_setup(index,group.secondary.model_bank_identity,&assets->secondary_models(),0,{}, {},
            uint64_t{0},&setup)==awl::WorldMapSecondarySetupStatus::RequiresConstruction,"selected actual secondary ACT prepares construction");
        std::unique_ptr<awl::WorldMapNativeAnimationChannel> channel;
        expect(awl::construct_world_map_animation_channel(&channel)==awl::WorldMapAnimationChannelConstructionStatus::Constructed,
            "every actual secondary uses owned constructor records");if(!channel)return;
        std::unique_ptr<awl::WorldMapNativeModel> model;
        const auto result=awl::construct_world_map_secondary_model(assets->secondary_models(),index,setup,&model);
        expect(result.status==awl::WorldMapModelConstructionStatus::Constructed,"selected actual secondary ACT constructs");if(!model)return;++constructed;
        awl::WorldMapAnimationPartialChannelStep channel_step;
        if(index==1){
            awl::WorldMapAnimationChannelState empty;
            std::vector<awl::WorldMapAnimationPartialPlaybackRecord> missing_records;
            expect(awl::advance_world_map_native_secondary_channel(model.get(),&empty,&missing_records,(index<<11)|(31u<<6),
                group.secondary_animation_bank_identity,&assets->secondary_animations(),&channel_step)==awl::WorldMapAnimationChannelStatus::InvalidInput &&
                !model->partial_playback().clip_10,"empty supplied channel still rejects without inventing playback records");
            const auto target=channel->state().target_8;
            expect(channel->setup_secondary(model.get(),(index<<11)|(31u<<6),group.secondary_animation_bank_identity,nullptr,&channel_step)==
                awl::WorldMapAnimationChannelStatus::RequiresBank && !model->partial_playback().clip_10 && channel->state().target_8==target &&
                !channel->records()[2].state.word_8,"missing reached bank preserves both owners and unknown records");
        }
        for(uint32_t round=0;round<3;++round){
            const auto status=channel->setup_secondary(model.get(),(index<<11)|(31u<<6),group.secondary_animation_bank_identity,
                round==0?&assets->secondary_animations():nullptr,&channel_step);
            expect(status==awl::WorldMapAnimationChannelStatus::Advanced && channel_step.branch==
                (round==0?awl::WorldMapAnimationChannelBranch::NoClip:round==1?awl::WorldMapAnimationChannelBranch::First:
                    awl::WorldMapAnimationChannelBranch::Interrupted),"reselection reaches no-clip, first and interrupted branches with owned records");
            expect(channel->apply_settings(model.get(),1,1,&channel_step)==awl::WorldMapAnimationChannelStatus::Advanced && model->playback() &&
                model->playback()->clip_10 && model->playback()->clip_10->offset==c.reference.offset,
                "settings copy selected real clip and preserve reached blend graph metadata");
            hash_channel(channel_digest,index,round,*channel,*model,channel_step);++channel_cases;
        }
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
    std::cout<<"LOCAL_OWNED_CHANNEL_SEQUENCE "<<channel_cases<<' '<<std::hex<<channel_digest<<std::dec<<'\n';
    expect(channel_cases==288 && channel_digest==0x3ef5a54351d90cf9ull,
        "all owned constructor/setup/settings sequences match independent original-instruction state comparisons");
}
std::vector<uint8_t> start_dol() {
    // Entirely invented section/table/descriptor contents. Only the fixed
    // lookup addresses and selector-one jump witness come from the trace.
    std::vector<uint8_t> b(0x1C1);
    constexpr uint32_t addresses[]{0x8029E4BC,0x80249238,0x8029BEF4,0x81001000,0x80282DF8};
    constexpr uint32_t offsets[]{0x100,0x120,0x140,0x160,0x1C0};
    constexpr uint32_t sizes[]{4,8,16,96,1};
    for(size_t i=0;i<5;++i){word(b,i*4,offsets[i]);word(b,0x48+i*4,addresses[i]);word(b,0x90+i*4,sizes[i]);}
    word(b,0x100,0x800289A8);
    for(uint32_t i=0;i<4;++i){
        b[0x120+i*2+1]=uint8_t(10+i);
        word(b,0x140+i*4,0x81001000+i*24);
        word(b,0x160+i*24,(1u<<10)|(i&1));
        word(b,0x164+i*24,63u|(127u<<11)|(255u<<19));
        word(b,0x168+i*24,0x1500+i);
    }
    return b;
}
awl::WorldMapAnimationInitializerState start_initializer() {
    awl::WorldMapAnimationInitializerState state;
    state.animation.base_descriptor_4=77;state.animation.current_descriptor_0=88;
    state.animation.model_identity_30=100;state.animation.flag_21=9;
    state.primary={0,0,20,21,22,0.25f,0};
    for(uint64_t id:{10ull,20ull,21ull,22ull})state.records.push_back({id,{0,1,0,5,std::nullopt,0,0}});
    state.has_optional_bindings=true;
    state.model_links=awl::WorldMapModelLinkState{{awl::WorldMapModelLinkNode{100}}};
    return state;
}
void start_animation_synthetic() {
    using S=awl::WorldMapPlayerStartAnimationStatus;
    using Tables=awl::WorldMapPlayerStartAnimationTables;
    Fixture f;f.complete();std::filesystem::create_directory(f.root/"sys");
    std::shared_ptr<const Tables> tables;
    auto b=start_dol();
    expect(awl::decode_world_map_player_start_animation_tables(b.data(),b.size(),&tables)==S::Decoded &&
        tables && !tables->target_verified(),"invented DOL tables decode into an immutable unverified owner");
    if(!tables)return;
    const auto* original=tables.get();
    awl::WorldMapPlayerStartAnimationCommand command;
    awl::WorldMapPlayerStartAnimationSelection selection;
    expect(tables->select(command,std::nullopt,&selection)==S::Selected && selection.choice==0 &&
        selection.descriptor_index==10 && selection.descriptor.identity==0x81001000 &&
        selection.descriptor.word_8==0x1500,"no item/action selects first record with all twelve bytes retained");
    command.action=0xFFFFFFFFu;command.variant=0xFFFFFFFFu;command.mode=0;
    expect(tables->select(command,std::nullopt,&selection)==S::Selected && selection.choice==2 &&
        selection.descriptor_index==12,"any nonzero action selects two; variant and mode are unread in selector one");
    command.item=7;
    expect(tables->select(command,std::nullopt,&selection)==S::RequiresItemType && selection.choice==2,
        "nonzero item cannot invent its metadata byte");
    expect(tables->select(command,awl::WorldMapPlayerStartItemType{7,5},&selection)==S::Selected &&
        selection.choice==1,"ordinary held item takes precedence over action");
    expect(tables->select(command,awl::WorldMapPlayerStartItemType{7,2},&selection)==S::Selected &&
        selection.choice==3,"special item type takes precedence over held/action branches");
    command.item=0x4FF;
    expect(tables->select(command,std::nullopt,&selection)==S::RequiresItemType &&
        tables->select(command,awl::WorldMapPlayerStartItemType{0x4FF,0},&selection)==S::Selected &&
        selection.choice==3,"4FF still reaches the metadata read, then overrides its category");
    expect(tables->select(command,awl::WorldMapPlayerStartItemType{7,0},&selection)==S::InvalidInput &&
        selection.choice==3,"wrong item metadata key preserves selection");
    command.item=0;command.action=0;
    expect(tables->select(command,awl::WorldMapPlayerStartItemType{0,1},&selection)==S::InvalidInput,
        "supplied empty-item byte must agree with owned table evidence");
    command.selector=2;
    expect(tables->select(command,std::nullopt,&selection)==S::UnsupportedCommand &&
        tables->select(command,std::nullopt,nullptr)==S::InvalidInput,"other selectors and missing output reject");
    command.selector=1;
    for(int fault=0;fault<13;++fault){
        auto bad=b;
        switch(fault){
        case 0:bad.resize(255);break;
        case 1:bad.pop_back();break;
        case 2:word(bad,0,0xFF);break;
        case 3:word(bad,0x90,1000);break;
        case 4:word(bad,0x48,0xFFFFFFFEu);break;
        case 5:word(bad,0x4C,0x8029E4BC);break;
        case 6:word(bad,4,0x100);break;
        case 7:word(bad,0x100,0x800289DC);break;
        case 8:bad[0x120]=bad[0x121]=0xFF;break;
        case 9:word(bad,0x140,0);break;
        case 10:word(bad,0x140,0x81001001);break;
        case 11:word(bad,0x140,0x81001058);break;
        case 12:word(bad,0x140,0x80200000);break;
        }
        expect(awl::decode_world_map_player_start_animation_tables(bad.data(),bad.size(),&tables)==S::UnsupportedLayout &&
            tables.get()==original,"bad DOL header/overlap/jump/index/pointer preserves the complete owner");
    }
    expect(awl::decode_world_map_player_start_animation_tables(nullptr,0,&tables)==S::InvalidInput &&
        awl::decode_world_map_player_start_animation_tables(b.data(),b.size(),nullptr)==S::InvalidInput,
        "null table decoder inputs reject");
    expect(awl::load_world_map_player_start_animation_tables(&tables)==S::ReadFailure && tables.get()==original,
        "unverified cached owner cannot bypass a missing target DOL");
    f.write("../sys/main.dol",b);
    expect(awl::load_world_map_player_start_animation_tables(&tables)==S::WrongDol && tables.get()==original,
        "synthetic shape cannot bypass exact target SHA1");
    allocation_probe::remaining=0;allocation_probe::enabled=true;
    const auto allocated=awl::decode_world_map_player_start_animation_tables(b.data(),b.size(),&tables);
    allocation_probe::enabled=false;
    expect(allocated==S::AllocationFailure && tables.get()==original,"table allocation failure preserves ownership");
    b.assign(b.size(),0);
    expect(tables->select(command,std::nullopt,&selection)==S::Selected && selection.descriptor.word_8==0x1500,
        "clearing source bytes cannot invalidate selected descriptor words");
    auto other_group=start_dol();word(other_group,0x160,1u<<25);
    std::shared_ptr<const Tables> group_tables;
    expect(awl::decode_world_map_player_start_animation_tables(other_group.data(),other_group.size(),&group_tables)==S::Decoded,
        "decoder retains unsupported descriptor group for explicit binding rejection");
    std::shared_ptr<const Assets> assets;
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::Loaded,"supplied animation banks own their clips");
    if(!assets)return;
    auto state=start_initializer();awl::WorldMapPlayerStartAnimationStep step;
    const awl::WorldMapAnimationModelBinding model_binding{100,10};
    expect(awl::prepare_world_map_player_start_animation(group_tables,command,std::nullopt,state,{},std::nullopt,{},&step)==S::RequiresGroup &&
        step.selection->descriptor.word_0>>25==1 && !step.binding,"unsupported group cannot substitute player group zero");
    expect(awl::prepare_world_map_player_start_animation({},command,std::nullopt,state,{},std::nullopt,{},&step)==S::RequiresTables,
        "command cannot select without descriptor tables");
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,{},std::nullopt,{},&step)==S::RequiresAssets &&
        step.selection->choice==0 && !step.initializer,"changed descriptor needs owned assets before initializer reads");
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,std::nullopt,{},&step)==S::InitializerIncomplete &&
        step.initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresModelSetup &&
        state.animation.base_descriptor_4==77,"selected descriptor reaches actual model-binding dependency without state acceptance");
    auto missing=state;missing.has_optional_bindings=false;
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,missing,assets,model_binding,{},&step)==S::InitializerIncomplete &&
        step.initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresOptionalBindings &&
        missing.animation.base_descriptor_4==77,"late unknown optional bindings do not accept the earlier initializer prefix");
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,model_binding,{},&step)==S::Prepared &&
        step.initializer_status==awl::WorldMapAnimationInitializerStatus::Prepared && step.initializer->loop &&
        step.initializer->after.animation.base_descriptor_4==selection.descriptor.identity &&
        step.initializer->after.records[0].state.clip_10 &&
        step.initializer->after.records[0].state.clip_10->bank_identity==assets->primary_animations().identity() &&
        step.initializer->after.records[0].state.limit_c==3 && state.animation.base_descriptor_4==77,
        "owned selection and supplied model/channel/hierarchy compose complete metadata without mutating input");
    const auto prior=step.selection->descriptor.identity;
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,model_binding,{},nullptr)==S::InvalidInput,
        "null animation proposal output rejects");
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,step.initializer->after,assets,model_binding,{},&step)==S::InvalidInput &&
        step.selection->descriptor.identity==prior,"aliased initializer output rejects without losing evidence");
    auto equal=state;equal.animation.base_descriptor_4=selection.descriptor.identity;
    equal.animation.current_descriptor_0=selection.descriptor.identity+12;
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,equal,{},std::nullopt,{},&step)==S::Unchanged &&
        !step.binding && step.initializer->after.animation.current_descriptor_0==selection.descriptor.identity+12,
        "equal base skips bank/setup even when current descriptor advanced");
    command.item=7;
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,equal,{},std::nullopt,{},&step)==S::RequiresItemType &&
        !step.selection,"descriptor equality cannot skip the earlier item metadata dependency");
    command.item=0;
    // Fail the playback-record data allocation. MSVC Debug also allocates
    // tiny proxies inside noexcept vector constructors; those terminate on
    // injected failure before ordinary bad_alloc handling can run.
    const awl::WorldMapAnimationInitializerObservations allocation_observations;
    allocation_probe::minimum_size=state.records.size()*sizeof(awl::WorldMapAnimationPlaybackRecord);
    allocation_probe::remaining=0;allocation_probe::enabled=true;
    const auto failed=awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,model_binding,allocation_observations,&step);
    allocation_probe::enabled=false;
    allocation_probe::minimum_size=0;
    expect(failed==S::AllocationFailure && !step.selection && state.animation.base_descriptor_4==77,
        "initializer allocation failure preserves previous output and original state");
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,model_binding,{},&step)==S::Prepared,
        "ownership retention prepares again from original inputs");
    const auto bank_id=assets->primary_animations().identity();
    assets.reset();tables.reset();awl::WorldMapAnimationClip retained;
    expect(step.tables && step.binding->assets->primary_animations().identity()==bank_id &&
        step.binding->assets->primary_animations().resolve(0,&retained) &&
        step.tables->select(command,std::nullopt,&selection)==S::Selected,
        "returned descriptor/bank owners survive caller owner release");
}
void start_animation_local(const char* disc) {
    using S=awl::WorldMapPlayerStartAnimationStatus;
    awl::filesystem_init();expect(awl::filesystem_mount("/",disc),"local startup animation mount succeeds");
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    std::shared_ptr<const Assets> assets;
    expect(awl::load_world_map_player_start_animation_tables(&tables)==S::Loaded && tables && tables->target_verified(),
        "mounted DOL identity and bounded startup tables verify");
    expect(awl::load_world_map_player_animation_assets(&assets)==Status::Loaded,"actual player banks load");
    if(!tables || !assets)return;
    auto state=start_initializer();awl::WorldMapPlayerStartAnimationStep step;
    awl::WorldMapPlayerStartAnimationCommand command;
    const awl::WorldMapAnimationModelBinding model_binding{100,10};
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,model_binding,{},&step)==S::Prepared &&
        step.selection->choice==0 && step.selection->descriptor_index==4 &&
        step.initializer->start->primary_setup->clip_index==0 && step.initializer->loop &&
        !step.initializer->start->fields->secondary_index && state.animation.base_descriptor_4==77,
        "actual no-item descriptor selects clip zero and prepares complete supplied-state metadata");
    const auto* cached=tables.get();
    expect(awl::load_world_map_player_start_animation_tables(&tables)==S::Loaded && tables.get()==cached,
        "verified immutable table owner reuses safely");
    command.action=1;
    expect(awl::prepare_world_map_player_start_animation(tables,command,std::nullopt,state,assets,model_binding,{},&step)==S::InitializerIncomplete &&
        step.selection->choice==2 && step.selection->descriptor_index==6 &&
        step.initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresSecondarySetup &&
        step.initializer->secondary_setup && state.animation.base_descriptor_4==77,
        "actual action branch reaches explicit owned secondary-model setup instead of fake completion");
    std::cout<<"LOCAL_START_ANIMATION: no-item clip0 metadata prepared; action secondary setup explicit; no parent acceptance\n";
}
void start_animation_choice_matrix() {
    uint64_t digest=14695981039346656037ull;unsigned cases=0;
    for(uint32_t item:{0u,1u,0x4FFu,0x500u,UINT32_MAX}) {
        for(uint32_t action:{0u,1u,UINT32_MAX}) for(uint32_t type=0;type<256;++type) {
            const auto choice=awl::classify_world_map_player_start_animation(item,action,uint8_t(type));
            for(uint32_t w:{item,action,type,choice})hash(digest,w);
            ++cases;
        }
    }
    std::cout<<"START_ANIMATION_CHOICE_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    // Independent FUN_80029138 instruction walk with a supplied metadata
    // byte at its lookup address; not validation of live item IDs/assets.
    expect(cases==3840 && digest==0xE23715C3B5D71F62ull,"all byte categories and ordered item/action cases match the mapped trace");
}
} // namespace
int main(int argc,char** argv){
#if defined(_MSC_VER) && defined(_DEBUG)
    for(int kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}){
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE);_CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
#endif
    synthetic();channel_constructor();awl::filesystem_shutdown();
    start_animation_synthetic();start_animation_choice_matrix();awl::filesystem_shutdown();
    if(argc==3 && std::string(argv[1])=="--player-banks-local")local(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-start-animation-local")start_animation_local(argv[2]);
    else if(argc!=1)expect(false,"usage: --player-banks-local <disc> | --player-start-animation-local <disc>");
    awl::filesystem_shutdown();return failures?1:0;
}
