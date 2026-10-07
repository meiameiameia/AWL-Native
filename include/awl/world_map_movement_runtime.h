#pragma once
#include "awl/world_map_movement.h"
#include "awl/world_map_scene_index.h"

namespace awl {
struct WorldMapMovementRuntimeStep {
    uint64_t tick = 0;
    HsdPadFrame pad;
    WorldMapMovementCandidate movement;
    WorldMapScenePositionUpdate scene;
};

// Owns the supported movement rehearsal's PAD history, steering, pose and
// scene bucket. Assets, object lists, guards and camera remain supplied.
// This is not original player construction, state dispatch or gameplay.
class WorldMapMovementRuntime {
public:
    [[nodiscard]] bool initialize(const std::array<float, 3>& position,
                                  int32_t scene_type = 0,
                                  const std::array<float, 3>& axis = {0, 0, 1});
    // Only dependency fields of the query are used. PAD, current position,
    // axis and steering come from this owner. Borrowed asset/list views
    // are consumed synchronously and are never retained.
    // Successful guarded ticks advance PAD history but keep pose/steering.
    // A failed tick preserves all owner state and output.
    [[nodiscard]] bool tick(const PadSample& sample,
                            WorldMapMovementQuery dependencies,
                            WorldMapMovementRuntimeStep* output);
    // Native focus policy: clear held PAD/steering, retain scene and pose.
    void pause();
    const std::array<float, 3>& position() const { return position_; }
    const WorldMapSteeringState& steering() const { return steering_; }
    const HsdPadFrame& pad() const { return filter_.frame(); }
    uint64_t tick_count() const { return tick_; }
    const WorldMapSceneBucketRegistry& scene() const { return scene_; }
private:
    WorldMapSceneBucketRegistry scene_;
    HsdPadFilter filter_;
    WorldMapSteeringState steering_;
    std::array<float, 3> position_{};
    std::array<float, 3> axis_{};
    uint64_t tick_ = 0;
    bool initialized_ = false;
};
} // namespace awl
