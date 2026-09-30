#include "awl/world_map_actor_animation.h"

#include <cmath>

namespace awl {
namespace {
WorldMapActorAnimationFields decode_fields(const WorldMapActorAnimationDescriptor& descriptor) {
    WorldMapActorAnimationFields fields;
    const uint32_t a = descriptor.word_0, b = descriptor.word_4;
    fields.group_index = a >> 25;
    fields.primary_clip_index = a & 0x3ffu;
    fields.blend_count = (b >> 6) & 0x1fu;
    if (fields.blend_count == 0x1f) fields.blend_count = 10;
    const uint32_t feature_38 = b & 0x3fu, feature_3c = (b >> 19) & 0xffu;
    const uint32_t secondary = (b >> 11) & 0x7fu;
    if (feature_38 != 0x3f) fields.feature_38_index = feature_38;
    if (feature_3c != 0xff) fields.feature_3c_index = feature_3c;
    if (secondary != 0x7f) fields.secondary_index = secondary;
    fields.mode_10 = (a >> 10) & 3u;
    fields.value_14 = (a >> 14) & 0x3ffu;
    fields.flag_12 = (a & 0x1000u) != 0;
    fields.flag_13 = (a & 0x2000u) != 0;
    fields.flag_24 = (a & 0x1000000u) != 0;
    return fields;
}
} // namespace

WorldMapActorAnimationStatus prepare_world_map_actor_animation_start(
    const WorldMapActorAnimationState& state, uint64_t descriptor_identity,
    const std::optional<WorldMapActorAnimationDescriptor>& descriptor,
    const std::optional<WorldMapActorAnimationGroup>& group, WorldMapActorAnimationStep* out) {
    using Status = WorldMapActorAnimationStatus;
    if (out == nullptr || &state == &out->after) return Status::InvalidInput;
    WorldMapActorAnimationStep step;
    step.after = state;
    auto finish = [&](Status status) { *out = step; return status; };
    // FUN_8017D69C compares +4, not current +0 or descriptor byte contents.
    if (descriptor_identity == state.base_descriptor_4) return finish(Status::Unchanged);
    if (descriptor_identity == 0) return Status::InvalidInput;
    step.descriptor_changed = true;
    step.after.current_descriptor_0 = step.after.base_descriptor_4 = descriptor_identity;
    step.after.completed_20 = step.after.flag_21 = 0;
    step.after.restart_count_2c = 0;
    if (!descriptor) return finish(Status::RequiresDescriptor);
    if (descriptor->identity != descriptor_identity) return Status::InvalidInput;
    step.fields = decode_fields(*descriptor);
    if (group && group->group_index != step.fields->group_index) return Status::InvalidInput;
    if (!group || state.model_identity_30 == 0) return finish(Status::RequiresBinding);
    if (group->bank_identity == 0) return Status::InvalidInput;
    WorldMapActorAnimationSetup setup;
    setup.model_identity = state.model_identity_30;
    setup.bank_identity = group->bank_identity;
    setup.clip_index = step.fields->primary_clip_index;
    setup.blend_count = step.fields->blend_count;
    // r6 = 0 and f1 = mapped +0.0 at 8034BAA8 before FUN_801A6878.
    step.primary_setup = setup;
    return finish(Status::RequiresModelSetup);
}

WorldMapActorPresentationToggleStatus prepare_world_map_actor_presentation_toggle(
    const WorldMapActorPresentationToggleState& state, uint32_t requested,
    WorldMapActorPresentationToggleStep* out) {
    using Status = WorldMapActorPresentationToggleStatus;
    if (out == nullptr || &state == &out->after) return Status::InvalidInput;
    WorldMapActorPresentationToggleStep step;
    step.after = state;
    step.requested_byte = static_cast<uint8_t>(requested);
    const bool signed_type_at_least_3a = (state.type_128 & 0x80000000u) == 0 && state.type_128 >= 0x3a;
    if (state.flag_158 != step.requested_byte && signed_type_at_least_3a) {
        step.clamp_requested = true;
        if (!state.clamp_limit) { *out = step; return Status::RequiresClampLimit; }
        if (!std::isfinite(*state.clamp_limit)) return Status::InvalidInput;
        // Source lower-bound check precedes upper-bound check. For its fixed
        // zero input, a negative upper limit wins; there is no second clamp.
        step.after.value_150 = *state.clamp_limit < 0 ? *state.clamp_limit : 0.0f;
    }
    step.after.flag_158 = step.requested_byte;
    *out = step;
    return Status::Prepared;
}

WorldMapActorPresentationToggleStatus advance_world_map_actor_presentation_toggle(
    WorldMapActorPresentationToggleState* state, uint32_t requested,
    WorldMapActorPresentationToggleStep* out) {
    using Status = WorldMapActorPresentationToggleStatus;
    if (state == nullptr || out == nullptr || state == &out->after) return Status::InvalidInput;
    const auto status = prepare_world_map_actor_presentation_toggle(*state, requested, out);
    if (status != Status::Prepared) return status;
    *state = out->after;
    return Status::Advanced;
}

} // namespace awl
