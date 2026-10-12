#pragma once
#include "awl/world_map_scene_index.h"
#include "awl/world_map_player_status.h"
#include <optional>

namespace awl {
struct WorldMapPlayerUpdateGuards {
    int32_t word_680 = -1;
    // Read only when +680 is -1.
    std::optional<int32_t> word_58c;
};
struct WorldMapPlayerUpdateEntry {
    std::optional<uint32_t> control_word;
    std::optional<WorldMapPlayerUpdateGuards> guards;
    // Scene global -> +4C -> +4, distinct from scene type +68.
    std::optional<int32_t> scene_child_word_4;
};
struct WorldMapPlayerUpdateQueueEntry {
    uint64_t node_identity = 0; // Opaque nonzero key, never a host pointer.
    uint32_t priority = 0;
};
// Supplied CPU counterparts of the reached global owners. No save-file
// format, live globals, renderer or queue processing is implied. The caller
// may consume nodes between updates. A node already in this queue at 1010
// is unlinked/reinserted before equals, as in C3F4 -> 989C. Other queues and
// changing an existing node's priority are outside this supplied boundary.
struct WorldMapPlayerUpdateOwners {
    WorldMapPlayerScenePose saved_pose; // +2A290/+29C78/+29F84
    std::array<float,3> position_802fc20c{}; // 8012B004, ignores its r3 owner
    // Presence establishes the owner needed by 8014FDA8 on scene type one.
    std::optional<std::array<float,3>> scene_one_position_240;
    std::vector<WorldMapPlayerUpdateQueueEntry> queue;
    // Supplied snapshot or explicitly initialized fresh projection, needed
    // only without control bit 800. Other saved-owner fields remain separate.
    std::optional<WorldMapPlayerStatusState> status;
};
enum class WorldMapPlayerUpdateStatus {
    Prepared, Advanced, RequiresControlWord, RequiresGuardOwner,
    RequiresGuardWord58c, RequiresGuardTransition, RequiresSceneWord,
    RequiresSceneTransition, RequiresStatusUpdate, RequiresEffectOwners,
    RequiresScenePositionOwner, RequiresQueueNode, InvalidQueue,
    UnsupportedQueuePriorityChange, CameraIncomplete, UnsupportedState, InvalidInput,
    AllocationFailure, SkippedByDispatchFilter, RequiresDispatchControl
};
// FUN_80031144's pre-callback decisions, restricted to state 29. The guard
// transition and scene transition are separate, ordered dependencies.
// No state F request, callback or write is acknowledged here.
[[nodiscard]] WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_entry(
    const WorldMapPlayerUpdateEntry& entry);

// Only 313C4..31464 on state 29, including the complete position-copy
// callees and 17CFB4/D324/D37C/C3F4 sorted insertion. Unsigned priorities
// insert before equals. The pose is observed at tail entry, not a movement
// proposal. Invalid/nonfinite pose, invalid queue, unsupported priority change or caught
// allocation failure preserves *after; input/output aliasing is supported.
// No queue consumer, live scene ownership or full game loop is activated.
[[nodiscard]] WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_tail(
    const WorldMapPlayerUpdateOwners& owners, const WorldMapPlayerScenePose& pose,
    uint64_t player_node_identity, WorldMapPlayerUpdateOwners* after);
} // namespace awl
