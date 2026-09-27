#include "awl/world_map_collision_records.h"

#include "awl/filesystem.h"
#include "awl/platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace awl {
namespace {

constexpr uint32_t kArcMagic = 0x55AA382Du;
constexpr size_t kArcHeaderSize = 0x20;
constexpr size_t kArcNodeSize = 12;

struct MapseTableRow {
    uint32_t id;
    uint32_t flag;
};

// 0x8023EB20: the 21 eight-byte rows read by FUN_800222B8/FUN_8001D714.
constexpr std::array<MapseTableRow, 21> kMapseTable{{
    {0, 3}, {1, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1},
    {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
    {9, 1}, {10, 1}, {11, 2}, {12, 2}, {13, 2}, {14, 1}, {15, 1}}};

struct RoomRecordIndexRow {
    uint8_t id;
    uint8_t record_index;
};

// Non-sentinel entries for input IDs 0..0x51 in 0x80299790. All other
// entries in that bounded range are 0xFFFFFFFF (no record).
constexpr std::array<RoomRecordIndexRow, 30> kRoomRecordIndices{{
    {3, 40}, {5, 41}, {6, 42}, {7, 52}, {9, 53}, {10, 49},
    {12, 44}, {19, 51}, {20, 10}, {23, 43}, {25, 9},
    {31, 12}, {32, 11}, {33, 6}, {34, 13}, {35, 7},
    {36, 14}, {40, 0}, {43, 8}, {45, 45}, {46, 46},
    {47, 47}, {49, 48}, {50, 52}, {51, 50},
    {77, 1}, {78, 2}, {79, 3}, {80, 4}, {81, 5}}};

struct RoomStaticConditionRow {
    uint32_t condition_id;
    uint32_t surface_bit;
};

// The 14 ordered (FUN_80024EBC condition ID, surface bit) pairs at 0x80299698.
constexpr std::array<RoomStaticConditionRow, 14> kRoomStaticConditions{{
    {0xA1, 0x0001}, {0x9E, 0x0002}, {0xEC, 0x0004},
    {0xEB, 0x2000}, {0x8D, 0x1000}, {0x8A, 0x0800},
    {0x11, 0x0400}, {0x17, 0x0200}, {0x18, 0x0040},
    {0x19, 0x0080}, {0x1A, 0x0010}, {0x1B, 0x0100},
    {0x1F, 0x0008}, {0x20, 0x0020}}};

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

WorldMapRoomCollisionStatus select_world_map_room_collision_record(
    uint32_t category,
    const WorldMapRoomCollisionState& state,
    WorldMapRoomCollisionSelection* out) {
    if (out != nullptr) {
        *out = {};
    }
    if (out == nullptr || category > 0x51u || state.phase_index > 5) {
        return WorldMapRoomCollisionStatus::UnsupportedInput;
    }
    uint32_t remapped = category;
    switch (category) {
    case 3:
        if (state.phase_index == 1) remapped = 0x2Du;
        else if (state.phase_index == 2) remapped = 0x2Eu;
        else if (state.phase_index >= 3) remapped = 0x2Fu;
        break;
    case 4:
        if (state.phase_index >= 3) remapped = 0x30u;
        break;
    case 5:
        if (state.phase_index >= 3) remapped = 0x31u;
        break;
    case 7:
        if (state.state_299a6) remapped = 0x32u;
        break;
    case 10:
        if (state.state_299a5) remapped = 0x33u;
        break;
    case 13:
        if (state.state_299ae) remapped = 0x34u;
        break;
    case 40:
        if (state.phase_index > 0) remapped = 0x4Cu + state.phase_index;
        break;
    default:
        break;
    }
    out->remapped_id = remapped;
    for (const RoomRecordIndexRow& row : kRoomRecordIndices) {
        if (row.id == remapped) {
            out->record_index = row.record_index;
            return WorldMapRoomCollisionStatus::Found;
        }
    }
    return WorldMapRoomCollisionStatus::NoMapping;
}

uint32_t world_map_room_static_surface_mask(
    const WorldMapRoomStaticConditions& conditions,
    uint32_t resolver_flags) {
    uint32_t mask = 0;
    for (size_t i = 0; i < kRoomStaticConditions.size(); ++i) {
        if (conditions[i]) {
            mask |= kRoomStaticConditions[i].surface_bit;
        }
    }
    if ((resolver_flags & 0x600u) != 0) {
        mask |= 0x8000u;
    }
    return mask;
}

std::array<uint32_t, 14> world_map_room_static_condition_ids() {
    std::array<uint32_t, 14> ids{};
    for (size_t i = 0; i < kRoomStaticConditions.size(); ++i) {
        ids[i] = kRoomStaticConditions[i].condition_id;
    }
    return ids;
}

bool read_world_map_packed_saved_value(
    const WorldMapPackedSavedValues& values,
    size_t index,
    uint32_t* out) {
    if (out != nullptr) {
        *out = 0;
    }
    if (out == nullptr || values.data == nullptr || values.width_bits == 0 ||
        values.width_bits > 32 ||
        index > std::numeric_limits<size_t>::max() / values.width_bits) {
        return false;
    }
    const size_t first_bit = index * values.width_bits;
    if (first_bit > std::numeric_limits<size_t>::max() -
                        (values.width_bits - 1)) {
        return false;
    }
    if ((first_bit + values.width_bits - 1) / 8 >= values.size) {
        return false;
    }
    uint32_t result = 0;
    for (uint32_t bit = 0; bit < values.width_bits; ++bit) {
        const size_t source_bit = first_bit + bit;
        result |= static_cast<uint32_t>(
                      (values.data[source_bit / 8] >> (source_bit % 8)) & 1u)
                  << bit;
    }
    *out = result;
    return true;
}

bool decode_world_map_room_saved_conditions(
    const WorldMapPackedSavedValues& values_120a4,
    const WorldMapPackedSavedValues& values_14ad4,
    WorldMapRoomConditionInputs* inputs) {
    if (inputs == nullptr) {
        return false;
    }
    WorldMapRoomConditionInputs decoded = *inputs;
    uint32_t value = 0;
    if (!read_world_map_packed_saved_value(values_120a4, 0x135, &value)) {
        return false;
    }
    decoded.value_120a4_135_nonzero = value != 0;
    if (!read_world_map_packed_saved_value(values_120a4, 0x170, &value)) {
        return false;
    }
    decoded.value_120a4_170_nonzero = value != 0;
    constexpr std::array<size_t, 8> kIndexedValues{
        0, 6, 7, 8, 9, 10, 14, 15};
    for (size_t index : kIndexedValues) {
        if (!read_world_map_packed_saved_value(values_14ad4, index, &value)) {
            return false;
        }
        decoded.values_14ad4_nonzero[index] = value != 0;
    }
    *inputs = decoded;
    return true;
}

bool evaluate_world_map_room_static_conditions(
    const WorldMapRoomConditionInputs& inputs,
    WorldMapRoomStaticConditions* out) {
    if (out != nullptr) {
        *out = {};
    }
    if (out == nullptr || inputs.phase_index > 5) {
        return false;
    }
    // FUN_80024EBC cases 0xEB and 0xEC use the same phase/0x170 decision.
    const bool phase_170 = inputs.phase_index == 0 ||
                           inputs.value_120a4_170_nonzero;
    (*out)[0] = inputs.state_299af == 0; // 0xA1
    (*out)[1] = inputs.state_299a7 != 0; // 0x9E
    (*out)[2] = phase_170;                 // 0xEC
    (*out)[3] = phase_170;                 // 0xEB
    (*out)[4] = std::any_of(
        inputs.counters_118bc_to_118be.begin(),
        inputs.counters_118bc_to_118be.end(),
        [](int8_t count) { return count > 0; }); // 0x8D
    (*out)[5] = inputs.value_120a4_135_nonzero; // 0x8A
    constexpr std::array<size_t, 8> kIndexedValues{
        0, 6, 7, 8, 9, 10, 14, 15};
    for (size_t i = 0; i < kIndexedValues.size(); ++i) {
        (*out)[6 + i] = inputs.values_14ad4_nonzero[kIndexedValues[i]];
    }
    return true;
}

bool resolve_type1_room_static_contact(
    uint32_t category,
    WorldMapRoomCollisionStatus lookup_status,
    const WorldMapCollisionRecordView* record,
    const WorldMapRoomStaticConditions& conditions,
    uint32_t resolver_flags,
    uint32_t carried_contact_flags,
    uint32_t prior_resolver_contact_bits,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& proposed_position,
    float moving_radius,
    WorldMapRoomStaticAdjustment* adjustment) {
    if (adjustment != nullptr) {
        *adjustment = {};
    }
    const auto finite = [](const std::array<float, 3>& point) {
        return std::isfinite(point[0]) && std::isfinite(point[1]) &&
               std::isfinite(point[2]);
    };
    if (adjustment == nullptr || category == 1 || category > 0x51u ||
        !finite(prior_position) || !finite(proposed_position) ||
        !std::isfinite(moving_radius) || moving_radius < 0.0f) {
        return false;
    }
    WorldMapRoomStaticAdjustment result;
    result.position = proposed_position;
    if ((resolver_flags & 1u) == 0) {
        *adjustment = result;
        return true;
    }
    if (lookup_status == WorldMapRoomCollisionStatus::NoMapping &&
        record == nullptr) {
        *adjustment = result;
        return true;
    }
    if (lookup_status != WorldMapRoomCollisionStatus::Found ||
        record == nullptr || record->data == nullptr || record->size == 0 ||
        record->analysis == nullptr || record->analysis->header_byte_6 != 0) {
        return false;
    }
    result.record_present = true;
    result.surface_mask = world_map_room_static_surface_mask(
        conditions, resolver_flags);
    if (!resolve_type1_dynamic_contact_narrow_phase(
            record->data, record->size, prior_position, proposed_position,
            moving_radius, result.surface_mask, carried_contact_flags & ~1u,
            &result.narrow_phase)) {
        return false;
    }
    result.contact = result.narrow_phase.contact;
    result.position = result.narrow_phase.position;
    if (result.contact && (prior_resolver_contact_bits & 5u) == 5u) {
        result.position[0] = prior_position[0];
        result.position[2] = prior_position[2];
        result.reverted_horizontal_to_prior = true;
    }
    *adjustment = result;
    return true;
}

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
            analysis.header_byte_6 != 0 || analysis.node_count != 1 ||
            analysis.leaf_count != 1) {
            return false;
        }
        Record record;
        record.offset = offset;
        record.size = size;
        record.name.assign(reinterpret_cast<const char*>(bytes.data() + name_at),
                           static_cast<const char*>(end));
        record.analysis = analysis;
        float radius_squared = 0.0f;
        for (size_t axis = 0; axis < 3; ++axis) {
            record.center_local[axis] =
                (analysis.root_min[axis] + analysis.root_max[axis]) * 0.5f;
            const float half_extent =
                record.center_local[axis] - analysis.root_min[axis];
            radius_squared += half_extent * half_extent;
        }
        record.radius_local = std::sqrt(radius_squared);
        if (!std::isfinite(record.radius_local)) {
            return false;
        }
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
    out->center_local = record.center_local;
    out->radius_local = record.radius_local;
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
    if (mapse_.record_count() != kMapseTable.size()) {
        AWL_LOG_ERROR("Mapse archive count does not match DOL table: %zu",
                      mapse_.record_count());
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

bool WorldMapCollisionRecordPools::find_mapse_matches(
    uint32_t id, std::vector<WorldMapMapseCollisionMatch>* out) const {
    if (out != nullptr) {
        out->clear();
    }
    if (out == nullptr || mapse_.record_count() != kMapseTable.size()) {
        return false;
    }
    for (size_t index = 0; index < kMapseTable.size(); ++index) {
        const MapseTableRow& row = kMapseTable[index];
        if (row.id != id) {
            continue;
        }
        WorldMapMapseCollisionMatch match;
        match.record_index = static_cast<uint32_t>(index);
        match.table_flag = row.flag;
        if (!mapse_.lookup(match.record_index, &match.record)) {
            out->clear();
            return false;
        }
        out->push_back(match);
    }
    return true;
}

WorldMapRoomCollisionStatus WorldMapCollisionRecordPools::lookup_room_object(
    uint32_t category,
    const WorldMapRoomCollisionState& state,
    WorldMapCollisionRecordView* out) const {
    if (out != nullptr) {
        *out = {};
    }
    if (out == nullptr) {
        return WorldMapRoomCollisionStatus::UnsupportedInput;
    }
    WorldMapRoomCollisionSelection selected;
    const WorldMapRoomCollisionStatus status =
        select_world_map_room_collision_record(category, state, &selected);
    if (status != WorldMapRoomCollisionStatus::Found) {
        return status;
    }
    if (!roomobj_.lookup(selected.record_index, out)) {
        return WorldMapRoomCollisionStatus::MissingRecord;
    }
    return WorldMapRoomCollisionStatus::Found;
}

} // namespace awl
