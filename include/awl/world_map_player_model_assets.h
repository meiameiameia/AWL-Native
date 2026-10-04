#pragma once

#include "awl/world_map_secondary_model.h"
#include "awl/tpl.h"

#include <memory>

namespace awl {
enum class WorldMapPlayerModelAssetsStatus {
    Loaded, RequiresPhase, RequiresAssets, RequiresResourcePreparation,
    RequiresAuxiliaryConstruction, ReadFailure, UnsupportedLayout,
    TextureFailure, InvalidInput, AllocationFailure,
};
enum class WorldMapPlayerTextureBank { Body, Eyes, Mouth };
struct WorldMapPlayerModelAssetView {
    WorldMapModelResourceReference reference;
    uint32_t size = 0;
    const uint8_t* data = nullptr; // Borrowed immutable archive extent.
};

// Owns slot zero's phase-selected flat archive. ACT lookup/preparation and
// embedded TPL descriptor/mip bounds are supported. GPL, SKN and optional
// TAM payloads are retained as OPAQUE extents, not decoded/validated bodies.
// Loading this bundle does not construct a primary model or execute skins.
class WorldMapPlayerModelAssets {
public:
    WorldMapPlayerModelAssets(const WorldMapPlayerModelAssets&) = delete;
    WorldMapPlayerModelAssets& operator=(const WorldMapPlayerModelAssets&) = delete;
    WorldMapPlayerModelAssets(WorldMapPlayerModelAssets&&) = delete;
    WorldMapPlayerModelAssets& operator=(WorldMapPlayerModelAssets&&) = delete;
    uint32_t variant() const { return variant_; }
    const WorldMapModelBank& models() const { return models_; }
    size_t file_count() const { return entries_.size(); }
    // FULL archive file node index; node zero is the directory, not an ACT.
    bool resource(uint32_t node_index, WorldMapPlayerModelAssetView* out) const;
    size_t texture_count(WorldMapPlayerTextureBank bank) const;
    const TplTexture* texture(WorldMapPlayerTextureBank bank, uint16_t index) const;
    uint64_t texture_bank_identity(WorldMapPlayerTextureBank bank) const;
private:
    WorldMapPlayerModelAssets() = default;
    friend WorldMapPlayerModelAssetsStatus load_world_map_player_model_assets(
        uint32_t, std::shared_ptr<const WorldMapPlayerModelAssets>*);
    uint32_t variant_ = 0;
    WorldMapModelBank models_;
    std::vector<uint8_t> bytes_;
    struct Entry { uint32_t offset = 0, size = 0; };
    std::vector<Entry> entries_;
    std::array<TplFile,3> textures_;
};

// FUN_8002BBB8's phase-byte table selects catalog rows 0,0,0,1,1,2 for
// player slot zero. Phase is supplied, not read from live global state.
// Nodes 1..6 are required; node 7 is optional. Original lookup order is ACT
// 1, SKN 3, GPL 2, TPL 4/5/6, optional TAM 7. Native staging retains all
// extents without mutating PPC pointers. Publication/replacement is atomic;
// a matching complete variant reuses ownership without I/O.
[[nodiscard]] WorldMapPlayerModelAssetsStatus load_world_map_player_model_assets(
    uint32_t phase, std::shared_ptr<const WorldMapPlayerModelAssets>* out);

struct WorldMapPlayerModelTextureBinding {
    uint8_t channel = 0;
    WorldMapPlayerTextureBank bank = WorldMapPlayerTextureBank::Body;
    uint16_t index = 0;
    uint64_t bank_identity = 0;
};
struct WorldMapPlayerModelSelection {
    std::shared_ptr<const WorldMapPlayerModelAssets> assets;
    // ACT preparation only. Its existing null-auxiliary size metadata does
    // not size the actual primary model's nonnull GPL/SKN construction.
    WorldMapPreparedModelResource model;
    WorldMapPlayerModelAssetView gpl, skin;
    std::optional<WorldMapPlayerModelAssetView> texture_animation;
    std::array<WorldMapPlayerModelTextureBinding,4> textures;
};

// FUN_8002D41C slot-zero path: ACT +28 and nonnull GPL/SKN auxiliary +2C
// precede ordered texture routes FF/body0, 0/eyes0, 1/mouth0, 2/body1.
// Returns ONLY RequiresAuxiliaryConstruction with a complete selection:
// CD50/D0FC with r5 nonnull is not the supported secondary constructor.
// No null-auxiliary substitute, texture cache mutation, holder publication,
// frame evaluation or gameplay acceptance occurs. Other stops preserve out.
[[nodiscard]] WorldMapPlayerModelAssetsStatus prepare_world_map_player_model_selection(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,
    WorldMapPlayerModelSelection* out);
} // namespace awl
