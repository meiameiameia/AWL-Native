#include "awl/world_map_actor_animation.h"
#include "awl/world_map_presentation_actor.h"
#include "awl/world_map_presentation.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
int failures = 0;
using State = awl::WorldMapActorAnimationState;
using Step = awl::WorldMapActorAnimationStep;
using Status = awl::WorldMapActorAnimationStatus;
using Descriptor = awl::WorldMapActorAnimationDescriptor;
using Group = awl::WorldMapActorAnimationGroup;
using ToggleState = awl::WorldMapActorPresentationToggleState;
using ToggleStep = awl::WorldMapActorPresentationToggleStep;
using ToggleStatus = awl::WorldMapActorPresentationToggleStatus;

void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
uint32_t bits(float value) {
    uint32_t word = 0; std::memcpy(&word, &value, sizeof(word)); return word;
}
State initial() {
    State state;
    state.current_descriptor_0 = 9; state.base_descriptor_4 = 1;
    state.speed_8 = 2; state.default_duration_c = 11; state.deadline_10 = 22;
    state.default_count_14 = 33; state.count_18 = 44; state.completed_count_1c = 55;
    state.completed_20 = 128; state.flag_21 = 255;
    state.restart_deadline_24 = 66; state.restart_limit_28 = 77; state.restart_count_2c = 88;
    state.model_identity_30 = 100;
    return state;
}
bool same(const State& a, const State& b) {
    return a.current_descriptor_0 == b.current_descriptor_0 && a.base_descriptor_4 == b.base_descriptor_4 &&
        bits(a.speed_8) == bits(b.speed_8) && a.default_duration_c == b.default_duration_c &&
        a.deadline_10 == b.deadline_10 && a.default_count_14 == b.default_count_14 && a.count_18 == b.count_18 &&
        a.completed_count_1c == b.completed_count_1c && a.completed_20 == b.completed_20 && a.flag_21 == b.flag_21 &&
        a.restart_deadline_24 == b.restart_deadline_24 && a.restart_limit_28 == b.restart_limit_28 &&
        a.restart_count_2c == b.restart_count_2c && a.model_identity_30 == b.model_identity_30;
}

void test_descriptor_start() {
    const auto state = initial();
    Step step;
    expect(awl::prepare_world_map_actor_animation_start(state, 1, std::nullopt, std::nullopt, &step) == Status::Unchanged &&
        !step.descriptor_changed && !step.fields && !step.primary_setup && same(step.after, state),
        "same base identity does not rewind an advanced current descriptor or reset state");
    expect(awl::prepare_world_map_actor_animation_start(state, 1, Descriptor{999, UINT32_MAX, UINT32_MAX, UINT32_MAX},
        Group{999, 0}, &step) == Status::Unchanged && same(step.after, state),
        "unchanged identity skips descriptor bytes and binding validation entirely");
    expect(awl::prepare_world_map_actor_animation_start(state, 2, std::nullopt, std::nullopt, &step) == Status::RequiresDescriptor &&
        step.descriptor_changed && step.after.current_descriptor_0 == 2 && step.after.base_descriptor_4 == 2 &&
        step.after.completed_20 == 0 && step.after.flag_21 == 0 && step.after.restart_count_2c == 0 &&
        step.after.deadline_10 == 22 && step.after.count_18 == 44 && step.after.completed_count_1c == 55 &&
        same(state, initial()), "new identity proposes exactly the traced resets but missing descriptor preserves supplied state");
    const Descriptor descriptor{2, (3u << 25) | 37u | (2u << 10) | (29u << 14) | 0x1003000u,
        0x3fu | (0x1fu << 6) | (0x7fu << 11) | (0xffu << 19), 0xfeed1234};
    expect(awl::prepare_world_map_actor_animation_start(state, 2, descriptor, std::nullopt, &step) == Status::RequiresBinding &&
        step.fields && step.fields->group_index == 3 && step.fields->primary_clip_index == 37 &&
        step.fields->blend_count == 10 && !step.fields->feature_38_index && !step.fields->feature_3c_index &&
        !step.fields->secondary_index && step.fields->mode_10 == 2 && step.fields->value_14 == 29 &&
        step.fields->flag_12 && step.fields->flag_13 && step.fields->flag_24 && !step.primary_setup,
        "packed fields preserve group/clip/control masks and feature/blend sentinel meanings");
    expect(awl::prepare_world_map_actor_animation_start(state, 2, descriptor, Group{3, 200}, &step) == Status::RequiresModelSetup &&
        step.primary_setup && step.primary_setup->model_identity == 100 && step.primary_setup->bank_identity == 200 &&
        step.primary_setup->clip_index == 37 && step.primary_setup->blend_count == 10 &&
        step.primary_setup->argument_6 == 0 && bits(step.primary_setup->start_value) == 0 && same(state, initial()),
        "first model setup preserves exact arguments and fixed positive zero without accepting animation effects");
    auto zero_fields = descriptor; zero_fields.word_0 = zero_fields.word_4 = 0;
    expect(awl::prepare_world_map_actor_animation_start(state, 2, zero_fields, Group{0, 200}, &step) == Status::RequiresModelSetup &&
        step.fields->feature_38_index == 0u && step.fields->feature_3c_index == 0u && step.fields->secondary_index == 0u &&
        step.primary_setup->blend_count == 0, "zero feature indices and blend zero remain present values, not sentinels");
    auto no_model = state; no_model.model_identity_30 = 0;
    expect(awl::prepare_world_map_actor_animation_start(no_model, 2, descriptor, Group{3, 200}, &step) == Status::RequiresBinding &&
        !step.primary_setup, "unknown/null supplied model binding cannot satisfy setup");
    const auto preserved = step;
    expect(awl::prepare_world_map_actor_animation_start(state, 2, Descriptor{3, 0, 0, 0}, std::nullopt, &step) == Status::InvalidInput &&
        awl::prepare_world_map_actor_animation_start(state, 2, descriptor, Group{4, 200}, &step) == Status::InvalidInput &&
        awl::prepare_world_map_actor_animation_start(state, 2, descriptor, Group{3, 0}, &step) == Status::InvalidInput &&
        awl::prepare_world_map_actor_animation_start(state, 0, std::nullopt, std::nullopt, &step) == Status::InvalidInput &&
        same(step.after, preserved.after) && step.descriptor_changed == preserved.descriptor_changed,
        "stale descriptor/group keys, null bank and new null identity fail without proposal mutation");
    expect(awl::prepare_world_map_actor_animation_start(state, 2, descriptor, Group{3, 200}, nullptr) == Status::InvalidInput &&
        awl::prepare_world_map_actor_animation_start(step.after, 2, descriptor, Group{3, 200}, &step) == Status::InvalidInput,
        "missing output and proposal aliasing are rejected");
    auto high_identity = state; high_identity.base_descriptor_4 = 0x100000001ull;
    expect(awl::prepare_world_map_actor_animation_start(high_identity, 1, Descriptor{1, 0, 0, 0}, Group{0, 200}, &step) ==
        Status::RequiresModelSetup, "native stable identities do not truncate to original pointer width");
    expect(awl::prepare_world_map_actor_animation_start(State{}, 0, std::nullopt, std::nullopt, &step) == Status::Unchanged,
        "same null base identity keeps the source no-read early return");
}

void test_toggle() {
    ToggleState state{0x3a, 1, 9, std::nullopt};
    ToggleStep step;
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 0, &step) == ToggleStatus::RequiresClampLimit &&
        step.clamp_requested && step.after.flag_158 == 1 && state.flag_158 == 1 && state.value_150 == 9,
        "changed byte at threshold blocks before clamp and byte store when limit is unknown");
    state.clamp_limit = 3.0f;
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 0, &step) == ToggleStatus::Advanced &&
        step.clamp_requested && state.flag_158 == 0 && bits(state.value_150) == 0,
        "reached float-zero clamp precedes changed byte store");
    state.value_150 = 9; state.clamp_limit.reset();
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 256, &step) == ToggleStatus::Advanced &&
        !step.clamp_requested && state.flag_158 == 0 && state.value_150 == 9,
        "only requested low byte is compared; unchanged byte never reads clamp limit");
    state.clamp_limit = -2.0f;
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 257, &step) == ToggleStatus::Advanced &&
        state.flag_158 == 1 && state.value_150 == -2, "negative upper limit wins without a second lower-bound clamp");
    state.clamp_limit = -0.0f;
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 0, &step) == ToggleStatus::Advanced &&
        bits(state.value_150) == 0, "equal negative-zero upper limit preserves original positive-zero input");
    state.type_128 = 0x39; state.value_150 = 9; state.clamp_limit.reset();
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 1, &step) == ToggleStatus::Advanced &&
        !step.clamp_requested && state.value_150 == 9, "below signed type threshold changes byte without float reset");
    state.type_128 = 0x8000003a;
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 0, &step) == ToggleStatus::Advanced &&
        !step.clamp_requested && state.value_150 == 9, "negative signed type is below threshold despite high unsigned value");
    state.type_128 = 0x3a; state.clamp_limit = std::numeric_limits<float>::quiet_NaN();
    const auto preserved = step;
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 1, &step) == ToggleStatus::InvalidInput &&
        state.flag_158 == 0 && state.value_150 == 9 && step.after.flag_158 == preserved.after.flag_158 &&
        step.requested_byte == preserved.requested_byte, "reached nonfinite upper limit rejects without state/output changes");
    expect(awl::advance_world_map_actor_presentation_toggle(&state, 0, &step) == ToggleStatus::Advanced &&
        !step.clamp_requested, "unreached nonfinite limit does not invalidate unchanged byte");
    expect(awl::advance_world_map_actor_presentation_toggle(nullptr, 0, &step) == ToggleStatus::InvalidInput &&
        awl::advance_world_map_actor_presentation_toggle(&state, 0, nullptr) == ToggleStatus::InvalidInput &&
        awl::prepare_world_map_actor_presentation_toggle(step.after, 0, &step) == ToggleStatus::InvalidInput,
        "toggle rejects absent pointers and input/proposal aliasing");
}

void hash_word(uint64_t& digest, uint32_t word) {
    for (unsigned shift : {24u, 16u, 8u, 0u}) digest = (digest ^ static_cast<uint8_t>(word >> shift)) * 1099511628211ull;
}
void hash_state(uint64_t& digest, const State& state) {
    for (uint32_t word : {static_cast<uint32_t>(state.current_descriptor_0), static_cast<uint32_t>(state.base_descriptor_4),
        bits(state.speed_8), state.default_duration_c, state.deadline_10, state.default_count_14, state.count_18,
        state.completed_count_1c, static_cast<uint32_t>(state.completed_20), static_cast<uint32_t>(state.flag_21),
        state.restart_deadline_24, state.restart_limit_28, state.restart_count_2c, static_cast<uint32_t>(state.model_identity_30)}) {
        hash_word(digest, word);
    }
}
void test_descriptor_matrix() {
    std::array<uint32_t, 66> patterns{};
    patterns[1] = UINT32_MAX;
    for (unsigned bit = 0; bit < 32; ++bit) {
        patterns[2 + bit] = 1u << bit; patterns[34 + bit] = ~(1u << bit);
    }
    uint64_t digest = 14695981039346656037ull;
    unsigned cases = 0;
    for (const auto a : patterns) for (const auto b : patterns) for (uint32_t same_id : {0u, 1u}) for (uint32_t bound : {0u, 1u}) {
        const auto state = initial();
        const uint64_t requested = same_id ? 1u : 2u;
        const Descriptor descriptor{requested, a, b, 0xfeed1234};
        Step step;
        const auto status = awl::prepare_world_map_actor_animation_start(state, requested, descriptor,
            bound ? std::optional<Group>{Group{a >> 25, 200}} : std::nullopt, &step);
        for (uint32_t word : {a, b, same_id, bound, static_cast<uint32_t>(status), static_cast<uint32_t>(step.descriptor_changed)}) {
            hash_word(digest, word);
        }
        hash_state(digest, step.after);
        hash_word(digest, step.fields ? 1u : 0u);
        if (step.fields) {
            const auto& f = *step.fields;
            for (uint32_t word : {f.group_index, f.primary_clip_index, f.blend_count, f.feature_38_index.value_or(UINT32_MAX),
                f.feature_3c_index.value_or(UINT32_MAX), f.secondary_index.value_or(UINT32_MAX), f.mode_10, f.value_14,
                static_cast<uint32_t>(f.flag_12), static_cast<uint32_t>(f.flag_13), static_cast<uint32_t>(f.flag_24)}) hash_word(digest, word);
        }
        hash_word(digest, step.primary_setup ? 1u : 0u);
        if (step.primary_setup) {
            const auto& setup = *step.primary_setup;
            for (uint32_t word : {static_cast<uint32_t>(setup.model_identity), static_cast<uint32_t>(setup.bank_identity),
                setup.clip_index, setup.blend_count, setup.argument_6, bits(setup.start_value)}) hash_word(digest, word);
        }
        expect(same(state, initial()), "matrix planning preserves actual supplied animation state");
        ++cases;
    }
    expect(cases == 17424 && digest == 0x89a249647260ee21ull,
        "descriptor identity/store and mapped packed-mask matrix matches independent probe");
}

void test_toggle_matrix() {
    uint64_t digest = 14695981039346656037ull;
    unsigned cases = 0;
    for (uint32_t type : {0u, 1u, 0x39u, 0x3au, 0x3bu, 0x7fffffffu, 0x8000003au, UINT32_MAX}) {
        for (uint32_t flag : {0u, 1u, 255u}) for (uint32_t requested : {0u, 1u, 255u, 256u, 257u, UINT32_MAX}) {
            for (uint32_t limit = 0; limit < 6; ++limit) {
                ToggleState state{type, static_cast<uint8_t>(flag), 9, std::nullopt};
                const float values[] = {0, 0, -1, 3, -0.0f, std::numeric_limits<float>::quiet_NaN()};
                if (limit != 0) state.clamp_limit = values[limit];
                ToggleStep step;
                step.after.flag_158 = 55; step.after.value_150 = 13; step.requested_byte = 165;
                const auto status = awl::prepare_world_map_actor_presentation_toggle(state, requested, &step);
                for (uint32_t word : {type, flag, requested, limit, static_cast<uint32_t>(status),
                    static_cast<uint32_t>(step.requested_byte), static_cast<uint32_t>(step.clamp_requested),
                    static_cast<uint32_t>(step.after.flag_158), bits(step.after.value_150)}) hash_word(digest, word);
                ++cases;
            }
        }
    }
    expect(cases == 864 && digest == 0xe77f78de5bea4551ull,
        "signed type, low-byte gate, fixed-zero clamp and explicit safety stops match probe");
}

void test_ordered_composition() {
    const uint8_t bytes[] = {0x30, 0}; // Invented control-only fixture.
    awl::WorldMapPresentationState presentation;
    presentation.resume_offset_20 = presentation.display_offset_1c = 0;
    presentation.manager_resource_60 = 0;
    awl::WorldMapPresentationStep progression;
    awl::WorldMapPresentationActorSnapshot snapshot;
    snapshot.registrations = std::vector<awl::WorldMapPresentationActorRegistration>{{0, 11}};
    awl::WorldMapPresentationActorStep calls;
    expect(awl::advance_world_map_presentation(bytes, sizeof(bytes), &presentation, 1, 0, 0, &progression) ==
        awl::WorldMapPresentationStatus::RequiresActorEffects && progression.actor_mode &&
        awl::prepare_world_map_presentation_actor(0, *progression.actor_mode, snapshot, &calls) ==
            awl::WorldMapPresentationActorStatus::RequiresActorCalls, "presentation reaches ordered actor calls");
    const auto animation = initial();
    ToggleState toggle{0x3a, 1, 9, 3.0f};
    Step start;
    expect(awl::prepare_world_map_actor_animation_start(animation, 2, std::nullopt, std::nullopt, &start) ==
        Status::RequiresDescriptor, "constructor-selected descriptor remains required evidence");
    expect(awl::prepare_world_map_actor_animation_start(animation, 2, Descriptor{2, 0, 0, 0}, Group{0, 200}, &start) ==
        Status::RequiresModelSetup && calls.call_count == 2 && calls.calls[1].arguments[0] == 0 &&
        same(animation, initial()) && toggle.flag_158 == 1 && toggle.value_150 == 9 &&
        presentation.phase == awl::WorldMapPresentationPhase::Reading,
        "supplied descriptor reaches first model setup; later toggle and presentation phase stay unconsumed");
}
} // namespace

int main() {
    test_descriptor_start();
    test_toggle();
    test_descriptor_matrix();
    test_toggle_matrix();
    test_ordered_composition();
    return failures == 0 ? 0 : 1;
}
