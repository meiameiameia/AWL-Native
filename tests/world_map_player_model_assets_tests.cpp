#include "awl/world_map_player_model_assets.h"
#include "awl/world_map_player_model_auxiliary.h"
#include "awl/world_map_player_model_setup.h"
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
        for(const char* name:{"boy_0.arc","boy_1.arc","boy_2.arc"})std::filesystem::remove(root/"files"/name,e);
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
void local(const char* disc){
    expect(awl::filesystem_mount("/",disc),"local disc mounts");
    uint64_t digest=14695981039346656037ull,auxiliary_digest=digest,setup_digest=digest;
    std::shared_ptr<const Assets> assets;size_t selections=0,relocations=0;
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
    }
    std::cout<<"LOCAL_PLAYER_MODEL_SELECTIONS "<<selections<<" metadata "<<std::hex<<digest<<std::dec<<'\n';
    expect(selections==6,"all six phase selections reach the explicit dependency boundary");
    expect(digest==0x94932159bae8bb64ull,
        "all six selections match independent DOL row/lookup/TPL-relocation/texture-route evidence");
    std::cout<<"LOCAL_PLAYER_AUXILIARY_METADATA "<<selections<<" relocations "<<relocations<<" digest "<<std::hex<<auxiliary_digest<<std::dec<<'\n';
    expect(relocations==2310 && auxiliary_digest==0x432873f8104e8d5cull,
        "all six GPL/SKN bindings and skin fixup spaces/order match independently executed DOL instructions");
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
    embedded_tpl();skin_metadata_checks();auxiliary_checks();awl::filesystem_shutdown();setup_checks();awl::filesystem_shutdown();synthetic();awl::filesystem_shutdown();
    if(argc==3 && std::string(argv[1])=="--player-model-local")local(argv[2]);
    else if(argc!=1)expect(false,"usage: --player-model-local <disc>");
    awl::filesystem_shutdown();return failures?1:0;
}
