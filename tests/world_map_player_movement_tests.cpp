#include "awl/world_map_player_movement.h"
#include "awl/world_map_trigger_asset.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include <cfenv>
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

namespace allocation_probe {bool enabled=false;size_t remaining=0,live=0;}
void* operator new(size_t size){if(allocation_probe::enabled){if(!allocation_probe::remaining)throw std::bad_alloc();--allocation_probe::remaining;}if(void* p=std::malloc(size?size:1)){++allocation_probe::live;return p;}throw std::bad_alloc();}
void operator delete(void* p) noexcept {if(p)--allocation_probe::live;std::free(p);}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,size_t) noexcept {::operator delete(p);}

namespace {
using S=awl::WorldMapPlayerMovementStatus;
int failures=0;
void expect(bool yes,const char* why){if(!yes){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
void word(std::vector<uint8_t>& b,size_t p,uint32_t w){for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(w>>(24-i*8));}
void half(std::vector<uint8_t>& b,size_t p,int16_t w){b[p]=uint8_t(uint16_t(w)>>8);b[p+1]=uint8_t(w);}
uint32_t bits(float f){uint32_t w;std::memcpy(&w,&f,4);return w;}
void hash(uint64_t& h,uint32_t w){for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}
std::vector<uint8_t> terrain(){
    // Invented large flat triangle, independently specified height four.
    std::vector<uint8_t> b(92);word(b,0,0xe7e3f1f4);b[4]=1;b[6]=1;
    for(size_t i=0;i<3;++i){half(b,8+i*2,-200);half(b,14+i*2,900);}
    word(b,0x24,1);word(b,0x28,3);word(b,0x30,0x34);word(b,0x34,0x3c);
    half(b,0x3c,0x20);half(b,0x40,1);half(b,0x42,2);
    constexpr std::array<std::array<int16_t,3>,3> points{{{-200,4,-200},{900,4,-200},{-200,4,900}}};
    for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)half(b,0x44+i*8+j*2,points[i][j]);return b;
}
std::vector<uint8_t> triggers(bool first_hit=false,bool second_hit=false){
    std::vector<uint8_t> b(252);word(b,0,0xf0f0e1ec);word(b,4,61);size_t group55=0;
    for(size_t i=0;i<61;++i){const auto at=b.size();word(b,8+i*4,uint32_t(at));b.resize(at+(i==55?12:4));if(i==55){word(b,at,2);group55=at;}}
    for(size_t slot=0;slot<2;++slot){const auto at=b.size();word(b,group55+4+slot*4,uint32_t(at));b.resize(at+68);word(b,at,0);word(b,at+4,5);
        const bool hit=slot?second_hit:first_hit;const float x=hit?53.0f:800.0f,z=hit?64.0f:800.0f;
        const std::array<std::array<float,3>,5> square{{{x,0,z},{x+2,0,z},{x+2,0,z+2},{x,0,z+2},{x,0,z}}};
        for(size_t i=0;i<5;++i)for(size_t j=0;j<3;++j)word(b,at+8+i*12+j*4,bits(square[i][j]));}
    return b;
}
struct Height {int calls=0,fail=-1;const std::vector<uint8_t>* bytes=nullptr;};
bool height(const std::array<float,3>& position,float* out,void* context){
    auto& h=*static_cast<Height*>(context);if(h.calls++==h.fail)return false;
    if(!h.bytes){*out=4;return true;}
    awl::CollisionHeightSample sample;if(!awl::sample_type1_collision_height(h.bytes->data(),h.bytes->size(),position[0],position[2],&sample))return false;
    *out=sample.height;return true;
}
struct Fixture {
    std::shared_ptr<awl::GameClock> clock=std::make_shared<awl::GameClock>();
    awl::WorldMapUpdateInputs inputs{clock};awl::WorldMapPlayerMovementState player;awl::WorldMapSceneBucketRegistry scene;
    Fixture(std::array<float,3> position={53.99f,7,65}){
        player.scene_identity=1;player.pose={1,position,{0,0,1}};player.steering.facing_z=1;
        awl::WorldMapCameraInitialProfile profile;expect(awl::make_world_map_camera_initial_profile(&profile),"checked constructor camera profile prepares");player.camera=profile.view_query;
        // Explicit diagnostic yaw/flag observation makes +X cross X=54.
        player.camera->target.camera.yaw=0.0f;player.camera->target.camera.flag_98=true;
        player.shared_contact=awl::WorldMapMovementContactTail{99,{91,92,93},{94,95,96},7,8,9,true};
        awl::WorldMapScenePositionUpdate update;expect(scene.register_object(1,1,position) && scene.update_pose(1,1,position,player.pose.heading,&update),"observed player pose binds a registered node");
    }
    void capture(awl::PadSample sample={true,0,80}){expect(clock->advance() && (inputs.observe_pad_sample(sample),inputs.capture_pad()==awl::WorldMapPadCaptureStatus::Captured),"explicit tick captures fresh PAD");}
};
void vector_hash(uint64_t& h,const std::array<float,3>& a){for(float v:a)hash(h,bits(v));}
void camera_hash(uint64_t& h,const awl::WorldMapCameraViewQuery& q){
    const auto& c=q.target.camera;vector_hash(h,c.position);hash(h,bits(c.field_18));hash(h,bits(c.yaw));vector_hash(h,c.bounds_min);vector_hash(h,c.bounds_max);hash(h,c.flag_98);hash(h,c.flag_99);
    vector_hash(h,q.target.origin_offset_0c);hash(h,bits(q.target.distance_30));hash(h,bits(q.target.pitch_offset_8c));hash(h,bits(q.target.yaw_offset_90));vector_hash(h,q.up_vector_24);hash(h,bits(q.pitch_offset_44));hash(h,bits(q.yaw_offset_48));
}
uint64_t snapshot(const Fixture& f){
    uint64_t h=14695981039346656037ull;const auto& p=f.player;hash(h,uint32_t(p.scene_identity));hash(h,uint32_t(p.scene_identity>>32));hash(h,uint32_t(p.pose.scene_type));vector_hash(h,p.pose.position);vector_hash(h,p.pose.heading);
    for(float v:{p.steering.direction_x,p.steering.direction_z,p.steering.facing_x,p.steering.facing_z,p.steering.target_speed,p.steering.current_speed,p.steering.intensity})hash(h,bits(v));
    hash(h,p.camera.has_value());if(p.camera)camera_hash(h,*p.camera);
    hash(h,p.shared_contact.has_value());if(p.shared_contact){const auto& t=*p.shared_contact;hash(h,uint32_t(t.recorded_category));vector_hash(h,t.recorded_prior);vector_hash(h,t.recorded_resolved);hash(h,t.polygon_queries);hash(h,t.state_requests);hash(h,uint32_t(t.accepted_slot));hash(h,t.movement_reset_requested);}
    hash(h,p.camera_update.has_value());if(p.camera_update){camera_hash(h,p.camera_update->final_camera);for(float v:p.camera_update->update.final_matrix_50)hash(h,bits(v));vector_hash(h,p.camera_update->update.final_target);hash(h,bits(p.camera_update->movement_plane_constant_180));hash(h,p.camera_update->target_on_positive_side_188);}
    for(int bucket=-1;bucket<59;++bucket){const auto nodes=f.scene.snapshot(bucket);hash(h,uint32_t(nodes.size()));for(const auto& n:nodes){hash(h,uint32_t(n.identity));hash(h,uint32_t(n.scene_type));vector_hash(h,n.position);hash(h,uint32_t(n.bucket));hash(h,n.context.facing_28.has_value());if(n.context.facing_28)vector_hash(h,*n.context.facing_28);}}
    hash(h,f.clock->state().loop_count);hash(h,f.clock->state().raw_time);return h;
}
void guards(){
    auto clock=std::make_shared<awl::GameClock>();awl::WorldMapUpdateInputs inputs(clock);uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed){const int32_t g680=std::array<int32_t,4>{-1,0,INT32_MIN,INT32_MAX}[seed%4];const int32_t g58=std::array<int32_t,8>{-1,0,1,2,3,INT32_MIN,INT32_MAX,0}[seed/4%8];const uint32_t mask=seed/32;
        awl::WorldMapPlayerMovementInput input;input.guards=awl::WorldMapPlayerUpdateGuards{g680,g680==-1?std::optional<int32_t>(g58):std::nullopt};input.outputs={bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8)};
        allocation_probe::enabled=true;const auto r=awl::advance_world_map_player_movement(inputs,input,nullptr,nullptr);allocation_probe::enabled=false;
        const bool active=g680==-1 && g58==0;expect(r.status==(active?S::RequiresOwners:S::SkippedByGuard),"guard defaults precede all unreached owners/PAD/assets and allocate nothing");
        for(uint32_t w:{seed,uint32_t(g680),uint32_t(g58),mask,uint32_t(active)})hash(digest,w);
        std::array<uint32_t,8> outputs;for(uint32_t i=0;i<8;++i)outputs[i]=(0x6e000000u+i)^(seed*0x10203u);
        if(r.metadata)outputs[0]=*r.metadata;if(r.direction)outputs[1]=*r.direction;
        if(r.position)for(size_t i=0;i<3;++i)outputs[2+i]=bits((*r.position)[i]);if(r.axis)for(size_t i=0;i<3;++i)outputs[5+i]=bits((*r.axis)[i]);
        for(uint32_t w:outputs)hash(digest,w);hash(digest,r.intensity?bits(*r.intensity):0x76543210u);
    }
    expect(digest==0x109ab1feef307ab0ull,"512 complete original guard/default and active-entry cases agree for all nullable destinations");
    digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<128;++seed){const std::array<float,3> position{float(int(seed%17)-8),seed%2?-0.0f:float(seed%7),float(int(seed%11)-5)};
        const std::array<float,3> axis{-.5f,float(int(seed%5)-2),.5f};awl::WorldMapContactResult miss;
        expect(awl::query_world_map_directional_contact(nullptr,0,1,{},position,axis,&miss),"empty directional list yields the complete caller miss projection");
        hash(digest,seed);vector_hash(digest,position);vector_hash(digest,axis);hash(digest,miss.metadata);hash(digest,miss.direction_code);vector_hash(digest,miss.world_position);vector_hash(digest,miss.heading_axis);
    }
    expect(digest==0xe97d4137fa630901ull,"128 actual original miss-store cases agree, including signed zero and the current full facing vector");
    awl::WorldMapPlayerMovementInput input;expect(awl::advance_world_map_player_movement(inputs,input,nullptr,nullptr).status==S::RequiresGuards,"missing first guard remains unknown");input.guards=awl::WorldMapPlayerUpdateGuards{-1,{}};
    expect(awl::advance_world_map_player_movement(inputs,input,nullptr,nullptr).status==S::RequiresGuardWord58c,"reached second guard is required");
}
void workflow(){
    Fixture f;auto bytes=terrain(),spl=triggers();awl::WorldMapTriggerAsset trigger;expect(trigger.load_from_bytes(spl.data(),spl.size()),"invented no-contact polygons load");
    awl::WorldMapMovementQuery dependency;dependency.collision.terrain_data=bytes.data();dependency.collision.terrain_size=bytes.size();
    // Spoofed nondependency fields must be ignored in favor of owned inputs.
    dependency.current_position={-9,-8,-7};dependency.camera_yaw_radians=std::numeric_limits<float>::quiet_NaN();dependency.pad.stick_x=-80;
    Height h;awl::WorldMapPlayerMovementInput input;input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.outputs={true,true,true,true};input.dependencies=&dependency;
    const auto stopped=[&](S status){const auto before=snapshot(f);const auto r=awl::advance_world_map_player_movement(f.inputs,input,&f.player,&f.scene);expect(r.status==status && snapshot(f)==before,"late movement stop preserves pose, steering, camera, shared contact, registry and clock");return r;};
    stopped(S::RequiresPad);f.capture();const auto camera=f.player.camera;f.player.camera.reset();stopped(S::RequiresCamera);f.player.camera=camera;
    const auto* saved=input.dependencies;input.dependencies=nullptr;stopped(S::RequiresCollision);input.dependencies=saved;
    stopped(S::RequiresTriggerOwner);input.triggers=&trigger;
    expect(stopped(S::RequiresSecondaryByte).stopped_slot==-1,"completed polygon queries are not reported as a stopped slot on a later camera-source failure");
    input.secondary_byte_3f1=uint8_t{0};stopped(S::RequiresHeightSource);
    input.sample_height=height;input.sample_context=&h;h.fail=1;stopped(S::CameraIncomplete);expect(h.calls==2,"second camera sample fails after earlier scene/output/tail staging");h.fail=-1;h.calls=0;
    for(size_t slot=0;slot<2;++slot){auto hit=triggers(slot==0,slot==1);awl::WorldMapTriggerAsset touched;expect(touched.load_from_bytes(hit.data(),hit.size()),"contact-stop polygon fixture loads");input.triggers=&touched;
        const auto r=stopped(S::RequiresActionRequest);expect(r.stopped_slot==int32_t(slot) && r.tail->polygon_queries==slot+1 && h.calls==0,"first reached trigger contact stops before action/reset, later slots and camera");}
    input.triggers=&trigger;
    {Fixture inside({175,7,115});inside.capture();auto region_input=input;region_input.secondary_byte_3f1.reset();h.calls=0;
        const auto r=awl::advance_world_map_player_movement(inside.inputs,region_input,&inside.player,&inside.scene);
        expect(r.status==S::Advanced && r.camera->followup.region_index==0,"fixed camera region without an L press skips the unknown secondary byte");
        awl::PadSample press{true,0,80};press.trigger_l=200;inside.capture(press);const auto before=snapshot(inside);
        expect(awl::advance_world_map_player_movement(inside.inputs,region_input,&inside.player,&inside.scene).status==S::RequiresSecondaryByte && snapshot(inside)==before,"reached L press requires the secondary byte even inside a fixed camera region");}
    const int round=std::fegetround();std::fesetround(FE_DOWNWARD);stopped(S::UnsupportedNumerics);std::fesetround(round);
    const auto identity=f.player.scene_identity;f.player.scene_identity=999;stopped(S::InvalidInput);f.player.scene_identity=identity;
    const auto position=f.player.pose.position;f.player.pose.position[0]+=1;stopped(S::InvalidInput);f.player.pose.position=position;
    const auto category=f.player.pose.scene_type;f.player.pose.scene_type=2;stopped(S::UnsupportedCategory);f.player.pose.scene_type=category;
    awl::WorldMapScenePositionUpdate unrelated;expect(f.scene.register_object(9,1,{60,4,65}) && f.scene.update_position(9,{60,4,65},&unrelated),"foreign scene node occupies destination bucket");
#if !defined(_DEBUG)
    size_t allocations=0;for(;allocations<256;++allocations){const auto before=snapshot(f);const auto live=allocation_probe::live;allocation_probe::remaining=allocations;allocation_probe::enabled=true;
        const auto r=awl::advance_world_map_player_movement(f.inputs,input,&f.player,&f.scene);allocation_probe::enabled=false;
        if(r.status==S::Advanced)break;expect(r.status==S::AllocationFailure && snapshot(f)==before && allocation_probe::live==live,"every reached allocation failure releases staging and preserves the complete entry projection");}
    expect(allocations>=3 && allocations<256,"allocation sweep reaches collision validation, registry copy and destination growth");
    std::cout<<"PLAYER_MOVEMENT_ALLOCATION_STOPS "<<allocations<<'\n';
#else
    expect(awl::advance_world_map_player_movement(f.inputs,input,&f.player,&f.scene).status==S::Advanced,"supported movement publishes a complete CPU projection");
#endif
    const auto* player=f.scene.object(1);expect(player && f.player.pose.position[0]>54 && f.player.pose.position[1]==4 && player->position==f.player.pose.position &&
        player->context.facing_28==f.player.pose.heading && f.scene.snapshot(6).back().identity==1 && f.scene.snapshot(6).front().identity==9,"movement publishes current facing/position and appends behind the destination's existing tail");
    expect(f.player.shared_contact->polygon_queries==2 && f.player.shared_contact->state_requests==0 && f.player.camera_update,"no-contact slots precede both camera stages and shared contact publication");
    f.capture({true,0,0,0});h.calls=0;const auto neutral=awl::advance_world_map_player_movement(f.inputs,input,&f.player,&f.scene);
    expect(neutral.status==S::Advanced && neutral.intensity==0.0f && neutral.axis==f.player.pose.heading && neutral.position==neutral.movement->proposed_position,
        "neutral stick retains current facing and directional miss outputs use the proposal, before terrain height replacement");
    expect(f.clock->advance(),"stale PAD scenario advances its explicit clock");stopped(S::RequiresPad);
    std::cout<<"PLAYER_MOVEMENT_WORKFLOW atomic scene/contact/camera, ordered trigger stops, fresh PAD and current-facing fallback\n";
}
std::vector<uint8_t> read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
void local(const char* root){
    expect(awl::filesystem_mount("/",root) && awl::verify_mounted_disc_identity().status==awl::DiscIdentityStatus::Verified,"local disc identity verifies before asset checks");
    awl::WorldMapTriggerAsset trigger;expect(trigger.load(),"actual local trigger owner loads");const auto camera=read(std::filesystem::path(root)/"files/jimen-camera.col");Height h;h.bytes=&camera;
    for(const char* name:{"jimen-move.col","jimen1-move.col"}){
        const auto bytes=read(std::filesystem::path(root)/"files"/name),statics=read(std::filesystem::path(root)/"files/mapobj.col");awl::CollisionSurfaceSample ground;
        expect(awl::sample_type1_collision_surface(bytes.data(),bytes.size(),120,168,&ground),"local fixture spawn has a containing terrain surface");Fixture f({120,ground.height,168});
        awl::WorldMapMovementQuery dependency;dependency.collision.terrain_data=bytes.data();dependency.collision.terrain_size=bytes.size();dependency.collision.static_data=statics.data();dependency.collision.static_size=statics.size();
        awl::WorldMapPlayerMovementInput input;input.guards=awl::WorldMapPlayerUpdateGuards{-1,0};input.outputs={true,true,true,true};input.dependencies=&dependency;input.triggers=&trigger;input.secondary_byte_3f1=uint8_t{0};input.sample_height=height;input.sample_context=&h;
        bool moved=false;
        for(int tick=0;tick<16;++tick){const auto prior=f.player.pose.position;awl::PadSample pad{true,0,int8_t(tick<8?80:0)};pad.trigger_l=tick<2?200:0;f.capture(pad);h.calls=0;const auto r=awl::advance_world_map_player_movement(f.inputs,input,&f.player,&f.scene);
            expect(r.status==S::Advanced && h.calls==2 && f.scene.object(1)->position==f.player.pose.position && r.tail->polygon_queries==2,"actual terrain/static/trigger/camera route publishes only after complete preparation");
            if(r.status!=S::Advanced){std::cerr<<"local stop "<<name<<' '<<tick<<' '<<int(r.status)<<'\n';break;}
            moved=moved || f.player.pose.position[0]!=prior[0] || f.player.pose.position[2]!=prior[2];
            if(tick==0)expect(r.movement->proposed_position[0]>120 && r.axis==f.player.pose.heading,"initial right-stick proposal and miss-facing outputs precede the L-trigger camera turn");
            if(tick>=14)expect(f.player.pose.position==prior && r.intensity==0.0f,"released local input decelerates fully and retains a stable position");
            if(tick<2)expect(r.camera->heading_yaw_branch_taken==(tick==0),"camera heading gate derives the fresh L-trigger press, not its held bit");
        }
        expect(f.player.steering.current_speed==0 && moved,"local route moves then decelerates to rest after camera turning");
        std::cout<<"LOCAL_PLAYER_MOVEMENT "<<name<<" 16 captured ticks; supplied scene/empty dynamic lists, CPU only\n";
    }
    awl::filesystem_shutdown();
}
} // namespace
int main(int argc,char** argv){guards();workflow();if(argc==3 && std::string(argv[1])=="--local")local(argv[2]);return failures?1:0;}
