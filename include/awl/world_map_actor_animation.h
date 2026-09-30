#pragma once

#include <cstdint>
#include <optional>

namespace awl {

struct WorldMapActorAnimationDescriptor {
    uint64_t identity = 0;
    uint32_t word_0 = 0;
    uint32_t word_4 = 0;
    // The runtime advances records by 12 bytes. The first setup does not
    // interpret this third word; it is not an eight-byte descriptor format.
    uint32_t word_8 = 0;
};

struct WorldMapActorAnimationFields {
    uint32_t group_index = 0;
    uint32_t primary_clip_index = 0;
    uint32_t blend_count = 0;
    std::optional<uint32_t> feature_38_index;
    std::optional<uint32_t> feature_3c_index;
    std::optional<uint32_t> secondary_index;
    uint32_t mode_10 = 0;
    uint32_t value_14 = 0;
    bool flag_12 = false;
    bool flag_13 = false;
    bool flag_24 = false;
};

struct WorldMapActorAnimationState {
    uint64_t current_descriptor_0 = 0;
    uint64_t base_descriptor_4 = 0;
    float speed_8 = 1;
    uint32_t default_duration_c = 0;
    uint32_t deadline_10 = 0;
    uint32_t default_count_14 = 0;
    uint32_t count_18 = 0;
    uint32_t completed_count_1c = 0;
    uint8_t completed_20 = 0;
    uint8_t flag_21 = 0;
    uint32_t restart_deadline_24 = 0;
    uint32_t restart_limit_28 = 0;
    uint32_t restart_count_2c = 0;
    uint64_t model_identity_30 = 0;
};

struct WorldMapActorAnimationGroup {
    uint32_t group_index = 0;
    uint64_t bank_identity = 0; // Observed selected group row's +0 pointer.
};

struct WorldMapActorAnimationSetup {
    uint64_t model_identity = 0;
    uint64_t bank_identity = 0;
    uint32_t clip_index = 0;
    uint32_t blend_count = 0;
    uint32_t argument_6 = 0;
    float start_value = 0;
};

enum class WorldMapActorAnimationStatus {
    Unchanged, RequiresDescriptor, RequiresBinding, RequiresModelSetup, InvalidInput,
};
struct WorldMapActorAnimationStep {
    // Unaccepted FUN_8017D69C prefix. Never resume initialization from it:
    // no setup acknowledgement or descriptor ownership exists here.
    WorldMapActorAnimationState after;
    bool descriptor_changed = false;
    std::optional<WorldMapActorAnimationFields> fields;
    std::optional<WorldMapActorAnimationSetup> primary_setup;
};

// FUN_8017D660 -> FUN_8017D69C and FUN_8017DB28 through the first model
// setup call FUN_801A6878. Same BASE identity skips all reads/setup even if
// current has advanced. Identities replace pointers; a new null is rejected.
// Missing descriptor/group/model is explicit; keys must match. Later setup,
// descriptor constructors, model/clip ownership and frame update are absent.
// Packed later fields are decoded metadata, not executed later state stores.
// Failure preserves output; state and out.after must not alias.
[[nodiscard]] WorldMapActorAnimationStatus prepare_world_map_actor_animation_start(
    const WorldMapActorAnimationState& state, uint64_t descriptor_identity,
    const std::optional<WorldMapActorAnimationDescriptor>& descriptor,
    const std::optional<WorldMapActorAnimationGroup>& group, WorldMapActorAnimationStep* out);

struct WorldMapActorPresentationToggleState {
    uint32_t type_128 = 0; // Signed comparison in FUN_8002F674.
    uint8_t flag_158 = 0;
    float value_150 = 0;
    // Observed upper-limit word at the pointed-to table +4. Needed only
    // when the original conditional FUN_8017E00C call is reached.
    std::optional<float> clamp_limit;
};
enum class WorldMapActorPresentationToggleStatus { Prepared, Advanced, RequiresClampLimit, InvalidInput };
struct WorldMapActorPresentationToggleStep {
    WorldMapActorPresentationToggleState after;
    uint8_t requested_byte = 0;
    bool clamp_requested = false;
};

// FUN_8007C910 -> FUN_8002F674 with its fixed float-zero FUN_8017E00C call.
// Tests the low byte, then a signed type >=0x3A, performs clamp before the
// byte store. Finite negative upper limits are preserved, not reclamped.
// Missing/nonfinite reached limits block/fail without input mutation.
[[nodiscard]] WorldMapActorPresentationToggleStatus prepare_world_map_actor_presentation_toggle(
    const WorldMapActorPresentationToggleState& state, uint32_t requested,
    WorldMapActorPresentationToggleStep* out);
// Applies only a Prepared step to supplied state. Does not acknowledge any
// earlier animation command, presentation phase, feedback or live actor.
[[nodiscard]] WorldMapActorPresentationToggleStatus advance_world_map_actor_presentation_toggle(
    WorldMapActorPresentationToggleState* state, uint32_t requested,
    WorldMapActorPresentationToggleStep* out);

} // namespace awl
