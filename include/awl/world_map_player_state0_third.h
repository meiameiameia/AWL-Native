#pragma once
#include "awl/world_map_player_context_update.h"
#include "awl/world_map_player_update.h"
#include "awl/world_map_update_inputs.h"

namespace awl {
struct WorldMapPlayerState0ThirdState {
    std::optional<uint32_t> control_word_1384;
    // Supplied observed UI/player/secondary projection, not a constructor.
    std::optional<WorldMapPlayerContextUpdateState> context;
};
struct WorldMapPlayerState0ThirdInput {
    std::optional<WorldMapPlayerUpdateGuards> guards;
    std::optional<WorldMapPlayerContextUpdateInput> context;
};
enum class WorldMapPlayerState0ThirdStatus {
    Prepared,Filtered,SkippedByGuard,AlreadyCompleted,RequiresControl,RequiresGuards,
    RequiresGuardWord58c,RequiresContextInput,RequiresPrefixProfile,RequiresPrefixAction,
    RequiresContextState,ContextIncomplete,RequiresPad,RequiresButtonAction,
    RequiresPrimary,RequiresAnimation,RequiresSecondary,InvalidInput
};
struct WorldMapPlayerState0ThirdResult {
    WorldMapPlayerState0ThirdStatus status=WorldMapPlayerState0ThirdStatus::InvalidInput;
    std::optional<WorldMapPlayerContextUpdateStep> context;
    std::optional<uint32_t> pressed,required_button;
};
struct WorldMapPlayerState0ThirdLoop {
    uint32_t loop_count=0,raw_time=0;
    uint64_t inputs_owner=0;
    WorldMapPlayerState0ThirdStatus completion=WorldMapPlayerState0ThirdStatus::Prepared;
};
// 31504's state-zero outer guards -> 42C5C -> 43120's control/scene prefix,
// supported context transaction and idle pressed-bit exits. Eligibility,
// control, scene-manager +68, icon clock and PAD come only from WorldMapUpdateInputs.
// Nonzero/non-two action reaches complete 1CD18; a false result stops before
// untranslated 1CD78/action creation. A/Y branches remain explicit stops.
// Every stop preserves *out, including a late PAD/button dependency after
// context staging. Filtered does not store control; guard skips do. No heap,
// clock/input advancement, full primary-pass/queue or gameplay receipt.
[[nodiscard]] WorldMapPlayerState0ThirdResult prepare_world_map_player_state0_third(
    const WorldMapUpdateInputs&,const WorldMapPlayerContextUpdateSources&,
    const WorldMapPlayerState0ThirdState&,const WorldMapPlayerState0ThirdInput&,
    WorldMapPlayerState0ThirdState* out) noexcept;
} // namespace awl
