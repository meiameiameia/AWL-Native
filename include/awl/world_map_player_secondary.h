#pragma once

#include "awl/world_map_player_animation_assets.h"
#include "awl/world_map_animation_channel_owner.h"
#include "awl/world_map_model_initialization.h"

namespace awl {
enum class WorldMapPlayerSecondaryStatus {
    ConstructedCpuSecondary, RequiresSelection, RequiresAssets, RequiresGroup,
    SetupIncomplete, ConstructionIncomplete, ChannelIncomplete, InvalidInput, AllocationFailure,
};
struct WorldMapPlayerSecondaryResult {
    WorldMapPlayerSecondaryStatus status = WorldMapPlayerSecondaryStatus::InvalidInput;
    uint32_t selected_index = 127;
    std::optional<WorldMapSecondarySetupStatus> setup_status;
    std::optional<WorldMapModelConstructionResult> construction;
    std::optional<WorldMapAnimationChannelStatus> channel_status;
    bool applying_settings = false;
    uint64_t required_record = 0;
    uint32_t required_fields = 0;
    // Diagnostics describe staged work. Only ConstructedCpuSecondary publishes.
};

// Owns one fresh, null-arena group-zero secondary model and its channel.
// Combines the existing DC4C..DD2C construction/setup path with the secondary
// DE18..DE94 settings calls, over explicitly supplied common loop/rate values.
// The model privately retains its ACT and selected animation bank. Borrowed
// blend keys remain within this owner; model destruction precedes the channel.
// This does not restore/rebind an existing holder, translate CE88 release,
// attach/evaluate/render the model or accept a whole DB28/player update.
class WorldMapPlayerSecondaryOwner {
public:
    WorldMapPlayerSecondaryOwner(const WorldMapPlayerSecondaryOwner&) = delete;
    WorldMapPlayerSecondaryOwner& operator=(const WorldMapPlayerSecondaryOwner&) = delete;
    const WorldMapNativeModel& model() const { return *model_; }
    const WorldMapNativeAnimationChannel& channel() const { return *channel_; }
    uint32_t selected_index() const { return selected_index_; }
    // One isolated 69C4, atomically publishing model/channel playback only.
    // Completion is returned, not applied to holder policy; no shared clock.
    [[nodiscard]] WorldMapAnimationChannelStatus progress(WorldMapAnimationChannelProgressStep* out);
private:
    WorldMapPlayerSecondaryOwner() = default;
    friend WorldMapPlayerSecondaryResult construct_world_map_player_secondary(
        const std::shared_ptr<const WorldMapPlayerAnimationAssets>&, uint32_t, uint32_t,
        uint32_t, float, std::unique_ptr<WorldMapPlayerSecondaryOwner>*);
    uint32_t selected_index_ = 127;
    std::unique_ptr<WorldMapNativeAnimationChannel> channel_;
    std::unique_ptr<WorldMapNativeModel> model_;
};

// Explicit fresh-selection operation. Index 127 requires no secondary and
// returns RequiresSelection without asset access. Index zero is unsupported.
// All failures preserve *out, including a late settings/allocation failure.
// Success replaces only a native owner; no original rebind/save semantics.
[[nodiscard]] WorldMapPlayerSecondaryResult construct_world_map_player_secondary(
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    uint32_t descriptor_word_0, uint32_t descriptor_word_4, uint32_t loop, float rate,
    std::unique_ptr<WorldMapPlayerSecondaryOwner>* out);
} // namespace awl
