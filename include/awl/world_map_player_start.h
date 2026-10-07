#pragma once
#include "awl/collision_asset.h"
#include "awl/player_input.h"
#include "awl/world_map_scene_index.h"

namespace awl {

// Reads only the position at owner +29C78 and heading at +29F84 consumed
// by FUN_8002FDF8. Bytes are an in-memory big-endian owner snapshot, NOT a
// disc/save-file format. Scene type comes separately from scene owner +68.
// Invalid/short/nonfinite inputs preserve output; valid headings are copied
// without normalization, including signed zero and non-unit vectors.
[[nodiscard]] bool decode_world_map_player_start_pose(
    const uint8_t* owner_bytes, size_t size, int32_t scene_type,
    WorldMapPlayerScenePose* output);

// Only FUN_80013AE0's constant pose writes, not its other phase/setup work.
// The entry source is (-1,0,-5.2), heading +Z. Scene type remains supplied.
[[nodiscard]] bool make_world_map_phase_entry_pose(
    int32_t scene_type, WorldMapPlayerScenePose* output);

struct WorldMapPlayerStartQuery {
    WorldMapPlayerScenePose saved_pose{};
    int32_t state_680 = -1;
    int32_t state_58c = 0;
    uint8_t secondary_byte_3f3 = 0;
    CollisionWorldMapInitialPlacementQuery placement{};
};

struct WorldMapPlayerStart {
    WorldMapPlayerScenePose pose{};
    WorldMapSteeringState steering{};
    bool placement_called = false;
    CollisionWorldMapInitialPlacement placement{};
};

enum class WorldMapPlayerStartStatus {
    Ready,
    InvalidInput,
    UnsupportedRelocation,
    UnsupportedMovementCategory,
    PlacementFailed,
    SceneRegistrationFailed
};

// Bounded constructor pose/steering before camera/model/state completion.
// Guarded placement calls FUN_8001E170's distinct supported sequence.
// The +3F3 relocation branch is rejected until its predicate is translated.
// All dependencies are supplied; no global owner or saved data is mutated.
[[nodiscard]] WorldMapPlayerStartStatus prepare_world_map_player_start(
    const WorldMapPlayerStartQuery& query, WorldMapPlayerStart* output);

} // namespace awl
