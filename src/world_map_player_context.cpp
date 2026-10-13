#include "awl/world_map_player_context.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include "world_map_dol_view.h"
#include <cfenv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <new>
#include <utility>

namespace awl {
namespace {
using V=WorldMapPlayerContextVector;
using S=WorldMapPlayerContextStatus;
using T=WorldMapPlayerContextTablesStatus;
int32_t signed_word(uint32_t word) noexcept {int32_t result;std::memcpy(&result,&word,4);return result;}
float float_word(uint32_t word) noexcept {float result;std::memcpy(&result,&word,4);return result;}
bool ordinary(float v) noexcept {return std::isfinite(v) && std::fpclassify(v)!=FP_SUBNORMAL;}
bool ordinary(const V& v) noexcept {for(float x:v)if(!ordinary(x))return false;return true;}
bool zero(const V& v) noexcept {return v[0]==0 && v[1]==0 && v[2]==0;}
bool difference(const V& a,const V& b,V& out) noexcept {
    if(!ordinary(a) || !ordinary(b))return false;
    for(size_t i=0;i<3;++i)out[i]=a[i]-b[i];return ordinary(out);
}
bool dot(const V& a,const V& b,float& out) noexcept {
    const float y=a[1]*b[1],z=a[2]*b[2],xy=std::fma(a[0],b[0],y);out=xy+z;
    return ordinary(y) && ordinary(z) && ordinary(xy) && ordinary(out);
}
bool normalized(V& v) noexcept {
    if(!ordinary(v))return false;
    const float x=v[0]*v[0],y=v[1]*v[1],xz=std::fma(v[2],v[2],x),squared=xz+y;
    if(!ordinary(x) || !ordinary(y) || !ordinary(xz) || !ordinary(squared) || squared==0)return false;
    const double seed=1.0/std::sqrt(static_cast<double>(squared));
    const float square=static_cast<float>(seed*seed),half=static_cast<float>(seed*0.5);
    const float correction=-std::fma(square,squared,-3.0f),inverse=correction*half;
    if(!ordinary(square) || !ordinary(half) || !ordinary(correction) || !ordinary(inverse))return false;
    for(float& xvalue:v)xvalue*=inverse;return ordinary(v);
}
bool direction(const V& origin,const V& position,const V& facing,const WorldMapPlayerContextTables& tables,uint32_t& out) noexcept {
    if(zero(facing)){out=4;return true;}
    V delta;if(!difference(position,origin,delta))return false;delta[1]=0;
    if(zero(delta)){out=0;return true;}
    V horizontal=facing;horizontal[1]=0;
    if(zero(horizontal)){out=0;return true;}
    if(!normalized(delta) || !normalized(horizontal))return false;
    float facing_dot;if(!dot(delta,horizontal,facing_dot))return false;
    if(facing_dot<=tables.back_threshold()){out=0;return true;}
    if(facing_dot>=tables.front_threshold()){out=1;return true;}
    const float dz=position[2]-origin[2],dx=position[0]-origin[0];
    const float first=dz*facing[0],second=facing[2]*dx,side=first-second;
    if(!ordinary(dz) || !ordinary(dx) || !ordinary(first) || !ordinary(second) || !ordinary(side))return false;
    out=side>0?3u:side<0?2u:0u;return true;
}
}
bool WorldMapPlayerContextTables::decode(const uint8_t* bytes,size_t size) noexcept {
    detail::DolView view;if(!view.initialize(bytes,size))return false;
    const auto* slots=view.read(0x8023df88,32);const auto* rows=view.read(0x8023e138,16*36);
    const auto* thresholds=view.read(0x80349e98,8);const auto* defaults=view.read(0x8025113c,12);
    if(!slots || !rows || !thresholds || !defaults)return false;
    for(size_t i=0;i<3;++i)if(detail::dol_word(defaults+i*4)!=0)return false;
    for(size_t i=0;i<8;++i){scan_slots_[i]=detail::dol_word(slots+i*4);if(scan_slots_[i]>=8)return false;}
    for(size_t i=0;i<16;++i){rows_[i].key=signed_word(detail::dol_word(rows+i*36));
        for(size_t j=0;j<8;++j)rows_[i].neighbors[j]=signed_word(detail::dol_word(rows+i*36+4+j*4));}
    back_threshold_=float_word(detail::dol_word(thresholds));front_threshold_=float_word(detail::dol_word(thresholds+4));
    return ordinary(back_threshold_) && ordinary(front_threshold_) && back_threshold_>=-1 && front_threshold_<=1 && back_threshold_<=front_threshold_;
}
bool WorldMapPlayerContextTables::neighbor(int32_t key,uint32_t slot,int32_t* out) const noexcept {
    if(!out || slot>=8)return false;
    for(const auto& row:rows_)if(row.key==key){*out=row.neighbors[slot];return true;}
    *out=-1;return true;
}
T decode_world_map_player_context_tables(const uint8_t* bytes,size_t size,std::shared_ptr<const WorldMapPlayerContextTables>* out) {
    if(!out)return T::InvalidInput;
    try {auto next=std::shared_ptr<WorldMapPlayerContextTables>(new WorldMapPlayerContextTables);
        if(!next->decode(bytes,size))return T::UnsupportedLayout;*out=std::move(next);return T::Decoded;
    }catch(const std::bad_alloc&){return T::AllocationFailure;}
}
T load_world_map_player_context_tables(std::shared_ptr<const WorldMapPlayerContextTables>* out) {
    if(!out)return T::InvalidInput;
    if(*out && (*out)->target_verified())return T::Loaded;
    try {
        char path[1024];if(!filesystem_resolve_path("/sys/main.dol",path,sizeof(path)))return T::ReadFailure;
        std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return T::ReadFailure;
        const auto size=input.tellg();if(size<0 || size>static_cast<std::streamoff>(detail::max_dol_size))return T::ReadFailure;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
        if(!input || (size!=0 && !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) ||
            input.peek()!=std::char_traits<char>::eof() || input.bad())return T::ReadFailure;
        Sha1 sha;if(!sha1_bytes(bytes.data(),bytes.size(),&sha))return T::ReadFailure;if(sha!=kTargetDolSha1)return T::WrongDol;
        auto next=std::shared_ptr<WorldMapPlayerContextTables>(new WorldMapPlayerContextTables);
        if(!next->decode(bytes.data(),bytes.size()))return T::UnsupportedLayout;
        next->target_verified_=true;*out=std::move(next);return T::Loaded;
    }catch(const std::bad_alloc&){return T::AllocationFailure;}
}
WorldMapPlayerContextStep query_world_map_player_context(const WorldMapPlayerContextTables* tables,
    const std::vector<WorldMapPlayerContextList>& lists,const WorldMapPlayerContextQuery& input,WorldMapPlayerContextResult* out) noexcept {
    WorldMapPlayerContextStep step;
    if(!out)return step;
    WorldMapPlayerContextResult next;
    if(std::fegetround()!=FE_TONEAREST || !ordinary(input.forward) || !ordinary(input.angle_radians) || !ordinary(input.min_distance) || !ordinary(input.max_distance)) {
        step.status=S::UnsupportedNumerics;return step;
    }
    const float minimum=input.min_distance*input.min_distance;next.nearest_squared_distance=input.max_distance*input.max_distance;
    if(!ordinary(minimum) || !ordinary(next.nearest_squared_distance)){step.status=S::UnsupportedNumerics;return step;}
    if(zero(input.forward)){*out=next;step.status=S::Prepared;return step;}
    if(!tables){step.status=S::RequiresTables;return step;}
    const float cone=static_cast<float>(std::cos(static_cast<double>(input.angle_radians)));
    V forward=input.forward;
    if(!ordinary(minimum) || !ordinary(next.nearest_squared_distance) || !ordinary(cone) || !normalized(forward)){step.status=S::UnsupportedNumerics;return step;}
    const auto scan=[&](int32_t key) {
        if(key==0 || key==-1)return S::Prepared;
        step.required_list=key;if(key<0)return S::InvalidInput;
        const WorldMapPlayerContextList* list=nullptr;
        for(const auto& candidate:lists)if(candidate.key==key){if(list)return S::InvalidInput;list=&candidate;}
        if(!list)return S::RequiresList;
        ++step.scanned_lists;const WorldMapPlayerContextCandidate* selected=nullptr;size_t selected_index=0;
        for(size_t i=0;i<list->candidates.size();++i){const auto& candidate=list->candidates[i];step.required_candidate=i;++step.visited_candidates;
            if(input.excluded_kinds){if(!candidate.owner_kind_8)return S::RequiresOwnerKind;
                const auto shift=*candidate.owner_kind_8&63u;const uint32_t mask=shift<32?1u<<shift:0u;if(input.excluded_kinds&mask)continue;}
            if(!input.origin)return S::RequiresOrigin;if(!candidate.position_1c)return S::RequiresPosition;
            V delta;if(!difference(*candidate.position_1c,*input.origin,delta))return S::UnsupportedNumerics;
            float squared;if(!dot(delta,delta,squared))return S::UnsupportedNumerics;
            if(squared>=next.nearest_squared_distance || squared<=minimum || squared==0)continue;
            if(!candidate.max_squared_distance_3c)return S::RequiresRange;
            if(!ordinary(*candidate.max_squared_distance_3c))return S::UnsupportedNumerics;
            if(squared>=*candidate.max_squared_distance_3c)continue;
            if(!normalized(delta))return S::UnsupportedNumerics;
            float angle;if(!dot(forward,delta,angle))return S::UnsupportedNumerics;if(cone>angle)continue;
            if(!candidate.facing_threshold_38)return S::RequiresFacingThreshold;
            if(!ordinary(*candidate.facing_threshold_38))return S::UnsupportedNumerics;
            if(!candidate.facing_28)return S::RequiresFacing;if(!ordinary(*candidate.facing_28))return S::UnsupportedNumerics;
            V reverse;for(size_t axis=0;axis<3;++axis)reverse[axis]=-delta[axis];
            if(!dot(*candidate.facing_28,reverse,angle))return S::UnsupportedNumerics;if(*candidate.facing_threshold_38>angle)continue;
            selected=&candidate;selected_index=i;next.nearest_squared_distance=squared;
        }
        if(selected){step.required_candidate=selected_index;
            if(!selected->owner_code_4)return S::RequiresOwnerCode;if(!selected->owner_kind_8)return S::RequiresOwnerKind;
            if(!selected->payload)return S::RequiresPayload;
            auto& record=next.record;record.owner_code=*selected->owner_code_4;record.owner_kind=*selected->owner_kind_8;record.payload=*selected->payload;
            record.position=*selected->position_1c;record.facing=*selected->facing_28;
            if(!direction(*input.origin,record.position,record.facing,*tables,record.direction))return S::UnsupportedNumerics;
        }
        step.required_candidate.reset();return S::Prepared;
    };
    step.status=scan(input.current_key);if(step.status!=S::Prepared)return step;
    for(const auto slot:tables->scan_slots()) {int32_t key;if(!tables->neighbor(input.current_key,slot,&key)){step.status=S::InvalidInput;return step;}
        step.status=scan(key);if(step.status!=S::Prepared)return step;}
    next.found=next.nearest_squared_distance<input.max_distance*input.max_distance;
    step.required_list.reset();step.required_candidate.reset();*out=next;return step;
}
} // namespace awl
