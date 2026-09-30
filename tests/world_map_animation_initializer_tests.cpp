#include "awl/world_map_animation_initializer.h"

#include <array>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
using State = awl::WorldMapAnimationInitializerState;
using Step = awl::WorldMapAnimationInitializerStep;
using Status = awl::WorldMapAnimationInitializerStatus;
using Descriptor = awl::WorldMapActorAnimationDescriptor;
using Channel = awl::WorldMapAnimationChannelState;
using Record = awl::WorldMapAnimationPlaybackRecord;
using Observations = awl::WorldMapAnimationInitializerObservations;
int failures = 0;
void expect(bool yes, const char* message) { if (!yes) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
uint32_t bits(float value) { uint32_t word; std::memcpy(&word, &value, 4); return word; }
void hash(uint64_t& value, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) { value ^= (word >> (24-i*8)) & 255; value *= 1099511628211ull; }
}
awl::WorldMapAnimationBank bank() {
    // Invented one-section raw clip with scalar 8 and ID zero.
    std::vector<uint8_t> bytes(24); bytes[5] = 1; bytes[8] = 0x41;
    awl::WorldMapAnimationBank result; expect(result.parse(200, bytes), "synthetic bank parses"); return result;
}
std::vector<Record> records() {
    std::vector<Record> result;
    for (uint32_t id = 1; id < 5; ++id) {
        awl::WorldMapAnimationPlayback p;
        p.position_0 = static_cast<float>(id*10); p.rate_4 = static_cast<float>(id*10+1);
        p.word_8 = id*10+2; p.limit_c = static_cast<float>(id*10+3);
        p.clip_10 = awl::WorldMapAnimationClipReference{77,id*10+4};
        p.link_14 = id*10+5; p.value_18 = static_cast<float>(id*10+6);
        result.push_back({id,p});
    }
    return result;
}
State initial() {
    State state; state.animation.current_descriptor_0 = state.animation.base_descriptor_4 = 1;
    state.animation.model_identity_30 = 100; state.animation.default_count_14 = 7;
    state.animation.default_duration_c = 13; state.animation.deadline_10 = 23;
    state.animation.count_18 = 5; state.animation.completed_count_1c = 6;
    state.animation.flag_21 = 1; state.animation.speed_8 = 2;
    state.primary = {1,2,2,3,4,-3,1}; state.records = records();
    state.has_optional_bindings = true; return state;
}
constexpr uint32_t absent = 63u | (127u<<11) | (255u<<19);
Status prepare(const State& state, const Descriptor& descriptor, const Observations& obs, Step* out) {
    const auto owned = bank();
    return awl::prepare_world_map_animation_initializer(state,2,descriptor,awl::WorldMapActorAnimationGroup{0,200},
        awl::WorldMapAnimationModelBinding{100,1},&owned,obs,out);
}
void test_initializer() {
    auto state = initial(); Step step; Observations obs;
    expect(prepare(state,{2,0,absent,0},obs,&step) == Status::RequiresModelHierarchy && step.hierarchy_model == 100 &&
        step.after.animation.base_descriptor_4 == 2 && !step.loop && step.rate == 1 &&
        step.after.animation.flag_21 == 0 && step.after.animation.count_18 == 5 && step.after.animation.deadline_10 == 23 &&
        step.after.records[0].state.link_14 == 0 && step.after.records[0].state.value_18 == 0 &&
        step.after.records[1].state.word_8 == 0 && step.after.records[1].state.rate_4 == 1 &&
        state.animation.base_descriptor_4 == 1 && state.primary.elapsed_0 == 1 && state.records[0].state.link_14 == 15,
        "complete supplied prefix reaches hierarchy without accepting descriptor, clocks, playback or a live model");
    expect(prepare(state,{2,3u<<10,absent,0},obs,&step) == Status::RequiresModelHierarchy && step.loop &&
        step.after.animation.count_18 == 7 && step.after.animation.completed_count_1c == 0,
        "count mode zero value uses default count and clears completed counter");
    for (uint32_t count : {0u,1u,2u,UINT32_MAX}) {
        state.animation.default_count_14 = count;
        expect(prepare(state,{2,3u<<10,absent,0},obs,&step) == Status::RequiresModelHierarchy &&
            step.loop == (count>1) && step.after.animation.count_18 == count, "count loop tests unsigned count greater than one");
    }
    state = initial();
    expect(prepare(state,{2,2u<<10,absent,0},obs,&step) == Status::RequiresClock &&
        step.after.animation.deadline_10 == 23 && !step.settings, "timed mode cannot advance from unknown clock");
    obs.clock = UINT32_MAX-5;
    expect(prepare(state,{2,2u<<10,absent,0},obs,&step) == Status::RequiresModelHierarchy &&
        step.after.animation.deadline_10 == 7 && step.loop, "zero timed value uses default duration with unsigned wrap");
    expect(prepare(state,{2,(2u<<10)|(2u<<14),absent,0},obs,&step) == Status::RequiresModelHierarchy &&
        step.after.animation.deadline_10 == 14, "nonzero timed value uses ten clock units per value");
    state.records[0].state.clip_10.reset();
    expect(prepare(state,{2,0x1001000u,absent,0},obs,&step) == Status::RequiresModelHierarchy && step.rate == -2 &&
        bits(step.after.records[1].state.position_0) == 0x40ffff2eu && step.after.records[0].state.link_14 == 0,
        "negative playback resets to limit minus verified epsilon and direct copy clears blend chain");
    state.animation.speed_8 = std::numeric_limits<float>::quiet_NaN();
    expect(prepare(state,{2,0,absent,0},obs,&step) == Status::RequiresModelHierarchy,
        "unread speed is ignored when its descriptor flag is clear");
    const auto saved = step.after.animation.base_descriptor_4;
    expect(prepare(state,{2,0x1000,absent,0},obs,&step) == Status::InvalidInput && step.after.animation.base_descriptor_4 == saved,
        "reached nonfinite speed fails without changing output");
    state = initial(); state.has_optional_bindings = false;
    expect(prepare(state,{2,0,absent,0},obs,&step) == Status::RequiresOptionalBindings && !step.settings,
        "unknown optional pointers do not silently become absent");
    state.has_optional_bindings = true; state.secondary_model_c0 = 300;
    expect(prepare(state,{2,0,absent,0},obs,&step) == Status::RequiresSecondaryRelease && !step.settings,
        "existing secondary model blocks at untranslated release before clocks/rate");
    state.secondary_model_c0 = 0;
    expect(prepare(state,{2,0,absent & ~(127u<<11),0},obs,&step) == Status::RequiresSecondarySetup && step.secondary_index == 0u &&
        !step.settings, "secondary zero is a real selection, not absence or primary index");
    state.feature_3c = awl::WorldMapAnimationFeature3c{500,5,6};
    expect(prepare(state,{2,0,absent,0},obs,&step) == Status::RequiresModelHierarchy &&
        step.after.feature_3c->resource_0 == 0 && step.after.feature_3c->value_4 == 0,
        "absent feature index changes pointer to null and clears both floats");
    state.feature_3c = awl::WorldMapAnimationFeature3c{0,5,6};
    expect(prepare(state,{2,0,(absent & ~(255u<<19))|(0x26u<<19),0},obs,&step) == Status::RequiresModelHierarchy &&
        step.after.feature_3c->value_4 == 5 && step.after.feature_3c->value_8 == 6,
        "special feature index 0x26 skips lookup and equal-null pointer preserves floats");
    const uint32_t feature_index = (absent & ~(255u<<19))|(3u<<19);
    expect(prepare(state,{2,0,feature_index,0},obs,&step) == Status::RequiresFeature3cLookup && step.feature_3c_index == 3u,
        "feature resource lookup remains keyed evidence");
    obs.feature_3c = awl::WorldMapAnimationFeature3cLookup{3,0x1000001f4ull};
    expect(prepare(state,{2,0,feature_index,0},obs,&step) == Status::RequiresModelHierarchy &&
        step.after.feature_3c->resource_0 == obs.feature_3c->resource_identity && step.after.feature_3c->value_8 == 0,
        "feature pointer change preserves all identity bits and resets values");
    const auto owned = bank();
    expect(awl::prepare_world_map_animation_initializer(state,1,std::nullopt,std::nullopt,std::nullopt,nullptr,{},&step) == Status::Unchanged &&
        !step.setup && step.after.animation.base_descriptor_4 == 1, "equal base skips every initializer dependency");
    expect(awl::prepare_world_map_animation_initializer(step.after,2,Descriptor{2,0,absent,0},std::nullopt,std::nullopt,&owned,{},&step) == Status::InvalidInput &&
        prepare(state,{2,0,absent,0},obs,nullptr) == Status::InvalidInput, "output alias and null output are rejected");
}
void test_feature() {
    using FStatus = awl::WorldMapAnimationFeatureStatus;
    awl::WorldMapAnimationFeature38 state{0x3a,0x24,2,500,{1,2,3,4},{5,6,7,8}};
    Observations obs; obs.clock = 99;
    obs.rows = {{500,0,0,10},{500,0,1,0},{600,0,0,20},{700,0,0,30}};
    obs.fallback_table_38 = 600; obs.fallback_table_24 = 700;
    awl::WorldMapAnimationFeatureStep step;
    expect(awl::prepare_world_map_animation_feature38(state,0,obs,&step) == FStatus::Prepared &&
        step.after.index_8 == 0 && step.after.first_14.resource_0 == 10 && step.after.second_24.resource_0 == 0 &&
        step.after.first_14.clock_4 == 99 && step.after.first_14.value_8 == 0 && step.after.second_24.rate_c == 1 && state.index_8 == 2,
        "local table resets two timers in column order, including observed null resource");
    state.index_8 = 0;
    expect(awl::prepare_world_map_animation_feature38(state,0,obs,&step) == FStatus::Prepared &&
        step.after.first_14.clock_4 == 99 && step.after.first_14.value_8 == 0,
        "equal local selection still resets timers; it is not a pointer-equality skip");
    state.index_8 = 2;
    expect(awl::prepare_world_map_animation_feature38(state,UINT32_MAX,obs,&step) == FStatus::Prepared &&
        step.after.first_14.resource_0 == 20 && step.after.second_24.resource_0 == 30,
        "selection removal restores independently gated fallback timers");
    state.index_8 = UINT32_MAX;
    expect(awl::prepare_world_map_animation_feature38(state,UINT32_MAX,{},&step) == FStatus::Prepared &&
        step.after.first_14.resource_0 == 1 && step.after.second_24.clock_4 == 6,
        "already absent selection skips fallback tables and clock");
    state.index_8 = 2; obs.rows.erase(obs.rows.begin()+1);
    expect(awl::prepare_world_map_animation_feature38(state,0,obs,&step) == FStatus::RequiresRow &&
        step.required_row->column == 1 && step.after.first_14.resource_0 == 1 && step.after.index_8 == 2,
        "missing second row rolls back first timer and selection store");
    obs.rows.push_back({500,0,1,0}); obs.rows.push_back({500,0,1,9});
    expect(awl::prepare_world_map_animation_feature38(state,0,obs,&step) == FStatus::InvalidInput,
        "ambiguous reached row is rejected");
    obs.rows.pop_back(); obs.clock.reset();
    expect(awl::prepare_world_map_animation_feature38(state,0,obs,&step) == FStatus::RequiresClock,
        "timer clock is required only on reached lookup");
    obs.fallback_table_38.reset();
    expect(awl::prepare_world_map_animation_feature38(state,UINT32_MAX,obs,&step) == FStatus::RequiresTable,
        "required global fallback table does not default to null");
    state.type_0 = 0x8000003a; state.type_4 = 0x80000024;
    expect(awl::prepare_world_map_animation_feature38(state,UINT32_MAX,{},&step) == FStatus::Prepared,
        "negative signed types skip both threshold gates");
    expect(awl::prepare_world_map_animation_feature38(step.after,0,obs,&step) == FStatus::InvalidInput,
        "feature output aliasing is rejected");
}
void test_model_links() {
    auto state=initial(); Step step;
    state.model_links=awl::WorldMapModelLinkState{{
        {100,99,0,{200,0,0,0},{1,2,3,4}}, {200,100,4,{300,0,0,0},{}}, {300,200,0,{999,0,0,0},{}}}};
    expect(prepare(state,{2,0,absent,0},{},&step)==Status::Prepared && step.hierarchy &&
        step.hierarchy->writes.size()==4 && step.after.model_links->nodes[0].parent_150==99 &&
        step.after.model_links->nodes[0].children_15c[0]==0 && step.after.model_links->nodes[1].parent_150==0 &&
        step.after.model_links->nodes[1].children_15c[0]==0 && step.after.model_links->nodes[2].children_15c[0]==999 &&
        step.after.animation.base_descriptor_4==2 && step.after.records[0].state.clip_10 &&
        state.animation.base_descriptor_4==1 && state.records[0].state.link_14==15 && state.model_links->nodes[0].children_15c[0]==200,
        "no-secondary initializer prepares complete supplied metadata/link proposal without applying live actor state");
    state.model_links->nodes.pop_back();
    expect(prepare(state,{2,0,absent,0},{},&step)==Status::RequiresModelHierarchy && step.hierarchy &&
        step.hierarchy->required_node==300 && step.hierarchy->writes.empty() &&
        step.after.model_links->nodes[1].parent_150==100 && step.after.records[0].state.clip_10,
        "missing nested hierarchy node blocks atomically at final helper after ordered metadata prefix");
    state.model_links->nodes[0].identity=101;
    expect(prepare(state,{2,0,absent,0},{},&step)==Status::RequiresModelHierarchy && step.hierarchy->required_node==100,
        "hierarchy root must match the actual supplied primary model identity");
    state.model_links->nodes[0].identity=100; state.model_links->nodes[0].flags_158=4;
    state.model_links->nodes[1].children_15c[0]=100;
    step.hierarchy_model=444;
    expect(prepare(state,{2,0,absent,0},{},&step)==Status::InvalidInput && step.hierarchy_model==444,
        "invalid final hierarchy preserves entire initializer output");
    expect(prepare(state,{2,0,absent & ~(127u<<11),0},{},&step)==Status::RequiresSecondarySetup && !step.hierarchy,
        "secondary setup boundary precedes unread invalid hierarchy");
    state.animation.base_descriptor_4=2;
    expect(prepare(state,{2,0,absent,0},{},&step)==Status::Unchanged && !step.hierarchy,
        "base equality skips all supplied link validation and stores");
}
void test_settings_matrix() {
    const std::array<std::array<uint32_t,2>,6> clocks{{{0,0},{0,1},{1,2},{2,2},{0xfffffffe,UINT32_MAX},{UINT32_MAX,1}}};
    const std::array<std::array<uint64_t,4>,5> layouts{{{1,2,3,4},{2,2,3,4},{3,2,3,4},{4,2,3,4},{1,1,1,1}}};
    uint64_t digest = 14695981039346656037ull; unsigned cases = 0;
    for (const auto& clock:clocks) for (uint32_t mode:{0u,1u,2u,3u,UINT32_MAX}) for (const auto& layout:layouts) {
        for (float rate:{0.0f,-0.0f,1.0f,-1.0f,2.5f,-2.5f}) {
            const auto pool=records(); const Channel channel{clock[0],clock[1],layout[1],layout[2],layout[3],0.25f,mode};
            awl::WorldMapAnimationChannelStep step;
            const auto status=awl::prepare_world_map_animation_channel_settings(channel,pool,100,awl::WorldMapAnimationModelBinding{100,layout[0]},257,rate,&step);
            expect(status==awl::WorldMapAnimationChannelStatus::Prepared,"all finite settings matrix cases prepare");
            if (status!=awl::WorldMapAnimationChannelStatus::Prepared) continue;
            for (uint32_t word:{clock[0],clock[1],mode,static_cast<uint32_t>(layout[0]),bits(rate)}) hash(digest,word);
            for (const auto& r:step.records_after) {
                const auto& p=r.state;
                for (uint32_t word:{static_cast<uint32_t>(r.identity),bits(p.position_0),bits(p.rate_4),p.word_8,bits(p.limit_c),
                    static_cast<uint32_t>(p.clip_10->bank_identity),p.clip_10->offset,static_cast<uint32_t>(p.link_14),bits(p.value_18)}) hash(digest,word);
            }
            ++cases;
        }
    }
    std::cout<<"SETTINGS_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==900 && digest==0x159ce296cb38b421ull,"settings/reset/copy/link matrix matches independent mapped instruction walk");
}
void test_tail_matrix() {
    uint64_t digest=14695981039346656037ull; unsigned cases=0;
    for (uint32_t mode=0;mode<4;++mode) for (uint32_t value:{0u,1u,2u,1023u}) for(uint32_t flags=0;flags<8;++flags) {
        for (uint32_t defaults:{0u,1u,2u,UINT32_MAX}) for(float speed:{0.0f,-0.0f,2.0f,-2.0f}) {
            auto state=initial();state.animation.default_count_14=state.animation.default_duration_c=defaults;state.animation.speed_8=speed;
            state.records[0].state.clip_10.reset();
            const uint32_t word=(mode<<10)|(value<<14)|((flags&1)<<12)|((flags&2)<<12)|((flags&4)<<22);
            Observations obs;obs.clock=UINT32_MAX-5;Step step;
            const auto status=prepare(state,{2,word,absent,0},obs,&step);
            expect(status==Status::RequiresModelHierarchy,"all supplied no-secondary tails stop at hierarchy");
            if(status!=Status::RequiresModelHierarchy) continue;
            const auto& a=step.after.animation;const auto& p=step.after.records[1].state;
            for(uint32_t v:{word,defaults,bits(speed),a.deadline_10,a.count_18,a.completed_count_1c,static_cast<uint32_t>(a.flag_21),
                static_cast<uint32_t>(step.loop),bits(step.rate),bits(p.position_0),bits(p.rate_4),p.word_8}) hash(digest,v);
            ++cases;
        }
    }
    std::cout<<"INITIALIZER_TAIL_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==2048 && digest==0xee6bf1cb710e2a8dull,"control/count/deadline/rate/flag matrix matches independent mapped instruction walk");
}
void test_feature_matrix() {
    uint64_t digest=14695981039346656037ull; unsigned cases=0;
    for (uint32_t type0:{0x39u,0x3au,0x3bu,0x8000003au,UINT32_MAX}) {
        for(uint32_t type4:{0x23u,0x24u,0x25u,0x80000024u,UINT32_MAX}) for(uint32_t old:{2u,UINT32_MAX}) {
            for(uint32_t index:{0u,1u,UINT32_MAX}) for(uint32_t local:{0u,500u}) {
                awl::WorldMapAnimationFeature38 state{type0,type4,old,local,{1,2,3,4},{5,6,7,8}};
                Observations obs;obs.clock=99;obs.fallback_table_38=600;obs.fallback_table_24=700;
                for(uint32_t table:{500u,600u,700u}) for(uint32_t row:{0u,1u}) for(uint32_t col:{0u,1u}) {
                    obs.rows.push_back({table,row,col,table+row*10+col});
                }
                awl::WorldMapAnimationFeatureStep step;
                const auto status=awl::prepare_world_map_animation_feature38(state,index,obs,&step);
                expect(status==awl::WorldMapAnimationFeatureStatus::Prepared,"all supplied finite feature cases prepare");
                if(status!=awl::WorldMapAnimationFeatureStatus::Prepared) continue;
                for(uint32_t word:{type0,type4,old,index,local,step.after.index_8}) hash(digest,word);
                for(const auto& timer:{step.after.first_14,step.after.second_24}) {
                    for(uint32_t word:{static_cast<uint32_t>(timer.resource_0),timer.clock_4,bits(timer.value_8),bits(timer.rate_c)}) hash(digest,word);
                }
                ++cases;
            }
        }
    }
    std::cout<<"FEATURE38_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==300 && digest==0x3fae9b931303fac5ull,"optional feature branch/store matrix matches independent mapped instruction walk");
}
void test_failure_order() {
    auto state=initial();Step step;Observations obs;
    state.feature_38=awl::WorldMapAnimationFeature38{0x3a,0x24,2,500,{1,2,3,4},{5,6,7,8}};
    const uint32_t word4=absent & ~63u;
    expect(prepare(state,{2,2u<<10,word4,0},obs,&step)==Status::RequiresFeatureRow && !step.settings &&
        step.after.feature_38->index_8==2 && step.after.animation.deadline_10==23,
        "optional feature row stop precedes later secondary/clock/control stores");
    obs.rows={{500,0,0,10},{500,0,1,11}};obs.clock=99;
    expect(prepare(state,{2,0,word4,0},obs,&step)==Status::RequiresModelHierarchy &&
        step.after.feature_38->first_14.resource_0==10 && step.after.feature_38->second_24.resource_0==11,
        "owned channel and keyed feature evidence compose to final hierarchy stop");
    using CStatus=awl::WorldMapAnimationChannelStatus;
    awl::WorldMapAnimationChannelStep channel_step;
    const auto pool=records();const Channel channel{1,2,2,3,4,0.25f,2};
    expect(awl::prepare_world_map_animation_channel_settings(channel,pool,100,std::nullopt,1,-1,&channel_step)==CStatus::RequiresModelBinding &&
        channel_step.records_after[1].state.rate_4==21,"missing model binding rolls back target loop/rate/reset prefix");
    auto missing=pool;missing.erase(missing.begin()+2);
    expect(awl::prepare_world_map_animation_channel_settings(channel,missing,100,awl::WorldMapAnimationModelBinding{100,1},1,1,&channel_step)==
        CStatus::RequiresPlaybackRecord && channel_step.required_record==3 && channel_step.records_after[0].state.position_0==10,
        "missing blend-link destination rolls back older-to-model copy and first link store");
    auto invalid=channel;invalid.blend_14=std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_animation_channel_settings(invalid,pool,100,awl::WorldMapAnimationModelBinding{100,1},1,1,&channel_step)==CStatus::InvalidInput,
        "reached nonfinite interrupted blend is rejected");
    invalid.mode_18=3;
    expect(awl::prepare_world_map_animation_channel_settings(invalid,pool,0,std::nullopt,256,1,&channel_step)==CStatus::Prepared &&
        channel_step.records_after[1].state.word_8==0,"unsupported model mode skips unused binding/blend while retaining target settings");
    invalid=channel;invalid.previous_c=0;
    expect(awl::prepare_world_map_animation_channel_settings(invalid,pool,100,awl::WorldMapAnimationModelBinding{100,1},1,1,&channel_step)==CStatus::InvalidInput,
        "reached null blend link destination is rejected");
    auto high=pool;high[1].identity=0x100000002ull;auto high_channel=channel;high_channel.mode_18=1;high_channel.target_8=high[1].identity;
    expect(awl::prepare_world_map_animation_channel_settings(high_channel,high,100,awl::WorldMapAnimationModelBinding{100,1},1,1,&channel_step)==CStatus::Prepared &&
        channel_step.records_after[0].state.link_14==high[1].identity,"blend record pointers retain all native identity bits");
    expect(awl::prepare_world_map_animation_channel_settings(channel_step.after,pool,100,std::nullopt,1,1,&channel_step)==CStatus::InvalidInput &&
        awl::prepare_world_map_animation_channel_settings(channel,channel_step.records_after,100,std::nullopt,1,1,&channel_step)==CStatus::InvalidInput,
        "channel settings output aliases are rejected");
}
} // namespace
int main(){test_initializer();test_feature();test_model_links();test_failure_order();test_settings_matrix();test_tail_matrix();test_feature_matrix();return failures==0?0:1;}
