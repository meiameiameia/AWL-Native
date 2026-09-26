#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace awl {

struct CollisionTreeAnalysis {
    uint8_t format = 0;
    uint8_t header_byte_5 = 0;
    uint8_t header_byte_6 = 0;
    uint8_t header_byte_7 = 0;
    size_t node_count = 0;
    size_t leaf_count = 0;
    size_t triangle_count = 0;
    size_t vertex_count = 0;
    size_t max_triangles_per_leaf = 0;
    size_t max_vertices_per_leaf = 0;
    uint32_t max_depth = 0;
    float coordinate_scale = 0.0f;
    std::array<float, 3> root_min{};
    std::array<float, 3> root_max{};
};

struct CollisionSurfaceSample {
    float height = 0.0f;
    uint16_t surface_flags = 0;
    uint32_t leaf_offset = 0;
    uint32_t triangle_index = 0;
};

struct CollisionEdgeSample {
    std::array<float, 3> position{};
    uint16_t surface_flags = 0;
    uint32_t leaf_offset = 0;
    uint32_t triangle_index = 0;
    uint8_t edge_index = 0;
    float distance_squared_xz = 0.0f;
};

struct CollisionTerrainAdjustment {
    std::array<float, 3> position{};
    uint16_t surface_flags = 0;
    bool used_edge_fallback = false;
};

struct CollisionResolverHeightAdjustment {
    std::array<float, 3> position{};
    uint16_t surface_flags = 0;
    uint32_t leaf_offset = 0;
    bool used_edge_fallback = false;
};

// Row-major 3x4 world-to-object transform used by the traced contact query.
using CollisionAffineTransform = std::array<float, 12>;

struct CollisionRadiusVertexAdjustment {
    std::array<float, 3> position{};
    bool contact = false;
    uint32_t triangle_index = 0;
    uint8_t vertex_index = 0;
};

struct CollisionRadiusEdgeAdjustment {
    std::array<float, 3> position{};
    bool contact = false;
    uint16_t surface_flags = 0;
    uint32_t triangle_index = 0;
    uint8_t edge_index = 0;
};

struct CollisionRadiusPassesAdjustment {
    std::array<float, 3> position{};
    bool contact = false;
    bool reverted_to_prior = false;
    uint8_t pass_count = 0;
};

struct CollisionDynamicContactAdjustment {
    std::array<float, 3> position{};
    bool contact = false;
    bool used_containment_shortcut = false;
    bool reverted_to_prior = false;
    bool first_edge_contact = false;
    uint16_t first_edge_surface_flags = 0;
    uint8_t pass_count = 0;
};

struct CollisionDynamicObjectQuery {
    CollisionAffineTransform world_to_object{};
    CollisionAffineTransform object_to_world{};
    std::array<float, 3> object_center_local{};
    float object_radius = 0.0f;
    float moving_radius = 0.0f;
    uint32_t surface_mask = 0;
    uint32_t contact_flags = 0;
};

struct CollisionDynamicObjectContactAdjustment {
    std::array<float, 3> position{};
    bool contact = false;
    bool broad_phase_passed = false;
    CollisionDynamicContactAdjustment local_narrow_phase{};
};

struct CollisionFirstPassObject {
    uint64_t identity = 0;
    bool enabled = false;
    int32_t category = 0;
    uint32_t collision_flags = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    CollisionAffineTransform world_to_object{};
    CollisionAffineTransform object_to_world{};
    std::array<float, 3> center_local{};
    std::array<float, 3> center_world{};
    float radius = 0.0f;
};

struct CollisionFirstDynamicPassAdjustment {
    std::array<float, 3> position{};
    bool contact = false;
    uint32_t contact_flags_after = 0;
    uint32_t resolver_contact_bit = 0;
    size_t queried_objects = 0;
    size_t contact_count = 0;
};

struct CollisionTerrainRadiusAdjustment {
    std::array<float, 3> position{};
    uint16_t surface_flags = 0;
    // FUN_8002009C's return for this branch: initial miss OR radius contact.
    bool terrain_contact = false;
    bool initial_edge_fallback = false;
    bool final_edge_fallback = false;
    bool radius_contact = false;
    bool reverted_to_prior = false;
    uint8_t radius_pass_count = 0;
};

// Validates the relocatable type-1 tree structure used by the verified
// movement collision assets, including their indexed triangle payloads.
[[nodiscard]] bool analyze_type1_collision_asset(
    const uint8_t* data,
    size_t size,
    CollisionTreeAnalysis* analysis);

// Selects the target leaf and returns the first triangle containing (x, z),
// matching the primary terrain-height branch observed in the target. The
// target's nearest-edge fallback and full movement resolver are not included.
[[nodiscard]] bool sample_type1_collision_surface(
    const uint8_t* data,
    size_t size,
    float x,
    float z,
    CollisionSurfaceSample* sample);

// Projects (x, z) onto the nearest triangle edge in the selected leaf and
// evaluates the selected triangle's height there. This is the target's
// FUN_801917FC type-1 fallback, not a movement collision resolver.
[[nodiscard]] bool project_type1_collision_to_edge(
    const uint8_t* data,
    size_t size,
    float x,
    float z,
    CollisionEdgeSample* sample);

// Isolates the height-enabled terrain lookup in FUN_8002009C: use the first
// containing triangle, or project to the nearest edge in the selected leaf.
// This does not apply the function's radius-aware passes or accept movement.
[[nodiscard]] bool adjust_type1_collision_terrain_height(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    CollisionTerrainAdjustment* adjustment);

// Isolates FUN_8001FF70's height lookup used after a resolver object pass.
// It selects a fresh leaf at the resulting X/Z and replaces only Y, even
// when an edge fallback supplies that height. It does not run object passes.
[[nodiscard]] bool resample_type1_collision_resolver_height(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& position,
    CollisionResolverHeightAdjustment* adjustment);

// Isolates FUN_80191B7C's X/Z distance gate before its narrow-phase query.
// The position's Y is cleared before transformation. A true result means the
// inputs were supported; *may_contact only means narrow phase must be checked.
[[nodiscard]] bool evaluate_dynamic_contact_broad_phase(
    const CollisionAffineTransform& world_to_object,
    const std::array<float, 3>& proposed_position,
    const std::array<float, 3>& object_center_local,
    float object_radius,
    float moving_radius,
    bool* may_contact);

// Isolates type-1 +0x20 (FUN_80194008) after a dynamic contact's edge pass.
// It filters triangles by surface mask, then tests all their vertices without
// the terrain radius pass's incident-edge-bit restriction. It does not run the
// containing-triangle check, edge pass, or repeated contact resolver.
[[nodiscard]] bool adjust_type1_dynamic_contact_vertex(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    CollisionRadiusVertexAdjustment* adjustment);

// Isolates type-1 +0x1C (FUN_80196D88) before the dynamic vertex pass.
// Eligible triangles contribute all three edges, regardless of edge bits.
// This does not run the containing-triangle check or repeated resolver.
[[nodiscard]] bool adjust_type1_dynamic_contact_edge(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    CollisionRadiusEdgeAdjustment* adjustment);

// Isolates type-1 FUN_80191898 after the object's broad-phase distance gate.
// The first containing triangle check uses the prior point; repeated edge and
// vertex responses stay in the leaf selected from the proposed point. This
// does not transform object coordinates or accept world-space movement.
[[nodiscard]] bool resolve_type1_dynamic_contact_narrow_phase(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    uint32_t surface_mask,
    uint32_t contact_flags,
    CollisionDynamicContactAdjustment* adjustment);

// Isolates FUN_80191B7C for a caller-supplied type-1 object and matrices.
// A miss preserves the proposed world point; contact transforms a local
// response back with local Y cleared. Object-list ownership is not included.
[[nodiscard]] bool resolve_type1_dynamic_object_contact(
    const uint8_t* data,
    size_t size,
    const CollisionDynamicObjectQuery& query,
    const std::array<float, 3>& prior_world_position,
    const std::array<float, 3>& proposed_world_position,
    CollisionDynamicObjectContactAdjustment* adjustment);

// Isolates FUN_8001E498's first ordered object-list pass (flag 0x4) for
// caller-supplied type-1 COL and circle objects. The alternate 0x10 path is
// unsupported. This does not own the game's object list or run later passes.
[[nodiscard]] bool resolve_type1_first_dynamic_object_pass(
    const CollisionFirstPassObject* objects,
    size_t object_count,
    uint64_t source_identity,
    int32_t category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float moving_radius,
    uint32_t initial_contact_flags,
    uint32_t resolver_flags,
    CollisionFirstDynamicPassAdjustment* adjustment);

// Isolates the type-1 +0x34 virtual (FUN_80194E30) used by the radius pass.
// The preceding +0x30 edge pass and full resolver are not included.
[[nodiscard]] bool adjust_type1_collision_radius_vertex(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusVertexAdjustment* adjustment);

// Isolates the type-1 +0x30 virtual (FUN_801969BC) used by the radius pass.
// This does not run the following vertex pass or the three-pass resolver.
[[nodiscard]] bool adjust_type1_collision_radius_edge(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusEdgeAdjustment* adjustment);

// Runs FUN_80191FD0's edge-then-vertex sequence against the initially selected
// leaf, including when a response moves the candidate across a leaf boundary.
[[nodiscard]] bool adjust_type1_collision_radius_passes(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionRadiusPassesAdjustment* adjustment);

// Isolates FUN_8002009C's height-enabled, header-byte-6 = 1 terrain branch.
// The initial query pins one leaf for height/edge fallback, radius passes, and
// contact-triggered height resampling. This is not the shared resolver or an
// accepted gameplay position update.
[[nodiscard]] bool adjust_type1_collision_terrain_with_radius(
    const uint8_t* data,
    size_t size,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float radius,
    CollisionTerrainRadiusAdjustment* adjustment);

} // namespace awl
