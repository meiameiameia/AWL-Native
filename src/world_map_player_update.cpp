#include "awl/world_map_player_update.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace awl {
WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_entry(
    const WorldMapPlayerUpdateEntry& entry) {
    using S = WorldMapPlayerUpdateStatus;
    if (!entry.control_word) return S::RequiresControlWord;
    if (!entry.guards) return S::RequiresGuardOwner;
    if (entry.guards->word_680 != -1) return S::RequiresGuardTransition;
    if (!entry.guards->word_58c) return S::RequiresGuardWord58c;
    if (*entry.guards->word_58c != 0) return S::RequiresGuardTransition;
    if (!entry.scene_child_word_4) return S::RequiresSceneWord;
    if (*entry.scene_child_word_4 == 1) return S::RequiresSceneTransition;
    return S::Prepared;
}

WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_tail(
    const WorldMapPlayerUpdateOwners& owners, const WorldMapPlayerScenePose& pose,
    uint64_t player_node_identity, WorldMapPlayerUpdateOwners* after) {
    using S = WorldMapPlayerUpdateStatus;
    if (!after) return S::InvalidInput;
    for (float value : pose.position) if (!std::isfinite(value)) return S::InvalidInput;
    for (float value : pose.heading) if (!std::isfinite(value)) return S::InvalidInput;
    if (pose.scene_type == 1 && !owners.scene_one_position_240)
        return S::RequiresScenePositionOwner;
    if (!player_node_identity) return S::RequiresQueueNode;
    // Typed native queue invariants replace intrusive pointer traversal.
    for (size_t i = 0; i < owners.queue.size(); ++i) {
        const auto& item = owners.queue[i];
        if (!item.node_identity || (i && owners.queue[i-1].priority > item.priority))
            return S::InvalidQueue;
        for (size_t j = 0; j < i; ++j)
            if (owners.queue[j].node_identity == item.node_identity) return S::InvalidQueue;
    }
    constexpr uint32_t priority = 0x1010;
    for (const auto& item : owners.queue)
        if (item.node_identity == player_node_identity && item.priority != priority)
            return S::UnsupportedQueuePriorityChange;
    try {
        auto next = owners;
        next.saved_pose = pose;
        if (pose.scene_type == 1) next.scene_one_position_240 = pose.position;
        next.position_802fc20c = pose.position;
        const auto existing = std::find_if(next.queue.begin(), next.queue.end(),
            [player_node_identity](const WorldMapPlayerUpdateQueueEntry& item) {
                return item.node_identity == player_node_identity;
            });
        if (existing != next.queue.end()) next.queue.erase(existing);
        const auto at = std::lower_bound(next.queue.begin(), next.queue.end(), priority,
            [](const WorldMapPlayerUpdateQueueEntry& item, uint32_t key) { return item.priority < key; });
        next.queue.insert(at, {player_node_identity, priority});
        *after = std::move(next);
        return S::Prepared;
    } catch (const std::bad_alloc&) { return S::AllocationFailure; }
}
} // namespace awl
