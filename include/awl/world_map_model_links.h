#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace awl {

// Supplied semantic snapshots, not serialized model objects or discovered
// runtime ownership. Whole-node identities preserve shared-child aliases.
struct WorldMapModelLinkNode {
    uint64_t identity = 0;
    uint64_t parent_150 = 0;
    uint32_t flags_158 = 0;
    std::array<uint64_t, 4> children_15c{};
    std::array<uint16_t, 4> attachments_16c{}; // Retained opaque halfwords.
};
struct WorldMapModelLinkState {
    std::vector<WorldMapModelLinkNode> nodes;
};
struct WorldMapModelLinkWrite {
    uint64_t identity = 0;
    uint32_t offset = 0;
    uint64_t value = 0;
};
enum class WorldMapModelLinkStatus { Prepared, Advanced, RequiresNode, InvalidInput };
struct WorldMapModelLinkStep {
    WorldMapModelLinkState after;
    std::vector<WorldMapModelLinkWrite> writes;
    uint64_t required_node = 0;
};

// FUN_8019F89C: visits the root's four slots in order. Each nonnull child
// has +150 cleared before descent; bit 0x4 in that child's +158 gates
// traversal of its four slots. The parent's slot clears after descent.
// Every flag and attachment halfword stays intact. The root parent clears
// only if the root is also reached as a child. Shared nodes use ordered state.
// Missing reached nodes block atomically. Zero/duplicate node keys, null
// root/output, output aliasing and a reached recursive cycle are invalid;
// invalid input preserves output. Unreached child links remain opaque.
// Iterative traversal avoids the original recursion's native stack limit.
// No pose/transform evaluation, release or parent acknowledgement occurs.
[[nodiscard]] WorldMapModelLinkStatus prepare_world_map_model_links_clear(
    const WorldMapModelLinkState& state, uint64_t root, WorldMapModelLinkStep* out);
// Applies only the complete helper to supplied snapshots, never live models.
[[nodiscard]] WorldMapModelLinkStatus advance_world_map_model_links_clear(
    WorldMapModelLinkState* state, uint64_t root, WorldMapModelLinkStep* out);

} // namespace awl
