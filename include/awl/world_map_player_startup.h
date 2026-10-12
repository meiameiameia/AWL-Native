#pragma once
#include "awl/world_map_player_animation_holder.h"
#include "awl/world_map_camera.h"
#include "awl/world_map_player_update.h"
#include "awl/world_map_update_inputs.h"
#include "awl/world_map_player_actor_prepare.h"
#include "awl/world_map_player_actor_schedule.h"

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
    // Established by a supported parent update or actor callback, not startup.
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
struct WorldMapPlayerStartupActorFrameInput {
    std::optional<uint8_t> saved_byte_29a01;
    std::optional<WorldMapPlayerActorScriptOwner> script;
    // Observed sphere mode is retained for isolated comparisons. Camera and
    // planes remain observations in both modes, not discovered live owners.
    std::optional<WorldMapPlayerActorSphere> sphere;
    std::optional<WorldMapPlayerActorVisibilityOwner> visibility;
    // Observed sphere mode reads placement only if visible. Normal mode
    // reads its position before culling; heading remains unread until visible.
    // An explicit root_pose is unsupported at the reached frame boundary.
    WorldMapPlayerPrimaryFrameInput frame;
    // Translate normal BBA0 bounds from frame.actor_placement.position, then
    // run BOTH visibility tests. Mutually exclusive with an observed sphere.
    // The first test can skip the frame; the screen test only proposes +15F.
    // Camera/plane/script producers still remain supplied observations.
    bool derive_normal_visibility = false;
    std::optional<WorldMapPlayerActorScreenOwner> screen;
};
enum class WorldMapPlayerStartupActorFrameStatus {
    FrameAdvanced, GateStopped, VisibilityStopped, RequiresSphere,
    RequiresVisibilityOwner, RequiresActorPlacement, UnsupportedState, InvalidInput, FrameIncomplete,
    BoundsIncomplete, RequiresScreenOwner, ScreenIncomplete, Prepared
};
struct WorldMapPlayerStartupActorFrameResult {
    WorldMapPlayerStartupActorFrameStatus status = WorldMapPlayerStartupActorFrameStatus::InvalidInput;
    std::optional<WorldMapPlayerActorGateStatus> gate;
    std::optional<WorldMapPlayerActorVisibilityResult> visibility;
    std::optional<WorldMapPlayerPrimaryUpdateResult> frame;
    std::optional<WorldMapPlayerActorBoundsStatus> bounds_status;
    std::optional<WorldMapPlayerActorBounds> bounds;
    std::optional<WorldMapPlayerActorScreenResult> screen;
    // Proposal, including on a later frame failure. advance_gated_actor_frame
    // never publishes it; the complete actor callback publishes on Completed.
    std::optional<uint8_t> proposed_effect_flag_15f;
};
struct WorldMapPlayerStartupActorCallbackInput {
    std::optional<uint32_t> control_word;
    WorldMapPlayerStartupActorFrameInput actor;
    std::optional<uint8_t> effect_gate;
};
enum class WorldMapPlayerStartupActorCallbackStatus {
    Completed, ScriptBlocked, RequiresControl, ActorIncomplete, ScheduleIncomplete,
    UnsupportedState, InvalidInput
};
struct WorldMapPlayerStartupActorCallbackResult {
    WorldMapPlayerStartupActorCallbackStatus status = WorldMapPlayerStartupActorCallbackStatus::InvalidInput;
    // Frame/metadata proposals on a later stop are diagnostic, not published.
    std::optional<WorldMapPlayerStartupActorFrameResult> actor;
    std::optional<WorldMapPlayerActorScheduleStatus> schedule_status;
    std::optional<bool> effect_scheduled;
};
struct WorldMapPlayerStartupQueuedActorInput {
    WorldMapPlayerStartupActorFrameInput actor;
    std::optional<uint8_t> effect_gate;
};
enum class WorldMapPlayerStartupQueueStatus {
    Completed, Empty, UnsupportedNode, UnsupportedPriority, InvalidQueue,
    RequiresControl, CallbackIncomplete, InvalidInput
};
struct WorldMapPlayerStartupQueueResult {
    WorldMapPlayerStartupQueueStatus status = WorldMapPlayerStartupQueueStatus::InvalidInput;
    std::optional<WorldMapPlayerStartupActorCallbackResult> callback;
    std::optional<uint64_t> completed_node;
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
    const WorldMapPlayerActorMetadata& actor_metadata() const { return actor_metadata_; }
    uint64_t update_node_identity() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&update_node_)); }
    uint64_t effect_node_identity() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&effect_node_)); }
    uint64_t render_node_identity() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&render_node_)); }
    uint64_t actor_wrapper_identity() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(model_.get())); }
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
    // CPU composition of the normal state-29 script and first visibility
    // gates before the existing placed-frame transaction. Blocked, culled,
    // unknown and failed cases preserve all player/model/camera/clock state.
    // This is NOT the full 31584/BBA0 callback: it neither publishes the
    // earlier control/+15E stores nor publishes 197614's +15F update, optional
    // effect, holder progression or queue acknowledgement. Derived normal
    // mode adds ordered post-frame F548 texture selection/cache publication;
    // observed-sphere mode retains its isolated frame-only contract.
    [[nodiscard]] WorldMapPlayerStartupActorFrameResult advance_gated_actor_frame(
        const WorldMapPlayerStartupActorFrameInput& input);
    // Supported normal 31584 callback: control -> script -> +15E -> BBA0
    // bounds/visibility/frame/texture features -> optional effect -> player
    // render scheduling. All reached work stages before atomic publication.
    // A blocked script publishes only control. A first-test cull still clears
    // +15E and queues the player, preserving its frame/+15F/effect node.
    // Requires derived normal visibility; observations/producers stay explicit.
    // This does not unlink the +74 update node, execute either render callback,
    // progress holders/clocks, discover a scene or activate gameplay. Discard
    // this owner's queued keys before destroying it. Missing later evidence
    // preserves control, actor, frame/cache and the supplied +48 queue.
    [[nodiscard]] WorldMapPlayerStartupActorCallbackResult advance_state29_actor_callback(
        const WorldMapPlayerStartupActorCallbackInput& input, WorldMapPlayerActorQueue* queue);
    // Bounded D018/D260 -> 31584 -> CFD4/D2C8/C8/989C composition. The
    // front of manager +74 must be this owner's update key at priority 1010,
    // registered by the existing parent API. Use the fresh captured control
    // without primary group filtering; callers cannot override it here.
    // Complete only this node after Completed OR ScriptBlocked; an empty queue
    // needs no callback/control inputs. Unknown earlier work is never skipped.
    // Failure preserves the player and both queues for retry. Other update
    // owner fields remain unchanged. Removing a handled front node immediately
    // is a checked native difference from the original two full passes; later
    // unknown nodes stay pending. No full queue drain, render execution, clock
    // progression or live scene activation is claimed. Discard borrowed keys
    // before destroying this owner.
    [[nodiscard]] WorldMapPlayerStartupQueueResult consume_state29_update(
        const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupQueuedActorInput& input,
        WorldMapPlayerUpdateOwners* updates, WorldMapPlayerActorQueue* render_queue);
    [[nodiscard]] WorldMapPlayerPrimaryUpdateResult advance(const WorldMapPlayerPrimaryFrameInput& input) {
        return model_->advance(input);
    }
    // Isolated F6D4 portion with retained raw clock, not the whole BA94 update.
    [[nodiscard]] WorldMapTextureTimerResult advance_texture_timers() noexcept {
        return model_->advance_texture_timers();
    }
    [[nodiscard]] WorldMapAnimationChannelStatus advance_primary_channel(WorldMapAnimationChannelProgressStep* out) {
        return model_->advance_primary_channel(out);
    }
    [[nodiscard]] WorldMapPlayerHolderProgressResult advance_animation() {
        return model_->advance_animation();
    }
private:
    WorldMapPlayerStartup() = default;
    [[nodiscard]] WorldMapPlayerState29CameraStep prepare_state29_camera(
        const WorldMapPlayerStartupState29Input& input) const;
    [[nodiscard]] WorldMapPlayerStartupActorFrameResult prepare_actor_frame(
        const WorldMapPlayerStartupActorFrameInput& input) const;
    [[nodiscard]] WorldMapPlayerStartupActorCallbackResult advance_actor_callback(
        std::optional<uint32_t> control, const WorldMapPlayerStartupActorFrameInput& actor,
        std::optional<uint8_t> effect_gate, WorldMapPlayerActorQueue* queue);
    friend WorldMapPlayerStartupResult construct_world_map_player_startup(
        const WorldMapPlayerStartupProviders&, const WorldMapPlayerStartupQuery&, std::unique_ptr<WorldMapPlayerStartup>*);
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> starts_;
    std::unique_ptr<WorldMapPlayerAnimationHolder> model_;
    WorldMapPlayerStartupState state_;
    std::optional<WorldMapPlayerStartupCameraState> camera_;
    WorldMapPlayerActorMetadata actor_metadata_;
    // Separate owned tags provide stable keys without reproducing PPC pointers.
    uint8_t update_node_ = 0, effect_node_ = 0, render_node_ = 0;
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
