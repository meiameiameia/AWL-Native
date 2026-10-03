#include "awl/world_map_model_links.h"

#include <iostream>

namespace {
using Node = awl::WorldMapModelLinkNode;
using State = awl::WorldMapModelLinkState;
using Step = awl::WorldMapModelLinkStep;
using Status = awl::WorldMapModelLinkStatus;
int failures = 0;
void expect(bool yes, const char* message) { if (!yes) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
bool same_features(const std::optional<awl::WorldMapModelFeatureState>& a,const std::optional<awl::WorldMapModelFeatureState>& b) {
    if(a.has_value()!=b.has_value())return false;if(!a)return true;
    if(a->head_50!=b->head_50 || a->nodes.size()!=b->nodes.size())return false;
    for(size_t i=0;i<a->nodes.size();++i)if(a->nodes[i].order_1!=b->nodes[i].order_1 ||
        a->nodes[i].feature_8!=b->nodes[i].feature_8 || a->nodes[i].next_14!=b->nodes[i].next_14)return false;
    return true;
}
bool same(const State& a, const State& b) {
    if (a.nodes.size() != b.nodes.size()) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const auto& x = a.nodes[i]; const auto& y = b.nodes[i];
        if (x.identity != y.identity || x.parent_150 != y.parent_150 || x.flags_158 != y.flags_158 ||
            x.children_15c != y.children_15c || x.attachments_16c != y.attachments_16c || !same_features(x.features,y.features)) return false;
    }
    return true;
}
bool write(const Step& step, size_t i, uint64_t identity, uint32_t offset) {
    return i < step.writes.size() && step.writes[i].identity == identity &&
        step.writes[i].offset == offset && step.writes[i].value == 0;
}
void test_order() {
    const State state{{{1,101,0,{2,3,0,0},{10,11,12,13}},
        {2,102,0x80000004u,{3,0,0,0},{20,21,22,23}}, {3,103,0xb,{999,0,0,0},{30,31,32,33}}}};
    Step step;
    expect(awl::prepare_world_map_model_links_clear(state,1,&step) == Status::Prepared &&
        step.after.nodes[0].parent_150 == 101 && step.after.nodes[0].children_15c == std::array<uint64_t,4>{} &&
        step.after.nodes[1].parent_150 == 0 && step.after.nodes[1].children_15c[0] == 0 &&
        step.after.nodes[2].parent_150 == 0 && step.after.nodes[2].children_15c[0] == 999,
        "root flags do not gate traversal; child bit four gates descent and unread missing grandchildren stay opaque");
    expect(step.writes.size() == 6 && write(step,0,2,0x150) && write(step,1,3,0x150) &&
        write(step,2,2,0x15c) && write(step,3,1,0x15c) && write(step,4,3,0x150) && write(step,5,1,0x160),
        "depth-first stores precede parent slot clear and shared child receives its second parent store");
    for (size_t i=0;i<state.nodes.size();++i) {
        expect(step.after.nodes[i].flags_158 == state.nodes[i].flags_158 &&
            step.after.nodes[i].attachments_16c == state.nodes[i].attachments_16c,
            "all flags and attachment halfwords are retained");
    }
    auto supplied = state;
    expect(awl::advance_world_map_model_links_clear(&supplied,1,&step) == Status::Advanced &&
        same(supplied,step.after) && state.nodes[1].parent_150 == 102,
        "complete isolated helper applies only to separate supplied snapshots");
    State high{{{0x100000001ull,0x200000064ull,0,{0x300000002ull,0,0,0},{}},
        {0x300000002ull,0x400000064ull,0,{}, {}}}};
    expect(awl::prepare_world_map_model_links_clear(high,high.nodes[0].identity,&step) == Status::Prepared &&
        step.after.nodes[0].parent_150 == 0x200000064ull && write(step,0,0x300000002ull,0x150) &&
        write(step,1,0x100000001ull,0x15c), "full native identity widths survive lookup, retained fields and write trace");
    State self{{{1,101,0,{1,0,0,0},{}}}};
    expect(awl::prepare_world_map_model_links_clear(self,1,&step) == Status::Prepared &&
        step.after.nodes[0].parent_150 == 0 && step.after.nodes[0].children_15c[0] == 0 && step.writes.size() == 2,
        "self link terminates when child descent bit is clear, including clearing root parent");
    State empty{{{1,101,4,{}, {}}}};
    expect(awl::prepare_world_map_model_links_clear(empty,1,&step) == Status::Prepared &&
        same(empty,step.after) && step.writes.empty(), "empty root preserves its own parent and flags without stores");
}
void test_failure() {
    State state{{{1,101,0,{2,0,0,0},{}}, {2,102,4,{3,0,0,0},{}}}};
    Step step;
    expect(awl::prepare_world_map_model_links_clear(state,1,&step) == Status::RequiresNode &&
        step.required_node == 3 && step.writes.empty() && same(state,step.after),
        "missing nested child rolls back earlier parent store and reports first reached identity");
    const auto before = state;
    expect(awl::advance_world_map_model_links_clear(&state,1,&step) == Status::RequiresNode && same(state,before),
        "incomplete helper cannot mutate supplied input");
    expect(awl::prepare_world_map_model_links_clear(state,9,&step) == Status::RequiresNode &&
        step.required_node == 9 && same(state,step.after), "missing root is explicit, never observed empty");
    step.required_node = 444; const auto saved = step.after;
    state.nodes.push_back({2,0,0,{}, {}});
    expect(awl::prepare_world_map_model_links_clear(state,1,&step) == Status::InvalidInput &&
        step.required_node == 444 && same(saved,step.after), "duplicate keys reject without overwriting output");
    state.nodes.back().identity = 0;
    expect(awl::prepare_world_map_model_links_clear(state,1,&step) == Status::InvalidInput && step.required_node == 444,
        "zero snapshot key is invalid");
    State cycle{{{1,101,4,{2,0,0,0},{}}, {2,102,4,{1,0,0,0},{}}}};
    expect(awl::prepare_world_map_model_links_clear(cycle,1,&step) == Status::InvalidInput &&
        step.required_node == 444 && same(saved,step.after), "reached recursive cycle is rejected atomically");
    expect(awl::advance_world_map_model_links_clear(&cycle,1,&step) == Status::InvalidInput &&
        cycle.nodes[1].parent_150 == 102 && cycle.nodes[0].children_15c[0] == 2,
        "cycle rejection preserves supplied graph, including parent writes");
    cycle.nodes[1].flags_158 = 0;
    expect(awl::prepare_world_map_model_links_clear(cycle,1,&step) == Status::Prepared &&
        step.after.nodes[1].children_15c[0] == 1, "masked cycle is unread and does not block a terminating original path");
    expect(awl::prepare_world_map_model_links_clear(cycle,0,&step) == Status::InvalidInput &&
        awl::prepare_world_map_model_links_clear(cycle,1,nullptr) == Status::InvalidInput &&
        awl::prepare_world_map_model_links_clear(step.after,1,&step) == Status::InvalidInput &&
        awl::advance_world_map_model_links_clear(nullptr,1,&step) == Status::InvalidInput &&
        awl::advance_world_map_model_links_clear(&cycle,1,nullptr) == Status::InvalidInput &&
        awl::advance_world_map_model_links_clear(&step.after,1,&step) == Status::InvalidInput,
        "null inputs and output aliases are rejected");
}
uint32_t next(uint32_t& random) { random = random*1664525u+1013904223u; return random; }
State dag(uint32_t seed) {
    State state; uint32_t random = seed;
    for (uint32_t i=1;i<=20;++i) {
        Node node; node.identity=i; node.parent_150=100+i; node.flags_158=next(random)&255u;
        for (uint32_t slot=0;slot<4;++slot) {
            node.attachments_16c[slot]=static_cast<uint16_t>(seed+i*17+slot*7);
            const uint32_t value=next(random);
            if (i<20 && value%4 != 0) node.children_15c[slot]=i+1+((value>>3)%(20-i));
        }
        state.nodes.push_back(node);
    }
    return state;
}
State chain(uint32_t length) {
    State state;
    for (uint32_t i=1;i<=length;++i) {
        Node node; node.identity=i; node.parent_150=100+i; node.flags_158=i==1 ? 0u : 15u;
        for (uint32_t slot=0;slot<4;++slot) node.attachments_16c[slot]=static_cast<uint16_t>(i*17+slot*7);
        if (i<length) node.children_15c[(i+length)%4]=i+1;
        state.nodes.push_back(node);
    }
    return state;
}
State aliases(uint32_t seed) {
    return {{{1,101,0,{2,2,3,1},{1,2,3,4}},
        {2,102,seed&255u,{4,1,0,0},{5,6,7,8}},
        {3,103,(seed>>1)&255u,{0,0,2,4},{9,10,11,12}},
        {4,104,0,{999,0,0,0},{13,14,15,16}}}};
}
void hash32(uint64_t& value,uint32_t word) {
    for(unsigned i=0;i<4;++i) {value^=(word>>(24-i*8))&255u;value*=1099511628211ull;}
}
void hash64(uint64_t& value,uint64_t word) {hash32(value,static_cast<uint32_t>(word>>32));hash32(value,static_cast<uint32_t>(word));}
void test_matrix() {
    uint64_t digest=14695981039346656037ull; unsigned cases=0;
    auto compare=[&](const State& state,uint32_t family,uint32_t seed) {
        Step step; const auto status=awl::prepare_world_map_model_links_clear(state,1,&step);
        expect(status==Status::Prepared,"all bounded mapped matrix cases prepare");
        if(status!=Status::Prepared) return;
        hash32(digest,family);hash32(digest,seed);hash32(digest,static_cast<uint32_t>(step.after.nodes.size()));
        for(const auto& node:step.after.nodes) {
            hash64(digest,node.identity);hash64(digest,node.parent_150);hash32(digest,*node.flags_158);
            for(auto child:node.children_15c) hash64(digest,child);
            for(auto attachment:node.attachments_16c) hash32(digest,attachment);
        }
        hash32(digest,static_cast<uint32_t>(step.writes.size()));
        for(const auto& w:step.writes) {hash64(digest,w.identity);hash32(digest,w.offset);hash64(digest,w.value);}
        ++cases;
    };
    for(uint32_t seed=0;seed<2048;++seed) compare(dag(seed),0,seed);
    for(uint32_t length=1;length<=64;++length) compare(chain(length),1,length);
    for(uint32_t seed=0;seed<256;++seed) compare(aliases(seed),2,seed);
    std::cout<<"MODEL_LINK_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==2368 && digest==0x3b2a8bdc489a78bcull,"final nodes and ordered stores match independent mapped PPC execution");
    const auto deep=chain(4096); Step step;
    expect(awl::prepare_world_map_model_links_clear(deep,1,&step)==Status::Prepared &&
        step.writes.size()==8190 && step.after.nodes.back().parent_150==0 &&
        step.after.nodes[0].parent_150==101,"long supplied chain uses bounded explicit traversal without native recursion");
}
void test_attachments() {
    using AStatus=awl::WorldMapModelAttachmentStatus;
    using Binding=awl::WorldMapModelAttachmentBinding;
    const State state{{{1,101,std::nullopt,{4,0,0,0},{10,11,12,13}},
        {2,102,std::nullopt,{4,0,0,0},{20,21,22,23}},
        {3,103,std::nullopt,{}, {30,31,32,33}}, {4,104,0u,{}, {40,41,42,43}}}};
    const std::vector<Binding> bindings{{1,uint16_t(0),std::nullopt},{2,uint16_t(2),uint16_t(0xbeef)},
        {3,uint16_t(1),uint16_t(0)},{4,uint16_t(0),uint16_t(0)}};
    awl::WorldMapModelAttachmentRequest request{1,2,0,3};awl::WorldMapModelAttachmentStep step;
    expect(awl::prepare_world_map_model_attachments(state,request,bindings,&step)==AStatus::Prepared &&
        step.auxiliary_slot==1u && step.secondary_slot==0u && !step.after.nodes[0].flags_158 &&
        step.after.nodes[0].children_15c[0]==2 && step.after.nodes[0].attachments_16c[0]==0xbeef &&
        step.after.nodes[1].parent_150==1 && step.after.nodes[1].flags_158==15u &&
        step.after.nodes[1].children_15c[1]==3 && step.after.nodes[1].attachments_16c[1]==2 &&
        step.after.nodes[2].parent_150==2 && step.after.nodes[2].flags_158==12u && step.after.nodes[3].parent_150==0,
        "primary clear, first-free auxiliary link, flags clear and resource-based secondary link preserve unknown root flag");
    expect(step.writes.size()==11 && step.writes[0].identity==4 && step.writes[0].offset==0x150 && step.writes[0].value==0,
        "ordered clear precedes all attachment stores");
    const std::array<uint32_t,9> offsets{0x150,0x160,0x16e,0x158,0x158,0x150,0x15c,0x16c,0x158};
    for(size_t i=0;i<offsets.size();++i)expect(step.writes[i+2].offset==offsets[i],"attach/halfword/flag stores have original order");
    request.feature_c4.reset();
    expect(awl::prepare_world_map_model_attachments(state,request,bindings,&step)==AStatus::RequiresSource &&
        step.required_field==0xc4 && same(step.after,state) && step.writes.empty(),"unknown C4 rolls back primary clearing");
    request.feature_c4=99;
    expect(awl::prepare_world_map_model_attachments(state,request,bindings,&step)==AStatus::RequiresFeatureBinding &&
        step.feature_node_index==2u && step.required_source==99 && same(step.after,state),
        "nonnull C4 records selected node and stops before untranslated feature binding");
    request.feature_c4=0;request.auxiliary_model_c8.reset();
    expect(awl::prepare_world_map_model_attachments(state,request,bindings,&step)==AStatus::RequiresSource &&
        step.required_field==0xc8 && same(step.after,state),"C8 evidence is required only after observed null C4");
    request.auxiliary_model_c8=0;
    expect(awl::prepare_world_map_model_attachments(state,request,{},&step)==AStatus::Prepared &&
        step.writes.size()==2 && !step.after.nodes[1].flags_158,"null C8 skips all secondary data/flag/index reads");
    request.auxiliary_model_c8=3;auto missing=bindings;missing[1].resource_attachment_index.reset();
    expect(awl::prepare_world_map_model_attachments(state,request,missing,&step)==AStatus::RequiresAttachmentIndex &&
        step.required_model==2 && same(step.after,state) && step.writes.empty(),"missing resource halfword rolls back auxiliary stores and flag clear");
    missing=bindings;missing[1].first_node_index.reset();
    expect(awl::prepare_world_map_model_attachments(state,request,missing,&step)==AStatus::RequiresNodeIndex &&
        step.required_model==2 && same(step.after,state) && step.writes.empty(),
        "C8 path selects the secondary model's node before reading the auxiliary child");
    auto full=state;full.nodes[1].children_15c={4,4,4,4};
    expect(awl::prepare_world_map_model_attachments(full,request,bindings,&step)==AStatus::RequiresFlags &&
        step.required_model==3 && step.required_field==0x158 && same(step.after,full),
        "full secondary slots skip flag initialization; later auxiliary flag read remains unknown");
    full.nodes[2].flags_158=0x80000007u;
    expect(awl::prepare_world_map_model_attachments(full,request,bindings,&step)==AStatus::Prepared && !step.auxiliary_slot &&
        step.after.nodes[2].parent_150==103 && step.after.nodes[2].flags_158==0x80000004u,
        "known full-slot path retains parent while still clearing only auxiliary flag bits three");
    State self{{{1,0,std::nullopt,{}, {1,2,3,4}}}};request={1,1,0,1};
    expect(awl::prepare_world_map_model_attachments(self,request,{{1,uint16_t(2),uint16_t(0xbeef)}},&step)==AStatus::Prepared &&
        step.after.nodes[0].children_15c==std::array<uint64_t,4>{1,1,0,0} && step.after.nodes[0].parent_150==1 &&
        step.after.nodes[0].flags_158==15u && step.after.nodes[0].attachments_16c[0]==2 && step.after.nodes[0].attachments_16c[1]==0xbeef,
        "whole-model self aliases observe sequential first-free slots and final flag overwrite");
    auto unknown=state;unknown.nodes[3].flags_158.reset();
    Step clear;
    expect(awl::prepare_world_map_model_links_clear(unknown,1,&clear)==Status::RequiresFlags && clear.required_node==4 &&
        clear.writes.empty() && same(clear.after,unknown),"reached unknown child flags roll back parent clear");
    step.required_model=77;
    expect(awl::prepare_world_map_model_attachments(step.after,request,bindings,&step)==AStatus::InvalidInput && step.required_model==77,
        "attachment output aliases reject without overwriting diagnostics");
}
void test_attachment_matrix() {
    using AStatus=awl::WorldMapModelAttachmentStatus;
    const std::array<std::array<uint64_t,3>,6> layouts{{{1,2,3},{1,2,2},{1,1,3},{1,1,1},{1,2,1},{0,2,3}}};
    uint64_t digest=14695981039346656037ull;unsigned cases=0;
    for(uint32_t seed=0;seed<128;++seed)for(uint32_t mask=0;mask<3;++mask)for(uint32_t family=0;family<6;++family)
        for(const auto& layout:layouts){
            State state;std::vector<awl::WorldMapModelAttachmentBinding> bindings;
            for(uint32_t i=1;i<=4;++i){
                Node node;node.identity=i;node.parent_150=100+i;node.flags_158=i==4?0u:(seed+i-1)&31u;
                if((i==3 && mask==1) || (i==4 && mask==2))node.flags_158.reset();
                for(uint32_t slot=0;slot<4;++slot){node.attachments_16c[slot]=static_cast<uint16_t>(i*10+slot);
                    if(i==1)node.children_15c[slot]=slot==0 && (seed&1)?4:0;
                    if(i==2)node.children_15c[slot]=(seed>>(slot+1))&1?4:0;
                    if(i==3)node.children_15c[slot]=slot==0?99:0;
                }
                state.nodes.push_back(node);uint16_t first=0;
                for(uint16_t j=0;j<4;++j){const uint32_t value=(seed>>j)&1?0xffffu:j==2?0x1ffffu:i*10+j;
                    if(value!=0xffff){first=j;break;}}
                bindings.push_back({i,first,seed&64?std::nullopt:std::optional<uint16_t>(static_cast<uint16_t>(seed*17+i))});
            }
            const awl::WorldMapModelAttachmentRequest request{layout[0],family==0?0:layout[1],
                family<2?std::nullopt:std::optional<uint64_t>(family==2?99:0),
                family<4?std::nullopt:std::optional<uint64_t>(family==4?0:layout[2])};
            awl::WorldMapModelAttachmentStep step;
            const auto status=awl::prepare_world_map_model_attachments(state,request,bindings,&step);
            expect(status!=AStatus::AllocationFailure && status!=AStatus::RequiresNode && status!=AStatus::RequiresNodeIndex,
                "bounded attachment matrix has supplied model/node metadata");
            for(uint32_t w:{seed,mask,family,uint32_t(layout[0]),uint32_t(layout[1]),uint32_t(layout[2]),uint32_t(status),
                uint32_t(step.required_model),uint32_t(step.required_source),step.required_field,uint32_t(step.feature_node_index.has_value()),
                uint32_t(step.feature_node_index.value_or(0))})hash32(digest,w);
            const auto& after=status==AStatus::InvalidInput?state:step.after;
            for(const auto& node:after.nodes){
                hash32(digest,uint32_t(node.identity));hash32(digest,uint32_t(node.parent_150));
                hash32(digest,node.flags_158?1u:0u);hash32(digest,node.flags_158.value_or(0));
                for(const auto child:node.children_15c)hash32(digest,uint32_t(child));
                for(const auto index:node.attachments_16c)hash32(digest,index);
            }
            hash32(digest,uint32_t(step.writes.size()));
            for(const auto& w:step.writes){hash32(digest,uint32_t(w.identity));hash32(digest,w.offset);hash32(digest,uint32_t(w.value));}
            ++cases;
        }
    std::cout<<"MODEL_ATTACHMENT_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==13824 && digest==0x14c76bc961fda019ull,"whole attachment wrapper agrees with raw instructions and first evidence stops");
}
void test_feature_binding() {
    using AStatus=awl::WorldMapModelAttachmentStatus;
    State state{{{1,0,std::nullopt,{},{}},{2,0,std::nullopt,{},{}}}};
    state.nodes[1].features=awl::WorldMapModelFeatureState{99,{{3,11,std::nullopt},{1,12,77},{1,0,std::nullopt},{2,14,88},{1,15,99},{0,0,66}}};
    const std::vector<awl::WorldMapModelAttachmentBinding> bindings{{2,uint16_t(2),uint16_t(0xbeef)}};
    awl::WorldMapModelAttachmentRequest request{1,2,0x100000063ull,std::nullopt};awl::WorldMapModelAttachmentStep step;
    expect(awl::prepare_world_map_model_attachments(state,request,bindings,&step)==AStatus::Prepared && step.writes.size()==16 &&
        step.after.nodes[0].children_15c[0]==2 && step.after.nodes[1].flags_158==15u && step.feature_node_index==2u,
        "nonnull feature path completes before attachment and never reads unknown C8");
    const auto& f=*step.after.nodes[1].features;
    expect(f.head_50==2u && f.nodes[1].next_14==3u && f.nodes[2].next_14==5u && f.nodes[4].next_14==4u &&
        f.nodes[3].next_14==1u && f.nodes[0].next_14==0u && f.nodes[5].next_14==66u && f.nodes[2].feature_8==*request.feature_c4,
        "stable ascending byte priorities include equal-order nodes in original order; inactive links stay opaque");
    expect(step.writes[0].node_index==2u && step.writes[0].offset==8 && step.writes[1].offset==0x50 &&
        step.writes[1].value==0 && !step.writes[1].node_index,"feature store precedes list-head reset and node insertion writes");
    const auto sorted=step.after;request.feature_c4=77;
    expect(awl::prepare_world_map_model_attachments(sorted,request,bindings,&step)==AStatus::Prepared && step.writes.size()==7 &&
        step.after.nodes[1].features->head_50==2u && step.after.nodes[1].features->nodes[2].feature_8==77 &&
        step.after.nodes[1].features->nodes[2].next_14==5u,"nonnull replacement skips sorting after primary clears its prior child");
    auto missing=bindings;missing[0].resource_attachment_index.reset();
    expect(awl::prepare_world_map_model_attachments(state,request,missing,&step)==AStatus::RequiresAttachmentIndex &&
        same(step.after,state) && step.writes.empty(),"late missing attachment metadata rolls back feature and sorted-list writes");
    missing=bindings;missing[0].first_node_index=uint16_t(9);step.required_model=123;
    expect(awl::prepare_world_map_model_attachments(state,request,missing,&step)==AStatus::InvalidInput && step.required_model==123,
        "out-of-range selected node preserves output before any accepted feature update");
    auto empty=state;empty.nodes[1].features->nodes.clear();
    expect(awl::prepare_world_map_model_attachments(empty,request,bindings,&step)==AStatus::InvalidInput,
        "zero-count selector fallback cannot fabricate a node or dereference an empty table");
}
void feature_binding_digest() {
    using AStatus=awl::WorldMapModelAttachmentStatus;
    const std::array<std::array<uint32_t,2>,4> layouts{{{1,2},{2,2},{0,2},{1,1}}};
    uint64_t digest=14695981039346656037ull;unsigned cases=0;
    for(uint32_t seed=0;seed<512;++seed)for(const auto& layout:layouts)for(uint32_t variant=0;variant<2;++variant) {
        const uint32_t count=seed%9,selected=count?(seed>>3)%count:0;
        State state;std::vector<awl::WorldMapModelAttachmentBinding> bindings;
        for(uint32_t model=1;model<=2;++model) {
            Node node;node.identity=model;node.flags_158.reset();node.features.emplace();node.features->head_50=seed&8?77:0;
            for(uint32_t i=0;i<count;++i)node.features->nodes.push_back({static_cast<uint8_t>((seed>>(i%8))&3),
                (seed>>(i%9))&1?100+i+model*10:0, (seed+i)&1?std::nullopt:std::optional<uint32_t>(90+i)});
            state.nodes.push_back(node);bindings.push_back({model,static_cast<uint16_t>(selected),
                seed&16?std::nullopt:std::optional<uint16_t>(static_cast<uint16_t>(500+model))});
        }
        const awl::WorldMapModelAttachmentRequest request{layout[0],layout[1],999+variant,std::nullopt};
        awl::WorldMapModelAttachmentStep step;const auto status=awl::prepare_world_map_model_attachments(state,request,bindings,&step);
        expect(status==AStatus::Prepared || status==AStatus::RequiresAttachmentIndex || status==AStatus::InvalidInput,
            "complete feature snapshots reach success or the independently mapped node/table/parent boundary");
        for(uint32_t w:{seed,layout[0],layout[1],variant,uint32_t(status),uint32_t(step.required_model),
            uint32_t(step.feature_node_index.has_value()),uint32_t(step.feature_node_index.value_or(0))})hash32(digest,w);
        const auto& after=status==AStatus::InvalidInput?state:step.after;
        for(const auto& node:after.nodes) {
            for(uint32_t w:{uint32_t(node.identity),uint32_t(node.parent_150),node.flags_158?1u:0u,node.flags_158.value_or(0)})hash32(digest,w);
            for(auto child:node.children_15c)hash32(digest,uint32_t(child));for(auto index:node.attachments_16c)hash32(digest,index);
            hash32(digest,node.features->head_50);
            for(const auto& f:node.features->nodes)for(uint32_t w:{uint32_t(f.order_1),uint32_t(f.feature_8),f.next_14?1u:0u,f.next_14.value_or(0)})hash32(digest,w);
        }
        hash32(digest,uint32_t(step.writes.size()));
        for(const auto& w:step.writes)for(uint32_t x:{uint32_t(w.identity),w.offset,uint32_t(w.value),w.node_index.value_or(UINT32_MAX)})hash32(digest,x);
        ++cases;
    }
    std::cout<<"MODEL_FEATURE_BINDING_MATRIX "<<cases<<' '<<std::hex<<digest<<std::dec<<'\n';
    expect(cases==4096 && digest==0xad407db874cb9771ull,"feature list and attachment order match mapped original instructions");
}

bool same_sources(const awl::WorldMapModelAttachmentRequest& a,const awl::WorldMapModelAttachmentRequest& b) {
    return a.primary_model==b.primary_model && a.secondary_model==b.secondary_model &&
        a.feature_c4==b.feature_c4 && a.auxiliary_model_c8==b.auxiliary_model_c8;
}
void test_source_changes() {
    using AStatus=awl::WorldMapModelAttachmentStatus;using Kind=awl::WorldMapModelSourceKind;
    State graph{{{1},{2},{3}}};graph.nodes[1].features=awl::WorldMapModelFeatureState{0,{{1,0,std::nullopt}}};
    const std::vector<awl::WorldMapModelAttachmentBinding> bindings{{2,uint16_t(0),uint16_t(0xbeef)}};
    awl::WorldMapModelAttachmentRequest sources{1,2,std::nullopt,std::nullopt};awl::WorldMapModelSourceStep step;
    expect(awl::prepare_world_map_model_source_change(graph,sources,{Kind::Feature,0x100000063ull},bindings,&step)==AStatus::Prepared &&
        step.after.feature_c4==0x100000063ull && step.after.auxiliary_model_c8==0u && step.source_writes &&
        (*step.source_writes)[0].offset==0xc4 && (*step.source_writes)[0].value==0x100000063ull &&
        (*step.source_writes)[1].offset==0xc8 && (*step.source_writes)[1].value==0 &&
        step.attachments.after.nodes[0].children_15c[0]==2,
        "feature selection establishes both unknown source fields and completes attachments, retaining all 64 key bits");
    graph=step.attachments.after;sources=step.after;
    expect(awl::prepare_world_map_model_source_change(graph,sources,{Kind::Feature,0x100000063ull},bindings,&step)==AStatus::Prepared &&
        step.attachments.writes.size()==7 && same(step.attachments.after,graph),
        "same feature selection still clears and reattaches, without unnecessary list rebuild");
    expect(awl::prepare_world_map_model_source_change(graph,sources,{Kind::AuxiliaryModel,3},bindings,&step)==AStatus::Prepared &&
        step.after.feature_c4==0u && step.after.auxiliary_model_c8==3u && step.attachments.after.nodes[1].children_15c[0]==3 &&
        step.attachments.after.nodes[2].flags_158==12u && step.attachments.after.nodes[1].features->nodes[0].feature_8==0x100000063ull,
        "auxiliary selection clears holder C4 while retaining the earlier node feature and applying auxiliary flags");
    graph=step.attachments.after;sources=step.after;
    expect(awl::prepare_world_map_model_source_change(graph,sources,{Kind::Clear,0},{},&step)==AStatus::Prepared &&
        step.after.feature_c4==0u && step.after.auxiliary_model_c8==0u && step.attachments.writes.size()==4 &&
        step.attachments.after.nodes[0].children_15c[0]==0 && step.attachments.after.nodes[1].children_15c[0]==0 &&
        step.attachments.after.nodes[1].features->nodes[0].feature_8==0x100000063ull &&
        step.attachments.after.nodes[1].attachments_16c[0]==0 && step.attachments.after.nodes[2].flags_158==12u,
        "clear detaches the reached graph without reading unused metadata or clearing retained features/halfwords/flags");
    expect(awl::prepare_world_map_model_source_change(graph,sources,{Kind::Feature,88},{{2,uint16_t(0),std::nullopt}},&step)==
        AStatus::RequiresAttachmentIndex && same_sources(step.after,sources) && !step.source_writes &&
        same(step.attachments.after,graph) && step.attachments.writes.empty(),
        "late attachment evidence stop rolls back both source stores and all earlier graph/feature changes");
    auto unknown=graph;unknown.nodes[1].flags_158.reset();
    expect(awl::prepare_world_map_model_source_change(unknown,sources,{Kind::Clear,0},{},&step)==AStatus::RequiresFlags &&
        step.attachments.required_field==0x158 && !step.source_writes && same_sources(step.after,sources) && same(step.attachments.after,unknown),
        "clear still requires reached child flags and cannot publish either source store alone");
    step.after.primary_model=432;
    expect(awl::prepare_world_map_model_source_change(graph,sources,{Kind::Clear,1},bindings,&step)==AStatus::InvalidInput &&
        awl::prepare_world_map_model_source_change(graph,sources,{static_cast<Kind>(99),0},bindings,&step)==AStatus::InvalidInput &&
        awl::prepare_world_map_model_source_change(graph,sources,{Kind::Clear,0},bindings,nullptr)==AStatus::InvalidInput &&
        awl::prepare_world_map_model_source_change(graph,step.after,{Kind::Clear,0},bindings,&step)==AStatus::InvalidInput &&
        awl::prepare_world_map_model_source_change(step.attachments.after,sources,{Kind::Clear,0},bindings,&step)==AStatus::InvalidInput &&
        step.after.primary_model==432,"invalid operations, nonzero clear keys and output aliases preserve output");
    expect(awl::prepare_world_map_model_source_change({}, {0,0,std::nullopt,std::nullopt}, {Kind::AuxiliaryModel,123}, {}, &step)==AStatus::Prepared &&
        step.after.feature_c4==0u && step.after.auxiliary_model_c8==123u && step.source_writes && step.attachments.writes.empty(),
        "null models still establish source fields without resolving or owning the unused auxiliary key");
}
} // namespace
int main() {test_order();test_failure();test_matrix();test_attachments();test_attachment_matrix();test_feature_binding();feature_binding_digest();test_source_changes();return failures==0?0:1;}
