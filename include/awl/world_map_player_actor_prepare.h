#pragma once
#include "awl/world_map_secondary_model.h"

namespace awl {
// Semantic observations of guard +60's bit array +658 and ordered +630
// list. These are not a script executor, packed pointers or live registration.
struct WorldMapPlayerActorScriptRecord {
    int32_t actor_code_d0 = 0;
    std::optional<uint32_t> active_word_78;
};
struct WorldMapPlayerActorScriptOwner {
    std::optional<uint32_t> flag_width_4;
    std::vector<uint8_t> flag_bytes;
    std::optional<std::vector<WorldMapPlayerActorScriptRecord>> records;
};
enum class WorldMapPlayerActorGateStatus {
    Ready, BlockedByScript, RequiresSavedFlag, RequiresAuxiliaryCallback,
    RequiresScriptOwner, RequiresFlagWidth, UnsupportedFlagWidth,
    RequiresFlagBytes, RequiresScriptList, RequiresRecordState
};
// 31584's normal state-29 precondition followed by 109120(guard+60,0).
// A nonzero saved +29A01 requires the untranslated auxiliary +DC virtual
// callback BEFORE script reads. Flag zero uses 18972C's little bit order;
// widths through 32 are supported, including zero (no byte reads). Nonzero
// flags bypass the list. Otherwise 10927C uses its first actor-code-zero
// record, and only that record's +78 is read. Duplicate codes are allowed.
// No control/actor-byte stores, callback acknowledgement or queue mutation.
[[nodiscard]] WorldMapPlayerActorGateStatus prepare_world_map_player_state29_actor_gate(
    std::optional<uint8_t> saved_byte_29a01,
    const std::optional<WorldMapPlayerActorScriptOwner>& script) noexcept;
// State zero reaches the same nonzero-saved-byte auxiliary branch as 29.
// Only these two supported normal states are established by the owner.
[[nodiscard]] WorldMapPlayerActorGateStatus prepare_world_map_player_normal_actor_gate(
    std::optional<uint8_t>,const std::optional<WorldMapPlayerActorScriptOwner>&) noexcept;

struct WorldMapPlayerActorSphere {
    std::array<float,3> center{};
    float radius = 0;
};
struct WorldMapPlayerActorBounds {
    WorldMapPlayerActorSphere sphere;
    float height_10 = 0, horizontal_radius_14 = 0;
};
enum class WorldMapPlayerActorBoundsStatus { Prepared, InvalidInput, UnsupportedNumerics };
// BBA0's position + (-1,0,-1)/(1,2,1), then 17BD64 -> 17BD94.
// Endpoint adds, ordered min/max, half extents, center, both lengths and
// doubled half-height retain binary32 order. This cannot be replaced by
// position+(0,1,0) and a constant radius: endpoint rounding changes extents.
// Lengths use native double 1/sqrt as the frsqrte seed, then the original
// rounded Newton correction. The hardware estimate table remains unmodeled;
// this explicit native profile does not establish original radius equality.
// Nearest rounding and normal/zero finite data/intermediates are required.
// Failure preserves output; the observed position may alias its center.
[[nodiscard]] WorldMapPlayerActorBoundsStatus prepare_world_map_player_normal_actor_bounds(
    const std::array<float,3>& position, WorldMapPlayerActorBounds* out) noexcept;
struct WorldMapPlayerActorPlane {
    std::array<float,3> point{}, normal{};
};
struct WorldMapPlayerActorVisibilityOwner {
    // 17BD10's pointed camera matrix, followed by the six 80315838 planes.
    std::optional<WorldMapModelMatrix> view;
    std::array<std::optional<WorldMapPlayerActorPlane>,6> planes;
};
enum class WorldMapPlayerActorVisibilityStatus {
    Visible, Culled, RequiresView, RequiresPlane, InvalidInput, UnsupportedNumerics
};
struct WorldMapPlayerActorVisibilityResult {
    WorldMapPlayerActorVisibilityStatus status = WorldMapPlayerActorVisibilityStatus::InvalidInput;
    std::optional<uint32_t> plane_index; // First rejecting, missing or invalid plane.
};
// Normal +15E=0 197568 test, including 1B8760/8AD8/8BBC's operation order.
// Sphere construction, camera/plane production, the alternate +15E test and
// 197614's later +15F/effect decision remain outside this supplied boundary.
// Paired-single operations use binary32 products/sums and native float fma;
// this is not a claim for every original paired-single numerical corner.
// Requires nearest rounding, finite normals/radius/inputs/intermediates and
// no reached subnormal values. Radius must be nonnegative. A tangent plane
// culls (dot >= radius); later planes are unread after the first rejection.
// This function is allocation-free and never mutates its observations.
[[nodiscard]] WorldMapPlayerActorVisibilityResult prepare_world_map_player_actor_visibility(
    const WorldMapPlayerActorSphere& sphere, const WorldMapPlayerActorVisibilityOwner& owner) noexcept;

struct WorldMapPlayerActorScreenOwner {
    // Separate globals: 12B9FC returns 802FC020, 12B9F0 returns 802FC010.
    // Neither is inferred from the first visibility owner's camera matrix.
    std::optional<WorldMapModelMatrix> view;
    std::array<std::optional<float>,2> half_extents;
};
enum class WorldMapPlayerActorScreenStatus {
    Inside, Outside, RequiresView, RequiresExtent, InvalidInput, UnsupportedNumerics
};
struct WorldMapPlayerActorScreenResult {
    WorldMapPlayerActorScreenStatus status = WorldMapPlayerActorScreenStatus::InvalidInput;
    std::optional<uint32_t> axis; // First outside, unknown or invalid X/Y extent.
};
// 197614: transform the center, then reject (coordinate+radius <= -extent)
// or (coordinate-radius >= extent), X before Y. Tangency is outside. This
// selects actor +15F/effects; Outside does NOT cull the model's CPU frame.
// The Y extent is unread after an X rejection. Native arithmetic limits are
// the same as the first visibility test. Does not publish actor metadata.
[[nodiscard]] WorldMapPlayerActorScreenResult prepare_world_map_player_actor_screen(
    const WorldMapPlayerActorSphere& sphere, const WorldMapPlayerActorScreenOwner& owner) noexcept;
} // namespace awl
