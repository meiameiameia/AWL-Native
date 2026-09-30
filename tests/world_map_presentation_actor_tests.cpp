#include "awl/world_map_presentation_actor.h"
#include "awl/world_map_presentation.h"

#include <cstdio>

namespace {
int failures = 0;
using Snapshot = awl::WorldMapPresentationActorSnapshot;
using Resource = awl::WorldMapPresentationActorResource;
using Step = awl::WorldMapPresentationActorStep;
using Status = awl::WorldMapPresentationActorStatus;
using Kind = awl::WorldMapPresentationActorCallKind;

void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

Snapshot linked(uint32_t id, uint64_t actor = 11) {
    Snapshot snapshot;
    snapshot.registrations = std::vector<awl::WorldMapPresentationActorRegistration>{{id, actor}};
    return snapshot;
}

Resource resource_case(uint32_t slot, unsigned index) {
    Resource resource;
    resource.slot = slot;
    resource.object_present = index != 0;
    resource.flag_query_1 = 0;
    resource.word_12c = 0x80000000u + index;
    resource.word_130 = 0x100u + index;
    switch (index) {
    case 1: resource.flag_query_1.reset(); break;
    case 2: resource.flag_query_1 = 1; break; // Unknown global lookup.
    case 4: resource.state_120 = 1; resource.result_124 = 0; break;
    case 5: resource.state_120 = 3; resource.result_124 = 0; break;
    case 6: resource.state_120 = 2; resource.result_124 = 0; break;
    case 7: resource.result_124 = 0; break;
    case 8: resource.flag_query_1 = 256; resource.global_lookup_result = UINT32_MAX; resource.word_130 = 6; break;
    case 9: resource.flag_query_1 = 1; resource.global_lookup_result = UINT32_MAX; resource.word_130 = 7; break;
    case 10: resource.flag_query_1 = 1; resource.global_lookup_result = UINT32_MAX; resource.word_130 = 5; break;
    case 11: resource.flag_query_1 = 1; resource.global_lookup_result = 0; resource.word_130 = 5; break;
    case 12: resource.flag_query_1 = UINT32_MAX; resource.global_lookup_result = 0x80000000; resource.word_130 = UINT32_MAX; break;
    case 13: resource.result_124 = 0x80000000; break;
    case 14: resource.state_120 = 0x80000001; resource.result_124 = 0; break;
    default: break;
    }
    return resource;
}

void test_lookup_and_order() {
    Step step;
    Snapshot unknown;
    expect(awl::prepare_world_map_presentation_actor(UINT32_MAX, 0, unknown, &step) == Status::NoEffects &&
        step.call_count == 0 && !step.resource_ready && !step.used_fallback,
        "minus-one resource bypasses all owner and resource discovery");
    expect(awl::prepare_world_map_presentation_actor(0x4b, 0, unknown, &step) == Status::RequiresLookup &&
        step.lookup_id == 0x27 && !step.used_fallback, "unknown list is distinct from an empty known list");
    auto snapshot = linked(0x27);
    snapshot.registrations->push_back({0x27, 22});
    expect(awl::prepare_world_map_presentation_actor(0x4b, 0, snapshot, &step) == Status::RequiresActorCalls &&
        step.matched_registration == 0 && step.actor_identity == 11 && !step.used_fallback && step.resource_slot == 0 &&
        step.call_count == 2 && step.calls[0].kind == Kind::DirectCommand &&
        step.calls[0].arguments == std::array<uint32_t, 5>{0x4b, 0, 0, 0, 0} &&
        step.calls[0].argument_count == 4 && step.calls[1].kind == Kind::Toggle && step.calls[1].arguments[0] == 0,
        "list remap retains original command ID, first matching actor, direct call and later toggle order");
    (*snapshot.registrations)[0].actor_d4 = 0;
    expect(awl::prepare_world_map_presentation_actor(0x4b, 5, snapshot, &step) == Status::RequiresLookup &&
        step.matched_registration == 0 && step.actor_identity == 0 && step.used_fallback,
        "null first match shadows later duplicate actors and requires fallback");
    snapshot.fallback = awl::WorldMapPresentationActorFallback{0x4b, 33, 128};
    expect(awl::prepare_world_map_presentation_actor(0x4b, 5, snapshot, &step) == Status::RequiresActorCalls &&
        step.actor_identity == 33 && step.calls[0].arguments[0] == 0x4b && step.calls[0].arguments[1] == 5 &&
        step.calls[1].actor_identity == 33 && step.calls[1].argument_count == 1 && step.calls[1].arguments[0] == 1,
        "fallback uses original ID and nonzero full byte gate; mode-five toggle follows command");
    snapshot.fallback->active_c = 0;
    expect(awl::prepare_world_map_presentation_actor(0x4b, 5, snapshot, &step) == Status::NoEffects &&
        step.actor_identity == 0 && step.call_count == 0, "inactive fallback object produces no actor calls");
    snapshot = linked(0x28, 44);
    expect(awl::prepare_world_map_presentation_actor(0x4c, 0, snapshot, &step) == Status::RequiresActorCalls &&
        step.lookup_id == 0x28 && step.actor_identity == 44, "second special ID has its separate registered-list remap");
    snapshot = linked(0x4b, 44);
    snapshot.fallback = awl::WorldMapPresentationActorFallback{0x4b, 55, 1};
    expect(awl::prepare_world_map_presentation_actor(0x4b, 0, snapshot, &step) == Status::RequiresActorCalls &&
        !step.matched_registration && step.actor_identity == 55, "unremapped list entry does not match special ID");
    snapshot.registrations->clear();
    expect(awl::prepare_world_map_presentation_actor(0x4b, 0, snapshot, &step) == Status::RequiresActorCalls &&
        step.used_fallback, "complete empty list can resolve a supplied fallback");
}

void test_resource_gates_and_failure() {
    auto snapshot = linked(35);
    Step step;
    expect(awl::prepare_world_map_presentation_actor(35, 5, snapshot, &step) == Status::RequiresResourceState &&
        step.resource_slot == 32 && step.actor_identity == 11, "mapped resource needs its selected slot snapshot");
    snapshot.resource = resource_case(32, 0);
    expect(awl::prepare_world_map_presentation_actor(35, 5, snapshot, &step) == Status::NoEffects &&
        step.resource_ready == false, "known null resource is unavailable without bit or global queries");
    snapshot.resource = resource_case(32, 8);
    expect(awl::prepare_world_map_presentation_actor(35, 5, snapshot, &step) == Status::RequiresActorCalls &&
        step.resource_ready == true && step.calls[0].kind == Kind::ResourceCommand &&
        step.calls[0].arguments == std::array<uint32_t, 5>{35, 5, 0x80000008u, 0, 6} &&
        step.calls[0].argument_count == 5 && step.calls[1].arguments[0] == 1,
        "whole nonzero bit-query word enables global lookup route and exact 12C/130 words reach resource command");
    snapshot.resource->global_lookup_result.reset();
    expect(awl::prepare_world_map_presentation_actor(35, 5, snapshot, &step) == Status::RequiresResourceState &&
        !step.resource_ready && step.call_count == 0, "global query precedes the six/seven alternative gate");
    snapshot.resource = resource_case(32, 4);
    expect(awl::prepare_world_map_presentation_actor(35, 5, snapshot, &step) == Status::RequiresActorCalls,
        "state one uses original sentinel path without global query");
    snapshot.registrations->clear();
    snapshot.fallback = awl::WorldMapPresentationActorFallback{35, 33, 0};
    snapshot.resource.reset();
    expect(awl::prepare_world_map_presentation_actor(35, 0, snapshot, &step) == Status::RequiresResourceState &&
        step.actor_identity == 0, "resource query occurs even after lookup returns no actor");
    snapshot.resource = resource_case(32, 3);
    expect(awl::prepare_world_map_presentation_actor(35, 0, snapshot, &step) == Status::NoEffects &&
        step.resource_ready == true && step.call_count == 0, "ready resource with null actor still sends no calls");
    step.resource_id = 99; step.call_count = 77;
    snapshot.resource->slot = 33;
    expect(awl::prepare_world_map_presentation_actor(35, 0, snapshot, &step) == Status::InvalidInput &&
        step.resource_id == 99 && step.call_count == 77, "stale selected-slot key fails without output mutation");
    snapshot.resource.reset(); snapshot.fallback->requested_id = 36;
    expect(awl::prepare_world_map_presentation_actor(35, 0, snapshot, &step) == Status::InvalidInput && step.call_count == 77,
        "fallback observation must correspond to the original requested ID");
    snapshot.fallback = awl::WorldMapPresentationActorFallback{35, 0, 1};
    expect(awl::prepare_world_map_presentation_actor(35, 0, snapshot, &step) == Status::InvalidInput && step.call_count == 77 &&
        awl::prepare_world_map_presentation_actor(35, 1, snapshot, &step) == Status::InvalidInput && step.call_count == 77 &&
        awl::prepare_world_map_presentation_actor(35, 0, snapshot, nullptr) == Status::InvalidInput,
        "inconsistent active identity, unsupported mode and missing output are rejected");
    snapshot = linked(0);
    snapshot.resource = resource_case(33, 1); // Unreached stale/unknown fields.
    snapshot.fallback = awl::WorldMapPresentationActorFallback{99, 0, 1};
    expect(awl::prepare_world_map_presentation_actor(0, 0, snapshot, &step) == Status::RequiresActorCalls &&
        !step.resource_ready, "unreached fallback and slot snapshots do not alter direct command path");
}

void hash_word(uint64_t& digest, uint32_t word) {
    for (unsigned shift : {24u, 16u, 8u, 0u}) digest = (digest ^ static_cast<uint8_t>(word >> shift)) * 1099511628211ull;
}

void test_independent_matrix() {
    uint64_t digest = 14695981039346656037ull;
    unsigned cases = 0;
    for (uint32_t id : {0u, 1u, 2u, 31u, 32u, 33u, 34u, 35u, 36u, 37u, 39u, 40u, 75u, 76u,
                        127u, 128u, 255u, 256u, 0x80000001u, UINT32_MAX}) {
        for (uint32_t mode : {0u, 5u}) {
            for (uint32_t actor : {0u, 1u, 2u}) {
                for (uint32_t state = 0; state < 16; ++state) {
                    const uint32_t lookup = id == 75 ? 39u : id == 76 ? 40u : id;
                    auto snapshot = linked(lookup, actor == 0 ? 11u : 0u);
                    snapshot.registrations->push_back({lookup, 22});
                    snapshot.fallback = awl::WorldMapPresentationActorFallback{id, 33, static_cast<uint8_t>(actor == 1)};
                    if (state != 15) snapshot.resource = resource_case(awl::world_map_presentation_resource_slot(id), state);
                    Step step;
                    const auto status = awl::prepare_world_map_presentation_actor(id, mode, snapshot, &step);
                    for (uint32_t word : {id, mode, actor, state, static_cast<uint32_t>(status), step.lookup_id,
                        step.resource_slot, step.matched_registration ? static_cast<uint32_t>(*step.matched_registration) : UINT32_MAX,
                        static_cast<uint32_t>(step.used_fallback), static_cast<uint32_t>(step.actor_identity),
                        step.resource_ready ? (*step.resource_ready ? 2u : 1u) : 0u, step.call_count}) hash_word(digest, word);
                    for (const auto& call : step.calls) {
                        hash_word(digest, static_cast<uint32_t>(call.kind));
                        hash_word(digest, static_cast<uint32_t>(call.actor_identity));
                        hash_word(digest, call.argument_count);
                        for (const auto word : call.arguments) hash_word(digest, word);
                    }
                    ++cases;
                }
            }
        }
    }
    // Filled from independent execution of mapped instruction branches.
    expect(cases == 1920 && digest == 0x043b1fbbeb26c9bdull,
        "actor lookup, resource gates and ordered argument matrix matches DOL probe");
    uint64_t slots = 14695981039346656037ull;
    for (uint32_t id = 0; id < 256; ++id) {
        hash_word(slots, id); hash_word(slots, awl::world_map_presentation_resource_slot(id));
    }
    for (uint32_t id : {256u, 0x80000001u, 0x80000023u, UINT32_MAX}) {
        hash_word(slots, id); hash_word(slots, awl::world_map_presentation_resource_slot(id));
    }
    expect(slots == 0x727dde37b456380dull, "signed-byte table scan keeps full ID comparison without input truncation");
}

void test_presentation_composition() {
    const uint8_t bytes[] = {0x30, 0}; // Invented control-only protocol fixture.
    awl::WorldMapPresentationState presentation;
    presentation.display_offset_1c = presentation.resume_offset_20 = 0;
    presentation.manager_resource_60 = 0;
    awl::WorldMapPresentationStep progression;
    expect(awl::advance_world_map_presentation(bytes, sizeof(bytes), &presentation, 1, 0, 0, &progression) ==
        awl::WorldMapPresentationStatus::RequiresActorEffects && progression.actor_mode == 0u &&
        progression.phase_after_effects == awl::WorldMapPresentationPhase::InputWait,
        "wait control stops after pointer proposal and before actor effects/member change");
    Step actor;
    expect(progression.actor_mode && awl::prepare_world_map_presentation_actor(presentation.manager_resource_60,
        *progression.actor_mode, linked(0), &actor) == Status::RequiresActorCalls && actor.call_count == 2 &&
        presentation.phase == awl::WorldMapPresentationPhase::Reading && presentation.resume_offset_20 == 0,
        "supplied actor identities prepare ordered effects without accepting presentation prefix");
    presentation.phase = awl::WorldMapPresentationPhase::InputWait;
    expect(awl::advance_world_map_presentation(bytes, sizeof(bytes), &presentation, 1, 0x100, 0, &progression) ==
        awl::WorldMapPresentationStatus::RequiresFeedback && progression.actor_mode == 5u &&
        presentation.phase == awl::WorldMapPresentationPhase::InputWait,
        "input wait requires feedback before the prepared actor branch; neither is bypassed");
}
} // namespace

int main() {
    test_lookup_and_order();
    test_resource_gates_and_failure();
    test_independent_matrix();
    test_presentation_composition();
    return failures == 0 ? 0 : 1;
}
