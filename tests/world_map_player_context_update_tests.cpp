#include "awl/world_map_player_context_update.h"
#include "awl/world_map_player_state0_third.h"
#include "awl/world_map_scene_index.h"
#include "awl/filesystem.h"
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace allocation_probe {bool enabled=false;}
void* operator new(size_t size){if(allocation_probe::enabled)throw std::bad_alloc();if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void* operator new[](size_t s){return ::operator new(s);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,size_t) noexcept {::operator delete(p);}
namespace {
using S=awl::WorldMapPlayerContextUpdateStatus;
int failures=0;
void expect(bool yes,const char* why){if(!yes){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
void word(std::vector<uint8_t>& b,size_t p,uint32_t w){for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(w>>(24-i*8));}
void hash(uint64_t& h,uint32_t w){for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}
uint32_t bits(float f){uint32_t w;std::memcpy(&w,&f,4);return w;}
std::vector<uint8_t> query_fixture(bool profile=true,bool prefix=true) {
    std::vector<uint8_t> b(0x3c0);
    const std::array<std::array<uint32_t,3>,7> sections{{{0x100,0x8023df88,32},{0x120,0x8023e138,576},
        {0x360,0x80349e98,8},{0x368,0x8025113c,12},{0x374,0x8034a250,profile?44u:0u},{0x3a0,0x80349fcc,16},
        {0x3b0,0x80349fb0,prefix?16u:0u}}};
    for(size_t i=0;i<sections.size();++i){word(b,i*4,sections[i][0]);word(b,0x48+i*4,sections[i][1]);word(b,0x90+i*4,sections[i][2]);}
    for(uint32_t i=0;i<8;++i)word(b,0x100+i*4,i);
    for(uint32_t i=0;i<16;++i){word(b,0x120+i*36,i+1);for(uint32_t j=0;j<8;++j)word(b,0x124+i*36+j*4,UINT32_MAX);}
    word(b,0x360,0xbf000000);word(b,0x364,0x3f000000);
    for(const auto& p:std::array<std::array<uint32_t,2>,9>{{{0x384,0x3f490fdb},{0x394,0x3f028f5c},{0x398,0x3fa66666},
        {0x374,0},{0x39c,0x40400000},{0x3a0,0x43070000},{0x3a4,0x430f0000},{0x3a8,0x42cc0000},{0x3ac,0x42da0000}}})word(b,p[0],p[1]);
    for(const auto& p:std::array<std::array<uint32_t,2>,4>{{{0x3b0,0x431d0000},{0x3b4,0x43660000},{0x3b8,0x42c80000},{0x3bc,0x431ecccd}}})word(b,p[0],p[1]);
    return b;
}
std::vector<uint8_t> label_fixture() {
    std::vector<uint8_t> b(0x16bf);
    const std::array<std::array<uint32_t,3>,5> sections{{{0x100,0x80292a78,5332},{0x15d4,0x802a7b08,124},
        {0x1650,0x8004f19c,4},{0x1654,0x8034a3c8,4},{0x1658,0x8024f694,103}}};
    for(size_t i=0;i<sections.size();++i){word(b,i*4,sections[i][0]);word(b,0x48+i*4,sections[i][1]);word(b,0x90+i*4,sections[i][2]);}
    for(uint32_t row=0;row<31;++row){word(b,0x15d4+row*4,0x8004f19c);
        for(uint32_t context=0;context<86;++context){b[0x100+row*172+context]=uint8_t(10+row%5);b[0x100+row*172+86+context]=uint8_t(20+context%5);}}
    word(b,0x1650,0x38600000);word(b,0x1654,0x3f266666);return b;
}
awl::WorldMapPlayerContextUpdateState prior(const awl::WorldMapPlayerContextIconSources* icons=nullptr) {
    awl::WorldMapPlayerContextUpdateState out;out.labels_128={91,92,93,94};out.target_1388={81,82,83};
    out.byte_1350=71;out.secondary_byte_574=72;out.secondary_word_564=73;
    out.ui.resource_manager_1278=icons?icons->resources->identity():0;out.ui.labels_60={0u,0u,0u,0u};
    if(icons)for(auto& entry:out.ui.elements){entry.emplace();entry->flags_120=0;entry->attachments_10c_110={uint64_t(0),uint64_t(0)};}
    return out;
}
uint64_t snapshot(const awl::WorldMapPlayerContextUpdateState& state) {
    uint64_t h=14695981039346656037ull;
    for(uint32_t v:state.labels_128)hash(h,v);for(uint32_t v:state.target_1388)hash(h,v);
    for(uint32_t v:{uint32_t(state.byte_1350),uint32_t(state.secondary_byte_574),state.secondary_word_564})hash(h,v);
    const auto identity=[&](uint64_t v){hash(h,uint32_t(v>>32));hash(h,uint32_t(v));};
    hash(h,state.ui.resource_manager_1278.has_value());identity(state.ui.resource_manager_1278.value_or(0));
    for(const auto& v:state.ui.labels_60){hash(h,v.has_value());hash(h,v.value_or(UINT32_MAX));}
    for(const auto& e:state.ui.elements){hash(h,e.has_value());if(e){
        identity(e->resources?e->resources->identity():0);identity(uint64_t(reinterpret_cast<uintptr_t>(e->clock.get())));
        hash(h,e->flags_120.value_or(UINT32_MAX));hash(h,e->animation_deadline);hash(h,e->image_group.offset);hash(h,e->icon_frame.offset);
        for(float v:e->position_f0)hash(h,bits(v));hash(h,bits(e->depth_114));hash(h,bits(e->scalar_128));
        for(float v:e->vector_cc)hash(h,bits(v));hash(h,e->scalar_count_130);hash(h,e->vector_count_132);
    }}return h;
}
awl::WorldMapPlayerContextUpdateInput input(uint32_t seed) {
    awl::WorldMapPlayerContextUpdateInput p;p.current_key=17;p.scene_type=seed/64%2?1:2;
    p.position=seed/16%2?awl::WorldMapPlayerContextVector{139,99,105}:awl::WorldMapPlayerContextVector{};
    p.facing=seed%11?awl::WorldMapPlayerContextVector{0,0,1}:awl::WorldMapPlayerContextVector{};
    p.held_row=awl::WorldMapPlayerContextItemRow{0,uint8_t(std::array<uint32_t,8>{0,1,2,3,4,9,16,255}[seed%8]),uint8_t(0),0u};
    p.action=std::array<int32_t,6>{0,2,48,56,3,-1}[seed/8%6];
    p.tracked_14ac=seed/48%2?7u:0u;p.tracked_comparison={seed/96%2?5u:6u,5u};
    p.scene_predicate_1728e4=uint8_t(seed/192%2?255:0);p.secondary_byte_3f1=uint8_t(seed/384%2);
    p.label_observations.terrain_clear={true,true};p.label_observations.secondary_ground_available=true;return p;
}
void populate(uint32_t seed,const awl::WorldMapPlayerContextUpdateInput& p,awl::WorldMapSceneBucketRegistry& registry) {
    constexpr std::array<uint32_t,7> codes{1,420,421,631,632,UINT32_MAX,0x80000000};
    for(uint32_t i=0;i<3;++i){auto position=p.position;position[2]+=i==0?std::array<float,3>{.25f,.75f,2.5f}[seed/8%3]:i==1?.75f:2.0f;
        expect(registry.register_object(i+1,3,position),"matrix scene node registers before bucket placement");
        awl::WorldMapScenePositionUpdate placement;expect(registry.update_position(i+1,position,&placement) && placement.next_bucket==17,"matrix scene node publishes into bucket17");
        awl::WorldMapPlayerContextMetadata metadata;
        metadata.owner_code_4=codes[seed/24%7];metadata.owner_kind_8=seed%5==0?4u:i==0?std::array<uint32_t,6>{3,4,0,1,32,64}[seed/3%6]:i==1?4u:3u;
        metadata.facing_28=awl::WorldMapPlayerContextVector{0,0,-1};metadata.facing_threshold_38=-1.0f;metadata.max_squared_distance_3c=100.0f;
        metadata.payload=std::array<uint32_t,2>{seed%7?1u:UINT32_MAX,seed%7?13u:UINT32_MAX};
        expect(registry.set_context_metadata(i+1,metadata),"matrix resolved payload observation binds");}
}
void digest_step(uint64_t& h,uint32_t seed,const awl::WorldMapPlayerContextUpdateStep& step,const awl::WorldMapPlayerContextUpdateState& state) {
    const uint32_t route=step.status==S::Prepared?0u:step.status==S::RequiresTransition?1u:2u;
    for(uint32_t v:{seed,route,step.query_count,step.query_masks[0],step.query_masks[1]})hash(h,v);
    const auto& target=*step.target;for(uint32_t v:{uint32_t(target.found),target.record.owner_code,target.record.owner_kind,target.record.payload[0],target.record.payload[1],target.record.direction})hash(h,v);
    hash(h,step.labels?step.labels->classification:UINT32_MAX);hash(h,step.labels?step.labels->context:UINT32_MAX);
    hash(h,step.requested_state.value_or(UINT32_MAX));hash(h,step.transition_context_138c.value_or(UINT32_MAX));hash(h,step.requested_action_kind.value_or(UINT32_MAX));
    if(step.status==S::Prepared){hash(h,state.secondary_byte_574);hash(h,state.secondary_word_564);
        for(uint32_t v:state.labels_128)hash(h,v);hash(h,state.byte_1350);for(uint32_t v:state.target_1388)hash(h,v);
        for(const auto& v:state.ui.labels_60)hash(h,*v);}
}
void matrix(const awl::WorldMapPlayerContextTables& queries,const awl::WorldMapPlayerContextLabelTables& labels,
    uint64_t expected,const awl::WorldMapPlayerContextIconSources* icons=nullptr) {
    uint64_t digest=14695981039346656037ull;std::array<uint32_t,3> routes{};
    for(uint32_t seed=0;seed<512;++seed){const auto p=input(seed);awl::WorldMapSceneBucketRegistry registry;populate(seed,p,registry);
        awl::WorldMapPlayerContextUpdateSources sources{&queries,&labels,&registry,icons};auto state=prior(icons);const auto before=snapshot(state);
        const auto step=awl::prepare_world_map_player_context_update(sources,state,p,&state);
        expect(step.status==S::Prepared || step.status==S::RequiresTransition || step.status==S::RequiresContextAction,"all matrix cases complete before buttons or stop at a real request dependency");
        if(!step.target)continue;
        if(step.status==S::Prepared){++routes[0];expect(step.icons && step.icons->prepared_mask==15 && state.byte_1350==0,"complete context batch owns all four UI channels and clears parent byte");}
        else{++routes[step.status==S::RequiresTransition?1:2];expect(snapshot(state)==before,"request stop preserves all accepted player/secondary/UI state");}
        digest_step(digest,seed,step,state);
    }
    std::cout<<"CONTEXT_UPDATE_MATRIX 512 "<<std::hex<<digest<<std::dec<<" routes "<<routes[0]<<'/'<<routes[1]<<'/'<<routes[2]<<'\n';
    expect(digest==expected && routes[0] && routes[1] && routes[2],"composed routes/query masks/targets/descriptors/completed fields match original common-path instructions within the documented supplied-callee scope");
}
void region_checks(const awl::WorldMapPlayerContextSearchProfile& profile) {
    uint64_t h=14695981039346656037ull;constexpr std::array<float,9> xs{134,135,139,143,144,-0.0f,INFINITY,-INFINITY,NAN};
    constexpr std::array<float,9> zs{101,102,105,109,110,-0.0f,INFINITY,-INFINITY,NAN};uint32_t count=0;
    for(int32_t scene:{-1,0,1,2})for(float x:xs)for(float z:zs){const bool result=awl::world_map_player_context_region(profile,scene,{x,NAN,z});
        for(uint32_t v:{uint32_t(scene),bits(x),bits(z),uint32_t(result)})hash(h,v);++count;}
    std::cout<<"CONTEXT_REGION_COMPARISONS "<<count<<' '<<std::hex<<h<<std::dec<<'\n';
    expect(h==0x1a768f3d644d5264ull && count==324,"inclusive rectangle edges and unordered/skipped coordinates agree with complete original 1CF3C instructions");
}
void third_checks(const awl::WorldMapPlayerContextTables& queries,const awl::WorldMapPlayerContextLabelTables& labels,
    const awl::WorldMapPlayerContextIconSources* icons=nullptr) {
    using T=awl::WorldMapPlayerState0ThirdStatus;using C=awl::WorldMapControlStatus;using P=awl::WorldMapPadCaptureStatus;
    awl::WorldMapSceneBucketRegistry registry;awl::WorldMapPlayerContextUpdateSources sources{&queries,&labels,&registry,icons};
    auto clock=icons?icons->clock:std::make_shared<awl::GameClock>();awl::WorldMapUpdateInputs inputs(clock);
    awl::WorldMapPlayerContextIconSources foreign_icons;
    if(icons){foreign_icons=*icons;foreign_icons.clock=std::make_shared<awl::GameClock>();sources.icons=&foreign_icons;}
    awl::WorldMapPlayerState0ThirdState state{0x1234u,prior(icons)},out=state;
    awl::WorldMapPlayerState0ThirdInput observed;observed.guards=awl::WorldMapPlayerUpdateGuards{-1,0};
    auto p=input(0);p.scene_type=2;p.action=0;p.facing={0,0,1};p.held_row->type_0=uint8_t{0};p.tracked_14ac=0;
    observed.context=p;
    const auto saved=snapshot(*out.context);
    const auto stopped=[&](T expected){const auto result=awl::prepare_world_map_player_state0_third(inputs,sources,state,observed,&out);
        expect(result.status==expected && out.control_word_1384==0x1234u && snapshot(*out.context)==saved,
            "third callback stops preserve prior control and complete context projection");return result;};
    stopped(T::RequiresControl);inputs.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t{0}},0});
    expect(inputs.capture_control()==C::Captured,"third callback derives captured control");
    observed.guards.reset();stopped(T::RequiresGuards);observed.guards=awl::WorldMapPlayerUpdateGuards{-1,{}};stopped(T::RequiresGuardWord58c);
    observed.guards=awl::WorldMapPlayerUpdateGuards{0,{}};observed.context.reset();
    auto result=awl::prepare_world_map_player_state0_third(inputs,{},state,observed,&out);
    expect(result.status==T::SkippedByGuard && out.control_word_1384==inputs.control()->dispatch_word && snapshot(*out.context)==saved,
        "outer +680 skip stores control without reading +58c, context, profile or PAD");out=state;
    observed.guards=awl::WorldMapPlayerUpdateGuards{-1,0};stopped(T::RequiresContextInput);observed.context=p;
    auto no_state=state;no_state.context.reset();
    expect(awl::prepare_world_map_player_state0_third(inputs,sources,no_state,observed,&out).status==T::RequiresContextState,
        "third callback cannot invent prior UI/player/secondary constructor fields");
    observed.context->held_row.reset();result=stopped(T::ContextIncomplete);
    expect(result.context && result.context->status==S::RequiresHeldRow,"common dependencies precede PAD capture");observed.context=p;
    result=stopped(T::RequiresPad);expect(result.context && result.context->status==S::Prepared,"missing PAD rolls back a completely staged context update");
    inputs.observe_pad_sample({true,0});expect(inputs.capture_pad()==P::Captured,"idle raw PAD captured");
    allocation_probe::enabled=true;result=awl::prepare_world_map_player_state0_third(inputs,sources,state,observed,&out);allocation_probe::enabled=false;
    expect(result.status==T::Prepared && result.pressed==0u && out.control_word_1384==inputs.control()->dispatch_word && out.context->labels_128[2]==101,
        "idle callback publishes common fields and control without allocation");
    if(icons)for(uint32_t channel=0;channel<4;++channel){uint8_t old_icon=0,new_icon=0;
        expect(labels.icon(*state.context->ui.labels_60[channel],&old_icon) && labels.icon(*out.context->ui.labels_60[channel],&new_icon),"local channel labels resolve");
        const auto& e=out.context->ui.elements[channel];
        expect(e && e->clock==(old_icon!=new_icon?clock:state.context->ui.elements[channel]->clock),"changed icon binds the input clock; retained icon preserves prior state");}
    expect(clock->advance(),"next loop advances explicitly");expect(inputs.capture_control()==C::Captured,"fresh control captures");out=state;stopped(T::RequiresPad);
    expect(awl::prepare_world_map_player_state0_third(inputs,sources,state,observed,nullptr).status==T::InvalidInput,"null third output rejects");
    // Independently compare original entry routing. A reached common path
    // stops at the absent observed context state. Original intermediate control
    // stores are compared to the captured value; native stops roll them back.
    uint64_t digest=14695981039346656037ull;std::array<uint32_t,3> routes{};
    for(uint32_t seed=0;seed<512;++seed){
        auto c=std::make_shared<awl::GameClock>();expect(c->advance(),"entry fixture samples loop one");awl::WorldMapUpdateInputs owner(c);
        const uint8_t override_byte=uint8_t(seed/8%2);
        owner.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,override_byte},0});expect(owner.capture_control()==C::Captured,"entry fixture captures derived control");
        if(seed/16%2){owner.request_scene_mode(4,0);awl::WorldMapSceneModeCommit commit;
            expect(owner.commit_scene_fields(0,uint8_t{0},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields && owner.scene().scene_type_68==1,"owned scene-manager commits scene one without blocking group five");}
        awl::WorldMapPlayerState0ThirdInput obs;obs.guards=awl::WorldMapPlayerUpdateGuards{
            std::array<int32_t,8>{-1,-1,0,1,-2,-1,-1,-1}[seed%8],std::array<int32_t,8>{0,1,0,0,0,0,-1,0}[seed%8]};
        obs.context=p;obs.context->action=std::array<int32_t,4>{0,2,3,-1}[seed/32%4];obs.context->scene_type=seed/128%2?1:2;
        obs.context->position={seed/256?200.0f:156.0f,99,120};
        awl::WorldMapPlayerState0ThirdState old{0x1234u,{}},next=old;
        const auto step=awl::prepare_world_map_player_state0_third(owner,sources,old,obs,&next);
        const uint32_t route=step.status==T::SkippedByGuard?0u:step.status==T::RequiresContextState?1u:2u;
        expect(step.status==T::SkippedByGuard || step.status==T::RequiresContextState || step.status==T::RequiresPrefixAction,"entry matrix reaches only independently compared boundaries");
        expect(!next.context && next.control_word_1384==(route==0?owner.control()->dispatch_word:0x1234u),"entry stops preserve state; skipped guards alone store control");
        ++routes[route];for(uint32_t v:{seed,route,owner.control()->dispatch_word})hash(digest,v);
    }
    std::cout<<"STATE0_THIRD_ENTRY_COMPARISONS 512 "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0x38144f262c0b1405ull && routes==std::array<uint32_t,3>{416,78,18},"entry decisions agree with original dispatcher/wrapper/prefix instructions");
    digest=14695981039346656037ull;
    for(uint16_t buttons:std::array<uint16_t,8>{0,0x100,0x800,0x900,0x200,0x400,0x1000,0xffff}){
        auto c=std::make_shared<awl::GameClock>();awl::WorldMapUpdateInputs owner(c);
        owner.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t{0}},0});expect(owner.capture_control()==C::Captured,"button fixture captures control");
        owner.observe_pad_sample({true,buttons});expect(owner.capture_pad()==P::Captured,"button fixture uses the actual HSD raw-sample edge filter");
        auto next=state;auto obs=observed;obs.context=p;
        const auto step=awl::prepare_world_map_player_state0_third(owner,sources,state,obs,&next);
        const auto required=step.required_button.value_or(0);hash(digest,buttons);hash(digest,required);
        expect(step.pressed==uint32_t(buttons) && step.status==(required?T::RequiresButtonAction:T::Prepared),"only reached A/Y presses stop after complete context staging");
        if(required)expect(next.control_word_1384==0x1234u && snapshot(*next.context)==snapshot(*state.context),"untranslated button actions do not publish staged UI/context changes");
        // A held button may repeat later, but cannot enter these press branches.
        for(unsigned i=0;i<15;++i){expect(c->advance(),"held-button loop advances");owner.observe_pad_sample({true,buttons});expect(owner.capture_pad()==P::Captured,"held button captures once per loop");}
        expect(owner.capture_control()==C::Captured,"held-button control refreshes");
        const auto held=awl::prepare_world_map_player_state0_third(owner,sources,state,obs,&next);
        expect(held.status==T::Prepared && held.pressed==0u && owner.pad_for_loop()->frame.repeated==uint32_t(buttons),"repeat bits cannot substitute for the original pressed word");
    }
    std::cout<<"STATE0_THIRD_BUTTON_COMPARISONS 8 "<<std::hex<<digest<<std::dec<<'\n';
    // Independently obtained from the ignored original pressed-branch probe.
    expect(digest==0xe028b33f51768f48ull,"pressed A/Y priority agrees with original tail instructions");
    auto prefix=*queries.search_profile();prefix.region_xz=*prefix.prefix_region_xz;digest=14695981039346656037ull;
    for(int32_t scene:{-1,0,1,2})for(float x:std::array<float,9>{156,157,200,230,231,-0.0f,INFINITY,-INFINITY,NAN})
        for(float z:std::array<float,9>{99,100,120,158.8f,160,-0.0f,INFINITY,-INFINITY,NAN}){
            const bool inside=awl::world_map_player_context_region(prefix,scene,{x,NAN,z});for(uint32_t v:{uint32_t(scene),bits(x),bits(z),uint32_t(inside)})hash(digest,v);}
    expect(digest==0x67daad093fc2cbc4ull,"complete prefix rectangle agrees with original inclusive/unordered 1CD18 decisions");
    inputs.request_scene_mode(4,0);awl::WorldMapSceneModeCommit scene_commit;
    expect(inputs.commit_scene_fields(0,uint8_t{0},&scene_commit)==awl::WorldMapSceneFilterStatus::CommittedFields,"prefix scene one commits");
    auto no_prefix_bytes=query_fixture(true,false);std::shared_ptr<const awl::WorldMapPlayerContextTables> no_prefix;
    expect(awl::decode_world_map_player_context_tables(no_prefix_bytes.data(),no_prefix_bytes.size(),&no_prefix)==awl::WorldMapPlayerContextTablesStatus::Decoded &&
        no_prefix->search_profile() && !no_prefix->search_profile()->prefix_region_xz,"older common-only profile stays explicit about its absent prefix constants");
    sources.queries=no_prefix.get();observed.context=p;observed.context->action=3;observed.context->scene_type=1;observed.context->position={200,0,120};out=state;
    stopped(T::RequiresPrefixProfile);observed.context->scene_type=2;stopped(T::RequiresPrefixAction);
    sources.queries=&queries;observed.context->scene_type=1;observed.context->position[0]=156;stopped(T::RequiresPrefixAction);
    observed.context->position[0]=200;stopped(T::RequiresPad);
    observed.context=p;awl::PadSample analog;analog.connected=true;analog.stick_x=80;analog.substick_y=80;analog.trigger_l=255;
    inputs.observe_pad_sample(analog);expect(inputs.capture_pad()==P::Captured,"direction/trigger-only raw sample captures");
    result=awl::prepare_world_map_player_state0_third(inputs,sources,state,observed,&out);
    expect(result.status==T::Prepared && result.pressed && *result.pressed!=0 && !result.required_button,"analog direction/trigger pressed bits cannot enter A/Y action branches");
    // A filtered group does not even enter 31504 or require guard/PAD data.
    inputs.request_scene_mode(0,0);awl::WorldMapSceneModeCommit commit;
    expect(inputs.commit_scene_fields(0,{},&commit)==awl::WorldMapSceneFilterStatus::CommittedFields,"filtered scene commits");out=state;
    expect(awl::prepare_world_map_player_state0_third(inputs,{},state,{},&out).status==T::Filtered && out.control_word_1384==state.control_word_1384 && snapshot(*out.context)==snapshot(*state.context),"filtered dispatch preserves complete parent/context without guard or PAD observations");
}
void checks() {
    const auto qb=query_fixture(),lb=label_fixture();std::shared_ptr<const awl::WorldMapPlayerContextTables> queries;
    std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> labels;
    expect(awl::decode_world_map_player_context_tables(qb.data(),qb.size(),&queries)==awl::WorldMapPlayerContextTablesStatus::Decoded && queries->search_profile().has_value(),"synthetic query search/region constants decode");
    expect(awl::decode_world_map_player_context_label_tables(lb.data(),lb.size(),&labels)==awl::WorldMapPlayerContextLabelTablesStatus::Decoded,"synthetic descriptors and equal-icon map decode");
    if(!queries || !labels)return;matrix(*queries,*labels,0xa1ff1f7b5913cf58ull);region_checks(*queries->search_profile());third_checks(*queries,*labels);
    awl::WorldMapSceneBucketRegistry registry;awl::WorldMapPlayerContextUpdateSources sources{queries.get(),labels.get(),&registry,nullptr};
    auto p=input(0);p.scene_type=1;p.position={139,99,105};p.facing={0,0,1};p.action=0;p.held_row->type_0=uint8_t(4);
    p.tracked_14ac=0;p.scene_predicate_1728e4=uint8_t(255);p.secondary_byte_3f1=uint8_t(0);auto state=prior(),out=prior();
    auto step=awl::prepare_world_map_player_context_update(sources,state,p,&out);
    expect(step.status==S::Prepared && out.labels_128==std::array<uint32_t,4>{49,0,101,20} && out.secondary_byte_574==1 && out.secondary_word_564==53 && out.target_1388==std::array<uint32_t,3>{0,UINT32_MAX,UINT32_MAX},"region49 override follows scene48 override and no-hit defaults publish");
    p.secondary_byte_3f1=uint8_t(1);step=awl::prepare_world_map_player_context_update(sources,state,p,&out);
    expect(step.status==S::Prepared && out.labels_128[0]==48,"nonzero secondary byte keeps earlier scene override");
    p.action=-1;p.secondary_byte_3f1=uint8_t(0);step=awl::prepare_world_map_player_context_update(sources,state,p,&out);
    expect(step.status==S::Prepared && out.target_1388[0]==UINT32_MAX && out.secondary_word_564==53 && out.labels_128[2]==0,"signed nonzero no-hit action fallback does not fabricate a found target");
    const auto saved=snapshot(out);const auto stopped=[&](S expected){const auto result=awl::prepare_world_map_player_context_update(sources,state,p,&out);
        expect(result.status==expected && snapshot(out)==saved,"missing/late reached dependency preserves accepted output");return result;};
    p.held_row.reset();stopped(S::RequiresHeldRow);p.held_row=awl::WorldMapPlayerContextItemRow{0,{},{},{}};stopped(S::RequiresHeldType);
    p.held_row->type_0=uint8_t(4);p.tracked_14ac.reset();stopped(S::RequiresTrackedTarget);p.tracked_14ac=7u;p.tracked_comparison[1].reset();stopped(S::RequiresTrackedComparison);
    p.tracked_14ac=0;p.scene_predicate_1728e4.reset();stopped(S::RequiresScenePredicate);p.scene_predicate_1728e4=uint8_t(0);
    p.secondary_byte_3f1.reset();stopped(S::RequiresSecondaryByte);p.secondary_byte_3f1=uint8_t(0);
    state.ui.labels_60[2].reset();step=stopped(S::IconsIncomplete);expect(step.icons && step.icons->channel==2 && step.icons->prepared_mask==0xb,"late UI failure rolls back earlier secondary/player/three channel staging");state=prior();
    sources.labels=nullptr;step=stopped(S::LabelsIncomplete);expect(step.labels && step.labels->status==awl::WorldMapPlayerContextLabelStatus::RequiresTables,"label dependency remains explicit");sources.labels=labels.get();
    sources.registry=nullptr;step=stopped(S::QueryIncomplete);expect(step.query && step.query->status==awl::WorldMapPlayerContextStatus::RequiresSceneRegistry,"reached registry dependency precedes later fields");sources.registry=&registry;
    auto legacy=query_fixture(false);std::shared_ptr<const awl::WorldMapPlayerContextTables> query_only;
    expect(awl::decode_world_map_player_context_tables(legacy.data(),legacy.size(),&query_only)==awl::WorldMapPlayerContextTablesStatus::Decoded && !query_only->search_profile(),"query-only fixture remains supported without inventing common-path constants");
    sources.queries=query_only.get();stopped(S::RequiresSearchProfile);sources.queries=queries.get();
    const auto* old=queries.get();for(const auto& bad:std::array<std::array<uint32_t,2>,7>{{{0x39c,0},{0x3a0,0x7fc00000},{0x3a8,0x430f0000},{0xa0,20},{0x3b0,0x7fc00000},{0x3b4,0x43000000},{0xa8,12}}}){
        auto bytes=qb;word(bytes,bad[0],bad[1]);expect(awl::decode_world_map_player_context_tables(bytes.data(),bytes.size(),&queries)==awl::WorldMapPlayerContextTablesStatus::UnsupportedLayout && queries.get()==old,"malformed search distances/region constants preserve query owner");}
    allocation_probe::enabled=true;step=awl::prepare_world_map_player_context_update(sources,state,p,&state);allocation_probe::enabled=false;
    expect(step.status==S::Prepared,"complete registry/descriptor/UI/player/secondary transaction allocates nothing and permits aliasing");
    const auto rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"rounding test mode changes");stopped(S::QueryIncomplete);expect(std::fesetround(rounding)==0,"rounding restored");
    expect(awl::prepare_world_map_player_context_update(sources,state,p,nullptr).status==S::InvalidInput,"null output rejects");
    {
        awl::WorldMapSceneBucketRegistry live;awl::WorldMapScenePositionUpdate placement;
        expect(live.register_object(9,3,{0,0,.75f}) && live.update_position(9,{0,0,.75f},&placement),"context target lifecycle publishes one actual registry node");
        awl::WorldMapPlayerContextMetadata metadata;metadata.owner_code_4=50u;metadata.owner_kind_8=0u;
        metadata.facing_28=awl::WorldMapPlayerContextVector{0,0,-1};metadata.facing_threshold_38=-1.0f;metadata.max_squared_distance_3c=100.0f;
        metadata.payload=std::array<uint32_t,2>{1,13};expect(live.set_context_metadata(9,metadata),"context target fields bind separately from owner code/kind");
        auto observed=input(1);observed.scene_type=2;observed.position={0,0,0};observed.facing={0,0,1};observed.action=0;observed.tracked_14ac=0;
        observed.label_observations.query_mode=1;observed.label_observations.source=awl::WorldMapPlayerContextLabelSource::Action;observed.label_observations.value=1000;
        awl::WorldMapPlayerContextUpdateSources linked{queries.get(),labels.get(),&live,nullptr};auto old_state=prior(),accepted=prior();
        allocation_probe::enabled=true;auto result=awl::prepare_world_map_player_context_update(linked,old_state,observed,&accepted);allocation_probe::enabled=false;
        expect(result.status==S::Prepared && result.labels->classification==1 && result.labels->context==6 && accepted.target_1388==std::array<uint32_t,3>{50,1,13} && accepted.secondary_word_564==1,"owned target query binds payloads and derived descriptor source/mode without allocation");
        const auto held=snapshot(accepted);observed.tracked_14ac=7u;observed.tracked_comparison={5u,6u};linked.labels=nullptr;
        result=awl::prepare_world_map_player_context_update(linked,old_state,observed,&accepted);
        expect(result.status==S::RequiresTransition && result.requested_state==0x99u && result.transition_context_138c==2u && !result.labels && snapshot(accepted)==held,"tracked mismatch stops before even a missing label provider and defers transition writes");
        linked.labels=labels.get();observed.tracked_14ac=0;
        metadata.owner_code_4=421u;expect(live.set_context_metadata(9,metadata),"context-action boundary metadata updates");
        old_state.ui.labels_60[2].reset();result=awl::prepare_world_map_player_context_update(linked,old_state,observed,&accepted);
        expect(result.status==S::IconsIncomplete && !result.requested_action_kind && snapshot(accepted)==held,"late icon stop precedes reached no-button action creation and preserves completed output");
        old_state=prior();result=awl::prepare_world_map_player_context_update(linked,old_state,observed,&accepted);
        expect(result.status==S::RequiresContextAction && result.requested_action_kind==0x2du && snapshot(accepted)==held,"in-range owner code requires actual action creation instead of publishing partial UI/player writes");
        metadata.owner_code_4=50u;metadata.payload=std::array<uint32_t,2>{47,64};expect(live.set_context_metadata(9,metadata),"context47 payload keeps owner kind separate");
        observed.held_item=1;observed.held_row=awl::WorldMapPlayerContextItemRow{1,uint8_t(0),uint8_t(0),0u};observed.action=48;
        observed.label_observations.item_row.reset();result=awl::prepare_world_map_player_context_update(linked,old_state,observed,&accepted);
        expect(result.status==S::LabelsIncomplete && result.labels->status==awl::WorldMapPlayerContextLabelStatus::RequiresItemRow && snapshot(accepted)==held,"action descriptor context47 needs its separate item-zero row; held-item row cannot substitute");
        observed.label_observations.item_row=awl::WorldMapPlayerContextItemRow{0,uint8_t(0),uint8_t(0),0u};
        result=awl::prepare_world_map_player_context_update(linked,old_state,observed,&accepted);
        expect(result.status==S::Prepared && result.labels->classification==3 && result.labels->context==47 && accepted.secondary_word_564==47,"separate action/item-zero observation completes after retry");
        expect(live.unregister_object(9),"context target removal unlinks owned node");
        result=awl::prepare_world_map_player_context_update(linked,accepted,observed,&accepted);
        expect(result.status==S::Prepared && !result.target->found && accepted.target_1388==std::array<uint32_t,3>{48,UINT32_MAX,UINT32_MAX} && accepted.secondary_word_564==53,"target removal restores no-hit payloads and keeps the current action fallback");
    }
}
void local(const char* disc) {
    expect(awl::filesystem_mount("/",disc),"local disc mounts");std::shared_ptr<const awl::WorldMapPlayerContextTables> queries;
    std::shared_ptr<const awl::WorldMapPlayerContextLabelTables> labels;awl::WorldMapPlayerContextIconSources icons;
    expect(awl::load_world_map_player_context_tables(&queries)==awl::WorldMapPlayerContextTablesStatus::Loaded && queries->target_verified(),"local query profile verifies target DOL");
    expect(awl::load_world_map_player_context_label_tables(&labels)==awl::WorldMapPlayerContextLabelTablesStatus::Loaded && labels->target_verified(),"local label/setup metadata verifies target DOL");
    expect(awl::load_world_map_context_icon_resources(&icons.resources)==awl::WorldMapContextIconResourcesStatus::Loaded,"local icon provider loads");icons.clock=std::make_shared<awl::GameClock>();
    if(queries && labels && icons.resources){matrix(*queries,*labels,0xe29e0f3454d69580ull,&icons);expect(icons.clock->state().raw_time==0 && icons.clock->state().loop_count==0,"composed local updates never advance the shared clock");third_checks(*queries,*labels,&icons);}
    awl::filesystem_shutdown();
}
}
int main(int argc,char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    for(int kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}){_CrtSetReportMode(kind,_CRTDBG_MODE_FILE);_CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);}
#endif
    checks();if(argc==3 && std::string(argv[1])=="--local")local(argv[2]);else if(argc!=1)expect(false,"usage: world_map_player_context_update_tests [--local disc]");
    return failures?1:0;
}
