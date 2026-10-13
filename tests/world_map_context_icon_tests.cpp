#include "awl/world_map_player_context_labels.h"
#include "awl/filesystem.h"
#include <cfenv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace allocation_probe {bool enabled=false;size_t remaining=0,live=0;}
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
using R=awl::WorldMapContextIconResourcesStatus;
using S=awl::WorldMapPlayerContextIconStatus;
int failures=0;
void expect(bool yes,const char* reason){if(!yes){++failures;std::cerr<<"FAIL: "<<reason<<'\n';}}
void word(std::vector<uint8_t>& b,size_t at,uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(v>>(24-i*8));}
uint32_t bits(float f){uint32_t v;std::memcpy(&v,&f,4);return v;}
void hash(uint64_t& h,uint32_t v){for(unsigned i=0;i<4;++i){h^=(v>>(24-i*8))&255;h*=1099511628211ull;}}
std::vector<uint8_t> agc_fixture() {
    // Invented 2 roots, 11 groups, 18 frames, one opaque four-byte leaf.
    std::vector<uint8_t> b(468);word(b,0,0x12345678);word(b,4,2);word(b,8,12);
    word(b,12,4);word(b,16,28);word(b,20,7);word(b,24,108);
    constexpr uint16_t flags[]{0,0x8001,0x4002,0xcfff,0x1fff,0xffff,0x2000};
    for(uint32_t g=0;g<11;++g){const uint32_t at=28+g*20,frame=248+(g<4?g:4+(g-4)*2)*12;
        word(b,at,g<4?1u:2u);word(b,at+4,frame);
        word(b,at+8,g<4?0u:uint32_t(flags[g-4])<<16|(g+27));
        word(b,at+12,g==4?0xfffffff0u:100+g);word(b,at+16,0x1000+g);
    }
    for(uint32_t f=0;f<18;++f){const uint32_t at=248+f*12;word(b,at,1);word(b,at+4,464);
        word(b,at+8,uint32_t(uint16_t(int32_t(f)*13-100))<<16|uint16_t(f*7+40));}
    return b;
}
std::vector<uint8_t> tpl_fixture() {
    // Two invented 8x8 I4 textures, separate headers and 32-byte payloads.
    std::vector<uint8_t> b(164);word(b,0,0x0020af30);word(b,4,2);word(b,8,12);
    for(uint32_t i=0;i<2;++i){const uint32_t at=28+i*36;word(b,12+i*8,at);
        word(b,at,0x00080008);word(b,at+8,100+i*32);std::memset(b.data()+100+i*32,int(0x21+i),32);}
    return b;
}
std::vector<uint8_t> label_fixture(bool setup=true) {
    std::vector<uint8_t> b(setup?0x1700:0x16bf);
    const auto section=[&](size_t i,uint32_t at,uint32_t address,uint32_t size){word(b,i*4,at);word(b,0x48+i*4,address);word(b,0x90+i*4,size);};
    section(0,0x100,0x80292a78,31*172);section(1,0x15d4,0x802a7b08,124);
    section(2,0x1650,0x8004f19c,4);section(3,0x1654,0x8034a3c8,4);section(4,0x1658,0x8024f694,103);
    for(uint32_t i=0;i<31;++i)word(b,0x15d4+i*4,0x8004f19c);
    word(b,0x1650,0x38600000);word(b,0x1654,0x3f266666);
    for(uint32_t i=0;i<103;++i)b[0x1658+i]=uint8_t(i%7);
    if(setup){section(5,0x16c0,0x8034a5d8,24);section(6,0x16d8,0x8024f700,12);
        section(7,0x16e4,0x8034ad1c,8);section(8,0x16ec,0x802bb108,20);
        for(const auto p:std::array<std::array<uint32_t,2>,12>{{{0x16c0,0x3f000000},{0x16c8,0},{0x16cc,0xc37f0000},
            {0x16d0,0x3f99999a},{0x16d4,0x3fd9999a},{0x16d8,0x3f99999a},{0x16dc,0x3f99999a},
            {0x16e0,0x3f99999a},{0x16e4,2},{0x16e8,4},{0x16f4,0x800e2084},{0x16fc,0x800e21bc}}})word(b,p[0],p[1]);
    }
    return b;
}
std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> labels(bool setup=true) {
    const auto b=label_fixture(setup);std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> out;
    expect(awl::decode_world_map_player_context_label_tables(b.data(),b.size(),&out)==awl::WorldMapPlayerContextLabelTablesStatus::Decoded,"invented label/map/setup metadata decodes");return out;
}
awl::WorldMapPlayerContextIconState prior_state(const awl::WorldMapPlayerContextIconSources& sources,uint32_t flags=0) {
    awl::WorldMapPlayerContextIconState state;state.resource_manager_1278=sources.resources->identity();state.labels_60={0u,0u,0u,0u};
    for(auto& entry:state.elements){entry.emplace();auto& e=*entry;e.flags_120=flags;e.attachments_10c_110={uint64_t(0),uint64_t(0)};
        e.image_position_10=std::array<float,2>{11,12};e.animation_position_54=std::array<float,2>{21,22};e.target_f8=std::array<float,4>{31,32,33,34};}
    return state;
}
// The digest order corresponds to the independently read original fields.
void element_words(uint64_t& h,const awl::WorldMapContextIconElement& e) {
    for(uint32_t v:{e.image_group.offset,e.image_group.mode(),e.image_group.parameter(),uint32_t(e.image_group.value_a),
        e.animation_root_offset,e.animation_root,e.animation_current,e.animation_deadline,e.animation_progress})hash(h,v);
    for(const auto& pair:{e.image_position_10,e.animation_position_54})for(float v:*pair)hash(h,bits(v));
    hash(h,bits(e.image_depth_18));hash(h,bits(e.animation_depth_5c));
    for(float v:e.position_f0)hash(h,bits(v));for(float v:*e.target_f8)hash(h,bits(v));
    hash(h,bits(e.depth_114));hash(h,*e.flags_120);
    for(float v:e.scalar_parameters_94)hash(h,bits(v));hash(h,e.scalar_duration_9c);
    hash(h,e.scalar_enabled_a0);hash(h,e.scalar_repeat_a1);
    for(float v:e.vector_parameters_a4)hash(h,bits(v));hash(h,e.vector_duration_ac);
    hash(h,e.vector_enabled_b0);hash(h,e.vector_repeat_b1);
    for(const auto& vector:{e.vector_cc,e.vector_base_d8,e.vector_step_e4})for(float v:vector)hash(h,bits(v));
    hash(h,bits(e.scalar_128));hash(h,bits(e.scalar_step_12c));hash(h,e.scalar_count_130);hash(h,e.vector_count_132);
    hash(h,e.frame_134);hash(h,e.repeat_135);hash(h,e.enabled_136);hash(h,e.channel_zero_137);
}
uint64_t snapshot(const awl::WorldMapPlayerContextIconState& state) {
    uint64_t h=14695981039346656037ull;
    const auto identity=[&](uint64_t id){hash(h,uint32_t(id>>32));hash(h,uint32_t(id));};
    hash(h,state.resource_manager_1278.has_value());identity(state.resource_manager_1278.value_or(0));
    for(const auto& label:state.labels_60){hash(h,label.has_value());hash(h,label.value_or(UINT32_MAX));}
    for(const auto& e:state.elements){hash(h,e.has_value());if(e){
        identity(e->resources?e->resources->identity():0);identity(uint64_t(reinterpret_cast<uintptr_t>(e->clock.get())));
        for(const auto& attachment:e->attachments_10c_110){hash(h,attachment.has_value());identity(attachment.value_or(0));}
        element_words(h,*e);
        for(const auto& f:{e->icon_frame,e->placement_frame})for(uint32_t v:{f.offset,f.leaf_count,f.leaf_offset,uint32_t(uint16_t(f.pair[0]))<<16|uint16_t(f.pair[1])})hash(h,v);
        for(uint32_t v:{e->image_group.frame_count,e->image_group.frame_table,e->image_group.duration_c,e->image_group.value_10})hash(h,v);
    }}
    return h;
}
void comparisons(const awl::WorldMapPlayerContextLabelTables& tables,
    const std::shared_ptr<const awl::WorldMapContextIconResources>& resources,uint64_t expected) {
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){
        constexpr uint32_t flags[]{0,1,2,4,7,0xfffffff8,0x80000001,0xffffffff};
        constexpr uint32_t intervals[]{0,1,2,UINT32_MAX},raw[]{0,16,33,UINT32_MAX};
        auto clock=std::make_shared<awl::GameClock>();clock->set_retrace_interval(intervals[seed%4]);expect(clock->advance(),"explicit raw clock setup advances");
        expect(clock->state().raw_time==raw[seed%4],"clock fixture raw values independently checked");
        awl::WorldMapPlayerContextIconSources sources{resources,clock};auto state=prior_state(sources,flags[seed%8]);
        const auto channel=(seed/8)%4,old=seed%103;uint32_t label=(seed*17+1)%103;uint8_t previous=0,next=0;
        expect(tables.icon(old,&previous) && tables.icon(label,&next),"comparison labels map");
        while(next==previous){label=(label+1)%103;expect(tables.icon(label,&next),"changed icon search maps");}
        state.labels_60[channel]=old;auto out=state;
        const auto step=awl::prepare_world_map_player_context_icon(&tables,state,channel,label,&out,&sources);
        expect(step.status==S::Prepared && out.labels_60[channel]==label,"changed mapped icon completes supported CPU setup");
        hash(digest,seed);hash(digest,channel);hash(digest,old);hash(digest,label);hash(digest,previous);hash(digest,next);
        if(step.status==S::Prepared){element_words(digest,*out.elements[channel]);expect(out.elements[channel]->resources==resources && out.elements[channel]->clock==clock,"element retains resource and shared clock owners");}
        expect(clock->state().raw_time==raw[seed%4] && clock->state().loop_count==1,"icon setup never advances raw clock");
    }
    std::cout<<"CONTEXT_ICON_SETUP_COMPARISONS 512 "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==expected,"all changed-icon projected fields agree with independently executed original instructions");
}
void synthetic_checks() {
    const auto agc=agc_fixture(),tpl=tpl_fixture();std::shared_ptr<const awl::WorldMapContextIconResources> resources;
    expect(awl::decode_world_map_context_icon_resources(agc.data(),agc.size(),tpl.data(),tpl.size(),&resources)==R::Decoded,"invented resource pair decodes together");if(!resources)return;
    const auto* retained=resources.get();
    for(size_t n=1;n<agc.size();++n)expect(awl::decode_world_map_context_icon_resources(agc.data(),n,tpl.data(),tpl.size(),&resources)==R::UnsupportedAgc && resources.get()==retained,"every truncated invented AGC preserves resource owner");
    for(const auto bad:std::array<std::array<uint32_t,2>,7>{{{8,UINT32_MAX},{4,UINT32_MAX},{16,13},{28,UINT32_MAX},{32,UINT32_MAX},{252,UINT32_MAX},{52,UINT32_MAX}}}){
        auto bytes=agc;word(bytes,bad[0],bad[1]);expect(awl::decode_world_map_context_icon_resources(bytes.data(),bytes.size(),tpl.data(),tpl.size(),&resources)==R::UnsupportedAgc && resources.get()==retained,"count/offset/alignment/leaf corruption preserves resource owner");}
    auto bad_tpl=tpl;word(bad_tpl,36,UINT32_MAX);
    expect(awl::decode_world_map_context_icon_resources(agc.data(),agc.size(),bad_tpl.data(),bad_tpl.size(),&resources)==R::UnsupportedTpl && resources.get()==retained,"late TPL failure preserves prior AGC/TPL pair");
    expect(awl::decode_world_map_context_icon_resources(nullptr,agc.size(),tpl.data(),tpl.size(),&resources)==R::InvalidInput && resources.get()==retained,"null source preserves owner");
    awl::WorldMapContextIconGroup group;expect(resources->group(1,3,&group) && group.mode()==1 && group.parameter()==4095 && group.value_a==34,"both mode flags prefer 8000 and low12 remains separate");
    const auto old_group=group;expect(!resources->group(1,7,&group) && group.offset==old_group.offset && !resources->group(2,0,&group),"group/root bounds preserve metadata");
    awl::WorldMapContextIconFrame frame;expect(resources->frame(0,0,0,&frame) && frame.pair==std::array<int16_t,2>{-100,40},"signed frame pair is separate from group flags");
    const auto old_frame=frame;expect(!resources->frame(0,0,1,&frame) && frame.offset==old_frame.offset,"frame bounds preserve output");
    expect(resources->textures().textures.size()==2 && resources->textures().textures[1].raw_data[0]==0x22,"TPL private ownership retains independent second texture payload");
    auto tables=labels();if(!tables)return;
    comparisons(*tables,resources,0x77addde51cd25df4ull);
    awl::WorldMapPlayerContextIconSources sources{resources,std::make_shared<awl::GameClock>()};auto state=prior_state(sources),out=state;
    awl::WorldMapPlayerContextLabels desired;desired.labels={1,2,3};
    expect(awl::prepare_world_map_player_context_icons(tables.get(),state,desired,&out,&sources).status==S::Prepared,"four-channel mixed retained/changed batch completes");
    const auto accepted=out;const auto saved=snapshot(out);const auto check=[&](S expected,const awl::WorldMapPlayerContextIconSources* provider,const awl::WorldMapPlayerContextLabelTables* metadata){
        const auto step=awl::prepare_world_map_player_context_icons(metadata,state,desired,&out,provider);
        expect(step.status==expected && snapshot(out)==saved,"late setup stop preserves labels/elements/resources together");return step;};
    state.elements[2]->attachments_10c_110[1]=uint64_t(123);
    auto step=check(S::RequiresAttachedElement,&sources,tables.get());expect(step.channel==2 && step.prepared_mask==0xb,"late channel2 stop retains original 0,1,3,2 preparation order");
    state.elements[2]->attachments_10c_110[1].reset();check(S::RequiresElementState,&sources,tables.get());
    state=prior_state(sources);state.elements[0]->flags_120.reset();check(S::RequiresElementState,&sources,tables.get());state=prior_state(sources);
    check(S::RequiresIconAssets,nullptr,tables.get());auto missing=sources;missing.clock.reset();check(S::RequiresClock,&missing,tables.get());
    auto label_only=labels(false);check(S::RequiresSetupConstants,&sources,label_only.get());
    state.elements[0]->clock=std::make_shared<awl::GameClock>();check(S::InvalidInput,&sources,tables.get());state=prior_state(sources);
    state.resource_manager_1278=uint64_t(17);check(S::InvalidInput,&sources,tables.get());state=prior_state(sources);
    const int rounding=std::fegetround();expect(std::fesetround(FE_DOWNWARD)==0,"rounding test changes mode");check(S::UnsupportedNumerics,&sources,tables.get());expect(std::fesetround(rounding)==0,"rounding restored");
    allocation_probe::remaining=0;allocation_probe::enabled=true;
    step=awl::prepare_world_map_player_context_icons(tables.get(),state,desired,&state,&sources);allocation_probe::enabled=false;
    expect(step.status==S::Prepared && snapshot(state)==snapshot(accepted),"complete aliasing publication performs no allocations");
    {
        auto ui=prior_state(sources,1);ui.elements[0]->image_position_10.reset();ui.elements[0]->animation_position_54.reset();
        expect(awl::prepare_world_map_player_context_icon(tables.get(),ui,0,1,&ui,&sources).status==S::Prepared &&
            !ui.elements[0]->image_position_10 && !ui.elements[0]->animation_position_54 && ui.elements[0]->target_f8.has_value(),"relative-position branch preserves unknown untouched image positions");
        ui=prior_state(sources);ui.elements[0]->target_f8.reset();
        expect(awl::prepare_world_map_player_context_icon(tables.get(),ui,0,1,&ui,&sources).status==S::Prepared && !ui.elements[0]->target_f8,"direct-position branch preserves unknown untouched target fields");
    }
    {
        auto sparse=agc;word(sparse,20,2);std::shared_ptr<const awl::WorldMapContextIconResources> limited;
        expect(awl::decode_world_map_context_icon_resources(sparse.data(),sparse.size(),tpl.data(),tpl.size(),&limited)==R::Decoded,"bounded smaller icon group is valid metadata");
        awl::WorldMapPlayerContextIconSources provider{limited,sources.clock};state=prior_state(provider);
        step=check(S::UnsupportedResources,&provider,tables.get());expect(step.channel==3 && step.prepared_mask==3,"unsupported channel3 resource keeps earlier channel0/1 work unpublished");
    }
    {
        auto extreme=agc;word(extreme,256,0x80007fff);std::shared_ptr<const awl::WorldMapContextIconResources> signed_pair;
        expect(awl::decode_world_map_context_icon_resources(extreme.data(),extreme.size(),tpl.data(),tpl.size(),&signed_pair)==R::Decoded,"signed pair extrema remain valid metadata");
        awl::WorldMapPlayerContextIconSources provider{signed_pair,sources.clock};auto ui=prior_state(provider);
        expect(awl::prepare_world_map_player_context_icon(tables.get(),ui,0,1,&ui,&provider).status==S::Prepared && ui.elements[0]->position_f0==std::array<float,2>{-32768,32767},"signed halfword extrema convert exactly to positions");
    }
    {
        auto bytes=label_fixture();const auto* old=tables.get();word(bytes,0x16f4,0x800e2088);
        expect(awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&tables)==awl::WorldMapPlayerContextLabelTablesStatus::UnsupportedLayout && tables.get()==old,"unexpected setter route rejects setup constants without replacing label owner");
        bytes=label_fixture();word(bytes,0x16e4,1);
        expect(awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&tables)==awl::WorldMapPlayerContextLabelTablesStatus::UnsupportedLayout && tables.get()==old,"unsupported mask layout preserves label owner");
    }
    {
        auto bytes=label_fixture();word(bytes,0x16cc,0x80000000);word(bytes,0x16d0,0);word(bytes,0x16d4,0x80000000);
        std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> zero;
        expect(awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&zero)==awl::WorldMapPlayerContextLabelTablesStatus::Decoded,"signed-zero endpoint constants decode");
        auto ui=prior_state(sources);
        expect(awl::prepare_world_map_player_context_icon(zero.get(),ui,0,1,&ui,&sources).status==S::Prepared &&
            bits(ui.elements[0]->scalar_step_12c)==0 && bits(ui.elements[0]->vector_step_e4[0])==0,"original equal-endpoint branch publishes positive zero steps, including a negative-zero difference");
        word(bytes,0x16c8,0xff7fffff);word(bytes,0x16cc,0x7f7fffff);
        expect(awl::decode_world_map_player_context_label_tables(bytes.data(),bytes.size(),&zero)==awl::WorldMapPlayerContextLabelTablesStatus::Decoded,"finite endpoint constants decode before arithmetic checks");
        state=prior_state(sources);check(S::UnsupportedNumerics,&sources,zero.get());
    }
    // Same label bypasses sources, flags, attachments, even an out-of-map code.
    state={};state.resource_manager_1278=uint64_t(0);state.labels_60={103u,0u,0u,0u};
    expect(awl::prepare_world_map_player_context_icon(nullptr,state,0,103,&state).status==S::Prepared,"unchanged icon retains no-I/O/no-element path");
    {
        auto a=agc_fixture(),t=tpl_fixture();std::shared_ptr<const awl::WorldMapContextIconResources> owned;
        expect(awl::decode_world_map_context_icon_resources(a.data(),a.size(),t.data(),t.size(),&owned)==R::Decoded,"lifetime fixture decodes");
        a.clear();t.clear();awl::WorldMapPlayerContextIconSources owner{owned,std::make_shared<awl::GameClock>()};auto ui=prior_state(owner);
        expect(awl::prepare_world_map_player_context_icon(tables.get(),ui,0,1,&ui,&owner).status==S::Prepared,"lifetime fixture binds");
        const auto weak=std::weak_ptr<const awl::WorldMapContextIconResources>(owned);owned.reset();owner={};
        expect(!weak.expired() && ui.elements[0]->resources->frame(1,1,0,&frame) && ui.elements[0]->resources->textures().textures[0].raw_data[0]==0x21,"element outlives caller buffers and provider handles");
        ui={};expect(weak.expired(),"last element releases private resource pair");
    }
#if !defined(_MSC_VER) || !defined(_DEBUG)
    const auto live=allocation_probe::live;size_t rejected=0;bool complete=false;
    for(size_t n=0;n<40;++n){allocation_probe::remaining=n;allocation_probe::enabled=true;
        const auto status=awl::decode_world_map_context_icon_resources(agc.data(),agc.size(),tpl.data(),tpl.size(),&resources);allocation_probe::enabled=false;
        if(status==R::Decoded){complete=true;break;}
        ++rejected;expect(status==(n<3?R::AllocationFailure:R::UnsupportedTpl) && resources.get()==retained && allocation_probe::live==live,"each resource allocation stop preserves old pair and frees staging");
    }
    expect(complete && rejected==6,"all six allocations are covered: owner, shared control block, AGC bytes, texture descriptors and two mip arrays");
    std::cout<<"CONTEXT_ICON_RESOURCE_ALLOCATION_STOPS "<<rejected<<'\n';
#endif
    retained=resources.get();
    awl::filesystem_shutdown();expect(awl::load_world_map_context_icon_resources(&resources)==R::ReadFailure && resources.get()==retained,"unmounted loader preserves resource pair");
    // Task-created ignored fixture only; remove the exact files/directories
    // created here, never a recursive cleanup of an existing path.
    const auto root=std::filesystem::path("build")/("context-icon-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories("build");
    if(!std::filesystem::create_directory(root)){expect(false,"unique resource fixture directory must not already exist");return;}
    expect(std::filesystem::create_directory(root/"files"),"unique resource fixture subdirectory created");
    const auto write=[&](const char* name,const std::vector<uint8_t>& bytes){std::ofstream file(root/"files"/name,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));expect(bool(file),"fixture file writes");};
    write("opguide.agc",agc);expect(awl::filesystem_mount("/",root.string().c_str()),"fixture mount accepts native path");
    expect(awl::load_world_map_context_icon_resources(&resources)==R::ReadFailure && resources.get()==retained,"missing TPL after AGC read preserves old resource pair");
    write("opguide.tpl",bad_tpl);
    expect(awl::load_world_map_context_icon_resources(&resources)==R::UnsupportedTpl && resources.get()==retained,"invalid TPL after AGC read preserves old resource pair");
    {std::ofstream large(root/"files"/"opguide.agc",std::ios::binary);large.seekp(16*1024*1024);large.put(0);expect(bool(large),"oversize fixture writes bounded payload");}
    expect(awl::load_world_map_context_icon_resources(&resources)==R::ReadFailure && resources.get()==retained,"oversize AGC rejects before allocating file payload");
    write("opguide.agc",agc);write("opguide.tpl",tpl);
    expect(awl::load_world_map_context_icon_resources(&resources)==R::Loaded && resources.get()!=retained,"corrected files retry and atomically replace the resource pair");
    awl::filesystem_shutdown();
    std::filesystem::remove(root/"files"/"opguide.agc");std::filesystem::remove(root/"files"/"opguide.tpl");
    std::filesystem::remove(root/"files");std::filesystem::remove(root);
}
void local_checks(const char* disc) {
    expect(awl::filesystem_mount("/",disc),"local disc mounts");
    std::shared_ptr<const awl::WorldMapContextIconResources> resources;std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> tables;
    expect(awl::load_world_map_context_icon_resources(&resources)==R::Loaded,"actual opguide AGC/TPL load through native file layer");
    expect(awl::load_world_map_player_context_label_tables(&tables)==awl::WorldMapPlayerContextLabelTablesStatus::Loaded && tables->target_verified(),"local labels/setup constants verify target DOL");
    if(resources && tables){
        uint64_t digest=14695981039346656037ull;uint32_t count=0;
        for(uint32_t r=0;r<resources->root_count();++r){uint32_t at,groups;expect(resources->root(r,&at,&groups),"local root resolves");
            for(uint32_t g=0;g<groups;++g){awl::WorldMapContextIconGroup group;expect(resources->group(r,g,&group),"local group resolves");
                for(uint32_t f=0;f<group.frame_count;++f){awl::WorldMapContextIconFrame frame;expect(resources->frame(r,g,f,&frame),"local frame resolves");
                    for(uint32_t v:{r,g,f,frame.offset,frame.leaf_count,frame.leaf_offset,uint32_t(uint16_t(frame.pair[0]))<<16|uint16_t(frame.pair[1])})hash(digest,v);++count;}
            }
        }
        expect(count==48 && digest==0x3c966d31316bc245ull,"all local 5-root/48-frame metadata selectors match original instructions");
        std::cout<<"LOCAL_CONTEXT_ICON_SELECTORS "<<count<<' '<<std::hex<<digest<<std::dec<<'\n';
        expect(resources->textures().textures.size()==2,"actual TPL has two retained texture descriptors");
        comparisons(*tables,resources,0xdeecdda30db4fc69ull);
    }
    awl::filesystem_shutdown();
}
}
int main(int argc,char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    for(int kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}){
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE);_CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
#endif
    synthetic_checks();
    if(argc==3 && std::string(argv[1])=="--local")local_checks(argv[2]);
    else if(argc!=1)expect(false,"usage: world_map_context_icon_tests [--local disc]");
    return failures?1:0;
}
