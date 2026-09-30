#pragma once

#include <algorithm>
#include <cstdint>

namespace awl::detail {

// Shared active-object fields used by the request and selection supplied views.
// FUN_80184920/8018499C sort unsigned endpoints after word-wrap addition.
template<class State>
void reset_transition_clock(State& state, uint32_t clock, bool finish) {
    const uint32_t end = clock + state.clock_duration_34;
    state.clock_begin_30 = std::min(clock, end);
    state.clock_end_2c = std::max(clock, end);
    state.clock_current_28 = finish ? end : clock;
}

template<class State>
bool valid_transition_clock(const State& state) {
    return state.clock_begin_30 <= state.clock_current_28 &&
           state.clock_current_28 <= state.clock_end_2c;
}

// FUN_80184A18 -> FUN_801826E0: parked upper endpoints ignore later clocks;
// active states zero/one become two/three and reset at the computed endpoint.
template<class State>
void update_active_transition(State& state, uint32_t clock) {
    if (state.clock_current_28 != state.clock_end_2c) {
        state.clock_current_28 = std::clamp(clock, state.clock_begin_30, state.clock_end_2c);
    }
    if (state.active_state_20 < 2 && state.clock_current_28 == state.clock_end_2c) {
        state.active_state_20 += 2;
        reset_transition_clock(state, clock, true);
    }
}

} // namespace awl::detail
