#include "awl/world_map_movement.h"

#include <cmath>

namespace awl {

namespace {

bool finite_position(const std::array<float, 3>& position) {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

bool finite_steering(const WorldMapSteeringState& state) {
    return std::isfinite(state.direction_x) &&
           std::isfinite(state.direction_z) &&
           std::isfinite(state.facing_x) &&
           std::isfinite(state.facing_z) &&
           std::isfinite(state.target_speed) &&
           std::isfinite(state.current_speed) &&
           std::isfinite(state.intensity);
}

// FUN_8017C02C tests XZ segments with inclusive starts and exclusive ends.
bool xz_segments_cross(const std::array<float, 3>& start,
                       const std::array<float, 3>& end,
                       const std::array<float, 3>& edge_start,
                       const std::array<float, 3>& edge_end,
                       bool* crossed) {
    const float move_x = end[0] - start[0];
    const float move_z = end[2] - start[2];
    const float edge_from_x = edge_start[0] - start[0];
    const float edge_from_z = edge_start[2] - start[2];
    const float edge_x = edge_end[0] - edge_start[0];
    const float edge_z = edge_end[2] - edge_start[2];
    if (!std::isfinite(move_x) || !std::isfinite(move_z) ||
        !std::isfinite(edge_from_x) || !std::isfinite(edge_from_z) ||
        !std::isfinite(edge_x) || !std::isfinite(edge_z)) {
        return false;
    }
    const float determinant = move_z * edge_x - move_x * edge_z;
    if (!std::isfinite(determinant)) {
        return false;
    }
    if (determinant == 0.0f) {
        *crossed = false;
        return true;
    }
    const float move_fraction =
        (edge_from_z * edge_x - edge_from_x * edge_z) / determinant;
    if (!std::isfinite(move_fraction)) {
        return false;
    }
    if (!(move_fraction >= 0.0f && move_fraction < 1.0f)) {
        *crossed = false;
        return true;
    }
    const float edge_fraction =
        (edge_from_z * move_x - edge_from_x * move_z) / determinant;
    if (!std::isfinite(edge_fraction)) {
        return false;
    }
    *crossed = edge_fraction >= 0.0f && edge_fraction < 1.0f;
    return true;
}

std::array<float, 3> as_array(const WorldMapPosition& position) {
    return {position.x, position.y, position.z};
}

} // namespace

bool calculate_world_map_movement_candidate(
    const WorldMapMovementQuery& query,
    WorldMapMovementCandidate* candidate) {
    if (candidate != nullptr) {
        *candidate = {};
    }
    if (candidate == nullptr || !finite_position(query.current_position) ||
        !finite_position(query.current_axis) ||
        !std::isfinite(query.camera_yaw_radians) ||
        !finite_steering(query.steering)) {
        return false;
    }

    WorldMapMovementCandidate result;
    result.steering = query.steering;
    result.proposed_position = query.current_position;
    result.resolved_position = query.current_position;
    result.camera_yaw_after_proposal = query.camera_yaw_radians;
    if (query.state_680 != -1 || query.state_58c != 0) {
        *candidate = result;
        return true;
    }
    if (query.directional_object_count != query.collision.first_object_count ||
        (query.directional_object_count != 0 &&
         (query.directional_objects == nullptr ||
          query.collision.first_objects == nullptr))) {
        return false;
    }
    for (size_t index = 0; index < query.directional_object_count; ++index) {
        const WorldMapContactObject& directional =
            query.directional_objects[index];
        const CollisionDynamicPassObject& resolver =
            query.collision.first_objects[index];
        if (directional.enabled != resolver.enabled ||
            directional.category != resolver.category ||
            directional.collision_flags != resolver.collision_flags ||
            directional.data != resolver.data ||
            directional.size != resolver.size) {
            return false;
        }
        if ((resolver.collision_flags & 2u) != 0 &&
            (directional.contact_query.world_to_object !=
                 resolver.world_to_object ||
             directional.contact_query.object_to_world !=
                 resolver.object_to_world ||
             directional.contact_query.object_center_local !=
                 resolver.center_local ||
             directional.contact_query.object_radius != resolver.radius)) {
            return false;
        }
    }

    result.movement_enabled = true;
    update_world_map_steering(query.pad, query.camera_yaw_radians,
                              result.steering);
    if (!finite_steering(result.steering)) {
        return false;
    }
    const WorldMapPosition current{query.current_position[0],
                                   query.current_position[1],
                                   query.current_position[2]};
    const WorldMapPositionProposal proposed =
        propose_world_map_position_with_camera(
            current, query.camera_yaw_radians, result.steering,
            query.camera_yaw_commit_enabled);
    result.proposed_position = as_array(proposed.position);
    result.camera_yaw_after_proposal = proposed.camera_yaw_after;
    result.camera_yaw_written = proposed.camera_yaw_written;
    if (!finite_position(result.proposed_position) ||
        !std::isfinite(result.camera_yaw_after_proposal)) {
        return false;
    }
    if (!query_world_map_directional_contact(
            query.directional_objects, query.directional_object_count, 1,
            query.current_position, result.proposed_position,
            query.current_axis, &result.directional_contact)) {
        return false;
    }

    CollisionCategory1MovementQuery collision = query.collision;
    collision.moving_radius = 0.3f;
    if (!resolve_type1_category1_movement_candidate(
            collision, query.current_position, result.proposed_position,
            &result.collision)) {
        return false;
    }
    result.resolved_position = result.collision.position;
    *candidate = result;
    return true;
}

bool plan_world_map_movement_contact_tail(
    int32_t collision_category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& resolved_position,
    const std::array<WorldMapMovementContactSlotOutcome, 2>& slot_outcomes,
    WorldMapMovementContactTail* output) {
    if (output == nullptr || !finite_position(prior_position) ||
        !finite_position(resolved_position)) {
        return false;
    }
    WorldMapMovementContactTail next;
    next.recorded_category = collision_category;
    next.recorded_prior = prior_position;
    next.recorded_resolved = resolved_position;
    // FUN_8001D9F8 returns one after writing these values, so its caller
    // reaches the slot loop exactly when the player category is one.
    if (collision_category == 1) {
        for (int32_t slot = 0; slot < 2; ++slot) {
            ++next.polygon_queries;
            const auto& outcome = slot_outcomes[static_cast<size_t>(slot)];
            if (!outcome.polygon_contact) {
                continue;
            }
            ++next.state_requests;
            if (outcome.state_request_accepted) {
                next.accepted_slot = slot;
                next.movement_reset_requested = true;
                break;
            }
        }
    }
    *output = next;
    return true;
}

bool query_world_map_polygon_contact(
    int32_t mode,
    const std::vector<std::array<float, 3>>& vertices,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& resolved_position,
    bool* contact) {
    if (contact == nullptr || mode < 0 || mode > 3 || vertices.size() < 2 ||
        !finite_position(prior_position) ||
        !finite_position(resolved_position)) {
        return false;
    }
    for (const auto& vertex : vertices) {
        if (!finite_position(vertex)) {
            return false;
        }
    }

    std::array<float, 3> end = resolved_position;
    if (mode == 0) {
        // r2-0x6174 = 10000.0f in the verified DOL.
        end = prior_position;
        end[0] += 10000.0f;
        if (!finite_position(end)) {
            return false;
        }
    }

    bool crossed = false;
    for (size_t index = 1; index < vertices.size(); ++index) {
        const auto& edge_start = vertices[index - 1];
        const auto& edge_end = vertices[index];
        bool edge_crossed = false;
        if (!xz_segments_cross(prior_position, end, edge_start, edge_end,
                               &edge_crossed)) {
            return false;
        }
        if (!edge_crossed) {
            continue;
        }
        if (mode == 0) {
            crossed = !crossed;
            continue;
        }
        if (mode == 1) {
            crossed = true;
            break;
        }
        const float edge_z = edge_end[2] - edge_start[2];
        const float edge_x = edge_end[0] - edge_start[0];
        const float move_x = resolved_position[0] - prior_position[0];
        const float move_z = resolved_position[2] - prior_position[2];
        const float orientation = edge_z * move_x - edge_x * move_z;
        if (!std::isfinite(orientation)) {
            return false;
        }
        if ((mode == 2 && orientation < 0.0f) ||
            (mode == 3 && orientation >= 0.0f)) {
            crossed = true;
            break;
        }
    }
    *contact = crossed;
    return true;
}

} // namespace awl
