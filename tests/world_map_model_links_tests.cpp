#include "awl/world_map_model_links.h"

#include <iostream>

namespace {
using Node = awl::WorldMapModelLinkNode;
using State = awl::WorldMapModelLinkState;
using Step = awl::WorldMapModelLinkStep;
using Status = awl::WorldMapModelLinkStatus;
int failures = 0;
void expect(bool yes, const char* message) { if (!yes) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
bool same(const State& a, const State& b) {
    if (a.nodes.size() != b.nodes.size()) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const auto& x = a.nodes[i]; const auto& y = b.nodes[i];
        if (x.identity != y.identity || x.parent_150 != y.parent_150 || x.flags_158 != y.flags_158 ||
            x.children_15c != y.children_15c || x.attachments_16c != y.attachments_16c) return false;
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
            hash64(digest,node.identity);hash64(digest,node.parent_150);hash32(digest,node.flags_158);
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
} // namespace
int main() {test_order();test_failure();test_matrix();return failures==0?0:1;}
