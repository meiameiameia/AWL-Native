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
uint32_t advance_world_map_scene_rng(uint32_t* state) noexcept {
    if(!state)return 0;
    const uint32_t first=*state*0x41c64e6du+0x3039u;
    *state=first*0x41c64e6du+0x3039u;
    return (*state&0xffff0000u)|(first>>16);
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
    if(!root(0x80349e88,false,thresholds_[0]) || !number(0x80349e8c,base_range) ||
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
    return rng_min_>=0 && rng_max_>=rng_min_ && rng_max_<=359 && radians_per_degree_>0 && target.distance_30>0;
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
    if(state.actor_3f8!=0x30){if(!state.heading_rng_seed)return S::RequiresHeadingRng;
        uint32_t seed=*state.heading_rng_seed;const auto word=advance_world_map_scene_rng(&seed);
        const uint32_t degrees=uint32_t(rng_min_)+word%uint32_t(rng_max_-rng_min_+1);
        const float angle=radians_per_degree_*static_cast<float>(degrees);
        next.mandatory_heading={static_cast<float>(std::sin(static_cast<double>(angle))),0,static_cast<float>(std::cos(static_cast<double>(angle)))};
        next.rng_after=seed;
    }
    if(!finite(next.mandatory_heading))return S::UnsupportedNumerics;
    actor.facing_28=next.mandatory_heading;*out=next;return S::Prepared;
}
} // namespace awl
