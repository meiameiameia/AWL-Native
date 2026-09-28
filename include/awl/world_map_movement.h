#pragma once

#include "awl/player_input.h"
#include "awl/world_map_contact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

// Caller-supplied state for the supported category-1 path through
// FUN_8003083C. The directional and resolver views of the first object list
// must refer to the same runtime entries in the same order.
struct WorldMapMovementQuery {
    int32_t state_680 = -1;
    int32_t state_58c = 0;
    HsdPadFrame pad{};
    float camera_yaw_radians = 0.0f;
    bool camera_yaw_commit_enabled = false;
    std::array<float, 3> current_position{};
    std::array<float, 3> current_axis{};
    WorldMapSteeringState steering{};
    const WorldMapContactObject* directional_objects = nullptr;
    size_t directional_object_count = 0;
    CollisionCategory1MovementQuery collision{};
};

struct WorldMapMovementCandidate {
    bool movement_enabled = false;
    WorldMapSteeringState steering{};
    std::array<float, 3> proposed_position{};
    std::array<float, 3> resolved_position{};
    float camera_yaw_after_proposal = 0.0f;
    bool camera_yaw_written = false;
    WorldMapContactResult directional_contact{};
    CollisionCategory1MovementAdjustment collision{};
};

// Composes steering, the pre-collision proposal, directional contact, and the
// supported type-1 collision resolver in DOL call order. The result stops
// before FUN_800107A4's scene-object position/lookup update, applying the
// reported camera yaw, later camera work, animation, or gameplay mutation.
[[nodiscard]] bool calculate_world_map_movement_candidate(
    const WorldMapMovementQuery& query,
    WorldMapMovementCandidate* candidate);

struct WorldMapMovementContactSlotOutcome {
    bool polygon_contact = false; // FUN_8001DA44 for this slot
    bool state_request_accepted = false; // FUN_8010A9C0(3, slot, 0)
};

struct WorldMapMovementContactTail {
    int32_t recorded_category = 0; // r13-0x58F8
    std::array<float, 3> recorded_prior{}; // 0x802E9278
    std::array<float, 3> recorded_resolved{}; // 0x802E9284
    uint32_t polygon_queries = 0;
    uint32_t state_requests = 0;
    int32_t accepted_slot = -1;
    bool movement_reset_requested = false; // FUN_800310A4(player, 1)
};

// Isolates FUN_8001D9F8's unconditional shared-state copy and the ordered
// category-1 two-slot branch at FUN_8003083C 0x80030D0C..0x80030D88.
// Polygon contacts and state-request results are supplied because their
// runtime owners remain unresolved. No globals, player, or actions change.
// Output is unchanged on invalid input.
[[nodiscard]] bool plan_world_map_movement_contact_tail(
    int32_t collision_category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& resolved_position,
    const std::array<WorldMapMovementContactSlotOutcome, 2>& slot_outcomes,
    WorldMapMovementContactTail* output);

// FUN_8019AC7C / FUN_8019AB50, reached through FUN_8001DA44. The caller
// supplies one polygon's ordered XYZ vertices; only consecutive pairs form
// edges, so a closing edge requires a repeated first vertex. Mode 0 tests
// odd crossings of a +X ray; modes 1..3 test movement-edge crossings with
// any, negative, or nonnegative orientation respectively. No runtime
// polygon lookup or action request is performed. Output is atomic on error.
[[nodiscard]] bool query_world_map_polygon_contact(
    int32_t mode,
    const std::vector<std::array<float, 3>>& vertices,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& resolved_position,
    bool* contact);

} // namespace awl
