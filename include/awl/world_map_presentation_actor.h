#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {

struct WorldMapPresentationActorRegistration {
    uint32_t id_d0 = 0;
    uint64_t actor_d4 = 0; // Native identity; zero represents the original null.
};

struct WorldMapPresentationActorFallback {
    uint32_t requested_id = 0;
    uint64_t actor_identity = 0;
    uint8_t active_c = 0;
};

struct WorldMapPresentationActorResource {
    uint32_t slot = 0;
    bool object_present = false;
    // Explicitly supplied results of FUN_8018972C(index 1) and, when used,
    // FUN_8005D12C. Missing is unknown, not a false/failed query result.
    std::optional<uint32_t> flag_query_1;
    std::optional<uint32_t> global_lookup_result;
    uint32_t state_120 = 0;
    uint32_t result_124 = UINT32_MAX;
    uint32_t word_12c = 0;
    uint32_t word_130 = 0;
};

struct WorldMapPresentationActorSnapshot {
    // Complete ordered supplied list. Missing means ownership is unknown.
    std::optional<std::vector<WorldMapPresentationActorRegistration>> registrations;
    // Observed FUN_8007E248(requested ID, 0) object and its byte +0xC.
    // This does not implement that function's fallback manager discovery.
    std::optional<WorldMapPresentationActorFallback> fallback;
    // Selected resource slot snapshot. object_present=false is known null.
    std::optional<WorldMapPresentationActorResource> resource;
};

enum class WorldMapPresentationActorCallKind { DirectCommand, ResourceCommand, Toggle };
struct WorldMapPresentationActorCall {
    WorldMapPresentationActorCallKind kind = WorldMapPresentationActorCallKind::DirectCommand;
    uint64_t actor_identity = 0;
    // r4 onward, in original order. DirectCommand has four argument words,
    // ResourceCommand five, Toggle one. Unused words are zero.
    std::array<uint32_t, 5> arguments{};
    uint32_t argument_count = 0;
};

enum class WorldMapPresentationActorStatus {
    NoEffects, RequiresLookup, RequiresResourceState, RequiresActorCalls, InvalidInput,
};

struct WorldMapPresentationActorStep {
    uint32_t resource_id = UINT32_MAX;
    uint32_t actor_mode = 0;
    uint32_t lookup_id = 0; // Only the registered-list search remaps 0x4B/4C.
    uint32_t resource_slot = 0;
    std::optional<size_t> matched_registration;
    bool used_fallback = false;
    uint64_t actor_identity = 0;
    std::optional<bool> resource_ready;
    std::array<WorldMapPresentationActorCall, 2> calls{};
    uint32_t call_count = 0;
};

// FUN_8007846C -> FUN_8007962C: signed full-word ID against 33 signed-byte
// key/value pairs. Returns the mapped slot 1..33, or zero on no match.
[[nodiscard]] uint32_t world_map_presentation_resource_slot(uint32_t resource_id);

// Ordered actor branch shared by zero/0x30 (mode 0) and resume/scroll/input
// (mode 5): FUN_8010A8D8 lookup, resource availability, command then toggle.
// RequiresActorCalls is an unaccepted call proposal, never execution. Even a
// NoEffects result does not acknowledge a presentation prefix or feedback.
// Other modes, stale fallback/slot keys, and inconsistent identities fail
// without changing output. Snapshot fields are consumed only when reached.
[[nodiscard]] WorldMapPresentationActorStatus prepare_world_map_presentation_actor(
    uint32_t resource_id, uint32_t actor_mode, const WorldMapPresentationActorSnapshot& snapshot,
    WorldMapPresentationActorStep* out);

} // namespace awl
