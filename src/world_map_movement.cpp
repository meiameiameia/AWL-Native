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

} // namespace awl
