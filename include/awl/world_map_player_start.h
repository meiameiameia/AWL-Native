#pragma once
#include "awl/collision_asset.h"
#include "awl/player_input.h"
#include "awl/world_map_scene_index.h"
#include "awl/world_map_animation_initializer.h"

namespace awl {

// Reads only the position at owner +29C78 and heading at +29F84 consumed
// by FUN_8002FDF8. Bytes are an in-memory big-endian owner snapshot, NOT a
// disc/save-file format. Scene type comes separately from scene owner +68.
// Invalid/short/nonfinite inputs preserve output; valid headings are copied
// without normalization, including signed zero and non-unit vectors.
[[nodiscard]] bool decode_world_map_player_start_pose(
    const uint8_t* owner_bytes, size_t size, int32_t scene_type,
    WorldMapPlayerScenePose* output);

// Only FUN_80013AE0's constant pose writes, not its other phase/setup work.
// The entry source is (-1,0,-5.2), heading +Z. Scene type remains supplied.
[[nodiscard]] bool make_world_map_phase_entry_pose(
    int32_t scene_type, WorldMapPlayerScenePose* output);

struct WorldMapPlayerStartQuery {
    WorldMapPlayerScenePose saved_pose{};
    int32_t state_680 = -1;
    int32_t state_58c = 0;
    uint8_t secondary_byte_3f3 = 0;
    CollisionWorldMapInitialPlacementQuery placement{};
};

// Borrowed big-endian memory-owner views, not disc/save-file formats.
// No pointers inside these snapshots are followed or retained.
struct WorldMapPlayerStartOwners {
    const uint8_t* saved = nullptr; size_t saved_size = 0;
    const uint8_t* scene = nullptr; size_t scene_size = 0;
    const uint8_t* guards = nullptr; size_t guards_size = 0;
    const uint8_t* secondary = nullptr; size_t secondary_size = 0;
};

struct WorldMapPlayerStartInputs {
    WorldMapPlayerStartQuery start{};
    int32_t saved_scene_type_2a290 = 0;
    uint8_t secondary_byte_3f2 = 0;
};

// Reads scene +68, guards +680/+58C, the saved pose/type and static-mask
// bytes. Asset/list views remain empty for the caller to supply. Saved
// message type is raw until its branch is reached; initial pose is finite
// and scene type must be supported. Failed decode preserves output/bytes.
[[nodiscard]] bool decode_world_map_player_start_inputs(
    const WorldMapPlayerStartOwners& owners, WorldMapPlayerStartInputs* output);

struct WorldMapPlayerConstructorTailQuery {
    // Capture these owners at the tail boundary; earlier setup can change
    // them. The action selector is supplied AFTER setup, not decoded from
    // a guessed initial save field.
    WorldMapPlayerStartInputs inputs{};
    bool binding_present = false; // player +1454
    uint32_t binding_word_0 = 0;
    int32_t action_148_after_setup = 0; // pointed player +135C owner
    bool payload_camera_byte_known = false;
    uint8_t payload_camera_byte = 0; // caller stack payload +1C
};

struct WorldMapPlayerConstructorTail {
    int32_t requested_state = 0;
    bool message_prepared = false;
    int32_t message_target_id = 0;
    WorldMapPlayerSceneMessage1F message{};
};

enum class WorldMapPlayerConstructorTailStatus {
    Ready,
    InvalidInput,
    UnsupportedRestoredPose,
    UnsupportedBusyBinding,
    MissingCameraByteEvidence
};

// Only the branches/payload at FUN_8002FDF8 800303B8..80030584.
// Message delivery, post-message model attachment and state effects remain
// pending. This never changes a pose or accepts a state request.
// The constructor does not initialize payload +1C; reached messages need
// explicit byte evidence. Unsupported branches preserve output.
[[nodiscard]] WorldMapPlayerConstructorTailStatus plan_world_map_player_constructor_tail(
    const WorldMapPlayerConstructorTailQuery& query, WorldMapPlayerConstructorTail* output);

// FUN_8004E190 consumes fields of the owner pointed to by player +135C,
// not the same offsets of the global saved owner. Integer -> binary32
// comparisons preserve the verified threshold words and nonzero-byte gate.
[[nodiscard]] uint32_t select_world_map_player_model_type(uint8_t byte_14c, int32_t word_20);

// Bounded FUN_8002F748 pointer-slot selection. Only table types 0/1 and
// observed phases 0..5 are supported. A slot address is an evidence key,
// never a host pointer; table contents must be supplied separately.
[[nodiscard]] bool select_world_map_player_model_type_slot(
    uint32_t phase, uint32_t table_type_134, int32_t saved_27e54, uint32_t* slot);

struct WorldMapPlayerModelTypeRow {
    uint32_t slot = 0, selector = 0;
    uint32_t type_0 = 0, type_4 = 0;
};
struct WorldMapPlayerStartState {
    int32_t state_1364 = 0;
    uint32_t counter_1368 = 0;
    uint32_t actor_table_type_134 = 0;
    WorldMapAnimationFeature38 actor_model_type; // actor +124
    uint8_t phase_byte_8 = 0, secondary_byte_3f0 = 0;
};
struct WorldMapPlayerStartStateQuery {
    int32_t requested_state = 0x29, scene_type_68 = 1;
    // Owner observations at the dispatch boundary, not guessed defaults.
    std::optional<uint8_t> audio_byte_90;
    uint32_t phase = 0;
    int32_t saved_27e54 = 0;
    uint8_t saved_subobject_byte_14c = 0;
    int32_t saved_subobject_word_20 = 0;
    std::optional<WorldMapPlayerModelTypeRow> model_row;
    WorldMapAnimationInitializerObservations model_observations;
    uint8_t secondary_byte_3f2 = 0;
    uint32_t variant_138c = 0, item_144 = 0, action_148 = 0;
};
struct WorldMapPlayerStartAnimationCommand {
    uint32_t selector = 1, variant = 0, item = 0, action = 0, mode = 4;
};
struct WorldMapPlayerStartStateStep {
    WorldMapPlayerStartState after;
    uint32_t model_selector = 0, model_slot = 0, callback_address = 0;
    std::optional<WorldMapAnimationFeatureRow> required_timer_row;
    std::optional<WorldMapPlayerStartAnimationCommand> animation_command;
};
enum class WorldMapPlayerStartStateStatus {
    Prepared, Advanced, InvalidInput, UnsupportedState, RequiresAudioByte,
    RequiresAudioStop, UnsupportedModelTable, RequiresModelRow,
    RequiresTimerTable, RequiresTimerRow, RequiresClock,
    UnsupportedRestoredPose, RequiresAnimationCommand
};

// FUN_80031D7C restricted to constructor requests 0F/29, including its
// ordered model-type call and verified first member callback. For 0F,
// FUN_80035884 clears PHASE-owner byte +8; the wrapper then sets 3F0=1.
// For 29, FUN_800349A4 requires 8007C0C0 before counter reset. Its animation
// command is reported, not acknowledged; no requested state is accepted.
// Audio and missing model/timer evidence stop before later work. All stops
// roll back supplied fields; InvalidInput preserves output. No live game
// owner, scene message, full update callback or animation is connected.
[[nodiscard]] WorldMapPlayerStartStateStatus prepare_world_map_player_start_state(
    const WorldMapPlayerStartState& state, const WorldMapPlayerStartStateQuery& query,
    WorldMapPlayerStartStateStep* output);
// Applies only the complete supported 0F proposal to supplied typed state.
[[nodiscard]] WorldMapPlayerStartStateStatus advance_world_map_player_start_state(
    WorldMapPlayerStartState* state, const WorldMapPlayerStartStateQuery& query,
    WorldMapPlayerStartStateStep* output);

struct WorldMapPlayerStart {
    WorldMapPlayerScenePose pose{};
    WorldMapSteeringState steering{};
    bool placement_called = false;
    CollisionWorldMapInitialPlacement placement{};
};

enum class WorldMapPlayerStartStatus {
    Ready,
    InvalidInput,
    UnsupportedRelocation,
    UnsupportedMovementCategory,
    PlacementFailed,
    SceneRegistrationFailed
};

// Bounded constructor pose/steering before camera/model/state completion.
// Guarded placement calls FUN_8001E170's distinct supported sequence.
// The +3F3 relocation branch is rejected until its predicate is translated.
// All dependencies are supplied; no global owner or saved data is mutated.
[[nodiscard]] WorldMapPlayerStartStatus prepare_world_map_player_start(
    const WorldMapPlayerStartQuery& query, WorldMapPlayerStart* output);

} // namespace awl
