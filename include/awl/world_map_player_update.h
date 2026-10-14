#pragma once
#include "awl/world_map_scene_index.h"
#include "awl/world_map_player_status.h"
#include "awl/world_map_player_start.h"
#include <optional>

namespace awl {
struct WorldMapPlayerUpdateGuards {
    int32_t word_680 = -1;
    // Read only when +680 is -1.
    std::optional<int32_t> word_58c;
};
struct WorldMapPlayerUpdateEntry {
    std::optional<uint32_t> control_word;
    std::optional<WorldMapPlayerUpdateGuards> guards;
    // Scene global -> +4C -> +4, distinct from scene type +68.
    std::optional<int32_t> scene_child_word_4;
};
struct WorldMapPlayerUpdateQueueEntry {
    uint64_t node_identity = 0; // Opaque nonzero key, never a host pointer.
    uint32_t priority = 0;
};
// Explicit projection, not a translated item-owner constructor.
struct WorldMapPlayerItemOwnerState {uint32_t mask_88=0;};
// Supplied CPU counterparts of the reached global owners. No save-file
// format, live globals, renderer or queue processing is implied. The caller
// may consume nodes between updates. A node already in this queue at 1010
// is unlinked/reinserted before equals, as in C3F4 -> 989C. Other queues and
// changing an existing node's priority are outside this supplied boundary.
struct WorldMapPlayerUpdateOwners {
    WorldMapPlayerScenePose saved_pose; // +2A290/+29C78/+29F84
    std::array<float,3> position_802fc20c{}; // 8012B004, ignores its r3 owner
    // Presence establishes the owner needed by 8014FDA8 on scene type one.
    std::optional<std::array<float,3>> scene_one_position_240;
    std::vector<WorldMapPlayerUpdateQueueEntry> queue;
    // Supplied snapshot or explicitly initialized fresh projection, needed
    // only without control bit 800. Other saved-owner fields remain separate.
    std::optional<WorldMapPlayerStatusState> status;
    // r13-5854's separate item owner; only its 7AFFC mask store is owned.
    std::optional<WorldMapPlayerItemOwnerState> item_owner;
};
enum class WorldMapPlayerUpdateStatus {
    Prepared, Advanced, RequiresControlWord, RequiresGuardOwner,
    RequiresGuardWord58c, RequiresGuardTransition, RequiresSceneWord,
    RequiresSceneTransition, RequiresStatusUpdate, RequiresEffectOwners,
    RequiresScenePositionOwner, RequiresQueueNode, InvalidQueue,
    UnsupportedQueuePriorityChange, CameraIncomplete, UnsupportedState, InvalidInput,
    AllocationFailure, SkippedByDispatchFilter, RequiresDispatchControl, AppearanceIncomplete,
    MovementIncomplete,AnimationIncomplete,RequiresPose,RequiresMovementOwners,RequiresMovementCamera,RequiresAnimationCommand
};
// FUN_80031144's pre-callback decisions, restricted to state 29. The guard
// transition and scene transition are separate, ordered dependencies.
// No state F request, callback or write is acknowledged here.
[[nodiscard]] WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_entry(
    const WorldMapPlayerUpdateEntry& entry);
// The same normal branches also apply to state zero. The caller must check
// its supported state; this helper cannot authorize other state callbacks.
[[nodiscard]] WorldMapPlayerUpdateStatus prepare_world_map_player_normal_update_entry(
    const WorldMapPlayerUpdateEntry& entry);

// Observations reached by 31484 -> state-29 row +18 -> 34B44. These
// globals are distinct owners even where their field offsets are equal.
struct WorldMapPlayerState29SecondaryInput {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    std::optional<int32_t> mode_word_60; // r13-5808; 189338 tests == 1
    std::optional<int32_t> scene_mode_60; // r13-57B8
    std::optional<int32_t> secondary_word_c, secondary_word_20; // r13-582C
    std::optional<uint8_t> saved_byte_14d35; // r13-57F4
    std::optional<uint8_t> secondary_byte_3f2; // r13-582C
    std::optional<bool> item_present; // (r13-5854)->+4 != null
    // Resolved save-record +6 reached through 79C10/5D034/DC24, not a
    // classification inferred from an item name or the startup descriptor.
    std::optional<uint8_t> item_type_6;
};
enum class WorldMapPlayerFollowupStatus {
    RetainedState, SkippedByGuard, TransitionRequired, RequiresGuards,
    RequiresGuardWord58c, RequiresModeWord, RequiresSceneMode,
    RequiresSecondaryWordC, RequiresSecondaryWord20, RequiresSavedByte,
    RequiresSecondaryByte, RequiresItemOwner, RequiresItemType
};
struct WorldMapPlayerState29SecondaryResult {
    WorldMapPlayerFollowupStatus status = WorldMapPlayerFollowupStatus::RequiresGuards;
    // Argument to 31D7C, BEFORE its remaps, state initializer and model work.
    // No transition or write is accepted when this field is present.
    std::optional<int32_t> requested_state;
};
[[nodiscard]] WorldMapPlayerState29SecondaryResult prepare_world_map_player_state29_secondary(
    const WorldMapPlayerState29SecondaryInput&) noexcept;
enum class WorldMapPlayerState0SecondaryStatus {
    RetainedState,SkippedByGuard,RequiresGuards,RequiresGuardWord58c,
    RequiresHolderCompleted,RequiresHolderFlag,RequiresMovement,TransitionRequired,
    RequiresControl,RequiresPrimary,RequiresAnimation,AlreadyCompleted,Filtered,InvalidInput
};
struct WorldMapPlayerState0SecondaryResult {
    WorldMapPlayerState0SecondaryStatus status=WorldMapPlayerState0SecondaryStatus::InvalidInput;
    std::optional<int32_t> requested_state;
};
// 31484 -> state-zero row +18 -> 42BBC. Owned holder +20/+21 byte getters;
// completed wins without reading +21. The reached 3083C movement body or
// state-one transition stops before execution. Control bit 800 does not skip
// this body. No control/holder/state writes or queue/pass acknowledgement.
[[nodiscard]] WorldMapPlayerState0SecondaryResult prepare_world_map_player_state0_secondary(
    const std::optional<WorldMapPlayerUpdateGuards>&,std::optional<uint8_t> completed_20,
    std::optional<uint8_t> flag_21) noexcept;
enum class WorldMapPlayerState1InitializerStatus {Prepared,RequiresAction,RequiresMask,RequiresItemOwner};
struct WorldMapPlayerState1InitializerResult {
    WorldMapPlayerState1InitializerStatus status=WorldMapPlayerState1InitializerStatus::RequiresAction;
    std::optional<uint32_t> mask;
};
enum class WorldMapPlayerState1AnimationStatus {
    Prepared,SkippedByControl,RequiresControl,RequiresProfile,RequiresIntensity,RequiresSpeed,
    RequiresGuards,RequiresGuardWord58c,RequiresItem,RequiresAction,UnsupportedNumerics
};
struct WorldMapPlayerState1AnimationResult {
    WorldMapPlayerState1AnimationStatus status=WorldMapPlayerState1AnimationStatus::RequiresControl;
    std::optional<WorldMapPlayerStartAnimationCommand> command;
    std::optional<float> command_speed,final_speed;
};
// 42CA8 after the supported movement return. 800 skips every body read;
// zero rechecks guards before selector zero, then always sets unit speed.
// Unit intensity selects three; all other finite intensities select two.
[[nodiscard]] WorldMapPlayerState1AnimationResult prepare_world_map_player_state1_animation(
    std::optional<uint32_t> control,const std::optional<WorldMapPlayerState1AnimationProfile>&,
    std::optional<float> intensity,std::optional<float> current_speed,
    const std::optional<WorldMapPlayerUpdateGuards>&,
    std::optional<uint32_t> item,std::optional<uint32_t> action) noexcept;
// Complete 42C7C -> 32B60 -> 7AFFC selection. Action zero/two skips the
// constant; all other signed words select that exact mask, replacing +88.
// Reached item owner is required after selection; no model/state writes here.
[[nodiscard]] WorldMapPlayerState1InitializerResult prepare_world_map_player_state1_initializer(
    std::optional<int32_t> action,std::optional<uint32_t> mask,bool item_owner_present) noexcept;
// 31504 -> row +24 -> 34CA8 (blr), including its earlier guard decisions.
[[nodiscard]] WorldMapPlayerFollowupStatus prepare_world_map_player_state29_tertiary(
    const std::optional<WorldMapPlayerUpdateGuards>&) noexcept;

// Only 313C4..31464 on state 29, including the complete position-copy
// callees and 17CFB4/D324/D37C/C3F4 sorted insertion. Unsigned priorities
// insert before equals. The pose is observed at tail entry, not a movement
// proposal. Invalid/nonfinite pose, invalid queue, unsupported priority change or caught
// allocation failure preserves *after; input/output aliasing is supported.
// No queue consumer, live scene ownership or full game loop is activated.
[[nodiscard]] WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_tail(
    const WorldMapPlayerUpdateOwners& owners, const WorldMapPlayerScenePose& pose,
    uint64_t player_node_identity, WorldMapPlayerUpdateOwners* after);
// Shared tail for supported states zero and 29 (neither is the state-two
// enqueue exclusion). Same atomic/native queue contract as the alias above.
[[nodiscard]] WorldMapPlayerUpdateStatus prepare_world_map_player_normal_update_tail(
    const WorldMapPlayerUpdateOwners&,const WorldMapPlayerScenePose&,
    uint64_t player_node_identity,WorldMapPlayerUpdateOwners* after);
} // namespace awl
