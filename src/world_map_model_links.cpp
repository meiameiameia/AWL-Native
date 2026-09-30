#include "awl/world_map_model_links.h"

#include <unordered_map>
#include <utility>

namespace awl {
WorldMapModelLinkStatus prepare_world_map_model_links_clear(
    const WorldMapModelLinkState& state, uint64_t root, WorldMapModelLinkStep* out) {
    using Status = WorldMapModelLinkStatus;
    if (out == nullptr || &state == &out->after || root == 0) return Status::InvalidInput;
    std::unordered_map<uint64_t, size_t> indices;
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        if (state.nodes[i].identity == 0 || !indices.emplace(state.nodes[i].identity, i).second) return Status::InvalidInput;
    }
    WorldMapModelLinkStep step;
    step.after = state;
    auto missing = [&](uint64_t identity) {
        step.after = state; step.writes.clear(); step.required_node = identity;
        *out = std::move(step); return Status::RequiresNode;
    };
    const auto first = indices.find(root);
    if (first == indices.end()) return missing(root);
    struct Frame { size_t node = 0, slot = 0; bool awaiting_child = false; };
    std::vector<Frame> stack{{first->second, 0, false}};
    std::vector<bool> active(state.nodes.size(), false);
    active[first->second] = true;
    auto clear_slot = [&](WorldMapModelLinkNode& node, size_t slot) {
        node.children_15c[slot] = 0;
        step.writes.push_back({node.identity, 0x15cu + static_cast<uint32_t>(slot) * 4u, 0});
    };
    while (!stack.empty()) {
        auto& frame = stack.back();
        auto& node = step.after.nodes[frame.node];
        if (frame.awaiting_child) {
            clear_slot(node, frame.slot);
            frame.awaiting_child = false; ++frame.slot;
        } else if (frame.slot == 4) {
            active[frame.node] = false; stack.pop_back();
        } else if (node.children_15c[frame.slot] == 0) {
            ++frame.slot;
        } else {
            const auto found = indices.find(node.children_15c[frame.slot]);
            if (found == indices.end()) return missing(node.children_15c[frame.slot]);
            auto& child = step.after.nodes[found->second];
            child.parent_150 = 0;
            step.writes.push_back({child.identity, 0x150, 0});
            if ((child.flags_158 & 0x4u) != 0) {
                if (active[found->second]) return Status::InvalidInput;
                frame.awaiting_child = true;
                active[found->second] = true;
                stack.push_back({found->second, 0, false});
            } else {
                clear_slot(node, frame.slot); ++frame.slot;
            }
        }
    }
    *out = std::move(step);
    return Status::Prepared;
}

WorldMapModelLinkStatus advance_world_map_model_links_clear(
    WorldMapModelLinkState* state, uint64_t root, WorldMapModelLinkStep* out) {
    if (state == nullptr || out == nullptr || state == &out->after) return WorldMapModelLinkStatus::InvalidInput;
    const auto status = prepare_world_map_model_links_clear(*state, root, out);
    if (status != WorldMapModelLinkStatus::Prepared) return status;
    *state = out->after;
    return WorldMapModelLinkStatus::Advanced;
}
} // namespace awl
