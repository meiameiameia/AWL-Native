#pragma once

#include <array>
#include <cstdint>

namespace awl {

// Native phases represent the four member-table pairs selected by +0x34.
enum class WorldMapSelectionPhase { Idle, Opening, Choosing, Closing };

struct WorldMapSelectionState {
    WorldMapSelectionPhase phase = WorldMapSelectionPhase::Idle;
    uint32_t result_4 = UINT32_MAX;
    uint32_t choice_index_28 = 0;
    uint32_t choice_count_2c = 0;
    uint8_t allow_cancel_30 = 0;
    // Relative to the separate active object selected by selection +0x1C.
    bool has_active_transition = false;
    uint32_t active_state_20 = 0;
    uint32_t clock_current_28 = 0;
    uint32_t clock_end_2c = 0;
    uint32_t clock_begin_30 = 0;
    uint32_t clock_duration_34 = 0;
};

enum class WorldMapSelectionStatus {
    Prepared,
    Advanced,
    RequiresActiveTransition,
    RequiresFeedback,
    RequiresResourceRelease,
    InvalidState,
    InvalidInput,
};

struct WorldMapSelectionStep {
    // A read-only proposal, not accepted feedback or resource release.
    WorldMapSelectionState after{};
    std::array<uint8_t, 2> feedback_ids{};
    uint8_t feedback_count = 0;
};

// FUN_80105414/80105488/801056E4, using supplied state, newly pressed bits
// (+8) and repeat bits (+0xC) from the first consumer PAD record. Feedback IDs
// retain FUN_8012A52C's ordered r4 values; each call has r5 = 0. No feedback
// is executed and no state changes. RequiresFeedback/RequiresResourceRelease
// expose the candidate only; missing/invalid inputs clear the output. Input
// state and out.after must be distinct; overlap is rejected without mutation.
[[nodiscard]] WorldMapSelectionStatus prepare_world_map_selection_step(
    const WorldMapSelectionState& state, uint32_t pressed, uint32_t repeat,
    uint32_t clock, WorldMapSelectionStep* out);

// Commits only steps with no required feedback/release. Other boundaries
// preserve supplied state and report their proposal. This neither creates a
// selection resource nor presents/accepts a live interaction or manager result.
[[nodiscard]] WorldMapSelectionStatus advance_world_map_selection(
    WorldMapSelectionState* state, uint32_t pressed, uint32_t repeat,
    uint32_t clock, WorldMapSelectionStep* out);

} // namespace awl
