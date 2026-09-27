#pragma once

#include "awl/world_map_actor_target.h"

#include <cstdint>
#include <optional>

namespace awl {

enum class WorldMapFirstActorActionRoute : uint8_t {
    WeightedTable,
    Call8015B460,
    Call8015BDA8
};

struct WorldMapFirstActorActionChoice {
    WorldMapFirstActorActionRoute route =
        WorldMapFirstActorActionRoute::WeightedTable;
    // The exact source table and selected row are present only on the
    // weighted path. The other routes require their separate function bodies.
    uint32_t table_address = 0;
    uint8_t table_row = 0;
    std::optional<int32_t> action_code; // actor +0xD8
    std::optional<int32_t> action_parameter; // actor +0xE0
    std::optional<uint8_t> animation_group; // FUN_8015C54C result
};

// Continues a dispatched FUN_8015A6D4 decision through the action routing
// in FUN_8015AA24 / FUN_8015AEE0 / FUN_8015B778. current_action and
// previous_action are actor +0xD8/+0xDC. A primary RNG word is required
// only for a weighted table; a second word is required for the variant-3
// idle override when previous_action == 3. The direct-call routes report
// their dependency without inventing the resulting state. Failure is atomic.
[[nodiscard]] bool choose_world_map_first_actor_action(
    const WorldMapFirstActorTargetDecision& selection,
    int32_t current_action, int32_t previous_action,
    std::optional<uint32_t> table_rng_word,
    std::optional<uint32_t> override_rng_word,
    WorldMapFirstActorActionChoice* choice);

} // namespace awl
