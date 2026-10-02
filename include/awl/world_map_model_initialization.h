#pragma once

#include "awl/world_map_secondary_model.h"

namespace awl {
struct WorldMapModelCoreAllocation { uint32_t offset = 0, size = 0; };
struct WorldMapModelCoreNode {
    uint32_t source_record_index = 0;
    uint8_t type_0 = 0, order_1 = 0;
    uint16_t parent_2 = 0xffff;
    uint32_t value_4 = 0;
    uint64_t feature_8 = 0;
    std::optional<WorldMapModelResourceReference> pose_c;
    uint32_t word_10 = 0;
    // +14 is NOT written by FUN_8019D534. No value is fabricated for it.
};
struct WorldMapModelCorePlayback {
    float position_0 = 0, rate_4 = 1;
    std::optional<WorldMapAnimationClipReference> clip_10;
    uint64_t link_14 = 0;
    // FUN_8019FDE8 leaves +8/+C/+18 unwritten; this is not a complete
    // WorldMapAnimationPlayback and must not be used as one.
};
struct WorldMapModelCore {
    WorldMapModelResource resource;
    // Offsets are relative to the original D330 cursor, not host pointers.
    // Zero-size allocations are retained in the observed order.
    std::vector<WorldMapModelCoreAllocation> allocations;
    uint32_t consumed_size = 0;
    WorldMapModelCorePlayback playback;
    std::vector<WorldMapModelCoreNode> nodes;
    std::vector<WorldMapModelMatrix> inverse_initial_matrices;
    uint64_t auxiliary_c = 0, allocation_10 = 0, feature_14 = 0, head_50 = 0;
    uint8_t byte_1c = 0;
    std::array<uint8_t,4> bytes_114{};
    uint32_t word_118 = 0, word_11c = 0, word_154 = 0;
    uint64_t parent_150 = 0;
    std::array<uint64_t,4> children_15c{};
    std::array<uint16_t,4> attachments_16c{0xffff,0xffff,0xffff,0xffff};
    // +158/+174 and all other absent fields remain unwritten/unknown.
};
enum class WorldMapModelCoreStatus { Prepared, SingularMatrix, InvalidInput };
struct WorldMapModelCoreInitialization {
    std::optional<WorldMapModelCore> core;
    uint32_t required_node_index = 0; // Only for the singular-matrix stop.
};
// FUN_8019D330/D534/FDE8/E848 on an owned-bank prepared resource view.
// Child (+10) before next sibling (+8), exact visited-count equality, original
// parent indices and type-one concatenation. Native traversal is iterative;
// invalid/cyclic/overlong/out-of-table links reject without partial output.
// Shared source records may be visited repeatedly when the final count fits.
// Matrices use observed FMA order and native reciprocal division, as in the
// pose conversion. A singular inverse returns an explicit unaccepted stop;
// the original ignores its SDK failure and retains the forward matrix.
// This prepares core metadata only: no arena binding/allocation, initialized
// opaque fields, complete playback, model publication, skinning or rendering.
[[nodiscard]] WorldMapModelCoreStatus prepare_world_map_model_core(
    const WorldMapPreparedModelResource& resource, WorldMapModelCoreInitialization* out);
} // namespace awl
