#include "awl/world_map_event_conditions.h"

#include "awl/filesystem.h"
#include "awl/platform.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>

namespace awl {
namespace {

constexpr std::array<const char*, 6> kPhasePaths{{
    "/files/EventCondition_C00.arc", "/files/EventCondition_C01.arc",
    "/files/EventCondition_C02.arc", "/files/EventCondition_C03.arc",
    "/files/EventCondition_C04.arc", "/files/EventCondition_C05.arc"}};
constexpr uint32_t kArcMagic = 0x55AA382Du;
constexpr uint32_t kNodeSize = 12;

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool in_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

} // namespace

void WorldMapEventConditions::clear() {
    bytes_.clear();
    entries_.clear();
    phase_ = -1;
}

bool WorldMapEventConditions::parse(std::vector<uint8_t> bytes,
                                    uint32_t phase) {
    clear();
    if (phase >= kPhasePaths.size() || bytes.size() < 0x20 ||
        be32(bytes.data()) != kArcMagic) {
        return false;
    }
    const uint32_t nodes = be32(bytes.data() + 4);
    const uint32_t metadata_size = be32(bytes.data() + 8);
    const uint32_t data_start = be32(bytes.data() + 12);
    if (nodes < 0x20 || nodes > data_start ||
        !in_range(bytes.size(), nodes, metadata_size) ||
        static_cast<uint64_t>(nodes) + metadata_size > data_start ||
        !in_range(bytes.size(), nodes, kNodeSize)) {
        return false;
    }
    const uint32_t count = be32(bytes.data() + nodes + 8);
    const uint64_t string_start = static_cast<uint64_t>(nodes) +
                                  static_cast<uint64_t>(count) * kNodeSize;
    const uint64_t string_end = static_cast<uint64_t>(nodes) + metadata_size;
    if (count < 2 || string_start >= string_end ||
        string_end > data_start ||
        !in_range(bytes.size(), nodes,
                  static_cast<uint64_t>(count) * kNodeSize) ||
        be32(bytes.data() + nodes) != 0x01000000u ||
        be32(bytes.data() + nodes + 4) != 0) {
        return false;
    }

    std::vector<Entry> parsed;
    parsed.reserve(count - 1);
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    extents.reserve(count - 1);
    for (uint32_t index = 1; index < count; ++index) {
        const size_t node = static_cast<size_t>(nodes) +
                            static_cast<size_t>(index) * kNodeSize;
        const uint32_t name_field = be32(bytes.data() + node);
        const uint32_t offset = be32(bytes.data() + node + 4);
        const uint32_t size = be32(bytes.data() + node + 8);
        const uint64_t name_at = string_start + (name_field & 0x00FFFFFFu);
        if ((name_field >> 24) != 0 || name_at >= string_end ||
            offset < data_start || size == 0 ||
            !in_range(bytes.size(), offset, size)) {
            return false;
        }
        const void* end = std::memchr(bytes.data() + name_at, 0,
                                     static_cast<size_t>(string_end - name_at));
        if (end == nullptr || end == bytes.data() + name_at) {
            return false;
        }
        Entry entry;
        entry.offset = offset;
        entry.size = size;
        entry.name.assign(
            reinterpret_cast<const char*>(bytes.data() + name_at),
            static_cast<const char*>(end));
        parsed.push_back(std::move(entry));
        extents.emplace_back(offset, static_cast<uint64_t>(offset) + size);
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i) {
        if (extents[i].first < extents[i - 1].second) {
            return false;
        }
    }
    bytes_.swap(bytes);
    entries_.swap(parsed);
    phase_ = static_cast<int32_t>(phase);
    return true;
}

bool WorldMapEventConditions::load_phase(uint32_t phase) {
    clear();
    if (phase >= kPhasePaths.size()) {
        return false;
    }
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file(kPhasePaths[phase], &raw, &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    const auto* first = static_cast<const uint8_t*>(raw);
    if (first == nullptr ||
        !parse(std::vector<uint8_t>(first, first + size), phase)) {
        AWL_LOG_ERROR("Unsupported or malformed event condition archive: %s",
                      kPhasePaths[phase]);
        return false;
    }
    return true;
}

bool WorldMapEventConditions::entry(
    size_t index, WorldMapEventConditionEntry* out) const {
    if (out != nullptr) {
        *out = {};
    }
    if (out == nullptr || !loaded() || index >= entries_.size()) {
        return false;
    }
    const Entry& selected = entries_[index];
    out->data = bytes_.data() + selected.offset;
    out->size = selected.size;
    out->name = selected.name.c_str();
    return true;
}

} // namespace awl
