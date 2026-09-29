#include "awl/world_map_event_conditions.h"

#include "awl/filesystem.h"
#include "awl/platform.h"
#include "awl/world_map_collision_records.h"

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

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool in_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

uint32_t record_bits(const uint8_t* data, size_t first, uint32_t width) {
    uint32_t value = 0;
    for (uint32_t bit = 0; bit < width; ++bit) {
        const size_t source = first + bit;
        value |= static_cast<uint32_t>(
                     (data[source / 8] >> (source % 8)) & 1u) << bit;
    }
    return value;
}

uint32_t stage_group(int32_t stage) {
    if (stage == 4) {
        return 2;
    }
    return stage == 3 || stage == 5 ? 1u : 0u;
}

bool hour_window(uint32_t first, uint32_t length, uint32_t clock_ticks) {
    if (first >= 24) {
        return true;
    }
    const uint32_t hour = (clock_ticks / 36000u) % 24u;
    if (length == 0) {
        return hour == first && (clock_ticks / 600u) % 60u == 0;
    }
    const uint32_t end = first + length;
    if (first <= hour && hour < end) {
        return true;
    }
    return end >= 24 && hour < end - 24;
}

} // namespace

WorldMapMovementRecordStatus evaluate_world_map_movement_record(
    const WorldMapMovementRequestRecord& record,
    const WorldMapMovementEvaluationState& state,
    WorldMapMovementRecordDecision* out) {
    using Status = WorldMapMovementRecordStatus;
    if (out != nullptr) {
        *out = {};
    }
    if (out == nullptr || record.data == nullptr || record.size != 0x20 ||
        record_bits(record.data, 0, 10) != record.saved_condition_id) {
        return Status::InvalidInput;
    }
    // Four ordered descriptors at 0x802541F4. The first three sentinel
    // fields skip their predicates. FUN_801281F0(0) tests state +0x11CEC.
    if (record_bits(record.data, 40, 4) != 0xfu ||
        record_bits(record.data, 44, 10) != 0x3ffu ||
        record_bits(record.data, 254, 1) != 1u) {
        return Status::RequiresUntranslatedPredicate;
    }
    if (record_bits(record.data, 255, 1) == 0u && state.state_11cec != 0) {
        return Status::Rejected;
    }
    // The paired predicate precedes the six-descriptor table. Its active
    // path remains outside the local record shapes.
    if (record_bits(record.data, 167, 6) != 0x3fu) {
        return Status::RequiresUntranslatedPredicate;
    }
    const uint32_t stage_mask = record_bits(record.data, 27, 4);
    if (stage_mask != 0xfu) {
        if (stage_mask != 1u || record_bits(record.data, 31, 1) != 1u ||
            !state.has_time_state) {
            return Status::RequiresUntranslatedPredicate;
        }
        // FUN_80127DB4: a nonzero transition fraction must stay within the
        // same stage group before the current stage is tested by the mask.
        if ((state.transition_fraction != 0.0f &&
             stage_group(state.transition_stage) !=
                 stage_group(state.current_stage)) ||
            (stage_mask & (1u << stage_group(state.current_stage))) == 0) {
            return Status::Rejected;
        }
    }
    constexpr std::array<std::pair<size_t, uint32_t>, 9> kSkipFields{{
        {115, 10}, {126, 10}, {137, 9}, {147, 9}, {157, 9},
        {177, 6}, {208, 6}, {231, 6}, {32, 4},
    }};
    for (const auto& field : kSkipFields) {
        if (record_bits(record.data, field.first, field.second) !=
            ((1u << field.second) - 1u)) {
            return Status::RequiresUntranslatedPredicate;
        }
    }
    const uint32_t first_hour = record_bits(record.data, 54, 5);
    if (first_hour != 0x1fu) {
        if (!state.has_time_state) {
            return Status::RequiresUntranslatedPredicate;
        }
        if (!hour_window(first_hour, record_bits(record.data, 59, 5),
                         state.clock_ticks)) {
            return Status::Rejected;
        }
    }
    // FUN_80128218 and FUN_80128488 receive only wildcard arguments, so
    // neither asks the runtime item/state tables for a value.
    constexpr std::array<std::pair<size_t, uint32_t>, 8> kWildcardFields{{
        {64, 7}, {71, 7}, {78, 6}, {84, 6}, {90, 6},
        {96, 6}, {102, 6}, {108, 6},
    }};
    for (const auto& field : kWildcardFields) {
        if (record_bits(record.data, field.first, field.second) !=
            ((1u << field.second) - 1u)) {
            return Status::RequiresUntranslatedPredicate;
        }
    }
    // FUN_80128788 also takes default success for the local 0x178 slot-1
    // ID, which misses all cases below 0x17D.
    if ((record.saved_condition_id < 0x17du &&
         record.saved_condition_id != 0x178u) ||
        record.saved_condition_id == 0x3ffu) {
        return Status::RequiresUntranslatedPredicate;
    }
    out->action_index = record.saved_condition_id;
    out->decoder_flag = static_cast<uint8_t>(
        record_bits(record.data, 26, 1));
    return Status::EligibleForStateRequest;
}

WorldMapMovementRecordStatus evaluate_world_map_movement_record(
    const WorldMapMovementRequestRecord& record,
    uint8_t state_11cec,
    WorldMapMovementRecordDecision* out) {
    WorldMapMovementEvaluationState state;
    state.state_11cec = state_11cec;
    return evaluate_world_map_movement_record(record, state, out);
}

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

bool WorldMapEventConditions::select_movement_request_records(
    int32_t slot, const WorldMapPackedSavedValues& saved_values,
    std::vector<WorldMapMovementRequestRecord>* out) const {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    if ((slot != 0 && slot != 1) || !loaded()) {
        return false;
    }
    // FUN_80126B40 requests ARC entry (type + 1); the public index is
    // zero-based after the ARC root, so type 3 is entry index 3.
    WorldMapEventConditionEntry source;
    constexpr size_t record_size = 0x20;
    if (!entry(3, &source) || source.size < record_size ||
        source.size % record_size != 0) {
        return false;
    }
    std::vector<WorldMapMovementRequestRecord> selected;
    bool saw_sentinel = false;
    for (size_t offset = 0; offset < source.size; offset += record_size) {
        const uint32_t head = le32(source.data + offset);
        const uint32_t condition_id = head & 0x3ffu;
        if (condition_id == 0x3ffu) {
            saw_sentinel = offset + record_size == source.size;
            break;
        }
        uint32_t saved = 0;
        if (!read_world_map_packed_saved_value(
                saved_values, condition_id, &saved)) {
            return false;
        }
        if (saved != 0) {
            continue;
        }
        const uint32_t slot_filter = (head >> 10) & 0xffu;
        const uint32_t value_filter = (head >> 18) & 0xffu;
        if ((slot_filter != 0xffu &&
             slot_filter != static_cast<uint32_t>(slot)) ||
            (value_filter != 0xffu && value_filter != 0u)) {
            continue;
        }
        selected.push_back({source.data + offset, record_size,
                            offset / record_size, condition_id});
    }
    if (!saw_sentinel) {
        return false;
    }
    out->swap(selected);
    return true;
}

} // namespace awl
