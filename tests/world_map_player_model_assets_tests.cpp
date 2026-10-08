#include "awl/world_map_player_model_assets.h"
#include "awl/world_map_player_model_auxiliary.h"
#include "awl/world_map_player_model_setup.h"
#include "awl/world_map_player_draw_commands.h"
#include "awl/world_map_player_skin_work.h"
#include "awl/world_map_player_frame.h"
#include "awl/world_map_player_topology.h"
#include "awl/world_map_player_geometry.h"
#include "awl/world_map_player_draw_state.h"
#include "awl/world_map_player_primary_owner.h"
#include "awl/world_map_player_start_animation.h"
#include "awl/world_map_player_animation_holder.h"
#include "awl/filesystem.h"

#include <chrono>
#include <cfenv>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <tuple>
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
using Status=awl::WorldMapPlayerModelAssetsStatus;
using Assets=awl::WorldMapPlayerModelAssets;
using Bank=awl::WorldMapPlayerTextureBank;
int failures=0;
void expect(bool yes,const char* why){if(!yes){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
void word(std::vector<uint8_t>& b,size_t p,uint32_t w){for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(w>>(24-i*8));}
void hash(uint64_t& h,uint32_t w){for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}

// Invented bounded payloads. GPL/SKN/TAM bytes deliberately have no format
// meaning: retaining an opaque extent cannot establish an auxiliary model.
std::vector<uint8_t> model(bool unsupported_rotation=false){
    std::vector<uint8_t> b(116);word(b,0,0x007b7960);word(b,4,1);word(b,12,32);word(b,20,0xffff0000);
    word(b,32,64);word(b,52,0x00010000);word(b,56,0x01000000);
    if(unsupported_rotation){word(b,64,0x02000000);word(b,80,0x3f800000);}
    return b;
}
std::vector<uint8_t> tpl(uint32_t count){
    const size_t headers=12+size_t(count)*8,data=headers+size_t(count)*36;
    std::vector<uint8_t> b(data+size_t(count)*32);word(b,0,0x0020af30);word(b,4,count);word(b,8,12);
    for(uint32_t i=0;i<count;++i){
        const size_t header=headers+size_t(i)*36;word(b,12+size_t(i)*8,uint32_t(header));
        word(b,header,0x00080008);word(b,header+4,0);word(b,header+8,uint32_t(data+size_t(i)*32));
        std::memset(b.data()+data+size_t(i)*32,int(17+i),32);
    }
    return b;
}
std::vector<uint8_t> archive(const std::vector<std::vector<uint8_t>>& files){
    const size_t count=files.size()+1,metadata=count*12+1+files.size()*6;
    std::vector<uint8_t> b((32+metadata+31)&~size_t(31));word(b,0,0x55aa382d);word(b,4,32);
    word(b,8,uint32_t(metadata));word(b,12,uint32_t(b.size()));word(b,32,0x01000000);word(b,40,uint32_t(count));
    for(size_t i=0;i<files.size();++i){
        const size_t node=44+i*12,name=32+count*12+1+i*6;
        constexpr char text[]="entry";std::memcpy(b.data()+name,text,sizeof(text));
        word(b,node,uint32_t(1+i*6));word(b,node+4,uint32_t(b.size()));word(b,node+8,uint32_t(files[i].size()));
        b.insert(b.end(),files[i].begin(),files[i].end());b.resize((b.size()+31)&~size_t(31));
    }
    return b;
}
std::vector<std::vector<uint8_t>> files(){return {model(),{2,3,4},{5,6,7,8},tpl(2),tpl(1),tpl(1),{9}};}
struct Fixture {
    std::filesystem::path root;
    Fixture(){
        root=std::filesystem::temp_directory_path()/std::filesystem::path("awl-player-model-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(root))throw std::runtime_error("Fixture directory already exists");
        std::filesystem::create_directory(root/"files");
        expect(awl::filesystem_mount("/",root.string().c_str()),"synthetic disc mounts");
    }
    ~Fixture(){
        std::error_code e;
        for(const char* name:{"boy_0.arc","boy_1.arc","boy_2.arc","boy_0.anm.arc","boy_0_subanm.arc","boy_0_subact.arc","char_com_eye.tam","char_com_mouth.tam"})std::filesystem::remove(root/"files"/name,e);
        std::filesystem::remove(root/"sys"/"main.dol",e);std::filesystem::remove(root/"sys",e);
        std::filesystem::remove(root/"files",e);std::filesystem::remove(root,e);
    }
    void write(const char* name,const std::vector<uint8_t>& b){
        std::ofstream f(root/"files"/name,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));
        if(!f)throw std::runtime_error("Fixture write failed");
    }
};

void embedded_tpl(){
    auto bytes=tpl(2);awl::TplFile owned;
    expect(awl::tpl_load_from_memory(bytes.data(),bytes.size(),&owned),"embedded TPL parses with shared bounds");
    if(owned.textures.size()!=2){expect(false,"two embedded descriptors survive");return;}
    const auto* old_data=owned.raw_file_data;
    expect(old_data!=bytes.data() && owned.textures[1].raw_data[0]==18,"embedded TPL owns a private copy");
    bytes.assign(1,255);expect(owned.textures[0].raw_data[0]==17,"input mutation/release cannot invalidate TPL views");
    expect(!awl::tpl_load_from_memory(nullptr,0,&owned) && owned.raw_file_data==old_data,"null bytes preserve existing TPL");
    expect(!awl::tpl_load_from_memory(bytes.data(),bytes.size(),nullptr),"null TPL output rejects");
    expect(awl::tpl_load_from_memory(static_cast<const uint8_t*>(owned.raw_file_data),owned.raw_file_size,&owned) &&
        owned.textures.size()==2 && owned.textures[1].raw_data[0]==18,"input/output storage alias copies before freeing");
    auto malformed=tpl(1);word(malformed,12,UINT32_MAX);
    expect(!awl::tpl_load_from_memory(malformed.data(),malformed.size(),&owned) && !owned.raw_file_data && owned.textures.empty(),
        "malformed embedded TPL clears prior ownership and rejects image-header overflow");
}

std::vector<uint8_t> skin(){
    std::vector<uint8_t> b(384);word(b,0,0x00010001);word(b,4,0x00010d00);
    word(b,8,32);word(b,12,96);word(b,16,212);word(b,20,48);word(b,24,1);word(b,28,320);
    word(b,32+48,300);word(b,32+52,0);
    word(b,96+96,304);word(b,96+100,308);word(b,96+104,12);
    word(b,212+48,312);word(b,212+60,316);word(b,212+52,0);word(b,212+56,24);
    return b;
}
std::vector<uint8_t> skinned_gpl(){
    std::vector<uint8_t> b(640);word(b,0,0x005bbc61);word(b,12,2);word(b,16,20);
    // Serialized order differs from physical order, so first-match handling
    // must use the paired table rather than sort the selected resources.
    word(b,20,256);word(b,24,608);word(b,28,64);word(b,32,616);
    for(size_t at:{size_t(64),size_t(256)}){
        word(b,at,24);word(b,at+16,120);word(b,at+24,40);
        word(b,at+28,0x00030006);word(b,at+124,132);b[at+129]=2;
        b[at+132]=1;b[at+133]=0;b[at+148]=3;b[at+149]=255;
    }
    std::memcpy(b.data()+608,"first",6);std::memcpy(b.data()+616,"second",7);
    return b;
}
void skin_metadata_checks(){
    using S=awl::WorldMapSkinMetadataStatus;using Space=awl::WorldMapSkinReferenceSpace;
    auto b=skin();awl::WorldMapSkinMetadata out;out.word_18=999;
    expect(awl::prepare_world_map_skin_metadata(nullptr,b.size(),64,&out)==S::InvalidInput && out.word_18==999,"null SKN data preserves output");
    expect(awl::prepare_world_map_skin_metadata(b.data(),b.size(),64,nullptr)==S::InvalidInput,"null SKN output rejects");
    for(size_t size=0;size<=320;++size)expect(awl::prepare_world_map_skin_metadata(b.data(),size,64,&out)!=S::Prepared && out.word_18==999,
        "every truncated required SKN prefix rejects without partial metadata");
    expect(awl::prepare_world_map_skin_metadata(b.data(),b.size(),64,&out)==S::Prepared &&
        out.counts==std::array<uint16_t,3>{1,1,1} && out.tables[0]==32u && out.tables[1]==96u && out.tables[2]==212u &&
        out.relocations.size()==14 && out.output_buffer_bound==64u,"three SKN record layouts and conditional root relocations prepare");
    constexpr uint32_t locations[]={8,12,16,20,28,80,84,192,196,200,260,272,264,268};
    constexpr uint32_t targets[]={32,96,212,48,320,300,0,304,308,12,312,316,0,24};
    for(size_t i=0;i<14 && i<out.relocations.size();++i){const auto& r=out.relocations[i];
        const auto space=i==3 || i==6 || i==9 || i==13?Space::OutputBuffer:Space::Asset;
        expect(r.location==locations[i] && r.target==targets[i] && r.space==space,"exact pointer store order preserves asset/output spaces and zero record targets");}
    const auto before=out.relocations.size();
    auto relocated=b;relocated[7]=255;
    expect(awl::prepare_world_map_skin_metadata(relocated.data(),relocated.size(),64,&out)==S::UnsupportedLayout && out.relocations.size()==before,
        "already-relocated SKN pointers cannot enter serialized parsing");
    auto overlap=b;word(overlap,12,80);
    expect(awl::prepare_world_map_skin_metadata(overlap.data(),overlap.size(),64,&out)==S::UnsupportedLayout,"overlapping writable record tables reject");
    auto header=b;word(header,8,4);
    expect(awl::prepare_world_map_skin_metadata(header.data(),header.size(),64,&out)==S::InvalidInput,"record table cannot alias count header");
    for(size_t location:{size_t(8),size_t(12),size_t(16),size_t(28),size_t(80),size_t(192),size_t(196),size_t(260),size_t(264),size_t(272)}){
        auto invalid=b;word(invalid,location,UINT32_MAX);
        expect(awl::prepare_world_map_skin_metadata(invalid.data(),invalid.size(),64,&out)==S::InvalidInput && out.relocations.size()==before,
            "each reached asset-relative target rejects overflow without output mutation");
    }
    for(size_t location:{size_t(20),size_t(84),size_t(200),size_t(268)}){
        auto invalid=b;word(invalid,location,65);
        expect(awl::prepare_world_map_skin_metadata(invalid.data(),invalid.size(),64,&out)==S::InvalidInput,"each output offset obeys the supplied buffer bound");
        expect(awl::prepare_world_map_skin_metadata(invalid.data(),invalid.size(),std::nullopt,&out)==S::Prepared,
            "unbound offset metadata remains inspectable without claiming a writable output buffer");
    }
    auto absent=std::vector<uint8_t>(32);word(absent,20,UINT32_MAX);
    expect(awl::prepare_world_map_skin_metadata(absent.data(),absent.size(),0,&out)==S::Prepared &&
        out.relocations.empty() && out.word_14==UINT32_MAX && !out.pointer_1c,
        "empty tables and zero root pointers stay absent; ungated word14 remains opaque");
    auto null_table=b;word(null_table,8,0);
    expect(awl::prepare_world_map_skin_metadata(null_table.data(),null_table.size(),64,&out)==S::InvalidInput,"nonnull count requires its table");
}
void auxiliary_checks(){
    using A=awl::WorldMapPlayerModelAuxiliaryStatus;using Owner=awl::WorldMapPlayerModelAuxiliaryMetadata;
    Fixture fixture;auto payloads=files();payloads[1]=skinned_gpl();payloads[2]=skin();
    std::shared_ptr<const Assets> assets;std::unique_ptr<Owner> owner;
    expect(awl::prepare_world_map_player_model_auxiliary(nullptr,&owner)==A::RequiresAssets && !owner,"auxiliary metadata requires actual providers");
    expect(awl::prepare_world_map_player_model_auxiliary(nullptr,nullptr)==A::InvalidInput,"null auxiliary output rejects first");
    fixture.write("boy_0.arc",archive(payloads));
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_auxiliary(assets,&owner)==A::Prepared && owner,"GPL/SKN metadata owns one retained provider");
    if(!owner)return;
    awl::WorldMapPlayerModelAssetView gpl;expect(assets->resource(2,&gpl),"owned GPL extent resolves");
    expect(owner->gpl().sections.size()==2 && owner->skin().relocations.size()==14 && owner->skin_target() &&
        owner->skin_target()->section_index==0 && owner->skin_target()->data.offset==gpl.reference.offset+296 &&
        owner->skin_target()->data.bank_identity==gpl.reference.bank_identity && owner->skin_buffer_size()==64,
        "first serialized component-six header supplies the GPL-bound skin output and aligned buffer size");
    expect(awl::prepare_world_map_player_model_auxiliary(owner->assets(),&owner)==A::Prepared && owner->assets()==assets,
        "provider/output-owner alias retains assets before replacing the borrowed input owner");
    awl::WorldMapGplMetadata metadata;
    expect(awl::prepare_world_map_gpl_metadata(payloads[1],awl::WorldMapGplSkinPolicy::Reject,&metadata)==awl::WorldMapGplMetadataStatus::RequiresSkin,
        "shared GPL extraction keeps the held-item skin rejection policy");
    expect(awl::prepare_world_map_gpl_metadata(payloads[1],static_cast<awl::WorldMapGplSkinPolicy>(255),&metadata)==awl::WorldMapGplMetadataStatus::InvalidInput,
        "invalid skin metadata policy rejects");
    const auto* retained=owner.get();auto null_first=payloads;word(null_first[1],256+24,0);
    fixture.write("boy_1.arc",archive(null_first));
    expect(awl::load_world_map_player_model_assets(3,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_auxiliary(assets,&owner)==A::Prepared && !owner->skin_target() && owner->skin_buffer_size()==64 &&
        !owner->skin().output_buffer_bound && owner.get()!=retained,
        "first null-data component-six match retains size and does not bind a later nonnull section");
    auto later=payloads;later[1][256+31]=3;fixture.write("boy_0.arc",archive(later));
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_auxiliary(assets,&owner)==A::Prepared && owner->skin_target() &&
        owner->skin_target()->section_index==1,"nonmatching first position continues to the later component-six section");
    auto no_skin=payloads;no_skin[1][64+31]=3;no_skin[1][256+31]=3;fixture.write("boy_2.arc",archive(no_skin));
    expect(awl::load_world_map_player_model_assets(5,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_auxiliary(assets,&owner)==A::Prepared && !owner->skin_target() && owner->skin_buffer_size()==0,
        "no component-six position leaves SKN binding absent and buffer size zero");
    const auto* prior=owner.get();
    for(unsigned kind=0;kind<4;++kind){
        auto invalid=payloads;
        if(kind==0)word(invalid[1],256+24,608); // Crosses trailing name boundary.
        if(kind==1)word(invalid[1],256+24,120); // Aliases material/command metadata.
        if(kind==2)invalid[2][7]=255;
        if(kind==3)word(invalid[2],96+104,65);
        fixture.write("boy_1.arc",archive(invalid));assets.reset();
        expect(awl::load_world_map_player_model_assets(3,&assets)==Status::Loaded &&
            awl::prepare_world_map_player_model_auxiliary(assets,&owner)==A::UnsupportedLayout && owner.get()==prior,
            "invalid geometry/skin binding cannot replace an existing metadata owner");
    }
    std::weak_ptr<const Assets> lifetime=owner->assets();assets.reset();
    expect(!lifetime.expired() && owner->assets()->resource(3,&gpl),"auxiliary metadata retains opaque payload ownership");
    owner.reset();expect(lifetime.expired(),"last metadata owner releases retained providers");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    fixture.write("boy_0.arc",archive(payloads));expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"allocation provider reloads");
    expect(awl::prepare_world_map_player_model_auxiliary(assets,&owner)==A::Prepared,"allocation metadata baseline prepares");
    const auto* old=owner.get();const auto live=allocation_probe::live;size_t rejected=0;bool prepared=false;
    for(size_t fail=0;fail<128;++fail){
        allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::prepare_world_map_player_model_auxiliary(assets,&owner);allocation_probe::enabled=false;
        if(status==A::Prepared){prepared=true;break;}++rejected;
        expect(status==A::AllocationFailure && owner.get()==old && allocation_probe::live==live,"failed auxiliary preparation preserves prior owner and releases staging");
    }
    expect(prepared && rejected>5,"auxiliary allocation sweep reaches metadata publication");
    std::cout<<"PLAYER_AUXILIARY_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}

// Invented hierarchy with repeated priorities and a reversed GPL section
// table. These bytes exercise layout decisions; no game payload is embedded.
std::vector<uint8_t> setup_model(bool extended=true){
    std::vector<uint8_t> b(320);word(b,0,0x007b7960);word(b,4,3);word(b,12,32);
    word(b,20,extended?0u:0xffff0000u);
    for(uint32_t i=0;i<3;++i){
        const uint32_t at=32+i*28;word(b,at,128+i*52);
        word(b,at+20,(i==1?1u:0u)<<16);word(b,at+24,(i==0?5u:2u)<<16);
    }
    word(b,48,60);word(b,68,88); // Child, then the child's sibling.
    return b;
}
std::vector<uint8_t> setup_gpl(){
    std::vector<uint8_t> b(1024);word(b,0,0x005bbc61);word(b,12,2);word(b,16,20);
    word(b,20,512);word(b,24,1008);word(b,28,64);word(b,32,1016);
    constexpr uint8_t types[]={1,3,2,1,1,1},channels[]={2,0,0,0,255,1};
    for(size_t at:{size_t(64),size_t(512)}){
        word(b,at,24);word(b,at+16,120);word(b,at+24,40);word(b,at+28,0x00030006);
        word(b,at+124,132);b[at+129]=6;
        for(size_t i=0;i<6;++i){const size_t p=at+132+i*16;
            b[p]=types[i];b[p+1]=channels[i];word(b,p+4,uint32_t(5+i));
            if(i==2){word(b,p+8,256);word(b,p+12,16);}
        }
    }
    std::memcpy(b.data()+1008,"first",6);std::memcpy(b.data()+1016,"second",7);return b;
}
void setup_checks(){
    using S=awl::WorldMapPlayerModelSetupStatus;using Plan=awl::WorldMapPlayerModelSetupPlan;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=setup_gpl();payloads[2]=skin();
    std::shared_ptr<const Assets> assets;std::unique_ptr<Plan> plan;
    expect(awl::prepare_world_map_player_model_setup(nullptr,nullptr)==S::InvalidInput,"setup null output rejects first");
    expect(awl::prepare_world_map_player_model_setup(nullptr,&plan)==S::RequiresAssets && !plan,"setup requires retained providers");
    auto load=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"setup provider loads");};
    load();expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::PreparedPlan && plan,"complete primary setup plan prepares");
    if(!plan)return;
    expect(plan->command_sections().size()==2 && plan->command_packets_offset()==288 && plan->command_allocation_size()==1184,
        "4078 reserves all tables before one alignment and per-section packet budgets");
    for(size_t i=0;i<2;++i){const auto& section=plan->command_sections()[i];
        expect(section.table_offset==8+i*16 && section.commands_offset==40+i*120 && section.packet_budget==448 && section.commands.size()==6,
            "serialized section order and twenty-byte command records stay intact");
        for(size_t j=0;j<section.commands.size();++j){const auto& command=section.commands[j];
            expect(command.table_offset==section.commands_offset+j*20 && command.packet_budget==(j==2?32u:64u),"command budget preserves type-two distinction");
            if(j==2)expect(command.display_list && command.display_list_size==16 && !command.texture,"bounded display-list reference remains opaque and untextured");
        }
        expect(section.commands[0].texture && section.commands[0].texture->bank==Bank::Body && section.commands[0].texture->index==1 &&
            section.commands[3].texture && section.commands[3].texture->bank==Bank::Eyes &&
            section.commands[4].texture && section.commands[4].texture->bank==Bank::Body && section.commands[4].texture->index==0 &&
            section.commands[5].texture && section.commands[5].texture->bank==Bank::Mouth && !section.commands[1].texture,
            "FF selects only FF; all four texture routes resolve their own command records");
    }
    constexpr uint32_t write_commands[]={4,4,3,3,5,5,0,0};
    expect(plan->texture_writes().size()==8,"duplicate channel matches retain every ordered selection");
    for(size_t i=0;i<plan->texture_writes().size() && i<8;++i)expect(plan->texture_writes()[i].section==i%2 && plan->texture_writes()[i].command==write_commands[i],
        "texture routes precede serialized section and command scans");
    expect(plan->features().size()==4 && !plan->features()[0].node && plan->feature_storage_size()==320 &&
        plan->feature_node_order()==std::vector<uint32_t>{1,2,0},"root and three node features preserve stable equal-priority ordering");
    expect(plan->model_allocation_size()==1120 && plan->preallocated_size()==1184 &&
        plan->storage_requests().back().offset==944 && plan->storage_requests().back().size==32,
        "nonnull auxiliary feature size is included; D064 adds the separate skin buffer");
    for(size_t i=0;i<plan->features().size();++i){const auto& f=plan->features()[i];
        expect(f.storage_offset==624+i*80 && f.cache_offset==944 && f.group==(i==2?1:0),"all features use one final four-slot cache and the selected GPL group");
        expect(f.matrix==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0} && f.buffer_34==0 && f.flags_38==0 && f.enabled_3c==1 && f.byte_3d==0,
            "arena-backed feature constructor preserves matrix/buffer/flag/byte initialization");}
    expect(plan->cache_channels()==std::optional<std::array<uint32_t,4>>({UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}),
        "shared cache initializes channels without inventing payload values");
    expect(plan->core_before_features().feature_14==0 && plan->core_before_features().auxiliary_c==0,
        "setup plan cannot fabricate live model/auxiliary pointer identities");
    expect(awl::prepare_world_map_player_model_setup(plan->selection().assets,&plan)==S::PreparedPlan && plan->selection().assets==assets,
        "provider stored inside replaced plan safely aliases input");
    payloads[0]=setup_model(false);load();
    expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::PreparedPlan && plan->features().size()==3 &&
        plan->model_allocation_size()==752 && plan->preallocated_size()==832,"null root-feature path preserves distinct CF3C and D064 alignment");
    for(uint32_t i=0;i<3;++i)word(payloads[0],32+i*28+20,0xffff0000);load();
    expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::PreparedPlan && plan->features().empty() &&
        plan->feature_node_order().empty() && !plan->cache_channels() && plan->feature_storage_size()==0,
        "no features leaves final cache bytes unknown rather than fabricating initialization");
    for(unsigned kind=0;kind<7;++kind){payloads[0]=setup_model();payloads[1]=setup_gpl();
        if(kind==0)word(payloads[1],512+132+32+8,496); // At the first section's name boundary.
        if(kind==1)word(payloads[1],512+132+32+12,UINT32_MAX);
        if(kind==2)word(payloads[1],512+132+32+8,0); // Null pointer/nonempty list.
        if(kind==3)word(payloads[0],20,2u<<16); // Missing root group.
        if(kind==4)word(payloads[0],32+20,2u<<16); // Missing node group.
        if(kind==5)word(payloads[0],48,32); // Cyclic hierarchy.
        if(kind==6)payloads[1][512+132]=128; // Unsupported command type.
        load();const auto* old=plan.get();
        expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::UnsupportedLayout && plan.get()==old,
            "unsafe list/group/hierarchy/command layouts preserve prior complete plan");
    }
    payloads[0]=setup_model();payloads[1]=setup_gpl();word(payloads[0],128,0x02000000);word(payloads[0],128+16,0x3f800000);load();
    const auto* old=plan.get();
    expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::RequiresResourcePreparation && plan.get()==old,"untranslated Euler pose preserves setup plan");
    payloads[0]=setup_model();word(payloads[0],128,0x01000000);load();
    expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::SingularMatrix && plan.get()==old,"singular skin matrix stops before publication");
    std::weak_ptr<const Assets> lifetime=plan->selection().assets;assets.reset();
    expect(!lifetime.expired() && plan->auxiliary().assets()==plan->selection().assets,"plan retains all resource and texture providers");
    plan.reset();expect(lifetime.expired(),"discarding last setup owner releases its provider");
    payloads[0]=setup_model();payloads[1]=setup_gpl();load();
    expect(awl::prepare_world_map_player_model_setup(assets,&plan)==S::PreparedPlan,"setup allocation baseline prepares");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    old=plan.get();const auto live=allocation_probe::live;size_t rejected=0;bool prepared=false;
    for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::prepare_world_map_player_model_setup(assets,&plan);allocation_probe::enabled=false;
        if(status==S::PreparedPlan){prepared=true;break;}++rejected;
        expect(status==S::AllocationFailure && plan.get()==old && allocation_probe::live==live,"failed setup releases staging and preserves prior plan");
    }
    expect(prepared && rejected>40,"allocation sweep reaches complete setup publication");std::cout<<"PLAYER_SETUP_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}

std::vector<uint8_t> draw_gpl(uint8_t uv_count=1){
    auto b=setup_gpl();
    for(size_t at:{size_t(64),size_t(512)}){
        word(b,at+4,280);word(b,at+8,304);word(b,at+12,288);b[at+20]=uv_count;
        word(b,at+28,0x00033d06); // Three interleaved signed16 XYZ/normal records.
        word(b,at+280,80);word(b,at+284,0x00010003);word(b,at+80,0x1234abcd);
        word(b,at+288,46);word(b,at+292,0x00033d06);
        for(uint8_t i=0;i<uv_count && i<8;++i){word(b,at+304+size_t(i)*16,96);word(b,at+308+size_t(i)*16,0x00023e02);}
        word(b,at+132+16+4,1);word(b,at+132+32+4,0xc3c);
        word(b,at+132+4,(7u<<13)|8191); // Explicit descriptor overrides low13 image index.
    }
    return b;
}
void draw_checks(){
    using S=awl::WorldMapPlayerDrawStatus;using Owner=awl::WorldMapPlayerDrawCommands;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=draw_gpl();payloads[2]=skin();
    std::shared_ptr<const Assets> assets;std::unique_ptr<Owner> owner;
    auto load=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"drawing providers load");};
    expect(awl::prepare_world_map_player_draw_commands(nullptr,nullptr)==S::InvalidInput,"null drawing output rejects first");
    expect(awl::prepare_world_map_player_draw_commands(nullptr,&owner)==S::RequiresAssets && !owner,"drawing requires assets");
    load();expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters && owner,"CPU drawing parameters prepare");
    if(!owner)return;
    for(const auto& section:owner->sections()){
        expect(section.arrays.size()==3 && section.arrays[0].attribute==9 && section.arrays[0].stride==12 && section.arrays[0].bounded_size==30 &&
            section.arrays[1].attribute==13 && section.arrays[1].stride==4 && section.arrays[1].bounded_size==8 &&
            section.arrays[2].attribute==10 && section.arrays[2].stride==12 && section.arrays[2].bounded_size==30,"interleaved arrays preserve call order and last-element extents");
        expect(section.vat_words==std::array<uint32_t,3>{0x5cf76cd7,0xc8241209,0x04824120} &&
            section.matrix_indices==std::array<uint8_t,9>{0,60,60,60,60,60,60,60,60},"VAT and matrix operands match mapped original routines");
        expect(section.constant_color==std::optional<std::array<uint8_t,4>>({16,69,165,255}),"RGB565 constant excludes color array format");
        const auto& texture=std::get<awl::WorldMapPlayerTextureParameters>(section.commands[0]);
        expect(texture.unit==7 && texture.binding.bank==Bank::Body && texture.binding.index==1 && !texture.mipmap && !texture.bias_clamp && texture.anisotropy==0,
            "explicit selected descriptor ignores embedded image index and preserves sampler defaults");
        const auto& d=std::get<std::vector<awl::WorldMapPlayerVertexDescriptor>>(section.commands[2]);
        expect(d.size()==3 && d[0].attribute==9 && d[0].mode==3 && d[1].attribute==10 && d[2].attribute==13,"VCD signed16 player profile decodes index16 descriptors");
        const auto& tev=std::get<awl::WorldMapPlayerTevParameters>(section.commands[1]);
        expect(tev.color_inputs==std::array<uint8_t,4>{15,8,10,15} && tev.alpha_inputs==std::array<uint8_t,4>{7,4,5,7} &&
            tev.texgen_source==4 && tev.post_matrix==125 && tev.raster_channel==4 && tev.texgens==1 && tev.color_channels==1 && tev.stages==1,
            "word-one TEV operands match original SDK boundary calls");
    }
    expect(awl::prepare_world_map_player_draw_commands(owner->setup().selection().assets,&owner)==S::PreparedParameters,"retained provider input safely aliases replaced drawing owner");
    constexpr std::array<uint8_t,4> colors[]={{16,69,165,255},{18,52,171,255},{18,52,171,255},{17,34,51,68},{0,4,32,211},{18,52,171,205}};
    for(uint8_t type=0;type<6;++type){payloads[1]=draw_gpl();payloads[1][512+286]=uint8_t(type<<4);load();
        expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters && owner->sections()[0].constant_color==colors[type],"all six constant-color branches match raw instruction samples");}
    payloads[1]=draw_gpl(8);word(payloads[1],512+284,0x00020003);word(payloads[1],512+292,0x00033d02);
    word(payloads[1],512+132+32+4,0x0bffffff);load();
    expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters,"eight UV arrays, RGB array, NBT3 and broad VCD prepare");
    if(owner->sections()[0].arrays.size()==11){const auto& section=owner->sections()[0];
        expect(section.arrays[1].attribute==11 && section.arrays[1].stride==2 && section.arrays.back().attribute==25 && section.arrays.back().stride==6 &&
            section.arrays.back().bounded_size==18 && section.vat_words==std::array<uint32_t,3>{0xdcf60ed7,0xbb9dcee7,0x73b9dcee},"NBT3 uses three-scalar stride and VAT bit31");
        const auto& d=std::get<std::vector<awl::WorldMapPlayerVertexDescriptor>>(section.commands[2]);
        expect(d.size()==14 && d[0].attribute==0 && d[0].mode==3 && d[1].attribute==25 && d[1].mode==2 && d.back().attribute==20,"matrix/NBT3 descriptors precede twelve attribute modes");
    }else expect(false,"eleven bindings survive broad setup");
    for(uint8_t type=0;type<6;++type){payloads[1]=draw_gpl();word(payloads[1],512+284,uint32_t(0x00020003)|(uint32_t(type)<<12));load();
        expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters && owner->sections()[0].arrays[1].stride==(type%3==0?2:type%3==1?3:4),"color arrays preserve original packed widths");}
    for(uint8_t type=0;type<5;++type){payloads[1]=draw_gpl();payloads[1][512+30]=uint8_t((type<<4)|7);load();
        expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters && owner->sections()[0].arrays[0].stride==(type<2?6:type<4?12:24),"all supported scalar widths preserve interleaved position stride");}
    payloads[1]=draw_gpl();auto sampler=tpl(2);const size_t headers=28,data=100;sampler.resize(data+256);
    for(size_t i=0;i<2;++i){const size_t at=headers+i*36;word(sampler,at+8,uint32_t(data+i*128));word(sampler,at+12,1);word(sampler,at+16,2);
        word(sampler,at+20,4);word(sampler,at+24,1);word(sampler,at+28,0x3e800000);sampler[at+32]=1;sampler[at+33]=1;sampler[at+34]=3;}
    payloads[3]=sampler;load();expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters,"mip sampler provider prepares");
    const auto& sampler_state=std::get<awl::WorldMapPlayerTextureParameters>(owner->sections()[0].commands[0]);
    expect(sampler_state.wrap_s==1 && sampler_state.wrap_t==2 && sampler_state.min_filter==4 && sampler_state.mag_filter==1 && sampler_state.mipmap &&
        sampler_state.edge_lod==1 && sampler_state.min_lod==1.0f && sampler_state.max_lod==3.0f && sampler_state.lod_bias==0.25f,"selected sampler preserves wrap/filter, LOD byte conversion, bias and mip flag");
    payloads[3]=tpl(2);
    for(unsigned kind=0;kind<15;++kind){payloads[1]=draw_gpl();S wanted=S::UnsupportedLayout;
        if(kind==0)word(payloads[1],512+24,495),wanted=S::RequiresModelSetup; // Last position would cross trailing names.
        if(kind==1)word(payloads[1],512+24,120),wanted=S::RequiresModelSetup; // Payload aliases writable material.
        if(kind==2)payloads[1][512+30]=0x5d;
        if(kind==3)word(payloads[1],512+288,0),wanted=S::RequiresNormalFallback;
        if(kind==4)payloads[1][512+295]=4;
        if(kind==5)word(payloads[1],512+304,UINT32_MAX);
        if(kind==6)payloads[1][512+20]=9;
        if(kind==7)word(payloads[1],512+132+16+4,2);
        if(kind==8)payloads[1][512+133]=3,wanted=S::RequiresTexture;
        if(kind==9)word(payloads[1],512+280,0);
        if(kind==10)payloads[1][512+286]=0x60;
        if(kind==11)word(payloads[1],512+308,0x00003e02);
        if(kind==12)word(payloads[1],512+288,495);
        if(kind==13)word(payloads[1],512+280,120);
        if(kind==14)word(payloads[1],512+308,0xffff3e02);
        load();const auto* prior=owner.get();
        const auto status=awl::prepare_world_map_player_draw_commands(assets,&owner);
        expect(status==wanted && owner.get()==prior,"unsupported reached drawing structures preserve complete owner");
    }
    payloads[1]=draw_gpl();word(payloads[1],512+4,0);word(payloads[1],512+288,0);load();
    expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters && owner->sections()[0].arrays.size()==2 && !owner->sections()[0].constant_color,
        "absent color bypasses normal fallback exactly as original branch");
    std::weak_ptr<const Assets> lifetime=owner->setup().selection().assets;assets.reset();
    expect(!lifetime.expired(),"drawing owner retains array and texture providers");owner.reset();expect(lifetime.expired(),"last drawing owner releases providers");
    payloads[1]=draw_gpl();load();expect(awl::prepare_world_map_player_draw_commands(assets,&owner)==S::PreparedParameters,"drawing allocation baseline prepares");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto* prior=owner.get();const auto live=allocation_probe::live;size_t rejected=0;bool prepared=false;
    for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::prepare_world_map_player_draw_commands(assets,&owner);allocation_probe::enabled=false;
        if(status==S::PreparedParameters){prepared=true;break;}++rejected;
        expect(status==S::AllocationFailure && owner.get()==prior && allocation_probe::live==live,"drawing allocation failure releases staging and preserves prior owner");}
    expect(prepared && rejected>69,"allocation sweep reaches drawing publication");std::cout<<"PLAYER_DRAW_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}

std::vector<uint8_t> work_skin(){
    std::vector<uint8_t> b(768);word(b,0,0x00010001);word(b,4,0x00010d00);
    word(b,8,64);word(b,12,128);word(b,16,244);word(b,20,40);word(b,24,16);word(b,28,512);word(b,32,3);
    word(b,64+48,320);word(b,64+52,0);word(b,64+56,0x00010002);b[64+60]=4;
    word(b,128+96,352);word(b,128+100,384);word(b,128+104,32);word(b,128+108,0x00000002);word(b,128+112,0x00020800);
    word(b,244+48,416);word(b,244+52,448);word(b,244+56,0);word(b,244+60,480);word(b,244+64,0x00020002);
    word(b,448,0x00010002);word(b,512,0x00000002);word(b,516,0x00020000);
    for(size_t row:{size_t(64),size_t(128),size_t(176),size_t(244)})for(size_t i:{size_t(0),size_t(5),size_t(10)})word(b,row+i*4,0x3f800000);
    return b;
}
void hash_skin_work(uint64_t&,uint32_t,const awl::WorldMapPlayerSkinWork&);
std::vector<awl::WorldMapModelMatrix> skin_palette(uint32_t seed,size_t count){
    std::vector<awl::WorldMapModelMatrix> palette(count);uint32_t state=seed+1;
    for(auto& matrix:palette)for(size_t i=0;i<12;++i){state=state*1664525u+1013904223u;
        matrix[i]=float(int((state>>16)%2049)-1024)/float(i%4==3?4096:256);}
    return palette;
}
std::vector<uint8_t> execution_skin(uint32_t seed){
    auto skin=work_skin();constexpr uint8_t scales[]{0,13,31};skin[6]=scales[seed%3];
    if(seed&1)word(skin,448,0x00020002);if(seed&2)word(skin,24,0);
    uint32_t state=seed+123;
    for(size_t start:{size_t(320),size_t(352),size_t(384),size_t(416),size_t(480)})for(size_t i=0;i<32;++i){
        state=state*1664525u+1013904223u;skin[start+i]=uint8_t(state>>24);}
    return skin;
}
std::array<uint32_t,13> explicit_pose(const awl::WorldMapModelMatrix& matrix){
    std::array<uint32_t,13> result{};result[0]=0x10000000;
    for(size_t i=0;i<12;++i)std::memcpy(&result[i+1],&matrix[i],4);
    return result;
}
awl::WorldMapPlayerFrameInput frame_input(uint32_t seed,size_t count){
    awl::WorldMapPlayerFrameInput result;result.root_pose=explicit_pose(skin_palette(seed+512,1)[0]);
    result.evaluate_nodes=(seed&2)!=0;result.request_skin=(seed&1)!=0;result.nodes.resize(count);
    const auto sampled=skin_palette(seed,count),post=skin_palette(seed+1024,count);
    for(size_t i=0;i<count;++i){if((seed+i)%3)result.nodes[i].sampled_pose=explicit_pose(sampled[i]);
        if((seed+i)%4==0)result.nodes[i].post_transform=post[i];}
    return result;
}
void hash_frame(uint64_t& digest,const awl::WorldMapPlayerFrame& frame){
    auto matrix=[&](const awl::WorldMapModelMatrix& m){for(float value:m){uint32_t raw=0;if(value!=0)std::memcpy(&raw,&value,4);hash(digest,raw);}};
    matrix(frame.root_matrix);hash(digest,frame.skin_executed?1:0);hash(digest,uint32_t(frame.node_matrices.size()));
    for(const auto& m:frame.node_matrices)matrix(m);hash(digest,uint32_t(frame.skin_palette.size()));
    for(const auto& m:frame.skin_palette)matrix(m);hash(digest,uint32_t(frame.feature_matrix_writes.size()));
    for(const auto& m:frame.feature_matrix_writes){hash(digest,m?1:0);if(m)matrix(*m);}
    hash(digest,uint32_t(frame.feature_write_order.size()));for(uint32_t index:frame.feature_write_order)hash(digest,index);
    hash(digest,uint32_t(frame.vertex_output.size()));for(uint8_t byte:frame.vertex_output)hash(digest,byte);
}
std::vector<uint8_t> animation_frame_fixture(uint32_t seed,uint32_t side) {
    std::vector<uint8_t> bytes(512);word(bytes,4,0x30000);
    for(uint32_t node=0;node<3;++node) {
        const size_t record=8+node*16;size_t cursor=64+node*128;
        const uint8_t tracks[]={0,1,3};const uint8_t flags=tracks[(seed+node)%3];
        const uint8_t components=flags==0?3:flags==1?2:0;
        word(bytes,record+4,uint32_t(cursor));bytes[record+9]=flags?3:0;
        const uint16_t id=uint16_t((seed+node+side)%4==0?node+7:node);bytes[record+10]=uint8_t(id>>8);bytes[record+11]=uint8_t(id);
        bytes[record+12]=0x37;bytes[record+13]=flags;bytes[record+14]=5;bytes[record+15]=components;
        uint32_t state=seed+1+side*991+node*17;
        auto value=[&](){state=state*1664525u+1013904223u;const uint16_t v=uint16_t(int((state>>16)%257)-128);
            bytes[cursor++]=uint8_t(v>>8);bytes[cursor++]=uint8_t(v);};
        if(components&2)for(unsigned i=0;i<3;++i)value();
        if(components&1)for(unsigned i=0;i<3;++i)value();
        if(flags)for(int time:{-3,2,9}) {
            const uint16_t t=uint16_t(time);bytes[cursor++]=uint8_t(t>>8);bytes[cursor++]=uint8_t(t);
            if(flags&2)for(unsigned i=0;i<3;++i)value();
            if(flags&1)for(unsigned i=0;i<3;++i)value();
        }
    }
    return bytes;
}
void animation_frame_checks() {
    using S=awl::WorldMapPlayerFrameStatus;using P=awl::WorldMapAnimationPoseStatus;
    Fixture fixture;auto payloads=files();payloads[1]=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;
    awl::WorldMapPlayerFrame frame;awl::WorldMapPlayerAnimationFrameInput input;
    awl::WorldMapAnimationBank first,second;std::vector<const awl::WorldMapAnimationBank*> banks{&first,&second};
    std::vector<awl::WorldMapAnimationPlaybackRecord> records;
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<256;++seed) {
        payloads[0]=setup_model();payloads[2]=execution_skin(seed);
        for(uint32_t i=0;i<3;++i)word(payloads[0],32+i*28+24,((seed+i)%3)<<24|((i==0?5u:2u)<<16));
        assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"animated frame providers load");
        expect(awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"animated frame work prepares");if(!work)return;
        expect(first.parse(200,animation_frame_fixture(seed,0)) && second.parse(201,animation_frame_fixture(seed,1)),"invented frame clips parse");
        frame.vertex_output.resize(64);for(size_t i=0;i<64;++i)frame.vertex_output[i]=uint8_t(i*7+seed);
        for(uint32_t pass=0;pass<2;++pass) {
            const auto supplied=frame_input(seed+pass*256,3);input={};input.root_pose=supplied.root_pose;
            input.evaluate_nodes=supplied.evaluate_nodes;input.request_skin=supplied.request_skin;
            for(const auto& node:supplied.nodes)input.node_post_transforms.push_back(node.post_transform);
            if(seed%8)input.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};
            input.playback.position_0=pass?4.25f:0.0f;input.playback.link_14=seed%4?2:0;
            input.playback.value_18=float(int(seed%9)-2)/8;
            awl::WorldMapAnimationPlayback linked;linked.clip_10=awl::WorldMapAnimationClipReference{201,0};linked.position_0=pass?9.0f:1.25f;
            records={{2,linked}};
            const auto result=awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,&frame);
            expect(result.status==S::Evaluated && !result.failed_node && !result.sampling_status,"sampled/blended animation feeds complete frame with atomic prior alias");
            hash(digest,seed);hash(digest,pass);hash_frame(digest,frame);
        }
    }
    std::cout<<"PLAYER_ANIMATION_FRAME_EVALUATION 512 digest "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0x23d7642c567b9eb5ull,"complete sampler/frame/skin instructions match for invented scale/translation clips");
    const auto before=frame;uint64_t before_hash=14695981039346656037ull;hash_frame(before_hash,before);
    auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_frame(h,frame);return h==before_hash;};
    input={};input.node_post_transforms.resize(3);input.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};
    auto damaged=animation_frame_fixture(255,0);word(damaged,8+2*16+4,UINT32_MAX);
    expect(first.parse(200,damaged),"late bad track remains opaque at parse");
    auto result=awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,&frame);
    expect(result.status==S::RequiresAnimationSampling && result.failed_node==2u && result.sampling_status==P::UnsupportedLayout && preserved(),
        "late track failure reports exact node/reason and preserves matrices, features and vertices");
    input.root_pose[0]=0x02000000;input.root_pose[4]=0x3f800000;
    result=awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,&frame);
    expect(result.status==S::RequiresPoseConversion && !result.failed_node && preserved(),"root conversion precedes node sampling failures");
    input.root_pose={};expect(first.parse(200,animation_frame_fixture(255,0)),"valid frame clip restores");
    input.playback.link_14=9;
    result=awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,&frame);
    expect(result.status==S::RequiresAnimationSampling && result.failed_node==0u && result.sampling_status==P::RequiresPlaybackRecord && preserved(),
        "reached missing blend record preserves complete frame");
    input.node_post_transforms.pop_back();
    expect(awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,&frame).status==S::InvalidInput && preserved(),
        "missing post observation rejects before sampling");
    input.node_post_transforms.resize(3);input.playback.link_14=0;
    expect(awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,nullptr).status==S::InvalidInput,"null animated frame output rejects");
    input.playback.clip_10.reset();input.playback.link_14=999;input.settings.quaternion_scale=std::numeric_limits<float>::quiet_NaN();
    expect(awl::evaluate_world_map_player_animation_frame(*work,input,{}, {},frame.vertex_output,&frame).status==S::Evaluated && frame.skin_executed,
        "null root clip reaches retained defaults without bank/link/settings evidence");
    input.evaluate_nodes=false;input.node_post_transforms.clear();input.playback.clip_10=awl::WorldMapAnimationClipReference{999,0};
    expect(awl::evaluate_world_map_player_animation_frame(*work,input,{}, {},frame.vertex_output,&frame).status==S::Evaluated && !frame.skin_executed && frame.node_matrices.empty(),
        "node-off skips all animation evidence and skinning");
    input={};input.node_post_transforms.resize(3);input.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto saved=frame;uint64_t saved_hash=14695981039346656037ull;hash_frame(saved_hash,saved);const auto live=allocation_probe::live;
    size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<32;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto step=awl::evaluate_world_map_player_animation_frame(*work,input,records,banks,frame.vertex_output,&frame);allocation_probe::enabled=false;
        if(step.status==S::Evaluated){reached=true;break;}++rejected;uint64_t h=14695981039346656037ull;hash_frame(h,frame);
        expect(step.status==S::AllocationFailure && h==saved_hash && allocation_probe::live==live,"animated-frame allocation failure releases staging and preserves output");}
    expect(reached && rejected==5,"animated frame adds no allocation before sampling beyond existing frame staging");
    std::cout<<"PLAYER_ANIMATION_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}
void frame_checks(){
    using S=awl::WorldMapPlayerFrameStatus;
    Fixture fixture;auto payloads=files();payloads[1]=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;
    auto prepare=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"frame providers load");
        expect(awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"frame setup prepares");};
    uint64_t digest=14695981039346656037ull;awl::WorldMapPlayerFrame frame;
    for(uint32_t seed=0;seed<256;++seed){payloads[0]=setup_model();payloads[2]=execution_skin(seed);
        for(uint32_t i=0;i<3;++i)word(payloads[0],32+i*28+24,((seed+i)%3)<<24|((i==0?5u:2u)<<16));prepare();if(!work)return;
        frame.vertex_output.resize(64);for(size_t i=0;i<64;++i)frame.vertex_output[i]=uint8_t(i*7+seed);
        for(uint32_t pass=0;pass<2;++pass){const auto input=frame_input(seed+pass*256,3);
            expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated,"complete supplied frame publishes atomically with prior-output alias");
            hash(digest,seed);hash(digest,pass);hash_frame(digest,frame);}
    }
    expect(digest==0xa22d86a6d7a4a50dull,"512 frame matrices, ordered feature writes and vertex outputs match mapped E438/C080 instructions");
    std::cout<<"PLAYER_FRAME_EVALUATION 512 digest "<<std::hex<<digest<<std::dec<<'\n';
    auto input=frame_input(3,3);const auto before=frame;uint64_t before_hash=14695981039346656037ull;hash_frame(before_hash,before);
    auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_frame(h,frame);return h==before_hash;};
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,nullptr)==S::InvalidInput,"null frame output rejects");
    input.nodes.pop_back();expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::InvalidInput && preserved(),"missing node observation preserves previous frame");
    input=frame_input(3,3);auto short_output=frame.vertex_output;short_output.pop_back();
    expect(awl::evaluate_world_map_player_frame(*work,input,short_output,&frame)==S::InvalidInput && preserved(),"wrong prior extent preserves frame");
    input.root_pose={};input.root_pose[0]=0x02000000;input.root_pose[4]=0x3f800000;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::RequiresPoseConversion && preserved(),"unsupported root Euler stops without publication");
    input=frame_input(3,3);input.nodes[2].sampled_pose=std::array<uint32_t,13>{};(*input.nodes[2].sampled_pose)[0]=0x02000000;(*input.nodes[2].sampled_pose)[4]=0x3f800000;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::RequiresPoseConversion && preserved(),"unsupported late node Euler rolls back preceding transforms");
    input=frame_input(3,3);input.nodes[2].sampled_pose=input.root_pose;(*input.nodes[2].sampled_pose)[4]=0x7fc00000;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedNumerics && preserved(),"late nonfinite node pose rolls back matrices and skin output");
    input=frame_input(3,3);input.nodes[2].post_transform=skin_palette(7,1)[0];(*input.nodes[2].post_transform)[0]=std::numeric_limits<float>::denorm_min();
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedNumerics && preserved(),"subnormal supplied node transform preserves frame");
    input=frame_input(3,3);input.nodes[2].sampled_pose=explicit_pose({2,0,0,0,0,2,0,0,0,0,2,0});
    input.nodes[2].post_transform=awl::WorldMapModelMatrix{std::numeric_limits<float>::max(),0,0,0,0,1,0,0,0,0,1,0};
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedNumerics && preserved(),"late matrix arithmetic overflow preserves complete frame");
    input=frame_input(3,3);const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"frame test selects unsupported rounding");
    const auto rounding_status=awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame);
    expect(std::fesetround(rounding)==0 && rounding_status==S::UnsupportedNumerics && preserved(),"frame preserves output and caller rounding mode at unsupported stop");
    input=frame_input(1,0);input.root_pose={};
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated && !frame.skin_executed && frame.node_matrices.empty() &&
        frame.skin_palette.empty() && frame.vertex_output==before.vertex_output && frame.feature_matrix_writes[0] && !frame.feature_matrix_writes[1],
        "node-off path ignores node observations and updates only root feature while retaining vertices");
    input=frame_input(3,3);
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto old=frame;const auto live=allocation_probe::live;uint64_t old_hash=14695981039346656037ull;hash_frame(old_hash,old);
    size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<32;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame);allocation_probe::enabled=false;
        if(status==S::Evaluated){reached=true;break;}++rejected;uint64_t h=14695981039346656037ull;hash_frame(h,frame);
        expect(status==S::AllocationFailure && h==old_hash && allocation_probe::live==live,"frame allocation failure releases staging and preserves matrices/vertices");}
    expect(reached && rejected==5,"frame sweep reaches node/palette/feature writes/order/vertex staging");std::cout<<"PLAYER_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
    input={};input.nodes.resize(3);input.root_pose=explicit_pose({1,0,0,100,0,1,0,200,0,0,1,300});
    input.nodes[0].sampled_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    input.nodes[1].sampled_pose=explicit_pose({0,-1,0,2,1,0,0,3,0,0,1,4});
    input.nodes[1].post_transform=awl::WorldMapModelMatrix{1,0,0,1,0,1,0,2,0,0,1,3};
    input.nodes[2].sampled_pose=explicit_pose({1,0,0,-1,0,1,0,-2,0,0,1,-3});
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated &&
        frame.node_matrices[1]==awl::WorldMapModelMatrix{0,-1,0,10,1,0,0,24,0,0,1,37} && frame.skin_palette[1]==frame.node_matrices[1] &&
        frame.node_matrices[2]==awl::WorldMapModelMatrix{1,0,0,-1,0,1,0,-2,0,0,1,-3} &&
        frame.feature_matrix_writes[2]==awl::WorldMapModelMatrix{0,-1,0,110,1,0,0,224,0,0,1,337} && frame.feature_write_order==std::vector<uint32_t>{1,2,3,0},
        "post transform precedes type-one parent composition; root affects feature world matrices, not skin palette; other types skip parent");
    const auto vertices=frame.vertex_output;input.root_pose={};
    expect(awl::evaluate_world_map_player_frame(*work,input,work->initial_output(),&frame)==S::Evaluated && frame.vertex_output==vertices,
        "changing root placement alone does not transform skin vertices twice");
    const auto saved=frame.vertex_output;payloads[0]=setup_model(false);prepare();
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedLayout && frame.vertex_output==saved,
        "nonnull skin path rejects missing root feature instead of following original null dereference");
}
void hash_frame_links(uint64_t& digest,const awl::WorldMapPlayerFrame& frame) {
    for(uint32_t value:frame.root_pose_after)hash(digest,value);
    for(const auto& write:frame.attachment_writes) {
        hash(digest,write?1:0);if(!write)continue;
        hash(digest,uint32_t(write->child));hash(digest,uint32_t(write->producer));hash(digest,write->evaluate_child?1:0);
        for(float value:write->matrix){uint32_t raw=0;if(value!=0)std::memcpy(&raw,&value,4);hash(digest,raw);}
    }
}
void attachment_frame_checks() {
    using S=awl::WorldMapPlayerFrameStatus;
    Fixture fixture;auto payloads=files();payloads[1]=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;
    awl::WorldMapPlayerFrame frame;awl::WorldMapPlayerFrameInput input;
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<256;++seed) {
        payloads[0]=setup_model();payloads[2]=execution_skin(seed);
        for(uint32_t i=0;i<3;++i)word(payloads[0],32+i*28+24,((seed+i)%3)<<24|((i==0?5u:2u)<<16));
        assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"attachment frame providers load");
        expect(awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"attachment frame work prepares");if(!work)return;
        input=frame_input(seed,3);input.evaluate_nodes=(seed&8)!=0;input.request_skin=(seed&4)!=0;
        input.links.identity=0x800000;
        auto& inherited=input.links.inherited;inherited.parent=seed%4?0xb30000:0;
        inherited.producer=seed%4?0xb30000:0x123;inherited.flags=seed%4>=2?8u:0u;
        inherited.matrix=skin_palette(seed+2048,1)[0];
        if(seed%4>=2) {
            input.root_pose[0]=(seed%4==2?0x1f000000:0x1e000000)|0x345678;
            const float scales[]{float(seed%7+1)/4,float(seed%5+1)/8,float(seed%3+1)/2};
            for(size_t i=0;i<3;++i)std::memcpy(&input.root_pose[i+1],&scales[i],4);
        }
        for(uint32_t slot=1;slot<4;++slot)input.links.children[slot]={slot<=2?0xb10000u:0xb20000u,uint16_t((seed+slot-1)%3),slot<=2?1u:0u};
        frame.vertex_output.resize(64);for(size_t i=0;i<64;++i)frame.vertex_output[i]=uint8_t(i*7+seed);
        expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated,
            "all inherited branches and node-on/off attachment propagation prepare atomically");
        hash(digest,seed);hash_frame(digest,frame);hash_frame_links(digest,frame);
    }
    std::cout<<"PLAYER_ATTACHMENT_FRAME_EVALUATION 256 digest "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0x95429a4e627445d7ull,"mapped E438/EB80 matrix and flag writes match, including slot aliases and node types");
    const auto before=frame;uint64_t before_hash=14695981039346656037ull;hash_frame(before_hash,before);hash_frame_links(before_hash,before);
    auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_frame(h,frame);hash_frame_links(h,frame);return h==before_hash;};
    input.links.inherited.producer=0;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedLayout && preserved(),"missing inherited acknowledgement preserves complete frame");
    input.links.inherited.producer=1;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedLayout && preserved(),"different inherited producer rejects original assertion state");
    input.links.inherited.producer=input.links.inherited.parent;input.links.inherited.flags.reset();
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedLayout && preserved(),"unknown reached inherited flags reject");
    input.links.inherited.flags=0;input.links.children[3].node=0xffff;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedLayout && preserved(),"late invalid attachment rolls back skinning and earlier slot writes");
    input.links.children[3].node=2;input.links.children[3].child_flags.reset();
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::UnsupportedLayout && preserved(),"unknown child recursion flags roll back all propagation");
    input.links.children[3].child_flags=0;input.links.children[2].child_flags=0;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::InvalidInput && preserved(),"shared child cannot have inconsistent flag snapshots");
    input.links.children[2].child_flags=1;input.links.identity=0;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::InvalidInput && preserved(),"nonnull children require producer identity");
    input={};input.evaluate_nodes=false;input.request_skin=false;
    input.root_pose[0]=0x02000000;input.root_pose[4]=0x3f800000; // Unsupported Euler if actually read.
    input.links.inherited={0x100000001ull,0x100000001ull,0u,{1,0,0,7,0,1,0,8,0,0,1,9}};
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated && frame.root_matrix==input.links.inherited.matrix &&
        frame.root_pose_after==input.root_pose,"copy-inherited path skips unsupported root pose and keeps full native identities");
    input.links.identity=0x100000002ull;input.links.children[1]={0x100000003ull,2,0u};input.nodes.resize(3);
    const auto copied=frame;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::RequiresPoseConversion && frame.root_matrix==copied.root_matrix &&
        frame.root_pose_after==copied.root_pose_after && !frame.attachment_writes[1],"node-off attachment EB80 reaches local root conversion even when main root copies inherited matrix");
    input.root_pose=explicit_pose({1,0,0,5,0,1,0,6,0,0,1,7});
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated &&
        frame.attachment_writes[1]->matrix==awl::WorldMapModelMatrix{1,0,0,12,0,1,0,14,0,0,1,16},
        "node-off attachment multiplies inherited root by EB80 local root, keeping the original extra root contribution");
    input.evaluate_nodes=true;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated &&
        frame.attachment_writes[1]->matrix==input.links.inherited.matrix,"node-on attachment uses evaluated node matrix without EB80 extra local root");
    input.evaluate_nodes=false;
    input.root_pose={};input.links.inherited.flags=8;input.root_pose[0]=0x1e123456;
    for(size_t i=1;i<13;++i)input.root_pose[i]=0x7fc00000; // All suppressed by scale-only mask with no scale bit.
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated && frame.root_pose_after[0]==0x00123456 &&
        frame.root_matrix==input.links.inherited.matrix && frame.attachment_writes[1]->producer==input.links.identity &&
        frame.attachment_writes[1]->child==input.links.children[1].child,"scale-only inherited branch masks flag byte before EB80 while ignoring absent scale and invalid rotation/translation words");
    input.links.inherited.parent=0;input.links.inherited.flags.reset();input.links.inherited.matrix[0]=std::numeric_limits<float>::quiet_NaN();
    input.links.children={};input.nodes.clear();input.root_pose={};
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::Evaluated && frame.root_matrix==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0},
        "unparented branch ignores producer/flags/inherited matrix and null child slot metadata");
    input.links.identity=1;input.links.children[3]={2,0,0u};
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame)==S::InvalidInput,"node-off with attachments still requires node observations");
    // Connect real sampler callback to EB80's child-to-parent traversal, then
    // compare with independently supplied per-node sample observations.
    awl::WorldMapAnimationBank bank;expect(bank.parse(200,animation_frame_fixture(255,0)),"attachment animation bank parses");
    awl::WorldMapPlayerAnimationFrameInput animated;animated.evaluate_nodes=false;animated.node_post_transforms.resize(3);
    animated.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};animated.playback.position_0=1.25f;
    animated.links.identity=0x800000;animated.links.children[0]={0xb10000,2,1u};input={};input.evaluate_nodes=false;input.nodes.resize(3);input.links=animated.links;
    std::vector<const awl::WorldMapAnimationBank*> banks{&bank};
    for(uint32_t i=0;i<3;++i){awl::WorldMapAnimationPose value{},prior{};
        const auto status=awl::sample_world_map_blended_animation_pose(animated.playback,i,{},banks,animated.settings,prior,&value);
        expect(status==awl::WorldMapAnimationPoseStatus::Sampled || status==awl::WorldMapAnimationPoseStatus::NoPose,"observed attachment pose prepares");
        if(status==awl::WorldMapAnimationPoseStatus::Sampled)input.nodes[i].sampled_pose=value;}
    awl::WorldMapPlayerFrame observed;
    expect(awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&observed)==S::Evaluated,"observed EB80 frame prepares");
    auto result=awl::evaluate_world_map_player_animation_frame(*work,animated,{},banks,frame.vertex_output,&frame);
    expect(result.status==S::Evaluated && frame.attachment_writes[0]->matrix==observed.attachment_writes[0]->matrix && !frame.skin_executed,
        "node-off attachment reaches connected sampler and ancestry, without skinning");
    uint64_t animated_hash=14695981039346656037ull;hash_frame(animated_hash,frame);hash_frame_links(animated_hash,frame);
    animated.playback.clip_10=awl::WorldMapAnimationClipReference{999,0};
    result=awl::evaluate_world_map_player_animation_frame(*work,animated,{},banks,frame.vertex_output,&frame);
    uint64_t after_hash=14695981039346656037ull;hash_frame(after_hash,frame);hash_frame_links(after_hash,frame);
    expect(result.status==S::RequiresAnimationSampling && result.failed_node==2u && result.sampling_status==awl::WorldMapAnimationPoseStatus::RequiresBank && animated_hash==after_hash,
        "node-off attachment sampling failure reports requested descendant before ancestors and preserves frame");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    input.evaluate_nodes=true;input.links.inherited={1,1,8u,{1,0,0,4,0,1,0,5,0,0,1,6}};
    const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<32;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::evaluate_world_map_player_frame(*work,input,frame.vertex_output,&frame);allocation_probe::enabled=false;
        if(status==S::Evaluated){reached=true;break;}++rejected;
        uint64_t h=14695981039346656037ull;hash_frame(h,frame);hash_frame_links(h,frame);
        expect(status==S::AllocationFailure && h==animated_hash && allocation_probe::live==live,"attachment frame allocation failure preserves prior root flags/child writes/matrices/vertices and frees staging");}
    expect(reached && rejected==5,"inherited root and four child slots add no allocations to complete frame staging");
    std::cout<<"PLAYER_ATTACHMENT_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}
void hash_hierarchy(uint64_t& digest,const awl::WorldMapModelHierarchyFrame& result) {
    hash(digest,uint32_t(result.models.size()));
    for(const auto& model:result.models) {
        hash(digest,uint32_t(model.identity));for(uint32_t word_value:model.root_pose)hash(digest,word_value);
        hash(digest,uint32_t(model.inherited.parent));hash(digest,uint32_t(model.inherited.producer));
        hash(digest,model.inherited.flags?1:0);if(model.inherited.flags)hash(digest,*model.inherited.flags);
        for(float v:model.inherited.matrix){uint32_t raw=0;if(v!=0)std::memcpy(&raw,&v,4);hash(digest,raw);}
        hash_frame(digest,model.frame);hash_frame_links(digest,model.frame);
    }
    hash(digest,uint32_t(result.evaluation_order.size()));for(uint64_t key:result.evaluation_order)hash(digest,uint32_t(key));
}
void hierarchy_frame_checks() {
    using S=awl::WorldMapModelHierarchyFrameStatus;using F=awl::WorldMapPlayerFrameStatus;
    Fixture fixture;auto payloads=files();payloads[1]=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;
    std::array<awl::WorldMapModelCore,4> cores;std::array<awl::WorldMapAnimationBank,4> clips;
    std::vector<const awl::WorldMapAnimationBank*> banks;for(const auto& clip:clips)banks.push_back(&clip);
    auto key=[](uint32_t j){return uint64_t(0x800000+j*0x1000);};
    std::vector<awl::WorldMapModelFrameSource> sources(4);awl::WorldMapModelHierarchyFrame frame;
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<256;++seed) {
        payloads[0]=setup_model();payloads[2]=execution_skin(seed);
        for(uint32_t i=0;i<3;++i)word(payloads[0],32+i*28+24,((seed+i)%3)<<24|((i==0?5u:2u)<<16));
        assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"hierarchy primary providers load");
        expect(awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"hierarchy primary prepares");if(!work)return;
        for(uint32_t j=0;j<4;++j) {
            sources[j]={};auto& source=sources[j];auto& input=source.input;
            cores[j]=work->drawing().setup().core_before_features();cores[j].auxiliary_c=0;cores[j].feature_14=0x123456;
            for(uint32_t i=0;i<3;++i){cores[j].nodes[i].type_0=uint8_t((seed+j+i)%3);
                cores[j].nodes[i].feature_8=(seed+j+i)%3?0x900000+(j*4+i)*0x100:0;}
            // Null +C means every inverse bind is unread, even with skin requested.
            for(auto& matrix:cores[j].inverse_initial_matrices)matrix.fill(std::numeric_limits<float>::quiet_NaN());
            if(j==0)source.primary=work.get();
            else {source.resource=&work->drawing().setup().selection().model;source.core=&cores[j];}
            input.links.identity=key(j);input.links.inherited.parent=j==3?key(1):j?key(0):0;
            input.links.inherited.flags=j==2?0u:((seed+j)&2)?9u:1u;
            input.links.inherited.matrix={1,0,0,0,0,1,0,0,0,0,1,0};
            input.root_pose=explicit_pose(skin_palette(seed+j*4096+512,1)[0]);
            if(j){input.root_pose[0]=0x1f345678;
                const float scales[]{float(seed%7+1)/4,float(seed%5+1)/8,float(seed%3+1)/2};
                for(size_t i=0;i<3;++i)std::memcpy(&input.root_pose[i+1],&scales[i],4);}
            input.evaluate_nodes=(seed&8)!=0;input.request_skin=(seed&4)!=0;input.node_post_transforms.resize(3);
            const auto posts=skin_palette(seed+j*4096+1024,3);
            for(uint32_t i=0;i<3;++i)if((seed+j+i)%4==0)input.node_post_transforms[i]=posts[i];
            expect(clips[j].parse(200+j,animation_frame_fixture(seed,j)),"hierarchy invented animation bank parses");
            if(seed%8)input.playback.clip_10=awl::WorldMapAnimationClipReference{200+j,0};
            input.playback.position_0=1.25f+float(j);
        }
        // Null input flags are resolved from reached child snapshots.
        for(uint32_t slot=0;slot<3;++slot)sources[0].input.links.children[slot]={key(slot<2?1:2),uint16_t((seed+slot)%3),std::nullopt};
        sources[1].input.links.children[3]={key(3),uint16_t(seed%3),std::nullopt};
        // Child 2 is propagated but never evaluated; its source is deliberately unsupported.
        sources[2].core=nullptr;sources[2].resource=nullptr;
        sources[0].previous_frame.vertex_output.resize(64);
        for(size_t i=0;i<64;++i)sources[0].previous_frame.vertex_output[i]=uint8_t(i*7+seed);
        const auto result=awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,32,&frame);
        expect(result.status==S::Evaluated && frame.evaluation_order==std::vector<uint64_t>{key(0),key(1),key(3),key(1),key(3)},
            "complete sampler/primary/no-skin hierarchy keeps ordered alias visits and skips disabled child payload");
        hash(digest,seed);hash_hierarchy(digest,frame);
    }
    std::cout<<"PLAYER_RECURSIVE_ANIMATION_HIERARCHIES 256 visits 1280 digest "<<std::hex<<digest<<std::dec<<'\n';
    // Filled from complete verified E438 recursion, without child/sampler/pose hooks.
    expect(digest==0x97f664057afaa176ull,"complete original hierarchy state/frames/flags/skin/visit order match");
    uint64_t prior_hash=14695981039346656037ull;hash_hierarchy(prior_hash,frame);
    auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_hierarchy(h,frame);return h==prior_hash;};
    auto check=[&](S wanted,uint64_t model,const char* why,size_t bound=32) {
        const auto result=awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,bound,&frame);
        expect(result.status==wanted && result.required_model==model && preserved(),why);return result;
    };
    check(S::EvaluationLimit,key(1),"visit bound rolls back prior root/child/grandchild work",3);
    sources[3].input.links.children[0]={key(0),0,std::nullopt};
    check(S::Cycle,key(0),"active-path cycle rolls back propagated root state instead of recursive stack overflow");
    sources[3].input.links.children={};sources[1].input.links.children[3].child=123;
    check(S::RequiresModel,123,"missing reached grandchild rolls back primary skin and all earlier propagation");
    sources[1].input.links.children[3].child=key(3);sources[3].input.links.inherited.parent=key(2);
    auto result=check(S::FrameFailure,key(3),"inconsistent grandchild parent acknowledgement blocks atomically");
    expect(result.frame_failure && result.frame_failure->status==F::UnsupportedLayout,"hierarchy carries inherited frame failure");
    sources[3].input.links.inherited.parent=key(1);sources[0].input.links.children[2].child_flags=1;
    check(S::InvalidInput,key(2),"child snapshot flags cannot contradict its parent slot observation");
    sources[0].input.links.children[2].child_flags.reset();sources[1].input.playback.clip_10=awl::WorldMapAnimationClipReference{999,0};
    result=check(S::FrameFailure,key(1),"late missing animation bank preserves complete previous hierarchy");
    expect(result.frame_failure && result.frame_failure->failed_node==0u && result.frame_failure->sampling_status==awl::WorldMapAnimationPoseStatus::RequiresBank,
        "hierarchy reports failing child and exact node/sampler reason");
    sources[1].input.playback.clip_10=awl::WorldMapAnimationClipReference{201,0};cores[1].auxiliary_c=1;
    result=check(S::FrameFailure,key(1),"no-skin secondary frame explicitly rejects a nonnull auxiliary layout");
    expect(result.frame_failure && result.frame_failure->status==F::UnsupportedLayout,"unsupported auxiliary is a frame layout failure");cores[1].auxiliary_c=0;
    const auto old_key=sources[2].input.links.identity;sources[2].input.links.identity=key(1);
    check(S::InvalidInput,0,"duplicate stable model keys preserve hierarchy");sources[2].input.links.identity=old_key;
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,123,{},banks,32,&frame).status==S::RequiresModel && preserved(),"missing root blocks without replacing previous result");
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,0,&frame).status==S::InvalidInput && preserved(),"zero visit bound is invalid");
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,32,nullptr).status==S::InvalidInput,"null hierarchy output is invalid");
    // Explicitly test a disabled recursive cycle: placement still updates, but
    // bit 0x01 off must not traverse or inspect that source's bad payload.
    sources[3].input.links.children[0]={key(2),0,std::nullopt};
    sources[2].input.links.children[0]={key(0),0,std::nullopt};
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,32,&frame).status==S::Evaluated && frame.models[2].inherited.producer==key(3),
        "nonrecursive child links retain ordered propagation even when their unused graph has a cycle");
    sources[3].input.links.children={};sources[2].input.links.children={};
    // Controls come from root arguments, not the child's stored preferences.
    sources[0].input.evaluate_nodes=false;sources[1].input.evaluate_nodes=true;sources[3].input.evaluate_nodes=true;
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,32,&frame).status==S::Evaluated &&
        frame.models[1].frame.node_matrices.empty() && frame.models[3].frame.node_matrices.empty() && !frame.models[0].frame.skin_executed,
        "node-off control propagates to every child, while reached ancestry still samples");
    // Previous vertices can be supplied from the result being replaced.
    for(size_t i=0;i<sources.size();++i){sources[i].previous_frame=frame.models[i].frame;
        sources[i].input.root_pose=frame.models[i].root_pose;sources[i].input.links.inherited=frame.models[i].inherited;}
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,32,&frame).status==S::Evaluated,
        "complete prior frame/state snapshots support a successive atomic hierarchy update");
    std::vector<awl::WorldMapModelFrameSource> deep(128);
    for(size_t i=0;i<deep.size();++i){auto& source=deep[i];source.resource=sources[1].resource;source.core=sources[1].core;
        source.input.links.identity=i+1;source.input.links.inherited.parent=i;
        source.input.links.inherited.flags=9;source.input.node_post_transforms.resize(3);
        if(i+1<deep.size())source.input.links.children[3]={i+2,0,std::nullopt};}
    awl::WorldMapModelHierarchyFrame deep_frame;
    expect(awl::evaluate_world_map_model_hierarchy_frame(deep,1,{}, {},128,&deep_frame).status==S::Evaluated &&
        deep_frame.evaluation_order.size()==128 && deep_frame.evaluation_order.front()==1 && deep_frame.evaluation_order.back()==128,
        "128-model chain uses iterative traversal and keeps ordered inherited state through stack growth");
    uint64_t deep_hash=14695981039346656037ull;hash_hierarchy(deep_hash,deep_frame);
    const auto deep_result=awl::evaluate_world_map_model_hierarchy_frame(deep,1,{}, {},127,&deep_frame);
    uint64_t deep_after_hash=14695981039346656037ull;hash_hierarchy(deep_after_hash,deep_frame);
    expect(deep_result.status==S::EvaluationLimit && deep_result.required_model==128 && deep_hash==deep_after_hash,
        "deep visit bound preserves all prior frames instead of publishing a partial chain");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    sources[0].input.evaluate_nodes=true;prior_hash=14695981039346656037ull;hash_hierarchy(prior_hash,frame);
    const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto step=awl::evaluate_world_map_model_hierarchy_frame(sources,key(0),{},banks,32,&frame);allocation_probe::enabled=false;
        if(step.status==S::Evaluated){reached=true;break;}++rejected;
        expect(step.status==S::AllocationFailure && preserved() && allocation_probe::live==live,"every hierarchy allocation failure frees staging and preserves root flags, all child frames and propagated state");}
    expect(reached && rejected>20,"allocation sweep reaches complete recursive hierarchy publication");
    std::cout<<"PLAYER_HIERARCHY_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}
std::vector<uint8_t> owner_frame_clip(float add=0) {
    std::vector<uint8_t> bytes(100);word(bytes,4,0x30000);
    for(uint32_t node=0;node<3;++node){const size_t row=8+node*16;word(bytes,row+4,64+node*12);
        bytes[row+11]=uint8_t(node);bytes[row+12]=0x40;bytes[row+15]=1;
        for(uint32_t i=0;i<3;++i){const float value=float(node*3+i+1)+add;uint32_t raw;std::memcpy(&raw,&value,4);word(bytes,64+node*12+i*4,raw);}}
    return bytes;
}
std::unique_ptr<awl::WorldMapNativeModel> frame_owner(bool attachment=false) {
    auto bytes=setup_model();if(attachment){bytes.resize(324);word(bytes,24,1);word(bytes,28,320);word(bytes,320,0x00010000);}
    awl::WorldMapModelBank bank;expect(bank.parse(300,archive({bytes})),"owned frame model bank parses");
    awl::WorldMapSecondarySetupStep setup;
    expect(awl::prepare_world_map_secondary_model_setup(1,300,&bank,0,std::nullopt,{},uint64_t(0),&setup)==awl::WorldMapSecondarySetupStatus::RequiresConstruction,
        "owned frame setup reaches checked construction");
    std::unique_ptr<awl::WorldMapNativeModel> owner;
    expect(awl::construct_world_map_secondary_model(bank,1,setup,&owner).status==awl::WorldMapModelConstructionStatus::Constructed,"owned frame model constructs");
    // Source snapshots disappear before the owner is ever evaluated.
    bank.clear();if(owner)expect(owner->prepared_resource().records.size()==3 && owner->prepared_resource().records[0].matrix==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0},
        "private re-prepared default matrices outlive the source bank and setup");return owner;
}
void partial_frame_sampling_checks() {
    using S=awl::WorldMapAnimationPoseStatus;awl::WorldMapAnimationBank first,second;
    std::vector<const awl::WorldMapAnimationBank*> banks{&first,&second};size_t compared=0;
    for(uint32_t seed=0;seed<256;++seed){expect(first.parse(200,animation_frame_fixture(seed,0)) && second.parse(201,animation_frame_fixture(seed,1)),"partial comparison banks parse");
        for(uint32_t node=0;node<3;++node)for(uint64_t link:{0ull,2ull}){
            awl::WorldMapAnimationPlayback complete;complete.clip_10=awl::WorldMapAnimationClipReference{200,0};complete.position_0=1.25f;
            complete.link_14=link;complete.value_18=0.375f;auto next=complete;next.clip_10=awl::WorldMapAnimationClipReference{201,0};next.link_14=0;
            const std::vector<awl::WorldMapAnimationPlaybackRecord> records{{2,next}};
            auto partial=awl::partial_world_map_animation_playback(complete),partial_next=awl::partial_world_map_animation_playback(next);
            for(auto* value:{&partial,&partial_next}){value->word_8.reset();value->limit_c.reset();value->rate_4=std::numeric_limits<float>::quiet_NaN();}
            const std::vector<awl::WorldMapAnimationPartialPlaybackRecord> partial_records{{2,partial_next}};
            awl::WorldMapAnimationPose prior{},expected{},actual{};prior.fill(0x12345678);expected=actual=prior;
            const auto wanted=awl::sample_world_map_blended_animation_pose(complete,node,records,banks,{},prior,&expected);
            const auto result=awl::sample_world_map_partial_blended_animation_pose(partial,node,partial_records,banks,{},prior,&actual);
            expect(result==wanted && actual==expected && !partial.complete() && !partial_next.complete(),"partial sampling matches verified complete path without establishing unused owner fields");++compared;
        }}
    std::cout<<"PARTIAL_FRAME_SAMPLING_PARITY "<<compared<<'\n';expect(compared==1536,"all complete/partial known-weight comparisons run");
    expect(first.parse(200,owner_frame_clip()) && second.parse(201,owner_frame_clip(4)),"known partial weight fixtures parse");
    awl::WorldMapAnimationPartialPlayback root,next;root.clip_10=awl::WorldMapAnimationClipReference{200,0};root.link_14=2;
    next.clip_10=awl::WorldMapAnimationClipReference{201,0};std::vector<awl::WorldMapAnimationPartialPlaybackRecord> records{{2,next}};
    awl::WorldMapAnimationPose prior{},output{};prior.fill(0x12345678);output=prior;
    expect(awl::sample_world_map_partial_blended_animation_pose(root,0,records,banks,{},prior,&output)==S::RequiresBlendWeight && output==prior,
        "reached unknown blend weight preserves the entire pose");
    records[0].state.clip_10.reset();
    expect(awl::sample_world_map_partial_blended_animation_pose(root,0,records,banks,{},prior,&output)==S::Sampled && output[8]==0x3f800000 && !root.value_18,
        "linked null clip stops before reading unknown root weight");
    records[0].state=next;output=prior;
    expect(awl::sample_world_map_partial_blended_animation_pose(root,99,records,banks,{},prior,&output)==S::NoPose && output==prior,"missing root node skips unknown blend fields");
    root.value_18=0.25f;
    expect(awl::sample_world_map_partial_blended_animation_pose(root,0,records,banks,{},prior,&output)==S::Sampled && output[8]==0x40000000,
        "known quarter-weight blends independently obvious translation 1 to 5 into 2");
    auto empty=owner_frame_clip();for(size_t row:{size_t(8),size_t(24),size_t(40)})empty[row+15]=0;
    expect(first.parse(200,empty) && second.parse(201,empty),"no-component partial clips parse");root.value_18.reset();
    expect(awl::sample_world_map_partial_blended_animation_pose(root,0,records,banks,{},prior,&output)==S::Sampled && output[0]==0x00345678 && !root.value_18,
        "blend without flagged components preserves unknown weight and unrelated pose words");
}
void owned_hierarchy_frame_checks() {
    using S=awl::WorldMapModelHierarchyFrameStatus;
    auto primary=frame_owner(),secondary=frame_owner(true),auxiliary=frame_owner();if(!primary || !secondary || !auxiliary)return;
    const auto p=primary->binding().model_identity,s=secondary->binding().model_identity,a=auxiliary->binding().model_identity;
    awl::WorldMapModelAttachmentStep attached;
    // DF68's feature and auxiliary branches are separate requests. The second
    // request retains the feature installed by the first.
    expect(awl::apply_world_map_native_model_attachments({primary.get(),secondary.get(),auxiliary.get()},{p,s,99,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced,
        "owned feature attachment establishes node-zero feature identity");
    expect(awl::apply_world_map_native_model_attachments({primary.get(),secondary.get(),auxiliary.get()},{p,s,0,a},&attached)==awl::WorldMapModelAttachmentStatus::Advanced &&
        primary->core().children_15c[0]==s && secondary->core().children_15c[0]==a && secondary->core().flags_158==15u && auxiliary->core().flags_158==12u,
        "owned attachment transaction establishes authoritative primary/secondary/disabled auxiliary graph");
    awl::WorldMapAnimationBank bank;expect(bank.parse(200,owner_frame_clip()),"owned frame animation source parses");
    std::vector<awl::WorldMapAnimationPartialPlaybackRecord> records;
    for(auto* owner:{primary.get(),secondary.get()}){const auto binding=owner->binding();awl::WorldMapAnimationChannelState channel;
        channel.target_8=channel.previous_c=channel.older_10=binding.playback_178;
        awl::WorldMapAnimationPartialChannelStep step;
        expect(awl::advance_world_map_native_animation_channel(owner,&channel,&records,{binding.model_identity,200,0,0,0,0},&bank,&step)==awl::WorldMapAnimationChannelStatus::Advanced &&
            !owner->playback() && owner->partial_playback().clip_10 && !owner->partial_playback().value_18,
            "first channel selection retains an active bank and leaves blend weight unknown");}
    bank.clear();expect(primary->animation_bank(200) && secondary->animation_bank(200) && primary->animation_banks().size()==1,
        "both independent retained animation snapshots survive source clear");
    std::vector<awl::WorldMapModelFrameSource> sources(3);
    sources[0].secondary=primary.get();sources[1].secondary=secondary.get();sources[2].secondary=auxiliary.get();
    for(auto& source:sources){source.input.node_post_transforms.resize(3);source.input.links.identity=123;
        source.input.links.inherited.parent=999;source.input.links.inherited.flags=0;
        source.input.links.children[3]={998,0,std::nullopt};source.input.playback.clip_10=awl::WorldMapAnimationClipReference{997,0};}
    sources[0].input.root_pose=explicit_pose({1,0,0,10,0,1,0,0,0,0,1,0});
    sources[1].input.root_pose=explicit_pose({1,0,0,500,0,1,0,600,0,0,1,700});
    sources[2].input.root_pose[0]=0x02000000;sources[2].input.root_pose[4]=0x3f800000;
    const auto primary_links=primary->model_links(),secondary_links=secondary->model_links(),auxiliary_links=auxiliary->model_links();
    const auto old_primary=primary->partial_playback(),old_secondary=secondary->partial_playback();
    awl::WorldMapModelHierarchyFrame frame;
    auto result=awl::evaluate_world_map_model_hierarchy_frame(sources,p,{}, {},32,&frame);
    expect(result.status==S::Evaluated && frame.evaluation_order==std::vector<uint64_t>{p,s} && frame.models[0].identity==p && frame.models[1].identity==s && frame.models[2].identity==a,
        "owned hierarchy derives unique keys/links/playback/resources/banks and ignores contradictory caller copies");
    expect(frame.models[1].frame.root_matrix==awl::WorldMapModelMatrix{1,0,0,14,0,1,0,5,0,0,1,6} && frame.models[1].root_pose[0]==0 &&
        frame.models[1].frame.feature_matrix_writes[0]==awl::WorldMapModelMatrix{1,0,0,15,0,1,0,7,0,0,1,9} &&
        frame.models[2].inherited.matrix==awl::WorldMapModelMatrix{1,0,0,15,0,1,0,7,0,0,1,9} && frame.models[2].inherited.producer==s && frame.models[2].frame.node_matrices.empty(),
        "owned sampled translation and node attachment placement produce independently known matrices, suppress child local placement and preserve disabled payload");
    expect(primary->core().children_15c==primary_links.children_15c && secondary->core().parent_150==secondary_links.parent_150 && auxiliary->core().flags_158==auxiliary_links.flags_158 &&
        primary->partial_playback().position_0==old_primary.position_0 && secondary->partial_playback().position_0==old_secondary.position_0 && !primary->playback() && !secondary->playback(),
        "evaluating owned frames leaves graph/clocks/unknown playback fields and native owners unchanged");
    uint64_t prior_hash=14695981039346656037ull;hash_hierarchy(prior_hash,frame);
    auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_hierarchy(h,frame);return h==prior_hash;};
    const std::vector<awl::WorldMapAnimationPlaybackRecord> spoofed{{secondary->binding().playback_178,{}}};
    result=awl::evaluate_world_map_model_hierarchy_frame(sources,p,spoofed, {},32,&frame);
    expect(result.status==S::InvalidInput && result.required_model==s && preserved(),"external record cannot duplicate an authoritative owner playback key");
    auto extra=bank;expect(extra.parse(200,owner_frame_clip(4)),"conflicting external bank parses");
    result=awl::evaluate_world_map_model_hierarchy_frame(sources,p,{}, {&extra},32,&frame);
    expect(result.status==S::FrameFailure && result.required_model==p && result.frame_failure && result.frame_failure->sampling_status==awl::WorldMapAnimationPoseStatus::InvalidInput && preserved(),
        "different bytes under a reached retained identity cannot substitute animation or publish prefix work");
    expect(extra.parse(200,owner_frame_clip()),"identical external animation parses");
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,p,{}, {&extra},32,&frame).status==S::Evaluated,"identical owned/external banks deduplicate without ambiguity");
    auto duplicate=sources;duplicate.push_back(sources[1]);
    result=awl::evaluate_world_map_model_hierarchy_frame(duplicate,p,{}, {},32,&frame);
    expect(result.status==S::InvalidInput && preserved(),"duplicate native owner rejects even when caller identity copies differ");
    auto missing=sources;missing.pop_back();
    result=awl::evaluate_world_map_model_hierarchy_frame(missing,p,{}, {},32,&frame);
    expect(result.status==S::RequiresModel && result.required_model==a && preserved(),"missing disabled owned child still blocks reached inherited placement");
    auto conflicting=sources;conflicting[1].core=&secondary->core();
    result=awl::evaluate_world_map_model_hierarchy_frame(conflicting,p,{}, {},32,&frame);
    expect(result.status==S::InvalidInput && result.required_model==s && preserved(),"owned source cannot also supply substitute prepared core views");
    sources[0].input.evaluate_nodes=false;
    expect(awl::evaluate_world_map_model_hierarchy_frame(sources,p,{}, {},32,&frame).status==S::Evaluated && frame.models[0].frame.node_matrices.empty() &&
        frame.models[1].frame.node_matrices.empty() && frame.models[1].frame.attachment_writes[0] && !primary->playback(),"owned node-off attachments sample retained partial playback through EB80 without claiming complete metadata");
    auto fresh=frame_owner();if(!fresh)return;std::vector<awl::WorldMapModelFrameSource> fresh_sources(1);fresh_sources[0].secondary=fresh.get();fresh_sources[0].input.node_post_transforms.resize(3);
    expect(awl::evaluate_world_map_model_hierarchy_frame(fresh_sources,fresh->binding().model_identity,{}, {},1,&frame).status==S::Evaluated &&
        frame.models[0].frame.node_matrices[0]==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0} && !fresh->playback(),
        "fresh owned null clip uses retained defaults while constructor fields remain unknown");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    sources[0].input.evaluate_nodes=true;prior_hash=14695981039346656037ull;hash_hierarchy(prior_hash,frame);
    const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto step=awl::evaluate_world_map_model_hierarchy_frame(sources,p,{}, {},32,&frame);allocation_probe::enabled=false;
        if(step.status==S::Evaluated){reached=true;break;}++rejected;
        expect(step.status==S::AllocationFailure && preserved() && allocation_probe::live==live && !primary->playback(),"owned frame allocation failures preserve every output/owner/unknown field and free staging");}
    expect(reached && rejected>10,"owned frame allocation sweep reaches complete hierarchy output");std::cout<<"PLAYER_OWNED_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
    // Exercise the mixed primary-skin/owned-secondary route with a supplied
    // primary identity matching the child's actual parent acknowledgement.
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=draw_gpl();payloads[2]=execution_skin(0);
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    fixture.write("boy_0.arc",archive(payloads));std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"mixed owned hierarchy primary skin provider prepares");
    if(work){auto mixed=sources;mixed[0].secondary=nullptr;mixed[0].primary=work.get();mixed[0].input.links.identity=p;
        mixed[0].input.links.inherited.parent=0;mixed[0].input.links.children={};
        mixed[0].input.links.children[0]={s,1,std::nullopt};mixed[0].input.evaluate_nodes=true;mixed[0].input.request_skin=true;
        mixed[0].input.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};mixed[0].previous_frame.vertex_output=work->initial_output();
        awl::WorldMapModelHierarchyFrame connected;
        const auto mixed_result=awl::evaluate_world_map_model_hierarchy_frame(mixed,p,{}, {},32,&connected);
        expect(mixed_result.status==S::Evaluated &&
            connected.evaluation_order==std::vector<uint64_t>{p,s} && connected.models[0].frame.skin_executed &&
            connected.models[0].frame.skin_palette.size()==3 && !connected.models[1].frame.skin_executed &&
            connected.models[1].frame.root_matrix==awl::WorldMapModelMatrix{1,0,0,14,0,1,0,5,0,0,1,6} && !secondary->playback(),
            "supplied primary skinning and authoritative owned secondary share retained banks and ordered attachment placement");}
    std::cout<<"PLAYER_OWNED_HIERARCHY graph/channel/bank/default/frame boundaries checked\n";
}
void hash_topology(uint64_t& h,const awl::WorldMapPlayerTopology& topology) {
    hash(h,uint32_t(topology.batches.size()));awl::WorldMapPlayerModelAssetView gpl;
    expect(topology.assets && topology.assets->resource(2,&gpl),"topology retains GPL provider");
    for(const auto& b:topology.batches){for(uint32_t v:{b.section,b.command,b.display_list.offset-gpl.reference.offset,b.display_list_size,b.nop_bytes,b.descriptor_word,
        uint32_t(b.texture.binding.bank),uint32_t(b.texture.binding.index),uint32_t(b.texture.unit),uint32_t(b.references.size())})hash(h,v);
        for(const auto& r:b.references)for(uint32_t v:{uint32_t(r.position),uint32_t(r.normal),uint32_t(r.uv)})hash(h,v);
        hash(h,uint32_t(b.primitives.size()));for(const auto& q:b.primitives)for(uint32_t v:{uint32_t(q.opcode),q.first_reference,q.count})hash(h,v);
        hash(h,uint32_t(b.triangle_indices.size()));for(auto v:b.triangle_indices)hash(h,v);}
}
std::vector<uint8_t> topology_gpl(uint8_t op=0x90,uint32_t count=3,uint32_t mask=7) {
    auto b=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){
        word(b,at+120,320);const auto vcd=((mask&1)?3u:2u)<<2|((mask&2)?3u:2u)<<4|((mask&4)?3u:2u)<<10;
        word(b,at+132+2*16+4,vcd);
        for(uint32_t j=2;j<5;++j){const size_t start=at+(j==2?320:384+(j-3)*32);const uint32_t n=j==2?count:3u;
            const uint8_t code=j==2?op:0x90;b[start]=code;b[start+1]=uint8_t(n>>8);b[start+2]=uint8_t(n);size_t cursor=start+3;
            for(uint32_t i=0;i<n;++i){const uint16_t indices[]{uint16_t(i%3),uint16_t((i+1)%3),uint16_t(i%2)};
                for(size_t a=0;a<3;++a){if(mask&(1u<<a))b[cursor++]=uint8_t(indices[a]>>8);b[cursor++]=uint8_t(indices[a]);}}
            word(b,at+132+j*16+8,uint32_t(start-at));word(b,at+132+j*16+12,uint32_t((cursor-start+15)&~size_t(15)));
        }}return b;
}
void topology_checks() {
    using T=awl::WorldMapPlayerTopologyStatus;Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[2]=skin();
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerDrawCommands> drawing;awl::WorldMapPlayerTopology topology;
    auto prepare=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
            awl::prepare_world_map_player_draw_commands(assets,&drawing)==awl::WorldMapPlayerDrawStatus::PreparedParameters,"synthetic topology drawing profile prepares");};
    size_t checked=0;const uint8_t ops[]{0x80,0x88,0x90,0x98,0xa0};
    for(uint32_t mask=0;mask<8;++mask)for(uint8_t op:ops){const uint32_t count=op==0x80 || op==0x88?4u:op==0x90?6u:5u;
        payloads[1]=topology_gpl(op,count,mask);prepare();if(!drawing)return;
        expect(awl::prepare_world_map_player_topology(*drawing,&topology).status==T::PreparedTopology && topology.batches.size()==6,"complete reversed-section topology and ordered material ranges prepare");
        if(topology.batches.size()!=6)return;
        const auto& batch=topology.batches[0];const std::vector<uint32_t> expected=op==0x80 || op==0x88?std::vector<uint32_t>{0,1,2,0,2,3}:
            op==0x90?std::vector<uint32_t>{0,1,2,3,4,5}:op==0x98?std::vector<uint32_t>{0,1,2,2,1,3,2,3,4}:std::vector<uint32_t>{0,1,2,0,2,3,0,3,4};
        expect(batch.references.size()==count && batch.triangle_indices==expected && batch.primitives.size()==1 && batch.nop_bytes==batch.display_list_size-3-count*(3+uint32_t((mask&1)!=0)+uint32_t((mask&2)!=0)+uint32_t((mask&4)!=0)),"index-width combinations, NOP padding and independently listed GX triangle winding match");
        for(uint32_t i=0;i<count;++i)expect(batch.references[i].position==i%3 && batch.references[i].normal==(i+1)%3 && batch.references[i].uv==i%2,"position/normal/UV index order matches independent invented references");
        expect(topology.batches[0].command==2 && topology.batches[1].command==3 && topology.batches[2].command==4 &&
            topology.batches[0].texture.binding.bank==Bank::Body && topology.batches[0].texture.binding.index==1 &&
            topology.batches[1].texture.binding.bank==Bank::Body && topology.batches[1].texture.binding.index==1 && topology.batches[2].texture.binding.bank==Bank::Eyes,
            "type-one attached lists use old texture before installing replacements");++checked;}
    std::cout<<"PLAYER_TOPOLOGY_PROFILES "<<checked<<" widths/primitives/order checked\n";
    auto previous=topology;uint64_t before=14695981039346656037ull;hash_topology(before,previous);
    auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_topology(h,topology);return h==before && topology.assets==previous.assets;};
    for(uint32_t kind=0;kind<11;++kind){payloads[1]=topology_gpl();const size_t at=64;
        if(kind==0)payloads[1][at+320]=0x91;
        if(kind==1)payloads[1][at+320]=0xb8;
        if(kind==2)word(payloads[1],at+132+2*16+12,2);
        if(kind==3)word(payloads[1],at+132+2*16+12,20);
        if(kind==4)payloads[1][at+324]=3; // First position equals count.
        if(kind==5)payloads[1][at+326]=3; // First normal equals count.
        if(kind==6)payloads[1][at+328]=2; // First UV equals count.
        if(kind==7)word(payloads[1],at+120,384);
        if(kind==8)word(payloads[1],at+132+2*16+4,0xc34); // Direct position.
        if(kind==9)payloads[1][at+322]=2; // Triangle group cannot have two refs.
        if(kind==10)word(payloads[1],at+132+4*16+8,120); // Metadata alias late in traversal.
        prepare();const auto result=awl::prepare_world_map_player_topology(*drawing,&topology);
        expect(result.status!=T::PreparedTopology && result.section==1u && preserved(),"malformed/unsupported late section preserves preceding batches, texture state and retained output");}
    for(uint32_t missing=0;missing<3;++missing){payloads[1]=topology_gpl();
        // Earlier commands carry valid standalone draw ranges, before one
        // prerequisite has been established by the serialized stream.
        const uint32_t command=missing==0?1u:missing==1?0u:2u;
        for(size_t at:{size_t(64),size_t(512)}){
            if(missing==2){payloads[1][at+132+16]=1;payloads[1][at+132+17]=255;word(payloads[1],at+132+16+4,0);}
            else {word(payloads[1],at+132+command*16+8,320);word(payloads[1],at+132+command*16+12,32);}}
        prepare();const auto result=awl::prepare_world_map_player_topology(*drawing,&topology);
        const auto wanted=missing==0?T::RequiresDescriptor:missing==1?T::RequiresDescriptor:T::RequiresTev;
        expect(result.status==wanted && preserved(),"ordered command prerequisites fail explicitly and preserve the complete prior topology");}
    // Install a VCD first, with its own range, before any texture is selected.
    payloads[1]=topology_gpl();for(size_t at:{size_t(64),size_t(512)}){
        payloads[1][at+132]=2;word(payloads[1],at+132+4,0xc3c);word(payloads[1],at+132+8,320);word(payloads[1],at+132+12,32);}
    prepare();expect(awl::prepare_world_map_player_topology(*drawing,&topology).status==T::RequiresTexture && preserved(),"first range cannot invent prior texture state");
    payloads[1]=topology_gpl();prepare();
    expect(awl::prepare_world_map_player_topology(*drawing,nullptr).status==T::InvalidInput,"null topology output rejects");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t failed=0;bool reached=false;
    for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto result=awl::prepare_world_map_player_topology(*drawing,&topology);allocation_probe::enabled=false;
        if(result.status==T::PreparedTopology){reached=true;break;}++failed;
        expect(result.status==T::AllocationFailure && preserved() && allocation_probe::live==live,"topology allocation failures preserve prior references/primitives/triangles/providers and free staging");}
    expect(reached && failed>10,"topology allocation sweep reaches all six complete ranges");std::cout<<"PLAYER_TOPOLOGY_ALLOCATION_FAILURES "<<failed<<'\n';
#endif
    auto lifetime=std::weak_ptr<const Assets>(topology.assets);assets.reset();drawing.reset();
    expect(!lifetime.expired() && topology.assets->texture(Bank::Body,1),"prepared topology keeps immutable texture/geometry providers live independently of drawing owner");
    previous={};topology={}; // Release the failure baseline as well as the published topology.
    expect(lifetime.expired(),"last new topology provider reference releases after replacement and destruction");
}
void hash_geometry(uint64_t& h,const awl::WorldMapPlayerGeometry& geometry) {
    hash_topology(h,geometry.topology());hash(h,uint32_t(geometry.vertices().size()));
    for(const auto& batch:geometry.vertices()){hash(h,uint32_t(batch.size()));for(const auto& vertex:batch)
        for(const float value:{vertex.position[0],vertex.position[1],vertex.position[2],vertex.normal[0],vertex.normal[1],vertex.normal[2],vertex.uv[0],vertex.uv[1]}){
            uint32_t raw;std::memcpy(&raw,&value,4);hash(h,raw);}}
}
std::vector<uint8_t> geometry_gpl() {
    auto b=topology_gpl();
    constexpr int positions[3][3]{{-32768,-1,32767},{8192,-8192,0},{1,16384,-16384}};
    constexpr int normals[3][3]{{16384,-16384,8192},{-32768,32767,-1},{1,0,-8192}};
    constexpr int uv[2][2]{{-32768,32767},{8192,-1}};
    const auto store=[&](size_t at,int value){const auto raw=uint16_t(value);b[at]=uint8_t(raw>>8);b[at+1]=uint8_t(raw);};
    for(size_t at:{size_t(64),size_t(512)}){word(b,at+24,32);word(b,at+288,38);
        for(size_t i=0;i<3;++i)for(size_t lane=0;lane<3;++lane){store(at+32+i*12+lane*2,positions[i][lane]);store(at+38+i*12+lane*2,normals[i][lane]);}
        for(size_t i=0;i<2;++i)for(size_t lane=0;lane<2;++lane)store(at+96+i*4+lane*2,uv[i][lane]);}
    return b;
}
void geometry_checks() {
    using G=awl::WorldMapPlayerGeometryStatus;Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[2]=execution_skin(1);
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;std::unique_ptr<awl::WorldMapPlayerGeometry> geometry;
    auto prepare=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
            awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"synthetic geometry skin/draw bindings prepare");};
    constexpr int positions[3][3]{{-32768,-1,32767},{8192,-8192,0},{1,16384,-16384}};
    constexpr int normals[3][3]{{16384,-16384,8192},{-32768,32767,-1},{1,0,-8192}};
    constexpr int uv[2][2]{{-32768,32767},{8192,-1}};
    size_t cases=0;
    for(uint8_t p:{uint8_t(0),uint8_t(13),uint8_t(15)})for(uint8_t t:{uint8_t(0),uint8_t(14),uint8_t(15)})for(uint8_t n:{uint8_t(0),uint8_t(7),uint8_t(15)}){
        payloads[1]=geometry_gpl();for(size_t at:{size_t(64),size_t(512)}){payloads[1][at+30]=uint8_t(0x30|p);payloads[1][at+310]=uint8_t(0x30|t);payloads[1][at+294]=uint8_t(0x30|n);}
        prepare();if(!work)return;
        expect(awl::prepare_world_map_player_geometry(*work,&geometry).status==G::PreparedGeometry && geometry,"signed16 initial CPU geometry prepares with independently addressed normal/UV references");if(!geometry)return;
        expect(geometry->vertices().size()==6,"all ranges in both serialized sections decode");
        for(size_t batch=0;batch<geometry->vertices().size();++batch)for(size_t i=0;i<3;++i){const auto& v=geometry->vertices()[batch][i];
            for(size_t lane=0;lane<3;++lane){expect(v.position[lane]==float(positions[i][lane])/float(1u<<p),"signed position extremes use configured fraction");
                expect(v.normal[lane]==float(normals[(i+1)%3][lane])/16384.0f,"normal references use fixed GX signed16 fraction independent of header fraction");}
            for(size_t lane=0;lane<2;++lane)expect(v.uv[lane]==float(uv[i%2][lane])/float(1u<<t),"UV references retain configured fraction without clamp/flip");}
        auto output=work->initial_output();output[0]=0;output[1]=64;output[18]=0x40;output[19]=0;
        const auto outside=geometry->vertices()[3];
        expect(geometry->decode(*work,output).status==G::DecodedVertices && geometry->vertices()[0][0].position[0]==64.0f/float(1u<<p) &&
            geometry->vertices()[0][0].normal[0]==1.0f && geometry->vertices()[3][0].position==outside[0].position && geometry->vertices()[3][0].normal==outside[0].normal,
            "updates route selected interleaved bytes through private skin output, leaving other-section arrays immutable");
        expect(geometry->vertices()[0][0].uv==std::array<float,2>{float(uv[0][0])/float(1u<<t),float(uv[0][1])/float(1u<<t)},"mutable skin output never substitutes UV bytes");++cases;}
    std::cout<<"PLAYER_GEOMETRY_PROFILES "<<cases<<" signed scales/independent references/private binding checked\n";
    uint64_t before=14695981039346656037ull;hash_geometry(before,*geometry);auto preserved=[&](){uint64_t h=14695981039346656037ull;hash_geometry(h,*geometry);return h==before;};
    auto wrong=work->initial_output();wrong.pop_back();expect(geometry->decode(*work,wrong).status==G::InvalidInput && preserved(),"short skin output preserves complete geometry");
    wrong.push_back(0);wrong.push_back(0);expect(geometry->decode(*work,wrong).status==G::InvalidInput && preserved(),"oversized skin output cannot hide extent mismatch");
    payloads[1]=geometry_gpl();prepare();expect(geometry->decode(*work,work->initial_output()).status==G::InvalidInput && preserved(),"different retained provider cannot silently update prior geometry");
    const auto* old=geometry.get();
    expect(awl::prepare_world_map_player_geometry(*work,nullptr).status==G::InvalidInput,"null geometry output rejects");
    for(uint32_t kind=0;kind<6;++kind){payloads[1]=geometry_gpl();
        if(kind==0)word(payloads[1],512+288,40); // Normal base is not position +6.
        if(kind==1)payloads[1][512+294]=0x1d; // Signed8 normal requires a separate decoder.
        if(kind==2)payloads[1][512+310]=0x2e; // Unsigned16 UV.
        if(kind==3)payloads[1][512+295]=3; // Normal stride is no longer interleaved.
        if(kind==4)word(payloads[1],512+304,32); // UV aliases writable output.
        if(kind==5)payloads[1][64+310]=0x2e; // Reject after preceding supported section.
        prepare();expect(awl::prepare_world_map_player_geometry(*work,&geometry).status==G::UnsupportedLayout && geometry.get()==old && preserved(),"unsupported binding/format rejects atomically, including a late section");}
    payloads[1]=geometry_gpl();payloads[1][64+320]=0xb8;prepare();const auto blocked=awl::prepare_world_map_player_geometry(*work,&geometry);
    expect(blocked.status==G::RequiresTopology && blocked.section==1 && blocked.topology_failure==awl::WorldMapPlayerTopologyStatus::UnsupportedLayout && geometry.get()==old && preserved(),"topology failure retains reached context and prior geometry");
    // A genuinely in-bounds INDEX8 value 255 is a GX skip marker, not a
    // normal vertex. A larger invented array separates that from bounds errors.
    const auto small=geometry_gpl();std::vector<uint8_t> large(4608);word(large,0,0x005bbc61);word(large,12,1);word(large,16,20);word(large,20,64);word(large,24,4500);
    std::memcpy(large.data()+4500,"large",6);constexpr size_t at=64;
    word(large,at,24);word(large,at+4,3200);word(large,at+8,3216);word(large,at+12,3208);word(large,at+16,3248);large[at+20]=1;
    word(large,at+24,32);word(large,at+28,0x01003d06);word(large,at+3208,38);word(large,at+3212,0x01003d06);
    word(large,at+3200,3496);word(large,at+3204,0x00010003);word(large,at+3216,3504);word(large,at+3220,0x00023e02);
    word(large,at+3248,3392);word(large,at+3252,3264);large[at+3257]=6;
    std::memcpy(large.data()+at+3264,small.data()+512+132,96);word(large,at+3264+2*16+4,0x828);
    for(uint32_t j=2;j<5;++j){const size_t start=at+3392+(j-2)*32;word(large,at+3264+j*16+8,uint32_t(start-at));word(large,at+3264+j*16+12,16);
        large[start]=0x90;large[start+2]=3;large[start+3]=255;large[start+7]=1;large[start+8]=1;large[start+9]=1;large[start+10]=2;}
    word(payloads[0],60+20,0); // This one-section fixture has no feature group 1.
    payloads[1]=large;prepare();expect(awl::prepare_world_map_player_geometry(*work,&geometry).status==G::UnsupportedLayout && geometry.get()==old && preserved(),
        "in-bounds INDEX8 maximal position index cannot invent skip-marker connectivity");
    for(uint32_t j=2;j<5;++j)large[at+3392+(j-2)*32+3]=254;
    payloads[1]=large;prepare();std::unique_ptr<awl::WorldMapPlayerGeometry> adjacent;
    expect(awl::prepare_world_map_player_geometry(*work,&adjacent).status==G::PreparedGeometry,"adjacent in-bounds INDEX8 position remains supported");adjacent.reset();
    payloads[0]=setup_model();payloads[1]=geometry_gpl();prepare();
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t failed=0;bool reached=false;
    for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto result=awl::prepare_world_map_player_geometry(*work,&geometry);allocation_probe::enabled=false;
        if(result.status==G::PreparedGeometry){reached=true;break;}++failed;
        expect(result.status==G::AllocationFailure && geometry.get()==old && preserved() && allocation_probe::live==live,"geometry preparation allocation failures free staging and preserve all topology/providers/vertices");}
    expect(reached && failed>47,"geometry preparation allocation sweep reaches full publication");std::cout<<"PLAYER_GEOMETRY_PREPARATION_ALLOCATION_FAILURES "<<failed<<'\n';
    before=14695981039346656037ull;hash_geometry(before,*geometry);const auto update_live=allocation_probe::live;failed=0;reached=false;
    auto output=work->initial_output();output[0]=0;output[1]=64;const auto update_with_input_live=allocation_probe::live;
    for(size_t fail=0;fail<32;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto result=geometry->decode(*work,output);allocation_probe::enabled=false;
        if(result.status==G::DecodedVertices){reached=true;break;}++failed;
        expect(result.status==G::AllocationFailure && preserved() && allocation_probe::live==update_with_input_live,"late decode allocation failure preserves every batch and frees all staging");}
    expect(reached && failed==7 && update_with_input_live==update_live+1,"one staging list and six batch allocations cover the complete update");std::cout<<"PLAYER_GEOMETRY_UPDATE_ALLOCATION_FAILURES "<<failed<<'\n';
#else
    expect(awl::prepare_world_map_player_geometry(*work,&geometry).status==G::PreparedGeometry,"geometry replacement prepares");
#endif
    const auto lifetime=std::weak_ptr<const Assets>(geometry->topology().assets);assets.reset();work.reset();
    expect(!lifetime.expired() && geometry->vertices().size()==6,"decoded geometry retains source/texture providers without borrowed work/output lifetime");geometry.reset();expect(lifetime.expired(),"last geometry owner releases providers");
}
uint64_t primary_snapshot(const awl::WorldMapPlayerPrimaryOwner& owner) {
    uint64_t h=14695981039346656037ull;hash_geometry(h,owner.geometry());
    hash(h,owner.cpu().frame().has_value());if(owner.cpu().frame())hash_frame(h,*owner.cpu().frame());
    for(uint32_t w:owner.cpu().root_pose())hash(h,w);
    for(const auto& m:owner.cpu().feature_matrices())for(float v:m){uint32_t raw;std::memcpy(&raw,&v,4);hash(h,raw);}
    hash(h,owner.model().core().byte_1c);return h;
}
void primary_owner_checks() {
    using C=awl::WorldMapPlayerPrimaryStatus;using U=awl::WorldMapPlayerPrimaryUpdateStatus;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    payloads[0].resize(324);word(payloads[0],24,1);word(payloads[0],28,320);word(payloads[0],320,0x00010000);
    auto load=[&](std::shared_ptr<const Assets>* target){fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,target)==Status::Loaded,"primary synthetic providers load");};
    std::shared_ptr<const Assets> assets;load(&assets);
    // Channel must outlive all models that borrow its record identities.
    std::unique_ptr<awl::WorldMapNativeAnimationChannel> channel;
    expect(awl::construct_world_map_animation_channel(&channel)==awl::WorldMapAnimationChannelConstructionStatus::Constructed,"primary channel constructs");
    std::unique_ptr<awl::WorldMapPlayerPrimaryOwner> owner;
    expect(awl::construct_world_map_player_primary(assets,&owner).status==C::ConstructedCpuPrimary && owner,"actual primary providers construct a CPU model");if(!owner || !channel)return;
    const auto& core=owner->model().core();
    expect(core.auxiliary_c && core.feature_14 && core.nodes.size()==3 && core.head_50==2 &&
        core.nodes[1].next_feature_14==3u && core.nodes[2].next_feature_14==1u && core.nodes[0].next_feature_14==0u,
        "primary binds nonnull auxiliary, root and three nodes with independently traced 1/2/0 feature order");
    for(size_t i=0;i<3;++i){expect(core.nodes[i].feature_8 && core.nodes[i].feature_8!=core.feature_14,"node feature keys differ from root");
        for(size_t j=0;j<i;++j)expect(core.nodes[i].feature_8!=core.nodes[j].feature_8,"each retained node feature key is distinct");}
    expect(owner->model().consumed_size()==976 && owner->setup().model_allocation_size()==1120 && owner->model().flags_174()==4 &&
        owner->model().storage_requests().size()==owner->setup().storage_requests().size(),"full primary cursor requests fit known allocation independently of secondary +20 sizing");
    expect(!owner->model().playback() && !owner->model().partial_playback().word_8 && !owner->model().partial_playback().limit_c &&
        !owner->model().partial_playback().value_18 && !owner->cpu().frame(),"primary constructor keeps unwritten fields unknown and publishes no invented evaluated frame");
    awl::WorldMapPlayerPrimaryFrameInput input;input.node_post_transforms.resize(3);
    input.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    expect(owner->advance(input,nullptr).status==U::Advanced && owner->cpu().frame()->skin_executed,"fresh null animation uses owned defaults and complete skin/mesh output");
    auto bank=awl::WorldMapAnimationBank{};expect(bank.parse(200,owner_frame_clip()),"known primary translation clip parses");
    awl::WorldMapNativeAnimationInitializerMetadata metadata;metadata.has_optional_bindings=true;
    metadata.animation.model_identity_30=owner->model().binding().model_identity;
    std::vector<awl::WorldMapNativeModel*> models{&owner->model()};
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    awl::WorldMapNativeAnimationInitializerStep initialized;
    expect(awl::advance_world_map_native_animation_initializer(&metadata,channel.get(),models,2,
        awl::WorldMapActorAnimationDescriptor{2,0,absent,0},awl::WorldMapActorAnimationGroup{0,200},&bank,{},&initialized)==
        awl::WorldMapNativeAnimationInitializerStatus::Advanced && owner->model().playback() && owner->model().animation_banks().size()==1,
        "initializer publishes authoritative playback on the primary with its owned skin/features");
    bank.clear();
    expect(owner->advance(input,channel.get()).status==U::Advanced && owner->cpu().feature_matrices()==std::vector<awl::WorldMapModelMatrix>{
        {1,0,0,10,0,1,0,20,0,0,1,30},{1,0,0,11,0,1,0,22,0,0,1,33},
        {1,0,0,14,0,1,0,25,0,0,1,36},{1,0,0,17,0,1,0,28,0,0,1,39}},
        "authoritative primary playback yields independently known translations after source bank clearing");
    const auto playback_before=owner->model().partial_playback();const auto channel_before=channel->state();
    auto snapshot=primary_snapshot(*owner);auto preserved=[&](){return primary_snapshot(*owner)==snapshot;};
    input.root_pose=explicit_pose({1,0,0,40,0,1,0,50,0,0,1,60});input.node_post_transforms.pop_back();
    auto update=owner->advance(input,channel.get());
    expect(update.status==U::FrameFailure && update.frame_failure && update.frame_failure->status==awl::WorldMapPlayerFrameStatus::InvalidInput && preserved(),
        "missing late node observation preserves pose/features/skin/mesh/core byte");input.node_post_transforms.resize(3);
    input.node_post_transforms[2]=awl::WorldMapModelMatrix{1,0,0,std::numeric_limits<float>::infinity(),0,1,0,0,0,0,1,0};
    expect(owner->advance(input,channel.get()).status==U::FrameFailure && preserved(),"nonfinite late post transform rejects the entire primary frame");input.node_post_transforms[2].reset();
    auto malformed=owner_frame_clip();word(malformed,40+4,UINT32_MAX);
    expect(bank.parse(201,std::move(malformed)),"malformed late track stays opaque until sampled");
    expect(awl::advance_world_map_native_animation_initializer(&metadata,channel.get(),models,3,
        awl::WorldMapActorAnimationDescriptor{3,0,absent,0},awl::WorldMapActorAnimationGroup{0,201},&bank,{},&initialized)==
        awl::WorldMapNativeAnimationInitializerStatus::Advanced,"malformed pose payload can select checked clip metadata before frame sampling");bank.clear();
    update=owner->advance(input,channel.get());
    expect(update.status==U::FrameFailure && update.frame_failure && update.frame_failure->failed_node==2u &&
        update.frame_failure->sampling_status==awl::WorldMapAnimationPoseStatus::UnsupportedLayout && preserved(),"late authoritative sampling failure preserves all earlier primary writes");
    expect(bank.parse(202,owner_frame_clip()) && awl::advance_world_map_native_animation_initializer(&metadata,channel.get(),models,4,
        awl::WorldMapActorAnimationDescriptor{4,0,absent,0},awl::WorldMapActorAnimationGroup{0,202},&bank,{},&initialized)==
        awl::WorldMapNativeAnimationInitializerStatus::Advanced,"new immutable bank restores primary clip without changing retained identity bytes");bank.clear();
    const auto* previous=owner.get();
    expect(awl::construct_world_map_player_primary({},&owner).status==C::RequiresAssets && owner.get()==previous && preserved(),"missing assets preserve existing primary model/frame/geometry");
    expect(awl::construct_world_map_player_primary(assets,nullptr).status==C::InvalidInput,"null primary output rejects");
    for(uint32_t kind=0;kind<3;++kind){auto saved=payloads;
        if(kind==0)payloads[1][512+310]=0x2e; // Unsupported signedness in a late geometry section.
        if(kind==1)payloads[1][64+320]=0xb8; // Unsupported primitive reached by topology.
        if(kind==2)payloads[2].resize(8); // Invalid skin metadata.
        std::shared_ptr<const Assets> invalid;load(&invalid);const auto rejected=awl::construct_world_map_player_primary(invalid,&owner);
        expect((kind<2?rejected.status==C::RequiresGeometry:rejected.status==C::RequiresFrameState) && owner.get()==previous && preserved(),
            "unsupported geometry/topology/skin cannot replace complete primary ownership");payloads=std::move(saved);}
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t failed=0;bool reached=false;
    for(size_t fail=0;fail<64;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        update=owner->advance(input,channel.get());allocation_probe::enabled=false;
        if(update.status==U::Advanced){reached=true;break;}++failed;
        expect(update.status==U::AllocationFailure && preserved() && allocation_probe::live==live,"every frame/feature/skin/late mesh allocation failure frees staging and preserves complete primary state");}
    expect(reached && failed>13,"primary sweep reaches geometry allocations after completed skin/frame work");
    std::cout<<"PLAYER_CPU_PRIMARY_UPDATE_ALLOCATION_FAILURES "<<failed<<'\n';snapshot=primary_snapshot(*owner);
#endif
    input.request_skin=false;const auto old_vertices=owner->cpu().vertex_output();
    expect(owner->advance(input,channel.get()).status==U::Advanced && owner->cpu().vertex_output()==old_vertices && !owner->cpu().frame()->skin_executed,
        "authoritative node-only update retains skin bytes while republishing geometry consistently");
    input.evaluate_nodes=false;input.node_post_transforms.clear();input.root_pose.reset();
    snapshot=primary_snapshot(*owner);
    expect(owner->advance(input,nullptr).status==U::Advanced && owner->cpu().frame()->node_matrices.empty() && owner->cpu().vertex_output()==old_vertices,
        "node-off update uses persistent root/skin without borrowing animation channel records");
    expect(owner->model().partial_playback().position_0==playback_before.position_0 && owner->model().partial_playback().link_14==playback_before.link_14 &&
        channel->state().target_8==channel_before.target_8 && channel->state().elapsed_0==channel_before.elapsed_0,"frame publication does not advance animation/channel clocks or acknowledge actor state");
    // All owners retain providers after loader ownership drops.
    const auto lifetime=std::weak_ptr<const Assets>(assets);assets.reset();
    expect(!lifetime.expired() && owner->advance(input,nullptr).status==U::Advanced,"primary frame/resources outlive source asset handle");
    auto child=frame_owner(true);if(!child)return;
    const auto p=owner->model().binding().model_identity,c=child->binding().model_identity;
    awl::WorldMapModelAttachmentStep attached;
    expect(awl::apply_world_map_native_model_attachments({&owner->model(),child.get()},{p,c,99,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced,
        "supported native graph operation attaches a child to the actual CPU primary");snapshot=primary_snapshot(*owner);
    expect(owner->advance(input,channel.get()).status==U::RequiresHierarchy && preserved(),"attached child requires whole hierarchy evaluation before primary publication");
    expect(awl::apply_world_map_native_model_attachments({&owner->model(),child.get()},{p,0,0,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced,
        "verified native clear detaches the child");
    expect(awl::apply_world_map_native_model_attachments({&owner->model(),child.get()},{c,p,99,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced,
        "supported source operation replaces primary node binding and attaches primary under another model");snapshot=primary_snapshot(*owner);
    expect(owner->advance(input,channel.get()).status==U::RequiresHierarchy && preserved(),"inherited primary also requires hierarchy provider");
    expect(awl::apply_world_map_native_model_attachments({&owner->model(),child.get()},{c,0,0,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced,
        "verified graph clear detaches inherited primary");
    expect(owner->advance(input,channel.get()).status==U::RequiresFeatureBindings && preserved(),"detached primary with an external feature cannot execute stale retained providers");
    child.reset();models.clear();metadata={};initialized={};
    // A feature newly added to an originally omitted node is also external.
    auto absent_payloads=payloads;absent_payloads[0]=setup_model();absent_payloads[0].resize(324);
    word(absent_payloads[0],24,1);word(absent_payloads[0],28,320);word(absent_payloads[0],320,0x00010000);
    for(size_t i=0;i<3;++i)word(absent_payloads[0],32+i*28+20,0xffff0000);
    fixture.write("boy_0.arc",archive(absent_payloads));std::shared_ptr<const Assets> absent_assets;
    expect(awl::load_world_map_player_model_assets(0,&absent_assets)==Status::Loaded,"omitted-feature synthetic providers load");
    auto parent=frame_owner(true);std::unique_ptr<awl::WorldMapPlayerPrimaryOwner> omitted;
    expect(awl::construct_world_map_player_primary(absent_assets,&omitted).status==C::ConstructedCpuPrimary && omitted &&
        omitted->setup().features().size()==1 && !omitted->setup().features()[0].node,
        "retained root with omitted node features constructs without fabricated node bindings");if(!omitted || !parent)return;
    expect(omitted->advance(input,nullptr).status==U::Advanced,"omitted features do not prevent supported CPU frame/skin/mesh evaluation");
    const auto parent_key=parent->binding().model_identity,omitted_key=omitted->model().binding().model_identity;
    expect(awl::apply_world_map_native_model_attachments({parent.get(),&omitted->model()},{parent_key,omitted_key,99,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced &&
        awl::apply_world_map_native_model_attachments({parent.get(),&omitted->model()},{parent_key,0,0,0},&attached)==awl::WorldMapModelAttachmentStatus::Advanced,
        "external feature can be added to an omitted node then detached through verified operations");
    expect(omitted->advance(input,nullptr).status==U::RequiresFeatureBindings,"newly added external feature cannot silently disappear from CPU evaluation");
    omitted.reset();parent.reset();absent_assets.reset();attached={};
#if !defined(_MSC_VER) || !defined(_DEBUG)
    auto retained=owner->setup().selection().assets;previous=owner.get();snapshot=primary_snapshot(*owner);
    const auto preparation_live=allocation_probe::live;size_t preparation_failed=0;bool preparation_reached=false;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto result=awl::construct_world_map_player_primary(retained,&owner);allocation_probe::enabled=false;
        if(result.status==C::ConstructedCpuPrimary){preparation_reached=true;break;}++preparation_failed;
        expect(result.status==C::AllocationFailure && owner.get()==previous && preserved() && allocation_probe::live==preparation_live,
            "every primary construction allocation failure preserves old model/frame/geometry and releases staged providers");}
    expect(preparation_reached && preparation_failed>180,"primary allocation sweep reaches full retained construction");
    std::cout<<"PLAYER_CPU_PRIMARY_CONSTRUCTION_ALLOCATION_FAILURES "<<preparation_failed<<'\n';retained.reset();
#endif
    owner.reset();expect(lifetime.expired(),"last primary releases all ACT/GPL/SKN/texture providers");
}
void local_primary(const char* disc) {
    expect(awl::filesystem_mount("/",disc),"local primary disc mounts");
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_start_animation_tables(&tables)==awl::WorldMapPlayerStartAnimationStatus::Loaded && tables && tables->target_verified(),"exact local DOL startup tables verify");
    expect(awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"local startup animation providers load");if(!tables || !animations)return;
    size_t checked=0;uint64_t frame_digest=14695981039346656037ull,geometry_digest=frame_digest;
    for(uint32_t phase=0;phase<6;++phase){std::shared_ptr<const Assets> assets;
        expect(awl::load_world_map_player_model_assets(phase,&assets)==Status::Loaded,"local primary phase providers load");
        std::unique_ptr<awl::WorldMapNativeAnimationChannel> channel;
        expect(awl::construct_world_map_animation_channel(&channel)==awl::WorldMapAnimationChannelConstructionStatus::Constructed,"local primary channel constructs");
        std::unique_ptr<awl::WorldMapPlayerPrimaryOwner> owner;
        expect(awl::construct_world_map_player_primary(assets,&owner).status==awl::WorldMapPlayerPrimaryStatus::ConstructedCpuPrimary,"local primary constructs complete CPU providers");if(!owner || !channel)return;
        expect(owner->model().core().auxiliary_c && owner->model().core().nodes.size()==55 && owner->model().core().feature_14 &&
            owner->model().consumed_size()<=owner->setup().model_allocation_size(),"actual 55-node primary has auxiliary/root bindings within original sizing plan");
        awl::WorldMapNativeAnimationInitializerMetadata metadata;metadata.has_optional_bindings=true;
        metadata.animation.model_identity_30=owner->model().binding().model_identity;
        awl::WorldMapPlayerStartAnimationCommand command;awl::WorldMapPlayerStartNativeAnimationStep step;
        expect(awl::advance_world_map_player_start_animation(tables,command,std::nullopt,&metadata,channel.get(),{&owner->model()},animations,{},&step)==
            awl::WorldMapPlayerStartAnimationStatus::Advanced && step.selection && step.selection->descriptor_index==4 && owner->model().playback(),
            "actual no-item startup initializes the real CPU primary instead of a no-skin substitute");
        if(!step.selection || !owner->model().playback())return;
        awl::WorldMapPlayerPrimaryFrameInput input;input.node_post_transforms.resize(55);
        input.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});assets.reset();
        expect(owner->advance(input,channel.get()).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && owner->cpu().frame() && owner->cpu().frame()->skin_executed,
            "actual primary startup playback reaches skin and all decoded mesh batches after source assets drop");if(!owner->cpu().frame())return;
        // Compare to the existing supplied CPU path with identical documented
        // native numerics. This checks wiring, not original-game equivalence.
        awl::WorldMapPlayerAnimationFrameInput supplied;supplied.root_pose=*input.root_pose;supplied.node_post_transforms=input.node_post_transforms;
        supplied.playback=*owner->model().playback();std::vector<const awl::WorldMapAnimationBank*> banks;
        for(const auto& bank:owner->model().animation_banks())banks.push_back(bank.get());
        awl::WorldMapPlayerFrame expected;
        expect(awl::evaluate_world_map_player_animation_frame(owner->cpu().work(),supplied,{},banks,owner->cpu().work().initial_output(),&expected).status==
            awl::WorldMapPlayerFrameStatus::Evaluated,"local supplied comparison frame evaluates");
        uint64_t wanted=14695981039346656037ull,actual=wanted;hash_frame(wanted,expected);hash_frame(actual,*owner->cpu().frame());
        expect(wanted==actual,"local authoritative frame matches already checked supplied path for pose/features/skin");
        std::unique_ptr<awl::WorldMapPlayerGeometry> expected_geometry;
        expect(awl::prepare_world_map_player_geometry(owner->cpu().work(),&expected_geometry).status==awl::WorldMapPlayerGeometryStatus::PreparedGeometry &&
            expected_geometry->decode(owner->cpu().work(),expected.vertex_output).status==awl::WorldMapPlayerGeometryStatus::DecodedVertices,"local supplied comparison geometry evaluates");
        if(!expected_geometry)return;
        wanted=actual=14695981039346656037ull;hash_geometry(wanted,*expected_geometry);hash_geometry(actual,owner->geometry());expect(wanted==actual,"all actual startup batches match supplied decode");
        hash(frame_digest,phase);hash_frame(frame_digest,*owner->cpu().frame());hash(geometry_digest,phase);hash_geometry(geometry_digest,owner->geometry());
        const auto before=primary_snapshot(*owner);const auto descriptor=metadata.animation.base_descriptor_4;const auto record=owner->model().partial_playback();command.action=1;
        expect(awl::advance_world_map_player_start_animation(tables,command,std::nullopt,&metadata,channel.get(),{&owner->model()},animations,{},&step)==
            awl::WorldMapPlayerStartAnimationStatus::InitializerIncomplete && step.initializer.initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresSecondarySetup &&
            metadata.animation.base_descriptor_4==descriptor && owner->model().partial_playback().clip_10->offset==record.clip_10->offset && primary_snapshot(*owner)==before,
            "actual action dependency preserves primary animation/frame/mesh instead of accepting startup");
        if(phase==5){animations.reset();step={};
            expect(owner->advance(input,channel.get()).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"actual selected animation snapshot survives startup asset and proposal release");}
        ++checked;
    }
    expect(checked==6,"all six actual phase CPU primaries verified");
    std::cout<<"LOCAL_CPU_PRIMARY_STARTUP "<<checked<<" phases / 55 nodes / frame "<<std::hex<<frame_digest<<" geometry "<<geometry_digest<<std::dec<<"; CPU ownership only, holder/actor/GPU pending\n";
}
uint64_t holder_snapshot(const awl::WorldMapPlayerAnimationHolder& holder) {
    uint64_t h=primary_snapshot(holder.primary());
    const auto key=[&](uint64_t v){hash(h,uint32_t(v>>32));hash(h,uint32_t(v));};
    const auto real=[&](float v){uint32_t w;std::memcpy(&w,&v,4);hash(h,w);};
    const auto optional_word=[&](const auto& v){hash(h,v.has_value());if(v)hash(h,uint32_t(*v));};
    const auto optional_real=[&](const auto& v){hash(h,v.has_value());if(v)real(*v);};
    const auto& s=holder.state();key(s.current_descriptor_0);key(s.base_descriptor_4);real(s.speed_8);
    hash(h,s.default_duration_c);optional_word(s.deadline_10);hash(h,s.default_count_14);
    optional_word(s.count_18);optional_word(s.completed_count_1c);optional_word(s.completed_20);optional_word(s.flag_21);
    for(uint32_t v:{s.restart_deadline_24,s.restart_limit_28,s.restart_count_2c})hash(h,v);
    for(uint64_t v:{s.model_identity_30,s.group_identity_34,s.secondary_model_c0,s.feature_c4,s.auxiliary_model_c8,s.arena_cc})key(v);
    hash(h,s.feature_38.has_value());if(s.feature_38){const auto& f=*s.feature_38;
        hash(h,f.type_0);hash(h,f.type_4);hash(h,f.index_8);key(f.table_c);
        for(const auto* t:{&f.first_14,&f.second_24}){key(t->resource_0);hash(h,t->clock_4);real(t->value_8);real(t->rate_c);}}
    hash(h,s.feature_3c.has_value());if(s.feature_3c){key(s.feature_3c->resource_0);real(s.feature_3c->value_4);real(s.feature_3c->value_8);}
    optional_word(s.feature_source_10);optional_word(s.feature_flag_34);key(reinterpret_cast<uintptr_t>(holder.initial_inputs().get()));
    key(reinterpret_cast<uintptr_t>(holder.timer_assets().get()));
    hash(h,holder.initial_model_state().has_value());if(holder.initial_model_state()){
        const auto& m=*holder.initial_model_state();hash(h,m.model_slot_4);hash(h,m.alternate_8);hash(h,m.runtime_entry_address_120);hash(h,m.initialized_c);
        hash(h,m.private_texture_animation.has_value());if(m.private_texture_animation){const auto& v=*m.private_texture_animation;
            key(v.reference.bank_identity);hash(h,v.reference.offset);hash(h,v.size);key(reinterpret_cast<uintptr_t>(v.data));}}
    key(reinterpret_cast<uintptr_t>(holder.game_clock().get()));
    if(holder.game_clock()){const auto& clock=holder.game_clock()->state();hash(h,clock.raw_time);hash(h,clock.loop_count);hash(h,clock.retrace_interval);}
    const auto partial=[&](const awl::WorldMapAnimationPartialPlayback& p){real(p.position_0);real(p.rate_4);optional_word(p.word_8);
        optional_real(p.limit_c);hash(h,p.clip_10.has_value());if(p.clip_10){key(p.clip_10->bank_identity);hash(h,p.clip_10->offset);}key(p.link_14);optional_real(p.value_18);};
    partial(holder.primary().model().partial_playback());key(holder.primary().model().animation_banks().size());
    for(const auto& bank:holder.primary().model().animation_banks())key(bank->identity());
    for(const auto* channel:{&holder.primary_channel(),&holder.secondary_channel()}){const auto& c=channel->state();
        hash(h,c.elapsed_0);hash(h,c.duration_4);key(c.target_8);key(c.previous_c);key(c.older_10);optional_real(c.blend_14);hash(h,c.mode_18);
        for(const auto& r:channel->records()){key(r.identity);partial(r.state);}}
    key(holder.descriptor().identity);hash(h,holder.descriptor().word_0);hash(h,holder.descriptor().word_4);hash(h,holder.descriptor().word_8);return h;
}
std::vector<uint8_t> holder_start_tables() {
    // Invented DOL sections, four descriptors and indices. Trace-derived
    // addresses only locate the existing checked selector-one decoder.
    std::vector<uint8_t> b(0x1C1);
    constexpr uint32_t addresses[]{0x8029E4BC,0x80249238,0x8029BEF4,0x81001000,0x80282DF8};
    constexpr uint32_t offsets[]{0x100,0x120,0x140,0x160,0x1C0},sizes[]{4,8,16,96,1};
    for(size_t i=0;i<5;++i){word(b,i*4,offsets[i]);word(b,0x48+i*4,addresses[i]);word(b,0x90+i*4,sizes[i]);}
    word(b,0x100,0x800289A8);
    constexpr uint32_t modes[]{1,3,2,0};
    for(uint32_t i=0;i<4;++i){b[0x121+i*2]=uint8_t(10+i);word(b,0x140+i*4,0x81001000+i*24);
        word(b,0x160+i*24,(modes[i]<<10)|(i&1));word(b,0x164+i*24,63u|((i==3?0u:127u)<<11)|(255u<<19));word(b,0x168+i*24,0x1500+i);}
    return b;
}
void holder_checks() {
    using H=awl::WorldMapPlayerAnimationHolderStatus;using S=awl::WorldMapPlayerStartAnimationStatus;using I=awl::WorldMapAnimationInitializerStatus;
    const awl::WorldMapPlayerAnimationHolderState fresh;
    expect(fresh.speed_8==1 && fresh.default_duration_c==1000 && fresh.default_count_14==1 &&
        !fresh.deadline_10 && !fresh.count_18 && !fresh.completed_count_1c && !fresh.completed_20 && !fresh.flag_21 &&
        !fresh.current_descriptor_0 && !fresh.base_descriptor_4 && !fresh.model_identity_30 && !fresh.group_identity_34 &&
        !fresh.secondary_model_c0 && !fresh.feature_c4 && !fresh.auxiliary_model_c8 && !fresh.arena_cc,
        "D560 constructor fields match exact stores while five unwritten words/bytes remain unknown");
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    fixture.write("boy_0.arc",archive(payloads));fixture.write("boy_0.anm.arc",archive({owner_frame_clip(),owner_frame_clip(4)}));
    fixture.write("boy_0_subanm.arc",archive({owner_frame_clip()}));fixture.write("boy_0_subact.arc",archive({setup_model()}));
    std::shared_ptr<const Assets> models;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_model_assets(0,&models)==Status::Loaded && awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,
        "holder synthetic model/animation providers load");if(!models || !animations)return;
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    const awl::WorldMapActorAnimationDescriptor initial{100,0,absent,0};std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,std::nullopt,{},&holder).status==H::ConstructedCpuHolder && holder,
        "fresh D610 binding initializes real CPU primary with two owned channels");if(!holder)return;
    const auto& state=holder->state();
    expect(state.current_descriptor_0==100 && state.base_descriptor_4==100 && state.completed_20==0u && state.flag_21==0u &&
        state.default_duration_c==1000 && state.default_count_14==1 && !state.deadline_10 && !state.count_18 && !state.completed_count_1c &&
        state.model_identity_30==holder->primary().model().binding().model_identity && state.group_identity_34 &&
        holder->primary().model().core().auxiliary_c && !state.feature_38 && !state.feature_3c,
        "mode-zero binding establishes descriptor/model/group and reset bytes without fabricating preserved counters");
    const auto& secondary=holder->secondary_channel();
    expect(secondary.state().elapsed_0==0 && secondary.state().duration_4==0 && secondary.state().mode_18==0 && !secondary.state().blend_14,
        "unused secondary channel retains traced fresh constructor state");
    for(const auto& r:secondary.records())expect(!r.state.clip_10 && !r.state.link_14 && r.state.position_0==0 && r.state.rate_4==1 &&
        !r.state.word_8 && !r.state.limit_c && !r.state.value_18,"unused secondary playback fields remain unknown");
    for(const auto& p:holder->primary_channel().records())for(const auto& s:secondary.records())expect(p.identity!=s.identity,"all six holder channel record keys are independent");
    awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(3);frame.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    expect(holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && holder->primary().cpu().frame()->skin_executed,
        "holder-owned primary playback reaches the retained CPU frame/skin/mesh path");
    auto before=holder_snapshot(*holder);auto preserved=[&](){return holder_snapshot(*holder)==before;};const auto* old=holder.get();
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,std::nullopt,{},nullptr).status==H::InvalidInput,"null holder output rejects");
    expect(awl::construct_world_map_player_animation_holder({},animations,initial,std::nullopt,{},&holder).status==H::RequiresModelAssets && holder.get()==old && preserved(),"missing model providers preserve complete existing holder");
    expect(awl::construct_world_map_player_animation_holder(models,{},initial,std::nullopt,{},&holder).status==H::RequiresAnimationAssets && holder.get()==old && preserved(),"missing animation bundle preserves holder");
    auto invalid=initial;invalid.identity=0;
    expect(awl::construct_world_map_player_animation_holder(models,animations,invalid,std::nullopt,{},&holder).status==H::InvalidInput && preserved(),"fresh null descriptor cannot fake D610 initialization");
    invalid=initial;invalid.word_0=1u<<25;
    expect(awl::construct_world_map_player_animation_holder(models,animations,invalid,std::nullopt,{},&holder).status==H::RequiresGroup && preserved(),"unsupported group preserves retained model/channels");
    invalid=initial;invalid.word_0=7;
    auto failed=awl::construct_world_map_player_animation_holder(models,animations,invalid,std::nullopt,{},&holder);
    expect(failed.status==H::InvalidInput && holder.get()==old && preserved(),"out-of-range reached clip rejects all new holder/model construction");
    invalid=initial;invalid.word_0=2u<<10;
    failed=awl::construct_world_map_player_animation_holder(models,animations,invalid,std::nullopt,{},&holder);
    expect(failed.status==H::InitializerIncomplete && failed.initializer_status==I::RequiresClock && preserved(),"timed binding cannot invent a clock or replace prior holder");
    awl::WorldMapAnimationFeature38 feature{0x3a,0,0,0};
    failed=awl::construct_world_map_player_animation_holder(models,animations,initial,feature,{},&holder);
    expect(failed.status==H::InitializerIncomplete && failed.initializer_status==I::RequiresFeatureTable && preserved(),"reached optional feature table stops after staging channel setup");
    awl::WorldMapAnimationInitializerObservations obs;obs.fallback_table_38=900;
    failed=awl::construct_world_map_player_animation_holder(models,animations,initial,feature,obs,&holder);
    expect(failed.status==H::InitializerIncomplete && failed.initializer_status==I::RequiresFeatureRow && failed.required_row && failed.required_row->table_identity==900 && preserved(),"feature row requirement remains visible without exposing fabricated counter snapshots");
    obs.rows={{900,0,0,777}};obs.clock=17;std::unique_ptr<awl::WorldMapPlayerAnimationHolder> featured;
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,feature,obs,&featured).status==H::ConstructedCpuHolder &&
        featured && featured->state().feature_38->first_14.resource_0==777 && featured->state().feature_38->first_14.clock_4==17 &&
        featured->state().feature_38->index_8==UINT32_MAX,"complete supplied optional feature snapshot executes its verified timer stores");featured.reset();
    auto bytes=holder_start_tables();std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==S::Decoded,"invented holder start tables decode");if(!tables)return;
    awl::WorldMapPlayerStartAnimationCommand command;
    expect(holder->start({},command,std::nullopt,{}).status==S::RequiresTables && preserved(),"missing selector tables preserve holder");
    command.selector=2;expect(holder->start(tables,command,std::nullopt,{}).status==S::UnsupportedCommand && preserved(),"unsupported selector preserves complete holder");command.selector=1;
    command.item=1;expect(holder->start(tables,command,std::nullopt,{}).status==S::RequiresItemType && preserved(),"nonzero item requires keyed classification before mutation");command.item=0;command.action=1;
    auto started=holder->start(tables,command,std::nullopt,{});
    expect(started.status==S::InitializerIncomplete && started.initializer_status==I::RequiresClock && preserved(),"late startup clock dependency preserves holder state, both channels and prior frame");command.action=0;
    started=holder->start(tables,command,std::nullopt,{});
    expect(started.status==S::Advanced && !holder->state().deadline_10 && !holder->state().count_18 && !holder->state().completed_count_1c &&
        holder->primary().model().playback()->word_8==1,"loop mode establishes playback while preserving all unknown counters");
    before=holder_snapshot(*holder);expect(holder->start(tables,command,std::nullopt,{}).status==S::Unchanged && preserved(),"same-base D660 start skips setup and changes no holder/channel/frame state");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    allocation_probe::remaining=0;allocation_probe::enabled=true;const auto equal=holder->start(tables,command,std::nullopt,{});allocation_probe::enabled=false;
    expect(equal.status==S::Unchanged && preserved(),"equal-base start performs no allocating model/channel setup");
#endif
    command.item=1;const awl::WorldMapPlayerStartItemType item{1,0};started=holder->start(tables,command,item,{});
    expect(started.status==S::Advanced && holder->state().count_18==1u && holder->state().completed_count_1c==0u && !holder->state().deadline_10 &&
        holder->primary().model().playback()->word_8==0,"count mode uses verified D560 default one and establishes only reached counters");
    command.item=0;command.action=1;obs={};obs.clock=UINT32_MAX-5;started=holder->start(tables,command,std::nullopt,obs);
    expect(started.status==S::Advanced && holder->state().deadline_10==994u && holder->state().count_18==1u && holder->state().completed_count_1c==0u,
        "timed mode uses verified default 1000 with unsigned wrapping and retains known count fields");
    before=holder_snapshot(*holder);expect(holder->start(tables,command,std::nullopt,{}).status==S::Unchanged && preserved(),"equal timed base skips now-unavailable clock on subsequent D660 start");
    command.item=0x4ff;command.action=0;started=holder->start(tables,command,awl::WorldMapPlayerStartItemType{0x4ff,0},{});
    expect(started.status==S::InitializerIncomplete && started.initializer_status==I::RequiresSecondarySetup && started.secondary_index==0u && preserved(),"selected secondary dependency preserves both owned channels, counters and primary frame");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    command.item=1;const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto result=holder->start(tables,command,item,{});allocation_probe::enabled=false;
        if(result.status==S::Advanced){reached=true;break;}++rejected;
        expect(result.status==S::AllocationFailure && preserved() && allocation_probe::live==live,"all holder startup allocation failures preserve known/unknown counters, both channels, primary and prior frame");}
    expect(reached && rejected>20,"holder startup allocation sweep reaches final publication");std::cout<<"PLAYER_HOLDER_START_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
    const auto table_lifetime=std::weak_ptr<const awl::WorldMapPlayerStartAnimationTables>(tables);tables.reset();
    expect(!table_lifetime.expired(),"holder retains accepted startup table provider");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    old=holder.get();before=holder_snapshot(*holder);const auto construction_live=allocation_probe::live;size_t failed_allocations=0;bool constructed=false;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto result=awl::construct_world_map_player_animation_holder(models,animations,initial,std::nullopt,{},&holder);allocation_probe::enabled=false;
        if(result.status==H::ConstructedCpuHolder){constructed=true;break;}++failed_allocations;
        expect(result.status==H::AllocationFailure && holder.get()==old && preserved() && allocation_probe::live==construction_live,
            "all channel/primary/late initializer construction allocation failures free staging and preserve the complete old holder");}
    expect(constructed && failed_allocations>193,"holder construction sweep includes channels and initialization after all primary allocations");std::cout<<"PLAYER_HOLDER_CONSTRUCTION_ALLOCATION_FAILURES "<<failed_allocations<<'\n';
    expect(table_lifetime.expired(),"successful fresh replacement releases only the old accepted table provider");
#endif
    const auto model_lifetime=std::weak_ptr<const Assets>(models);const auto animation_lifetime=std::weak_ptr<const awl::WorldMapPlayerAnimationAssets>(animations);
    models.reset();animations.reset();
    expect(!model_lifetime.expired() && !animation_lifetime.expired() && holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,
        "holder keeps CPU providers and bank group alive after source handles drop");
    holder.reset();expect(model_lifetime.expired() && animation_lifetime.expired() && table_lifetime.expired(),"holder destruction releases providers after its model and borrowed channel keys");
}
void local_holder(const char* disc) {
    using H=awl::WorldMapPlayerAnimationHolderStatus;using S=awl::WorldMapPlayerStartAnimationStatus;
    expect(awl::filesystem_mount("/",disc),"local holder disc mounts");
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_start_animation_tables(&tables)==S::Loaded && tables && tables->target_verified(),"local holder tables verify exact DOL");
    expect(awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"local holder bank group loads");if(!tables || !animations)return;
    awl::WorldMapPlayerStartAnimationCommand command;awl::WorldMapPlayerStartAnimationSelection selected;
    expect(tables->select(command,std::nullopt,&selected)==S::Selected,"local supplied initial no-item descriptor selects");
    uint64_t frame_digest=14695981039346656037ull,geometry_digest=frame_digest;size_t checked=0;
    for(uint32_t phase=0;phase<6;++phase){std::shared_ptr<const Assets> models;std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        expect(awl::load_world_map_player_model_assets(phase,&models)==Status::Loaded,"local holder phase model loads");
        expect(awl::construct_world_map_player_animation_holder(models,animations,selected.descriptor,std::nullopt,{},&holder).status==H::ConstructedCpuHolder && holder,
            "actual CPU model/group construct holder using supplied initial descriptor and null optional fixture");if(!holder)return;
        expect(holder->state().default_count_14==1 && holder->state().default_duration_c==1000 &&
            !holder->state().deadline_10 && !holder->state().count_18 && !holder->state().completed_count_1c &&
            holder->primary().model().core().nodes.size()==55 && holder->primary().model().core().auxiliary_c,
            "actual loop descriptor retains D560 unknown counters on the complete 55-node CPU primary");
        models.reset();if(phase==5)animations.reset();
        awl::WorldMapPlayerPrimaryFrameInput input;input.node_post_transforms.resize(55);input.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
        expect(holder->advance(input).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && holder->primary().cpu().frame() && holder->primary().cpu().frame()->skin_executed,
            "actual owned holder executes CPU frame/skin/mesh after source handles drop");if(!holder->primary().cpu().frame())return;
        hash(frame_digest,phase);hash_frame(frame_digest,*holder->primary().cpu().frame());hash(geometry_digest,phase);hash_geometry(geometry_digest,holder->primary().geometry());
        const auto before=holder_snapshot(*holder);command.action=0;
        expect(holder->start(tables,command,std::nullopt,{}).status==S::Unchanged && holder_snapshot(*holder)==before,"actual equal-base D660 start preserves holder and frame");command.action=1;
        const auto failed=holder->start(tables,command,std::nullopt,{});
        expect(failed.status==S::InitializerIncomplete && failed.initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresSecondarySetup &&
            holder_snapshot(*holder)==before,"actual action-only secondary dependency rolls back whole owned holder");command.action=0;++checked;
    }
    expect(checked==6 && frame_digest==0xcc652d4bb4e9690aull && geometry_digest==0xf5968658e37d53caull,
        "all holder frames/meshes preserve the previous local primary wiring fingerprints under documented native numerics");
    std::cout<<"LOCAL_PLAYER_HOLDER "<<checked<<" phases / two owned channels / unknown counters retained / prior CPU fingerprints unchanged; actor startup and GPU pending\n";
}
std::vector<uint8_t> initial_input_dol() {
    // Invented sections, indices, descriptors, phase rows and scale catalog.
    std::vector<uint8_t> b(0x248);
    constexpr uint32_t addresses[]{0x80249230,0x8029E4B8,0x80282DF8,0x8029BECC,0x81000000,0x8029F818,0x81000100,0x8024A6E0,0x80249A6C};
    constexpr uint32_t offsets[]{0x100,0x108,0x10C,0x110,0x130,0x160,0x180,0x1B0,0x1B8}, sizes[]{8,4,1,20,48,24,48,6,144};
    for(size_t i=0;i<9;++i){word(b,i*4,offsets[i]);word(b,0x48+i*4,addresses[i]);word(b,0x90+i*4,sizes[i]);}
    word(b,0x108,0x800289A8);
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    for(uint32_t i=0;i<4;++i){b[0x101+i*2]=uint8_t(i+1);word(b,0x114+i*4,0x81000000+i*12);
        word(b,0x130+i*12,1u<<10);word(b,0x134+i*12,absent);word(b,0x138+i*12,900+i);}
    for(uint32_t i=0;i<6;++i){word(b,0x160+i*4,0x81000100+i*8);word(b,0x180+i*8,0x3A+i);word(b,0x184+i*8,0x24+i);}
    constexpr uint8_t catalog[]{5,2,4,1,3,0};
    constexpr uint32_t scale_words[]{0x3F800001,0x80000000,0,0x3F000000,0xC0400000,0x40000000};
    for(size_t i=0;i<6;++i){b[0x1B0+i]=catalog[i];word(b,0x1BC+i*24,scale_words[i]);}
    return b;
}
awl::WorldMapAnimationInitializerObservations initial_timers(const awl::WorldMapPlayerInitialAnimationSelection& selection,uint32_t clock) {
    awl::WorldMapAnimationInitializerObservations obs;obs.fallback_table_38=900;obs.fallback_table_24=901;obs.clock=clock;
    obs.rows={{900,selection.type_0-0x3A,0,700+selection.phase},{901,selection.type_4-0x24,0,800+selection.phase}};return obs;
}
void initial_root_scale_checks() {
    using S=awl::WorldMapPlayerInitialAnimationStatus;
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<256;++seed)for(uint32_t raw:{0x3F800000u,0x40000000u,0xC0400000u,0u,0x80000000u,0x3F000000u,0x3F800001u}){
        awl::WorldMapAnimationPose before;for(uint32_t i=0;i<13;++i)before[i]=0x7FC01234u^((seed+i)*0x01020304u);
        float scale;std::memcpy(&scale,&raw,4);auto result=before;
        expect(awl::prepare_world_map_player_initial_root_pose(before,scale,&result)==S::Prepared,"initial root scale writes accept finite bits without reading other pose floats");
        expect(result[0]==((before[0]&0x00FFFFFFu)|0x01000000u) && result[1]==raw && result[2]==raw && result[3]==raw,
            "initial root clear/set changes only flag byte and three uniform scale words");
        for(size_t i=4;i<13;++i)expect(result[i]==before[i],"unwritten root pose components survive initial scale writes");
        hash(digest,seed);hash(digest,raw);for(uint32_t word:result)hash(digest,word);
        auto alias=before;expect(awl::prepare_world_map_player_initial_root_pose(alias,scale,&alias)==S::Prepared && alias==result,"root setup supports whole-pose output alias");
    }
    expect(digest==0xA55B0FCE86438450ull,"1792 root setup cases match original clear-byte and 1FC4 instruction writes");
    awl::WorldMapAnimationPose before;before.fill(0xFFFFFFFFu);auto out=before;
    for(float value:{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        expect(awl::prepare_world_map_player_initial_root_pose(before,value,&out)==S::InvalidInput && out==before,"unsupported root scale numerics preserve output");
    expect(awl::prepare_world_map_player_initial_root_pose(before,1,nullptr)==S::InvalidInput,"null initial root setup output rejects");
    std::cout<<"PLAYER_INITIAL_ROOT_SCALE 1792 digest "<<std::hex<<digest<<std::dec<<'\n';
}
void initial_input_checks() {
    using S=awl::WorldMapPlayerInitialAnimationStatus;using H=awl::WorldMapPlayerInitialHolderStatus;
    auto bytes=initial_input_dol();std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> inputs;
    expect(awl::decode_world_map_player_initial_animation_inputs(bytes.data(),bytes.size(),&inputs)==S::Decoded && inputs && !inputs->target_verified(),
        "invented initial static inputs decode without claiming target identity");if(!inputs)return;
    const auto* previous=inputs.get();awl::WorldMapPlayerInitialAnimationQuery query;
    constexpr uint32_t expected_scales[]{0x40000000,0,0xC0400000,0x80000000,0x3F000000,0x3F800001};
    for(uint32_t phase=0;phase<6;++phase){query.phase=phase;const auto selected=inputs->select(query);
        expect(selected.status==S::Selected && selected.selection && selected.selection->descriptor_index==1 &&
            selected.selection->descriptor.identity==0x81000000 && selected.selection->descriptor.word_8==900 &&
            selected.selection->type_0==0x3A+phase && selected.selection->type_4==0x24+phase &&
            selected.selection->feature_source_10==0 && selected.selection->feature_flag_34==0,"initial slot-zero descriptor and distinct phase type rows retain exact keys");
        if(!selected.selection)return;
        uint32_t scale_bits;std::memcpy(&scale_bits,&selected.selection->scale,4);
        expect(scale_bits==expected_scales[phase],"phase byte selects catalog stride and exact finite scale, including negative and signed zero");
        auto obs=initial_timers(*selected.selection,UINT32_MAX);const auto prepared=awl::prepare_world_map_player_initial_animation(inputs,query,obs);
        expect(prepared.status==S::Prepared && prepared.feature && prepared.feature->index_8==UINT32_MAX && prepared.feature->table_c==0 &&
            prepared.feature->first_14.resource_0==700+phase && prepared.feature->second_24.resource_0==800+phase &&
            prepared.feature->first_14.clock_4==UINT32_MAX && prepared.feature->second_24.clock_4==UINT32_MAX &&
            prepared.feature->first_14.value_8==0 && prepared.feature->second_24.value_8==0 &&
            prepared.feature->first_14.rate_c==1 && prepared.feature->second_24.rate_c==1,"both initial timers have independently known resource/clock/zero/one reset fields");}
    query.phase=6;expect(inputs->select(query).status==S::RequiresPhase,"unsupported phase cannot index static initial rows");query.phase=0;
    query.model_slot=1;expect(inputs->select(query).status==S::UnsupportedModelSlot,"other actor model slots remain explicit stops");query.model_slot=0;
    query.alternate=0;expect(inputs->select(query).status==S::UnsupportedAlternate,"alternate 286D8 descriptor/model branch remains unsupported");query.alternate=UINT32_MAX;
    expect(awl::prepare_world_map_player_initial_animation({},query,{}).status==S::RequiresInputs,"missing initial provider has no invented feature snapshot");
    for(size_t size=0;size<bytes.size();++size)expect(awl::decode_world_map_player_initial_animation_inputs(bytes.data(),size,&inputs)==S::UnsupportedLayout && inputs.get()==previous,"every truncated initial DOL prefix preserves previous provider");
    for(auto change:std::vector<std::pair<size_t,uint32_t>>{{0x108,0},{0x114,0},{0x114,0x81000001},{0x114,0x81000028},
        {0x160,0},{0x174,0x81000129},{0x180,0x39},{0x180,UINT32_MAX},{0x184,0x23},{0x184,0x80000024},
        {4,0x100},{0x4C,0x80249230},{0x60,0xFFFFFFF0},{0x90+6*4,49},{0x90+7*4,5},{0x90+8*4,124},
        {0x1BC,0x7F800000},{0x234,0x7FC01234}}){auto malformed=bytes;word(malformed,change.first,change.second);
        expect(awl::decode_world_map_player_initial_animation_inputs(malformed.data(),malformed.size(),&inputs)==S::UnsupportedLayout && inputs.get()==previous,"malformed reached tables/sections/pointers and unwritten timer types reject atomically");}
    auto outside=bytes;outside[0x1B5]=255;
    expect(awl::decode_world_map_player_initial_animation_inputs(outside.data(),outside.size(),&inputs)==S::UnsupportedLayout && inputs.get()==previous,"out-of-extent phase scale index preserves old provider");
    auto classified=bytes;classified[0x10C]=1;
    std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> type_one;
    expect(awl::decode_world_map_player_initial_animation_inputs(classified.data(),classified.size(),&type_one)==S::Decoded &&
        type_one->select(query).selection->descriptor_index==4 && type_one->select(query).selection->descriptor.word_8==903,
        "constructor selector zero still honors the exact empty-item type classification");type_one.reset();
    expect(awl::decode_world_map_player_initial_animation_inputs(nullptr,bytes.size(),&inputs)==S::InvalidInput &&
        awl::decode_world_map_player_initial_animation_inputs(bytes.data(),bytes.size(),nullptr)==S::InvalidInput,"null initial decoder arguments reject");
    Fixture fixture;
    expect(awl::load_world_map_player_initial_animation_inputs(&inputs)==S::ReadFailure && inputs.get()==previous,"unverified provider cannot skip missing mounted DOL read");
    std::filesystem::create_directory(fixture.root/"sys");{
        std::ofstream dol(fixture.root/"sys"/"main.dol",std::ios::binary);dol.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        if(!dol)throw std::runtime_error("Initial DOL fixture write failed");}
    expect(awl::load_world_map_player_initial_animation_inputs(&inputs)==S::WrongDol && inputs.get()==previous,"well-formed synthetic DOL cannot pass target SHA1 gate");
    expect(awl::load_world_map_player_initial_animation_inputs(nullptr)==S::InvalidInput,"null initial loader output rejects");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<8;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::decode_world_map_player_initial_animation_inputs(bytes.data(),bytes.size(),&inputs);allocation_probe::enabled=false;
        if(status==S::Decoded){reached=true;break;}++rejected;
        expect(status==S::AllocationFailure && inputs.get()==previous && allocation_probe::live==live,"initial provider object/control allocation failures preserve ownership and free staging");}
    expect(reached && rejected==2,"initial provider decode sweep covers object and control block");
#endif
    auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    fixture.write("boy_0.arc",archive(payloads));fixture.write("boy_0.anm.arc",archive({owner_frame_clip(),owner_frame_clip(4)}));
    fixture.write("boy_0_subanm.arc",archive({owner_frame_clip()}));fixture.write("boy_0_subact.arc",archive({setup_model()}));
    std::shared_ptr<const Assets> models;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_model_assets(0,&models)==Status::Loaded && awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"initial holder model/bank fixtures load");if(!models || !animations)return;
    const auto selected=inputs->select(query);auto obs=initial_timers(*selected.selection,17);std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
    expect(awl::construct_world_map_player_initial_animation_holder(models,animations,inputs,query,obs,&holder).status==H::ConstructedCpuHolder && holder,"constructor initial inputs publish nonnull complete feature and owned CPU holder");if(!holder)return;
    awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(3);frame.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    expect(holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"initial holder reaches CPU frame with retained descriptor/feature");
    const auto before=holder_snapshot(*holder);const auto* old=holder.get();auto preserved=[&](){return holder.get()==old && holder_snapshot(*holder)==before;};
    expect(holder->state().feature_source_10==0u && holder->state().feature_flag_34==0u && holder->state().feature_38 &&
        holder->state().feature_38->first_14.resource_0==700 && holder->state().feature_38->second_24.resource_0==800 &&
        holder->initial_inputs()==inputs,"actual constructor source/flag and both timer writes retain immutable provider");
    auto construct=[&](const awl::WorldMapAnimationInitializerObservations& evidence){return awl::construct_world_map_player_initial_animation_holder(models,animations,inputs,query,evidence,&holder);};
    auto failed=construct({});expect(failed.status==H::InitialInputsIncomplete && failed.initial.status==S::RequiresTimerTable && !failed.initial.feature && preserved(),"missing first timer owner stops before holder replacement or exposing unknown timers");
    auto missing=obs;missing.rows.clear();failed=construct(missing);
    expect(failed.initial.status==S::RequiresTimerRow && failed.initial.required_row && failed.initial.required_row->table_identity==900 && !failed.initial.feature && preserved(),"first keyed timer row is reported");
    missing=obs;missing.clock.reset();failed=construct(missing);expect(failed.initial.status==S::RequiresClock && !failed.initial.feature && preserved(),"constructor timers require a real supplied clock");
    missing=obs;missing.fallback_table_24.reset();failed=construct(missing);expect(failed.initial.status==S::RequiresTimerTable && !failed.initial.feature && preserved(),"late missing second timer table rolls back first reset");
    missing=obs;missing.rows.pop_back();failed=construct(missing);expect(failed.initial.status==S::RequiresTimerRow && failed.initial.required_row && failed.initial.required_row->table_identity==901 && !failed.initial.feature && preserved(),"late missing second timer row preserves whole prior holder/frame");
    missing=obs;missing.rows.push_back(missing.rows.front());failed=construct(missing);expect(failed.status==H::InvalidInput && !failed.initial.feature && preserved(),"duplicate reached timer observations reject atomically");
    expect(awl::construct_world_map_player_initial_animation_holder({},animations,inputs,query,obs,&holder).status==H::RequiresModelAssets && preserved(),"missing primary assets cannot replace initial holder");
    query.phase=3;failed=construct(initial_timers(*inputs->select(query).selection,17));expect(failed.status==H::PhaseMismatch && preserved(),"phase keyed static types cannot bind a different primary asset variant");query.phase=0;
    expect(awl::construct_world_map_player_initial_animation_holder(models,animations,inputs,query,obs,nullptr).status==H::InvalidInput,"null initial holder output rejects");
    auto secondary=bytes;word(secondary,0x134,63u|(255u<<19));std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> blocked;
    expect(awl::decode_world_map_player_initial_animation_inputs(secondary.data(),secondary.size(),&blocked)==S::Decoded,"synthetic secondary initial descriptor decodes as opaque first record");
    failed=awl::construct_world_map_player_initial_animation_holder(models,animations,blocked,query,obs,&holder);
    expect(failed.status==H::HolderIncomplete && failed.holder && failed.holder->initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresSecondarySetup && preserved(),"late DB28 secondary dependency preserves complete previous holder after prepared feature resets");blocked.reset();
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto construction_live=allocation_probe::live;size_t rejected_construction=0;bool constructed=false;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto result=construct(obs);allocation_probe::enabled=false;
        if(result.status==H::ConstructedCpuHolder){constructed=true;break;}++rejected_construction;
        expect(result.status==H::AllocationFailure && preserved() && allocation_probe::live==construction_live,"initial input/feature/holder construction allocation failure frees staging and preserves frame, both channels and providers");}
    expect(constructed && rejected_construction>=224,"initial constructor sweep reaches every holder staging allocation");std::cout<<"PLAYER_INITIAL_HOLDER_ALLOCATION_FAILURES "<<rejected_construction<<'\n';
#endif
    const auto lifetime=std::weak_ptr<const awl::WorldMapPlayerInitialAnimationInputs>(inputs);inputs.reset();
    expect(!lifetime.expired() && holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"initial provider and feature survive source handle release");holder.reset();expect(lifetime.expired(),"initial provider is released with its holder");
}
void local_initial_holder(const char* disc) {
    using S=awl::WorldMapPlayerInitialAnimationStatus;using H=awl::WorldMapPlayerInitialHolderStatus;
    expect(awl::filesystem_mount("/",disc),"local initial constructor disc mounts");
    std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> inputs;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_initial_animation_inputs(&inputs)==S::Loaded && inputs && inputs->target_verified(),"local initial static inputs verify exact DOL bytes");
    expect(awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"local initial bank group loads");if(!inputs || !animations)return;
    // Phase, runtime table identities/resources and clock are supplied here.
    // These invented timer observations are not decoded original table owners.
    uint64_t digest=14695981039346656037ull,frame_digest=digest,geometry_digest=digest;size_t checked=0;
    const auto real=[&](float v){uint32_t w;std::memcpy(&w,&v,4);hash(digest,w);};
    for(uint32_t phase=0;phase<6;++phase){awl::WorldMapPlayerInitialAnimationQuery query;query.phase=phase;const auto selected=inputs->select(query);if(!selected.selection)return;
        for(uint32_t clock=0;clock<64;++clock){const auto obs=initial_timers(*selected.selection,clock);const auto prepared=awl::prepare_world_map_player_initial_animation(inputs,query,obs);
            expect(prepared.status==S::Prepared && prepared.feature,"actual initial descriptor/type rows prepare with supplied timer evidence");if(!prepared.feature)return;
            hash(digest,phase);hash(digest,clock);const auto& f=*prepared.feature;
            for(uint32_t v:{f.type_0,f.type_4,f.index_8,uint32_t(f.table_c),selected.selection->feature_source_10})hash(digest,v);
            for(const auto* t:{&f.first_14,&f.second_24}){hash(digest,uint32_t(t->resource_0));hash(digest,t->clock_4);real(t->value_8);real(t->rate_c);}hash(digest,selected.selection->feature_flag_34);}
        hash(digest,phase);hash(digest,uint32_t(selected.selection->descriptor.identity));for(uint32_t v:{selected.selection->descriptor.word_0,selected.selection->descriptor.word_4,selected.selection->descriptor.word_8})hash(digest,v);
        const auto missing=awl::prepare_world_map_player_initial_animation(inputs,query,{});
        expect(missing.status==S::RequiresTimerTable && !missing.feature,"actual initial feature cannot use a null optional shortcut");
        std::shared_ptr<const Assets> models;std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        expect(awl::load_world_map_player_model_assets(phase,&models)==Status::Loaded,"local initial model phase loads");const auto obs=initial_timers(*selected.selection,17);
        expect(awl::construct_world_map_player_initial_animation_holder(models,animations,inputs,query,obs,&holder).status==H::ConstructedCpuHolder && holder,"actual static initial inputs and supplied runtime timer evidence reach CPU holder");if(!holder)return;
        expect(holder->state().feature_38 && holder->state().feature_source_10==0u && holder->state().feature_flag_34==0u &&
            holder->state().feature_38->first_14.resource_0==700+phase && holder->state().feature_38->second_24.resource_0==800+phase &&
            !holder->state().deadline_10 && !holder->state().count_18 && !holder->state().completed_count_1c,"actual loop descriptor retains unknown counters after complete nonnull feature reset");
        models.reset();if(phase==5){animations.reset();inputs.reset();}
        awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(55);frame.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
        expect(holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && holder->primary().cpu().frame()->skin_executed,"initial holder retains providers through complete 55-node CPU frame");if(!holder->primary().cpu().frame())return;
        hash(frame_digest,phase);hash_frame(frame_digest,*holder->primary().cpu().frame());hash(geometry_digest,phase);hash_geometry(geometry_digest,holder->primary().geometry());++checked;
    }
    expect(digest==0x9729d29f9b58af5eull,"384 constructor feature resets and six descriptor selections match actual mapped instructions with supplied phase/timer tables/clock");
    expect(checked==6 && frame_digest==0xcc652d4bb4e9690aull && geometry_digest==0xf5968658e37d53caull,"initial constructor input wiring preserves prior six-phase CPU frame and geometry fingerprints");
    std::cout<<"LOCAL_PLAYER_INITIAL_HOLDER "<<checked<<" phases / static inputs and nonnull feature / 384 instruction comparisons "<<std::hex<<digest<<std::dec<<"; supplied runtime timer tables/clock, actor startup and GPU pending\n";
}
std::vector<uint8_t> timer_table(uint32_t rows=6,uint32_t columns=1) {
    // Invented two-level TAM metadata with reversed physical resources and
    // opaque 16-byte payloads. No animation body layout is asserted.
    const uint32_t arrays=8+rows*8,data=arrays+rows*columns*4,total=rows*columns;
    std::vector<uint8_t> b(data+total*16);word(b,0,0xF4E1EEED);word(b,4,rows);
    for(uint32_t row=0;row<rows;++row){word(b,8+row*8,columns);word(b,12+row*8,arrays+row*columns*4);}
    for(uint32_t i=0;i<total;++i){const auto offset=data+(total-1-i)*16;word(b,arrays+i*4,offset);
        std::memset(b.data()+offset,int(i+1),16);}return b;
}
uint64_t timer_binding_snapshot(const awl::WorldMapPlayerTimerBinding& binding) {
    uint64_t h=14695981039346656037ull;const auto key=[&](uint64_t v){hash(h,uint32_t(v>>32));hash(h,uint32_t(v));};
    key(reinterpret_cast<uintptr_t>(binding.assets.get()));const auto& obs=binding.observations;
    hash(h,obs.clock.has_value());if(obs.clock)hash(h,*obs.clock);
    for(const auto* table:{&obs.fallback_table_38,&obs.fallback_table_24}){hash(h,table->has_value());if(*table)key(**table);}
    hash(h,uint32_t(obs.rows.size()));for(const auto& row:obs.rows){key(row.table_identity);hash(h,row.index);hash(h,row.column);key(row.resource_identity);}return h;
}
void timer_asset_checks() {
    using S=awl::WorldMapPlayerTimerAssetsStatus;using B=awl::WorldMapPlayerTimerBank;using H=awl::WorldMapPlayerTimedInitialHolderStatus;
    auto eyes=timer_table(),mouth=timer_table();std::shared_ptr<const awl::WorldMapPlayerTimerAssets> assets;
    expect(awl::decode_world_map_player_timer_assets(eyes,mouth,&assets)==S::Decoded && assets,"two invented raw timer tables decode atomically");if(!assets)return;
    auto* old=assets.get();expect(assets->row_count(B::Eyes)==6 && assets->row_count(B::Mouth)==6 && assets->table_identity(B::Eyes)!=assets->table_identity(B::Mouth),"timer owners have independent stable table keys");
    for(B bank:{B::Eyes,B::Mouth})for(uint32_t row=0;row<6;++row){awl::WorldMapPlayerTimerResourceView view;
        expect(assets->column_count(bank,row)==1 && assets->resource(bank,row,0,&view) && view.offset==80+(5-row)*16 && view.size==16 && view.identity==reinterpret_cast<uintptr_t>(view.data) && view.data[0]==row+1,
            "serialized row order reaches independently specified reversed physical opaque payloads");}
    awl::WorldMapPlayerTimerResourceView view;expect(assets->resource(B::Eyes,0,0,&view),"prior resource view obtains");const auto before_view=view;
    for(auto [bank,row,column]:std::vector<std::tuple<B,uint32_t,uint32_t>>{{B::Eyes,6,0},{B::Mouth,0,1},{B(9),0,0}})
        expect(!assets->resource(bank,row,column,&view) && view.data==before_view.data && view.offset==before_view.offset && view.size==before_view.size && view.identity==before_view.identity,"invalid resource lookup preserves prior view");
    expect(!assets->resource(B::Eyes,0,0,nullptr) && assets->row_count(B(9))==0 && assets->column_count(B(9),0)==0 && assets->table_identity(B(9))==0,"invalid bank/output never indexes outside owned tables");
    auto multi=timer_table(2,2);std::shared_ptr<const awl::WorldMapPlayerTimerAssets> several;
    expect(awl::decode_world_map_player_timer_assets(multi,multi,&several)==S::Decoded && several->resource(B::Eyes,1,1,&view) && view.offset==40 && view.size==16 && view.data[0]==4,"two-level multi-column indexing follows row stride eight and pointer stride four");
    auto alias=multi;word(alias,28,88);
    expect(awl::decode_world_map_player_timer_assets(alias,multi,&several)==S::Decoded,"shared opaque resource offsets are supported");
    awl::WorldMapPlayerTimerResourceView a,b;expect(several->resource(B::Eyes,0,0,&a) && several->resource(B::Eyes,0,1,&b) && a.identity==b.identity && a.offset==88 && a.size==16 && b.size==16,"aliased rows preserve same owned resource identity/span");several.reset();
    // A TAM declares no body lengths. Prefixes before the final resource's
    // first four bytes reject; a shorter last opaque slice is permitted.
    for(size_t size=0;size<92;++size){auto prefix=multi;prefix.resize(size);
        expect(awl::decode_world_map_player_timer_assets(prefix,mouth,&assets)==S::UnsupportedLayout && assets.get()==old,"truncated metadata or reached resource pointer preserves complete prior table pair");}
    auto short_body=multi;short_body.resize(92);
    expect(awl::decode_world_map_player_timer_assets(short_body,multi,&several)==S::Decoded && several->resource(B::Eyes,0,0,&view) && view.size==4,"opaque final slice does not invent a complete texture-animation body");several.reset();
    for(auto change:std::vector<std::pair<size_t,uint32_t>>{{0,0},{0,UINT32_MAX},{4,0},{4,UINT32_MAX},{8,0},{8,UINT32_MAX},{12,0},{12,25},{12,104},
        {20,24},{24,0},{24,41},{24,36},{24,104},{16,20}}){auto bad=multi;word(bad,change.first,change.second);
        expect(awl::decode_world_map_player_timer_assets(bad,mouth,&assets)==S::UnsupportedLayout && assets.get()==old,"unsupported marker/row/array/resource bounds and overlapping metadata reject atomically");}
    auto bad=mouth;word(bad,0,0);expect(awl::decode_world_map_player_timer_assets(eyes,bad,&assets)==S::UnsupportedLayout && assets.get()==old,"late mouth parse rejection cannot publish a new eyes owner");
    expect(awl::decode_world_map_player_timer_assets(eyes,mouth,nullptr)==S::InvalidInput,"null pair decode output rejects");
    eyes.back()=0xEE;expect(before_view.data[15]==1,"decoded timer resources retain private immutable bytes after input mutation");eyes=timer_table();
    awl::WorldMapPlayerTimerBinding binding;auto bound=awl::bind_world_map_player_timer_assets(assets,0x3A,0x24,17,&binding);
    expect(bound.status==S::Bound && binding.assets==assets && binding.observations.rows.size()==2,"owned timer binding publishes both keyed rows and raw clock");
    const auto prior=timer_binding_snapshot(binding);auto preserved=[&](){return timer_binding_snapshot(binding)==prior;};
    expect(awl::bind_world_map_player_timer_assets({},0x3A,0x24,17,&binding).status==S::RequiresAssets && preserved(),"missing assets preserve old timer binding");
    expect(awl::bind_world_map_player_timer_assets(assets,0x39,0x24,17,&binding).status==S::InvalidInput && preserved(),"below-threshold constructor types cannot invent preserved timers");
    expect(awl::bind_world_map_player_timer_assets(assets,0x3A,UINT32_MAX,17,&binding).status==S::InvalidInput && preserved(),"negative reached timer type rejects");
    bound=awl::bind_world_map_player_timer_assets(assets,0x3A+6,0x24,std::nullopt,&binding);
    expect(bound.status==S::RequiresRow && bound.bank==B::Eyes && bound.required_row==6u && preserved(),"first row lookup precedes missing clock");
    bound=awl::bind_world_map_player_timer_assets(assets,0x3A,0x24+6,std::nullopt,&binding);
    expect(bound.status==S::RequiresClock && bound.bank==B::Eyes && preserved(),"first timer clock precedes unavailable second row");
    bound=awl::bind_world_map_player_timer_assets(assets,0x3A,0x24+6,17,&binding);
    expect(bound.status==S::RequiresRow && bound.bank==B::Mouth && bound.required_row==6u && preserved(),"late second row dependency preserves both previous timers");
    expect(awl::bind_world_map_player_timer_assets(assets,0x3A,0x24,17,nullptr).status==S::InvalidInput,"null timer binding output rejects");
    expect(awl::bind_world_map_player_timer_assets(binding.assets,0x3A,0x24,17,&binding).status==S::Bound && preserved(),"owner/output alias retains live bytes before publication");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;allocation_probe::remaining=0;allocation_probe::enabled=true;
    const auto no_storage=awl::bind_world_map_player_timer_assets(assets,0x3A,0x24,17,&binding);allocation_probe::enabled=false;
    expect(no_storage.status==S::AllocationFailure && preserved() && allocation_probe::live==live,"row snapshot allocation failure preserves previous retained binding");
    size_t rejected=0;bool decoded=false;const auto decode_live=allocation_probe::live;
    for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto result=awl::decode_world_map_player_timer_assets(eyes,mouth,&assets);allocation_probe::enabled=false;
        if(result==S::Decoded){decoded=true;break;}++rejected;
        expect(result==S::AllocationFailure && assets.get()==old && allocation_probe::live==decode_live,"every pair parser/owner allocation failure frees staging and preserves old eyes/mouth");}
    expect(decoded && rejected>10,"timer table decode sweep reaches both byte/row/resource owners");std::cout<<"PLAYER_TIMER_ASSET_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
    binding={};Fixture fixture;std::shared_ptr<const awl::WorldMapPlayerTimerAssets> loaded;
    expect(awl::load_world_map_player_timer_assets(&loaded)==S::ReadFailure && !loaded,"missing first timer file cannot publish");fixture.write("char_com_eye.tam",eyes);
    expect(awl::load_world_map_player_timer_assets(&loaded)==S::ReadFailure && !loaded,"missing second timer file releases first staging");fixture.write("char_com_mouth.tam",bad);
    expect(awl::load_world_map_player_timer_assets(&loaded)==S::UnsupportedLayout && !loaded,"late invalid second file releases staged pair");fixture.write("char_com_mouth.tam",mouth);
    expect(awl::load_world_map_player_timer_assets(&loaded)==S::Loaded && loaded,"actual logical timer paths load in original player order");if(!loaded)return;
    auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    fixture.write("boy_0.arc",archive(payloads));fixture.write("boy_0.anm.arc",archive({owner_frame_clip(),owner_frame_clip(4)}));
    fixture.write("boy_0_subanm.arc",archive({owner_frame_clip()}));fixture.write("boy_0_subact.arc",archive({setup_model()}));
    std::shared_ptr<const Assets> models;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_model_assets(0,&models)==Status::Loaded && awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"timer holder synthetic model/animation inputs load");
    auto dol=initial_input_dol();std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> inputs;
    expect(awl::decode_world_map_player_initial_animation_inputs(dol.data(),dol.size(),&inputs)==awl::WorldMapPlayerInitialAnimationStatus::Decoded,"timer holder initial static fixture decodes");if(!inputs || !models || !animations)return;
    awl::WorldMapPlayerInitialAnimationQuery query;std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
    auto construct=[&](const std::optional<uint32_t>& clock){return awl::construct_world_map_player_initial_holder_with_timers(models,animations,inputs,loaded,query,clock,&holder);};
    expect(construct(17).status==H::ConstructedCpuHolder && holder,"owned actual-path timer tables compose complete initial holder");if(!holder)return;
    awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(3);frame.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    expect(holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"owned timer holder evaluates CPU frame");
    const auto before=holder_snapshot(*holder);const auto* old_holder=holder.get();auto holder_preserved=[&](){return holder.get()==old_holder && holder_snapshot(*holder)==before;};
    expect(loaded->resource(B::Eyes,0,0,&a) && loaded->resource(B::Mouth,0,0,&b),"loaded timer resources resolve before ownership transfer");
    expect(holder->timer_assets()==loaded && holder->state().feature_38->first_14.resource_0==a.identity && holder->state().feature_38->second_24.resource_0==b.identity,"complete holder timers reference authoritative retained bytes");
    const auto missing=construct(std::nullopt);expect(missing.status==H::TimerInputsIncomplete && missing.timer.status==S::RequiresClock && holder_preserved(),"raw clock remains explicit and missing clock preserves whole holder/frame");
    query.phase=6;expect(construct(17).status==H::InitialInputsIncomplete && holder_preserved(),"unsupported static phase stops before timer publication");query.phase=0;
    auto secondary=dol;word(secondary,0x134,63u|(255u<<19));std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> blocked;
    expect(awl::decode_world_map_player_initial_animation_inputs(secondary.data(),secondary.size(),&blocked)==awl::WorldMapPlayerInitialAnimationStatus::Decoded,"opaque secondary descriptor fixture decodes");
    const auto late=awl::construct_world_map_player_initial_holder_with_timers(models,animations,blocked,loaded,query,17,&holder);
    expect(late.status==H::HolderIncomplete && late.holder.holder && late.holder.holder->initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresSecondarySetup && holder_preserved(),"late animation dependency preserves complete holder after owned timer binding");
    auto clock=std::make_shared<awl::GameClock>();std::unique_ptr<awl::WorldMapPlayerAnimationHolder> clock_holder;
    const auto with_clock=[&](const std::shared_ptr<awl::GameClock>& provider,const auto& initial){
        return awl::construct_world_map_player_initial_model_with_clock(models,animations,initial,loaded,query,provider,&clock_holder);};
    expect(with_clock(clock,inputs).status==H::ConstructedCpuHolder && clock_holder,"fresh owned game clock composes real-path timer holder");if(!clock_holder)return;
    expect(clock_holder->game_clock()==clock && clock_holder->state().feature_38->first_14.clock_4==0 && clock_holder->state().feature_38->second_24.clock_4==0 &&
        clock->state().loop_count==0,"fresh clock snapshot initializes both timers without advancing shared time");
    const auto& model_state=clock_holder->initial_model_state();awl::WorldMapPlayerModelAssetView private_tam;
    expect(model_state && model_state->model_slot_4==0 && model_state->alternate_8==UINT32_MAX && model_state->initialized_c==1 &&
        model_state->runtime_entry_address_120==0x802ED6C0 && model_state->private_texture_animation && models->resource(7,&private_tam) &&
        model_state->private_texture_animation->data==private_tam.data && model_state->private_texture_animation->size==1 &&
        clock_holder->state().feature_38->table_c==reinterpret_cast<uintptr_t>(private_tam.data),"model tail copies retained opaque private TAM, symbolic runtime entry and final wrapper fields");
    const auto& root=clock_holder->primary().cpu().root_pose();
    expect(root[0]==0x01000000 && root[1]==0x40000000 && root[2]==0x40000000 && root[3]==0x40000000 &&
        clock_holder->primary().model().core().byte_1c==1 && !clock_holder->primary().cpu().frame(),"initial scale is read from catalog and synchronized with model before any frame exists");
    awl::WorldMapPlayerPrimaryFrameInput initial_frame;initial_frame.node_post_transforms.resize(3);
    expect(clock_holder->advance(initial_frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced &&
        clock_holder->primary().cpu().frame()->root_matrix==awl::WorldMapModelMatrix{2,0,0,0,0,2,0,0,0,0,2,0} &&
        clock->state().raw_time==0 && clock->state().loop_count==0,"first CPU frame uses retained initial scale without supplied placement or clock advance");
    expect(!holder->initial_model_state() && holder->state().feature_38->table_c==0,"legacy holder-only construction does not claim model tail writes");
    clock->set_retrace_interval(2);expect(clock->advance() && clock->advance() && clock->state().raw_time==66 && clock->state().loop_count==2,"shared clock advances separately through two original-equivalent loops");
    expect(clock_holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && clock->state().raw_time==66 && clock->state().loop_count==2 &&
        clock_holder->state().feature_38->first_14.clock_4==0 && clock_holder->state().feature_38->second_24.clock_4==0,"CPU frame evaluation preserves shared clock and initial timer snapshots before TAM execution exists");
    const auto clock_before=holder_snapshot(*clock_holder);const auto* old_clock_holder=clock_holder.get();
    const auto clock_preserved=[&](){return clock_holder.get()==old_clock_holder && holder_snapshot(*clock_holder)==clock_before;};
    const auto no_clock=with_clock({},inputs);expect(no_clock.status==H::TimerInputsIncomplete && no_clock.timer.status==S::RequiresClock && clock_preserved(),"missing owned clock preserves prior holder and clock state");
    query.phase=6;expect(with_clock({},inputs).status==H::InitialInputsIncomplete && clock_preserved(),"static phase stop precedes missing owned clock");query.phase=0;
    const auto clock_late=with_clock(clock,blocked);expect(clock_late.status==H::HolderIncomplete && clock_late.holder.holder &&
        clock_late.holder.holder->initializer_status==awl::WorldMapAnimationInitializerStatus::RequiresSecondarySetup && clock_preserved(),"late animation dependency cannot publish or advance shared clock owner");blocked.reset();
    expect(awl::construct_world_map_player_initial_model_with_clock(models,animations,inputs,loaded,query,clock,nullptr).status==H::InvalidInput,"null initial model output rejects");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto construction_live=allocation_probe::live;size_t rejected_holder=0;bool constructed=false;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto result=with_clock(clock,inputs);allocation_probe::enabled=false;
        if(result.status==H::ConstructedCpuHolder){constructed=true;break;}++rejected_holder;
        expect(result.status==H::AllocationFailure && clock_preserved() && holder_preserved() && allocation_probe::live==construction_live,"all owned clock/timer binding/holder allocation failures preserve shared time, providers, both channels and prior CPU frame");}
    expect(constructed && rejected_holder>224,"owned timer construction sweep includes binding snapshot before existing holder allocations");std::cout<<"PLAYER_TIMED_HOLDER_ALLOCATION_FAILURES "<<rejected_holder<<'\n';
#endif
    expect(with_clock(clock_holder->game_clock(),inputs).status==H::ConstructedCpuHolder && clock_holder->game_clock()==clock &&
        clock_holder->state().feature_38->first_14.clock_4==66 && clock_holder->state().feature_38->second_24.clock_4==66,"clock provider/output alias retains ownership and selects current snapshot before replacing old holder");
    auto absent_payloads=payloads;absent_payloads.resize(6);fixture.write("boy_0.arc",archive(absent_payloads));std::shared_ptr<const Assets> absent_model;
    expect(awl::load_world_map_player_model_assets(0,&absent_model)==Status::Loaded,"model without optional private TAM loads");
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> absent_holder;
    expect(awl::construct_world_map_player_initial_model_with_clock(absent_model,animations,inputs,loaded,query,clock,&absent_holder).status==H::ConstructedCpuHolder &&
        absent_holder->initial_model_state()->initialized_c==1 && !absent_holder->initial_model_state()->private_texture_animation && absent_holder->state().feature_38->table_c==0,
        "absent private TAM follows verified null copy and still finishes CPU model initialization");absent_holder.reset();absent_model.reset();
    const auto clock_lifetime=std::weak_ptr<awl::GameClock>(clock);clock.reset();
    expect(!clock_lifetime.expired() && clock_holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && clock_holder->game_clock()->state().raw_time==66,"holder retains shared CPU clock after caller handle release");
    clock_holder.reset();expect(clock_lifetime.expired(),"shared clock releases after last retained holder");
    const auto lifetime=std::weak_ptr<const awl::WorldMapPlayerTimerAssets>(loaded);loaded.reset();
    expect(!lifetime.expired() && holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"last caller timer handle may drop while holder-owned timer resources remain live");
    awl::filesystem_shutdown();auto retained=holder->timer_assets();expect(awl::load_world_map_player_timer_assets(&retained)==S::Loaded && retained==holder->timer_assets(),"complete timer owner reuses without mounted file I/O");retained.reset();
    holder.reset();expect(lifetime.expired(),"timer resources release after the last holder/provider owner");
}
void local_timer_holder(const char* disc,bool owned_clock=false,bool initial_model=false) {
    using S=awl::WorldMapPlayerTimerAssetsStatus;using B=awl::WorldMapPlayerTimerBank;using H=awl::WorldMapPlayerTimedInitialHolderStatus;
    expect(awl::filesystem_mount("/",disc),"local timer assets disc mounts");std::shared_ptr<const awl::WorldMapPlayerTimerAssets> timers;
    std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> inputs;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_timer_assets(&timers)==S::Loaded,"actual eye/mouth timer tables load");
    expect(awl::load_world_map_player_initial_animation_inputs(&inputs)==awl::WorldMapPlayerInitialAnimationStatus::Loaded && inputs && inputs->target_verified(),"actual timer startup static inputs verify exact DOL");
    expect(awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"actual animation bank group loads");if(!timers || !inputs || !animations)return;
    uint64_t table_digest=14695981039346656037ull;
    for(B bank:{B::Eyes,B::Mouth}){hash(table_digest,uint32_t(bank));hash(table_digest,uint32_t(timers->row_count(bank)));
        for(uint32_t row=0;row<timers->row_count(bank);++row)for(uint32_t column=0;column<timers->column_count(bank,row);++column){awl::WorldMapPlayerTimerResourceView view;
            expect(timers->resource(bank,row,column,&view),"actual timer resource view resolves");hash(table_digest,row);hash(table_digest,column);hash(table_digest,view.offset);hash(table_digest,view.size);}}
    expect(timers->row_count(B::Eyes)==18 && timers->row_count(B::Mouth)==14 && table_digest==0x3B8C90FA1D03FFA2ull,"32 actual row/column/offset/span views agree with mapped 824C/82D4/82E8 lookup");
    uint64_t digest=14695981039346656037ull,frame_digest=digest,geometry_digest=digest;size_t checked=0;
    auto clock=owned_clock?std::make_shared<awl::GameClock>():std::shared_ptr<awl::GameClock>{};
    const auto clock_lifetime=std::weak_ptr<awl::GameClock>(clock);if(clock)clock->set_retrace_interval(2);
    const auto real=[&](float v){uint32_t w;std::memcpy(&w,&v,4);hash(digest,w);};
    for(uint32_t phase=0;phase<6;++phase){awl::WorldMapPlayerInitialAnimationQuery query;query.phase=phase;const auto selected=inputs->select(query);if(!selected.selection)return;
        awl::WorldMapPlayerTimerResourceView first,second;expect(timers->resource(B::Eyes,selected.selection->type_0-0x3A,0,&first) && timers->resource(B::Mouth,selected.selection->type_4-0x24,0,&second),"actual startup timer resources resolve");
        for(uint32_t raw_clock:{0u,1u,33u,999u,UINT32_MAX-5,UINT32_MAX}){awl::WorldMapPlayerTimerBinding binding;
            expect(awl::bind_world_map_player_timer_assets(timers,selected.selection->type_0,selected.selection->type_4,raw_clock,&binding).status==S::Bound,"actual timer owners bind exact reached rows");
            const auto prepared=awl::prepare_world_map_player_initial_animation(inputs,query,binding.observations);expect(prepared.status==awl::WorldMapPlayerInitialAnimationStatus::Prepared && prepared.feature,"actual timer resources prepare nonnull complete initial feature");if(!prepared.feature)return;
            const auto& f=*prepared.feature;expect(f.first_14.resource_0==first.identity && f.second_24.resource_0==second.identity,"actual feature reset uses authoritative owner resource keys");
            hash(digest,phase);hash(digest,raw_clock);for(uint32_t v:{f.type_0,f.type_4,f.index_8,uint32_t(f.table_c),selected.selection->feature_source_10})hash(digest,v);
            for(auto [timer,offset]:std::array<std::pair<const awl::WorldMapAnimationFeatureTimer*,uint32_t>,2>{{{&f.first_14,first.offset},{&f.second_24,second.offset}}}){hash(digest,offset);hash(digest,timer->clock_4);real(timer->value_8);real(timer->rate_c);}hash(digest,selected.selection->feature_flag_34);}
        std::shared_ptr<const Assets> models;std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        expect(awl::load_world_map_player_model_assets(phase,&models)==Status::Loaded,"actual timer holder primary phase loads");
        const auto result=initial_model?awl::construct_world_map_player_initial_model_with_clock(models,animations,inputs,timers,query,clock,&holder):
            owned_clock?awl::construct_world_map_player_initial_holder_with_clock(models,animations,inputs,timers,query,clock,&holder):
            awl::construct_world_map_player_initial_holder_with_timers(models,animations,inputs,timers,query,17,&holder);
        expect(result.status==H::ConstructedCpuHolder && holder,"actual static/TAM/bank/model inputs reach owned initial CPU holder");if(!holder)return;
        if(initial_model){awl::WorldMapPlayerModelAssetView private_tam;const auto& state=holder->initial_model_state();
            expect(state && state->initialized_c==1 && state->model_slot_4==0 && state->alternate_8==UINT32_MAX && state->runtime_entry_address_120==0x802ED6C0 &&
                state->private_texture_animation && models->resource(7,&private_tam) && private_tam.reference.offset==152064 && private_tam.size==2104 &&
                state->private_texture_animation->data==private_tam.data && holder->state().feature_38->table_c==reinterpret_cast<uintptr_t>(private_tam.data),
                "all six actual initial models retain reached private TAM and exact wrapper stores");
            expect(selected.selection->scale==1 && holder->primary().model().core().byte_1c==1 && holder->primary().cpu().root_pose()[0]==0x01000000 &&
                holder->primary().cpu().root_pose()[1]==0x3F800000 && holder->primary().cpu().root_pose()[2]==0x3F800000 && holder->primary().cpu().root_pose()[3]==0x3F800000,
                "actual phase catalog scale agrees with E47C mapped lookup");
            awl::WorldMapPlayerPrimaryFrameInput first_frame;first_frame.node_post_transforms.resize(55);
            expect(holder->advance(first_frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && holder->primary().cpu().frame()->skin_executed &&
                holder->primary().cpu().frame()->root_matrix==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0} && clock->state().raw_time==phase*3300u,
                "first actual 55-node CPU frame consumes initialized pose without caller placement or implicit clock advance");}
        if(owned_clock){expect(holder->game_clock()==clock && holder->state().feature_38->first_14.clock_4==phase*3300u && holder->state().feature_38->second_24.clock_4==phase*3300u,
                "actual startup binds both real resource timers to current shared clock snapshot");
            for(uint32_t tick=0;tick<100;++tick)expect(holder->game_clock()->advance(),"owned raw clock advances between actual CPU holder/frame checks");
            expect(clock->state().raw_time==(phase+1)*3300u && clock->state().loop_count==(phase+1)*100u,"shared two-retrace clock persists across successive phase constructions");}
        models.reset();if(phase==5){inputs.reset();timers.reset();animations.reset();clock.reset();}
        if(initial_model)expect(holder->initial_model_state()->private_texture_animation->data[0]==0xF4 &&
            holder->state().feature_38->table_c==reinterpret_cast<uintptr_t>(holder->initial_model_state()->private_texture_animation->data),
            "private TAM bytes and feature key remain live after caller model handle release");
        awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(55);frame.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
        expect(holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && holder->primary().cpu().frame()->skin_executed,"actual timer owner lifetimes survive full 55-node CPU frame publication");if(!holder->primary().cpu().frame())return;
        hash(frame_digest,phase);hash_frame(frame_digest,*holder->primary().cpu().frame());hash(geometry_digest,phase);hash_geometry(geometry_digest,holder->primary().geometry());++checked;
    }
    expect(digest==0x45896C1D4A78E995ull,"36 actual resource/feature resets match mapped constructor bodies with supplied phase/raw clock");
    expect(checked==6 && frame_digest==0xCC652D4BB4E9690Aull && geometry_digest==0xF5968658E37D53CAull,"owned actual timer resources preserve prior six-phase CPU frame and mesh fingerprints");
    if(owned_clock)expect(clock_lifetime.expired(),"last actual holder releases shared clock after all caller handles drop");
    std::cout<<(initial_model?"LOCAL_PLAYER_INITIAL_MODEL ":owned_clock?"LOCAL_PLAYER_CLOCK_HOLDER ":"LOCAL_PLAYER_TIMER_HOLDER ")<<checked<<" phases / 32 resource views / 36 actual resource comparisons; "
        <<(owned_clock?"600 owned clock updates; ":"raw clock supplied; ")<<"TAM execution and actor/GPU pending\n";
}
void hash_channels(uint64_t& h,const awl::WorldMapModelChannelControls& controls) {
    hash(h,controls.count);for(size_t i=0;i<controls.count;++i){const auto& c=controls.calls[i];
        for(uint32_t v:{uint32_t(c.channel),uint32_t(c.enabled),uint32_t(c.ambient_source),uint32_t(c.material_source),c.light_mask,uint32_t(c.diffuse),uint32_t(c.attenuation)})hash(h,v);}
}
void hash_draw_state(uint64_t& h,awl::WorldMapPlayerDrawStateStatus status,const awl::WorldMapPlayerFeatureDrawState& state) {
    hash(h,uint32_t(status));if(status==awl::WorldMapPlayerDrawStateStatus::SkippedFeature)return;
    hash(h,state.feature);hash(h,state.section);const auto floats=[&](const auto& values){for(float value:values){uint32_t raw;value=value==0?0:value;std::memcpy(&raw,&value,4);hash(h,raw);}};
    floats(state.feature_matrix);floats(state.model_view);hash(h,state.normal_matrix.has_value());if(state.normal_matrix)floats(*state.normal_matrix);
    hash(h,state.matrix_index);hash_channels(h,state.channels);hash(h,state.material_color.has_value());if(state.material_color)for(auto v:*state.material_color)hash(h,v);
}
void feature_draw_state_checks() {
    using D=awl::WorldMapPlayerDrawStateStatus;
    awl::WorldMapModelChannelControls channels;uint64_t channel_digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){const uint32_t normals[]{0,0x100,1,0x101};const uint8_t enables[]{0,1,255};
        const awl::WorldMapModelLightingSnapshot lights{enables[(seed/4)%3],0x80000000u|seed*17u,~seed,int32_t(seed%7)-2};
        expect(awl::prepare_world_map_model_channel_controls(normals[seed%4],uint8_t(seed%2),lights,&channels)==D::PreparedChannels,"complete channel controls select from supplied lighting snapshots");
        hash(channel_digest,seed);hash_channels(channel_digest,channels);}
    std::cout<<"PLAYER_CHANNEL_CONTROLS 512 digest "<<std::hex<<channel_digest<<std::dec<<'\n';
    expect(channel_digest==0xd780ba0691868a15ull,"complete 7BA4 call operands match independently executed DOL branches");
    const auto before_channels=channels;auto channels_preserved=[&](){uint64_t a=0,b=0;hash_channels(a,channels);hash_channels(b,before_channels);return a==b;};
    awl::WorldMapModelLightingSnapshot missing;
    expect(awl::prepare_world_map_model_channel_controls(1,0,missing,&channels)==D::RequiresLightingState && channels_preserved(),"unknown enable is not treated as disabled lighting");
    missing.enabled=uint8_t(1);expect(awl::prepare_world_map_model_channel_controls(1,0,missing,&channels)==D::RequiresLightingState && channels_preserved(),"lit color mask and alpha mode require observations");
    missing.color_mask=0;missing.alpha_mode=1;expect(awl::prepare_world_map_model_channel_controls(1,0,missing,&channels)==D::RequiresLightingState && channels_preserved(),"lit alpha mode needs its separate mask and preserves preceding color proposal");
    expect(awl::prepare_world_map_model_channel_controls(1,2,missing,&channels)==D::InvalidInput && channels_preserved(),"unsupported raster source rejects atomically");
    expect(awl::prepare_world_map_model_channel_controls(0,0,{},nullptr)==D::InvalidInput,"null channel output rejects");
    expect(awl::prepare_world_map_model_channel_controls(0x100,1,{},&channels)==D::PreparedChannels && channels.count==1 && channels.calls[0].channel==4 && channels.calls[0].material_source==1,
        "low-byte zero skips every global lighting field");
    missing.enabled=uint8_t(0);missing.color_mask.reset();missing.alpha_mode.reset();
    expect(awl::prepare_world_map_model_channel_controls(1,0,missing,&channels)==D::PreparedChannels,"disabled global lighting skips masks/mode");
    missing.enabled=uint8_t(1);missing.color_mask=0x12345678;missing.alpha_mode=-1;missing.alpha_mask.reset();
    expect(awl::prepare_world_map_model_channel_controls(1,0,missing,&channels)==D::PreparedChannels && channels.count==2 && channels.calls[1].enabled==0 && channels.calls[1].diffuse==2 && channels.calls[1].attenuation==1,
        "other signed alpha modes disable alpha without reading its mask or changing its diffuse/attenuation fields");
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);fixture.write("boy_0.arc",archive(payloads));
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerFrameOwner> owner;std::unique_ptr<awl::WorldMapPlayerGeometry> geometry;
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded && awl::prepare_world_map_player_frame_owner(assets,&owner).status==awl::WorldMapPlayerFrameOwnerStatus::PreparedCpuState,
        "feature draw CPU owner prepares");if(!owner)return;
    expect(awl::prepare_world_map_player_geometry(owner->work(),&geometry).status==awl::WorldMapPlayerGeometryStatus::PreparedGeometry,"feature draw geometry prepares");if(!geometry)return;
    awl::WorldMapPlayerFeatureDrawState state;awl::WorldMapPlayerFeatureDrawInput input;uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){awl::WorldMapPlayerOwnedFrameInput placement;placement.evaluate_nodes=false;placement.request_skin=false;placement.root_pose=explicit_pose(skin_palette(seed,1)[0]);
        expect(owner->advance(placement,{},{}).status==awl::WorldMapPlayerFrameStatus::Evaluated,"supplied feature placement publishes without animation/skin dependencies");
        input={};if(seed%17)input.feature=seed%4;input.view=skin_palette(seed+128,1)[0];input.array_override=0;
        input.raster=awl::WorldMapPlayerFeatureRasterSnapshot{uint8_t(seed%11?1:0),uint8_t(seed%5?0:2),std::array<uint8_t,4>{uint8_t(seed),uint8_t(seed>>1),uint8_t(255-seed),128}};
        input.lighting={uint8_t(seed%3?1:0),0x80000000u|seed*17u,~seed,int32_t(seed%7)-2};
        const auto result=awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,input,&state);
        expect(result==(input.feature && input.raster->enabled_3c?D::PreparedFeatureState:D::SkippedFeature),"feature gate, matrices, channel operands, known-null override and color prepare together");
        hash(digest,seed);hash_draw_state(digest,result,state);}
    std::cout<<"PLAYER_FEATURE_DRAW_STATE 512 digest "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0x70e9ae6b8767cf6full,"512 reached feature matrices/normal loads/channel controls/colors match independently executed 2CFC/7D6C/7BA4 instructions");
    awl::WorldMapPlayerOwnedFrameInput placement;placement.evaluate_nodes=false;placement.request_skin=false;
    placement.root_pose=explicit_pose({2,0,0,3,0,3,0,9,0,0,4,-2});expect(owner->advance(placement,{},{}).status==awl::WorldMapPlayerFrameStatus::Evaluated,"nonuniform feature placement publishes");
    input={};input.feature=0;input.view=awl::WorldMapModelMatrix{0,-1,0,5,1,0,0,7,0,0,1,11};input.lighting.enabled=uint8_t(0);input.array_override=0;
    expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,input,&state)==D::PreparedFeatureState &&
        state.model_view==awl::WorldMapModelMatrix{0,-3,0,-4,2,0,0,10,0,0,4,9} && state.normal_matrix==std::array<float,9>{0,-3,0,2,0,0,0,0,4} && state.section==0 && state.matrix_index==0 &&
        state.material_color==std::optional<std::array<uint8_t,4>>({16,69,165,255}),"view precedes feature placement and the original directly loads its 3x3 normal block, with retained registered material color");
    uint64_t before=0;hash_draw_state(before,D::PreparedFeatureState,state);const auto provider=state.assets;
    auto preserved=[&](){uint64_t h=0;hash_draw_state(h,D::PreparedFeatureState,state);return h==before && state.assets==provider;};
    expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,input,nullptr)==D::InvalidInput,"null feature draw output rejects");
    auto invalid=input;invalid.feature=99;expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::InvalidInput && preserved(),"missing retained feature rejects without state publication");
    invalid=input;invalid.view.reset();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::RequiresView && preserved(),"unknown view is not replaced by an identity camera");
    invalid=input;invalid.lighting.enabled.reset();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::RequiresLightingState && preserved(),"feature normals cannot infer global lighting enable");
    invalid=input;invalid.array_override.reset();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::RequiresArrayBinding && preserved(),"feature +34 zero cannot infer the distinct global array override");
    invalid.array_override=123;expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::RequiresArrayBinding && preserved(),"nonnull global buffer needs a separately justified binding");
    invalid=input;invalid.raster=awl::WorldMapPlayerFeatureRasterSnapshot{1,2,{}};expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::RequiresMaterialColor && preserved(),"unknown reached color override does not invent RGBA");
    invalid=input;(*invalid.view)[11]=std::numeric_limits<float>::quiet_NaN();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::UnsupportedNumerics && preserved(),"nonfinite view rejects complete state");
    (*invalid.view)[11]=std::numeric_limits<float>::denorm_min();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::UnsupportedNumerics && preserved(),"subnormal view rejects");
    invalid=input;(*invalid.view)[0]=std::numeric_limits<float>::max();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::UnsupportedNumerics && preserved(),"matrix arithmetic overflow rolls back preceding products");
    const int rounding=std::fegetround();if(std::fesetround(FE_DOWNWARD)==0){expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,input,&state)==D::UnsupportedNumerics && preserved(),"unsupported rounding preserves proposal");std::fesetround(rounding);}
    invalid={};invalid.feature=0;invalid.raster=awl::WorldMapPlayerFeatureRasterSnapshot{0,2,{}};invalid.view=awl::WorldMapModelMatrix{};(*invalid.view)[0]=std::numeric_limits<float>::quiet_NaN();invalid.array_override=999;
    expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::SkippedFeature && !state.assets && !state.normal_matrix && !state.channels.count,"disabled feature ignores unreached view/global/color observations");
    invalid.feature.reset();expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,invalid,&state)==D::SkippedFeature,"known null feature skips before all other fields");
    input.raster=awl::WorldMapPlayerFeatureRasterSnapshot{1,2,std::array<uint8_t,4>{1,2,3,4}};
    expect(awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,input,&state)==D::PreparedFeatureState && state.material_color==input.raster->color_3e && state.channels.calls[0].material_source==0,"feature material override selects register source and supplied RGBA");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    allocation_probe::remaining=0;allocation_probe::enabled=true;const auto no_allocation=awl::prepare_world_map_player_feature_draw_state(*owner,*geometry,input,&state);allocation_probe::enabled=false;
    expect(no_allocation==D::PreparedFeatureState,"prepared draw-state updates require no allocation");
#endif
    std::unique_ptr<awl::WorldMapPlayerFrameOwner> other;std::shared_ptr<const Assets> other_assets;
    expect(awl::load_world_map_player_model_assets(0,&other_assets)==Status::Loaded && awl::prepare_world_map_player_frame_owner(other_assets,&other).status==awl::WorldMapPlayerFrameOwnerStatus::PreparedCpuState,"distinct draw-state provider prepares");
    if(other)expect(awl::prepare_world_map_player_feature_draw_state(*other,*geometry,input,&state)==D::InvalidInput && state.assets==assets,"different owner cannot attach matrices to prior geometry");
}
void primary_frame_owner_checks() {
    using O=awl::WorldMapPlayerFrameOwnerStatus;using F=awl::WorldMapPlayerFrameStatus;using P=awl::WorldMapAnimationPoseStatus;
    Fixture fixture;auto payloads=files();payloads[1]=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerFrameOwner> owner;
    awl::WorldMapAnimationBank first,second;std::vector<const awl::WorldMapAnimationBank*> banks{&first,&second};size_t compared=0;
    auto prepare=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
            awl::prepare_world_map_player_frame_owner(assets,&owner).status==O::PreparedCpuState,"primary CPU frame owner retains checked providers and skin work");};
    for(uint32_t seed=0;seed<256;++seed){payloads[0]=setup_model();payloads[2]=execution_skin(seed);
        for(uint32_t i=0;i<3;++i)word(payloads[0],32+i*28+24,((seed+i)%3)<<24|((i==0?5u:2u)<<16));prepare();if(!owner)return;
        expect(!owner->frame() && owner->root_pose()[0]==0 && owner->vertex_output()==owner->work().initial_output(),"preparation fabricates no evaluated frame and retains initial vertex bytes");
        expect(first.parse(200,animation_frame_fixture(seed,0)) && second.parse(201,animation_frame_fixture(seed,1)),"primary owner comparison clips parse");
        awl::WorldMapPlayerFrame expected;expected.vertex_output=owner->work().initial_output();
        for(uint32_t pass=0;pass<2;++pass){const auto supplied=frame_input(seed+pass*256,3);awl::WorldMapPlayerAnimationFrameInput plain;
            plain.root_pose=supplied.root_pose;plain.evaluate_nodes=supplied.evaluate_nodes;plain.request_skin=supplied.request_skin;
            for(const auto& node:supplied.nodes)plain.node_post_transforms.push_back(node.post_transform);
            if(seed%8)plain.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};plain.playback.position_0=pass?4.25f:0.0f;
            plain.playback.link_14=seed%4?2:0;plain.playback.value_18=float(int(seed%9)-2)/8;
            auto linked=plain.playback;linked.clip_10=awl::WorldMapAnimationClipReference{201,0};linked.link_14=0;linked.position_0=pass?9.0f:1.25f;
            const std::vector<awl::WorldMapAnimationPlaybackRecord> complete{{2,linked}};
            const std::vector<awl::WorldMapAnimationPartialPlaybackRecord> partial{{2,awl::partial_world_map_animation_playback(linked)}};
            awl::WorldMapPlayerOwnedFrameInput input;input.root_pose=plain.root_pose;input.evaluate_nodes=plain.evaluate_nodes;input.request_skin=plain.request_skin;
            input.playback=awl::partial_world_map_animation_playback(plain.playback);input.playback.word_8.reset();input.playback.limit_c.reset();
            input.node_post_transforms=plain.node_post_transforms;
            expect(awl::evaluate_world_map_player_animation_frame(owner->work(),plain,complete,banks,expected.vertex_output,&expected).status==F::Evaluated &&
                owner->advance(input,partial,banks).status==F::Evaluated && owner->frame(),"persistent primary frames compose the previously verified supplied animation path");
            if(!owner->frame())return;uint64_t a=14695981039346656037ull,b=a;hash_frame(a,expected);hash_frame(b,*owner->frame());
            expect(a==b && owner->root_pose()==expected.root_pose_after && !input.playback.complete(),"all frame matrices/features/skin bytes match while unused supplied playback fields remain unknown");++compared;
        }}
    expect(compared==512,"all persistent complete/partial primary frame comparisons run");std::cout<<"PLAYER_PRIMARY_OWNED_FRAME_PARITY "<<compared<<'\n';
    payloads[0]=setup_model();payloads[2]=execution_skin(0);prepare();if(!owner)return;
    const awl::WorldMapModelMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0};
    expect(owner->feature_matrices()==std::vector<awl::WorldMapModelMatrix>{identity,identity,identity,identity},"all four persistent features start with independently traced constructor identity matrices");
    expect(first.parse(200,owner_frame_clip()) && second.parse(201,owner_frame_clip(4)),"persistent primary translation clips parse");
    awl::WorldMapPlayerOwnedFrameInput input;input.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    input.node_post_transforms.resize(3);input.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};
    expect(owner->advance(input,{},banks).status==F::Evaluated && owner->feature_matrices()==std::vector<awl::WorldMapModelMatrix>{
        {1,0,0,10,0,1,0,20,0,0,1,30},{1,0,0,11,0,1,0,22,0,0,1,33},
        {1,0,0,14,0,1,0,25,0,0,1,36},{1,0,0,17,0,1,0,28,0,0,1,39}},"reached root/node feature writes persist with independently known translations");
    const auto vertices=owner->vertex_output();input.request_skin=false;input.root_pose.reset();input.playback.clip_10=awl::WorldMapAnimationClipReference{201,0};
    expect(owner->advance(input,{},banks).status==F::Evaluated && owner->vertex_output()==vertices && !owner->frame()->skin_executed && owner->frame()->skin_palette.empty() &&
        owner->feature_matrices()[1]==awl::WorldMapModelMatrix{1,0,0,15,0,1,0,26,0,0,1,37},"skin-off retains preceding vertex bytes while updated node animation still publishes feature matrices");
    const auto feature=owner->feature_matrices()[1];
    input.evaluate_nodes=false;input.root_pose=explicit_pose({1,0,0,40,0,1,0,50,0,0,1,60});input.node_post_transforms.clear();
    input.playback.link_14=999;input.playback.value_18.reset();
    expect(owner->advance(input,{},{}).status==F::Evaluated && owner->vertex_output()==vertices && owner->feature_matrices()[1]==feature &&
        owner->feature_matrices()[0]==awl::WorldMapModelMatrix{1,0,0,40,0,1,0,50,0,0,1,60} && owner->frame()->node_matrices.empty(),
        "node-off retains previously written node feature and skin bytes, updates root feature and ignores unavailable animation inputs");
    input.root_pose.reset();
    expect(owner->advance(input,{},{}).status==F::Evaluated && owner->root_pose()==explicit_pose({1,0,0,40,0,1,0,50,0,0,1,60}),"absent placement reuses the retained root pose across frames");
    input.links.inherited={991,991,8u,{1,0,0,100,0,1,0,200,0,0,1,300}};
    expect(owner->advance(input,{},{}).status==F::Evaluated && owner->root_pose()[0]==0 && owner->feature_matrices()[0]==input.links.inherited.matrix,
        "inherited scale-only placement persists the original flag-byte mutation and root feature write");
    auto fingerprint=[&](){uint64_t h=14695981039346656037ull;hash_frame(h,*owner->frame());
        for(auto v:owner->root_pose())hash(h,v);for(const auto& m:owner->feature_matrices())for(float v:m){uint32_t raw;std::memcpy(&raw,&v,4);hash(h,raw);}return h;};
    const auto before=fingerprint();auto preserved=[&](){return fingerprint()==before;};
    input.links={};input.evaluate_nodes=true;input.request_skin=true;input.playback.clip_10=awl::WorldMapAnimationClipReference{200,0};
    input.node_post_transforms.resize(3);input.root_pose=explicit_pose({1,0,0,9,0,1,0,8,0,0,1,7});
    input.playback.link_14=2;awl::WorldMapAnimationPartialPlayback linked;linked.clip_10=awl::WorldMapAnimationClipReference{201,0};
    const std::vector<awl::WorldMapAnimationPartialPlaybackRecord> records{{2,linked}};
    auto result=owner->advance(input,records,banks);
    expect(result.status==F::RequiresAnimationSampling && result.failed_node==0u && result.sampling_status==P::RequiresBlendWeight && preserved(),"reached unknown blend weight cannot replace any persistent placement/feature/frame/vertex state");
    input.playback.link_14=0;auto malformed=owner_frame_clip();word(malformed,40+4,UINT32_MAX);expect(first.parse(200,malformed),"late persistent-frame malformed track remains opaque until reached");
    result=owner->advance(input,{},banks);
    expect(result.status==F::RequiresAnimationSampling && result.failed_node==2u && result.sampling_status==P::UnsupportedLayout && preserved(),"late node failure rolls back already prepared root/node features and skin inputs");
    input.links.children[0]={123,0,1u};
    expect(owner->advance(input,{},banks).status==F::RequiresHierarchy && preserved(),"attached child requires complete hierarchy publication before any primary state is accepted");input.links={};
    expect(first.parse(200,owner_frame_clip()),"persistent-frame valid clip restores");input.node_post_transforms.pop_back();
    expect(owner->advance(input,{},banks).status==F::InvalidInput && preserved(),"missing node observations preserve all persistent state");input.node_post_transforms.resize(3);
    const auto* previous=owner.get();
    const auto missing=awl::prepare_world_map_player_frame_owner({},&owner);
    expect(missing.status==O::RequiresSkinWork && missing.skin_work_status==awl::WorldMapPlayerSkinWorkStatus::RequiresAssets && owner.get()==previous && preserved(),"missing providers cannot replace a working CPU frame owner");
    expect(awl::prepare_world_map_player_frame_owner(assets,nullptr).status==O::InvalidInput,"null owner output rejects");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<32;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto step=owner->advance(input,{},banks);allocation_probe::enabled=false;
        if(step.status==F::Evaluated){reached=true;break;}++rejected;
        expect(step.status==F::AllocationFailure && preserved() && allocation_probe::live==live,"every primary publication allocation failure preserves root/feature/skin/frame state and releases staging");}
    expect(reached && rejected==6,"primary owner sweep includes feature-state publication after all five frame allocations");std::cout<<"PLAYER_PRIMARY_PUBLICATION_ALLOCATION_FAILURES "<<rejected<<'\n';
    const auto* old=owner.get();const auto live_after=allocation_probe::live;rejected=0;reached=false;
    for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto step=awl::prepare_world_map_player_frame_owner(assets,&owner);allocation_probe::enabled=false;
        if(step.status==O::PreparedCpuState){reached=true;break;}++rejected;
        expect(step.status==O::AllocationFailure && owner.get()==old && allocation_probe::live==live_after,"primary CPU owner replacement allocation failure preserves previous live owner and releases all providers/staging");}
    expect(reached && rejected>121,"primary owner preparation sweep reaches private feature state and owner publication");std::cout<<"PLAYER_PRIMARY_OWNER_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
    std::weak_ptr<const Assets> lifetime=assets;assets.reset();
    expect(!lifetime.expired() && owner->advance(input,{},banks).status==F::Evaluated,"persistent primary owner keeps selected providers live after caller release");
    owner.reset();expect(lifetime.expired(),"destroying the last CPU owner releases retained asset providers");
}
void skin_execution_checks(){
    using S=awl::WorldMapPlayerSkinExecutionStatus;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=draw_gpl();
    for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    std::shared_ptr<const Assets> assets;std::unique_ptr<awl::WorldMapPlayerSkinWork> work;
    auto prepare=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"execution providers load");
        expect(awl::prepare_world_map_player_skin_work(assets,&work)==awl::WorldMapPlayerSkinWorkStatus::PreparedWork,"execution jobs prepare");};
    uint64_t digest=14695981039346656037ull;std::vector<uint8_t> output;
    for(uint32_t seed=0;seed<256;++seed){payloads[2]=execution_skin(seed);prepare();if(!work)return;
        output.resize(64);for(size_t i=0;i<64;++i)output[i]=uint8_t(i*7+seed);
        for(uint32_t frame=0;frame<2;++frame){const auto palette=skin_palette(seed+frame*256,3);
            const auto status=awl::execute_world_map_player_skin_work(*work,palette,output,&output);
            expect(status==S::Executed,"three job classes execute two aliased frames");
            hash(digest,seed);hash(digest,frame);for(uint8_t byte:output)hash(digest,byte);}
    }
    expect(digest==0xd62abf643c3f21bbull,"512 complete outputs match original instruction execution with independent rational rounding");
    std::cout<<"PLAYER_SKIN_EXECUTION 512 digest "<<std::hex<<digest<<std::dec<<'\n';
    const auto prior=output;auto palette=skin_palette(255,3);
    expect(awl::execute_world_map_player_skin_work(*work,palette,output,nullptr)==S::InvalidInput,"null execution output rejects");
    auto truncated=prior;truncated.pop_back();
    expect(awl::execute_world_map_player_skin_work(*work,palette,truncated,&output)==S::InvalidInput && output==prior,"wrong output extent preserves result");
    auto short_palette=palette;short_palette.pop_back();
    expect(awl::execute_world_map_player_skin_work(*work,short_palette,output,&output)==S::InvalidInput && output==prior,"missing frame matrix preserves result");
    auto extra_palette=palette;extra_palette.push_back(palette[0]);
    expect(awl::execute_world_map_player_skin_work(*work,extra_palette,output,&output)==S::InvalidInput && output==prior,"extra frame matrix rejects incompatible palette");
    for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::denorm_min()}){
        palette[2][11]=invalid;
        expect(awl::execute_world_map_player_skin_work(*work,palette,output,&output)==S::UnsupportedNumerics && output==prior,"unsupported palette numerics preserve complete previous frame");}
    palette=skin_palette(255,3);palette[2][0]=std::numeric_limits<float>::max();
    expect(awl::execute_world_map_player_skin_work(*work,palette,output,&output)==S::UnsupportedNumerics && output==prior,"late arithmetic overflow rolls back earlier rigid writes");
    palette=skin_palette(255,3);const int rounding=std::fegetround();
    expect(std::fesetround(FE_DOWNWARD)==0,"test selects unsupported rounding");
    const auto rounding_status=awl::execute_world_map_player_skin_work(*work,palette,output,&output);
    expect(std::fesetround(rounding)==0,"test restores rounding");
    expect(rounding_status==S::UnsupportedNumerics && output==prior,"unsupported rounding preserves result and does not alter caller mode");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto before=allocation_probe::live;allocation_probe::remaining=0;allocation_probe::enabled=true;
    const auto allocation_status=awl::execute_world_map_player_skin_work(*work,palette,output,&output);allocation_probe::enabled=false;
    expect(allocation_status==S::AllocationFailure && output==prior && allocation_probe::live==before,"frame staging allocation failure preserves output without leaking");
#endif
    // Empty schedules retain caller-owned prior bytes instead of resetting GPL.
    payloads[2]=work_skin();word(payloads[2],0,0);word(payloads[2],4,0x00000d00);prepare();
    expect(awl::execute_world_map_player_skin_work(*work,palette,output,&output)==S::Executed && output==prior,"empty frame preserves all prior bytes");
    // Two half-weight contributions of raw 3 truncate separately: 0 -> 1 -> 2,
    // not a single summed 3. The next frame starts at 2 and reaches 4.
    payloads[2]=work_skin();word(payloads[2],0,0);word(payloads[2],24,0);word(payloads[2],32,0);word(payloads[2],448,0);
    for(size_t i=0;i<12;++i){payloads[2][416+i*2]=0;payloads[2][417+i*2]=3;}
    payloads[2][480]=128;payloads[2][481]=128;prepare();
    palette.assign(3,awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0});output.assign(64,0xa5);std::memset(output.data(),0,12);
    for(unsigned frame=1;frame<=2;++frame){expect(awl::execute_world_map_player_skin_work(*work,palette,output,&output)==S::Executed,"additive-only frame executes");
        bool ordered=true;for(size_t i=0;i<6;++i)ordered=ordered && output[i*2]==0 && output[i*2+1]==frame*2;
        for(size_t i=12;i<64;++i)ordered=ordered && output[i]==0xa5;
        expect(ordered,"duplicate scatter quantizes after each write and carries prior frame without touching unrelated bytes");}
    payloads[2]=work_skin();word(payloads[2],0,0x00010000);word(payloads[2],4,0x00000d00);
    for(size_t vertex=0;vertex<2;++vertex){const size_t at=324+vertex*12;word(payloads[2],at,0x1fff0000);word(payloads[2],at+4,0);word(payloads[2],at+8,0);}
    prepare();palette.assign(3,awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0});
    // Exact product is below the quantization boundary. A separate rounded
    // multiply/add would store 1 here instead of the fused result 0.
    uint32_t coefficient=0x3f800001,translation=0xbf7ff002;
    std::memcpy(&palette[1][0],&coefficient,4);std::memcpy(&palette[1][3],&translation,4);
    expect(awl::execute_world_map_player_skin_work(*work,palette,work->initial_output(),&output)==S::Executed && output[4]==0 && output[5]==0,
        "fused multiply-add preserves the integer-boundary cancellation result");
    palette[1][3]=5;palette[1][7]=-5;palette[1][11]=0.5f/8192;
    expect(awl::execute_world_map_player_skin_work(*work,palette,work->initial_output(),&output)==S::Executed &&
        output[4]==0x7f && output[5]==0xff && output[6]==0x80 && output[7]==0 && output[8]==0 && output[9]==0 && output[10]==0 && output[11]==0,
        "signed stores saturate both limits, truncate fractions and do not translate normals");
}
void skin_work_checks(){
    using S=awl::WorldMapPlayerSkinWorkStatus;using Owner=awl::WorldMapPlayerSkinWork;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=draw_gpl();for(size_t at:{size_t(64),size_t(512)}){word(payloads[1],at+24,32);word(payloads[1],at+288,38);}
    payloads[2]=work_skin();
    std::shared_ptr<const Assets> assets;std::unique_ptr<Owner> owner;
    auto load=[&](){assets.reset();fixture.write("boy_0.arc",archive(payloads));expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"skin work providers load");};
    expect(awl::prepare_world_map_player_skin_work(nullptr,nullptr)==S::InvalidInput,"null skin work output rejects first");
    expect(awl::prepare_world_map_player_skin_work(nullptr,&owner)==S::RequiresAssets && !owner,"skin work requires providers");
    load();expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::PreparedWork && owner,"three bounded skin job classes prepare CPU ownership");
    if(!owner)return;
    uint64_t fixture_digest=14695981039346656037ull;hash_skin_work(fixture_digest,9,*owner);
    expect(fixture_digest==0x39c4e772fa3e4cd3ull,"invented three-job schedule matches independently executed original control flow");
    expect(owner->jobs().size()==3 && owner->initial_output().size()==64 && (reinterpret_cast<uintptr_t>(owner->workspace())&31u)==0 &&
        Owner::banks==std::array<uint32_t,4>{0,4096,8192,12288} && Owner::triplets[1]==std::array<uint32_t,3>{8192,12288,14336},"aligned private workspace preserves original bank and alternate-triplet offsets");
    expect(owner->weight_quantization()==0x08040804 && owner->vertex_quantization()==0x0d070d07 &&
        owner->clear_before_accumulation() && owner->clear_before_accumulation()->offset==32 && owner->clear_before_accumulation()->size==32 &&
        owner->flush_indices()==std::vector<uint16_t>{0,2,2},"quantization, cache-line clear and duplicate flush indices prepare without execution");
    const auto& rigid=owner->jobs()[0];const auto& two=owner->jobs()[1];const auto& accumulated=owner->jobs()[2];
    expect(rigid.record_offset==64 && rigid.matrix_indices==std::vector<uint16_t>{1} && rigid.vertex_count==2 && rigid.prefix==4 &&
        rigid.source.offset==320 && rigid.source.size==32 && rigid.output.offset==0 && rigid.output.size==32 && !rigid.weights && !rigid.indices,"rigid job retains rounded block and matrix selection");
    expect(two.matrix_indices==std::vector<uint16_t>{0,2} && two.prefix==8 && two.source.offset==352 &&
        two.weights && two.weights->offset==384 && two.weights->size==32 && two.output.offset==32 && two.output.size==32,"two-matrix job retains weight transfer and output block");
    expect(accumulated.indices && accumulated.indices->offset==448 && accumulated.weights && accumulated.weights->offset==480 &&
        accumulated.scatter_indices==std::vector<uint16_t>{1,2} && accumulated.output.size==0,"additive job retains ordered scatter targets");
    const auto& selection=owner->drawing().setup().selection();const auto& target=*owner->drawing().setup().auxiliary().skin_target();
    expect(std::memcmp(owner->initial_output().data(),selection.gpl.data+target.data.offset-selection.gpl.reference.offset,target.size)==0,
        "private initial output exactly retains source bytes rather than prematurely clearing or skinning");
    expect(awl::prepare_world_map_player_skin_work(owner->drawing().setup().selection().assets,&owner)==S::PreparedWork,"retained asset input aliases atomic skin replacement safely");
    for(unsigned kind=0;kind<21;++kind){payloads[2]=work_skin();S wanted=S::UnsupportedLayout;
        if(kind==0)word(payloads[2],64+56,0x00010001); // count-1 loop with count one.
        if(kind==1)word(payloads[2],64+56,0x00030002); // palette index == count.
        if(kind==2)word(payloads[2],128+108,0x00000003);
        if(kind==3)word(payloads[2],244+64,0x00020000);
        if(kind==4)word(payloads[2],64+48,736); // Rounded block then padding crosses end when count increases.
        if(kind==4)word(payloads[2],64+56,0x00010003);
        if(kind==5)word(payloads[2],128+100,752); // Weight DMA needs a complete 32-byte block.
        if(kind==6)word(payloads[2],244+52,752);
        if(kind==7)word(payloads[2],448,0xffff0002);
        if(kind==8)word(payloads[2],512,0xffff0002);
        if(kind==9)word(payloads[2],24,UINT32_MAX);
        if(kind==10)word(payloads[2],64+52,32); // Two records fit only before rounded end.
        if(kind==10)word(payloads[2],64+56,0x00010003);
        if(kind==11)word(payloads[2],64+48,128); // Source overlaps writable matrix table.
        if(kind==12)word(payloads[2],8,32),wanted=S::RequiresDrawingPreparation; // Table overlaps the reached 36-byte root.
        if(kind==13)payloads[2][6]=32;
        if(kind==14)payloads[2][64+60]=3;
        if(kind==15)word(payloads[2],128+100,386);
        if(kind==16)word(payloads[2],244+60,482);
        if(kind==17)word(payloads[2],32,UINT32_MAX);
        if(kind==18)word(payloads[2],244+48,420);
        if(kind==19)word(payloads[2],64+56,0x00010157); // Transfer exceeds one 4-KB bank.
        if(kind==20)word(payloads[2],64+52,4); // Untranslated unaligned block output.
        load();const auto* prior=owner.get();const auto status=awl::prepare_world_map_player_skin_work(assets,&owner);
        expect(status==wanted && owner.get()==prior,"unsafe skin job payloads preserve prior complete ownership");
    }
    payloads[2]=work_skin();word(payloads[2],128,0x7fc00000);load();
    expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::PreparedWork,"initial SKN matrix scratch may be nonfinite because original overwrites it before use");
    awl::WorldMapModelMatrix frame{0,1,2,3,4,5,6,7,8,9,10,11},reordered{};
    expect(awl::reorder_world_map_player_skin_matrix(frame,&reordered) && reordered==awl::WorldMapModelMatrix{0,4,8,1,5,9,2,6,10,3,7,11},
        "frame matrix permutation matches raw paired-single helper");
    expect(awl::reorder_world_map_player_skin_matrix(frame,&frame) && frame==reordered,"matrix input/output alias reorders safely");
    uint32_t invalid_bits=0x7fc00000;std::memcpy(&frame[0],&invalid_bits,4);const auto unchanged=reordered;
    expect(!awl::reorder_world_map_player_skin_matrix(frame,&reordered) && reordered==unchanged && !awl::reorder_world_map_player_skin_matrix(unchanged,nullptr),
        "nonfinite frame matrices and null output reject without partial result");
    payloads[2]=work_skin();payloads[1][512+31]=3;payloads[1][64+31]=3;load();
    const auto* stopped=owner.get();expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::RequiresSkinTarget && owner.get()==stopped,"missing component-six target preserves prior buffers");
    payloads[1][512+31]=6;payloads[1][64+31]=6;word(payloads[1],512+132+16+4,2);load();
    expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::RequiresDrawingPreparation && owner.get()==stopped,"untranslated drawing configuration preserves prior skin work");
    word(payloads[1],512+132+16+4,1);
    payloads[2]=work_skin();word(payloads[2],0,0);word(payloads[2],4,0x00000d00);load();
    expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::PreparedWork && owner->jobs().empty() && !owner->clear_before_accumulation() && owner->flush_indices().empty(),
        "empty job tables skip additive root effects while retaining workspace and quantization setup");
    payloads[2]=work_skin();word(payloads[2],24,0);word(payloads[2],32,0);load();
    expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::PreparedWork && !owner->clear_before_accumulation() && owner->flush_indices().empty(),
        "zero root clear/flush fields bypass only their conditional operations");
    payloads[2]=work_skin();word(payloads[2],244+64,0x00020001);word(payloads[2],448,0x00020002);load();
    expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::PreparedWork && owner->jobs()[2].vertex_count==1,"additive direct-count loop admits one vertex");
    std::weak_ptr<const Assets> lifetime=owner->drawing().setup().selection().assets;assets.reset();expect(!lifetime.expired(),"skin work retains all source providers");
    owner.reset();expect(lifetime.expired(),"discarding last skin work releases retained providers");
    payloads[2]=work_skin();load();expect(awl::prepare_world_map_player_skin_work(assets,&owner)==S::PreparedWork,"skin allocation baseline prepares");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto* prior=owner.get();const auto live=allocation_probe::live;size_t rejected=0;bool prepared=false;
    for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::prepare_world_map_player_skin_work(assets,&owner);allocation_probe::enabled=false;
        if(status==S::PreparedWork){prepared=true;break;}++rejected;
        expect(status==S::AllocationFailure && owner.get()==prior && allocation_probe::live==live,"failed skin ownership preparation preserves prior buffers and releases staging");}
    expect(prepared && rejected>106,"allocation sweep reaches complete skin work publication");std::cout<<"PLAYER_SKIN_WORK_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
}

void synthetic(){
    Fixture fixture;std::shared_ptr<const Assets> assets;awl::WorldMapPlayerModelSelection selection;
    selection.gpl.size=123;
    expect(awl::load_world_map_player_model_assets(0,nullptr)==Status::InvalidInput,"null owner output rejects");
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::ReadFailure && !assets,"missing selected archive publishes nothing");
    expect(awl::prepare_world_map_player_model_selection(nullptr,&selection)==Status::RequiresAssets && selection.gpl.size==123,
        "missing selection assets preserve output");
    fixture.write("boy_0.arc",{});
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::ReadFailure && !assets,"filesystem rejects empty archive before parsing");
    auto payloads=files();auto too_short=payloads;too_short.resize(5);fixture.write("boy_0.arc",archive(too_short));
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::UnsupportedLayout && !assets,"missing required mouth bank rejects");
    auto bad_archive=archive(payloads);word(bad_archive,60,UINT32_MAX);fixture.write("boy_0.arc",bad_archive);
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::UnsupportedLayout && !assets,"overflowing opaque GPL extent rejects");
    auto bad_model=payloads;bad_model[0]={0};fixture.write("boy_0.arc",archive(bad_model));
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::UnsupportedLayout && !assets,"node one must be supported ACT metadata");
    for(size_t bank=3;bank<6;++bank){
        auto bad_texture=payloads;bad_texture[bank]={0};fixture.write("boy_0.arc",archive(bad_texture));
        expect(awl::load_world_map_player_model_assets(0,&assets)==Status::TextureFailure && !assets,"each malformed required TPL rejects atomically");
    }
    for(const char* path:{"boy_0.arc","boy_1.arc","boy_2.arc"})fixture.write(path,archive(payloads));
    constexpr uint32_t variants[]={0,0,0,1,1,2};
    for(uint32_t phase=0;phase<6;++phase){
        const auto previous=assets;
        expect(awl::load_world_map_player_model_assets(phase,&assets)==Status::Loaded && assets && assets->variant()==variants[phase],
            "all six phases select the verified primary archive variant");
        if(!assets)return;
        if(previous && previous->variant()==variants[phase])expect(previous==assets,"same-variant phases reuse immutable owner");
        else expect(previous!=assets,"different variant stages a distinct immutable owner");
        expect(awl::prepare_world_map_player_model_selection(assets,&selection)==Status::RequiresAuxiliaryConstruction,
            "primary model never substitutes the supported null-auxiliary secondary construction");
        expect(selection.model.metadata.count_6==1 && selection.model.records.size()==1 && selection.gpl.size==3 &&
            selection.skin.size==4 && selection.texture_animation && selection.texture_animation->size==1,
            "prepared ACT and opaque GPL/SKN/TAM preserve distinct numbered entries");
        constexpr uint8_t channels[]={255,0,1,2};constexpr uint16_t indices[]={0,0,0,1};
        constexpr Bank banks[]={Bank::Body,Bank::Eyes,Bank::Mouth,Bank::Body};
        for(size_t i=0;i<4;++i){const auto& binding=selection.textures[i];
            expect(binding.channel==channels[i] && binding.index==indices[i] && binding.bank==banks[i] &&
                binding.bank_identity==assets->texture_bank_identity(banks[i]),"four primary texture routes preserve original order and bank identity");}
    }
    const auto retained=assets;
    fixture.write("boy_2.arc",{});
    expect(awl::load_world_map_player_model_assets(5,&assets)==Status::Loaded && assets==retained,"loaded variant does not reread changed source bytes");
    fixture.write("boy_0.arc",bad_archive);
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::UnsupportedLayout && assets==retained,
        "failed variant replacement preserves an existing complete owner");
    for(uint32_t phase=6;phase<256;++phase)expect(awl::load_world_map_player_model_assets(phase,&assets)==Status::RequiresPhase && assets==retained,
        "out-of-range supplied phases stop without disturbing ownership");
    expect(awl::load_world_map_player_model_assets(UINT32_MAX,&assets)==Status::RequiresPhase,"maximum phase cannot wrap to a catalog row");
    awl::WorldMapPlayerModelAssetView view;view.size=999;
    expect(!assets->resource(0,&view) && !assets->resource(UINT32_MAX,&view) && view.size==999 && !assets->resource(1,nullptr),
        "directory/out-of-range/null extent lookups reject without output mutation");
    awl::WorldMapModelResource not_act;
    expect(assets->resource(2,&view) && view.data[0]==2 && !assets->models().resolve(2,&not_act),"opaque GPL stays distinct from ACT lookup");
    const auto invalid_bank=static_cast<Bank>(255);
    expect(!assets->texture(invalid_bank,0) && !assets->texture(Bank::Eyes,1) &&
        assets->texture_count(invalid_bank)==0 && assets->texture_bank_identity(invalid_bank)==0,"invalid bank and texture indices reject");
    expect(awl::prepare_world_map_player_model_selection(assets,nullptr)==Status::InvalidInput,"null selection output rejects");
    expect(awl::prepare_world_map_player_model_selection(selection.assets,&selection)==Status::RequiresAuxiliaryConstruction,
        "selection input/provider alias safely retains ownership");

    auto six=payloads;six.resize(6);fixture.write("boy_0.arc",archive(six));assets.reset();
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_selection(assets,&selection)==Status::RequiresAuxiliaryConstruction &&
        !selection.texture_animation,"missing optional node seven remains absent");
    auto missing_body=payloads;missing_body[3]=tpl(1);fixture.write("boy_1.arc",archive(missing_body));
    expect(awl::load_world_map_player_model_assets(3,&assets)==Status::Loaded,"descriptor bounds validate before route selection");
    auto previous=selection.assets;
    expect(awl::prepare_world_map_player_model_selection(assets,&selection)==Status::TextureFailure && selection.assets==previous,
        "missing body index one cannot publish a partial texture route");
    auto null_image=payloads;word(null_image[5],12,0);fixture.write("boy_2.arc",archive(null_image));
    expect(awl::load_world_map_player_model_assets(5,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_selection(assets,&selection)==Status::TextureFailure && selection.assets==previous,
        "reached null image descriptor cannot supply primary texture binding");
    auto euler=payloads;euler[0]=model(true);fixture.write("boy_1.arc",archive(euler));assets.reset();
    expect(awl::load_world_map_player_model_assets(3,&assets)==Status::Loaded &&
        awl::prepare_world_map_player_model_selection(assets,&selection)==Status::RequiresResourcePreparation && selection.assets==previous,
        "untranslated nonzero Euler pose blocks selection before auxiliary construction");
    previous.reset();std::weak_ptr<const Assets> lifetime=selection.assets;assets.reset();
    expect(!lifetime.expired() && selection.skin.data[0]==5 && selection.assets->texture(Bank::Body,1)->raw_data[0]==18,
        "selection keeps opaque and texture views alive after external owner release");
    selection={};expect(lifetime.expired(),"last selection releases owned archive and texture storage");
    for(const char* path:{"boy_0.arc","boy_1.arc","boy_2.arc"})fixture.write(path,archive(payloads));

#if !defined(_MSC_VER) || !defined(_DEBUG)
    // Preserve MSVC Debug iterator checks: their noexcept proxy allocations
    // are not injectable. Release sweeps only catchable allocation paths.
    std::shared_ptr<const Assets> old;
    expect(awl::load_world_map_player_model_assets(0,&old)==Status::Loaded,"allocation replacement baseline loads");
    assets=old;const auto live=allocation_probe::live;size_t rejected=0;bool loaded=false;
    for(size_t fail=0;fail<256;++fail){
        allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::load_world_map_player_model_assets(3,&assets);allocation_probe::enabled=false;
        if(status==Status::Loaded){loaded=true;break;}++rejected;
        expect((status==Status::AllocationFailure || status==Status::TextureFailure) && assets==old && allocation_probe::live==live,
            "failed allocation frees pending copies and preserves prior variant");
    }
    expect(loaded && rejected>10,"loader allocation sweep reaches complete publication");
    std::cout<<"PLAYER_MODEL_LOAD_ALLOCATION_FAILURES "<<rejected<<'\n';
    expect(awl::prepare_world_map_player_model_selection(old,&selection)==Status::RequiresAuxiliaryConstruction,"selection allocation baseline prepares");
    const auto old_selection=selection.assets;const auto live_selection=allocation_probe::live;size_t stopped=0;bool prepared=false;
    for(size_t fail=0;fail<128;++fail){
        allocation_probe::remaining=fail;allocation_probe::enabled=true;
        const auto status=awl::prepare_world_map_player_model_selection(assets,&selection);allocation_probe::enabled=false;
        if(status==Status::RequiresAuxiliaryConstruction){prepared=true;break;}++stopped;
        expect(status==Status::AllocationFailure && selection.assets==old_selection && allocation_probe::live==live_selection,
            "selection allocation failure preserves prior plan and releases staging");
    }
    expect(prepared && stopped>0,"selection allocation sweep reaches dependency stop");
    std::cout<<"PLAYER_MODEL_SELECTION_ALLOCATION_FAILURES "<<stopped<<'\n';
#endif
}

void hash_auxiliary(uint64_t& h,uint32_t phase,const awl::WorldMapPlayerModelAuxiliaryMetadata& auxiliary){
    hash(h,phase);hash(h,auxiliary.assets()->variant());hash(h,uint32_t(auxiliary.gpl().sections.size()));
    for(const auto& section:auxiliary.gpl().sections){
        hash(h,section.offset);hash(h,section.name_offset);hash(h,section.material_offset);
        for(uint32_t offset:section.sub_offsets)hash(h,offset);
        hash(h,section.position.has_value());
        if(section.position){const auto& p=*section.position;
            for(uint32_t value:{p.header_offset,p.data_offset.value_or(UINT32_MAX),uint32_t(p.count),uint32_t(p.format),uint32_t(p.component_count)})hash(h,value);}
        hash(h,uint32_t(section.commands.size()));
        for(const auto& command:section.commands){hash(h,command.type_0);hash(h,command.channel_1);}
    }
    const auto& skin=auxiliary.skin();
    for(uint16_t count:skin.counts)hash(h,count);
    for(const auto& table:skin.tables)hash(h,table.value_or(UINT32_MAX));
    for(uint32_t value:{uint32_t(skin.control_6),uint32_t(skin.marker_7),skin.word_14,skin.word_18,
        skin.pointer_1c.value_or(UINT32_MAX),skin.output_buffer_bound.value_or(UINT32_MAX),uint32_t(skin.relocations.size())})hash(h,value);
    for(const auto& r:skin.relocations){hash(h,r.location);hash(h,uint32_t(r.space));hash(h,r.target);}
    hash(h,auxiliary.skin_buffer_size());hash(h,auxiliary.skin_target().has_value());
    if(auxiliary.skin_target()){
        hash(h,auxiliary.skin_target()->section_index);hash(h,auxiliary.skin_target()->data.offset);hash(h,auxiliary.skin_target()->size);
    }
}
void hash_setup(uint64_t& h,uint32_t phase,const awl::WorldMapPlayerModelSetupPlan& plan){
    const auto base=plan.selection().gpl.reference.offset;
    auto texture=[&](const awl::WorldMapPlayerModelTextureBinding& t){hash(h,t.channel);hash(h,uint32_t(t.bank));hash(h,t.index);};
    hash(h,phase);hash(h,plan.selection().assets->variant());hash(h,uint32_t(plan.command_sections().size()));
    for(const auto& section:plan.command_sections()){
        hash(h,section.table_offset);hash(h,section.commands_offset);hash(h,section.packet_budget);hash(h,uint32_t(section.commands.size()));
        for(const auto& c:section.commands){hash(h,c.header.offset-base);hash(h,c.type);hash(h,c.channel);hash(h,c.word_4);
            hash(h,c.display_list?c.display_list->offset-base:UINT32_MAX);hash(h,c.display_list_size);hash(h,c.table_offset);hash(h,c.packet_budget);}
    }
    hash(h,plan.command_packets_offset());hash(h,plan.command_allocation_size());
    hash(h,plan.core_before_features().resource.count_6);hash(h,uint32_t(plan.features().size()));hash(h,plan.feature_storage_size());
    hash(h,plan.model_allocation_size());hash(h,plan.preallocated_size());hash(h,uint32_t(plan.storage_requests().size()));
    for(const auto& s:plan.storage_requests()){hash(h,s.offset);hash(h,s.size);}
    hash(h,uint32_t(plan.features().size()));
    for(const auto& f:plan.features()){hash(h,f.node.value_or(UINT32_MAX));hash(h,f.group);hash(h,f.storage_offset);hash(h,f.resource.offset-base);hash(h,f.cache_offset);}
    hash(h,uint32_t(plan.feature_node_order().size()));for(auto i:plan.feature_node_order())hash(h,i);
    hash(h,uint32_t(plan.texture_writes().size()));for(const auto& w:plan.texture_writes()){hash(h,w.section);hash(h,w.command);texture(w.texture);}
    for(const auto& s:plan.command_sections())for(const auto& c:s.commands){hash(h,c.texture.has_value());if(c.texture)texture(*c.texture);}
}
void hash_draw(uint64_t& h,uint32_t phase,const awl::WorldMapPlayerDrawCommands& owner){
    hash(h,phase);hash(h,uint32_t(owner.sections().size()));
    for(const auto& section:owner.sections()){
        hash(h,uint32_t(section.arrays.size()));for(const auto& a:section.arrays){hash(h,a.attribute);hash(h,a.data.offset-owner.setup().selection().gpl.reference.offset);hash(h,a.stride);}
        hash(h,uint32_t(section.formats.size()));for(const auto& f:section.formats){hash(h,f.attribute);hash(h,f.components);hash(h,f.type);hash(h,f.fraction);}
        hash(h,section.constant_color.has_value());if(section.constant_color)for(auto c:*section.constant_color)hash(h,c);
        for(auto v:section.vat_words)hash(h,v);for(auto v:section.matrix_indices)hash(h,v);
        hash(h,uint32_t(section.commands.size()));
        for(const auto& state:section.commands){hash(h,uint32_t(state.index()));
            if(const auto* texture=std::get_if<awl::WorldMapPlayerTextureParameters>(&state)){
                for(auto v:{uint32_t(texture->binding.channel),uint32_t(texture->binding.bank),uint32_t(texture->binding.index),uint32_t(texture->unit),
                    texture->wrap_s,texture->wrap_t,texture->min_filter,texture->mag_filter,texture->anisotropy,uint32_t(texture->mipmap),uint32_t(texture->bias_clamp),uint32_t(texture->edge_lod)})hash(h,v);
                for(float v:{texture->min_lod,texture->max_lod,texture->lod_bias}){uint32_t bits;std::memcpy(&bits,&v,4);hash(h,bits);}
            }else if(const auto* descriptors=std::get_if<std::vector<awl::WorldMapPlayerVertexDescriptor>>(&state)){
                hash(h,uint32_t(descriptors->size()));for(const auto& d:*descriptors){hash(h,d.attribute);hash(h,d.mode);}
            }else{const auto& t=std::get<awl::WorldMapPlayerTevParameters>(state);
                for(auto v:t.color_inputs)hash(h,v);for(auto v:t.alpha_inputs)hash(h,v);
                for(auto v:{t.operation,t.bias,t.scale,t.clamp,t.output_register,t.stage,t.coordinate,t.map,t.raster_channel,t.texgen,t.texgen_type,t.texgen_source,
                    t.normalize,t.post_matrix,t.texgens,t.color_channels,t.stages})hash(h,v);
            }
        }
    }
}
void hash_skin_work(uint64_t& h,uint32_t phase,const awl::WorldMapPlayerSkinWork& work){
    hash(h,phase);hash(h,uint32_t(work.initial_output().size()));hash(h,work.workspace_size);hash(h,work.weight_quantization());hash(h,work.vertex_quantization());
    hash(h,work.clear_before_accumulation().has_value());if(work.clear_before_accumulation()){hash(h,work.clear_before_accumulation()->offset);hash(h,work.clear_before_accumulation()->size);}
    hash(h,uint32_t(work.flush_indices().size()));for(auto index:work.flush_indices())hash(h,index);
    hash(h,uint32_t(work.jobs().size()));
    for(const auto& j:work.jobs()){
        hash(h,uint32_t(j.kind));hash(h,j.record_offset);hash(h,j.vertex_count);hash(h,j.prefix);hash(h,uint32_t(j.matrix_indices.size()));for(auto index:j.matrix_indices)hash(h,index);

        hash(h,j.source.offset);hash(h,j.source.size);
        for(const auto& span:{j.weights,j.indices}){hash(h,span.has_value());if(span){hash(h,span->offset);hash(h,span->size);}}
        hash(h,j.output.offset);hash(h,j.output.size);hash(h,uint32_t(j.scatter_indices.size()));for(auto index:j.scatter_indices)hash(h,index);
    }
}
void local(const char* disc,bool frame_evidence=false){
    expect(awl::filesystem_mount("/",disc),"local disc mounts");
    uint64_t digest=14695981039346656037ull,auxiliary_digest=digest,setup_digest=digest,draw_digest=digest,skin_work_digest=digest,skin_execution_digest=digest;
    uint64_t animation_frame_digest=digest,owned_primary_digest=digest,topology_digest=digest,initial_geometry_digest=digest,animated_geometry_digest=digest,feature_draw_digest=digest;
    std::ifstream animation_file(std::filesystem::path(disc)/"files"/"boy_0.anm.arc",std::ios::binary);
    std::vector<uint8_t> animation_bytes((std::istreambuf_iterator<char>(animation_file)),{});
    awl::WorldMapAnimationBank animation_bank;
    expect(animation_bank.parse(200,std::move(animation_bytes)) && animation_bank.clip_count()==126,"local animated-frame bank loads");
    if(!animation_bank.loaded())return;
    uint64_t frame_digest=digest;std::ofstream frame_inputs;
    if(frame_evidence){frame_inputs.open("build/terrain-trace/player-frame-native-inputs.txt");expect(bool(frame_inputs),"ignored local frame input evidence opens");}
    std::shared_ptr<const Assets> assets;size_t selections=0,relocations=0,owned_frames=0;
    for(uint32_t phase=0;phase<6;++phase){
        expect(awl::load_world_map_player_model_assets(phase,&assets)==Status::Loaded && assets,"phase-selected local model archive loads");
        if(!assets)return;
        awl::WorldMapPlayerModelSelection selection;
        expect(awl::prepare_world_map_player_model_selection(assets,&selection)==Status::RequiresAuxiliaryConstruction,
            "actual primary ACT and texture routes prepare up to required auxiliary construction");
        if(!selection.assets)return;
        ++selections;hash(digest,phase);hash(digest,assets->variant());hash(digest,uint32_t(assets->file_count()));
        for(uint32_t node:{1u,3u,2u,4u,5u,6u,7u}){
            awl::WorldMapPlayerModelAssetView view;
            const bool present=assets->resource(node,&view);hash(digest,node);hash(digest,present?view.reference.offset:0);hash(digest,present?view.size:0);
        }
        hash(digest,selection.model.metadata.count_6);hash(digest,selection.model.metadata.field_14);
        for(Bank bank:{Bank::Body,Bank::Eyes,Bank::Mouth}){
            hash(digest,uint32_t(assets->texture_count(bank)));
            for(uint32_t i=0;i<assets->texture_count(bank);++i){const auto* texture=assets->texture(bank,uint16_t(i));
                expect(texture && texture->raw_data,"every local texture descriptor retains image storage");if(!texture)return;
                for(uint32_t value:{uint32_t(i),texture->image_header_offset,texture->palette_header_offset,
                    uint32_t(texture->header.height),uint32_t(texture->header.width),texture->header.format,
                    texture->header.data_offset,texture->encoded_data_size,uint32_t(texture->mip_levels.size())})hash(digest,value);
                awl::TplDecodedTexture decoded;
                expect(awl::tpl_decode_mip_chain_to_rgba8(*texture,&decoded) && decoded.mip_levels.size()==texture->mip_levels.size(),
                    "each owned local player texture decodes without GPU/window interaction");
            }
        }
        for(const auto& binding:selection.textures){hash(digest,binding.channel);hash(digest,uint32_t(binding.bank));hash(digest,binding.index);}
        std::unique_ptr<awl::WorldMapPlayerModelAuxiliaryMetadata> auxiliary;
        expect(awl::prepare_world_map_player_model_auxiliary(assets,&auxiliary)==awl::WorldMapPlayerModelAuxiliaryStatus::Prepared && auxiliary,
            "all actual player GPL/SKN pairs prepare bounded auxiliary metadata");
        if(!auxiliary)return;
        relocations+=auxiliary->skin().relocations.size();hash_auxiliary(auxiliary_digest,phase,*auxiliary);
        std::unique_ptr<awl::WorldMapPlayerModelSetupPlan> plan;
        expect(awl::prepare_world_map_player_model_setup(assets,&plan)==awl::WorldMapPlayerModelSetupStatus::PreparedPlan && plan,
            "each local phase prepares the required nonnull-auxiliary setup plan");
        if(!plan)return;
        expect(plan->core_before_features().nodes.size()==55 && plan->features().size()==1 && !plan->features()[0].node &&
            plan->feature_storage_size()==80 && plan->model_allocation_size()==7120 && plan->preallocated_size()==29120 &&
            plan->storage_requests().back().offset==4448 && plan->command_allocation_size()==608 && plan->texture_writes().size()==4,
            "actual primary archives preserve distinct allocation/cursor sizes, root feature and four command bindings");
        hash_setup(setup_digest,phase,*plan);
        std::unique_ptr<awl::WorldMapPlayerDrawCommands> draw;
        expect(awl::prepare_world_map_player_draw_commands(assets,&draw)==awl::WorldMapPlayerDrawStatus::PreparedParameters && draw,"all local player phases prepare CPU drawing parameters");
        if(!draw)return;
        const auto& arrays=draw->sections()[0].arrays;
        expect(arrays.size()==3 && arrays[0].count==1831 && arrays[0].bounded_size==21966 && arrays[1].count==1102 && arrays[1].bounded_size==4408 &&
            arrays[2].count==1831 && arrays[2].bounded_size==21966,"actual interleaved array bounds include the last XYZ/normal and UV element");
        hash_draw(draw_digest,phase,*draw);
        awl::WorldMapPlayerTopology topology;
        expect(awl::prepare_world_map_player_topology(*draw,&topology).status==awl::WorldMapPlayerTopologyStatus::PreparedTopology,
            "every actual player material range decodes complete bounded references/triangle topology");
        hash(topology_digest,phase);hash_topology(topology_digest,topology);
        size_t references=0,triangles=0;for(const auto& batch:topology.batches){references+=batch.references.size();triangles+=batch.triangle_indices.size()/3;}
        expect(topology.batches.size()==4 && references==4352 && triangles==2706,"each actual model selection preserves all four ranges / 4352 references / 2706 triangles");
        std::unique_ptr<awl::WorldMapPlayerSkinWork> skin_work;
        const auto work_status=awl::prepare_world_map_player_skin_work(assets,&skin_work);
        expect(work_status==awl::WorldMapPlayerSkinWorkStatus::PreparedWork && skin_work,"all local skin jobs prepare owned CPU storage");
        if(!skin_work){std::cerr<<"SKIN_WORK_STOP "<<int(work_status)<<'\n';return;}
        expect(skin_work->jobs().size()==124 && skin_work->initial_output().size()==21984 && skin_work->clear_before_accumulation() &&
            skin_work->clear_before_accumulation()->offset==21664 && skin_work->clear_before_accumulation()->size==320 && skin_work->flush_indices().size()==137,
            "actual skin job counts, rounded clear and cache flush targets match original schedule");
        hash_skin_work(skin_work_digest,phase,*skin_work);
        std::vector<uint8_t> skinned=skin_work->initial_output();
        for(uint32_t frame=0;frame<2;++frame){
            expect(awl::execute_world_map_player_skin_work(*skin_work,skin_palette(phase+frame*256,55),skinned,&skinned)==awl::WorldMapPlayerSkinExecutionStatus::Executed,
                "all local jobs execute with supplied diagnostic frame matrices");
            hash(skin_execution_digest,phase);hash(skin_execution_digest,frame);for(uint8_t byte:skinned)hash(skin_execution_digest,byte);
        }
        const auto& setup=skin_work->drawing().setup();const auto& core=setup.core_before_features();
        if(frame_evidence)for(size_t node=0;node<core.nodes.size();++node){frame_inputs<<phase<<' '<<node;
            for(const auto& m:{*setup.selection().model.records[core.nodes[node].source_record_index].matrix,core.inverse_initial_matrices[node]})
                for(float value:m){uint32_t raw;std::memcpy(&raw,&value,4);frame_inputs<<' '<<std::hex<<raw<<std::dec;}frame_inputs<<'\n';}
        awl::WorldMapPlayerFrame evaluated;evaluated.vertex_output=skin_work->initial_output();
        for(uint32_t pass=0;pass<2;++pass){auto input=frame_input(phase+pass*256,55);input.evaluate_nodes=true;input.request_skin=true;
            if(pass==0){input.nodes.assign(55,{});input.root_pose=explicit_pose({1,0,0,3,0,1,0,9,0,0,1,-2});}
            expect(awl::evaluate_world_map_player_frame(*skin_work,input,evaluated.vertex_output,&evaluated)==awl::WorldMapPlayerFrameStatus::Evaluated &&
                evaluated.skin_executed && evaluated.node_matrices.size()==55 && evaluated.skin_palette.size()==55 && evaluated.feature_matrix_writes.size()==1,
                "local retained hierarchy/default/inverse matrices feed CPU skinning with supplied frame poses");
            hash(frame_digest,phase);hash(frame_digest,pass);hash_frame(frame_digest,evaluated);
        }
        awl::WorldMapAnimationClip primary,linked;
        expect(animation_bank.resolve(phase,&primary) && animation_bank.resolve(phase+1,&linked),"diagnostic local frame clips resolve");
        awl::WorldMapPlayerAnimationFrameInput animated;
        animated.root_pose=explicit_pose({1,0,0,3,0,1,0,9,0,0,1,-2});animated.node_post_transforms.resize(55);
        animated.playback.clip_10=primary.reference;animated.playback.link_14=2;animated.playback.value_18=0.375f;
        awl::WorldMapAnimationPlayback linked_playback;linked_playback.clip_10=linked.reference;
        const std::vector<const awl::WorldMapAnimationBank*> banks{&animation_bank};
        evaluated={};evaluated.vertex_output=skin_work->initial_output();
        std::vector<uint8_t> first_vertices;bool changed=false;
        std::unique_ptr<awl::WorldMapPlayerFrameOwner> primary_owner;
        expect(awl::prepare_world_map_player_frame_owner(assets,&primary_owner).status==awl::WorldMapPlayerFrameOwnerStatus::PreparedCpuState,
            "actual primary CPU frame owner prepares retained skin/output/feature state");if(!primary_owner)return;
        std::unique_ptr<awl::WorldMapPlayerGeometry> geometry;
        expect(awl::prepare_world_map_player_geometry(primary_owner->work(),&geometry).status==awl::WorldMapPlayerGeometryStatus::PreparedGeometry,
            "all local primary phases prepare persistent complete signed16 geometry");if(!geometry)return;
        hash(initial_geometry_digest,phase);hash_geometry(initial_geometry_digest,*geometry);
        uint32_t pass=0;
        for(float time:{-5.0f,1.25f,10000.0f}) {
            animated.playback.position_0=time;linked_playback.position_0=time;
            const std::vector<awl::WorldMapAnimationPlaybackRecord> records{{2,linked_playback}};
            const auto result=awl::evaluate_world_map_player_animation_frame(*skin_work,animated,records,banks,evaluated.vertex_output,&evaluated);
            expect(result.status==awl::WorldMapPlayerFrameStatus::Evaluated && !result.failed_node && evaluated.skin_executed &&
                evaluated.node_matrices.size()==55 && evaluated.skin_palette.size()==55 && evaluated.feature_write_order==std::vector<uint32_t>{0},
                "local sampled/blended clips feed hierarchy, feature placement and quantized skinning");
            hash(animation_frame_digest,phase);hash(animation_frame_digest,pass);hash_frame(animation_frame_digest,evaluated);
            awl::WorldMapPlayerOwnedFrameInput owned_input;
            if(pass==0)owned_input.root_pose=animated.root_pose;
            owned_input.playback=awl::partial_world_map_animation_playback(animated.playback);owned_input.playback.word_8.reset();owned_input.playback.limit_c.reset();
            owned_input.node_post_transforms=animated.node_post_transforms;
            auto owned_link=awl::partial_world_map_animation_playback(linked_playback);owned_link.value_18.reset();
            const std::vector<awl::WorldMapAnimationPartialPlaybackRecord> owned_records{{2,owned_link}};
            expect(primary_owner->advance(owned_input,owned_records,banks).status==awl::WorldMapPlayerFrameStatus::Evaluated && primary_owner->frame() &&
                primary_owner->frame()->skin_executed && primary_owner->frame()->node_matrices.size()==55 && primary_owner->feature_matrices().size()==1 &&
                primary_owner->feature_matrices()[0]==evaluated.root_matrix,"local sampled CPU frame publishes persistent primary root feature and quantized skin output");
            if(!primary_owner->frame())return;
            hash(owned_primary_digest,phase);hash(owned_primary_digest,pass);hash_frame(owned_primary_digest,*primary_owner->frame());
            expect(geometry->decode(primary_owner->work(),primary_owner->vertex_output()).status==awl::WorldMapPlayerGeometryStatus::DecodedVertices,
                "successive persistent CPU frame bytes feed every retained geometry reference");
            hash(animated_geometry_digest,phase);hash(animated_geometry_digest,pass);hash_geometry(animated_geometry_digest,*geometry);
            awl::WorldMapPlayerFeatureDrawInput draw_input;draw_input.feature=0;draw_input.view=skin_palette(phase*3+pass+256,1)[0];draw_input.array_override=0;
            draw_input.lighting={uint8_t(pass!=0),uint32_t(1u<<phase),uint32_t(2u<<phase),int32_t(pass)};
            awl::WorldMapPlayerFeatureDrawState draw_state;
            const auto draw_status=awl::prepare_world_map_player_feature_draw_state(*primary_owner,*geometry,draw_input,&draw_state);
            expect(draw_status==awl::WorldMapPlayerDrawStateStatus::PreparedFeatureState && draw_state.normal_matrix && draw_state.section==0 &&
                draw_state.feature_matrix==primary_owner->feature_matrices()[0],"all local sampled primary frames supply retained feature placement and checked drawing operands");
            hash(feature_draw_digest,phase);hash(feature_draw_digest,pass);hash_draw_state(feature_draw_digest,draw_status,draw_state);
            if(frame_evidence){std::ofstream evidence("build/terrain-trace/player-geometry-frame-"+std::to_string(phase)+"-"+std::to_string(pass)+".bin",std::ios::binary);
                const auto& bytes=primary_owner->vertex_output();evidence.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
                expect(bool(evidence),"ignored native vertex-byte evidence writes completely");}
            if(pass==0)first_vertices=evaluated.vertex_output;else if(evaluated.vertex_output!=first_vertices)changed=true;
            ++pass;
        }
        expect(changed,"local sampled time changes produce different actual vertex bytes");
        uint64_t before_skip=14695981039346656037ull;hash_geometry(before_skip,*geometry);
        awl::WorldMapPlayerOwnedFrameInput skipped;skipped.evaluate_nodes=false;skipped.request_skin=false;
        expect(primary_owner->advance(skipped,{},{}).status==awl::WorldMapPlayerFrameStatus::Evaluated &&
            geometry->decode(primary_owner->work(),primary_owner->vertex_output()).status==awl::WorldMapPlayerGeometryStatus::DecodedVertices,
            "node/skin-off persistent frame can decode its retained output without animation inputs");
        uint64_t after_skip=14695981039346656037ull;hash_geometry(after_skip,*geometry);
        expect(before_skip==after_skip,"skipped skinning preserves all decoded geometry and topology");
        // Diagnostic no-skin owner over the actual ACT. This does not claim
        // that the primary ACT is the game's selected secondary model.
        auto source_models=assets->models();awl::WorldMapSecondarySetupStep owner_setup;
        expect(awl::prepare_world_map_secondary_model_setup(1,source_models.identity(),&source_models,0,std::nullopt,{},uint64_t(0),&owner_setup)==
            awl::WorldMapSecondarySetupStatus::RequiresConstruction,"actual ACT prepares a diagnostic no-skin owner");
        std::unique_ptr<awl::WorldMapNativeModel> owner;
        expect(awl::construct_world_map_secondary_model(source_models,1,owner_setup,&owner).status==awl::WorldMapModelConstructionStatus::Constructed,
            "actual ACT constructs with privately retained prepared defaults");
        if(!owner)return;source_models.clear();
        awl::WorldMapAnimationChannelState owner_channel;const auto owner_binding=owner->binding();
        owner_channel.target_8=owner_channel.previous_c=owner_channel.older_10=owner_binding.playback_178;
        std::vector<awl::WorldMapAnimationPartialPlaybackRecord> owner_records;awl::WorldMapAnimationPartialChannelStep owner_step;
        auto source_animation=animation_bank;
        expect(awl::advance_world_map_native_animation_channel(owner.get(),&owner_channel,&owner_records,
            {owner_binding.model_identity,200,phase,0,0,1.25f},&source_animation,&owner_step)==awl::WorldMapAnimationChannelStatus::Advanced,
            "actual diagnostic owner retains its selected local clip and bank");source_animation.clear();
        std::vector<awl::WorldMapModelFrameSource> owner_sources(1);owner_sources[0].secondary=owner.get();
        owner_sources[0].input.root_pose=animated.root_pose;owner_sources[0].input.node_post_transforms.resize(55);
        awl::WorldMapModelHierarchyFrame owned;
        expect(awl::evaluate_world_map_model_hierarchy_frame(owner_sources,owner_binding.model_identity,{}, {},1,&owned).status==
            awl::WorldMapModelHierarchyFrameStatus::Evaluated && owned.models.size()==1,"actual diagnostic hierarchy uses only owner-retained data");
        if(owned.models.size()!=1)return;
        // Compare the ownership adapter to the already verified supplied frame
        // path, with explicit unblended inputs and the same native numerics.
        auto supplied=owner_sources[0].input;supplied.playback.clip_10=primary.reference;supplied.playback.position_0=1.25f;
        awl::WorldMapPlayerFrame expected;
        expect(awl::evaluate_world_map_model_animation_frame(owner->prepared_resource(),owner->core(),supplied,{},banks,{},&expected).status==
            awl::WorldMapPlayerFrameStatus::Evaluated && owned.models[0].frame.node_matrices==expected.node_matrices &&
            owned.models[0].frame.root_matrix==expected.root_matrix && owned.models[0].frame.node_matrices.size()==55 &&
            owned.models[0].frame.skin_palette.empty() && !owned.models[0].frame.skin_executed && !owner->playback() &&
            !owner->partial_playback().value_18 && owner->partial_playback().position_0==1.25f,
            "all 55 owned node matrices match explicit supplied playback after source clearing, without inventing weight or skinning");
        ++owned_frames;
    }
    std::cout<<"LOCAL_PLAYER_MODEL_SELECTIONS "<<selections<<" metadata "<<std::hex<<digest<<std::dec<<'\n';
    expect(selections==6,"all six phase selections reach the explicit dependency boundary");
    std::cout<<"LOCAL_PLAYER_OWNED_MODEL_FRAMES "<<owned_frames<<" diagnostic no-skin owners / 55 nodes each\n";
    expect(owned_frames==6,"all six actual archive selections verify the retained ownership frame adapter");
    expect(digest==0x94932159bae8bb64ull,
        "all six selections match independent DOL row/lookup/TPL-relocation/texture-route evidence");
    std::cout<<"LOCAL_PLAYER_AUXILIARY_METADATA "<<selections<<" relocations "<<relocations<<" digest "<<std::hex<<auxiliary_digest<<std::dec<<'\n';
    expect(relocations==2310 && auxiliary_digest==0x432873f8104e8d5cull,
        "all six GPL/SKN bindings and skin fixup spaces/order match independently executed DOL instructions");
    expect(skin_work_digest==0x5c08e5c6ab1c096aull,"all six skin work schedules match executed original preparation/control flow");
    std::cout<<"LOCAL_PLAYER_SKIN_WORK "<<selections<<" digest "<<std::hex<<skin_work_digest<<std::dec<<'\n';
    expect(skin_execution_digest==0xc3015abf20bed0efull,"all local vertex output bytes match original instructions for two supplied frames across six phases");
    std::cout<<"LOCAL_PLAYER_SKIN_EXECUTION "<<selections*2<<" digest "<<std::hex<<skin_execution_digest<<std::dec<<'\n';
    std::cout<<"LOCAL_PLAYER_FRAME_EVALUATION "<<selections*2<<" digest "<<std::hex<<frame_digest<<std::dec<<'\n';
    expect(frame_digest==0xd46f30d9043eee3cull,"all local frame matrices and vertices match mapped instructions with supplied native default/inverse matrices");
    std::cout<<"LOCAL_PLAYER_ANIMATION_FRAMES_NATIVE_NUMERICS 18 digest "<<std::hex<<animation_frame_digest<<std::dec<<'\n';
    expect(animation_frame_digest==0x30d965797df91c84ull,"local complete animation frames match mapped instructions with native normalization factor/math and supplied prepared matrices");
    std::cout<<"LOCAL_PLAYER_PRIMARY_OWNED_FRAMES 18 digest "<<std::hex<<owned_primary_digest<<std::dec<<'\n';
    expect(owned_primary_digest==0x30d965797df91c84ull,"persistent primary CPU frames match the existing local mapped-instruction digest under the same documented native numerical substitutions");
    expect(draw_digest==0xef4c5a9a9aa08adaull,"all six CPU drawing parameter sets match independently executed original routines");
    std::cout<<"LOCAL_PLAYER_DRAW_PARAMETERS "<<selections<<" digest "<<std::hex<<draw_digest<<std::dec<<'\n';
    std::cout<<"LOCAL_PLAYER_TOPOLOGY 6 selections digest "<<std::hex<<topology_digest<<std::dec<<'\n';
    expect(topology_digest==0x6421e51e33e9ada2ull,"all local ranges, prior texture bindings, exact reference triples, primitive boundaries and triangle indices match independent raw-byte parsing");
    std::cout<<"LOCAL_PLAYER_INITIAL_GEOMETRY 6 digest "<<std::hex<<initial_geometry_digest<<std::dec<<'\n';
    std::cout<<"LOCAL_PLAYER_ANIMATED_GEOMETRY 18 digest "<<std::hex<<animated_geometry_digest<<std::dec<<'\n';
    expect(initial_geometry_digest==0xfa5b11bdd89ae606ull && animated_geometry_digest==0xc7236931e39f9331ull,
        "all initial/updated local vertex values and preserved topology match independent raw-array/skin-byte dequantization");
    std::cout<<"LOCAL_PLAYER_FEATURE_DRAW_STATE 18 digest "<<std::hex<<feature_draw_digest<<std::dec<<'\n';
    expect(feature_draw_digest==0xa7e436eecc3560faull,"eighteen local feature/view/normal/channel/material proposals match independently executed draw-prefix instructions with supplied diagnostic view/lighting state");
    std::cout<<"LOCAL_PLAYER_SETUP_PLANS "<<selections<<" digest "<<std::hex<<setup_digest<<std::dec<<'\n';
    expect(setup_digest==0xd9d84507566b1a0cull,
        "all six setup tables/budgets, features/sizes/shared caches and texture selections match mapped original instructions");
}
} // namespace
int main(int argc,char** argv){
#if defined(_MSC_VER) && defined(_DEBUG)
    for(int kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}){
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE);_CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
#endif
    embedded_tpl();skin_metadata_checks();auxiliary_checks();awl::filesystem_shutdown();setup_checks();awl::filesystem_shutdown();draw_checks();awl::filesystem_shutdown();skin_work_checks();awl::filesystem_shutdown();skin_execution_checks();awl::filesystem_shutdown();frame_checks();awl::filesystem_shutdown();animation_frame_checks();awl::filesystem_shutdown();attachment_frame_checks();awl::filesystem_shutdown();hierarchy_frame_checks();awl::filesystem_shutdown();partial_frame_sampling_checks();owned_hierarchy_frame_checks();awl::filesystem_shutdown();primary_frame_owner_checks();awl::filesystem_shutdown();topology_checks();awl::filesystem_shutdown();geometry_checks();awl::filesystem_shutdown();primary_owner_checks();awl::filesystem_shutdown();holder_checks();awl::filesystem_shutdown();initial_root_scale_checks();initial_input_checks();awl::filesystem_shutdown();timer_asset_checks();awl::filesystem_shutdown();feature_draw_state_checks();awl::filesystem_shutdown();synthetic();awl::filesystem_shutdown();
    if(argc==3 && std::string(argv[1])=="--player-model-local")local(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-frame-local")local(argv[2],true);
    else if(argc==3 && std::string(argv[1])=="--player-primary-local")local_primary(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-holder-local")local_holder(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-initial-holder-local")local_initial_holder(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-timer-holder-local")local_timer_holder(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-clock-holder-local")local_timer_holder(argv[2],true);
    else if(argc==3 && std::string(argv[1])=="--player-initial-model-local")local_timer_holder(argv[2],true,true);
    else if(argc!=1)expect(false,"usage: --player-model-local <disc>, --player-frame-local <disc>, --player-primary-local <disc>, --player-holder-local <disc>, --player-initial-holder-local <disc>, --player-timer-holder-local <disc>, --player-clock-holder-local <disc> or --player-initial-model-local <disc>");
    awl::filesystem_shutdown();return failures?1:0;
}
