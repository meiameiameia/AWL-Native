#pragma once

#include "awl/world_map_player_model_assets.h"
#include "awl/world_map_gpl_metadata.h"
#include "awl/world_map_skin_metadata.h"

namespace awl {
enum class WorldMapPlayerModelAuxiliaryStatus {
    Prepared, RequiresAssets, UnsupportedLayout, InvalidInput, AllocationFailure,
};
struct WorldMapPlayerSkinTarget {
    uint32_t section_index = 0;
    WorldMapModelResourceReference data;
    uint32_t size = 0; // align32(position count * 12), not a decoded vertex stride.
};
// Owns prepared metadata and retains actual archive/texture providers. This
// is NOT the constructed PPC GPL/SKN wrapper or primary model. Only reached
// section/command headers, SKN fixup references, first component-six buffer
// sizing and D58 binding metadata are supported; payloads stay opaque.
class WorldMapPlayerModelAuxiliaryMetadata {
public:
    WorldMapPlayerModelAuxiliaryMetadata(const WorldMapPlayerModelAuxiliaryMetadata&) = delete;
    WorldMapPlayerModelAuxiliaryMetadata& operator=(const WorldMapPlayerModelAuxiliaryMetadata&) = delete;
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets() const { return assets_; }
    const WorldMapGplMetadata& gpl() const { return gpl_; }
    const WorldMapSkinMetadata& skin() const { return skin_; }
    const std::optional<WorldMapPlayerSkinTarget>& skin_target() const { return target_; }
    uint32_t skin_buffer_size() const { return buffer_size_; } // 42F0 includes a null-data first match.
private:
    WorldMapPlayerModelAuxiliaryMetadata() = default;
    friend WorldMapPlayerModelAuxiliaryStatus prepare_world_map_player_model_auxiliary(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&, std::unique_ptr<WorldMapPlayerModelAuxiliaryMetadata>*);
    std::shared_ptr<const WorldMapPlayerModelAssets> assets_;
    WorldMapGplMetadata gpl_;
    WorldMapSkinMetadata skin_;
    std::optional<WorldMapPlayerSkinTarget> target_;
    uint32_t buffer_size_ = 0;
};
// 3C24 -> 3DB8/3CF0 + CD1C/3D58 metadata, before command compilation,
// skin backend setup, primary feature construction and runtime publication.
// D58 chooses the FIRST component-six position header in serialized order;
// a null data pointer does not continue to a later nonnull match. 42F0 sizes
// that same first header regardless of data being null. Other inputs stop
// explicitly. Any failed preparation preserves an existing metadata owner.
[[nodiscard]] WorldMapPlayerModelAuxiliaryStatus prepare_world_map_player_model_auxiliary(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,
    std::unique_ptr<WorldMapPlayerModelAuxiliaryMetadata>* out);
} // namespace awl
