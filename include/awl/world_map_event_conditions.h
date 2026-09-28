#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace awl {

struct WorldMapEventConditionEntry {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const char* name = nullptr;
};

// Owns the ARC selected for owner +0x4538 by FUN_8010A6D8. Its entries
// remain opaque until FUN_80126B40's condition decoder is translated.
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
