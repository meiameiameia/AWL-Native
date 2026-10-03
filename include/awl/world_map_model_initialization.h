#pragma once

#include "awl/world_map_secondary_model.h"

#include <memory>

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

enum class WorldMapModelConstructionStatus {
    Constructed, RequiresArenaBinding, RequiresResourcePreparation,
    SingularMatrix, AllocationFailure, InvalidInput,
};
struct WorldMapModelConstructionResult {
    WorldMapModelConstructionStatus status = WorldMapModelConstructionStatus::InvalidInput;
    uint32_t required_index = 0; // Resource record for Euler, core node for singular inverse.
    uint64_t required_arena = 0;
};
class WorldMapNativeModel;
// Consumes a supplied setup proposal, re-resolving/preparing its resource from
// the bank before construction. CD50 assigns flag 4; D0FC's fresh no-feature
// path records its final 0x20 cursor request. A compatible saved playback is
// restored through the caller's FUN_801A040C step without FECC blend resets.
// Unsupported external arenas, resource layouts/Euler and singular matrices
// stop explicitly. All returned failures preserve *out and its existing owner.
// Successful replacement destroys only the old native owner in *out; this
// does not translate CE88 frees of original resource/feature/heap objects.
// No secondary channel, attachment, frame evaluation or actor acknowledgement.
[[nodiscard]] WorldMapModelConstructionResult construct_world_map_secondary_model(
    const WorldMapModelBank& bank, uint32_t index, const WorldMapSecondarySetupStep& setup,
    std::unique_ptr<WorldMapNativeModel>* out);

// Native owner for the CD50/D0FC null-arena, null-secondary-resource path.
// Typed C++ storage replaces the PPC heap/cursor's packed pointer layout.
// It owns a private immutable bank snapshot and the constructed core, so
// clearing/reusing the source bank cannot invalidate resource references.
// No original heap bookkeeping, external arena or live actor is represented.
class WorldMapNativeModel {
public:
    WorldMapNativeModel(const WorldMapNativeModel&) = delete;
    WorldMapNativeModel& operator=(const WorldMapNativeModel&) = delete;
    WorldMapNativeModel(WorldMapNativeModel&&) = delete;
    WorldMapNativeModel& operator=(WorldMapNativeModel&&) = delete;
    const WorldMapModelCore& core() const { return core_; }
    const WorldMapModelBank& bank() const { return *bank_; }
    const std::vector<WorldMapModelCoreAllocation>& storage_requests() const { return storage_requests_; }
    uint32_t consumed_size() const { return core_.consumed_size + 0x20u; }
    uint32_t flags_174() const { return 4; }
    // Fresh construction leaves three playback fields unknown. Only the
    // caller's compatible seven-field restore makes this snapshot complete.
    const std::optional<WorldMapAnimationPlayback>& playback() const { return playback_; }
    // Opaque host identities are valid only while this owner lives. They
    // replace native pointer keys, not serialized PPC addresses or game IDs.
    WorldMapAnimationModelBinding binding() const;
    WorldMapSecondaryModelRecord record() const;
private:
    friend WorldMapModelConstructionResult construct_world_map_secondary_model(
        const WorldMapModelBank&, uint32_t, const WorldMapSecondarySetupStep&,
        std::unique_ptr<WorldMapNativeModel>*);
    WorldMapNativeModel(std::unique_ptr<const WorldMapModelBank> bank, WorldMapModelCore core,
        std::optional<WorldMapAnimationPlayback> playback);
    std::unique_ptr<const WorldMapModelBank> bank_;
    WorldMapModelCore core_;
    std::vector<WorldMapModelCoreAllocation> storage_requests_;
    std::optional<WorldMapAnimationPlayback> playback_;
};
} // namespace awl
