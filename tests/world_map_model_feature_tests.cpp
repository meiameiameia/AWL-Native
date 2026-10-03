#include "awl/world_map_model_feature.h"

#include <cstring>
#include <iostream>
#include <limits>

namespace {
using Status=awl::WorldMapModelFeatureStatus;
using State=awl::WorldMapModelFeatureObjectState;
using Step=awl::WorldMapHeldItemFeatureStep;
using Bank=awl::WorldMapHeldItemFeatureBank;
using Observations=awl::WorldMapHeldItemFeatureObservations;
int failures=0;
void expect(bool yes,const char* message){if(!yes){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
uint32_t bits(float f){uint32_t w;std::memcpy(&w,&f,4);return w;}
bool same_texture(const std::optional<awl::WorldMapModelFeatureTexture>& a,const std::optional<awl::WorldMapModelFeatureTexture>& b){
    return a.has_value()==b.has_value() && (!a || (a->bank_identity==b->bank_identity && a->index==b->index));
}
bool same(const State& a,const State& b){
    if(a.resource_0!=b.resource_0 || a.buffer_34!=b.buffer_34 || a.flags_38!=b.flags_38 || a.enabled_3c!=b.enabled_3c ||
        a.byte_3d!=b.byte_3d || a.metadata_44!=b.metadata_44 || a.cache_48.size()!=b.cache_48.size())return false;
    for(size_t i=0;i<a.matrix_4.size();++i)if(bits(a.matrix_4[i])!=bits(b.matrix_4[i]))return false;
    for(size_t i=0;i<a.cache_48.size();++i)if(a.cache_48[i].channel!=b.cache_48[i].channel || !same_texture(a.cache_48[i].texture,b.cache_48[i].texture))return false;
    return true;
}
Observations observations(){
    Observations o;o.row=awl::WorldMapHeldItemFeatureRow{11,2,3,9,10,20,30,40};o.null_item_alternate_group=uint8_t{26};
    o.resource=awl::WorldMapModelFeatureResourceBinding{Bank::Alternate,9,100,200,
        std::vector<awl::WorldMapModelFeatureCommand>{{1,1},{7,0},{1,0},{1,0},{1,2}}};
    o.texture_bank_identity=0x100000003ull;return o;
}
void test_routes_and_cache(){
    State state;state.resource_0=91;state.buffer_34=92;state.flags_38=0x80000007u;state.enabled_3c=0;state.byte_3d=99;
    state.metadata_44=93;state.matrix_4.fill(std::numeric_limits<float>::quiet_NaN());
    state.cache_48={{UINT32_MAX,std::nullopt},{0,awl::WorldMapModelFeatureTexture{77,8}},{1,std::nullopt}};
    auto o=observations();Step step;
    expect(awl::prepare_world_map_held_item_feature(state,11,o,&step)==Status::Prepared && step.route && step.route->bank==Bank::Alternate &&
        step.route->group==9 && step.route->textures==std::array<uint16_t,2>{30,40} && step.after.resource_0==100 && step.after.metadata_44==200 &&
        step.after.buffer_34==0 && step.after.enabled_3c==1 && step.after.byte_3d==0 && step.after.flags_38==state.flags_38 &&
        step.after.matrix_4==awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0},
        "split item route resets the observed feature fields without reading stale matrix/buffer contents or changing flags");
    expect(step.after.cache_48[0].channel==UINT32_MAX && !step.after.cache_48[0].texture &&
        step.after.cache_48[1].texture->bank_identity==0x100000003ull && step.after.cache_48[1].texture->index==30 &&
        step.after.cache_48[2].texture->index==40 && step.writes.size()==23 &&
        step.writes[17].cache_index==1u && step.writes[19].cache_index==1u && step.writes[21].cache_index==2u,
        "existing cache IDs precede earlier free slots, repeated commands repeat stores, and channel zero scans before one");
    auto full=state;full.cache_48={{7,std::nullopt},{8,awl::WorldMapModelFeatureTexture{77,8}}};
    expect(awl::prepare_world_map_held_item_feature(full,11,o,&step)==Status::Prepared && step.writes.size()==17 &&
        step.after.cache_48[0].channel==7 && !step.after.cache_48[0].texture && step.after.cache_48[1].texture->index==8,
        "full unmatched cache is a supported no-op rather than invented eviction or a failure");
    o.null_item_alternate_group=uint8_t{9};o.resource->bank=Bank::Primary;o.resource->group=3;
    expect(awl::prepare_world_map_held_item_feature(state,11,o,&step)==Status::Prepared && step.route->bank==Bank::Primary &&
        step.route->group==3 && step.route->textures==std::array<uint16_t,2>{10,20},"split row matching row-zero alternate group uses primary fields");
    o.row->type_0=3;o.null_item_alternate_group.reset();
    expect(awl::prepare_world_map_held_item_feature(state,11,o,&step)==Status::Prepared && step.route->bank==Bank::Primary,
        "ordinary types do not read the split-route baseline");
    o.row->item_id=0x4ff;
    expect(awl::prepare_world_map_held_item_feature(state,0x4ff,o,&step)==Status::RequiresBaseline && same(step.after,state) && step.writes.empty(),
        "special item 4FF uses the split route even with an ordinary type");
    expect(awl::prepare_world_map_held_item_feature(state,0,{},&step)==Status::Prepared && !step.route && step.writes.size()==17 &&
        step.after.resource_0==0 && step.after.metadata_44==0 && step.after.cache_48[1].texture->index==8 &&
        step.after.cache_48[2].channel==1 && !step.after.cache_48[2].texture && step.after.flags_38==state.flags_38,
        "item zero resets resources but retains cache payloads and flags without reading observations");
}
void test_stops(){
    State state;state.resource_0=123;state.cache_48.resize(2);Step step;auto o=observations();
    auto missing=o;missing.row.reset();expect(awl::prepare_world_map_held_item_feature(state,11,missing,&step)==Status::RequiresRow && same(step.after,state),"missing row stops atomically");
    missing=o;missing.null_item_alternate_group.reset();expect(awl::prepare_world_map_held_item_feature(state,11,missing,&step)==Status::RequiresBaseline,"split baseline is required before resources");
    missing=o;missing.resource.reset();expect(awl::prepare_world_map_held_item_feature(state,11,missing,&step)==Status::RequiresResource && step.route && same(step.after,state),"selected resource is required after routing");
    missing=o;missing.texture_bank_identity.reset();missing.resource->commands.reset();
    expect(awl::prepare_world_map_held_item_feature(state,11,missing,&step)==Status::RequiresTextureBank && step.writes.empty() && same(step.after,state),
        "texture-bank reference occurs before metadata scan and missing evidence rolls back reset prefix");
    missing=o;missing.resource->commands.reset();expect(awl::prepare_world_map_held_item_feature(state,11,missing,&step)==Status::RequiresCommands && same(step.after,state),"unknown command count/headers stop atomically");
    step.after.resource_0=999;
    for(unsigned fault=0;fault<5;++fault){
        auto bad=o;
        if(fault==0)bad.row->item_id=12;
        if(fault==1)bad.resource->group=8;
        if(fault==2)bad.resource->bank=Bank::Primary;
        if(fault==3)bad.resource->metadata_44=0;
        if(fault==4)bad.texture_bank_identity=0;
        expect(awl::prepare_world_map_held_item_feature(state,11,bad,&step)==Status::InvalidInput && step.after.resource_0==999,"invalid reached binding or unsafe null pointer preserves output");
    }
    expect(awl::prepare_world_map_held_item_feature(state,-1,o,&step)==Status::InvalidInput &&
        awl::prepare_world_map_held_item_feature(state,INT32_MAX,o,&step)==Status::InvalidInput &&
        awl::prepare_world_map_held_item_feature(state,0,{},nullptr)==Status::InvalidInput &&
        awl::prepare_world_map_held_item_feature(step.after,0,{},&step)==Status::InvalidInput && step.after.resource_0==999,
        "negative/overflowing table indices and output aliases reject without overwriting diagnostics");
}
void test_native_lifetime(){
    std::unique_ptr<awl::WorldMapNativeModelFeature> feature;
    expect(awl::construct_world_map_native_model_feature(2,&feature)==Status::Constructed && feature && feature->identity()!=0 &&
        !feature->attachment_source() && feature->state().flags_38==2 && feature->state().cache_48.size()==2 &&
        feature->state().cache_48[0].channel==UINT32_MAX && !feature->state().cache_48[0].texture,"native empty feature owns two initialized cache IDs with unknown payloads");
    if(!feature)return;const auto identity=feature->identity();Step step;auto o=observations();
    expect(awl::advance_world_map_native_held_item_feature(feature.get(),11,o,&step)==Status::Advanced && feature->attachment_source() &&
        feature->attachment_source()->key==identity && feature->state().cache_48[0].texture->index==30 &&
        feature->state().cache_48[1].texture->index==40,"complete supplied resource update exposes a stable owned feature key for attachment");
    const auto before=feature->state();auto missing=o;missing.texture_bank_identity.reset();
    expect(awl::advance_world_map_native_held_item_feature(feature.get(),11,missing,&step)==Status::RequiresTextureBank &&
        same(feature->state(),before),"native missing texture bank cannot mutate the feature owner");
    o.resource->resource_0=0;
    expect(awl::advance_world_map_native_held_item_feature(feature.get(),11,o,&step)==Status::Advanced && !feature->attachment_source(),
        "nonnull metadata can bind caches while a null resource keeps the actual source predicate empty");
    expect(awl::advance_world_map_native_held_item_feature(feature.get(),0,{},&step)==Status::Advanced && !feature->attachment_source() &&
        feature->identity()==identity && feature->state().cache_48[1].texture->index==40,"empty reset retains owned identity and prior cache entries");
    auto* original=feature.get();
    expect(awl::construct_world_map_native_model_feature(UINT32_MAX,&feature)==Status::InvalidInput && feature.get()==original &&
        awl::construct_world_map_native_model_feature(2,nullptr)==Status::InvalidInput &&
        awl::advance_world_map_native_held_item_feature(nullptr,0,{},&step)==Status::InvalidInput &&
        awl::advance_world_map_native_held_item_feature(feature.get(),0,{},nullptr)==Status::InvalidInput,"native invalid construction/update preserves owners");
}
void hash32(uint64_t& h,uint32_t w){for(unsigned i=0;i<4;++i){h^=(w>>(24-i*8))&255;h*=1099511628211ull;}}
void hash_texture(uint64_t& h,const std::optional<awl::WorldMapModelFeatureTexture>& t){hash32(h,t?1u:0u);hash32(h,t?uint32_t(t->bank_identity):0u);hash32(h,t?t->index:0u);}
void hash_state(uint64_t& h,const State& s){
    for(uint32_t w:{uint32_t(s.resource_0),uint32_t(s.buffer_34),s.flags_38,uint32_t(s.enabled_3c),uint32_t(s.byte_3d),uint32_t(s.metadata_44),uint32_t(s.cache_48.size())})hash32(h,w);
    for(float f:s.matrix_4)hash32(h,bits(f));
    for(const auto& entry:s.cache_48){hash32(h,entry.channel);hash_texture(h,entry.texture);}
}
void empty_feature_matrix(){
    uint64_t digest=14695981039346656037ull;
    for(uint32_t capacity=0;capacity<32;++capacity){
        std::unique_ptr<awl::WorldMapNativeModelFeature> feature;
        expect(awl::construct_world_map_native_model_feature(capacity,&feature)==Status::Constructed,"bounded native feature construction succeeds");
        if(!feature)continue;hash32(digest,capacity);hash_state(digest,feature->state());
    }
    std::cout<<"NATIVE_EMPTY_FEATURE_MATRIX 32 "<<std::hex<<digest<<std::dec<<'\n';
    expect(digest==0x9bfe660e103e5225ull,"native constructed fields and unknown cache payloads match mapped 27D0 reset/initialization");
}
void feature_matrix(){
    uint64_t digest=14695981039346656037ull;unsigned cases=0;
    for(uint32_t seed=0;seed<256;++seed)for(uint32_t choice=0;choice<3;++choice)for(uint32_t id_case=0;id_case<3;++id_case)for(uint32_t missing=0;missing<7;++missing){
        State state;state.resource_0=444;state.buffer_34=999;state.flags_38=seed;state.enabled_3c=88;state.byte_3d=99;state.metadata_44=555;
        for(uint32_t i=0;i<12;++i)state.matrix_4[i]=float(50+i+seed);
        for(uint32_t i=0;i<seed%4;++i){const auto value=(seed>>(i*2))&3;
            state.cache_48.push_back({value==0?UINT32_MAX:value==1?0u:value==2?1u:7u,
                (seed+i)&1?std::optional<awl::WorldMapModelFeatureTexture>({77,static_cast<uint16_t>(i+5)}):std::nullopt});}
        const int32_t item=id_case==0?0:id_case==1?1:0x4ff;
        Observations o;o.row=awl::WorldMapHeldItemFeatureRow{item,static_cast<uint8_t>(seed),static_cast<uint8_t>(seed%7),
            static_cast<uint8_t>((seed+choice)%7),static_cast<uint16_t>(seed*251+choice*3),static_cast<uint16_t>(65535-seed),
            static_cast<uint16_t>(seed*97+choice*5),static_cast<uint16_t>(seed+100)};o.null_item_alternate_group=static_cast<uint8_t>(seed%7);
        const auto& row=*o.row;const bool split=row.type_0==1 || row.type_0==2 || row.type_0==4 || item==0x4ff;
        const bool alternate=split && row.alternate_group_3!=*o.null_item_alternate_group;
        const uint8_t group=alternate?row.alternate_group_3:row.group_2;
        o.resource=awl::WorldMapModelFeatureResourceBinding{alternate?Bank::Alternate:Bank::Primary,group,
            uint64_t(100+group+(alternate?1000:0)),uint64_t(10000+group+(alternate?1000:0)),std::vector<awl::WorldMapModelFeatureCommand>{}};
        for(uint32_t j=0;j<seed%7;++j)o.resource->commands->push_back({uint8_t((seed>>(j%8))&1?1:7),static_cast<uint8_t>((j+seed+choice)%3)});
        o.texture_bank_identity=alternate?201:200;
        if(missing==1)o.row.reset();if(missing==2)o.null_item_alternate_group.reset();if(missing==3)o.resource.reset();
        if(missing==4)o.texture_bank_identity.reset();if(missing==5)o.resource->commands.reset();if(missing==6)o.resource->metadata_44=0;
        Step step;const auto status=awl::prepare_world_map_held_item_feature(state,item,o,&step);
        for(uint32_t w:{seed,choice,id_case,missing,uint32_t(status),uint32_t(step.route.has_value())})hash32(digest,w);
        if(step.route)for(uint32_t w:{uint32_t(step.route->bank),uint32_t(step.route->group),uint32_t(step.route->textures[0]),uint32_t(step.route->textures[1])})hash32(digest,w);
        hash_state(digest,status==Status::InvalidInput?state:step.after);hash32(digest,uint32_t(step.writes.size()));
        for(const auto& w:step.writes){hash32(digest,w.offset);hash32(digest,uint32_t(w.value));hash32(digest,w.cache_index.value_or(UINT32_MAX));hash_texture(digest,w.texture);}
        ++cases;
    }
    std::cout<<"HELD_ITEM_FEATURE_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==16128 && digest==0x90cb04aea4f20ed5ull,"held-item routes, ordered feature/cache stores and evidence stops match independently mapped instructions");
}
} // namespace
int main(){test_routes_and_cache();test_stops();test_native_lifetime();empty_feature_matrix();feature_matrix();return failures?1:0;}
