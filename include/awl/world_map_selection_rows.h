#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapSelectionRows {
    uint32_t row_count = 0;
    // FUN_801074EC units: tag 2 and the 0x8x family each add one.
    // This is not a decoded character count or native font measurement.
    uint32_t max_width_units = 0;
    uint32_t byte_budget = 0;
    // Original caller's four-byte-rounded request, not host capacity/alignment.
    uint32_t aligned_storage_size = 0;
    size_t stop_token_offset = 0;
    size_t consumed_bytes = 0;
    float width = 0;
    float height = 0;
    // Owned copied tokens through the requested row boundary, replacing the
    // final separator with zero. Includes that terminator, excludes slack.
    std::vector<uint8_t> bytes;
};

enum class WorldMapSelectionRowsStatus {
    Prepared,
    InvalidInput,
    TruncatedToken,
    MissingStopToken,
    InsufficientRows,
};

// Bounded counting/copying portion of FUN_80105110, its counting visitor,
// FUN_800FD320, and the two sizing helpers. A supplied count must be positive
// and signed-representable; a zero tag before that count is rejected. The
// input span is bounded to UINT32_MAX - 4 to prevent budget/alignment overflow.
// Does not allocate original resources, initialize active/selection state,
// interpret glyphs, draw, or accept a manager result. Failure preserves output.
[[nodiscard]] WorldMapSelectionRowsStatus prepare_world_map_selection_rows(
    const uint8_t* data, size_t size, uint32_t row_count, WorldMapSelectionRows* out);

} // namespace awl
