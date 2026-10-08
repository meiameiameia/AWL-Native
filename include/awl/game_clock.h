#pragma once
#include <cstdint>

namespace awl {
// CRT clears r13-551C/-5520; the DOL's r13-6048 initializer is one.
// Raw time is the game's rounded counter, not native elapsed milliseconds.
struct GameClockState {
    uint32_t raw_time = 0;
    uint32_t loop_count = 0;
    uint32_t retrace_interval = 1;
};
// FUN_8017A304's raw/tick prefix through 8017A364, including 80235DA4.
// Each conversion/multiply/divide/add rounds to binary32, then truncates
// unsigned with saturation. Loop count wraps independently. Zero interval
// still rounds the prior raw time and increments the loop count.
// Requires the default nearest rounding mode; a stop preserves out.
// Whole-state input/output alias is supported. No allocation or waiting.
[[nodiscard]] bool prepare_game_clock_advance(const GameClockState& state,
    GameClockState* out) noexcept;

// Shared CPU owner with justified fresh-process fields. The caller advances
// once per completed original-equivalent game loop, after presentation.
// No QPC/PAD/Present scheduling, OS-time field or holder sequence execution.
class GameClock {
public:
    const GameClockState& state() const noexcept { return state_; }
    // FUN_8017A398 is a pure word store; it does not reset either counter.
    void set_retrace_interval(uint32_t interval) noexcept { state_.retrace_interval = interval; }
    [[nodiscard]] bool advance() noexcept { return prepare_game_clock_advance(state_, &state_); }
private:
    GameClockState state_;
};
} // namespace awl
