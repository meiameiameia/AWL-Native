#include "awl/world_map_actor_step.h"

#include "awl/world_map_collision_registry.h"

#include <cmath>
#include <cstring>

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
    if (state.actor_id < 0x2c || state.actor_id > 0x30 ||
        !finite_position(state.proposal) || !finite_position(state.target) ||
        !finite_position(state.heading)) {
        return false;
    }

    // FUN_801466A4 indexes 0x802C41A8 by actor ID at owner +0x128.
    // These are the exact five DOL float words at 0x802C4258..0x802C4268.
    constexpr std::array<uint32_t, 5> speed_bits{
        0x3f666666u, 0x3fe66666u, 0x3f999999u, 0x3f199999u, 0x3e999999u};
    const uint32_t bits = speed_bits[static_cast<size_t>(state.actor_id - 0x2c)];
    float speed = 0.0f;
    static_assert(sizeof(bits) == sizeof(speed), "DOL speed uses one float word");
    std::memcpy(&speed, &bits, sizeof(speed));
    // FUN_8015C398 divides by 30.0 at 0x8034B78C, then optionally
    // multiplies by 1.5 at 0x8034B790.
    float step = speed / 30.0f;
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

bool calculate_world_map_first_actor_step(
    int32_t actor_id, const WorldMapFirstActorStepState& state,
    const CollisionCategory1MovementQuery& collision_query,
    WorldMapFirstActorResolvedStep* result) {
    if (result == nullptr) {
        return false;
    }
    WorldMapFirstActorResolvedStep next;
    if (!propose_world_map_first_actor_step(state, &next.proposed)) {
        return false;
    }
    if (!next.proposed.collision_required) {
        if (!finalize_world_map_first_actor_step(
                next.proposed, {}, nullptr, 0, &next.finished)) {
            return false;
        }
        *result = next;
        return true;
    }
    if (actor_id < 0x2c || actor_id > 0x30 || actor_id != state.actor_id) {
        return false;
    }

    CollisionCategory1MovementQuery actor_query = collision_query;
    actor_query.moving_radius =
        world_map_first_actor_circle_spec(actor_id).radius;
    if (!resolve_type1_category1_mode2_actor_candidate(
            actor_query, state.proposal, next.proposed.state.proposal,
            &next.collision) ||
        !finalize_world_map_first_actor_step(
            next.proposed, next.collision.position, actor_query.terrain_data,
            actor_query.terrain_size, &next.finished)) {
        return false;
    }
    *result = next;
    return true;
}

} // namespace awl
