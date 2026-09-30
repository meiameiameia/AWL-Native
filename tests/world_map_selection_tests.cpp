#include "awl/world_map_selection.h"
#include "awl/world_map_request_transition.h"

#include <cstdio>

namespace {

int failures = 0;
using State = awl::WorldMapSelectionState;
using Phase = awl::WorldMapSelectionPhase;
using Status = awl::WorldMapSelectionStatus;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool same(const State& a, const State& b) {
    return a.phase == b.phase && a.result_4 == b.result_4 &&
        a.choice_index_28 == b.choice_index_28 && a.choice_count_2c == b.choice_count_2c &&
        a.allow_cancel_30 == b.allow_cancel_30 && a.has_active_transition == b.has_active_transition &&
        a.active_state_20 == b.active_state_20 && a.clock_current_28 == b.clock_current_28 &&
        a.clock_begin_30 == b.clock_begin_30 && a.clock_end_2c == b.clock_end_2c &&
        a.clock_duration_34 == b.clock_duration_34;
}

State choosing(uint32_t index = 1) {
    State state;
    state.phase = Phase::Choosing;
    state.result_4 = 17;
    state.choice_count_2c = 3;
    state.choice_index_28 = index;
    state.allow_cancel_30 = 1;
    state.has_active_transition = true;
    state.active_state_20 = 2;
    state.clock_current_28 = 24;
    state.clock_begin_30 = 22;
    state.clock_end_2c = 24;
    state.clock_duration_34 = 2;
    return state;
}

void test_selection_workflow() {
    State state = choosing();
    state.phase = Phase::Opening;
    state.active_state_20 = 0;
    state.clock_current_28 = 20;
    state.clock_begin_30 = 20;
    state.clock_end_2c = 22;
    awl::WorldMapSelectionStep step;
    expect(awl::advance_world_map_selection(&state, 0x100, 0x20000, 21, &step) == Status::Advanced &&
               state.phase == Phase::Opening && state.clock_current_28 == 21 &&
               state.choice_index_28 == 1 && state.result_4 == 17 && step.feedback_count == 0,
           "opening ignores choosing input and waits for its own endpoint");
    expect(awl::advance_world_map_selection(&state, 0x100, 0x20000, 22, &step) == Status::Advanced &&
               state.phase == Phase::Choosing && state.active_state_20 == 2 &&
               state.clock_begin_30 == 22 && state.clock_end_2c == 24 && state.clock_current_28 == 24 &&
               state.choice_index_28 == 1 && state.result_4 == 17,
           "endpoint selects choosing without processing input in the same update");
    const auto before_input = state;
    expect(awl::advance_world_map_selection(&state, 0x300, 0x30000, 30, &step) == Status::RequiresFeedback &&
               same(state, before_input) && step.feedback_count == 2 &&
               step.feedback_ids[0] == 1 && step.feedback_ids[1] == 3 &&
               step.after.choice_index_28 == 2 && step.after.result_4 == 2 &&
               step.after.phase == Phase::Closing && step.after.active_state_20 == 1 &&
               step.after.clock_current_28 == 30 && step.after.clock_begin_30 == 30 && step.after.clock_end_2c == 32,
           "navigation precedes confirmation, both priority rules hold, and feedback stays unconsumed");
    // Explicitly supplied post-feedback snapshot for testing the next branch.
    // This test does not implement/accept the original feedback calls.
    State closing = step.after;
    expect(awl::advance_world_map_selection(&closing, 0x100, 0x20000, 31, &step) == Status::Advanced &&
               closing.phase == Phase::Closing && closing.clock_current_28 == 31 &&
               closing.result_4 == 2 && closing.choice_index_28 == 2 && step.feedback_count == 0,
           "supplied closing snapshot preserves the choice and ignores input");
    const auto before_release = closing;
    expect(awl::advance_world_map_selection(&closing, 0, 0, 32, &step) == Status::RequiresResourceRelease &&
               same(closing, before_release) && step.after.phase == Phase::Idle &&
               step.after.result_4 == 2 && step.after.active_state_20 == 3 &&
               step.after.clock_begin_30 == 32 && step.after.clock_current_28 == 34,
           "selection cannot become idle before its separate resource release");
    expect(awl::advance_world_map_selection(&closing, 0, 0, 33, &step) == Status::RequiresResourceRelease &&
               same(closing, before_release), "retry at unsupported release is atomic");
    awl::WorldMapRequestTransitionState manager;
    manager.manager_state_48 = 3;
    manager.manager_result_1c0 = closing.result_4;
    manager.has_active_transition = true;
    manager.active_state_20 = 2;
    const auto original_manager = manager;
    expect(awl::advance_world_map_request_transition(&manager, 33) ==
               awl::WorldMapRequestTransitionStatus::RequiresPresentation &&
               manager.manager_state_48 == original_manager.manager_state_48 &&
               manager.manager_result_1c0 == 2,
           "supplied selection result alone cannot finish the parent request presentation");
}

void test_selection_input_order() {
    struct Case {
        uint32_t index, pressed, repeat;
        uint8_t cancel;
        uint32_t expected_index, expected_result;
        Phase phase;
        uint8_t count, first, second;
    };
    // Invented rows with independent expectations from the verified branches.
    const Case cases[] = {
        {1, 0, 0, 1, 1, 17, Phase::Choosing, 0, 0, 0},
        {2, 0, 0x20000, 1, 0, 17, Phase::Choosing, 1, 1, 0},
        {0, 0, 0x10000, 1, 2, 17, Phase::Choosing, 1, 1, 0},
        {1, 0, 0x30000, 1, 2, 17, Phase::Choosing, 1, 1, 0},
        {1, 0x100, 0, 1, 1, 1, Phase::Closing, 1, 3, 0},
        {1, 0x200, 0, 1, 1, UINT32_MAX, Phase::Closing, 1, 2, 0},
        {1, 0x300, 0, 1, 1, 1, Phase::Closing, 1, 3, 0},
        {1, 0x200, 0, 0, 1, 17, Phase::Choosing, 0, 0, 0},
        {1, 0x300, 0, 0, 1, 1, Phase::Closing, 1, 3, 0},
        {0, 0x100, 0x10000, 1, 2, 2, Phase::Closing, 2, 1, 3},
        {2, 0x200, 0x20000, 1, 0, UINT32_MAX, Phase::Closing, 2, 1, 2},
        {2, 0x200, 0x20000, 0, 0, 17, Phase::Choosing, 1, 1, 0},
        {1, 0x30000, 0x300, 1, 1, 17, Phase::Choosing, 0, 0, 0},
    };
    for (const auto& test : cases) {
        auto state = choosing(test.index);
        state.allow_cancel_30 = test.cancel;
        const auto before = state;
        awl::WorldMapSelectionStep step;
        expect(awl::prepare_world_map_selection_step(state, test.pressed, test.repeat, 40, &step) ==
                   (test.count == 0 ? Status::Prepared : Status::RequiresFeedback) &&
                   same(state, before) && step.after.choice_index_28 == test.expected_index &&
                   step.after.result_4 == test.expected_result && step.after.phase == test.phase &&
                   step.feedback_count == test.count && step.feedback_ids[0] == test.first &&
                   step.feedback_ids[1] == test.second,
               "selection proposal preserves navigation, confirmation, cancellation, and feedback order");
    }
    auto one = choosing(0);
    one.choice_count_2c = 1;
    awl::WorldMapSelectionStep step;
    expect(awl::prepare_world_map_selection_step(one, 0x100, 0x20000, 40, &step) == Status::RequiresFeedback &&
               step.after.choice_index_28 == 0 && step.after.result_4 == 0 && step.feedback_count == 2,
           "single choice still produces navigation feedback before confirmation");
    auto large = choosing(0);
    large.choice_count_2c = 0x7fffffff;
    large.allow_cancel_30 = 255;
    expect(awl::prepare_world_map_selection_step(large, 0x200, 0x10000, 40, &step) == Status::RequiresFeedback &&
               step.after.choice_index_28 == 0x7ffffffe && step.after.result_4 == UINT32_MAX,
           "maximum positive signed count and nonzero cancel byte are supported");
    large.choice_index_28 = 0x7ffffffe;
    expect(awl::prepare_world_map_selection_step(large, 0x100, 0x20000, 40, &step) == Status::RequiresFeedback &&
               step.after.choice_index_28 == 0 && step.after.result_4 == 0,
           "last index wraps without entering signed overflow");
}

void test_selection_boundaries() {
    State state;
    awl::WorldMapSelectionStep step;
    const auto idle = state;
    expect(awl::advance_world_map_selection(&state, UINT32_MAX, UINT32_MAX, 1, &step) == Status::Advanced &&
               same(state, idle) && step.feedback_count == 0, "idle ignores input and needs no active resource");
    state = choosing();
    const auto neutral = state;
    expect(awl::advance_world_map_selection(&state, 0, 0, 1000, &step) == Status::Advanced &&
               same(state, neutral), "choosing does not update the parked active clock on neutral input");
    state.has_active_transition = false;
    const auto missing = state;
    expect(awl::advance_world_map_selection(&state, 0x100, 0, 10, &step) == Status::RequiresActiveTransition &&
               same(state, missing) && step.feedback_count == 0, "missing active object stops before feedback planning");
    for (uint32_t count : {0u, 0x80000000u, UINT32_MAX}) {
        state = choosing();
        state.choice_count_2c = count;
        const auto before = state;
        expect(awl::advance_world_map_selection(&state, 0x100, 0x20000, 10, &step) == Status::InvalidInput &&
                   same(state, before) && step.feedback_count == 0, "invalid signed choice count cannot be consumed");
    }
    state = choosing(3);
    const auto bad_index = state;
    expect(awl::advance_world_map_selection(&state, 0x100, 0, 10, &step) == Status::InvalidInput &&
               same(state, bad_index), "out-of-range row is rejected");
    state = choosing();
    state.clock_begin_30 = 25;
    const auto bad_clock = state;
    expect(awl::advance_world_map_selection(&state, 0, 0, 10, &step) == Status::InvalidState &&
               same(state, bad_clock), "malformed active counter is rejected atomically");
    state = choosing();
    state.active_state_20 = 4;
    expect(awl::prepare_world_map_selection_step(state, 0, 0, 10, &step) == Status::InvalidState,
           "unsupported active state is rejected");
    state = choosing();
    state.phase = static_cast<Phase>(99);
    const auto invalid = state;
    expect(awl::advance_world_map_selection(&state, 0, 0, 10, &step) == Status::InvalidState &&
               same(state, invalid) && step.after.phase == Phase::Idle && step.feedback_count == 0,
           "unsupported member-table phase clears stale proposal and preserves supplied state");
    state = choosing();
    expect(awl::advance_world_map_selection(nullptr, 0, 0, 10, &step) == Status::InvalidState &&
               awl::advance_world_map_selection(&state, 0, 0, 10, nullptr) == Status::InvalidState &&
               awl::prepare_world_map_selection_step(state, 0, 0, 10, nullptr) == Status::InvalidState,
           "missing caller state and output are rejected");
    state = choosing();
    step.after = state;
    expect(awl::prepare_world_map_selection_step(step.after, 0x100, 0, 10, &step) == Status::InvalidState &&
               same(step.after, state) &&
               awl::advance_world_map_selection(&step.after, 0x100, 0, 10, &step) == Status::InvalidState &&
               same(step.after, state), "overlapping input/proposal cannot destroy supplied state");
    state = choosing();
    state.phase = Phase::Opening;
    state.active_state_20 = 0;
    state.clock_begin_30 = state.clock_current_28 = state.clock_end_2c = 10;
    state.clock_duration_34 = 0;
    expect(awl::advance_world_map_selection(&state, 0, 0, 10, &step) == Status::Advanced &&
               state.phase == Phase::Choosing && state.active_state_20 == 2,
           "zero duration opening reaches choosing on its supplied update");
    state = choosing();
    state.clock_duration_34 = 4;
    expect(awl::prepare_world_map_selection_step(state, 0x100, 0, 0xfffffffe, &step) == Status::RequiresFeedback &&
               step.after.phase == Phase::Closing && step.after.clock_begin_30 == 2 &&
               step.after.clock_end_2c == 0xfffffffe && step.after.clock_current_28 == 0xfffffffe,
           "confirmation preparation preserves unsigned closing-clock wrap");
    auto wrapped = step.after;
    const auto before_wrap_release = wrapped;
    expect(awl::advance_world_map_selection(&wrapped, 0, 0, UINT32_MAX, &step) == Status::RequiresResourceRelease &&
               same(wrapped, before_wrap_release) && step.after.clock_begin_30 == 3 &&
               step.after.clock_end_2c == UINT32_MAX && step.after.clock_current_28 == 3,
           "wrapped closing endpoint remains blocked on resource release");
}

} // namespace

int main() {
    test_selection_workflow();
    test_selection_input_order();
    test_selection_boundaries();
    if (failures != 0) return 1;
    std::printf("World-map selection tests passed.\n");
    return 0;
}
