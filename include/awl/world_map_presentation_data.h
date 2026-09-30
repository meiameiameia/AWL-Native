#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace awl {

enum class WorldMapPresentationStop { NoStart, Terminator, Separator, UnitLimit };

struct WorldMapPresentationPass {
    uint32_t units = 0;
    std::optional<size_t> stop_offset;
    std::optional<size_t> next_offset;
    WorldMapPresentationStop stop = WorldMapPresentationStop::NoStart;
};

struct WorldMapPresentationWindow {
    std::array<WorldMapPresentationPass, 3> passes{};
    uint32_t units = 0;
};

struct WorldMapPresentationResume {
    size_t start_offset = 0;
    bool skipped_separator = false;
    WorldMapPresentationWindow window;
};

struct WorldMapPresentationAdvance {
    std::optional<size_t> start_offset;
    uint32_t dropped_units = 0;
    uint32_t remaining_revealed_units = 0;
    WorldMapPresentationWindow window;
};

enum class WorldMapPresentationDataStatus {
    Prepared, InvalidInput, TruncatedToken, MissingStopToken,
};

// Data-only portions of FUN_80102894/FUN_80103550/FUN_80102C68.
// Counting uses visitor 802BD2A0: three passes, each capped at 21 tag-2/8x
// units. At the cap, the extra countable token stops without a continuation;
// a separator continues after itself, while zero continues at itself.
// Offsets must be token boundaries before or at the first zero. Null start
// models the original null pointer, rather than pointing at byte zero.
// Failure preserves output. No resource, clock, actor, feedback, selection,
// presentation member dispatch, or live game state effects are performed.
[[nodiscard]] WorldMapPresentationDataStatus count_world_map_presentation_window(
    const uint8_t* data, size_t size, std::optional<size_t> start_offset,
    WorldMapPresentationWindow* out);

// Visitor 802BD1D8 skips at most one leading tag 1; a second separator, zero,
// or any other token is retained as the new start. Then recounts the window.
[[nodiscard]] WorldMapPresentationDataStatus resume_world_map_presentation_data(
    const uint8_t* data, size_t size, size_t start_offset, WorldMapPresentationResume* out);

// Drops one counting pass, subtracts its units from the supplied revealed
// word with original uint32 wrap, and recounts from its continuation.
// Caller owns the timing/eligibility decision; this does not execute an update.
[[nodiscard]] WorldMapPresentationDataStatus advance_world_map_presentation_data(
    const uint8_t* data, size_t size, std::optional<size_t> start_offset,
    uint32_t revealed_units, WorldMapPresentationAdvance* out);

} // namespace awl
