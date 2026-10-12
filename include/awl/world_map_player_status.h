#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace awl {
// CPU projection of player +135C's saved-owner subobject, not a serialized
// save format or the entire constructor. Raw words preserve PPC wrapping
// arithmetic/signed comparisons. The update changes only +4/+14/+150/+154.
struct WorldMapPlayerStatusState {
    uint32_t word_4 = 0, word_8 = 0, word_c = 0;
    uint32_t word_14 = 0, word_18 = 0, word_1c = 0, word_20 = 0;
    uint32_t word_24 = 0, word_28 = 0;
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
    UnsupportedRateIndex, InvalidInput, UnsupportedNumerics, AlreadyInitialized
};
// 8004B9E4 reads these three r2 words in this order. Keep them separate from
// the mutable r13 coefficients consumed by the per-update status path.
struct WorldMapPlayerStatusResetValues {
    uint32_t word_4 = 0, word_20 = 0, word_14 = 0;
};
// 8004B26C's complete projection onto the fields above. 105BC copies the
// supplied phase raw time; 10210 adds 216000 seconds * 10 with uint32 wrap.
// A single phase snapshot is used for native atomic preparation. This does
// not construct the omitted arrays/vtables/items or restore a saved game.
[[nodiscard]] WorldMapPlayerStatusCode construct_world_map_player_status(
    std::optional<uint32_t> phase_raw, WorldMapPlayerStatusState* out) noexcept;
// 8004B9E4: ordered signed lower-then-upper clamps for +4, +20, then +14.
// All other projected fields remain unchanged. Aliasing is supported.
[[nodiscard]] WorldMapPlayerStatusCode reset_world_map_player_status(
    const WorldMapPlayerStatusState& before, const WorldMapPlayerStatusResetValues& values,
    WorldMapPlayerStatusState* out) noexcept;
struct WorldMapPlayerStatusStep {
    WorldMapPlayerStatusCode status = WorldMapPlayerStatusCode::RequiresGlobal;
    std::optional<WorldMapPlayerStatusGlobal> required_global;
    // Present only on complete preparation; never an accepted partial prefix.
    std::optional<WorldMapPlayerStatusState> after;
};
// CDB8's elapsed factor, including wrapped subtraction, saturation and the
// ordered binary32 sine curve. Curve evaluation requires FE_TONEAREST;
// rejection preserves output and the caller's rounding mode.
[[nodiscard]] WorldMapPlayerStatusCode prepare_world_map_player_status_elapsed(
    uint32_t saved_raw, uint32_t phase_raw, uint32_t* out) noexcept;
// Complete C87C/CC14 and 32074 -> C7E8/BE3C, including the elapsed factor
// and CC4C/CC84/CCA4/CD30. Descriptor +8 comes from the retained
// current holder record, not an animation clip index. No allocation/mutation.
[[nodiscard]] WorldMapPlayerStatusStep prepare_world_map_player_status(
    const WorldMapPlayerStatusState& before, uint32_t descriptor_word_8,
    const WorldMapPlayerStatusInputs& inputs) noexcept;
} // namespace awl
