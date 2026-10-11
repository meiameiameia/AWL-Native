#include "awl/world_map_player_status.h"
#include <iostream>

namespace {
using G = awl::WorldMapPlayerStatusGlobal;
using S = awl::WorldMapPlayerStatusCode;
int failures = 0;
void expect(bool yes, const char* why) {
    if (!yes) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
void hash(uint64_t& digest, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) { digest ^= (value >> (24 - i * 8)) & 255; digest *= 1099511628211ull; }
}
auto words(const awl::WorldMapPlayerStatusState& s) {
    return std::array<uint32_t,11>{s.word_4,s.word_8,s.word_c,s.word_14,s.word_18,s.word_1c,
        s.word_20,s.word_12c,s.word_150,s.word_154,s.word_160};
}
void set(awl::WorldMapPlayerStatusInputs& input, G key, std::optional<uint32_t> value) {
    input.globals[static_cast<size_t>(key)] = value;
}
awl::WorldMapPlayerStatusInputs observations(uint32_t code) {
    awl::WorldMapPlayerStatusInputs input;
    // Invented coefficients, independently exercised in actual DOL bodies.
    constexpr uint32_t values[]{200000,300,1200000,1800000,75,100,60,100,uint32_t(-3),45,100,100,3};
    for (size_t i = 0; i < input.globals.size(); ++i) input.globals[i] = values[i];
    input.phase_raw = 123456;
    input.rate = awl::WorldMapPlayerStatusRate{code,1000};
    return input;
}
void scalar_checks() {
    awl::WorldMapPlayerStatusState state;
    state.word_4 = 100000; state.word_8 = 200000; state.word_14 = 1000000; state.word_18 = 2000000;
    state.word_12c = 123455; state.word_150 = 77; state.word_154 = 88;
    auto input = observations(3);
    // CC14: 500 - 300*100000/100000 = 200; decay 200*10*20/100/10=40.
    // CD30: 2499975/24999=100. CC84: 999960*75/1350000 +20=75.
    // C7E8: 1000*200*(100+75)/2000000=17.
    auto step = awl::prepare_world_map_player_status(state,3,input);
    expect(step.status == S::Prepared && step.after && step.after->word_14 == 999960 &&
        step.after->word_4 == 100017 && step.after->word_150 == 1000 && step.after->word_154 == 88,
        "ordered first decay and low-code calculation use updated word14 and preserve other rate");
    expect(words(state) == std::array<uint32_t,11>{100000,200000,0,1000000,2000000,0,0,123455,77,88,0},
        "preparation never mutates the supplied state");
    input.rate->descriptor_word_8 = 64;
    step = awl::prepare_world_map_player_status(state,64,input);
    // CCA4=100; CC4C=(1800000-999960)*200/1350000+100=218;
    // elapsed=100; BE3C decrease=(100+218+100)*1000*400/3000000=55.
    expect(step.status == S::Prepared && step.after && step.after->word_4 == 99945 &&
        step.after->word_154 == 1000 && step.after->word_150 == 77,
        "code64 selects the other halfword and unsigned decrease branch");
    state.word_1c = 2000001; state.word_18 = 2;
    step = awl::prepare_world_map_player_status(state,64,input);
    expect(step.after && step.after->word_14 == 2000001,
        "signed clamp takes lower branch first even for supplied reversed limits");
    input.rate->value = 0;
    input.phase_raw.reset();
    expect(awl::prepare_world_map_player_status(state,64,input).status == S::RequiresPhaseRaw,
        "zero rate does not skip reached getter/time dependencies");
}
void dependency_checks() {
    awl::WorldMapPlayerStatusState state;
    awl::WorldMapPlayerStatusInputs input;
    auto check = [&](uint32_t code, S expected, std::optional<G> global = {}) {
        const auto step = awl::prepare_world_map_player_status(state,code,input);
        expect(step.status == expected && step.required_global == global && step.after.has_value() == (expected == S::Prepared),
            "missing or unsupported reached input exposes a diagnostic without a partial state");
    };
    check(3,S::RequiresGlobal,G::Word7ec0);
    set(input,G::Word7ec0,0); check(3,S::RequiresGlobal,G::Word7ebc);
    state.word_4 = 1; check(3,S::RequiresRate); // CC14 greater branch skips coefficient.
    input.rate = awl::WorldMapPlayerStatusRate{64,0}; check(3,S::RequiresRate);
    check(UINT32_MAX,S::UnsupportedRateIndex);
    input.rate->descriptor_word_8 = 3; check(3,S::RequiresPhaseRaw);
    input.phase_raw = 0; check(3,S::RequiresElapsedCurve); // phase=stamp, exact curve is reached.
    input.phase_raw = 1; check(3,S::RequiresGlobal,G::Word7e88); // unsigned time wrap -> constant100.
    set(input,G::Word7e88,0); check(3,S::RequiresGlobal,G::Word7ea0);
    set(input,G::Word7ea0,0); check(3,S::Prepared);
    // Secondary path reaches its two coefficient getters before phase time.
    input={}; state.word_4=1; set(input,G::Word7ec0,0);
    input.rate=awl::WorldMapPlayerStatusRate{64,0};
    check(64,S::RequiresGlobal,G::Word7e9c);
    set(input,G::Word7e9c,0); check(64,S::RequiresGlobal,G::Word7e98);
    set(input,G::Word7e98,0); check(64,S::RequiresGlobal,G::Word7eb0);
    set(input,G::Word7eb0,0); check(64,S::RequiresGlobal,G::Word7eac);
    set(input,G::Word7eac,0); check(64,S::RequiresPhaseRaw);
    input.phase_raw=1; check(64,S::Prepared);
    // Supply just the actual branch's globals; absent alternatives stay unread.
    state.word_20=3; set(input,G::Word7e9c,1000000); set(input,G::Word7e98,{});
    check(64,S::RequiresGlobal,G::Word7e94);
    set(input,G::Word7e94,1000000); check(64,S::RequiresGlobal,G::Word7e90);
    set(input,G::Word7e90,0); check(64,S::Prepared);
    set(input,G::Word7e94,3000000); set(input,G::Word7e90,{});
    check(64,S::RequiresGlobal,G::Word7e8c);
    set(input,G::Word7e8c,0); check(64,S::Prepared);
}
void matrices() {
    uint64_t digest = 14695981039346656037ull;
    size_t count = 0;
    constexpr uint32_t values[]{0,1,0x7fffffff,0x80000000,0xffffffff,200000,400000};
    constexpr uint32_t scalar20[]{0,24999,25000,74999,75000,0xffffffff,0x80000000,18000000};
    constexpr uint32_t deltas[]{1584010,2160000,0xffffffff,4000000};
    constexpr uint32_t modes[]{0,1,2,3,0xffffffff};
    for (uint32_t seed = 0; seed < 1024; ++seed) for (uint32_t code : {0u,3u,25u,63u,64u,67u}) {
        awl::WorldMapPlayerStatusState state;
        state.word_4=values[seed%7]; state.word_8=values[(seed/7)%7]; state.word_c=values[(seed/49)%7];
        state.word_14=values[(seed/3)%7]; state.word_18=values[(seed/21)%7]; state.word_1c=values[(seed/147)%7];
        state.word_20=scalar20[seed%8]; state.word_12c=123456u+deltas[seed%4];
        state.word_150=777; state.word_154=888; state.word_160=modes[seed%5];
        state.byte_2e=uint8_t(seed*17); state.byte_32=uint8_t(seed*29);
        auto input=observations(code);
        if (seed%3==1) input.globals[seed%input.globals.size()]=0x80000001u+seed*100003u;
        input.rate->value=uint16_t(seed*73);
        const auto step=awl::prepare_world_map_player_status(state,code,input);
        expect(step.status==S::Prepared && step.after.has_value(),"complete integer status matrix prepares");
        if (!step.after) return;
        for (uint32_t word : {seed,code}) hash(digest,word);
        for (uint32_t word : words(state)) hash(digest,word);
        for (const auto& word : input.globals) hash(digest,*word);
        for (uint32_t word : {uint32_t(input.rate->value),uint32_t(state.byte_2e),uint32_t(state.byte_32)}) hash(digest,word);
        for (uint32_t word : words(*step.after)) hash(digest,word);
        expect(step.after->byte_2e==state.byte_2e && step.after->byte_32==state.byte_32,
            "conditional byte inputs are preserved");
        ++count;
    }
    expect(count==6144 && digest==0x98FDC6B3D63718B9ull,"6144 cases match complete actual DOL integer status bodies without callback hooks");
    std::cout<<"PLAYER_STATUS_COMPARISONS "<<count<<" digest "<<std::hex<<digest<<std::dec<<'\n';
    digest=14695981039346656037ull; count=0;
    for (uint32_t raw : {0u,1u,123456u,UINT32_MAX})
        for (uint32_t delta : {0u,1u,1583999u,1584000u,1584009u,1584010u,2159999u,2160000u,UINT32_MAX}) {
            auto input=observations(3); input.phase_raw=raw;
            awl::WorldMapPlayerStatusState state; state.word_12c=raw+delta;
            const auto step=awl::prepare_world_map_player_status(state,3,input);
            const bool curve=delta/10<=158400;
            expect(step.status==(curve?S::RequiresElapsedCurve:S::Prepared) && step.after.has_value()!=curve,
                "elapsed threshold, wrapped subtraction and saturation agree with original branch");
            for (uint32_t word : {raw,delta,curve?0x8004ce50u:0u,curve?0u:100u}) hash(digest,word);
            ++count;
        }
    expect(count==36 && digest==0x255F710A3851B92Bull,"36 exact elapsed branch boundaries match actual DOL instructions");
}
} // namespace
int main() {
    scalar_checks(); dependency_checks(); matrices();
    return failures ? 1 : 0;
}
