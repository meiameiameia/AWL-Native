#include "awl/world_map_player_status.h"
#include <cfenv>
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
constexpr uint32_t awl::WorldMapPlayerStatusState::* initial_fields[]{
    &awl::WorldMapPlayerStatusState::word_4,&awl::WorldMapPlayerStatusState::word_8,
    &awl::WorldMapPlayerStatusState::word_c,&awl::WorldMapPlayerStatusState::word_14,
    &awl::WorldMapPlayerStatusState::word_18,&awl::WorldMapPlayerStatusState::word_1c,
    &awl::WorldMapPlayerStatusState::word_20,&awl::WorldMapPlayerStatusState::word_24,
    &awl::WorldMapPlayerStatusState::word_28,&awl::WorldMapPlayerStatusState::word_12c,
    &awl::WorldMapPlayerStatusState::word_150,&awl::WorldMapPlayerStatusState::word_154,
    &awl::WorldMapPlayerStatusState::word_160};
auto initial_words(const awl::WorldMapPlayerStatusState& state) {
    std::array<uint32_t,15> result{};
    for(size_t i=0;i<13;++i)result[i]=state.*initial_fields[i];
    result[13]=state.byte_2e;result[14]=state.byte_32;
    return result;
}
void construction_checks() {
    constexpr uint32_t raws[]{0,1,9,10,599,600,0x7fffffff,0x80000000,0xffdf0a7f,0xffdf0a80,0xfffffffe,0xffffffff};
    uint64_t digest=14695981039346656037ull;
    awl::WorldMapPlayerStatusState state;
    for(uint32_t seed=0;seed<128;++seed) {
        const uint32_t raw=seed<24?raws[seed%12]:seed*15485863u;
        for(auto field:initial_fields)state.*field=0xA5000000u+seed;
        state.byte_2e=17;state.byte_32=29;
        expect(awl::construct_world_map_player_status(raw,&state)==S::Prepared,
            "fresh constructor overwrites the whole supported projection");
        hash(digest,seed);hash(digest,raw);
        for(uint32_t word:initial_words(state))hash(digest,word);
    }
    expect(digest==0xCB4928F9C6FE020Full,
        "128 phase/poison cases match the entire original constructor and reached array/time helpers");
    std::cout<<"PLAYER_STATUS_CONSTRUCTOR_COMPARISONS 128 digest "<<std::hex<<digest<<std::dec<<'\n';
    const auto prior=initial_words(state);
    expect(awl::construct_world_map_player_status({},&state)==S::RequiresPhaseRaw && initial_words(state)==prior &&
        awl::construct_world_map_player_status({},nullptr)==S::InvalidInput,
        "missing constructor phase/output rejects without changing a prior projection");

    constexpr uint32_t values[]{0,1,0x7fffffff,0x80000000,0xffffffff,200000,400000};
    digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<512;++seed) {
        for(size_t i=0;i<13;++i)state.*initial_fields[i]=values[(seed/(i%4+1)+i)%7];
        state.byte_2e=uint8_t(seed*17);state.byte_32=uint8_t(seed*29);
        const awl::WorldMapPlayerStatusResetValues reset{values[seed%7],values[(seed/2+2)%7],values[(seed/3+4)%7]};
        const auto before=initial_words(state);awl::WorldMapPlayerStatusState after;
        expect(awl::reset_world_map_player_status(state,reset,&after)==S::Prepared && initial_words(state)==before,
            "reset preserves the source projection");
        hash(digest,seed);for(uint32_t word:before)hash(digest,word);
        for(uint32_t word:{reset.word_4,reset.word_20,reset.word_14})hash(digest,word);
        for(uint32_t word:initial_words(after))hash(digest,word);
        expect(awl::reset_world_map_player_status(state,reset,&state)==S::Prepared && initial_words(state)==initial_words(after),
            "reset aliasing preserves all untouched fields and matches separate output");
    }
    expect(digest==0xCD874A61A02CBA4Aull,
        "512 signed/reversed-bound reset cases match actual B9E4 instructions without hooks");
    expect(awl::reset_world_map_player_status(state,{},nullptr)==S::InvalidInput,"null reset output rejects");
    std::cout<<"PLAYER_STATUS_RESET_COMPARISONS 512 digest "<<std::hex<<digest<<std::dec<<'\n';
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
    state.word_24 = 123; state.word_28 = 456;
    auto input = observations(3);
    // CC14: 500 - 300*100000/100000 = 200; decay 200*10*20/100/10=40.
    // CD30: 2499975/24999=100. CC84: 999960*75/1350000 +20=75.
    // C7E8: 1000*200*(100+75)/2000000=17.
    auto step = awl::prepare_world_map_player_status(state,3,input);
    expect(step.status == S::Prepared && step.after && step.after->word_14 == 999960 &&
        step.after->word_4 == 100017 && step.after->word_150 == 1000 && step.after->word_154 == 88 &&
        step.after->word_24 == 123 && step.after->word_28 == 456,
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
    input.phase_raw = 0; check(3,S::RequiresGlobal,G::Word7e88); // phase=stamp traverses the curve.
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
void matrices(bool curve) {
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
        if (curve) state.word_12c=123456u+(seed*15473u)%1584001u;
        state.word_150=777; state.word_154=888; state.word_160=modes[seed%5];
        state.byte_2e=uint8_t(seed*17); state.byte_32=uint8_t(seed*29);
        auto input=observations(code);
        if (seed%3==1) input.globals[seed%input.globals.size()]=0x80000001u+seed*100003u;
        input.rate->value=uint16_t(seed*73);
        const auto step=awl::prepare_world_map_player_status(state,code,input);
        expect(step.status==S::Prepared && step.after.has_value(),"complete status matrix prepares");
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
    expect(count==6144 && digest==(curve?0xACECFF5A1AACC6E1ull:0x98FDC6B3D63718B9ull),
        "6144 cases match complete actual DOL status bodies without callback hooks");
    std::cout<<"PLAYER_STATUS_COMPARISONS curve "<<curve<<" cases "<<count<<" digest "<<std::hex<<digest<<std::dec<<'\n';
}
void elapsed_checks() {
    uint64_t digest=14695981039346656037ull; size_t count=0;
    uint32_t factor=0;
    for (uint32_t remaining=57600;remaining<=216000;++remaining) {
        const uint32_t delta=(216000-remaining)*10;
        const auto result=awl::prepare_world_map_player_status_elapsed(delta,0,&factor);
        expect(result==S::Prepared && factor>=100 && factor<=150,"every reachable curve input prepares a bounded factor");
        hash(digest,remaining);hash(digest,factor);++count;
    }
    expect(count==158401 && digest==0x1607848EE2BE0044ull,
        "full finite curve domain matches actual DOL sine/reduction/kernel instructions without hooks");
    std::cout<<"PLAYER_STATUS_CURVE_COMPARISONS "<<count<<" digest "<<std::hex<<digest<<std::dec<<'\n';
    digest=14695981039346656037ull;count=0;
    for (uint32_t raw : {0u,1u,123456u,UINT32_MAX})
        for (uint32_t delta : {0u,1u,1583999u,1584000u,1584009u,1584010u,2159999u,2160000u,UINT32_MAX}) {
            const auto result=awl::prepare_world_map_player_status_elapsed(raw+delta,raw,&factor);
            expect(result==S::Prepared,
                "elapsed threshold, wrapped subtraction and saturation agree with original branch");
            for (uint32_t word : {raw,delta,factor}) hash(digest,word);
            ++count;
        }
    expect(count==36 && digest==0x72718389274C4FC3ull,"36 complete elapsed boundaries match actual DOL instructions");
    expect(awl::prepare_world_map_player_status_elapsed(0,0,nullptr)==S::InvalidInput,"null factor output rejects");
    const auto original_rounding=std::fegetround();
    auto input=observations(3);input.phase_raw=0;
    const awl::WorldMapPlayerStatusState state;
    for (int mode : {FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        expect(std::fesetround(mode)==0,"nondefault rounding test setup");
        factor=0x12345678;
        expect(awl::prepare_world_map_player_status_elapsed(0,0,&factor)==S::UnsupportedNumerics && factor==0x12345678,
            "reached curve rejects unsupported rounding without changing output");
        const auto step=awl::prepare_world_map_player_status(state,3,input);
        expect(step.status==S::UnsupportedNumerics && !step.after && std::fegetround()==mode,
            "unsupported curve numerics expose no partial status or rounding-mode change");
        expect(awl::prepare_world_map_player_status_elapsed(1584010,0,&factor)==S::Prepared && factor==100 && std::fegetround()==mode,
            "integer-only elapsed branch does not require default floating-point rounding");
    }
    expect(std::fesetround(original_rounding)==0,"restore original rounding mode");
}
void condition_checks(){
    using C=awl::WorldMapPlayerStatusConditionCode;using CG=awl::WorldMapPlayerStatusConditionGlobal;
    constexpr uint32_t energy[]{0,10000,20000,24999,25000,39999,40000,44999,45000,100000,UINT32_MAX,0x80000000u,0x7fffffffu};
    constexpr uint32_t fatigue[]{0,135000,270000,1350000,UINT32_MAX,0x80000000u,0x7fffffffu};
    constexpr uint8_t flags[]{0,1,2,255};constexpr uint32_t coefficients[]{100,99,101,0,UINT32_MAX,0x80000000u};
    constexpr uint32_t deltas[]{2160000,1584000,1,0,576001,UINT32_MAX,1368000};uint64_t digest=14695981039346656037ull;uint32_t routes=0;
    for(uint32_t seed=0;seed<4096;++seed){const uint32_t phase=100000+seed%7,action=seed%2*7;awl::WorldMapPlayerStatusState state;
        state.word_4=energy[seed%13];state.word_14=fatigue[seed/13%7];state.word_12c=phase+deltas[seed/13%7];state.word_134=phase+(seed%3?576000u:0u);state.word_13c=phase+(seed%2?1u:UINT32_MAX);
        state.byte_158=flags[seed/7%4];state.byte_159=flags[seed/14%4];state.byte_15a=flags[seed/21%4];state.byte_15b=flags[seed/28%4];state.byte_15c=flags[seed/35%4];
        awl::WorldMapPlayerStatusConditionFactors factors;for(size_t i=0;i<7;++i)factors.globals[i]=coefficients[seed/(13*(i+1))%6];
        const auto r=awl::prepare_world_map_player_status_conditions(state,factors,phase,action);
        expect((r.status==C::Prepared || r.status==C::TransitionRequired) && r.after,"complete condition observations yield an atomic proposal or an explicit transition");if(!r.after)continue;
        const auto fields=[](const auto& st){return std::array<uint32_t,10>{st.word_4,st.word_14,st.word_12c,*st.word_134,*st.word_13c,*st.byte_158,*st.byte_159,*st.byte_15a,*st.byte_15b,*st.byte_15c};};
        for(uint32_t v:{seed,phase,action})hash(digest,v);for(uint32_t v:fields(state))hash(digest,v);for(const auto& v:factors.globals)hash(digest,*v);
        hash(digest,r.gate?*r.gate:UINT32_MAX);hash(digest,r.requested_state.value_or(UINT32_MAX));for(uint32_t v:fields(*r.after))hash(digest,v);
        routes|=1u<<(r.gate?*r.gate:7);
    }
    expect(digest==0x00a639494f8afc7full && routes==255,"4096 original seven-condition priority/latch/deadline/wrapping cases match, including every selected gate and no-event route");
    std::cout<<"STATE1_STATUS_CONDITIONS 4096 "<<std::hex<<digest<<std::dec<<'\n';
    awl::WorldMapPlayerStatusState fresh;expect(awl::construct_world_map_player_status(UINT32_MAX,&fresh)==S::Prepared && fresh.word_134==575999u && fresh.word_13c==UINT32_MAX && fresh.byte_14c==uint8_t{0} && fresh.byte_158==uint8_t{0} && fresh.byte_159==uint8_t{0} && fresh.byte_15a==uint8_t{0} && fresh.byte_15b==uint8_t{0} && fresh.byte_15c==uint8_t{0},"fresh deadline and condition latch extension follows original constructor writes with unsigned wrap");
    awl::WorldMapPlayerStatusState observed;observed.word_12c=1000;awl::WorldMapPlayerStatusConditionFactors factors;
    expect(awl::prepare_world_map_player_status_conditions(observed,factors,{},0).status==C::RequiresPhaseRaw,"first condition requires phase before every status field/global");
    auto r=awl::prepare_world_map_player_status_conditions(observed,factors,1000,7);expect(r.status==C::TransitionRequired && r.gate==uint8_t{0} && r.requested_state==0x1fu,"expired first condition skips all unknown later fields and globals");
    r=awl::prepare_world_map_player_status_conditions(observed,factors,999,0);expect(r.status==C::RequiresField && r.required_field==0x134u && !r.after,"unreached constructor extension cannot be silently filled");
    observed.word_134=1000;r=awl::prepare_world_map_player_status_conditions(observed,factors,999,0);expect(r.status==C::RequiresGlobal && r.required_global==CG::Word7ee0 && !r.after,"clock-gated second condition skips unknown latch and requires only next reached global");
    factors.globals[0]=100;observed.word_4=100000;r=awl::prepare_world_map_player_status_conditions(observed,factors,999,0);expect(r.status==C::RequiresField && r.required_field==0x158u && !r.after,"reached missing first hysteresis latch preserves the prior owner");
    observed.byte_158=uint8_t{0};r=awl::prepare_world_map_player_status_conditions(observed,factors,999,0);expect(r.status==C::RequiresGlobal && r.required_global==CG::Word7edc && !r.after,"zero latch selects entry coefficient before reset coefficient");
    observed.byte_158=uint8_t{255};r=awl::prepare_world_map_player_status_conditions(observed,factors,999,0);expect(r.status==C::RequiresGlobal && r.required_global==CG::Word7ed8,"every nonzero latch selects the separate reset coefficient");
}
} // namespace
int main() {
    construction_checks(); condition_checks(); scalar_checks(); dependency_checks(); matrices(false); matrices(true); elapsed_checks();
    return failures ? 1 : 0;
}
