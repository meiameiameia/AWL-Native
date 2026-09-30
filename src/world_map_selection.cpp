#include "awl/world_map_selection.h"

#include "world_map_transition_clock.h"

namespace awl {

WorldMapSelectionStatus prepare_world_map_selection_step(
    const WorldMapSelectionState& state, uint32_t pressed, uint32_t repeat,
    uint32_t clock, WorldMapSelectionStep* out) {
    using Status = WorldMapSelectionStatus;
    using Phase = WorldMapSelectionPhase;
    // A proposal may be used as a later supplied snapshot, but cannot also be
    // this call's output: clearing/filling it would violate input preservation.
    if (out == nullptr || &out->after == &state) return Status::InvalidState;
    *out = {};
    if (state.phase != Phase::Idle && state.phase != Phase::Opening &&
        state.phase != Phase::Choosing && state.phase != Phase::Closing) {
        return Status::InvalidState;
    }
    WorldMapSelectionStep plan;
    plan.after = state;
    auto& next = plan.after;
    Status status = Status::Prepared;
    if (state.phase != Phase::Idle) {
        if (!state.has_active_transition) return Status::RequiresActiveTransition;
        if (state.active_state_20 > 3 || !detail::valid_transition_clock(state)) {
            return Status::InvalidState;
        }
        if (state.phase == Phase::Choosing) {
            // Bound the original signed count/index operations to valid rows.
            if (state.choice_count_2c == 0 || state.choice_count_2c > 0x7fffffffu ||
                state.choice_index_28 >= state.choice_count_2c) return Status::InvalidInput;
            if ((repeat & 0x20000u) != 0) {
                plan.feedback_ids[plan.feedback_count++] = 1;
                if (++next.choice_index_28 == next.choice_count_2c) next.choice_index_28 = 0;
            } else if ((repeat & 0x10000u) != 0) {
                plan.feedback_ids[plan.feedback_count++] = 1;
                next.choice_index_28 = next.choice_index_28 == 0
                    ? next.choice_count_2c - 1u : next.choice_index_28 - 1u;
            }
            bool close = false;
            if ((pressed & 0x100u) != 0) {
                plan.feedback_ids[plan.feedback_count++] = 3;
                next.result_4 = next.choice_index_28;
                close = true;
            } else if (state.allow_cancel_30 != 0 && (pressed & 0x200u) != 0) {
                plan.feedback_ids[plan.feedback_count++] = 2;
                next.result_4 = UINT32_MAX;
                close = true;
            }
            if (close) {
                next.active_state_20 = 1;
                detail::reset_transition_clock(next, clock, false);
                next.phase = Phase::Closing;
            }
            if (plan.feedback_count != 0) status = Status::RequiresFeedback;
        } else {
            detail::update_active_transition(next, clock);
            if (state.phase == Phase::Opening && next.active_state_20 == 2) {
                next.phase = Phase::Choosing;
            } else if (state.phase == Phase::Closing && next.active_state_20 == 3) {
                // FUN_801056E4 releases selection +0x10 before choosing idle.
                // This proposal is not accepted while that release is absent.
                next.phase = Phase::Idle;
                status = Status::RequiresResourceRelease;
            }
        }
    }
    *out = plan;
    return status;
}

WorldMapSelectionStatus advance_world_map_selection(
    WorldMapSelectionState* state, uint32_t pressed, uint32_t repeat,
    uint32_t clock, WorldMapSelectionStep* out) {
    if (state != nullptr && out != nullptr && state == &out->after) {
        return WorldMapSelectionStatus::InvalidState;
    }
    if (out != nullptr) *out = {};
    if (state == nullptr) return WorldMapSelectionStatus::InvalidState;
    const auto status = prepare_world_map_selection_step(*state, pressed, repeat, clock, out);
    if (status != WorldMapSelectionStatus::Prepared) return status;
    *state = out->after;
    return WorldMapSelectionStatus::Advanced;
}

} // namespace awl
