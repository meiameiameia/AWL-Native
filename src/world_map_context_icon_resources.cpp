#include "awl/world_map_context_icon_resources.h"
#include "awl/filesystem.h"
#include <cstring>
#include <fstream>
#include <new>
#include <utility>

namespace awl {
namespace {
using S=WorldMapContextIconResourcesStatus;
constexpr size_t max_resource_size=16*1024*1024;
uint32_t word(const uint8_t* p) noexcept {return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
uint16_t half(const uint8_t* p) noexcept {return uint16_t(uint16_t(p[0])<<8|p[1]);}
int16_t signed_half(const uint8_t* p) noexcept {const auto u=half(p);int16_t s;std::memcpy(&s,&u,2);return s;}
bool table(size_t size,uint32_t at,uint32_t count,uint32_t stride) noexcept {
    return at>=12 && !(at&3) && at<=size && uint64_t(count)*stride<=size-at;
}
struct File {
    std::vector<uint8_t> bytes;
    bool read(const char* logical_path){
        char path[1024];if(!filesystem_resolve_path(logical_path,path,sizeof(path)))return false;
        std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return false;
        const auto size=input.tellg();if(size<=0 || size>static_cast<std::streamoff>(max_resource_size))return false;
        bytes.resize(static_cast<size_t>(size));input.seekg(0);
        return input && input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())) &&
            input.peek()==std::char_traits<char>::eof() && !input.bad();
    }
};
}
bool WorldMapContextIconResources::parse_agc(const uint8_t* data,size_t size) {
    if(size<12 || size>max_resource_size)return false;
    const uint32_t roots=word(data+4),root_table=word(data+8);
    if(!roots || !table(size,root_table,roots,8))return false;
    // Bound total traversal even for aliased hostile tables. This is a native
    // resource budget, not a claim that every possible AGC is supported.
    uint64_t groups=0,frames=0;
    for(uint32_t r=0;r<roots;++r){const auto* root=data+root_table+size_t(r)*8;
        const auto count=word(root),at=word(root+4);groups+=count;
        if(!count || groups>size/20 || !table(size,at,count,20))return false;
        for(uint32_t g=0;g<count;++g){const auto* group=data+at+size_t(g)*20;
            const auto n=word(group),f=word(group+4);frames+=n;
            if(!n || frames>size/12 || !table(size,f,n,12))return false;
            for(uint32_t i=0;i<n;++i){const auto* frame=data+f+size_t(i)*12;
                // Leaf contents are opaque. Only reject an impossible nonempty
                // reference; neither a leaf size nor drawable body is inferred.
                const auto leaf=word(frame+4);
                if(word(frame) && (leaf<12 || (leaf&3) || leaf>size || size-leaf<4))return false;
            }
        }
    }
    agc_.assign(data,data+size);return true;
}
uint32_t WorldMapContextIconResources::root_count() const noexcept {return word(agc_.data()+4);}
bool WorldMapContextIconResources::root(uint32_t index,uint32_t* offset,uint32_t* groups) const noexcept {
    if(!offset || !groups || index>=root_count())return false;
    const uint32_t at=word(agc_.data()+8)+index*8;*offset=at;*groups=word(agc_.data()+at);return true;
}
bool WorldMapContextIconResources::group(uint32_t r,uint32_t index,WorldMapContextIconGroup* out) const noexcept {
    uint32_t root_at,count;if(!out || !root(r,&root_at,&count) || index>=count)return false;
    const uint32_t at=word(agc_.data()+root_at+4)+index*20;const auto* p=agc_.data()+at;
    *out={at,word(p),word(p+4),half(p+8),half(p+10),word(p+12),word(p+16)};return true;
}
bool WorldMapContextIconResources::frame(uint32_t r,uint32_t g,uint32_t index,WorldMapContextIconFrame* out) const noexcept {
    WorldMapContextIconGroup group_info;if(!out || !group(r,g,&group_info) || index>=group_info.frame_count)return false;
    const uint32_t at=group_info.frame_table+index*12;const auto* p=agc_.data()+at;
    *out={at,word(p),word(p+4),{signed_half(p+8),signed_half(p+10)}};return true;
}
S decode_world_map_context_icon_resources(const uint8_t* agc,size_t agc_size,const uint8_t* tpl,size_t tpl_size,
    std::shared_ptr<const WorldMapContextIconResources>* out) {
    if(!out || !agc || !tpl || !agc_size || !tpl_size || agc_size>max_resource_size || tpl_size>max_resource_size)return S::InvalidInput;
    try{auto next=std::shared_ptr<WorldMapContextIconResources>(new WorldMapContextIconResources);
        if(!next->parse_agc(agc,agc_size))return S::UnsupportedAgc;
        if(!tpl_load_from_memory(tpl,tpl_size,&next->tpl_))return S::UnsupportedTpl;
        *out=std::move(next);return S::Decoded;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
S load_world_map_context_icon_resources(std::shared_ptr<const WorldMapContextIconResources>* out) {
    if(!out)return S::InvalidInput;
    try{File agc,tpl;if(!agc.read("/files/opguide.agc") || !tpl.read("/files/opguide.tpl"))return S::ReadFailure;
        const auto status=decode_world_map_context_icon_resources(agc.bytes.data(),agc.bytes.size(),tpl.bytes.data(),tpl.bytes.size(),out);
        return status==S::Decoded?S::Loaded:status;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}
} // namespace awl
