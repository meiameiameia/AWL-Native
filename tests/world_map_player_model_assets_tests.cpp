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
#include "awl/world_map_player_startup.h"
#include "awl/world_map_player_sources.h"
#include "awl/world_map_scene_assembly.h"
#include "awl/world_map_fade.h"
#include "awl/world_map_phase.h"
#include "awl/world_map_player_context.h"
#include "awl/world_map_player_context_labels.h"
#include "awl/world_map_scene_index.h"
#include "awl/world_map_trigger_asset.h"
#include "awl/filesystem.h"

#include <chrono>
#include <cfenv>
#include <cmath>
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
uint32_t actor_bits(float value) {uint32_t word;std::memcpy(&word,&value,4);return word;}
void actor_placement_comparisons() {
    using S=awl::WorldMapPlayerActorPoseStatus;using M=awl::WorldMapModelPreparationStatus;
    constexpr std::array<std::array<float,2>,10> headings{{{0,1},{1,0},{0,-1},{-1,0},{-0.0f,0},{0,-0.0f},{-0.0f,-0.0f},{1,1},{-1,1},{7,-3}}};
    uint64_t digest=14695981039346656037ull,chain_digest=digest;
    awl::WorldMapAnimationPose before{},after{};
    for(uint32_t seed=0;seed<256;++seed) {
        before[0]=(seed<<24)|((seed*73)&0xffffffu);
        constexpr float values[]{2,-3,4,7,8,9,11,12,13,14,15,16};
        for(size_t i=0;i<12;++i)before[i+1]=actor_bits(values[i]);
        const auto& h=headings[seed%headings.size()];
        const awl::WorldMapPlayerActorPlacement placement{{float(seed)*0.125f-12,float(-int32_t(seed))*0.25f,float(seed)*0.375f+24},
            {h[0],std::numeric_limits<float>::quiet_NaN(),h[1]}};
        expect(awl::prepare_world_map_player_actor_pose(before,placement,&after)==S::Prepared,
            "actor placement reads X/Z heading only and overwrites only the reached pose words");
        hash(digest,seed);for(uint32_t word:before)hash(digest,word);
        for(float value:placement.position)hash(digest,actor_bits(value));
        hash(digest,actor_bits(h[0]));hash(digest,actor_bits(h[1]));for(uint32_t word:after)hash(digest,word);
        awl::WorldMapModelMatrix placed;
        expect(awl::prepare_world_map_model_pose(after,&placed)==M::Prepared,"translated actor pose feeds the reached yaw conversion");
        hash(chain_digest,seed);for(uint32_t word:after)hash(chain_digest,word);for(float value:placed)hash(chain_digest,actor_bits(value));
        auto alias=before;
        expect(awl::prepare_world_map_player_actor_pose(alias,placement,&alias)==S::Prepared && alias==after,
            "actor placement supports input/output aliasing and preserves scale/header/unused quaternion word");
    }
    expect(digest==0xB135E0FDC48EDDCDull,"256 actor placements match original atan2/getter/setter instructions without hooks");
    expect(chain_digest==0xB08C355C1C9DCDD6ull,"256 connected placement/yaw matrices match the original instruction bodies without math hooks");
    std::cout<<"PLAYER_ACTOR_PLACEMENT_COMPARISONS 256 digest "<<std::hex<<digest<<std::dec<<'\n';
    std::cout<<"PLAYER_ACTOR_PLACED_MATRIX_COMPARISONS 256 digest "<<std::hex<<chain_digest<<std::dec<<'\n';
    const auto saved=after;
    awl::WorldMapPlayerActorPlacement invalid{{1,2,3},{0,0,1}};
    expect(awl::prepare_world_map_player_actor_pose(before,invalid,nullptr)==S::InvalidInput,"null actor pose output rejects");
    invalid.heading[0]=std::numeric_limits<float>::infinity();
    expect(awl::prepare_world_map_player_actor_pose(before,invalid,&after)==S::InvalidInput && after==saved,
        "nonfinite reached heading preserves pose output");
    invalid.heading[0]=0;invalid.position[2]=std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_player_actor_pose(before,invalid,&after)==S::InvalidInput && after==saved,
        "nonfinite position preserves pose output");
    const int rounding=std::fegetround();invalid.position[2]=3;
    for(int mode:{FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        expect(std::fesetround(mode)==0,"actor placement rounding setup");
        expect(awl::prepare_world_map_player_actor_pose(before,invalid,&after)==S::UnsupportedNumerics && after==saved &&
            std::fegetround()==mode,"unsupported placement rounding preserves output and the caller mode");
    }
    expect(std::fesetround(rounding)==0,"restore actor placement rounding");

    constexpr float angles[]{0,-0.0f,1,-1,45,-45,90,-90,180,-180,360,-360,1230,-1230,1e6f,-1e6f};
    constexpr uint32_t flags[]{2,3,10,11,0x8b};digest=14695981039346656037ull;
    awl::WorldMapModelMatrix matrix{};
    for(uint32_t seed=0;seed<512;++seed) {
        before={(flags[(seed/16)%5]<<24)|((seed*71)&0xffffffu),actor_bits(2),actor_bits(-3),actor_bits(4),
            actor_bits(-0.0f),actor_bits(angles[seed%16]),actor_bits(0),0x12345678,actor_bits(7),actor_bits(8),actor_bits(9),0xdeadbeef,0xffffffff};
        expect(awl::prepare_world_map_model_pose(before,&matrix)==M::Prepared,"supported yaw-only Euler pose prepares");
        hash(digest,seed);for(uint32_t word:before)hash(digest,word);for(float value:matrix)hash(digest,actor_bits(value));
    }
    expect(digest==0xA6B583872FB75587ull,"512 yaw matrices match original conversion/sine/cosine/scale/translation instructions without hooks");
    std::cout<<"PLAYER_ACTOR_YAW_COMPARISONS 512 digest "<<std::hex<<digest<<std::dec<<'\n';
    const auto saved_matrix=matrix;
    before[4]=actor_bits(1);
    expect(awl::prepare_world_map_model_pose(before,&matrix)==M::RequiresEulerRotation && matrix==saved_matrix,
        "nonzero pitch remains an explicit unsupported dependency");
    before[4]=0;before[6]=actor_bits(1);
    expect(awl::prepare_world_map_model_pose(before,&matrix)==M::RequiresEulerRotation && matrix==saved_matrix,
        "nonzero roll remains an explicit unsupported dependency");
    before[6]=0;before[5]=1; // Smallest subnormal binary32 yaw.
    expect(awl::prepare_world_map_model_pose(before,&matrix)==M::UnsupportedNumerics && matrix==saved_matrix,
        "subnormal yaw rejects without changing matrix output");
    before[5]=actor_bits(90);
    for(int mode:{FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        expect(std::fesetround(mode)==0,"yaw conversion rounding setup");
        expect(awl::prepare_world_map_model_pose(before,&matrix)==M::UnsupportedNumerics && matrix==saved_matrix &&
            std::fegetround()==mode,"reached yaw math rejects unsupported rounding atomically");
    }
    expect(std::fesetround(rounding)==0,"restore yaw conversion rounding");
}

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
        for(const char* name:{"boy_0.arc","boy_1.arc","boy_2.arc","boy_0.anm.arc","boy_0_subanm.arc","boy_0_subact.arc","char_com_eye.tam","char_com_mouth.tam","jimen-camera.col",
            "jimen-move.col","jimen1-move.col","mapobj.col","mapobj2.col","mapobj3.col","mapobj4.col","mapobj5.col","trigger.spl","mapjump.col",
            "roomobj.col.arc","mapse.col.arc","maperase.col.arc"})std::filesystem::remove(root/"files"/name,e);
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
    hash(h,owner.model().core().byte_1c);
    hash(h,owner.texture_cache().has_value());if(owner.texture_cache())for(const auto& entry:*owner.texture_cache()){
        hash(h,entry.channel);hash(h,entry.texture.has_value());if(entry.texture){const auto& t=*entry.texture;
            hash(h,t.channel);hash(h,uint32_t(t.bank));hash(h,t.index);hash(h,uint32_t(t.bank_identity>>32));hash(h,uint32_t(t.bank_identity));}}
    return h;
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
uint64_t holder_snapshot(const awl::WorldMapPlayerAnimationHolder& holder,bool include_primary=true,bool include_timers=true,bool include_playback=true) {
    uint64_t h=include_primary?primary_snapshot(holder.primary()):14695981039346656037ull;
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
        if(include_timers)for(const auto* t:{&f.first_14,&f.second_24}){key(t->resource_0);hash(h,t->clock_4);real(t->value_8);real(t->rate_c);}}
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
    if(include_playback)partial(holder.primary().model().partial_playback());key(holder.primary().model().animation_banks().size());
    for(const auto& bank:holder.primary().model().animation_banks())key(bank->identity());
    if(const auto* secondary=holder.secondary_model()) {
        key(reinterpret_cast<uintptr_t>(secondary));partial(secondary->partial_playback());
        key(secondary->bank().identity());hash(h,secondary->record().count_4);
        for(const auto& bank:secondary->animation_banks())key(bank->identity());
        const auto& core=secondary->core();key(core.parent_150);
        for(auto child:core.children_15c)key(child);
    }
    for(const auto* channel:{&holder.primary_channel(),&holder.secondary_channel()}){if(!include_playback && channel==&holder.primary_channel())continue;const auto& c=channel->state();
        hash(h,c.elapsed_0);hash(h,c.duration_4);key(c.target_8);key(c.previous_c);key(c.older_10);optional_real(c.blend_14);hash(h,c.mode_18);
        for(const auto& r:channel->records()){key(r.identity);partial(r.state);}}
    key(holder.descriptor().identity);hash(h,holder.descriptor().word_0);hash(h,holder.descriptor().word_4);hash(h,holder.descriptor().word_8);return h;
}
std::vector<uint8_t> holder_start_tables(bool normal=false) {
    // Invented DOL sections, four descriptors and indices. Trace-derived
    // addresses only locate the existing checked selector-one decoder.
    std::vector<uint8_t> b(0x234);
    constexpr uint32_t addresses[]{0x8029E4BC,0x80249238,0x8029BEF4,0x81001000,0x80282DF8,0x80346B00,0x80295394,0x8034A360};
    constexpr uint32_t offsets[]{0x100,0x120,0x140,0x160,0x1C0,0x1D0,0x220,0x228},sizes[]{4,8,16,96,1,0x44,8,12};
    for(size_t i=0;i<8;++i){word(b,i*4,offsets[i]);word(b,0x48+i*4,addresses[i]);word(b,0x90+i*4,sizes[i]);}
    // Deliberately differ from the constructor to prove reset constants are read.
    word(b,0x228,90000);word(b,0x22C,24999);word(b,0x230,1200000);
    // Invented initial status coefficients and reached big-endian rates.
    constexpr uint32_t globals[]{200000,300,1200000,1800000,75,100,60,100,uint32_t(-3),45,100,100,3};
    constexpr size_t global_offsets[]{0,4,0x10,0x14,0x20,0x24,0x28,0x2C,0x30,0x34,0x38,0x3C,0x40};
    for(size_t i=0;i<13;++i)word(b,0x1D0+global_offsets[i],globals[i]);
    for(size_t i=0;i<4;++i){b[0x220+i*2]=uint8_t((1000+i)>>8);b[0x221+i*2]=uint8_t(1000+i);}
    word(b,0x100,0x800289A8);
    constexpr uint32_t modes[]{1,3,2,0};
    for(uint32_t i=0;i<4;++i){b[0x121+i*2]=uint8_t(10+i);word(b,0x140+i*4,0x81001000+i*24);
        word(b,0x160+i*24,(modes[i]<<10)|(i&1));word(b,0x164+i*24,63u|((i==3?0u:127u)<<11)|(255u<<19));word(b,0x168+i*24,0x1500+i);
        word(b,0x16C+i*24,1023u);word(b,0x170+i*24,127u<<11);word(b,0x174+i*24,UINT32_MAX);}
    if(normal){
        b.resize(0x264);word(b,0,0x234);word(b,0x48,0x8029e4b8);word(b,0x90,16);
        for(uint32_t s=0;s<4;++s)word(b,0x234+s*4,0x800289a8);
        word(b,4,0x244);word(b,0x4c,0x80249230);word(b,0x94,32);
        for(uint32_t i=0;i<4;++i)b[0x24d+i*2]=uint8_t(10+i);
        const auto pointers=b.size();b.resize(pointers+64);word(b,8,uint32_t(pointers));word(b,0x98,64);
        for(uint32_t i=0;i<4;++i)word(b,pointers+i*4,0x81001000+i*24);
        const auto descriptors=b.size();b.resize(descriptors+288);word(b,8*4,uint32_t(descriptors));word(b,0x48+8*4,0x81003000);word(b,0x90+8*4,288);
        uint32_t row=0;
        for(uint32_t s:{0u,2u,3u})for(uint32_t i=0;i<4;++i,++row){const auto index=14+row;
            b[0x245+s*8+i*2]=uint8_t(index);word(b,pointers+(index-10)*4,0x81003000+row*24);
            word(b,descriptors+row*24,(1u<<10)|(s?0x1000u|1u:0u));word(b,descriptors+row*24+4,63u|((i==3?0u:127u)<<11)|(255u<<19));word(b,descriptors+row*24+8,0x1500+i);
            word(b,descriptors+row*24+12,1023);word(b,descriptors+row*24+16,127u<<11);word(b,descriptors+row*24+20,UINT32_MAX);}
        const auto coefficients=b.size();b.resize(coefficients+32);word(b,9*4,uint32_t(coefficients));word(b,0x48+9*4,0x80346ae0);word(b,0x90+9*4,32);
        for(size_t i=0;i<8;++i)word(b,coefficients+i*4,100);
    }
    return b;
}
std::vector<uint8_t> holder_sequence_tables(uint32_t next_word=0) {
    auto bytes=holder_start_tables();bytes.resize(0x264);
    word(bytes,8*4,0x240);word(bytes,0x48+8*4,0x81002000);word(bytes,0x90+8*4,36);
    word(bytes,0x140,0x81002000);
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    word(bytes,0x240,0);word(bytes,0x244,absent);word(bytes,0x248,0x1500);
    word(bytes,0x24C,next_word|1u);word(bytes,0x250,absent);word(bytes,0x254,UINT32_MAX);
    word(bytes,0x258,1023);word(bytes,0x25C,127u<<11);word(bytes,0x260,UINT32_MAX);
    return bytes;
}
void holder_sequence_owner_checks(const std::shared_ptr<const Assets>& models,
    const std::shared_ptr<const awl::WorldMapPlayerAnimationAssets>& animations,const awl::WorldMapPlayerPrimaryFrameInput& frame) {
    using T=awl::WorldMapPlayerStartAnimationStatus;using H=awl::WorldMapPlayerAnimationHolderStatus;
    using S=awl::WorldMapPlayerHolderProgressStatus;using I=awl::WorldMapAnimationInitializerStatus;
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    const awl::WorldMapActorAnimationDescriptor initial{300,1u<<10,absent,0};
    const awl::WorldMapAnimationFeature38 inactive{0,0,UINT32_MAX,0};
    auto bytes=holder_sequence_tables(0x2000);std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==T::Decoded,"three-record invented sequence decodes");
    if(!tables)return;std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,inactive,{},&holder).status==H::ConstructedCpuHolder && holder,
        "sequence fixture owns a checked primary/model/channel");if(!holder)return;
    awl::WorldMapPlayerStartAnimationCommand command;
    expect(holder->start(tables,command,{},{}).status==T::Advanced,"sequence fixture retains exact sequence owner through D69C start");
    const auto accepted=holder_snapshot(*holder);awl::WorldMapPlayerHolderProgressResult result;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;bool reached=false;size_t failed=0;
    for(size_t fail=0;fail<128;++fail) {
        allocation_probe::remaining=fail;allocation_probe::enabled=true;result=holder->advance_animation();allocation_probe::enabled=false;
        if(result.status==S::Advanced){reached=true;break;}++failed;
        expect(result.status==S::AllocationFailure && holder_snapshot(*holder)==accepted && allocation_probe::live==live,
            "every direct initializer/link-graph staging allocation failure preserves the entire holder and frees staging");
    }
    expect(reached && failed>3,"sequence allocation sweep reaches initialization publication");
    std::cout<<"PLAYER_SEQUENCE_ALLOCATION_FAILURES "<<failed<<'\n';
#else
    (void)accepted;result=holder->advance_animation();
#endif
    expect(result.status==S::Advanced && result.initializer_status==I::Prepared && result.initialized_records==1 && !result.terminal_feature_reset &&
        holder->state().current_descriptor_0==0x8100200C && holder->state().base_descriptor_4==0x81002000 && holder->state().completed_20==0u &&
        holder->state().restart_count_2c==0 && holder->state().flag_21==1u && !holder->state().deadline_10 && !holder->state().count_18 &&
        holder->primary().model().partial_playback().clip_10 && holder->primary().model().partial_playback().word_8==0u,
        "direct sequence DB28 changes current/playback/flag without resetting base or fabricating unknown counters");
    expect(holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"next CPU frame consumes the new sequence playback");
    result=holder->advance_animation();
    expect(result.status==S::Advanced && result.initialized_records==0 && result.terminal_feature_reset && result.feature_status==awl::WorldMapAnimationFeatureStatus::Prepared &&
        holder->state().current_descriptor_0==0x81002018 && holder->state().base_descriptor_4==0x81002000 && holder->state().completed_20==1u &&
        holder->state().restart_count_2c==1 && holder->state().flag_21==1u && holder->descriptor().word_0==1023,
        "terminal record executes feature reset and restart accounting without initializing clip 1023 or clearing the prior flag");
    result=holder->advance_animation();
    expect(result.status==S::Advanced && !result.terminal_feature_reset && holder->state().restart_count_2c==2 && holder->state().current_descriptor_0==0x81002018,
        "already completed terminal stays within its retained sequence and still accounts for the next restart opportunity");
    const auto terminal=holder_snapshot(*holder);
    expect(!holder->configure_repeats(awl::WorldMapPlayerHolderRepeatKind::Duration,10) && holder_snapshot(*holder)==terminal,
        "duration request without retained clock preserves a completed owned sequence");
    expect(holder->configure_repeats(awl::WorldMapPlayerHolderRepeatKind::Count,4) && holder->state().restart_count_2c==2,
        "count request needs no clock and preserves accumulated repeat accounting");
    result=holder->advance_animation();
    expect(result.status==S::Advanced && result.initialized_records==1 && holder->state().current_descriptor_0==0x81002000 &&
        holder->state().completed_20==1u && holder->state().restart_count_2c==3 && holder->state().flag_21==0u &&
        holder->primary_channel().state().elapsed_0==0 && holder->primary().model().partial_playback().position_0==0,
        "counted terminal restart executes same-base DB28 without resetting completion or accumulated restart count");
    result=holder->advance_animation();expect(result.status==S::Advanced && result.initialized_records==1 &&
        holder->state().current_descriptor_0==0x8100200C && holder->state().restart_count_2c==3,"restarted sequence advances in order with prior completion retained");
    result=holder->advance_animation();expect(result.status==S::Advanced && result.initialized_records==0 && result.terminal_feature_reset &&
        holder->state().current_descriptor_0==0x81002018 && holder->state().restart_count_2c==4,"reached count limit ends at the terminal without another restart");
    holder.reset();const awl::WorldMapAnimationFeature38 active{0x3a,0,UINT32_MAX,0};
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,active,{},&holder).status==H::ConstructedCpuHolder && holder,
        "late timer failure fixture constructs valid primary playback");if(!holder)return;
    expect(holder->start(tables,command,{},{}).status==T::Advanced,"late failure fixture enters owned sequence");
    const auto pending=holder_snapshot(*holder);result=holder->advance_animation();
    expect(result.status==S::TimerIncomplete && result.initialized_records==1 && result.initializer_status==I::Prepared && result.timers &&
        result.timers->status==awl::WorldMapTextureTimerStatus::RequiresClock && holder_snapshot(*holder)==pending,
        "timer failure after successful direct initialization rolls back descriptor, playback, flag and authoritative link graph");
    holder.reset();bytes=holder_sequence_tables();word(bytes,0x250,(absent&~(127u<<11))|(1u<<11));
    expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==T::Decoded,"reached secondary sequence metadata decodes without claiming ownership");
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,inactive,{},&holder).status==H::ConstructedCpuHolder && holder,
        "secondary stop fixture constructs");if(!holder)return;
    expect(holder->start(tables,command,{},{}).status==T::Advanced,"secondary stop fixture enters supported first record");
    result=holder->advance_animation();
    expect(result.status==S::Advanced && result.initializer_status==I::Prepared && result.initialized_records==1 &&
        holder->secondary_model() && holder->state().current_descriptor_0==0x8100200Cu,
        "reached next-record selected secondary constructs and publishes with the ordered primary update");
}
void holder_policy_checks() {
    using S=awl::WorldMapPlayerHolderProgressStatus;
    constexpr uint32_t values[]{0,1,2,3,UINT32_MAX,UINT32_MAX-1};
    constexpr uint32_t times[]{0,1,200,UINT32_MAX},nows[]{0,1,199,200,UINT32_MAX};
    constexpr uint32_t clips[]{0,3,1023},completed[]{0,1,255},missing_values[]{0,1,2,4,8,15};
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<4096;++seed) {
        awl::WorldMapPlayerAnimationHolderState state;state.current_descriptor_0=0x40000;state.base_descriptor_4=0x50000;
        const auto missing=missing_values[seed/72%6];
        if(!(missing&1))state.completed_20=static_cast<uint8_t>(completed[seed/24%3]);
        if(!(missing&2))state.deadline_10=times[seed/11%4];
        if(!(missing&4))state.count_18=values[seed/5%6];
        if(!(missing&8))state.completed_count_1c=values[seed/7%6];
        state.restart_limit_28=values[seed/17%6];state.restart_count_2c=values[seed/19%6];state.restart_deadline_24=times[seed/23%4];
        if(seed/53%2)state.feature_38.emplace();
        const awl::WorldMapActorAnimationDescriptor descriptor{0x40000,((seed%4)<<10)|clips[seed/4%3],(seed/47%2?1u:127u)<<11,77};
        std::optional<awl::WorldMapActorAnimationDescriptor> next;
        if(seed/37%2==0)next=awl::WorldMapActorAnimationDescriptor{0x4000c,((seed/43%4)<<10)|(seed/41%2?1023u:5u),127u<<11,88};
        std::optional<uint32_t> now;if(seed/432%2==0)now=nows[seed/13%5];
        awl::WorldMapPlayerHolderPolicyStep step;
        const auto result=awl::prepare_world_map_player_holder_policy(state,descriptor,seed/12%2!=0,now,next,&step);
        const auto& a=step.after;
        const uint32_t known=(a.completed_20?1u:0u)|(a.deadline_10?2u:0u)|(a.count_18?4u:0u)|(a.completed_count_1c?8u:0u);
        for(uint32_t v:{seed,uint32_t(result),step.required_field.value_or(0),uint32_t(step.required_descriptor.value_or(0)),
            uint32_t(step.disable_primary_loop),uint32_t(step.disable_secondary_loop),uint32_t(a.current_descriptor_0),uint32_t(a.base_descriptor_4),known,
            uint32_t(a.completed_20.value_or(0)),a.deadline_10.value_or(0),a.count_18.value_or(0),a.completed_count_1c.value_or(0),a.restart_count_2c})hash(digest,v);
        if(result!=S::Prepared)expect(step.descriptor.identity==descriptor.identity && step.descriptor.word_0==descriptor.word_0 &&
            !step.disable_primary_loop && !step.disable_secondary_loop,"stopped holder policy restores descriptor and loop proposals");
    }
    std::cout<<"PLAYER_HOLDER_POLICY 4096 digest "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0x3e42a8dd17ee287bull,"holder policy matches actual mapped D854..D9E4/A2FC/6C4C instructions, with explicit call stops and atomic rollback");
    using K=awl::WorldMapPlayerHolderRepeatKind;uint64_t repeats=14695981039346656037ull;
    constexpr uint32_t repeat_values[]{0,1,2,200,UINT32_MAX,UINT32_MAX-1};
    for(uint32_t seed=0;seed<1024;++seed) {
        awl::WorldMapPlayerAnimationHolderState before;before.restart_deadline_24=repeat_values[seed/5%6];
        before.restart_limit_28=repeat_values[seed/7%6];before.restart_count_2c=repeat_values[seed/11%6];auto after=before;
        const auto now=seed/72%2==0?std::optional<uint32_t>(repeat_values[seed/12%6]):std::nullopt;
        const auto success=awl::prepare_world_map_player_holder_repeat(before,seed%2?K::Count:K::Duration,repeat_values[seed/2%6],now,&after);
        for(uint32_t word:{seed,uint32_t(success),after.restart_deadline_24,after.restart_limit_28,after.restart_count_2c})hash(repeats,word);
    }
    std::cout<<"PLAYER_HOLDER_REPEAT 1024 digest "<<std::hex<<repeats<<std::dec<<'\n';
    expect(repeats==0x1895303ed37db0c7ull,"repeat setters match whole original D9FC/DA44/A2FC instructions with missing-clock rollback");
    awl::WorldMapPlayerAnimationHolderState alias;alias.restart_count_2c=7;
    expect(awl::prepare_world_map_player_holder_repeat(alias,K::Duration,2,UINT32_MAX,&alias) && alias.restart_deadline_24==1 &&
        awl::prepare_world_map_player_holder_repeat(alias,K::Count,3,{},&alias) && alias.restart_deadline_24==1 && alias.restart_count_2c==7,
        "repeat setters support whole-state aliasing, clock wrap and count/deadline coexistence without resetting accumulated count");
    expect(!awl::prepare_world_map_player_holder_repeat(alias,static_cast<K>(99),0,0,&alias) && alias.restart_limit_28==3 &&
        !awl::prepare_world_map_player_holder_repeat(alias,K::Count,0,{},nullptr),"invalid repeat kind/output preserves prior state");
    awl::WorldMapPlayerAnimationHolderState state;state.current_descriptor_0=100;state.base_descriptor_4=100;
    state.completed_20=uint8_t{0};state.count_18=3u;state.completed_count_1c=1u;
    awl::WorldMapActorAnimationDescriptor descriptor{100,3u<<10,1u<<11,0};awl::WorldMapPlayerHolderPolicyStep step;
    auto run=[&](bool done,std::optional<uint32_t> now={},std::optional<awl::WorldMapActorAnimationDescriptor> next={}) {
        return awl::prepare_world_map_player_holder_policy(state,descriptor,done,now,next,&step);
    };
    expect(run(true)==S::Prepared && step.after.completed_count_1c==2u && step.disable_primary_loop && step.disable_secondary_loop,
        "penultimate counted completion disables both loops without advancing the descriptor");
    state.completed_count_1c=2;
    expect(run(true)==S::RequiresNextDescriptor && step.required_descriptor==112u && step.after.completed_count_1c==2u,
        "final counted completion requires the exact next record and rolls back its increment");
    const awl::WorldMapActorAnimationDescriptor terminal{112,1023,127u<<11,0};
    expect(run(true,{},terminal)==S::Prepared && step.after.completed_20==1u && step.after.restart_count_2c==1 && step.descriptor.identity==112,
        "known terminal without optional feature completes and enters restart accounting");
    state.feature_38.emplace();expect(run(true,{},terminal)==S::RequiresFeatureReset && step.after.current_descriptor_0==100,
        "reached terminal feature reset remains explicit and preserves the holder");state.feature_38.reset();
    state.restart_limit_28=2;
    expect(run(true,{},terminal)==S::RequiresInitialization && step.required_descriptor==100u && step.after.restart_count_2c==0,
        "repeat restart requires unconditional current initialization rather than an equal-base D69C start");
    step.after.current_descriptor_0=777;descriptor.identity=101;
    expect(run(true)==S::InvalidInput && step.after.current_descriptor_0==777,"mismatched descriptor preserves policy diagnostics");
    descriptor.identity=100;state.current_descriptor_0=UINT32_MAX-4;descriptor.identity=state.current_descriptor_0;
    expect(run(true)==S::InvalidInput && step.after.current_descriptor_0==777,"overflowing PPC descriptor addition rejects without output mutation");
    expect(awl::prepare_world_map_player_holder_policy(state,descriptor,true,{},terminal,nullptr)==S::InvalidInput,"absent policy output rejects");
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
    holder_sequence_owner_checks(models,animations,frame);
    using P=awl::WorldMapPlayerHolderProgressStatus;
    const auto pending=holder_snapshot(*holder);const auto sequence=holder->advance_animation();
    expect(sequence.status==P::RequiresNextDescriptor && sequence.required_descriptor==112u && holder_snapshot(*holder)==pending,
        "reached sequence advance rolls back primary progress and preserves the full holder/frame");
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> counted;
    const awl::WorldMapActorAnimationDescriptor twice{200,(3u<<10)|(2u<<14),absent,0};
    const awl::WorldMapAnimationFeature38 inactive_timers{0,0,UINT32_MAX,0};
    expect(awl::construct_world_map_player_animation_holder(models,animations,twice,inactive_timers,{},&counted).status==H::ConstructedCpuHolder && counted,
        "counted holder constructs with both timer updates legitimately bypassed");
    if(counted) {
        const auto progress=counted->advance_animation();const auto& c=counted->primary_channel();
        expect(progress.status==P::Advanced && progress.primary_completed==true && progress.timers && progress.timers->updated_mask==0 &&
            counted->state().completed_count_1c==1u && counted->primary().model().partial_playback().word_8==1u,
            "penultimate completion publishes count after model copy while inactive timers need no invented clock/assets");
        for(const auto& r:c.records())if(r.identity==c.state().target_8)expect(r.state.word_8==0u,"target loop is disabled after the model retains its earlier loop word");
        const auto accepted=holder_snapshot(*counted);const auto stopped=counted->advance_animation();
        expect(stopped.status==P::RequiresNextDescriptor && stopped.required_descriptor==212u && holder_snapshot(*counted)==accepted,
            "final repeat leaves accepted count, model and target unchanged until the next descriptor is owned");
    }
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> no_timers;
    const awl::WorldMapActorAnimationDescriptor loop{300,1u<<10,absent,0};
    expect(awl::construct_world_map_player_animation_holder(models,animations,loop,std::nullopt,{},&no_timers).status==H::ConstructedCpuHolder && no_timers,
        "missing-feature fixture retains a valid normal primary channel");
    if(no_timers){const auto accepted=holder_snapshot(*no_timers);const auto stopped=no_timers->advance_animation();
        expect(stopped.status==P::TimerIncomplete && stopped.primary_completed==true && stopped.timers &&
            stopped.timers->status==awl::WorldMapTextureTimerStatus::RequiresFeature && holder_snapshot(*no_timers)==accepted,
            "late missing timer feature rolls back channel, completion byte and restart counter together");}
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
    expect(awl::construct_world_map_player_animation_holder(models,animations,loop,feature,obs,&featured).status==H::ConstructedCpuHolder && featured,
        "timer rollback fixture constructs with a supplied initial stamp but no retained clock");
    if(featured){const auto accepted=holder_snapshot(*featured);const auto stopped=featured->advance_animation();
        expect(stopped.status==P::TimerIncomplete && stopped.primary_completed==true && stopped.timers &&
            stopped.timers->status==awl::WorldMapTextureTimerStatus::RequiresClock && holder_snapshot(*featured)==accepted,
            "reached missing timer clock rolls back already prepared playback and completion accounting");}featured.reset();
    auto bytes=holder_start_tables();std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==S::Decoded,"invented holder start tables decode");if(!tables)return;
    awl::WorldMapPlayerStartAnimationCommand command;
    expect(holder->start({},command,std::nullopt,{}).status==S::RequiresTables && preserved(),"missing selector tables preserve holder");
    command.selector=2;expect(holder->start(tables,command,std::nullopt,{}).status==S::RequiresSelectorTables && preserved(),"missing supported selector profile preserves complete holder");
    command.selector=4;expect(holder->start(tables,command,std::nullopt,{}).status==S::UnsupportedCommand && preserved(),"untranslated selector preserves complete holder");command.selector=1;
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
    expect(started.status==S::InvalidInput && started.initializer_status==I::RequiresSecondarySetup && started.secondary_index==0u && preserved(),"unsupported secondary index zero preserves both owned channels, counters and primary frame");
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
    counted.reset();no_timers.reset();
    const auto model_lifetime=std::weak_ptr<const Assets>(models);const auto animation_lifetime=std::weak_ptr<const awl::WorldMapPlayerAnimationAssets>(animations);
    models.reset();animations.reset();
    expect(!model_lifetime.expired() && !animation_lifetime.expired() && holder->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,
        "holder keeps CPU providers and bank group alive after source handles drop");
    holder.reset();expect(model_lifetime.expired() && animation_lifetime.expired() && table_lifetime.expired(),"holder destruction releases providers after its model and borrowed channel keys");
}
std::vector<uint8_t> holder_pair_clip(float duration) {
    std::vector<uint8_t> bytes(24);uint32_t raw;std::memcpy(&raw,&duration,4);
    word(bytes,0,0x00ac7472);word(bytes,4,0x00010000);word(bytes,8,raw);return bytes;
}
void hash_holder_pair(uint64_t& h,const awl::WorldMapPlayerAnimationHolder& holder) {
    const auto real=[&](float v){uint32_t w;std::memcpy(&w,&v,4);hash(h,w);};
    const auto& s=holder.state();
    const uint32_t mask=unsigned(s.deadline_10.has_value())|unsigned(s.count_18.has_value())*2u|unsigned(s.completed_count_1c.has_value())*4u;
    for(uint32_t w:{mask,s.deadline_10.value_or(0),s.count_18.value_or(0),s.completed_count_1c.value_or(0),
        uint32_t(s.completed_20.value_or(0)),uint32_t(s.flag_21.value_or(0)),s.restart_count_2c})hash(h,w);
    for(uint32_t which=0;which<2;++which) {
        const auto& channel=which?holder.secondary_channel():holder.primary_channel();
        const auto* model=which?holder.secondary_model():&holder.primary().model();
        const auto key=[&](uint64_t identity){if(!identity)return 0u;if(model && identity==model->binding().playback_178)return 1u;
            for(uint32_t i=0;i<channel.records().size();++i)if(channel.records()[i].identity==identity)return i+2u;
            expect(false,"paired snapshot contains only retained channel/model keys");return UINT32_MAX;};
        const auto& c=channel.state();for(uint32_t w:{c.elapsed_0,c.duration_4,key(c.older_10),key(c.previous_c),key(c.target_8),unsigned(c.blend_14.has_value())})hash(h,w);
        real(c.blend_14.value_or(0));hash(h,c.mode_18);
        const auto playback=[&](uint32_t id,const awl::WorldMapAnimationPartialPlayback& p){
            hash(h,id);real(p.position_0);real(p.rate_4);
            const uint32_t known=unsigned(p.word_8.has_value())|unsigned(p.limit_c.has_value())*2u|unsigned(p.value_18.has_value())*4u;
            hash(h,known);hash(h,p.word_8.value_or(0));real(p.limit_c.value_or(0));hash(h,p.clip_10.has_value());uint32_t index=0;
            if(p.clip_10){bool found=false;const auto* bank=model?model->animation_bank(p.clip_10->bank_identity):&holder.animations()->secondary_animations();
                if(bank)for(uint32_t i=0;i<bank->clip_count();++i){awl::WorldMapAnimationClip clip;
                    if(bank->resolve(i,&clip) && clip.reference.offset==p.clip_10->offset){index=i;found=true;break;}}
                expect(found,"paired clip resolves inside its retained immutable bank");}
            hash(h,index);hash(h,key(p.link_14));real(p.value_18.value_or(0));
        };
        for(uint32_t i=0;i<channel.records().size();++i)playback(i+2u,channel.records()[i].state);
        if(model)playback(1,model->partial_playback());
    }
}
void holder_pair_checks() {
    using H=awl::WorldMapPlayerAnimationHolderStatus;using P=awl::WorldMapPlayerHolderProgressStatus;
    using I=awl::WorldMapAnimationInitializerStatus;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    fixture.write("boy_0.arc",archive(payloads));fixture.write("boy_0.anm.arc",archive({holder_pair_clip(3),holder_pair_clip(7)}));
    fixture.write("boy_0_subanm.arc",archive({holder_pair_clip(11),holder_pair_clip(13)}));
    fixture.write("boy_0_subact.arc",archive({setup_model(),setup_model()}));
    std::shared_ptr<const Assets> models;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_model_assets(0,&models)==Status::Loaded &&
        awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"paired holder providers load");if(!models || !animations)return;
    const awl::WorldMapAnimationFeature38 inactive{0,0,UINT32_MAX,0};
    awl::WorldMapAnimationInitializerObservations obs;obs.clock=0xfffffff0u;
    uint64_t digest=14695981039346656037ull;uint32_t checked=0,ignored_secondary_completions=0;
    for(uint32_t seed=0;seed<256;++seed) {
        constexpr uint32_t values[]{0,1,2,1023};const uint32_t mode=seed%4,blend=seed/4%32,index=1+seed/128%2;
        const uint32_t a=(seed/16%2)|(mode<<10)|((seed/32%2)<<12)|((seed/64%2)<<13)|
            (values[seed/7%4]<<14)|((seed/8%2)<<24),b=63u|(blend<<6)|(index<<11)|(255u<<19);
        const awl::WorldMapActorAnimationDescriptor descriptor{100,a,b,0};std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        const auto constructed=awl::construct_world_map_player_animation_holder(models,animations,descriptor,inactive,obs,&holder);
        expect(constructed.status==H::ConstructedCpuHolder && holder && holder->secondary_model(),"fresh selected descriptor constructs both CPU models in one holder");if(!holder || !holder->secondary_model())return;
        expect(holder->state().secondary_model_c0==holder->secondary_model()->binding().model_identity &&
            holder->secondary_model()->core().parent_150==0 && holder->primary().model().core().children_15c==std::array<uint64_t,4>{},
            "null C4/C8 executes DF68's clear and early return without inventing an attachment");
        hash(digest,seed);hash_holder_pair(digest,*holder);
        for(uint32_t tick=0;tick<16;++tick){const auto before=holder_snapshot(*holder);const auto step=holder->advance_animation();
            const uint32_t code=step.status==P::Advanced?0u:step.status==P::RequiresNextDescriptor?1u:step.status==P::RequiresClock?2u:99u;
            expect(code!=99 && step.primary_completed && step.secondary_completed,"paired update reaches both channels before supported policy or dependency stop");
            if(code)expect(holder_snapshot(*holder)==before,"unavailable next descriptor/clock rolls back both progressed models/channels and holder");
            if(step.secondary_completed==true && step.primary_completed==false && step.status==P::Advanced)++ignored_secondary_completions;
            for(uint32_t w:{code,unsigned(step.primary_completed.value_or(false)),unsigned(step.secondary_completed.value_or(false))})hash(digest,w);
            hash_holder_pair(digest,*holder);
        }++checked;
    }
    std::cout<<"PLAYER_HOLDER_PAIR_MATRIX "<<checked<<" / 16 ticks "<<std::hex<<digest<<std::dec<<" ignored secondary completions "<<ignored_secondary_completions<<'\n';
    expect(digest==0x477618a716445b0cull && ignored_secondary_completions==45,
        "paired holder setup/settings/ticks/policy match actual instructions with supplied resolved clips/models and rollback on missing evidence");
    const awl::WorldMapActorAnimationDescriptor counted{200,(3u<<10)|(2u<<14),63u|(1u<<11)|(255u<<19),0};
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
    expect(awl::construct_world_map_player_animation_holder(models,animations,counted,inactive,obs,&holder).status==H::ConstructedCpuHolder,"paired count fixture constructs");if(!holder)return;
    for(unsigned tick=0;tick<4;++tick)expect(holder->advance_animation().status==P::Advanced,"paired count advances to first strict endpoint crossing");
    expect(holder->state().completed_count_1c==1u && holder->primary().model().partial_playback().word_8==1u && holder->secondary_model()->partial_playback().word_8==1u,
        "penultimate policy runs after both model copies");
    for(const auto* channel:{&holder->primary_channel(),&holder->secondary_channel()})for(const auto& r:channel->records())
        if(r.identity==channel->state().target_8)expect(r.state.word_8==0u,"penultimate count disables both target loops");
    const auto preserved=holder_snapshot(*holder);const auto* old=holder.get();
    auto invalid=counted;invalid.word_4=63u|(3u<<11)|(255u<<19);
    const auto failed=awl::construct_world_map_player_animation_holder(models,animations,invalid,inactive,obs,&holder);
    expect(failed.status==H::InvalidInput && failed.secondary_setup_status==awl::WorldMapSecondarySetupStatus::InvalidInput &&
        holder.get()==old && holder_snapshot(*holder)==preserved,"missing secondary ACT preserves the complete previous holder");
    auto late=counted;late.word_0=1u<<10;
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> no_timers;
    expect(awl::construct_world_map_player_animation_holder(models,animations,late,std::nullopt,obs,&no_timers).status==H::ConstructedCpuHolder,"late timer failure pair constructs");
    if(no_timers){const auto before=holder_snapshot(*no_timers);const auto stop=no_timers->advance_animation();
        expect(stop.status==P::TimerIncomplete && stop.secondary_completed && holder_snapshot(*no_timers)==before,"late timer stop preserves both already staged channels/models and policy");}
    // The primary feature dependency must still win before secondary lookup.
    awl::WorldMapAnimationFeature38 feature{0x3a,0,0,0};
    const auto earlier=awl::construct_world_map_player_animation_holder(models,animations,invalid,feature,{},&holder);
    expect(earlier.status==H::InitializerIncomplete && earlier.initializer_status==I::RequiresFeatureTable && !earlier.secondary_setup_status &&
        holder.get()==old && holder_snapshot(*holder)==preserved,"ordered primary feature failure precedes invalid secondary resource");
    awl::WorldMapAnimationInitializerObservations timer_obs;timer_obs.clock=17;timer_obs.fallback_table_38=900;timer_obs.rows={{900,0,0,777}};
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> timed;
    expect(awl::construct_world_map_player_animation_holder(models,animations,late,feature,timer_obs,&timed).status==H::ConstructedCpuHolder,"paired timer failure fixture constructs");
    if(timed){const auto before=holder_snapshot(*timed);const auto stop=timed->advance_animation();
        expect(stop.status==P::TimerIncomplete && stop.timers && stop.timers->status==awl::WorldMapTextureTimerStatus::RequiresClock &&
            stop.secondary_channel_status==awl::WorldMapAnimationChannelStatus::Prepared && holder_snapshot(*timed)==before,
            "missing retained timer clock rolls back both prepared model/channel updates");}
#if !defined(_MSC_VER) || !defined(_DEBUG)
    {
        const auto live=allocation_probe::live;uint32_t rejected=0;bool reached=false;
        for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            const auto result=awl::construct_world_map_player_animation_holder(models,animations,late,inactive,obs,&holder);allocation_probe::enabled=false;
            if(result.status==H::ConstructedCpuHolder){reached=true;break;}++rejected;
            expect(result.status==H::AllocationFailure && holder.get()==old && holder_snapshot(*holder)==preserved && allocation_probe::live==live,
                "paired construction allocation failures release staging and preserve the previous holder");}
        expect(reached && rejected>210,"paired constructor sweep reaches publication after both immutable bank copies");
        std::cout<<"PLAYER_HOLDER_PAIR_CONSTRUCTION_ALLOCATION_FAILURES "<<rejected<<'\n';
    }
    {
        const auto before=holder_snapshot(*holder),live=allocation_probe::live;uint32_t rejected=0;bool reached=false;
        for(size_t fail=0;fail<64;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto step=holder->advance_animation();allocation_probe::enabled=false;
            if(step.status==P::Advanced){reached=true;break;}++rejected;
            expect(step.status==P::AllocationFailure && holder_snapshot(*holder)==before && allocation_probe::live==live,"paired update allocation failures roll back both channels/models");}
        // Each channel allocates a copied pool, its model append, and the
        // progress proposal. This path legitimately bypasses timer allocation.
        expect(reached && rejected==6,"pair progression sweep covers all six staging allocations and reaches publication");std::cout<<"PLAYER_HOLDER_PAIR_PROGRESS_ALLOCATION_FAILURES "<<rejected<<'\n';
    }
#endif
    // CE88 clears only the owned model; its channel and clip snapshots survive.
    auto table_bytes=holder_start_tables();std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    expect(awl::decode_world_map_player_start_animation_tables(table_bytes.data(),table_bytes.size(),&tables)==awl::WorldMapPlayerStartAnimationStatus::Decoded,"pair reselect tables decode");
    const auto secondary_before=holder->secondary_channel().state();const auto pool_before=holder->secondary_channel().records();
    const auto bank_lifetime=std::weak_ptr<const awl::WorldMapAnimationBank>(holder->secondary_model()->animation_banks().front());
    awl::WorldMapPlayerStartAnimationCommand command;
    const auto reselect=holder->start(tables,command,std::nullopt,{});
    expect(reselect.status==awl::WorldMapPlayerStartAnimationStatus::Advanced && !holder->secondary_model() && !holder->state().secondary_model_c0 &&
        holder->secondary_channel().state().elapsed_0==secondary_before.elapsed_0 && holder->secondary_channel().state().mode_18==secondary_before.mode_18 &&
        holder->secondary_channel().records().size()==pool_before.size() && !bank_lifetime.expired(),"removal clears owned C0 while retaining channel history and its immutable clip bank");
    const auto before=holder_snapshot(*holder);
    fixture.write("boy_0_subanm.arc",archive({holder_pair_clip(11)}));
    std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> missing_clip;
    expect(awl::load_world_map_player_animation_assets(&missing_clip)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"incomplete paired catalog still loads bounded bytes");
    auto missing=late;missing.word_4=63u|(2u<<11)|(255u<<19);
    const auto failed_clip=awl::construct_world_map_player_animation_holder(models,missing_clip,missing,inactive,obs,&holder);
    expect(failed_clip.status==H::InvalidInput && failed_clip.secondary_construction &&
        failed_clip.secondary_construction->status==awl::WorldMapModelConstructionStatus::Constructed && holder_snapshot(*holder)==before,
        "missing secondary clip after successful model construction frees staging and preserves the old pair");
    models.reset();animations.reset();
    expect(!bank_lifetime.expired() && holder->advance_animation().status==P::Advanced && !holder->secondary_model(),
        "retained secondary channel clips outlive removal and dropping external provider handles");
    holder.reset();expect(bank_lifetime.expired(),"removed secondary clip storage releases with its channel owner");
}
void holder_lifecycle_checks() {
    using H=awl::WorldMapPlayerAnimationHolderStatus;using T=awl::WorldMapPlayerStartAnimationStatus;
    using P=awl::WorldMapPlayerHolderProgressStatus;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    auto single=setup_model();word(single,4,1);word(single,48,0);
    fixture.write("boy_0.arc",archive(payloads));fixture.write("boy_0.anm.arc",archive({holder_pair_clip(3),holder_pair_clip(7)}));
    fixture.write("boy_0_subanm.arc",archive({holder_pair_clip(11),holder_pair_clip(13),holder_pair_clip(5)}));
    fixture.write("boy_0_subact.arc",archive({setup_model(),setup_model(),single}));
    std::shared_ptr<const Assets> models;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_model_assets(0,&models)==Status::Loaded &&
        awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"secondary lifecycle providers load");if(!models || !animations)return;
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    const awl::WorldMapActorAnimationDescriptor initial{100,1u<<10,absent,0};
    const awl::WorldMapAnimationFeature38 inactive{0,0,UINT32_MAX,0};
    const auto command=[](uint32_t choice){awl::WorldMapPlayerStartAnimationCommand c;c.item=(choice==1 || choice==3)?7u:0u;c.action=choice==2?1u:0u;return c;};
    const auto type=[](uint32_t choice)->std::optional<awl::WorldMapPlayerStartItemType>{
        if(choice==1 || choice==3)return awl::WorldMapPlayerStartItemType{7,uint8_t(choice==3?1:5)};return {};};
    uint64_t digest=14695981039346656037ull;uint32_t compatible=0,incompatible=0,removed=0,interrupted=0,completed=0;
    for(uint32_t seed=0;seed<64;++seed) {
        auto bytes=holder_start_tables();
        for(uint32_t choice=0;choice<4;++choice){const uint32_t index=choice==3?127:choice+1;
            word(bytes,0x160+choice*24,(choice&1u)|(1u<<10)|((seed/32)<<24));
            word(bytes,0x164+choice*24,63u|((seed%32)<<6)|(index<<11)|(255u<<19));}
        std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
        expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==T::Decoded,"lifecycle selector tables decode");
        std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        expect(awl::construct_world_map_player_animation_holder(models,animations,initial,inactive,{},&holder).status==H::ConstructedCpuHolder,"lifecycle holder constructs");if(!holder || !tables)return;
        hash(digest,seed);
        constexpr uint32_t choices[]{0,1,0,2,1,3,0,1};
        for(uint32_t operation=0;operation<8;++operation) {
            const uint32_t choice=choices[operation],count=choice==2?1u:3u;
            const auto* old=holder->secondary_model();const auto old_identity=old?old->binding().model_identity:0;
            const auto before_channel=holder->secondary_channel().state();
            const bool retain=old && old->record().count_4==count && choice!=3;
            if(retain)++compatible;else if(old && choice!=3)++incompatible;else if(old)++removed;
            const auto started=holder->start(tables,command(choice),type(choice),{});
            expect(started.status==T::Advanced && holder->state().secondary_model_c0==(holder->secondary_model()?holder->secondary_model()->binding().model_identity:0),
                "selector lifecycle publishes model ownership with C0 and both channels");if(started.status!=T::Advanced)return;
            if(choice==3)expect(!holder->secondary_model() && holder->secondary_channel().state().elapsed_0==before_channel.elapsed_0 &&
                holder->secondary_channel().state().mode_18==before_channel.mode_18,"CE88 leaves secondary channel timing/history intact");
            else {
                expect(holder->secondary_model() && holder->secondary_model()->binding().model_identity!=old_identity && holder->secondary_model()->record().count_4==count,
                    "every changed selection constructs a distinct model even for compatible counts");
                const uint32_t mode=!retain?0u:(before_channel.elapsed_0>=before_channel.duration_4 || before_channel.mode_18==0)?1u:2u;
                expect(holder->secondary_channel().state().mode_18==mode,"restored clip presence and retained channel history choose the original setup branch");
                if(mode==2)++interrupted;if(mode==1)++completed;
            }
            hash(digest,choice);hash(digest,holder->secondary_model()?holder->secondary_model()->record().count_4:0u);hash_holder_pair(digest,*holder);
            const auto before=holder_snapshot(*holder);
            expect(holder->start(tables,command(choice),type(choice),{}).status==T::Unchanged && holder_snapshot(*holder)==before,
                "equal-base start does not reconstruct or reset accepted secondary history");
            constexpr uint32_t ticks[]{1,4,20};
            for(uint32_t tick=0;tick<ticks[(seed+operation)%3];++tick){const auto step=holder->advance_animation();
                expect(step.status==P::Advanced,"owned replacement/removal continues paired or primary-only progression");
                hash(digest,unsigned(step.primary_completed.value_or(false)));hash(digest,unsigned(step.secondary_completed.value_or(false)));hash_holder_pair(digest,*holder);}
        }
    }
    std::cout<<"PLAYER_HOLDER_LIFECYCLE 64 / 8 selections digest "<<std::hex<<digest<<std::dec<<" compatible "<<compatible<<" incompatible "<<incompatible
        <<" removals "<<removed<<" interrupted "<<interrupted<<" completed "<<completed<<'\n';
    expect(digest==0xd38f97a2b607a0afull && compatible==192 && incompatible==128 && removed==64 && interrupted==44 && completed==148,
        "512 lifecycle starts and subsequent paired ticks match actual count/save/restore/setup/settings/progression/policy instructions under supplied construction/clip boundaries");

    // Direct current initialization may reconstruct twice in one policy tick.
    auto bytes=holder_sequence_tables(1u<<10);word(bytes,0x244,63u|(1u<<11)|(255u<<19));word(bytes,0x250,63u|(2u<<11)|(255u<<19));
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==T::Decoded,"paired direct/restart sequence decodes");
    std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,inactive,{},&holder).status==H::ConstructedCpuHolder &&
        holder->start(tables,command(0),{},{}).status==T::Advanced && holder->configure_repeats(awl::WorldMapPlayerHolderRepeatKind::Count,3),"paired sequence starts and repeat count configures");if(!holder)return;
    for(uint32_t tick=0;tick<3;++tick)expect(holder->advance_animation().status==P::Advanced,"paired sequence reaches primary endpoint");
    const auto before=holder_snapshot(*holder),live=allocation_probe::live;
    awl::WorldMapPlayerHolderProgressResult step;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    {
    uint32_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;step=holder->advance_animation();allocation_probe::enabled=false;
        if(step.status==P::Advanced){reached=true;break;}++rejected;
        expect(step.status==P::AllocationFailure && holder_snapshot(*holder)==before && allocation_probe::live==live,
            "both staged reconstructions roll back atomically at every allocation failure without leaks");}
    expect(reached && rejected>60,"double reconstruction allocation sweep reaches publication");std::cout<<"PLAYER_HOLDER_RECONSTRUCTION_ALLOCATION_FAILURES "<<rejected<<'\n';
    }
#else
    (void)before;(void)live;step=holder->advance_animation();
#endif
    expect(step.status==P::Advanced && step.initialized_records==2 && holder->state().current_descriptor_0==holder->state().base_descriptor_4 &&
        holder->state().restart_count_2c==1 && holder->state().completed_20==0u && holder->secondary_model(),
        "next mode-one initialization then common-tail restart publish only the final reconstructed pair");

    auto terminal_bytes=holder_sequence_tables();word(terminal_bytes,0x244,63u|(1u<<11)|(255u<<19));word(terminal_bytes,0x250,63u|(2u<<11)|(255u<<19));
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> terminal_tables;
    expect(awl::decode_world_map_player_start_animation_tables(terminal_bytes.data(),terminal_bytes.size(),&terminal_tables)==T::Decoded &&
        awl::construct_world_map_player_animation_holder(models,animations,initial,inactive,{},&holder).status==H::ConstructedCpuHolder &&
        holder->start(terminal_tables,command(0),{},{}).status==T::Advanced,"paired terminal fixture starts");
    for(unsigned tick=0;tick<12;++tick)expect(holder->advance_animation().status==P::Advanced,"paired sequence progresses through both clips to terminal");
    const auto terminal_identity=holder->secondary_model()?holder->secondary_model()->binding().model_identity:0;
    const auto elapsed=holder->secondary_channel().state().elapsed_0;
    step=holder->advance_animation();
    expect(terminal_identity && holder->state().current_descriptor_0==0x81002018u && holder->state().secondary_model_c0==terminal_identity &&
        holder->secondary_channel().state().elapsed_0==elapsed && !step.secondary_channel_status,
        "clip-1023 terminal retains C0 but its absent secondary descriptor skips secondary progression");
    expect(holder->configure_repeats(awl::WorldMapPlayerHolderRepeatKind::Count,100),"terminal repeat request configures without resetting count");
    step=holder->advance_animation();
    expect(step.status==P::Advanced && step.initialized_records==1 && holder->state().current_descriptor_0==holder->state().base_descriptor_4 &&
        holder->state().completed_20==1u && holder->state().secondary_model_c0!=terminal_identity,
        "terminal restart reconstructs retained compatible secondary without D69C's completed-byte reset");

    // A missing retained timer clock occurs AFTER replacement preparation.
    const awl::WorldMapAnimationFeature38 active{0x3a,0,UINT32_MAX,0};
    expect(awl::construct_world_map_player_animation_holder(models,animations,initial,active,{},&holder).status==H::ConstructedCpuHolder &&
        holder->start(tables,command(0),{},{}).status==T::Advanced,"late failure pair owns active timers without inventing a live clock");
    if(holder){awl::WorldMapAnimationChannelProgressStep primary;
        for(unsigned i=0;i<3;++i)expect(holder->advance_primary_channel(&primary)==awl::WorldMapAnimationChannelStatus::Advanced,"late replacement fixture reaches primary endpoint without timer execution");
        const auto prior=holder_snapshot(*holder);step=holder->advance_animation();
        expect(step.status==P::TimerIncomplete && step.initialized_records==1 && holder_snapshot(*holder)==prior,"late timer failure after reconstruction rolls back model ownership, both channels and policy");}

    // A different sequence base forces D69C; equal-base starts must not swap
    // retained tables merely to replace the next record under a live owner.
    word(bytes,0x140,0x81003000);word(bytes,0x48+8*4,0x81003000);word(bytes,0x250,absent);
    expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&tables)==T::Decoded &&
        holder->start(tables,command(0),{},{}).status==T::Advanced,"late removal sequence starts at a distinct owned base");
    if(holder){awl::WorldMapAnimationChannelProgressStep primary;
        for(unsigned i=0;i<3;++i)expect(holder->advance_primary_channel(&primary)==awl::WorldMapAnimationChannelStatus::Advanced,"late removal fixture reaches primary endpoint");
        const auto prior=holder_snapshot(*holder);step=holder->advance_animation();
        expect(step.status==P::TimerIncomplete && step.initialized_records==1 && holder_snapshot(*holder)==prior && holder->secondary_model(),
            "timer failure after staged CE88 removal preserves the original model and all channel history");}

    auto removal_bytes=holder_start_tables();std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> removal_tables;
    expect(awl::decode_world_map_player_start_animation_tables(removal_bytes.data(),removal_bytes.size(),&removal_tables)==T::Decoded,"removal selector decodes");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    {
        const auto prior=holder_snapshot(*holder),allocated=allocation_probe::live;uint32_t rejected=0;bool reached=false;
        for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            const auto removal=holder->start(removal_tables,command(0),{},{});allocation_probe::enabled=false;
            if(removal.status==T::Advanced){reached=true;break;}++rejected;
            expect(removal.status==T::AllocationFailure && holder_snapshot(*holder)==prior && allocation_probe::live==allocated,
                "allocation failure after staged secondary removal retains its owner and channel bank without leaks");}
        expect(reached && rejected>20 && !holder->secondary_model(),"removal allocation sweep reaches final model deletion");
        std::cout<<"PLAYER_HOLDER_REMOVAL_ALLOCATION_FAILURES "<<rejected<<'\n';
    }
#endif
    fixture.write("boy_0_subanm.arc",archive({holder_pair_clip(11),holder_pair_clip(13)}));
    std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> missing_clip;
    expect(awl::load_world_map_player_animation_assets(&missing_clip)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded &&
        awl::construct_world_map_player_animation_holder(models,missing_clip,initial,inactive,{},&holder).status==H::ConstructedCpuHolder,
        "late clip rejection fixture retains valid current resources");
    auto selected_bytes=holder_start_tables();
    for(uint32_t choice=0;choice<4;++choice){word(selected_bytes,0x160+choice*24,1u<<10);
        word(selected_bytes,0x164+choice*24,63u|((choice?3u:1u)<<11)|(255u<<19));}
    expect(awl::decode_world_map_player_start_animation_tables(selected_bytes.data(),selected_bytes.size(),&tables)==T::Decoded &&
        holder->start(tables,command(0),{},{}).status==T::Advanced,"late clip fixture starts an owned secondary");
    const auto prior=holder_snapshot(*holder);
    expect(holder->start(tables,command(1),type(1),{}).status==T::InvalidInput && holder_snapshot(*holder)==prior,
        "missing replacement clip after valid ACT construction preserves the accepted old pair");
    word(selected_bytes,0x164+24,63u|(4u<<11)|(255u<<19));
    expect(awl::decode_world_map_player_start_animation_tables(selected_bytes.data(),selected_bytes.size(),&tables)==T::Decoded &&
        holder->start(tables,command(1),type(1),{}).status==T::InvalidInput && holder_snapshot(*holder)==prior,
        "missing replacement ACT rejects before save/construction without changing the accepted old pair");
}
void local_holder_lifecycle(const char* disc) {
    using T=awl::WorldMapPlayerStartAnimationStatus;using P=awl::WorldMapPlayerHolderProgressStatus;
    expect(awl::filesystem_mount("/",disc),"local lifecycle disc mounts");
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_start_animation_tables(&tables)==T::Loaded && tables && tables->target_verified() &&
        awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"local lifecycle target verifies");if(!tables || !animations)return;
    const awl::WorldMapAnimationFeature38 inactive{0,0,UINT32_MAX,0};awl::WorldMapAnimationInitializerObservations obs;obs.clock=17;
    uint32_t starts=0,restarts=0,removals=0;uint64_t ticks=0;
    for(uint32_t phase=0;phase<6;++phase){std::shared_ptr<const Assets> models;
        expect(awl::load_world_map_player_model_assets(phase,&models)==Status::Loaded,"local lifecycle primary loads");
        awl::WorldMapPlayerStartAnimationCommand command;awl::WorldMapPlayerStartAnimationSelection selected;
        expect(tables->select(command,{},&selected)==T::Selected,"local default choice selects");
        std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        expect(awl::construct_world_map_player_animation_holder(models,animations,selected.descriptor,inactive,obs,&holder).status==awl::WorldMapPlayerAnimationHolderStatus::ConstructedCpuHolder,"local lifecycle holder constructs");if(!holder)return;
        for(uint32_t choice:{1u,2u,3u,0u,1u,0u,2u}) {
            command.item=(choice==1 || choice==3)?7u:0u;command.action=choice==2?1u:0u;
            std::optional<awl::WorldMapPlayerStartItemType> type;if(command.item)type=awl::WorldMapPlayerStartItemType{7,uint8_t(choice==3?1:5)};
            const bool was_secondary=holder->secondary_model()!=nullptr;
            expect(holder->start(tables,command,type,obs).status==T::Advanced,"actual selectors construct/replace/remove secondaries");++starts;
            if(was_secondary && !holder->secondary_model())++removals;
            constexpr uint32_t counts[]{0,2,1,3};
            expect(choice?holder->secondary_model() && holder->secondary_model()->record().count_4==counts[choice]:!holder->secondary_model(),
                "actual choices retain their two/one/three-node secondary or remove it");
            expect(holder->configure_repeats(awl::WorldMapPlayerHolderRepeatKind::Count,3),"local paired repeat configures");
            for(uint32_t tick=0;tick<256;++tick){const auto step=holder->advance_animation();
                expect(step.status==P::Advanced,"actual paired transitions/restarts and inactive timers complete atomically");if(step.status!=P::Advanced)return;
                restarts+=step.initialized_records;++ticks;}
        }
    }
    expect(starts==42 && removals==12 && restarts>0 && ticks==10752,"all actual phase/choice lifecycle paths reach repeat reconstruction and removal");
    std::cout<<"LOCAL_PLAYER_HOLDER_LIFECYCLE "<<starts<<" starts / "<<removals<<" removals / "<<restarts<<" direct initializations / "<<ticks<<" ticks; null sources/inactive diagnostic timers, CPU only\n";
}
void local_holder_pair(const char* disc) {
    using H=awl::WorldMapPlayerAnimationHolderStatus;using P=awl::WorldMapPlayerHolderProgressStatus;
    expect(awl::filesystem_mount("/",disc),"local paired holder disc mounts");
    std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;std::shared_ptr<const awl::WorldMapPlayerAnimationAssets> animations;
    expect(awl::load_world_map_player_start_animation_tables(&tables)==awl::WorldMapPlayerStartAnimationStatus::Loaded && tables && tables->target_verified() &&
        awl::load_world_map_player_animation_assets(&animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded,"local pair providers and DOL verify");if(!tables || !animations)return;
    std::vector<std::unique_ptr<awl::WorldMapPlayerAnimationHolder>> holders;
    uint32_t completed_primary=0,completed_secondary=0,blocked=0;uint64_t ticks=0;
    for(uint32_t phase=0;phase<6;++phase)for(uint32_t choice=1;choice<4;++choice) {
        awl::WorldMapPlayerStartAnimationCommand command;command.action=choice==2?1u:0u;command.item=choice==2?0u:7u;
        std::optional<awl::WorldMapPlayerStartItemType> type;if(command.item)type=awl::WorldMapPlayerStartItemType{7,uint8_t(choice==3?1:5)};
        awl::WorldMapPlayerStartAnimationSelection selected;
        expect(tables->select(command,type,&selected)==awl::WorldMapPlayerStartAnimationStatus::Selected && selected.choice==choice,"actual pair descriptor resolves");
        std::shared_ptr<const Assets> models;expect(awl::load_world_map_player_model_assets(phase,&models)==Status::Loaded,"local pair phase model loads");
        const awl::WorldMapAnimationFeature38 inactive{0,0,UINT32_MAX,0};awl::WorldMapAnimationInitializerObservations obs;obs.clock=17;
        std::unique_ptr<awl::WorldMapPlayerAnimationHolder> holder;
        expect(awl::construct_world_map_player_animation_holder(models,animations,selected.descriptor,inactive,obs,&holder).status==H::ConstructedCpuHolder &&
            holder && holder->secondary_model(),"all actual selected pairs construct with null C4/C8 and inactive diagnostic timers");if(!holder || !holder->secondary_model())return;
        const auto index=(selected.descriptor.word_4>>11)&127u;awl::WorldMapModelResource resource;awl::WorldMapAnimationClip secondary_clip;
        expect(animations->secondary_models().resolve(index,&resource) && animations->secondary_animations().resolve(index-1,&secondary_clip) &&
            holder->secondary_model()->record().count_4==resource.count_6 && holder->secondary_model()->partial_playback().clip_10 &&
            holder->secondary_model()->partial_playback().clip_10->offset==secondary_clip.reference.offset &&
            holder->secondary_model()->partial_playback().limit_c==secondary_clip.parameter_zero && holder->primary().model().core().nodes.size()==55,
            "actual 55-node primary and selected secondary retain matching ACT/clip metadata");models.reset();
        for(uint32_t tick=0;tick<256;++tick){const auto before=holder_snapshot(*holder);const auto step=holder->advance_animation();
            if(step.primary_completed==true)++completed_primary;if(step.secondary_completed==true)++completed_secondary;
            if(step.status!=P::Advanced){expect((step.status==P::RequiresNextDescriptor || step.status==P::RequiresClock) && holder_snapshot(*holder)==before,
                "local pair stops atomically at missing sequence/clock");++blocked;break;}++ticks;}
        holders.push_back(std::move(holder));
    }
    const auto lifetime=std::weak_ptr<const awl::WorldMapPlayerAnimationAssets>(animations);animations.reset();tables.reset();
    expect(!lifetime.expired() && holders.size()==18,"all actual paired holders retain their providers after caller handles drop");
    for(const auto& holder:holders){const auto before=holder_snapshot(*holder);const auto step=holder->advance_animation();
        expect(step.secondary_channel_status==awl::WorldMapAnimationChannelStatus::Prepared &&
            (step.status==P::Advanced || holder_snapshot(*holder)==before),"retained actual paired models continue staging/progression safely");}
    expect(ticks>0 && completed_primary>0 && completed_secondary>0,"actual paired progression reaches independent primary and secondary boundaries");
    std::cout<<"LOCAL_PLAYER_HOLDER_PAIR "<<holders.size()<<" pairs / "<<ticks<<" accepted ticks / primary boundaries "<<completed_primary
        <<" secondary boundaries "<<completed_secondary<<" dependency stops "<<blocked<<"; diagnostic timers/null sources, CPU only\n";
    holders.clear();expect(lifetime.expired(),"paired holder destruction releases models before their borrowed channels and retained providers");
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
        const auto started=holder->start(tables,command,std::nullopt,{});
        expect(started.status==S::Advanced && holder->secondary_model() && holder->secondary_model()->record().count_4==1,
            "actual action-only selection constructs secondary index 67 through the owned selector path");command.action=0;
        expect(holder->start(tables,command,{},{}).status==S::Advanced && !holder->secondary_model(),"actual no-item selection removes the owned secondary");++checked;
    }
    expect(checked==6 && frame_digest==0xcc652d4bb4e9690aull && geometry_digest==0xf5968658e37d53caull,
        "all holder frames/meshes preserve the previous local primary wiring fingerprints under documented native numerics");
    std::cout<<"LOCAL_PLAYER_HOLDER "<<checked<<" phases / two owned channels / unknown counters retained / prior CPU fingerprints unchanged; actor startup and GPU pending\n";
}
std::vector<uint8_t> initial_input_dol(bool state1_profile=false) {
    // Invented sections, indices, descriptors, phase rows and scale catalog.
    std::vector<uint8_t> b(0x368);
    constexpr uint32_t addresses[]{0x80249230,0x8029E4B8,0x80282DF8,0x8029BECC,0x81000000,0x8029F818,0x81000100,0x8024A6E0,0x80249A6C,0x81000130};
    constexpr uint32_t offsets[]{0x100,0x108,0x10C,0x110,0x130,0x160,0x180,0x1B0,0x1B8,0x248}, sizes[]{8,4,1,20,48,24,48,6,144,0x120};
    for(size_t i=0;i<10;++i){word(b,i*4,offsets[i]);word(b,0x48+i*4,addresses[i]);word(b,0x90+i*4,sizes[i]);}
    word(b,0x108,0x800289A8);
    constexpr uint32_t absent=63u|(127u<<11)|(255u<<19);
    for(uint32_t i=0;i<4;++i){b[0x101+i*2]=uint8_t(i+1);word(b,0x114+i*4,0x81000000+i*12);
        word(b,0x130+i*12,1u<<10);word(b,0x134+i*12,absent);word(b,0x138+i*12,900+i);}
    for(uint32_t i=0;i<6;++i){word(b,0x160+i*4,0x81000100+i*8);word(b,0x180+i*8,0x3A+i);word(b,0x184+i*8,0x24+i);}
    constexpr uint8_t catalog[]{5,2,4,1,3,0};
    constexpr uint32_t scale_words[]{0x3F800001,0x80000000,0,0x3F000000,0xC0400000,0x40000000};
    for(size_t i=0;i<6;++i){b[0x1B0+i]=catalog[i];word(b,0x1BC+i*24,scale_words[i]);}
    for(uint32_t i=6;i<42;++i){word(b,0x248+(i-6)*8,0x3A+i%6);word(b,0x24C+(i-6)*8,0x24+i%6);}
    if(state1_profile){b.resize(0x37c);word(b,0x28,0x368);word(b,0x70,0x8034a1cc);word(b,0xb8,4);word(b,0x368,0x40000080);
        word(b,0x2c,0x36c);word(b,0x74,0x8034a250);word(b,0xbc,16);
        for(size_t i=0;i<4;++i)word(b,0x36c+i*4,actor_bits(std::array<float,4>{0,8,2,1}[i]));}
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
    for(uint32_t phase=0;phase<6;++phase)for(uint32_t selector:{0u,1u,7u,0x24u}){awl::WorldMapPlayerModelTypeRow row;
        expect(inputs->select_model_type(phase,selector,&row) && row.slot==0x8029F818+phase*4 && row.selector==selector &&
            row.type_0==0x3A+(phase+selector)%6 && row.type_4==0x24+(phase+selector)%6,"retained dispatch rows follow phase pointer, selector stride and exact typed keys");}
    awl::WorldMapPlayerModelTypeRow row_before{9,10,11,12},row_out=row_before;
    expect(!inputs->select_model_type(6,0,&row_out) && !inputs->select_model_type(0,2,&row_out) && !inputs->select_model_type(0,0,nullptr) &&
        row_out.slot==9 && row_out.selector==10 && row_out.type_0==11 && row_out.type_4==12,"invalid dispatch row query preserves prior output");
    query.model_slot=1;expect(inputs->select(query).status==S::UnsupportedModelSlot,"other actor model slots remain explicit stops");query.model_slot=0;
    query.alternate=0;expect(inputs->select(query).status==S::UnsupportedAlternate,"alternate 286D8 descriptor/model branch remains unsupported");query.alternate=UINT32_MAX;
    expect(awl::prepare_world_map_player_initial_animation({},query,{}).status==S::RequiresInputs,"missing initial provider has no invented feature snapshot");
    for(size_t size=0;size<bytes.size();++size)expect(awl::decode_world_map_player_initial_animation_inputs(bytes.data(),size,&inputs)==S::UnsupportedLayout && inputs.get()==previous,"every truncated initial DOL prefix preserves previous provider");
    for(auto change:std::vector<std::pair<size_t,uint32_t>>{{0x108,0},{0x114,0},{0x114,0x81000001},{0x114,0x81000028},
        {0x160,0},{0x174,0x81000129},{0x180,0x39},{0x180,UINT32_MAX},{0x184,0x23},{0x184,0x80000024},
        {4,0x100},{0x4C,0x80249230},{0x60,0xFFFFFFF0},{0x90+6*4,49},{0x90+7*4,5},{0x90+8*4,124},
        {0x1BC,0x7F800000},{0x234,0x7FC01234},{0x90+9*4,0x11F}}){auto malformed=bytes;word(malformed,change.first,change.second);
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
std::vector<uint8_t> sampled_timer_table() {
    auto bytes=timer_table();
    for(size_t at=80;at<bytes.size();at+=16){word(bytes,at,1);word(bytes,at+8,0);}
    return bytes;
}
std::vector<uint8_t> texture_records(uint32_t seed,uint32_t channel=0) {
    const uint32_t count=seed%6+1;std::vector<uint8_t> bytes(8+count*8);
    word(bytes,0,count);word(bytes,4,0xdeadbeef);
    for(uint32_t i=0;i<count;++i){word(bytes,8+i*8,(seed*7+i*11+channel*3)%(channel?36u:58u));
        word(bytes,12+i*8,actor_bits(float((i*(seed%3+1))%7)));}return bytes;
}
std::vector<uint8_t> one_resource_table(const std::vector<uint8_t>& resource) {
    std::vector<uint8_t> bytes(20);word(bytes,0,0xF4E1EEED);word(bytes,4,1);word(bytes,8,1);word(bytes,12,16);word(bytes,16,20);
    bytes.insert(bytes.end(),resource.begin(),resource.end());return bytes;
}
bool same_timer(const awl::WorldMapAnimationFeatureTimer& a,const awl::WorldMapAnimationFeatureTimer& b) {
    return a.resource_0==b.resource_0 && a.clock_4==b.clock_4 && actor_bits(a.value_8)==actor_bits(b.value_8) && actor_bits(a.rate_c)==actor_bits(b.rate_c);
}
bool same_feature(const awl::WorldMapAnimationFeature38& a,const awl::WorldMapAnimationFeature38& b) {
    return a.type_0==b.type_0 && a.type_4==b.type_4 && a.index_8==b.index_8 && a.table_c==b.table_c &&
        same_timer(a.first_14,b.first_14) && same_timer(a.second_24,b.second_24);
}
void texture_timer_checks() {
    using S=awl::WorldMapTextureTimerStatus;using T=awl::WorldMapPlayerTimerBank;
    constexpr float rates[]{-1,-0.0f,0,.5f,1,1.25f,2},durations[]{0,.5f,1,3,20,-1,100000};
    constexpr uint32_t deltas[]{0,1,499,500,999,1000,1999,2000,5000,UINT32_MAX,UINT32_MAX-1,0x1000001,0xffffff};
    constexpr uint32_t nows[]{0,1,33,UINT32_MAX,UINT32_MAX-7,0x1000000};
    uint64_t digest=14695981039346656037ull;
    std::vector<uint8_t> header(8);word(header,0,0xfefefefe);
    awl::WorldMapPlayerTimerResourceView resource{1,0,8,header.data()};
    for(uint32_t seed=0;seed<1024;++seed){const auto now=nows[seed%6],stamp=now-deltas[seed/6%13];
        const auto rate=rates[seed/11%7],duration=durations[seed/23%7];word(header,4,actor_bits(duration));
        awl::WorldMapAnimationFeatureTimer timer{1,stamp,std::numeric_limits<float>::quiet_NaN(),rate},after;
        expect(awl::prepare_world_map_texture_timer(timer,resource,now,&after)==S::Prepared && after.resource_0==1 && actor_bits(after.rate_c)==actor_bits(rate),
            "elapsed timer ignores its old value/count and preserves resource/rate");
        for(uint32_t w:{seed,now,stamp,actor_bits(rate),actor_bits(duration),after.clock_4,actor_bits(after.value_8)})hash(digest,w);
        auto alias=timer;expect(awl::prepare_world_map_texture_timer(alias,resource,now,&alias)==S::Prepared && same_timer(alias,after),"timer input/output alias has identical rounded result");}
    expect(digest==0x513843A8F8E76049ull,"1024 complete elapsed/wrap/rounding/boundary timer cases match actual E03C/A2FC instructions");
    std::cout<<"TEXTURE_TIMER_PROGRESS 1024 "<<std::hex<<digest<<std::dec<<'\n';
    word(header,4,actor_bits(1));awl::WorldMapAnimationFeatureTimer timer{1,0,999,1},after{9,8,7,6};const auto prior=after;
    expect(awl::prepare_world_map_texture_timer(timer,resource,2000,&after)==S::Prepared && after.value_8==1 && after.clock_4==2000,
        "equality after one subtraction retains one full duration instead of repeated wrapping");
    expect(awl::prepare_world_map_texture_timer(after,resource,2000,&after)==S::Prepared && after.value_8==0 && after.clock_4==2000,
        "same-clock update recomputes from the reset stamp rather than accumulating the prior remainder");
    expect(awl::prepare_world_map_texture_timer(timer,resource,6000,&after)==S::Prepared && after.value_8==5 && after.clock_4==6000,
        "large jumps subtract duration once and reset the clock");after=prior;
    for(uint32_t size=0;size<8;++size){resource.size=size;expect(awl::prepare_world_map_texture_timer(timer,resource,1,&after)==S::UnsupportedLayout && same_timer(after,prior),"truncated duration header preserves output");}resource.size=8;
    resource.identity=2;expect(awl::prepare_world_map_texture_timer(timer,resource,1,&after)==S::RequiresResource && same_timer(after,prior),"foreign timer key rejects without dereference");resource.identity=1;
    expect(awl::prepare_world_map_texture_timer(timer,resource,1,nullptr)==S::InvalidInput,"null timer output rejects");
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::denorm_min()}){
        timer.rate_c=bad;expect(awl::prepare_world_map_texture_timer(timer,resource,1,&after)==S::UnsupportedNumerics && same_timer(after,prior),"nonfinite/subnormal reached rate preserves output");
        timer.rate_c=1;word(header,4,actor_bits(bad));expect(awl::prepare_world_map_texture_timer(timer,resource,1,&after)==S::UnsupportedNumerics && same_timer(after,prior),"nonfinite/subnormal reached duration preserves output");word(header,4,actor_bits(1));}
    timer.rate_c=std::numeric_limits<float>::max();expect(awl::prepare_world_map_texture_timer(timer,resource,UINT32_MAX,&after)==S::UnsupportedNumerics && same_timer(after,prior),"overflowing intermediate product cannot publish infinity");
    timer.rate_c=std::numeric_limits<float>::min();expect(awl::prepare_world_map_texture_timer(timer,resource,1,&after)==S::UnsupportedNumerics && same_timer(after,prior),"subnormal divided value stops explicitly");timer.rate_c=1;
    constexpr uint32_t types[]{0,57,58,59,UINT32_MAX,INT32_MAX,0x80000000},mouths[]{0,35,36,37,UINT32_MAX,INT32_MAX,0x80000000};
    constexpr uint32_t indices[]{UINT32_MAX,0,INT32_MAX,0x80000000};digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<1024;++seed){const auto now=nows[seed%6];const uint8_t gate=seed/196%2?uint8_t{255}:uint8_t{0};
        awl::WorldMapAnimationFeature38 feature;feature.type_0=types[seed%7];feature.type_4=mouths[seed/7%7];feature.index_8=indices[seed/49%4];feature.table_c=0xabcdef01;
        auto eyes=texture_records(1),mouth=eyes;std::array<uint32_t,6> initial{};
        for(uint32_t channel=0;channel<2;++channel){const auto stamp=now-deltas[(seed+channel*5)%13];const auto rate=rates[(seed/11+channel)%7],duration=durations[(seed/23+channel)%7];
            (channel?feature.second_24:feature.first_14)={0,stamp,7.25f,rate};word(channel?mouth:eyes,4,actor_bits(duration));
            initial[channel*3]=stamp;initial[channel*3+1]=actor_bits(rate);initial[channel*3+2]=actor_bits(duration);}
        std::shared_ptr<const awl::WorldMapPlayerTimerAssets> assets;
        expect(awl::decode_world_map_player_timer_assets(one_resource_table(eyes),one_resource_table(mouth),&assets)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"pair fixture retains exact common-bank keys");if(!assets)return;
        awl::WorldMapPlayerTimerResourceView eye_view,mouth_view;expect(assets->resource(T::Eyes,0,0,&eye_view) && assets->resource(T::Mouth,0,0,&mouth_view),"pair fixture resolves both resources");
        feature.first_14.resource_0=eye_view.identity;feature.second_24.resource_0=mouth_view.identity;awl::WorldMapAnimationFeature38 output;
        const auto result=awl::prepare_world_map_player_texture_timers(feature,gate,assets.get(),now,&output);
        expect(result.status==S::Prepared && !result.failed_timer && output.type_0==feature.type_0 && output.type_4==feature.type_4 && output.index_8==feature.index_8 && output.table_c==feature.table_c,
            "ordered timer pair preserves types/index/private table and publishes only reached timers");
        for(uint32_t w:{seed,feature.type_0,feature.type_4,feature.index_8,uint32_t(gate),now})hash(digest,w);for(auto w:initial)hash(digest,w);
        hash(digest,result.updated_mask);for(const auto* t:{&output.first_14,&output.second_24}){hash(digest,t->clock_4);hash(digest,actor_bits(t->value_8));}
        auto alias=feature;expect(awl::prepare_world_map_player_texture_timers(alias,gate,assets.get(),now,&alias).status==S::Prepared && same_feature(alias,output),"pair alias publishes identical complete state");}
    expect(digest==0x6F0127399FA59F4Aull,"1024 signed first/mouth/index/gate routes agree with whole original F6D4/E03C/A2FC instructions");
    std::cout<<"TEXTURE_TIMER_GATES 1024 "<<std::hex<<digest<<std::dec<<'\n';
    auto bytes=texture_records(1);word(bytes,4,actor_bits(1));std::shared_ptr<const awl::WorldMapPlayerTimerAssets> assets;
    expect(awl::decode_world_map_player_timer_assets(one_resource_table(bytes),one_resource_table(bytes),&assets)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"rollback fixture loads");if(!assets)return;
    awl::WorldMapPlayerTimerResourceView eye,mouth;expect(assets->resource(T::Eyes,0,0,&eye) && assets->resource(T::Mouth,0,0,&mouth),"rollback keys resolve");
    awl::WorldMapAnimationFeature38 feature;feature.type_0=58;feature.type_4=36;feature.index_8=UINT32_MAX;
    feature.first_14={eye.identity,0,9,1};feature.second_24={mouth.identity,0,8,1};auto output=feature;output.type_0=999;const auto untouched=output;
    const auto stopped=[&](S status,std::optional<uint8_t> gate,const awl::WorldMapPlayerTimerAssets* bank,std::optional<uint32_t> now){
        const auto result=awl::prepare_world_map_player_texture_timers(feature,gate,bank,now,&output);
        expect(result.status==status && result.updated_mask==0 && same_feature(output,untouched),"missing or invalid reached pair dependencies preserve the complete previous output");return result;};
    expect(stopped(S::RequiresClock,{},nullptr,{}).failed_timer==0u,"first timer clock is reached before the missing mouth gate");
    stopped(S::RequiresTimerAssets,{},nullptr,1000);feature.first_14.resource_0=999;stopped(S::RequiresResource,{},assets.get(),1000);feature.first_14.resource_0=eye.identity;
    expect(stopped(S::RequiresMouthGate,{},assets.get(),1000).failed_timer==1u,"late missing mouth byte rolls back a completed first timer");
    feature.index_8=0;stopped(S::RequiresMouthGate,{},assets.get(),1000);feature.index_8=UINT32_MAX;
    feature.second_24.resource_0=999;expect(stopped(S::RequiresResource,uint8_t{1},assets.get(),1000).failed_timer==1u,"late foreign mouth resource cannot publish first timer");
    const auto skipped=awl::prepare_world_map_player_texture_timers(feature,uint8_t{0},assets.get(),1000,&output);
    expect(skipped.status==S::Prepared && skipped.updated_mask==1 && output.first_14.value_8==0 && output.first_14.clock_4==1000 &&
        same_timer(output.second_24,feature.second_24),"mouth gate zero with negative index skips even an invalid mouth resource");
    feature.second_24.resource_0=mouth.identity;
    const int rounding=std::fegetround();for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
        expect(std::fesetround(mode)==0,"test selects nondefault timer rounding");output=untouched;
        stopped(S::UnsupportedNumerics,uint8_t{1},assets.get(),1000);
        auto unused=feature;unused.type_0=0;unused.type_4=0;unused.first_14.rate_c=std::numeric_limits<float>::quiet_NaN();
        const auto result=awl::prepare_world_map_player_texture_timers(unused,{},nullptr,{},&output);
        expect(result.status==S::Prepared && result.updated_mask==0 && same_feature(output,unused) && std::fegetround()==mode,
            "unreached timers ignore assets/clock/flags/numerics and preserve caller rounding");}
    expect(std::fesetround(rounding)==0,"test restores its original rounding");
    expect(awl::prepare_world_map_player_texture_timers(feature,uint8_t{1},assets.get(),1000,nullptr).status==S::InvalidInput,"null pair output rejects");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    allocation_probe::remaining=0;allocation_probe::enabled=true;
    const auto no_alloc=awl::prepare_world_map_player_texture_timers(feature,uint8_t{1},assets.get(),1000,&output);allocation_probe::enabled=false;
    expect(no_alloc.status==S::Prepared && no_alloc.updated_mask==3,"both timer updates require no heap allocation");
#endif
}
void texture_feature_checks() {
    texture_timer_checks();
    using S=awl::WorldMapTextureAnimationSampleStatus;using F=awl::WorldMapPlayerTextureFeatureStatus;
    using T=awl::WorldMapPlayerTimerBank;using U=awl::WorldMapPlayerPrimaryUpdateStatus;
    constexpr float values[]{-1,0,.5f,1,2,3,4,10};uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){auto bytes=texture_records(seed);const auto count=seed%6+1;const float value=values[seed/6%8];
        awl::WorldMapPlayerTimerResourceView view{1,0,uint32_t(bytes.size()),bytes.data()};awl::WorldMapTextureAnimationSample sample;
        expect(awl::sample_world_map_texture_animation(view,value,&sample)==S::Sampled,"bounded texture record selection succeeds");
        hash(digest,seed);hash(digest,count);hash(digest,actor_bits(value));for(uint32_t i=0;i<count;++i){hash(digest,(seed*7+i*11)%58);hash(digest,actor_bits(float((i*(seed%3+1))%7)));}
        for(uint32_t word:{sample.current,sample.next,sample.texture_index})hash(digest,word);
    }
    expect(digest==0x3870D6DCDE6E473Eull,"512 record selections match mapped 82F8 instructions, including strict boundaries and unsorted times");
    std::cout<<"PLAYER_TEXTURE_SAMPLE_COMPARISONS 512 "<<std::hex<<digest<<std::dec<<'\n';
    auto bytes=texture_records(2);awl::WorldMapPlayerTimerResourceView view{1,0,uint32_t(bytes.size()),bytes.data()};awl::WorldMapTextureAnimationSample sample{7,8,9};
    const auto preserved=[&](){return sample.current==7 && sample.next==8 && sample.texture_index==9;};
    for(uint32_t size=0;size<bytes.size();++size){view.size=size;
        expect(awl::sample_world_map_texture_animation(view,0,&sample)==S::UnsupportedLayout && preserved(),"truncated declared record extent preserves output");}view.size=uint32_t(bytes.size());
    word(bytes,0,0);expect(awl::sample_world_map_texture_animation(view,0,&sample)==S::UnsupportedLayout && preserved(),"zero count cannot emulate original unsafe pointer");word(bytes,0,UINT32_MAX);
    expect(awl::sample_world_map_texture_animation(view,0,&sample)==S::UnsupportedLayout && preserved(),"overflowing count rejects");word(bytes,0,3);
    word(bytes,20,0x7fc00000);expect(awl::sample_world_map_texture_animation(view,0,&sample)==S::UnsupportedNumerics && preserved(),"reached nonfinite next time rejects");word(bytes,20,1);
    expect(awl::sample_world_map_texture_animation(view,0,&sample)==S::UnsupportedNumerics && preserved(),"reached subnormal time rejects");
    word(bytes,20,actor_bits(1));word(bytes,28,0x7fc00000);word(bytes,12,0x7fc00000);
    expect(awl::sample_world_map_texture_animation(view,0,&sample)==S::Sampled && sample.current==0 && sample.next==1,"current time and later unused nonfinite time are unread");
    expect(awl::sample_world_map_texture_animation(view,1,&sample)==S::UnsupportedNumerics && sample.current==0,"equality advances to the next comparison and stops on reached invalid time");
    word(bytes,0,1);expect(awl::sample_world_map_texture_animation(view,std::numeric_limits<float>::quiet_NaN(),&sample)==S::Sampled && sample.current==0 && sample.next==0,
        "single-record sampling ignores even a nonfinite stored value and time");
    expect(awl::sample_world_map_texture_animation(view,0,nullptr)==S::InvalidInput,"null sampling output rejects");
    Fixture fixture;auto payloads=files();payloads[0]=setup_model(true);payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    payloads[3]=tpl(4);payloads[4]=tpl(58);payloads[5]=tpl(36);fixture.write("boy_0.arc",archive(payloads));std::shared_ptr<const Assets> assets;
    expect(awl::load_world_map_player_model_assets(0,&assets)==Status::Loaded,"texture feature banks load invented bounded TPLs");if(!assets)return;
    digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){std::shared_ptr<const awl::WorldMapPlayerTimerAssets> timers;
        expect(awl::decode_world_map_player_timer_assets(one_resource_table(texture_records(seed)),one_resource_table(texture_records(seed,1)),&timers)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"sampling fixture retains independent resources");
        if(!timers)return;awl::WorldMapPlayerTimerResourceView eye,mouth;expect(timers->resource(T::Eyes,0,0,&eye) && timers->resource(T::Mouth,0,0,&mouth),"sampling fixture resolves both keys");
        constexpr uint32_t eyes[]{0,1,15,54,55,56,57,58,59},mouths[]{0,1,35,36,37},modes[]{UINT32_MAX,0,0xfffffffe,INT32_MAX,0x80000000},sources[]{0,22,23};
        awl::WorldMapPlayerTextureFeatureInput input;input.feature.emplace();auto& f=*input.feature;
        f.type_0=eyes[seed%9];f.type_4=mouths[seed/9%5];f.index_8=modes[seed/45%5];input.source_10=sources[seed/7%3];input.timers=timers.get();
        f.first_14={eye.identity,UINT32_MAX,values[seed/6%8],std::numeric_limits<float>::quiet_NaN()};f.second_24={mouth.identity,0,values[(seed/6+3)%8],-100};
        const auto result=awl::prepare_world_map_player_texture_features(input,*assets);expect(result.status==F::Selected && result.textures[0] && result.textures[1],"supported F548 types and timer resources select eyes/mouth");
        hash(digest,seed);for(uint32_t word:{f.type_0,f.type_4,f.index_8,*input.source_10,actor_bits(f.first_14.value_8),actor_bits(f.second_24.value_8),
            result.textures[0]?result.textures[0]->index:UINT32_MAX,result.textures[1]?result.textures[1]->index:UINT32_MAX,result.textures[2]?result.textures[2]->index:UINT32_MAX})hash(digest,word);
    }
    expect(digest==0x5E3F1C4DE76E06ADull,"512 F548 routes match actual first/second sampler, bank lookup and special body branches");
    std::cout<<"PLAYER_TEXTURE_FEATURE_COMPARISONS 512 "<<std::hex<<digest<<std::dec<<'\n';
    awl::WorldMapPlayerTextureFeatureInput input;auto result=awl::prepare_world_map_player_texture_features(input,*assets);
    expect(result.status==F::RequiresFeature,"absent feature does not invent types");input.feature.emplace();input.feature->type_0=UINT32_MAX;
    result=awl::prepare_world_map_player_texture_features(input,*assets);expect(result.status==F::InvalidTexture && result.failed_channel==0u,"negative direct type cannot wrap into a host texture address");
    input.feature->type_0=0;result=awl::prepare_world_map_player_texture_features(input,*assets);expect(result.status==F::RequiresSource,"source is read after both texture selections");
    input.source_10=0;input.feature->type_4=36;result=awl::prepare_world_map_player_texture_features(input,*assets);
    expect(result.status==F::RequiresTimerAssets && result.failed_channel==1u,"unreached first timer is skipped, second requires retained assets");
    std::shared_ptr<const awl::WorldMapPlayerTimerAssets> timers;expect(awl::decode_world_map_player_timer_assets(sampled_timer_table(),sampled_timer_table(),&timers)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"complete single-record timers decode");input.timers=timers.get();
    result=awl::prepare_world_map_player_texture_features(input,*assets);expect(result.status==F::RequiresResource && result.failed_channel==1u,"unknown resource key never dereferences a host pointer or picks a default");
    awl::WorldMapPlayerTimerResourceView resource;expect(timers->resource(T::Mouth,0,0,&resource),"known mouth resource resolves");input.feature->second_24.resource_0=resource.identity;
    expect(awl::prepare_world_map_player_texture_features(input,*assets).status==F::Selected,"exact retained key resolves sampled mouth");
    input.feature->type_0=58;input.feature->type_4=UINT32_MAX;result=awl::prepare_world_map_player_texture_features(input,*assets);
    expect(result.status==F::RequiresResource && result.failed_channel==0u,"first timer resolution precedes validation of a direct second texture");
    input.feature->type_0=0;input.feature->type_4=0;
    std::unique_ptr<awl::WorldMapPlayerPrimaryOwner> owner;expect(awl::construct_world_map_player_primary(assets,&owner).status==awl::WorldMapPlayerPrimaryStatus::ConstructedCpuPrimary,"texture cache CPU primary constructs");if(!owner)return;
    digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<256;++seed){awl::WorldMapPlayerTextureFeatureInput direct;direct.feature.emplace();
        direct.feature->type_0=std::array<uint32_t,4>{0,15,55,57}[seed%4];direct.feature->type_4=seed/4%36;direct.source_10=seed&1?22u:0u;
        const auto selected=awl::prepare_world_map_player_texture_features(direct,*assets);
        constexpr uint32_t patterns[4][4]{{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},{0,1,2,255},{8,9,10,11},{0,0,UINT32_MAX,UINT32_MAX}};
        std::optional<std::array<awl::WorldMapPlayerTextureCacheEntry,4>> cache;cache.emplace();
        for(size_t i=0;i<4;++i){auto& entry=(*cache)[i];entry.channel=patterns[seed/8%4][i];
            if(entry.channel!=UINT32_MAX)entry.texture=awl::WorldMapPlayerModelTextureBinding{uint8_t(entry.channel),Bank::Body,uint16_t(100+i),assets->texture_bank_identity(Bank::Body)};}
        auto after=cache;uint32_t writes=999;expect(awl::prepare_world_map_player_texture_cache(owner->setup(),selected,cache,&after,&writes),"retained feature commands stage shared-cache routes");
        for(uint32_t word:{seed,direct.feature->type_0,direct.feature->type_4,*direct.source_10})hash(digest,word);
        for(auto channel:patterns[seed/8%4])hash(digest,channel);hash(digest,writes);
        for(const auto& entry:*after){hash(digest,entry.channel);hash(digest,entry.texture?uint32_t(entry.texture->bank):UINT32_MAX);hash(digest,entry.texture?entry.texture->index:UINT32_MAX);}
    }
    expect(digest==0xD71566EA9DDB3AA5ull,"256 complete shared-cache routes match actual root/list/command/slot instructions and repeated store counts");
    std::cout<<"PLAYER_TEXTURE_CACHE_COMPARISONS 256 "<<std::hex<<digest<<std::dec<<'\n';
    awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(3);frame.root_pose=explicit_pose({1,0,0,10,0,1,0,20,0,0,1,30});
    auto update=owner->advance(frame,nullptr,&input);expect(update.status==U::Advanced && update.texture_features && update.texture_features->cache_writes==8 && owner->texture_cache() &&
        (*owner->texture_cache())[0].channel==0 && (*owner->texture_cache())[1].channel==1,"ordered root plus three features publish both channels into a shared four-slot cache");
    const auto prior=primary_snapshot(*owner);frame.root_pose=explicit_pose({1,0,0,40,0,1,0,50,0,0,1,60});input.source_10.reset();
    update=owner->advance(frame,nullptr,&input);expect(update.status==U::TextureFeaturesFailure && update.texture_features && update.texture_features->status==F::RequiresSource && primary_snapshot(*owner)==prior,
        "missing post-frame source preserves prior root/features/skin/mesh/cache after frame preparation");
    frame.node_post_transforms.back()=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,std::numeric_limits<float>::quiet_NaN()};
    update=owner->advance(frame,nullptr,&input);expect(update.status==U::FrameFailure && !update.texture_features && primary_snapshot(*owner)==prior,"late frame failure precedes missing post-frame feature dependency");frame.node_post_transforms.back().reset();input.source_10=22;
    update=owner->advance(frame,nullptr,&input);expect(update.status==U::Advanced && update.texture_features && update.texture_features->cache_writes==12 && (*owner->texture_cache())[2].channel==2 && (*owner->texture_cache())[2].texture->index==1,
        "source 0x16 adds body index one after existing eye/mouth slot updates");
    auto cache=owner->texture_cache();for(size_t i=0;i<4;++i)(*cache)[i].channel=uint32_t(8+i);auto after=cache;uint32_t writes=999;
    expect(awl::prepare_world_map_player_texture_cache(owner->setup(),*update.texture_features,cache,&after,&writes) && writes==0 && (*after)[0].channel==8 && (*after)[3].channel==11,
        "full shared cache is a successful no-op");
    cache.emplace();(*cache)[0].channel=0;(*cache)[1].channel=0;
    expect(awl::prepare_world_map_player_texture_cache(owner->setup(),*update.texture_features,cache,&cache,&writes) && writes==12 && (*cache)[0].texture && !(*cache)[1].texture && (*cache)[2].channel==1 && (*cache)[3].channel==2,
        "duplicate channels update first match, then first free slot; whole-cache alias and repeated stores are supported");
    auto invalid=*update.texture_features;invalid.textures[0].reset();writes=999;
    expect(!awl::prepare_world_map_player_texture_cache(owner->setup(),invalid,cache,&after,&writes) && writes==999 && (*after)[0].channel==8,
        "incomplete selected route preserves previous cache output and write count");
    invalid=*update.texture_features;invalid.textures[1]->bank_identity=0;
    expect(!awl::prepare_world_map_player_texture_cache(owner->setup(),invalid,cache,&after,&writes) && writes==999 && (*after)[3].channel==11,
        "foreign texture binding rejects atomically before any cache staging stores");
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
    expect(holder->advance_texture_timers().status==awl::WorldMapTextureTimerStatus::RequiresClock && holder_preserved(),
        "a construction clock snapshot cannot substitute for a retained live clock during timer progression");
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
uint64_t camera_snapshot(const awl::WorldMapPlayerStartupCameraState& state) {
    uint64_t h=14695981039346656037ull;
    const auto real=[&](float value){uint32_t w;std::memcpy(&w,&value,4);hash(h,w);};
    const auto vector=[&](const auto& values){for(float v:values)real(v);};
    const auto view=[&](const awl::WorldMapCameraView& v){vector(v.target.raw);vector(v.target.bounded);hash(h,v.target.clamped);
        vector(v.second_point);vector(v.rotated_up);vector(v.matrix_50);};
    const auto& q=state.camera;const auto& c=q.target.camera;vector(c.position);real(c.field_18);real(c.yaw);vector(c.bounds_min);vector(c.bounds_max);
    hash(h,c.flag_98);hash(h,c.flag_99);vector(q.target.origin_offset_0c);real(q.target.distance_30);real(q.target.pitch_offset_8c);real(q.target.yaw_offset_90);
    vector(q.up_vector_24);real(q.pitch_offset_44);real(q.yaw_offset_48);
    const auto& u=state.update;view(u.first_view);vector(u.first_plane.normal);real(u.first_plane.constant);real(u.temporary_pitch_offset_8c);view(u.pitched_view);
    vector(u.final_plane.normal);real(u.final_plane.constant);vector(u.final_target);vector(u.final_matrix_50);hash(h,u.terrain_clamped);
    const auto key=reinterpret_cast<uintptr_t>(state.collision.get());hash(h,uint32_t(uint64_t(key)>>32));hash(h,uint32_t(key));return h;
}
uint64_t startup_snapshot(const awl::WorldMapPlayerStartup& owner) {
    auto h=holder_snapshot(owner.model());const auto& s=owner.state();
    hash(h,uint32_t(s.state_1364));hash(h,s.counter_1368);hash(h,s.secondary_byte_3f0);
    hash(h,s.control_word_1384.has_value());hash(h,s.control_word_1384.value_or(0));
    hash(h,s.target_14ac.has_value());hash(h,s.target_14ac.value_or(0));hash(h,s.direction_14b4.has_value());hash(h,s.direction_14b4.value_or(0));
    hash(h,owner.state0_context().has_value());if(owner.state0_context()){
        const auto& context=*owner.state0_context();for(uint32_t v:context.labels_128)hash(h,v);for(uint32_t v:context.target_1388)hash(h,v);
        hash(h,context.byte_1350);hash(h,context.secondary_byte_574);hash(h,context.secondary_word_564);
        hash(h,context.ui.resource_manager_1278.has_value());hash(h,uint32_t(context.ui.resource_manager_1278.value_or(0)>>32));hash(h,uint32_t(context.ui.resource_manager_1278.value_or(0)));
        for(const auto& v:context.ui.labels_60){hash(h,v.has_value());hash(h,v.value_or(UINT32_MAX));}
        for(const auto& e:context.ui.elements){hash(h,e.has_value());if(e){
            hash(h,uint32_t(reinterpret_cast<uintptr_t>(e->resources.get())));hash(h,uint32_t(reinterpret_cast<uintptr_t>(e->clock.get())));
            hash(h,e->flags_120.value_or(UINT32_MAX));hash(h,e->animation_deadline);hash(h,e->image_group.offset);hash(h,e->icon_frame.offset);
            for(float f:e->position_f0){uint32_t v;std::memcpy(&v,&f,4);hash(h,v);}
        }}
    }
    hash(h,owner.state0_third_loop().has_value());if(owner.state0_third_loop()){
        const auto& third=*owner.state0_third_loop();hash(h,third.loop_count);hash(h,third.raw_time);
        hash(h,uint32_t(third.inputs_owner>>32));hash(h,uint32_t(third.inputs_owner));hash(h,uint32_t(third.completion));hash(h,uint32_t(third.state));}
    hash(h,owner.animation_loop().has_value());if(owner.animation_loop()){
        hash(h,owner.animation_loop()->loop_count);hash(h,owner.animation_loop()->raw_time);hash(h,owner.animation_loop()->valid);}
    hash(h,owner.primary_loop().has_value());if(owner.primary_loop()){
        hash(h,owner.primary_loop()->loop_count);hash(h,owner.primary_loop()->raw_time);hash(h,owner.primary_loop()->valid);
        hash(h,uint32_t(owner.primary_loop()->updates_owner>>32));hash(h,uint32_t(owner.primary_loop()->updates_owner));
        hash(h,owner.primary_loop()->followups_completed);
        hash(h,uint32_t(owner.primary_loop()->fade_owner>>32));hash(h,uint32_t(owner.primary_loop()->fade_owner));
        hash(h,owner.primary_loop()->queued_completed);
        hash(h,uint32_t(owner.primary_loop()->inputs_owner>>32));hash(h,uint32_t(owner.primary_loop()->inputs_owner));
        hash(h,owner.primary_loop()->pending_state_request.has_value());hash(h,uint32_t(owner.primary_loop()->pending_state_request.value_or(-1)));
        hash(h,owner.primary_loop()->state0_transition_completed);hash(h,owner.primary_loop()->state0_parent_completed);
        hash(h,owner.primary_loop()->state0_secondary_completed);hash(h,owner.primary_loop()->state1_transition_completed);
        hash(h,owner.primary_loop()->state1_parent_completed);hash(h,owner.primary_loop()->state1_secondary_completed);
        for(uint64_t key:{owner.primary_loop()->movement_owner,owner.primary_loop()->scene_owner}){hash(h,uint32_t(key>>32));hash(h,uint32_t(key));}}
    if(owner.primary_loop())for(const auto& v:{owner.primary_loop()->item_144,owner.primary_loop()->action_148}){hash(h,v.has_value());hash(h,v.value_or(0));}
    const auto key=reinterpret_cast<uintptr_t>(owner.starts().get());hash(h,uint32_t(uint64_t(key)>>32));hash(h,uint32_t(key));
    hash(h,owner.camera().has_value());if(owner.camera()){const auto camera=camera_snapshot(*owner.camera());hash(h,uint32_t(camera>>32));hash(h,uint32_t(camera));}
    const auto& actor=owner.actor_metadata();hash(h,actor.flag_15d);hash(h,actor.flag_15e);hash(h,actor.flag_15f);
    hash(h,actor.effect_wrapper.has_value());hash(h,uint32_t(actor.effect_wrapper.value_or(0)>>32));hash(h,uint32_t(actor.effect_wrapper.value_or(0)));return h;
}
struct State29HeightProbe {
    unsigned calls=0,fail_at=0;bool nonfinite=false;
    std::array<std::array<float,3>,2> positions{};
    static bool sample(const std::array<float,3>& p,float* height,void* context){auto& s=*static_cast<State29HeightProbe*>(context);
        if(s.calls<2)s.positions[s.calls]=p;++s.calls;if(s.calls==s.fail_at)return false;
        *height=s.nonfinite?std::numeric_limits<float>::quiet_NaN():0.0f;return true;}
};
uint64_t update_owners_snapshot(const awl::WorldMapPlayerUpdateOwners& owners) {
    uint64_t h=14695981039346656037ull;
    const auto vector=[&](const auto& values){for(float v:values){uint32_t bits;std::memcpy(&bits,&v,4);hash(h,bits);}};
    hash(h,uint32_t(owners.saved_pose.scene_type));vector(owners.saved_pose.position);vector(owners.saved_pose.heading);
    vector(owners.position_802fc20c);hash(h,owners.scene_one_position_240.has_value());
    if(owners.scene_one_position_240)vector(*owners.scene_one_position_240);
    if(owners.status){const auto& s=*owners.status;hash(h,1);
        for(uint32_t v:{s.word_4,s.word_8,s.word_c,s.word_14,s.word_18,s.word_1c,s.word_20,s.word_24,s.word_28,
            uint32_t(s.byte_2e),uint32_t(s.byte_32),s.word_12c,s.word_150,s.word_154,s.word_160})hash(h,v);}
    if(owners.status){const auto& s=*owners.status;for(const auto& v:{s.word_134,s.word_13c}){hash(h,v.has_value());hash(h,v.value_or(0));}
        for(const auto& v:{s.byte_14c,s.byte_158,s.byte_159,s.byte_15a,s.byte_15b,s.byte_15c}){hash(h,v.has_value());hash(h,v.value_or(0));}}
    if(owners.saved_selection){hash(h,1);for(const auto& v:{owners.saved_selection->item_144,owners.saved_selection->action_148}){hash(h,v.has_value());hash(h,v.value_or(0));}}
    hash(h,owners.item_owner.has_value());if(owners.item_owner)hash(h,owners.item_owner->mask_88);
    hash(h,uint32_t(owners.queue.size()));for(const auto& e:owners.queue){hash(h,uint32_t(e.node_identity>>32));hash(h,uint32_t(e.node_identity));hash(h,e.priority);}return h;
}
uint32_t fade_color_word(const awl::WorldMapFadeColor& color) {
    uint32_t word=0;for(auto byte:color)word=(word<<8)|byte;return word;
}
uint64_t actor_queue_snapshot(const awl::WorldMapPlayerActorQueue& queue);
uint64_t fade_snapshot(const awl::WorldMapFade& fade) {
    uint64_t h=14695981039346656037ull;const auto& s=fade.state();
    for(uint32_t word:{fade_color_word(s.from_4c),fade_color_word(s.to_50),uint32_t(s.color_54.has_value()),
        s.color_54?fade_color_word(*s.color_54):0u,actor_bits(s.duration_58),uint32_t(s.started_5c.has_value()),s.started_5c.value_or(0),
        uint32_t(s.mode_60),uint32_t(s.hold_64.has_value()),uint32_t(s.hold_64.value_or(0)),s.render_priority_68,uint32_t(s.registered)})hash(h,word);
    hash(h,fade.loop().has_value());if(fade.loop()){hash(h,fade.loop()->loop_count);hash(h,fade.loop()->raw_time);hash(h,fade.loop()->valid);
        hash(h,uint32_t(fade.loop()->render_owner>>32));hash(h,uint32_t(fade.loop()->render_owner));
        hash(h,uint32_t(fade.loop()->inputs_owner>>32));hash(h,uint32_t(fade.loop()->inputs_owner));}return h;
}
void fade_checks() {
    using S=awl::WorldMapFadeStatus;
    const std::array<float,8> durations{0,.1f,1,7,1000,1000.0f/60.0f,std::numeric_limits<float>::max(),1.e-20f};
    const std::array<uint32_t,12> elapsed_values{0,1,99,100,101,999,1000,1001,UINT32_MAX,16777216,16777217,250000};
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<2304;++seed) {
        awl::WorldMapFadeState state;state.mode_60=int32_t(seed%3);state.duration_58=durations[seed/3%8];
        const auto elapsed=elapsed_values[seed/24%12];state.hold_64=uint8_t(seed/288%2?255:0);
        state.started_5c=std::array<uint32_t,3>{0,0xfffffff5,1000}[seed%3];state.registered=true;
        state.color_54=awl::WorldMapFadeColor{0xaa,0xbb,0xcc,0xdd};
        for(size_t i=0;i<4;++i){state.from_4c[i]=uint8_t(seed*37+i*71);state.to_50[i]=uint8_t(seed*53+i*19);}
        awl::WorldMapFadeStep step;
        const auto status=awl::prepare_world_map_fade_update(state,*state.started_5c+elapsed,&step);
        expect(status==(state.mode_60==0?S::Idle:S::Prepared) && step.after.color_54,"complete fade sample prepares or preserves its supplied color");
        for(uint32_t word:{seed,uint32_t(state.mode_60),elapsed,actor_bits(state.duration_58),uint32_t(*state.hold_64),uint32_t(step.after.mode_60),
            uint32_t(step.after.registered),step.after.color_54?fade_color_word(*step.after.color_54):0u,uint32_t(step.schedule_render)})hash(digest,word);
    }
    expect(digest==0x28fd93479cc69424ull,"2304 original fade updates match timing, color, primary removal and render scheduling");
    std::cout<<"WORLD_MAP_FADE_UPDATE 2304 "<<std::hex<<digest<<std::dec<<'\n';
    digest=14695981039346656037ull;
    const std::array<float,8> seconds{0,.001f,.1f,1,7,1000,1.0f/60.0f,16777.217f};
    for(uint32_t seed=0;seed<1024;++seed) {
        awl::WorldMapFadeState state;state.color_54=awl::WorldMapFadeColor{0xaa,0xbb,0xcc,0xdd};
        awl::WorldMapFadeColor from,to;for(size_t i=0;i<4;++i){from[i]=uint8_t(seed*37+i*71);to[i]=uint8_t(seed*53+i*19);}
        const auto duration=seconds[seed%8];const auto now=std::array<uint32_t,4>{0,1,0xfffffffe,16777217}[seed/8%4];
        const auto hold=uint8_t(seed/32%2?255:0),front=uint8_t(seed/64%2?2:0);
        expect(awl::prepare_world_map_fade_start(state,duration,from,to,hold,front,now,&state)==S::Prepared,"fade start permits whole-state aliases");
        for(uint32_t word:{seed,actor_bits(duration),now,uint32_t(hold),uint32_t(front),fade_color_word(state.from_4c),fade_color_word(state.to_50),
            fade_color_word(*state.color_54),actor_bits(state.duration_58),*state.started_5c,uint32_t(state.mode_60),uint32_t(*state.hold_64),
            state.render_priority_68,uint32_t(state.registered),0x10000u})hash(digest,word);
    }
    expect(digest==0xcd5a9c9373a103e5ull,"1024 fade starts match original fields and primary registration");
    auto clock=std::make_shared<awl::GameClock>();awl::WorldMapFade fade(clock);awl::WorldMapUpdateInputs inputs(clock),missing(clock),foreign(std::make_shared<awl::GameClock>());
    expect(fade.state().mode_60==0 && !fade.state().registered && !fade.state().color_54 && !fade.state().started_5c && !fade.state().hold_64 &&
        fade.state().duration_58==0 && fade.state().render_priority_68==0xcffe && fade.advance_for_loop(missing,nullptr)==S::Idle,
        "fresh fade preserves constructor-unwritten fields and inactive membership skips capture/queue");
    expect(fade.start_world_map_entry()==S::Started && fade.state().from_4c==awl::WorldMapFadeColor{0,0,0,255} &&
        fade.state().to_50==awl::WorldMapFadeColor{} && fade.state().duration_58==1000 && fade.state().mode_60==1,
        "world-map setup starts one-second opaque-to-transparent black with no hold");
    auto before=fade_snapshot(fade);awl::WorldMapPlayerActorQueue queue{{7,0x2002},{8,0xcffe},{9,UINT32_MAX}};
    const auto queue_before=actor_queue_snapshot(queue);
    expect(fade.advance_for_loop(missing,&queue)==S::RequiresControl && fade.advance_for_loop(foreign,&queue)==S::InvalidInput &&
        fade_snapshot(fade)==before && actor_queue_snapshot(queue)==queue_before,"missing or foreign fade control preserves state and render queue");
    inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{0}},0});
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"fade uses the same captured raw-loop boundary as player/model");
    expect(fade.advance_for_loop(inputs,nullptr)==S::RequiresRenderQueue && fade_snapshot(fade)==before,"reached queue is required before publishing fade color or membership");
    awl::WorldMapPlayerActorQueue bad{{0,0}};
    expect(fade.advance_for_loop(inputs,&bad)==S::InvalidQueue && fade_snapshot(fade)==before,"invalid render queue cannot consume the fade sample");
    bool accepted=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t failed=0;
    for(size_t fail=0;fail<8;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto status=fade.advance_for_loop(inputs,&queue);allocation_probe::enabled=false;
        if(status==S::Advanced){accepted=true;break;}++failed;
        expect(status==S::AllocationFailure && fade_snapshot(fade)==before && actor_queue_snapshot(queue)==queue_before && allocation_probe::live==live,
            "all fade queue allocation failures preserve state/receipt/queue and release staging");}
    expect(accepted && failed==2,"fade render queue stages copying and growth before publication");
    std::cout<<"WORLD_MAP_FADE_ALLOCATION_FAILURES "<<failed<<'\n';
#endif
    if(!accepted)expect(fade.advance_for_loop(inputs,&queue)==S::Advanced,"first eligible fade update publishes");
    expect(fade.state().color_54==awl::WorldMapFadeColor{0,0,0,255} && queue.size()==4 && queue[1].node_identity==fade.render_node_identity() &&
        queue[1].priority==0xcffe && queue[2].node_identity==8,"fade starts at source color and inserts before equal-priority render work");
    before=fade_snapshot(fade);const auto retained_queue=actor_queue_snapshot(queue);auto other=queue;
    awl::WorldMapUpdateInputs alternate_inputs(clock);alternate_inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{0}},0});
    expect(alternate_inputs.capture_control()==awl::WorldMapControlStatus::Captured && fade.advance_for_loop(alternate_inputs,&queue)==S::InvalidInput && fade_snapshot(fade)==before,
        "a second captured-control owner on the same clock cannot replace an accepted fade sample");
    expect(fade.advance_for_loop(inputs,&other)==S::InvalidInput && fade.advance_for_loop(inputs,nullptr)==S::AlreadyAdvanced &&
        fade_snapshot(fade)==before && actor_queue_snapshot(queue)==retained_queue,"same-loop receipt prevents repeat work and rejects a different queue owner");
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"fade test selects unsupported arithmetic");
    expect(fade.start_world_map_entry()==S::UnsupportedNumerics && fade_snapshot(fade)==before,"unsupported start arithmetic preserves accepted fade/receipt");
    expect(std::fesetround(rounding)==0,"fade test restores caller rounding");
    expect(fade.start(-1,{},{},0,0)==S::InvalidInput && fade.start(std::numeric_limits<float>::max(),{},{},0,0)==S::InvalidInput &&
        fade_snapshot(fade)==before,"invalid/overflowing durations preserve previous fade");
    // 60 retraces produce an exactly represented raw increment of 1000.
    clock->set_retrace_interval(60);expect(clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"fade fixture reaches exact duration");
    expect(fade.advance_for_loop(inputs,&queue)==S::Advanced && fade.state().mode_60==1 && fade.state().registered && fade.state().color_54==awl::WorldMapFadeColor{},
        "exactly 100 percent keeps active mode one while color reaches the endpoint");
    clock->set_retrace_interval(1);expect(clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"fade fixture moves strictly beyond duration");
    expect(fade.advance_for_loop(inputs,&queue)==S::Advanced && fade.state().mode_60==0 && !fade.state().registered && fade.state().color_54==awl::WorldMapFadeColor{} &&
        queue.size()==4 && fade.advance_for_loop(inputs,nullptr)==S::AlreadyAdvanced,"first sample above duration unregisters primary but queues final color exactly once");
    expect(clock->advance() && fade.advance_for_loop(inputs,nullptr)==S::Idle,"later inactive loops skip stale capture and render work");
    expect(fade.start(0,{1,2,3,4},{5,6,7,8},255,2)==S::Started && inputs.capture_control()==awl::WorldMapControlStatus::Captured,
        "zero-time held fade can restart with front render priority");
    before=fade_snapshot(fade);
    expect(fade.advance_for_loop(inputs,&queue)==S::UnsupportedPriorityChange && fade_snapshot(fade)==before,"unconsumed same-node render priority changes stop explicitly");
    queue.clear();expect(fade.advance_for_loop(inputs,&queue)==S::Advanced && fade.state().mode_60==2 && fade.state().registered &&
        fade.state().color_54==awl::WorldMapFadeColor{5,6,7,8} && queue[0].priority==0xdfff,"zero-time hold clamps endpoint and retains active membership");
    expect(fade.start_world_map_entry()==S::Started && fade.advance_for_loop(inputs,&queue)==S::RequiresNextLoop,"restart after an accepted callback cannot run retroactively in the same loop");
    awl::WorldMapFadeState state;state.mode_60=1;state.duration_58=0;awl::WorldMapFadeStep step;step.after.mode_60=77;
    expect(awl::prepare_world_map_fade_update(state,{},&step)==S::RequiresHold && step.after.mode_60==77,"zero duration skips clock/start but needs hold at completion");
    state.duration_58=1;expect(awl::prepare_world_map_fade_update(state,{},&step)==S::RequiresClock && step.after.mode_60==77,"nonzero duration needs raw time before start time");
    expect(awl::prepare_world_map_fade_update(state,0,&step)==S::RequiresStartTime && step.after.mode_60==77,"missing constructor-unwritten start time preserves output");
    state.mode_60=2;state.duration_58=std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_fade_update(state,{},&step)==S::Prepared && step.after.color_54==state.to_50,"held mode never reads old duration/start/hold");
    state.mode_60=1;state.duration_58=1000;state.started_5c=0;
    expect(awl::prepare_world_map_fade_update(state,1000,&step)==S::Prepared && step.after.mode_60==1,
        "exact duration does not read constructor-unwritten hold byte");
    expect(awl::prepare_world_map_fade_update(state,1001,&step)==S::RequiresHold,"first sample beyond exact duration requires hold before mode/membership writes");
    state.duration_58=-1;step.after.mode_60=77;
    expect(awl::prepare_world_map_fade_update(state,1001,&step)==S::InvalidInput && step.after.mode_60==77,"negative reached duration is explicitly unsupported without mutation");
    state.mode_60=0;
    expect(awl::prepare_world_map_fade_update(state,{},&step)==S::Idle && !step.schedule_render,"idle mode skips invalid duration and missing timing fields");
    state.mode_60=-1;expect(awl::prepare_world_map_fade_update(state,{},&step)==S::UnsupportedMode,"unknown original mode cannot use an undefined incoming float register");
    awl::WorldMapFade no_clock(nullptr);expect(no_clock.start_world_map_entry()==S::RequiresClock && !no_clock.state().registered,"missing owned raw clock cannot activate the fade");
    expect(fade.primary_node_identity()!=fade.render_node_identity(),"fade retains distinct stable primary/render keys");
    clock->set_retrace_interval(0);expect(clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"restarted fade samples a distinct zero-time loop");
    inputs.request_scene_mode(4,0);awl::WorldMapSceneModeCommit commit;
    expect(inputs.commit_scene_fields(0,uint8_t{1},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields &&
        inputs.dispatch(0x5001).status==awl::WorldMapDispatchStatus::Filtered && inputs.dispatch(0x10000).status==awl::WorldMapDispatchStatus::Ready,
        "scene filtering may block the player while fade group16 remains eligible");
    queue.clear();expect(fade.advance_for_loop(inputs,&queue)==S::Advanced && fade.state().mode_60==1 && fade.state().color_54==awl::WorldMapFadeColor{0,0,0,255},
        "restarted fade progresses in the next eligible zero-time loop without inheriting old completion or player filtering");
    auto sequence_clock=std::make_shared<awl::GameClock>();awl::WorldMapFade sequence(sequence_clock);awl::WorldMapUpdateInputs sequence_inputs(sequence_clock);
    sequence_inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{0}},0});
    expect(sequence.start_world_map_entry()==S::Started,"bounded fade/player sequence starts on the justified clear setup branch");
    awl::WorldMapPlayerActorQueue sequence_queue;digest=14695981039346656037ull;uint32_t first_transition=UINT32_MAX;
    for(uint32_t tick=0;tick<64;++tick) {
        expect(sequence_inputs.capture_control()==awl::WorldMapControlStatus::Captured && sequence.advance_for_loop(sequence_inputs,&sequence_queue)==S::Advanced,
            "each eligible sequence loop updates fade before the player secondary decision");
        awl::WorldMapPlayerState29SecondaryInput secondary;secondary.guards=awl::WorldMapPlayerUpdateGuards{-1,0};
        secondary.mode_word_60=sequence.state().mode_60;secondary.scene_mode_60=0;secondary.secondary_byte_3f2=uint8_t{0};secondary.item_present=false;
        const auto decision=awl::prepare_world_map_player_state29_secondary(secondary);
        const auto request=uint32_t(decision.requested_state.value_or(-1));
        if(request!=UINT32_MAX && first_transition==UINT32_MAX)first_transition=tick;
        for(uint32_t word:{tick,sequence_clock->state().raw_time,uint32_t(sequence.state().mode_60),fade_color_word(*sequence.state().color_54),
            uint32_t(sequence.state().registered),request,uint32_t(!sequence_queue.empty())})hash(digest,word);
        sequence_queue.clear(); // Supplied render completion, not a presented frame.
        expect(sequence_clock->advance(),"bounded sequence advances the raw clock after supplied queue completion");
    }
    expect(digest==0xb77643c95bdf85e5ull && first_transition==63,"64-loop fade/player/clock sequence matches original state-zero request at loop63");
    std::cout<<"WORLD_MAP_FADE_PLAYER_SEQUENCE 64 "<<std::hex<<digest<<std::dec<<" first state-zero request "<<first_transition<<'\n';
}
std::vector<uint8_t> context_tables_dol() {
    std::vector<uint8_t> bytes(0x504,0);
    for(const auto& section:std::array<std::array<uint32_t,3>,3>{{{{0x100,0x8023df88,0x3f0}},{{0x4f0,0x80349e98,8}},{{0x4f8,0x8025113c,12}}}}){
        const auto i=section[0]==0x100?0u:section[0]==0x4f0?1u:2u;
        word(bytes,i*4,section[0]);word(bytes,0x48+i*4,section[1]);word(bytes,0x90+i*4,section[2]);}
    for(uint32_t slot=0;slot<8;++slot)word(bytes,0x100+slot*4,slot);
    for(uint32_t key=1;key<=16;++key){const auto base=0x2b0+(key-1)*36;word(bytes,base,key);
        for(uint32_t slot=0;slot<8;++slot)word(bytes,base+4+slot*4,slot==0?key%16+1:slot==2?key:slot%2?0u:UINT32_MAX);}
    word(bytes,0x4f0,0xbf3504f3);word(bytes,0x4f4,0x3f3504f3);return bytes;
}
std::array<uint32_t,13> context_words(const awl::WorldMapPlayerContextResult& result) {
    const auto& r=result.record;return {uint32_t(result.found),actor_bits(result.nearest_squared_distance),r.owner_code,r.owner_kind,r.payload[0],r.payload[1],
        actor_bits(r.position[0]),actor_bits(r.position[1]),actor_bits(r.position[2]),actor_bits(r.facing[0]),actor_bits(r.facing[1]),actor_bits(r.facing[2]),r.direction};
}
void context_matrix(const awl::WorldMapPlayerContextTables& tables,uint64_t expected) {
    using S=awl::WorldMapPlayerContextStatus;
    uint64_t digest=14695981039346656037ull;
    const std::array<awl::WorldMapPlayerContextVector,4> forwards{{{0,0,1},{1,0,0},{0,0,0},{1,0,1}}};
    const std::array<awl::WorldMapPlayerContextVector,4> facings{{{0,0,-1},{0,0,0},{1,0,0},{0,1,0}}};
    for(uint32_t seed=0;seed<512;++seed){awl::WorldMapPlayerContextQuery query;query.forward=forwards[seed%4];
        query.origin=awl::WorldMapPlayerContextVector{float(int(seed%3)-1),0,float(int(seed/3%3)-1)};
        query.angle_radians=std::array<float,4>{0,0x1.921fb6p-1f,0x1.921fb6p+0f,0x1.921fb6p+1f}[seed/4%4];
        query.min_distance=float(seed/16%2);query.max_distance=float(1+seed/32%4);query.excluded_kinds=std::array<uint32_t,4>{0,1,2,1u<<31}[seed/128%4];query.current_key=1+int32_t(seed%16);
        std::vector<awl::WorldMapPlayerContextList> lists;
        for(uint32_t group=1;group<=16;++group){awl::WorldMapPlayerContextList list;list.key=int32_t(group);
            for(uint32_t i=0;i<3;++i){const auto n=seed*17+group*13+i*7;awl::WorldMapPlayerContextCandidate candidate;
                candidate.position_1c=awl::WorldMapPlayerContextVector{float(int(n%7)-3),float(int(n/7%3)-1),float(int(n/21%7)-3)};
                candidate.facing_28=facings[n%4];candidate.facing_threshold_38=std::array<float,5>{-1,-.5f,0,.5f,1}[n/4%5];
                candidate.max_squared_distance_3c=std::array<float,5>{1,4,9,16,100}[n/20%5];candidate.owner_kind_8=std::array<uint32_t,5>{0,1,31,32,64}[n%5];
                candidate.owner_code_4=1000+group*10+i;candidate.payload=std::array<uint32_t,2>{UINT32_MAX,UINT32_MAX};
                const auto shift=*candidate.owner_kind_8&63u;
                if(query.excluded_kinds && shift<32 && (query.excluded_kinds&(1u<<shift))){candidate.position_1c.reset();candidate.facing_28.reset();
                    candidate.facing_threshold_38.reset();candidate.max_squared_distance_3c.reset();candidate.owner_code_4.reset();candidate.payload.reset();}
                list.candidates.push_back(candidate);}
            lists.push_back(std::move(list));}
        awl::WorldMapPlayerContextResult result;
        expect(awl::query_world_map_player_context(&tables,lists,query,&result).status==S::Prepared,"supplied context scene prepares under the documented native numerical profile");
        if(std::getenv("AWL_CONTEXT_TRACE")){std::cout<<"CONTEXT_ROW "<<seed;for(auto value:context_words(result))std::cout<<' '<<std::hex<<value<<std::dec;std::cout<<'\n';}
        hash(digest,seed);for(auto word_value:context_words(result))hash(digest,word_value);
    }
    expect(digest==expected,"512 complete context results match original query instructions under the explicit native numerical substitutions");
    std::cout<<"PLAYER_CONTEXT_QUERY_NATIVE_PROFILE 512 "<<std::hex<<digest<<std::dec<<'\n';
}
void context_registry_sequence(const awl::WorldMapPlayerContextTables& tables) {
    using S=awl::WorldMapPlayerContextStatus;
    const std::array<awl::WorldMapPlayerContextVector,4> forwards{{{0,0,1},{1,0,0},{0,0,0},{1,0,1}}};
    const std::array<awl::WorldMapPlayerContextVector,4> facings{{{0,0,-1},{0,0,0},{1,0,0},{0,1,0}}};
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<128;++seed){awl::WorldMapSceneBucketRegistry registry;awl::WorldMapScenePositionUpdate update;
        const int32_t scene_type=seed%2?3:1;
        for(uint32_t i=0;i<3;++i){awl::WorldMapPlayerContextMetadata metadata;metadata.owner_code_4=101+i;metadata.owner_kind_8=1+i;
            metadata.facing_28=facings[(seed/8+i)%4];metadata.facing_threshold_38=seed/32%2?0.0f:-1.0f;metadata.max_squared_distance_3c=10000.0f;
            metadata.payload=std::array<uint32_t,2>{UINT32_MAX,UINT32_MAX};
            expect(registry.register_object(i+1,scene_type,{10,0,11.0f+float(i)}) && registry.set_context_metadata(i+1,metadata),"sequence registers explicit context observations without positioning the node");}
        awl::WorldMapPlayerContextQuery query;query.current_key=seed%2?17:1;query.origin=awl::WorldMapPlayerContextVector{10,0,10};query.forward=forwards[seed%4];
        query.angle_radians=std::array<float,3>{0x1.921fb6p-1f,0x1.921fb6p+0f,0x1.921fb6p+1f}[seed/4%3];
        query.min_distance=float(seed/12%2);query.max_distance=std::array<float,3>{2,4,80}[seed/24%3];query.excluded_kinds=std::array<uint32_t,3>{0,2,8}[seed/16%3];
        for(uint32_t stage=0;stage<9;++stage){
            if(stage==1){for(uint32_t i=0;i<3;++i)expect(registry.update_position(i+1,{10,0,11.0f+float(i)},&update),"sequence positions each node through its original bin rule");}
            else if(stage==2)expect(registry.update_position(1,{10,0,12},&update),"same-bucket write creates a strict distance tie without reordering");
            else if(stage==3)expect(registry.update_type_and_position(3,4,{10,0,10.5f},&update),"fixed-type move preserves context metadata");
            else if(stage==4)expect(registry.update_type_and_position(2,1,{10,0,64},&update),"exact Z threshold moves into neighboring bin two");
            else if(stage==5)expect(registry.update_position(1,{10,0,90},&update),"later position update preserves metadata and uses current scene type");
            else if(stage==6)expect(registry.update_type_and_position(2,scene_type,{10,0,11},&update),"returning node appends after the destination tail");
            else if(stage==7)expect(registry.unregister_object(2),"selected node removal is visible to the next query");
            else if(stage==8)registry.clear();
            awl::WorldMapPlayerContextResult result;
            expect(awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared,"owned scene registry query completes without copied list observations");
            std::vector<uint32_t> words{seed,stage};for(auto value:context_words(result))words.push_back(value);
            for(int32_t key=-1;key<=58;++key){const auto objects=registry.snapshot(key);words.push_back(uint32_t(key));words.push_back(uint32_t(objects.size()));
                for(const auto& object:objects){const auto& metadata=object.context;words.push_back(uint32_t(object.identity));words.push_back(uint32_t(object.scene_type));
                    for(float value:object.position)words.push_back(actor_bits(value));words.push_back(*metadata.owner_code_4);words.push_back(*metadata.owner_kind_8);
                    for(float value:*metadata.facing_28)words.push_back(actor_bits(value));words.push_back(actor_bits(*metadata.facing_threshold_38));words.push_back(actor_bits(*metadata.max_squared_distance_3c));}}
            for(auto value:words)hash(digest,value);
            if(std::getenv("AWL_CONTEXT_REGISTRY_TRACE")){uint64_t row=14695981039346656037ull;for(auto value:words)hash(row,value);
                std::cout<<"CONTEXT_REGISTRY_ROW "<<seed<<' '<<stage<<' '<<std::hex<<row<<std::dec<<'\n';}
        }
    }
    expect(digest==0xf1aee0cf021596dfull,"1152 complete registry/query samples agree with actual insertion, position, rebin, removal and query instructions under the native numerical profile");
    std::cout<<"PLAYER_CONTEXT_REGISTRY_SEQUENCE 1152 "<<std::hex<<digest<<std::dec<<'\n';
}
void context_registry_checks(const awl::WorldMapPlayerContextTables& tables) {
    using S=awl::WorldMapPlayerContextStatus;awl::WorldMapSceneBucketRegistry registry;awl::WorldMapScenePositionUpdate update;
    awl::WorldMapPlayerContextQuery query;query.current_key=17;query.origin=awl::WorldMapPlayerContextVector{0,0,0};query.forward={0,0,1};query.max_distance=4;
    awl::WorldMapPlayerContextResult result;result.record.owner_code=999;const auto prior=context_words(result);
    expect(awl::query_world_map_player_scene_context(&tables,nullptr,query,&result).status==S::RequiresSceneRegistry && context_words(result)==prior,"missing reached registry preserves output");
    expect(awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared && !result.found,"owned empty bucket is known empty");
    expect(registry.register_object(1,3,{0,0,1}) && awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared && !result.found,
        "unpositioned node stays in constructor minus-one bucket and cannot become a target");
    expect(registry.update_position(1,{0,0,1},&update) && awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::RequiresRange,"positioned node's reached metadata is never fabricated");
    awl::WorldMapPlayerContextMetadata metadata;metadata.owner_code_4=101u;metadata.owner_kind_8=1u;metadata.facing_28=awl::WorldMapPlayerContextVector{0,0,-1};
    metadata.facing_threshold_38=1.0f;metadata.max_squared_distance_3c=16.0f;metadata.payload=std::array<uint32_t,2>{5,6};
    expect(registry.set_context_metadata(1,metadata),"existing node owns explicit context observations");metadata.owner_code_4=102u;
    expect(registry.register_object(2,3,{0,0,1}) && registry.set_context_metadata(2,metadata) && registry.update_position(2,{0,0,1},&update),"second equal target appends to existing bucket");
    expect(awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared && result.record.owner_code==101 && result.record.payload==std::array<uint32_t,2>{5,6},
        "equal-distance winner follows original tail insertion and metadata was copied from its source");
    expect(registry.update_position(1,{0,0,2},&update) && !update.relink_required && awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared &&
        result.record.owner_code==102,"query immediately uses same-bucket position writes");
    expect(registry.update_type_and_position(2,4,{0,0,1},&update) && awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared && result.record.owner_code==101,
        "rebin removes the prior winner from the reached bucket");
    expect(registry.update_type_and_position(2,3,{0,0,2},&update) && awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared && result.record.owner_code==101,
        "returning equal target appends behind the retained node");
    awl::WorldMapPlayerScenePose pose;awl::WorldMapPlayerSceneMessage1F message{3,{0,0,2},{0,0,1}};
    expect(awl::apply_world_map_player_scene_message_1f(1,message,&registry,&pose,&update) && awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::Prepared &&
        result.record.owner_code==102 && registry.snapshot(17)[0].context.facing_28==std::optional<awl::WorldMapPlayerContextVector>{{0,0,1}},
        "message heading writes reach the same facing field used by the context query");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    {const auto saved=registry.snapshot(17);const auto live=allocation_probe::live;bool failed=false;
        allocation_probe::remaining=0;allocation_probe::enabled=true;
        try{(void)registry.update_pose(1,5,{0,0,3},{1,0,0},&update);}catch(const std::bad_alloc&){failed=true;}
        allocation_probe::enabled=false;const bool released=allocation_probe::live==live;const auto current=registry.snapshot(17);
        expect(failed && released && registry.size(19)==0 && current.size()==saved.size() && current[0].identity==saved[0].identity &&
            current[0].position==saved[0].position && current[0].scene_type==saved[0].scene_type && current[0].context.facing_28==saved[0].context.facing_28 &&
            current[0].context.payload==saved[0].context.payload,"destination allocation failure preserves pose, context observations, bucket/order and storage");
        std::cout<<"PLAYER_CONTEXT_POSE_ALLOCATION_FAILURES 1\n";}
#endif
    const auto retained=context_words(result);const auto before=registry.snapshot(17);const float nan=std::numeric_limits<float>::quiet_NaN();
    expect(!registry.update_pose(1,4,{0,0,3},{nan,0,1},&update) && !registry.update_position(1,{nan,0,3},&update) &&
        !registry.set_context_metadata(0,metadata) && !registry.set_context_metadata(99,metadata) && registry.snapshot(17)[0].position==before[0].position &&
        registry.snapshot(17)[0].context.facing_28==before[0].context.facing_28 && registry.size(18)==0,"invalid pose/identity preserves geometry, metadata, membership and order");
    metadata={};metadata.owner_kind_8=1u;expect(registry.set_context_metadata(1,metadata),"metadata can be cleared without moving the node");query.excluded_kinds=2;
    // Second kind is also one: both entries are excluded before missing fields.
    allocation_probe::remaining=0;allocation_probe::enabled=true;
    const bool stored=registry.set_context_metadata(1,metadata);const auto step=awl::query_world_map_player_scene_context(&tables,&registry,query,&result);allocation_probe::enabled=false;
    expect(stored && step.status==S::Prepared && !result.found,"metadata publication and registry query allocate nothing; masked unknown geometry/payload is unread");
    query.excluded_kinds=0;result.record.owner_code=retained[2];const auto stopped=context_words(result);
    expect(awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::RequiresRange && context_words(result)==stopped,"missing reached metadata preserves the previous complete result");
    query.current_key=59;expect(awl::query_world_map_player_scene_context(&tables,&registry,query,&result).status==S::RequiresList && context_words(result)==stopped,"unowned bucket cannot masquerade as empty");
    query.forward={0,0,0};query.origin.reset();expect(awl::query_world_map_player_scene_context(nullptr,nullptr,query,&result).status==S::Prepared && !result.found,"zero heading skips even absent registry/tables");
    expect(registry.unregister_object(1) && registry.register_object(1,3,{0,0,1}) && !registry.snapshot(-1)[0].context.owner_code_4 && !registry.snapshot(-1)[0].context.facing_28,
        "reused identity starts with unknown metadata instead of restoring stale observations");
    registry.clear();expect(registry.size(-1)==0 && registry.size(17)==0,"clear removes every owned context node");
    std::cout<<"PLAYER_CONTEXT_REGISTRY owned positions/order/metadata, message heading and lifecycle failures\n";
}
std::vector<uint8_t> context_label_dol() {
    std::vector<uint8_t> bytes(0x1658,0);
    const std::array<std::array<uint32_t,3>,4> sections{{{{0x100,0x80292a78,31*172}},{{0x15d4,0x802a7b08,124}},{{0x1650,0x8004f19c,4}},{{0x1654,0x8034a3c8,4}}}};
    for(uint32_t i=0;i<sections.size();++i){word(bytes,i*4,sections[i][0]);word(bytes,0x48+i*4,sections[i][1]);word(bytes,0x90+i*4,sections[i][2]);}
    for(uint32_t row=0;row<31;++row)for(uint32_t context=0;context<86;++context){bytes[0x100+row*172+context]=uint8_t((row*7+context*3)%40);
        bytes[0x100+row*172+86+context]=uint8_t((row*11+context*5)%40);}
    for(uint32_t type=0;type<31;++type)word(bytes,0x15d4+type*4,0x8004f19c);
    word(bytes,0x1650,0x38600000);word(bytes,0x1654,0x3f266666);return bytes;
}
std::array<uint32_t,15> context_label_words(const awl::WorldMapPlayerContextLabels& output) {
    return {uint32_t(output.scene_type),actor_bits(output.position[0]),actor_bits(output.position[1]),actor_bits(output.position[2]),
        actor_bits(output.facing[0]),actor_bits(output.facing[1]),actor_bits(output.facing[2]),uint32_t(output.target_kind),uint32_t(output.target_payload),output.direction,
        uint32_t(output.item),uint32_t(output.action),output.labels[0],output.labels[1],output.labels[2]};
}
void context_label_matrix(const awl::WorldMapPlayerContextLabelTables& tables,uint64_t expected) {
    uint64_t digest=14695981039346656037ull;using S=awl::WorldMapPlayerContextLabelStatus;
    for(uint32_t seed=0;seed<1024;++seed){awl::WorldMapPlayerContextLabelInput input;const bool action=seed%2!=0;
        input.source=action?awl::WorldMapPlayerContextLabelSource::Action:awl::WorldMapPlayerContextLabelSource::HeldItem;
        input.value=action?std::array<int32_t,16>{0,1,2,3,47,48,55,56,57,58,59,60,61,INT32_MIN,-1,INT32_MAX}[seed/2%16]:
            std::array<int32_t,12>{0,2,3,4,6,30,0x459,0x4f6,0x4f7,0x4f8,0x4f9,64}[seed/2%12];
        awl::WorldMapPlayerContextItemRow row;row.item_id=action?0:input.value;row.type_0=uint8_t(seed/4%32);row.flags_1=uint8_t(seed*37);
        row.flags_1c=std::array<uint32_t,4>{0,2,4,6}[seed/32%4];input.item_row=row;input.item_record_resolved=true;input.item_record_byte_c=std::array<uint8_t,3>{0,2,255}[seed/128%3];
        input.action_record_resolved=true;input.action_record_flags_28=uint8_t(seed*13);input.target_record_resolved=true;input.target_record_predicate=(seed&128)!=0;
        input.target_record_flags_28=uint8_t(seed*13);input.context_item_available={bool(seed&256),bool(seed&512)};input.context8_scene_blocked=bool(seed&64);input.item_flag1_available=bool(seed&32);
        input.terrain_clear={bool(seed&16),bool(seed&32)};input.secondary_ground_available=bool(seed&64);
        const std::array<float,5> values{0,1,-1,.1f,120.125f};for(size_t axis=0;axis<3;++axis)input.position[axis]=values[(seed+axis)%5];
        input.facing=std::array<awl::WorldMapPlayerContextVector,4>{{{0,0,1},{1,0,0},{0,0,0},{0,0,-1}}}[seed/4%4];
        input.scene_type=int32_t(seed%48)-2;input.query_mode=std::array<int32_t,4>{0,1,2,-1}[seed/8%4];input.target_kind=int32_t(seed/16%53)-1;
        input.target_payload=int32_t(seed*3%80)-1;input.direction=seed%5;awl::WorldMapPlayerContextLabels output;
        const auto step=awl::prepare_world_map_player_context_labels(&tables,input,&output);expect(step.status==S::Prepared,"complete observed context descriptor prepares across constructor modes and target classes");
        hash(digest,seed);for(auto value:context_label_words(output))hash(digest,value);hash(digest,step.classification);hash(digest,step.context);hash(digest,step.probe_mask);
        for(float value:step.terrain_position.value_or(awl::WorldMapPlayerContextVector{}))hash(digest,actor_bits(value));
        if(std::getenv("AWL_CONTEXT_LABEL_TRACE")){std::cout<<"CONTEXT_LABEL_ROW "<<seed;for(auto value:context_label_words(output))std::cout<<' '<<std::hex<<value;
            std::cout<<' '<<step.classification<<' '<<step.context<<' '<<uint32_t(step.probe_mask);for(float value:step.terrain_position.value_or(awl::WorldMapPlayerContextVector{}))std::cout<<' '<<actor_bits(value);std::cout<<std::dec<<'\n';}
    }
    expect(digest==expected,"1024 complete descriptor words, classifications and reached probe positions agree with original instructions over explicit record/predicate observations");
    std::cout<<"PLAYER_CONTEXT_LABEL_COMPARISONS 1024 "<<std::hex<<digest<<std::dec<<'\n';
}
void context_label_classification_checks(const awl::WorldMapPlayerContextLabelTables& tables) {
    using S=awl::WorldMapPlayerContextLabelStatus;uint64_t digest=14695981039346656037ull;uint32_t count=0;
    std::vector<int32_t> payloads;for(int32_t payload=-2;payload<=80;++payload)payloads.push_back(payload);payloads.push_back(INT32_MIN);payloads.push_back(INT32_MAX);
    for(int32_t kind=-1;kind<=51;++kind)for(int32_t payload:payloads)for(uint32_t direction:{0u,1u,2u,3u,4u,UINT32_MAX}){
        awl::WorldMapPlayerContextLabelInput input;input.target_kind=kind;input.target_payload=payload;input.direction=direction;input.facing={0,0,1};
        input.target_record_resolved=true;const bool predicate=((uint32_t(kind)^uint32_t(payload)^direction)&1)!=0;const uint8_t flags=uint8_t(uint32_t(payload)&3);
        input.target_record_predicate=predicate;input.target_record_flags_28=flags;input.context_item_available={true,true};input.context8_scene_blocked=false;
        input.terrain_clear={true,true};input.secondary_ground_available=true;input.item_row=awl::WorldMapPlayerContextItemRow{0,{},{},0u};
        awl::WorldMapPlayerContextLabels output;const auto step=awl::prepare_world_map_player_context_labels(&tables,input,&output);
        expect(step.status==S::Prepared,"all target routes prepare with complete reached observations");for(uint32_t value:{uint32_t(kind),uint32_t(payload),direction,uint32_t(predicate),uint32_t(flags),step.context})hash(digest,value);++count;
    }
    expect(count==27030 && digest==0x8ba9bebe67c93b54ull,"all 51 target routes and signed boundaries agree with original classification instructions");
    std::cout<<"PLAYER_CONTEXT_TARGET_CLASSIFICATION "<<count<<' '<<std::hex<<digest<<std::dec<<'\n';digest=14695981039346656037ull;
    for(uint32_t flags=0;flags<256;++flags){awl::WorldMapPlayerContextLabelInput input;input.value=64;input.query_mode=1;
        input.item_row=awl::WorldMapPlayerContextItemRow{64,uint8_t(0),uint8_t(flags),0u};awl::WorldMapPlayerContextLabels output;
        const auto step=awl::prepare_world_map_player_context_labels(&tables,input,&output);expect(step.status==S::Prepared,"all item flag bytes prepare");hash(digest,flags);hash(digest,step.classification);}
    expect(digest==0x4c9c08e222202925ull,"all 256 item flag bytes retain original ordered bit precedence with the same invented final routing");std::cout<<"PLAYER_CONTEXT_ITEM_FLAGS 256 "<<std::hex<<digest<<std::dec<<'\n';
    digest=14695981039346656037ull;count=0;
    for(int32_t action:{2,47,48,55,56,57,58,59,60,61,-1,INT32_MIN,INT32_MAX})for(uint32_t flags=0;flags<256;++flags){
        awl::WorldMapPlayerContextLabelInput input;input.source=awl::WorldMapPlayerContextLabelSource::Action;input.value=action;input.query_mode=1;input.target_kind=30;
        input.action_record_resolved=true;input.action_record_flags_28=uint8_t(flags);awl::WorldMapPlayerContextLabels output;
        const auto step=awl::prepare_world_map_player_context_labels(&tables,input,&output);std::array<uint32_t,2> base{};
        expect(step.status==S::Prepared && tables.labels(step.classification,42,&base),"all reached action flags prepare with their original classification");
        const uint32_t result=output.labels[0]==base[0]?1u:0u;for(uint32_t value:{uint32_t(action),flags,result})hash(digest,value);++count;
    }
    expect(count==3328 && digest==0x68082f4c104c6b25ull,"all action flag bytes/boundaries agree with original 4F25C mask selection");std::cout<<"PLAYER_CONTEXT_ACTION_FLAGS "<<count<<' '<<std::hex<<digest<<std::dec<<'\n';
}
void context_icon_matrix(const awl::WorldMapPlayerContextLabelTables& tables,uint64_t expected) {
    using S=awl::WorldMapPlayerContextIconStatus;uint64_t digest=14695981039346656037ull;uint32_t count=0;
    for(uint32_t old=0;old<103;++old)for(uint32_t label=0;label<103;++label)for(uint32_t channel=0;channel<4;++channel){
        awl::WorldMapPlayerContextIconState state;state.resource_manager_1278=uint64_t(0x12345678);state.labels_60={11u,22u,33u,44u};state.labels_60[channel]=old;auto next=state;
        const auto step=awl::prepare_world_map_player_context_icon(&tables,state,channel,label,&next);
        expect(step.status==S::Prepared || step.status==S::RequiresIconAssets,"every bounded icon pair completes retained state or stops before changed-resource selection");
        for(uint32_t value:{old,label,channel,uint32_t(step.status==S::RequiresIconAssets)})hash(digest,value);
        for(const auto& value:next.labels_60)hash(digest,*value);hash(digest,step.previous_icon?uint32_t(*step.previous_icon):256u);hash(digest,step.next_icon?uint32_t(*step.next_icon):256u);++count;
    }
    expect(count==42436 && digest==expected,"all bounded label/icon pairs and four channels agree with actual original retained paths/resource stops");
    std::cout<<"PLAYER_CONTEXT_ICON_RETENTION "<<count<<' '<<std::hex<<digest<<std::dec<<'\n';
}
void context_icon_checks(const std::vector<uint8_t>& label_bytes) {
    using D=awl::WorldMapPlayerContextLabelTablesStatus;using S=awl::WorldMapPlayerContextIconStatus;
    auto bytes=label_bytes;bytes.resize(0x16bf);word(bytes,4*4,0x1658);word(bytes,0x48+4*4,0x8024f694);word(bytes,0x90+4*4,103);
    for(uint32_t i=0;i<103;++i)bytes[0x1658+i]=uint8_t(i%7);
    std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> tables;
    expect(awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&tables)==D::Decoded,"bounded icon map decodes beside label metadata");if(!tables)return;
    context_icon_matrix(*tables,0xe3c75fc6ac068975ull);
    awl::WorldMapPlayerContextIconState state,output;output.resource_manager_1278=uint64_t(999);output.labels_60={99u,88u,77u,66u};const auto prior=output;
    const auto preserved=[&](){return output.labels_60==prior.labels_60 && output.resource_manager_1278==prior.resource_manager_1278;};
    expect(awl::prepare_world_map_player_context_icon(nullptr,state,0,0,&output).status==S::RequiresResourceManager && preserved(),"resource-manager pointer observation precedes even equal-label completion");
    state.resource_manager_1278=uint64_t(0);expect(awl::prepare_world_map_player_context_icon(nullptr,state,0,0,&output).status==S::RequiresPreviousLabel && preserved(),"known null manager is not dereferenced on retained paths; previous label is still required");
    state.labels_60[0]=7u;expect(awl::prepare_world_map_player_context_icon(nullptr,state,0,0,&output).status==S::RequiresIconMap && preserved(),"changed label needs mapping before icon comparison");
    {std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> label_only;uint8_t icon=255;
        expect(awl::decode_world_map_player_context_label_tables(label_bytes.data(),label_bytes.size(),&label_only)==D::Decoded && !label_only->icon(0,&icon) && icon==255 &&
            awl::prepare_world_map_player_context_icon(label_only.get(),state,0,0,&output).status==S::RequiresIconMap && preserved(),"label-only metadata never claims an absent icon section is validated");}
    auto step=awl::prepare_world_map_player_context_icon(tables.get(),state,0,1,&output);expect(step.status==S::RequiresIconAssets && step.previous_icon==uint8_t(0) && step.next_icon==uint8_t(1) && preserved(),"different mapped icon stops before asset setup without changing any label or manager field");
    expect(awl::prepare_world_map_player_context_icon(tables.get(),state,4,0,&output).status==S::InvalidInput && preserved() && awl::prepare_world_map_player_context_icon(tables.get(),state,0,0,nullptr).status==S::InvalidInput,"invalid icon output/channel rejects");
    state.labels_60[0]=103u;expect(awl::prepare_world_map_player_context_icon(tables.get(),state,0,0,&output).status==S::UnsupportedLabel && preserved(),"reached unsupported previous label cannot index the map");
    expect(awl::prepare_world_map_player_context_icon(nullptr,state,0,103,&output).status==S::Prepared && output.labels_60[0]==103u && !output.labels_60[1],"equal label skips mapping/bounds and leaves unrelated unknown fields intact");output=prior;
    state.labels_60={7u,0u,14u,21u};awl::WorldMapPlayerContextLabels labels;labels.labels={0,0,1};
    step=awl::prepare_world_map_player_context_icons(tables.get(),state,labels,&output);expect(step.status==S::RequiresIconAssets && step.channel==2 && step.prepared_mask==0xbu && preserved(),"late channel2 resource stop follows 0,1,3 order and preserves all staged label changes");
    state.labels_60[1].reset();step=awl::prepare_world_map_player_context_icons(tables.get(),state,labels,&output);expect(step.status==S::RequiresPreviousLabel && step.channel==1 && step.prepared_mask==1 && preserved(),"missing channel1 stops before channel3/2 without publishing accepted channel0");
    state.labels_60[1]=0u;labels.labels={0,7,14};allocation_probe::remaining=0;allocation_probe::enabled=true;
    step=awl::prepare_world_map_player_context_icons(tables.get(),state,labels,&state);allocation_probe::enabled=false;
    expect(step.status==S::Prepared && step.prepared_mask==15 && state.labels_60==std::array<std::optional<uint32_t>,4>{0u,0u,14u,7u} && state.resource_manager_1278==uint64_t(0),"all four retained-icon channels publish in their original slots atomically without allocation and support aliasing");
    uint8_t icon=255;bytes.clear();expect(tables->icon(102,&icon) && icon==4 && !tables->icon(103,&icon) && icon==4,"owned icon map outlives source bytes and bounds preserve output");
    std::cout<<"PLAYER_CONTEXT_ICON_BATCH retained labels, ordered changed-resource stops and atomic publication\n";
}
void context_label_checks() {
    using D=awl::WorldMapPlayerContextLabelTablesStatus;using S=awl::WorldMapPlayerContextLabelStatus;
    auto bytes=context_label_dol();std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> tables;
    expect(awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&tables)==D::Decoded && tables && !tables->target_verified(),"invented context label metadata is owned without target identity");if(!tables)return;
    const auto* old=tables.get();for(size_t size=0;size<bytes.size();++size)expect(awl::decode_world_map_player_context_label_tables(bytes.data(),size,&tables)==D::UnsupportedLayout && tables.get()==old,"every truncated label DOL preserves prior metadata");
    for(const auto invalid:std::array<std::array<uint32_t,2>,4>{{{{0x15d4,0x8004f198}},{{0x1650,0x38800000}},{{0x1650,0x3860001f}},{{0x1654,0x7fc00000}}}}){auto bad=bytes;word(bad,invalid[0],invalid[1]);
        expect(awl::decode_world_map_player_context_label_tables(bad.data(),bad.size(),&tables)==D::UnsupportedLayout && tables.get()==old,"invalid routing/code/distance rejects without replacing metadata");}
    std::array<uint32_t,2> labels{999,999};expect(!tables->labels(31,0,&labels) && !tables->labels(0,86,&labels) && !tables->labels(0,0,nullptr) && labels==std::array<uint32_t,2>{999,999},"label lookup bounds preserve output");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    {const auto live=allocation_probe::live;size_t failed=0;bool decoded=false;
        for(size_t fail=0;fail<8;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto status=awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&tables);allocation_probe::enabled=false;
            if(status==D::Decoded){decoded=true;break;}++failed;expect(status==D::AllocationFailure && tables.get()==old && allocation_probe::live==live,"both label metadata allocations preserve retained owner and release staging");}
        expect(decoded && failed==2,"label tables publish only after both allocations succeed");std::cout<<"PLAYER_CONTEXT_LABEL_ALLOCATION_FAILURES "<<failed<<'\n';}
#endif
    context_label_matrix(*tables,0x9ca42ae922786a33ull);
    context_label_classification_checks(*tables);
    context_icon_checks(bytes);
    awl::WorldMapPlayerContextLabelInput input;input.query_mode=1;input.facing={0,0,1};awl::WorldMapPlayerContextLabels output;output.labels={777,888,999};
    auto prior=context_label_words(output);const auto stopped=[&](S status){const auto step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);
        expect(step.status==status && context_label_words(output)==prior,"reached context-label dependency preserves every previous descriptor word");return step;};
    expect(awl::prepare_world_map_player_context_labels(nullptr,input,&output).status==S::RequiresTables && context_label_words(output)==prior && awl::prepare_world_map_player_context_labels(tables.get(),input,nullptr).status==S::InvalidInput,"missing table/output rejects");
    expect(awl::prepare_world_map_player_context_labels(tables.get(),input,&output).status==S::Prepared && output.labels==std::array<uint32_t,3>{13,21,102},"fixed mode uses context42 and no-item classification1 without item/target observations");prior=context_label_words(output);
    input.query_mode=0;input.target_kind=30;stopped(S::RequiresTargetRecord);input.target_record_resolved=true;stopped(S::RequiresTargetPredicate);input.target_record_predicate=true;stopped(S::RequiresTargetFlags);
    input.target_record_flags_28=uint8_t(3);expect(awl::prepare_world_map_player_context_labels(tables.get(),input,&output).status==S::Prepared,"target record classification resolves after its ordered observations");prior=context_label_words(output);
    input={};input.query_mode=1;input.value=30;input.item_row=awl::WorldMapPlayerContextItemRow{30,uint8_t(9),{},{}};stopped(S::RequiresItemRecord);input.item_record_resolved=true;
    auto step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && step.classification==29,"type9 record resolution precedes item30 early return while later row flags/record byte are unread");prior=context_label_words(output);
    input.value=64;input.item_row=awl::WorldMapPlayerContextItemRow{64,uint8_t(8),{},{}};input.item_record_resolved.reset();stopped(S::RequiresItemRecord);input.item_record_resolved=true;stopped(S::RequiresItemFlags);
    input.item_row->flags_1=uint8_t(16);step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && step.classification==23,"ordered flags classifier precedes type8 return");prior=context_label_words(output);
    input.item_row->type_0=uint8_t(9);input.item_row->flags_1=uint8_t(0);stopped(S::RequiresItemRecordByte);input.item_record_byte_c=uint8_t(2);
    step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && step.classification==15,"type9 saved byte two skips later flags word");prior=context_label_words(output);
    input.item_record_byte_c=uint8_t(0);stopped(S::RequiresItemFlags);input.item_row->flags_1c=2;
    step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && step.classification==21,"flags2/4 precedence selects item class21");prior=context_label_words(output);
    input={};input.value=0x4f6;input.query_mode=1;input.facing={0,0,1};input.item_row=awl::WorldMapPlayerContextItemRow{0x4f6,uint8_t(0),uint8_t(0),0u};
    step=stopped(S::RequiresTerrain);expect(step.terrain_channel==0u && step.probe_mask==1 && step.terrain_position && actor_bits((*step.terrain_position)[2])==0x3f266666,"first label29 reports the actual forward probe point");
    input.terrain_clear[0]=false;step=stopped(S::RequiresTerrain);expect(step.terrain_channel==1u && step.probe_mask==3,"secondary terrain dependency follows accepted primary observation");input.terrain_clear[1]=true;
    allocation_probe::remaining=0;allocation_probe::enabled=true;step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);allocation_probe::enabled=false;
    expect(step.status==S::Prepared && output.labels==std::array<uint32_t,3>{0,29,102},"two ordered terrain results publish together without allocation");prior=context_label_words(output);
    const auto rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"context label test selects unsupported rounding");stopped(S::UnsupportedNumerics);expect(std::fesetround(rounding)==0,"context label test restores rounding");
    input.facing[1]=std::numeric_limits<float>::infinity();stopped(S::UnsupportedNumerics);
    input={};input.target_kind=34;stopped(S::RequiresItemAvailability);input.context_item_available[0]=false;
    step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && step.context==76,"unavailable context74 remaps to76 before label lookup");prior=context_label_words(output);
    input={};input.target_kind=4;input.direction=2;stopped(S::RequiresScenePredicate);input.context8_scene_blocked=true;
    step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && step.context==0,"context8 scene predicate remaps before label lookup");prior=context_label_words(output);
    input={};input.source=awl::WorldMapPlayerContextLabelSource::Action;input.value=56;input.query_mode=1;input.target_kind=30;stopped(S::RequiresActionRecord);input.action_record_resolved=true;stopped(S::RequiresActionFlags);
    input.action_record_flags_28=uint8_t(8);step=awl::prepare_world_map_player_context_labels(tables.get(),input,&output);expect(step.status==S::Prepared && output.labels[0]==35,"action56 checks bit4 rather than bit8 and increments primary label when clear");prior=context_label_words(output);
    input.value=2;input.action_record_flags_28.reset();expect(awl::prepare_world_map_player_context_labels(tables.get(),input,&output).status==S::Prepared,"action2 requires record resolution but skips flags");prior=context_label_words(output);
    input={};input.target_kind=47;input.target_payload=64;stopped(S::RequiresItemRow);input.item_row=awl::WorldMapPlayerContextItemRow{0,{},{},2u};stopped(S::RequiresItemAvailability);input.item_flag1_available=true;
    expect(awl::prepare_world_map_player_context_labels(tables.get(),input,&output).status==S::Prepared && output.labels[0]==35,"context47 item flag availability can replace primary label");
    {auto bad=bytes;bad[0x100+172+86+42]=25;std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> ground;
        expect(awl::decode_world_map_player_context_label_tables(bad.data(),bad.size(),&ground)==D::Decoded,"invented reached ground label decodes");input={};input.query_mode=1;prior=context_label_words(output);
        expect(awl::prepare_world_map_player_context_labels(ground.get(),input,&output).status==S::RequiresGround && context_label_words(output)==prior,"label25 requires ground before publication");input.secondary_ground_available=false;
        expect(awl::prepare_world_map_player_context_labels(ground.get(),input,&output).status==S::Prepared && output.labels[1]==0,"failed ground predicate clears only secondary label");}
    {std::shared_ptr<const awl::WorldMapPlayerContextTables> neighbors;auto neighbor_bytes=context_tables_dol();expect(awl::decode_world_map_player_context_tables(neighbor_bytes.data(),neighbor_bytes.size(),&neighbors)==awl::WorldMapPlayerContextTablesStatus::Decoded,"chain neighbors decode");
        awl::WorldMapSceneBucketRegistry registry;awl::WorldMapPlayerContextMetadata metadata;metadata.owner_code_4=100u;metadata.owner_kind_8=4u;metadata.facing_28=awl::WorldMapPlayerContextVector{0,0,-1};metadata.facing_threshold_38=1.0f;metadata.max_squared_distance_3c=16.0f;metadata.payload=std::array<uint32_t,2>{1,999};awl::WorldMapScenePositionUpdate update;
        expect(registry.register_object(1,3,{0,0,1}) && registry.set_context_metadata(1,metadata) && registry.update_position(1,{0,0,1},&update),"chain owns target payload distinct from owner kind");
        awl::WorldMapPlayerContextQuery query;query.current_key=17;query.origin=awl::WorldMapPlayerContextVector{};query.forward={0,0,1};query.max_distance=4;awl::WorldMapPlayerContextResult target;
        expect(awl::query_world_map_player_scene_context(neighbors.get(),&registry,query,&target).status==awl::WorldMapPlayerContextStatus::Prepared && target.found,"chain query selects owned target");input={};input.facing=query.forward;
        step=awl::prepare_world_map_player_context_labels_from_target(tables.get(),target,input,&output);expect(step.status==S::Prepared && step.context==6 && output.target_kind==1 && output.target_payload==999,
            "query-to-label adapter binds resolved payload words rather than owner kind/code");
        const auto retained=context_label_words(output);metadata.payload=std::array<uint32_t,2>{30,999};expect(registry.set_context_metadata(1,metadata) &&
            awl::query_world_map_player_scene_context(neighbors.get(),&registry,query,&target).status==awl::WorldMapPlayerContextStatus::Prepared &&
            awl::prepare_world_map_player_context_labels_from_target(tables.get(),target,input,&output).status==S::RequiresTargetRecord && context_label_words(output)==retained,
            "changed owned target can reach a late record dependency without losing the prior descriptor");
        expect(registry.unregister_object(1) && awl::query_world_map_player_scene_context(neighbors.get(),&registry,query,&target).status==awl::WorldMapPlayerContextStatus::Prepared && !target.found,
            "removed owned target produces the query's complete no-hit defaults");input.target_kind=1;input.target_payload=999;
        step=awl::prepare_world_map_player_context_labels_from_target(tables.get(),target,input,&output);expect(step.status==S::Prepared && step.context==0 && output.target_kind==-1 && output.target_payload==-1,
            "no-hit binding replaces stale target observations with original signed minus-one payload defaults");}
    {Fixture fixture;const auto* retained=tables.get();expect(awl::load_world_map_player_context_label_tables(&tables)==D::ReadFailure && tables.get()==retained,"missing label DOL preserves prior provider");std::filesystem::create_directory(fixture.root/"sys");
        {std::ofstream file(fixture.root/"sys"/"main.dol",std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));expect(bool(file),"invented label fixture writes");}
        expect(awl::load_world_map_player_context_label_tables(&tables)==D::WrongDol && tables.get()==retained,"invented label layout cannot bypass target SHA1");awl::filesystem_shutdown();}
    bytes.clear();expect(tables->labels(1,42,&labels) && labels==std::array<uint32_t,2>{13,21},"decoded labels outlive their source bytes");
    std::cout<<"PLAYER_CONTEXT_LABELS item/action/target classification, ordered predicates and query payload binding\n";
}
void context_checks() {
    using D=awl::WorldMapPlayerContextTablesStatus;using S=awl::WorldMapPlayerContextStatus;
    auto bytes=context_tables_dol();std::shared_ptr<const awl::WorldMapPlayerContextTables> tables;
    expect(awl::decode_world_map_player_context_tables(bytes.data(),bytes.size(),&tables)==D::Decoded && tables && !tables->target_verified(),"context decoder owns all reached invented metadata without claiming target identity");if(!tables)return;
    const auto* old=tables.get();
    for(size_t size=0;size<bytes.size();++size)expect(awl::decode_world_map_player_context_tables(bytes.data(),size,&tables)==D::UnsupportedLayout && tables.get()==old,
        "every truncated context-table prefix rejects before replacing the retained provider");
    auto bad=bytes;word(bad,0x100,8);expect(awl::decode_world_map_player_context_tables(bad.data(),bad.size(),&tables)==D::UnsupportedLayout && tables.get()==old,"unsupported neighbor slot cannot index beyond the eight entries");
    bad=bytes;word(bad,0x4f8,1);expect(awl::decode_world_map_player_context_tables(bad.data(),bad.size(),&tables)==D::UnsupportedLayout && tables.get()==old,"changed constructor-vector layout rejects");
    bad=bytes;word(bad,0x4f0,0x7fc00000);expect(awl::decode_world_map_player_context_tables(bad.data(),bad.size(),&tables)==D::UnsupportedLayout && tables.get()==old,"nonfinite facing thresholds reject");
    int32_t neighbor=99;expect(!tables->neighbor(1,8,&neighbor) && neighbor==99 && !tables->neighbor(1,0,nullptr),"invalid lookup preserves output");
    expect(tables->neighbor(999,0,&neighbor) && neighbor==-1,"missing static key returns the original minus-one result");
    bad=bytes;word(bad,0x2b0+36,1);std::shared_ptr<const awl::WorldMapPlayerContextTables> duplicate;
    expect(awl::decode_world_map_player_context_tables(bad.data(),bad.size(),&duplicate)==D::Decoded && duplicate->neighbor(1,0,&neighbor) && neighbor==2,
        "duplicate lookup rows preserve first-match behavior rather than rejecting or sorting the original table");
#if !defined(_MSC_VER) || !defined(_DEBUG)
    {const auto live=allocation_probe::live;size_t failed=0;bool decoded=false;
        for(size_t fail=0;fail<8;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            const auto status=awl::decode_world_map_player_context_tables(bytes.data(),bytes.size(),&tables);allocation_probe::enabled=false;
            if(status==D::Decoded){decoded=true;break;}++failed;
            expect(status==D::AllocationFailure && tables.get()==old && allocation_probe::live==live,"context metadata allocation failures preserve owner and release storage");}
        expect(decoded && failed==2,"context provider publishes only after object and shared-control allocation");std::cout<<"PLAYER_CONTEXT_TABLE_ALLOCATION_FAILURES "<<failed<<'\n';}
#endif
    context_matrix(*tables,0xa0da16549db546e1ull);
    context_registry_sequence(*tables);context_registry_checks(*tables);
    awl::WorldMapPlayerContextQuery query;query.forward={0,0,1};query.origin=awl::WorldMapPlayerContextVector{0,0,0};query.angle_radians=0;query.max_distance=4;query.current_key=1;
    std::vector<awl::WorldMapPlayerContextList> lists{{1,{}},{2,{}}};
    awl::WorldMapPlayerContextCandidate first;first.owner_code_4=100u;first.owner_kind_8=1u;first.position_1c=awl::WorldMapPlayerContextVector{0,0,1};
    first.facing_28=awl::WorldMapPlayerContextVector{0,0,-1};first.facing_threshold_38=1.0f;first.max_squared_distance_3c=16.0f;first.payload=std::array<uint32_t,2>{5,6};
    lists[0].candidates={first};lists[1].candidates={first};lists[1].candidates[0].owner_code_4=200u;
    awl::WorldMapPlayerContextResult result;result.record.owner_code=999;const auto prior=context_words(result);
    const auto stopped=[&](S status){const auto step=awl::query_world_map_player_context(tables.get(),lists,query,&result);
        expect(step.status==status && context_words(result)==prior,"context dependency/numeric stops preserve the complete previous result");return step;};
    expect(awl::query_world_map_player_context(tables.get(),lists,query,nullptr).status==S::InvalidInput,"null context output rejects");
    expect(awl::query_world_map_player_context(nullptr,lists,query,&result).status==S::RequiresTables && context_words(result)==prior,"nonzero heading requires lookup ownership");
    lists.pop_back();auto step=stopped(S::RequiresList);expect(step.required_list==2,"missing neighbor remains a reached dependency after selecting the current list");lists.push_back({2,{first}});
    query.origin.reset();stopped(S::RequiresOrigin);query.origin=awl::WorldMapPlayerContextVector{0,0,0};
    lists[0].candidates[0].position_1c.reset();stopped(S::RequiresPosition);lists[0].candidates[0]=first;
    lists[0].candidates[0].max_squared_distance_3c.reset();stopped(S::RequiresRange);lists[0].candidates[0]=first;
    lists[0].candidates[0].facing_threshold_38.reset();stopped(S::RequiresFacingThreshold);lists[0].candidates[0]=first;
    lists[0].candidates[0].facing_28.reset();stopped(S::RequiresFacing);lists[0].candidates[0]=first;
    lists[0].candidates[0].owner_code_4.reset();stopped(S::RequiresOwnerCode);lists[0].candidates[0]=first;
    lists[0].candidates[0].owner_kind_8.reset();stopped(S::RequiresOwnerKind);lists[0].candidates[0]=first;
    lists[0].candidates[0].payload.reset();stopped(S::RequiresPayload);lists[0].candidates[0]=first;
    query.excluded_kinds=2;lists[0].candidates[0]={};stopped(S::RequiresOwnerKind);
    lists[0].candidates[0].owner_kind_8=1u;lists[1].candidates[0]={};lists[1].candidates[0].owner_kind_8=1u;
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared && !result.found,"excluded owners skip all unknown geometry and payload fields");
    query.excluded_kinds=0;lists[0].candidates={first};lists[1].candidates={first};lists[1].candidates[0].owner_code_4=200u;
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared && result.found && result.record.owner_code==100 &&
        result.record.payload==std::array<uint32_t,2>{5,6} && result.record.direction==0 && result.nearest_squared_distance==1,"strict ties retain the current list's earlier candidate and complete payload/direction");
    lists[1].candidates[0].position_1c=awl::WorldMapPlayerContextVector{0,0,.5f};
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared && result.record.owner_code==200,"closer neighbor replaces the prior result");
    lists[0].candidates[0].payload.reset();const auto retained=context_words(result);
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::RequiresPayload && context_words(result)==retained,
        "earlier list's selected payload is required before visiting a later closer list");
    lists[0].candidates[0].position_1c=awl::WorldMapPlayerContextVector{0,0,2};lists[0].candidates.push_back(first);
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared,"unselected earlier candidate payload is unread within one list");
    query.forward={0,0,0};query.origin.reset();
    expect(awl::query_world_map_player_context(nullptr,{},query,&result).status==S::Prepared && !result.found && result.record.payload==std::array<uint32_t,2>{UINT32_MAX,UINT32_MAX},
        "zero forward returns the complete original default record without tables, lists or origin");
    query.forward={0,0,1};query.current_key=0;
    expect(awl::query_world_map_player_context(tables.get(),{},query,&result).status==S::Prepared && !result.found,"zero list key and missing static neighbors skip origin/list reads");
    query.current_key=-2;expect(awl::query_world_map_player_context(tables.get(),{},query,&result).status==S::InvalidInput,"unsupported negative list key cannot index before runtime storage");
    query.current_key=1;query.origin=awl::WorldMapPlayerContextVector{0,0,0};lists={{1,{first}},{2,{}}};
    query.min_distance=1;expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared && !result.found,"equal lower distance is excluded");
    query.min_distance=0;query.max_distance=1;lists[0].candidates[0].max_squared_distance_3c.reset();
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared && !result.found,"equal global upper distance skips even missing candidate range");
    query.max_distance=4;lists[0].candidates[0]=first;lists[0].candidates[0].max_squared_distance_3c=1.0f;lists[0].candidates[0].facing_28.reset();
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::Prepared && !result.found,"equal per-candidate range skips missing facing data");
    lists[0].candidates[0]=first;lists.push_back({1,{}});
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::InvalidInput,"duplicate reached runtime list keys cannot fabricate a direct-index owner");lists.pop_back();
    allocation_probe::remaining=0;allocation_probe::enabled=true;const auto no_allocation=awl::query_world_map_player_context(tables.get(),lists,query,&result);allocation_probe::enabled=false;
    expect(no_allocation.status==S::Prepared && result.found,"complete context query requires no dynamic allocation");
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"context test selects unsupported arithmetic");
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::UnsupportedNumerics,"context query rejects unsupported rounding");
    expect(std::fesetround(rounding)==0,"context test restores caller rounding");
    lists[0].candidates[0].position_1c=awl::WorldMapPlayerContextVector{0,std::numeric_limits<float>::infinity(),1};
    expect(awl::query_world_map_player_context(tables.get(),lists,query,&result).status==S::UnsupportedNumerics,"reached nonfinite geometry is explicit unsupported input");
    {Fixture fixture;const auto* retained_tables=tables.get();
        expect(awl::load_world_map_player_context_tables(&tables)==D::ReadFailure && tables.get()==retained_tables,"missing DOL preserves the unverified retained decoder owner");
        std::filesystem::create_directory(fixture.root/"sys");
        {std::ofstream file(fixture.root/"sys"/"main.dol",std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
            expect(bool(file),"invented context-only DOL fixture writes");}
        expect(awl::load_world_map_player_context_tables(&tables)==D::WrongDol && tables.get()==retained_tables,"valid invented layout cannot bypass target SHA1 at the mounted loader");
        awl::filesystem_shutdown();}
    bytes.clear();expect(tables->neighbor(1,0,&neighbor) && neighbor==2,"context metadata owns its rows after source bytes are released");
    std::cout<<"PLAYER_CONTEXT supplied ordered lists, dependencies, strict ties, payload timing and native profile\n";
}
void state0_initializer_checks() {
    using S=awl::WorldMapPlayerState0TransitionStatus;
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<432;++seed){awl::WorldMapPlayerState0TransitionInput input;
        input.item_144=seed%2?7u:0u;input.item_binding_1454=seed/2%2!=0;input.action_148=seed/4%2?-1:0;input.action_binding_3ec=seed/8%2!=0;
        const auto completed=std::array<uint8_t,3>{0,1,255}[seed/16%3],flag=std::array<uint8_t,3>{0,1,255}[seed/48%3],prior=std::array<uint8_t,3>{0,1,255}[seed/144%3];
        const auto step=awl::prepare_world_map_player_state0_initializer(input,completed,flag,prior);
        const uint32_t route=step.status==S::RequiresItemAttachment?1u:step.status==S::RequiresActionAttachment?2u:0u;
        expect(step.status==S::Prepared || route!=0,"complete initializer observations prepare the no-attachment path or stop before the reached attachment body");
        for(uint32_t word:{seed,*input.item_144,uint32_t(*input.item_binding_1454),uint32_t(*input.action_148),uint32_t(*input.action_binding_3ec),
            uint32_t(completed),uint32_t(flag),uint32_t(prior),route,uint32_t(step.secondary_byte_3f0.value_or(prior))})hash(digest,word);
    }
    expect(digest==0x0ae35e9eaad42101ull,"432 initializer/holder-tail paths match actual original instructions and attachment precedence");
    std::cout<<"PLAYER_STATE0_INITIALIZER 432 "<<std::hex<<digest<<std::dec<<'\n';
    awl::WorldMapPlayerState0TransitionInput input;
    const auto prepare=[&](std::optional<uint8_t> complete={},std::optional<uint8_t> flag={}){
        return awl::prepare_world_map_player_state0_initializer(input,complete,flag,255);};
    expect(prepare().status==S::RequiresItem,"initializer reads item before action or holder flags");
    input.item_144=0u;expect(prepare().status==S::RequiresAction,"zero item skips its unknown binding");
    input.action_148=0;expect(prepare().status==S::RequiresHolderCompleted,"no attachments reaches holder completed byte");
    expect(prepare(uint8_t{0}).status==S::RequiresHolderFlag,"clear completion reaches second flag");
    expect(prepare(uint8_t{255}).status==S::Prepared && prepare(uint8_t{255}).secondary_byte_3f0==0,"nonzero completed byte skips unknown second flag and clears secondary byte");
    expect(prepare(uint8_t{0},uint8_t{255}).secondary_byte_3f0==255,"clear completion and nonzero flag preserve raw prior secondary byte");
    input.item_144=7u;input.action_148.reset();expect(prepare().status==S::RequiresItemBinding,"nonzero item requires its binding before action");
    input.item_binding_1454=true;expect(prepare().status==S::RequiresItemAttachment,"reached item attachment stops without reading unknown action");
    input.item_binding_1454=false;expect(prepare().status==S::RequiresAction,"null item binding falls through to action");
    input.action_148=-1;expect(prepare().status==S::RequiresActionBinding,"all nonzero action words reach their independent binding");
    input.action_binding_3ec=true;expect(prepare().status==S::RequiresActionAttachment,"action attachment remains untranslated");
    input.action_binding_3ec=false;expect(prepare(uint8_t{0},uint8_t{0}).status==S::Prepared,"two observed null bindings permit the complete no-attachment initializer path");
}
void state0_secondary_checks() {
    using S=awl::WorldMapPlayerState0SecondaryStatus;
    uint64_t digest=14695981039346656037ull;std::array<uint32_t,4> routes{};
    for(uint32_t seed=0;seed<432;++seed){
        const uint32_t control=std::array<uint32_t,4>{0,0x800,UINT32_MAX,0x100}[seed%4];
        const int32_t g680=std::array<int32_t,3>{-1,0,-2}[seed/4%3],g58=std::array<int32_t,3>{0,1,-1}[seed/12%3];
        const uint8_t completed=std::array<uint8_t,3>{0,1,255}[seed/36%3],flag=std::array<uint8_t,4>{0,1,255,128}[seed/108%4];
        const auto result=awl::prepare_world_map_player_state0_secondary(awl::WorldMapPlayerUpdateGuards{g680,g58},completed,flag);
        const uint32_t route=result.status==S::SkippedByGuard?0u:result.status==S::RetainedState?1u:result.status==S::RequiresMovement?2u:3u;
        expect(result.status==S::SkippedByGuard || result.status==S::RetainedState || result.status==S::RequiresMovement || result.status==S::TransitionRequired,"complete secondary observations reach only original compared routes");
        ++routes[route];for(uint32_t v:{seed,control,uint32_t(g680),uint32_t(g58),uint32_t(completed),uint32_t(flag),route,uint32_t(result.requested_state.value_or(-1))})hash(digest,v);
    }
    std::cout<<"STATE0_SECONDARY_COMPARISONS 432 "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0xd08e6e02ef84f371ull && routes==std::array<uint32_t,4>{384,12,4,32},"secondary decisions agree with actual original guards/row/holder getters");
    expect(awl::prepare_world_map_player_state0_secondary({}, {},{}).status==S::RequiresGuards,"second pass requires outer guards first");
    expect(awl::prepare_world_map_player_state0_secondary(awl::WorldMapPlayerUpdateGuards{0,{}},{},{}).status==S::SkippedByGuard,"blocked +680 skips +58c and both holder bytes");
    expect(awl::prepare_world_map_player_state0_secondary(awl::WorldMapPlayerUpdateGuards{-1,{}},{},{}).status==S::RequiresGuardWord58c,"clear +680 requires reached +58c");
    const awl::WorldMapPlayerUpdateGuards guard{-1,0};
    expect(awl::prepare_world_map_player_state0_secondary(guard,{},{}).status==S::RequiresHolderCompleted,"unblocked second pass requires completion byte");
    expect(awl::prepare_world_map_player_state0_secondary(guard,uint8_t{255},{}).requested_state==1,"completed byte wins without reading flag and preserves raw state-one request");
    expect(awl::prepare_world_map_player_state0_secondary(guard,uint8_t{0},{}).status==S::RequiresHolderFlag,"clear completion requires the second byte");
    expect(awl::prepare_world_map_player_state0_secondary(guard,uint8_t{0},uint8_t{128}).status==S::RetainedState,"nonboolean nonzero flag retains state");
    expect(awl::prepare_world_map_player_state0_secondary(guard,uint8_t{0},uint8_t{0}).status==S::RequiresMovement,"zero flags reach actual movement even without a button press");
    digest=14695981039346656037ull;routes={};uint32_t movement_calls=0;
    constexpr std::array<uint32_t,8> intensity_bits{0,0x80000000,1,0x00800000,0x7f7fffff,0xbf800000,0x3f000000,0x3f800000};
    for(uint32_t seed=0;seed<768;++seed){
        const uint32_t control=std::array<uint32_t,4>{0,0x800,UINT32_MAX,0x100}[seed%4];
        const int32_t g680=std::array<int32_t,3>{-1,0,INT32_MIN}[seed/4%3],g58=seed/12%2?-1:0;
        const uint8_t completed=seed/24%2?255u:0u,flag=seed/48%2?128u:0u;const uint32_t raw=intensity_bits[seed/96%8];float intensity;std::memcpy(&intensity,&raw,4);
        const auto prefix=awl::prepare_world_map_player_state0_secondary(awl::WorldMapPlayerUpdateGuards{g680,g58},completed,flag);
        const auto result=awl::prepare_world_map_player_state0_secondary(awl::WorldMapPlayerUpdateGuards{g680,g58},completed,flag,intensity);
        const uint32_t route=result.status==S::SkippedByGuard?0u:result.status==S::RetainedState?1u:result.status==S::RequiresBody?2u:3u;
        expect(result.status==S::SkippedByGuard || result.status==S::RetainedState || result.status==S::RequiresBody || result.status==S::TransitionRequired,"finite observed movement returns reach only original compared routes");
        ++routes[route];const uint32_t moved=prefix.status==S::RequiresMovement;movement_calls+=moved;
        for(uint32_t v:{seed,control,uint32_t(g680),uint32_t(g58),uint32_t(completed),uint32_t(flag),raw,route,uint32_t(result.requested_state.value_or(-1)),moved})hash(digest,v);
    }
    expect(digest==0xa7c1a1b0e2a6ce9dull && routes==std::array<uint32_t,4>{640,32,8,88} && movement_calls==32,"768 original31484/42BBC finite return routes agree, including signed zero, subnormal, negative and maximal finite intensities");
    std::cout<<"STATE0_MOVEMENT_RETURN_COMPARISONS 768 "<<std::hex<<digest<<std::dec<<" movement result supplied; direct-body/state-one callees not executed in this comparison\n";
    expect(awl::prepare_world_map_player_state0_secondary(guard,uint8_t{0},uint8_t{0},std::numeric_limits<float>::quiet_NaN()).status==S::UnsupportedNumerics,"native movement boundary explicitly rejects nonfinite intensity");
    expect(awl::prepare_world_map_player_state0_secondary(guard,uint8_t{255},{},std::numeric_limits<float>::quiet_NaN()).requested_state==1,"completed holder never reads unused movement return");
    digest=14695981039346656037ull;
    for(uint32_t state:{0u,0x29u})for(uint8_t saved:{uint8_t{0},uint8_t{1},uint8_t{255}}){
        const auto status=awl::prepare_world_map_player_normal_actor_gate(saved,{});
        expect(status==(saved?awl::WorldMapPlayerActorGateStatus::RequiresAuxiliaryCallback:awl::WorldMapPlayerActorGateStatus::RequiresScriptOwner),"both normal states reach shared auxiliary-before-script routing");
        for(uint32_t v:{state,uint32_t(saved),uint32_t(status==awl::WorldMapPlayerActorGateStatus::RequiresAuxiliaryCallback),0x1234u})hash(digest,v);
    }
    expect(digest==0x35074d0894ca46e2ull,"both supported actor prefixes agree with original saved-UI routing");
}
void state1_initializer_checks() {
    using S=awl::WorldMapPlayerState1InitializerStatus;using D=awl::WorldMapPlayerInitialAnimationStatus;
    for(bool invented:{false,true}){uint64_t digest=14695981039346656037ull;const uint32_t mask=invented?0x40000080u:2u;
        for(uint32_t seed=0;seed<512;++seed){const auto action=std::array<int32_t,8>{INT32_MIN,-1,0,1,2,3,INT32_MAX,INT32_MIN+1}[seed%8];
            const uint32_t old=seed*0x1020304u^0x89abcdefu;const auto result=awl::prepare_world_map_player_state1_initializer(action,mask,true);
            expect(result.status==S::Prepared && result.mask,"complete initializer yields a mask without touching other state");
            for(uint32_t v:{seed,uint32_t(action),old,result.mask.value_or(UINT32_MAX)})hash(digest,v);}
        expect(digest==(invented?0x0bd2e42caa1bb83full:0xc35cec3fb4fcb873ull),"512 full original state-one initializer/setter cases agree, including mask replacement and signed action boundaries");}
    expect(awl::prepare_world_map_player_state1_initializer({}, {},false).status==S::RequiresAction,"action read precedes constant/item owner");
    expect(awl::prepare_world_map_player_state1_initializer(3,{},false).status==S::RequiresMask,"nonzero/non-two action requires constant before item owner");
    for(int32_t action:{0,2})expect(awl::prepare_world_map_player_state1_initializer(action,{},true).mask==0u &&
        awl::prepare_world_map_player_state1_initializer(action,{},false).status==S::RequiresItemOwner,"zero/two actions skip unknown constants but require the reached item owner");
    auto bytes=initial_input_dol(true);std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> profile;
    expect(awl::decode_world_map_player_initial_animation_inputs(bytes.data(),bytes.size(),&profile)==D::Decoded && profile->state1_action_mask()==0x40000080u,"invented reached mask metadata decodes separately from instruction bodies");
    word(bytes,0x368,0);expect(profile->state1_action_mask()==0x40000080u,"mask metadata owns copied values after source mutation");
    const auto* old=profile.get();word(bytes,0xb8,3);
    expect(awl::decode_world_map_player_initial_animation_inputs(bytes.data(),bytes.size(),&profile)==D::UnsupportedLayout && profile.get()==old,"partial mapped action mask rejects and preserves metadata owner");
    bytes=initial_input_dol();expect(awl::decode_world_map_player_initial_animation_inputs(bytes.data(),bytes.size(),&profile)==D::Decoded && !profile->state1_action_mask(),"legacy profile stays valid without inventing transition constants");
    uint64_t digest=14695981039346656037ull;
    for(uint32_t state:{0u,1u})for(uint32_t seed=0;seed<128;++seed){auto clock=std::make_shared<awl::GameClock>();expect(clock->advance(),"third comparison clock samples loop one");awl::WorldMapUpdateInputs inputs(clock);
        const uint32_t control=seed%2?0x800u:0u;const int32_t g680=seed/2%2?0:-1,g58=int32_t(seed/4%2);
        inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t(seed%2)},0});expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"shared third captures derived control");
        awl::WorldMapPlayerState0ThirdInput observed;observed.guards=awl::WorldMapPlayerUpdateGuards{g680,g58};awl::WorldMapPlayerState0ThirdState before{0x1234u,{}},after=before;
        const auto result=awl::prepare_world_map_player_normal_third(inputs,{},before,observed,&after);
        const bool common=result.status==awl::WorldMapPlayerState0ThirdStatus::RequiresContextInput;
        expect(common || result.status==awl::WorldMapPlayerState0ThirdStatus::SkippedByGuard,"both supported third entries reach original common/skip routes");
        expect(after.control_word_1384==(common?0x1234u:control),"native missing-context stop rolls back original earlier control store");
        for(uint32_t v:{state,seed,control,uint32_t(g680),uint32_t(g58),uint32_t(common),inputs.control()->dispatch_word})hash(digest,v);}
    expect(digest==0xc888a50ce5b88f25ull,"shared third entries agree with actual state-zero/one rows and guard/control decisions");
    digest=14695981039346656037ull;
    for(uint32_t state:{0u,1u,0x29u})for(uint8_t saved:{uint8_t{0},uint8_t{1},uint8_t{255}}){const auto gate=awl::prepare_world_map_player_normal_actor_gate(saved,{});
        for(uint32_t v:{state,uint32_t(saved),uint32_t(gate==awl::WorldMapPlayerActorGateStatus::RequiresAuxiliaryCallback),0x1234u})hash(digest,v);}
    expect(digest==0xa5fc864a0707c481ull,"three supported actor prefixes agree with original auxiliary-before-script routing");
}
void followup_decision_checks() {
    using S=awl::WorldMapPlayerFollowupStatus;
    awl::WorldMapPlayerState29SecondaryInput input;
    const auto status=[&](){return awl::prepare_world_map_player_state29_secondary(input).status;};
    expect(status()==S::RequiresGuards,"secondary wrapper reaches guards before mode or callback data");
    input.guards=awl::WorldMapPlayerUpdateGuards{0,{}};
    expect(status()==S::SkippedByGuard,"blocked first guard skips unknown second guard and secondary callback");
    input.guards=awl::WorldMapPlayerUpdateGuards{-1,{}};
    expect(status()==S::RequiresGuardWord58c,"clear first guard reaches second guard");
    input.guards->word_58c=1;
    expect(status()==S::SkippedByGuard,"blocked second guard skips callback rather than requesting a primary guard transition");
    input.guards->word_58c=0;
    expect(status()==S::RequiresModeWord,"unblocked secondary reaches the distinct mode owner");
    input.mode_word_60=1;
    expect(status()==S::RetainedState,"mode one retains state29 without reading any scene/item fields");
    input.mode_word_60=0;expect(status()==S::RequiresSceneMode,"other mode words reach scene mode");
    for(const auto& pair:std::array<std::pair<int32_t,S>,4>{{{0x1d,S::RequiresSavedByte},{0x25,S::RequiresSecondaryWordC},
        {0x28,S::RequiresSecondaryWord20},{0,S::RequiresSecondaryByte}}}) {
        input.scene_mode_60=pair.first;expect(status()==pair.second,"each reached scene branch requires only its own observation");}
    input.secondary_byte_3f2=uint8_t{0};expect(status()==S::RequiresItemOwner,"clear secondary byte reaches item owner");
    input.item_present=true;expect(status()==S::RequiresItemType,"present item requires resolved save-record type");
    input.item_present=false;expect(status()==S::TransitionRequired,"absent item requests zero without reading type");
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<3072;++seed) {
        const auto i=seed%24;const int32_t scene=i==0?INT32_MIN:i==1?-1:i==2?INT32_MAX:int32_t(i+0x16);
        const auto flags=seed/24%32;const auto mode=std::array<int32_t,4>{1,0,2,INT32_MIN}[seed/768];
        const auto type=std::array<uint8_t,4>{7,0,255,128}[i%4];
        input={};input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.mode_word_60=mode;input.scene_mode_60=scene;
        input.secondary_word_c=flags&1?-1:0;input.secondary_word_20=flags&2?INT32_MIN:0;
        input.saved_byte_14d35=uint8_t(flags&4?255:0);input.secondary_byte_3f2=uint8_t(flags&8?128:0);
        input.item_present=(flags&16)!=0;input.item_type_6=type;
        const auto result=awl::prepare_world_map_player_state29_secondary(input);
        expect(result.status==(mode==1?S::RetainedState:S::TransitionRequired) && result.requested_state.has_value()==(mode!=1),
            "complete secondary observations either retain state29 or stop with a raw requested transition");
        for(uint32_t word:{seed,uint32_t(mode),uint32_t(scene),flags,uint32_t(type),uint32_t(result.requested_state.value_or(-1))})hash(digest,word);
    }
    expect(digest==0x64fd30e8f1e05ce9ull,"3072 secondary decisions match actual original instructions and jump-table destinations");
    std::cout<<"PLAYER_STATE29_SECONDARY_DECISIONS 3072 "<<std::hex<<digest<<std::dec<<'\n';
    digest=14695981039346656037ull;input={};input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};
    input.mode_word_60=0;input.scene_mode_60=0;input.secondary_byte_3f2=uint8_t{0};input.item_present=true;
    for(uint32_t type=0;type<256;++type){input.item_type_6=uint8_t(type);const auto result=awl::prepare_world_map_player_state29_secondary(input);
        expect(result.status==S::TransitionRequired && result.requested_state,"all reached type bytes produce an explicit transition request");
        hash(digest,type);hash(digest,uint32_t(result.requested_state.value_or(-1)));}
    expect(digest==0xbce71a896460c335ull,"all 256 item types match signed-byte original classification");
    digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<768;++seed){const auto first=std::array<int32_t,4>{-1,0,1,INT32_MIN}[seed%4];
        const auto second=std::array<int32_t,3>{0,1,-1}[seed/4%3];const auto control=std::array<uint32_t,4>{0,0x800,0xffffffff,0x12345678}[seed/12%4];
        const auto entry=seed/48%2==0?0x80031484u:0x80031504u;
        input={};input.guards=awl::WorldMapPlayerUpdateGuards{first,second};input.mode_word_60=1;
        const auto result=entry==0x80031484?awl::prepare_world_map_player_state29_secondary(input).status:
            awl::prepare_world_map_player_state29_tertiary(input.guards);
        expect(result==S::RetainedState || result==S::SkippedByGuard,"both followup wrappers handle clear and blocked guards");
        for(uint32_t word:{seed,uint32_t(first),uint32_t(second),control,entry,uint32_t(result==S::RetainedState),control})hash(digest,word);}
    expect(digest==0x52ee49a869dc9fe5ull,"768 guard routes match both original entry wrappers");
}
void parent_update_checks() {
    using S=awl::WorldMapPlayerUpdateStatus;
    awl::WorldMapPlayerUpdateEntry entry;expect(awl::prepare_world_map_player_state29_update_entry(entry)==S::RequiresControlWord,"parent control word must be observed before guard reads");
    entry.control_word=0;expect(awl::prepare_world_map_player_state29_update_entry(entry)==S::RequiresGuardOwner,"missing guard owner cannot run callback");
    entry.guards=awl::WorldMapPlayerUpdateGuards{-1,{}};expect(awl::prepare_world_map_player_state29_update_entry(entry)==S::RequiresGuardWord58c,"clear first guard reaches unknown second guard");
    entry.guards->word_680=0;expect(awl::prepare_world_map_player_state29_update_entry(entry)==S::RequiresGuardTransition,"blocked first guard does not need second guard or scene");
    entry.guards=awl::WorldMapPlayerUpdateGuards{-1,0};expect(awl::prepare_world_map_player_state29_update_entry(entry)==S::RequiresSceneWord,"scene child field remains a distinct reached dependency");
    uint64_t digest=14695981039346656037ull;
    constexpr uint32_t controls[]{0,0x800,0x7ff,UINT32_MAX,0x1000,0x80000000,0x801,0x80000800};
    constexpr int32_t first[]{-1,0,1,INT32_MIN},second[]{0,1,-1},scenes[]{0,1,-1,2};
    for(uint32_t seed=0;seed<384;++seed){const auto control=controls[seed/48];const auto g680=first[seed%4],g58c=second[seed/4%3],scene=scenes[seed/12%4];
        entry={control,awl::WorldMapPlayerUpdateGuards{g680,g680==-1?std::optional<int32_t>{g58c}:std::nullopt},
            g680==-1 && g58c==0?std::optional<int32_t>{scene}:std::nullopt};
        const auto result=awl::prepare_world_map_player_state29_update_entry(entry);
        const auto expected=g680!=-1 || g58c!=0?S::RequiresGuardTransition:scene==1?S::RequiresSceneTransition:S::Prepared;
        expect(result==expected,"state29 guard short circuit and scene decision match original pre-callback routes");
        const uint32_t route=result==S::RequiresGuardTransition?0x80031250u:result==S::RequiresSceneTransition?0x80031364u:0x80031388u;
        for(uint32_t w:{seed,control,uint32_t(g680),uint32_t(g58c),uint32_t(scene),route,control})hash(digest,w);
    }
    expect(digest==0x2DEA788AAF99D6E5ull,"384 parent entry routes agree with bounded actual instructions and trapped unreached reads");
    std::cout<<"PLAYER_PARENT_ENTRY 384 digest "<<std::hex<<digest<<std::dec<<'\n';
    digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<640;++seed){awl::WorldMapPlayerUpdateOwners owners,after;owners.scene_one_position_240=std::array<float,3>{900,901,902};
        constexpr int32_t types[]{0,1,-1,44,INT32_MIN};constexpr uint32_t priorities[]{0,0x100f,0x1010,0x1010,0x1011,0x80000000,UINT32_MAX};
        awl::WorldMapPlayerScenePose pose{types[seed%5],{float(seed%17)-8,seed%2?-0.0f:float(seed%7),float(seed%11)-5},{-0.0f,float(seed%5)-2,2}};
        for(uint32_t i=0;i<seed%8;++i)owners.queue.push_back({i+1,priorities[i]});
        if(seed/8%2 && owners.queue.size()>=4)owners.queue[3].node_identity=99;
        expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,99,&after)==S::Prepared,"state29 tail copies raw pose and inserts or reinserts own queue node");
        for(uint32_t w:{seed,uint32_t(pose.scene_type)})hash(digest,w);
        const auto vector=[&](const auto& values){for(float v:values){uint32_t bits;std::memcpy(&bits,&v,4);hash(digest,bits);}};
        vector(pose.position);vector(pose.heading);hash(digest,uint32_t(after.saved_pose.scene_type));vector(after.saved_pose.position);vector(after.saved_pose.heading);
        vector(after.position_802fc20c);vector(*after.scene_one_position_240);hash(digest,uint32_t(after.queue.size()));
        for(const auto& item:after.queue){hash(digest,uint32_t(item.node_identity));hash(digest,item.priority);}
        auto alias=owners;expect(awl::prepare_world_map_player_state29_update_tail(alias,pose,99,&alias)==S::Prepared && update_owners_snapshot(alias)==update_owners_snapshot(after),"tail supports input/output aliasing with identical state");
    }
    expect(digest==0x912F8894AA493C9Aull,"640 tail cases match actual pose/position and queue insertion/unlink instruction bodies");
    std::cout<<"PLAYER_PARENT_TAIL 640 digest "<<std::hex<<digest<<std::dec<<'\n';
    awl::WorldMapPlayerUpdateOwners owners,after;after.queue={{8,4}};const auto untouched=update_owners_snapshot(after);
    awl::WorldMapPlayerScenePose pose{1,{2,0,0},{-0.0f,0,2}};
    expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,99,&after)==S::RequiresScenePositionOwner && update_owners_snapshot(after)==untouched,"scene-one copy needs its owner and preserves output");
    pose.scene_type=2;expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,0,&after)==S::RequiresQueueNode && update_owners_snapshot(after)==untouched,"queue node cannot be invented");
    for(const auto& entries:std::vector<std::vector<awl::WorldMapPlayerUpdateQueueEntry>>{{{0,1}},{{1,2},{2,1}},{{1,1},{1,2}}}){owners.queue=entries;
        expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,99,&after)==S::InvalidQueue && update_owners_snapshot(after)==untouched,"null keys, unordered priorities and duplicate identities reject atomically");}
    owners.queue={{99,0}};expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,99,&after)==S::UnsupportedQueuePriorityChange && update_owners_snapshot(after)==untouched,"changing an existing priority is not inferred from normal update");
    owners.queue.clear();pose.heading[1]=std::numeric_limits<float>::infinity();
    expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,99,&after)==S::InvalidInput && update_owners_snapshot(after)==untouched,"nonfinite heading does not publish earlier pose/position writes");
    pose.heading[1]=0;pose.position[1]=std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_player_state29_update_tail(owners,pose,99,&after)==S::InvalidInput && update_owners_snapshot(after)==untouched &&
        awl::prepare_world_map_player_state29_update_tail(owners,pose,99,nullptr)==S::InvalidInput,"nonfinite position and null output preserve state");
}
void state29_camera_checks() {
    using S=awl::WorldMapPlayerState29CameraStatus;uint64_t digest=14695981039346656037ull;
    awl::WorldMapPlayerState29CameraQuery q;q.previous_camera.target.distance_30=2;q.previous_camera.up_vector_24={0,1,0};
    q.previous_camera.target.camera.position={999,999,999};
    for(uint32_t seed=0;seed<512;++seed){constexpr uint32_t counters[]{0,1,2,3,UINT32_MAX,0x80000000};
        constexpr int32_t actions[]{0,1,-1,INT32_MIN};q.counter_1368=counters[seed%6];q.action_148=actions[(seed/6)%4];
        q.player_position={float(seed%17)-8,float((seed/17)%7)-3,float(seed%11)-5};State29HeightProbe probe;
        const auto result=awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe);
        const bool blocked=q.counter_1368==2 && *q.action_148!=0;
        expect(result.status==(blocked?S::RequiresAuxiliarySource:S::Prepared) && probe.calls==2 && result.final_camera && result.update &&
            result.final_camera->target.camera.position==q.player_position && result.counter_after.has_value()==!blocked &&
            (blocked || *result.counter_after==q.counter_1368+1u),"state29 copies position, completes both camera queries, gates source before increment and wraps word counter");
        for(uint32_t v:{seed,q.counter_1368,static_cast<uint32_t>(*q.action_148),0x80085884u,0x80085998u})hash(digest,v);
        for(float v:q.player_position){uint32_t bits;std::memcpy(&bits,&v,4);hash(digest,bits);}hash(digest,blocked);hash(digest,result.counter_after.value_or(UINT32_MAX));
    }
    expect(digest==0xE81DBA5EC8A4E964ull,"512 callback copy/order/counter/dependency results match mapped original instructions with camera/source/animation calls supplied");
    std::cout<<"PLAYER_STATE29_CALLBACK 512 digest "<<std::hex<<digest<<std::dec<<'\n';
    q.counter_1368=2;q.action_148.reset();q.player_position={2,0,0};State29HeightProbe probe;
    auto result=awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe);
    expect(result.status==S::RequiresActionTarget && probe.calls==2 && result.update && !result.counter_after,"unknown action blocks only after both camera queries at counter two");
    for(unsigned fail:{1u,2u}){probe={};probe.fail_at=fail;result=awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe);
        expect(result.status==S::CameraFailure && probe.calls==fail && !result.update && !result.final_camera && !result.counter_after,"first/second terrain failure precedes unknown action and exposes no complete camera/counter result");}
    probe={};probe.nonfinite=true;expect(awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe).status==S::CameraFailure && probe.calls==1,"nonfinite sampled height rejects before action");
    q.counter_1368=1;probe={};expect(awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe).status==S::Prepared,"unreached action remains unknown outside counter two");
    q.previous_camera.target.camera.yaw=0.25f;q.previous_camera.target.camera.field_18=0.5f;q.previous_camera.target.camera.flag_99=true;
    q.previous_camera.target.pitch_offset_8c=0.125f;q.previous_camera.target.yaw_offset_90=-0.125f;probe={};result=awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe);
    expect(result.status==S::Prepared && result.final_camera->target.camera.yaw==0.25f && result.final_camera->target.camera.field_18==0.5f &&
        result.final_camera->target.camera.flag_99 && result.final_camera->target.pitch_offset_8c==0 && result.final_camera->target.yaw_offset_90==0,
        "state29 preserves region/yaw/flags and clears only post-update temporary offsets");
    q.player_position[0]=std::numeric_limits<float>::quiet_NaN();probe={};expect(awl::prepare_world_map_player_state29_camera(q,State29HeightProbe::sample,&probe).status==S::CameraFailure && !probe.calls,"nonfinite actor position rejects before height queries");
    expect(awl::prepare_world_map_player_state29_camera(q,nullptr,nullptr).status==S::InvalidInput,"missing sampler cannot invent a camera source");
}
std::vector<uint8_t> startup_camera_col() {
    // Invented single-leaf camera COL: one flat Y=0 triangle, scale one.
    std::vector<uint8_t> b(92);word(b,0,0xE7E3F1F4);b[4]=1;
    word(b,8+4,10);word(b,8+8,0x000A000A); // bounds (0,0,0)..(10,10,10)
    word(b,8+0x1C,1);word(b,8+0x20,3);word(b,8+0x28,0x34);word(b,8+0x2C,0x3C);
    word(b,60,0x00200000);word(b,64,0x00010002);word(b,76,0x000A0000);word(b,84,0);word(b,88,0x000A0000);return b;
}
void parent_owner_checks(const awl::WorldMapPlayerStartupProviders& providers,const awl::WorldMapPlayerStartupQuery& query,
    const awl::WorldMapPlayerPrimaryFrameInput& frame,const std::shared_ptr<awl::WorldMapCameraCollisionAsset>& camera,
    const awl::WorldMapCameraViewQuery& camera_query) {
    using S=awl::WorldMapPlayerUpdateStatus;using C=awl::WorldMapPlayerState29CameraStatus;
    std::unique_ptr<awl::WorldMapPlayerStartup> owner;
    expect(awl::construct_world_map_player_startup(providers,query,&owner).status==awl::WorldMapPlayerStartupStatus::ConstructedCpuStartup && owner &&
        owner->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"parent update fixture owns an evaluated CPU startup");if(!owner)return;
    awl::WorldMapPlayerUpdateOwners owners;owners.saved_pose={7,{40,50,60},{3,2,1}};owners.position_802fc20c={70,80,90};
    owners.queue={{1,0x100f},{2,0x1010},{3,UINT32_MAX}};
    awl::WorldMapPlayerStartupUpdateInput input;input.pose={1,{2,0,0},{-0.0f,0,2}};input.player_node_identity=99;input.action_148=0;
    input.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera,camera_query};
    const auto before=startup_snapshot(*owner),effects_before=update_owners_snapshot(owners),model_before=holder_snapshot(owner->model());
    const auto clock_before=providers.clock->state();
    const auto preserved=[&](){return startup_snapshot(*owner)==before && update_owners_snapshot(owners)==effects_before;};
    auto step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresControlWord && !step.callback && preserved(),"unknown control word stops before camera with no constructor value guessed");
    input.entry.control_word=0x800;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresGuardOwner && !step.callback && preserved(),"missing guard owner preserves full prior frame and effects");
    input.entry.guards=awl::WorldMapPlayerUpdateGuards{0,{}};step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresGuardTransition && !step.callback && preserved(),"blocked guard stops before unknown second guard, scene and callback");
    input.entry.guards=awl::WorldMapPlayerUpdateGuards{-1,0};step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresSceneWord && !step.callback && preserved(),"scene child field cannot be replaced with pose scene type");
    input.entry.scene_child_word_4=1;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresSceneTransition && !step.callback && preserved(),"scene transition precedes state29 camera and counter");
    input.entry.scene_child_word_4=0;input.entry.control_word=0;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresStatusUpdate && step.callback && step.callback->status==C::Prepared && preserved(),"control bit clear reaches untranslated status pair only after staged callback, without accepting camera/control/counter");
    input.entry.control_word=0x800;step=owner->advance_state29_parent(input,nullptr);
    expect(step.status==S::RequiresEffectOwners && step.callback && step.callback->status==C::Prepared && preserved(),"late missing effect owners roll back successful camera calculation");
    step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresScenePositionOwner && step.callback && preserved(),"scene-one position owner is required after callback and before queue publication");
    owners.scene_one_position_240=std::array<float,3>{90,91,92};const auto ready_effects=update_owners_snapshot(owners);
    const auto ready_preserved=[&](){return startup_snapshot(*owner)==before && update_owners_snapshot(owners)==ready_effects;};
    input.player_node_identity=0;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::RequiresQueueNode && ready_preserved(),"late unknown queue node preserves all owners");
    input.player_node_identity=99;input.pose.heading[2]=std::numeric_limits<float>::quiet_NaN();step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::InvalidInput && step.callback && step.callback->status==C::Prepared && ready_preserved(),"invalid tail heading rolls back a complete camera result");
    input.pose.heading[2]=2;input.first_camera->camera.target.distance_30=0;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::CameraIncomplete && step.callback && step.callback->status==C::CameraFailure && ready_preserved(),"camera failure prevents tail owner/queue writes");
    input.first_camera->camera=camera_query;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live_before=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<64;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;step=owner->advance_state29_parent(input,&owners);allocation_probe::enabled=false;
        if(step.status==S::Advanced){reached=true;break;}++rejected;
        expect(step.status==S::AllocationFailure && ready_preserved() && allocation_probe::live==live_before,"each camera and late queue-copy/insertion allocation failure preserves owner/effects and frees staging");}
    expect(reached && rejected>=10,"parent allocation sweep reaches both camera passes, queue copy and insertion before success");
    std::cout<<"PLAYER_PARENT_ALLOCATION_FAILURES "<<rejected<<'\n';
#else
    step=owner->advance_state29_parent(input,&owners);
#endif
    expect(step.status==S::Advanced && step.callback && step.callback->status==C::Advanced && owner->state().counter_1368==1 && owner->state().control_word_1384==0x800u &&
        owners.saved_pose.scene_type==1 && owners.saved_pose.position==input.pose.position && owners.saved_pose.heading==input.pose.heading &&
        owners.position_802fc20c==input.pose.position && owners.scene_one_position_240==input.pose.position && owners.queue.size()==4 &&
        owners.queue[1].node_identity==99 && owners.queue[2].node_identity==2 && owners.queue[3].priority==UINT32_MAX,"complete parent publishes camera/counter/control, direct raw pose copies and unsigned insertion before equal priorities together");
    input.first_camera.reset();input.entry.control_word=0x80000800;input.pose.scene_type=2;input.pose.position[0]=3;
    std::swap(owners.queue[1],owners.queue[2]);step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::Advanced && owner->state().counter_1368==2 && owner->state().control_word_1384==0x80000800u && owners.queue.size()==4 &&
        owners.queue[1].node_identity==99 && owners.scene_one_position_240==std::array<float,3>{2,0,0} && owners.position_802fc20c==input.pose.position,
        "repeated parent update reinserts existing node ahead of equals, copies new position and preserves unreached scene-one owner");
    const auto accepted=startup_snapshot(*owner),accepted_effects=update_owners_snapshot(owners);input.action_148.reset();
    step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::CameraIncomplete && step.callback && step.callback->status==C::RequiresActionTarget && startup_snapshot(*owner)==accepted &&
        update_owners_snapshot(owners)==accepted_effects,"counter-two unknown action preserves previously accepted parent/effects");
    input.action_148=-1;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::CameraIncomplete && step.callback && step.callback->status==C::RequiresAuxiliarySource && startup_snapshot(*owner)==accepted &&
        update_owners_snapshot(owners)==accepted_effects,"nonzero attachment action cannot reach post-callback owners");
    input.action_148=0;step=owner->advance_state29_parent(input,&owners);
    expect(step.status==S::Advanced && owner->state().counter_1368==3 && owners.queue.size()==4 && holder_snapshot(owner->model())==model_before &&
        providers.clock->state().raw_time==clock_before.raw_time && providers.clock->state().loop_count==clock_before.loop_count,"successful retry completes parent update without model/frame or shared-clock advancement");
}
void dispatch_owner_checks(awl::WorldMapPlayerStartupProviders providers,const awl::WorldMapPlayerStartupQuery& query,
    const awl::WorldMapPlayerPrimaryFrameInput& frame,const std::shared_ptr<awl::WorldMapCameraCollisionAsset>& camera,
    const awl::WorldMapCameraViewQuery& camera_query) {
    using S=awl::WorldMapPlayerUpdateStatus;using C=awl::WorldMapControlStatus;using F=awl::WorldMapSceneFilterStatus;
    providers.clock=std::make_shared<awl::GameClock>();
    std::unique_ptr<awl::WorldMapPlayerStartup> player;
    expect(awl::construct_world_map_player_startup(providers,query,&player).status==awl::WorldMapPlayerStartupStatus::ConstructedCpuStartup && player &&
        player->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"dispatch fixture owns its own shared clock and complete CPU startup");if(!player)return;
    awl::WorldMapUpdateInputs inputs(providers.clock);awl::WorldMapPlayerStartupDispatchInput input;
    awl::WorldMapPlayerUpdateOwners owners;owners.scene_one_position_240=std::array<float,3>{9,8,7};
    const auto before=startup_snapshot(*player),effects=update_owners_snapshot(owners),model=holder_snapshot(player->model());
    const auto preserved=[&](){return startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects;};
    auto step=player->dispatch_state29_parent(inputs,input,nullptr);
    expect(step.status==S::RequiresDispatchControl && !step.callback && preserved(),"dispatch cannot use an uncaptured word or reach unknown callback owners");
    inputs.request_scene_mode(4,0);awl::WorldMapSceneModeCommit commit;
    expect(inputs.commit_scene_fields(0,uint8_t{1},&commit)==F::CommittedFields,"mode-four fixture blocks the real player priority group");
    inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
    expect(inputs.capture_control()==C::Captured && inputs.control()->dispatch_word==0x800u && inputs.scene().global_flag_59af==0,"derived override capture consumes the owned scene-request event");
    step=player->dispatch_state29_parent(inputs,input,nullptr);
    expect(step.status==S::SkippedByDispatchFilter && !step.callback && preserved(),"filtered node needs no guard, scene, pose, camera or effect owner and changes no player state");
    inputs.request_scene_mode(4,0);
    expect(inputs.commit_scene_fields(0,uint8_t{0},&commit)==F::CommittedFields && inputs.capture_control()==C::AlreadyCaptured && inputs.scene().global_flag_59af==1,
        "unblocking scene commit retains earlier control and newer event until a later loop");
    step=player->dispatch_state29_parent(inputs,input,&owners);
    expect(step.status==S::RequiresGuardOwner && !step.callback && preserved(),"allowed dispatch reaches existing ordered unknown-guard stop");
    awl::WorldMapUpdateInputs wrong_clock(std::make_shared<awl::GameClock>());
    wrong_clock.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
    expect(wrong_clock.capture_control()==C::Captured,"independent clock fixture captures");
    step=player->dispatch_state29_parent(wrong_clock,input,&owners);
    expect(step.status==S::InvalidInput && !step.callback && preserved(),"allowed dispatch rejects a clock different from the retained player clock");
    input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.scene_child_word_4=0;
    input.pose={1,{2,0,0},{-0.0f,0,2}};input.action_148=0;input.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera,camera_query};
    step=player->dispatch_state29_parent(inputs,input,nullptr);
    expect(step.status==S::RequiresEffectOwners && step.callback && preserved() && inputs.control()->dispatch_word==0x800u && inputs.scene().global_flag_59af==1,
        "late parent stop preserves player/effects and keeps captured control plus later event available for retry");
    input.player_node_identity=99;step=player->dispatch_state29_parent(inputs,input,&owners);
    expect(step.status==S::Advanced && player->state().counter_1368==1 && player->state().control_word_1384==0x800u &&
        owners.saved_pose.position==input.pose.position && owners.saved_pose.heading==input.pose.heading && owners.queue.size()==1 &&
        owners.queue[0].node_identity==99 && inputs.scene().global_flag_59af==1 && holder_snapshot(player->model())==model,
        "successful retry uses owned control and fixed priority, publishes existing parent path and leaves pending later event/model untouched");
    input.first_camera.reset();const auto accepted_effects=update_owners_snapshot(owners);
    expect(providers.clock->advance(),"dispatch fixture advances only its own clock");
    const auto accepted=startup_snapshot(*player); // Include the intentional shared-clock advance.
    step=player->dispatch_state29_parent(inputs,input,&owners);
    expect(step.status==S::RequiresDispatchControl && !step.callback && startup_snapshot(*player)==accepted && update_owners_snapshot(owners)==accepted_effects,
        "next loop cannot dispatch a stale snapshot");
    inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t{0}},0});
    expect(inputs.capture_control()==C::Captured && inputs.control()->dispatch_word==0x2100u && inputs.scene().global_flag_59af==0 &&
        inputs.scene().global_flag_59b0==0,"active next-loop control includes both flags from the classified mode-four request without forcing skip-status bit");
    step=player->dispatch_state29_parent(inputs,input,&owners);
    expect(step.status==S::RequiresStatusUpdate && step.callback && startup_snapshot(*player)==accepted && update_owners_snapshot(owners)==accepted_effects,
        "derived active control stops at untranslated status pair after staging camera, preserving complete parent/effects");
    inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
    expect(inputs.capture_control()==C::AlreadyCaptured && inputs.control()->dispatch_word==0x2100u,"late source change cannot rewrite current-loop control to bypass status dependency");
    step=player->dispatch_state29_parent(inputs,input,&owners);
    expect(step.status==S::RequiresStatusUpdate && startup_snapshot(*player)==accepted && update_owners_snapshot(owners)==accepted_effects,
        "same-loop retry retains the same unsupported-status stop");
    expect(providers.clock->advance() && inputs.capture_control()==C::Captured && inputs.control()->dispatch_word==0x800u,"later-loop observed override creates a new supported control snapshot");
    const auto later_model=holder_snapshot(player->model());
    step=player->dispatch_state29_parent(inputs,input,&owners);
    expect(step.status==S::Advanced && player->state().counter_1368==2 && owners.queue.size()==1 && holder_snapshot(player->model())==later_model,
        "later supported snapshot completes callback without duplicate queue nodes or model progression");
    inputs.request_scene_mode(4,0);
    expect(inputs.commit_scene_fields(0,uint8_t{1},&commit)==F::CommittedFields,"dispatch fixture restores group block");
    const auto final=startup_snapshot(*player),final_effects=update_owners_snapshot(owners);input={};input.pose.position[0]=std::numeric_limits<float>::quiet_NaN();
    step=player->dispatch_state29_parent(inputs,input,nullptr);
    expect(step.status==S::SkippedByDispatchFilter && !step.callback && startup_snapshot(*player)==final && update_owners_snapshot(owners)==final_effects,
        "filtered later invocation skips malformed and unknown callback observations without acknowledgement");
    std::cout<<"PLAYER_OWNED_DISPATCH blocked/allowed/stale/status/retry checks\n";
}
void status_parent_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapPlayerUpdateOwners& effects,
    awl::WorldMapPlayerStartupDispatchInput input,bool allocations=false) {
    using S=awl::WorldMapPlayerUpdateStatus;using P=awl::WorldMapPlayerStatusCode;using G=awl::WorldMapPlayerStatusGlobal;
    awl::WorldMapUpdateInputs controls(player.model().game_clock());
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t{0}},0});
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured && (controls.control()->dispatch_word&0x800u)==0,
        "status integration derives active control from the shared owned sources");
    const auto before=startup_snapshot(player),model=holder_snapshot(player.model());const auto counter=player.state().counter_1368;
    const auto clock=player.model().game_clock()->state();const auto without_status=update_owners_snapshot(effects);
    auto step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::RequiresStatusUpdate && step.callback && !step.status_update && startup_snapshot(player)==before &&
        update_owners_snapshot(effects)==without_status,"missing pointed status owner stops after staged camera without publishing");
    awl::WorldMapPlayerStatusState state;state.word_4=100000;state.word_8=200000;
    state.word_14=1000000;state.word_18=2000000;state.word_12c=123455;state.word_150=77;state.word_154=88;
    effects.status=state;input.status_factors_override.emplace();const auto pending=update_owners_snapshot(effects);
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(effects)==pending;};
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::RequiresStatusUpdate && step.status_update && step.status_update->status==P::RequiresGlobal &&
        step.status_update->required_global==G::Word7ec0 && !step.status_update->after && preserved(),
        "active dispatch retains exact global dependency without accepting first decay/camera/counter");
    constexpr uint32_t coefficients[]{200000,300,1200000,1800000,75,100,60,100,uint32_t(-3),45,100,100,3};
    for(size_t i=0;i<input.status_factors_override->globals.size();++i)input.status_factors_override->globals[i]=coefficients[i];
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status_update && step.status_update->status==P::RequiresRate && preserved(),"rate table word is an explicit keyed dependency after first decay");
    const auto code=player.model().descriptor().word_8;
    input.status_factors_override->rate=awl::WorldMapPlayerStatusRate{code^1u,1000};
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status_update && step.status_update->status==P::RequiresRate && preserved(),"supplied rate cannot replace the current retained descriptor code");
    input.status_factors_override->rate->descriptor_word_8=code;
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},{},uint8_t{0}},0});
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status_update && step.status_update->status==P::RequiresPhaseRaw && preserved(),"phase raw time is separate from the owned GameClock");
    const auto node=input.player_node_identity;input.player_node_identity=0;
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},state.word_12c,uint8_t{0}},0});
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::RequiresQueueNode && step.status_update && step.status_update->status==P::Prepared && preserved(),
        "late queue stop rolls back a complete curved status proposal and staged camera");
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},123456u,uint8_t{0}},0});
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::RequiresQueueNode && step.status_update && step.status_update->status==P::Prepared && preserved(),
        "late missing queue node rolls back complete status and camera proposals");
    input.player_node_identity=node;const auto heading=input.pose.heading;input.pose.heading[2]=std::numeric_limits<float>::quiet_NaN();
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::InvalidInput && step.status_update && step.status_update->status==P::Prepared && preserved(),
        "invalid tail heading preserves complete pointed status owner and parent");
    input.pose.heading=heading;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){step={};const auto live=allocation_probe::live;size_t rejected=0;bool accepted=false;
        for(size_t fail=0;fail<64;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            step=player.dispatch_state29_parent(controls,input,&effects);allocation_probe::enabled=false;
            if(step.status==S::Advanced){accepted=true;break;}++rejected;
            expect(step.status==S::AllocationFailure && preserved() && allocation_probe::live==live,
                "camera and late queue allocation failures preserve status/parent/effects and release staging");}
        expect(accepted && rejected>=10,"active-status allocation sweep reaches successful atomic parent publication");
        std::cout<<"PLAYER_STATUS_PARENT_ALLOCATION_FAILURES "<<rejected<<'\n';
    }else step=player.dispatch_state29_parent(controls,input,&effects);
#else
    (void)allocations;step=player.dispatch_state29_parent(controls,input,&effects);
#endif
    expect(step.status==S::Advanced && step.status_update && step.status_update->status==P::Prepared && effects.status &&
        effects.status->word_14==999960 && effects.status->word_4==(code<64?100017u:99945u) &&
        effects.status->word_150==(code<64?1000u:77u) && effects.status->word_154==(code<64?88u:1000u) &&
        player.state().counter_1368==counter+1 && player.state().control_word_1384==controls.control()->dispatch_word &&
        effects.saved_pose.position==input.pose.position && effects.position_802fc20c==input.pose.position &&
        holder_snapshot(player.model())==model && player.model().game_clock()->state().raw_time==clock.raw_time &&
        player.model().game_clock()->state().loop_count==clock.loop_count,
        "active owned dispatch atomically publishes exact status/camera/pose/queue changes without model or clock progression");
    size_t copies=0;bool first_equal=true;
    for(const auto& entry:effects.queue){if(entry.node_identity==node){++copies;expect(entry.priority==0x1010 && first_equal,"status parent enqueues before equal priorities");}
        if(entry.priority==0x1010)first_equal=false;}
    expect(copies==1 && player.camera() && player.camera()->camera.target.camera.position==input.pose.position,
        "active status publication includes one queued player node and the prepared camera position");
    input.first_camera.reset();const auto accepted=startup_snapshot(player),accepted_effects=update_owners_snapshot(effects);
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},state.word_12c,uint8_t{0}},0});
    const int original_rounding=std::fegetround();expect(std::fesetround(FE_DOWNWARD)==0,"unsupported curve rounding setup");
    step=player.dispatch_state29_parent(controls,input,&effects);
    const int preserved_rounding=std::fegetround();expect(std::fesetround(original_rounding)==0,"restore parent test rounding");
    expect(step.status==S::RequiresStatusUpdate && step.status_update && step.status_update->status==P::UnsupportedNumerics &&
        preserved_rounding==FE_DOWNWARD &&
        startup_snapshot(player)==accepted && update_owners_snapshot(effects)==accepted_effects,
        "unsupported curve numerics preserve previously accepted status/camera/queue and caller rounding");
    awl::WorldMapPlayerStartupUpdateInput skipped;skipped.entry={0x800u,input.guards,input.scene_child_word_4};
    skipped.pose=input.pose;skipped.action_148=0;skipped.player_node_identity=node;
    step=player.advance_state29_parent(skipped,&effects);
    expect(step.status==S::Advanced && !step.status_update && effects.status && effects.status->word_14==999960 &&
        effects.status->word_4==(code<64?100017u:99945u),"bit800 skips all missing status inputs and preserves accepted status values");
    const auto curved_counter=player.state().counter_1368;
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::Advanced && step.status_update && step.status_update->status==P::Prepared && effects.status &&
        effects.status->word_14==999920 && effects.status->word_4==(code<64?100029u:99885u) &&
        effects.status->word_150==(code<64?1000u:77u) && effects.status->word_154==(code<64?88u:1000u) &&
        player.state().counter_1368==curved_counter+1 && effects.saved_pose.position==input.pose.position &&
        effects.position_802fc20c==input.pose.position && holder_snapshot(player.model())==model &&
        player.model().game_clock()->state().raw_time==clock.raw_time && player.model().game_clock()->state().loop_count==clock.loop_count,
        "second curved status update matches original instructions and atomically publishes without model/clock progression");
    copies=0;for(const auto& entry:effects.queue)if(entry.node_identity==node)++copies;
    expect(copies==1 && player.camera() && player.camera()->camera.target.camera.position==input.pose.position,
        "curved status success retains a single queued player node and camera publication");
}
auto status_words(const awl::WorldMapPlayerStatusState& s) {
    return std::array<uint32_t,11>{s.word_4,s.word_8,s.word_c,s.word_14,s.word_18,s.word_1c,
        s.word_20,s.word_12c,s.word_150,s.word_154,s.word_160};
}
void owned_status_parent_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapPlayerUpdateOwners& effects,
    awl::WorldMapPlayerStartupDispatchInput input) {
    using S=awl::WorldMapPlayerUpdateStatus;using P=awl::WorldMapPlayerStatusCode;
    awl::WorldMapUpdateInputs controls(player.model().game_clock());
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t{0}},0});
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured,"owned status parameter test captures active control");
    input.first_camera.reset();input.status_factors_override.reset();
    awl::WorldMapPlayerStatusState state;state.word_4=100000;state.word_8=200000;
    state.word_14=1000000;state.word_18=2000000;state.word_12c=123455;state.word_150=77;state.word_154=88;
    effects.status=state;
    const auto before=startup_snapshot(player),pending=update_owners_snapshot(effects),model=holder_snapshot(player.model());
    const auto clock=player.model().game_clock()->state();const auto counter=player.state().counter_1368;
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(effects)==pending;};
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},{},uint8_t{0}},0});
    auto step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::RequiresStatusUpdate && step.status_update && step.status_update->status==P::RequiresPhaseRaw && preserved(),
        "owned DOL parameters do not fabricate the separate phase source");
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},state.word_12c,uint8_t{0}},0});
    input.status_factors_override.emplace();step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status_update && step.status_update->status==P::RequiresGlobal &&
        step.status_update->required_global==awl::WorldMapPlayerStatusGlobal::Word7ec0 && preserved(),
        "explicit empty diagnostic override is never filled from owned DOL parameters");
    input.status_factors_override->globals[0]=300000;step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status_update && step.status_update->status==P::RequiresGlobal &&
        step.status_update->required_global==awl::WorldMapPlayerStatusGlobal::Word7ebc && preserved(),
        "partial diagnostic override preserves its own next reached dependency");
    input.status_factors_override.reset();const auto node=input.player_node_identity;input.player_node_identity=0;
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::RequiresQueueNode && step.status_update && step.status_update->status==P::Prepared && preserved(),
        "late queue stop rolls back status prepared from retained parameters");
    input.player_node_identity=node;
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"owned parameter rounding rejection setup");
    step=player.dispatch_state29_parent(controls,input,&effects);
    const int kept=std::fegetround();expect(std::fesetround(rounding)==0,"restore owned parameter rounding");
    expect(step.status_update && step.status_update->status==P::UnsupportedNumerics && kept==FE_UPWARD && preserved(),
        "owned parameter curve failure preserves all accepted state and caller rounding");
    const auto factors=player.starts()->status_factors(player.model().descriptor());
    const bool actual=player.starts()->target_verified();
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::Advanced && step.status_update && effects.status && factors.rate &&
        effects.status->word_4==(actual?100003u:99940u) && effects.status->word_14==(actual?999980u:999960u) &&
        effects.status->word_150==(actual?uint32_t(factors.rate->value):77u) && effects.status->word_154==(actual?88u:uint32_t(factors.rate->value)) &&
        player.state().counter_1368==counter+1,"default dispatch uses retained actual or invented DOL coefficients/rate for the curved status update");
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},state.word_12c+1,uint8_t{0}},0});
    step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==S::Advanced && effects.status && effects.status->word_4==(actual?100008u:99885u) &&
        effects.status->word_14==(actual?999960u:999920u) && player.state().counter_1368==counter+2 &&
        effects.saved_pose.position==input.pose.position && effects.position_802fc20c==input.pose.position &&
        player.camera() && player.camera()->camera.target.camera.position==input.pose.position && holder_snapshot(player.model())==model &&
        player.model().game_clock()->state().raw_time==clock.raw_time && player.model().game_clock()->state().loop_count==clock.loop_count,
        "second owned constant status update publishes pose/camera/counter together without model or clock progression");
    size_t copies=0;for(const auto& entry:effects.queue)if(entry.node_identity==node)++copies;
    expect(copies==1,"owned parameter updates preserve one queued player node");
}
void fresh_status_parent_checks(awl::WorldMapPlayerStartup& player,const awl::WorldMapPlayerUpdateOwners& prior_effects,
    awl::WorldMapPlayerStartupDispatchInput input) {
    using P=awl::WorldMapPlayerStatusCode;using U=awl::WorldMapPlayerUpdateStatus;
    // A separate fresh saved-owner projection, preserving the earlier accepted fixture.
    auto effects=prior_effects;effects.status.reset();
    awl::WorldMapUpdateInputs controls(player.model().game_clock());
    const auto before=startup_snapshot(player),pending=update_owners_snapshot(effects),model=holder_snapshot(player.model());
    const auto clock=player.model().game_clock()->state();const auto counter=player.state().counter_1368;
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(effects)==pending;};
    expect(player.initialize_fresh_status(controls,nullptr)==P::InvalidInput &&
        player.initialize_fresh_status(controls,&effects)==P::RequiresPhaseRaw && preserved(),
        "fresh status requires output and its phase owner without changing any accepted state");
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},{}},{}});
    expect(player.initialize_fresh_status(controls,&effects)==P::RequiresPhaseRaw && preserved(),
        "constructor requires phase raw even when control byte8 is clear");
    awl::WorldMapUpdateInputs foreign(std::make_shared<awl::GameClock>());
    foreign.observe_control_sources({awl::WorldMapControlPhase{{},UINT32_MAX,{}},{}});
    expect(player.initialize_fresh_status(foreign,&effects)==P::InvalidInput && preserved(),
        "fresh status rejects a foreign clock boundary before publication");
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},UINT32_MAX,{}},{}});
    expect(player.initialize_fresh_status(controls,&effects)==P::Prepared && effects.status && !controls.control() &&
        startup_snapshot(player)==before,"fresh status reads the phase source without capturing control or changing player/model/clock");
    if(!effects.status)return;
    const bool actual=player.starts()->target_verified();const auto& fresh=*effects.status;
    expect(fresh.word_4==(actual?100000u:90000u) && fresh.word_20==(actual?50000u:24999u) &&
        fresh.word_14==(actual?1350000u:1200000u) && fresh.word_12c==2159999 && fresh.word_24==99999 &&
        fresh.word_28==0 && fresh.byte_2e==1 && fresh.byte_32==1 && fresh.word_150==100 && fresh.word_154==100,
        "fresh owner composes traced constructor and actual or invented reset constants");
    const auto initialized=update_owners_snapshot(effects);
    controls.observe_control_sources({});
    expect(player.initialize_fresh_status(controls,&effects)==P::AlreadyInitialized && update_owners_snapshot(effects)==initialized &&
        startup_snapshot(player)==before,"repeated initialization never resets a prior status or reads missing phase");
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},UINT32_MAX,uint8_t{0}},0});
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured && (controls.control()->dispatch_word&0x800u)==0,
        "fresh owner test captures active control without bypassing status");
    input.first_camera.reset();input.status_factors_override.reset();const auto node=input.player_node_identity;input.player_node_identity=0;
    auto step=player.dispatch_state29_parent(controls,input,&effects);
    expect(step.status==U::RequiresQueueNode && step.status_update && step.status_update->status==P::Prepared &&
        update_owners_snapshot(effects)==initialized && startup_snapshot(player)==before,
        "late queue failure rolls back prepared fresh status and parent/camera for retry");
    input.player_node_identity=node;
    const auto rate=player.starts()->status_factors(player.model().descriptor()).rate;
    for(uint32_t tick=0;tick<3;++tick) {
        controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},UINT32_MAX+tick,uint8_t{0}},0});
        step=player.dispatch_state29_parent(controls,input,&effects);
        expect(step.status==U::Advanced && effects.status && rate &&
            effects.status->word_4==(actual?100000u:90000u-59u*(tick+1)) &&
            effects.status->word_14==(actual?1350000u-20u*(tick+1):1200000u-46u*(tick+1)) &&
            effects.status->word_150==(actual?uint32_t(rate->value):100u) && effects.status->word_154==(actual?100u:uint32_t(rate->value)) &&
            effects.status->word_12c==2159999 && effects.status->word_24==99999 && effects.status->word_28==0 &&
            player.state().counter_1368==counter+tick+1,
            "three fresh-status parent updates through phase wrap match original constructor/reset/status instruction outputs");
        const auto accepted=update_owners_snapshot(effects);
        expect(player.initialize_fresh_status(controls,&effects)==P::AlreadyInitialized && update_owners_snapshot(effects)==accepted,
            "post-update initialization cannot erase evolved status");
    }
    size_t copies=0;for(const auto& entry:effects.queue)if(entry.node_identity==node)++copies;
    expect(copies==1 && effects.saved_pose.position==input.pose.position && holder_snapshot(player.model())==model &&
        player.model().game_clock()->state().raw_time==clock.raw_time && player.model().game_clock()->state().loop_count==clock.loop_count,
        "fresh status updates publish one queued node and pose while preserving model/frame/time");
}
uint64_t actor_queue_snapshot(const awl::WorldMapPlayerActorQueue& queue) {
    uint64_t h=14695981039346656037ull;hash(h,uint32_t(queue.size()));
    for(const auto& entry:queue){hash(h,uint32_t(entry.node_identity>>32));hash(h,uint32_t(entry.node_identity));hash(h,entry.priority);}return h;
}
void actor_callback_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using S=awl::WorldMapPlayerStartupActorCallbackStatus;using Q=awl::WorldMapPlayerActorScheduleStatus;
    using F=awl::WorldMapPlayerStartupActorFrameStatus;using P=awl::WorldMapPlayerPrimaryUpdateStatus;
    const auto& metadata=player.actor_metadata();
    expect(metadata.flag_15d==1 && metadata.flag_15e==1 && metadata.flag_15f==1 && !metadata.effect_wrapper,
        "supported fresh startup owns original constructor flags and an unwritten effect wrapper");
    const auto effect=player.effect_node_identity(),render=player.render_node_identity(),wrapper=player.actor_wrapper_identity();
    expect(effect && render && wrapper && effect!=render && effect!=wrapper && render!=wrapper,"startup owns three stable distinct process-local node/wrapper keys");
    const auto parent=player.state();const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    const auto model_before=holder_snapshot(player.model()),timers=holder_snapshot(player.model(),false);
    awl::WorldMapPlayerActorQueue queue{{1,0},{2,0x21},{3,0x2002},{4,UINT32_MAX}};
    const auto queue_before=actor_queue_snapshot(queue),before=startup_snapshot(player);
    awl::WorldMapPlayerStartupActorCallbackInput input;
    auto step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::RequiresControl && !step.actor && startup_snapshot(player)==before && actor_queue_snapshot(queue)==queue_before,
        "callback requires observed control without guessing constructor zero or changing owners");
    input.control_word=0x12345678;input.actor.saved_byte_29a01=uint8_t(0);input.actor.script.emplace();
    input.actor.script->flag_width_4=1;input.actor.script->flag_bytes={1};input.actor.script->records.emplace();
    input.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{std::numeric_limits<float>::quiet_NaN(),20,30},{0,0,1}};
    step=player.advance_state29_actor_callback(input,nullptr);
    expect(step.status==S::ScriptBlocked && player.state().control_word_1384==*input.control_word &&
        holder_snapshot(player.model())==model_before && player.actor_metadata().flag_15e==1 && player.actor_metadata().flag_15f==1 &&
        !player.actor_metadata().effect_wrapper && actor_queue_snapshot(queue)==queue_before,
        "blocked script publishes only control and skips invalid placement, unknown visibility/effect and absent queue");
    const auto pending=startup_snapshot(player);const auto preserved=[&](){return startup_snapshot(player)==pending && actor_queue_snapshot(queue)==queue_before;};
    input.control_word=0x87654321;input.actor.saved_byte_29a01=uint8_t(1);input.actor.script.reset();
    step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::ActorIncomplete && step.actor && step.actor->gate==awl::WorldMapPlayerActorGateStatus::RequiresAuxiliaryCallback && preserved(),
        "reached auxiliary callback preserves prior control, metadata, frame/cache and queue");
    input.actor.saved_byte_29a01=uint8_t(0);input.actor.script.emplace();input.actor.script->flag_width_4=1;
    input.actor.script->flag_bytes={0};input.actor.script->records.emplace();input.actor.derive_normal_visibility=true;
    input.actor.frame.actor_placement->position={40,50,60};
    input.actor.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());
    input.actor.visibility.emplace();input.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;input.actor.visibility->planes[i]=plane;}
    input.actor.screen.emplace();input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents={1000.0f,1000.0f};
    input.actor.derive_normal_visibility=false;input.actor.sphere=awl::WorldMapPlayerActorSphere{{40,51,60},1};
    step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::InvalidInput && preserved(),"scheduled normal callback cannot accept isolated observed-sphere frame mode");
    input.actor.derive_normal_visibility=true;input.actor.sphere.reset();
    step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::ScheduleIncomplete && step.schedule_status==Q::RequiresEffectGate && step.actor && step.actor->frame &&
        step.actor->frame->status==P::Advanced && step.actor->frame->texture_features && preserved(),
        "missing optional-effect gate is reached after successful frame/texture staging but rolls back all published owners");
    awl::WorldMapModelMatrix invalid{1,0,0,0,0,1,0,0,0,0,1,std::numeric_limits<float>::quiet_NaN()};
    input.actor.frame.node_post_transforms.back()=invalid;step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::ActorIncomplete && step.actor && step.actor->status==F::FrameIncomplete && !step.schedule_status && preserved(),
        "late CPU frame failure precedes missing effect gate and leaves prior metadata/control/cache/queue intact");
    input.actor.frame.node_post_transforms.back().reset();input.effect_gate=uint8_t(255);
    queue[1].node_identity=1;const auto bad_queue=actor_queue_snapshot(queue);step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::ScheduleIncomplete && step.schedule_status==Q::InvalidQueue && startup_snapshot(player)==pending && actor_queue_snapshot(queue)==bad_queue,
        "invalid late render queue rolls back prepared frame/cache and actor flags");queue[1].node_identity=2;
    queue[1].node_identity=effect;queue[1].priority=0x20;const auto changed_queue=actor_queue_snapshot(queue);
    step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::ScheduleIncomplete && step.schedule_status==Q::UnsupportedPriorityChange && startup_snapshot(player)==pending &&
        actor_queue_snapshot(queue)==changed_queue,"unsupported existing effect priority preserves complete callback for retry");queue[1]={2,0x21};
    step=player.advance_state29_actor_callback(input,nullptr);
    expect(step.status==S::InvalidInput && step.actor && step.actor->frame && preserved(),"absent reached queue cannot publish a staged frame");
    bool completed=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t frame_failures=0,queue_failures=0;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            step=player.advance_state29_actor_callback(input,&queue);allocation_probe::enabled=false;
            if(step.status==S::Completed){completed=true;break;}
            if(step.status==S::ActorIncomplete){++frame_failures;expect(step.actor && step.actor->frame && step.actor->frame->status==P::AllocationFailure,"frame allocation stop retains its exact cause");}
            else {++queue_failures;expect(step.status==S::ScheduleIncomplete && step.schedule_status==Q::AllocationFailure,"late schedule allocation stop retains its exact cause");}
            expect(preserved() && allocation_probe::live==live,"every frame and queue allocation failure preserves all prior owners and frees staging");}
        expect(completed && frame_failures>5 && queue_failures>0,"callback allocation sweep covers CPU and later queue stages through complete publication");
        std::cout<<"PLAYER_ACTOR_CALLBACK_ALLOCATION_FAILURES "<<frame_failures<<" frame / "<<queue_failures<<" queue\n";}
#else
    (void)allocations;
#endif
    if(!completed)step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::Completed && step.effect_scheduled==true && player.actor_metadata().flag_15d==1 &&
        player.actor_metadata().flag_15e==0 && player.actor_metadata().flag_15f==1 && player.actor_metadata().effect_wrapper==wrapper &&
        queue.size()==6 && queue[1].node_identity==effect && queue[1].priority==0x21 && queue[2].node_identity==2 &&
        queue[3].node_identity==render && queue[3].priority==0x2002 && queue[4].node_identity==3 &&
        player.state().control_word_1384==*input.control_word && player.model().primary().cpu().frame()->root_matrix[3]==40,
        "complete callback publishes frame/cache, metadata/wrapper and effect-before-player queue ordering with new control");
    const auto accepted_queue=actor_queue_snapshot(queue);
    input.actor.frame.actor_placement->position[0]=41;step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::Completed && step.effect_scheduled==true && actor_queue_snapshot(queue)==accepted_queue &&
        player.model().primary().cpu().frame()->root_matrix[3]==41,"repeat callback advances frame with stable owned keys and no duplicate queue nodes");
    const auto accepted_model=holder_snapshot(player.model());input.actor.visibility->planes[0]->point[0]=0;
    input.actor.screen.reset();input.effect_gate.reset();input.actor.frame.actor_placement->heading[0]=std::numeric_limits<float>::quiet_NaN();
    step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::Completed && step.effect_scheduled==false && step.actor && step.actor->status==F::VisibilityStopped && !step.actor->frame &&
        player.actor_metadata().flag_15f==1 && player.actor_metadata().flag_15e==0 && player.actor_metadata().effect_wrapper==wrapper &&
        actor_queue_snapshot(queue)==accepted_queue && holder_snapshot(player.model())==accepted_model,
        "first visibility cull preserves old screen/effect/frame, skips unknown screen/gate/heading, and still schedules player");
    input.actor.visibility->planes[0]->point[0]=1000;input.actor.frame.actor_placement->heading[0]=0;
    input.actor.screen.emplace();input.actor.screen->view=awl::WorldMapModelMatrix{1,0,0,2000,0,1,0,0,0,0,1,0};input.actor.screen->half_extents[0]=1000.0f;
    input.actor.frame.actor_placement->position[0]=42;step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::Completed && step.effect_scheduled==false && player.actor_metadata().flag_15f==0 &&
        player.actor_metadata().effect_wrapper==wrapper && actor_queue_snapshot(queue)==accepted_queue &&
        player.model().primary().cpu().frame()->root_matrix[3]==42,"screen outside publishes new frame and effect-off flag with unread global gate/Y extent and retained queued effect");
    input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents[1]=1000.0f;input.effect_gate=uint8_t(0);
    step=player.advance_state29_actor_callback(input,&queue);
    expect(step.status==S::Completed && step.effect_scheduled==false && player.actor_metadata().flag_15f==1 && actor_queue_snapshot(queue)==accepted_queue,
        "global effect gate zero still publishes screen-inside flag and player schedule");
    expect(player.effect_node_identity()==effect && player.render_node_identity()==render && player.actor_wrapper_identity()==wrapper &&
        player.state().counter_1368==parent.counter_1368 && player.state().state_1364==parent.state_1364 &&
        (player.camera()?camera_snapshot(*player.camera()):0)==camera && holder_snapshot(player.model(),false)==timers,
        "CPU actor callbacks preserve node lifetime, parent counter/camera, holders/timers and shared clock");
}
void queued_actor_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using S=awl::WorldMapPlayerStartupQueueStatus;using C=awl::WorldMapPlayerStartupActorCallbackStatus;
    using P=awl::WorldMapPlayerPrimaryUpdateStatus;using Q=awl::WorldMapPlayerActorScheduleStatus;
    const auto key=player.update_node_identity();
    expect(key && key!=player.effect_node_identity() && key!=player.render_node_identity() && key!=player.actor_wrapper_identity(),
        "update node owns a distinct stable key for registration through completion");
    awl::WorldMapUpdateInputs controls(player.model().game_clock()),missing({});
    awl::WorldMapPlayerStartupQueuedActorInput input;
    awl::WorldMapPlayerUpdateOwners owners;owners.saved_pose={7,{8,9,10},{11,12,13}};
    owners.position_802fc20c={14,15,16};owners.scene_one_position_240=std::array<float,3>{17,18,19};
    owners.status.emplace();owners.status->word_4=12345;
    awl::WorldMapPlayerActorQueue render{{1,0},{2,0x21},{3,0x2002},{4,UINT32_MAX}};
    const auto initial=startup_snapshot(player),initial_owners=update_owners_snapshot(owners);
    auto step=player.consume_state29_update(missing,input,&owners,nullptr);
    expect(step.status==S::Empty && !step.callback && !step.completed_node && startup_snapshot(player)==initial &&
        update_owners_snapshot(owners)==initial_owners,"empty update queue skips clock/control, actor and render dependencies");
    expect(player.consume_state29_update(controls,input,nullptr,&render).status==S::InvalidInput && startup_snapshot(player)==initial,
        "absent update owner rejects without publishing anything");
    const auto rejected=[&](S status,const awl::WorldMapUpdateInputs& source){
        const auto before=startup_snapshot(player),effects=update_owners_snapshot(owners),draw=actor_queue_snapshot(render);
        auto result=player.consume_state29_update(source,input,&owners,&render);
        expect(result.status==status && !result.completed_node && startup_snapshot(player)==before &&
            update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw,
            "rejected queue work preserves player, update owner and render queue for retry");return result;};
    for(const auto& entries:std::vector<std::vector<awl::WorldMapPlayerUpdateQueueEntry>>{
        {{0,0x1010}},{{key,0x1010},{1,0}},{{key,0x1010},{key,0x1010}}}){
        owners.queue=entries;rejected(S::InvalidQueue,controls);}
    owners.queue={{1,0x100f},{key,0x1010}};step=rejected(S::UnsupportedNode,controls);
    expect(!step.callback,"unknown earlier work is not skipped to reach the supported player");
    owners.queue={{key,0x1011}};rejected(S::UnsupportedPriority,controls);
    owners.queue={{key,0x1010}};rejected(S::RequiresControl,controls);
    awl::WorldMapUpdateInputs foreign(std::make_shared<awl::GameClock>());rejected(S::InvalidInput,foreign);
    controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured && controls.queue_control().control_word==0x800u,
        "queue uses a fresh captured control from the retained shared clock");
    // Invented keys/fields; expected digest independently obtained by executing
    // actual first-node D018/D260/31584 and CFD4/D2C8/C8/989C prefixes.
    input.actor.saved_byte_29a01=uint8_t{0};input.actor.script.emplace();
    input.actor.script->flag_width_4=1;input.actor.script->flag_bytes={1};input.actor.script->records.emplace();
    input.actor.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());
    input.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{std::numeric_limits<float>::quiet_NaN(),0,0},{0,0,1}};
    constexpr uint32_t priorities[]{0x1010,0x1010,0x1011,0x80000000,UINT32_MAX};
    const auto model=holder_snapshot(player.model()),draw=actor_queue_snapshot(render);
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<(allocations?512u:1u);++seed){owners.queue={{key,0x1010}};
        for(uint32_t i=0;i<seed%6;++i)owners.queue.push_back({i+1,priorities[i]});
        auto expected=owners;expected.queue.erase(expected.queue.begin());
        step=player.consume_state29_update(controls,input,&owners,nullptr);
        expect(step.status==S::Completed && step.callback && step.callback->status==C::ScriptBlocked && step.completed_node==key &&
            player.state().control_word_1384==0x800u && update_owners_snapshot(owners)==update_owners_snapshot(expected) &&
            holder_snapshot(player.model())==model && actor_queue_snapshot(render)==draw,
            "script-blocked front node completes without frame/render inputs and preserves all other owner fields and later entries");
        for(uint32_t w:{seed,*player.state().control_word_1384,step.completed_node==key?99u:0u,uint32_t(owners.queue.size())})hash(digest,w);
        for(const auto& entry:owners.queue){hash(digest,uint32_t(entry.node_identity));hash(digest,entry.priority);}
        rejected(owners.queue.empty()?S::Empty:S::UnsupportedNode,controls);
    }
    if(allocations){expect(digest==0x9ED5FD5A76E91798ull,"512 first-node script-block/completion prefixes agree with actual mapped instructions");
        std::cout<<"PLAYER_UPDATE_COMPLETION_PREFIX 512 digest "<<std::hex<<digest<<std::dec<<'\n';}
#if !defined(_MSC_VER) || !defined(_DEBUG)
    owners.queue={{key,0x1010}};allocation_probe::remaining=0;allocation_probe::enabled=true;
    step=player.consume_state29_update(controls,input,&owners,nullptr);allocation_probe::enabled=false;
    expect(step.status==S::Completed && owners.queue.empty(),"blocked script and completion allocate nothing even with retained frame observations");
#endif
    owners.queue={{1,0x1010},{2,0x1011},{3,UINT32_MAX}};
    awl::WorldMapPlayerStartupDispatchInput parent;parent.guards=awl::WorldMapPlayerUpdateGuards{-1,0};
    parent.scene_child_word_4=0;parent.pose={0,{40,50,60},{0,0,1}};parent.action_148=0;parent.player_node_identity=key;
    const auto requeue=[&](){expect(player.dispatch_state29_parent(controls,parent,&owners).status==awl::WorldMapPlayerUpdateStatus::Advanced &&
        owners.queue.front().node_identity==key && owners.queue.front().priority==0x1010,
        "existing owned parent callback registers the same update node before equal priorities");};
    requeue();
    input.actor.saved_byte_29a01=uint8_t{1};step=rejected(S::CallbackIncomplete,controls);
    expect(step.callback && step.callback->status==C::ActorIncomplete,"untranslated auxiliary callback retains pending update work");
    input.actor.saved_byte_29a01=uint8_t{0};input.actor.script->flag_bytes[0]=0;input.actor.derive_normal_visibility=true;
    input.actor.frame.actor_placement->position=parent.pose.position;
    step=rejected(S::CallbackIncomplete,controls);
    expect(step.callback && !step.callback->schedule_status,"missing visibility stops before render scheduling");
    input.actor.visibility.emplace();input.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;
        plane.normal[i/2]=i%2?-1.0f:1.0f;input.actor.visibility->planes[i]=plane;}
    input.actor.screen.emplace();input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents={1000.0f,1000.0f};
    step=rejected(S::CallbackIncomplete,controls);
    expect(step.callback && step.callback->schedule_status==Q::RequiresEffectGate,"late missing effect gate retains the update node and staged CPU frame");
    input.effect_gate=uint8_t{255};render[1].node_identity=1;step=rejected(S::CallbackIncomplete,controls);
    expect(step.callback && step.callback->schedule_status==Q::InvalidQueue,"invalid render queue cannot consume the update node");render[1].node_identity=2;
    {const auto before=startup_snapshot(player),effects=update_owners_snapshot(owners),draw_before=actor_queue_snapshot(render);
        step=player.consume_state29_update(controls,input,&owners,nullptr);
        expect(step.status==S::CallbackIncomplete && step.callback && step.callback->status==C::InvalidInput &&
            startup_snapshot(player)==before && update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw_before,
            "absent reached render owner retains the prepared frame and update node for retry");}
    input.actor.frame.node_post_transforms.back()=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,std::numeric_limits<float>::quiet_NaN()};
    step=rejected(S::CallbackIncomplete,controls);
    expect(step.callback && step.callback->actor && step.callback->actor->frame && step.callback->actor->frame->status!=P::Advanced,
        "late CPU frame failure cannot consume the update node");input.actor.frame.node_post_transforms.back().reset();
    const auto pending=startup_snapshot(player),pending_owners=update_owners_snapshot(owners),pending_render=actor_queue_snapshot(render);
    const auto preserved=[&](){return startup_snapshot(player)==pending && update_owners_snapshot(owners)==pending_owners && actor_queue_snapshot(render)==pending_render;};
    auto expected=owners;expected.queue.erase(expected.queue.begin());bool completed=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){size_t frame_failures=0,queue_failures=0;const auto live=allocation_probe::live;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            step=player.consume_state29_update(controls,input,&owners,&render);allocation_probe::enabled=false;
            if(step.status==S::Completed){completed=true;break;}
            expect(step.status==S::CallbackIncomplete && step.callback && preserved() && allocation_probe::live==live,
                "every allocation stop keeps the update pending, preserves both queues/player and releases staging");
            if(step.callback && step.callback->status==C::ActorIncomplete){++frame_failures;expect(step.callback->actor && step.callback->actor->frame &&
                step.callback->actor->frame->status==P::AllocationFailure,"queued frame stop retains allocation cause");}
            else {++queue_failures;expect(step.callback && step.callback->schedule_status==Q::AllocationFailure,"queued schedule stop retains allocation cause");}}
        expect(completed && frame_failures>5 && queue_failures>0,"queued allocation sweep reaches CPU and later scheduling failures through successful completion");
        std::cout<<"PLAYER_QUEUED_CALLBACK_ALLOCATION_FAILURES "<<frame_failures<<" frame / "<<queue_failures<<" queue\n";}
#else
    (void)allocations;(void)preserved;
#endif
    if(!completed)step=player.consume_state29_update(controls,input,&owners,&render);
    expect(step.status==S::Completed && step.completed_node==key && step.callback && step.callback->status==C::Completed &&
        step.callback->effect_scheduled==true && update_owners_snapshot(owners)==update_owners_snapshot(expected) && render.size()==6 &&
        render[1].node_identity==player.effect_node_identity() && render[3].node_identity==player.render_node_identity() &&
        player.model().primary().cpu().frame()->root_matrix[3]==40,"handled visible callback publishes frame/render work and removes only its owned update node");
    rejected(S::UnsupportedNode,controls);
    const auto accepted_render=actor_queue_snapshot(render);input.actor.frame.actor_placement->position[0]=41;requeue();
    step=player.consume_state29_update(controls,input,&owners,&render);
    expect(step.status==S::Completed && actor_queue_snapshot(render)==accepted_render && player.model().primary().cpu().frame()->root_matrix[3]==41,
        "parent can requeue after completion and next callback advances without duplicate render entries");
    requeue();const auto accepted_model=holder_snapshot(player.model());input.actor.visibility->planes[0]->point[0]=0;
    input.actor.screen.reset();input.effect_gate.reset();input.actor.frame.actor_placement->heading[0]=std::numeric_limits<float>::quiet_NaN();
    step=player.consume_state29_update(controls,input,&owners,&render);
    expect(step.status==S::Completed && step.callback && step.callback->status==C::Completed && step.callback->effect_scheduled==false &&
        holder_snapshot(player.model())==accepted_model && actor_queue_snapshot(render)==accepted_render,
        "first visibility cull still schedules player and completes its update while skipping unread frame/effect inputs");
    requeue();input.actor.visibility->planes[0]->point[0]=1000;input.actor.frame.actor_placement->heading[0]=0;
    input.actor.frame.actor_placement->position[0]=42;input.actor.screen.emplace();
    input.actor.screen->view=awl::WorldMapModelMatrix{1,0,0,2000,0,1,0,0,0,0,1,0};input.actor.screen->half_extents[0]=1000.0f;
    step=player.consume_state29_update(controls,input,&owners,&render);
    expect(step.status==S::Completed && step.callback && step.callback->effect_scheduled==false &&
        player.actor_metadata().flag_15f==0 && player.model().primary().cpu().frame()->root_matrix[3]==42 && actor_queue_snapshot(render)==accepted_render,
        "screen-outside callback advances its CPU frame and completes without the unread effect gate");
    owners.queue.clear();rejected(S::Empty,missing);
    expect(player.update_node_identity()==key,"owned update key survives callback completion and reinsertion");
}
void texture_timer_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using S=awl::WorldMapTextureTimerStatus;using Q=awl::WorldMapPlayerStartupQueueStatus;
    const auto& holder=player.model();const auto clock=holder.game_clock();if(!clock || !holder.state().feature_38)return;
    const auto original_interval=clock->state().retrace_interval;const auto parent=player.state();const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    awl::WorldMapUpdateInputs controls(clock);controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured,"timer integration starts with current captured control");
    awl::WorldMapPlayerStartupQueuedActorInput input;input.actor.saved_byte_29a01=uint8_t{0};input.actor.script.emplace();
    input.actor.script->flag_width_4=1;input.actor.script->flag_bytes={0};input.actor.script->records.emplace();input.actor.derive_normal_visibility=true;
    input.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{40,50,60},{0,0,1}};
    input.actor.frame.node_post_transforms.resize(holder.primary().model().core().nodes.size());
    input.actor.visibility.emplace();input.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;input.actor.visibility->planes[i]=plane;}
    input.actor.screen.emplace();input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents={1000.0f,1000.0f};input.effect_gate=uint8_t{0};
    awl::WorldMapPlayerUpdateOwners updates;awl::WorldMapPlayerActorQueue render;
    for(uint32_t interval:{30u,60u,300u}){updates.queue={{player.update_node_identity(),0x1010}};
        clock->set_retrace_interval(interval);expect(clock->advance(),"diagnostic fixture explicitly advances its retained game clock before the next CPU step");
        const auto before=holder_snapshot(holder,true,false);awl::WorldMapAnimationFeature38 expected;
        const auto proposed=awl::prepare_world_map_player_texture_timers(*holder.state().feature_38,holder.state().feature_flag_34,
            holder.timer_assets().get(),clock->state().raw_time,&expected);
        expect(proposed.status==S::Prepared && (proposed.updated_mask&1)!=0,"retained startup feature reaches the justified common eye timer");
        awl::WorldMapTextureTimerResult result;
#if !defined(_MSC_VER) || !defined(_DEBUG)
        if(allocations){allocation_probe::remaining=0;allocation_probe::enabled=true;}
#else
        (void)allocations;
#endif
        result=player.advance_texture_timers();allocation_probe::enabled=false;
        expect(result.status==S::Prepared && result.updated_mask==proposed.updated_mask && same_feature(*holder.state().feature_38,expected) &&
            holder_snapshot(holder,true,false)==before,"owned timers publish the pair without allocation or changes to CPU frame/cache/playback/providers or shared clock");
        const auto accepted=startup_snapshot(player),pending=update_owners_snapshot(updates),draw=actor_queue_snapshot(render);
        auto step=player.consume_state29_update(controls,input,&updates,&render);
        expect(step.status==Q::RequiresControl && startup_snapshot(player)==accepted && update_owners_snapshot(updates)==pending && actor_queue_snapshot(render)==draw,
            "stale queue control cannot consume work or undo the separately accepted timer stage");
        expect(controls.capture_control()==awl::WorldMapControlStatus::Captured,"next loop recaptures control after the explicit clock advance");
        const auto selection=awl::prepare_world_map_player_texture_features({holder.state().feature_38,holder.state().feature_source_10,holder.timer_assets().get()},
            *holder.primary().setup().selection().assets);
        expect(selection.status==awl::WorldMapPlayerTextureFeatureStatus::Selected,"advanced retained timer values select supported textures before frame publication");
        step=player.consume_state29_update(controls,input,&updates,&render);
        expect(step.status==Q::Completed && step.callback && step.callback->actor && step.callback->actor->frame &&
            step.callback->actor->frame->texture_features && updates.queue.empty() && render.size()==1 && render[0].node_identity==player.render_node_identity() &&
            same_feature(*holder.state().feature_38,expected),"recaptured queued CPU frame consumes the new timer snapshot without advancing it a second time");
        if(step.callback && step.callback->actor && step.callback->actor->frame && step.callback->actor->frame->texture_features){const auto& sampled=*step.callback->actor->frame->texture_features;
            for(size_t c=0;c<3;++c)expect(sampled.textures[c].has_value()==selection.textures[c].has_value() && (!sampled.textures[c] ||
                (sampled.textures[c]->bank==selection.textures[c]->bank && sampled.textures[c]->index==selection.textures[c]->index)),
                "queued frame uses the newly selected eye/mouth/body texture identities");}}
    const int rounding=std::fegetround();expect(std::fesetround(FE_DOWNWARD)==0,"owner retry fixture selects unsupported rounding");
    const auto accepted=startup_snapshot(player);const auto rejected=player.advance_texture_timers();
    expect(rejected.status==S::UnsupportedNumerics && rejected.updated_mask==0 && startup_snapshot(player)==accepted && std::fegetround()==FE_DOWNWARD,
        "unsupported owned timer arithmetic preserves all accepted state and caller mode");expect(std::fesetround(rounding)==0,"owner retry fixture restores rounding");
    clock->set_retrace_interval(original_interval);
    expect(player.state().counter_1368==parent.counter_1368 && player.state().state_1364==parent.state_1364 &&
        (player.camera()?camera_snapshot(*player.camera()):0)==camera,"timer/frame composition does not execute the parent camera/status or animation-holder update");
}
uint64_t progression_playback_snapshot(const awl::WorldMapAnimationPartialPlayback& p) {
    uint64_t h=14695981039346656037ull;
    const auto key=[&](uint64_t v){hash(h,uint32_t(v>>32));hash(h,uint32_t(v));};
    hash(h,actor_bits(p.position_0));hash(h,actor_bits(p.rate_4));hash(h,p.word_8.has_value());if(p.word_8)hash(h,*p.word_8);
    hash(h,p.limit_c.has_value());if(p.limit_c)hash(h,actor_bits(*p.limit_c));hash(h,p.value_18.has_value());if(p.value_18)hash(h,actor_bits(*p.value_18));
    key(p.link_14);hash(h,p.clip_10.has_value());if(p.clip_10){key(p.clip_10->bank_identity);hash(h,p.clip_10->offset);}return h;
}
void channel_progress_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using S=awl::WorldMapAnimationChannelStatus;using Q=awl::WorldMapPlayerStartupQueueStatus;
    const auto& holder=player.model();const auto& channel=holder.primary_channel();const auto& model=holder.primary().model();
    const auto binding=model.binding();const auto parent=player.state();const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    const auto before=startup_snapshot(player);
    expect(player.advance_primary_channel(nullptr)==S::InvalidInput && startup_snapshot(player)==before,"owned channel rejects absent output without mutation");
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"select unsupported owned channel rounding");
    awl::WorldMapAnimationChannelProgressStep rejected;
    expect(player.advance_primary_channel(&rejected)==S::UnsupportedNumerics && startup_snapshot(player)==before && !rejected.completed &&
        std::fegetround()==FE_UPWARD,"rejected owned channel arithmetic preserves all startup/frame state and caller rounding");
    expect(std::fesetround(rounding)==0,"restore owned channel rounding");
    awl::WorldMapUpdateInputs controls(holder.game_clock());controls.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured,"channel integration captures current owned queue control");
    awl::WorldMapPlayerStartupQueuedActorInput input;input.actor.saved_byte_29a01=uint8_t{0};input.actor.script.emplace();
    input.actor.script->flag_width_4=1;input.actor.script->flag_bytes={0};input.actor.script->records.emplace();input.actor.derive_normal_visibility=true;
    input.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{40,50,60},{0,0,1}};
    input.actor.frame.node_post_transforms.resize(model.core().nodes.size());
    input.actor.visibility.emplace();input.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.f:1000.f;plane.normal[i/2]=i%2?-1.f:1.f;input.actor.visibility->planes[i]=plane;}
    input.actor.screen.emplace();input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents={1000.f,1000.f};input.effect_gate=uint8_t{0};
    awl::WorldMapPlayerUpdateOwners updates;awl::WorldMapPlayerActorQueue render;
    for(uint32_t tick=0;tick<16;++tick) {
        auto pool=channel.records();pool.push_back({binding.playback_178,model.partial_playback()});awl::WorldMapAnimationChannelProgressStep expected;
        expect(awl::prepare_world_map_partial_animation_channel_progress(channel.state(),pool,binding.model_identity,binding,&expected)==S::Prepared,
            "retained startup channel has all inputs for the independently compared 69C4 body");
        const auto unrelated=holder_snapshot(holder,true,true,false);awl::WorldMapAnimationChannelProgressStep actual;S status=S::InvalidInput;
#if !defined(_MSC_VER) || !defined(_DEBUG)
        if(allocations && tick==0) {
            const auto accepted=startup_snapshot(player),live=allocation_probe::live;bool reached=false;size_t failed=0;
            actual.update.after.elapsed_0=77;actual.completed=true;
            for(size_t fail=0;fail<32;++fail) {
                allocation_probe::remaining=fail;allocation_probe::enabled=true;status=player.advance_primary_channel(&actual);allocation_probe::enabled=false;
                if(status==S::Advanced){reached=true;break;}++failed;
                expect(status==S::AllocationFailure && startup_snapshot(player)==accepted && allocation_probe::live==live &&
                    actual.update.after.elapsed_0==77 && actual.completed && actual.update.records_after.empty(),
                    "each owned channel allocation stop preserves model, records, counter, diagnostics and frees staging");
            }
            expect(reached && failed>0,"owned channel allocation sweep reaches publication");
            std::cout<<"PLAYER_CHANNEL_PROGRESS_ALLOCATION_FAILURES "<<failed<<'\n';
        } else
#else
        (void)allocations;
#endif
        status=player.advance_primary_channel(&actual);
        expect(status==S::Advanced && actual.completed==expected.completed && holder_snapshot(holder,true,true,false)==unrelated &&
            channel.state().elapsed_0==expected.update.after.elapsed_0 && channel.state().duration_4==expected.update.after.duration_4 &&
            channel.state().mode_18==expected.update.after.mode_18 && channel.state().target_8==expected.update.after.target_8 &&
            channel.state().previous_c==expected.update.after.previous_c && channel.state().older_10==expected.update.after.older_10 &&
            channel.state().blend_14==expected.update.after.blend_14,"owned 69C4 publishes channel and playback without changing frame, holder, timers, providers or clock");
        for(size_t i=0;i<channel.records().size();++i)expect(channel.records()[i].identity==expected.update.records_after[i].identity &&
            progression_playback_snapshot(channel.records()[i].state)==progression_playback_snapshot(expected.update.records_after[i].state),
            "owned channel retains exact ordered external record results");
        const auto playback=progression_playback_snapshot(model.partial_playback());
        expect(expected.update.records_after.back().identity==binding.playback_178 && playback==progression_playback_snapshot(expected.update.records_after.back().state),
            "model receives authoritative current blend playback under its stable key");
        updates.queue={{player.update_node_identity(),0x1010}};input.actor.script->flag_bytes[0]=1;
        auto consumed=player.consume_state29_update(controls,input,&updates,&render);
        expect(consumed.status==Q::Completed && consumed.callback && consumed.callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::ScriptBlocked &&
            updates.queue.empty() && progression_playback_snapshot(model.partial_playback())==playback,
            "script-blocked queued frame does not undo or repeat the separately accepted channel update");
        updates.queue={{player.update_node_identity(),0x1010}};input.actor.script->flag_bytes[0]=0;
        consumed=player.consume_state29_update(controls,input,&updates,&render);
        expect(consumed.status==Q::Completed && consumed.callback && consumed.callback->actor && consumed.callback->actor->frame &&
            updates.queue.empty() && render.size()==1 && render[0].node_identity==player.render_node_identity() && progression_playback_snapshot(model.partial_playback())==playback &&
            channel.state().elapsed_0==expected.update.after.elapsed_0,"queued CPU frame samples the advanced model/channel exactly once, preserves playback and completes work");
    }
    using H=awl::WorldMapPlayerHolderProgressStatus;
    const auto clock=holder.game_clock();if(!clock || !holder.state().feature_38)return;
    const auto interval=clock->state().retrace_interval;
    clock->set_retrace_interval(60);expect(clock->advance(),"holder integration explicitly advances time before its atomic animation update");
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured,"holder integration recaptures the explicitly advanced clock");
    for(uint32_t tick=0;tick<8;++tick) {
        auto pool=channel.records();pool.push_back({binding.playback_178,model.partial_playback()});awl::WorldMapAnimationChannelProgressStep progress;
        expect(awl::prepare_world_map_partial_animation_channel_progress(channel.state(),pool,binding.model_identity,binding,&progress)==S::Prepared,
            "normal composed update prepares verified channel progression");
        awl::WorldMapPlayerHolderPolicyStep policy;
        expect(awl::prepare_world_map_player_holder_policy(holder.state(),holder.descriptor(),progress.completed,clock->state().raw_time,{},&policy)==H::Prepared,
            "normal descriptor reaches fully supported completion/restart accounting");
        awl::WorldMapAnimationFeature38 timers;
        const auto timer=awl::prepare_world_map_player_texture_timers(*policy.after.feature_38,policy.after.feature_flag_34,holder.timer_assets().get(),clock->state().raw_time,&timers);
        expect(timer.status==awl::WorldMapTextureTimerStatus::Prepared,"normal timer proposal has all retained clock/resource inputs");
        const auto cpu=primary_snapshot(holder.primary());const auto raw=clock->state().raw_time;awl::WorldMapPlayerHolderProgressResult actual;
#if !defined(_MSC_VER) || !defined(_DEBUG)
        if(allocations && tick==0) {
            const auto accepted=startup_snapshot(player),live=allocation_probe::live;size_t failed=0;bool reached=false;
            for(size_t fail=0;fail<32;++fail) {
                allocation_probe::remaining=fail;allocation_probe::enabled=true;actual=player.advance_animation();allocation_probe::enabled=false;
                if(actual.status==H::Advanced){reached=true;break;}++failed;
                expect(actual.status==H::AllocationFailure && startup_snapshot(player)==accepted && allocation_probe::live==live,
                    "each atomic holder allocation stop preserves playback, timers, metadata, CPU frame and frees staging");
            }
            expect(reached && failed>0,"atomic holder allocation sweep reaches publication");
            std::cout<<"PLAYER_HOLDER_PROGRESS_ALLOCATION_FAILURES "<<failed<<'\n';
        } else
#endif
        actual=player.advance_animation();
        expect(actual.status==H::Advanced && actual.channel_status==S::Prepared && actual.primary_completed==progress.completed && actual.timers &&
            actual.timers->updated_mask==timer.updated_mask && same_feature(*holder.state().feature_38,timers) &&
            holder.state().completed_20==policy.after.completed_20 && holder.state().restart_count_2c==policy.after.restart_count_2c &&
            holder.state().current_descriptor_0==policy.after.current_descriptor_0 && holder.state().base_descriptor_4==policy.after.base_descriptor_4 &&
            channel.state().elapsed_0==progress.update.after.elapsed_0 && channel.state().mode_18==progress.update.after.mode_18 &&
            channel.state().blend_14==progress.update.after.blend_14 && primary_snapshot(holder.primary())==cpu && clock->state().raw_time==raw,
            "BA94 normal progression publishes channel, completion/restarts and timer pair together without evaluating geometry or advancing time");
        for(size_t i=0;i<channel.records().size();++i)expect(channel.records()[i].identity==progress.update.records_after[i].identity &&
            progression_playback_snapshot(channel.records()[i].state)==progression_playback_snapshot(progress.update.records_after[i].state),
            "atomic holder retains exact ordered channel records");
        expect(progression_playback_snapshot(model.partial_playback())==progression_playback_snapshot(progress.update.records_after.back().state),
            "atomic holder publishes exact authoritative model blend playback");
        const auto accepted=holder_snapshot(holder,false);updates.queue={{player.update_node_identity(),0x1010}};
        const auto consumed=player.consume_state29_update(controls,input,&updates,&render);
        expect(consumed.status==Q::Completed && consumed.callback && consumed.callback->actor && consumed.callback->actor->frame &&
            updates.queue.empty() && render.size()==1 && holder_snapshot(holder,false)==accepted,
            "queued CPU frame samples the atomic animation/timer update without repeating it");
    }
    const auto accepted=startup_snapshot(player);expect(std::fesetround(FE_UPWARD)==0,"select unsupported atomic holder numerics");
    const auto stopped=player.advance_animation();
    expect(stopped.status==H::ChannelIncomplete && stopped.channel_status==S::UnsupportedNumerics && startup_snapshot(player)==accepted &&
        std::fegetround()==FE_UPWARD,"rejected primary progression prevents holder/timer publication and preserves caller rounding");
    expect(std::fesetround(rounding)==0,"restore atomic holder numerical profile");clock->set_retrace_interval(interval);
    if(allocations) {
        using K=awl::WorldMapPlayerHolderRepeatKind;
        const auto raw=clock->state().raw_time,count=holder.state().restart_count_2c;
        expect(player.configure_animation_repeats(K::Duration,1000) && holder.state().restart_deadline_24==raw+1000 &&
            holder.state().restart_limit_28==0 && holder.state().restart_count_2c==count,"owned duration setter uses retained raw time without resetting accumulated count");
        const auto pending=startup_snapshot(player),cpu=primary_snapshot(holder.primary());awl::WorldMapPlayerHolderProgressResult repeated;
#if !defined(_MSC_VER) || !defined(_DEBUG)
        const auto live=allocation_probe::live;size_t failed=0;bool reached=false;
        for(size_t fail=0;fail<128;++fail) {
            allocation_probe::remaining=fail;allocation_probe::enabled=true;repeated=player.advance_animation();allocation_probe::enabled=false;
            if(repeated.status==H::Advanced){reached=true;break;}++failed;
            expect(repeated.status==H::AllocationFailure && startup_snapshot(player)==pending && allocation_probe::live==live,
                "every timed restart allocation failure preserves completion, current/base, playback, timers, graph and clock");
        }
        expect(reached && failed>3,"timed same-base restart allocation sweep reaches publication");
        std::cout<<"PLAYER_RESTART_ALLOCATION_FAILURES "<<failed<<'\n';
#else
        (void)pending;repeated=player.advance_animation();
#endif
        expect(repeated.status==H::Advanced && repeated.primary_completed==true && repeated.initialized_records==1 &&
            holder.state().current_descriptor_0==holder.state().base_descriptor_4 && holder.state().completed_20==1u &&
            holder.state().restart_count_2c==count+1 && holder.primary_channel().state().elapsed_0==0 && model.partial_playback().position_0==0 &&
            primary_snapshot(holder.primary())==cpu && clock->state().raw_time==raw,
            "unexpired duration restarts equal-base mode-one playback through DB28 while preserving completion/count and accepted geometry");
        const auto reset=holder_snapshot(holder,false);updates.queue={{player.update_node_identity(),0x1010}};
        const auto consumed=player.consume_state29_update(controls,input,&updates,&render);
        expect(consumed.status==Q::Completed && consumed.callback && consumed.callback->actor && consumed.callback->actor->frame &&
            updates.queue.empty() && holder_snapshot(holder,false)==reset,"queued CPU frame consumes restarted playback without repeating its initializer/timers");
        expect(player.configure_animation_repeats(K::Duration,0),"zero duration expires at the current raw time");
        repeated=player.advance_animation();expect(repeated.status==H::Advanced && repeated.primary_completed==true && repeated.initialized_records==0 &&
            holder.state().restart_count_2c==count+2,"now equal to restart deadline does not restart even when playback completes");
        expect(player.configure_animation_repeats(K::Count,1) && holder.state().restart_deadline_24==raw,
            "count setter preserves an existing deadline in the owned player");
        expect(player.configure_animation_repeats(K::Duration,0u-raw) && holder.state().restart_deadline_24==0 && holder.state().restart_limit_28==0,
            "explicit wrapped deadline request returns the synthetic holder to disabled repeat policy without clearing accumulated count");
    }
    expect(player.state().counter_1368==parent.counter_1368 && player.state().state_1364==parent.state_1364 &&
        (player.camera()?camera_snapshot(*player.camera()):0)==camera,"channel/holder/frame composition preserves parent camera and status state");
}
void normal_actor_frame_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using S=awl::WorldMapPlayerStartupActorFrameStatus;using V=awl::WorldMapPlayerActorVisibilityStatus;
    using B=awl::WorldMapPlayerActorBoundsStatus;using E=awl::WorldMapPlayerActorScreenStatus;
    const auto before=startup_snapshot(player),timers=holder_snapshot(player.model(),false);
    const auto parent=player.state();const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    const auto vertices=player.model().primary().cpu().frame()->vertex_output;
    const auto preserved=[&](){return startup_snapshot(player)==before;};
    awl::WorldMapPlayerStartupActorFrameInput input;input.derive_normal_visibility=true;input.saved_byte_29a01=uint8_t(0);
    input.script.emplace();input.script->flag_width_4=1;input.script->flag_bytes={0};input.script->records.emplace();
    auto step=player.advance_gated_actor_frame(input);
    expect(step.status==S::RequiresActorPlacement && preserved(),"derived normal bounds require observed position before camera inputs");
    input.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{10,20,30},{0,0,1}};
    input.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());input.sphere=awl::WorldMapPlayerActorSphere{};
    step=player.advance_gated_actor_frame(input);expect(step.status==S::InvalidInput && preserved(),"observed sphere and normal bounds are conflicting sources");input.sphere.reset();
    input.frame.actor_placement->position[0]=std::numeric_limits<float>::quiet_NaN();input.script->flag_bytes[0]=1;
    step=player.advance_gated_actor_frame(input);expect(step.status==S::GateStopped && !step.bounds_status && preserved(),"blocked script skips even invalid unread bounds input");
    input.script->flag_bytes[0]=0;step=player.advance_gated_actor_frame(input);
    expect(step.status==S::BoundsIncomplete && step.bounds_status==B::InvalidInput && !step.bounds && preserved(),"invalid derived bounds do not publish a partial player");
    input.frame.actor_placement->position[0]=10;input.visibility.emplace();input.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;input.visibility->planes[i]=plane;}
    step=player.advance_gated_actor_frame(input);
    expect(step.status==S::RequiresScreenOwner && step.bounds && step.bounds->sphere.center==std::array<float,3>{10,21,30} &&
        actor_bits(step.bounds->sphere.radius)==0x3fddb3d7 && preserved(),"normal bounds derive from actor position and reach the separate screen owner");
    input.visibility->planes[0]->point[0]=0;input.frame.actor_placement->heading[0]=std::numeric_limits<float>::quiet_NaN();
    step=player.advance_gated_actor_frame(input);
    expect(step.status==S::VisibilityStopped && step.visibility && step.visibility->status==V::Culled && !step.screen && !step.frame && preserved(),
        "first visibility cull skips separate screen dependencies and unread invalid heading");
    input.visibility->planes[0]->point[0]=1000;input.frame.actor_placement->heading[0]=0;input.screen.emplace();
    step=player.advance_gated_actor_frame(input);
    expect(step.status==S::ScreenIncomplete && step.screen && step.screen->status==E::RequiresView && preserved(),"unknown separate screen view stops without frame publication");
    input.screen->view=awl::WorldMapModelMatrix{1,0,0,2000,0,1,0,0,0,0,1,0};input.screen->half_extents[0]=1000.0f;
    awl::WorldMapModelMatrix invalid{1,0,0,0,0,1,0,0,0,0,1,0};invalid[11]=std::numeric_limits<float>::quiet_NaN();
    input.frame.node_post_transforms.back()=invalid;step=player.advance_gated_actor_frame(input);
    expect(step.status==S::FrameIncomplete && step.screen && step.screen->status==E::Outside && step.proposed_effect_flag_15f==uint8_t(0) && preserved(),
        "screen-outside proposes effect-off and still reaches the late frame failure atomically");input.frame.node_post_transforms.back().reset();
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;step=player.advance_gated_actor_frame(input);allocation_probe::enabled=false;
            if(step.status==S::FrameAdvanced){reached=true;break;}++rejected;
            expect(step.status==S::FrameIncomplete && step.frame && step.frame->status==awl::WorldMapPlayerPrimaryUpdateStatus::AllocationFailure &&
                step.proposed_effect_flag_15f==uint8_t(0) && preserved() && allocation_probe::live==live,"all normal-bounds/screen/frame allocation failures roll back owners and release staging");}
        expect(reached && rejected>5,"normal visibility allocation sweep reaches full frame/mesh preparation");
        std::cout<<"PLAYER_NORMAL_ACTOR_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';}
#else
    (void)allocations;
#endif
    constexpr std::array<std::array<float,3>,4> headings{{{0,0,1},{1,0,0},{0,0,-1},{-1,0,0}}};
    for(size_t tick=0;tick<4;++tick){const auto accepted=startup_snapshot(player);
        input.frame.actor_placement->position={40+float(tick),50+float(tick),60+float(tick)};input.frame.actor_placement->heading=headings[tick];
        input.screen->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};input.screen->half_extents={1000.0f,1000.0f};
        input.screen->half_extents[1].reset();step=player.advance_gated_actor_frame(input);
        expect(step.status==S::ScreenIncomplete && step.screen && step.screen->status==E::RequiresExtent && step.screen->axis==1u &&
            startup_snapshot(player)==accepted,"missing reached Y screen extent preserves each preceding accepted frame");input.screen->half_extents[1]=1000.0f;
        if(tick==1){(*input.screen->view)[3]=2000;input.screen->half_extents[1].reset();}
        if(tick==2)(*input.screen->view)[7]=2000;
        step=player.advance_gated_actor_frame(input);const auto& frame=*player.model().primary().cpu().frame();const bool inside=tick==0 || tick==3;
        expect(step.status==S::FrameAdvanced && step.screen && step.screen->status==(inside?E::Inside:E::Outside) &&
            step.proposed_effect_flag_15f==uint8_t(inside) && step.bounds && step.bounds->sphere.center==
            std::array<float,3>{40+float(tick),51+float(tick),60+float(tick)} && actor_bits(step.bounds->sphere.radius)==0x3fddb3d7 &&
            frame.root_matrix[3]==40+float(tick) && frame.root_matrix[7]==50+float(tick) && frame.root_matrix[11]==60+float(tick) &&
            frame.vertex_output==vertices && holder_snapshot(player.model(),false)==timers,
            "derived normal visibility publishes inside AND screen-outside CPU frames, with separate effect proposals and unchanged skin/timers/clock");
        expect(step.frame && step.frame->texture_features && step.frame->texture_features->status==awl::WorldMapPlayerTextureFeatureStatus::Selected &&
            step.frame->texture_features->cache_writes>0 && player.model().primary().texture_cache() &&
            (*player.model().primary().texture_cache())[0].channel==0 && (*player.model().primary().texture_cache())[1].channel==1 &&
            (*player.model().primary().texture_cache())[0].texture && (*player.model().primary().texture_cache())[1].texture &&
            (*player.model().primary().texture_cache())[0].texture->bank_identity==player.model().primary().setup().selection().assets->texture_bank_identity(Bank::Eyes) &&
            (*player.model().primary().texture_cache())[1].texture->bank_identity==player.model().primary().setup().selection().assets->texture_bank_identity(Bank::Mouth),
            "inside and screen-outside routes update authoritative eye/mouth cache bindings after each CPU frame without advancing timers");
    }
    expect(player.state().counter_1368==parent.counter_1368 && player.state().state_1364==parent.state_1364 &&
        player.state().control_word_1384==parent.control_word_1384 && (player.camera()?camera_snapshot(*player.camera()):0)==camera,
        "normal visibility diagnostics do not acknowledge original parent, camera, metadata or queue stores");
}
void coordinated_animation_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using A=awl::WorldMapPlayerAnimationLoopStatus;using Q=awl::WorldMapPlayerStartupQueueStatus;
    using C=awl::WorldMapPlayerStartupActorCallbackStatus;using H=awl::WorldMapPlayerHolderProgressStatus;
    const auto clock=player.model().game_clock();if(!clock)return;
    const auto interval=clock->state().retrace_interval;
    awl::WorldMapUpdateInputs controls(clock),missing(clock),foreign(std::make_shared<awl::GameClock>());
    const awl::WorldMapControlSources sources{awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0};
    controls.observe_control_sources(sources);foreign.observe_control_sources(sources);
    expect(controls.capture_control()==awl::WorldMapControlStatus::Captured && foreign.capture_control()==awl::WorldMapControlStatus::Captured,"coordinated animation captures its current shared-clock input");
    awl::WorldMapPlayerStartupQueuedActorInput input;
    awl::WorldMapPlayerUpdateOwners updates;updates.queue={{player.update_node_identity(),0x1010}};
    awl::WorldMapPlayerActorQueue render;
    auto before=startup_snapshot(player),pending=update_owners_snapshot(updates);
    expect(player.consume_state29_update_after_animation(controls,input,&updates,&render).status==Q::RequiresAnimation &&
        startup_snapshot(player)==before && update_owners_snapshot(updates)==pending,"queued frame cannot run before its loop's animation slot is accepted");
    expect(player.advance_animation_for_loop(missing).status==A::RequiresControl && player.advance_animation_for_loop(foreign).status==A::InvalidInput &&
        startup_snapshot(player)==before,"missing capture or a different clock cannot advance either animation channel or create a receipt");
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"coordinated update selects unsupported arithmetic");
    const auto rejected=player.advance_animation_for_loop(controls);expect(std::fesetround(rounding)==0,"coordinated update restores caller rounding");
    expect(rejected.status==A::AnimationIncomplete && rejected.animation && rejected.animation->status==H::ChannelIncomplete && startup_snapshot(player)==before &&
        !player.animation_loop(),"failed paired/channel preparation publishes no playback, timer or loop acknowledgement");
    awl::WorldMapPlayerAnimationLoopResult advanced;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t failed=0;bool reached=false;
        for(size_t fail=0;fail<256;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            advanced=player.advance_animation_for_loop(controls);allocation_probe::enabled=false;
            if(advanced.status==A::Advanced){reached=true;break;}++failed;
            expect(advanced.status==A::AnimationIncomplete && advanced.animation && advanced.animation->status==H::AllocationFailure &&
                startup_snapshot(player)==before && allocation_probe::live==live,"animation allocation failures retain absent receipt, accepted owners and free all staging");}
        expect(reached && failed>=3,"coordinated animation allocation sweep reaches publication");
        std::cout<<"PLAYER_ANIMATION_LOOP_ALLOCATION_FAILURES "<<failed<<'\n';}
    else advanced=player.advance_animation_for_loop(controls);
#else
    (void)allocations;advanced=player.advance_animation_for_loop(controls);
#endif
    expect(advanced.status==A::Advanced && advanced.animation && player.animation_loop() &&
        player.animation_loop()->loop_count==clock->state().loop_count && player.animation_loop()->raw_time==clock->state().raw_time &&
        bool(advanced.animation->secondary_channel_status)==(((player.model().descriptor().word_4>>11)&127u)!=127),
        "eligible slot publishes ordered primary/selected-secondary progression and timers before its loop receipt");
    before=startup_snapshot(player);pending=update_owners_snapshot(updates);
    expect(player.advance_animation_for_loop(controls).status==A::AlreadyAdvanced && startup_snapshot(player)==before,
        "repeated same-loop progression never adds another channel tick or timer update");
    auto step=player.consume_state29_update_after_animation(controls,input,&updates,&render);
    expect(step.status==Q::CallbackIncomplete && startup_snapshot(player)==before && update_owners_snapshot(updates)==pending && render.empty() &&
        player.advance_animation_for_loop(controls).status==A::AlreadyAdvanced,"missing frame dependencies retain accepted animation and pending update for retry");
    input.actor.saved_byte_29a01=uint8_t{0};input.actor.script.emplace();input.actor.script->flag_width_4=1;
    input.actor.script->flag_bytes={0};input.actor.script->records.emplace();input.actor.derive_normal_visibility=true;
    input.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{40,50,60},{0,0,1}};
    input.actor.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());
    input.actor.visibility.emplace();input.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;input.actor.visibility->planes[i]=plane;}
    input.actor.screen.emplace();input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents={1000.0f,1000.0f};
    input.effect_gate=uint8_t{0};
    auto invalid=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};invalid[11]=std::numeric_limits<float>::quiet_NaN();
    input.actor.frame.node_post_transforms.back()=invalid;
    step=player.consume_state29_update_after_animation(controls,input,&updates,&render);
    expect(step.status==Q::CallbackIncomplete && startup_snapshot(player)==before && update_owners_snapshot(updates)==pending,
        "late frame failure cannot roll back or repeat the separately accepted model-pool update");input.actor.frame.node_post_transforms.back().reset();
    input.effect_gate.reset();step=player.consume_state29_update_after_animation(controls,input,&updates,&render);
    expect(step.status==Q::CallbackIncomplete && startup_snapshot(player)==before && update_owners_snapshot(updates)==pending && render.empty(),
        "missing effect after frame preparation retains the same loop receipt and retry node");input.effect_gate=uint8_t{0};
    bool completed=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t failed=0;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            step=player.consume_state29_update_after_animation(controls,input,&updates,&render);allocation_probe::enabled=false;
            if(step.status==Q::Completed){completed=true;break;}++failed;
            expect(step.status==Q::CallbackIncomplete && startup_snapshot(player)==before && update_owners_snapshot(updates)==pending && render.empty() &&
                allocation_probe::live==live && player.advance_animation_for_loop(controls).status==A::AlreadyAdvanced,
                "every frame/queue allocation failure preserves the accepted receipt, pending work and both animation channels");}
        expect(completed && failed>5,"coordinated callback allocation sweep reaches frame and queue publication");
        std::cout<<"PLAYER_ANIMATED_QUEUE_ALLOCATION_FAILURES "<<failed<<'\n';}
#endif
    if(!completed)step=player.consume_state29_update_after_animation(controls,input,&updates,&render);
    expect(step.status==Q::Completed && step.callback && step.callback->status==C::Completed && updates.queue.empty() &&
        player.model().primary().cpu().frame() && player.model().primary().cpu().frame()->root_matrix[3]==40 &&
        player.advance_animation_for_loop(controls).status==A::AlreadyAdvanced,"successful frame consumes accepted playback/timers and removes the owned node once");
    // Explicit diagnostic clock steps represent distinct loop samples; this
    // test does not acknowledge a rendered/presented complete game loop.
    for(uint32_t tick=0;tick<4;++tick){clock->set_retrace_interval(tick==0?0u:2u);expect(clock->advance(),"diagnostic fixture moves to its next shared-clock loop");
        updates.queue={{player.update_node_identity(),0x1010}};before=startup_snapshot(player);pending=update_owners_snapshot(updates);
        expect(player.advance_animation_for_loop(controls).status==A::RequiresControl &&
            player.consume_state29_update_after_animation(controls,input,&updates,&render).status==Q::RequiresControl &&
            startup_snapshot(player)==before && update_owners_snapshot(updates)==pending,"clock step invalidates old control before any next-loop model or frame work");
        expect(controls.capture_control()==awl::WorldMapControlStatus::Captured,"next loop captures fresh control");
        expect(player.consume_state29_update_after_animation(controls,input,&updates,&render).status==Q::RequiresAnimation && startup_snapshot(player)==before,
            "fresh input alone cannot reuse the prior loop's accepted animation receipt");
        expect(player.advance_animation_for_loop(controls).status==A::Advanced,"each new loop advances eligible animation exactly once, including zero raw-time increment");
        const auto playback=holder_snapshot(player.model(),false);const auto time=clock->state();
        if(tick==1)input.actor.script->flag_bytes[0]=1;
        if(tick==2){input.actor.visibility->planes[0]->point[0]=0;input.actor.screen.reset();input.effect_gate.reset();}
        step=player.consume_state29_update_after_animation(controls,input,&updates,&render);
        expect(step.status==Q::Completed && updates.queue.empty() && holder_snapshot(player.model(),false)==playback &&
            clock->state().raw_time==time.raw_time && clock->state().loop_count==time.loop_count && player.advance_animation_for_loop(controls).status==A::AlreadyAdvanced,
            "visible, script-blocked and culled callbacks finish without repeating animation/timers or advancing the clock");
        if(tick==1)expect(step.callback && step.callback->status==C::ScriptBlocked,"script block occurs after eligible pool progression");
        if(tick==2)expect(step.callback && step.callback->actor && step.callback->actor->status==awl::WorldMapPlayerStartupActorFrameStatus::VisibilityStopped,"culled frame still completes after pool progression");
        input.actor.script->flag_bytes[0]=0;input.actor.visibility->planes[0]->point[0]=1000;
        input.actor.screen.emplace();input.actor.screen->view=*input.actor.visibility->view;input.actor.screen->half_extents={1000.0f,1000.0f};input.effect_gate=uint8_t{0};
    }
    updates.queue.clear();expect(player.consume_state29_update_after_animation(missing,{},&updates,nullptr).status==Q::Empty,"empty coordinated queue skips receipt and control dependencies");
    awl::WorldMapAnimationChannelProgressStep isolated;
    expect(player.advance_primary_channel(&isolated)==awl::WorldMapAnimationChannelStatus::Advanced && player.animation_loop() && !player.animation_loop()->valid,
        "isolated channel mutation invalidates coordinated proof while preserving its handled loop key");
    before=startup_snapshot(player);updates.queue={{player.update_node_identity(),0x1010}};
    expect(player.advance_animation_for_loop(controls).status==A::RequiresNextLoop &&
        player.consume_state29_update_after_animation(controls,input,&updates,&render).status==Q::RequiresAnimation && startup_snapshot(player)==before,
        "mixing isolated diagnostics cannot reopen an already advanced loop or acknowledge its stale frame proof");
    clock->set_retrace_interval(interval);
    std::cout<<"PLAYER_ANIMATION_LOOP 5 eligible loops / visible, blocked, culled and late-retry frames; explicit clock, owned slot only\n";
}
void gated_actor_frame_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations) {
    using S=awl::WorldMapPlayerStartupActorFrameStatus;using G=awl::WorldMapPlayerActorGateStatus;
    using V=awl::WorldMapPlayerActorVisibilityStatus;
    const auto before=startup_snapshot(player),timers=holder_snapshot(player.model(),false);
    const auto parent=player.state();const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    const auto vertices=player.model().primary().cpu().frame()->vertex_output;
    const auto preserved=[&](){return startup_snapshot(player)==before;};
    awl::WorldMapPlayerStartupActorFrameInput input;
    auto step=player.advance_gated_actor_frame(input);
    expect(step.status==S::GateStopped && step.gate==G::RequiresSavedFlag && preserved(),
        "unknown queued saved flag preserves complete accepted player");
    input.saved_byte_29a01=uint8_t(1);step=player.advance_gated_actor_frame(input);
    expect(step.status==S::GateStopped && step.gate==G::RequiresAuxiliaryCallback && preserved(),
        "normal state29 requires reached auxiliary callback before script/visibility/frame reads");
    input.saved_byte_29a01=uint8_t(0);input.script.emplace();input.script->flag_width_4=1;input.script->flag_bytes={1};
    step=player.advance_gated_actor_frame(input);
    expect(step.status==S::GateStopped && step.gate==G::BlockedByScript && !step.visibility && !step.frame && preserved(),
        "script flag blocks before unknown list, camera and frame state");
    input.script->flag_bytes[0]=0;input.script->records=std::vector<awl::WorldMapPlayerActorScriptRecord>{{17,{}},{0,1},{0,0}};
    step=player.advance_gated_actor_frame(input);
    expect(step.status==S::GateStopped && step.gate==G::BlockedByScript && preserved(),"active first matching script record blocks CPU frame");
    (*input.script->records)[1].active_word_78=0;step=player.advance_gated_actor_frame(input);
    expect(step.status==S::RequiresSphere && preserved(),"inactive first match reaches sphere rather than later duplicate");
    input.sphere=awl::WorldMapPlayerActorSphere{{10,21,30},1};step=player.advance_gated_actor_frame(input);
    expect(step.status==S::RequiresVisibilityOwner && preserved(),"unknown camera/planes do not silently allow a placed frame");
    input.visibility.emplace();input.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;input.visibility->planes[i]=plane;}
    step=player.advance_gated_actor_frame(input);
    expect(step.status==S::RequiresActorPlacement && step.visibility && step.visibility->status==V::Visible && preserved(),"visible frame still requires actor pose source");
    input.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{10,20,30},{0,0,1}};
    input.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());
    input.frame.root_pose=player.model().primary().cpu().root_pose();step=player.advance_gated_actor_frame(input);
    expect(step.status==S::InvalidInput && preserved(),"gated CPU adapter rejects contradictory root sources");input.frame.root_pose.reset();
    input.sphere->center[0]=2000;input.frame.actor_placement->heading[0]=std::numeric_limits<float>::quiet_NaN();
    input.visibility->planes[5].reset();step=player.advance_gated_actor_frame(input);
    expect(step.status==S::VisibilityStopped && step.visibility && step.visibility->status==V::Culled &&
        step.visibility->plane_index==0u && !step.frame && preserved(),"culled player bypasses unknown later planes and invalid unread frame pose");
    input.sphere->center[0]=10;input.frame.actor_placement->heading[0]=0;step=player.advance_gated_actor_frame(input);
    expect(step.status==S::VisibilityStopped && step.visibility && step.visibility->status==V::RequiresPlane &&
        step.visibility->plane_index==5u && preserved(),"missing final visibility plane stops before model publication");
    input.visibility->planes[5]=awl::WorldMapPlayerActorPlane{{0,0,-1000},{0,0,-1}};
    awl::WorldMapModelMatrix invalid{1,0,0,0,0,1,0,0,0,0,1,0};invalid[11]=std::numeric_limits<float>::quiet_NaN();
    input.frame.node_post_transforms.back()=invalid;step=player.advance_gated_actor_frame(input);
    expect(step.status==S::FrameIncomplete && step.frame && step.frame->status==awl::WorldMapPlayerPrimaryUpdateStatus::FrameFailure && preserved(),
        "late frame failure after allowed gates preserves accepted player/camera/clock");input.frame.node_post_transforms.back().reset();
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;step=player.advance_gated_actor_frame(input);allocation_probe::enabled=false;
            if(step.status==S::FrameAdvanced){reached=true;break;}++rejected;
            expect(step.status==S::FrameIncomplete && step.frame && step.frame->status==awl::WorldMapPlayerPrimaryUpdateStatus::AllocationFailure &&
                preserved() && allocation_probe::live==live,"every gated-frame allocation failure rolls back player and releases staging");}
        expect(reached && rejected>5,"gated-frame allocation sweep reaches skin/features and mesh publication");
        std::cout<<"PLAYER_GATED_ACTOR_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';}
#else
    (void)allocations;
#endif
    constexpr std::array<std::array<float,3>,4> headings{{{0,0,1},{1,0,0},{0,0,-1},{-1,0,0}}};
    for(size_t tick=0;tick<4;++tick){const auto accepted=startup_snapshot(player);
        input.frame.actor_placement->position={40+float(tick),50+float(tick),60+float(tick)};input.frame.actor_placement->heading=headings[tick];
        input.sphere->center={40+float(tick),51+float(tick),60+float(tick)};input.script->flag_bytes[0]=1;
        step=player.advance_gated_actor_frame(input);
        expect(step.status==S::GateStopped && startup_snapshot(player)==accepted,"script-blocked route retains preceding accepted model at each tick");
        input.script->flag_bytes[0]=0;input.sphere->center[0]=2000;step=player.advance_gated_actor_frame(input);
        expect(step.status==S::VisibilityStopped && step.visibility && step.visibility->status==V::Culled && startup_snapshot(player)==accepted,
            "off-camera route retains preceding accepted model at each tick");
        input.sphere->center[0]=input.frame.actor_placement->position[0];step=player.advance_gated_actor_frame(input);
        const auto& cpu=player.model().primary().cpu();const auto& frame=*cpu.frame();
        expect(step.status==S::FrameAdvanced && frame.root_matrix[3]==input.frame.actor_placement->position[0] &&
            frame.root_matrix[7]==input.frame.actor_placement->position[1] && frame.root_matrix[11]==input.frame.actor_placement->position[2] &&
            frame.vertex_output==vertices && holder_snapshot(player.model(),false)==timers,"visible route publishes placed CPU geometry without advancing playback/timers/clock");
    }
    expect(player.state().counter_1368==parent.counter_1368 && player.state().state_1364==parent.state_1364 &&
        player.state().control_word_1384==parent.control_word_1384 && (player.camera()?camera_snapshot(*player.camera()):0)==camera,
        "gated CPU slice does not publish callback control, camera, queue or actor metadata effects");
    normal_actor_frame_owner_checks(player,allocations);
    actor_callback_owner_checks(player,allocations);
    queued_actor_owner_checks(player,allocations);
    texture_timer_owner_checks(player,allocations);
    channel_progress_owner_checks(player,allocations);
    coordinated_animation_owner_checks(player,allocations);
}
void actor_placement_owner_checks(awl::WorldMapPlayerStartup& player,bool allocations=false) {
    using S=awl::WorldMapPlayerPrimaryUpdateStatus;using F=awl::WorldMapPlayerFrameStatus;
    const auto& primary=player.model().primary();const auto& cpu=primary.cpu();
    expect(cpu.frame().has_value(),"actor placement integration starts from an accepted CPU frame");if(!cpu.frame())return;
    const auto before=holder_snapshot(player.model()),timers=holder_snapshot(player.model(),false);
    const auto root_before=cpu.root_pose();const auto vertices_before=cpu.frame()->vertex_output;
    const auto parent=player.state();const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    const auto preserved=[&](){return holder_snapshot(player.model())==before;};
    awl::WorldMapPlayerPrimaryFrameInput input;input.node_post_transforms.resize(primary.model().core().nodes.size());
    input.actor_placement=awl::WorldMapPlayerActorPlacement{{10,20,30},{0,0,1}};
    input.root_pose=root_before;auto step=player.advance(input);
    expect(step.status==S::FrameFailure && step.frame_failure && step.frame_failure->status==F::InvalidInput && preserved(),
        "conflicting explicit/actor root sources preserve all CPU state");input.root_pose.reset();
    input.actor_placement->position[0]=std::numeric_limits<float>::quiet_NaN();step=player.advance(input);
    expect(step.status==S::FrameFailure && step.frame_failure && step.frame_failure->status==F::InvalidInput && preserved(),
        "invalid actor placement preserves model/root/features/frame/mesh");input.actor_placement->position[0]=10;
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"actor frame rounding setup");
    step=player.advance(input);const auto kept=std::fegetround();expect(std::fesetround(rounding)==0,"restore actor frame rounding");
    expect(step.status==S::FrameFailure && step.frame_failure && step.frame_failure->status==F::UnsupportedNumerics &&
        kept==FE_UPWARD && preserved(),"actor frame rejects unsupported rounding before publishing the translated root");
    awl::WorldMapModelMatrix invalid{1,0,0,0,0,1,0,0,0,0,1,0};invalid[11]=std::numeric_limits<float>::quiet_NaN();
    input.node_post_transforms.back()=invalid;step=player.advance(input);
    expect(step.status==S::FrameFailure && step.frame_failure && preserved(),
        "late last-node failure rolls back earlier actor placement and all frame/mesh effects");input.node_post_transforms.back().reset();
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations) {
        const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
        for(size_t fail=0;fail<128;++fail) {
            allocation_probe::remaining=fail;allocation_probe::enabled=true;step=player.advance(input);allocation_probe::enabled=false;
            if(step.status==S::Advanced){reached=true;break;}++rejected;
            expect(step.status==S::AllocationFailure && preserved() && allocation_probe::live==live,
                "every placed-frame allocation failure preserves root/features/skin/mesh and releases staging");
        }
        expect(reached && rejected>5,"placed-frame allocation sweep covers sampling, skin/features and geometry staging");
        std::cout<<"PLAYER_ACTOR_FRAME_ALLOCATION_FAILURES "<<rejected<<'\n';
    }
#else
    (void)allocations;
#endif
    constexpr std::array<std::array<float,3>,4> headings{{{0,0,1},{1,0,0},{0,0,-1},{-1,0,0}}};
    constexpr float cosines[]{1,0,-1,0},sines[]{0,1,0,-1};
    float scales[3];for(size_t i=0;i<3;++i)std::memcpy(&scales[i],&root_before[i+1],4);
    for(size_t tick=0;tick<4;++tick) {
        input.actor_placement->position={10+float(tick),20+float(tick),30+float(tick)};
        input.actor_placement->heading=headings[tick];input.actor_placement->heading[1]=std::numeric_limits<float>::quiet_NaN();
        step=player.advance(input);const auto& frame=*cpu.frame();
        const awl::WorldMapModelMatrix expected{cosines[tick]*scales[0],0,sines[tick]*scales[2],10+float(tick),
            0,scales[1],0,20+float(tick),-sines[tick]*scales[0],0,cosines[tick]*scales[2],30+float(tick)};
        bool same=true;for(size_t i=0;i<12;++i)if(std::abs(frame.root_matrix[i]-expected[i])>1e-5f)same=false;
        expect(step.status==S::Advanced && same && frame.root_pose_after==cpu.root_pose() &&
            primary.model().core().byte_1c==uint8_t(cpu.root_pose()[0]>>24) &&
            (cpu.root_pose()[0]&0x00ffffffu)==(root_before[0]&0x00ffffffu),
            "four placed CPU frames turn with the actor heading, preserve scale/header and retain translated root state");
        expect(frame.vertex_output==vertices_before && holder_snapshot(player.model(),false)==timers,
            "world placement is applied through root/features once; skin vertices and playback/timers/clock remain unchanged");
        for(size_t i=0;i<primary.setup().features().size();++i)if(!primary.setup().features()[i].node)
            expect(cpu.feature_matrices()[i]==frame.root_matrix,"retained root feature publishes the placed world matrix");
    }
    expect(player.state().counter_1368==parent.counter_1368 && player.state().state_1364==parent.state_1364 &&
        player.state().control_word_1384==parent.control_word_1384 &&
        (player.camera()?camera_snapshot(*player.camera()):0)==camera,
        "CPU actor placement does not acknowledge parent/camera updates or queue consumption");
    gated_actor_frame_owner_checks(player,allocations);
}
void primary_slice_owner_checks(const awl::WorldMapPlayerStartupProviders&,const awl::WorldMapPlayerStartupQuery&,
    const std::shared_ptr<awl::WorldMapCameraCollisionAsset>&,const awl::WorldMapCameraViewQuery&,bool);
void scene_assembly_checks(const awl::WorldMapPlayerStartupProviders&,const awl::WorldMapPlayerStartupQuery&,
    const std::shared_ptr<awl::WorldMapCameraCollisionAsset>&,const awl::WorldMapCameraViewQuery&,
    const awl::WorldMapPlayerContextUpdateSources*,bool);
std::vector<uint8_t> state1_movement_terrain();
std::vector<uint8_t> scene_constructor_dol();
void startup_tick_owner_checks(const awl::WorldMapPlayerStartupProviders&,const awl::WorldMapPlayerStartupQuery&,
    const std::shared_ptr<awl::WorldMapCameraCollisionAsset>&,const awl::WorldMapCameraViewQuery&,bool,
    const awl::WorldMapPlayerContextUpdateSources* =nullptr,bool owned_sources=false);
void player_sources_fixture(Fixture&);
void startup_owner_checks() {
    using S=awl::WorldMapPlayerStartupStatus;using A=awl::WorldMapPlayerStartAnimationStatus;using I=awl::WorldMapAnimationInitializerStatus;
    Fixture fixture;auto payloads=files();payloads[0]=setup_model();payloads[1]=geometry_gpl();payloads[2]=execution_skin(1);
    fixture.write("boy_0.arc",archive(payloads));fixture.write("boy_0.anm.arc",archive({owner_frame_clip(),owner_frame_clip(4)}));
    fixture.write("boy_0_subanm.arc",archive({owner_frame_clip()}));fixture.write("boy_0_subact.arc",archive({setup_model()}));
    auto initial_bytes=initial_input_dol(true),start_bytes=holder_start_tables(true);awl::WorldMapPlayerStartupProviders p;
    expect(awl::load_world_map_player_model_assets(0,&p.models)==Status::Loaded &&
        awl::load_world_map_player_animation_assets(&p.animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded &&
        awl::decode_world_map_player_initial_animation_inputs(initial_bytes.data(),initial_bytes.size(),&p.initial)==awl::WorldMapPlayerInitialAnimationStatus::Decoded &&
        awl::decode_world_map_player_start_animation_tables(start_bytes.data(),start_bytes.size(),&p.starts)==A::Decoded &&
        awl::decode_world_map_player_timer_assets(sampled_timer_table(),sampled_timer_table(),&p.timers)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"complete invented startup providers prepare");
    if(!p.models || !p.animations || !p.initial || !p.starts || !p.timers)return;
    p.clock=std::make_shared<awl::GameClock>();p.clock->set_retrace_interval(2);expect(p.clock->advance() && p.clock->advance(),"startup test clock advances explicitly before construction");
    awl::WorldMapPlayerStartupQuery q;q.tail.inputs.start.saved_pose.scene_type=1;q.audio_byte_90=uint8_t(0);
    std::unique_ptr<awl::WorldMapPlayerStartup> owner;
    const auto result=awl::construct_world_map_player_startup(p,q,&owner);
    expect(result.status==S::ConstructedCpuStartup && result.animation && result.animation->status==A::Advanced && owner,"normal no-message constructor reaches owned CPU state 29 after a changed animation callback");if(!owner)return;
    awl::WorldMapPlayerTimerResourceView eye,mouth;
    expect(p.timers->resource(awl::WorldMapPlayerTimerBank::Eyes,1,0,&eye) && p.timers->resource(awl::WorldMapPlayerTimerBank::Mouth,1,0,&mouth),"selected synthetic dispatch resources resolve");
    const auto& state=owner->state();const auto& feature=*owner->model().state().feature_38;
    expect(state.state_1364==0x29 && state.counter_1368==0 && state.secondary_byte_3f0==1 && state.target_14ac==0u && state.direction_14b4==7u && feature.type_0==0x3B && feature.type_4==0x25 &&
        feature.first_14.resource_0==eye.identity && feature.second_24.resource_0==mouth.identity && feature.first_14.clock_4==66 && feature.second_24.clock_4==66 &&
        owner->model().initial_model_state()->initialized_c==1 && owner->starts()==p.starts && !owner->model().primary().cpu().frame() && p.clock->state().loop_count==2,
        "model timer writes, retained startup tables and parent fields publish together without frame/time advancement");
    awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(3);
    expect(owner->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && owner->model().primary().cpu().frame()->skin_executed &&
        owner->model().primary().cpu().frame()->root_matrix==awl::WorldMapModelMatrix{2,0,0,0,0,2,0,0,0,0,2,0},"owned parent startup reaches first scaled CPU frame without supplied pose");
    const auto before=startup_snapshot(*owner);const auto* old=owner.get();
    const auto preserved=[&](){return owner.get()==old && startup_snapshot(*owner)==before;};
    auto bad=p;bad.clock.reset();auto stopped=awl::construct_world_map_player_startup(bad,q,&owner);
    expect(stopped.status==S::InitialModelIncomplete && stopped.initial_model.timer.status==awl::WorldMapPlayerTimerAssetsStatus::RequiresClock && preserved(),"missing constructor clock preserves complete prior startup/frame");bad=p;
    auto request=q;request.tail.inputs.start.state_58c=1;request.tail.action_148_after_setup=17;
    stopped=awl::construct_world_map_player_startup(p,request,&owner);
    expect(stopped.status==S::UnsupportedConstructorState && stopped.tail && stopped.tail->requested_state==0xF && preserved(),"blocked guard stops at F before message/audio and cannot publish state 29");
    request=q;request.tail.inputs.secondary_byte_3f2=1;
    expect(awl::construct_world_map_player_startup(p,request,&owner).status==S::ConstructorTailIncomplete && preserved(),"restored-pose constructor remains an explicit dependency");
    request=q;request.tail.binding_present=true;request.tail.binding_word_0=9;
    expect(awl::construct_world_map_player_startup(p,request,&owner).status==S::ConstructorTailIncomplete && preserved(),"busy binding cannot silently skip model source effects");
    request=q;request.tail.action_148_after_setup=17;
    stopped=awl::construct_world_map_player_startup(p,request,&owner);
    expect(stopped.status==S::ConstructorTailIncomplete && stopped.tail_status==awl::WorldMapPlayerConstructorTailStatus::MissingCameraByteEvidence && preserved(),"reached message retains missing camera-byte evidence");
    request.tail.payload_camera_byte_known=true;
    expect(awl::construct_world_map_player_startup(p,request,&owner).status==S::RequiresMessageDelivery && preserved(),"prepared scene-object message still requires actual recipient/attachment delivery");
    request=q;request.audio_byte_90.reset();expect(awl::construct_world_map_player_startup(p,request,&owner).status==S::RequiresAudioByte && preserved(),"scene-one audio byte is required before model work");
    request.audio_byte_90=uint8_t(1);expect(awl::construct_world_map_player_startup(p,request,&owner).status==S::RequiresAudioStop && preserved(),"active audio remains a dependency before model work");
    bad.starts.reset();stopped=awl::construct_world_map_player_startup(bad,q,&owner);
    expect(stopped.status==S::AnimationIncomplete && stopped.animation && stopped.animation->status==A::RequiresTables && preserved(),"late selector-table stop rolls back already staged timer/type changes");bad=p;
    request=q;request.item_144_after_setup=1;stopped=awl::construct_world_map_player_startup(p,request,&owner);
    expect(stopped.status==S::AnimationIncomplete && stopped.animation && stopped.animation->status==A::RequiresItemType && preserved(),"late nonzero item metadata dependency preserves complete previous startup");
    request.item_type=awl::WorldMapPlayerStartItemType{1,1};stopped=awl::construct_world_map_player_startup(p,request,&owner);
    expect(stopped.status==S::AnimationIncomplete && stopped.animation && stopped.animation->initializer_status==I::RequiresSecondarySetup && preserved(),"selected secondary setup remains blocked after owned model timer work");
    expect(awl::decode_world_map_player_timer_assets(timer_table(),timer_table(1),&bad.timers)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"limited mouth provider prepares constructor row but omits dispatch row");
    stopped=awl::construct_world_map_player_startup(bad,q,&owner);
    expect(stopped.status==S::RequiresTimerRow && stopped.required_row && stopped.required_row->table_identity==bad.timers->table_identity(awl::WorldMapPlayerTimerBank::Mouth) &&
        stopped.required_row->index==1 && preserved(),"late second model timer row failure rolls back first reset and parent/animation state");bad=p;
    expect(awl::construct_world_map_player_startup(p,q,nullptr).status==S::InvalidInput,"null startup output rejects");
    // Same-base selector still calls the callback and clears the parent counter.
    auto same=start_bytes;word(same,0x54,0x81000000);
    for(uint32_t i=0;i<4;++i)word(same,0x264+i*4,0x81000000+i*24);
    word(same,0x160,1u<<10);word(same,0x164,63u|(127u<<11)|(255u<<19));
    expect(awl::decode_world_map_player_start_animation_tables(same.data(),same.size(),&bad.starts)==A::Decoded,"same-base selector fixture decodes");
    std::unique_ptr<awl::WorldMapPlayerStartup> equal;
    stopped=awl::construct_world_map_player_startup(bad,q,&equal);
    expect(stopped.status==S::ConstructedCpuStartup && stopped.animation && stopped.animation->status==A::Unchanged && equal->state().counter_1368==0 &&
        equal->state().secondary_byte_3f0==1 && equal->starts()==bad.starts,"same-base animation no-op still establishes callback counter/flag writes and retains selection owner");equal.reset();bad=p;
    // Below-threshold changes preserve both previous timers and need no row.
    auto low=initial_bytes;word(low,0x250,UINT32_MAX);word(low,0x254,0x23);
    expect(awl::decode_world_map_player_initial_animation_inputs(low.data(),low.size(),&bad.initial)==awl::WorldMapPlayerInitialAnimationStatus::Decoded,"dispatch-only negative/below-threshold rows retain known initial timer construction");
    expect(awl::decode_world_map_player_timer_assets(timer_table(1),timer_table(1),&bad.timers)==awl::WorldMapPlayerTimerAssetsStatus::Decoded,"single-row timer providers suffice for initial construction");
    stopped=awl::construct_world_map_player_startup(bad,q,&equal);
    expect(stopped.status==S::ConstructedCpuStartup && equal && equal->model().state().feature_38->type_0==UINT32_MAX &&
        equal->model().state().feature_38->type_4==0x23 && equal->model().state().feature_38->first_14.clock_4==66 &&
        bad.timers->resource(awl::WorldMapPlayerTimerBank::Eyes,0,0,&eye) && equal->model().state().feature_38->first_14.resource_0==eye.identity,
        "negative/below-threshold dispatch types preserve initial timers without inventing row lookups");equal.reset();bad={};
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t rejected=0;bool reached=false;
    for(size_t fail=0;fail<512;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;const auto attempt=awl::construct_world_map_player_startup(p,q,&owner);allocation_probe::enabled=false;
        if(attempt.status==S::ConstructedCpuStartup){reached=true;break;}++rejected;
        expect(attempt.status==S::AllocationFailure && preserved() && allocation_probe::live==live,"every startup allocation stop releases staged model/timer/animation/parent work and preserves previous frame and clock");}
    expect(reached && rejected>225,"startup sweep includes model type row staging and changed callback setup after initial model construction");
    std::cout<<"PLAYER_STARTUP_ALLOCATION_FAILURES "<<rejected<<'\n';
#endif
    {
        auto paired_bytes=start_bytes;word(paired_bytes,0x164,63u|(1u<<11)|(255u<<19));auto paired=p;
        paired.clock=std::make_shared<awl::GameClock>(*p.clock);
        expect(awl::decode_world_map_player_start_animation_tables(paired_bytes.data(),paired_bytes.size(),&paired.starts)==A::Decoded,
            "coordinated paired startup selector decodes");
        std::unique_ptr<awl::WorldMapPlayerStartup> pair;
        expect(awl::construct_world_map_player_startup(paired,q,&pair).status==S::ConstructedCpuStartup && pair && pair->model().secondary_model(),
            "full startup owns selected secondary for coordinated loop/frame retry checks");
        if(pair)coordinated_animation_owner_checks(*pair,true);
    }
    using C=awl::WorldMapPlayerState29CameraStatus;
    expect(owner->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"camera rollback checks retain an evaluated CPU frame");
    const auto no_camera=startup_snapshot(*owner);awl::WorldMapPlayerStartupState29Input callback;callback.player_position={2,0,0};
    expect(owner->advance_state29_camera(callback).status==C::RequiresCamera && startup_snapshot(*owner)==no_camera,"first callback requires explicit camera evidence and preserves prior model/frame/counter");
    auto camera=std::make_shared<awl::WorldMapCameraCollisionAsset>();awl::WorldMapCameraViewQuery camera_query;camera_query.target.distance_30=2;camera_query.up_vector_24={0,1,0};
    callback.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera,camera_query};
    expect(owner->advance_state29_camera(callback).status==C::RequiresCamera && startup_snapshot(*owner)==no_camera,"unloaded camera provider preserves complete startup");
    fixture.write("jimen-camera.col",startup_camera_col());expect(camera->load(),"invented flat camera terrain loads through the real provider");
    parent_owner_checks(p,q,frame,camera,camera_query);
    dispatch_owner_checks(p,q,frame,camera,camera_query);
    primary_slice_owner_checks(p,q,camera,camera_query,true);
    startup_tick_owner_checks(p,q,camera,camera_query,true);
    {
        auto paired=p;auto bytes=start_bytes;word(bytes,0x164,63u|(1u<<11)|(255u<<19));
        expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&paired.starts)==A::Decoded,"ordered paired selector decodes");
        primary_slice_owner_checks(paired,q,camera,camera_query,true);
        startup_tick_owner_checks(paired,q,camera,camera_query,true);
    }
    {
        // Invented timed, looping descriptor remains incomplete across the
        // diagnostic startup ticks, reaching 42BBC's actual movement route.
        auto moving=p;auto bytes=start_bytes;word(bytes,0x160,(2u<<10)|(1023u<<14));
        expect(awl::decode_world_map_player_start_animation_tables(bytes.data(),bytes.size(),&moving.starts)==A::Decoded,"state-zero movement selector decodes a bounded long duration");
        startup_tick_owner_checks(moving,q,camera,camera_query,true);
        player_sources_fixture(fixture);
        startup_tick_owner_checks(moving,q,camera,camera_query,true,nullptr,true);
        auto record=state1_movement_terrain();record[6]=0;
        fixture.write("roomobj.col.arc",archive(std::vector<std::vector<uint8_t>>(54,record)));
        fixture.write("mapse.col.arc",archive(std::vector<std::vector<uint8_t>>(21,record)));
        fixture.write("maperase.col.arc",archive({record}));
        scene_assembly_checks(moving,q,camera,camera_query,nullptr,true);
        scene_assembly_checks(p,q,camera,camera_query,nullptr,false);
        {const auto constructor_bytes=scene_constructor_dol();std::shared_ptr<const awl::WorldMapSceneConstructorTables> tables;
            expect(awl::WorldMapSceneConstructorTables::decode(constructor_bytes.data(),constructor_bytes.size(),&tables)==awl::WorldMapSceneConstructorStatus::Decoded,"invented constructor metadata prepares for wrong-target check");
            const auto* previous_constructor_tables=tables.get();std::filesystem::create_directory(fixture.root/"sys");
            {std::ofstream file(fixture.root/"sys"/"main.dol",std::ios::binary);file.write(reinterpret_cast<const char*>(constructor_bytes.data()),std::streamsize(constructor_bytes.size()));expect(bool(file),"task-owned synthetic DOL writes");}
            expect(awl::WorldMapSceneConstructorTables::load(&tables)==awl::WorldMapSceneConstructorStatus::WrongDol && tables.get()==previous_constructor_tables,"plausible constructor layout with wrong SHA1 cannot gain verified identity");
            std::filesystem::remove(fixture.root/"sys"/"main.dol");std::filesystem::remove(fixture.root/"sys");}
    }
    {
        // This fixture now advances its shared clock. Isolate it from the
        // original startup whose camera failure snapshot is checked afterward.
        auto status_providers=p;status_providers.clock=std::make_shared<awl::GameClock>(*p.clock);
        std::unique_ptr<awl::WorldMapPlayerStartup> status_player;
        expect(awl::construct_world_map_player_startup(status_providers,q,&status_player).status==S::ConstructedCpuStartup && status_player &&
            status_player->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced,"status integration owns a complete CPU startup/frame");
        if(status_player){awl::WorldMapPlayerUpdateOwners effects;effects.scene_one_position_240=std::array<float,3>{9,8,7};
            effects.queue={{1,0x100f},{2,0x1010},{3,UINT32_MAX}};
            awl::WorldMapPlayerStartupDispatchInput input;input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.scene_child_word_4=0;
            input.pose={1,{2,0,0},{-0.0f,0,2}};input.action_148=0;input.player_node_identity=99;
            input.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera,camera_query};
            status_parent_checks(*status_player,effects,input,true);
            owned_status_parent_checks(*status_player,effects,input);
            fresh_status_parent_checks(*status_player,effects,input);
            actor_placement_owner_checks(*status_player,true);}
    }
    callback.first_camera->camera.target.distance_30=0;
    expect(owner->advance_state29_camera(callback).status==C::CameraFailure && startup_snapshot(*owner)==no_camera,"degenerate seed does not bind provider or consume counter");
    callback.first_camera->camera=camera_query;
    const auto clock_before=p.clock->state();const auto model_before=holder_snapshot(owner->model());
    auto camera_step=owner->advance_state29_camera(callback);
    expect(camera_step.status==C::Advanced && owner->camera() && owner->state().counter_1368==1 &&
        owner->camera()->camera.target.camera.position==callback.player_position && owner->camera()->update.final_target==std::array<float,3>{2,0,2},
        "first camera callback binds checked terrain, copies position, builds view and increments counter atomically");
    auto camera_before=startup_snapshot(*owner);
    expect(owner->advance_state29_camera(callback).status==C::InvalidInput && startup_snapshot(*owner)==camera_before,"bound camera cannot be silently reset by another initial seed");
    callback.first_camera.reset();callback.action_148=17;
    expect(owner->advance_state29_camera(callback).status==C::Advanced && owner->state().counter_1368==2,"nonzero target is not consumed at counter one");
    camera_before=startup_snapshot(*owner);callback.action_148.reset();callback.player_position[0]=3;
    camera_step=owner->advance_state29_camera(callback);
    expect(camera_step.status==C::RequiresActionTarget && camera_step.update && !camera_step.counter_after && startup_snapshot(*owner)==camera_before,
        "late unknown action reports staged camera diagnostics while preserving accepted camera/model/frame/counter");
    callback.action_148=-1;camera_step=owner->advance_state29_camera(callback);
    expect(camera_step.status==C::RequiresAuxiliarySource && camera_step.update && startup_snapshot(*owner)==camera_before,"all nonzero action words require actual auxiliary source at counter two");
    callback.player_position[0]=std::numeric_limits<float>::quiet_NaN();camera_step=owner->advance_state29_camera(callback);
    expect(camera_step.status==C::CameraFailure && !camera_step.update && startup_snapshot(*owner)==camera_before,"invalid later pose fails before attachment and preserves previous frame/camera");
    callback.player_position[0]=3;callback.action_148=0;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto camera_live=allocation_probe::live;size_t camera_rejected=0;bool camera_reached=false;
    for(size_t fail=0;fail<64;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;camera_step=owner->advance_state29_camera(callback);allocation_probe::enabled=false;
        if(camera_step.status==C::Advanced){camera_reached=true;break;}++camera_rejected;
        expect(camera_step.status==C::AllocationFailure && startup_snapshot(*owner)==camera_before && allocation_probe::live==camera_live,
            "every first/second terrain allocation failure rolls back camera and parent counter without leaking staged work");}
    expect(camera_reached && camera_rejected>1,"camera allocation sweep reaches both terrain passes and successful retry");
    std::cout<<"PLAYER_STATE29_CAMERA_ALLOCATION_FAILURES "<<camera_rejected<<'\n';
#else
    camera_step=owner->advance_state29_camera(callback);
#endif
    expect(camera_step.status==C::Advanced && owner->state().counter_1368==3 && holder_snapshot(owner->model())==model_before &&
        p.clock->state().raw_time==clock_before.raw_time && p.clock->state().loop_count==clock_before.loop_count,
        "zero-target retry publishes camera/counter alone and never advances model playback, CPU frame or shared clock");
    const auto camera_lifetime=std::weak_ptr<const awl::WorldMapCameraCollisionAsset>(camera);camera.reset();
    expect(!camera_lifetime.expired() && owner->advance_state29_camera(callback).status==C::Advanced,"camera terrain remains usable after its last caller handle drops");
    const auto model_lifetime=std::weak_ptr<const Assets>(p.models);const auto inputs_lifetime=std::weak_ptr<const awl::WorldMapPlayerInitialAnimationInputs>(p.initial);
    const auto starts_lifetime=std::weak_ptr<const awl::WorldMapPlayerStartAnimationTables>(p.starts);const auto clock_lifetime=std::weak_ptr<awl::GameClock>(p.clock);p={};
    expect(!model_lifetime.expired() && !inputs_lifetime.expired() && !starts_lifetime.expired() && !clock_lifetime.expired() &&
        owner->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && owner->model().game_clock()->state().raw_time==66,
        "CPU startup retains model, both static input owners and clock after all caller handles drop");owner.reset();
    expect(model_lifetime.expired() && inputs_lifetime.expired() && starts_lifetime.expired() && clock_lifetime.expired() && camera_lifetime.expired(),"last startup releases model, camera providers and shared clock");
}
void primary_slice_owner_checks(const awl::WorldMapPlayerStartupProviders& source,const awl::WorldMapPlayerStartupQuery& query,
    const std::shared_ptr<awl::WorldMapCameraCollisionAsset>& camera,const awl::WorldMapCameraViewQuery& view,bool allocations) {
    using S=awl::WorldMapPlayerPrimarySliceStatus;using P=awl::WorldMapPlayerUpdateStatus;using A=awl::WorldMapPlayerAnimationLoopStatus;
    using Q=awl::WorldMapPlayerStartupQueueStatus;using F=awl::WorldMapPlayerPrimaryPassesStatus;
    using G=awl::WorldMapPlayerFollowupStatus;
    auto providers=source;providers.clock=std::make_shared<awl::GameClock>(*source.clock);
    std::unique_ptr<awl::WorldMapPlayerStartup> player;
    expect(awl::construct_world_map_player_startup(providers,query,&player).status==awl::WorldMapPlayerStartupStatus::ConstructedCpuStartup && player,
        "ordered primary fixture owns a fresh startup and private shared clock");if(!player)return;
    const auto registrations=player->primary_registration();
    expect(registrations[0].priority==0x5001 && registrations[1].priority==0xf000 && registrations[0].node_identity &&
        registrations[1].node_identity && registrations[0].node_identity!=registrations[1].node_identity && registrations[0].node_identity!=player->update_node_identity() &&
        registrations[1].node_identity!=player->update_node_identity(),"native primary keys retain actual player/model-manager priorities and distinct update ownership");
    awl::WorldMapUpdateInputs inputs(providers.clock),missing(providers.clock),foreign(std::make_shared<awl::GameClock>());
    const awl::WorldMapControlSources sources{awl::WorldMapControlPhase{uint8_t{1},123456u,uint8_t{0}},0};
    inputs.observe_control_sources(sources);foreign.observe_control_sources(sources);
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured && foreign.capture_control()==awl::WorldMapControlStatus::Captured,"ordered primary inputs capture active status control");
    awl::WorldMapPlayerUpdateOwners owners;owners.scene_one_position_240=std::array<float,3>{};owners.queue={{900,0x1011}};
    expect(player->initialize_fresh_status(inputs,&owners)==awl::WorldMapPlayerStatusCode::Prepared,"ordered primary pointed status initializes from its phase owner");
    awl::WorldMapPlayerStartupDispatchInput input;input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.pose={1,{2,0,0},{0,0,1}};
    input.action_148=0;input.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera,view};
    auto before=startup_snapshot(*player),effects=update_owners_snapshot(owners);
    expect(player->complete_state29_primary_passes(missing,{},{}).status==F::RequiresControl &&
        player->complete_state29_primary_passes(foreign,{},{}).status==F::InvalidInput &&
        player->complete_state29_primary_passes(inputs,{},{}).status==F::RequiresPrimary && startup_snapshot(*player)==before,
        "followup callbacks require matching clock, fresh capture and an accepted first-pass parent before reading observations");
    {awl::WorldMapPlayerUpdateOwners pending;pending.queue={{player->update_node_identity(),0x1010}};
        expect(player->consume_state29_update_after_primary_slice(inputs,{},&pending,nullptr).status==Q::RequiresPrimary && startup_snapshot(*player)==before && pending.queue.size()==1,
            "owned queued work requires its ordered primary receipt before actor observations");}
    expect(player->advance_state29_primary_slice(missing,input,&owners).status==S::RequiresControl &&
        player->advance_state29_primary_slice(foreign,input,&owners).status==S::InvalidInput && startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects,
        "missing/stale or foreign primary inputs preserve both callbacks and receipts");
    input.player_node_identity=99;
    expect(player->advance_state29_primary_slice(inputs,input,&owners).status==S::InvalidInput && startup_snapshot(*player)==before,
        "ordered primary facade derives the owned update key and rejects an overriding foreign key");input.player_node_identity=0;
    auto step=player->advance_state29_primary_slice(inputs,input,&owners);
    expect(step.status==S::ParentIncomplete && step.parent && step.parent->status==P::RequiresSceneWord && !step.animation &&
        startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects,"parent dependency stops before model-manager progression");input.scene_child_word_4=0;
    const auto model_before=holder_snapshot(player->model());
    awl::WorldMapPlayerStatusInputs status;const auto factors=player->starts()->status_factors(player->model().descriptor());
    status.globals=factors.globals;status.rate=factors.rate;status.phase_raw=123456;
    const auto expected=awl::prepare_world_map_player_status(*owners.status,player->model().descriptor().word_8,status);
    expect(expected.status==awl::WorldMapPlayerStatusCode::Prepared && expected.after,"independently compared parent status body prepares its one-update result");
    bool complete=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t failed=0;bool accepted_parent=false;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            step=player->advance_state29_primary_slice(inputs,input,&owners);allocation_probe::enabled=false;
            if(step.status==S::AnimationIncomplete){accepted_parent=true;break;}
            if(step.status==S::Completed){complete=true;break;}++failed;
            expect(step.status==S::ParentIncomplete && step.parent && step.parent->status==P::AllocationFailure &&
                startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects && allocation_probe::live==live,
                "every pre-publication camera/status/queue allocation failure preserves both primary callbacks and receipts");}
        expect(accepted_parent && failed>0 && player->primary_loop() && player->primary_loop()->valid && !player->animation_loop() &&
            holder_snapshot(player->model())==model_before && player->state().counter_1368==1,"late model failure retains accepted parent/status/registration and untouched animation");
        std::cout<<"PLAYER_PRIMARY_PREFIX_ALLOCATION_FAILURES "<<failed<<'\n';
        before=startup_snapshot(*player);effects=update_owners_snapshot(owners);
        expect(player->complete_state29_primary_passes(inputs,{},{}).status==F::RequiresAnimation && startup_snapshot(*player)==before,
            "accepted parent alone cannot allow secondary/tertiary callbacks before model manager progression");
        // Retry observations cannot replace already accepted parent work.
        input.pose.position[0]=std::numeric_limits<float>::quiet_NaN();input.player_node_identity=99;
        const auto retained=allocation_probe::live;failed=0;
        for(size_t fail=0;fail<128;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            step=player->advance_state29_primary_slice(inputs,input,&owners);allocation_probe::enabled=false;
            if(step.status==S::Completed){complete=true;break;}++failed;
            expect(step.status==S::AnimationIncomplete && step.parent_reused && !step.parent && step.animation &&
                step.animation->animation && step.animation->animation->status==awl::WorldMapPlayerHolderProgressStatus::AllocationFailure &&
                startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects && allocation_probe::live==retained,
                "model retry allocation failures preserve the accepted parent and never repeat camera/status or registration");}
        expect(complete && failed>=3,"model retry sweep reaches ordered completion");std::cout<<"PLAYER_PRIMARY_MODEL_RETRY_ALLOCATION_FAILURES "<<failed<<'\n';}
#else
    (void)allocations;(void)model_before;
#endif
    if(!complete)step=player->advance_state29_primary_slice(inputs,input,&owners);
    expect(step.status==S::Completed && player->primary_loop() && player->primary_loop()->valid && player->animation_loop() &&
        player->state().counter_1368==1 && owners.status && expected.after && status_words(*owners.status)==status_words(*expected.after) &&
        owners.queue.size()==2 && owners.queue[0].node_identity==player->update_node_identity() && owners.queue[0].priority==0x1010,
        "ordered completion publishes exactly one parent camera/status update and owned registration before model progression");
    before=startup_snapshot(*player);effects=update_owners_snapshot(owners);
    {auto other=owners;
        expect(player->advance_state29_primary_slice(inputs,{},&other).status==S::InvalidInput &&
            player->consume_state29_update_after_primary_slice(inputs,{},&other,nullptr).status==Q::InvalidInput && startup_snapshot(*player)==before &&
            update_owners_snapshot(other)==effects,"accepted parent work cannot be rebound to another update owner on retry or completion");}
    step=player->advance_state29_primary_slice(inputs,{},nullptr);
    expect(step.status==S::Completed && step.parent_reused && !step.parent && step.animation && step.animation->status==A::AlreadyAdvanced &&
        startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects,"same-loop primary retry reuses both accepted callbacks without reading unavailable observations");
    expect(player->consume_state29_update_after_primary_passes(inputs,{},&owners,nullptr).status==Q::RequiresPrimaryPasses && startup_snapshot(*player)==before,
        "strict queued callback cannot bypass unhandled secondary/tertiary player passes");
    awl::WorldMapPlayerState29SecondaryInput followup;
    auto passes=player->complete_state29_primary_passes(inputs,followup,{});
    expect(passes.status==F::SecondaryIncomplete && passes.secondary && passes.secondary->status==G::RequiresGuards &&
        !passes.tertiary && startup_snapshot(*player)==before,"missing secondary observations preserve accepted parent/model and pending work");
    followup.guards=awl::WorldMapPlayerUpdateGuards{0,{}};
    passes=player->complete_state29_primary_passes(inputs,followup,{});
    expect(passes.status==F::TertiaryIncomplete && passes.secondary && passes.secondary->status==G::SkippedByGuard &&
        passes.tertiary==G::RequiresGuards && startup_snapshot(*player)==before,
        "skipped second-pass callback still requires the independently observed third-pass guard");
    followup.guards=awl::WorldMapPlayerUpdateGuards{-1,0};followup.mode_word_60=0;
    followup.scene_mode_60=0;followup.secondary_byte_3f2=uint8_t{0};followup.item_present=false;
    passes=player->complete_state29_primary_passes(inputs,followup,{});
    expect(passes.status==F::TransitionRequired && passes.secondary && passes.secondary->requested_state==0 && !passes.tertiary &&
        startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects && !player->primary_loop()->followups_completed,
        "requested zero stops before state remaps/initializer and does not acknowledge tertiary or queued work");
    followup.mode_word_60=1;
    passes=player->complete_state29_primary_passes(inputs,followup,awl::WorldMapPlayerUpdateGuards{-1,{}});
    expect(passes.status==F::TertiaryIncomplete && passes.secondary && passes.secondary->status==G::RetainedState &&
        passes.tertiary==G::RequiresGuardWord58c && startup_snapshot(*player)==before,"late third-pass dependency preserves the accepted first-pass state and uncompleted receipt");
    passes=player->complete_state29_primary_passes(inputs,followup,awl::WorldMapPlayerUpdateGuards{-1,1});
    expect(passes.status==F::Completed && passes.secondary && passes.secondary->status==G::RetainedState && passes.tertiary==G::SkippedByGuard &&
        player->primary_loop()->followups_completed && player->state().state_1364==0x29 && player->state().control_word_1384==inputs.control()->dispatch_word &&
        update_owners_snapshot(owners)==effects,"retained-state second pass and guard-skipped third pass complete without model/status/queue changes");
    before=startup_snapshot(*player);
    expect(player->complete_state29_primary_passes(inputs,{},{}).status==F::AlreadyCompleted && startup_snapshot(*player)==before,
        "same-loop followup retry never reads replacement observations or repeats accepted callbacks");
    expect(player->consume_state29_update_after_primary_passes(inputs,{},&owners,nullptr).status==Q::CallbackIncomplete &&
        startup_snapshot(*player)==before && update_owners_snapshot(owners)==effects &&
        player->complete_state29_primary_passes(inputs,{},{}).status==F::AlreadyCompleted,
        "late actor dependency preserves accepted followups and pending queue without repeating earlier model or player passes");
    {auto other=owners;expect(player->consume_state29_update_after_primary_passes(inputs,{},&other,nullptr).status==Q::InvalidInput &&
        startup_snapshot(*player)==before,"stricter queue consumer retains accepted parent owner binding");}
    awl::WorldMapPlayerStartupQueuedActorInput actor;actor.actor.saved_byte_29a01=uint8_t{0};actor.actor.script.emplace();
    actor.actor.script->flag_width_4=1;actor.actor.script->flag_bytes={1};
    const auto accepted_model=holder_snapshot(player->model());
    const auto consumed=player->consume_state29_update_after_primary_passes(inputs,actor,&owners,nullptr);
    expect(consumed.status==Q::Completed && owners.queue.size()==1 && owners.queue[0].node_identity==900 &&
        holder_snapshot(player->model())==accepted_model && player->state().counter_1368==1,
        "checked primary-slice consumer completes a script-blocked owned front node and preserves later work");
    expect(providers.clock->advance(),"diagnostic fixture samples its next clock loop");
    before=startup_snapshot(*player);effects=update_owners_snapshot(owners);
    expect(player->advance_state29_primary_slice(inputs,input,&owners).status==S::RequiresControl && startup_snapshot(*player)==before,
        "next clock loop requires fresh capture before either ordered callback");
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"ordered fixture recaptures its next active loop");
    inputs.request_scene_mode(4,0);awl::WorldMapSceneModeCommit commit;
    expect(inputs.commit_scene_fields(0,uint8_t{1},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields &&
        inputs.dispatch(0x5001).status==awl::WorldMapDispatchStatus::Filtered && inputs.dispatch(0xf000).status==awl::WorldMapDispatchStatus::Ready,
        "actual primary priorities select blocked player group 5 and unblocked model-manager group 15");
    before=startup_snapshot(*player);effects=update_owners_snapshot(owners);const auto counter=player->state().counter_1368;
    step=player->advance_state29_primary_slice(inputs,{},nullptr);
    expect(step.status==S::Completed && step.parent && step.parent->status==P::SkippedByDispatchFilter && step.animation && step.animation->status==A::Advanced &&
        player->state().counter_1368==counter && update_owners_snapshot(owners)==effects && startup_snapshot(*player)!=before,
        "filtered parent skips every parent dependency while eligible model-manager progression still completes");
    expect(!player->primary_loop()->followups_completed,"new first-pass receipt never inherits prior-loop followup completion");
    passes=player->complete_state29_primary_passes(inputs,{},{});
    expect(passes.status==F::Completed && passes.filtered && !passes.secondary && !passes.tertiary && player->primary_loop()->followups_completed &&
        player->state().counter_1368==counter && update_owners_snapshot(owners)==effects,
        "filtered later player passes skip all callback observations while the manager's two empty callbacks need none");
    // An isolated later parent mutation invalidates ordered proof; the model
    // cannot be retroactively preceded by another parent callback in this loop.
    expect(player->advance_state29_camera({{2,0,0},0,{}}).status==awl::WorldMapPlayerState29CameraStatus::Advanced,"isolated camera diagnostic changes the accepted parent");
    before=startup_snapshot(*player);step=player->advance_state29_primary_slice(inputs,{},nullptr);
    expect(step.status==S::RequiresNextLoop && startup_snapshot(*player)==before && player->primary_loop() && !player->primary_loop()->valid,
        "diagnostic parent mutation preserves handled loop key and blocks retroactive ordered replay");
    expect(player->complete_state29_primary_passes(inputs,{},{}).status==F::RequiresPrimary && startup_snapshot(*player)==before,
        "invalidated first-pass proof also invalidates later callback completion");
    expect(providers.clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured &&
        player->dispatch_animation_for_loop(inputs).status==A::Advanced,"isolated model-manager facade advances its next eligible captured loop");
    before=startup_snapshot(*player);step=player->advance_state29_primary_slice(inputs,{},nullptr);
    expect(step.status==S::RequiresNextLoop && startup_snapshot(*player)==before,"ordered primary facade rejects executing a parent retroactively after independent model progression");
    std::cout<<"PLAYER_PRIMARY_SLICE ordered parent/model, secondary/tertiary callbacks, transition stop, filtered passes and retry preservation\n";
}

void state1_animation_decision_checks() {
    using S=awl::WorldMapPlayerState1AnimationStatus;const awl::WorldMapPlayerState1AnimationProfile profile{0,30,1.5f,1};
    expect(awl::prepare_world_map_player_state1_animation({}, {}, {}, {}, {}, {}, {}).status==S::RequiresControl,"state-one command requires control first");
    expect(awl::prepare_world_map_player_state1_animation(0x800, {}, {}, {}, {}, {}, {}).status==S::SkippedByControl,"paused callback skips every other command source");
    expect(awl::prepare_world_map_player_state1_animation(0, {}, {}, {}, {}, {}, {}).status==S::RequiresProfile,"active command requires retained numerical profile");
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<768;++seed){
        const uint32_t control=(seed%2)*0x800u;const float intensity=std::array<float,4>{0,-0.0f,.5f,1}[seed/2%4];
        const int32_t g680=std::array<int32_t,3>{-1,0,INT32_MIN}[seed/8%3],g58=std::array<int32_t,2>{0,9}[seed/24%2];
        const float speed=std::array<float,4>{0,.125f,1.5f,2}[seed/48%4];const uint32_t item=seed/192%2?0x4ffu:0,action=seed/384?7u:0;
        const bool command=!control && (intensity!=0 || (g680==-1 && g58==0));
        const auto r=awl::prepare_world_map_player_state1_animation(control,profile,intensity,speed,awl::WorldMapPlayerUpdateGuards{g680,g680==-1?std::optional<int32_t>(g58):std::nullopt},item,action);
        expect(r.status==(control?S::SkippedByControl:S::Prepared) && r.command.has_value()==command,"original state-one control/intensity/guard route selects a command only when reached");
        if(command)expect(r.command->selector==(intensity==0?0u:intensity==1?3u:2u) && r.command->variant==0x35 && r.command->item==item && r.command->action==action && r.command->mode==4 && r.command_speed==20*speed,"idle/walk/run command arguments and rate use the verified original order");
        if(!control)expect(r.final_speed==(intensity==0?1:20*speed),"idle final unit rate is applied even when guards skip its command");
        for(uint32_t v:{seed,control,actor_bits(intensity),uint32_t(g680),uint32_t(g58),actor_bits(speed),item,action,uint32_t(command),r.command?r.command->selector:UINT32_MAX,r.command_speed?actor_bits(*r.command_speed):UINT32_MAX,r.final_speed?actor_bits(*r.final_speed):UINT32_MAX})hash(digest,v);
    }
    expect(digest==0x8d95eb2a4eafeb85ull,"768 original42CA8 decision and rate cases agree within the hooked dependency boundary");
    std::cout<<"STATE1_ANIMATION_DECISION 768 "<<std::hex<<digest<<std::dec<<" supplied movement result; original comparison hooks only movement/command/speed/appearance\n";
    expect(awl::prepare_world_map_player_state1_animation(0,profile,0.0f,{},awl::WorldMapPlayerUpdateGuards{0,{}},{},{}).status==S::Prepared,"blocked idle route never reads speed/item/action");
    expect(awl::prepare_world_map_player_state1_animation(0,profile,0.0f,{},awl::WorldMapPlayerUpdateGuards{-1,{}},{},{}).status==S::RequiresGuardWord58c,"idle reads second guard only after clear first guard");
    expect(awl::prepare_world_map_player_state1_animation(0,profile,.5f,1.0f,{}, {}, {}).status==S::RequiresAction,"moving command reads action before item and does not inspect guards");
    expect(awl::prepare_world_map_player_state1_animation(0,profile,.5f,1.0f,{}, {}, 0).status==S::RequiresItem,"moving command requires reached item");
    auto catalog=holder_start_tables(true);std::shared_ptr<const awl::WorldMapPlayerStartAnimationTables> tables;
    expect(awl::decode_world_map_player_start_animation_tables(catalog.data(),catalog.size(),&tables)==awl::WorldMapPlayerStartAnimationStatus::Decoded,"all four invented normal selector rows decode");
    for(uint32_t selector=0;selector<4;++selector)for(uint32_t choice=0;choice<4;++choice){awl::WorldMapPlayerStartAnimationSelection selected;awl::WorldMapPlayerStartAnimationCommand command{selector,99,choice==1 || choice==3?1u:0u,choice==2?7u:0u,8};
        const std::optional<awl::WorldMapPlayerStartItemType> item=command.item?std::optional<awl::WorldMapPlayerStartItemType>{{command.item,uint8_t(choice==3?2:0)}}:std::nullopt;
        expect(tables->select(command,item,&selected)==awl::WorldMapPlayerStartAnimationStatus::Selected && selected.choice==choice && selected.descriptor_index==(selector==1?10+choice:14+(selector==0?0:selector==2?4:8)+choice),"all normal selectors share the original item/action classification and ignore unused command fields");awl::WorldMapActorAnimationDescriptor terminal;
        expect(tables->sequence_record(selected.descriptor.identity,selected.descriptor.identity+12,&terminal) && (terminal.word_0&1023)==1023 && tables->status_factors(selected.descriptor).rate.has_value(),"each selector retains checked terminal and independent rate metadata");}
    auto malformed=catalog;word(malformed,0x234,0x800289ac);const auto retained=tables;
    expect(awl::decode_world_map_player_start_animation_tables(malformed.data(),malformed.size(),&tables)==awl::WorldMapPlayerStartAnimationStatus::UnsupportedLayout && tables==retained,"wrong selector-zero branch witness rejects atomically");
    malformed=catalog;word(malformed,0x90,3);expect(awl::decode_world_map_player_start_animation_tables(malformed.data(),malformed.size(),&tables)==awl::WorldMapPlayerStartAnimationStatus::UnsupportedLayout && tables==retained,"partially mapped selector witness cannot count as an omitted row");
    auto initial=initial_input_dol(true);std::shared_ptr<const awl::WorldMapPlayerInitialAnimationInputs> initial_owner;
    expect(awl::decode_world_map_player_initial_animation_inputs(initial.data(),initial.size(),&initial_owner)==awl::WorldMapPlayerInitialAnimationStatus::Decoded && initial_owner->state1_animation_profile() && initial_owner->state1_animation_profile()->multiplier==8 && initial_owner->state1_animation_profile()->divisor==2,"state-one profile reads deliberately different synthetic constants");
    const auto old_initial=initial_owner;word(initial,0x374,0);expect(awl::decode_world_map_player_initial_animation_inputs(initial.data(),initial.size(),&initial_owner)==awl::WorldMapPlayerInitialAnimationStatus::UnsupportedLayout && initial_owner==old_initial,"invalid mapped animation profile preserves the retained owner");
    auto invalid=profile;invalid.divisor=0;expect(awl::prepare_world_map_player_state1_animation(0,invalid,1.0f,1.0f,{},0,0).status==S::UnsupportedNumerics,"invalid profile cannot divide by zero");
    using C=awl::WorldMapAnimationChannelStatus;awl::WorldMapAnimationChannelState channel;channel.elapsed_0=13;channel.duration_4=29;channel.target_8=10;channel.previous_c=20;channel.older_10=30;channel.blend_14.reset();channel.mode_18=7;
    awl::WorldMapAnimationPartialPlayback play;play.position_0=7.25f;play.rate_4=2;play.link_14=99;
    const std::vector<awl::WorldMapAnimationPartialPlaybackRecord> records{{10,play},{20,play},{30,play}};
    for(float rate:{-3.0f,-0.0f,0.0f,.125f,4.0f}){awl::WorldMapAnimationPartialChannelStep out;
        expect(awl::prepare_world_map_partial_animation_channel_rate(channel,records,rate,&out)==C::Prepared && out.after.elapsed_0==13 && out.after.duration_4==29 && !out.after.blend_14 && out.after.mode_18==7 && out.records_after[0].state.position_0==7.25f && actor_bits(out.records_after[0].state.rate_4)==actor_bits(rate) && !out.records_after[0].state.word_8 && !out.records_after[0].state.clip_10 && out.records_after[0].state.link_14==99 && out.records_after[1].state.rate_4==2,"rate setter changes only the target rate, preserving unknown fields, position and prior records");}
    awl::WorldMapAnimationPartialChannelStep out;channel.target_8=40;expect(awl::prepare_world_map_partial_animation_channel_rate(channel,records,1,&out)==C::RequiresPlaybackRecord && out.required_record==40,"rate setter requires the reached target record");
}
std::vector<uint8_t> state1_movement_terrain(){
    // Invented triangle at Y=4, large enough for the separate camera fixture.
    std::vector<uint8_t> b(92);word(b,0,0xe7e3f1f4);b[4]=1;b[6]=1;
    const auto half=[&](size_t at,int16_t v){b[at]=uint8_t(uint16_t(v)>>8);b[at+1]=uint8_t(v);};
    for(size_t i=0;i<3;++i){half(8+i*2,-200);half(14+i*2,900);}word(b,0x24,1);word(b,0x28,3);word(b,0x30,0x34);word(b,0x34,0x3c);half(0x3c,0x20);half(0x40,1);half(0x42,2);
    constexpr std::array<std::array<int16_t,3>,3> points{{{-200,4,-200},{900,4,-200},{-200,4,900}}};for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)half(0x44+i*8+j*2,points[i][j]);return b;
}
std::vector<uint8_t> state1_no_contact(){
    std::vector<uint8_t> b(252);word(b,0,0xf0f0e1ec);word(b,4,61);size_t group=0;
    for(size_t i=0;i<61;++i){const auto at=b.size();word(b,8+i*4,uint32_t(at));b.resize(at+(i==55?12:4));if(i==55){word(b,at,2);group=at;}}
    for(size_t slot=0;slot<2;++slot){const auto at=b.size();word(b,group+4+slot*4,uint32_t(at));b.resize(at+68);word(b,at+4,5);
        constexpr std::array<std::array<float,3>,5> square{{{800,0,800},{802,0,800},{802,0,802},{800,0,802},{800,0,800}}};for(size_t i=0;i<5;++i)for(size_t j=0;j<3;++j)word(b,at+8+i*12+j*4,actor_bits(square[i][j]));}return b;
}
uint64_t state1_motion_snapshot(const awl::WorldMapPlayerMovementState& motion,const awl::WorldMapSceneBucketRegistry& scene){
    uint64_t h=14695981039346656037ull;const auto vector=[&](const auto& v){for(float f:v)hash(h,actor_bits(f));};
    hash(h,uint32_t(motion.scene_identity));hash(h,uint32_t(motion.scene_identity>>32));hash(h,uint32_t(motion.pose.scene_type));vector(motion.pose.position);vector(motion.pose.heading);
    const auto& v=motion.steering;for(float f:{v.direction_x,v.direction_z,v.facing_x,v.facing_z,v.target_speed,v.current_speed,v.intensity})hash(h,actor_bits(f));
    hash(h,motion.camera.has_value());if(motion.camera){awl::WorldMapPlayerStartupCameraState c;c.camera=*motion.camera;const auto value=camera_snapshot(c);hash(h,uint32_t(value>>32));hash(h,uint32_t(value));}
    hash(h,motion.shared_contact.has_value());if(motion.shared_contact){vector(motion.shared_contact->recorded_prior);vector(motion.shared_contact->recorded_resolved);hash(h,motion.shared_contact->polygon_queries);hash(h,motion.shared_contact->state_requests);}
    hash(h,motion.camera_update.has_value());if(motion.camera_update){vector(motion.camera_update->update.final_matrix_50);vector(motion.camera_update->update.final_target);hash(h,actor_bits(motion.camera_update->movement_plane_constant_180));}
    for(int32_t i=-1;i<59;++i){const auto nodes=scene.snapshot(i);hash(h,uint32_t(nodes.size()));for(const auto& node:nodes){hash(h,uint32_t(node.identity));hash(h,uint32_t(node.scene_type));vector(node.position);if(node.context.facing_28)vector(*node.context.facing_28);}}
    return h;
}
std::vector<uint8_t> state1_probe_terrain(){
    // Invented two vertical triangles. Exponent one scales X=2/1 to 1/.5.
    std::vector<uint8_t> bytes(124);word(bytes,0,0xe7e3f1f4);bytes[4]=1;bytes[5]=1;
    const auto half=[&](size_t at,int16_t v){bytes[at]=uint8_t(uint16_t(v)>>8);bytes[at+1]=uint8_t(v);};
    for(size_t i=0;i<3;++i){half(8+i*2,-16);half(14+i*2,16);}word(bytes,0x24,2);word(bytes,0x28,6);word(bytes,0x30,0x34);word(bytes,0x34,0x44);
    constexpr std::array<std::array<std::array<int16_t,3>,3>,2> triangles{{{{{2,0,0},{2,4,0},{2,0,8}}},{{{1,0,0},{1,4,0},{1,0,8}}}}};
    for(size_t i=0;i<2;++i){half(0x3c+i*8,int16_t(i?2:0xf001));for(size_t j=0;j<3;++j){half(0x3e+i*8+j*2,int16_t(i*3+j));for(size_t k=0;k<3;++k)half(0x4c+(i*3+j)*8+k*2,triangles[i][j][k]);}}return bytes;
}
void state1_terrain_probe_checks(){
    using T=awl::WorldMapPlayerTerrainProbeStatus;auto bytes=state1_probe_terrain();awl::WorldMapPlayerTerrainProbeAsset asset;
    expect(asset.load_from_bytes(bytes.data(),bytes.size()),"invented slot-five geometry loads with validated scaled topology");uint64_t digest=14695981039346656037ull;
    constexpr float x[]{-.25f,0,.25f,.5f,.75f,1,1.25f,2},z[]{-.25f,0,.25f,1,2,3.75f,4,4.25f};
    for(uint32_t seed=0;seed<256;++seed){const std::array<float,3> start{x[seed%8],7,z[seed/8%8]},end{start[0]+(seed/64%2?1.0f:-1.0f),-9,start[2]+(seed/128?.25f:0.0f)};
        awl::CollisionDirectedTriangleSample sample;expect(awl::query_type1_collision_directed_triangle(bytes.data(),bytes.size(),start,end,&sample),"valid directed query succeeds for hits and misses");
        hash(digest,seed);for(float f:start)hash(digest,actor_bits(f));for(float f:end)hash(digest,actor_bits(f));hash(digest,sample.found);hash(digest,sample.surface_flags);for(const auto& v:sample.triangle)for(float f:v)hash(digest,actor_bits(f));}
    expect(digest==0x089525ec3a2e1dd1ull,"256 original1938C8/C02C bound-leaf cases agree for first serialized crossing, scale, endpoints, flags and full triangle outputs");
    const awl::WorldMapPlayerScenePose pose{1,{.25f,7,1},{1,0,0}};auto hit=asset.query(pose);
    expect(hit.status==T::Prepared && hit.code==1 && hit.triangle->triangle_index==0 && hit.triangle->edge_index==1,"farther first triangle wins over the nearer later triangle; mask strips high flag bits");
    auto oblique=pose;oblique.heading={1,0,2};const auto rejected_angle=asset.query(oblique);
    expect(rejected_angle.status==T::Prepared && rejected_angle.code==7 && rejected_angle.triangle->found,"a crossing below the 45-degree normal-dot threshold remains no-event code seven");
    oblique.heading={1,0,.5f};expect(asset.query(oblique).code==1,"a crossing above the normal-dot threshold returns the masked surface code");
    auto empty=bytes;word(empty,0x24,0);awl::WorldMapPlayerTerrainProbeAsset empty_asset;
    expect(empty_asset.load_from_bytes(empty.data(),empty.size()) && empty_asset.query(pose).status==T::Prepared && empty_asset.query(pose).code==7,"a validated empty leaf is an original directed-probe miss rather than a fabricated surface event");
    auto miss=pose;miss.heading={0,2,0};expect(asset.query(miss).status==T::Prepared && asset.query(miss).code==7 && !asset.query(miss).triangle,"zero XZ heading skips normalization and selected geometry");
    miss.heading={1,0,0};miss.position[0]=0;expect(asset.query(miss).status==T::Prepared && asset.query(miss).code==2 && asset.query(miss).triangle->triangle_index==1,"movement endpoint on first triangle is excluded, allowing the later interior crossing");
    auto invalid=bytes;invalid[4]=0;expect(!asset.load_from_bytes(invalid.data(),invalid.size()) && !asset.loaded() && asset.query(pose).status==T::RequiresAsset,"unsupported reload clears the previous borrowed slot-five payload");
    expect(asset.load_from_bytes(bytes.data(),bytes.size()),"valid slot-five provider reloads after failure");
    auto bad_pose=pose;bad_pose.heading[0]=std::numeric_limits<float>::quiet_NaN();expect(asset.query(bad_pose).status==T::InvalidInput,"nonfinite heading cannot become a no-contact success");
    const int prior=std::fegetround();std::fesetround(FE_DOWNWARD);expect(asset.query(pose).status==T::UnsupportedNumerics,"terrain normalization rejects unsupported rounding");std::fesetround(prior);
    awl::WorldMapPlayerStatusState healthy;expect(awl::construct_world_map_player_status(100000,&healthy)==awl::WorldMapPlayerStatusCode::Prepared,"terrain handler boundary owns healthy status");awl::WorldMapPlayerState1SecondaryQuery query;
    query.control=0;query.status=&healthy;query.action=0;query.phase_raw=100000;for(auto& f:query.factors.globals)f=100u;query.pose=pose;query.dependencies.terrain_slot_present=true;query.dependencies.terrain=&asset;
    expect(awl::prepare_world_map_player_state1_secondary(query).status==awl::WorldMapPlayerState1SecondaryStatus::RequiresTerrainHandler,"reached terrain code one stops before controller1504 and does not inspect missing PAD");
    std::cout<<"STATE1_DIRECTED_TRIANGLE 256 "<<std::hex<<digest<<std::dec<<" first crossing, independent full triangle comparison\n";
}
void state1_secondary_route_checks(){
    using S=awl::WorldMapPlayerState1SecondaryStatus;awl::WorldMapPlayerStatusState status;
    expect(awl::construct_world_map_player_status(100000u,&status)==awl::WorldMapPlayerStatusCode::Prepared,"second route fixture owns a fresh status");
    awl::WorldMapPlayerState1SecondaryQuery query;query.control=0;query.status=&status;query.phase_raw=100000u;query.dependencies.terrain_slot_present=false;
    for(auto& factor:query.factors.globals)factor=100u;
    constexpr uint32_t buttons[]{0,0x1000,0x200,0x400,0x10,0x02000000,0x1600,0x02000010,0x02000400};uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<1296;++seed){const uint32_t pad=buttons[seed%9],item=std::array<uint32_t,3>{0,1,UINT32_MAX}[seed/9%3],action=std::array<uint32_t,3>{0,7,UINT32_MAX}[seed/27%3];
        const int32_t scene=int32_t(seed/81%2),category=int32_t(seed/162%2+1);const uint8_t secondary=uint8_t(seed/324%2?255:0);const bool inside=seed/648%2!=0;
        query.pressed=pad;query.item=item;query.action=action;query.manager_scene_type=scene;query.dependencies.secondary_byte_3f1=secondary;
        query.pose=awl::WorldMapPlayerScenePose{category,{inside?175.0f:120.0f,0,inside?115.0f:168.0f},{0,0,1}};
        const auto r=awl::prepare_world_map_player_state1_secondary(query);
        expect((r.status==S::Completed || r.status==S::TransitionRequired) && r.conditions && r.conditions->after,"complete no-terrain observations finish the body or stop before a reached effect");
        for(uint32_t v:{seed,pad,item,action,uint32_t(scene),uint32_t(category),uint32_t(secondary),uint32_t(inside),r.requested_state.value_or(UINT32_MAX)})hash(digest,v);
        expect(r.reset_required==(r.requested_state==0x27u) && r.action_setup_required==(r.requested_state==7u || r.requested_state==4u) && r.scene_cleanup_required==(r.requested_state==2u),"request diagnostics retain reset/action/cleanup prerequisites without applying them");
    }
    expect(digest==0x68ab625af78ff1a9ull,"1296 actual second PAD/region routes match, including button priority and nonzero-action early returns");
    query={};query.control=0x800;expect(awl::prepare_world_map_player_state1_secondary(query).status==S::Completed,"control800 skips every body dependency");
    query.control=0;query.status=&status;query.action=0;query.phase_raw=100000u;for(auto& factor:query.factors.globals)factor=100u;
    expect(awl::prepare_world_map_player_state1_secondary(query).status==S::RequiresTerrainSlot,"healthy prefix requires an explicit slot-five presence observation");query.dependencies.terrain_slot_present=true;
    expect(awl::prepare_world_map_player_state1_secondary(query).status==S::RequiresTerrainAsset,"present slot cannot be silently treated as absent");query.dependencies.terrain_slot_present=false;
    expect(awl::prepare_world_map_player_state1_secondary(query).status==S::RequiresPad,"checked absent slot skips pose but requires the reached pressed word");query.pressed=0;
    expect(awl::prepare_world_map_player_state1_secondary(query).status==S::Completed,"no buttons skips item/scene/secondary/pose sources");
    query.pressed=0x400;expect(awl::prepare_world_map_player_state1_secondary(query).status==S::RequiresItem,"reached action button requires held item before choosing request");
    std::cout<<"STATE1_SECONDARY_BUTTON_ROUTES 1296 "<<std::hex<<digest<<std::dec<<'\n';
}
void state1_normal_owner_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapUpdateInputs& inputs,awl::WorldMapPlayerUpdateOwners& owners,bool local,bool allocations,
    const awl::WorldMapPlayerContextUpdateSources* context_sources,awl::WorldMapPlayerActorQueue& render,const awl::WorldMapPlayerStartupQueuedActorInput& frame_actor){
    using P=awl::WorldMapPlayerUpdateStatus;using S=awl::WorldMapPlayerPrimarySliceStatus;using Second=awl::WorldMapPlayerState1SecondaryStatus;using Third=awl::WorldMapPlayerState0ThirdStatus;using Q=awl::WorldMapPlayerStartupQueueStatus;
    owners.status.reset();expect(player.initialize_fresh_status(inputs,&owners)==awl::WorldMapPlayerStatusCode::Prepared,"normal second fixture constructs status at its explicit current phase after the preceding appearance phase jump");owners.status->word_20=75000;
    auto terrain=state1_movement_terrain(),polygons=state1_no_contact();std::vector<uint8_t> statics;awl::WorldMapTriggerAsset trigger;
    awl::WorldMapPlayerTerrainProbeAsset probe;auto probe_bytes=state1_probe_terrain();
    expect(local?probe.load():probe.load_from_bytes(probe_bytes.data(),probe_bytes.size()),"normal second owns the actual or invented slot-five resource");
    std::array<float,3> position{2,4,2};
    if(local){const auto read=[](const char* logical){char path[1024];expect(awl::filesystem_resolve_path(logical,path,sizeof(path)),"movement asset resolves through existing mount");std::ifstream f(path,std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};};
        terrain=read("/files/jimen-move.col");statics=read("/files/mapobj.col");expect(trigger.load(),"actual state-one trigger polygons load");awl::CollisionSurfaceSample ground;
        expect(awl::sample_type1_collision_surface(terrain.data(),terrain.size(),120,168,&ground),"actual state-one spawn is contained");position={120,ground.height,168};
    }else expect(trigger.load_from_bytes(polygons.data(),polygons.size()),"invented distant no-contact polygons load");
    awl::WorldMapPlayerMovementState movement;movement.scene_identity=70;movement.pose={1,position,{0,0,1}};movement.steering.facing_z=1;
    awl::WorldMapSceneBucketRegistry scene;awl::WorldMapScenePositionUpdate placed;expect(scene.register_object(70,1,position) && scene.update_pose(70,1,position,movement.pose.heading,&placed),"movement fixture registers explicit category-one pose");
    awl::WorldMapMovementQuery dependencies;dependencies.collision.terrain_data=terrain.data();dependencies.collision.terrain_size=terrain.size();dependencies.collision.static_data=statics.empty()?nullptr:statics.data();dependencies.collision.static_size=statics.size();
    awl::WorldMapPlayerMovementInput motion;motion.dependencies=&dependencies;motion.triggers=&trigger;motion.secondary_byte_3f1=uint8_t{0};
    // A bogus caller camera/height sampler cannot override the retained owner.
    movement.camera.emplace();movement.camera->target.camera.yaw=std::numeric_limits<float>::quiet_NaN();
    awl::WorldMapPlayerState1ParentInput input;input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.scene_child_word_4=0;input.movement=&motion;input.item_144=0;input.action_148=0;input.saved_subobject_byte_14c=uint8_t{0};
    auto before=startup_snapshot(player),effects=update_owners_snapshot(owners),moving=state1_motion_snapshot(movement,scene);
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(owners)==effects && state1_motion_snapshot(movement,scene)==moving;};
    const auto stopped=[&](P expected){const auto r=player.dispatch_state1_parent(inputs,input,&owners,&movement,&scene);if(r.status!=expected)std::cerr<<"state1 stop actual "<<int(r.status)<<" expected "<<int(expected)<<" movement "<<(r.movement?int(r.movement->status):-1)<<'\n';expect(r.status==expected && preserved(),"state-one late failure preserves animation, movement, registry, camera, parent and queue");return r;};
    expect(player.dispatch_state1_parent(inputs,input,&owners,nullptr,&scene).status==P::RequiresMovementOwners && preserved(),"active state-one requires both explicit movement owners");
    stopped(P::MovementIncomplete);inputs.observe_pad_sample({true,0,80});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"state-one movement consumes fresh owned PAD");
    input.action_148.reset();expect(stopped(P::RequiresAnimationCommand).command->status==awl::WorldMapPlayerState1AnimationStatus::RequiresAction,"movement staging cannot publish before action source arrives");input.action_148=0;
    input.saved_subobject_byte_14c.reset();stopped(P::AppearanceIncomplete);input.saved_subobject_byte_14c=uint8_t{0};
    const auto old_status=owners.status;owners.status.reset();effects=update_owners_snapshot(owners);stopped(P::AppearanceIncomplete);owners.status=old_status;effects=update_owners_snapshot(owners);
    input.status_factors_override.emplace();stopped(P::RequiresStatusUpdate);input.status_factors_override.reset();
    const auto old_position=owners.scene_one_position_240;owners.scene_one_position_240.reset();effects=update_owners_snapshot(owners);stopped(P::RequiresScenePositionOwner);owners.scene_one_position_240=old_position;effects=update_owners_snapshot(owners);
#if !defined(_DEBUG)
    if(allocations){size_t rejected=0;bool completed=false;for(size_t n=0;n<512;++n){const auto live=allocation_probe::live;allocation_probe::remaining=n;allocation_probe::enabled=true;const auto r=player.dispatch_state1_parent(inputs,input,&owners,&movement,&scene);allocation_probe::enabled=false;
        if(r.status==P::Advanced){const auto& channel=player.model().primary_channel();const auto target=std::find_if(channel.records().begin(),channel.records().end(),[&](const auto& record){return record.identity==channel.state().target_8;});
            expect(target!=channel.records().end() && target->state.rate_4==player.model().state().speed_8 && target->state.position_0==0 && player.model().primary().model().partial_playback().rate_4==1,"direct parent speed setter follows setup/model copy and changes target rate only before F000");completed=true;expect(n>8,"parent allocation sweep reaches motion, animation, appearance and queue staging");break;}++rejected;expect(r.status==P::AllocationFailure && preserved() && allocation_probe::live==live,"every reached state-one parent allocation failure rolls back all owners and frees staging");}
        expect(completed,"parent allocation sweep reaches successful publication");
        std::cout<<"PLAYER_STATE1_PARENT_ALLOCATION_STOPS "<<rejected<<'\n';
        // The successful direct parent above has no loop receipt. Use a fresh
        // loop for the ordered facade and compare against that accepted pose.
        expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"allocation sweep completion advances a fresh ordered-loop fixture");inputs.observe_pad_sample({true,0,80});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"fresh movement PAD captures after sweep");
    }
#else
    (void)allocations;
#endif
    std::optional<awl::WorldMapPlayerState1ParentResult> retained_parent;
#if !defined(_DEBUG)
    if(allocations){bool reached=false;for(size_t n=0;n<512;++n){const auto a=startup_snapshot(player),b=update_owners_snapshot(owners),c=state1_motion_snapshot(movement,scene),live=allocation_probe::live;
        allocation_probe::remaining=n;allocation_probe::enabled=true;auto partial=player.advance_state1_primary_slice(inputs,input,&owners,&movement,&scene);allocation_probe::enabled=false;
        if(partial.status==S::AnimationIncomplete){retained_parent=partial.parent;expect(retained_parent && retained_parent->status==P::Advanced && player.primary_loop()->state1_parent_completed,"late F000 failure retains the accepted state-one parent exactly once");reached=true;break;}
        expect(partial.status==S::ParentIncomplete && partial.parent->status==P::AllocationFailure && startup_snapshot(player)==a && update_owners_snapshot(owners)==b && state1_motion_snapshot(movement,scene)==c && allocation_probe::live==live,"ordered first allocation failures preserve the unaccepted prefix");}
        expect(reached,"allocation sweep reaches the separately staged F000 callback");
        before=startup_snapshot(player);effects=update_owners_snapshot(owners);moving=state1_motion_snapshot(movement,scene);const auto live=allocation_probe::live;
        allocation_probe::remaining=0;allocation_probe::enabled=true;const auto retry=player.advance_state1_primary_slice(inputs,{},nullptr,nullptr,nullptr);allocation_probe::enabled=false;
        expect(retry.status==S::AnimationIncomplete && retry.parent_reused && preserved() && allocation_probe::live==live,"failed F000 retry preserves accepted movement/status/queue and releases staging");
    }
#endif
    const auto prior_position=movement.pose.position;const auto raw=inputs.game_clock()->state().raw_time,loop=inputs.game_clock()->state().loop_count;
    auto accepted=player.advance_state1_primary_slice(inputs,input,&owners,&movement,&scene);
    if(retained_parent){expect(accepted.parent_reused && !accepted.parent,"successful F000 retry does not rerun first");accepted.parent=retained_parent;}
    if(accepted.status!=S::Completed)std::cerr<<"state1 primary stop "<<int(accepted.status)<<" parent "<<(accepted.parent?int(accepted.parent->status):-1)<<" anim "<<(accepted.animation?int(accepted.animation->status):-1)<<'\n';
    expect(accepted.status==S::Completed && accepted.parent && accepted.parent->movement && accepted.parent->animation && player.primary_loop()->state1_parent_completed && !player.primary_loop()->state1_secondary_completed,"owned state-one first callback and F000 progression complete in order");if(accepted.status!=S::Completed)return;
    expect((retained_parent || movement.pose.position!=prior_position) && scene.object(70)->position==movement.pose.position && owners.saved_pose.position==movement.pose.position && player.state().target_14ac==accepted.parent->movement->metadata && player.state().direction_14b4==accepted.parent->movement->direction && player.camera()->camera.target.camera.position==movement.camera->target.camera.position,"motion, scene, queue pose, contact outputs and owned camera publish coherently");
    expect(player.model().state().speed_8==accepted.parent->command->final_speed && inputs.game_clock()->state().raw_time==raw && inputs.game_clock()->state().loop_count==loop,"animation rate derives accepted movement without advancing time");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);moving=state1_motion_snapshot(movement,scene);
    expect(player.advance_state1_primary_slice(inputs,{},nullptr,nullptr,nullptr).parent_reused && preserved(),"same-loop retry reuses accepted motion and animation without observations");
    {auto foreign_scene=scene;auto foreign_owners=owners;expect(player.advance_state1_primary_slice(inputs,{},&owners,&movement,&foreign_scene).status==S::InvalidInput && player.advance_state1_primary_slice(inputs,{},&foreign_owners,&movement,&scene).status==S::InvalidInput && preserved(),"same-loop receipt cannot rebind registry or update owners");}
    {auto foreign=movement;expect(player.advance_state1_primary_slice(inputs,{},&owners,&foreign,&scene).status==S::InvalidInput && preserved(),"same-loop receipt cannot rebind its movement owner");}
    expect(player.advance_state1_secondary(inputs,{} )==Second::RequiresGuards && preserved(),"normal second requires its reached guards");
    expect(player.advance_state1_secondary(inputs,awl::WorldMapPlayerUpdateGuards{-1,0})==Second::RequiresBody && preserved(),"minimal second entry stops until active dependency owners are supplied");
    expect(player.advance_state1_third(inputs,{},{}).status==Third::RequiresSecondary && player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::RequiresThird && preserved(),"active second stop cannot be bypassed through third or queue completion");
    awl::WorldMapPlayerState1SecondaryInput secondary;secondary.guards=awl::WorldMapPlayerUpdateGuards{-1,0};secondary.terrain_slot_present=true;secondary.terrain=&probe;secondary.secondary_byte_3f1=uint8_t{0};
    const auto failed_second=[&](Second wanted){const auto r=player.advance_state1_secondary(inputs,secondary,&owners,&movement);
        if(r.status!=wanted)std::cerr<<"state1 secondary stop actual "<<int(r.status)<<" expected "<<int(wanted)<<" condition "<<(r.conditions?int(r.conditions->status):-1)<<" gate "<<(r.conditions && r.conditions->gate?int(*r.conditions->gate):-1)<<'\n';
        expect(r.status==wanted && preserved(),"second failure preserves accepted first/model, proposed latches, camera, pose and queues");return r;};
    expect(player.advance_state1_secondary(inputs,secondary,nullptr,&movement).status==Second::RequiresUpdateOwners && preserved(),"active second requires the accepted status/update owner");
    {auto foreign=owners;expect(player.advance_state1_secondary(inputs,secondary,&foreign,&movement).status==Second::InvalidInput && preserved(),"active second rejects a replacement update owner");}
    {const auto saved=owners.status;owners.status.reset();effects=update_owners_snapshot(owners);failed_second(Second::RequiresStatus);owners.status=saved;effects=update_owners_snapshot(owners);}
    {auto foreign=movement;expect(player.advance_state1_secondary(inputs,secondary,&owners,&foreign).status==Second::InvalidInput && preserved(),"active second rejects a replacement movement owner");}
    secondary.condition_factors_override.emplace();failed_second(Second::ConditionsIncomplete);secondary.condition_factors_override.reset();
    secondary.terrain_slot_present.reset();failed_second(Second::RequiresTerrainSlot);secondary.terrain_slot_present=true;
    secondary.terrain=nullptr;failed_second(Second::RequiresTerrainAsset);secondary.terrain=&probe;
    const auto status_before=*owners.status;owners.status->word_12c=*inputs.sources().phase->raw_word_0;effects=update_owners_snapshot(owners);
    expect(failed_second(Second::TransitionRequired).requested_state==0x21u,"reached deadline selects the original state request without applying its initializer");*owners.status=status_before;effects=update_owners_snapshot(owners);
    owners.status->byte_158=uint8_t{255};owners.status->byte_159=uint8_t{255};owners.status->byte_15a=uint8_t{255};owners.status->byte_15b=uint8_t{255};effects=update_owners_snapshot(owners);
    secondary.terrain=nullptr;failed_second(Second::RequiresTerrainAsset);secondary.terrain=&probe;
#if !defined(_DEBUG)
    if(allocations){bool completed=false;size_t rejected=0;for(size_t n=0;n<64;++n){const auto live=allocation_probe::live;allocation_probe::remaining=n;allocation_probe::enabled=true;const auto r=player.advance_state1_secondary(inputs,secondary,&owners,&movement);allocation_probe::enabled=false;
        if(r.status==Second::Completed){completed=true;break;}++rejected;expect(r.status==Second::AllocationFailure && preserved() && allocation_probe::live==live,"every terrain validation allocation failure preserves staged condition clears and the first/model receipts");}
        expect(completed && rejected>0,"second allocation sweep reaches complete latch publication");std::cout<<"PLAYER_STATE1_SECOND_ALLOCATION_STOPS "<<rejected<<'\n';}
#endif
    const auto complete_loop=[&](){
        const auto second=player.advance_state1_secondary(inputs,secondary,&owners,&movement);
        expect(second.status==Second::Completed || second.status==Second::AlreadyCompleted,"no-event second accepts actual status and slot-five geometry");
        if(second.status!=Second::Completed && second.status!=Second::AlreadyCompleted){std::cerr<<"state1 active loop second stop "<<int(second.status)<<'\n';return false;}
        expect(owners.status->byte_158==uint8_t{0} && owners.status->byte_159==uint8_t{0} && owners.status->byte_15a==uint8_t{0} && owners.status->byte_15b==uint8_t{0},"healthy conditions publish original hysteresis resets only on complete second acceptance");
        awl::WorldMapPlayerState0ThirdInput third;third.guards=awl::WorldMapPlayerUpdateGuards{0,{}};
        if(context_sources){third.guards=awl::WorldMapPlayerUpdateGuards{-1,0};third.context.emplace();third.context->scene_type=movement.pose.scene_type;third.context->current_key=17;third.context->position=movement.pose.position;
            third.context->action=0;third.context->facing=movement.pose.heading;third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};third.context->tracked_14ac=player.state().target_14ac;
            third.context->scene_predicate_1728e4=uint8_t{0};third.context->secondary_byte_3f1=uint8_t{0};
            third.context->label_observations.terrain_clear={true,true};third.context->label_observations.secondary_ground_available=true;}
        const auto third_step=player.advance_state1_third(inputs,context_sources?*context_sources:awl::WorldMapPlayerContextUpdateSources{},third);
        expect(third_step.status==(context_sources?Third::Prepared:Third::SkippedByGuard),"supported shared third follows the accepted normal second");
        if(third_step.status!=Third::Prepared && third_step.status!=Third::SkippedByGuard){std::cerr<<"state1 active loop third stop "<<int(third_step.status)<<" context "<<(third_step.context?int(third_step.context->status):-1)<<'\n';return false;}
        awl::WorldMapPlayerStartupQueuedActorInput actor;
        if(context_sources){actor=frame_actor;actor.actor.script->flag_bytes[0]=0;actor.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{movement.pose.position,movement.pose.heading};
            render.erase(std::remove_if(render.begin(),render.end(),[&](const auto& entry){return entry.node_identity==player.render_node_identity() || entry.node_identity==player.effect_node_identity();}),render.end());
        }else{actor.actor.saved_byte_29a01=uint8_t{0};actor.actor.script.emplace();actor.actor.script->flag_width_4=1;actor.actor.script->flag_bytes={1};}
        const auto queued=player.consume_state1_update_after_third(inputs,actor,&owners,context_sources?&render:nullptr);
        expect(queued.status==Q::Completed && owners.queue.size()==1 && owners.queue.front().node_identity==900 && player.primary_loop()->queued_completed,"normal CPU loop finishes only its owned update node after first/model/second/third");
        if(context_sources)expect(queued.callback && queued.callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::Completed && player.model().primary().cpu().frame()->skin_executed,"normal local loop publishes a skinned CPU player frame at its accepted movement pose");
        return queued.status==Q::Completed;
    };
    expect(complete_loop(),"first supported normal active second/third/queue completes");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);moving=state1_motion_snapshot(movement,scene);
    expect(player.advance_state1_secondary(inputs,awl::WorldMapPlayerState1SecondaryInput{},nullptr,nullptr).status==Second::AlreadyCompleted && player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::AlreadyCompleted && preserved(),"completed second and queued retries need no replacement observations and cannot consume foreign work");
    {auto foreign=owners;expect(player.advance_state1_secondary(inputs,secondary,&foreign,&movement).status==Second::InvalidInput && preserved(),"completed second retry cannot acknowledge a replacement status owner");}
    uint32_t selectors=1u<<accepted.parent->command->command->selector;size_t unchanged=0;const auto started_position=movement.pose.position;
    for(int tick=0;tick<16;++tick){expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"normal movement sequence advances explicit owned loop");
        inputs.observe_pad_sample({true,0,int8_t(tick<7?80:0)});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"normal sequence captures hold/release PAD");
        const auto next=player.advance_state1_primary_slice(inputs,input,&owners,&movement,&scene);
        expect(next.status==S::Completed && next.parent && next.parent->command && next.parent->command->command && next.parent->animation,"hold/release sequence completes supported normal first and F000");if(next.status!=S::Completed)break;
        selectors|=1u<<next.parent->command->command->selector;if(next.parent->animation->status==awl::WorldMapPlayerStartAnimationStatus::Unchanged)++unchanged;
        expect(player.model().state().speed_8==next.parent->command->final_speed && owners.saved_pose.position==movement.pose.position && scene.object(70)->position==movement.pose.position,"sequence retains motion/pose/animation rate coherence across descriptor changes and equal-base starts");
        if(tick>=14)expect(next.parent->movement->intensity==0.0f && movement.steering.current_speed==0,"released normal input reaches idle after deceleration");
        expect(complete_loop(),"held/released native CPU ticks complete normal second, shared third and queued frames");
    }
    expect(selectors==0xdu && unchanged>3 && movement.pose.position!=started_position,"normal sequence reaches walk, run and idle, including retained-base commands");
    expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"request scenario advances a fresh active loop");
    inputs.observe_pad_sample({true,awl::pad_button_mask(awl::PadButton::Start),0});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"request scenario captures an actual owned start press");
    expect(player.advance_state1_primary_slice(inputs,input,&owners,&movement,&scene).status==S::Completed,"button request follows its accepted first/model");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);moving=state1_motion_snapshot(movement,scene);
    const auto request=failed_second(Second::TransitionRequired);expect(request.requested_state==0x27u && request.reset_required && !player.primary_loop()->state1_secondary_completed,"owned pressed input requests the original reset/state path without consuming it");
    expect(player.advance_state1_third(inputs,{},{}).status==Third::RequiresSecondary && player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::RequiresThird && preserved(),"button request stop cannot bypass incomplete second into third/queue");
    // Next paused loop exercises the fully supported normal-loop skip path.
    auto sources=inputs.sources();sources.phase->byte_a=uint8_t{1};inputs.observe_control_sources(sources);expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"paused next loop captures control800");
    awl::WorldMapPlayerState1ParentInput paused;paused.guards=awl::WorldMapPlayerUpdateGuards{-1,0};paused.scene_child_word_4=0;
    auto step=player.advance_state1_primary_slice(inputs,paused,&owners,&movement,&scene);expect(step.status==S::Completed && step.parent && !step.parent->movement && !step.parent->command && step.parent->status==P::Advanced,"paused first skips PAD, movement, item, animation command and appearance/status dependencies while retaining normal pose/enqueue");
    expect(player.advance_state1_secondary(inputs,awl::WorldMapPlayerUpdateGuards{-1,0})==Second::Completed && player.advance_state1_third(inputs,{},awl::WorldMapPlayerState0ThirdInput{awl::WorldMapPlayerUpdateGuards{0,{}}}).status==Third::SkippedByGuard,"paused second and guard-skipped shared third complete under their own receipts");
    awl::WorldMapPlayerStartupQueuedActorInput actor;actor.actor.saved_byte_29a01=uint8_t{0};actor.actor.script.emplace();actor.actor.script->flag_width_4=1;actor.actor.script->flag_bytes={1};
    expect(player.consume_state1_update_after_third(inputs,actor,&owners,nullptr).status==Q::Completed && owners.queue.size()==1 && owners.queue.front().node_identity==900,"paused normal loop completes only its script-blocked owned queue node");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);moving=state1_motion_snapshot(movement,scene);
    expect(player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::AlreadyCompleted && preserved(),"paused queued retry preserves foreign work");
    expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"filtered state-one loop captures control");
    inputs.request_scene_mode(0,0);awl::WorldMapSceneModeCommit commit;expect(inputs.commit_scene_fields(0,{},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields,"state-one group-five filter commits");
    const auto filtered=player.advance_state1_primary_slice(inputs,{},nullptr,nullptr,nullptr);
    expect(filtered.status==S::Completed && filtered.parent && filtered.parent->status==P::SkippedByDispatchFilter && filtered.animation,"filtered first reads no callback or movement owners and still runs F000");
    expect(player.advance_state1_secondary(inputs,{})==Second::Filtered && player.advance_state1_third(inputs,{},{}).status==Third::Filtered && player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::UnsupportedNode,"filtered second/third skips cannot consume a remaining foreign queue node");
    std::cout<<(local?"LOCAL_":"")<<"PLAYER_STATE1_NORMAL 17 active CPU first/model/second/third/queue ticks; retained requests/terrain effects stop; paused/filtered completion; supplied context/visibility/scene, CPU only\n";
}
bool state0_movement_second_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapUpdateInputs& inputs,
    awl::WorldMapPlayerUpdateOwners& owners,awl::WorldMapPlayerActorQueue& render,
    const awl::WorldMapPlayerStartupQueuedActorInput& frame_actor,
    const awl::WorldMapPlayerContextUpdateSources* context_sources,bool allocations,
    awl::WorldMapPlayerMovementState& movement,awl::WorldMapSceneBucketRegistry& scene){
    using S=awl::WorldMapPlayerState0SecondaryStatus;using B=awl::WorldMapPlayerState1SecondaryStatus;
    using Third=awl::WorldMapPlayerState0ThirdStatus;using Q=awl::WorldMapPlayerStartupQueueStatus;
    auto terrain=state1_movement_terrain(),polygons=state1_no_contact();std::vector<uint8_t> statics;
    awl::WorldMapTriggerAsset trigger;awl::WorldMapPlayerTerrainProbeAsset probe;auto probe_bytes=state1_probe_terrain();
    const bool local=context_sources!=nullptr;std::array<float,3> position{2,4,2};
    expect(local?probe.load():probe.load_from_bytes(probe_bytes.data(),probe_bytes.size()),"state-zero second owns its separate slot-five asset");
    if(local){const auto read=[](const char* logical){char path[1024];expect(awl::filesystem_resolve_path(logical,path,sizeof(path)),"state-zero movement asset resolves");std::ifstream f(path,std::ios::binary);return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};};
        terrain=read("/files/jimen-move.col");statics=read("/files/mapobj.col");expect(trigger.load(),"state-zero actual trigger polygons load");awl::CollisionSurfaceSample ground;
        expect(awl::sample_type1_collision_surface(terrain.data(),terrain.size(),120,168,&ground),"state-zero actual spawn is contained");position={120,ground.height,168};
    }else expect(trigger.load_from_bytes(polygons.data(),polygons.size()),"state-zero distant invented trigger polygons load");
    owners.status.reset();expect(player.initialize_fresh_status(inputs,&owners)==awl::WorldMapPlayerStatusCode::Prepared,"state-zero second fixture explicitly constructs fresh status at its current phase");owners.status->word_20=75000;
    movement.scene_identity=71;movement.pose={1,position,{0,0,1}};movement.steering.facing_z=1;
    movement.camera.emplace();movement.camera->target.camera.yaw=std::numeric_limits<float>::quiet_NaN();
    awl::WorldMapScenePositionUpdate placed;
    expect(scene.register_object(71,1,position) && scene.update_pose(71,1,position,movement.pose.heading,&placed),"state-zero fixture registers its explicit movement pose");
    awl::WorldMapMovementQuery dependencies;dependencies.collision.terrain_data=terrain.data();dependencies.collision.terrain_size=terrain.size();
    dependencies.collision.static_data=statics.empty()?nullptr:statics.data();dependencies.collision.static_size=statics.size();
    awl::WorldMapPlayerMovementInput motion;motion.dependencies=&dependencies;motion.triggers=&trigger;motion.secondary_byte_3f1=uint8_t{0};motion.outputs={true,true,true,true};
    awl::WorldMapPlayerState0SecondaryInput input;input.body.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.body.terrain_slot_present=true;
    input.body.terrain=&probe;input.body.secondary_byte_3f1=uint8_t{0};input.item_144=0;input.action_148=0;input.movement=&motion;
    awl::WorldMapPlayerState0ParentInput parent;parent.guards=awl::WorldMapPlayerUpdateGuards{-1,0};parent.scene_child_word_4=0;parent.saved_subobject_byte_14c=uint8_t{0};
    const auto begin=[&](uint16_t buttons,int8_t stick,bool capture){
        expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"state-zero fixture starts an explicit new CPU loop");
        if(capture){inputs.observe_pad_sample({true,buttons,stick});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"state-zero loop captures owned PAD");}
        parent.pose=movement.pose;
        expect(player.advance_state0_primary_slice(inputs,parent,&owners).status==awl::WorldMapPlayerPrimarySliceStatus::Completed,"state-zero first saves the pre-second pose and advances F000 once");
    };
    begin(0,0,false);
    auto before=startup_snapshot(player),effects=update_owners_snapshot(owners),moving=state1_motion_snapshot(movement,scene),draw=actor_queue_snapshot(render);
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(owners)==effects && state1_motion_snapshot(movement,scene)==moving && actor_queue_snapshot(render)==draw;};
    const auto refresh=[&](){before=startup_snapshot(player);effects=update_owners_snapshot(owners);moving=state1_motion_snapshot(movement,scene);draw=actor_queue_snapshot(render);};
    const auto failed=[&](S wanted){const auto r=player.advance_state0_secondary(inputs,input,&owners,&movement,&scene);
        if(r.status!=wanted)std::cerr<<"state0 second stop "<<int(r.status)<<" expected "<<int(wanted)<<" move "<<(r.movement?int(r.movement->status):-1)<<" body "<<(r.body?int(r.body->status):-1)<<'\n';
        expect(r.status==wanted && preserved(),"state-zero second stop preserves accepted first/model and all staged movement/camera/status/queue effects");return r;};
    expect(player.advance_state0_secondary(inputs,input,nullptr,&movement,&scene).status==S::RequiresUpdateOwners && preserved(),"state-zero second requires the accepted update owner");
    {auto foreign=owners;expect(player.advance_state0_secondary(inputs,input,&foreign,&movement,&scene).status==S::InvalidInput && preserved(),"state-zero second rejects a replacement update owner");}
    expect(player.advance_state0_secondary(inputs,input,&owners,nullptr,&scene).status==S::RequiresMovementOwners && preserved(),"state-zero movement needs both explicit owners");
    {auto pose=movement.pose;movement.pose.position[0]+=1;refresh();failed(S::InvalidInput);movement.pose=pose;refresh();}
    input.movement=nullptr;failed(S::RequiresMovement);input.movement=&motion;
    expect(failed(S::MovementIncomplete).movement->status==awl::WorldMapPlayerMovementStatus::RequiresPad,"state-zero movement cannot consume previous-loop PAD");
    inputs.observe_pad_sample({true,0});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"neutral PAD reaches the direct shared body");
    input.action_148.reset();expect(failed(S::BodyIncomplete).body->status==B::RequiresAction,"zero-intensity route reaches conditions while parent state remains zero");input.action_148=0;
    {const auto status=owners.status;owners.status.reset();refresh();expect(failed(S::BodyIncomplete).body->status==B::RequiresStatus,"zero-intensity body requires actual status");owners.status=status;refresh();}
    input.body.condition_factors_override.emplace();expect(failed(S::BodyIncomplete).body->status==B::ConditionsIncomplete,"incomplete conditions cannot acknowledge the second");input.body.condition_factors_override.reset();
    input.body.terrain_slot_present.reset();expect(failed(S::BodyIncomplete).body->status==B::RequiresTerrainSlot,"zero movement still reaches slot-five presence");input.body.terrain_slot_present=true;
    input.body.terrain=nullptr;expect(failed(S::BodyIncomplete).body->status==B::RequiresTerrainAsset,"missing slot-five bytes roll back prepared movement");input.body.terrain=&probe;
    const auto status=*owners.status;owners.status->word_12c=*inputs.sources().phase->raw_word_0;refresh();
    expect(failed(S::BodyIncomplete).body->requested_state==0x21u,"state-zero direct body preserves a condition transition before its effects");*owners.status=status;
    owners.status->byte_158=uint8_t{255};owners.status->byte_159=uint8_t{255};owners.status->byte_15a=uint8_t{255};owners.status->byte_15b=uint8_t{255};refresh();
    expect(player.advance_state0_third(inputs,{},{}).status==Third::RequiresSecondary && player.advance_state1_third(inputs,{},{}).status==Third::RequiresPrimary && preserved(),"incomplete zero-state second cannot borrow a state-one receipt or run third");
    std::optional<awl::WorldMapPlayerState0SecondarySliceResult> accepted;
#if !defined(_DEBUG)
    if(allocations){size_t rejected=0;for(size_t n=0;n<512;++n){const auto live=allocation_probe::live;allocation_probe::remaining=n;allocation_probe::enabled=true;
        const auto r=player.advance_state0_secondary(inputs,input,&owners,&movement,&scene);allocation_probe::enabled=false;
        if(r.status==S::RetainedState){accepted=r;break;}++rejected;expect(r.status==S::AllocationFailure && preserved() && allocation_probe::live==live,"all second movement/terrain allocations roll back staging and release storage");}
        expect(accepted.has_value() && rejected>4,"state-zero allocation sweep reaches movement and direct-body publication");std::cout<<"PLAYER_STATE0_SECOND_ALLOCATION_STOPS "<<rejected<<'\n';}
#else
    (void)allocations;
#endif
    if(!accepted)accepted=player.advance_state0_secondary(inputs,input,&owners,&movement,&scene);
    expect(accepted->status==S::RetainedState && accepted->movement && accepted->movement->intensity==0.0f && accepted->body && accepted->body->status==B::Completed && player.state().state_1364==0 && player.primary_loop()->state0_secondary_completed && !player.primary_loop()->state1_secondary_completed,"zero movement runs the no-event body without entering state one");
    if(accepted->status!=S::RetainedState)return false;
    expect(!accepted->movement->metadata && !accepted->movement->direction && !accepted->movement->position && !accepted->movement->axis && scene.object(71)->position==movement.pose.position && player.camera()->camera.target.camera.position==movement.camera->target.camera.position,"42BBC overrides every requested nullable output and publishes the owned camera and registry");
    expect(owners.saved_pose.position==parent.pose.position && owners.saved_pose.heading==parent.pose.heading && owners.status->byte_158==uint8_t{0} && owners.status->byte_159==uint8_t{0},"second retains the earlier saved pose and accepts condition clears only with the complete body");
    refresh();expect(player.advance_state0_secondary(inputs,awl::WorldMapPlayerState0SecondaryInput{},nullptr,nullptr,nullptr).status==S::AlreadyCompleted && preserved(),"completed state-zero second needs no repeated observations");
    {auto foreign=movement;auto other=scene;expect(player.advance_state0_secondary(inputs,input,&owners,&foreign,&scene).status==S::InvalidInput && player.advance_state0_secondary(inputs,input,&owners,&movement,&other).status==S::InvalidInput && preserved(),"completed second cannot substitute equal-valued movement or registry storage");}
    const auto finish=[&](){
        awl::WorldMapPlayerState0ThirdInput third;third.guards=awl::WorldMapPlayerUpdateGuards{0,{}};
        if(context_sources){third.guards=awl::WorldMapPlayerUpdateGuards{-1,0};third.context.emplace();third.context->scene_type=movement.pose.scene_type;third.context->current_key=17;
            third.context->position=movement.pose.position;third.context->facing=movement.pose.heading;third.context->action=0;third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};third.context->tracked_14ac=0;
            third.context->scene_predicate_1728e4=uint8_t{0};third.context->secondary_byte_3f1=uint8_t{0};third.context->label_observations.terrain_clear={true,true};third.context->label_observations.secondary_ground_available=true;}
        const auto tail=player.advance_state0_third(inputs,context_sources?*context_sources:awl::WorldMapPlayerContextUpdateSources{},third);
        const bool paused=(inputs.queue_control().control_word.value_or(0)&0x800u)!=0;
        expect(tail.status==(context_sources && !paused?Third::Prepared:Third::SkippedByGuard) && player.normal_third_loop()->state==0,"direct shared second authorizes only the state-zero common third");
        awl::WorldMapPlayerStartupQueuedActorInput actor;
        if(context_sources){actor=frame_actor;actor.actor.script->flag_bytes[0]=0;actor.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{movement.pose.position,movement.pose.heading};
            render.erase(std::remove_if(render.begin(),render.end(),[&](const auto& entry){return entry.node_identity==player.render_node_identity() || entry.node_identity==player.effect_node_identity();}),render.end());
        }else{actor.actor.saved_byte_29a01=uint8_t{0};actor.actor.script.emplace();actor.actor.script->flag_width_4=1;actor.actor.script->flag_bytes={1};}
        const auto queued=player.consume_state0_update_after_third(inputs,actor,&owners,context_sources?&render:nullptr);
        expect(queued.status==Q::Completed && owners.queue.size()==1 && owners.queue.front().node_identity==900,"state-zero movement loop consumes only its own queued update after third");
        if(context_sources)expect(queued.callback && queued.callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::Completed && player.model().primary().cpu().frame()->skin_executed,"state-zero no-event loop publishes an actual local skinned CPU frame");
    };
    finish();
    begin(0,0,true);const auto repeat=player.advance_state0_secondary(inputs,input,&owners,&movement,&scene);
    expect(repeat.status==S::RetainedState && repeat.body && player.state().state_1364==0,"consecutive neutral loop retains state zero through the shared active body");if(repeat.status!=S::RetainedState)return false;finish();
    const auto active_sources=inputs.sources();auto paused_sources=active_sources;paused_sources.phase->byte_a=uint8_t{1};inputs.observe_control_sources(paused_sources);
    const auto active_status=owners.status;const auto body=input.body;owners.status.reset();parent.saved_subobject_byte_14c=uint8_t{255};
    input.body={};input.body.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.item_144.reset();input.action_148.reset();begin(0,0,true);
    const auto paused=player.advance_state0_secondary(inputs,input,&owners,&movement,&scene);
    expect(inputs.queue_control().control_word==0x800u && paused.status==S::RetainedState && paused.movement && paused.movement->status==awl::WorldMapPlayerMovementStatus::Advanced && paused.body && paused.body->status==B::Completed && !paused.body->conditions && !owners.status,"control800 still executes state-zero movement before the shared body skips unread status/item/action/terrain dependencies");
    if(paused.status!=S::RetainedState)return false;finish();
    inputs.observe_control_sources(active_sources);owners.status=active_status;parent.saved_subobject_byte_14c=uint8_t{0};input.body=body;input.item_144=0;input.action_148=0;
    begin(awl::pad_button_mask(awl::PadButton::Start),0,true);refresh();const auto event=failed(S::BodyIncomplete);
    expect(event.body && event.body->requested_state==0x27u && event.body->reset_required && !player.primary_loop()->pending_state_request,"actual captured Start stops before reset/state27 without converting it into state-one movement");
    // The next explicit fixture loop explores a different input after the
    // unsupported event stop. This does not complete that event or its effects.
    begin(0,80,true);refresh();input.body={};input.body.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.action_148.reset();input.item_144.reset();
    const auto prior_pose=owners.saved_pose;const auto old_target=player.state().target_14ac,old_direction=player.state().direction_14b4;
    const auto moving_request=player.advance_state0_secondary(inputs,input,&owners,&movement,&scene);
    expect(moving_request.status==S::TransitionRequired && moving_request.requested_state==1 && moving_request.movement && *moving_request.movement->intensity!=0 && !moving_request.body && player.primary_loop()->pending_state_request==1 && !player.primary_loop()->state0_secondary_completed,"nonzero intensity publishes movement and retains request one without reading zero-branch body/item/action dependencies");
    if(moving_request.status!=S::TransitionRequired)return false;
    expect(movement.pose.position!=prior_pose.position && owners.saved_pose.position==prior_pose.position && owners.saved_pose.heading==prior_pose.heading && player.state().target_14ac==old_target && player.state().direction_14b4==old_direction,"moving second keeps the pre-second saved pose and null-output parent fields");
    refresh();expect(player.advance_state0_secondary(inputs,awl::WorldMapPlayerState0SecondaryInput{},nullptr,nullptr,nullptr).requested_state==1 && preserved(),"pending initializer retry never moves a second time");
    {auto foreign=movement;expect(player.advance_state0_secondary(inputs,input,&owners,&foreign,&scene).status==S::InvalidInput && preserved(),"pending movement request cannot bind a replacement movement owner");}
    expect(player.advance_state0_third(inputs,{},{}).status==Third::RequiresSecondary && player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::RequiresPrimary && preserved(),"moving request alone does not authorize either state's third/queue");
    std::cout<<"PLAYER_STATE0_MOVEMENT_SECOND two consecutive neutral and one control800 first/model/second/third/queued loops; Start stop; movement request to checked initializer\n";
    return true;
}
void state1_transition_owner_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapUpdateInputs& inputs,
    awl::WorldMapPlayerUpdateOwners& owners,awl::WorldMapPlayerActorQueue& render,
    const awl::WorldMapPlayerStartupQueuedActorInput& frame_actor,
    const awl::WorldMapPlayerContextUpdateSources* context_sources,bool allocations) {
    using S=awl::WorldMapPlayerState1TransitionStatus;using Second=awl::WorldMapPlayerState0SecondaryStatus;
    using Third=awl::WorldMapPlayerState0ThirdStatus;using Q=awl::WorldMapPlayerStartupQueueStatus;using A=awl::WorldMapPlayerState0TransitionStatus;
    expect(inputs.game_clock()->advance(),"state-one diagnostic loop advances explicitly");
    inputs.request_scene_mode(4,0);expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"state-one fixture captures active control before scene commit");
    awl::WorldMapSceneModeCommit commit;expect(inputs.commit_scene_fields(0,uint8_t{0},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields && inputs.scene().scene_type_68==1,"actual owned manager scene one selects the audio dependency");
    owners.status->word_20=0;awl::WorldMapPlayerState0ParentInput parent;parent.guards=awl::WorldMapPlayerUpdateGuards{-1,0};parent.scene_child_word_4=0;
    parent.pose=owners.saved_pose;parent.saved_subobject_byte_14c=uint8_t{0};
    expect(player.advance_state0_primary_slice(inputs,parent,&owners).status==awl::WorldMapPlayerPrimarySliceStatus::Completed,"state-one fixture owns a fresh normal parent/model prefix");
    expect(player.advance_state1_transition(inputs,{},&owners).status==S::RequiresPendingTransition,"state-one initialization requires an explicitly retained request after an eligible owned prefix");
    awl::WorldMapPlayerMovementState movement;awl::WorldMapSceneBucketRegistry movement_scene;
    auto request=player.request_state1_transition(inputs,awl::WorldMapPlayerUpdateGuards{-1,0});
    const bool movement_required=request.status==Second::RequiresMovement;
    if(request.status!=Second::TransitionRequired){expect(request.status==Second::RequiresMovement && !player.primary_loop()->pending_state_request && player.state().state_1364==0,"movement-required owner cannot fabricate a completed-holder state-one request");
        if(!state0_movement_second_checks(player,inputs,owners,render,frame_actor,context_sources,allocations,movement,movement_scene))return;}
    request=player.request_state1_transition(inputs,awl::WorldMapPlayerUpdateGuards{-1,0});
    expect(request.requested_state==1 && player.primary_loop()->pending_state_request==1 && !player.primary_loop()->state0_secondary_completed,"completed-holder request is retained without accepting a transition or second pass");
    auto before=startup_snapshot(player),effects=update_owners_snapshot(owners),draw=actor_queue_snapshot(render);
    const auto moving=state1_motion_snapshot(movement,movement_scene);
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw && state1_motion_snapshot(movement,movement_scene)==moving;};
    expect(player.advance_state0_secondary(inputs,awl::WorldMapPlayerUpdateGuards{0,{}}).requested_state==1 &&
        player.request_state1_transition(inputs,{}).requested_state==1 && preserved(),"retry cannot discard a retained state-one request by replacing guards");
    {auto other=owners;expect(player.advance_state1_transition(inputs,{},&other).status==S::InvalidInput && preserved(),"transition binds the accepted update owner");}
    awl::WorldMapUpdateInputs other_inputs(inputs.game_clock());other_inputs.observe_control_sources(inputs.sources());
    expect(other_inputs.capture_control()==awl::WorldMapControlStatus::Captured && player.advance_state1_transition(other_inputs,{},&owners).status==S::InvalidInput && preserved(),"same-clock replacement input cannot supply a different transition source");
    awl::WorldMapPlayerState1TransitionInput transition;
    const auto stopped=[&](S expected){const auto result=player.advance_state1_transition(inputs,transition,&owners);
        expect(result.status==expected && preserved(),"missing/unsupported state-one dependencies preserve pending request and all accepted game/model/item/queue state");return result;};
    stopped(S::RequiresSavedRemapByte);transition.saved_byte_14d35=uint8_t{0};stopped(S::RequiresSecondaryRemapByte);
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){transition.saved_byte_14d35=uint8_t(seed%256);transition.secondary_byte_3f1=uint8_t(seed/256*255);
        const auto result=stopped(seed%256 || seed/256?S::UnsupportedRemappedState:S::RequiresAudioByte);
        for(uint32_t v:{seed,uint32_t(*transition.saved_byte_14d35),uint32_t(*transition.secondary_byte_3f1),uint32_t(result.remapped_state.value_or(-1))})hash(digest,v);}
    expect(digest==0x36607f6f7e377b58ull,"512 retained requested-one remaps agree with actual 31D7C precedence");
    transition.saved_byte_14d35=uint8_t{0};transition.secondary_byte_3f1=uint8_t{0};transition.audio_byte_90=uint8_t{255};stopped(S::RequiresAudioStop);
    transition.audio_byte_90=uint8_t{0};expect(stopped(S::AppearanceIncomplete).appearance->status==A::RequiresModelByte,"appearance follows remap/audio checks");
    transition.saved_subobject_byte_14c=uint8_t{0};
    {const auto status=owners.status;owners.status.reset();effects=update_owners_snapshot(owners);expect(stopped(S::AppearanceIncomplete).appearance->status==A::RequiresStatus,"reached appearance selector requires the pointed owned status");owners.status=status;effects=update_owners_snapshot(owners);}
    const auto sources=inputs.sources();auto phase_sources=sources;phase_sources.phase->raw_word_0.reset();inputs.observe_control_sources(phase_sources);
    expect(stopped(S::AppearanceIncomplete).appearance->status==A::RequiresPhaseRaw,"appearance cannot substitute the game clock for current phase");
    phase_sources=sources;const auto phase=(awl::derive_world_map_phase(*sources.phase->raw_word_0)+2)%6;
    constexpr std::array<uint32_t,6> phase_origins{0,34560000,103680000,207360000,276480000,311040000};phase_sources.phase->raw_word_0=phase_origins[phase]+123456;
    inputs.observe_control_sources(phase_sources);owners.status->word_20=75000;effects=update_owners_snapshot(owners);
    auto result=stopped(S::InitializerIncomplete);expect(result.initializer && result.initializer->status==awl::WorldMapPlayerState1InitializerStatus::RequiresAction && result.appearance->model_row->selector==1,"refreshed appearance stages before the initializer action read");
    transition.action_148=3;result=stopped(S::InitializerIncomplete);
    expect(result.initializer->status==awl::WorldMapPlayerState1InitializerStatus::RequiresItemOwner && result.initializer->mask==player.model().initial_inputs()->state1_action_mask(),"nonzero action chooses the retained metadata mask before requiring its separate owner");
    owners.item_owner=awl::WorldMapPlayerItemOwnerState{0xf0f0f0f0};effects=update_owners_snapshot(owners);transition.action_148.reset();
#if !defined(_DEBUG)
    if(allocations)for(size_t allocation=0;allocation<2;++allocation){const auto live=allocation_probe::live;allocation_probe::remaining=allocation;allocation_probe::enabled=true;
        const auto failed=player.advance_state1_transition(inputs,transition,&owners);allocation_probe::enabled=false;
        expect(failed.status==S::AllocationFailure && preserved() && allocation_probe::live==live,"both reached appearance timer allocations preserve the pending state-one transition and release staging");}
#else
    (void)allocations;
#endif
    transition.action_148=phase%2?2:0;
    const auto prior_holder=player.model().state();const auto prior_counter=player.state().counter_1368;const auto prior_frame=primary_snapshot(player.model().primary());
    const auto prior_playback=progression_playback_snapshot(player.model().primary().model().partial_playback());
    const auto prior_camera=player.camera()?camera_snapshot(*player.camera()):0;
    const auto prior_queue=owners.queue;const auto prior_raw=inputs.game_clock()->state().raw_time,prior_loop=inputs.game_clock()->state().loop_count;
    result=player.advance_state1_transition(inputs,transition,&owners);
    awl::WorldMapPlayerModelTypeRow row;expect(player.model().initial_inputs()->select_model_type(phase,1,&row),"current-phase expected model row resolves");
    expect(result.status==S::Advanced && result.remapped_state==1 && player.state().state_1364==1 && player.state().secondary_byte_3f0==0 && owners.item_owner->mask_88==0 &&
        !player.primary_loop()->pending_state_request && player.primary_loop()->state1_transition_completed && player.primary_loop()->state0_secondary_completed && !player.primary_loop()->followups_completed,
        "state-one transition atomically publishes appearance, replacement mask, state and secondary clear without acknowledging third/queue work");
    expect(player.model().state().feature_38->type_0==row.type_0 && player.model().state().feature_38->type_4==row.type_4 &&
        player.model().state().feature_38->index_8==prior_holder.feature_38->index_8 && player.model().state().current_descriptor_0==prior_holder.current_descriptor_0 &&
        player.model().state().base_descriptor_4==prior_holder.base_descriptor_4 && player.model().state().completed_20==prior_holder.completed_20 &&
        primary_snapshot(player.model().primary())==prior_frame && progression_playback_snapshot(player.model().primary().model().partial_playback())==prior_playback &&
        player.state().counter_1368==prior_counter && (player.camera()?camera_snapshot(*player.camera()):0)==prior_camera && actor_queue_snapshot(render)==draw &&
        owners.queue.size()==prior_queue.size() && inputs.game_clock()->state().raw_time==prior_raw && inputs.game_clock()->state().loop_count==prior_loop,
        "state-one initializer changes no animation command, counter, camera, CPU frame, playback, scheduling or time");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);
    expect(player.advance_state1_transition(inputs,{},&owners).status==S::AlreadyAdvanced && preserved(),"completed transition ignores replacement initializer observations");
    expect(player.advance_state0_third(inputs,{},{}).status==Third::RequiresPrimary && player.consume_state0_update_after_third(inputs,{},&owners,&render).status==Q::RequiresPrimary && preserved(),"state-zero-only APIs cannot acknowledge state-one callbacks");
    expect(player.consume_state1_update_after_third(inputs,{},&owners,&render).status==Q::RequiresThird && preserved(),"state-one queue requires its current third pass");
    expect(player.advance_state1_third(inputs,{},{}).status==Third::RequiresGuards && preserved(),"state-one shared third requires its own outer guards");
    awl::WorldMapPlayerState0ThirdInput third;third.guards=awl::WorldMapPlayerUpdateGuards{0,{}};
    if(context_sources){third.guards=awl::WorldMapPlayerUpdateGuards{-1,0};third.context.emplace();third.context->scene_type=2;third.context->current_key=17;
        third.context->action=*transition.action_148;third.context->facing={0,0,1};third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};third.context->tracked_14ac=0;
        third.context->label_observations.terrain_clear={true,true};third.context->label_observations.secondary_ground_available=true;
        if(movement_required){third.context->scene_type=movement.pose.scene_type;third.context->position=movement.pose.position;third.context->facing=movement.pose.heading;
            third.context->scene_predicate_1728e4=uint8_t{0};third.context->secondary_byte_3f1=uint8_t{0};}
        if(!inputs.pad_for_loop())expect(player.advance_state1_third(inputs,*context_sources,third).status==Third::RequiresPad && preserved(),"new state-one common staging cannot consume stale PAD or partially publish context");}
    inputs.observe_pad_sample({true,0});const auto pad_capture=inputs.capture_pad();expect(pad_capture==awl::WorldMapPadCaptureStatus::Captured || pad_capture==awl::WorldMapPadCaptureStatus::AlreadyCaptured,"state-one PAD is owned by the current loop");
    expect(player.advance_state1_third(inputs,context_sources?*context_sources:awl::WorldMapPlayerContextUpdateSources{},third).status==(context_sources?Third::Prepared:Third::SkippedByGuard) &&
        player.state0_third_loop()->state==1 && player.primary_loop()->followups_completed,"state-one direct 43120 row uses the shared bounded third transaction with a state-qualified receipt");
    awl::WorldMapPlayerStartupQueuedActorInput actor;actor.actor.saved_byte_29a01=uint8_t{255};before=startup_snapshot(player);effects=update_owners_snapshot(owners);
    expect(player.consume_state1_update_after_third(inputs,actor,&owners,&render).status==Q::CallbackIncomplete && preserved(),"state-one queued actor retains the original auxiliary UI dependency");
    if(context_sources){
        actor=frame_actor;actor.actor.script->flag_bytes[0]=0;
        if(movement_required)actor.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{movement.pose.position,movement.pose.heading};
        auto invalid=*actor.actor.visibility->view;invalid[11]=std::numeric_limits<float>::quiet_NaN();actor.actor.frame.node_post_transforms.back()=invalid;
        expect(player.consume_state1_update_after_third(inputs,actor,&owners,&render).status==Q::CallbackIncomplete && preserved(),"late state-one frame failure preserves the transitioned appearance/context and both queues");
        actor.actor.frame.node_post_transforms.back().reset();
        const auto queued=player.consume_state1_update_after_third(inputs,actor,&owners,&render);
        expect(queued.status==Q::Completed && queued.callback && queued.callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::Completed &&
            player.model().primary().cpu().frame() && owners.queue.size()==1 && owners.queue.front().node_identity==900,"state-one queued callback publishes an actual local CPU frame after appearance refresh and removes only its owned node");
    }else{
        actor.actor.saved_byte_29a01=uint8_t{0};actor.actor.script.emplace();actor.actor.script->flag_width_4=1;actor.actor.script->flag_bytes={1};
        expect(player.consume_state1_update_after_third(inputs,actor,&owners,nullptr).status==Q::Completed && owners.queue.size()==1 && owners.queue.front().node_identity==900,"shared state-one script-blocked queued callback completes only its owned node");
    }
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    expect(player.advance_state1_third(inputs,{},{}).status==Third::AlreadyCompleted && player.consume_state1_update_after_third(inputs,{},&owners,nullptr).status==Q::AlreadyCompleted && preserved(),"same-loop state-one retries cannot repeat context or consume foreign work");
    expect(inputs.game_clock()->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"next diagnostic state-one loop captures fresh control");
    expect(player.advance_state1_third(inputs,{},{}).status==Third::RequiresPrimary && player.advance_state0_primary_slice(inputs,{},&owners).status==awl::WorldMapPlayerPrimarySliceStatus::InvalidInput,"state-one next-loop movement/primary dependencies cannot be replaced by state-zero work");
    state1_normal_owner_checks(player,inputs,owners,context_sources!=nullptr,allocations,context_sources,render,frame_actor);
    std::cout<<"PLAYER_STATE1_TRANSITION advanced / mask replaced / shared third and queued completion / next-loop state-one first connected\n";
}
void state0_normal_owner_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapUpdateInputs& inputs,
    awl::WorldMapPlayerUpdateOwners& owners,awl::WorldMapPlayerActorQueue& render,
    const awl::WorldMapPlayerContextUpdateSources* context_sources,bool allocations) {
    using Q=awl::WorldMapPlayerStartupQueueStatus;using P=awl::WorldMapPlayerUpdateStatus;using S=awl::WorldMapPlayerPrimarySliceStatus;
    using A=awl::WorldMapPlayerState0TransitionStatus;using Second=awl::WorldMapPlayerState0SecondaryStatus;using Third=awl::WorldMapPlayerState0ThirdStatus;
    auto before=startup_snapshot(player),effects=update_owners_snapshot(owners),draw=actor_queue_snapshot(render);
    const auto preserved=[&](){return startup_snapshot(player)==before && update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw;};
    awl::WorldMapPlayerStartupQueuedActorInput actor;actor.actor.saved_byte_29a01=uint8_t{255};
    auto queue=player.consume_state0_update_after_third(inputs,actor,&owners,&render);
    expect(queue.status==Q::CallbackIncomplete && queue.callback && queue.callback->actor && queue.callback->actor->gate==awl::WorldMapPlayerActorGateStatus::RequiresAuxiliaryCallback && preserved(),
        "state-zero saved-UI flag stops before script/frame work and preserves accepted third callback");
    actor.actor.saved_byte_29a01=uint8_t{0};
    expect(player.consume_state0_update_after_third(inputs,actor,&owners,&render).status==Q::CallbackIncomplete && preserved(),"missing shared script owner keeps the state-zero update pending");
    actor.actor.script.emplace();actor.actor.script->flag_width_4=1;actor.actor.script->flag_bytes={0};actor.actor.script->records.emplace();
    actor.actor.derive_normal_visibility=true;actor.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{owners.saved_pose.position,owners.saved_pose.heading};
    actor.actor.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());
    actor.actor.visibility.emplace();actor.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;
        plane.normal[i/2]=i%2?-1.0f:1.0f;actor.actor.visibility->planes[i]=plane;}
    actor.actor.screen.emplace();actor.actor.screen->view=*actor.actor.visibility->view;actor.actor.screen->half_extents={1000.0f,1000.0f};actor.effect_gate=uint8_t{0};
    auto invalid=*actor.actor.visibility->view;invalid[11]=std::numeric_limits<float>::quiet_NaN();actor.actor.frame.node_post_transforms.back()=invalid;
    expect(player.consume_state0_update_after_third(inputs,actor,&owners,&render).status==Q::CallbackIncomplete && preserved(),"late state-zero CPU-frame failure preserves context, model, render and pending update queues");
    expect(player.advance_state0_third(inputs,{},{}).status==Third::AlreadyCompleted && preserved(),"late queued failure cannot repeat context/icon/PAD work");
    actor.actor.frame.node_post_transforms.back().reset();
    {auto other=owners;expect(player.consume_state0_update_after_third(inputs,actor,&other,&render).status==Q::InvalidInput && preserved(),"state-zero queue cannot substitute a same-valued update owner");}
    expect(player.advance_state0_primary_slice(inputs,{},&owners).status==S::RequiresNextLoop && preserved(),"normal state-zero first pass cannot run retroactively after the startup transition/third pass");
    queue=player.consume_state0_update_after_third(inputs,actor,&owners,&render);
    expect(queue.status==Q::Completed && queue.callback && queue.callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::Completed &&
        player.model().primary().cpu().frame() && player.primary_loop()->queued_completed && owners.queue.size()==1 && owners.queue.front().node_identity==900,
        "state-zero queued actor publishes an actual CPU frame and removes only its owned front update");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    expect(player.consume_state0_update_after_third(inputs,{},&owners,nullptr).status==Q::AlreadyCompleted && preserved(),"same-loop queue retry cannot consume the next foreign node");
    expect(inputs.game_clock()->advance(),"normal state-zero loop advances explicitly after supplied rendering");
    expect(player.advance_state0_primary_slice(inputs,{},&owners).status==S::RequiresControl,"normal first pass requires new captured control");
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"normal state-zero control captures once");
    expect(player.advance_state0_third(inputs,{},{}).status==Third::RequiresPrimary && player.advance_state0_secondary(inputs,{}).status==Second::RequiresPrimary,"later passes cannot fabricate a normal first pass");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    awl::WorldMapPlayerState0ParentInput parent;
    const auto stopped=[&](P expected){const auto result=player.dispatch_state0_parent(inputs,parent,&owners);
        expect(result.status==expected && preserved(),"normal parent stop preserves appearance, status, context, pose, queues, frame and receipts");return result;};
    stopped(P::RequiresGuardOwner);parent.guards=awl::WorldMapPlayerUpdateGuards{0,{}};stopped(P::RequiresGuardTransition);
    parent.guards=awl::WorldMapPlayerUpdateGuards{-1,{}};stopped(P::RequiresGuardWord58c);parent.guards->word_58c=0;stopped(P::RequiresSceneWord);
    parent.scene_child_word_4=1;stopped(P::RequiresSceneTransition);parent.scene_child_word_4=0;
    expect(stopped(P::AppearanceIncomplete).appearance->status==A::RequiresModelByte,"model refresh follows outer entry checks");
    parent.saved_subobject_byte_14c=uint8_t{0};parent.pose=owners.saved_pose;
    {const auto status=owners.status;owners.status.reset();effects=update_owners_snapshot(owners);
        expect(stopped(P::AppearanceIncomplete).appearance->status==A::RequiresStatus,"selector requires the owned pointed status before the active status pair");owners.status=status;effects=update_owners_snapshot(owners);}
    const auto sources=inputs.sources();auto unknown_phase=sources;unknown_phase.phase->raw_word_0.reset();inputs.observe_control_sources(unknown_phase);
    expect(stopped(P::AppearanceIncomplete).appearance->status==A::RequiresPhaseRaw,"appearance uses the current phase owner, not constructor phase or GameClock");inputs.observe_control_sources(sources);
    const auto old_word20=owners.status->word_20;
    parent.status_factors_override.emplace();
    for(const auto& choice:std::array<std::pair<uint32_t,uint32_t>,6>{{{0,7},{24999,7},{25000,0},{74999,0},{75000,1},{0x80000000u,7}}}){
        owners.status->word_20=choice.first;effects=update_owners_snapshot(owners);const auto result=stopped(P::RequiresStatusUpdate);
        expect(result.appearance && result.appearance->model_row && result.appearance->model_row->selector==choice.second,"appearance selector reads pre-status signed word20 at independently verified thresholds");}
    owners.status->word_20=old_word20;effects=update_owners_snapshot(owners);parent.status_factors_override.reset();
    parent.pose.position[0]=std::numeric_limits<float>::quiet_NaN();stopped(P::InvalidInput);parent.pose=owners.saved_pose;
    {const auto position=owners.scene_one_position_240;owners.scene_one_position_240.reset();effects=update_owners_snapshot(owners);
        auto saved_pose=parent.pose;parent.pose.scene_type=1;stopped(P::RequiresScenePositionOwner);parent.pose=saved_pose;
        owners.scene_one_position_240=position;effects=update_owners_snapshot(owners);}
#if !defined(_DEBUG)
    if(allocations){for(size_t allocation=0;allocation<2;++allocation){const auto live=allocation_probe::live;
        allocation_probe::remaining=allocation;allocation_probe::enabled=true;const auto result=player.dispatch_state0_parent(inputs,parent,&owners);allocation_probe::enabled=false;
        expect(result.status==P::AllocationFailure && preserved() && allocation_probe::live==live,"both late normal-parent queue allocations roll back staged appearance/status and release staging");}}
#else
    (void)allocations;
#endif
    const auto old_counter=player.state().counter_1368;const auto camera=player.camera()?camera_snapshot(*player.camera()):0;
    auto primary=player.advance_state0_primary_slice(inputs,parent,&owners);
    expect(primary.status==S::Completed && primary.parent && primary.parent->status==P::Advanced && primary.animation &&
        player.primary_loop()->state0_parent_completed && !player.primary_loop()->state0_secondary_completed && !player.primary_loop()->followups_completed &&
        owners.queue.size()==2 && owners.queue.front().node_identity==player.update_node_identity() &&
        player.state().counter_1368==old_counter && (player.camera()?camera_snapshot(*player.camera()):0)==camera,
        "normal first pass refreshes appearance/status/pose/owned queue before exactly one model progression without a state29 camera/counter callback");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    primary=player.advance_state0_primary_slice(inputs,{},nullptr);
    expect(primary.status==S::Completed && primary.parent_reused && preserved(),"same-loop normal first-pass retry reuses parent and model instead of repeating status or queue insertion");
    expect(player.consume_state0_update_after_third(inputs,{},&owners,&render).status==Q::RequiresThird && preserved(),"normal queue cannot precede its fresh third callback");
    expect(player.advance_state0_third(inputs,{},{}).status==Third::RequiresSecondary && preserved(),"normal third cannot precede the actual second pass");
    expect(player.advance_state0_secondary(inputs,{}).status==Second::RequiresGuards && preserved(),"normal second requires its own outer guard observations");
    const auto& holder=player.model().state();const auto expected=*holder.completed_20?Second::TransitionRequired:*holder.flag_21?Second::RetainedState:Second::RequiresMovement;
    auto second=player.advance_state0_secondary(inputs,awl::WorldMapPlayerUpdateGuards{-1,0});
    expect(second.status==expected,"active second pass reads the owned holder bytes and reaches the original retained/movement/state-one route");
    if(expected!=Second::RetainedState){expect(preserved() && !player.primary_loop()->state0_secondary_completed,"untranslated movement/state-one action preserves pending work and acknowledges no second pass");
        // A separate explicitly blocked-guard scenario proves the skip path;
        // this is not acceptance of the preceding active movement/state request.
        second=player.advance_state0_secondary(inputs,awl::WorldMapPlayerUpdateGuards{0,{}});
        expect(second.status==Second::SkippedByGuard,"diagnostic blocked-guard scenario skips the state-zero body");}
    expect(player.advance_state0_secondary(inputs,{}).status==Second::AlreadyCompleted,"accepted second pass ignores replacement observations");
    inputs.observe_pad_sample({true,0});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"normal idle PAD captures explicitly");
    awl::WorldMapPlayerState0ThirdInput third;third.guards=awl::WorldMapPlayerUpdateGuards{0,{}};
    if(context_sources){third.guards=awl::WorldMapPlayerUpdateGuards{-1,0};third.context.emplace();third.context->scene_type=2;third.context->current_key=17;
        third.context->facing={0,0,1};third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};third.context->tracked_14ac=0;}
    const auto completed=player.advance_state0_third(inputs,context_sources?*context_sources:awl::WorldMapPlayerContextUpdateSources{},third);
    expect(completed.status==(context_sources?Third::Prepared:Third::SkippedByGuard) && player.primary_loop()->followups_completed,"normal owned first/second/third sequence completes in order under the explicitly supplied guard scenario");
    actor.actor.script->flag_bytes[0]=1;const auto model_before=holder_snapshot(player.model());
    queue=player.consume_state0_update_after_third(inputs,actor,&owners,nullptr);
    expect(queue.status==Q::Completed && queue.callback && queue.callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::ScriptBlocked &&
        holder_snapshot(player.model())==model_before && owners.queue.size()==1 && owners.queue.front().node_identity==900,"script-blocked normal state-zero queued actor completes only its own update without frame/render work");
    // Paused control skips status, but not 42B80 appearance or 42BBC's body.
    expect(inputs.game_clock()->advance(),"paused diagnostic loop advances explicitly");
    auto paused=sources;paused.phase->byte_a=uint8_t{1};inputs.observe_control_sources(paused);
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured && inputs.control()->dispatch_word==0x800,"paused control derives from the actual last override");
    const auto old_status=owners.status;owners.status.reset();parent.saved_subobject_byte_14c=uint8_t{255};parent.status_factors_override.emplace();
    primary=player.advance_state0_primary_slice(inputs,parent,&owners);
    expect(primary.status==S::Completed && primary.parent && primary.parent->appearance && primary.parent->appearance->model_row->selector==0x24 &&
        !primary.parent->status_update && !owners.status,"paused first pass still refreshes appearance while skipping unread status/global/rate dependencies");
    const auto& paused_holder=player.model().state();const auto paused_expected=*paused_holder.completed_20?Second::TransitionRequired:*paused_holder.flag_21?Second::RetainedState:Second::RequiresMovement;
    expect(player.advance_state0_secondary(inputs,awl::WorldMapPlayerUpdateGuards{-1,0}).status==paused_expected,"control bit800 cannot bypass the original second-pass holder/movement decisions");
    expect(player.advance_state0_secondary(inputs,awl::WorldMapPlayerUpdateGuards{0,{}}).status==(paused_expected==Second::RetainedState?Second::AlreadyCompleted:Second::SkippedByGuard),"explicit paused guard scenario completes only the supported second branch");
    third={};third.guards=awl::WorldMapPlayerUpdateGuards{-1,0};
    expect(player.advance_state0_third(inputs,{},third).status==Third::SkippedByGuard,"paused third skips context and stale/missing PAD only after outer guards");
    expect(player.consume_state0_update_after_third(inputs,actor,&owners,nullptr).status==Q::Completed,"paused script-blocked actor completes its own queue node");owners.status=old_status;
    // Committed scene groups filter player callbacks while F000 still runs.
    expect(inputs.game_clock()->advance(),"filtered diagnostic loop advances explicitly");inputs.observe_control_sources(sources);
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"filtered loop captures control");
    inputs.request_scene_mode(0,0);awl::WorldMapSceneModeCommit commit;
    expect(inputs.commit_scene_fields(0,{},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields,"owned scene commits a group-five filter");
    primary=player.advance_state0_primary_slice(inputs,{},nullptr);
    expect(primary.status==S::Completed && primary.parent && primary.parent->status==P::SkippedByDispatchFilter && primary.animation,
        "filtered first pass reads no callback observations or update owners and still progresses the owned model");
    before=startup_snapshot(player);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    expect(player.advance_state0_secondary(inputs,{}).status==Second::Filtered && player.advance_state0_third(inputs,{},{}).status==Third::Filtered,
        "filtered normal second and third require no guard, holder, context or PAD observations");
    expect(update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw &&
        player.consume_state0_update_after_third(inputs,{},&owners,nullptr).status==Q::UnsupportedNode,"filtered player callbacks cannot invent an owned update or consume remaining foreign work");
    state1_transition_owner_checks(player,inputs,owners,render,actor,context_sources,allocations);
    std::cout<<"PLAYER_STATE0_NORMAL owned startup-transition CPU frame / next-loop parent-model / second route "<<int(expected)<<" / explicit blocked-guard completion / script-blocked queued node\n";
}
void player_sources_fixture(Fixture& fixture){
    const auto terrain=state1_movement_terrain(),statics=state1_probe_terrain();
    fixture.write("jimen-move.col",terrain);fixture.write("jimen1-move.col",terrain);
    for(const char* name:{"mapobj.col","mapobj2.col","mapobj3.col","mapobj4.col","mapobj5.col"})fixture.write(name,statics);
    fixture.write("mapjump.col",statics);fixture.write("trigger.spl",state1_no_contact());
}
void player_sources_owner_checks(awl::WorldMapPlayerStartup& player,awl::WorldMapUpdateInputs& inputs,
    const awl::WorldMapPlayerUpdateOwners& prior,awl::WorldMapPlayerActorQueue& render,
    const awl::WorldMapPlayerContextUpdateSources* context_sources,bool allocations){
    using S=awl::WorldMapPlayerSourcesStatus;using Init=awl::WorldMapPlayerSourcesInitialization;
    using Second=awl::WorldMapPlayerState0SecondaryStatus;using B=awl::WorldMapPlayerState1SecondaryStatus;
    const bool local=context_sources!=nullptr;
    awl::WorldMapPlayerSources owner(inputs);awl::WorldMapPlayerSourcesSeed seed;seed.updates=prior;
    seed.updates.status.reset();seed.updates.saved_selection.reset();seed.updates.queue={{900,0x1011}};
    seed.updates.item_owner=awl::WorldMapPlayerItemOwnerState{0}; // explicit observed mask owner
    seed.movement.scene_identity=0x701;seed.movement.pose={1,{20,4,20},{0,0,1}};seed.movement.steering.facing_z=1;
    seed.fields.scene_child_word_4=0;seed.fields.terrain_variant_299a4=uint8_t{0};seed.fields.secondary_byte_3f1=uint8_t{0};
    seed.fields.static_flags.emplace();seed.fields.saved_remap_byte_14d35=uint8_t{0};seed.fields.audio_byte_90=uint8_t{0};
    std::shared_ptr<const awl::WorldMapPlayerCollisionSources> resources;
    const awl::WorldMapCollisionSourceState selection{*inputs.sources().phase->raw_word_0,0};
    const auto loaded=awl::WorldMapPlayerCollisionSources::load(selection,player.camera()->collision,local,&resources);
    expect(loaded==S::Completed && resources,"shared collision providers load through the current disc mount");if(!resources)return;
    if(local){awl::CollisionSurfaceSample surface;const auto& bytes=resources->movement().terrain_bytes();
        expect(awl::sample_type1_collision_surface(bytes.data(),bytes.size(),120,168,&surface),"owned-source local pose is explicitly contained in verified terrain");
        seed.movement.pose.position={120,surface.height,168};}
    expect(owner.advance_primary(player).status==S::RequiresInitialization,"source owner cannot run before explicit initialization");
    {awl::WorldMapUpdateInputs foreign(std::make_shared<awl::GameClock>());awl::WorldMapPlayerSources wrong(foreign);
        expect(wrong.initialize(player,seed,Init::Fresh)==S::ForeignPlayer,"source initialization rejects a different clock");}
    const auto observed=inputs.sources();auto missing=observed;missing.phase->raw_word_0.reset();inputs.observe_control_sources(missing);
    const auto before_init=startup_snapshot(player);
    expect(owner.initialize(player,seed,Init::Fresh)==S::RequiresPhase && !owner.updates().status && startup_snapshot(player)==before_init,"missing phase preserves uninitialized sources and player");
    inputs.observe_control_sources(observed);
#if !defined(_DEBUG)
    if(allocations){size_t rejected=0;bool done=false;for(size_t n=0;n<64;++n){const auto live=allocation_probe::live;allocation_probe::remaining=n;allocation_probe::enabled=true;
        const auto result=owner.initialize(player,seed,Init::Fresh);allocation_probe::enabled=false;
        if(result==S::Initialized){done=true;break;}++rejected;
        expect(result==S::AllocationFailure && !owner.updates().status && startup_snapshot(player)==before_init && allocation_probe::live==live,"source construction allocation failures preserve every unpublished owner");}
        expect(done && rejected>0,"source initialization sweep reaches scene and queue allocation stages");std::cout<<"PLAYER_SOURCES_INIT_ALLOCATION_STOPS "<<rejected<<'\n';}
#else
    (void)allocations;
#endif
    if(!owner.updates().status)expect(owner.initialize(player,seed,Init::Fresh)==S::Initialized,"fresh shared sources initialize once");
    expect(owner.initialize(player,seed,Init::Fresh)==S::AlreadyInitialized && owner.fields().guards->word_680==-1 &&
        owner.fields().guards->word_58c==0 && owner.updates().saved_selection->item_144==0u && owner.updates().saved_selection->action_148==0u,"fresh source projection retains traced status, guard and selection defaults");
    {awl::WorldMapPlayerSources restored(inputs);auto projection=seed;projection.updates=owner.updates();projection.fields.guards.reset();
        expect(restored.initialize(player,projection,Init::Observed)==S::Initialized && !restored.fields().guards,"observed initialization preserves unknown fields rather than applying fresh defaults");}
    auto retained=resources;
    expect(awl::WorldMapPlayerCollisionSources::load({selection.phase_counter_word,2},player.camera()->collision,false,&resources)==S::InvalidInput && resources==retained &&
        awl::WorldMapPlayerCollisionSources::load(selection,{},false,&resources)==S::RequiresCamera && resources==retained,"failed resource selection and camera binding preserve the prior provider");
    if(!local)expect(awl::WorldMapPlayerCollisionSources::load(selection,player.camera()->collision,true,&resources)==S::AssetFailure && resources==retained,"missing object archive preserves the installed-candidate resource bundle");
#if !defined(_DEBUG)
    if(allocations)for(size_t n=0;n<2;++n){const auto live=allocation_probe::live;allocation_probe::remaining=n;allocation_probe::enabled=true;
        const auto result=awl::WorldMapPlayerCollisionSources::load(selection,player.camera()->collision,false,&resources);allocation_probe::enabled=false;
        expect(result==S::AllocationFailure && resources==retained && allocation_probe::live==live,"resource owner/control-block allocation failures preserve the prior provider and release staging");}
#endif
    expect(owner.install_collision(resources)==S::Completed && owner.probe_present()==true,"source owner retains the immutable resource bundle and binds slot five");
    std::weak_ptr<const awl::WorldMapPlayerCollisionSources> weak=resources;resources.reset();retained.reset();
    auto fields=owner.fields();
    const auto fresh_loop=[&](int8_t stick,uint16_t buttons=0){expect(inputs.game_clock()->advance(),"source workflow advances the existing shared clock explicitly");
        expect(owner.advance_primary(player).status==S::RequiresControl,"stale control cannot start either normal state");
        expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"source workflow captures current control");
        inputs.observe_pad_sample({true,buttons,stick});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"source workflow captures current PAD");};
    const auto snapshot=[&](){return std::array<uint64_t,3>{startup_snapshot(player),update_owners_snapshot(owner.updates()),state1_motion_snapshot(owner.movement(),owner.scene())};};
    awl::WorldMapPlayerStartupQueuedActorInput actor;actor.actor.saved_byte_29a01=uint8_t{0};actor.actor.script.emplace();actor.actor.script->flag_width_4=1;
    actor.actor.script->flag_bytes={uint8_t(local?0:1)};actor.actor.script->records.emplace();actor.actor.derive_normal_visibility=true;
    actor.actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{9999,9999,9999},{1,0,0}};
    actor.actor.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());
    actor.actor.visibility.emplace();actor.actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;actor.actor.visibility->planes[i]=plane;}
    actor.actor.screen.emplace();actor.actor.screen->view=*actor.actor.visibility->view;actor.actor.screen->half_extents={1000.0f,1000.0f};actor.effect_gate=uint8_t{0};
    bool queue_allocation_checked=false;
    const auto finish=[&](){
        awl::WorldMapPlayerState0ThirdInput third;
        if(local){third.context.emplace();third.context->current_key=17;third.context->scene_type=99;third.context->position={9999,9999,9999};third.context->action=999;
            third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};third.context->scene_predicate_1728e4=uint8_t{0};
            third.context->label_observations.terrain_clear={true,true};third.context->label_observations.secondary_ground_available=true;
        }else{auto skipped=owner.fields();skipped.guards=awl::WorldMapPlayerUpdateGuards{0,{}};owner.observe_fields(skipped);}
        auto step=owner.advance_third(player,local?*context_sources:awl::WorldMapPlayerContextUpdateSources{},third);
        if(step.status!=S::Completed)std::cerr<<"sources third stop "<<int(step.status)<<" third "<<(step.third?int(step.third->status):-1)<<" context "<<(step.third && step.third->context?int(step.third->context->status):-1)<<'\n';
        expect(step.status==S::Completed,"owned current pose/selection/guards feed the state-qualified third callback");if(step.status!=S::Completed)return false;
        owner.observe_fields(fields);owner.observe_selection({{}, {}});
        expect(owner.advance_third(player,{},third).status==S::Completed,"accepted third ignores now-unknown replacement selection");owner.observe_selection({0u,0u});
        render.erase(std::remove_if(render.begin(),render.end(),[&](const auto& e){return e.node_identity==player.render_node_identity() || e.node_identity==player.effect_node_identity();}),render.end());
#if !defined(_DEBUG)
        if(allocations && !queue_allocation_checked){const auto before=snapshot();const auto draw=actor_queue_snapshot(render),live=allocation_probe::live;
            allocation_probe::remaining=0;allocation_probe::enabled=true;const auto failed=owner.consume_queued(player,actor,local?&render:nullptr);allocation_probe::enabled=false;
            expect(failed.status==S::AllocationFailure && snapshot()==before && actor_queue_snapshot(render)==draw && allocation_probe::live==live,"queued source placement staging failure preserves accepted third and both queues");queue_allocation_checked=true;}
#else
        (void)queue_allocation_checked;
#endif
        const auto queued=owner.consume_queued(player,actor,local?&render:nullptr);
        expect(queued.status==S::Completed && owner.updates().queue.size()==1 && owner.updates().queue[0].node_identity==900,"owned queue completion removes only the player's node");
        if(local)expect(player.model().primary().cpu().frame() && player.model().primary().cpu().frame()->skin_executed &&
            player.model().primary().cpu().frame()->root_matrix[3]==owner.movement().pose.position[0],"owned current movement pose overrides stale supplied frame placement");
        const auto before=snapshot();expect(owner.consume_queued(player,{},nullptr).status==S::Completed && snapshot()==before,"queued retry leaves foreign work and accepted pose unchanged");return queued.status==S::Completed;
    };
    bool connected=false;unsigned state0_ticks=0;
    for(unsigned tick=0;tick<4 && !connected;++tick){fresh_loop(tick<2?0:80);
        const auto primary=owner.advance_primary(player);expect(primary.status==S::Completed,"owned state-zero first and model complete");if(primary.status!=S::Completed)return;
        if(tick==0){const auto before=snapshot();expect(owner.advance_third(player,{},{}).status==S::CallbackIncomplete && owner.consume_queued(player,{},nullptr).status==S::CallbackIncomplete && snapshot()==before,"third and queue cannot bypass a pending second");}
        auto second=owner.advance_secondary(player);
        if(second.status==S::RequiresCollisionLists){const auto before=snapshot();expect(owner.advance_secondary(player).status==S::RequiresCollisionLists && snapshot()==before,"unknown collision lists preserve accepted first/model");owner.reset_collision_lists();second=owner.advance_secondary(player);}
        if(second.state0_secondary && second.state0_secondary->status==Second::TransitionRequired){
            const auto before=snapshot();auto unknown=owner.fields();unknown.saved_remap_byte_14d35.reset();owner.observe_fields(unknown);
            expect(owner.advance_transition(player).status==S::CallbackIncomplete && snapshot()==before,"missing transition source retains pending state-zero movement/receipt");owner.observe_fields(fields);
            const auto transitioned=owner.advance_transition(player);
            if(transitioned.status!=S::Completed)std::cerr<<"sources transition stop "<<int(transitioned.status)<<" detail "<<(transitioned.transition?int(transitioned.transition->status):-1)<<'\n';
            expect(transitioned.status==S::Completed && player.state().state_1364==1,"shared sources connect state zero to the checked state-one initializer");if(transitioned.status!=S::Completed)return;connected=true;
        }else expect(second.status==S::Completed && second.state0_secondary && second.state0_secondary->status==Second::RetainedState,"owned state-zero neutral movement and no-event second complete");
        ++state0_ticks;if(!finish())return;
    }
    expect(connected,"held input or completed holder reaches the connected state-one path");if(!connected)return;
    expect(inputs.game_clock()->advance(),"pool lifetime scenario starts after the accepted movement loop");
    owner.reset_collision_lists();
    awl::WorldMapRegisteredCollisionObject object;object.collision.identity=0x900;object.collision.enabled=false;object.collision.category=1;
    object.collision.collision_flags=2;
    expect(owner.register_collision_object(awl::WorldMapCollisionList::First,object,{{1,0}})==(local?S::Completed:S::RequiresRecordPool),"type-one registrations bind only retained pool records");
    if(local)expect(owner.clear_collision()==S::Busy && owner.unregister_collision_object(0x900),"registered pool views prevent premature resource release");
    object.collision.data=reinterpret_cast<const uint8_t*>(uintptr_t{1});
    expect(owner.register_collision_object(awl::WorldMapCollisionList::First,object)==S::InvalidInput,"unowned raw geometry is rejected");
    object.collision.data=nullptr;object.collision.collision_flags=0;object.collision.enabled=false;
    expect(owner.register_collision_object(awl::WorldMapCollisionList::Later,object)==S::Completed,"native list owner accepts a geometry-free supplied circle");
    for(unsigned tick=0;tick<6;++tick){fresh_loop(tick<3?80:0);const auto before=snapshot();
        if(tick==0){auto variant=fields;variant.terrain_variant_299a4=uint8_t{1};owner.observe_fields(variant);
            expect(owner.advance_primary(player).status==S::WrongCollisionSelection && snapshot()==before,"changed terrain selector cannot silently reuse the old slot-ten bytes");owner.observe_fields(fields);}
        awl::WorldMapPlayerSourcesResult primary;bool swept=false;
#if !defined(_DEBUG)
        if(allocations && tick==0){size_t rejected=0;for(size_t n=0;n<256;++n){const auto previous=snapshot();const auto live=allocation_probe::live;
            allocation_probe::remaining=n;allocation_probe::enabled=true;primary=owner.advance_primary(player);allocation_probe::enabled=false;
            if(primary.status==S::Completed){swept=true;break;}++rejected;
            const bool late=primary.state1_primary && primary.state1_primary->status==awl::WorldMapPlayerPrimarySliceStatus::AnimationIncomplete;
            if(late){expect(player.primary_loop()->state1_parent_completed && owner.clear_collision()==S::Busy,"late model allocation stop retains the accepted source-owned movement exactly once");}
            else expect((primary.status==S::AllocationFailure || (primary.state1_primary && primary.state1_primary->parent && primary.state1_primary->parent->status==awl::WorldMapPlayerUpdateStatus::AllocationFailure)) &&
                snapshot()==previous && allocation_probe::live==live,"source snapshot and first-pass allocation failures preserve every unaccepted owner and release staging");}
            expect(swept && rejected>8,"source-owned first sweep crosses snapshot, movement and model staging");std::cout<<"PLAYER_SOURCES_FIRST_ALLOCATION_STOPS "<<rejected<<'\n';}
#endif
        if(!swept)primary=owner.advance_primary(player);
        expect(primary.status==S::Completed && primary.state1_primary && player.primary_loop()->movement_owner!=0 && owner.updates().saved_pose.position==owner.movement().pose.position,"same owner drives state-one first movement and model progression");if(primary.status!=S::Completed)return;
        const auto accepted=snapshot();expect(owner.clear_collision()==S::Busy && !weak.expired() && owner.advance_primary(player).status==S::Completed && snapshot()==accepted,"accepted movement retains resource lifetime and retries exactly once");
        owner.observe_selection({0u,{}});const auto unknown=snapshot();auto second=owner.advance_secondary(player);
        expect(second.state1_secondary && second.state1_secondary->status==B::RequiresAction && snapshot()==unknown,"second reads current unknown action instead of its first-pass observation");
        owner.observe_selection({0u,0u});owner.clear_probe();
        if(tick==5){auto changed=fields;changed.guards=awl::WorldMapPlayerUpdateGuards{0,{}};owner.observe_fields(changed);owner.observe_selection({{}, {}});}
        second=owner.advance_secondary(player);expect(second.status==S::Completed,"known cleared slot-five skips only the terrain probe");
        if(tick==5){expect(second.state1_secondary && second.state1_secondary->status==B::Completed,"guard mutation between passes skips unread current selection and terrain");owner.observe_fields(fields);owner.observe_selection({0u,0u});}
        expect(owner.bind_probe(),"retained slot-five bytes can be rebound without reloading assets");if(!finish())return;
    }
    // Item/action can change between callbacks. A newly held item changes B's
    // request; failed second leaves accepted first/model and current sources.
    fresh_loop(0,0x200);expect(owner.advance_primary(player).status==S::Completed,"item-change scenario accepts its first pass");owner.observe_selection({1u,0u});
    const auto before_request=snapshot();auto requested=owner.advance_secondary(player);
    expect(requested.state1_secondary && requested.state1_secondary->status==B::TransitionRequired && requested.state1_secondary->requested_state==7u &&
        snapshot()==before_request,"second uses the current held item and preserves the untranslated action request");
    expect(owner.advance_third(player,{},{}).status==S::CallbackIncomplete && owner.consume_queued(player,{},nullptr).status==S::CallbackIncomplete,"pending action request cannot bypass second completion");
    expect(inputs.game_clock()->advance(),"resource teardown moves past accepted movement's loop");owner.reset_collision_lists();
    expect(owner.clear_collision()==S::Completed && !owner.collision() && weak.expired() && owner.probe_present()==false,"explicit teardown releases installed slots after borrowed object views and loop use end");
    auto paused=observed;paused.phase->byte_a=uint8_t{1};inputs.observe_control_sources(paused);owner.observe_status({});owner.observe_selection({{}, {}});
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured && owner.advance_primary(player).status==S::Completed && owner.advance_secondary(player).status==S::Completed &&
        owner.advance_third(player,{},{}).status==S::Completed,"paused state-one callbacks skip unloaded collision and unknown status/selection without inventing sources");
    auto hidden=actor;hidden.actor.script->flag_bytes[0]=1;
    expect(owner.consume_queued(player,hidden,nullptr).status==S::Completed && owner.updates().queue.size()==1,"paused source-owned completion preserves the foreign node");
    std::cout<<(local?"LOCAL_":"")<<"PLAYER_OWNED_SOURCES "<<state0_ticks<<" state-zero ticks / checked state-one connection / 6 active state-one ticks / current selection, slot lifetime, ordered CPU completion\n";
}
std::vector<uint8_t> scene_constructor_dol(){
    // Invented mapped sections/values, never copied game payloads. Callback
    // addresses are source keys checked by the bounded descriptor reader.
    std::vector<uint8_t> b(0x19300);
    constexpr std::array<uint32_t,3> addresses{0x8023e000,0x802ab000,0x80348000};
    constexpr std::array<uint32_t,3> sizes{0x13200,0x2000,0x4000},offsets{0x100,0x13300,0x15300};
    for(size_t i=0;i<3;++i){word(b,i*4,offsets[i]);word(b,0x48+i*4,addresses[i]);word(b,0x90+i*4,sizes[i]);}
    const auto at=[&](uint32_t address){for(size_t i=0;i<3;++i)if(address>=addresses[i] && address-addresses[i]<sizes[i])return size_t(offsets[i]+address-addresses[i]);throw std::runtime_error("constructor fixture address");};
    const auto f=[&](uint32_t a,float v){word(b,at(a),actor_bits(v));};
    const auto d=[&](uint32_t a,double v){uint64_t bits;std::memcpy(&bits,&v,8);word(b,at(a),uint32_t(bits>>32));word(b,at(a)+4,uint32_t(bits));};
    for(size_t i=0;i<4;++i){const uint32_t address=std::array<uint32_t,4>{0x8024f910,0x8024f920,0x8024f930,0x8023e3d0}[i];f(address,30+float(i)*5);f(address+4,19);f(address+8,30+float(i)*5);}
    f(0x80251138,1);f(0x80251160,1);f(0x8025115c,-1);
    constexpr uint32_t descriptors[]{0x802abf88,0x802ac4c8,0x802aca80,0x802acfc0},callbacks[]{0x80089460,0x80089ff4,0x8008acbc,0x8008b874};
    for(size_t i=0;i<4;++i){word(b,at(descriptors[i])+4,UINT32_MAX);word(b,at(descriptors[i])+8,callbacks[i]);}
    f(0x80349e88,1);f(0x80349e8c,4);d(0x8034a700,4);d(0x8034a718,9);d(0x8034a728,16);f(0x8034a710,49);f(0x80349f24,3);f(0x80349f28,25);
    f(0x802ab950,1.5f);f(0x802ab958,-0.1f);f(0x802ab968,1);f(0x802ab970,12);
    d(0x8034b7c0,36);d(0x8034b880,49);
    word(b,at(0x8034887c),359);f(0x8034b774,0.0174532924f);return b;
}
void native_scene_source_checks(const awl::WorldMapPlayerStartupProviders& original,const awl::WorldMapPlayerStartupQuery& startup,
    const std::shared_ptr<const awl::WorldMapPlayerCollisionSources>& collision,const awl::WorldMapPlayerSourcesSeed& entry,
    const awl::WorldMapPlayerContextUpdateSources* context,bool allocations){
    using C=awl::WorldMapSceneConstructorStatus;using A=awl::WorldMapSceneAssemblyStatus;using T=awl::WorldMapPlayerStartupTickStatus;using S=awl::WorldMapPlayerSourcesStatus;
    std::shared_ptr<const awl::WorldMapSceneConstructorTables> tables;auto bytes=scene_constructor_dol();
    const auto loaded=context?awl::WorldMapSceneConstructorTables::load(&tables):awl::WorldMapSceneConstructorTables::decode(bytes.data(),bytes.size(),&tables);
    expect(loaded==(context?C::Loaded:C::Decoded) && tables,"constructor sources load bounded DOL data, target-verified for local assets");if(!tables)return;
    if(allocations){const auto* old=tables.get();auto bad=bytes;word(bad,0x48+4,0x8023e000);
        expect(awl::WorldMapSceneConstructorTables::decode(bad.data(),bad.size(),&tables)==C::UnsupportedLayout && tables.get()==old,"overlapping constructor DOL sections preserve earlier provider");
        bad=bytes;word(bad,0x100+(0x80251130-0x8023e000),actor_bits(2));
        expect(awl::WorldMapSceneConstructorTables::decode(bad.data(),bad.size(),&tables)==C::UnsupportedLayout && tables.get()==old,"unsupported initial forward source cannot enter the bounded Y-rotation translation");
        bad=bytes;word(bad,0x13300+(0x802abf88-0x802ab000)+8,0);
        expect(awl::WorldMapSceneConstructorTables::decode(bad.data(),bad.size(),&tables)==C::UnsupportedLayout && tables.get()==old,"missing initialized mode-zero callback cannot be replaced by a stub");
        expect(awl::WorldMapSceneConstructorTables::load(&tables)==C::ReadFailure && tables.get()==old,"absent mounted DOL preserves the earlier unverified constructor provider");}
    if(allocations || (context && startup.initial.phase==0)){
        auto rng_clock=std::make_shared<awl::GameClock>();awl::GameRng shared_rng(rng_clock);uint64_t shared_hash=14695981039346656037ull;
        expect(shared_rng.seed()==0 && !shared_rng.loop_started(),"global RNG begins at verified CRT-zero state before the first outer loop");
        awl::GameRng missing_clock({});expect(!missing_clock.begin_loop() && missing_clock.seed()==0,"missing loop owner cannot consume RNG");
        for(uint32_t i=0;i<128;++i){if(i)expect(rng_clock->advance() && !shared_rng.loop_started(),"next original-equivalent loop invalidates only the RNG receipt");
            expect(shared_rng.begin_loop(),"outer loop consumes its original discarded random word");hash(shared_hash,i);hash(shared_hash,shared_rng.seed());
            const auto receipt_seed=shared_rng.seed();expect(shared_rng.begin_loop() && shared_rng.seed()==receipt_seed,"loop retry preserves RNG sequence");
            for(uint32_t j=0;j<i%4;++j){hash(shared_hash,shared_rng.draw());hash(shared_hash,shared_rng.seed());}}
        expect(shared_hash==0x91947a95ed3ff5f3ull,"128 shared RNG loops with interleaved consumers match complete original instructions");
        uint64_t awake_hash=14695981039346656037ull;
        for(uint32_t i=0;i<1024;++i){const uint32_t raw=i*15485863u,wake=std::array<uint32_t,6>{0,1,59,60,125,UINT32_MAX}[i%6],sleep=std::array<uint32_t,6>{0,1,59,60,125,UINT32_MAX}[i/6%6];
            for(uint32_t v:{i,raw,wake,sleep,uint32_t(awl::world_map_optional_actor_b_awake(raw,wake,sleep))})hash(awake_hash,v);}
        expect(awake_hash==0x7fd25e9237ffd7b7ull,"all 1024 optional B time predicates agree with complete original getters and unsigned comparisons");
        expect(awl::world_map_optional_actor_b_awake(6*36000,0,0) && awl::world_map_optional_actor_b_awake(21*36000,0,0) &&
            !awl::world_map_optional_actor_b_awake(6*36000-1,0,0) && !awl::world_map_optional_actor_b_awake(21*36000+600,0,0),"optional B wake and sleep boundaries are inclusive to the minute");
        uint64_t rng_hash=14695981039346656037ull;
        for(uint32_t i=0;i<1024;++i){const uint32_t before=i*15485863u+0x12345678u;auto seed=before;const auto value=awl::advance_game_rng(&seed);
            for(uint32_t word:{i,before,value,seed})hash(rng_hash,word);}
        expect(rng_hash==0x4cb1feb470048fa9ull,"all 1024 source RNG transitions match complete original instructions");
        uint64_t route_hash=14695981039346656037ull;
        for(int32_t mode=-2;mode<48;++mode)for(uint32_t byte:{0u,1u,255u})for(int32_t prior:{-1,1,3,4}){
            awl::WorldMapSceneModeRequestState scene;scene.prior_mode_60=mode;scene.scene_byte_78=uint8_t(byte);scene.prior_scene_type_6c=prior;scene.scene_type_68=1;
            const auto route=awl::world_map_scene_entry_request(scene);
            for(uint32_t word:{uint32_t(mode),byte,uint32_t(prior),1u,route.requested?0x8010a9c0u:0x80014604u,uint32_t(route.kind),uint32_t(route.from),uint32_t(route.to)})hash(route_hash,word);}
        expect(route_hash==0x7f49679765e02b35ull,"all 600 entry routes match original instructions, distinguishing decimal 28/29/30 from hex aliases");
    }
    awl::WorldMapSceneConstructorState defaults;
    expect(defaults.actor_a_29a64==-1 && defaults.actor_b_29a60==-1 && defaults.actor_3f8==0x2c && defaults.secondary_49==0 &&
        defaults.fixed.selected_variant[0]==3 && defaults.fixed.direct_enabled[0]==0,"real saved/secondary constructors establish absence, default actor and disabled fixed variants");
    awl::WorldMapSceneConstructorProjection projection;projection.camera.target.distance_30=999;
    expect(tables->prepare(collision->movement(),defaults,{1,2,3,4,5},&projection)==C::RequiresHeadingRng && projection.camera.target.distance_30==999,"unknown reached heading RNG source stops without publishing constructor results");
    defaults.actor_3f8=0x30;expect(tables->prepare(collision->movement(),defaults,{1,2,3,4,5},&projection)==C::Prepared && !projection.rng_after && projection.mandatory_heading==std::array<float,3>{0,0,1},"actor ID 0x30 copies forward without reading or advancing unknown RNG");
    projection.rng_after=99u;projection.mandatory_context.payload=std::array<uint32_t,2>{12,34};
    expect(tables->prepare_mandatory(defaults,&projection)==C::Prepared && !projection.rng_after && !projection.mandatory_context.payload,
        "reused constructor proposal cannot retain stale RNG or an unimplemented actor payload");
    if(context && startup.initial.phase==0){uint64_t digest=14695981039346656037ull;
        for(uint32_t i=0;i<128;++i)for(int32_t id:{0x29,0x2a,0x2b,0x27,0x28}){const bool b=id<0x29;awl::WorldMapSceneConstructorState raw;
            raw.held_constructor_11cc8=INT32_MAX;awl::WorldMapOptionalActorSaved saved;saved.position={120,999,168};saved.action=b?0x2b:0x23;
            saved.heading=std::array<std::array<float,3>,4>{{{0,0,1},{1,0,0},{0,0,-1},{-1,0,0}}}[i%4];
            if(b){raw.actor_b_29a60=id;raw.optional_b=saved;}else{raw.actor_a_29a64=id;raw.optional_a=saved;}
            awl::WorldMapOptionalActorProjection actor;expect(tables->prepare_optional(b,collision->movement(),raw,12*36000,1,&actor)==C::Prepared,"target tables derive optional saved-record collision/context projection");
            const auto& m=actor.scene.context;const auto& c=actor.collision.collision;
            for(uint32_t v:{i,uint32_t(id),*m.owner_code_4,uint32_t(actor.scene.scene_type),actor_bits(actor.scene.position[0]),actor_bits(actor.scene.position[2]),
                actor_bits((*m.facing_28)[0]),actor_bits((*m.facing_28)[1]),actor_bits((*m.facing_28)[2]),actor_bits(*m.facing_threshold_38),actor_bits(*m.max_squared_distance_3c),
                uint32_t(c.enabled),c.collision_flags,actor_bits(c.radius),0x12345678u})hash(digest,v);
            expect(!m.payload && (b?actor.scene.position[1]==999:actor.scene.position[1]!=999),"A samples terrain while B retains saved Y; unknown selected payload stays unknown");}
        expect(digest==0x0382431b5f7b4025ull,"640 consumed optional constructor projections agree with original saved-copy/setup tails under documented substitutions");
        std::cout<<"LOCAL_OPTIONAL_CONSTRUCTORS 640 "<<std::hex<<digest<<std::dec<<'\n';}
    if(context && startup.initial.phase==0){uint64_t digest=14695981039346656037ull;
        for(uint32_t seed=0;seed<256;++seed){defaults.actor_3f8=0x30;defaults.byte_299aa=std::array<uint8_t,3>{0,1,255}[seed%3];defaults.byte_299b0=std::array<uint8_t,3>{0,1,255}[seed/3%3];
            expect(tables->prepare(collision->movement(),defaults,{1,2,3,4,5},&projection)==C::Prepared,"all current saved payload bytes prepare reached constructor sources");
            for(uint32_t i=0;i<5;++i){const auto& o=projection.spatial[i];const auto& m=o.context;
                for(uint32_t word:{seed,uint32_t(defaults.byte_299aa),uint32_t(defaults.byte_299b0),i,*m.owner_code_4,*m.owner_kind_8,uint32_t(o.scene_type),actor_bits(o.position[0]),actor_bits(o.position[2]),
                    actor_bits((*m.facing_28)[0]),actor_bits((*m.facing_28)[1]),actor_bits((*m.facing_28)[2]),actor_bits(*m.facing_threshold_38),actor_bits(*m.max_squared_distance_3c),(*m.payload)[0],(*m.payload)[1]})hash(digest,word);}}
        expect(digest==0x87f19bfedd83c2a7ull,"1280 constructor source records match original setup tails/callbacks/payload getters under explicit sqrt and terrain/registration substitutions");
        std::cout<<"LOCAL_CONSTRUCTOR_SOURCES 1280 "<<std::hex<<digest<<std::dec<<'\n';}
    auto providers=original;providers.clock=std::make_shared<awl::GameClock>();awl::WorldMapUpdateInputs inputs(providers.clock);
    auto state=std::make_shared<awl::WorldMapSceneConstructorState>();state->byte_299aa=7;state->rng=std::make_shared<awl::GameRng>(providers.clock);
    auto seed=entry;seed.updates.status.emplace();
    constexpr std::array<uint32_t,6> origins{0,34560000,103680000,207360000,276480000,311040000};
    inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},origins[startup.initial.phase]+432000u,uint8_t{0}},0});
    expect(providers.starts->prepare_fresh_status(inputs.sources().phase->raw_word_0,&*seed.updates.status)==awl::WorldMapPlayerStatusCode::Prepared,"native constructor source workflow prepares status once before scene assembly");
    seed.updates.status->word_28=0x7654;seed.updates.saved_selection=awl::WorldMapPlayerSavedSelection{0u,0u};seed.updates.item_owner=awl::WorldMapPlayerItemOwnerState{};
    std::unique_ptr<awl::WorldMapSceneAssembly> scene;
    const auto create=[&](){return awl::WorldMapSceneAssembly::create_from_sources(providers,seed,startup,inputs,tables,state,collision,&scene);};
    expect(create().status==A::RequiresControl && !scene,"native entry cannot capture its own replacement control");
    inputs.request_scene_mode(4,0);expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured && create().status==A::RequiresSceneCommit && !scene,"source entry waits for actual scene field commit");
    awl::WorldMapSceneModeCommit commit;expect(inputs.commit_scene_fields(0,uint8_t{0},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields,"existing manager commits requested mode four");
    state->rng.reset();
    {const auto position=seed.movement.pose.position;seed.movement.pose.position[0]=NAN;
        expect(create().status==A::PlacementIncomplete && !scene,"initial placement failure precedes unread post-player constructor RNG");seed.movement.pose.position=position;}
    expect(create().constructors==C::RequiresHeadingRng && !scene && inputs.sources().phase->byte_8==1,"missing constructor RNG preserves actual manager/control/phase");state->rng=std::make_shared<awl::GameRng>(providers.clock);
    expect(create().constructors==C::RequiresRngLoop && !scene,"source scene requires the actual pre-callback loop draw, without advancing RNG itself");
    expect(state->rng->seed()==0 && state->rng->begin_loop(),"actual shared RNG starts from CRT zero and advances at the outer-loop boundary");
    const auto loop_seed=state->rng->seed();expect(state->rng->begin_loop() && state->rng->seed()==loop_seed,"same-loop retry cannot consume the discarded outer-loop draw twice");
    state->actor_a_29a64=0x29;expect(create().constructors==C::RequiresOptionalFields && !scene,"optional presence requires the current saved-record inputs");state->actor_a_29a64=-1;
    const auto captured=inputs.control()->dispatch_word;auto result=create();
    if(result.status!=A::AssembledCpuScene)std::cerr<<"native source assembly stop "<<int(result.status)<<" ctor "<<(result.constructors?int(*result.constructors):-1)<<'\n';
    expect(result.status==A::AssembledCpuScene && scene && &scene->inputs()==&inputs && state->secondary_3f4==7 && inputs.sources().phase->byte_8==0 &&
        inputs.control()->dispatch_word==captured,"source assembly borrows the actual manager, publishes constructor callback fields and preserves captured control");if(!scene)return;
    {const auto* old=scene.get();const auto before=state->rng->seed();
        state->held_constructor_11cc8=-1;state->actor_a_29a64=0x29;state->actor_b_29a60=0x28;
        awl::WorldMapOptionalActorSaved a;a.position=seed.movement.pose.position;a.position[0]+=0.2f;a.position[1]=999;a.action=0x23;
        awl::WorldMapOptionalActorSaved b;b.position={650,4,650};b.action=0x2b;state->optional_a=a;state->optional_b=b;
        state->optional_a->action=0;
        expect(create().constructors==C::RequiresOptionalAction && scene.get()==old && state->rng->seed()==before,"reached optional action cannot publish geometry or consume shared RNG prematurely");state->optional_a=a;
        state->held_constructor_11cc8=0x3a;
        expect(create().constructors==C::RequiresOptionalAction && scene.get()==old && state->rng->seed()==before,"held A pose/action override stops at untranslated constructor effects");state->held_constructor_11cc8=-1;
        state->optional_b->special_14e6c=1;
        expect(create().constructors==C::RequiresOptionalAction && scene.get()==old && state->rng->seed()==before,"B special action branch cannot masquerade as completed no-action setup");state->optional_b=b;
        state->optional_a->scene_type=3;
        expect(create().constructors==C::UnsupportedSceneType && scene.get()==old,"untranslated optional terrain category preserves previous scene");state->optional_a=a;
#if !defined(_DEBUG)
        if(allocations){const auto optional_rng=state->rng->seed();size_t rejected=0;bool completed=false;
            for(size_t n=0;n<1024;++n){const auto live=allocation_probe::live;bool failed=false;
                {allocation_probe::remaining=n;allocation_probe::enabled=true;const auto attempt=create();allocation_probe::enabled=false;
                    if(attempt.status==A::AssembledCpuScene)completed=true;else{failed=true;
                        expect((attempt.status==A::AllocationFailure || attempt.status==A::ConstructorIncomplete || attempt.status==A::PlayerIncomplete) && scene.get()==old &&
                            state->rng->seed()==optional_rng && inputs.control()->dispatch_word==captured,"optional factory allocation stop preserves prior scene and the actual shared RNG/control");}}
                if(completed)break;if(failed){++rejected;expect(allocation_probe::live==live,"optional constructor staging failure releases all owned geometry/views");}}
            expect(completed && rejected>25,"optional allocation sweep reaches both actors and post-player publication");std::cout<<"OPTIONAL_FACTORY_ALLOCATION_STOPS "<<rejected<<'\n';}
#endif
        expect(create().status==A::AssembledCpuScene && scene,"native optional constructors assemble from current saved sources");
        if(scene){const auto lists=scene->collision_snapshot();
            expect(lists.first_directional.size()==3 && lists.first_directional[1].world_position==b.position && lists.first_directional[2].world_position[0]==a.position[0],"optional constructor order remains mandatory then B then A");
            expect(scene->initial_placement().pose.position!=seed.movement.pose.position,"derived optional A participates in pre-player initial placement");
            std::cout<<(context?"LOCAL_":"")<<"OPTIONAL_SCENE_CONSTRUCTORS saved A/B / terrain distinction / pre-player placement / action stops\n";}
        state->actor_a_29a64=-1;state->actor_b_29a60=-1;
        expect(create().status==A::AssembledCpuScene && scene,"optional teardown/reconstruction retains actual manager and shared RNG owner");if(!scene)return;
    }
    if(allocations)for(int32_t mode:{28,29,30}){auto routed=providers;routed.clock=std::make_shared<awl::GameClock>();awl::WorldMapUpdateInputs manager(routed.clock);
        manager.observe_control_sources(inputs.sources());manager.request_scene_mode(mode,0);awl::WorldMapSceneModeCommit committed;
        expect(manager.capture_control()==awl::WorldMapControlStatus::Captured && manager.commit_scene_fields(0,uint8_t{0},&committed)==awl::WorldMapSceneFilterStatus::CommittedFields && routed.clock->advance(),"sequence fixture commits actual predecessor mode");
        manager.request_scene_mode(4,0);expect(manager.capture_control()==awl::WorldMapControlStatus::Captured && manager.commit_scene_fields(0,uint8_t{0},&committed)==awl::WorldMapSceneFilterStatus::CommittedFields,"sequence fixture commits actual world-map request");
        const auto rng=state->rng->seed();std::unique_ptr<awl::WorldMapSceneAssembly> stopped;auto sequence_state=std::make_shared<awl::WorldMapSceneConstructorState>(*state);sequence_state->rng.reset();
        expect(awl::WorldMapSceneAssembly::create_from_sources(routed,seed,startup,manager,tables,sequence_state,collision,&stopped).status==A::SequenceRequired && !stopped && state->rng->seed()==rng,
            "actual manager prior mode selects interpreter stop before constructor RNG consumption");}
#if !defined(_DEBUG)
    if(allocations){const auto* old=scene.get();const auto rng=state->rng->seed();size_t rejected=0;bool completed=false;
        for(size_t n=0;n<1024;++n){const auto live=allocation_probe::live;bool failed=false;
            {allocation_probe::remaining=n;allocation_probe::enabled=true;const auto step=create();allocation_probe::enabled=false;
                if(step.status==A::AssembledCpuScene)completed=true;else{failed=true;
                    expect((step.status==A::AllocationFailure || step.status==A::ConstructorIncomplete || step.status==A::PlayerIncomplete) && scene.get()==old && state->rng->seed()==rng &&
                        inputs.control()->dispatch_word==captured && inputs.sources().phase->byte_8==0,"native scene allocation stop preserves prior scene, actual input owner and RNG");}}
            if(completed)break;if(failed){++rejected;expect(allocation_probe::live==live,"native constructor-source failure releases all staging");}}
        expect(completed && rejected>25,"native source allocation sweep crosses terrain, model and scene ownership");std::cout<<"NATIVE_SCENE_SOURCE_ALLOCATION_STOPS "<<rejected<<'\n';}
#else
    (void)allocations;
#endif
    const auto snapshot=scene->collision_snapshot();expect(snapshot.later_resolver.size()==25 && snapshot.first_resolver.size()==1 && snapshot.third_resolver.size()==1,"real constructor sources populate all three scene collision lists");
    for(const auto& o:snapshot.later_resolver){expect(o.data && o.size && o.radius>0 && o.category>=7,"fixed native objects bind retained geometry and local bounds");
        expect(actor_bits(o.world_to_object[3])==0x80000000u && o.object_to_world[0]==1 && o.world_to_object[10]==1,"identity/inverse constructor matrices preserve negative-zero translations");}
    const auto find=[&](uint32_t code)->std::optional<awl::WorldMapSceneObject>{for(int bucket=-1;bucket<=58;++bucket)for(const auto& o:scene->sources().scene().snapshot(bucket))if(o.context.owner_code_4==code)return o;return {};};
    expect(find(0x7b3) && find(0x7b3)->bucket==-1 && find(0x4d) && find(0x4d)->scene_type==1 && find(0x50) && find(0x50)->context.payload==std::array<uint32_t,2>{0x32,0x31},"constructor-derived spatial types, membership and virtual payloads replace invented fixture categories");
    auto& player=scene->player();auto& owner=scene->sources();const auto* status_owner=&owner.updates();
    expect(scene->observe_startup_camera({})==S::Busy,"native constructor camera source cannot be replaced by a supplied result");
    awl::WorldMapPlayerStartupTickInput tick;tick.secondary.secondary_byte_3f2=uint8_t{0};tick.secondary.item_present=false;
    tick.actor.actor.saved_byte_29a01=uint8_t{0};tick.actor.actor.script.emplace();tick.actor.actor.script->flag_width_4=1;tick.actor.actor.script->flag_bytes={1};awl::WorldMapPlayerActorQueue render;
    for(unsigned i=0;i<64;++i){if(i){render.clear();expect(providers.clock->advance() && state->rng->begin_loop() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"native source workflow advances shared clock and pre-callback RNG");}
        const auto step=scene->advance_startup(tick,&render);expect(step.status==(i==63?T::TransitionRequired:T::Completed),"constructor-sourced camera/status feed complete ordered startup ticks");
        if(step.status!=T::Completed && step.status!=T::TransitionRequired)return;}
    expect(scene->advance_entry(uint8_t{255})==awl::WorldMapSceneEntryStatus::Released && scene->advance_entry()==awl::WorldMapSceneEntryStatus::Completed &&
        scene->advance_state0_transition({},{}).status==awl::WorldMapPlayerState0TransitionStatus::Advanced,"retained native secondary entry gate supersedes supplied overrides and drives checked zero transition");
    if(context){awl::WorldMapPlayerContextUpdateState ui;ui.ui.resource_manager_1278=context->icons->resources->identity();ui.ui.labels_60={0u,0u,0u,0u};
        for(auto& e:ui.ui.elements){e.emplace();e->flags_120=0;e->attachments_10c_110={uint64_t{0},uint64_t{0}};}expect(player.seed_state0_context(ui),"native source workflow seeds remaining explicit UI observations");}
    expect(owner.advance_third(player,{},{}).status==S::Completed && owner.consume_queued(player,tick.actor,nullptr).status==S::Completed,"same native status owner completes final paused startup queue");
    const auto fields=owner.fields();
    for(unsigned i=0;i<8;++i){expect(providers.clock->advance() && state->rng->begin_loop() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"native source normal loop advances shared RNG and captures current control");
        inputs.observe_pad_sample({true,0,int8_t(i<3?0:80)});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"native source normal loop captures recorded PAD");
        expect(owner.advance_primary(player).status==S::Completed,"constructor-derived scene supports normal primary/model callback");const auto second=owner.advance_secondary(player);
        if(second.state0_secondary && second.state0_secondary->status==awl::WorldMapPlayerState0SecondaryStatus::TransitionRequired)expect(owner.advance_transition(player).status==S::Completed,"source scene reaches checked state-one initialization");else expect(second.status==S::Completed,"source scene completes supported normal second");
        awl::WorldMapPlayerState0ThirdInput third;if(context){third.context.emplace();third.context->current_key=17;third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};
            third.context->scene_predicate_1728e4=uint8_t{0};third.context->label_observations.terrain_clear={true,true};third.context->label_observations.secondary_ground_available=true;
        }else{auto skip=fields;skip.guards=awl::WorldMapPlayerUpdateGuards{0,{}};owner.observe_fields(skip);}
        state->byte_299aa=uint8_t(i%2);state->byte_299b0=uint8_t((i+1)%2);
        expect(owner.advance_third(player,context?*context:awl::WorldMapPlayerContextUpdateSources{},third).status==S::Completed,"current saved source bytes drive native scene context callbacks");owner.observe_fields(fields);
        expect(find(0x4e)->context.payload==std::array<uint32_t,2>{0x2f,state->byte_299aa?0x41u:0x40u} && find(0x4f)->context.payload==std::array<uint32_t,2>{0x30,state->byte_299b0?0x43u:0x42u} && state->secondary_3f4==7,"virtual payloads read current bytes while constructor's separate copied secondary field stays unchanged");
        expect(owner.consume_queued(player,tick.actor,nullptr).status==S::Completed && &owner.updates()==status_owner && owner.updates().status->word_28==0x7654,"native constructors preserve status ownership through normal callbacks and queue completion");}
    expect(player.state().state_1364==1,"constructor-sourced native scene reaches state one without resetting saved status");
    std::cout<<(context?"LOCAL_":"")<<"NATIVE_SCENE_SOURCES 25 fixed / 5 spatial / retained manager + saved sources / 64 startup + 8 normal loops\n";
}
void scene_assembly_checks(const awl::WorldMapPlayerStartupProviders& source,const awl::WorldMapPlayerStartupQuery& startup,
    const std::shared_ptr<awl::WorldMapCameraCollisionAsset>& camera,const awl::WorldMapCameraViewQuery& view,
    const awl::WorldMapPlayerContextUpdateSources* context,bool allocations){
    using A=awl::WorldMapSceneAssemblyStatus;using S=awl::WorldMapPlayerSourcesStatus;using T=awl::WorldMapPlayerStartupTickStatus;
    using E=awl::WorldMapSceneEntryStatus;
    auto providers=source;providers.clock=std::make_shared<awl::GameClock>();
    constexpr std::array<uint32_t,6> origins{0,34560000,103680000,207360000,276480000,311040000};
    awl::WorldMapSceneAssemblyQuery q;q.control={awl::WorldMapControlPhase{uint8_t{1},origins[startup.initial.phase]+123456u,uint8_t{0}},0};
    q.startup=startup;q.camera=view;q.saved_actor_a_29a64=-1;q.saved_actor_b_29a60=-1;q.previous_mode=0;
    auto& seed=q.player;seed.movement.scene_identity=0x701;seed.movement.pose={1,{20,4,20},{0,0,1}};
    seed.updates.saved_pose=seed.movement.pose;seed.updates.scene_one_position_240=std::array<float,3>{};
    seed.updates.queue={{900,0x1011}};seed.updates.item_owner=awl::WorldMapPlayerItemOwnerState{0};
    seed.updates.saved_selection=awl::WorldMapPlayerSavedSelection{0u,0u};
    awl::WorldMapPlayerStatusState status;expect(providers.starts->prepare_fresh_status(q.control.phase->raw_word_0,&status)==awl::WorldMapPlayerStatusCode::Prepared,"scene observes one prepared saved/status lifecycle");
    status.word_28=0x4567;seed.updates.status=status;
    seed.fields.guards=awl::WorldMapPlayerUpdateGuards{-1,0};seed.fields.terrain_variant_299a4=uint8_t{0};seed.fields.static_flags.emplace();
    seed.fields.secondary_byte_3f1=uint8_t{0};seed.fields.saved_remap_byte_14d35=uint8_t{0};seed.fields.audio_byte_90=uint8_t{0};
    expect(awl::WorldMapPlayerCollisionSources::load({*q.control.phase->raw_word_0,0},camera,true,&q.collision)==S::Completed,"scene owns retained object pools and terrain/camera slots");
    if(!q.collision)return;
    if(context){awl::CollisionSurfaceSample surface;const auto& bytes=q.collision->movement().terrain_bytes();
        expect(awl::sample_type1_collision_surface(bytes.data(),bytes.size(),120,168,&surface),"scene uses an explicitly supplied local terrain patch");
        seed.movement.pose.position={120,surface.height,168};seed.updates.saved_pose=seed.movement.pose;}
    constexpr awl::CollisionAffineTransform identity{1,0,0,0,0,1,0,0,0,0,1,0};
    q.fixed_state.selected_variant.fill(3);q.fixed_state.direct_enabled[0]=1;
    for(size_t i=0;i<25;++i){auto& o=q.fixed_objects[i];o.collision.identity=100+i;o.collision.radius=1000;
        o.collision.world_to_object=o.collision.object_to_world=identity;}
    for(size_t i=0;i<5;++i){auto& o=q.spatial_objects[i];o.scene.identity=200+i;o.scene.scene_type=int32_t(3+i);
        o.scene.position={600,4,600};o.positioned=i!=0;}
    q.mandatory_actor.object.scene.identity=300;q.mandatory_actor.collision.collision.identity=300;q.mandatory_actor.collision.heading_axis={0,0,1};
    std::unique_ptr<awl::WorldMapSceneAssembly> scene;
    auto result=awl::WorldMapSceneAssembly::create(providers,q,&scene);
    if(result.status!=A::AssembledCpuScene)std::cerr<<"scene create stop "<<int(result.status)<<" placement "<<(result.placement?int(*result.placement):-1)<<" player "<<(result.player?int(result.player->status):-1)<<'\n';
    expect(result.status==A::AssembledCpuScene && scene,"complete bounded CPU scene assembles atomically");if(!scene)return;
    auto* initial=scene.get();const auto initial_status=update_owners_snapshot(scene->sources().updates());
    const auto preserved=[&](){return scene.get()==initial && update_owners_snapshot(scene->sources().updates())==initial_status;};
    auto bad=q;bad.saved_actor_a_29a64.reset();expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::RequiresActorPresence && preserved(),"unknown saved actor presence cannot be guessed");
    bad=q;bad.saved_actor_a_29a64=0x29;expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::RequiresPreparedActor && preserved(),"reached untranslated optional constructor requires prepared observations");
    bad=q;bad.fixed_objects[0].collision.identity=seed.movement.scene_identity;expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::InvalidInput && preserved(),"cross-list identity alias rolls back complete scene");
    bad=q;bad.player.updates.status.reset();expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::RequiresStatus && preserved(),"observed status cannot be silently reconstructed");
    bad=q;bad.previous_mode=29;expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::SequenceRequired && preserved(),"reached interpreter entry stops before publishing any scene");
    for(int32_t mode:{4,6,28,29,30}){bad=q;bad.previous_mode=mode;bad.sequence_request_byte=uint8_t{255};
        expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::SequenceRequired && preserved(),"each reached sequence route requires actual interpreter work");}
    bad=q;bad.previous_mode=4;expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::RequiresEntrySources && preserved(),"previous mode four cannot invent request byte");
    bad=q;bad.fixed_objects[0].collision.radius=-1;expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::InvalidInput && preserved(),"invalid prepared fixed bounds cannot hide behind a different category");
    bad=q;bad.player.fields.guards->word_58c=1;expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::UnsupportedEntryGate && preserved(),"blocked setup cannot invent fade/control release");
    bad=q;bad.player.movement.pose.position[0]=std::numeric_limits<float>::quiet_NaN();
    expect(awl::WorldMapSceneAssembly::create(providers,bad,&scene).status==A::PlacementIncomplete && preserved(),"failed initial placement preserves previous full scene");
    {auto fresh=q;fresh.status_mode=awl::WorldMapPlayerSourcesInitialization::Fresh;fresh.player.updates.status.reset();fresh.player.updates.saved_selection.reset();
        std::unique_ptr<awl::WorldMapSceneAssembly> other;
        expect(awl::WorldMapSceneAssembly::create(providers,fresh,&other).status==A::AssembledCpuScene && other && other->sources().updates().status->word_28!=0x4567,
            "fresh construction occurs once before the player, while observed construction preserves current saved fields");}
#if !defined(_DEBUG)
    if(allocations){size_t rejected=0;bool completed=false;
        for(size_t n=0;n<1024;++n){const auto live=allocation_probe::live;bool failed=false;
            {allocation_probe::remaining=n;allocation_probe::enabled=true;const auto step=awl::WorldMapSceneAssembly::create(providers,q,&scene);allocation_probe::enabled=false;
                if(step.status==A::AssembledCpuScene)completed=true;else{failed=true;expect((step.status==A::AllocationFailure || step.status==A::PlayerIncomplete) && preserved(),"every scene factory allocation stop preserves previous scene/status/queue");}}
            if(completed)break;if(failed){++rejected;expect(allocation_probe::live==live,"failed complete scene staging releases retained resources and views");}}
        expect(completed && rejected>25,"scene allocation sweep reaches player, scene, lists and final fade setup");std::cout<<"SCENE_ASSEMBLY_ALLOCATION_STOPS "<<rejected<<'\n';}
#else
    (void)allocations;
#endif
    auto snapshot=scene->collision_snapshot();
    expect(snapshot.later_resolver.size()==25 && snapshot.first_resolver.size()==1 && snapshot.first_resolver[0].identity==300 &&
        snapshot.third_resolver.size()==1 && snapshot.third_resolver[0].identity==0x701,"final scene has fixed later objects, mandatory first actor and separate third-list player");
    for(size_t i=0;i<25;++i)expect(snapshot.later_resolver[i].identity==124-i && snapshot.later_resolver[i].category==(i==0?13:7),"fixed head insertion retains reverse construction order and category filtering");
    expect(scene->sources().scene().object(200)->bucket==-1 && scene->sources().scene().object(201)->bucket==18 &&
        scene->sources().scene().object(300)->position==snapshot.first_resolver[0].center_world,"unpositioned and positioned context objects retain distinct construction paths");
    // Reconstruct only the expected PRE-player query from the final snapshot,
    // excluding the actor that is constructed later. Include it separately to
    // prove the fixture detects an incorrectly early mandatory registration.
    if(!context){auto overlap=q;overlap.player.movement.pose.position={263.2f,4,179};std::unique_ptr<awl::WorldMapSceneAssembly> other;
        expect(awl::WorldMapSceneAssembly::create(providers,overlap,&other).status==A::AssembledCpuScene && other &&
            other->initial_placement().pose.position==overlap.player.movement.pose.position,"nearby mandatory actor cannot displace initial player before it is constructed");
        if(other){const auto final_objects=other->collision_snapshot();awl::CollisionWorldMapInitialPlacementQuery early;
            const auto& terrain=q.collision->movement().terrain_bytes();const auto& statics=q.collision->movement().static_bytes();
            early.terrain_data=terrain.data();early.terrain_size=terrain.size();early.static_data=statics.data();early.static_size=statics.size();
            early.list_92d8=final_objects.first_resolver.data();early.list_92d8_count=final_objects.first_resolver.size();awl::CollisionWorldMapInitialPlacement displaced;
            expect(awl::resolve_type1_world_map_initial_placement(early,1,overlap.player.movement.pose.position,&displaced) && displaced.position!=other->initial_placement().pose.position,
                "ordering fixture detects incorrectly including the mandatory actor during initial placement");}}
    {auto pair=q;pair.saved_actor_a_29a64=0x29;pair.saved_actor_b_29a60=0x27;
        for(size_t i=0;i<2;++i){awl::WorldMapPreparedSceneActor actor;actor.object.scene.identity=400+i;actor.object.scene.scene_type=1;actor.object.scene.position={650+float(i),4,650};actor.object.positioned=true;
            if(i==0){actor.object.scene.position=pair.player.movement.pose.position;actor.object.scene.position[0]+=0.2f;}
            actor.collision.collision.identity=400+i;actor.collision.collision.category=1;actor.collision.collision.collision_flags=1;actor.collision.collision.enabled=true;actor.collision.collision.radius=0.3f;
            actor.collision.world_position=actor.collision.collision.center_world=actor.object.scene.position;actor.collision.heading_axis={0,0,1};if(i==0)pair.actor_a=actor;else pair.actor_b=actor;}
        std::unique_ptr<awl::WorldMapSceneAssembly> other;const auto step=awl::WorldMapSceneAssembly::create(providers,pair,&other);
        expect(step.status==A::AssembledCpuScene && other,"both observed optional constructors assemble before player");if(other){const auto list=other->collision_snapshot();
            expect(other->initial_placement().pose.position!=pair.player.movement.pose.position,"pre-player optional A already participates in placement collision");
            expect(list.first_resolver.size()==3 && list.first_resolver[0].identity==300 && list.first_resolver[1].identity==401 && list.first_resolver[2].identity==400,"mandatory/B/A first-list order follows original head insertion");
            expect(other->observe_actor(400,{660,4,660},{1,0,0},true,{})==S::Completed && other->collision_snapshot().first_resolver[2].identity==400,
                "refreshing optional A cannot move it ahead of B or the mandatory actor");}}
    awl::WorldMapPlayerContextMetadata metadata;metadata.owner_code_4=55;metadata.owner_kind_8=0;
    expect(scene->observe_actor(300,{620,4,620},{1,0,0},true,metadata)==S::Completed && scene->collision_snapshot().first_resolver[0].center_world==std::array<float,3>{620,4,620} &&
        scene->sources().scene().object(300)->position==std::array<float,3>{620,4,620} && scene->sources().scene().object(300)->context.owner_code_4==55u,"actor producer updates both collision/context views without moving list nodes");
    expect(scene->observe_actor(300,{NAN,4,620},{1,0,0},true,{})==S::InvalidInput && scene->collision_snapshot().first_resolver[0].center_world[0]==620,"bad live actor observation preserves both views");
#if !defined(_DEBUG)
    if(allocations){size_t rejected=0;bool completed=false;for(size_t n=0;n<64;++n){const auto live=allocation_probe::live;
        allocation_probe::remaining=n;allocation_probe::enabled=true;const auto step=scene->observe_actor(300,{621,4,620},{1,0,0},true,metadata);allocation_probe::enabled=false;
        if(step==S::Completed){completed=true;break;}++rejected;
        expect(step==S::AllocationFailure && allocation_probe::live==live && scene->collision_snapshot().first_resolver[0].center_world[0]==620 &&
            scene->sources().scene().object(300)->position[0]==620,"live actor allocation stops preserve collision and context atomically");}
        expect(completed && rejected>0,"live actor sweep reaches both staged registries");std::cout<<"SCENE_ACTOR_ALLOCATION_STOPS "<<rejected<<'\n';}
#endif
    auto& player=scene->player();auto& inputs=scene->inputs();auto& owner=scene->sources();const auto* owners=&owner.updates();
    expect(owner.updates().status->word_28==0x4567 && inputs.sources().phase->byte_8==0 && inputs.control(),"observed sentinel status survives construction and fade clear preserves captured entry control");
    expect(scene->advance_entry(uint8_t{0})==E::RequiresFade,"scene continuation cannot precede owned fade callback");
    awl::WorldMapPlayerStartupTickInput tick;tick.secondary.secondary_byte_3f2=uint8_t{0};tick.secondary.item_present=false;
    tick.actor.actor.saved_byte_29a01=uint8_t{0};tick.actor.actor.script.emplace();tick.actor.actor.script->flag_width_4=1;tick.actor.actor.script->flag_bytes={1};
    awl::WorldMapPlayerActorQueue render{{11,0x2002}};
#if !defined(_DEBUG)
    if(allocations){const auto before=update_owners_snapshot(owner.updates());const auto live=allocation_probe::live;
        allocation_probe::remaining=0;allocation_probe::enabled=true;const auto step=scene->advance_startup(tick,&render);allocation_probe::enabled=false;
        expect(step.status==T::AllocationFailure && allocation_probe::live==live && update_owners_snapshot(owner.updates())==before && !player.primary_loop(),
            "startup observation-copy failure preserves every callback and status before retry");}
#endif
    {auto wrong=view;wrong.target.distance_30=0;expect(scene->observe_startup_camera(wrong)==S::Completed,"unbound camera observation can be corrected explicitly");
        const auto before=update_owners_snapshot(owner.updates());expect(scene->advance_startup(tick,&render).status==T::PrimaryIncomplete && update_owners_snapshot(owner.updates())==before && !player.camera(),
            "invalid reached camera preserves startup/status/queue for a same-loop retry");expect(scene->observe_startup_camera(view)==S::Completed,"corrected camera enables retry without rebuilding status");}
    for(unsigned sample=0;sample<64;++sample){if(sample){render.erase(std::remove_if(render.begin(),render.end(),[&](const auto& e){return e.node_identity==scene->fade().render_node_identity();}),render.end());
            expect(providers.clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"scene startup advances explicit shared clock and captures current control");}
        auto step=scene->advance_startup(tick,&render);
        if(step.status!=(sample==63?T::TransitionRequired:T::Completed))std::cerr<<"scene tick "<<sample<<" stop "<<int(step.status)<<" passes "<<(step.passes?int(step.passes->status):-1)<<'\n';
        expect(step.status==(sample==63?T::TransitionRequired:T::Completed) && &owner.updates()==owners && owner.updates().status->word_28==0x4567,"startup retains same status/update owner, sentinel and ordered CPU completion");
        if(step.status!=T::Completed && step.status!=T::TransitionRequired)return;
        if(sample==0){expect(scene->observe_startup_camera(view)==S::Busy,"accepted camera binding cannot be silently reseeded");
            const auto before=update_owners_snapshot(owner.updates());expect(scene->advance_startup({},nullptr).status==T::Completed && update_owners_snapshot(owner.updates())==before,"same-loop scene retry reuses accepted startup work");}
        if(sample<63)expect(scene->advance_entry({})==E::WaitingForFade,"fade keeps phase disabled without requiring unread secondary byte");
    }
    const auto current_control=inputs.control()->dispatch_word;
    expect(scene->advance_entry({})==E::RequiresSecondaryByte && inputs.sources().phase->byte_8==0,"missing reached +49 leaves one-shot continuation pending");
    expect(scene->advance_entry(uint8_t{0})==E::Released && inputs.sources().phase->byte_8==1 && inputs.capture_control()==awl::WorldMapControlStatus::AlreadyCaptured &&
        inputs.control()->dispatch_word==current_control && scene->advance_entry(uint8_t{255})==E::Completed,"fade completion enables only future captured control and consumes continuation once");
    const auto status_before=update_owners_snapshot(owner.updates());
    expect(scene->advance_state0_transition({},{}).status==awl::WorldMapPlayerState0TransitionStatus::Advanced && player.state().state_1364==0 &&
        update_owners_snapshot(owner.updates())==status_before && &owner.updates()==owners,"29-to-zero transition preserves current status and queued node without Fresh reinitialization");
    awl::WorldMapPlayerStartupQueuedActorInput actor=tick.actor;
    if(context){awl::WorldMapPlayerContextUpdateState initial_context;initial_context.ui.resource_manager_1278=context->icons->resources->identity();initial_context.ui.labels_60={0u,0u,0u,0u};
        for(auto& e:initial_context.ui.elements){e.emplace();e->flags_120=0;e->attachments_10c_110={uint64_t{0},uint64_t{0}};}
        expect(player.seed_state0_context(initial_context),"populated scene seeds explicit UI observations before active normal callbacks");}
    expect(owner.advance_third(player,{},{}).status==S::Completed && owner.consume_queued(player,actor,nullptr).status==S::Completed,"paused final startup loop completes zero third and queued callbacks using same owner");
    if(context){auto& a=actor.actor;a.script->flag_bytes[0]=0;a.script->records.emplace();a.derive_normal_visibility=true;
        a.frame.actor_placement=awl::WorldMapPlayerActorPlacement{{9999,9999,9999},{1,0,0}};
        a.frame.node_post_transforms.resize(player.model().primary().model().core().nodes.size());a.visibility.emplace();a.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
        for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;plane.normal[i/2]=i%2?-1.0f:1.0f;a.visibility->planes[i]=plane;}
        a.screen.emplace();a.screen->view=*a.visibility->view;a.screen->half_extents={1000.0f,1000.0f};actor.effect_gate=uint8_t{0};}
    auto fields=owner.fields();unsigned zero_ticks=0;
    const auto finish=[&](){awl::WorldMapPlayerState0ThirdInput third;
        if(context){third.context.emplace();third.context->current_key=17;third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};
            third.context->scene_predicate_1728e4=uint8_t{0};third.context->label_observations.terrain_clear={true,true};third.context->label_observations.secondary_ground_available=true;
        }else{auto skip=fields;skip.guards=awl::WorldMapPlayerUpdateGuards{0,{}};owner.observe_fields(skip);}
        const auto third_step=owner.advance_third(player,context?*context:awl::WorldMapPlayerContextUpdateSources{},third);owner.observe_fields(fields);
        render.erase(std::remove_if(render.begin(),render.end(),[&](const auto& e){return e.node_identity==player.render_node_identity() || e.node_identity==player.effect_node_identity();}),render.end());
        expect(third_step.status==S::Completed && owner.consume_queued(player,actor,context?&render:nullptr).status==S::Completed,"assembled scene completes normal third and supplied actor queue");
        if(context)expect(player.model().primary().cpu().frame() && player.model().primary().cpu().frame()->skin_executed &&
            player.model().primary().cpu().frame()->root_matrix[3]==owner.movement().pose.position[0],"assembled scene skins actual local player at current owned pose");};
    for(unsigned loop=0;loop<8;++loop){expect(providers.clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"scene captures next normal loop after released fade");
        inputs.observe_pad_sample({true,0,int8_t(loop<3?0:80)});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"scene supplies repeatable neutral/moving PAD");
        expect(owner.advance_primary(player).status==S::Completed,"assembled collision/context sources drive normal primary/model callback");
        auto second=owner.advance_secondary(player);
        if(player.state().state_1364==0){++zero_ticks;
            if(second.state0_secondary && second.state0_secondary->status==awl::WorldMapPlayerState0SecondaryStatus::TransitionRequired)
                expect(owner.advance_transition(player).status==S::Completed,"assembled source owner checks zero-to-one initializer after accepted state-zero second");
            else expect(second.status==S::Completed,"long-duration fixture moves in zero before changing state");
        }else expect(second.status==S::Completed,"assembled first/later/third lists drive active state-one second");
        finish();expect(&owner.updates()==owners && owner.updates().status->word_28==0x4567 && owner.updates().saved_selection->item_144==0u,
            "initialization through normal zero/one retains current status/selection storage");
    }
    expect(player.state().state_1364==1 && zero_ticks>0 && owner.updates().queue.size()==1 && owner.updates().queue[0].node_identity==900,"complete scene path reaches one and preserves unrelated queued node");
    expect(scene->collision_snapshot().third_resolver[0].center_world==owner.movement().pose.position &&
        scene->collision_snapshot().third_objects[0].heading_axis==owner.movement().pose.heading,"linked player third-list views follow current pose after state-one movement");
    if(!context && allocations){auto blocked_providers=providers;blocked_providers.clock=std::make_shared<awl::GameClock>();std::unique_ptr<awl::WorldMapSceneAssembly> blocked;
        expect(awl::WorldMapSceneAssembly::create(blocked_providers,q,&blocked).status==A::AssembledCpuScene && blocked,"secondary-blocked scene owns an independent entry clock");
        if(blocked){awl::WorldMapPlayerActorQueue blocked_render;
            for(unsigned sample=0;sample<64;++sample){if(sample){blocked_render.clear();expect(blocked_providers.clock->advance() && blocked->inputs().capture_control()==awl::WorldMapControlStatus::Captured,"blocked scene captures next loop");}
                const auto step=blocked->advance_startup(tick,&blocked_render);expect(step.status==(sample==63?T::TransitionRequired:T::Completed),"secondary-blocked fixture reaches real owned fade completion");}
            expect(blocked->advance_entry({})==E::RequiresSecondaryByte && blocked_providers.clock->advance(),"missing continuation source can arrive after fade has unregistered itself");
            expect(blocked->advance_entry(uint8_t{255})==E::RequiresControl && blocked->inputs().capture_control()==awl::WorldMapControlStatus::Captured,
                "later scene continuation still requires capture before scene work");
            expect(blocked->advance_entry(uint8_t{255})==E::Completed && blocked->inputs().sources().phase->byte_8==0 &&
                blocked->advance_entry(uint8_t{0})==E::Completed && blocked->inputs().sources().phase->byte_8==0,"nonzero +49 consumes continuation without enabling control, even when later cleared");}}
    std::cout<<(context?"LOCAL_":"")<<"SCENE_ASSEMBLY 25 fixed / ordered optional actors / initial placement / 64 startup ticks / "<<zero_ticks<<" zero ticks / normal one / retained status\n";
    const auto* old_scene=scene.get();
    expect(awl::WorldMapSceneAssembly::create_from_sources(providers,q.player,startup,scene->inputs(),{},{},q.collision,&scene).status==A::InvalidInput && scene.get()==old_scene,
        "source reassembly cannot destroy the input owner it would borrow from a diagnostic scene");
    native_scene_source_checks(source,startup,q.collision,q.player,context,allocations);
}
void startup_tick_owner_checks(const awl::WorldMapPlayerStartupProviders& source,const awl::WorldMapPlayerStartupQuery& query,
    const std::shared_ptr<awl::WorldMapCameraCollisionAsset>& camera,const awl::WorldMapCameraViewQuery& view,bool allocations,
    const awl::WorldMapPlayerContextUpdateSources* context_sources,bool owned_sources) {
    using S=awl::WorldMapPlayerStartupTickStatus;using F=awl::WorldMapFadeStatus;using P=awl::WorldMapPlayerPrimaryPassesStatus;
    using Q=awl::WorldMapPlayerStartupQueueStatus;
    auto providers=source;providers.clock=std::make_shared<awl::GameClock>();
    std::unique_ptr<awl::WorldMapPlayerStartup> player;
    expect(awl::construct_world_map_player_startup(providers,query,&player).status==awl::WorldMapPlayerStartupStatus::ConstructedCpuStartup && player,
        "startup-tick fixture owns primary/secondary models on a private shared clock");if(!player)return;
    awl::WorldMapFade fade(providers.clock),other_fade(providers.clock),foreign_fade(std::make_shared<awl::GameClock>());
    expect(fade.start_world_map_entry()==F::Started,"startup-tick fade starts from the bounded clear setup branch");
    awl::WorldMapUpdateInputs inputs(providers.clock),missing(providers.clock);
    using Third=awl::WorldMapPlayerState0ThirdStatus;
    expect(!player->seed_state0_context({}) && player->advance_state0_third(inputs,{},{}).status==Third::RequiresControl,
        "startup state29 cannot seed state-zero context or bypass captured control");
    constexpr std::array<uint32_t,6> phase_origins{0,34560000,103680000,207360000,276480000,311040000};
    const awl::WorldMapControlSources phase_sources{awl::WorldMapControlPhase{uint8_t{1},phase_origins[query.initial.phase]+123456u,uint8_t{0}},0};
    inputs.observe_control_sources(phase_sources);
    awl::WorldMapPlayerUpdateOwners owners;owners.scene_one_position_240=std::array<float,3>{};owners.queue={{900,0x1011}};
    expect(player->initialize_fresh_status(inputs,&owners)==awl::WorldMapPlayerStatusCode::Prepared,"startup tick initializes pointed status using the retained phase source");
    awl::WorldMapPlayerActorQueue render{{11,0x2002},{12,0xcffe},{13,UINT32_MAX}};
    awl::WorldMapPlayerStartupTickInput input;input.parent.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.parent.scene_child_word_4=0;
    input.parent.pose={1,{2,0,0},{0,0,1}};input.parent.action_148=0;input.parent.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera,view};
    auto before=startup_snapshot(*player),fade_before=fade_snapshot(fade),effects=update_owners_snapshot(owners),draw=actor_queue_snapshot(render);
    const auto preserved=[&](){return startup_snapshot(*player)==before && fade_snapshot(fade)==fade_before && update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw;};
    expect(player->advance_state29_startup_tick(inputs,input,nullptr,&owners,&render).status==S::InvalidInput &&
        player->advance_state29_startup_tick(inputs,input,&foreign_fade,&owners,&render).status==S::InvalidInput &&
        player->advance_state29_startup_tick(inputs,input,&fade,nullptr,&render).status==S::InvalidInput && preserved(),
        "absent/foreign required tick owners reject before any callback");
    expect(player->advance_state29_startup_tick(inputs,input,&other_fade,&owners,&render).status==S::FadeIncomplete && preserved(),
        "unstarted constructor fade cannot silently bypass the justified world-map entry sequence");
    auto tick=player->advance_state29_startup_tick(missing,input,&fade,&owners,&render);
    expect(tick.status==S::PrimaryIncomplete && tick.primary && !tick.fade && preserved(),"missing captured control stops before parent/model/fade work");
    expect(inputs.capture_control()==awl::WorldMapControlStatus::Captured,"startup tick captures active control once");
    expect(player->advance_pending_state0_transition(inputs,{},&fade,&owners).status==awl::WorldMapPlayerState0TransitionStatus::RequiresPendingTransition,
        "state-zero transition cannot precede its owned second-pass request");
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,nullptr);
    expect(tick.status==S::FadeIncomplete && tick.primary && tick.primary->status==awl::WorldMapPlayerPrimarySliceStatus::Completed &&
        tick.fade==F::RequiresRenderQueue && player->primary_loop()->fade_owner==fade.primary_node_identity() && !fade.loop() &&
        player->state().counter_1368==1 && owners.queue.size()==2,"late fade dependency preserves accepted parent/model and binds the owned retention source");
    input.parent={};input.parent.pose.position[0]=std::numeric_limits<float>::quiet_NaN();input.parent.player_node_identity=99;
    before=startup_snapshot(*player);fade_before=fade_snapshot(fade);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    awl::WorldMapUpdateInputs alternate_inputs(providers.clock);
    alternate_inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{0}},0});
    expect(alternate_inputs.capture_control()==awl::WorldMapControlStatus::Captured &&
        player->advance_state29_startup_tick(alternate_inputs,input,&fade,&owners,&render).status==S::PrimaryIncomplete && preserved(),
        "a second control capture on the same clock cannot replace the accepted tick's original control owner");
    expect(other_fade.start_world_map_entry()==F::Started,"replacement fixture uses a complete started fade on the same clock");
    expect(player->advance_state29_startup_tick(inputs,input,&other_fade,&owners,&render).status==S::InvalidInput && preserved(),
        "accepted startup prefix cannot rebind its fade source before retry");
    bool fade_accepted=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t failed=0;
        for(size_t fail=0;fail<8;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);allocation_probe::enabled=false;
            if(tick.status==S::PassesIncomplete){fade_accepted=true;break;}++failed;
            expect(tick.status==S::FadeIncomplete && tick.fade==F::AllocationFailure && preserved() && allocation_probe::live==live,
                "composed fade queue allocation failures retain accepted parent/model/source and release staging");}
        expect(fade_accepted && failed==2,"both composed fade queue allocation stages preserve the earlier accepted prefix");
        std::cout<<"PLAYER_STARTUP_TICK_FADE_ALLOCATION_FAILURES "<<failed<<'\n';}
#else
    (void)allocations;
#endif
    if(!fade_accepted)tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::PassesIncomplete && tick.passes && tick.passes->status==P::SecondaryIncomplete && fade.loop() &&
        fade.state().mode_60==1 && render.size()==4 && render[1].node_identity==fade.render_node_identity() && player->state().counter_1368==1,
        "owned fade progresses and schedules before missing player second-pass observations stop");
    before=startup_snapshot(*player);fade_before=fade_snapshot(fade);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    input.secondary.guards=awl::WorldMapPlayerUpdateGuards{0,{}};input.secondary.mode_word_60=99;
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::PassesIncomplete && tick.passes && tick.passes->status==P::TertiaryIncomplete && preserved(),
        "blocked second-pass guard skips even a contradictory mode override before the independent third-pass guard");
    input.secondary.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.secondary.mode_word_60=0;
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::InvalidInput && tick.fade==F::AlreadyAdvanced && !tick.passes && preserved(),
        "reached diagnostic override cannot replace the actual owned fade retention word");input.secondary.mode_word_60.reset();
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::PassesIncomplete && tick.passes && tick.passes->status==P::TertiaryIncomplete && preserved(),
        "late third-pass dependency does not repeat accepted fade, counters, status, playback or queue insertion");
    input.tertiary_guards=awl::WorldMapPlayerUpdateGuards{-1,0};
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::QueueIncomplete && tick.queued && tick.queued->status==Q::CallbackIncomplete &&
        player->primary_loop()->followups_completed && !player->primary_loop()->queued_completed,
        "late actor failure preserves all accepted earlier callbacks and pending owned update");
    before=startup_snapshot(*player);fade_before=fade_snapshot(fade);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    {auto other=render;tick=player->advance_state29_startup_tick(inputs,{},&fade,&owners,&other);
        expect(tick.status==S::FadeIncomplete && tick.fade==F::InvalidInput && preserved(),"accepted fade render owner cannot be rebound on a frame retry");}
    input.actor.actor.saved_byte_29a01=uint8_t{0};input.actor.actor.script.emplace();input.actor.actor.script->flag_width_4=1;input.actor.actor.script->flag_bytes={1};
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::Completed && tick.queued && tick.queued->status==Q::Completed && player->primary_loop()->queued_completed &&
        owners.queue.size()==1 && owners.queue[0].node_identity==900 && player->state().counter_1368==1 && fade_snapshot(fade)==fade_before && actor_queue_snapshot(render)==draw,
        "composed startup tick finishes its script-blocked owned node after all prior callbacks without touching later foreign work");
    before=startup_snapshot(*player);effects=update_owners_snapshot(owners);
    tick=player->advance_state29_startup_tick(inputs,{},&fade,&owners,nullptr);
    expect(tick.status==S::Completed && tick.queue_reused && !tick.queued && preserved(),"same-loop completed tick reuses owned completion and never consumes the next foreign node");
    // Subsequent supplied samples run the active parent/status path through
    // the original rounded fade deadline. Supplied queue completion is not GPU.
    input.parent={};input.parent.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.parent.scene_child_word_4=0;
    input.parent.pose={1,{2,0,0},{0,0,1}};input.parent.action_148=0;
    input.secondary.scene_mode_60=0;input.secondary.secondary_byte_3f2=uint8_t{0};input.secondary.item_present=false;
    const auto blocked_actor=input.actor;
    for(uint32_t sample=1;sample<64;++sample){
        render.erase(std::remove_if(render.begin(),render.end(),[&](const auto& entry){return entry.node_identity==fade.render_node_identity() ||
            entry.node_identity==player->render_node_identity() || entry.node_identity==player->effect_node_identity();}),render.end());
        expect(providers.clock->advance() && inputs.capture_control()==awl::WorldMapControlStatus::Captured,"startup fixture explicitly advances after supplied render completion");
        if(sample==2){auto& actor=input.actor.actor;actor.script->flag_bytes[0]=0;actor.script->records.emplace();actor.derive_normal_visibility=true;
            actor.frame.actor_placement=awl::WorldMapPlayerActorPlacement{input.parent.pose.position,input.parent.pose.heading};
            actor.frame.node_post_transforms.resize(player->model().primary().model().core().nodes.size());
            actor.visibility.emplace();actor.visibility->view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
            for(size_t i=0;i<6;++i){awl::WorldMapPlayerActorPlane plane;plane.point[i/2]=i%2?-1000.0f:1000.0f;
                plane.normal[i/2]=i%2?-1.0f:1.0f;actor.visibility->planes[i]=plane;}
            actor.screen.emplace();actor.screen->view=*actor.visibility->view;actor.screen->half_extents={1000.0f,1000.0f};input.actor.effect_gate=uint8_t{0};
            auto invalid=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};invalid[11]=std::numeric_limits<float>::quiet_NaN();
            actor.frame.node_post_transforms.back()=invalid;
            tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
            expect(tick.status==S::QueueIncomplete && player->state().counter_1368==3 && player->primary_loop()->followups_completed &&
                !player->primary_loop()->queued_completed,"late visible frame failure retains accepted parent/model/fade/pass work and pending update");
            before=startup_snapshot(*player);fade_before=fade_snapshot(fade);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
            tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
            expect(tick.status==S::QueueIncomplete && preserved(),"repeated failing frame cannot repeat earlier callback work or partially publish geometry");
            actor.frame.node_post_transforms.back().reset();}
        if(sample==3)input.actor=blocked_actor;
        tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
        expect(tick.status==(sample==63?S::TransitionRequired:S::Completed) && player->state().counter_1368==sample+1 &&
            fade.state().mode_60==(sample==63?0:1) && player->state().state_1364==0x29,
            "each connected CPU startup sample advances parent/model/fade once and stops before the first requested state transition");
        if(sample==2)expect(tick.queued && tick.queued->callback && tick.queued->callback->status==awl::WorldMapPlayerStartupActorCallbackStatus::Completed &&
            player->model().primary().cpu().frame() && player->model().primary().cpu().frame()->root_matrix[3]==2 && render.size()==5,
            "corrected full CPU frame publishes from accepted playback/pose and schedules player before fade in the composed tick");
    }
    expect(tick.passes && tick.passes->secondary && tick.passes->secondary->requested_state==0 && !tick.queued &&
        !player->primary_loop()->followups_completed && !player->primary_loop()->queued_completed && owners.queue.size()==2 &&
        owners.queue[0].node_identity==player->update_node_identity(),"fade completion requests zero and preserves the queued player until that transition body is translated");
    before=startup_snapshot(*player);fade_before=fade_snapshot(fade);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::TransitionRequired && preserved(),"transition stop retries never repeat accepted parent, animation or final fade color");
    {awl::WorldMapPlayerStartupTickInput changed;changed.secondary.guards=awl::WorldMapPlayerUpdateGuards{0,{}};changed.secondary.mode_word_60=99;
        tick=player->advance_state29_startup_tick(inputs,changed,&fade,&owners,&render);
        expect(tick.status==S::TransitionRequired && tick.passes && tick.passes->secondary && tick.passes->secondary->requested_state==0 && preserved(),
            "accepted state request stays fixed when retry observations try to skip or replace the requesting callback");}
    using T=awl::WorldMapPlayerState0TransitionStatus;
    awl::WorldMapPlayerState0TransitionInput transition;
    const auto stopped=[&](T status){const auto step=player->advance_pending_state0_transition(inputs,transition,&fade,&owners);
        expect(step.status==status && preserved(),"stopped state-zero preparation preserves parent/model/fade/status/queues and pending request");return step;};
    expect(player->advance_pending_state0_transition(missing,transition,&fade,&owners).status==T::RequiresControl &&
        player->advance_pending_state0_transition(inputs,transition,nullptr,&owners).status==T::InvalidInput && preserved(),"state-zero owner/control preflight preserves accepted startup work");
    stopped(T::RequiresSavedRemapByte);transition.saved_byte_14d35=uint8_t{0};stopped(T::RequiresSecondaryRemapByte);
    uint64_t remap_digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){transition.saved_byte_14d35=uint8_t(seed%256);transition.secondary_byte_3f1=uint8_t(seed/256*255);
        const auto step=stopped(seed%256 || seed/256?T::UnsupportedRemappedState:T::RequiresSceneType);
        expect(step.remapped_state.has_value(),"complete saved remap bytes prepare an explicit state number");
        for(uint32_t word:{seed,uint32_t(*transition.saved_byte_14d35),uint32_t(*transition.secondary_byte_3f1),uint32_t(step.remapped_state.value_or(-1))})hash(remap_digest,word);}
    expect(remap_digest==0xc5e7f32d12a63e76ull,"512 requested-zero remaps match the original saved-byte precedence");
    transition.saved_byte_14d35=uint8_t{0};transition.secondary_byte_3f1=uint8_t{0};transition.scene_type_68=1;
    stopped(T::RequiresAudioByte);transition.audio_byte_90=uint8_t{255};stopped(T::RequiresAudioStop);transition.audio_byte_90=uint8_t{0};
    stopped(T::RequiresModelByte);transition.saved_subobject_byte_14c=uint8_t{0};
    {const auto status=owners.status;owners.status.reset();effects=update_owners_snapshot(owners);stopped(T::RequiresStatus);owners.status=status;effects=update_owners_snapshot(owners);}
    const auto previous_sources=inputs.sources();auto unknown_phase=previous_sources;unknown_phase.phase->raw_word_0.reset();inputs.observe_control_sources(unknown_phase);
    stopped(T::RequiresPhaseRaw);inputs.observe_control_sources(previous_sources);
    const auto old_word20=owners.status->word_20;
    for(const auto& choice:std::array<std::pair<uint32_t,uint32_t>,4>{{{0,7},{24999,7},{25000,0},{75000,1}}}){
        owners.status->word_20=choice.first;effects=update_owners_snapshot(owners);const auto step=stopped(T::RequiresItem);
        expect(step.model_row && step.model_row->selector==choice.second && step.model && step.model->after.index_8==player->model().state().feature_38->index_8,
            "transition selector reads the pointed owned status and preserves feature index through prepared model refresh");}
    owners.status->word_20=old_word20;effects=update_owners_snapshot(owners);
    transition.saved_subobject_byte_14c=uint8_t{255};
    {const auto status=owners.status;owners.status.reset();effects=update_owners_snapshot(owners);const auto step=stopped(T::RequiresItem);
        expect(step.model_row && step.model_row->selector==0x24,"nonzero pointed byte skips even an absent status word20");owners.status=status;effects=update_owners_snapshot(owners);}
    transition.saved_subobject_byte_14c=uint8_t{0};
    const int rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"state-zero test selects unsupported selector arithmetic");
    stopped(T::UnsupportedNumerics);expect(std::fesetround(rounding)==0,"state-zero test restores nearest arithmetic");
    transition.item_144=0u;stopped(T::RequiresAction);transition.item_144=7u;stopped(T::RequiresItemBinding);
    transition.item_binding_1454=true;stopped(T::RequiresItemAttachment);transition.item_binding_1454=false;stopped(T::RequiresAction);
    transition.action_148=-1;stopped(T::RequiresActionBinding);transition.action_binding_3ec=true;stopped(T::RequiresActionAttachment);
    transition.scene_type_68=0;transition.audio_byte_90.reset();stopped(T::RequiresActionAttachment);
    transition.item_144=0u;transition.action_148=0;transition.scene_type_68=query.tail.inputs.start.saved_pose.scene_type;transition.audio_byte_90=uint8_t{0};
    auto transition_sources=previous_sources;const auto transition_phase=allocations?(query.initial.phase+2)%6:query.initial.phase;
    transition_sources.phase->raw_word_0=phase_origins[transition_phase]+123456u;inputs.observe_control_sources(transition_sources);
    const auto prior_feature=*player->model().state().feature_38;const auto prior_secondary=player->state().secondary_byte_3f0;
    const auto prior_counter=player->state().counter_1368;const auto frame_before=primary_snapshot(player->model().primary());
    awl::WorldMapPlayerState0TransitionResult transitioned;bool applied=false;
#if !defined(_MSC_VER) || !defined(_DEBUG)
    if(allocations){const auto live=allocation_probe::live;size_t failed=0;
        for(size_t fail=0;fail<8;++fail){allocation_probe::remaining=fail;allocation_probe::enabled=true;
            transitioned=player->advance_pending_state0_transition(inputs,transition,&fade,&owners);allocation_probe::enabled=false;
            if(transitioned.status==T::Advanced){applied=true;break;}++failed;
            expect(transitioned.status==T::AllocationFailure && preserved() && allocation_probe::live==live,"state-zero model timer staging failures preserve the entire pending transition and release rows");}
        expect(applied && failed==2,"both reached timer-row allocations precede atomic state-zero publication");std::cout<<"PLAYER_STATE0_TRANSITION_ALLOCATION_FAILURES "<<failed<<'\n';}
#endif
    if(!applied)transitioned=player->advance_pending_state0_transition(inputs,transition,&fade,&owners);
    expect(transitioned.status==T::Advanced && transitioned.model_row && transitioned.model && player->state().state_1364==0 &&
        player->state().counter_1368==prior_counter && player->primary_loop()->state0_transition_completed && !player->primary_loop()->pending_state_request &&
        !player->primary_loop()->followups_completed && !player->primary_loop()->queued_completed &&
        primary_snapshot(player->model().primary())==frame_before && fade_snapshot(fade)==fade_before && update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw,
        "no-attachment state-zero transition publishes model/parent/secondary fields while preserving counter, frame, pending queue, fade and clock");
    const auto expected_selector=old_word20>=75000?1u:old_word20>=25000?0u:7u;
    awl::WorldMapPlayerModelTypeRow expected_row;expect(player->model().initial_inputs()->select_model_type(transition_phase,expected_selector,&expected_row) && transitioned.model_row &&
        transitioned.model_row->slot==expected_row.slot && player->model().state().feature_38->type_0==expected_row.type_0 && player->model().state().feature_38->type_4==expected_row.type_4,
        "state-zero model table follows the current raw phase owner, including a supplied change since construction");
    const auto& holder=player->model().state();const auto expected_secondary=*holder.completed_20 || !*holder.flag_21?uint8_t{0}:prior_secondary;
    expect(player->state().secondary_byte_3f0==expected_secondary && holder.feature_38->index_8==prior_feature.index_8 && holder.feature_38->table_c==prior_feature.table_c,
        "state-zero holder-tail result and preserved feature index/table match the independently compared original branches");
    before=startup_snapshot(*player);
    expect(player->advance_pending_state0_transition(inputs,{},&fade,&owners).status==T::AlreadyAdvanced && preserved(),"repeated accepted transition ignores missing replacement observations");
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::PassesIncomplete && !tick.queued && preserved(),"state zero cannot reuse state29's third/queued callback as accepted normal gameplay");
    expect(fade.start_world_map_entry()==F::Started,"diagnostic restart invalidates a handled fade proof");
    before=startup_snapshot(*player);fade_before=fade_snapshot(fade);
    tick=player->advance_state29_startup_tick(inputs,input,&fade,&owners,&render);
    expect(tick.status==S::RequiresNextLoop && preserved(),"restarting handled fade cannot retroactively update it before accepted later work");
    // Fresh owners cannot be retroactively placed ahead of an independently
    // handled fade, even if they happen to share the same clock values.
    std::unique_ptr<awl::WorldMapPlayerStartup> fresh;
    expect(awl::construct_world_map_player_startup(providers,query,&fresh).status==awl::WorldMapPlayerStartupStatus::ConstructedCpuStartup && fresh,"reverse-order fixture constructs");
    if(fresh){const auto unchanged=startup_snapshot(*fresh);
        expect(fresh->advance_state29_startup_tick(inputs,input,&fade,&owners,&render).status==S::RequiresNextLoop && startup_snapshot(*fresh)==unchanged,
            "unbound handled fade prevents retroactive parent/model startup ordering");}
    before=startup_snapshot(*player);fade_before=fade_snapshot(fade);effects=update_owners_snapshot(owners);draw=actor_queue_snapshot(render);
    const auto third_model=holder_snapshot(player->model());
    expect(player->advance_state0_third(missing,{},{}).status==Third::RequiresControl && preserved(),"third callback requires fresh owned control");
    missing.observe_control_sources(phase_sources);expect(missing.capture_control()==awl::WorldMapControlStatus::Captured,"alternate same-clock input captures");
    expect(player->advance_state0_third(missing,{},{}).status==Third::InvalidInput && preserved(),"same-clock replacement input cannot replace the transition owner");
    awl::WorldMapUpdateInputs foreign(std::make_shared<awl::GameClock>());
    expect(player->advance_state0_third(foreign,{},{}).status==Third::InvalidInput && preserved(),"foreign clock cannot supply third-pass control or PAD");
    expect(player->advance_state0_third(inputs,{},{}).status==Third::RequiresGuards && preserved(),"missing third guards preserve model/frame/queues/context");
    expect(player->consume_state0_update_after_third(inputs,{},&owners,&render).status==Q::RequiresThird && preserved(),"startup-transition queue requires its changed state-zero third callback");
    awl::WorldMapPlayerState0ThirdInput third;third.guards=awl::WorldMapPlayerUpdateGuards{-1,0};
    third.context.emplace();third.context->scene_type=2;third.context->current_key=17;third.context->facing={0,0,1};
    third.context->held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t{0},uint8_t{0},0u};third.context->tracked_14ac=0;
    if(context_sources){
        expect(player->advance_state0_third(inputs,*context_sources,third).status==Third::RequiresContextState && preserved(),"common callback cannot invent constructor-unwritten context state");
        awl::WorldMapPlayerContextUpdateState seed;seed.ui.resource_manager_1278=context_sources->icons->resources->identity();seed.ui.labels_60={0u,0u,0u,0u};
        for(auto& e:seed.ui.elements){e.emplace();e->flags_120=0;e->attachments_10c_110={uint64_t{0},uint64_t{0}};}
        expect(player->seed_state0_context(seed) && !player->seed_state0_context(seed),"owned state-zero context accepts one explicit observed projection");
        seed.labels_128[0]=999;expect(player->state0_context()->labels_128[0]==0,"seed values are copied and cannot be externally replaced");
        before=startup_snapshot(*player);
        expect(player->advance_state0_third(inputs,*context_sources,third).status==Third::RequiresPad && preserved(),"late PAD stop preserves context, player, model, fade and both queues");
        inputs.observe_pad_sample({true,0});expect(inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured,"owned idle PAD captures after a retryable missing dependency");
        allocation_probe::enabled=true;const auto complete=player->advance_state0_third(inputs,*context_sources,third);allocation_probe::enabled=false;
        expect(complete.status==Third::Prepared && complete.pressed==0u && player->state0_third_loop() &&
            player->state0_context()->labels_128[2]==101 && player->state0_context()->secondary_word_564==53 &&
            holder_snapshot(player->model())==third_model && !player->primary_loop()->followups_completed && !player->primary_loop()->queued_completed &&
            fade_snapshot(fade)==fade_before && update_owners_snapshot(owners)==effects && actor_queue_snapshot(render)==draw,
            "owned idle third publishes context/receipt without advancing model, consuming queues or acknowledging other unfinished passes");
        for(uint32_t channel=0;channel<4;++channel){uint8_t old_icon=0,new_icon=0;
            expect(context_sources->labels->icon(*seed.ui.labels_60[channel],&old_icon) &&
                context_sources->labels->icon(*player->state0_context()->ui.labels_60[channel],&new_icon),"actual context channel labels resolve");
            const auto& e=player->state0_context()->ui.elements[channel];
            expect(e && e->clock==(old_icon!=new_icon?providers.clock:seed.ui.elements[channel]->clock),"changed context icons bind the actual model/input clock; retained icons keep their observed fields");}
    } else {
        third.guards=awl::WorldMapPlayerUpdateGuards{0,{}};third.context.reset();
        expect(player->advance_state0_third(inputs,{},third).status==Third::SkippedByGuard && player->state0_third_loop() &&
            !player->state0_context() && !player->primary_loop()->followups_completed,"guard-skipped third completes without fabricated context/PAD or full-pass acknowledgement");
    }
    before=startup_snapshot(*player);
    expect(player->advance_state0_third(inputs,{},{}).status==Third::AlreadyCompleted && preserved() && !player->seed_state0_context({}),
        "accepted third receipt ignores replacement observations and prohibits reseeding");
    if(owned_sources)player_sources_owner_checks(*player,inputs,owners,render,context_sources,allocations);
    else state0_normal_owner_checks(*player,inputs,owners,render,context_sources,allocations);
    std::cout<<"PLAYER_STARTUP_TICK 64 ordered active-control samples / owned no-attachment state-zero transition / "
        <<(context_sources?"owned idle third callback":"guard-skipped third callback")<<" / owned queued CPU frame / normal first pass / active second movement/state-one stop\n";
}
void state1_local_selector_checks(const awl::WorldMapPlayerStartAnimationTables& tables){
    uint64_t digest=14695981039346656037ull;size_t cases=0;
    // Empty item type is zero in the target metadata; nonempty lookup bytes
    // are supplied observations, not verified live item producers.
    for(uint32_t selector=0;selector<4;++selector)for(uint32_t item:{0u,1u,0x4ffu})for(uint32_t action:{0u,7u,UINT32_MAX})for(uint32_t type=0;type<256;++type){
        if(!item && type!=0)continue;awl::WorldMapPlayerStartAnimationSelection selected;
        expect(tables.select({selector,99,item,action,8},item?std::optional<awl::WorldMapPlayerStartItemType>{{item,uint8_t(type)}}:std::nullopt,&selected)==awl::WorldMapPlayerStartAnimationStatus::Selected,"actual normal selector accepts a bounded observed item/action choice");
        for(uint32_t w:{selector,item,action,type,99u,8u,selected.choice,uint32_t(selected.descriptor_index),uint32_t(selected.descriptor.identity),selected.descriptor.word_0,selected.descriptor.word_4,selected.descriptor.word_8})hash(digest,w);++cases;
    }
    expect(cases==6156 && digest==0xce4eaafa42173ae8ull,"all actual selectors zero through three agree with original288E8/29138 executions and row/descriptor reads");
    std::cout<<"LOCAL_STATE1_SELECTOR_ROWS "<<cases<<' '<<std::hex<<digest<<std::dec<<" observed nonempty item categories\n";
}
void local_animation_loops(const char* disc) {
    using S=awl::WorldMapPlayerStartupStatus;
    expect(awl::filesystem_mount("/",disc),"local coordinated animation disc mounts");
    std::shared_ptr<const awl::WorldMapPlayerContextTables> context_tables;
    expect(awl::load_world_map_player_context_tables(&context_tables)==awl::WorldMapPlayerContextTablesStatus::Loaded && context_tables && context_tables->target_verified(),
        "local context lookup metadata loads only from the verified target DOL");
    if(context_tables){uint64_t digest=14695981039346656037ull;for(int32_t key=1;key<=16;++key)for(uint32_t slot=0;slot<8;++slot){int32_t neighbor;
        expect(context_tables->neighbor(key,slot,&neighbor),"all verified context key/slot lookups resolve");for(uint32_t w:{uint32_t(key),slot,uint32_t(neighbor)})hash(digest,w);}
        expect(digest==0x2f3276d5d8b5b6f5ull,"all 128 local context neighbors match actual original lookup instructions");
        std::cout<<"LOCAL_PLAYER_CONTEXT_NEIGHBORS 128 "<<std::hex<<digest<<std::dec<<'\n';context_matrix(*context_tables,0x542f2a040b6b50b0ull);}
    std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> context_labels;
    expect(awl::load_world_map_player_context_label_tables(&context_labels)==awl::WorldMapPlayerContextLabelTablesStatus::Loaded && context_labels && context_labels->target_verified(),"local context labels load only from target-verified DOL metadata");
    if(context_labels)context_label_matrix(*context_labels,0x5d00b3cdd08c3bf9ull);
    if(context_labels)context_icon_matrix(*context_labels,0xcd1975f29e8238f5ull);
    awl::WorldMapPlayerContextIconSources context_icons;context_icons.clock=std::make_shared<awl::GameClock>();
    expect(awl::load_world_map_context_icon_resources(&context_icons.resources)==awl::WorldMapContextIconResourcesStatus::Loaded,"local state-zero third resources load");
    if(!context_tables || !context_labels || !context_icons.resources){awl::filesystem_shutdown();return;}
    awl::WorldMapSceneBucketRegistry empty_context_registry;
    const awl::WorldMapPlayerContextUpdateSources context_sources{context_tables.get(),context_labels.get(),&empty_context_registry,&context_icons};
    awl::WorldMapPlayerStartupProviders providers;
    expect(awl::load_world_map_player_animation_assets(&providers.animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded &&
        awl::load_world_map_player_start_animation_tables(&providers.starts)==awl::WorldMapPlayerStartAnimationStatus::Loaded && providers.starts->target_verified() &&
        awl::load_world_map_player_initial_animation_inputs(&providers.initial)==awl::WorldMapPlayerInitialAnimationStatus::Loaded &&
        awl::load_world_map_player_timer_assets(&providers.timers)==awl::WorldMapPlayerTimerAssetsStatus::Loaded,"local coordinated providers and DOL verify");
    if(!providers.animations || !providers.starts || !providers.initial || !providers.timers)return;
    state1_local_selector_checks(*providers.starts);
    auto camera=std::make_shared<awl::WorldMapCameraCollisionAsset>();awl::WorldMapCameraInitialProfile profile;
    expect(camera->load() && awl::make_world_map_camera_initial_profile(&profile),"local primary-slice camera terrain and diagnostic view load");
    uint32_t checked=0,paired=0,blocked=0;
    for(uint32_t phase=0;phase<6;++phase){expect(awl::load_world_map_player_model_assets(phase,&providers.models)==Status::Loaded,"local coordinated phase model loads");
        std::unique_ptr<awl::WorldMapPlayerStartup> player;
        for(uint32_t choice=0;choice<4;++choice){providers.clock=std::make_shared<awl::GameClock>();
            awl::WorldMapPlayerStartupQuery query;query.initial.phase=phase;query.tail.inputs.start.saved_pose.scene_type=1;query.audio_byte_90=uint8_t{0};
            query.tail.action_148_after_setup=choice==2?1:0;query.item_144_after_setup=(choice==1 || choice==3)?7u:0u;
            if(query.item_144_after_setup)query.item_type=awl::WorldMapPlayerStartItemType{7,uint8_t(choice==3?1:5)};
            const auto before=player?startup_snapshot(*player):0;
            const auto constructed=awl::construct_world_map_player_startup(providers,query,&player);
            if(choice==2){expect(constructed.status==S::ConstructorTailIncomplete && constructed.tail_status==awl::WorldMapPlayerConstructorTailStatus::MissingCameraByteEvidence &&
                player && startup_snapshot(*player)==before,"actual action-only constructor requires untranslated message delivery before its secondary selection and preserves previous startup");++blocked;continue;}
            if(constructed.status!=S::ConstructedCpuStartup)std::cout<<"LOCAL_ANIMATION_STARTUP_STOP phase "<<phase<<" choice "<<choice<<" status "<<int(constructed.status)
                <<" tail "<<(constructed.tail_status?int(*constructed.tail_status):-1)<<" animation "<<(constructed.animation?int(constructed.animation->status):-1)
                <<" initializer "<<(constructed.animation && constructed.animation->initializer_status?int(*constructed.animation->initializer_status):-1)<<'\n';
            expect(constructed.status==S::ConstructedCpuStartup && player,"actual primary/secondary choice reaches complete native CPU startup");if(!player)return;
            if(player->model().secondary_model())++paired;
            coordinated_animation_owner_checks(*player,false);++checked;
            primary_slice_owner_checks(providers,query,camera,profile.view_query,false);
            startup_tick_owner_checks(providers,query,camera,profile.view_query,false,&context_sources);
            if(choice==0){startup_tick_owner_checks(providers,query,camera,profile.view_query,false,&context_sources,true);
                scene_assembly_checks(providers,query,camera,profile.view_query,&context_sources,false);}
        }
    }
    expect(checked==18 && paired==12 && blocked==6,"all supported actual phase/selection startup pairs complete coordinated checks and action-only constructors stop explicitly");
    std::cout<<"LOCAL_PLAYER_ANIMATION_LOOPS "<<checked<<" startups / "<<paired<<" pairs / "<<checked*5<<" eligible loop samples / "<<blocked
        <<" action-only constructor message stops; actual timers/clips, supplied script/visibility/pose, CPU only\n";
    std::cout<<"LOCAL_PLAYER_PRIMARY_SLICES "<<checked<<" ordered parent/model and second/third callbacks with checked queued completions; supplied guards/mode/phase/pose, CPU only\n";
    std::cout<<"LOCAL_PLAYER_STARTUP_TICKS "<<checked<<" owners / "<<checked*64<<" active-control parent/model/fade samples / no-attachment state-zero transitions; observed guards/scene/script/pose, CPU only\n";
}
void local_status_parameters(const awl::WorldMapPlayerStartAnimationTables& tables) {
    uint64_t digest=14695981039346656037ull;size_t count=0;
    constexpr uint32_t values[]{0,1,0x7fffffff,0x80000000,0xffffffff,200000,400000};
    constexpr uint32_t scalar20[]{0,24999,25000,74999,75000,0xffffffff,0x80000000,18000000};
    constexpr uint32_t modes[]{0,1,2,3,0xffffffff};
    for(uint32_t choice=0;choice<4;++choice){awl::WorldMapPlayerStartAnimationCommand command;
        std::optional<awl::WorldMapPlayerStartItemType> item;
        if(choice==1 || choice==3){command.item=1;item=awl::WorldMapPlayerStartItemType{1,uint8_t(choice==1?5:1)};}
        if(choice==2)command.action=1;
        awl::WorldMapPlayerStartAnimationSelection selection;
        expect(tables.select(command,item,&selection)==awl::WorldMapPlayerStartAnimationStatus::Selected && selection.choice==choice,
            "all four actual selector-one first records select for status parameter comparison");
        const auto factors=tables.status_factors(selection.descriptor);
        expect(factors.rate.has_value(),"actual retained first record resolves its owned rate");if(!factors.rate)return;
        for(uint32_t seed=0;seed<64;++seed){awl::WorldMapPlayerStatusState state;
            state.word_4=values[seed%7];state.word_8=values[(seed/7)%7];state.word_c=values[(seed/49)%7];
            state.word_14=values[(seed/3)%7];state.word_18=values[(seed/21)%7];state.word_1c=values[(seed/147)%7];
            state.word_20=scalar20[seed%8];state.word_12c=123456+(seed*15473)%1584001;
            state.word_150=777;state.word_154=888;state.word_160=modes[seed%5];
            state.byte_2e=uint8_t(seed*17);state.byte_32=uint8_t(seed*29);
            awl::WorldMapPlayerStatusInputs input;input.globals=factors.globals;input.rate=factors.rate;input.phase_raw=123456;
            const auto step=awl::prepare_world_map_player_status(state,selection.descriptor.word_8,input);
            expect(step.status==awl::WorldMapPlayerStatusCode::Prepared && step.after,"owned actual parameter case prepares");if(!step.after)return;
            for(uint32_t word:{choice,seed,uint32_t(selection.descriptor.identity),selection.descriptor.word_0,selection.descriptor.word_4,selection.descriptor.word_8})hash(digest,word);
            for(const auto& global:factors.globals){expect(global.has_value(),"all initial coefficient words are owned");if(!global)return;hash(digest,*global);}
            hash(digest,factors.rate->value);for(uint32_t word:status_words(state))hash(digest,word);for(uint32_t word:status_words(*step.after))hash(digest,word);++count;
        }
    }
    expect(count==256 && digest==0x41B8FA0C40C6DBDAull,"actual DOL initial coefficients/rates and full status updates match original instructions");
    std::cout<<"LOCAL_OWNED_STATUS_PARAMETERS "<<count<<" digest "<<std::hex<<digest<<std::dec<<'\n';
}
void local_texture_samples(const awl::WorldMapPlayerTimerAssets& timers) {
    using T=awl::WorldMapPlayerTimerBank;uint64_t digest=14695981039346656037ull;uint32_t count=0;
    constexpr float values[]{-1,0,.5f,1,2,3,4,10};
    for(auto bank:{T::Eyes,T::Mouth})for(uint32_t row=0;row<timers.row_count(bank);++row)
        for(uint32_t column=0;column<timers.column_count(bank,row);++column){awl::WorldMapPlayerTimerResourceView resource;
            expect(timers.resource(bank,row,column,&resource),"local texture sampling resolves retained raw slice");
            for(float value:values){awl::WorldMapTextureAnimationSample sample;
                expect(awl::sample_world_map_texture_animation(resource,value,&sample)==awl::WorldMapTextureAnimationSampleStatus::Sampled,"local declared record extent and reached times are supported");
                for(uint32_t word:{uint32_t(bank),row,column,actor_bits(value),sample.current,sample.next,sample.texture_index})hash(digest,word);++count;}}
    expect(count==256 && digest==0x32505E8FA69BE803ull,"all 32 actual resources at eight values match mapped 82F8 instruction outputs");
    std::cout<<"LOCAL_PLAYER_TEXTURE_SAMPLE_COMPARISONS "<<count<<" "<<std::hex<<digest<<std::dec<<'\n';
}
void local_texture_timer_progress(const awl::WorldMapPlayerTimerAssets& timers) {
    using T=awl::WorldMapPlayerTimerBank;uint64_t digest=14695981039346656037ull;uint32_t count=0;
    for(auto bank:{T::Eyes,T::Mouth}){std::vector<awl::WorldMapPlayerTimerResourceView> resources;
        for(uint32_t row=0;row<timers.row_count(bank);++row)for(uint32_t column=0;column<timers.column_count(bank,row);++column){
            awl::WorldMapPlayerTimerResourceView resource;expect(timers.resource(bank,row,column,&resource),"local timer resource resolves");resources.push_back(resource);}
        std::sort(resources.begin(),resources.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
        resources.erase(std::unique(resources.begin(),resources.end(),[](const auto& a,const auto& b){return a.offset==b.offset;}),resources.end());
        for(uint32_t ordinal=0;ordinal<resources.size();++ordinal)for(uint32_t now:{0u,1u,33u,999u,1000u,5000u,60000u,UINT32_MAX}){
            const auto& resource=resources[ordinal];awl::WorldMapAnimationFeatureTimer timer{resource.identity,0,std::numeric_limits<float>::quiet_NaN(),1},after;
            expect(awl::prepare_world_map_texture_timer(timer,resource,now,&after)==awl::WorldMapTextureTimerStatus::Prepared,"local duration header supports complete elapsed update");
            for(uint32_t w:{uint32_t(bank),ordinal,resource.offset,now,after.clock_4,actor_bits(after.value_8)})hash(digest,w);++count;}}
    expect(count==256 && digest==0xDF67DAB063AB0DE2ull,"all 32 actual common timer resources at eight raw times match complete original E03C/A2FC");
    std::cout<<"LOCAL_TEXTURE_TIMER_PROGRESS "<<count<<" "<<std::hex<<digest<<std::dec<<'\n';
}
void local_startup(const char* disc) {
    using S=awl::WorldMapPlayerStartupStatus;using A=awl::WorldMapPlayerStartAnimationStatus;using B=awl::WorldMapPlayerTimerBank;
    expect(awl::filesystem_mount("/",disc),"local player startup disc mounts");awl::WorldMapPlayerStartupProviders p;
    expect(awl::load_world_map_player_initial_animation_inputs(&p.initial)==awl::WorldMapPlayerInitialAnimationStatus::Loaded && p.initial && p.initial->target_verified() &&
        awl::load_world_map_player_animation_assets(&p.animations)==awl::WorldMapPlayerAnimationAssetsStatus::Loaded &&
        awl::load_world_map_player_timer_assets(&p.timers)==awl::WorldMapPlayerTimerAssetsStatus::Loaded &&
        awl::load_world_map_player_start_animation_tables(&p.starts)==A::Loaded,"verified actual constructor static, animation, timer and selector providers load");
    if(!p.initial || !p.animations || !p.timers || !p.starts)return;
    local_texture_samples(*p.timers);
    local_texture_timer_progress(*p.timers);
    local_status_parameters(*p.starts);
    auto baseline_clock=std::make_shared<awl::GameClock>();baseline_clock->set_retrace_interval(2);
    auto camera_collision=std::make_shared<awl::WorldMapCameraCollisionAsset>();awl::WorldMapCameraInitialProfile profile;
    expect(camera_collision->load() && awl::make_world_map_camera_initial_profile(&profile),"actual slot-one camera terrain and supplied constructor profile prepare");
    uint64_t rows_digest=14695981039346656037ull,state_digest=rows_digest;size_t checked=0,changed=0,unchanged=0,camera_updates=0,owned_dispatches=0,status_dispatches=0;
    for(uint32_t phase=0;phase<6;++phase){expect(baseline_clock->advance(),"explicit local startup clock advances between phases");
        expect(awl::load_world_map_player_model_assets(phase,&p.models)==Status::Loaded,"actual startup model phase loads");if(!p.models)return;
        for(const auto [flag,word20]:std::array<std::pair<uint8_t,int32_t>,4>{{{uint8_t(0),25000},{uint8_t(0),75000},{uint8_t(0),0},{uint8_t(1),0}}}){
            // Each case retains its own clock for later progression, while the
            // independent phase baseline preserves the existing constructor digest.
            p.clock=std::make_shared<awl::GameClock>(*baseline_clock);
            awl::WorldMapPlayerStartupQuery q;q.initial.phase=phase;q.tail.inputs.start.saved_pose.scene_type=1;q.audio_byte_90=uint8_t(0);
            q.saved_subobject_byte_14c=flag;q.saved_subobject_word_20=word20;
            const auto clock_before=p.clock->state();std::unique_ptr<awl::WorldMapPlayerStartup> owner;
            const auto result=awl::construct_world_map_player_startup(p,q,&owner);
            expect(result.status==S::ConstructedCpuStartup && owner && result.model_row && result.animation,"all actual no-message constructor selectors reach owned CPU parent state");
            if(!owner || !result.model_row || !result.animation)return;
            changed+=result.animation->status==A::Advanced;unchanged+=result.animation->status==A::Unchanged;
            const auto& row=*result.model_row;const auto& parent=owner->state();const auto& h=owner->model().state();const auto& f=*h.feature_38;
            expect(parent.state_1364==0x29 && parent.counter_1368==0 && parent.secondary_byte_3f0==1 && f.type_0==row.type_0 && f.type_4==row.type_4 &&
                h.feature_source_10==0u && f.table_c!=0 && owner->starts()==p.starts && owner->model().game_clock()==p.clock &&
                !owner->model().primary().cpu().frame() && clock_before.raw_time==p.clock->state().raw_time && clock_before.loop_count==p.clock->state().loop_count,
                "actual model types, parent callback fields and retained providers publish without evaluating frame or advancing clock");
            for(uint32_t v:{phase,row.selector,row.slot,row.type_0,row.type_4})hash(rows_digest,v);
            for(uint32_t v:{phase,row.selector,uint32_t(parent.state_1364),parent.counter_1368,uint32_t(parent.secondary_byte_3f0),f.type_0,f.type_4,f.index_8,uint32_t(f.table_c!=0),h.feature_source_10.value_or(UINT32_MAX)})hash(state_digest,v);
            awl::WorldMapPlayerTimerResourceView eye,mouth;
            expect(p.timers->resource(B::Eyes,f.type_0-0x3A,0,&eye) && p.timers->resource(B::Mouth,f.type_4-0x24,0,&mouth) &&
                f.first_14.resource_0==eye.identity && f.second_24.resource_0==mouth.identity,"actual dispatch timers use selected authoritative resources");
            for(auto [timer,offset]:std::array<std::pair<const awl::WorldMapAnimationFeatureTimer*,uint32_t>,2>{{{&f.first_14,eye.offset},{&f.second_24,mouth.offset}}}){
                hash(state_digest,offset);hash(state_digest,timer->clock_4);
                for(float value:{timer->value_8,timer->rate_c}){uint32_t bits;std::memcpy(&bits,&value,4);hash(state_digest,bits);}}
            const auto initial=owner->model().initial_model_state();awl::WorldMapPlayerModelAssetView private_tam;
            expect(initial && initial->initialized_c==1 && initial->runtime_entry_address_120==0x802ED6C0 && initial->private_texture_animation &&
                p.models->resource(7,&private_tam) && initial->private_texture_animation->data==private_tam.data && f.table_c==reinterpret_cast<uintptr_t>(private_tam.data),
                "actual parent retains initial wrapper and private TAM ownership after state callback");
            // Drop all caller handles on the last case, then consume the retained providers.
            const bool last=phase==5 && flag==1;const auto clock_lifetime=std::weak_ptr<awl::GameClock>(p.clock);
            const auto starts_lifetime=std::weak_ptr<const awl::WorldMapPlayerStartAnimationTables>(p.starts);
            const auto models_lifetime=std::weak_ptr<const Assets>(p.models);
            if(last)p={};
            awl::WorldMapPlayerPrimaryFrameInput frame;frame.node_post_transforms.resize(55);
            expect(owner->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && owner->model().primary().cpu().frame()->skin_executed &&
                owner->model().primary().cpu().frame()->root_matrix==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0} &&
                owner->model().game_clock()->state().raw_time==clock_before.raw_time && owner->model().game_clock()->state().loop_count==clock_before.loop_count,
                "first actual startup frame samples 55 nodes and skins at initialized root without caller placement or implicit time advancement");
            const auto model_before=holder_snapshot(owner->model());awl::WorldMapPlayerStartupUpdateInput callback;
            callback.entry={0x800u,awl::WorldMapPlayerUpdateGuards{-1,0},0};callback.pose.scene_type=1;callback.pose.heading={0,0,1};callback.player_node_identity=99;
            awl::WorldMapPlayerUpdateOwners effects;effects.scene_one_position_240=std::array<float,3>{0,0,0};effects.queue={{1,0},{2,0x1010},{3,UINT32_MAX}};
            callback.first_camera=awl::WorldMapPlayerStartupCameraSeed{camera_collision,profile.view_query};callback.action_148=0;
            const auto camera_lifetime=std::weak_ptr<const awl::WorldMapCameraCollisionAsset>(camera_collision);if(last)camera_collision.reset();
            for(uint32_t tick=0;tick<8;++tick){callback.pose.position={120.0f+float(tick)*0.125f,5.21f,168.0f};
                const auto step=owner->advance_state29_parent(callback,&effects);expect(step.status==awl::WorldMapPlayerUpdateStatus::Advanced && step.callback &&
                    step.callback->status==awl::WorldMapPlayerState29CameraStatus::Advanced && owner->camera() && owner->state().control_word_1384==0x800u &&
                    effects.saved_pose.position==callback.pose.position && effects.saved_pose.heading==callback.pose.heading && effects.saved_pose.scene_type==1 &&
                    effects.position_802fc20c==callback.pose.position && effects.scene_one_position_240==callback.pose.position && effects.queue.size()==4 &&
                    effects.queue[1].node_identity==99 && effects.queue[2].node_identity==2 && effects.queue[3].priority==UINT32_MAX &&
                    owner->state().counter_1368==tick+1 && owner->camera()->camera.target.camera.position==callback.pose.position &&
                    owner->camera()->camera.target.camera.yaw==profile.view_query.target.camera.yaw && owner->camera()->camera.target.pitch_offset_8c==0 &&
                    owner->camera()->camera.target.yaw_offset_90==0 && holder_snapshot(owner->model())==model_before &&
                    owner->model().game_clock()->state().raw_time==clock_before.raw_time && owner->model().game_clock()->state().loop_count==clock_before.loop_count,
                    "all actual startup phases complete normal parent updates over real camera terrain with supplied guards/control/pose/owners, no duplicate queue nodes or model/clock advancement");
                callback.first_camera.reset();++camera_updates;
            }
            {
                awl::WorldMapUpdateInputs inputs(owner->model().game_clock());
                inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{1}},0});
                inputs.request_scene_mode(4,0);awl::WorldMapSceneModeCommit commit;
                expect(inputs.commit_scene_fields(0,uint8_t{1},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields &&
                    inputs.capture_control()==awl::WorldMapControlStatus::Captured,"actual CPU startup derives control from owned observed sources and scene request");
                const auto blocked=startup_snapshot(*owner),blocked_effects=update_owners_snapshot(effects);
                auto step=owner->dispatch_state29_parent(inputs,{},nullptr);
                expect(step.status==awl::WorldMapPlayerUpdateStatus::SkippedByDispatchFilter && !step.callback && startup_snapshot(*owner)==blocked &&
                    update_owners_snapshot(effects)==blocked_effects,"actual CPU startup filter skips all callback inputs without changing camera/model/effects");
                inputs.request_scene_mode(4,0);
                expect(inputs.commit_scene_fields(0,uint8_t{0},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields,"actual CPU startup scene fields unblock player group");
                awl::WorldMapPlayerStartupDispatchInput dispatch;dispatch.guards=awl::WorldMapPlayerUpdateGuards{-1,0};dispatch.scene_child_word_4=0;
                dispatch.pose=callback.pose;dispatch.action_148=0;dispatch.player_node_identity=99;
                step=owner->dispatch_state29_parent(inputs,dispatch,&effects);
                expect(step.status==awl::WorldMapPlayerUpdateStatus::Advanced && step.callback && owner->state().counter_1368==9 &&
                    owner->state().control_word_1384==0x800u && effects.queue.size()==4 && effects.queue[1].node_identity==99 &&
                    effects.saved_pose.position==dispatch.pose.position && holder_snapshot(owner->model())==model_before &&
                    inputs.scene().global_flag_59af==1 && owner->model().game_clock()->state().loop_count==clock_before.loop_count,
                    "actual startup uses derived control and retained filter for parent update; later event, model and clock remain untouched");
                ++owned_dispatches;
                status_parent_checks(*owner,effects,dispatch);
                owned_status_parent_checks(*owner,effects,dispatch);
                fresh_status_parent_checks(*owner,effects,dispatch);
                actor_placement_owner_checks(*owner);++status_dispatches;
                const auto raw=owner->model().game_clock()->state().raw_time,restarts=owner->model().state().restart_count_2c;
                expect(owner->configure_animation_repeats(awl::WorldMapPlayerHolderRepeatKind::Duration,1000),"actual startup accepts an explicit diagnostic duration request");
                bool restarted=false;uint32_t ticks=0;
                for(;ticks<4096;++ticks) {
                    const auto progressed=owner->advance_animation();
                    expect(progressed.status==awl::WorldMapPlayerHolderProgressStatus::Advanced,"actual primary animation progresses until its natural completion");
                    if(progressed.status!=awl::WorldMapPlayerHolderProgressStatus::Advanced)break;
                    if(progressed.initialized_records==1){restarted=true;++ticks;break;}
                }
                expect(restarted && owner->model().state().current_descriptor_0==owner->model().state().base_descriptor_4 &&
                    owner->model().state().completed_20==1u && owner->model().state().restart_count_2c==restarts+1 &&
                    owner->model().primary_channel().state().elapsed_0==0 && owner->model().primary().model().partial_playback().position_0==0 &&
                    owner->model().game_clock()->state().raw_time==raw,"natural actual clip completion executes same-base DB28 once without advancing time or resetting holder completion/count");
                const auto playback=holder_snapshot(owner->model(),false);
                expect(owner->advance(frame).status==awl::WorldMapPlayerPrimaryUpdateStatus::Advanced && holder_snapshot(owner->model(),false)==playback,
                    "actual 55-node CPU frame consumes restarted model/channel without repeating holder/timer progression");
                std::cout<<"LOCAL_PLAYER_ANIMATION_RESTART "<<phase<<' '<<unsigned(flag)<<' '<<word20<<" ticks "<<ticks<<'\n';
            }
            owner.reset();if(last)expect(clock_lifetime.expired() && starts_lifetime.expired() && models_lifetime.expired() && camera_lifetime.expired(),"last actual startup releases retained caller providers and camera terrain");++checked;
        }
    }
    std::cout<<"LOCAL_PLAYER_STARTUP_ROWS "<<checked<<" digest "<<std::hex<<rows_digest<<std::dec<<'\n';
    std::cout<<"LOCAL_PLAYER_STARTUP_STATE "<<checked<<" digest "<<std::hex<<state_digest<<std::dec<<'\n';
    expect(checked==24 && rows_digest==0x6A990DF449C60A45ull,"all 24 phase/selector rows match independently mapped F748 tables");
    expect(state_digest==0xC57D5EFAEAF64EBDull,"all 24 parent/model timer states match original 31D7C/F3E8/349A4 instructions with the animation callback supplied");
    std::cout<<"LOCAL_PLAYER_STARTUP "<<checked<<" first 55-node CPU frames; callbacks changed "<<changed<<" unchanged "<<unchanged<<"; scene-message, live actor and GPU pending\n";
    std::cout<<"LOCAL_PLAYER_STATE29_CAMERA "<<camera_updates<<" updates across "<<checked<<" startups; supplied diagnostic positions/profile, actual camera COL; full parent loop pending\n";
    std::cout<<"LOCAL_PLAYER_STATE29_PARENT "<<camera_updates<<" guarded updates with atomic pose/position/queue effects; supplied owners/control, live scheduling pending\n";
    expect(owned_dispatches==24,"all 24 actual startups pass owned blocked/allowed dispatch");
    std::cout<<"LOCAL_PLAYER_OWNED_DISPATCH "<<owned_dispatches<<" blocked/allowed cases; derived control from observed sources, real camera COL; live loop and queue consumption pending\n";
    expect(status_dispatches==24,"all 24 actual startups pass active status dispatch and rollback checks");
    std::cout<<"LOCAL_PLAYER_STATUS_DISPATCH "<<status_dispatches<<" active cases with constant/curved status and rollback, invented status/global/rate/phase observations, actual CPU player and camera COL; live status ownership pending\n";
    std::cout<<"LOCAL_PLAYER_OWNED_STATUS_DISPATCH "<<status_dispatches<<" active cases with retained initial DOL coefficients/rates and rollback; mutable status/phase supplied, live writers pending\n";
    std::cout<<"LOCAL_PLAYER_FRESH_STATUS_DISPATCH "<<status_dispatches<<" fresh constructor/reset cases with three active updates through phase wrap and late rollback; phase observed, live saved-owner activation pending\n";
    std::cout<<"LOCAL_PLAYER_ACTOR_PLACEMENT "<<status_dispatches<<" startups / four turning and translated CPU frames each; root/features published once, prior skin output and playback/clock preserved; queue/TAM/GPU pending\n";
    std::cout<<"LOCAL_PLAYER_GATED_ACTOR_FRAME "<<status_dispatches<<" startups / four blocked-culled-visible routes each; supplied script/sphere/view/plane observations, atomic CPU frame publication; live sources/complete callback/queue pending\n";
    std::cout<<"LOCAL_PLAYER_NORMAL_ACTOR_FRAME "<<status_dispatches<<" startups / four derived-bounds/inside-screen/outside-screen CPU frames and post-frame eye/mouth shared-cache updates each; native reciprocal-sqrt seed, separate camera globals observed; metadata/effect/progression/queue/live sources pending\n";
    std::cout<<"LOCAL_PLAYER_ACTOR_CALLBACK "<<status_dispatches<<" startups / four CPU frame+texture+metadata/scheduling updates and one first-test cull each; owned effect/player keys, observed gates; render execution and live sources pending\n";
    std::cout<<"LOCAL_PLAYER_UPDATE_COMPLETION "<<status_dispatches<<" startups / parent registration, one script block, three queued CPU frames, one cull and retry/ordering checks each; front owned node only, later work pending; no live activation\n";
    std::cout<<"LOCAL_PLAYER_OWNED_TEXTURE_TIMERS "<<status_dispatches<<" startups / three explicit clock steps, atomic timer pairs, stale-control retry and queued CPU frame/texture consumption each; holder/channel and live order remain pending\n";
    std::cout<<"LOCAL_PLAYER_CHANNEL_PROGRESS "<<status_dispatches<<" startups / sixteen atomic owned channel advances, blocked-frame preservation and queued CPU frame consumption each; holder descriptor/restart and live order remain pending\n";
    std::cout<<"LOCAL_PLAYER_HOLDER_PROGRESS "<<status_dispatches<<" startups / eight combined channel/holder/timer updates with queued frames, then one natural clip completion/direct restart and CPU frame each; live sources/scheduling and gameplay comparison pending\n";
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
    actor_placement_comparisons();embedded_tpl();skin_metadata_checks();auxiliary_checks();awl::filesystem_shutdown();setup_checks();awl::filesystem_shutdown();draw_checks();awl::filesystem_shutdown();skin_work_checks();awl::filesystem_shutdown();skin_execution_checks();awl::filesystem_shutdown();frame_checks();awl::filesystem_shutdown();animation_frame_checks();awl::filesystem_shutdown();attachment_frame_checks();awl::filesystem_shutdown();hierarchy_frame_checks();awl::filesystem_shutdown();partial_frame_sampling_checks();owned_hierarchy_frame_checks();awl::filesystem_shutdown();primary_frame_owner_checks();awl::filesystem_shutdown();topology_checks();awl::filesystem_shutdown();geometry_checks();awl::filesystem_shutdown();primary_owner_checks();awl::filesystem_shutdown();holder_checks();awl::filesystem_shutdown();initial_root_scale_checks();initial_input_checks();awl::filesystem_shutdown();timer_asset_checks();awl::filesystem_shutdown();texture_feature_checks();awl::filesystem_shutdown();state29_camera_checks();parent_update_checks();startup_owner_checks();awl::filesystem_shutdown();feature_draw_state_checks();awl::filesystem_shutdown();synthetic();awl::filesystem_shutdown();
    fade_checks();context_checks();context_label_checks();state0_initializer_checks();state0_secondary_checks();state1_initializer_checks();state1_animation_decision_checks();state1_secondary_route_checks();state1_terrain_probe_checks();followup_decision_checks();holder_policy_checks();holder_pair_checks();awl::filesystem_shutdown();holder_lifecycle_checks();awl::filesystem_shutdown();
    if(argc==3 && std::string(argv[1])=="--player-model-local")local(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-frame-local")local(argv[2],true);
    else if(argc==3 && std::string(argv[1])=="--player-primary-local")local_primary(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-holder-local")local_holder(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-holder-pair-local")local_holder_pair(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-holder-lifecycle-local")local_holder_lifecycle(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-animation-loop-local")local_animation_loops(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-initial-holder-local")local_initial_holder(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-timer-holder-local")local_timer_holder(argv[2]);
    else if(argc==3 && std::string(argv[1])=="--player-clock-holder-local")local_timer_holder(argv[2],true);
    else if(argc==3 && std::string(argv[1])=="--player-initial-model-local")local_timer_holder(argv[2],true,true);
    else if(argc==3 && std::string(argv[1])=="--player-startup-local")local_startup(argv[2]);
    else if(argc!=1)expect(false,"usage: --player-model-local <disc>, --player-frame-local <disc>, --player-primary-local <disc>, --player-holder-local <disc>, --player-holder-pair-local <disc>, --player-holder-lifecycle-local <disc>, --player-animation-loop-local <disc>, --player-initial-holder-local <disc>, --player-timer-holder-local <disc>, --player-clock-holder-local <disc>, --player-initial-model-local <disc> or --player-startup-local <disc>");
    awl::filesystem_shutdown();return failures?1:0;
}
