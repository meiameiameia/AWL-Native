#pragma once

#include "awl/world_map_model_feature.h"
#include "awl/tpl.h"

namespace awl {
enum class WorldMapHeldItemAssetsStatus {
    Prepared, Loaded, ReadFailure, TextureFailure, UnsupportedLayout,
    RequiresSkin, InvalidInput, AllocationFailure,
};
struct WorldMapHeldItemGplSection {
    uint32_t offset = 0, name_offset = 0, material_offset = 0;
    std::vector<WorldMapModelFeatureCommand> commands;
};
struct WorldMapHeldItemGplMetadata {
    std::vector<WorldMapHeldItemGplSection> sections;
};
// 3DB8/4078's eight-byte section pairs and retained serialized command
// headers. Immutable metadata only: no GPL relocation, command compilation,
// GX state, geometry, skinning, or drawing. Only types 1/2/3 are supported.
// Shared section offsets, overlapping metadata and missing materials reject.
// Position component-count 6 with nonnull data requires the SKN path.
// Every failure preserves output; catchable allocation failure is reported.
[[nodiscard]] WorldMapHeldItemAssetsStatus prepare_world_map_held_item_gpl_metadata(
    const std::vector<uint8_t>& bytes, WorldMapHeldItemGplMetadata* out);

class WorldMapHeldItemAssets {
public:
    WorldMapHeldItemAssets(const WorldMapHeldItemAssets&) = delete;
    WorldMapHeldItemAssets& operator=(const WorldMapHeldItemAssets&) = delete;
    WorldMapHeldItemAssets(WorldMapHeldItemAssets&&) = delete;
    WorldMapHeldItemAssets& operator=(WorldMapHeldItemAssets&&) = delete;
    const WorldMapHeldItemGplMetadata& metadata(WorldMapHeldItemFeatureBank bank) const;
    size_t texture_count(WorldMapHeldItemFeatureBank bank) const;
    const TplTexture* texture(WorldMapHeldItemFeatureBank bank, uint16_t index) const;
    // Bounds both selected texture indices even when a cache has no slots.
    // Output keeps its supplied row/baseline and receives selected bindings.
    // Keys are stable native identities, not PPC addresses or draw resources.
    [[nodiscard]] bool resolve(const WorldMapHeldItemFeatureRoute& route,
                               WorldMapHeldItemFeatureObservations* out) const;
private:
    friend WorldMapHeldItemAssetsStatus load_world_map_held_item_assets(
        std::shared_ptr<const WorldMapHeldItemAssets>*);
    WorldMapHeldItemAssets() = default;
    struct Bank {
        std::vector<uint8_t> bytes;
        WorldMapHeldItemGplMetadata metadata;
        TplFile textures;
    };
    std::array<Bank,2> banks_;
};
// 80011EA0 loads symbol.gpl, symbol.tpl, real.gpl, real.tpl in this order.
// Uses the mounted native filesystem; no scene/global activation. An existing
// complete owner is retained without I/O, matching the lazy loaded guards.
// A fresh bundle publishes only after all four files validate. Partial loads
// are cleaned up rather than exposed as the original's intermediate globals.
[[nodiscard]] WorldMapHeldItemAssetsStatus load_world_map_held_item_assets(
    std::shared_ptr<const WorldMapHeldItemAssets>* out);
// Resolves the supplied item row against owned files and publishes the
// complete supported feature update. Accepted providers stay alive through
// reset/replacement because retained cache entries may still refer to them.
// Providers are deduplicated and conservatively retained until destruction;
// this does not reproduce the original game's resource-release timing.
[[nodiscard]] WorldMapModelFeatureStatus advance_world_map_native_held_item_feature_from_assets(
    WorldMapNativeModelFeature* feature, int32_t item_id,
    const std::optional<WorldMapHeldItemFeatureRow>& row,
    std::optional<uint8_t> baseline,
    std::shared_ptr<const WorldMapHeldItemAssets> assets, WorldMapHeldItemFeatureStep* out);
} // namespace awl
