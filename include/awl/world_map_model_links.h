#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {

// Supplied semantic snapshots, not serialized model objects or discovered
// runtime ownership. Whole-node identities preserve shared-child aliases.
struct WorldMapModelLinkNode {
    uint64_t identity = 0;
    uint64_t parent_150 = 0;
    std::optional<uint32_t> flags_158 = 0; // Native construction leaves this unknown.
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
enum class WorldMapModelLinkStatus { Prepared, Advanced, RequiresNode, InvalidInput, RequiresFlags };
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
// Missing reached nodes or flags block atomically. Zero/duplicate node keys, null
// root/output, output aliasing and a reached recursive cycle are invalid;
// invalid input preserves output. Unreached child links remain opaque.
// Iterative traversal avoids the original recursion's native stack limit.
// No pose/transform evaluation, release or parent acknowledgement occurs.
[[nodiscard]] WorldMapModelLinkStatus prepare_world_map_model_links_clear(
    const WorldMapModelLinkState& state, uint64_t root, WorldMapModelLinkStep* out);
// Applies only the complete helper to supplied snapshots, never live models.
[[nodiscard]] WorldMapModelLinkStatus advance_world_map_model_links_clear(
    WorldMapModelLinkState* state, uint64_t root, WorldMapModelLinkStep* out);

struct WorldMapModelAttachmentRequest {
    uint64_t primary_model = 0, secondary_model = 0; // Zero is observed null.
    std::optional<uint64_t> feature_c4, auxiliary_model_c8; // Unknown versus observed null.
};
struct WorldMapModelAttachmentBinding {
    uint64_t model = 0;
    // DB4C/DB94: first node with full word +4 != FFFF, or index zero.
    std::optional<uint16_t> first_node_index;
    // A0688 -> already prepared resource +1C: first reached halfword only.
    std::optional<uint16_t> resource_attachment_index;
};
enum class WorldMapModelAttachmentStatus {
    Prepared, Advanced, RequiresNode, RequiresFlags, RequiresSource,
    RequiresNodeIndex, RequiresAttachmentIndex, RequiresFeatureBinding,
    InvalidInput, AllocationFailure,
};
struct WorldMapModelAttachmentStep {
    WorldMapModelLinkState after;
    std::vector<WorldMapModelLinkWrite> writes;
    uint64_t required_model = 0, required_source = 0;
    uint32_t required_field = 0; // Unknown scene +C4/+C8, or model +158.
    std::optional<uint16_t> feature_node_index;
    std::optional<uint32_t> auxiliary_slot, secondary_slot; // No free slot means no attachment stores.
};
// FUN_8017DF68's primary clear and observed-null +C4 path. A nonnull +C8
// selects a secondary node, attaches C8 under secondary at that index,
// clears C8's flag bits 3, then attaches secondary under primary using its
// resource halfword. First free
// slot and all writes/whole-model aliases follow F7B0/F808/FB78 order.
// Nonnull +C4 stops before DBF4's untranslated feature sorting/binding.
// Missing reached evidence rolls back all writes; invalid preserves output.
// Does not own models, interpret whole attachment payloads or evaluate poses.
[[nodiscard]] WorldMapModelAttachmentStatus prepare_world_map_model_attachments(
    const WorldMapModelLinkState& state, const WorldMapModelAttachmentRequest& request,
    const std::vector<WorldMapModelAttachmentBinding>& bindings, WorldMapModelAttachmentStep* out);

} // namespace awl
