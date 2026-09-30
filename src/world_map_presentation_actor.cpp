#include "awl/world_map_presentation_actor.h"

namespace awl {

uint32_t world_map_presentation_resource_slot(uint32_t resource_id) {
    // Verified ordered pairs at 8024F594: (1,1)..(31,31),(35,32),(36,33).
    if (resource_id >= 1 && resource_id <= 31) return resource_id;
    if (resource_id == 35) return 32;
    if (resource_id == 36) return 33;
    return 0;
}

WorldMapPresentationActorStatus prepare_world_map_presentation_actor(
    uint32_t resource_id, uint32_t actor_mode, const WorldMapPresentationActorSnapshot& snapshot,
    WorldMapPresentationActorStep* out) {
    using Status = WorldMapPresentationActorStatus;
    if (out == nullptr || (actor_mode != 0 && actor_mode != 5)) return Status::InvalidInput;
    WorldMapPresentationActorStep step;
    step.resource_id = resource_id;
    step.actor_mode = actor_mode;
    auto finish = [&](Status status) { *out = step; return status; };
    if (resource_id == UINT32_MAX) return finish(Status::NoEffects);
    step.lookup_id = resource_id == 0x4b ? 0x27 : resource_id == 0x4c ? 0x28 : resource_id;
    if (!snapshot.registrations) return finish(Status::RequiresLookup);
    for (size_t i = 0; i < snapshot.registrations->size(); ++i) {
        const auto& entry = (*snapshot.registrations)[i];
        if (entry.id_d0 == step.lookup_id) {
            step.matched_registration = i;
            step.actor_identity = entry.actor_d4;
            break; // A null first match still shadows all later matches.
        }
    }
    if (step.actor_identity == 0) {
        step.used_fallback = true;
        if (!snapshot.fallback) return finish(Status::RequiresLookup);
        const auto& fallback = *snapshot.fallback;
        if (fallback.requested_id != resource_id || (fallback.active_c != 0 && fallback.actor_identity == 0)) {
            return Status::InvalidInput;
        }
        if (fallback.active_c != 0) step.actor_identity = fallback.actor_identity;
    }
    step.resource_slot = world_map_presentation_resource_slot(resource_id);
    if (step.resource_slot != 0) {
        // The source checks this even when actor lookup returned null.
        if (!snapshot.resource) return finish(Status::RequiresResourceState);
        const auto& resource = *snapshot.resource;
        if (resource.slot != step.resource_slot) return Status::InvalidInput;
        bool ready = false;
        if (resource.object_present) {
            if (!resource.flag_query_1) return finish(Status::RequiresResourceState);
            // FUN_8005BCE8 uses the entire bit-query word, not its low byte.
            if (*resource.flag_query_1 != 0) {
                if (!resource.global_lookup_result) return finish(Status::RequiresResourceState);
                ready = *resource.global_lookup_result != UINT32_MAX || resource.word_130 == 6 || resource.word_130 == 7;
            } else {
                ready = resource.state_120 == 1 || resource.state_120 == 3 || resource.result_124 == UINT32_MAX;
            }
        }
        step.resource_ready = ready;
        if (!ready || step.actor_identity == 0) return finish(Status::NoEffects);
        auto& command = step.calls[0];
        command.kind = WorldMapPresentationActorCallKind::ResourceCommand; // FUN_8007C324.
        command.actor_identity = step.actor_identity;
        command.arguments = {resource_id, actor_mode, resource.word_12c, 0, resource.word_130};
        command.argument_count = 5;
    } else {
        if (step.actor_identity == 0) return finish(Status::NoEffects);
        auto& command = step.calls[0];
        command.kind = WorldMapPresentationActorCallKind::DirectCommand; // FUN_8007BDB0.
        command.actor_identity = step.actor_identity;
        command.arguments = {resource_id, actor_mode, 0, 0, 0};
        command.argument_count = 4;
    }
    auto& toggle = step.calls[1]; // FUN_8007C910, after the command returns.
    toggle.kind = WorldMapPresentationActorCallKind::Toggle;
    toggle.actor_identity = step.actor_identity;
    toggle.arguments[0] = actor_mode == 5 ? 1u : 0u;
    toggle.argument_count = 1;
    step.call_count = 2;
    return finish(Status::RequiresActorCalls);
}

} // namespace awl
