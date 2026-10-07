#include "awl/world_map_movement_runtime.h"
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace awl {
bool WorldMapMovementRuntime::initialize(const std::array<float, 3>& position,
                                         int32_t scene_type,
                                         const std::array<float, 3>& axis) {
    for (float value : axis) if (!std::isfinite(value)) return false;
    try {
        WorldMapSceneBucketRegistry scene;
        WorldMapScenePositionUpdate update;
        if (!scene.register_object(1, scene_type, position) ||
            !scene.update_position(1, position, &update)) return false;
        scene_ = std::move(scene);
    } catch (const std::bad_alloc&) {
        return false;
    }
    position_ = position;
    axis_ = axis;
    tick_ = 0;
    initialized_ = true;
    pause();
    return true;
}

void WorldMapMovementRuntime::pause() {
    steering_ = {};
    filter_.reset();
    // World-map scene mode 4 has divisor two: max(1,30/2), max(1,6/2).
    filter_.set_repeat_timing(15, 3);
}

bool WorldMapMovementRuntime::tick(const PadSample& sample,
                                   WorldMapMovementQuery query,
                                   WorldMapMovementRuntimeStep* output) {
    if (!initialized_ || !output ||
        tick_ == std::numeric_limits<uint64_t>::max()) return false;
    HsdPadFilter filter = filter_;
    filter.begin_frame(sample);
    query.pad = filter.frame();
    query.current_position = position_;
    query.current_axis = axis_;
    query.steering = steering_;
    WorldMapMovementRuntimeStep next;
    next.tick = tick_ + 1;
    next.pad = filter.frame();
    if (!calculate_world_map_movement_candidate(query, &next.movement))
        return false;
    if (next.movement.movement_enabled) {
        try {
            if (!scene_.update_position(1, next.movement.resolved_position,
                                         &next.scene)) return false;
        } catch (const std::bad_alloc&) {
            return false;
        }
        position_ = next.movement.resolved_position;
        steering_ = next.movement.steering;
    }
    filter_ = filter;
    tick_ = next.tick;
    *output = next;
    return true;
}
} // namespace awl
