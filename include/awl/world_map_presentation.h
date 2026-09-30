#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace awl {

// Six initialized member records at 802BD0C8, sourced from 802BD080.
enum class WorldMapPresentationPhase { Reading, Scrolling, InputWait, Selection, Hold, Completion };

struct WorldMapPresentationState {
    WorldMapPresentationPhase phase = WorldMapPresentationPhase::Reading;
    std::optional<size_t> display_offset_1c;
    std::optional<size_t> resume_offset_20;
    uint32_t revealed_units_24 = 0;
    uint32_t window_units_28 = 0;
    uint32_t deadline_2c = 0;
    uint32_t interval_30 = 26;
    uint8_t completion_gate_34 = 0;
    uint8_t pacing_stop_35 = 0;
    uint8_t fast_36 = 0;
    uint8_t lock_fast_37 = 0;
    float scroll_48 = 0;
    uint32_t manager_input_mode_58 = 0;
    uint32_t manager_resource_60 = UINT32_MAX;
};

enum class WorldMapPresentationStatus {
    Prepared, Advanced, RequiresFeedback, RequiresActorEffects,
    RequiresSelection, RequiresControlHandler, InvalidInput, InvalidState,
};

struct WorldMapPresentationStep {
    // Supported prefix stores only. At an effect stop this is NOT a snapshot
    // from which execution may resume; no effect acknowledgement exists yet.
    WorldMapPresentationState after;
    uint32_t visited_tokens = 0;
    std::optional<size_t> blocked_token_offset;
    std::optional<size_t> next_token_offset;
    std::optional<uint16_t> feedback_id;
    // Requires FUN_8018CD6C query and conditional FUN_8018B25C before ID 6.
    bool feedback_channel_check = false;
    // Ordered actor/resource branch parameters, not an executed backend.
    std::optional<uint32_t> actor_mode;
    std::optional<WorldMapPresentationPhase> phase_after_effects;
    bool reports_complete = false;
};

// Supplied-state update portions of FUN_80102BCC/80102C68/80102EA0 and
// runtime handlers for generic tokens, tag 2/8x, zero, tag 0x30 and tag 0x31.
// Uses raw FUN_8017A2FC clock words, and first PAD pressed/repeat words.
// A full bounded structural stream is required; supplied offsets must be
// token boundaries before or at its first zero. Unsupported controls stop.
// Feedback and actor/resource effects are reported in order, never executed.
// Selection update/activation, drawing, live ownership, and parent request
// completion/release are absent. Failure preserves output; aliasing state
// and out.after is rejected. Effect stops produce an unaccepted prefix plan.
[[nodiscard]] WorldMapPresentationStatus prepare_world_map_presentation_step(
    const uint8_t* data, size_t size, const WorldMapPresentationState& state,
    uint32_t clock, uint32_t pressed, uint32_t repeat, WorldMapPresentationStep* out);

// Accepts only Prepared steps into the supplied state; effect stops preserve
// it for repeatable retries. This does not update a live game/request manager.
[[nodiscard]] WorldMapPresentationStatus advance_world_map_presentation(
    const uint8_t* data, size_t size, WorldMapPresentationState* state,
    uint32_t clock, uint32_t pressed, uint32_t repeat, WorldMapPresentationStep* out);

} // namespace awl
