#include "awl/world_map_selection_rows.h"

#include "awl/world_map_message_stream.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace awl {

WorldMapSelectionRowsStatus prepare_world_map_selection_rows(
    const uint8_t* data, size_t size, uint32_t row_count, WorldMapSelectionRows* out) {
    using Status = WorldMapSelectionRowsStatus;
    if (data == nullptr || out == nullptr || row_count == 0 || row_count > 0x7fffffffu ||
        size > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 4) return Status::InvalidInput;
    WorldMapSelectionRows rows;
    uint32_t current_width = 0;
    size_t offset = 0;
    while (offset < size) {
        WorldMapMessageToken token;
        const auto status = read_world_map_message_token(data, size, offset, &token);
        if (status == WorldMapMessageStreamStatus::TruncatedToken) return Status::TruncatedToken;
        if (status != WorldMapMessageStreamStatus::Decoded) return Status::InvalidInput;
        offset += token.byte_count;
        if (token.tag == 0 || token.tag == 1) {
            // FUN_8010742C finalizes a row for tag 1. FUN_801073FC also
            // invokes it for zero, then stops regardless of the row budget.
            rows.max_width_units = std::max(rows.max_width_units, current_width);
            current_width = 0;
            ++rows.row_count;
            if (rows.row_count == row_count) {
                rows.stop_token_offset = token.offset;
                rows.consumed_bytes = offset;
                rows.byte_budget = static_cast<uint32_t>(offset) + 1u;
                rows.aligned_storage_size = (rows.byte_budget + 3u) & ~3u;
                // The original sizing helpers operate on wrapped word values
                // before unsigned-word-to-single conversion.
                rows.width = static_cast<float>(rows.max_width_units * 24u + 48u);
                rows.height = static_cast<float>(rows.row_count * 32u);
                // FUN_800FD320 copies only when written + token_length is
                // strictly less than byte_budget - 1. At this final one-byte
                // token equality holds, so it writes zero instead and stops.
                rows.bytes.assign(data, data + token.offset);
                rows.bytes.push_back(0);
                *out = std::move(rows);
                return Status::Prepared;
            }
            if (token.terminator) return Status::InsufficientRows;
        } else if (token.tag == 2 || token.visitor_slot == 0xbc) {
            // The forwarding visitor calls +0x18 for tag 2 and +0xC0 for
            // 0x8x. Both selection routes reach FUN_801074EC.
            ++current_width;
        }
        // All other counting-visitor handlers delegate to +0xC, whose only
        // effect is adding the already-verified token length to the budget.
    }
    return Status::MissingStopToken;
}

} // namespace awl
