#pragma once

#include "awl/world_map_message_stream.h"
#include "awl/world_map_message_staging.h"
#include "awl/world_map_selection_rows.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapMessageKey {
    uint8_t bank = 0;
    uint32_t index = 0;
};

// FUN_800FB498 truncates the bank to a byte and remaps bank 59 across four
// item-description banks. Unsupported bank bytes fail and preserve output.
[[nodiscard]] bool resolve_world_map_message_key(
    uint32_t bank_word, uint32_t index_word, WorldMapMessageKey* out);

// The 64 verified FUN_800FB540 catalog entries, mapped to native disc paths.
// Accepts a resolved bank (no byte truncation); invalid values return null.
[[nodiscard]] const char* world_map_message_bank_path(uint32_t bank);

struct WorldMapMessageBounds {
    uint32_t offset = 0;
    size_t size = 0;
};

// Owns the supported MES container/index only. Entries remain opaque token
// bytes; bounds end at the next greater offset or file end, possibly including
// padding. Index order and aliases are preserved. The explicit helpers below
// compose bounded parsing/preparation; no live presentation or feedback runs.
class WorldMapMessageBank {
public:
    // The verified local shape has marker CDC3B0B0, a nonzero BE count,
    // and aligned offsets after the complete table. Reordered/aliased offsets
    // are supported. Failed parse/load clears previous ownership.
    [[nodiscard]] bool parse(std::vector<uint8_t> bytes);
    [[nodiscard]] bool load(uint32_t resolved_bank);
    void clear();
    [[nodiscard]] bool loaded() const { return !bytes_.empty(); }
    [[nodiscard]] size_t entry_count() const { return entries_.size(); }
    [[nodiscard]] size_t byte_size() const { return bytes_.size(); }
    // Missing/out-of-range inputs preserve output. Copies include opaque
    // alignment padding; they are not decoded text or validated token streams.
    [[nodiscard]] bool entry_bounds(uint32_t index, WorldMapMessageBounds* out) const;
    [[nodiscard]] bool copy_entry(uint32_t index, std::vector<uint8_t>* out) const;
    // Scans token boundaries within the indexed safety extent. No visitor
    // effects are executed; invalid indices and scan failure preserve output.
    [[nodiscard]] WorldMapMessageStreamStatus scan_entry(
        uint32_t index, WorldMapMessageStream* out) const;
    // Uses the same indexed safety extent for supplied-count row preparation.
    // This does not establish that the game uses this message as a selection.
    [[nodiscard]] WorldMapSelectionRowsStatus prepare_selection_rows(
        uint32_t index, uint32_t row_count, WorldMapSelectionRows* out) const;
    // Stages only the supported token subset within the indexed safety extent.
    [[nodiscard]] WorldMapMessageStagingStatus stage_entry(
        uint32_t index, const WorldMapMessageStagingContext& context,
        uint32_t byte_limit, WorldMapStagedMessage* out) const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<WorldMapMessageBounds> entries_;
};

} // namespace awl
