#include "awl/world_map_collision_records.h"

#include "awl/filesystem.h"
#include "awl/platform.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace awl {
namespace {

constexpr uint32_t kArcMagic = 0x55AA382Du;
constexpr size_t kArcHeaderSize = 0x20;
constexpr size_t kArcNodeSize = 12;

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool range_within(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

bool read_archive(const char* path, WorldMapCollisionArchive* archive) {
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file(path, &raw, &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    if (raw == nullptr || size == 0) {
        AWL_LOG_ERROR("Empty collision archive: %s", path);
        return false;
    }
    const auto* first = static_cast<const uint8_t*>(raw);
    if (!archive->parse(std::vector<uint8_t>(first, first + size))) {
        AWL_LOG_ERROR("Unsupported or malformed collision archive: %s", path);
        return false;
    }
    return true;
}

} // namespace

bool WorldMapCollisionArchive::parse(std::vector<uint8_t> bytes) {
    clear();
    if (bytes.size() < kArcHeaderSize || be32(bytes.data()) != kArcMagic) {
        return false;
    }
    const uint32_t nodes = be32(bytes.data() + 4);
    const uint32_t metadata_size = be32(bytes.data() + 8);
    const uint32_t data_start = be32(bytes.data() + 12);
    if (nodes < kArcHeaderSize || nodes > data_start ||
        !range_within(bytes.size(), nodes, metadata_size) ||
        static_cast<uint64_t>(nodes) + metadata_size > data_start ||
        !range_within(bytes.size(), nodes, kArcNodeSize)) {
        return false;
    }
    const uint32_t count = be32(bytes.data() + nodes + 8);
    const uint64_t strings = static_cast<uint64_t>(nodes) +
                             static_cast<uint64_t>(count) * kArcNodeSize;
    const uint64_t string_end = static_cast<uint64_t>(nodes) + metadata_size;
    if (count < 2 || strings >= string_end || string_end > data_start ||
        !range_within(bytes.size(), nodes,
                      static_cast<uint64_t>(count) * kArcNodeSize) ||
        be32(bytes.data() + nodes) != 0x01000000u ||
        be32(bytes.data() + nodes + 4) != 0) {
        return false;
    }

    std::vector<Record> records;
    records.reserve(count - 1);
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    extents.reserve(count - 1);
    for (uint32_t index = 1; index < count; ++index) {
        const size_t node = static_cast<size_t>(nodes) +
                            static_cast<size_t>(index) * kArcNodeSize;
        const uint32_t name_field = be32(bytes.data() + node);
        const uint32_t offset = be32(bytes.data() + node + 4);
        const uint32_t size = be32(bytes.data() + node + 8);
        const uint64_t name_at = strings + (name_field & 0x00FFFFFFu);
        if ((name_field >> 24) != 0 || name_at >= string_end ||
            offset < data_start || size == 0 ||
            !range_within(bytes.size(), offset, size)) {
            return false;
        }
        const void* end = std::memchr(bytes.data() + name_at, 0,
                                      static_cast<size_t>(string_end - name_at));
        if (end == nullptr || end == bytes.data() + name_at) {
            return false;
        }
        CollisionTreeAnalysis analysis;
        if (!analyze_type1_collision_asset(bytes.data() + offset, size,
                                           &analysis) ||
            analysis.header_byte_6 != 0) {
            return false;
        }
        Record record;
        record.offset = offset;
        record.size = size;
        record.name.assign(reinterpret_cast<const char*>(bytes.data() + name_at),
                           static_cast<const char*>(end));
        record.analysis = analysis;
        records.push_back(std::move(record));
        extents.emplace_back(offset,
                             static_cast<uint64_t>(offset) + size);
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i) {
        if (extents[i].first < extents[i - 1].second) {
            return false;
        }
    }
    bytes_.swap(bytes);
    records_.swap(records);
    return true;
}

void WorldMapCollisionArchive::clear() {
    bytes_.clear();
    records_.clear();
}

bool WorldMapCollisionArchive::lookup(
    uint32_t index, WorldMapCollisionRecordView* out) const {
    if (out != nullptr) {
        *out = {};
    }
    if (out == nullptr || index >= records_.size()) {
        return false;
    }
    const Record& record = records_[index];
    out->data = bytes_.data() + record.offset;
    out->size = record.size;
    out->analysis = &record.analysis;
    out->name = record.name.c_str();
    return true;
}

bool WorldMapCollisionRecordPools::load() {
    clear();
    if (!read_archive("/files/maperase.col.arc", &maperase_) ||
        !read_archive("/files/mapse.col.arc", &mapse_) ||
        !read_archive("/files/roomobj.col.arc", &roomobj_)) {
        clear();
        return false;
    }
    return true;
}

void WorldMapCollisionRecordPools::clear() {
    maperase_.clear();
    mapse_.clear();
    roomobj_.clear();
}

bool WorldMapCollisionRecordPools::lookup(
    int32_t group, uint32_t index, WorldMapCollisionRecordView* out) const {
    if (group == 1) return roomobj_.lookup(index, out);
    if (group == 2) return mapse_.lookup(index, out);
    return maperase_.lookup(index, out);
}

size_t WorldMapCollisionRecordPools::record_count(int32_t group) const {
    if (group == 1) return roomobj_.record_count();
    if (group == 2) return mapse_.record_count();
    return maperase_.record_count();
}

} // namespace awl
