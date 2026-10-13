#pragma once
#include "awl/world_map_player_animation_holder.h"
#include "awl/world_map_camera.h"
#include "awl/world_map_player_update.h"
#include "awl/world_map_update_inputs.h"
#include "awl/world_map_player_actor_prepare.h"
#include "awl/world_map_player_actor_schedule.h"
#include "awl/world_map_fade.h"
#include "awl/world_map_player_state0_third.h"

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

// Native receipt for this owned pool slot, not the entire original pool/pass.
// Raw time is retained for diagnostics; loop_count distinguishes zero-time loops.
struct WorldMapPlayerAnimationLoop {
    uint32_t loop_count = 0, raw_time = 0;
    bool valid = true;
};
enum class WorldMapPlayerAnimationLoopStatus {
    Advanced, AlreadyAdvanced, RequiresControl, RequiresModelTail,
    AnimationIncomplete, InvalidInput, RequiresNextLoop, Filtered
};
struct WorldMapPlayerAnimationLoopResult {
    WorldMapPlayerAnimationLoopStatus status = WorldMapPlayerAnimationLoopStatus::InvalidInput;
    std::optional<WorldMapPlayerHolderProgressResult> animation;
};
enum class WorldMapPlayerPrimarySliceStatus {
    Completed, RequiresControl, ParentIncomplete, AnimationIncomplete,
    RequiresNextLoop, InvalidInput
};
struct WorldMapPlayerPrimaryLoop {
    uint32_t loop_count = 0, raw_time = 0;
    bool valid = true;
    // Borrowed identity only. Keep this update owner's storage alive through
    // queued completion; no pointer is reconstructed or dereferenced by key.
    uint64_t updates_owner = 0;
    // Only this owned player's second/third callbacks and the manager's
    // empty second/third callbacks; not other nodes or scene work.
    bool followups_completed = false;
    uint64_t fade_owner = 0; // bounded startup-tick source binding
    bool queued_completed = false;
    uint64_t inputs_owner = 0; // same captured-control owner through completion
    std::optional<int32_t> pending_state_request;
    bool state0_transition_completed = false;
    bool state0_parent_completed = false, state0_secondary_completed = false;
};
struct WorldMapPlayerPrimarySliceResult {
    WorldMapPlayerPrimarySliceStatus status = WorldMapPlayerPrimarySliceStatus::InvalidInput;
    std::optional<WorldMapPlayerStartupUpdateResult> parent;
    std::optional<WorldMapPlayerAnimationLoopResult> animation;
    bool parent_reused = false;
};
enum class WorldMapPlayerPrimaryPassesStatus {
    Completed, AlreadyCompleted, RequiresControl, RequiresPrimary,
    RequiresAnimation, SecondaryIncomplete, TertiaryIncomplete,
    TransitionRequired, InvalidInput
};
struct WorldMapPlayerPrimaryPassesResult {
    WorldMapPlayerPrimaryPassesStatus status = WorldMapPlayerPrimaryPassesStatus::InvalidInput;
    std::optional<WorldMapPlayerState29SecondaryResult> secondary;
    std::optional<WorldMapPlayerFollowupStatus> tertiary;
    bool filtered = false;
};
enum class WorldMapPlayerStartupQueueStatus {
    Completed, Empty, UnsupportedNode, UnsupportedPriority, InvalidQueue,
    RequiresControl, CallbackIncomplete, InvalidInput, RequiresAnimation, RequiresPrimary, RequiresPrimaryPasses,
    RequiresThird, AlreadyCompleted
};
struct WorldMapPlayerStartupQueueResult {
    WorldMapPlayerStartupQueueStatus status = WorldMapPlayerStartupQueueStatus::InvalidInput;
    std::optional<WorldMapPlayerStartupActorCallbackResult> callback;
    std::optional<uint64_t> completed_node;
};
struct WorldMapPlayerStartupTickInput {
    WorldMapPlayerStartupDispatchInput parent;
    WorldMapPlayerState29SecondaryInput secondary;
    std::optional<WorldMapPlayerUpdateGuards> tertiary_guards;
    WorldMapPlayerStartupQueuedActorInput actor;
};
enum class WorldMapPlayerStartupTickStatus {
    Completed, PrimaryIncomplete, FadeIncomplete, PassesIncomplete,
    TransitionRequired, QueueIncomplete, RequiresNextLoop, InvalidInput
};
struct WorldMapPlayerStartupTickResult {
    WorldMapPlayerStartupTickStatus status = WorldMapPlayerStartupTickStatus::InvalidInput;
    std::optional<WorldMapPlayerPrimarySliceResult> primary;
    std::optional<WorldMapFadeStatus> fade;
    std::optional<WorldMapPlayerPrimaryPassesResult> passes;
    std::optional<WorldMapPlayerStartupQueueResult> queued;
    bool queue_reused = false;
};
struct WorldMapPlayerState0TransitionInput {
    std::optional<uint8_t> saved_byte_14d35, secondary_byte_3f1;
    std::optional<int32_t> scene_type_68;
    std::optional<uint8_t> audio_byte_90;
    std::optional<uint8_t> saved_subobject_byte_14c;
    std::optional<uint32_t> item_144;
    std::optional<bool> item_binding_1454;
    std::optional<int32_t> action_148;
    std::optional<bool> action_binding_3ec;
};
enum class WorldMapPlayerState0TransitionStatus {
    Prepared, Advanced, AlreadyAdvanced, RequiresControl, RequiresPendingTransition,
    RequiresAnimation, RequiresFade, RequiresSavedRemapByte, RequiresSecondaryRemapByte,
    UnsupportedRemappedState, RequiresSceneType, RequiresAudioByte, RequiresAudioStop,
    RequiresModelByte, RequiresStatus, RequiresPhaseRaw, RequiresModelType,
    RequiresTimerRow, RequiresItem, RequiresItemBinding, RequiresItemAttachment,
    RequiresAction, RequiresActionBinding, RequiresActionAttachment,
    RequiresHolderCompleted, RequiresHolderFlag, InvalidInput, AllocationFailure,
    UnsupportedNumerics
};
struct WorldMapPlayerState0TransitionResult {
    WorldMapPlayerState0TransitionStatus status = WorldMapPlayerState0TransitionStatus::InvalidInput;
    std::optional<int32_t> remapped_state;
    std::optional<WorldMapPlayerModelTypeRow> model_row;
    // Preparation on a late dependency stop is diagnostic, not published.
    std::optional<WorldMapAnimationFeatureStep> model;
};
struct WorldMapPlayerState0InitializerResult {
    WorldMapPlayerState0TransitionStatus status = WorldMapPlayerState0TransitionStatus::InvalidInput;
    std::optional<uint8_t> secondary_byte_3f0;
};
struct WorldMapPlayerState0ParentInput {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    std::optional<int32_t> scene_child_word_4;
    WorldMapPlayerScenePose pose;
    std::optional<uint8_t> saved_subobject_byte_14c;
    std::optional<WorldMapPlayerStatusFactors> status_factors_override;
};
struct WorldMapPlayerState0ParentResult {
    WorldMapPlayerUpdateStatus status=WorldMapPlayerUpdateStatus::InvalidInput;
    std::optional<WorldMapPlayerState0TransitionResult> appearance;
    std::optional<WorldMapPlayerStatusStep> status_update;
};
struct WorldMapPlayerState0PrimarySliceResult {
    WorldMapPlayerPrimarySliceStatus status=WorldMapPlayerPrimarySliceStatus::InvalidInput;
    std::optional<WorldMapPlayerState0ParentResult> parent;
    std::optional<WorldMapPlayerAnimationLoopResult> animation;
    bool parent_reused=false;
};
// 42B18's no-reached-attachment path, then 31F1C..31F60 for state zero.
// Remap/audio/model observations in input are unused here. Item attachment
// takes precedence over action; completed flag can skip the holder +21 read.
[[nodiscard]] WorldMapPlayerState0InitializerResult prepare_world_map_player_state0_initializer(
    const WorldMapPlayerState0TransitionInput&,std::optional<uint8_t> completed_20,
    std::optional<uint8_t> flag_21,uint8_t prior_secondary_byte_3f0) noexcept;
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
    const std::optional<WorldMapPlayerAnimationLoop>& animation_loop() const { return animation_loop_; }
    const std::optional<WorldMapPlayerPrimaryLoop>& primary_loop() const { return primary_loop_; }
    const std::optional<WorldMapPlayerContextUpdateState>& state0_context() const {return state0_context_;}
    const std::optional<WorldMapPlayerState0ThirdLoop>& state0_third_loop() const {return state0_third_loop_;}
    // One explicit observed context projection after the supported transition,
    // before any third-pass receipt. Does not translate UI construction.
    [[nodiscard]] bool seed_state0_context(const WorldMapPlayerContextUpdateState&) noexcept;
    // Current startup transition OR normal parent/model/second prerequisites,
    // same input owner, guarded idle third and once-per-loop completion.
    // Normal completion acknowledges only this owned pair's later passes;
    // queued execution and other nodes/scene work remain separate.
    [[nodiscard]] WorldMapPlayerState0ThirdResult advance_state0_third(
        const WorldMapUpdateInputs&,const WorldMapPlayerContextUpdateSources&,
        const WorldMapPlayerState0ThirdInput&) noexcept;
    // Supported 31144 -> 42B80 model refresh -> status pair -> pose/queue
    // tail, followed by owned F000 progression. Current status/phase select
    // appearance before the status changes. Every callback is atomic; a late
    // model failure reuses the accepted parent. No movement/camera update.
    [[nodiscard]] WorldMapPlayerState0ParentResult dispatch_state0_parent(
        const WorldMapUpdateInputs&,const WorldMapPlayerState0ParentInput&,WorldMapPlayerUpdateOwners*);
    [[nodiscard]] WorldMapPlayerState0PrimarySliceResult advance_state0_primary_slice(
        const WorldMapUpdateInputs&,const WorldMapPlayerState0ParentInput&,WorldMapPlayerUpdateOwners*);
    // Normal state-zero first/model receipts must precede this second pass.
    // Holder bytes come from the owned model. Only retention/guard/filter
    // completes; movement and state-one requests preserve state explicitly.
    [[nodiscard]] WorldMapPlayerState0SecondaryResult advance_state0_secondary(
        const WorldMapUpdateInputs&,const std::optional<WorldMapPlayerUpdateGuards>&) noexcept;
    // Either the startup transition's completed idle third or the normal
    // first/second/third sequence must precede the shared 31584 actor path.
    // Saved +29A01 != 0 still requires untranslated UI delivery. Successful
    // completion removes only this owned front node and retains a retry receipt.
    [[nodiscard]] WorldMapPlayerStartupQueueResult consume_state0_update_after_third(
        const WorldMapUpdateInputs&,const WorldMapPlayerStartupQueuedActorInput&,
        WorldMapPlayerUpdateOwners*,WorldMapPlayerActorQueue*);
    // Fixed primary-list registrations observed at 14448 and 12B58. Keys
    // represent this bounded native parent/model-manager pair; this does not
    // register the original manager's other models or activate a shared list.
    std::array<WorldMapPlayerUpdateQueueEntry,2> primary_registration() const noexcept {
        return {{{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&primary_node_)),0x5001},
            {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&model_manager_node_)),0xf000}}};
    }
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
    // Eligible first-slot portion of EBBC -> ED7C -> BA94: advance the primary
    // and selected-secondary channels, holder and timers once per fresh captured loop.
    // Pool +90's separate feature tail must be null. A stop preserves model
    // and receipt. Retry after acceptance returns AlreadyAdvanced, including
    // after a later frame failure. Control is a freshness check; BA94 does not
    // consume its bits. The manager's registration/filter, other pool entries
    // and other primary passes remain dependencies; this does not dispatch the
    // whole manager or advance time. Call only for an eligible owned slot.
    [[nodiscard]] WorldMapPlayerAnimationLoopResult advance_animation_for_loop(const WorldMapUpdateInputs& inputs);
    // Bind the traced F000 registration/group to the eligible-slot operation.
    // Supported scene masks leave group 15 unblocked. A future blocked mask
    // is an explicit stop, not invented successful animation progression.
    [[nodiscard]] WorldMapPlayerAnimationLoopResult dispatch_animation_for_loop(const WorldMapUpdateInputs& inputs);
    // Ordered owned primary slice: parent 5001 -> model manager F000, before
    // the checked queued actor callback. Derives this owner's update key;
    // contradictory supplied keys reject on the reached unfiltered branch.
    // Each callback commits atomically. If animation fails after an accepted
    // parent, retry reuses that parent result without repeating counter/status
    // or queue writes. This is not a full primary pass: other nodes/pool slots,
    // secondary/tertiary passes, scene callbacks and render/clock work remain.
    [[nodiscard]] WorldMapPlayerPrimarySliceResult advance_state29_primary_slice(
        const WorldMapUpdateInputs&,const WorldMapPlayerStartupDispatchInput&,WorldMapPlayerUpdateOwners*);
    // After the accepted first-pass pair, handle the owned player 31484/31504
    // callbacks in order. The model-manager callbacks 9540/9538 are empty.
    // Independent guard observations allow changes between the two passes.
    // Reached transitions stop BEFORE 31D7C; no state initializer is invented.
    // Unknown later dependencies preserve control and the completion receipt.
    // This does not acknowledge other primary nodes/pool slots or scene work.
    [[nodiscard]] WorldMapPlayerPrimaryPassesResult complete_state29_primary_passes(
        const WorldMapUpdateInputs&,const WorldMapPlayerState29SecondaryInput&,
        const std::optional<WorldMapPlayerUpdateGuards>& tertiary_guards) noexcept;
    // Same bounded front-node callback/completion, additionally requiring the
    // owned animation slot's accepted receipt for this loop. Empty queues still
    // skip every dependency; invalid/foreign earlier nodes are never skipped.
    // Frame failure retains the already accepted animation and pending node.
    // No primary-pass completion, render execution or clock advance is implied.
    [[nodiscard]] WorldMapPlayerStartupQueueResult consume_state29_update_after_animation(
        const WorldMapUpdateInputs& inputs, const WorldMapPlayerStartupQueuedActorInput& input,
        WorldMapPlayerUpdateOwners* updates, WorldMapPlayerActorQueue* render_queue);
    // Require both accepted ordered primary callbacks, then reuse the same
    // front-node frame/scheduling transaction. Other primary nodes/passes are
    // not acknowledged by these two owned receipts.
    [[nodiscard]] WorldMapPlayerStartupQueueResult consume_state29_update_after_primary_slice(
        const WorldMapUpdateInputs&,const WorldMapPlayerStartupQueuedActorInput&,
        WorldMapPlayerUpdateOwners*,WorldMapPlayerActorQueue*);
    // Additionally require the owned second/third callbacks. The weaker APIs
    // above remain isolated diagnostics, not complete loop-prefix evidence.
    [[nodiscard]] WorldMapPlayerStartupQueueResult consume_state29_update_after_primary_passes(
        const WorldMapUpdateInputs&,const WorldMapPlayerStartupQueuedActorInput&,
        WorldMapPlayerUpdateOwners*,WorldMapPlayerActorQueue*);
    // Bounded startup tick: player5001 -> modelF000 -> fade10000 -> owned
    // second/third player callbacks -> owned-front queued actor completion.
    // All owners share one captured clock sample. Secondary mode is derived
    // from fade, with a contradictory reached diagnostic override rejected.
    // Each accepted earlier callback survives late failure; retries reuse it.
    // Fade must already have a justified start; constructor mode zero alone
    // cannot bypass the world-map entry fade. Keep fade/update/render storage
    // and captured-input storage alive through the loop's completion.
    // A separately completed later pass cannot be retroactively preceded by
    // fade. Reached state transitions stop before initialization. Completed
    // does not acknowledge other primary nodes, scene work, drawing or time.
    [[nodiscard]] WorldMapPlayerStartupTickResult advance_state29_startup_tick(
        const WorldMapUpdateInputs&,const WorldMapPlayerStartupTickInput&,
        WorldMapFade*,WorldMapPlayerUpdateOwners*,WorldMapPlayerActorQueue*);
    // Only a zero request retained by the composed startup tick. Owns the
    // no-remap/no-audio-stop/no-reached-attachment 31D7C -> 42B18 transition.
    // Derives model selector from the owned pointed status +20 and supplied
    // reached +14C; phase comes from the current input owner's raw phase.
    // Model type/timers, parent state/control and secondary +3F0 publish
    // atomically. Counter, playback, pose, CPU mesh, queues and clock survive.
    // State zero's changed third/queued callbacks remain separate dependencies.
    [[nodiscard]] WorldMapPlayerState0TransitionResult advance_pending_state0_transition(
        const WorldMapUpdateInputs&,const WorldMapPlayerState0TransitionInput&,
        const WorldMapFade*,WorldMapPlayerUpdateOwners*);
    [[nodiscard]] WorldMapPlayerPrimaryUpdateResult advance(const WorldMapPlayerPrimaryFrameInput& input) {
        return model_->advance(input);
    }
    // Isolated diagnostic mutations invalidate coordinated proof while keeping
    // the handled loop key. That loop cannot be advanced a second time; resume
    // coordinated work only after a new clock loop and fresh control capture.
    // F6D4 portion with retained raw clock, not the whole BA94 update.
    [[nodiscard]] WorldMapTextureTimerResult advance_texture_timers() noexcept {
        auto result=model_->advance_texture_timers();
        if (result.status==WorldMapTextureTimerStatus::Prepared && animation_loop_) animation_loop_->valid=false;
        return result;
    }
    [[nodiscard]] WorldMapAnimationChannelStatus advance_primary_channel(WorldMapAnimationChannelProgressStep* out) {
        const auto status=model_->advance_primary_channel(out);
        if (status==WorldMapAnimationChannelStatus::Advanced && animation_loop_) animation_loop_->valid=false;
        return status;
    }
    [[nodiscard]] WorldMapPlayerHolderProgressResult advance_animation() {
        auto result=model_->advance_animation();
        if (result.status==WorldMapPlayerHolderProgressStatus::Advanced && animation_loop_) animation_loop_->valid=false;
        return result;
    }
    [[nodiscard]] bool configure_animation_repeats(WorldMapPlayerHolderRepeatKind kind,uint32_t value) noexcept {
        return model_->configure_repeats(kind,value);
    }
private:
    enum class QueuePrerequisite { None, Animation, Primary, Passes, State0Third };
    [[nodiscard]] WorldMapPlayerStartupQueueResult consume_update(
        const WorldMapUpdateInputs&,const WorldMapPlayerStartupQueuedActorInput&,
        WorldMapPlayerUpdateOwners*,WorldMapPlayerActorQueue*,QueuePrerequisite);
    WorldMapPlayerStartup() = default;
    [[nodiscard]] WorldMapPlayerState29CameraStep prepare_state29_camera(
        const WorldMapPlayerStartupState29Input& input) const;
    [[nodiscard]] WorldMapPlayerStartupActorFrameResult prepare_actor_frame(
        const WorldMapPlayerStartupActorFrameInput& input,bool state0=false) const;
    [[nodiscard]] WorldMapPlayerStartupActorCallbackResult advance_actor_callback(
        std::optional<uint32_t> control, const WorldMapPlayerStartupActorFrameInput& actor,
        std::optional<uint8_t> effect_gate, WorldMapPlayerActorQueue* queue,bool state0=false);
    [[nodiscard]] WorldMapPlayerState0TransitionResult prepare_state0_appearance(
        const WorldMapUpdateInputs&,std::optional<uint8_t>,const WorldMapPlayerUpdateOwners*) const;
    friend WorldMapPlayerStartupResult construct_world_map_player_startup(
        const WorldMapPlayerStartupProviders&, const WorldMapPlayerStartupQuery&, std::unique_ptr<WorldMapPlayerStartup>*);
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> starts_;
    std::unique_ptr<WorldMapPlayerAnimationHolder> model_;
    WorldMapPlayerStartupState state_;
    std::optional<WorldMapPlayerStartupCameraState> camera_;
    WorldMapPlayerActorMetadata actor_metadata_;
    // Isolated animation/timer/channel mutation invalidates coordinated proof.
    std::optional<WorldMapPlayerAnimationLoop> animation_loop_;
    std::optional<WorldMapPlayerPrimaryLoop> primary_loop_;
    std::optional<WorldMapPlayerContextUpdateState> state0_context_;
    std::optional<WorldMapPlayerState0ThirdLoop> state0_third_loop_;
    // Separate owned tags provide stable keys without reproducing PPC pointers.
    uint8_t update_node_ = 0, effect_node_ = 0, render_node_ = 0, primary_node_ = 0, model_manager_node_ = 0;
};
// Fresh slot-zero / alternate -1 / type-zero startup only. Supplied phase,
// post-setup guards/binding/item fields and audio byte remain explicit.
// Reached scene-object messages, restored poses, busy bindings, guard state F,
// audio stop and runtime-entry contents are unsupported.
// Ordered model-type timer dependencies precede selector-one initialization;
// only a successful/unchanged callback permits counter reset and +3F0=1.
// Every stop releases staging and preserves *out, its frame/mesh/providers and
// shared clock. Construction never advances time or evaluates/draws a frame.
[[nodiscard]] WorldMapPlayerStartupResult construct_world_map_player_startup(
    const WorldMapPlayerStartupProviders& providers, const WorldMapPlayerStartupQuery& query,
    std::unique_ptr<WorldMapPlayerStartup>* out);
} // namespace awl
