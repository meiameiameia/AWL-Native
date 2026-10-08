#include "awl/game_clock.h"
#include <cfenv>
#include <limits>

namespace awl {
bool prepare_game_clock_advance(const GameClockState& state, GameClockState* out) noexcept {
    if (!out || std::fegetround() != FE_TONEAREST) return false;
    // 8034BA40/44 = 1000/60. fsubs also rounds both unsigned inputs to
    // binary32. Preserve the distinct fmuls/fdivs/fadds intermediates.
    const float interval = static_cast<float>(state.retrace_interval);
    const float scaled = 1000.0f * interval;
    const float increment = scaled / 60.0f;
    const float previous = static_cast<float>(state.raw_time);
    const float accumulated = previous + increment;
    auto next = state;
    // All reached inputs/results are finite and nonnegative. Never cast
    // 2^32 (including rounded UINT32_MAX) to uint32_t: DA4 returns all ones.
    next.raw_time = accumulated >= 0x1p+32f ? std::numeric_limits<uint32_t>::max() :
        static_cast<uint32_t>(accumulated);
    next.loop_count = state.loop_count + 1u;
    *out = next;
    return true;
}
} // namespace awl
