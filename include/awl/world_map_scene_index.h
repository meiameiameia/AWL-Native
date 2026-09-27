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

} // namespace awl
