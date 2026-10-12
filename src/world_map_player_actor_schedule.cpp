#include "awl/world_map_player_actor_schedule.h"
#include <algorithm>
#include <new>
#include <utility>

namespace awl {
WorldMapPlayerActorScheduleStatus prepare_world_map_player_actor_schedule(
    const WorldMapPlayerActorMetadata& before, const WorldMapPlayerActorQueue& queue,
    const WorldMapPlayerActorScheduleInput& input, WorldMapPlayerActorSchedule* out) {
    using S = WorldMapPlayerActorScheduleStatus;
    if (!out || (input.screen_flag_15f && *input.screen_flag_15f > 1)) return S::InvalidInput;
    const bool reached_effect = input.screen_flag_15f && *input.screen_flag_15f && before.flag_15d;
    if (reached_effect && !input.effect_gate) return S::RequiresEffectGate;
    const bool effect = reached_effect && *input.effect_gate != 0;
    if (effect && (!input.wrapper_identity || !input.effect_node_identity)) return S::RequiresEffectNode;
    if (!input.render_node_identity) return S::RequiresRenderNode;
    if (effect && input.render_node_identity == input.effect_node_identity) return S::InvalidInput;
    for (size_t i = 0; i < queue.size(); ++i) {
        const auto& entry = queue[i];
        if (!entry.node_identity || (i && queue[i-1].priority > entry.priority)) return S::InvalidQueue;
        for (size_t j = 0; j < i; ++j) if (queue[j].node_identity == entry.node_identity) return S::InvalidQueue;
        if ((entry.node_identity == input.render_node_identity && entry.priority != 0x2002) ||
            (effect && entry.node_identity == input.effect_node_identity && entry.priority != 0x21))
            return S::UnsupportedPriorityChange;
    }
    try {
        WorldMapPlayerActorSchedule next{before,queue,effect};
        next.metadata.flag_15e = 0;
        if (input.screen_flag_15f) next.metadata.flag_15f = *input.screen_flag_15f;
        const auto enqueue = [&](uint64_t key, uint32_t priority) {
            const auto existing = std::find_if(next.queue.begin(),next.queue.end(),
                [key](const auto& entry) { return entry.node_identity == key; });
            if (existing != next.queue.end()) next.queue.erase(existing);
            const auto at = std::lower_bound(next.queue.begin(),next.queue.end(),priority,
                [](const auto& entry, uint32_t value) { return entry.priority < value; });
            next.queue.insert(at,{key,priority});
        };
        if (effect) {
            next.metadata.effect_wrapper = input.wrapper_identity;
            enqueue(input.effect_node_identity,0x21);
        }
        enqueue(input.render_node_identity,0x2002);
        *out = std::move(next);
        return S::Prepared;
    } catch (const std::bad_alloc&) { return S::AllocationFailure; }
}
} // namespace awl
