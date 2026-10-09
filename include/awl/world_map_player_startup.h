#pragma once
#include "awl/world_map_player_animation_holder.h"

namespace awl {
struct WorldMapPlayerStartupProviders {
    std::shared_ptr<const WorldMapPlayerModelAssets> models;
    std::shared_ptr<const WorldMapPlayerAnimationAssets> animations;
    std::shared_ptr<const WorldMapPlayerInitialAnimationInputs> initial;
    std::shared_ptr<const WorldMapPlayerTimerAssets> timers;
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> starts;
    std::shared_ptr<GameClock> clock;
};
struct WorldMapPlayerStartupQuery {
    WorldMapPlayerInitialAnimationQuery initial;
    // Observations at the post-setup constructor tail, not a save parser.
    WorldMapPlayerConstructorTailQuery tail;
    std::optional<uint8_t> audio_byte_90;
    uint8_t saved_subobject_byte_14c = 0;
    int32_t saved_subobject_word_20 = 0;
    uint32_t item_144_after_setup = 0;
    std::optional<WorldMapPlayerStartItemType> item_type;
};
struct WorldMapPlayerStartupState {
    int32_t state_1364 = 0x29;
    uint32_t counter_1368 = 0;
    uint8_t secondary_byte_3f0 = 1;
};
enum class WorldMapPlayerStartupStatus {
    ConstructedCpuStartup, InitialModelIncomplete, ConstructorTailIncomplete,
    UnsupportedConstructorState, RequiresMessageDelivery, RequiresAudioByte,
    RequiresAudioStop, RequiresModelTypeInputs, RequiresTimerRow,
    AnimationIncomplete, InvalidInput, AllocationFailure
};
struct WorldMapPlayerStartupResult {
    WorldMapPlayerStartupStatus status = WorldMapPlayerStartupStatus::InvalidInput;
    WorldMapPlayerTimedInitialHolderResult initial_model;
    std::optional<WorldMapPlayerConstructorTailStatus> tail_status;
    std::optional<WorldMapPlayerConstructorTail> tail;
    std::optional<WorldMapPlayerModelTypeRow> model_row;
    std::optional<WorldMapAnimationFeatureRow> required_row;
    std::optional<WorldMapPlayerAnimationHolderStartResult> animation;
};
// Owns the supported CPU initial model -> no-message tail -> normal state-29
// callback boundary. Only the three known parent/secondary writes are exposed;
// constructor-unwritten or untouched parent words are not invented. This is
// not a live actor, scene registration, game loop or gameplay acknowledgement.
class WorldMapPlayerStartup {
public:
    WorldMapPlayerStartup(const WorldMapPlayerStartup&) = delete;
    WorldMapPlayerStartup& operator=(const WorldMapPlayerStartup&) = delete;
    const WorldMapPlayerStartupState& state() const { return state_; }
    const WorldMapPlayerAnimationHolder& model() const { return *model_; }
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& starts() const { return starts_; }
    [[nodiscard]] WorldMapPlayerPrimaryUpdateResult advance(const WorldMapPlayerPrimaryFrameInput& input) {
        return model_->advance(input);
    }
private:
    WorldMapPlayerStartup() = default;
    friend WorldMapPlayerStartupResult construct_world_map_player_startup(
        const WorldMapPlayerStartupProviders&, const WorldMapPlayerStartupQuery&, std::unique_ptr<WorldMapPlayerStartup>*);
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> starts_;
    std::unique_ptr<WorldMapPlayerAnimationHolder> model_;
    WorldMapPlayerStartupState state_;
};
// Fresh slot-zero / alternate -1 / type-zero startup only. Supplied phase,
// post-setup guards/binding/item fields and audio byte remain explicit.
// Reached scene-object messages, restored poses, busy bindings, guard state F,
// audio stop, secondary setup and runtime-entry contents are unsupported.
// Ordered model-type timer dependencies precede selector-one initialization;
// only a successful/unchanged callback permits counter reset and +3F0=1.
// Every stop releases staging and preserves *out, its frame/mesh/providers and
// shared clock. Construction never advances time or evaluates/draws a frame.
[[nodiscard]] WorldMapPlayerStartupResult construct_world_map_player_startup(
    const WorldMapPlayerStartupProviders& providers, const WorldMapPlayerStartupQuery& query,
    std::unique_ptr<WorldMapPlayerStartup>* out);
} // namespace awl
