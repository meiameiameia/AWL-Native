#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace awl {

struct WorldMapPackedSavedValues;

struct WorldMapEventConditionEntry {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const char* name = nullptr;
};

struct WorldMapMovementRequestRecord {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t record_index = 0;
    uint32_t saved_condition_id = 0;
};

// Owns the ARC selected for owner +0x4538 by FUN_8010A6D8. The type-3
// request header is partially decoded; later record conditions remain opaque.
// Entry views are invalidated by load_phase(), parse(), clear(), or destruction.
class WorldMapEventConditions {
public:
    [[nodiscard]] bool load_phase(uint32_t phase);
    [[nodiscard]] bool parse(std::vector<uint8_t> bytes, uint32_t phase);
    void clear();

    [[nodiscard]] bool loaded() const { return phase_ >= 0; }
    [[nodiscard]] int32_t phase() const { return phase_; }
    [[nodiscard]] size_t entry_count() const { return entries_.size(); }
    [[nodiscard]] bool entry(size_t index,
                             WorldMapEventConditionEntry* out) const;
    // FUN_80126B40's type-3 entry and initial saved-value/slot/value-zero
    // filters only. Matching records still require later condition checks.
    [[nodiscard]] bool select_movement_request_records(
        int32_t slot, const WorldMapPackedSavedValues& saved_values,
        std::vector<WorldMapMovementRequestRecord>* out) const;

private:
    struct Entry {
        uint32_t offset = 0;
        uint32_t size = 0;
        std::string name;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    int32_t phase_ = -1;
};

} // namespace awl
