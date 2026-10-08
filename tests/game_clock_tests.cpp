#include "awl/game_clock.h"
#include <cfenv>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool yes, const char* why) {
    if (!yes) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
bool equal(const awl::GameClockState& a, const awl::GameClockState& b) {
    return a.raw_time == b.raw_time && a.loop_count == b.loop_count && a.retrace_interval == b.retrace_interval;
}
void hash(uint64_t& digest, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) { digest ^= (word >> (24 - i * 8)) & 255; digest *= 1099511628211ull; }
}
} // namespace
int main() {
    using State = awl::GameClockState;
    awl::GameClock clock;
    expect(equal(clock.state(), {0,0,1}), "fresh process fields match CRT zeroes and initialized interval one");
    expect(clock.advance() && equal(clock.state(), {16,1,1}), "one retrace truncates to sixteen raw units");
    clock.set_retrace_interval(2);
    expect(equal(clock.state(), {16,1,2}) && clock.advance() && equal(clock.state(), {49,2,2}), "interval store preserves both counters before next rounded advance");
    State out{7,8,9};
    expect(awl::prepare_game_clock_advance({0,UINT32_MAX,2}, &out) && equal(out,{33,0,2}), "raw time truncates independently of wrapping loop count");
    expect(awl::prepare_game_clock_advance({0x1000001,3,0}, &out) && equal(out,{0x1000000,4,0}), "zero interval still rounds odd raw values above binary32 integer precision");
    expect(awl::prepare_game_clock_advance({0xffffff80,3,1}, &out) && equal(out,{UINT32_MAX,4,1}), "rounding at unsigned ceiling saturates without an invalid integer cast");
    expect(awl::prepare_game_clock_advance({UINT32_MAX,3,0}, &out) && equal(out,{UINT32_MAX,4,0}), "rounded maximum saturates even at zero interval");
    expect(!awl::prepare_game_clock_advance(out,nullptr), "null output rejects");
    State alias{0,UINT32_MAX,2};
    expect(awl::prepare_game_clock_advance(alias,&alias) && equal(alias,{33,0,2}), "whole state alias preserves input reads before publication");
    const auto prior = out; const auto owner_prior = clock.state(); const int rounding = std::fegetround();
    for (int mode : {FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        expect(std::fesetround(mode) == 0, "test can select a nondefault rounding mode");
        expect(!awl::prepare_game_clock_advance(prior,&out) && equal(out,prior) && !clock.advance() && equal(clock.state(),owner_prior), "unsupported rounding mode preserves proposed and owned clock state");
    }
    expect(std::fesetround(rounding) == 0 && rounding == FE_TONEAREST, "test restores the original default rounding mode");

    // Invented state words exercise the actual mapped instruction digest.
    // No DOL bytes or OS-tick payload is embedded here.
    uint64_t digest = 14695981039346656037ull; size_t cases = 0;
    const auto compare = [&](const State& state) {
        State after;
        expect(awl::prepare_game_clock_advance(state,&after), "default-rounding supplied state advances");
        for (uint32_t word : {state.raw_time,state.loop_count,state.retrace_interval,after.raw_time,after.loop_count,after.retrace_interval}) hash(digest,word);
        ++cases;
    };
    for (uint32_t raw : {0u,1u,16u,33u,99u,0x7fffffu,0xffffffu,0x1000000u,0x1000001u,0x1000002u,0x1ffffffu,0x2000000u,0x7fffff00u,0x7fffffffu,0x80000000u,0x80000001u,0xffffff00u,0xffffff7fu,0xffffff80u,0xfffffffeu,UINT32_MAX})
        for (uint32_t interval : {0u,1u,2u,3u,15u,60u,0xffffffu,0x1000001u,0x7fffffffu,0x80000000u,UINT32_MAX})
            compare({raw,raw*17u+interval,interval});
    uint32_t seed = 0x12345678;
    for (uint32_t i = 0; i < 2048; ++i) {
        seed = seed * 1664525u + 1013904223u; const uint32_t raw = seed;
        seed = seed * 1664525u + 1013904223u;
        compare({raw,i*0x01010101u,seed});
    }
    expect(cases == 2279 && digest == 0x5DC172A130F64CA2ull, "binary32 operations and unsigned conversion match bounded 8017A304/80235DA4 instructions");
    clock = {}; digest = 14695981039346656037ull;
    for (uint32_t i = 0; i < 900; ++i) {
        clock.set_retrace_interval(i < 300 ? 1 : i < 600 ? 2 : 0);
        expect(clock.advance(), "shared clock advances through interval changes");
        for (uint32_t word : {i,clock.state().raw_time,clock.state().loop_count,clock.state().retrace_interval}) hash(digest,word);
    }
    expect(digest == 0xD30BF1B33B3C1D45ull && equal(clock.state(),{14700,900,0}), "nine hundred owned updates match mapped interval sequence");
    std::cout << "GAME_CLOCK_COMPARISONS " << cases << " / 900 owned updates; OS-time tail and live game-loop activation pending\n";
    return failures ? 1 : 0;
}
