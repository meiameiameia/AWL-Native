#pragma once

#include "awl/world_map_animation_initializer.h"

#include <memory>

namespace awl {
enum class WorldMapPlayerAnimationAssetsStatus {
    Loaded, Bound, RequiresAssets, RequiresGroup,
    ReadFailure, UnsupportedLayout, InvalidInput, AllocationFailure,
};

// Owns one supported group (player zero, optional 65/66): primary animations, secondary ACTs,
// and secondary animations. Read-only bank addresses are stable identities.
// Parsing validates archive extents; lookup checks each reached entry's
// supported metadata. Geometry and evaluated poses remain unsupported.
class WorldMapPlayerAnimationAssets {
public:
    WorldMapPlayerAnimationAssets(const WorldMapPlayerAnimationAssets&) = delete;
    WorldMapPlayerAnimationAssets& operator=(const WorldMapPlayerAnimationAssets&) = delete;
    const WorldMapAnimationBank& primary_animations() const { return primary_; }
    const WorldMapModelBank& secondary_models() const { return models_; }
    const WorldMapAnimationBank& secondary_animations() const { return secondary_; }
    uint32_t group_index() const noexcept {return group_;}
private:
    WorldMapPlayerAnimationAssets() = default;
    friend WorldMapPlayerAnimationAssetsStatus load_world_map_player_animation_assets(
        std::shared_ptr<const WorldMapPlayerAnimationAssets>*);
    friend WorldMapPlayerAnimationAssetsStatus load_world_map_optional_animation_assets(
        uint32_t,std::shared_ptr<const WorldMapPlayerAnimationAssets>*);
    uint32_t group_=0;
    WorldMapAnimationBank primary_, secondary_;
    WorldMapModelBank models_;
};

// FUN_80028104 group zero's catalog names at 80248B40/+4/+8. All three
// files stage in original primary/secondary-animation/secondary-model order.
// Existing complete owners skip I/O; failures preserve output ownership.
// Phase activation, archive-manager lookup and other groups remain absent.
[[nodiscard]] WorldMapPlayerAnimationAssetsStatus load_world_map_player_animation_assets(
    std::shared_ptr<const WorldMapPlayerAnimationAssets>* out);
// Reached 298B4/AEB0 descriptors use groups 65 (both dog IDs) and 66 (cats).
[[nodiscard]] WorldMapPlayerAnimationAssetsStatus load_world_map_optional_animation_assets(
    uint32_t group,std::shared_ptr<const WorldMapPlayerAnimationAssets>* out);

struct WorldMapPlayerAnimationGroupBinding {
    // Keeps every returned bank identity/view live after the loader owner drops.
    std::shared_ptr<const WorldMapPlayerAnimationAssets> assets;
    WorldMapActorAnimationGroup primary;
    WorldMapAnimationSecondaryGroup secondary;
    uint64_t secondary_animation_bank_identity = 0;
};

// FUN_8017DB28 selects a 48-byte group from descriptor word zero bits 25..31;
// row +0 supplies primary animations, inline +4 the ACT archive wrapper,
// and +2C the secondary animation bank. Supported owners are player group
// zero and optional groups 65/66. Other groups/missing assets preserve out.
// The existing secondary setup/channel helpers retain their separate lookup,
// saved-playback, arena, construction and (secondary-index-1)&FFFF ordering.
// Binding is metadata, not initializer completion or a live player selection.
[[nodiscard]] WorldMapPlayerAnimationAssetsStatus bind_world_map_player_animation_group(
    uint32_t descriptor_word_0,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    WorldMapPlayerAnimationGroupBinding* out);
} // namespace awl
