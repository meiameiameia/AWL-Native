#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace awl {
// Supplied CPU snapshot of player +135C's pointed owner. Raw words preserve
// PPC wrapping arithmetic and signed comparisons; these are not save fields
// or discovered constructor defaults. Only +4/+14/+150/+154 can change here.
struct WorldMapPlayerStatusState {
    uint32_t word_4 = 0, word_8 = 0, word_c = 0;
    uint32_t word_14 = 0, word_18 = 0, word_1c = 0, word_20 = 0;
    uint8_t byte_2e = 0, byte_32 = 0;
    uint32_t word_12c = 0, word_150 = 0, word_154 = 0, word_160 = 0;
};
// Reached r13 words, in address order. Later writers/ownership are not
// inferred from their initial DOL values. Unreached slots may stay unknown.
enum class WorldMapPlayerStatusGlobal : uint8_t {
    Word7ec0, Word7ebc, Word7eb0, Word7eac, Word7ea0,
    Word7e9c, Word7e98, Word7e94, Word7e90, Word7e8c,
    Word7e88, Word7e84, Word7e80, Count
};
struct WorldMapPlayerStatusRate {
    uint32_t descriptor_word_8 = 0;
    // Explicit observation of the reached unsigned halfword at
    // 802929E0 + code*2 or 80292A14 + (code-64)*2. No table length is inferred.
    uint16_t value = 0;
};
struct WorldMapPlayerStatusFactors {
    std::array<std::optional<uint32_t>, static_cast<size_t>(WorldMapPlayerStatusGlobal::Count)> globals;
    std::optional<WorldMapPlayerStatusRate> rate;
};
struct WorldMapPlayerStatusInputs : WorldMapPlayerStatusFactors {
    // Phase owner raw +0, separate from GameClock. Read by CDB8 only.
    std::optional<uint32_t> phase_raw;
};
enum class WorldMapPlayerStatusCode {
    Prepared, RequiresGlobal, RequiresRate, RequiresPhaseRaw,
    RequiresElapsedCurve, UnsupportedRateIndex
};
struct WorldMapPlayerStatusStep {
    WorldMapPlayerStatusCode status = WorldMapPlayerStatusCode::RequiresGlobal;
    std::optional<WorldMapPlayerStatusGlobal> required_global;
    // Present only on complete preparation; never an accepted partial prefix.
    std::optional<WorldMapPlayerStatusState> after;
};
// Complete C87C/CC14 and the integer branches of 32074 -> C7E8/BE3C,
// including CC4C/CC84/CCA4/CD30 and CDB8's constant-100 branch. The reached
// elapsed curve remains explicit. Descriptor +8 comes from the retained
// current holder record, not an animation clip index. No allocation/mutation.
[[nodiscard]] WorldMapPlayerStatusStep prepare_world_map_player_status(
    const WorldMapPlayerStatusState& before, uint32_t descriptor_word_8,
    const WorldMapPlayerStatusInputs& inputs) noexcept;
} // namespace awl
