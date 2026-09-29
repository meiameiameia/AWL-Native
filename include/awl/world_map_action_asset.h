#pragma once

#include "awl/world_map_event_conditions.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

// FUN_8017837C/80178450, bounded to the single-block CLZ version-zero
// shape used by the two currently selected Common.arc entries.
// output_limit bounds allocation. Failure preserves the caller's output.
[[nodiscard]] bool decode_world_map_action_clz(
    const uint8_t* data, size_t size, size_t output_limit,
    std::vector<uint8_t>* out);

// Owns the flat /files/Common.arc selected for owner +0x44E4. Indices
// retain the DOL's ARC node numbering, including the non-file root at zero.
// Decoded bytes remain opaque: this does not start an action or scene.
class WorldMapGlobalActionArchive {
public:
    [[nodiscard]] bool load();
    [[nodiscard]] bool parse(std::vector<uint8_t> bytes);
    void clear();

    [[nodiscard]] bool loaded() const { return !entries_.empty(); }
    [[nodiscard]] size_t node_count() const { return entries_.size(); }
    [[nodiscard]] bool decode_prepared_request(
        const WorldMapMovementRequestPreparation& request,
        size_t output_limit, std::vector<uint8_t>* out) const;

private:
    struct Entry {
        uint32_t offset = 0;
        uint32_t size = 0;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
};

} // namespace awl
