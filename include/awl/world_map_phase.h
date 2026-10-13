#pragma once
#include <array>
#include <cstdint>

namespace awl {
namespace detail {
inline constexpr std::array<uint32_t,6> world_map_phase_lengths{1u,2u,3u,2u,1u,1u};
inline constexpr uint32_t world_map_phase_unit=34560000u;
}
// FFB0 -> FEE8, reused by collision selection and actor appearance. Raw
// phase-owner time is distinct from GameClock. All unsigned words yield 0..5.
[[nodiscard]] constexpr uint32_t derive_world_map_phase(uint32_t raw) noexcept {
    uint32_t remaining=raw/detail::world_map_phase_unit,phase=0;
    for(const auto threshold:detail::world_map_phase_lengths) {
        if (remaining<threshold) break;
        remaining-=threshold;++phase;
    }
    return phase<6?phase:5;
}
} // namespace awl
