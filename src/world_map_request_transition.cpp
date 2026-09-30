#include "awl/world_map_request_transition.h"

#include <algorithm>

namespace awl {
namespace {

// FUN_80184920/8018499C: unsigned word addition wraps before the endpoints
// are ordered. The latter parks current at the computed endpoint, not now.
void reset_clock(WorldMapRequestTransitionState& state, uint32_t clock, bool finish) {
    const uint32_t end = clock + state.clock_duration_34;
    state.clock_begin_30 = std::min(clock, end);
    state.clock_end_2c = std::max(clock, end);
    state.clock_current_28 = finish ? end : clock;
}

// FUN_80184A18: a parked endpoint ignores later clock values.
void update_clock(WorldMapRequestTransitionState& state, uint32_t clock) {
    if (state.clock_current_28 != state.clock_end_2c) {
        state.clock_current_28 = std::clamp(clock, state.clock_begin_30, state.clock_end_2c);
    }
}

bool valid_clock(const WorldMapRequestTransitionState& state) {
    return state.clock_begin_30 <= state.clock_current_28 &&
           state.clock_current_28 <= state.clock_end_2c;
}

} // namespace

WorldMapRequestTransitionStatus advance_world_map_request_transition(
    WorldMapRequestTransitionState* state, uint32_t clock) {
    using Status = WorldMapRequestTransitionStatus;
    if (state == nullptr || state->manager_state_48 > 4) return Status::InvalidState;
    if (state->manager_state_48 == 0) return Status::Idle;
    if (state->manager_state_48 == 3) return Status::RequiresPresentation;
    if (!state->has_active_transition) return Status::RequiresActiveTransition;
    auto next = *state;
    if (next.manager_state_48 == 1) {
        next.active_state_20 = 0;
        reset_clock(next, clock, false);
        next.manager_state_48 = 2;
    } else {
        if (next.active_state_20 > 3 || !valid_clock(next)) return Status::InvalidState;
        update_clock(next, clock);
        // FUN_801826E0 updates the active object in states 0/1 only, then
        // FUN_80102458/801025E8 examine the resulting active state.
        if (next.active_state_20 < 2 && next.clock_current_28 == next.clock_end_2c) {
            next.active_state_20 += 2;
            reset_clock(next, clock, true);
        }
        if (next.manager_state_48 == 2 && next.active_state_20 == 2) {
            next.manager_state_48 = 3;
        } else if (next.manager_state_48 == 4 && next.active_state_20 == 3) {
            // The DOL clears +0x48, releases manager +0xCC/+0x10, then clears
            // +0x60. Native pauses atomically before those unsupported effects.
            return Status::RequiresResourceRelease;
        }
    }
    *state = next;
    return Status::Advanced;
}

WorldMapRequestTransitionStatus close_world_map_request_transition(
    WorldMapRequestTransitionState* state, uint32_t clock) {
    using Status = WorldMapRequestTransitionStatus;
    if (state == nullptr || state->manager_state_48 > 4) return Status::InvalidState;
    if (state->manager_state_48 == 0) return Status::Idle;
    if (state->manager_state_48 == 4) return Status::Unchanged;
    if (!state->has_active_transition) return Status::RequiresActiveTransition;
    auto next = *state;
    next.manager_state_48 = 4;
    next.active_state_20 = 1;
    reset_clock(next, clock, false);
    *state = next;
    return Status::Advanced;
}

} // namespace awl
