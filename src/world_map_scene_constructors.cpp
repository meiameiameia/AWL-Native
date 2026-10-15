#include "awl/world_map_scene_constructors.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include "world_map_dol_view.h"
#include <cfenv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <new>

namespace awl {
namespace {
using S=WorldMapSceneConstructorStatus;
float as_float(uint32_t v) noexcept {float f;std::memcpy(&f,&v,4);return f;}
int32_t as_signed(uint32_t v) noexcept {int32_t s;std::memcpy(&s,&v,4);return s;}
bool finite(const std::array<float,3>& v) noexcept {for(float f:v)if(!std::isfinite(f))return false;return true;}
}
WorldMapSceneEntryRequest world_map_scene_entry_request(const WorldMapSceneModeRequestState& scene) noexcept {
    const auto mode=scene.prior_mode_60;
    if((mode==4 || mode==6) && scene.scene_byte_78)return {true,1,scene.prior_scene_type_6c,scene.scene_type_68};
    if(mode==28 || mode==30)return {true,6,0,0};
    if(mode==29)return {true,6,1,0};
    return {};
}
bool world_map_optional_actor_b_awake(uint32_t raw,uint32_t wake,uint32_t sleep) noexcept {
    const uint32_t hour=raw/36000u%24u,minute=raw/600u%60u;
    const uint32_t first=wake/60u+6u,last=sleep/60u+21u;
    return !(hour<first || (hour==first && minute<wake%60u) ||
        hour>last || (hour==last && minute>sleep%60u));
}
bool WorldMapSceneConstructorTables::decode_fields(const uint8_t* data,size_t size) noexcept {
    detail::DolView dol;if(!dol.initialize(data,size))return false;
    const auto number=[&](uint32_t address,float& out){const auto* p=dol.read(address,4);if(!p)return false;out=as_float(detail::dol_word(p));return std::isfinite(out);};
    const auto vector=[&](uint32_t address,std::array<float,3>& out){for(uint32_t i=0;i<3;++i)if(!number(address+i*4,out[i]))return false;return true;};
    const auto root=[&](uint32_t address,bool wide,float& out){double v=0;
        if(wide){const auto* p=dol.read(address,8);if(!p)return false;const uint64_t bits=(uint64_t(detail::dol_word(p))<<32)|detail::dol_word(p+4);std::memcpy(&v,&bits,8);}
        else {float f;if(!number(address,f))return false;v=f;}
        if(!std::isfinite(v) || v<0)return false;out=static_cast<float>(std::sqrt(v));return std::isfinite(out);};
    if(std::fegetround()!=FE_TONEAREST || !vector(0x8025113c,zero_) || !vector(0x80251130,forward_) ||
        forward_!=std::array<float,3>{0,0,1})return false; // supported Y-rotated forward source
    constexpr uint32_t positions[]{0x8024f910,0x8024f920,0x8024f930,0x8023e3d0};
    constexpr uint32_t headings[]{0x80251130,0x80251160,0x80251130,0x80251154};
    for(size_t i=0;i<4;++i)if(!vector(positions[i],positions_[i]) || !vector(headings[i],headings_[i]))return false;
    // Static source descriptors copied by 897E4/A3D8/B098/BFF0. Each is
    // a direct member call: adjustment 0, virtual index -1, exact callback.
    constexpr uint32_t descriptors[]{0x802abf88,0x802ac4c8,0x802aca80,0x802acfc0};
    constexpr uint32_t callbacks[]{0x80089460,0x80089ff4,0x8008acbc,0x8008b874};
    for(size_t i=0;i<4;++i){const auto* p=dol.read(descriptors[i],12);
        if(!p || detail::dol_word(p)!=0 || detail::dol_word(p+4)!=UINT32_MAX || detail::dol_word(p+8)!=callbacks[i])return false;}
    float base_range=0,range50=0,threshold50=0;
    if(!root(0x8034b7c0,true,optional_thresholds_[0]) || !root(0x8034b880,true,optional_thresholds_[1]) ||
        !root(0x80349e88,false,thresholds_[0]) || !number(0x80349e8c,base_range) ||
        !root(0x8034a700,true,thresholds_[1]) || !root(0x8034a718,true,thresholds_[2]) ||
        !root(0x8034a728,true,thresholds_[3]) || !number(0x8034a710,ranges_[2]) ||
        !number(0x80349f24,range50) || !number(0x80349f28,threshold50) || threshold50<0)return false;
    thresholds_[4]=static_cast<float>(std::sqrt(static_cast<double>(threshold50)));
    ranges_[0]=ranges_[1]=ranges_[3]=base_range*base_range;ranges_[4]=range50*range50;
    for(float r:ranges_)if(!std::isfinite(r) || r<0)return false;
    auto& target=camera_.target;
    if(!vector(0x802ab940,target.camera.position) || !vector(0x802ab94c,target.origin_offset_0c) ||
        !number(0x802ab958,target.camera.field_18) || !number(0x802ab95c,target.camera.yaw) ||
        !vector(0x802ab964,camera_.up_vector_24) || !number(0x802ab970,target.distance_30))return false;
    target.camera.flag_98=true;target.camera.flag_99=false;
    const auto* lo=dol.read(0x80348878,4);const auto* hi=dol.read(0x8034887c,4);
    if(!lo || !hi || !number(0x8034b774,radians_per_degree_))return false;
    rng_min_=as_signed(detail::dol_word(lo));rng_max_=as_signed(detail::dol_word(hi));
    const auto action_descriptors=[&](uint32_t base,auto& values){for(size_t i=0;i<values.size();++i){const auto* p=dol.read(base+uint32_t(i)*60,12);
        if(!p || detail::dol_word(p)!=0 || detail::dol_word(p+4)!=UINT32_MAX)return false;
        values[i]=detail::dol_word(p+8);if(values[i]<0x80000000u || values[i]>=0x80300000u || (values[i]&3))return false;}return true;};
    if(!action_descriptors(0x802c64e8,actions_.a_initializers) || !action_descriptors(0x802c76e8,actions_.b_initializers) ||
        actions_.a_initializers[0x12]!=0x80162918 || actions_.a_initializers[0x13]!=0x80162ad0 ||
        actions_.b_initializers[2]!=0x80168bb0 || actions_.b_initializers[0x11]!=0x80169e7c || actions_.b_initializers[0x25]!=0x8016b834)return false;
    // 16816C reaches these three rows in the supported constructor paths.
    // The arithmetic base is not evidence for 42 contiguous duration rows.
    for(uint32_t i:{2u,0x11u,0x25u}){const auto* p=dol.read(0x802c7538+i*8,8);if(!p)return false;
        auto& row=actions_.b_durations[i];row={detail::dol_word(p),detail::dol_word(p+4)};
        if(row[0]>row[1] || row[1]-row[0]==UINT32_MAX)return false;actions_.b_duration_known[i]=true;}
    for(uint32_t i=0;i<5;++i)if(!number(0x802c41a8+(0x27+i)*4,actions_.speeds[i]) ||
        !number(0x8024a7cc+i*16,actions_.model_scales[i]) || actions_.model_scales[i]<=0)return false;
    if(!number(0x8034b7e4,actions_.rate_divisors[0]) || !number(0x8034b88c,actions_.rate_divisors[1]) ||
        actions_.rate_divisors[0]<=0 || actions_.rate_divisors[1]<=0 || !vector(0x80256514,actions_.night_position) ||
        !root(0x8034b8f0,true,actions_.night_threshold))return false;
    actions_.forward=forward_;
    for(size_t i=0;i<2;++i){const auto* p=dol.read(i?0x803469d8:0x803469d0,4);if(!p)return false;
        refresh_upper_[i]=detail::dol_word(p);if(refresh_upper_[i]>4)return false;}
    return rng_min_>=0 && rng_max_>=rng_min_ && rng_max_<=359 && radians_per_degree_>0 && target.distance_30>0;
}
S WorldMapSceneConstructorTables::refresh_actor_selection(WorldMapSceneActorRefresh path,WorldMapSceneConstructorState* state) noexcept {
    if(!state || (path!=WorldMapSceneActorRefresh::From1307C && path!=WorldMapSceneActorRefresh::From13AE0))return S::InvalidInput;
    if(!state->rng)return S::RequiresHeadingRng;
    if(!state->rng_refresh_upper_bounds)return S::RequiresRngParameters;
    const auto upper=(*state->rng_refresh_upper_bounds)[path==WorldMapSceneActorRefresh::From1307C?0:1];
    if(upper>4)return S::UnsupportedLayout;
    uint32_t seed=state->rng->seed();const auto word=advance_game_rng(&seed);
    state->actor_3f8=0x2c+int32_t(word%(upper+1));state->rng->seed_=seed;return S::Prepared;
}
S WorldMapSceneConstructorTables::decode(const uint8_t* bytes,size_t size,std::shared_ptr<const WorldMapSceneConstructorTables>* out){
    if(!out)return S::InvalidInput;
    try {auto next=std::shared_ptr<WorldMapSceneConstructorTables>(new WorldMapSceneConstructorTables);
        if(!next->decode_fields(bytes,size))return S::UnsupportedLayout;*out=std::move(next);return S::Decoded;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
S WorldMapSceneConstructorTables::load(std::shared_ptr<const WorldMapSceneConstructorTables>* out){
    if(!out)return S::InvalidInput;if(*out && (*out)->verified_)return S::Loaded;
    try {
        char path[1024];if(!filesystem_resolve_path("/sys/main.dol",path,sizeof(path)))return S::ReadFailure;
        std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return S::ReadFailure;
        const auto size=input.tellg();if(size<0 || size>static_cast<std::streamoff>(detail::max_dol_size))return S::ReadFailure;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
        if(!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) ||
            input.peek()!=std::char_traits<char>::eof() || input.bad())return S::ReadFailure;
        Sha1 sha;if(!sha1_bytes(bytes.data(),bytes.size(),&sha))return S::ReadFailure;if(sha!=kTargetDolSha1)return S::WrongDol;
        auto next=std::shared_ptr<WorldMapSceneConstructorTables>(new WorldMapSceneConstructorTables);
        if(!next->decode_fields(bytes.data(),bytes.size()))return S::UnsupportedLayout;
        next->verified_=true;*out=std::move(next);return S::Loaded;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
S WorldMapSceneConstructorTables::prepare(const WorldMapCollisionAssets& collision,const WorldMapSceneConstructorState& state,
    const std::array<uint64_t,5>& keys,WorldMapSceneConstructorProjection* out) const noexcept {
    if(!out)return S::InvalidInput;
    if((state.actor_a_29a64>=0x29 && state.actor_a_29a64<=0x2b) || state.actor_b_29a60==0x27 || state.actor_b_29a60==0x28)return S::RequiresOptionalConstructor;
    WorldMapSceneConstructorProjection next;
    auto status=prepare_spatial(collision,state,keys,&next);if(status!=S::Prepared)return status;
    status=prepare_mandatory(state,&next);if(status!=S::Prepared)return status;
    *out=next;return S::Prepared;
}
S WorldMapSceneConstructorTables::prepare_spatial(const WorldMapCollisionAssets& collision,const WorldMapSceneConstructorState& state,
    const std::array<uint64_t,5>& keys,WorldMapSceneConstructorProjection* out) const noexcept {
    if(!out)return S::InvalidInput;
    try {
    if(std::fegetround()!=FE_TONEAREST)return S::UnsupportedNumerics;
    WorldMapSceneConstructorProjection next;next.camera=camera_;next.secondary_3f4=state.byte_299aa;
    constexpr uint32_t codes[]{0x7b3,0x4d,0x4e,0x4f,0x50};
    const std::array<std::array<uint32_t,2>,5> payloads{{{0x33,UINT32_MAX},{0x1c,UINT32_MAX},
        {0x2f,state.byte_299aa?0x41u:0x40u},{0x30,state.byte_299b0?0x43u:0x42u},{0x32,0x31}}};
    const auto& terrain=collision.terrain_bytes();
    for(size_t i=0;i<5;++i){if(!keys[i])return S::InvalidInput;for(size_t j=0;j<i;++j)if(keys[j]==keys[i])return S::InvalidInput;
        auto& object=next.spatial[i];object.identity=keys[i];object.scene_type=i?1:0;object.position=zero_;
        if(i){CollisionResolverHeightAdjustment height;
            if(!resample_type1_collision_resolver_height(terrain.data(),terrain.size(),positions_[i-1],&height))return S::TerrainIncomplete;
            object.position=height.position;}
        object.context.owner_code_4=codes[i];object.context.owner_kind_8=i?6u:1u;
        object.context.facing_28=i?headings_[i-1]:zero_;object.context.facing_threshold_38=thresholds_[i];object.context.max_squared_distance_3c=ranges_[i];object.context.payload=payloads[i];
    }
    *out=next;return S::Prepared;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
S WorldMapSceneConstructorTables::prepare_mandatory(const WorldMapSceneConstructorState& state,WorldMapSceneConstructorProjection* out) const noexcept {
    if(!out || state.actor_3f8<0x2c || state.actor_3f8>0x30)return S::InvalidInput;
    if(std::fegetround()!=FE_TONEAREST)return S::UnsupportedNumerics;
    auto next=*out;next.rng_after.reset();next.mandatory_context={};auto& actor=next.mandatory_context;
    constexpr uint32_t constructors[]{0x3d,0x41,0x3e,0x3f,0x40};
    actor.owner_code_4=constructors[state.actor_3f8-0x2c];actor.owner_kind_8=1;
    actor.facing_threshold_38=thresholds_[0];actor.max_squared_distance_3c=ranges_[0];
    // The mandatory actor's virtual payload/model/action setup is still a
    // reached dependency when selected by context. Do not invent its payload.
    next.mandatory_heading=forward_;
    if(state.actor_3f8!=0x30){if(!state.rng)return S::RequiresHeadingRng;
        if(!state.rng->loop_started())return S::RequiresRngLoop;
        uint32_t seed=state.rng->seed();const auto word=advance_game_rng(&seed);
        const uint32_t degrees=uint32_t(rng_min_)+word%uint32_t(rng_max_-rng_min_+1);
        const float angle=radians_per_degree_*static_cast<float>(degrees);
        next.mandatory_heading={static_cast<float>(std::sin(static_cast<double>(angle))),0,static_cast<float>(std::cos(static_cast<double>(angle)))};
        next.rng_after=seed;
    }
    if(!finite(next.mandatory_heading))return S::UnsupportedNumerics;
    actor.facing_28=next.mandatory_heading;*out=next;return S::Prepared;
}
S WorldMapSceneConstructorTables::prepare_optional(bool b,const WorldMapCollisionAssets& collision,
    const WorldMapSceneConstructorState& state,uint32_t raw,uint64_t key,WorldMapOptionalActorProjection* out) const noexcept {
    if(!out || !key)return S::InvalidInput;
    const int32_t id=b?state.actor_b_29a60:state.actor_a_29a64;
    if(b?!(id==0x27 || id==0x28):!(id>=0x29 && id<=0x2b))return S::Absent;
    const auto& saved=b?state.optional_b:state.optional_a;
    if(!saved || !state.held_constructor_11cc8)return S::RequiresOptionalFields;
    if(!finite(saved->position) || !finite(saved->heading))return S::InvalidInput;
    if(saved->scene_type!=1)return S::UnsupportedSceneType;
    if(std::fegetround()!=FE_TONEAREST)return S::UnsupportedNumerics;
    try {
        WorldMapOptionalActorProjection next;auto& object=next.scene;
        object.identity=key;object.scene_type=saved->scene_type;object.position=saved->position;
        if(!b){CollisionHeightSample height;const auto& terrain=collision.terrain_bytes();
            if(!sample_type1_collision_height(terrain.data(),terrain.size(),object.position[0],object.position[2],&height))return S::TerrainIncomplete;
            object.position[1]=height.height;
        }
        const int32_t code=b?(id==0x27?0x38:0x39):0x3a+(id-0x29);
        const bool held=*state.held_constructor_11cc8==(b?0x38:code);
        WorldMapOptionalActionQuery action;action.actor_id=id;action.phase_raw=raw;action.held_scene_type=state.held_scene_type_2a290;
        action.kind=b?WorldMapOptionalActionKind::BExplicit:WorldMapOptionalActionKind::AExplicit;
        action.before={b?0x2b:0x23,saved->previous_action,saved->deadline,saved->restart_latch};
        action.requested=saved->action;action.minimum=action.maximum=b?2u:10u;
        auto heading=saved->heading;
        if(held){if(!state.held_scene_type_2a290)return S::RequiresOptionalFields;
            action.requested=b?0x11:0x12;if(b)action.kind=WorldMapOptionalActionKind::BTable;
            else {if(!state.held_position_29c78 || !state.held_heading_29f84)return S::RequiresOptionalFields;
                if(*state.held_scene_type_2a290!=1)return S::UnsupportedSceneType;
                object.position=*state.held_position_29c78;heading=*state.held_heading_29f84;
                if(!finite(object.position) || !finite(heading))return S::InvalidInput;}
        }else if(b && !world_map_optional_actor_b_awake(raw,saved->wake_minutes,saved->sleep_minutes)){
            action.requested=0x25;action.kind=WorldMapOptionalActionKind::BTable;
        }else if(b && saved->special_14e6c==1)return S::RequiresOptionalContinuation; // 16C908 remains reached after action two
        const bool reads_rng=action.requested!=(b?0x2b:0x23);
        if(reads_rng){if(!state.rng)return S::RequiresHeadingRng;if(!state.rng->loop_started())return S::RequiresRngLoop;action.rng_before=state.rng->seed();}
        const auto prepared=prepare_world_map_optional_actor_action(actions_,action,&next.action);
        if(prepared!=WorldMapOptionalActionStatus::Prepared)return prepared==WorldMapOptionalActionStatus::RequiresHeldType?S::RequiresOptionalFields:S::RequiresOptionalAction;
        next.action.actor_identity=key;
        next.action.bind_secondary_model=next.action.bind_secondary_model || held;
        if(held){auto& commands=next.action.model_commands;
            if(next.action.model_command_count>=commands.size())return S::RequiresOptionalAction;
            if(!b){for(size_t i=next.action.model_command_count;i>0;--i)commands[i]=commands[i-1];commands[0]={WorldMapOptionalModelOperation::BindSecondary,0,0,0};}
            else commands[next.action.model_command_count]={WorldMapOptionalModelOperation::BindSecondary,0,0,0};
            ++next.action.model_command_count;}
        if(next.action.position)object.position=*next.action.position;
        if(next.action.heading)heading=*next.action.heading;
        auto& metadata=object.context;metadata.owner_code_4=uint32_t(code);metadata.owner_kind_8=1;
        metadata.facing_28=heading;metadata.facing_threshold_38=optional_thresholds_[b?1:0];metadata.max_squared_distance_3c=ranges_[0];
        // Selected virtual payload and model/AI remain untranslated.
        auto& c=next.collision;c.world_position=object.position;c.heading_axis=heading;
        c.collision.identity=key;c.collision.category=saved->scene_type;c.collision.enabled=next.action.collision_enabled;
        c.collision.radius=world_map_first_actor_circle_spec(id).radius;c.collision.collision_flags=1;
        c.collision.center_world=object.position;
        *out=next;return S::Prepared;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
} // namespace awl
