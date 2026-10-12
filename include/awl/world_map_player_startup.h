#pragma once
#include "awl/world_map_player_animation_holder.h"
#include "awl/world_map_camera.h"
#include "awl/world_map_player_update.h"
#include "awl/world_map_update_inputs.h"

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
    // Established only by a complete supported parent update, not startup.
    std::optional<uint32_t> control_word_1384;
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
struct WorldMapPlayerStartupCameraSeed {
    std::shared_ptr<const WorldMapCameraCollisionAsset> collision;
    // Explicit observation at callback entry, not inferred constructor state.
    WorldMapCameraViewQuery camera{};
};
struct WorldMapPlayerStartupCameraState {
    std::shared_ptr<const WorldMapCameraCollisionAsset> collision;
    WorldMapCameraViewQuery camera{};
    WorldMapCameraPostUpdate update{};
};
struct WorldMapPlayerStartupState29Input {
    std::array<float,3> player_position{};
    std::optional<int32_t> action_148;
    // Required only for the first successful callback; later reseeding rejects.
    std::optional<WorldMapPlayerStartupCameraSeed> first_camera;
};
struct WorldMapPlayerStartupUpdateInput {
    WorldMapPlayerUpdateEntry entry;
    WorldMapPlayerScenePose pose;
    std::optional<int32_t> action_148;
    std::optional<WorldMapPlayerStartupCameraSeed> first_camera;
    uint64_t player_node_identity = 0;
    WorldMapPlayerStatusInputs status_inputs;
};
struct WorldMapPlayerStartupUpdateResult {
    WorldMapPlayerUpdateStatus status = WorldMapPlayerUpdateStatus::InvalidInput;
    // Prepared callback results on a late stop are diagnostic, not published.
    std::optional<WorldMapPlayerState29CameraStep> callback;
    // Complete status proposal on a late tail stop is also diagnostic only.
    std::optional<WorldMapPlayerStatusStep> status_update;
};
// Callback observations only. Control and priority are derived by the owned
// input boundary; callers cannot supply an overriding raw control word.
struct WorldMapPlayerStartupDispatchInput {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    std::optional<int32_t> scene_child_word_4;
    WorldMapPlayerScenePose pose;
    std::optional<int32_t> action_148;
    std::optional<WorldMapPlayerStartupCameraSeed> first_camera;
    uint64_t player_node_identity = 0;
    // Normally use the retained startup tables' initial DOL parameters.
    // An explicit diagnostic override (even empty) replaces that whole snapshot;
    // no missing fields are filled. Phase still comes from the control source.
    std::optional<WorldMapPlayerStatusFactors> status_factors_override;
};
// Owns the supported CPU initial model -> no-message tail -> normal state-29
// callback boundary. Only known parent/secondary writes are exposed;
// constructor-unwritten or untouched parent words are not invented. This is
// not a live actor, scene registration, game loop or gameplay acknowledgement.
class WorldMapPlayerStartup {
public:
    WorldMapPlayerStartup(const WorldMapPlayerStartup&) = delete;
    WorldMapPlayerStartup& operator=(const WorldMapPlayerStartup&) = delete;
    const WorldMapPlayerStartupState& state() const { return state_; }
    const WorldMapPlayerAnimationHolder& model() const { return *model_; }
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& starts() const { return starts_; }
    const std::optional<WorldMapPlayerStartupCameraState>& camera() const { return camera_; }
    // Explicit fresh saved-status boundary: constructor projection + B9E4
    // reset, using retained constants and the phase source at status construction.
    // Later phase observations may change before dispatch; reset never reseeds
    // +12C. The original scene/phase setup order is not activated here.
    // Requires the player's shared clock and an empty status slot. Existing
    // status is never reset on retry. Does not initialize other saved owners,
    // infer post-setup item/action fields, construct a scene or advance time.
    [[nodiscard]] WorldMapPlayerStatusCode initialize_fresh_status(
        const WorldMapUpdateInputs& inputs, WorldMapPlayerUpdateOwners* owners) const noexcept;
    // Own only the normal 34A6C callback's camera/counter result. Position and
    // reached action remain observed inputs. Failure preserves parent/camera,
    // model/frame/mesh and clock; late-stop camera results are diagnostic only.
    // No frame evaluation, clock advance, movement or full parent update.
    [[nodiscard]] WorldMapPlayerState29CameraStep advance_state29_camera(
        const WorldMapPlayerStartupState29Input& input);
    // Normal 31144 state-29 path with supplied guard/scene/control/pose and
    // writable CPU owner counterparts. Without bit 0x800, the status pair
    // requires status/global/rate/phase inputs. Camera/counter/control/
    // status and owner effects commit together;
    // every stop preserves owners and the prior model/frame/camera/clock.
    // This does not discover live owners, consume the queue or schedule ticks.
    [[nodiscard]] WorldMapPlayerStartupUpdateResult advance_state29_parent(
        const WorldMapPlayerStartupUpdateInput& input, WorldMapPlayerUpdateOwners* owners);
    // Mode-four registration's priority 0x5001 through the retained group
    // filter/control snapshot. Filtered nodes skip all callback dependencies.
    // Uses the same shared GameClock as the player. Capture/scene commit are
    // separate earlier boundaries; a callback stop preserves them for retry.
    // No live registration, other manager passes or queue consumer is implied.
    [[nodiscard]] WorldMapPlayerStartupUpdateResult dispatch_state29_parent(
        const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupDispatchInput& input,
        WorldMapPlayerUpdateOwners* owners);
    [[nodiscard]] WorldMapPlayerPrimaryUpdateResult advance(const WorldMapPlayerPrimaryFrameInput& input) {
        return model_->advance(input);
    }
private:
    WorldMapPlayerStartup() = default;
    [[nodiscard]] WorldMapPlayerState29CameraStep prepare_state29_camera(
        const WorldMapPlayerStartupState29Input& input) const;
    friend WorldMapPlayerStartupResult construct_world_map_player_startup(
        const WorldMapPlayerStartupProviders&, const WorldMapPlayerStartupQuery&, std::unique_ptr<WorldMapPlayerStartup>*);
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> starts_;
    std::unique_ptr<WorldMapPlayerAnimationHolder> model_;
    WorldMapPlayerStartupState state_;
    std::optional<WorldMapPlayerStartupCameraState> camera_;
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
