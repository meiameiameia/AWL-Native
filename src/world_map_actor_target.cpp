#include "awl/world_map_actor_target.h"

#include <cmath>

namespace awl {

namespace {

bool finite_position(const std::array<float, 3>& position) {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

struct TargetHit {
    int32_t index = -1;
    uint32_t score = 50;
};

TargetHit scan_targets(
    const std::array<float, 3>& actor_position,
    const std::array<WorldMapFirstActorTargetCandidate, 37>& candidates,
    double maximum_distance) {
    TargetHit hit;
    for (const auto& candidate : candidates) {
        if (candidate.category != 1) {
            continue;
        }
        const double dx = static_cast<double>(actor_position[0]) -
                          candidate.position[0];
        const double dy = static_cast<double>(actor_position[1]) -
                          candidate.position[1];
        const double dz = static_cast<double>(actor_position[2]) -
                          candidate.position[2];
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < maximum_distance && candidate.relationship_score < hit.score) {
            hit.index = candidate.index;
            hit.score = candidate.relationship_score;
        }
    }
    return hit;
}

} // namespace

bool select_world_map_first_actor_target(
    const WorldMapFirstActorTargetState& state,
    const std::array<WorldMapFirstActorTargetCandidate, 37>& candidates,
    std::optional<uint32_t> dispatch_rng_word,
    WorldMapFirstActorTargetDecision* decision) {
    if (decision == nullptr) {
        return false;
    }
    WorldMapFirstActorTargetDecision next;
    next.state = state;
    next.previous_mode = state.mode;
    if (!state.active) {
        *decision = next;
        return true;
    }
    next.position_write_allowed = true;
    if ((state.update_flags & 0x800u) != 0) {
        *decision = next;
        return true;
    }
    if (state.actor_id < 0x2c || state.actor_id > 0x30 ||
        state.mode < 0 || state.mode > 2 || state.action_timer < 0 ||
        !finite_position(state.position)) {
        return false;
    }
    for (size_t order = 0; order < candidates.size(); ++order) {
        if (candidates[order].index < 0 ||
            (order != 0 && candidates[order].index !=
                               static_cast<int32_t>(order + 1)) ||
            !finite_position(candidates[order].position)) {
            return false;
        }
    }
    if ((state.update_flags & 4u) != 0) {
        if (next.state.action_timer > 0) {
            --next.state.action_timer;
        }
        if (next.state.fallback_timer > 0) {
            --next.state.fallback_timer;
        }
    }

    // All five 0x802558E4 table rows contain (6.0, 3.0).
    const TargetHit first = scan_targets(state.position, candidates, 6.0);
    const TargetHit second = scan_targets(state.position, candidates, 3.0);
    next.selector_ran = true;
    next.first_target_index = first.index;
    next.second_target_index = second.index;
    if (first.index != -1) {
        next.first_score = first.score;
    }
    if (second.index != -1) {
        next.second_score = second.score;
    }
    next.state.mode = second.index != -1 ? 2 :
        (first.index != -1 || next.state.fallback_timer > 0 ? 1 : 0);
    if (state.mode >= next.state.mode && next.state.action_timer != 0) {
        *decision = next;
        return true;
    }
    if (!dispatch_rng_word) {
        return false;
    }
    next.state.moving = false;
    // r13-0x6140/-0x613C hold inclusive bounds 0..6 in the verified DOL.
    next.state.action_timer = static_cast<int32_t>(*dispatch_rng_word % 7u) + 7;
    if (next.state.mode == 2) {
        next.handler = WorldMapFirstActorHandler::SecondTarget;
        next.handler_target_index = second.index;
        next.handler_score = second.score;
    } else if (next.state.mode == 1) {
        next.handler = WorldMapFirstActorHandler::FirstTarget;
        next.handler_target_index = first.index;
        next.handler_score = next.first_score;
    } else {
        next.handler = WorldMapFirstActorHandler::Idle;
    }
    *decision = next;
    return true;
}

} // namespace awl
