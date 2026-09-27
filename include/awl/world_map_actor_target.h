#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace awl {

// One caller-supplied state record visited by FUN_8015A6D4. Entry zero is
// the runtime index at state +0x27E54; entries 1..36 are fixed indices 2..37.
// category/position come from indexed state arrays. relationship_score is
// the unsigned result of the still-untranslated FUN_80077798 lookup.
struct WorldMapFirstActorTargetCandidate {
    int32_t index = 0;
    int32_t category = 0;
    std::array<float, 3> position{};
    uint32_t relationship_score = 50;
};

struct WorldMapFirstActorTargetState {
    int32_t actor_id = 0;
    bool active = false; // actor +0xBC
    uint32_t update_flags = 0;
    std::array<float, 3> position{}; // actor +0xFC
    int32_t mode = 0; // actor +0xC4
    int32_t action_timer = 0; // actor +0xE4
    int32_t fallback_timer = 0; // actor +0xE8
    bool moving = false; // actor +0xC1 == 1
};

enum class WorldMapFirstActorHandler : uint8_t {
    None, Idle, FirstTarget, SecondTarget
};

struct WorldMapFirstActorTargetDecision {
    WorldMapFirstActorTargetState state{};
    bool selector_ran = false;
    bool position_write_allowed = false;
    int32_t previous_mode = 0; // actor +0xC8 after selection
    int32_t first_target_index = -1;
    int32_t second_target_index = -1;
    std::optional<uint32_t> first_score;
    std::optional<uint32_t> second_score;
    WorldMapFirstActorHandler handler = WorldMapFirstActorHandler::None;
    int32_t handler_target_index = -1;
    std::optional<uint32_t> handler_score;
};

// Isolates FUN_8015A22C's active/flag/timer gate and FUN_8015A6D4's two
// ordered target scans, mode choice, and handler dispatch decision. The
// caller supplies all 37 state records and an RNG word only when dispatch
// is required. Handler bodies, score lookup, and game-owned state are not
// implemented. Failure leaves the output unchanged.
[[nodiscard]] bool select_world_map_first_actor_target(
    const WorldMapFirstActorTargetState& state,
    const std::array<WorldMapFirstActorTargetCandidate, 37>& candidates,
    std::optional<uint32_t> dispatch_rng_word,
    WorldMapFirstActorTargetDecision* decision);

} // namespace awl
