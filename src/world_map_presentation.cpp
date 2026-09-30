#include "awl/world_map_presentation.h"

#include "awl/world_map_message_stream.h"
#include "awl/world_map_presentation_data.h"

#include <cmath>
#include <limits>

namespace awl {
namespace {
using Status = WorldMapPresentationStatus;
using Phase = WorldMapPresentationPhase;

bool valid_offset(const WorldMapMessageStream& stream, std::optional<size_t> offset) {
    if (!offset) return true;
    for (const auto& token : stream.tokens) if (token.offset == *offset) return true;
    return false;
}

void reset_clock(WorldMapPresentationState& state, uint32_t clock) {
    state.deadline_2c = clock - (state.interval_30 + 1u); // FUN_80103748.
}

Status change_phase(WorldMapPresentationStep& step, Phase phase, uint32_t actor_mode) {
    if (step.after.manager_resource_60 != UINT32_MAX) {
        step.actor_mode = actor_mode;
        step.phase_after_effects = phase;
        return Status::RequiresActorEffects;
    }
    step.after.phase = phase;
    return Status::Prepared;
}

Status resume_data(const uint8_t* data, size_t size, WorldMapPresentationStep& step, uint32_t clock) {
    auto& next = step.after;
    if (next.resume_offset_20) {
        WorldMapPresentationResume resume;
        if (resume_world_map_presentation_data(data, size, *next.resume_offset_20, &resume) !=
            WorldMapPresentationDataStatus::Prepared) return Status::InvalidInput;
        next.display_offset_1c = resume.start_offset;
        next.revealed_units_24 = 0;
        next.window_units_28 = resume.window.units;
        reset_clock(next, clock);
    }
    return change_phase(step, Phase::Reading, 5);
}

bool generic_slot(uint8_t slot) {
    // All inherited runtime callback bodies verified against 800FF9B4.
    // Structural slots precede forwarded callback slots by four bytes.
    return slot == 0x10 || (slot >= 0x18 && slot <= 0x74) ||
        slot == 0x94 || slot == 0x98 || slot == 0xc0;
}

Status read_tokens(const uint8_t* data, size_t size, const WorldMapMessageStream& stream,
                   WorldMapPresentationStep& step, uint32_t clock) {
    auto& next = step.after;
    if (!next.resume_offset_20) return Status::Prepared;
    const size_t start = *next.resume_offset_20;
    for (const auto& token : stream.tokens) {
        if (token.offset < start) continue;
        ++step.visited_tokens;
        const size_t following = token.offset + token.byte_count;
        if (token.tag == 0 || token.tag == 0x30) {
            // These two handlers bypass generic pacing. Pointer store precedes
            // their actor calls; the member store follows those effects.
            next.resume_offset_20 = token.tag == 0 ? std::nullopt : std::optional<size_t>{following};
            const auto phase = token.tag == 0 ? Phase::Completion : Phase::InputWait;
            const auto status = change_phase(step, phase, 0);
            if (status != Status::Prepared) step.blocked_token_offset = token.offset;
            return status;
        }
        const bool glyph = token.tag == 2 || token.visitor_slot == 0xbc;
        const bool resume = token.tag == 0x31;
        if (!glyph && !resume && !generic_slot(token.visitor_slot)) {
            step.blocked_token_offset = token.offset;
            return Status::RequiresControlHandler;
        }
        if (next.pacing_stop_35 != 0) {
            next.resume_offset_20 = token.offset; // FUN_80103784.
            return Status::Prepared;
        }
        if (glyph) {
            ++next.revealed_units_24;
            if (next.fast_36 != 0) {
                // subfc/subfze stores unsigned >= as a byte, including word wrap.
                next.pacing_stop_35 = next.revealed_units_24 >= next.window_units_28 ? 1u : 0u;
            } else {
                next.pacing_stop_35 = 1;
                step.feedback_id = uint16_t{6};
                step.feedback_channel_check = true;
                step.blocked_token_offset = token.offset;
                step.next_token_offset = following;
                return Status::RequiresFeedback;
            }
        } else if (resume) {
            next.resume_offset_20 = following;
            const auto status = resume_data(data, size, step, clock);
            if (status != Status::Prepared) step.blocked_token_offset = token.offset;
            return status; // Tag 0x31 always stops this walk.
        }
    }
    return Status::InvalidInput;
}
} // namespace

WorldMapPresentationStatus prepare_world_map_presentation_step(
    const uint8_t* data, size_t size, const WorldMapPresentationState& state,
    uint32_t clock, uint32_t pressed, uint32_t repeat, WorldMapPresentationStep* out) {
    if (out == nullptr || &state == &out->after) return Status::InvalidInput;
    if (state.phase < Phase::Reading || state.phase > Phase::Completion || !std::isfinite(state.scroll_48)) {
        return Status::InvalidState;
    }
    if (data == nullptr || size > std::numeric_limits<uint32_t>::max()) return Status::InvalidInput;
    WorldMapMessageStream stream;
    if (scan_world_map_message_stream(data, size, &stream) != WorldMapMessageStreamStatus::Decoded ||
        !valid_offset(stream, state.display_offset_1c) || !valid_offset(stream, state.resume_offset_20)) {
        return Status::InvalidInput;
    }
    WorldMapPresentationStep step;
    step.after = state;
    auto& next = step.after;
    Status status = Status::Prepared;
    switch (state.phase) {
    case Phase::Reading:
        if (next.lock_fast_37 == 0) next.fast_36 = static_cast<uint8_t>((repeat >> 8) & 1u);
        // Original unsigned strict comparison is not a wrap-aware deadline.
        if (next.deadline_2c < clock) {
            next.deadline_2c = clock + next.interval_30;
            next.pacing_stop_35 = 0;
            status = read_tokens(data, size, stream, step, clock);
        }
        break;
    case Phase::Scrolling:
        if (next.scroll_48 < 32.0f) {
            next.scroll_48 += 2.1f; // DOL float word 0x40066666.
        } else {
            WorldMapPresentationAdvance advance;
            if (advance_world_map_presentation_data(data, size, next.display_offset_1c,
                    next.revealed_units_24, &advance) != WorldMapPresentationDataStatus::Prepared) return Status::InvalidInput;
            next.display_offset_1c = advance.start_offset;
            next.revealed_units_24 = advance.remaining_revealed_units;
            next.window_units_28 = advance.window.units;
            reset_clock(next, clock);
            next.scroll_48 = 0;
            status = change_phase(step, Phase::Reading, 5);
        }
        break;
    case Phase::InputWait: {
        constexpr uint32_t masks[] = {0x100, 0x800, 0x900};
        if (next.manager_input_mode_58 >= 3) return Status::InvalidState;
        if ((pressed & masks[next.manager_input_mode_58]) != 0) {
            step.feedback_id = uint16_t{3};
            step.phase_after_effects = Phase::Reading;
            if (next.manager_resource_60 != UINT32_MAX) step.actor_mode = 5;
            status = Status::RequiresFeedback; // Before actor calls and member store.
        }
        break;
    }
    case Phase::Selection:
        status = Status::RequiresSelection;
        break;
    case Phase::Hold:
        break; // FUN_80103040 returns zero; no timed update is inferred.
    case Phase::Completion:
        step.reports_complete = next.completion_gate_34 == 0; // FUN_80102C58.
        break;
    }
    if (status == Status::InvalidInput) return status;
    *out = step;
    return status;
}

WorldMapPresentationStatus advance_world_map_presentation(
    const uint8_t* data, size_t size, WorldMapPresentationState* state,
    uint32_t clock, uint32_t pressed, uint32_t repeat, WorldMapPresentationStep* out) {
    if (state == nullptr || out == nullptr || state == &out->after) return Status::InvalidInput;
    const auto status = prepare_world_map_presentation_step(data, size, *state, clock, pressed, repeat, out);
    if (status != Status::Prepared) return status;
    *state = out->after;
    return Status::Advanced;
}
} // namespace awl
