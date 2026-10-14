#pragma once
#include "awl/world_map_movement.h"
#include "awl/world_map_camera.h"
#include "awl/world_map_player_update.h"
#include "awl/world_map_update_inputs.h"

namespace awl {
class WorldMapTriggerAsset;
struct WorldMapPlayerMovementOutputRequest {
    bool metadata=false,direction=false,position=false,axis=false;
};
// Explicit entry projection. No constructor, live player or dispatch receipt
// is inferred. Pose +64 must be category one and match the registered node.
struct WorldMapPlayerMovementState {
    uint64_t scene_identity=0;
    WorldMapPlayerScenePose pose;
    WorldMapSteeringState steering;
    std::optional<WorldMapCameraViewQuery> camera;
    std::optional<WorldMapMovementContactTail> shared_contact;
    std::optional<WorldMapPlayerMovementCameraResult> camera_update;
};
struct WorldMapPlayerMovementInput {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    WorldMapPlayerMovementOutputRequest outputs;
    // Only asset/list/source fields are used. PAD/pose/steering/yaw/guards
    // derive from the entry owner and the current captured input.
    const WorldMapMovementQuery* dependencies=nullptr;
    // Observed runtime category-one polygon provider. Asset ownership alone
    // does not establish the original runtime enabled/category producers.
    const WorldMapTriggerAsset* triggers=nullptr;
    std::optional<uint8_t> secondary_byte_3f1;
    WorldMapCameraHeightSampler sample_height=nullptr;
    void* sample_context=nullptr;
};
enum class WorldMapPlayerMovementStatus {
    Advanced,SkippedByGuard,RequiresGuards,RequiresGuardWord58c,RequiresPad,
    RequiresOwners,RequiresCamera,RequiresCollision,CollisionIncomplete,
    SceneIncomplete,RequiresTriggerOwner,TriggerIncomplete,RequiresActionRequest,
    RequiresSecondaryByte,RequiresHeightSource,CameraIncomplete,
    UnsupportedCategory,UnsupportedNumerics,InvalidInput,AllocationFailure
};
struct WorldMapPlayerMovementResult {
    WorldMapPlayerMovementStatus status=WorldMapPlayerMovementStatus::InvalidInput;
    // Staged diagnostics on failure; only Advanced/SkippedByGuard completes
    // the routine. Requested output fields follow the four nullable arguments.
    std::optional<WorldMapMovementCandidate> movement;
    std::optional<WorldMapScenePositionUpdate> scene;
    std::optional<WorldMapMovementContactTail> tail;
    std::optional<WorldMapPlayerMovementCameraResult> camera;
    std::optional<uint32_t> metadata,direction;
    std::optional<std::array<float,3>> position,axis;
    std::optional<float> intensity;
    int32_t stopped_slot=-1;
};
// Supported 3083C composition: guard defaults, steering/proposal/contact,
// collision, registered scene pose, four optional outputs, shared contact
// recording, ordered no-contact trigger slots, camera follow-up and plane side.
// A reached trigger contact stops before 10A9C0/310A4 action/reset effects.
// Native state + registry publish together after preparation. PAD capture and
// height-sampler side effects are earlier/external work and cannot roll back.
// No animation, status, parent/model receipt, queue, clock or live game changes.
[[nodiscard]] WorldMapPlayerMovementResult advance_world_map_player_movement(
    const WorldMapUpdateInputs&,const WorldMapPlayerMovementInput&,
    WorldMapPlayerMovementState*,WorldMapSceneBucketRegistry*);
} // namespace awl
