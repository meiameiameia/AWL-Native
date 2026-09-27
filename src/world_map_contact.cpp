#include "awl/world_map_contact.h"

#include "awl/player_input.h"

#include <cmath>

namespace awl {

namespace {

bool finite_position(const std::array<float, 3>& position) {
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

WorldMapPosition as_world_map_position(const std::array<float, 3>& position) {
    return {position[0], position[1], position[2]};
}

} // namespace

bool query_world_map_directional_contact(
    const WorldMapContactObject* objects,
    size_t object_count,
    int32_t required_category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    const std::array<float, 3>& fallback_axis,
    WorldMapContactResult* result) {
    if (result != nullptr) {
        *result = {};
    }
    if (result == nullptr || (object_count != 0 && objects == nullptr) ||
        !finite_position(prior_position) ||
        !finite_position(proposed_position) ||
        !finite_position(fallback_axis)) {
        return false;
    }

    WorldMapContactResult candidate;
    candidate.world_position = proposed_position;
    candidate.heading_axis = fallback_axis;
    const float dx = proposed_position[0] - prior_position[0];
    const float dz = proposed_position[2] - prior_position[2];
    const float distance_squared = dx * dx + dz * dz;
    if (!std::isfinite(distance_squared)) {
        return false;
    }
    if (distance_squared == 0.0f) {
        *result = candidate;
        return true;
    }

    for (size_t index = 0; index < object_count; ++index) {
        const WorldMapContactObject& object = objects[index];
        if (!object.enabled || object.category != required_category ||
            (object.collision_flags & 2u) == 0) {
            continue;
        }
        if (!finite_position(object.world_position) ||
            !finite_position(object.heading_axis)) {
            return false;
        }
        CollisionDynamicObjectQuery query = object.contact_query;
        query.moving_radius = 0.3f;
        query.surface_mask = 0x10000u;
        query.contact_flags = 0u;
        CollisionDynamicObjectContactAdjustment contact;
        if (!resolve_type1_dynamic_object_contact(
                object.data, object.size, query, prior_position,
                proposed_position, &contact)) {
            return false;
        }
        ++candidate.queried_objects;
        if (!contact.contact) {
            continue;
        }
        ++candidate.contacting_objects;
        // FUN_80191898 supplies the mask through its first edge call. A
        // vertex-only hit has no supported directional mask in this helper.
        if (!contact.local_narrow_phase.first_edge_contact) {
            continue;
        }
        uint8_t code = 7;
        if (!classify_world_map_directional_contact(
                as_world_map_position(prior_position),
                as_world_map_position(proposed_position),
                object.heading_axis[0], object.heading_axis[2],
                contact.local_narrow_phase.first_edge_surface_flags & 0xFu,
                &code)) {
            continue;
        }
        candidate.matched = true;
        candidate.direction_code = code;
        candidate.metadata = object.metadata;
        candidate.world_position = object.world_position;
        candidate.heading_axis = object.heading_axis;
        *result = candidate;
        return true;
    }
    *result = candidate;
    return true;
}

} // namespace awl
