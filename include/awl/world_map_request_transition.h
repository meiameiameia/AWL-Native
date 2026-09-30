#pragma once

#include <cstdint>

namespace awl {

// Represented request-manager/active-transition fields only. The original
// active object is selected by manager +0x5C; no live ownership is supplied here.
struct WorldMapRequestTransitionState {
    uint32_t manager_state_48 = 0;
    uint32_t manager_result_1c0 = UINT32_MAX;
    uint32_t manager_resource_60 = UINT32_MAX;
    bool has_active_transition = false;
    uint32_t active_state_20 = 0;
    uint32_t clock_current_28 = 0;
    uint32_t clock_end_2c = 0;
    uint32_t clock_begin_30 = 0;
    uint32_t clock_duration_34 = 0;
};

enum class WorldMapRequestTransitionStatus {
    Advanced,
    Idle,
    Unchanged,
    RequiresActiveTransition,
    RequiresPresentation,
    RequiresResourceRelease,
    InvalidState,
};

// Bounded update-field portion of FUN_801023DC/80102458/801025E8, after
// the caller's ordered registration through FUN_8017CF0C. Uses a supplied raw
// clock word from FUN_8017A2FC, not milliseconds or an invented frame duration.
// Idle does nothing. State 3 needs its untranslated presentation backend.
// Closing completion preserves all state until resource release is supported;
// no update here makes the manager idle or changes its resource/result fields.
[[nodiscard]] WorldMapRequestTransitionStatus advance_world_map_request_transition(
    WorldMapRequestTransitionState* state, uint32_t clock);

// FUN_801022A8 -> FUN_80182644: states 1..3 start closing and reset its clock.
// Idle/already-closing requests do nothing. No presentation/resource release.
[[nodiscard]] WorldMapRequestTransitionStatus close_world_map_request_transition(
    WorldMapRequestTransitionState* state, uint32_t clock);

} // namespace awl
