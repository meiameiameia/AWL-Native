#pragma once

#include "awl/world_map_model_links.h"
#include "awl/world_map_secondary_model.h"

#include <memory>

namespace awl {
struct WorldMapModelFeatureTexture {
    uint64_t bank_identity = 0; // Borrowed prepared bank, never dereferenced.
    uint16_t index = 0; // Entry at bank +8 base plus index * 8.
};
struct WorldMapModelFeatureCacheEntry {
    uint32_t channel = UINT32_MAX;
    std::optional<WorldMapModelFeatureTexture> texture; // Constructor leaves +4 unknown.
};
struct WorldMapModelFeatureObjectState {
    uint64_t resource_0 = 0, buffer_34 = 0;
    WorldMapModelMatrix matrix_4{};
    uint32_t flags_38 = 2; // Native-owned 27D0 null-arena allocation.
    uint8_t enabled_3c = 1, byte_3d = 0;
    uint64_t metadata_44 = 0;
    std::vector<WorldMapModelFeatureCacheEntry> cache_48;
};
struct WorldMapHeldItemFeatureRow {
    int32_t item_id = 0;
    uint8_t type_0 = 0, group_2 = 0, alternate_group_3 = 0;
    uint16_t texture_4 = 0, texture_6 = 0, alternate_texture_8 = 0, alternate_texture_a = 0;
};
enum class WorldMapHeldItemFeatureBank { Primary, Alternate };
struct WorldMapHeldItemFeatureRoute {
    WorldMapHeldItemFeatureBank bank = WorldMapHeldItemFeatureBank::Primary;
    uint8_t group = 0;
    std::array<uint16_t,2> textures{};
};
struct WorldMapModelFeatureCommand { uint8_t type_0 = 0, channel_1 = 0; };
// Observed selected container entry, not a parser for container/GPL/SKN bytes.
// 2A4C uses (group & FFFF)*8 for the resource and *16 for metadata.
struct WorldMapModelFeatureResourceBinding {
    WorldMapHeldItemFeatureBank bank = WorldMapHeldItemFeatureBank::Primary;
    uint8_t group = 0;
    uint64_t resource_0 = 0, metadata_44 = 0;
    std::optional<std::vector<WorldMapModelFeatureCommand>> commands;
};
struct WorldMapHeldItemFeatureObservations {
    std::optional<WorldMapHeldItemFeatureRow> row;
    std::optional<uint8_t> null_item_alternate_group; // Row zero +3, only on split routes.
    std::optional<WorldMapModelFeatureResourceBinding> resource;
    std::optional<uint64_t> texture_bank_identity; // Selected primary/alternate bank +8 base.
};
enum class WorldMapModelFeatureStatus {
    Prepared, Advanced, Constructed, RequiresRow, RequiresBaseline,
    RequiresResource, RequiresCommands, RequiresTextureBank, InvalidInput, AllocationFailure,
};
struct WorldMapModelFeatureWrite {
    uint32_t offset = 0;
    uint64_t value = 0;
    std::optional<uint32_t> cache_index;
    std::optional<WorldMapModelFeatureTexture> texture; // Typed cache +4 store.
};
struct WorldMapHeldItemFeatureStep {
    WorldMapModelFeatureObjectState after;
    std::optional<WorldMapHeldItemFeatureRoute> route;
    std::vector<WorldMapModelFeatureWrite> writes;
};
// FUN_80031010 -> 80011F2C/1F68 -> 80012030, or 801A2AD8 for item zero.
// Types 1/2/4 and ID 4FF use the split route; matching row-zero +3 selects
// primary fields, otherwise alternate fields. Other types use primary.
// Nonzero item updates reset resource/matrix/buffer/bytes/metadata before
// channel 0 then 1 scans. Type-1 matching commands update first existing
// cache ID, else first FFFFFFFF slot; full cache is a successful no-op.
// Duplicate matches retain repeated stores. Reset keeps cache/flags intact.
// Missing reached observations roll back all writes; invalid preserves output.
// Negative/overflowing item table indices and null reached metadata/bank reject.
// Input/out.after aliases reject. This does not execute commands, decode their
// payloads, release old resources/buffers or bind the real player/global state.
[[nodiscard]] WorldMapModelFeatureStatus prepare_world_map_held_item_feature(
    const WorldMapModelFeatureObjectState& state, int32_t item_id,
    const WorldMapHeldItemFeatureObservations& observations, WorldMapHeldItemFeatureStep* out);

class WorldMapNativeModelFeature;
class WorldMapHeldItemAssets;
// Complete 80012000/27D0 null-resource/null-arena construction with typed
// owned cache storage. Initial player calls use the verified capacity two.
// Payload words remain unknown until binding. Unsafe PPC size wrap rejects;
// catchable allocation failure preserves the existing output owner.
// Successful replacement destroys the old native owner; release its model
// borrowers first. Original feature frees and actor lifetime are untranslated.
[[nodiscard]] WorldMapModelFeatureStatus construct_world_map_native_model_feature(
    uint32_t cache_capacity, std::unique_ptr<WorldMapNativeModelFeature>* out);
// Applies only the complete supported supplied-data item update atomically.
// Resource/texture identities stay borrowed; no private asset bank is acquired.
[[nodiscard]] WorldMapModelFeatureStatus advance_world_map_native_held_item_feature(
    WorldMapNativeModelFeature* feature, int32_t item_id,
    const WorldMapHeldItemFeatureObservations& observations, WorldMapHeldItemFeatureStep* out);
class WorldMapNativeModelFeature {
public:
    WorldMapNativeModelFeature(const WorldMapNativeModelFeature&) = delete;
    WorldMapNativeModelFeature& operator=(const WorldMapNativeModelFeature&) = delete;
    WorldMapNativeModelFeature(WorldMapNativeModelFeature&&) = delete;
    WorldMapNativeModelFeature& operator=(WorldMapNativeModelFeature&&) = delete;
    const WorldMapModelFeatureObjectState& state() const { return state_; }
    uint64_t identity() const noexcept;
    // FUN_801A2A28: a null resource is empty even when a feature exists.
    // Nonempty sources may feed the separate attachment bridge while this
    // owner and its borrowed resource/texture providers remain live.
    std::optional<WorldMapModelSourceChange> attachment_source() const noexcept;
private:
    friend WorldMapModelFeatureStatus construct_world_map_native_model_feature(
        uint32_t, std::unique_ptr<WorldMapNativeModelFeature>*);
    friend WorldMapModelFeatureStatus advance_world_map_native_held_item_feature(
        WorldMapNativeModelFeature*, int32_t, const WorldMapHeldItemFeatureObservations&, WorldMapHeldItemFeatureStep*);
    friend WorldMapModelFeatureStatus advance_world_map_native_held_item_feature_from_assets(
        WorldMapNativeModelFeature*, int32_t, const std::optional<WorldMapHeldItemFeatureRow>&,
        std::optional<uint8_t>, std::shared_ptr<const WorldMapHeldItemAssets>, WorldMapHeldItemFeatureStep*);
    explicit WorldMapNativeModelFeature(WorldMapModelFeatureObjectState state) noexcept;
    WorldMapModelFeatureObjectState state_;
    std::vector<std::shared_ptr<const WorldMapHeldItemAssets>> asset_providers_;
};
} // namespace awl
