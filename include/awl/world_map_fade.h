#pragma once
#include "awl/world_map_update_inputs.h"
#include "awl/world_map_player_actor_schedule.h"
#include <array>
#include <utility>

namespace awl {
using WorldMapFadeColor = std::array<uint8_t,4>;
// B0D0's bounded fade fields. +54/+5C/+64 are constructor-unwritten.
// registered is native membership bookkeeping, not a packed original field.
struct WorldMapFadeState {
    WorldMapFadeColor from_4c{}, to_50{};
    std::optional<WorldMapFadeColor> color_54;
    float duration_58 = 0;
    std::optional<uint32_t> started_5c;
    int32_t mode_60 = 0;
    std::optional<uint8_t> hold_64;
    uint32_t render_priority_68 = 0xcffe;
    bool registered = false;
};
enum class WorldMapFadeStatus {
    Prepared, Started, Advanced, Idle, AlreadyAdvanced, Filtered,
    RequiresClock, RequiresControl, RequiresStartTime, RequiresHold,
    RequiresRenderQueue, InvalidQueue, UnsupportedPriorityChange,
    UnsupportedMode, UnsupportedNumerics, InvalidInput, AllocationFailure,
    RequiresNextLoop
};
struct WorldMapFadeStep {
    WorldMapFadeState after;
    bool schedule_render = false;
};
// 89128's supported direct start fields and 188D58/188FFC CPU update.
// Nearest binary32 arithmetic, unsigned elapsed wrap and STRICT >1 completion.
// Negative/nonfinite durations and unknown modes stop explicitly. Mode zero
// reads no timing/color data; mode two ignores old duration/start/hold fields.
// Whole-state aliasing is supported; every stop preserves *out.
[[nodiscard]] WorldMapFadeStatus prepare_world_map_fade_start(
    const WorldMapFadeState&, float seconds, const WorldMapFadeColor& from,
    const WorldMapFadeColor& to, uint8_t hold, uint8_t front,
    std::optional<uint32_t> now, WorldMapFadeState* out) noexcept;
[[nodiscard]] WorldMapFadeStatus prepare_world_map_fade_update(
    const WorldMapFadeState&, std::optional<uint32_t> now, WorldMapFadeStep* out) noexcept;

struct WorldMapFadeLoop {
    uint32_t loop_count = 0, raw_time = 0;
    bool valid = true;
    uint64_t render_owner = 0; // borrowed identity; never dereferenced
};
// Owned CPU fade with the shared raw clock, fixed primary registration at
// 10000 (group 16) and a stable render key. No GX/DX11 drawing or shared-list
// activation. The caller retains the render queue through queued completion.
class WorldMapFade {
public:
    explicit WorldMapFade(std::shared_ptr<GameClock> clock) noexcept : clock_(std::move(clock)) {}
    WorldMapFade(const WorldMapFade&) = delete;
    WorldMapFade& operator=(const WorldMapFade&) = delete;
    const WorldMapFadeState& state() const noexcept { return state_; }
    const std::shared_ptr<GameClock>& game_clock() const noexcept { return clock_; }
    const std::optional<WorldMapFadeLoop>& loop() const noexcept { return loop_; }
    uint64_t primary_node_identity() const noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&primary_node_)); }
    uint64_t render_node_identity() const noexcept { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&render_node_)); }
    [[nodiscard]] WorldMapFadeStatus start(float seconds,const WorldMapFadeColor& from,
        const WorldMapFadeColor& to,uint8_t hold,uint8_t front) noexcept;
    // 1462C..14654 after clear world-map setup guards: opaque black ->
    // transparent black, one second, no hold, render priority CFFE.
    // The caller must justify that setup branch; this does not construct it.
    [[nodiscard]] WorldMapFadeStatus start_world_map_entry() noexcept {
        return start(1.0f,{0,0,0,255},{0,0,0,0},0,0);
    }
    // State/color/membership and ordered render insertion publish together.
    // Retry reuses a completed same-loop sample; rebinding its queue rejects.
    // Failures preserve pending state/receipt/queue. Never advances the clock.
    [[nodiscard]] WorldMapFadeStatus advance_for_loop(
        const WorldMapUpdateInputs&,WorldMapPlayerActorQueue* render_queue);
private:
    std::shared_ptr<GameClock> clock_;
    WorldMapFadeState state_;
    std::optional<WorldMapFadeLoop> loop_;
    uint8_t primary_node_ = 0, render_node_ = 0;
};
} // namespace awl
