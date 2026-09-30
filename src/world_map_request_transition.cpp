#include "awl/world_map_request_transition.h"

#include "world_map_transition_clock.h"

namespace awl {

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
        detail::reset_transition_clock(next, clock, false);
        next.manager_state_48 = 2;
    } else {
        if (next.active_state_20 > 3 || !detail::valid_transition_clock(next)) return Status::InvalidState;
        // FUN_801826E0 updates the active object in states 0/1 only, then
        // FUN_80102458/801025E8 examine the resulting active state.
        detail::update_active_transition(next, clock);
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
    detail::reset_transition_clock(next, clock, false);
    *state = next;
    return Status::Advanced;
}

} // namespace awl
