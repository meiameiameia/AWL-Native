#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapScenePositionUpdate {
    std::array<float, 3> position{};
    int32_t previous_bucket = 0;
    uint8_t next_bucket = 0;
    bool relink_required = false;
};

// Isolates FUN_800107A4's position and bucket decision for verified scene
// types 0..44. Type 0 uses bucket 0, types 1/2 use position bins 1..16,
// and types 3..44 use fixed buckets 17..58. This returns a plan only: it
// neither mutates an object nor performs the target's list operations.
[[nodiscard]] bool plan_world_map_scene_position_update(
    int32_t scene_type,
    int32_t previous_bucket,
    const std::array<float, 3>& resolved_position,
    WorldMapScenePositionUpdate* update);

struct WorldMapSceneObject {
    // Identity represents the target scene object's intrusive list node.
    uint64_t identity = 0;
    int32_t scene_type = 0;
    std::array<float, 3> position{};
    int32_t bucket = -1;
};

// Caller-supplied scene objects start in the constructor's -1 bucket, then
// move to a supported 0..58 spatial bucket on the first position update.
// FUN_8000C2BC inserts at a bucket's front. FUN_800107A4 writes XYZ on
// every supported position update and relinks only when the key changes.
// This registry does not discover scene objects or update live gameplay.
class WorldMapSceneBucketRegistry {
public:
    [[nodiscard]] bool register_object(uint64_t identity,
                                       int32_t scene_type,
                                       const std::array<float, 3>& position);
    [[nodiscard]] bool update_position(uint64_t identity,
                                       const std::array<float, 3>& position,
                                       WorldMapScenePositionUpdate* update);
    // FUN_80010830 writes both the scene type and position before deciding
    // whether the object's bucket changes.
    [[nodiscard]] bool update_type_and_position(
        uint64_t identity,
        int32_t scene_type,
        const std::array<float, 3>& position,
        WorldMapScenePositionUpdate* update);
    [[nodiscard]] bool unregister_object(uint64_t identity);
    void clear();
    [[nodiscard]] size_t size(int32_t bucket) const;
    [[nodiscard]] std::vector<WorldMapSceneObject> snapshot(int32_t bucket) const;

private:
    [[nodiscard]] bool update_object(uint64_t identity,
                                     bool replace_type,
                                     int32_t scene_type,
                                     const std::array<float, 3>& position,
                                     WorldMapScenePositionUpdate* update);
    // Array index zero is the constructor's key -1; indices 1..59 are 0..58.
    std::array<std::vector<WorldMapSceneObject>, 60> buckets_{};
};

struct WorldMapPlayerScenePose {
    int32_t scene_type = 0;
    std::array<float, 3> position{};
    std::array<float, 3> heading{};
};

// The bounded scene-pose fields of FUN_80031758's message 0x1F payload.
// Its camera flag and other side effects are not represented here.
struct WorldMapPlayerSceneMessage1F {
    int32_t scene_type = 0;
    std::array<float, 3> position{};
    std::array<float, 3> heading{};
};

// Applies the supplied message to an already registered player scene node.
// This does not source messages, update game globals, or drive live gameplay.
[[nodiscard]] bool apply_world_map_player_scene_message_1f(
    uint64_t scene_identity,
    const WorldMapPlayerSceneMessage1F& message,
    WorldMapSceneBucketRegistry* registry,
    WorldMapPlayerScenePose* pose,
    WorldMapScenePositionUpdate* update);

// FUN_80013DC8 sends one fixed 0x1F payload to player scene ID 1. This
// applies only its scene-pose effects; the source trigger is not connected.
[[nodiscard]] bool apply_world_map_player_fixed_scene_message_1f(
    WorldMapSceneBucketRegistry* registry,
    WorldMapPlayerScenePose* pose,
    WorldMapScenePositionUpdate* update);

struct WorldMapSceneModeRequestState {
    int32_t mode_58 = 0;
    int32_t previous_mode_5c = 0;
    int32_t state_64 = 0;
    uint8_t scene_byte_78 = 0;
    uint8_t global_flag_59af = 0;
    uint8_t global_flag_59b0 = 0;
};

// The state and global-flag effects of FUN_8017767C. The step-0 and step-3
// callers still need their surrounding scene operations and live ownership.
[[nodiscard]] bool apply_world_map_scene_mode_request(
    WorldMapSceneModeRequestState* state,
    int32_t requested_mode,
    uint8_t scene_byte);

struct WorldMapPlayerFixedTransitionStepState {
    // FUN_8010ACF4 reads these two guards and its sequence step.
    int32_t state_680 = -1;
    int32_t state_58c = 0;
    int32_t sequence_step_4574 = 2; // FUN_80109F20's initial value.
    // FUN_80177FDC clears this byte before sending the fixed message.
    uint8_t scene_byte_79 = 0;
};

// FUN_8010AC5C resets sequence step 2 or 5 to 0 and leaves others alone.
// Returns true only when it resets the step.
[[nodiscard]] bool reset_world_map_player_sequence_step(
    WorldMapPlayerFixedTransitionStepState* state);

// Isolates the step-1 -> step-2 pose/flag effect in FUN_8010ACF4.
// Other steps and the sender's additional scene operations are unsupported.
[[nodiscard]] bool apply_world_map_player_fixed_transition_step(
    WorldMapPlayerFixedTransitionStepState* state,
    WorldMapSceneBucketRegistry* registry,
    WorldMapPlayerScenePose* pose,
    WorldMapScenePositionUpdate* update);

} // namespace awl
