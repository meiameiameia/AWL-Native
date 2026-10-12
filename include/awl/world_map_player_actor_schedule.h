#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {
// Fresh 8007B798 flags. Later actor variants/writers are not activated here.
struct WorldMapPlayerActorMetadata {
    uint8_t flag_15d = 1, flag_15e = 1, flag_15f = 1;
    // Effect node +18 is unwritten until 15C858 schedules it.
    std::optional<uint64_t> effect_wrapper;
};
struct WorldMapPlayerActorQueueEntry {
    uint64_t node_identity = 0;
    uint32_t priority = 0;
};
// Supplied counterpart of manager +48, distinct from its +74 update queue.
// Keys are opaque, process-local and never dereferenced. Discard borrowed keys
// before releasing their owners. No drawing or queue consumption is implied.
using WorldMapPlayerActorQueue = std::vector<WorldMapPlayerActorQueueEntry>;
struct WorldMapPlayerActorScheduleInput {
    // Absent means the first visibility test culled BBA0. Preserve +15F and
    // skip the effect, but still enqueue the player at 2002.
    std::optional<uint8_t> screen_flag_15f;
    std::optional<uint8_t> effect_gate; // r13-5638, read only if +15F/+15D != 0.
    uint64_t wrapper_identity = 0, effect_node_identity = 0, render_node_identity = 0;
};
enum class WorldMapPlayerActorScheduleStatus {
    Prepared, RequiresEffectGate, RequiresEffectNode, RequiresRenderNode,
    InvalidQueue, UnsupportedPriorityChange, InvalidInput, AllocationFailure
};
struct WorldMapPlayerActorSchedule {
    WorldMapPlayerActorMetadata metadata;
    WorldMapPlayerActorQueue queue;
    bool effect_scheduled = false;
};
// Post-frame 31584/BBA0 -> 12BA14/15C858 -> CF0C/D208/C45C/98D0.
// Clear +15E, optionally publish +15F, schedule effect at 21 before player at
// 2002. Unsigned priorities insert before equals; existing same-priority nodes
// move to the front of equals. Changed priorities and cross-queue membership
// remain unsupported. Every stop preserves *out; queue/output aliasing works.
[[nodiscard]] WorldMapPlayerActorScheduleStatus prepare_world_map_player_actor_schedule(
    const WorldMapPlayerActorMetadata& before, const WorldMapPlayerActorQueue& queue,
    const WorldMapPlayerActorScheduleInput& input, WorldMapPlayerActorSchedule* out);
} // namespace awl
