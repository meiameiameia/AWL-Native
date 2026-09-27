#include "awl/world_map_actor_step.h"

#include "awl/collision_asset.h"

#include <cmath>

namespace awl {

namespace {

bool finite_position(const std::array<float, 3>& position) {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

} // namespace

bool propose_world_map_first_actor_step(
    const WorldMapFirstActorStepState& state,
    WorldMapFirstActorStepProposal* proposal) {
    if (proposal == nullptr) {
        return false;
    }
    WorldMapFirstActorStepProposal next;
    next.state = state;
    if (!state.moving) {
        *proposal = next;
        return true;
    }
    if (state.base_constructor_id < 0x3d ||
        state.base_constructor_id > 0x41 ||
        !finite_position(state.proposal) || !finite_position(state.target) ||
        !finite_position(state.heading)) {
        return false;
    }

    // FUN_801466A4 indexes 0x802C41A8 by constructor ID. All five
    // selected entries (0x3D..0x41) hold 1.95. FUN_8015C398 divides by
    // the 30.0 constant at 0x8034B78C, then optionally multiplies by
    // the 1.5 constant at 0x8034B790.
    float step = 1.95f / 30.0f;
    if (state.selector_d4 == 2 && state.selector_d8 == 2) {
        step *= 1.5f;
    }
    const double dx = static_cast<double>(state.target[0]) - state.proposal[0];
    const double dy = static_cast<double>(state.target[1]) - state.proposal[1];
    const double dz = static_cast<double>(state.target[2]) - state.proposal[2];
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance)) {
        return false;
    }
    next.collision_required = true;
    if (distance <= static_cast<double>(step)) {
        next.state.moving = false;
        next.state.timer = 0;
        next.target_reached = true;
    } else {
        for (size_t axis = 0; axis < 3; ++axis) {
            next.state.proposal[axis] += state.heading[axis] * step;
        }
        if (!finite_position(next.state.proposal)) {
            return false;
        }
    }
    *proposal = next;
    return true;
}

bool finalize_world_map_first_actor_step(
    const WorldMapFirstActorStepProposal& proposal,
    const std::array<float, 3>& collision_position,
    const uint8_t* terrain_data, size_t terrain_size,
    WorldMapFirstActorStepResult* result) {
    if (result == nullptr) {
        return false;
    }
    WorldMapFirstActorStepResult next;
    next.state = proposal.state;
    if (!proposal.collision_required) {
        *result = next;
        return true;
    }
    if (!finite_position(collision_position)) {
        return false;
    }
    CollisionResolverHeightAdjustment height;
    if (!resample_type1_collision_resolver_height(
            terrain_data, terrain_size, collision_position, &height)) {
        return false;
    }
    next.collision_altered_horizontal =
        collision_position[0] != proposal.state.proposal[0] ||
        collision_position[2] != proposal.state.proposal[2];
    next.state.proposal = height.position;
    if (next.collision_altered_horizontal) {
        next.state.moving = false;
        next.state.timer = 0;
    }
    *result = next;
    return true;
}

} // namespace awl
