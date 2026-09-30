#pragma once

#include "awl/world_map_animation_channel.h"

namespace awl {
struct WorldMapModelResourceReference { uint64_t bank_identity = 0; uint32_t offset = 0; };
struct WorldMapModelResource {
    WorldMapModelResourceReference reference;
    uint32_t size = 0;
    uint16_t count_6 = 0, field_14 = 0;
    // Isolated FUN_8019D2C0/CF3C arithmetic with secondary argument r5=0.
    // These sizes do not execute the reached FUN_801A04E0 resource fixups.
    uint32_t core_storage_size = 0, allocation_size = 0;
};
// Owns a bounded flat U8 bank, retaining heterogeneous file node order.
// Model lookup uses the FULL signed node index, not animation's low-halfword
// index plus one. Only raw 0x007B7960 header/count/record-table metadata is
// supported. Payload offsets, ACT hierarchy/matrices, GPL/SKN and fixups stay
// opaque. Already-relocated marker 0xFFFFFFFF is unsupported. No bytes mutate.
class WorldMapModelBank {
public:
    [[nodiscard]] bool parse(uint64_t identity, std::vector<uint8_t> bytes);
    void clear();
    [[nodiscard]] bool loaded() const { return identity_ != 0; }
    [[nodiscard]] uint64_t identity() const { return identity_; }
    [[nodiscard]] size_t file_count() const { return entries_.size(); }
    [[nodiscard]] bool resolve(uint32_t node_index, WorldMapModelResource* out) const;
private:
    struct Entry { uint32_t offset = 0, size = 0; };
    uint64_t identity_ = 0;
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
};

struct WorldMapSecondaryModelRecord {
    uint64_t identity = 0;
    uint64_t resource_0 = 0;
    uint32_t count_4 = 0; // Constructor stores resource's count_6, not an asset ID.
    uint64_t auxiliary_c = 0, allocation_10 = 0, feature_14 = 0;
    uint32_t flags_174 = 0;
    uint64_t playback_178 = 0;
};
struct WorldMapSecondaryFeatureRecord {
    uint64_t identity = 0, buffer_34 = 0;
    uint32_t flags_38 = 0;
};
struct WorldMapSecondaryConstructionRequest {
    WorldMapModelResource resource;
    uint64_t arena_identity = 0;
    uint32_t argument_5 = 0; // The initializer always passes zero.
    std::optional<uint32_t> allocation_size; // Only when arena +CC is null.
    uint32_t result_flags_174 = 0; // 4 for allocated storage, otherwise 0.
};
enum class WorldMapSecondarySetupStatus {
    RequiresBank, RequiresModel, RequiresPlayback, RequiresArena,
    RequiresConstruction, InvalidInput,
};
struct WorldMapSecondarySetupStep {
    std::optional<WorldMapModelResource> resource;
    bool retain_playback = false;
    std::optional<WorldMapAnimationPlayback> saved_playback;
    uint64_t required_record = 0;
    std::optional<WorldMapSecondaryConstructionRequest> construction;
};
// FUN_8017DC4C..DCDC: owned resource lookup, full count equality, then
// complete seven-field playback save only on equality, followed by the
// typed construction request. It never reconstructs a model, restores the
// saved playback, frees an old model, or executes a secondary channel.
// Missing evidence is ordered; reached nonfinite save fields and invalid keys
// fail without changing output. Unused model/playback/arena fields are ignored.
[[nodiscard]] WorldMapSecondarySetupStatus prepare_world_map_secondary_model_setup(
    uint32_t index, uint64_t bank_identity, const WorldMapModelBank* bank,
    uint64_t model_identity, const std::optional<WorldMapSecondaryModelRecord>& model,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::optional<uint64_t>& arena, WorldMapSecondarySetupStep* out);

enum class WorldMapSecondaryReleaseKind { Asset, Auxiliary, Allocation };
struct WorldMapSecondaryReleaseCall {
    WorldMapSecondaryReleaseKind kind = WorldMapSecondaryReleaseKind::Asset;
    uint64_t owner_identity = 0, resource_identity = 0;
    uint32_t argument_offset = 0;
};
enum class WorldMapSecondaryReleaseStatus { Prepared, RequiresModel, RequiresFeature, RequiresBackend, InvalidInput };
struct WorldMapSecondaryReleaseStep {
    uint64_t wrapper_after = 0;
    std::optional<WorldMapSecondaryModelRecord> model_after;
    std::optional<WorldMapSecondaryFeatureRecord> feature_after;
    std::optional<WorldMapSecondaryReleaseCall> required_call;
    uint64_t required_record = 0;
};
// FUN_8019CE88 / FUN_801A361C, stopping BEFORE the first untranslated free.
// Only paths needing no asset/auxiliary/allocation free can prepare the final
// wrapper-null and feature-buffer clears. Blocked paths retain input snapshots;
// invalid input preserves output. Flags are tested in original order, without
// classifying pointer ownership from nonzero values alone. No success word,
// actual heap free, live owner mutation or parent acknowledgement is accepted.
[[nodiscard]] WorldMapSecondaryReleaseStatus prepare_world_map_secondary_model_release(
    uint64_t model_identity, const std::optional<WorldMapSecondaryModelRecord>& model,
    const std::optional<WorldMapSecondaryFeatureRecord>& feature, WorldMapSecondaryReleaseStep* out);
} // namespace awl
