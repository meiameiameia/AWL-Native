#pragma once
#include "awl/world_map_player_status.h"
#include "awl/world_map_player_update.h"
#include "awl/collision_asset.h"
#include <vector>

namespace awl {
enum class WorldMapPlayerTerrainProbeStatus {
    Prepared,RequiresSlot,RequiresAsset,InvalidInput,UnsupportedLayout,UnsupportedNumerics,AllocationFailure
};
struct WorldMapPlayerTerrainProbeResult {
    WorldMapPlayerTerrainProbeStatus status=WorldMapPlayerTerrainProbeStatus::RequiresSlot;
    std::optional<int32_t> code;
    std::optional<CollisionDirectedTriangleSample> triangle;
};
// 1CADC installs mapjump.col into slot five, separate from movement/camera.
// This owns validated bytes; runtime slot presence remains an observation.
// Failed reload clears the prior payload; neither loader activates a slot.
class WorldMapPlayerTerrainProbeAsset {
public:
    [[nodiscard]] bool load();
    [[nodiscard]] bool load_from_bytes(const uint8_t*,size_t);
    [[nodiscard]] bool loaded() const noexcept {return !bytes_.empty();}
    [[nodiscard]] WorldMapPlayerTerrainProbeResult query(const WorldMapPlayerScenePose&) const;
private:
    std::vector<uint8_t> bytes_;
};
enum class WorldMapPlayerState1SecondaryStatus {
    Completed,AlreadyCompleted,Filtered,RequiresControl,RequiresPrimary,RequiresAnimation,
    RequiresGuards,RequiresGuardWord58c,RequiresBody,InvalidInput,
    RequiresUpdateOwners,RequiresStatus,RequiresAction,ConditionsIncomplete,
    RequiresTerrainSlot,RequiresTerrainAsset,TerrainIncomplete,RequiresTerrainHandler,
    RequiresPad,RequiresItem,RequiresMovementOwner,RequiresSceneType,RequiresSecondaryByte,
    TransitionRequired,AllocationFailure
};
struct WorldMapPlayerState1SecondaryResult {
    WorldMapPlayerState1SecondaryStatus status=WorldMapPlayerState1SecondaryStatus::InvalidInput;
    std::optional<WorldMapPlayerStatusConditionStep> conditions;
    std::optional<WorldMapPlayerTerrainProbeResult> terrain;
    std::optional<uint32_t> requested_state;
    bool reset_required=false,action_setup_required=false,scene_cleanup_required=false;
};
struct WorldMapPlayerState1SecondaryInput {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    std::optional<bool> terrain_slot_present;
    const WorldMapPlayerTerrainProbeAsset* terrain=nullptr;
    std::optional<uint8_t> secondary_byte_3f1;
    // Replaces the entire initial-DOL snapshot when supplied, even empty.
    std::optional<WorldMapPlayerStatusConditionFactors> condition_factors_override;
};
// Isolated observations for original instruction comparisons. The owned
// startup facade derives status/action/item/PAD/pose/scene from its owners.
struct WorldMapPlayerState1SecondaryQuery {
    std::optional<uint32_t> control,phase_raw,action,item,pressed;
    const WorldMapPlayerStatusState* status=nullptr;
    WorldMapPlayerStatusConditionFactors factors;
    std::optional<WorldMapPlayerScenePose> pose;
    std::optional<int32_t> manager_scene_type;
    WorldMapPlayerState1SecondaryInput dependencies;
};
// 42DFC body through condition checks, slot-five terrain, and ordered PAD
// routes. Reached requests stop before controller1504/reset/action/state
// initialization. Completed alone authorizes condition-latch publication.
[[nodiscard]] WorldMapPlayerState1SecondaryResult prepare_world_map_player_state1_secondary(
    const WorldMapPlayerState1SecondaryQuery&);
} // namespace awl
