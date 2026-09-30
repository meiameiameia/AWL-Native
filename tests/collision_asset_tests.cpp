#include "awl/collision_asset.h"
#include "awl/world_map_contact.h"
#include "awl/world_map_collision_registry.h"
#include "awl/world_map_actor_step.h"
#include "awl/world_map_actor_target.h"
#include "awl/world_map_actor_action.h"
#include "awl/world_map_actor_heading.h"
#include "awl/world_map_movement.h"
#include "awl/world_map_trigger_asset.h"
#include "awl/world_map_event_conditions.h"
#include "awl/world_map_action_asset.h"
#include "awl/world_map_request_transition.h"
#include "awl/world_map_selection.h"
#include "awl/world_map_message_asset.h"
#include "awl/world_map_presentation_data.h"
#include "awl/world_map_presentation.h"
#include "awl/world_map_feedback.h"
#include "awl/world_map_presentation_actor.h"
#include "awl/world_map_actor_animation.h"
#include "awl/world_map_animation_channel.h"
#include "awl/world_map_animation_initializer.h"
#include "awl/world_map_camera.h"
#include "awl/world_map_scene_index.h"
#include "awl/world_map_collision_assets.h"
#include "awl/development_wall_route.h"
#include "awl/world_map_collision_records.h"
#include "awl/filesystem.h"
#include "awl/memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void put_be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

void put_be_float(std::vector<uint8_t>& bytes, size_t offset, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_be32(bytes, offset, bits);
}

void put_be16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<uint8_t>(value);
}

void put_be_s16(std::vector<uint8_t>& bytes, size_t offset, int16_t value) {
    put_be16(bytes, offset, static_cast<uint16_t>(value));
}

void initialize_header(std::vector<uint8_t>& bytes) {
    put_be32(bytes, 0, 0xE7E3F1F4u);
    bytes[4] = 1;
    bytes[5] = 6;
    bytes[6] = 1;
}

void initialize_leaf(std::vector<uint8_t>& bytes, uint32_t offset) {
    constexpr uint32_t node_size = 0x34;
    put_be32(bytes, offset + 0x28, node_size);
    put_be32(bytes, offset + 0x2c, node_size);
    put_be32(bytes, offset + 0x30, node_size);
}

std::vector<uint8_t> make_single_leaf() {
    std::vector<uint8_t> bytes(8 + 0x34, 0);
    initialize_header(bytes);
    initialize_leaf(bytes, 8);
    return bytes;
}

std::vector<uint8_t> make_record_arc(const std::vector<uint8_t>& col) {
    std::vector<uint8_t> arc(0x50 + col.size(), 0);
    put_be32(arc, 0, 0x55AA382Du);
    put_be32(arc, 4, 0x20);
    put_be32(arc, 8, 0x30);
    put_be32(arc, 12, 0x50);
    put_be32(arc, 0x20, 0x01000000u);
    put_be32(arc, 0x28, 2);
    put_be32(arc, 0x2c, 1);
    put_be32(arc, 0x30, 0x50);
    put_be32(arc, 0x34, static_cast<uint32_t>(col.size()));
    const char name[] = "fixture.col";
    std::memcpy(arc.data() + 0x39, name, sizeof(name));
    std::copy(col.begin(), col.end(), arc.begin() + 0x50);
    return arc;
}

std::vector<uint8_t> make_movement_request_arc() {
    constexpr size_t data = 0xa0;
    constexpr size_t record_size = 0x20;
    std::vector<uint8_t> arc(data + 6 * record_size, 0);
    put_be32(arc, 0, 0x55AA382Du);
    put_be32(arc, 4, 0x20);
    put_be32(arc, 8, 0x50);
    put_be32(arc, 12, 0x80);
    put_be32(arc, 0x20, 0x01000000u);
    put_be32(arc, 0x28, 5);
    for (size_t index = 0; index < 4; ++index) {
        const size_t node = 0x2c + index * 12;
        put_be32(arc, node, static_cast<uint32_t>(1 + index * 2));
        put_be32(arc, node + 4,
                 static_cast<uint32_t>(index == 3 ? data : 0x80 + index));
        put_be32(arc, node + 8,
                 static_cast<uint32_t>(index == 3 ? 6 * record_size : 1));
        arc[0x5d + index * 2] = static_cast<uint8_t>('a' + index);
    }
    const std::array<uint32_t, 6> heads{{
        19u, // slot 0, value 0
        20u | (1u << 10) | (0xffu << 18), // slot 1, any value
        21u | (0xffu << 10), // either slot, value 0
        22u | (1u << 10) | (1u << 18), // wrong value
        23u, // saved flag set
        0xffffffffu, // 10-bit sentinel
    }};
    for (size_t index = 0; index < heads.size(); ++index) {
        const uint32_t head = heads[index];
        const size_t offset = data + index * record_size;
        arc[offset] = static_cast<uint8_t>(head);
        arc[offset + 1] = static_cast<uint8_t>(head >> 8);
        arc[offset + 2] = static_cast<uint8_t>(head >> 16);
        arc[offset + 3] = static_cast<uint8_t>(head >> 24);
    }
    return arc;
}

std::vector<uint8_t> make_single_record_arc() {
    std::vector<uint8_t> col = make_single_leaf();
    col[5] = 0;
    col[6] = 0;
    put_be_s16(col, 8 + 6, 6);
    put_be_s16(col, 8 + 8, 8);
    return make_record_arc(col);
}

void test_world_map_collision_archive() {
    const auto fixture = make_single_record_arc();
    awl::WorldMapCollisionArchive archive;
    awl::WorldMapCollisionRecordView view;
    expect(archive.parse(fixture) && archive.record_count() == 1 &&
               archive.lookup(0, &view) && view.size == 0x3c &&
               view.analysis != nullptr && view.analysis->header_byte_6 == 0 &&
               std::strcmp(view.name, "fixture.col") == 0 &&
               view.center_local == std::array<float, 3>{3.0f, 4.0f, 0.0f} &&
               view.radius_local == 5.0f,
           "ARC entry one maps to record zero with DOL-derived local bounds");
    expect(!archive.lookup(1, &view) && view.data == nullptr &&
               !archive.lookup(0, nullptr),
           "out-of-range and null record lookups fail without a sentinel");
    awl::WorldMapCollisionRecordPools unloaded;
    std::vector<awl::WorldMapMapseCollisionMatch> unavailable(1);
    expect(!unloaded.find_mapse_matches(2, &unavailable) &&
               unavailable.empty(),
           "unloaded mapse lookup fails and clears stale matches");

    auto invalid = fixture;
    put_be32(invalid, 0x30, static_cast<uint32_t>(invalid.size() - 2));
    expect(!archive.parse(invalid) && archive.record_count() == 0,
           "out-of-bounds ARC entry fails and clears prior records");
    invalid = fixture;
    put_be32(invalid, 0x28, UINT32_MAX);
    expect(!archive.parse(invalid), "overflowing ARC entry count is rejected");
    invalid = fixture;
    put_be32(invalid, 0x2c, 0x00FFFFFFu);
    expect(!archive.parse(invalid), "out-of-bounds ARC name is rejected");
    invalid = fixture;
    invalid[0x39] = 0;
    expect(!archive.parse(invalid), "empty ARC file name is rejected");
    invalid = fixture;
    invalid.resize(0x50 + 8);
    expect(!archive.parse(invalid), "truncated embedded COL is rejected");
    invalid = fixture;
    invalid[0x50 + 6] = 2;
    expect(!archive.parse(invalid), "unsupported embedded COL mode is rejected");
    invalid.assign(0x60 + 0x3c, 0);
    put_be32(invalid, 0, 0x55AA382Du);
    put_be32(invalid, 4, 0x20);
    put_be32(invalid, 8, 0x40);
    put_be32(invalid, 12, 0x60);
    put_be32(invalid, 0x20, 0x01000000u);
    put_be32(invalid, 0x28, 3);
    for (const size_t node : {size_t{0x2c}, size_t{0x38}}) {
        put_be32(invalid, node, 1);
        put_be32(invalid, node + 4, 0x60);
        put_be32(invalid, node + 8, 0x3c);
    }
    std::memcpy(invalid.data() + 0x45, "fixture.col", 12);
    std::copy(fixture.begin() + 0x50, fixture.end(), invalid.begin() + 0x60);
    expect(!archive.parse(invalid), "overlapping ARC payloads are rejected");
    invalid = fixture;
    put_be32(invalid, 0, 0);
    expect(!archive.parse(invalid), "incorrect ARC signature is rejected");
}

void test_world_map_event_conditions() {
    const auto fixture = make_record_arc(std::vector<uint8_t>{1, 2, 3, 4});
    awl::WorldMapEventConditions conditions;
    awl::WorldMapEventConditionEntry entry;
    expect(conditions.parse(fixture, 2) && conditions.loaded() &&
               conditions.phase() == 2 && conditions.entry_count() == 1 &&
               conditions.entry(0, &entry) && entry.size == 4 &&
               entry.data[0] == 1 && entry.data[3] == 4 &&
               std::strcmp(entry.name, "fixture.col") == 0,
           "phase archive owns an opaque entry and exposes its bytes");
    expect(!conditions.entry(1, &entry) && entry.data == nullptr &&
               !conditions.entry(0, nullptr),
           "phase archive rejects invalid entry requests without stale data");
    auto invalid = fixture;
    put_be32(invalid, 0x30, static_cast<uint32_t>(invalid.size() - 2));
    expect(!conditions.parse(invalid, 2) && !conditions.loaded() &&
               conditions.entry_count() == 0,
           "out-of-bounds phase entry clears prior archive");
    invalid = fixture;
    put_be32(invalid, 0x28, UINT32_MAX);
    expect(!conditions.parse(invalid, 2),
           "overflowing phase entry table is rejected");
    invalid = fixture;
    invalid[0x39] = 0;
    expect(!conditions.parse(invalid, 2),
           "empty phase entry name is rejected");
    invalid.assign(0x60 + 8, 0);
    put_be32(invalid, 0, 0x55AA382Du);
    put_be32(invalid, 4, 0x20);
    put_be32(invalid, 8, 0x40);
    put_be32(invalid, 12, 0x60);
    put_be32(invalid, 0x20, 0x01000000u);
    put_be32(invalid, 0x28, 3);
    for (const size_t node : {size_t{0x2c}, size_t{0x38}}) {
        put_be32(invalid, node, 1);
        put_be32(invalid, node + 4, 0x60);
        put_be32(invalid, node + 8, 4);
    }
    std::memcpy(invalid.data() + 0x45, "entry", 6);
    expect(!conditions.parse(invalid, 2),
           "overlapping phase entry payloads are rejected");
    expect(!conditions.parse(fixture, 6),
           "unsupported phase cannot select a request archive");

    const auto requests = make_movement_request_arc();
    uint8_t saved_bytes[3]{};
    saved_bytes[2] = 0x80; // condition ID 23 is already set
    const awl::WorldMapPackedSavedValues saved{saved_bytes, 3, 1};
    std::vector<awl::WorldMapMovementRequestRecord> matches;
    expect(conditions.parse(requests, 0) &&
               conditions.select_movement_request_records(0, saved, &matches) &&
               matches.size() == 2 &&
               matches[0].saved_condition_id == 19 &&
               matches[0].record_index == 0 &&
               matches[1].saved_condition_id == 21 &&
               matches[1].record_index == 2 &&
               matches[0].size == 0x20,
           "type-3 request slot 0 keeps ordered unset and wildcard records");
    expect(conditions.select_movement_request_records(1, saved, &matches) &&
               matches.size() == 2 &&
               matches[0].saved_condition_id == 20 &&
               matches[1].saved_condition_id == 21,
           "type-3 request slot 1 accepts wildcard auxiliary value");
    expect(!conditions.select_movement_request_records(2, saved, &matches) &&
               matches.empty(), "unsupported trigger slot is rejected");
    const awl::WorldMapPackedSavedValues truncated{saved_bytes, 1, 1};
    expect(!conditions.select_movement_request_records(0, truncated, &matches) &&
               matches.empty(), "incomplete saved flags cannot select a request");
    invalid = requests;
    std::fill(invalid.begin() + 0xa0 + 5 * 0x20,
              invalid.begin() + 0xa0 + 5 * 0x20 + 4, uint8_t{0});
    expect(conditions.parse(invalid, 0) &&
               !conditions.select_movement_request_records(0, saved, &matches),
           "missing request sentinel is rejected");

    std::array<uint8_t, 0x20> wildcard_record{};
    wildcard_record.fill(0xff);
    // A synthetic ID on the DOL's default-success branch, slot 0, wildcard
    // request value, and a clear decoder flag. No game record is embedded.
    const uint32_t head = (0x1fu << 27) | (0xffu << 18) | 400u;
    for (size_t byte = 0; byte < 4; ++byte) {
        wildcard_record[byte] = static_cast<uint8_t>(head >> (byte * 8));
    }
    wildcard_record[31] = 0x7f; // bit 255 calls FUN_801281F0(0)
    awl::WorldMapMovementRequestRecord supported{
        wildcard_record.data(), wildcard_record.size(), 0, 400};
    awl::WorldMapMovementRecordDecision decision;
    using RequestStatus = awl::WorldMapMovementRecordStatus;
    expect(awl::evaluate_world_map_movement_record(
               supported, 0, &decision) ==
               RequestStatus::EligibleForStateRequest &&
               decision.action_index == 400 && decision.decoder_flag == 0,
           "wildcard record reaches the decoder's default-success branch");
    expect(awl::evaluate_world_map_movement_record(
               supported, 1, &decision) == RequestStatus::Rejected &&
               decision.action_index == 0,
           "nonzero supplied global byte rejects the record in DOL order");
    wildcard_record[31] |= 0x80u;
    expect(awl::evaluate_world_map_movement_record(
               supported, 1, &decision) ==
               RequestStatus::EligibleForStateRequest,
           "set predicate bit skips the supplied global-byte gate");
    wildcard_record[31] &= 0x7fu;
    wildcard_record[3] &= static_cast<uint8_t>(~0x08u);
    expect(awl::evaluate_world_map_movement_record(
               supported, 0, &decision) ==
               RequestStatus::RequiresUntranslatedPredicate,
           "active later predicate is reported as unsupported");
    wildcard_record[3] |= 0x08u;
    wildcard_record[8] &= static_cast<uint8_t>(~0x01u);
    expect(awl::evaluate_world_map_movement_record(
               supported, 0, &decision) ==
               RequestStatus::RequiresUntranslatedPredicate,
           "active item/state condition is reported as unsupported");
    expect(awl::evaluate_world_map_movement_record(
               supported, 0, nullptr) ==
               RequestStatus::InvalidInput,
           "missing decoder output is rejected");

    // Synthetic active stage and clock descriptors use the DOL's local
    // slot-1 shape without embedding archive bytes.
    wildcard_record[8] |= 0x01u;
    auto timed_record = wildcard_record;
    auto set_bits = [&timed_record](size_t first, uint32_t width,
                                    uint32_t value) {
        for (uint32_t bit = 0; bit < width; ++bit) {
            const size_t at = first + bit;
            const uint8_t mask = static_cast<uint8_t>(1u << (at % 8));
            if ((value & (1u << bit)) != 0) {
                timed_record[at / 8] |= mask;
            } else {
                timed_record[at / 8] &= static_cast<uint8_t>(~mask);
            }
        }
    };
    set_bits(0, 10, 0x178);
    set_bits(27, 4, 1);
    set_bits(31, 1, 1);
    set_bits(54, 5, 19);
    set_bits(59, 5, 5);
    awl::WorldMapMovementRequestRecord timed{
        timed_record.data(), timed_record.size(), 1, 0x178};
    awl::WorldMapMovementEvaluationState time;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::RequiresUntranslatedPredicate,
           "active time predicates require a supplied state snapshot");
    time.has_time_state = true;
    time.clock_ticks = 19u * 36000u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::EligibleForStateRequest &&
               decision.action_index == 0x178,
           "stage zero and the inclusive 19:00 boundary qualify");
    time.clock_ticks = 18u * 36000u + 59u * 600u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::Rejected,
           "the minute before the time window is rejected");
    time.clock_ticks = 24u * 36000u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::Rejected,
           "the midnight end boundary is exclusive");
    time.clock_ticks = 20u * 36000u;
    time.current_stage = 4;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::Rejected,
           "stage four is outside the first stage mask");
    time.current_stage = 0;
    time.transition_stage = 3;
    time.transition_fraction = 0.5f;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::Rejected,
           "an active transition across stage groups rejects first");
    time.transition_fraction = 0.0f;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::EligibleForStateRequest,
           "zero transition fraction uses only the current stage group");
    set_bits(54, 5, 22);
    set_bits(59, 5, 4);
    time.clock_ticks = 1u * 36000u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::EligibleForStateRequest,
           "a wrapping clock window qualifies after midnight");
    time.clock_ticks = 2u * 36000u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::Rejected,
           "the wrapping clock window has an exclusive end");
    set_bits(54, 5, 7);
    set_bits(59, 5, 0);
    time.clock_ticks = 7u * 36000u + 599u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::EligibleForStateRequest,
           "zero-length window requires its exact hour and zero minute");
    time.clock_ticks = 7u * 36000u + 600u;
    expect(awl::evaluate_world_map_movement_record(timed, time, &decision) ==
               RequestStatus::Rejected,
           "zero-length window rejects the next minute");

    // First-match composition must stop at an unknown earlier predicate.
    auto ordered = requests;
    std::array<uint8_t, 0x20> later{};
    later.fill(0xff);
    const uint32_t later_head = (0x1fu << 27) | (0xffu << 18) | 401u;
    for (size_t byte = 0; byte < 4; ++byte) {
        later[byte] = static_cast<uint8_t>(later_head >> (byte * 8));
    }
    later[31] = 0x7f;
    auto earlier = later;
    const uint32_t earlier_head = (later_head & ~0x3ffu) | 400u;
    for (size_t byte = 0; byte < 4; ++byte) {
        earlier[byte] = static_cast<uint8_t>(earlier_head >> (byte * 8));
    }
    earlier[3] &= static_cast<uint8_t>(~0x08u); // unknown active predicate
    std::copy(earlier.begin(), earlier.end(), ordered.begin() + 0xa0);
    std::copy(later.begin(), later.end(), ordered.begin() + 0xc0);
    std::array<uint8_t, 128> ordered_saved_bytes{};
    const awl::WorldMapPackedSavedValues ordered_saved{
        ordered_saved_bytes.data(), ordered_saved_bytes.size(), 1};
    awl::WorldMapMovementEvaluationState ordered_state;
    awl::WorldMapMovementRequestPreparation prepared;
    using PreparationStatus = awl::WorldMapMovementRequestStatus;
    expect(conditions.parse(ordered, 0) &&
               conditions.prepare_movement_request(
                   0, ordered_saved, ordered_state, -1, 0, &prepared) ==
                   PreparationStatus::RequiresUntranslatedPredicate &&
               prepared.action_index == 0 &&
               conditions.prepare_movement_request(
                   0, ordered_saved, ordered_state, 3, 0, &prepared) ==
                   PreparationStatus::RequiresUntranslatedPredicate,
           "unknown earlier predicate stops ordered request preparation");
    ordered_saved_bytes[400 / 8] |= static_cast<uint8_t>(1u << (400 % 8));
    expect(conditions.prepare_movement_request(
               0, ordered_saved, ordered_state, -1, 0, &prepared) ==
               PreparationStatus::ReadyForActionPath &&
               prepared.record_index == 1 && prepared.action_index == 401u &&
               prepared.decoder_flag == 0 &&
               prepared.use_global_action_list &&
               prepared.action_list_index == 101,
           "saved earlier record exposes the next eligible action route");
    expect(conditions.prepare_movement_request(
               0, ordered_saved, ordered_state, 3, 0, &prepared) ==
               PreparationStatus::BlockedByOwnerState &&
               prepared.action_index == 0 &&
               conditions.prepare_movement_request(
                   0, ordered_saved, ordered_state, -1, 1, &prepared) ==
                   PreparationStatus::BlockedByOwnerState,
           "either FUN_8010AAC4 owner gate blocks after decoding");
    ordered[0xa0 + 3] |= 0x08u;
    ordered_saved_bytes[400 / 8] &=
        static_cast<uint8_t>(~(1u << (400 % 8)));
    expect(conditions.parse(ordered, 0) &&
               conditions.prepare_movement_request(
                   0, ordered_saved, ordered_state, -1, 0, &prepared) ==
                   PreparationStatus::ReadyForActionPath &&
               prepared.record_index == 0 && prepared.action_index == 400u &&
               prepared.action_list_index == 100,
           "first eligible record wins over a later eligible record");
    expect(conditions.prepare_movement_request(
               2, ordered_saved, ordered_state, -1, 0, &prepared) ==
               PreparationStatus::InvalidInput &&
               conditions.prepare_movement_request(
                   0, ordered_saved, ordered_state, -1, 0, nullptr) ==
                   PreparationStatus::InvalidInput,
           "preparation rejects invalid slot and missing output");
}

void test_world_map_action_asset() {
    // Two literals then a six-byte overlapping distance-two match: ABABABAB.
    std::vector<uint8_t> clz(21, 0);
    std::memcpy(clz.data(), "CLZ\0", 4);
    put_be32(clz, 4, 8);
    put_be32(clz, 12, 8);
    clz[16] = 4;
    clz[17] = 'A';
    clz[18] = 'B';
    clz[19] = 0xfe;
    clz[20] = 0xf3;
    const std::vector<uint8_t> expected{'A','B','A','B','A','B','A','B'};
    std::vector<uint8_t> decoded{42};
    expect(awl::decode_world_map_action_clz(
               clz.data(), clz.size(), 8, &decoded) && decoded == expected,
           "CLZ low-bit flags and overlapping match produce known bytes");
    const auto preserved = decoded;
    expect(!awl::decode_world_map_action_clz(
               clz.data(), clz.size(), 7, &decoded) && decoded == preserved,
           "CLZ output budget failure preserves prior bytes");
    auto broken = clz;
    broken.pop_back();
    expect(!awl::decode_world_map_action_clz(
               broken.data(), broken.size(), 8, &decoded),
           "CLZ rejects truncated match tokens");
    broken = clz;
    broken[16] = 1; // match before any history
    expect(!awl::decode_world_map_action_clz(
               broken.data(), broken.size(), 8, &decoded),
           "CLZ rejects a reference before decoded history");
    broken = clz;
    broken[20] = 0xf4; // seven-byte match overshoots output by one
    expect(!awl::decode_world_map_action_clz(
               broken.data(), broken.size(), 8, &decoded),
           "CLZ rejects a match exceeding the block output count");
    broken = clz;
    broken.push_back(0);
    expect(!awl::decode_world_map_action_clz(
               broken.data(), broken.size(), 8, &decoded),
           "bounded single-block CLZ rejects trailing bytes");
    for (const size_t field : {size_t{3}, size_t{8}, size_t{12}}) {
        broken = clz;
        broken[field] = 1;
        expect(!awl::decode_world_map_action_clz(
                   broken.data(), broken.size(), 8, &decoded),
               "CLZ rejects unsupported version, stride, and block count");
    }
    expect(!awl::decode_world_map_action_clz(nullptr, 0, 8, &decoded) &&
               !awl::decode_world_map_action_clz(
                   clz.data(), clz.size(), 8, nullptr) && decoded == preserved,
           "CLZ rejects missing input/output without publishing partial bytes");
    std::vector<uint8_t> literals(27, 0);
    std::memcpy(literals.data(), "CLZ\0", 4);
    put_be32(literals, 4, 9);
    put_be32(literals, 12, 9);
    for (size_t i = 0; i < 8; ++i) {
        literals[17 + i] = static_cast<uint8_t>(i + 1);
    }
    literals[26] = 9;
    expect(awl::decode_world_map_action_clz(
               literals.data(), literals.size(), 9, &decoded) &&
               decoded == std::vector<uint8_t>({1,2,3,4,5,6,7,8,9}),
           "CLZ reloads flags after eight literal tokens");

    awl::WorldMapGlobalActionArchive archive;
    const auto arc = make_record_arc(clz);
    awl::WorldMapMovementRequestPreparation request;
    request.action_index = 301;
    request.use_global_action_list = true;
    request.action_list_index = 1;
    expect(archive.parse(arc) && archive.node_count() == 2 &&
               archive.decode_prepared_request(request, 8, &decoded) &&
               decoded == expected,
           "global action ARC keeps DOL node numbering and decodes its file");
    request.action_list_index = 0;
    expect(!archive.decode_prepared_request(request, 8, &decoded),
           "action archive rejects inconsistent routing");
    request.action_index = 300;
    expect(!archive.decode_prepared_request(request, 8, &decoded),
           "global action zero index cannot decode the ARC directory root");
    request.action_index = 301;
    request.action_list_index = 1;
    request.use_global_action_list = false;
    expect(!archive.decode_prepared_request(request, 8, &decoded),
           "global archive cannot satisfy a phase-list request");
    for (const size_t field : {size_t{0x28}, size_t{0x2c}, size_t{0x30}}) {
        auto invalid_arc = arc;
        put_be32(invalid_arc, field, UINT32_MAX);
        expect(!archive.parse(invalid_arc) && !archive.loaded(),
               "invalid ARC count, node type/name, or payload clears the owner");
    }
}

void test_world_map_action_script() {
    // Invented instructions and an odd-sized string chunk exercise the DOL's
    // big-endian lengths and unpadded cursor; no game script is embedded.
    std::vector<uint8_t> bytes(77, 0);
    std::memcpy(bytes.data(), "RIFF", 4);
    put_be32(bytes, 4, 77);
    std::memcpy(bytes.data() + 8, "SCR CODE", 8);
    put_be32(bytes, 16, 20);
    put_be32(bytes, 20, 2);
    bytes[24] = 0x12;
    bytes[25] = 1;
    bytes[26] = 0x34;
    bytes[27] = 0x56;
    put_be32(bytes, 28, 0x789abcde);
    bytes[32] = 0x21;
    put_be32(bytes, 36, 0xfedcba98);
    std::memcpy(bytes.data() + 40, "STR ", 4);
    put_be32(bytes, 44, 17);
    put_be32(bytes, 48, 2);
    put_be32(bytes, 52, 0);
    put_be32(bytes, 56, 2);
    std::memcpy(bytes.data() + 60, "a\0bc", 5);
    std::memcpy(bytes.data() + 65, "OPT ", 4);
    put_be32(bytes, 69, 4);
    put_be32(bytes, 73, 9);
    awl::WorldMapActionScript script;
    awl::WorldMapActionInstruction instruction;
    expect(script.parse(bytes) && script.loaded() &&
               script.instruction_count() == 2 && script.string_count() == 2 &&
               script.options() == 9 && script.instruction(0, &instruction) &&
               instruction.opcode == 0x12 && instruction.flags == 1 &&
               instruction.reserved == 0x3456 && instruction.operand == 0x789abcde,
           "SCR parses bounded big-endian code and unpadded string/options chunks");
    expect(script.instruction(1, &instruction) &&
               instruction.opcode == 0x21 && instruction.flags == 0 &&
               instruction.reserved == 0 && instruction.operand == 0xfedcba98 &&
               !script.instruction(2, &instruction) && instruction.operand == 0 &&
               !script.instruction(SIZE_MAX, &instruction) &&
               !script.instruction(0, nullptr),
           "SCR instruction view reads the last record and rejects invalid indices");
    awl::WorldMapActionScriptState state;
    state.state_4 = 42;
    state.instruction_index_0c = 13;
    state.stack_20.fill(UINT32_MAX);
    state.stack_depth_1b0 = 100;
    state.variables_1b8.fill(UINT32_MAX);
    state.operand_base_4d8 = 19;
    state.mode_flags_528 = 7;
    expect(script.initialize_state(0x80000001, &state) &&
               state.state_4 == 1 && state.instruction_index_0c == 0 &&
               state.instruction_count_10 == 2 && state.string_count_14 == 2 &&
               state.stack_depth_1b0 == 0 && state.options_1b4 == 9 &&
               state.operand_base_4d8 == 0 && state.mode_flags_528 == 0x80000001 &&
               std::all_of(state.stack_20.begin(), state.stack_20.end(),
                           [](uint32_t value) { return value == 0; }) &&
               std::all_of(state.variables_1b8.begin(), state.variables_1b8.end(),
                           [](uint32_t value) { return value == 0; }) &&
               !script.initialize_state(0, nullptr),
           "SCR initialization resets all represented DOL stack/variable fields");
    auto without_strings = bytes;
    without_strings.erase(without_strings.begin() + 40,
                          without_strings.begin() + 65);
    put_be32(without_strings, 4, 52);
    expect(script.parse(without_strings) && script.string_count() == 0 &&
               script.initialize_state(0, &state) && state.string_count_14 == 0,
           "SCR load without strings clears prior optional metadata");
    auto rejects = [&script](std::vector<uint8_t> invalid) {
        return !script.parse(std::move(invalid)) && !script.loaded() &&
               script.instruction_count() == 0 && script.string_count() == 0 &&
               script.options() == 0;
    };
    for (const size_t offset : {size_t{0}, size_t{8}, size_t{12}}) {
        auto invalid = bytes;
        invalid[offset] = '?';
        expect(rejects(invalid), "SCR rejects unsupported signatures or chunk tags");
    }
    for (const size_t field : {size_t{4}, size_t{16}, size_t{20},
                              size_t{44}, size_t{48}, size_t{69}}) {
        auto invalid = bytes;
        put_be32(invalid, field, UINT32_MAX);
        expect(rejects(invalid), "SCR rejects oversized bounds, counts, and chunks");
    }
    auto invalid = bytes;
    put_be32(invalid, 20, 0);
    expect(rejects(invalid), "SCR rejects an empty CODE instruction list");
    invalid = bytes;
    invalid.resize(73);
    put_be32(invalid, 4, 73);
    expect(rejects(invalid), "SCR rejects a truncated chunk payload");
    invalid = bytes;
    invalid.push_back(0);
    put_be32(invalid, 4, 78);
    expect(rejects(invalid), "SCR stops at the exact bound and rejects partial next headers");
    invalid.assign(bytes.begin(), bytes.begin() + 40);
    put_be32(invalid, 4, 40);
    expect(rejects(invalid), "SCR requires explicit options instead of stale owner data");
    invalid.assign(bytes.begin(), bytes.begin() + 12);
    invalid.insert(invalid.end(), bytes.begin() + 65, bytes.end());
    put_be32(invalid, 4, 24);
    expect(rejects(invalid), "SCR requires code before initialization");
    for (const auto range : {std::pair<size_t, size_t>{12, 40},
                             {40, 65}, {65, 77}}) {
        invalid = bytes;
        invalid.insert(invalid.end(), bytes.begin() + range.first,
                       bytes.begin() + range.second);
        put_be32(invalid, 4, static_cast<uint32_t>(invalid.size()));
        expect(rejects(invalid), "SCR rejects duplicate known chunks");
    }
    state.state_4 = 99;
    state.stack_20[99] = 31;
    state.variables_1b8[199] = 37;
    expect(!script.initialize_state(3, &state) && state.state_4 == 99 &&
               state.stack_20[99] == 31 && state.variables_1b8[199] == 37 &&
               !script.instruction(0, &instruction),
           "failed SCR load cannot initialize state or expose stale code");
}

std::vector<uint8_t> make_action_script(
    const std::vector<awl::WorldMapActionInstruction>& instructions,
    uint32_t options = 7) {
    const size_t code_length = 4 + instructions.size() * 8;
    const size_t opt_offset = 20 + code_length;
    std::vector<uint8_t> bytes(opt_offset + 12, 0);
    std::memcpy(bytes.data(), "RIFF", 4);
    put_be32(bytes, 4, static_cast<uint32_t>(bytes.size()));
    std::memcpy(bytes.data() + 8, "SCR CODE", 8);
    put_be32(bytes, 16, static_cast<uint32_t>(code_length));
    put_be32(bytes, 20, static_cast<uint32_t>(instructions.size()));
    for (size_t i = 0; i < instructions.size(); ++i) {
        const size_t offset = 24 + i * 8;
        bytes[offset] = instructions[i].opcode;
        bytes[offset + 1] = instructions[i].flags;
        put_be32(bytes, offset + 4, instructions[i].operand);
    }
    std::memcpy(bytes.data() + opt_offset, "OPT ", 4);
    put_be32(bytes, opt_offset + 4, 4);
    put_be32(bytes, opt_offset + 8, options);
    return bytes;
}

bool same_action_state(const awl::WorldMapActionScriptState& a,
                       const awl::WorldMapActionScriptState& b) {
    return a.state_4 == b.state_4 &&
           a.instruction_index_0c == b.instruction_index_0c &&
           a.instruction_count_10 == b.instruction_count_10 &&
           a.string_count_14 == b.string_count_14 && a.stack_20 == b.stack_20 &&
           a.stack_depth_1b0 == b.stack_depth_1b0 && a.options_1b4 == b.options_1b4 &&
           a.variables_1b8 == b.variables_1b8 &&
           a.operand_base_4d8 == b.operand_base_4d8 &&
           a.mode_flags_528 == b.mode_flags_528;
}

void test_world_map_action_execution() {
    using Status = awl::WorldMapActionStepStatus;
    using Instruction = awl::WorldMapActionInstruction;
    auto ins = [](uint8_t opcode, uint32_t operand = 0, uint8_t flags = 0) {
        return Instruction{opcode, flags, 0, operand};
    };
    awl::WorldMapActionScript script;
    awl::WorldMapActionScriptState state;
    awl::WorldMapActionStep result;
    // An invented program assigns 10, adds 7, subtracts 2, and calls a
    // subroutine that adds 1. The expected variable values are 17 and 16.
    const std::vector<Instruction> program{
        ins(0x17, 3), ins(0x17, 10), ins(1), ins(0x16),
        ins(0x17, 3), ins(0x17, 7), ins(2), ins(0x16),
        ins(0x13, 3), ins(0x17, 2), ins(8), ins(0x14, 4),
        ins(0x1f, 15), ins(0x24), ins(0), ins(0x13, 4),
        ins(0x17, 1), ins(7), ins(0x14, 4), ins(0x20)};
    expect(script.parse(make_action_script(program)) && script.initialize_state(9, &state),
           "supplied-state action program initializes");
    bool valid = true;
    Status status = Status::Advanced;
    size_t steps = 0;
    for (; steps < 32 && status == Status::Advanced; ++steps) {
        status = script.step(&state, &result);
        if (result.instruction_index == 12) {
            valid = valid && status == Status::Advanced &&
                    state.instruction_index_0c == 15 && state.stack_depth_1b0 == 1 &&
                    state.stack_20[1] == 13;
        }
    }
    expect(valid && status == Status::Halted && steps == 19 && state.state_4 == 0 &&
               state.instruction_index_0c == 14 && state.variables_1b8[3] == 17 &&
               state.variables_1b8[4] == 16 && state.stack_depth_1b0 == 0 &&
               state.mode_flags_528 == 9 && script.step(&state, &result) == Status::NotRunning,
           "action program preserves call/return order and halts with expected variables");
    auto prepare = [&](uint8_t opcode, uint32_t operand = 0, uint8_t flags = 0,
                       uint32_t options = 7) {
        return script.parse(make_action_script({ins(opcode, operand, flags), ins(0x24)}, options)) &&
               script.initialize_state(0, &state);
    };
    struct BinaryCase { uint8_t opcode; uint32_t left; uint32_t right; uint32_t expected; };
    for (const auto& test : std::vector<BinaryCase>{
             {7, 0xfffffffe, 4, 2}, {8, 0, 1, UINT32_MAX},
             {0x0c, 2, 3, 1}, {0x0c, 0, 3, 0},
             {0x0d, 0, 7, 1}, {0x0d, 0, 0, 0}}) {
        expect(prepare(test.opcode), "binary action test initializes");
        state.stack_depth_1b0 = 2;
        state.stack_20[1] = test.left;
        state.stack_20[2] = test.right;
        expect(script.step(&state, &result) == Status::Advanced &&
                   state.stack_depth_1b0 == 1 && state.stack_20[1] == test.expected,
               "integer action arithmetic wraps and logical results normalize");
    }
    for (const auto& test : std::vector<BinaryCase>{
             {0x10, 0x80000000, 7, 0x80000000}, {0x11, 0, 7, 1},
             {0x11, UINT32_MAX, 7, 0}, {0x0e, 1, 7, 129},
             {0x0f, 0, 7, 0xffffff80}, {0x0e, 1, 32, 1},
             {0x0e, 1, 64, 2}}) {
        expect(prepare(test.opcode, 0, 0, test.right), "unary action test initializes");
        state.stack_20[0] = test.left;
        expect(script.step(&state, &result) == Status::Advanced &&
                   state.stack_20[0] == test.expected && state.stack_depth_1b0 == 0,
               "unary actions preserve sentinel and PowerPC shift/wrap behavior");
    }
    expect(prepare(3), "indexed subtract initializes");
    state.stack_depth_1b0 = 2;
    state.stack_20[1] = 199;
    state.stack_20[2] = 3;
    state.variables_1b8[199] = 1;
    expect(script.step(&state, &result) == Status::Advanced &&
               state.variables_1b8[199] == 0xfffffffe && state.stack_20[1] == 0xfffffffe,
           "indexed variable subtraction stores and leaves its wrapped result");
    expect(prepare(0x15), "duplicate action initializes");
    state.stack_20[0] = 11;
    expect(script.step(&state, &result) == Status::Advanced && state.stack_depth_1b0 == 1 &&
               state.stack_20[0] == 11 && state.stack_20[1] == 11,
           "duplicate pushes a copy of the current sentinel/top");
    expect(prepare(0x16) && script.step(&state, &result) == Status::Advanced &&
               state.stack_depth_1b0 == 0, "drop saturates at depth zero");
    for (const uint8_t opcode : {uint8_t{0x19}, uint8_t{0x1a}, uint8_t{0x1b},
                                uint8_t{0x1c}, uint8_t{0x1d}, uint8_t{0x1e}}) {
        for (const int value : {-1, 0, 1}) {
            const bool taken = (opcode == 0x19 && value < 0) ||
                (opcode == 0x1a && value <= 0) || (opcode == 0x1b && value == 0) ||
                (opcode == 0x1c && value != 0) || (opcode == 0x1d && value >= 0) ||
                (opcode == 0x1e && value > 0);
            expect(prepare(opcode), "signed branch initializes");
            state.stack_depth_1b0 = 1;
            state.stack_20[1] = static_cast<uint32_t>(value);
            expect(script.step(&state, &result) == Status::Advanced &&
                       state.instruction_index_0c == (taken ? 0u : 1u) &&
                       state.stack_depth_1b0 == 0,
                   "six signed conditional branches consume their condition");
        }
    }
    expect(prepare(0x1b, UINT32_MAX), "untaken out-of-range branch initializes");
    state.stack_20[0] = 1;
    expect(script.step(&state, &result) == Status::Advanced && state.instruction_index_0c == 1,
           "untaken conditional branch does not validate an unused destination");
    for (const uint8_t opcode : {uint8_t{0x21}, uint8_t{0x22}, uint8_t{0x23}}) {
        expect(prepare(opcode, 3), "operand base action initializes");
        state.operand_base_4d8 = 8;
        const uint32_t expected = opcode == 0x21 ? 3u : (opcode == 0x22 ? 11u : 5u);
        expect(script.step(&state, &result) == Status::Advanced &&
                   state.operand_base_4d8 == expected, "operand base set/add/subtract follow order");
    }
    expect(prepare(0x13, 2, 3), "base-relative read initializes");
    state.operand_base_4d8 = 4;
    state.variables_1b8[6] = 33;
    expect(script.step(&state, &result) == Status::Advanced && result.effective_operand == 6 &&
               state.stack_20[1] == 33, "only flag bit zero adds the operand base");
    expect(prepare(0x17, UINT32_MAX, 1), "relative operand wrap initializes");
    state.operand_base_4d8 = 2;
    expect(script.step(&state, &result) == Status::Advanced && state.stack_20[1] == 1,
           "effective operand addition wraps at 32 bits");
    for (const uint8_t opcode : {uint8_t{4}, uint8_t{5}, uint8_t{6}, uint8_t{9},
                                uint8_t{10}, uint8_t{11}, uint8_t{0x12}, uint8_t{0xff},
                                uint8_t{0x25}}) {
        expect(prepare(opcode, 42, 1), "untranslated action initializes");
        state.operand_base_4d8 = 3;
        const auto before = state;
        expect(script.step(&state, &result) == (opcode == 0x25
                   ? Status::RequiresCallback : Status::UnsupportedOpcode) &&
                   same_action_state(state, before) && result.instruction_index == 0 &&
                   result.opcode == opcode && result.effective_operand == 45,
               "unsupported instructions and callback boundary preserve supplied state");
    }
    for (const uint8_t opcode : {uint8_t{0x13}, uint8_t{0x14}, uint8_t{0x18},
                                uint8_t{0x1f}, uint8_t{0x21}, uint8_t{0x22},
                                uint8_t{0x23}, uint8_t{0x1b}}) {
        expect(prepare(opcode, 200), "invalid operand action initializes");
        const auto before = state;
        expect(script.step(&state, &result) == Status::InvalidOperand &&
                   same_action_state(state, before), "invalid operands reject atomically");
    }
    for (const uint8_t opcode : {uint8_t{0x17}, uint8_t{0x15}, uint8_t{0x1f}}) {
        expect(prepare(opcode), "stack capacity action initializes");
        state.stack_depth_1b0 = 99;
        const auto before = state;
        expect(script.step(&state, &result) == Status::InvalidOperand &&
                   same_action_state(state, before), "stack push rejects the DOL's overlapping slot 100");
    }
    expect(prepare(1), "invalid indexed store initializes");
    state.stack_depth_1b0 = 1;
    state.stack_20[0] = 200;
    state.stack_20[1] = 3;
    const auto before = state;
    expect(script.step(&state, &result) == Status::InvalidOperand &&
               same_action_state(state, before), "invalid indexed store rolls back its pop");
    expect(prepare(0x20), "invalid return initializes");
    state.stack_20[0] = 2;
    const auto before_return = state;
    expect(script.step(&state, &result) == Status::InvalidOperand &&
               same_action_state(state, before_return), "return cannot leave the code extent");
    expect(prepare(0), "invalid state test initializes");
    for (int field = 0; field < 6; ++field) {
        auto invalid = state;
        if (field == 0) invalid.state_4 = 2;
        if (field == 1) invalid.instruction_index_0c = 2;
        if (field == 2) invalid.stack_depth_1b0 = 100;
        if (field == 3) invalid.instruction_count_10 = 3;
        if (field == 4) invalid.string_count_14 = 1;
        if (field == 5) invalid.options_1b4 = 8;
        const auto original = invalid;
        expect(script.step(&invalid, &result) == Status::InvalidState &&
                   same_action_state(invalid, original), "inconsistent action state rejects atomically");
    }
    expect(script.step(nullptr, &result) == Status::InvalidState &&
               script.step(&state, nullptr) == Status::InvalidState,
           "action step requires supplied state and output");
    script.clear();
    expect(script.step(&state, &result) == Status::InvalidState,
           "unloaded script cannot execute stale state");
}

void test_world_map_action_callbacks() {
    using Status = awl::WorldMapActionStepStatus;
    using Instruction = awl::WorldMapActionInstruction;
    awl::WorldMapActionScript script;
    awl::WorldMapActionScriptState state;
    awl::WorldMapActionStep result;
    auto prepare = [&](uint32_t command, uint32_t options = 7, uint8_t flags = 0) {
        return script.parse(make_action_script(
                   {Instruction{0x25, flags, 0, command}, Instruction{0x24, 0, 0, 0}}, options)) &&
               script.initialize_state(0x2011, &state);
    };
    expect(prepare(0), "world-map yield command initializes");
    state.stack_depth_1b0 = 2;
    state.stack_20[1] = 111;
    state.stack_20[2] = 222;
    state.variables_1b8[199] = 333;
    auto expected = state;
    ++expected.instruction_index_0c;
    expect(script.step_world_map(&state, &result) == Status::Yielded &&
               same_action_state(state, expected) && result.effective_operand == 0 &&
               std::all_of(result.callback_arguments.begin(), result.callback_arguments.end(),
                           [](uint32_t value) { return value == 0; }),
           "command zero yields after advancing PC without clearing active state or popping");
    expect(script.step_world_map(&state, &result) == Status::Halted &&
               state.state_4 == 0 && state.instruction_index_0c == 2,
           "a later supplied pass resumes after yield and can halt");
    const auto stopped = state;
    expect(script.step_world_map(&state, &result) == Status::NotRunning &&
               same_action_state(state, stopped), "halted world-map script stays stopped");
    struct ArgumentCase { uint32_t options; uint32_t word; uint32_t decoded; };
    for (const auto& test : std::vector<ArgumentCase>{
             {7, 384, 3}, {7, 0xffffff7f, 0xfffffffe}, {7, UINT32_MAX, UINT32_MAX},
             {7, 127, 0}, {0, 123, 123}, {32, 123, 0},
             {32, 0xffffff7f, UINT32_MAX}, {64, 123, 123}}) {
        expect(prepare(66, test.options), "integer callback argument initializes");
        state.stack_depth_1b0 = 1;
        state.stack_20[1] = test.word;
        state.stack_20[0] = 999;
        auto after = state;
        ++after.instruction_index_0c;
        after.stack_depth_1b0 = 0;
        expect(script.step_world_map(&state, &result) == Status::Advanced &&
                   same_action_state(state, after) && result.callback_arguments[0] == test.decoded &&
                   std::all_of(result.callback_arguments.begin() + 1,
                               result.callback_arguments.end(),
                               [](uint32_t value) { return value == 0; }),
               "command 66 decodes signed fixed-point integer and pops exactly once");
    }
    expect(prepare(66), "integer callback at sentinel initializes");
    state.stack_20[0] = 256;
    auto after_sentinel = state;
    ++after_sentinel.instruction_index_0c;
    expect(script.step_world_map(&state, &result) == Status::Advanced &&
               same_action_state(state, after_sentinel) && result.callback_arguments[0] == 2,
           "integer callback reads the empty-stack sentinel with saturating pop");
    expect(prepare(64, 7, 1), "base-relative command initializes");
    state.operand_base_4d8 = 2;
    state.stack_20[0] = 128;
    expect(script.step_world_map(&state, &result) == Status::Advanced &&
               result.effective_operand == 66 && result.callback_arguments[0] == 1,
           "world-map command selection uses the common effective operand");
    for (const uint32_t command : {1u, 4u, 65u, 67u, 221u, 222u, UINT32_MAX}) {
        expect(prepare(command), "untranslated world-map command initializes");
        state.stack_depth_1b0 = 1;
        state.stack_20[1] = 128;
        const auto original = state;
        expect(script.step_world_map(&state, &result) == Status::RequiresCallback &&
                   same_action_state(state, original) && result.effective_operand == command &&
                   script.step_world_map(&state, &result) == Status::RequiresCallback &&
                   same_action_state(state, original),
               "untranslated commands remain unconsumed despite the original default branch");
    }
    // Invented progression: one ordinary push, one consumed command, a yield,
    // and then an untranslated command in the following supplied pass.
    const std::vector<Instruction> program{
        {0x17, 0, 0, 384}, {0x25, 0, 0, 66}, {0x25, 0, 0, 0},
        {0x17, 0, 0, 128}, {0x25, 0, 0, 65}};
    expect(script.parse(make_action_script(program)) && script.initialize_state(0, &state),
           "mixed world-map script initializes");
    size_t consumed = 0;
    Status status = Status::Advanced;
    while (consumed < 16 && status == Status::Advanced) {
        status = script.step_world_map(&state, &result);
        ++consumed;
    }
    expect(consumed == 3 && status == Status::Yielded && state.state_4 == 1 &&
               state.instruction_index_0c == 3 && state.stack_depth_1b0 == 0,
           "supplied pass stops on yield before processing later instructions");
    expect(script.step_world_map(&state, &result) == Status::Advanced &&
               script.step_world_map(&state, &result) == Status::RequiresCallback &&
               state.instruction_index_0c == 4 && state.stack_depth_1b0 == 1,
           "following supplied pass advances to a stable untranslated callback boundary");
    const auto original = state;
    expect(script.step_world_map(&state, nullptr) == Status::InvalidState &&
               same_action_state(state, original) &&
               script.step_world_map(nullptr, &result) == Status::InvalidState,
           "world-map step preserves state on missing output/state");
    script.clear();
    expect(script.step_world_map(&state, &result) == Status::InvalidState &&
               same_action_state(state, original), "unloaded world-map script cannot resume");
}

void test_world_map_command_preparation() {
    using Status = awl::WorldMapCommandPreparationStatus;
    using Effect = awl::WorldMapCommandEffect;
    using Instruction = awl::WorldMapActionInstruction;
    awl::WorldMapActionScript script;
    awl::WorldMapActionScriptState state;
    awl::WorldMapCommand4Snapshot manager;
    awl::WorldMapCommandPreparation prepared;
    auto near = [](float actual, float expected) {
        return std::fabs(actual - expected) < 0.00001f;
    };
    auto initialize = [&](uint32_t command, uint32_t options = 7) {
        return script.parse(make_action_script({Instruction{0x25, 0, 0, command}}, options)) &&
               script.initialize_state(0x2011, &state);
    };
    expect(initialize(4), "request command preparation initializes");
    state.stack_depth_1b0 = 3;
    state.stack_20[1] = 1280;
    state.stack_20[2] = 2560;
    state.stack_20[3] = 0xffffff7f; // -129 rounds down to -2 under sraw 7
    const auto original = state;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               same_action_state(state, original) && prepared.effect == Effect::BeginRequest4 &&
               prepared.instruction.callback_arguments[0] == 10 &&
               prepared.instruction.callback_arguments[1] == 20 &&
               prepared.instruction.callback_arguments[2] == 0xfffffffe &&
               prepared.after_arguments.stack_depth_1b0 == 0 &&
               prepared.after_arguments.instruction_index_0c == 1 &&
               prepared.next_key_530 == 10 && prepared.next_key_534 == 20 &&
               !prepared.has_stack_result && manager.key_530 == UINT32_MAX,
           "request preparation decodes reverse slots and plans new keys without acceptance");
    manager.key_530 = 10;
    manager.key_534 = 20;
    expect(script.prepare_world_map_command(state, &manager, &prepared) ==
               Status::RequiresManagerSnapshot && prepared.after_arguments.state_4 == 0,
           "same request keys require supplied manager state and clear a stale plan");
    manager.has_manager_state = true;
    manager.manager_state_48 = 3;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.effect == Effect::PollRequest4 && prepared.has_stack_result &&
               prepared.stack_result == 0 && prepared.next_key_530 == 10 &&
               same_action_state(state, original), "busy request plans zero without clearing keys");
    manager.manager_state_48 = 0;
    manager.manager_result_1c0 = 2;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.effect == Effect::CompleteRequest4 && prepared.stack_result == 3 &&
               prepared.next_key_530 == UINT32_MAX && prepared.next_key_534 == UINT32_MAX,
           "completed request plans result plus one and resets both keys");
    manager.manager_result_1c0 = UINT32_MAX;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.stack_result == UINT32_MAX, "request completion preserves the minus-one sentinel");
    manager.manager_result_1c0 = 0x7fffffff;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.stack_result == 0x80000000, "request result increment preserves word wrap");
    state.stack_20[3] = 512;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.effect == Effect::CompleteRequest4,
           "third argument changes do not restart a request with the same two keys");
    manager.key_534 = 21;
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.effect == Effect::BeginRequest4, "either changed key selects a new request");
    expect(initialize(65), "timed level preparation initializes");
    state.stack_depth_1b0 = 3;
    state.stack_20[1] = 128; // target 1
    state.stack_20[2] = 64 * 128;
    state.stack_20[3] = 192; // 1.5 seconds
    const auto level_original = state;
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::Prepared &&
               same_action_state(state, level_original) && prepared.effect == Effect::LevelTransition65 &&
               near(prepared.normalized_level, 64.0f / 127.0f) &&
               near(prepared.transition_seconds, 1.5f) && prepared.after_arguments.stack_depth_1b0 == 0,
           "level preparation decodes integer/integer/float without performing backend work");
    state.stack_20[2] = 200 * 128;
    state.stack_20[3] = 100 * 128;
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::Prepared &&
               near(prepared.normalized_level, 1.0f) && near(prepared.transition_seconds, 65.535f),
           "level and duration preserve the verified upper clamps");
    state.stack_20[2] = static_cast<uint32_t>(-128);
    state.stack_20[3] = static_cast<uint32_t>(-128);
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::Prepared &&
               near(prepared.normalized_level, -1.0f / 127.0f) && prepared.transition_seconds == 0,
           "negative level is retained while negative duration clamps to zero");
    state.stack_20[1] = 258 * 128;
    state.stack_20[3] = 191; // 1492.1875 ms, truncated by fctiwz
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::Prepared &&
               prepared.effect == Effect::IndexedLevel65 && prepared.indexed_target == 2 &&
               prepared.indexed_level == 255 && prepared.transition_milliseconds == 1492,
           "indexed path narrows target/level bytes and truncates bounded milliseconds");
    state.stack_20[1] = static_cast<uint32_t>(-128);
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::Prepared &&
               prepared.effect == Effect::IndexedLevel65 && prepared.indexed_target == 255,
           "negative target chooses the indexed path");
    expect(initialize(65, 32) &&
               script.prepare_world_map_command(state, nullptr, &prepared) == Status::InvalidOperand &&
               state.instruction_index_0c == 0, "zero float scale fails without consuming arguments");
    expect(initialize(4), "sentinel request preparation initializes");
    state.stack_20[0] = 256;
    manager = {};
    expect(script.prepare_world_map_command(state, &manager, &prepared) == Status::Prepared &&
               prepared.instruction.callback_arguments[0] == 2 &&
               prepared.instruction.callback_arguments[1] == 2 &&
               prepared.instruction.callback_arguments[2] == 2 &&
               prepared.after_arguments.stack_depth_1b0 == 0,
           "typed arguments saturate at the empty-stack sentinel in DOL order");
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::InvalidState &&
               script.prepare_world_map_command(state, &manager, nullptr) == Status::InvalidState,
           "request preparation requires its supplied key snapshot and output");
    expect(initialize(67) && script.prepare_world_map_command(state, nullptr, &prepared) ==
               Status::UnsupportedCommand, "preparation rejects unknown command profiles");
    state.stack_depth_1b0 = 100;
    expect(script.prepare_world_map_command(state, nullptr, &prepared) == Status::InvalidState,
           "preparation retains common stack bounds validation");
}

void test_world_map_request_consumption() {
    using Status = awl::WorldMapActionStepStatus;
    using Instruction = awl::WorldMapActionInstruction;
    awl::WorldMapActionScript script;
    awl::WorldMapActionScriptState state;
    awl::WorldMapCommand4Snapshot manager;
    awl::WorldMapActionStep result;
    // Invented repeatable program: three fixed-point arguments, command 4,
    // save its result, and retry. Expectations come from the traced branches.
    expect(script.parse(make_action_script({
               Instruction{0x17, 0, 0, 640}, Instruction{0x17, 0, 0, 1024},
               Instruction{0x17, 0, 0, 0xffffff80}, Instruction{0x25, 0, 0, 4},
               Instruction{0x14, 0, 0, 2}, Instruction{0x18, 0, 0, 0}})) &&
               script.initialize_state(0x2011, &state), "request retry program initializes");
    auto arguments = [&]() {
        for (unsigned i = 0; i < 3; ++i) {
            expect(script.step_world_map(&state, &result) == Status::Advanced,
                   "request retry program supplies its next arguments");
        }
    };
    auto store_and_retry = [&](uint32_t expected) {
        expect(script.step_world_map(&state, &result) == Status::Advanced &&
                   state.variables_1b8[2] == expected && state.stack_depth_1b0 == 0 &&
                   script.step_world_map(&state, &result) == Status::Advanced &&
                   state.instruction_index_0c == 0, "request result saves before retry");
    };
    arguments();
    const auto unresolved = state;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::RequiresCallback &&
               same_action_state(state, unresolved) && manager.key_530 == UINT32_MAX &&
               manager.key_534 == UINT32_MAX, "missing manager snapshot cannot consume new request");
    manager.has_manager_state = true;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::RequiresCallback &&
               same_action_state(state, unresolved) && manager.key_530 == UINT32_MAX,
           "idle request remains blocked on untranslated presentation resources");
    manager.manager_state_48 = 3;
    manager.manager_result_1c0 = 23;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::Advanced &&
               state.instruction_index_0c == 4 && state.stack_depth_1b0 == 1 &&
               state.stack_20[1] == 0xffffff80 && manager.key_530 == 5 &&
               manager.key_534 == 8 && manager.manager_state_48 == 3 &&
               manager.manager_result_1c0 == 23 && result.callback_arguments[2] == UINT32_MAX,
           "busy start rejects with scaled minus one but retains new keys and manager fields");
    store_and_retry(0xffffff80);
    arguments();
    manager.has_manager_state = false;
    const auto missing_poll = state;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::RequiresCallback &&
               same_action_state(state, missing_poll) && manager.key_530 == 5 && manager.key_534 == 8,
           "missing same-key manager snapshot preserves script and keys");
    manager.has_manager_state = true;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::Advanced &&
               state.stack_20[1] == 0 && state.stack_depth_1b0 == 1 && manager.key_530 == 5 &&
               manager.key_534 == 8 && manager.manager_result_1c0 == 23,
           "same-key pending request consumes arguments and pushes zero without finishing manager");
    store_and_retry(0);
    arguments();
    manager.manager_state_48 = 0;
    manager.manager_result_1c0 = 2;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::Advanced &&
               state.stack_20[1] == 384 && state.stack_depth_1b0 == 1 &&
               manager.key_530 == UINT32_MAX && manager.key_534 == UINT32_MAX &&
               manager.manager_result_1c0 == 2,
           "ready request pushes scaled incremented result and clears only interpreter keys");
    store_and_retry(384);
    arguments();
    const auto retry = state;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::RequiresCallback &&
               same_action_state(state, retry) && manager.key_530 == UINT32_MAX &&
               manager.key_534 == UINT32_MAX, "completed request retry needs actual begin backend again");

    // Sentinel argument reads and PPC shifts are checked independently of the
    // retry program. Result -1 is preserved; other results increment with wrap.
    for (uint32_t options : {0u, 7u, 31u, 32u, 63u, 64u}) {
        expect(script.parse(make_action_script({Instruction{0x25, 0, 0, 4}}, options)) &&
                   script.initialize_state(0, &state), "request scale boundary initializes");
        manager = {};
        manager.key_530 = 0;
        manager.key_534 = 0;
        manager.has_manager_state = true;
        manager.manager_result_1c0 = UINT32_MAX;
        const uint32_t minus_one = options == 7 ? 0xffffff80u :
            options == 31 ? 0x80000000u : options == 32 || options == 63 ? 0u : UINT32_MAX;
        expect(script.step_world_map_request(&state, &manager, &result) == Status::Advanced &&
                   state.stack_depth_1b0 == 1 && state.stack_20[1] == minus_one &&
                   manager.key_530 == UINT32_MAX && manager.key_534 == UINT32_MAX,
               "empty-stack completion preserves sentinel and verified low-six-bit result scale");
    }
    expect(script.parse(make_action_script({Instruction{0x25, 1, 0, 3}}, 0)) &&
               script.initialize_state(0, &state), "effective request command initializes");
    state.operand_base_4d8 = 1;
    state.stack_depth_1b0 = 4;
    state.stack_20[1] = 0xfeedbeef;
    state.stack_20[2] = 10;
    state.stack_20[3] = 20;
    state.stack_20[4] = 99;
    state.variables_1b8[199] = 0x12345678;
    manager = {};
    manager.key_530 = 10;
    manager.key_534 = 20;
    manager.has_manager_state = true;
    manager.manager_result_1c0 = 0xfffffffe;
    auto expected = state;
    expected.instruction_index_0c = 1;
    expected.stack_depth_1b0 = 2;
    expected.stack_20[2] = UINT32_MAX;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::Advanced &&
               same_action_state(state, expected) && result.effective_operand == 4,
           "completion respects effective command, lower stack, full state, and result wrap");
    expect(script.initialize_state(0, &state), "invalid request state initializes");
    state.stack_depth_1b0 = 100;
    const auto invalid = state;
    manager.key_530 = 17;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::InvalidState &&
               same_action_state(state, invalid) && manager.key_530 == 17 &&
               script.step_world_map_request(nullptr, &manager, &result) == Status::InvalidState &&
               script.step_world_map_request(&state, nullptr, &result) == Status::InvalidState &&
               script.step_world_map_request(&state, &manager, nullptr) == Status::InvalidState,
           "invalid pointers and stack bounds preserve all supplied request state");
    expect(script.parse(make_action_script({Instruction{0x25, 0, 0, 65}})) &&
               script.initialize_state(0, &state), "unsupported request command initializes");
    const auto unsupported = state;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::RequiresCallback &&
               same_action_state(state, unsupported) && manager.key_530 == 17,
           "request entry point leaves other callbacks unconsumed");
    expect(script.parse(make_action_script({Instruction{0x17, 0, 0, 128}})) &&
               script.initialize_state(0, &state), "ordinary instruction initializes");
    const auto ordinary = state;
    expect(script.step_world_map_request(&state, &manager, &result) == Status::InvalidOperand &&
               same_action_state(state, ordinary), "request entry point does not consume ordinary instructions");
}

bool same_request_transition(const awl::WorldMapRequestTransitionState& a,
                             const awl::WorldMapRequestTransitionState& b) {
    return a.manager_state_48 == b.manager_state_48 &&
        a.manager_result_1c0 == b.manager_result_1c0 && a.manager_resource_60 == b.manager_resource_60 &&
        a.has_active_transition == b.has_active_transition && a.active_state_20 == b.active_state_20 &&
        a.clock_current_28 == b.clock_current_28 && a.clock_end_2c == b.clock_end_2c &&
        a.clock_begin_30 == b.clock_begin_30 && a.clock_duration_34 == b.clock_duration_34;
}

void test_world_map_request_transition() {
    using Status = awl::WorldMapRequestTransitionStatus;
    using State = awl::WorldMapRequestTransitionState;
    State state;
    const auto idle = state;
    expect(awl::advance_world_map_request_transition(&state, 100) == Status::Idle &&
               awl::close_world_map_request_transition(&state, 100) == Status::Idle &&
               same_request_transition(state, idle), "idle manager leaves transition fields untouched");
    state.manager_state_48 = 1;
    const auto missing = state;
    expect(awl::advance_world_map_request_transition(&state, 100) == Status::RequiresActiveTransition &&
               awl::close_world_map_request_transition(&state, 100) == Status::RequiresActiveTransition &&
               same_request_transition(state, missing), "missing active request object cannot advance or cancel");
    state.has_active_transition = true;
    state.manager_result_1c0 = 2;
    state.manager_resource_60 = 17;
    state.clock_duration_34 = 4;
    state.active_state_20 = 99; // opening resets the previous state/counter
    expect(awl::advance_world_map_request_transition(&state, 100) == Status::Advanced &&
               state.manager_state_48 == 2 && state.active_state_20 == 0 &&
               state.clock_current_28 == 100 && state.clock_begin_30 == 100 && state.clock_end_2c == 104,
           "opening initializes the active clock before moving manager from one to two");
    expect(awl::advance_world_map_request_transition(&state, 103) == Status::Advanced &&
               state.manager_state_48 == 2 && state.active_state_20 == 0 && state.clock_current_28 == 103,
           "opening stays pending before its unsigned endpoint");
    expect(awl::advance_world_map_request_transition(&state, 104) == Status::Advanced &&
               state.manager_state_48 == 3 && state.active_state_20 == 2 &&
               state.clock_begin_30 == 104 && state.clock_end_2c == 108 && state.clock_current_28 == 108,
           "opening endpoint parks a newly reset clock and enters presentation state");
    const auto presentation = state;
    expect(awl::advance_world_map_request_transition(&state, 109) == Status::RequiresPresentation &&
               same_request_transition(state, presentation), "presentation requires its real backend before proceeding");
    expect(awl::close_world_map_request_transition(&state, 110) == Status::Advanced &&
               state.manager_state_48 == 4 && state.active_state_20 == 1 &&
               state.clock_current_28 == 110 && state.clock_begin_30 == 110 && state.clock_end_2c == 114,
           "cancellation from presentation starts a separate closing clock");
    const auto closing = state;
    expect(awl::close_world_map_request_transition(&state, 112) == Status::Unchanged &&
               same_request_transition(state, closing), "repeated cancellation does not restart closing");
    expect(awl::advance_world_map_request_transition(&state, 113) == Status::Advanced &&
               state.manager_state_48 == 4 && state.clock_current_28 == 113,
           "closing stays busy before its endpoint");
    const auto before_release = state;
    expect(awl::advance_world_map_request_transition(&state, 114) == Status::RequiresResourceRelease &&
               awl::advance_world_map_request_transition(&state, 115) == Status::RequiresResourceRelease &&
               same_request_transition(state, before_release) && state.manager_result_1c0 == 2 &&
               state.manager_resource_60 == 17, "unsupported release cannot clear manager or lose resource/result ownership");
    awl::WorldMapActionScript script;
    awl::WorldMapActionScriptState script_state;
    awl::WorldMapActionStep instruction;
    expect(script.parse(make_action_script({awl::WorldMapActionInstruction{0x25, 0, 0, 4}})) &&
               script.initialize_state(0, &script_state), "request closing polling composition initializes");
    awl::WorldMapCommand4Snapshot poll;
    poll.key_530 = 0;
    poll.key_534 = 0;
    poll.has_manager_state = true;
    poll.manager_state_48 = state.manager_state_48;
    poll.manager_result_1c0 = state.manager_result_1c0;
    expect(script.step_world_map_request(&script_state, &poll, &instruction) ==
               awl::WorldMapActionStepStatus::Advanced && script_state.stack_20[1] == 0 &&
               poll.key_530 == 0 && poll.key_534 == 0,
           "script still polls zero after a closing endpoint until real resource release completes");

    for (uint32_t manager_state : {1u, 2u, 3u}) {
        state = {};
        state.has_active_transition = true;
        state.manager_state_48 = manager_state;
        state.clock_duration_34 = 3;
        expect(awl::close_world_map_request_transition(&state, 20) == Status::Advanced &&
                   state.manager_state_48 == 4 && state.active_state_20 == 1 &&
                   state.clock_current_28 == 20 && state.clock_end_2c == 23,
               "all three active manager states share the verified cancellation branch");
    }
    state = {};
    state.has_active_transition = true;
    state.manager_state_48 = 1;
    expect(awl::advance_world_map_request_transition(&state, 50) == Status::Advanced &&
               state.manager_state_48 == 2 && state.active_state_20 == 0 &&
               awl::advance_world_map_request_transition(&state, 50) == Status::Advanced &&
               state.manager_state_48 == 3 && state.active_state_20 == 2,
           "zero duration still requires the next manager update to finish opening");
    state.manager_state_48 = 2;
    expect(awl::advance_world_map_request_transition(&state, 100) == Status::Advanced &&
               state.manager_state_48 == 3 && state.clock_current_28 == 50 && state.clock_end_2c == 50,
           "parked active endpoint ignores subsequent clock values");
    state.manager_state_48 = 1;
    state.clock_duration_34 = 10;
    expect(awl::advance_world_map_request_transition(&state, 100) == Status::Advanced &&
               awl::advance_world_map_request_transition(&state, 90) == Status::Advanced &&
               state.clock_current_28 == 100 && state.manager_state_48 == 2 &&
               awl::advance_world_map_request_transition(&state, 200) == Status::Advanced &&
               state.manager_state_48 == 3 && state.clock_begin_30 == 200 &&
               state.clock_current_28 == 210 && state.clock_end_2c == 210,
           "rewind clamps to begin and overshoot parks relative to the supplied clock");
    state.manager_state_48 = 1;
    state.clock_duration_34 = 4;
    expect(awl::advance_world_map_request_transition(&state, 0xfffffffe) == Status::Advanced &&
               state.clock_begin_30 == 2 && state.clock_end_2c == 0xfffffffe &&
               state.clock_current_28 == 0xfffffffe &&
               awl::advance_world_map_request_transition(&state, 0xffffffff) == Status::Advanced &&
               state.manager_state_48 == 3 && state.active_state_20 == 2 &&
               state.clock_begin_30 == 3 && state.clock_end_2c == UINT32_MAX && state.clock_current_28 == 3,
           "word overflow preserves unsigned endpoint sorting and original parked-current behavior");
    state = {};
    state.has_active_transition = true;
    state.manager_state_48 = 2;
    state.active_state_20 = 1;
    state.clock_end_2c = 4;
    state.clock_duration_34 = 4;
    expect(awl::advance_world_map_request_transition(&state, 4) == Status::Advanced &&
               state.manager_state_48 == 2 && state.active_state_20 == 3,
           "opening manager tests exact active state two instead of any completed transition");
    state.manager_state_48 = 4;
    state.active_state_20 = 0;
    expect(awl::advance_world_map_request_transition(&state, 8) == Status::Advanced &&
               state.manager_state_48 == 4 && state.active_state_20 == 2,
           "closing manager tests exact active state three instead of any completed transition");
    state.manager_state_48 = 2;
    state.clock_begin_30 = 10;
    state.clock_end_2c = 5;
    const auto bad_clock = state;
    expect(awl::advance_world_map_request_transition(&state, 20) == Status::InvalidState &&
               same_request_transition(state, bad_clock), "invalid counter bounds fail atomically");
    state.clock_begin_30 = 0;
    state.clock_current_28 = 0;
    state.clock_end_2c = 5;
    state.active_state_20 = 4;
    const auto bad_active = state;
    expect(awl::advance_world_map_request_transition(&state, 20) == Status::InvalidState &&
               same_request_transition(state, bad_active), "unsupported active transition state is rejected");
    state.manager_state_48 = 5;
    const auto invalid = state;
    expect(awl::advance_world_map_request_transition(&state, 20) == Status::InvalidState &&
               awl::close_world_map_request_transition(&state, 20) == Status::InvalidState &&
               same_request_transition(state, invalid) &&
               awl::advance_world_map_request_transition(nullptr, 20) == Status::InvalidState &&
               awl::close_world_map_request_transition(nullptr, 20) == Status::InvalidState,
           "invalid manager state and null outputs cannot enter the unchecked DOL dispatch table");
}

void test_world_map_room_collision_mapping() {
    using Status = awl::WorldMapRoomCollisionStatus;
    awl::WorldMapRoomCollisionState state;
    awl::WorldMapRoomCollisionSelection selected;
    constexpr uint32_t expected_three[6] = {40, 45, 46, 47, 47, 47};
    constexpr uint32_t expected_forty[6] = {0, 1, 2, 3, 4, 5};
    for (uint32_t phase = 0; phase < 6; ++phase) {
        state.phase_index = phase;
        expect(awl::select_world_map_room_collision_record(
                   3, state, &selected) == Status::Found &&
                   selected.record_index == expected_three[phase],
               "room ID 3 follows the DOL phase remap");
        expect(awl::select_world_map_room_collision_record(
                   40, state, &selected) == Status::Found &&
                   selected.record_index == expected_forty[phase] &&
                   selected.remapped_id ==
                       (phase == 0 ? 40u : 76u + phase),
               "room ID 40 selects its six phase records");
        expect(awl::select_world_map_room_collision_record(
                   5, state, &selected) == Status::Found &&
                   selected.record_index == (phase < 3 ? 41u : 48u),
               "room ID 5 changes record in later phases");
    }
    state = {};
    expect(awl::select_world_map_room_collision_record(
               7, state, &selected) == Status::Found &&
               selected.remapped_id == 7 && selected.record_index == 52,
           "room ID 7 clear-state record");
    state.state_299a6 = true;
    expect(awl::select_world_map_room_collision_record(
               7, state, &selected) == Status::Found &&
               selected.remapped_id == 50 && selected.record_index == 52,
           "room ID 7 state byte remaps its key");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               10, state, &selected) == Status::Found &&
               selected.record_index == 49,
           "room ID 10 clear-state record");
    state.state_299a5 = true;
    expect(awl::select_world_map_room_collision_record(
               10, state, &selected) == Status::Found &&
               selected.remapped_id == 51 && selected.record_index == 50,
           "room ID 10 state byte selects a different record");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               13, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 13,
           "room ID 13 has no record with clear state");
    state.state_299ae = true;
    expect(awl::select_world_map_room_collision_record(
               13, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 52,
           "room ID 13 remains unmapped after its state remap");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               4, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 4,
           "room ID 4 sentinel returns no record");
    state.phase_index = 3;
    expect(awl::select_world_map_room_collision_record(
               4, state, &selected) == Status::NoMapping &&
               selected.remapped_id == 48,
           "room ID 4 later-phase key is also unmapped");
    state = {};
    expect(awl::select_world_map_room_collision_record(
               82, state, &selected) == Status::UnsupportedInput &&
               selected.remapped_id == 0,
           "room IDs outside the bounded table are rejected");
    state.phase_index = 6;
    expect(awl::select_world_map_room_collision_record(
               40, state, &selected) == Status::UnsupportedInput &&
               selected.remapped_id == 0 &&
               awl::select_world_map_room_collision_record(
                   40, state, nullptr) == Status::UnsupportedInput,
           "invalid phase and null result are rejected");
    awl::WorldMapCollisionRecordPools unloaded;
    awl::WorldMapCollisionRecordView view;
    state = {};
    expect(unloaded.lookup_room_object(40, state, &view) ==
               Status::MissingRecord && view.data == nullptr &&
               unloaded.lookup_room_object(4, state, &view) ==
                   Status::NoMapping &&
               unloaded.lookup_room_object(40, state, nullptr) ==
                   Status::UnsupportedInput,
           "room lookup distinguishes absent data, no mapping, and bad output");
}

std::vector<uint8_t> make_one_level_tree() {
    constexpr uint32_t root = 8;
    constexpr uint32_t node_size = 0x34;
    std::vector<uint8_t> bytes(root + node_size * 5, 0);
    initialize_header(bytes);
    for (uint32_t index = 0; index < 4; ++index) {
        const uint32_t child = root + node_size * (index + 1);
        put_be32(bytes, root + 0x0c + index * 4, child);
        initialize_leaf(bytes, child);
    }
    put_be32(bytes, root + 0x28, node_size);
    put_be32(bytes, root + 0x2c, node_size);
    put_be32(bytes, root + 0x30, 0);
    return bytes;
}

void test_world_map_archive_rejects_untranslated_tree_shape() {
    auto tree = make_one_level_tree();
    tree[6] = 0;
    awl::CollisionTreeAnalysis shape;
    expect(awl::analyze_type1_collision_asset(tree.data(), tree.size(), &shape) &&
               shape.node_count == 5 && shape.leaf_count == 4,
           "multi-leaf rejection fixture is independently valid type-1 COL");
    awl::WorldMapCollisionArchive archive;
    expect(!archive.parse(make_record_arc(tree)) && archive.record_count() == 0,
           "multi-leaf archive record waits for translated leaf selection");
}

void initialize_sample_payload(std::vector<uint8_t>& bytes,
                               uint32_t node,
                               uint32_t payload,
                               int16_t origin_x,
                               int16_t origin_z) {
    put_be_s16(bytes, node, origin_x);
    put_be_s16(bytes, node + 2, 0);
    put_be_s16(bytes, node + 4, origin_z);
    put_be_s16(bytes, node + 6, static_cast<int16_t>(origin_x + 10));
    put_be_s16(bytes, node + 8, 20);
    put_be_s16(bytes, node + 10, static_cast<int16_t>(origin_z + 10));
    put_be32(bytes, node + 0x1c, 1);
    put_be32(bytes, node + 0x20, 3);
    put_be32(bytes, node + 0x24, 0);
    put_be32(bytes, node + 0x28, payload - node);
    put_be32(bytes, node + 0x2c, payload + 8 - node);
    put_be32(bytes, node + 0x30, 0);

    put_be16(bytes, payload, 0x20);
    put_be16(bytes, payload + 2, 0);
    put_be16(bytes, payload + 4, 1);
    put_be16(bytes, payload + 6, 2);

    const uint32_t vertices = payload + 8;
    put_be_s16(bytes, vertices, origin_x);
    put_be_s16(bytes, vertices + 2, 0);
    put_be_s16(bytes, vertices + 4, origin_z);
    put_be_s16(bytes, vertices + 8, static_cast<int16_t>(origin_x + 10));
    put_be_s16(bytes, vertices + 10, 10);
    put_be_s16(bytes, vertices + 12, origin_z);
    put_be_s16(bytes, vertices + 16, origin_x);
    put_be_s16(bytes, vertices + 18, 20);
    put_be_s16(bytes, vertices + 20, static_cast<int16_t>(origin_z + 10));
}

std::vector<uint8_t> make_sample_leaf() {
    constexpr uint32_t node = 8;
    constexpr uint32_t payload = node + 0x34;
    std::vector<uint8_t> bytes(payload + 8 + 3 * 8, 0);
    initialize_header(bytes);
    bytes[5] = 0;
    initialize_sample_payload(bytes, node, payload, 0, 0);
    return bytes;
}

std::vector<uint8_t> make_one_level_sample_tree() {
    constexpr uint32_t root = 8;
    constexpr uint32_t node_size = 0x34;
    constexpr uint32_t selected_leaf = root + node_size * 4;
    constexpr uint32_t payload = root + node_size * 5;
    std::vector<uint8_t> bytes(payload + 8 + 3 * 8, 0);
    initialize_header(bytes);
    bytes[5] = 0;
    put_be_s16(bytes, root, 0);
    put_be_s16(bytes, root + 2, 0);
    put_be_s16(bytes, root + 4, 0);
    put_be_s16(bytes, root + 6, 20);
    put_be_s16(bytes, root + 8, 20);
    put_be_s16(bytes, root + 10, 20);
    for (uint32_t index = 0; index < 4; ++index) {
        const uint32_t child = root + node_size * (index + 1);
        put_be32(bytes, root + 0x0c + index * 4, child);
        initialize_leaf(bytes, child);
    }
    put_be32(bytes, root + 0x28, node_size);
    put_be32(bytes, root + 0x2c, node_size);
    initialize_sample_payload(bytes, selected_leaf, payload, 10, 10);
    return bytes;
}

bool analyze(const std::vector<uint8_t>& bytes,
             awl::CollisionTreeAnalysis& analysis) {
    return awl::analyze_type1_collision_asset(bytes.data(), bytes.size(),
                                              &analysis);
}

void test_valid_structures() {
    awl::CollisionTreeAnalysis analysis;
    std::vector<uint8_t> bytes = make_single_leaf();
    expect(analyze(bytes, analysis), "single-leaf collision tree is valid");
    expect(analysis.format == 1 && analysis.header_byte_5 == 6 &&
               analysis.header_byte_6 == 1 && analysis.header_byte_7 == 0,
           "collision header bytes are preserved without guessed semantics");
    expect(analysis.node_count == 1 && analysis.leaf_count == 1 &&
               analysis.triangle_count == 0 && analysis.vertex_count == 0 &&
               analysis.max_depth == 0,
           "single-leaf collision metadata is exact");

    bytes = make_one_level_tree();
    expect(analyze(bytes, analysis), "one-level collision tree is valid");
    expect(analysis.node_count == 5 && analysis.leaf_count == 4 &&
               analysis.max_depth == 1,
           "one-level collision metadata is exact");

    bytes = make_sample_leaf();
    expect(analyze(bytes, analysis), "indexed triangle payload is valid");
    expect(analysis.triangle_count == 1 && analysis.vertex_count == 3 &&
               analysis.max_triangles_per_leaf == 1 &&
               analysis.max_vertices_per_leaf == 3 &&
               analysis.coordinate_scale == 1.0f &&
               analysis.root_min[0] == 0.0f &&
               analysis.root_max[1] == 20.0f,
           "indexed payload metadata and root bounds are exact");
}

void test_rejects_unsupported_or_truncated_files() {
    awl::CollisionTreeAnalysis analysis;
    std::vector<uint8_t> bytes = make_single_leaf();
    bytes[0] = 0;
    expect(!analyze(bytes, analysis), "wrong collision marker is rejected");
    expect(analysis.node_count == 0, "failed analysis clears its output");

    bytes = make_single_leaf();
    bytes[4] = 0;
    expect(!analyze(bytes, analysis), "unsupported collision format is rejected");

    bytes = make_single_leaf();
    bytes.pop_back();
    expect(!analyze(bytes, analysis), "truncated collision root is rejected");
    expect(!awl::analyze_type1_collision_asset(nullptr, 0, &analysis),
           "null collision data is rejected");
    expect(!awl::analyze_type1_collision_asset(bytes.data(), bytes.size(), nullptr),
           "null collision output is rejected");
}

void test_rejects_invalid_offsets() {
    awl::CollisionTreeAnalysis analysis;
    std::vector<uint8_t> bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x10, 0);
    expect(!analyze(bytes, analysis), "partial child sets are rejected");

    bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x10, static_cast<uint32_t>(bytes.size()));
    expect(!analyze(bytes, analysis), "out-of-range child nodes are rejected");

    bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x10, 8 + 0x34);
    expect(!analyze(bytes, analysis), "shared child nodes are rejected");

    bytes = make_one_level_tree();
    put_be32(bytes, 8 + 0x0c, 8);
    expect(!analyze(bytes, analysis), "collision tree cycles are rejected");

    bytes = make_single_leaf();
    put_be32(bytes, 8 + 0x28, 0xFFFFFFFFu);
    expect(!analyze(bytes, analysis), "out-of-range leaf payloads are rejected");

    bytes = make_sample_leaf();
    put_be16(bytes, 8 + 0x34 + 2, 3);
    expect(!analyze(bytes, analysis), "out-of-range vertex indices are rejected");

    bytes = make_sample_leaf();
    put_be32(bytes, 8 + 0x24, 1);
    expect(!analyze(bytes, analysis), "unsupported third payloads are rejected");

    bytes = make_sample_leaf();
    put_be_s16(bytes, 8, 11);
    expect(!analyze(bytes, analysis), "inverted node bounds are rejected");

    bytes = make_sample_leaf();
    bytes[5] = 31;
    expect(!analyze(bytes, analysis), "unsafe coordinate exponents are rejected");
}

void test_primary_surface_sample() {
    awl::CollisionSurfaceSample sample;
    std::vector<uint8_t> bytes = make_sample_leaf();
    expect(awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                               2.0f, 2.0f, &sample),
           "point inside a leaf triangle produces a surface sample");
    expect(sample.height == 6.0f && sample.surface_flags == 0x20 &&
               sample.leaf_offset == 8 && sample.triangle_index == 0,
           "surface height, flags, leaf, and triangle are exact");

    put_be16(bytes, 8 + 0x34 + 4, 2);
    put_be16(bytes, 8 + 0x34 + 6, 1);
    expect(awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                               2.0f, 2.0f, &sample) &&
               sample.height == 6.0f,
           "surface containment accepts the opposite winding");

    expect(!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                                20.0f, 20.0f, &sample),
           "point outside the leaf triangle has no primary sample");
    expect(sample.height == 0.0f && sample.surface_flags == 0,
           "failed surface sampling clears its output");
    expect(!awl::sample_type1_collision_surface(
               bytes.data(), bytes.size(),
               std::numeric_limits<float>::quiet_NaN(), 0.0f, &sample),
           "non-finite surface coordinates are rejected");
    expect(!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                                0.0f, 0.0f, nullptr),
           "null surface output is rejected");

    bytes = make_one_level_sample_tree();
    expect(awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                               12.0f, 12.0f, &sample),
           "quadtree traversal selects the matching populated leaf");
    expect(sample.height == 6.0f && sample.leaf_offset == 8 + 0x34 * 4,
           "quadtree surface sample preserves the selected leaf");
    expect(!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                                2.0f, 2.0f, &sample),
           "quadtree traversal does not search unrelated leaves");
}

void test_nearest_edge_fallback() {
    awl::CollisionEdgeSample sample;
    std::vector<uint8_t> bytes = make_sample_leaf();
    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 6.0f, -3.0f, &sample),
           "outside point projects onto the nearest triangle edge");
    expect(sample.position[0] == 6.0f && sample.position[1] == 6.0f &&
               sample.position[2] == 0.0f &&
               sample.distance_squared_xz == 9.0f &&
               sample.surface_flags == 0x20 && sample.leaf_offset == 8 &&
               sample.triangle_index == 0 && sample.edge_index == 0,
           "edge projection and plane height match the triangle geometry");

    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 12.0f, -3.0f, &sample),
           "projection clamps to the nearest edge endpoint");
    expect(sample.position[0] == 10.0f && sample.position[1] == 10.0f &&
               sample.position[2] == 0.0f &&
               sample.distance_squared_xz == 13.0f && sample.edge_index == 0,
           "endpoint ties keep the first edge in serialized order");

    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 8.0f, 8.0f, &sample),
           "fallback selects the sloped triangle edge");
    expect(sample.position[0] == 5.0f && sample.position[1] == 15.0f &&
               sample.position[2] == 5.0f &&
               sample.distance_squared_xz == 18.0f && sample.edge_index == 1,
           "sloped edge projection has the expected height and distance");

    expect(!awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(),
               std::numeric_limits<float>::infinity(), 0.0f, &sample),
           "non-finite edge query is rejected");
    expect(sample.position[0] == 0.0f && sample.surface_flags == 0,
           "failed edge projection clears its output");

    bytes = make_single_leaf();
    expect(!awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 0.0f, 0.0f, &sample),
           "empty leaf has no edge projection");

    bytes = make_one_level_sample_tree();
    expect(awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 18.0f, 18.0f, &sample) &&
               sample.leaf_offset == 8 + 0x34 * 4,
           "edge fallback uses the selected leaf");
    expect(!awl::project_type1_collision_to_edge(
               bytes.data(), bytes.size(), 2.0f, 2.0f, &sample),
           "edge fallback does not search other leaves");
}

void test_terrain_height_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionTerrainAdjustment adjustment;
    expect(awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f}, &adjustment),
           "terrain adjustment samples a containing triangle");
    expect(adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               adjustment.surface_flags == 0x20 &&
               !adjustment.used_edge_fallback,
           "inside terrain replaces only height and reports no fallback");

    expect(awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {6.0f, 50.0f, -3.0f}, &adjustment),
           "terrain adjustment handles a point outside the triangle");
    expect(adjustment.position == std::array<float, 3>{6.0f, 6.0f, 0.0f} &&
               adjustment.surface_flags == 0x20 &&
               adjustment.used_edge_fallback,
           "outside terrain uses the nearest edge position and height");

    expect(!awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(),
               {0.0f, std::numeric_limits<float>::infinity(), 0.0f},
               &adjustment),
           "non-finite proposed height is rejected");
    expect(adjustment.position == std::array<float, 3>{} &&
               !adjustment.used_edge_fallback,
           "failed terrain adjustment clears its output");
    expect(!awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {2.0f, 0.0f, 2.0f}, nullptr),
           "null terrain adjustment output is rejected");

    bytes = make_single_leaf();
    expect(!awl::adjust_type1_collision_terrain_height(
               bytes.data(), bytes.size(), {0.0f, 0.0f, 0.0f}, &adjustment),
           "empty selected leaf cannot provide terrain adjustment");
}

void test_resolver_height_resampling() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionResolverHeightAdjustment adjustment;
    expect(awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f},
               &adjustment) &&
               adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               adjustment.surface_flags == 0x20 &&
               adjustment.leaf_offset == 8 &&
               !adjustment.used_edge_fallback,
           "resolver height samples the final position's containing triangle");
    expect(awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {6.0f, 50.0f, -3.0f},
               &adjustment) &&
               adjustment.position == std::array<float, 3>{6.0f, 6.0f, -3.0f} &&
               adjustment.surface_flags == 0x20 &&
               adjustment.used_edge_fallback,
           "resolver fallback replaces Y without moving the final X/Z");

    bytes = make_one_level_sample_tree();
    constexpr uint32_t populated_leaf = 8 + 0x34 * 4;
    expect(awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {12.0f, 50.0f, 12.0f},
               &adjustment) && adjustment.leaf_offset == populated_leaf &&
               adjustment.position == std::array<float, 3>{12.0f, 6.0f, 12.0f},
           "resolver height queries the populated final-position leaf");
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f},
               &adjustment) &&
               adjustment.position == std::array<float, 3>{} &&
               adjustment.leaf_offset == 0,
           "crossing into an empty leaf does not reuse the prior leaf");

    bytes = make_sample_leaf();
    bytes[6] = 0;
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f},
               &adjustment),
           "unverified resolver height mode is rejected");
    bytes[6] = 1;
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(),
               {2.0f, std::numeric_limits<float>::infinity(), 2.0f},
               &adjustment),
           "nonfinite resolver position is rejected");
    expect(!awl::resample_type1_collision_resolver_height(
               bytes.data(), bytes.size(), {2.0f, 50.0f, 2.0f}, nullptr),
           "null resolver height output is rejected");
}

void test_dynamic_contact_broad_phase() {
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    bool may_contact = false;
    expect(awl::evaluate_dynamic_contact_broad_phase(
               identity, {3.0f, 100.0f, 4.0f}, {0.0f, -50.0f, 0.0f},
               2.0f, 3.0f, &may_contact) && may_contact,
           "exact combined-radius boundary reaches narrow phase in X/Z");
    expect(awl::evaluate_dynamic_contact_broad_phase(
               identity, {3.0f, 100.0f, 4.0f}, {0.0f, 0.0f, 0.0f},
               2.0f, 2.0f, &may_contact) && !may_contact,
           "candidate beyond the combined radius skips narrow phase");

    awl::CollisionAffineTransform translated = identity;
    translated[3] = 10.0f;
    translated[11] = -5.0f;
    translated[1] = 1000.0f;
    translated[9] = 1000.0f;
    expect(awl::evaluate_dynamic_contact_broad_phase(
               translated, {3.0f, 100.0f, 4.0f}, {13.0f, -50.0f, -1.0f},
               0.0f, 0.0f, &may_contact) && may_contact,
           "world-to-object translation applies after the query Y is cleared");

    awl::CollisionAffineTransform rotated = identity;
    rotated[0] = 0.0f;
    rotated[2] = 1.0f;
    rotated[8] = -1.0f;
    rotated[10] = 0.0f;
    expect(awl::evaluate_dynamic_contact_broad_phase(
               rotated, {3.0f, 0.0f, 4.0f}, {4.0f, 0.0f, -3.0f},
               0.0f, 0.0f, &may_contact) && may_contact,
           "world-to-object rotation affects the local X/Z distance");

    expect(!awl::evaluate_dynamic_contact_broad_phase(
               identity, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
               -1.0f, 0.0f, &may_contact) && !may_contact,
           "negative collision radius is rejected and output is cleared");
    translated[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::evaluate_dynamic_contact_broad_phase(
               translated, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
               1.0f, 1.0f, &may_contact) && !may_contact,
           "nonfinite transform is rejected");
    expect(!awl::evaluate_dynamic_contact_broad_phase(
               identity, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
               1.0f, 1.0f, nullptr),
           "null broad-phase output is rejected");
}

void test_radius_vertex_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusVertexAdjustment adjustment;
    const std::array<float, 3> query{0.5f, 7.0f, 0.0f};
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment),
           "supported type-1 vertex radius query succeeds");
    expect(!adjustment.contact && adjustment.position == query,
           "unmarked triangle edges do not produce vertex contact");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment),
           "marked triangle edge participates in vertex radius query");
    expect(adjustment.contact && adjustment.triangle_index == 0 &&
               adjustment.vertex_index == 0 &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               adjustment.position[1] == 0.0f &&
               adjustment.position[2] == 0.0f,
           "nearest incident vertex pushes X/Z to radius plus 0.01 and uses vertex Y");

    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 0.0f}, 1.0f,
               &adjustment) &&
               adjustment.contact &&
               adjustment.position == std::array<float, 3>{0.0f, 7.0f, 0.0f},
           "exact vertex contact preserves the original point");
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), {1.0f, 7.0f, 0.0f}, 1.0f,
               &adjustment) && !adjustment.contact,
           "point exactly at radius has no vertex contact");

    put_be16(bytes, 8 + 0x34, 0x4000);
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment) &&
               !adjustment.contact,
           "edge-one bit does not mark the unrelated first vertex");
    expect(awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 9.5f}, 1.0f,
               &adjustment) &&
               adjustment.contact && adjustment.vertex_index == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f &&
               adjustment.position[1] == 20.0f,
           "edge-one bit marks its second endpoint for radius response");

    bytes[6] = 0;
    expect(!awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, &adjustment),
           "other collision mode is rejected by the bounded radius helper");
    expect(adjustment.position == std::array<float, 3>{} &&
               !adjustment.contact,
           "failed vertex query clears its output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, -1.0f, &adjustment),
           "negative radius is rejected");
    expect(!awl::adjust_type1_collision_radius_vertex(
               bytes.data(), bytes.size(), query, 1.0f, nullptr),
           "null vertex query output is rejected");
}

void test_dynamic_contact_vertex_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusVertexAdjustment adjustment;
    const std::array<float, 3> query{0.5f, 7.0f, 0.0f};
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x20u,
               &adjustment) &&
               adjustment.contact && adjustment.triangle_index == 0 &&
               adjustment.vertex_index == 0 &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               adjustment.position[1] == 0.0f &&
               adjustment.position[2] == 0.0f,
           "dynamic vertex pass accepts an unmarked vertex on a matching surface");

    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) &&
               !adjustment.contact && adjustment.position == query,
           "surface mask excludes a nonmatching nonzero triangle flag");
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x10000u,
               &adjustment) && adjustment.contact,
           "surface mask bypass bit accepts the triangle");
    put_be16(bytes, 8 + 0x34, 0);
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && adjustment.contact,
           "zero surface flags pass the mask filter");

    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 0.0f}, 1.0f,
               0x40u, &adjustment) && adjustment.contact &&
               adjustment.position == std::array<float, 3>{0.0f, 7.0f, 0.0f},
           "exact dynamic vertex contact leaves the candidate unchanged");
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), {1.0f, 7.0f, 0.0f}, 1.0f,
               0x40u, &adjustment) && !adjustment.contact,
           "dynamic vertex radius boundary is strict");
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), {5.0f, 7.0f, 0.0f}, 6.0f,
               0x40u, &adjustment) && adjustment.contact &&
               adjustment.vertex_index == 0 &&
               std::fabs(adjustment.position[0] - 6.01f) < 0.0001f,
           "equal-distance vertices keep the first serialized vertex");

    bytes[6] = 0;
    expect(awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f,
           "type-1 mode zero keeps the dynamic vertex response");
    bytes[6] = 2;
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unverified dynamic vertex mode is rejected and clears output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, -1.0f, 0x40u,
               &adjustment),
           "negative dynamic vertex radius is rejected");
    expect(!awl::adjust_type1_dynamic_contact_vertex(
               bytes.data(), bytes.size(), query, 1.0f, 0x40u, nullptr),
           "null dynamic vertex output is rejected");
}

void test_dynamic_contact_edge_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusEdgeAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x20u,
               &adjustment) && adjustment.contact &&
               adjustment.surface_flags == 0x20 &&
               adjustment.triangle_index == 0 && adjustment.edge_index == 0 &&
               adjustment.position[0] == 5.0f &&
               adjustment.position[1] == 7.0f &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "dynamic edge pass checks an unmarked edge on a matching surface");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == proposed,
           "dynamic edge pass excludes a nonmatching surface");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x10000u, &adjustment) && adjustment.contact,
           "dynamic edge pass accepts the surface-mask bypass bit");
    put_be16(bytes, 8 + 0x34, 0);
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && adjustment.contact,
           "zero surface flags remain eligible for dynamic edge response");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, {5.0f, 7.0f, 0.5f},
               1.0f, 0x40u, &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "dynamic edge response handles a candidate crossing the plane");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), {5.0f, 7.0f, 0.0f}, proposed,
               1.0f, 0x40u, &adjustment) && !adjustment.contact,
           "prior point on the edge plane does not produce dynamic contact");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, {10.0f, 7.0f, -0.5f},
               1.0f, 0x40u, &adjustment) && !adjustment.contact,
           "dynamic edge excludes its final endpoint");
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 0.5f, 0x40u,
               &adjustment) && !adjustment.contact,
           "dynamic edge contact is strict at the radius boundary");

    bytes[6] = 0;
    expect(awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "type-1 mode zero keeps the dynamic edge response");
    bytes[6] = 2;
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unverified dynamic edge mode is rejected and clears output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, -1.0f, 0x40u,
               &adjustment),
           "negative dynamic edge radius is rejected");
    expect(!awl::adjust_type1_dynamic_contact_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x40u,
               nullptr),
           "null dynamic edge output is rejected");
}

void test_dynamic_contact_narrow_phase() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionDynamicContactAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, prior, 1.0f, 0x20u,
               0u, &adjustment) && !adjustment.contact &&
               adjustment.pass_count == 1 && adjustment.position == prior,
           "clear dynamic narrow phase ends after one edge-vertex pass");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x20u,
               0u, &adjustment) && adjustment.contact &&
               adjustment.first_edge_contact &&
               adjustment.first_edge_surface_flags == 0x20 &&
               !adjustment.reverted_to_prior && adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "first dynamic edge contact clears on the second pinned-leaf pass");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f, 0x20u,
               1u, &adjustment) && adjustment.contact &&
               adjustment.reverted_to_prior && adjustment.pass_count == 1 &&
               adjustment.position == prior,
           "contact flag one restores the prior position after the first hit");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 2.0f},
               {0.0f, 7.0f, 0.0f}, 1.0f, 0x20u, 0u, &adjustment) &&
               adjustment.contact && adjustment.reverted_to_prior &&
               adjustment.pass_count == 3 &&
               adjustment.position == std::array<float, 3>{0.0f, 7.0f, 2.0f},
           "persistent dynamic vertex contact restores the prior position");

    const std::array<float, 3> inside_prior{2.0f, 7.0f, 2.0f};
    const std::array<float, 3> inside_proposed{3.0f, 7.0f, 2.0f};
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), inside_prior, inside_proposed,
               1.0f, 0x20u, 2u, &adjustment) &&
               adjustment.used_containment_shortcut && adjustment.contact &&
               adjustment.reverted_to_prior && adjustment.pass_count == 0 &&
               adjustment.position == inside_prior,
           "matching prior triangle with flag two reports contact and prior point");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), inside_prior, inside_proposed,
               1.0f, 0x20u, 4u, &adjustment) &&
               adjustment.used_containment_shortcut && !adjustment.contact &&
               !adjustment.reverted_to_prior && adjustment.pass_count == 0 &&
               adjustment.position == inside_proposed,
           "matching prior triangle with flag four keeps the proposed point");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), inside_prior, inside_proposed,
               1.0f, 0x40u, 2u, &adjustment) &&
               !adjustment.used_containment_shortcut && !adjustment.contact &&
               adjustment.pass_count == 1 &&
               adjustment.position == inside_proposed,
           "nonmatching surface mask bypasses the containment shortcut");
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, {5.0f, 7.0f, 0.5f},
               1.0f, 0x20u, 2u, &adjustment) &&
               !adjustment.used_containment_shortcut && adjustment.contact,
           "containment shortcut tests the prior point, not the proposal");

    bytes = make_one_level_sample_tree();
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, 0x20u, 0u,
               &adjustment) && adjustment.contact &&
               !adjustment.reverted_to_prior && adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f,
           "dynamic response keeps its selected leaf after crossing a boundary");
    bytes[6] = 0;
    expect(awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, 0x20u, 0u,
               &adjustment) && adjustment.contact &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f,
           "type-1 mode zero keeps the pinned-leaf narrow-phase response");
    bytes[6] = 2;
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x20u, 0u, &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "unverified dynamic narrow-phase mode is rejected");
    bytes[6] = 1;
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               0x20u, 0u, &adjustment),
           "negative dynamic narrow-phase radius is rejected");
    expect(!awl::resolve_type1_dynamic_contact_narrow_phase(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               0x20u, 0u, nullptr),
           "null dynamic narrow-phase output is rejected");
}

void test_dynamic_object_contact() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicObjectQuery query;
    query.world_to_object = identity;
    query.object_to_world = identity;
    query.object_center_local = {5.0f, 0.0f, 0.0f};
    query.moving_radius = 1.0f;
    query.surface_mask = 0x20u;
    awl::CollisionDynamicObjectContactAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && adjustment.broad_phase_passed &&
               adjustment.contact && adjustment.local_narrow_phase.contact &&
               adjustment.position[0] == 5.0f &&
               adjustment.position[1] == 0.0f &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "contacted object response clears local Y before world transform");

    query.object_center_local = {100.0f, 0.0f, 0.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && !adjustment.broad_phase_passed &&
               !adjustment.contact && adjustment.position == proposed,
           "broad-phase miss preserves the original world proposal");
    query.object_center_local = {5.0f, 0.0f, -2.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, prior,
               &adjustment) && adjustment.broad_phase_passed &&
               !adjustment.contact &&
               adjustment.local_narrow_phase.pass_count == 1 &&
               adjustment.position == prior,
           "narrow-phase miss also preserves the original world proposal");

    query.world_to_object = identity;
    query.object_to_world = identity;
    query.world_to_object[1] = 1000.0f;
    query.world_to_object[9] = 1000.0f;
    query.world_to_object[3] = 10.0f;
    query.world_to_object[7] = -42.0f;
    query.world_to_object[11] = -5.0f;
    query.object_to_world[1] = 1000.0f;
    query.object_to_world[9] = 1000.0f;
    query.object_to_world[3] = -10.0f;
    query.object_to_world[7] = 42.0f;
    query.object_to_world[11] = 5.0f;
    query.object_center_local = {5.0f, 0.0f, 0.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, {-5.0f, 7.0f, 3.0f},
               {-5.0f, 9.0f, 4.5f}, &adjustment) &&
               adjustment.contact && adjustment.position[0] == -5.0f &&
               adjustment.position[1] == 42.0f &&
               std::fabs(adjustment.position[2] - 3.99f) < 0.0001f,
           "translation uses horizontal inputs and transforms contacted local output");

    query.world_to_object = {
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f, 0.0f};
    query.object_to_world = {
        0.0f, 0.0f, -1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f};
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, {2.0f, 7.0f, 5.0f},
               {0.5f, 9.0f, 5.0f}, &adjustment) &&
               adjustment.contact &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               adjustment.position[1] == 0.0f &&
               adjustment.position[2] == 5.0f,
           "rotation takes the local contact result back to world X/Z");

    query.object_to_world[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "nonfinite output transform is rejected and clears output");
    query.object_to_world = identity;
    query.world_to_object = identity;
    bytes[6] = 0;
    expect(awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "type-1 mode zero supports the transformed object response");
    bytes[6] = 2;
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               &adjustment),
           "unverified object collision mode is rejected");
    bytes[6] = 1;
    expect(!awl::resolve_type1_dynamic_object_contact(
               bytes.data(), bytes.size(), query, prior, proposed,
               nullptr),
           "null object contact output is rejected");
}

void test_first_dynamic_object_pass() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject object;
    object.identity = 1;
    object.enabled = true;
    object.category = 1;
    object.collision_flags = 2u;
    object.data = bytes.data();
    object.size = bytes.size();
    object.world_to_object = identity;
    object.object_to_world = identity;
    object.center_local = {5.0f, 0.0f, 0.0f};
    awl::CollisionDynamicPassAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};

    expect(awl::resolve_type1_first_dynamic_object_pass(
               &object, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 1 &&
               adjustment.contact_count == 1 &&
               adjustment.contact_flags_after == 1u &&
               adjustment.resolver_contact_bit == 4u &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "first list pass applies one type-1 contact and sets sticky bit one");

    awl::CollisionDynamicPassObject skipped_self = object;
    skipped_self.identity = 99;
    awl::CollisionDynamicPassObject skipped_disabled = object;
    skipped_disabled.identity = 2;
    skipped_disabled.enabled = false;
    awl::CollisionDynamicPassObject skipped_category = object;
    skipped_category.identity = 3;
    skipped_category.category = 7;
    awl::CollisionDynamicPassObject skipped_flags = object;
    skipped_flags.identity = 4;
    skipped_flags.collision_flags = 0;
    const std::array<awl::CollisionDynamicPassObject, 5> filtered{
        skipped_self, skipped_disabled, skipped_category, skipped_flags,
        object};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               filtered.data(), filtered.size(), 99, 1, prior, proposed,
               1.0f, 0u, 4u, &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 1 &&
               adjustment.contact_count == 1,
           "list pass skips self, disabled, wrong-category, and inert objects");

    awl::CollisionDynamicPassObject second = object;
    second.identity = 2;
    second.world_to_object[11] = 1.0f;
    second.object_to_world[11] = -1.0f;
    const std::array<awl::CollisionDynamicPassObject, 2> ordered{object, second};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               ordered.data(), ordered.size(), 0, 1, prior, proposed,
               1.0f, 0u, 4u, &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 2 &&
               adjustment.contact_count == 2 &&
               adjustment.contact_flags_after == 1u &&
               adjustment.position[0] == prior[0] &&
               adjustment.position[2] == prior[2],
           "second object sees the first response and sticky contact flag");

    expect(awl::resolve_type1_first_dynamic_object_pass(
               ordered.data(), ordered.size(), 0, 1, prior, proposed,
               1.0f, 0x20u, 0u, &adjustment) && !adjustment.contact &&
               adjustment.queried_objects == 0 &&
               adjustment.contact_flags_after == 0x20u &&
               adjustment.position == proposed,
           "disabled resolver pass leaves the proposal and flags unchanged");
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               ordered.data(), ordered.size(), 0, 1, prior, proposed,
               1.0f, 0u, 0x14u, &adjustment) && !adjustment.contact &&
               adjustment.position == std::array<float, 3>{},
           "alternate flag-0x10 branch is rejected by this bounded pass");
    awl::CollisionDynamicPassObject circle = object;
    circle.collision_flags = 1u;
    circle.center_world = {5.0f, 20.0f, 0.0f};
    circle.radius = 0.5f;
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 1 &&
               adjustment.contact_count == 1 &&
               adjustment.contact_flags_after == 1u &&
               adjustment.resolver_contact_bit == 4u &&
               std::fabs(adjustment.position[2] + 1.51f) < 0.0001f &&
               adjustment.position[1] == proposed[1],
           "circle overlap pushes in X/Z and preserves the proposed height");
    const std::array<float, 3> circle_boundary{5.0f, 9.0f, -1.5f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, circle_boundary, 1.0f, 0u, 4u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == circle_boundary &&
               adjustment.queried_objects == 1,
           "circle contact uses a strict radius boundary");
    const std::array<float, 3> circle_outside{5.0f, 9.0f, -2.0f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, circle_outside, 1.0f, 0u, 4u,
               &adjustment) && !adjustment.contact &&
               adjustment.position == circle_outside,
           "separated circles leave the candidate unchanged");
    const std::array<float, 3> circle_center{5.0f, 9.0f, 0.0f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, circle_center, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               adjustment.position[0] == circle_center[0] &&
               std::fabs(adjustment.position[2] - 1.51f) < 0.0001f &&
               adjustment.position[1] == circle_center[1],
           "coincident circle centers use the DOL's positive-Z fallback");
    circle.center_world = {5.0f, 0.0f, -1.0f};
    const std::array<awl::CollisionDynamicPassObject, 2> mixed{object, circle};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               mixed.data(), mixed.size(), 0, 1, prior, proposed, 1.0f,
               0u, 4u, &adjustment) && adjustment.contact &&
               adjustment.queried_objects == 2 &&
               adjustment.contact_count == 2 &&
               std::fabs(adjustment.position[2] + 2.51f) < 0.0001f &&
               adjustment.position[1] == 0.0f,
           "circle response uses the type-1 result in ordered traversal");
    circle.collision_flags = 3u;
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment) && adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "type-1 path takes priority when both collision bits are set");
    circle.collision_flags = 1u;
    circle.radius = -0.5f;
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               &circle, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment),
           "invalid circle radius is rejected");
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               nullptr, 1, 0, 1, prior, proposed, 1.0f, 0u, 4u,
               &adjustment),
           "missing nonempty object list is rejected");
    expect(!awl::resolve_type1_first_dynamic_object_pass(
               &object, 1, 0, 1, prior, proposed, -1.0f, 0u, 4u,
               &adjustment),
           "negative moving radius is rejected");
}

void test_later_dynamic_object_pass() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject first;
    first.identity = 1;
    first.enabled = true;
    first.category = 1;
    first.collision_flags = 2u;
    first.data = bytes.data();
    first.size = bytes.size();
    first.world_to_object = identity;
    first.object_to_world = identity;
    first.center_local = {5.0f, 0.0f, 0.0f};
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};
    awl::CollisionDynamicPassAdjustment first_result;
    expect(awl::resolve_type1_first_dynamic_object_pass(
               &first, 1, 0, 1, prior, proposed, 1.0f, 0u, 0x67u,
               &first_result) && first_result.contact &&
               first_result.resolver_contact_bit == 4u,
           "movement flags enter the pre-terrain object pass");

    awl::CollisionDynamicPassObject later;
    later.identity = 1;
    later.enabled = true;
    later.category = 1;
    later.collision_flags = 1u;
    later.center_world = {5.0f, 100.0f, -1.0f};
    later.radius = 0.5f;
    std::array<float, 3> after_terrain = first_result.position;
    after_terrain[1] = 4.5f;
    awl::CollisionDynamicPassAdjustment later_result;
    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               first_result.contact_flags_after, 0x67u,
               &later_result) && later_result.contact &&
               later_result.queried_objects == 1 &&
               later_result.contact_count == 1 &&
               later_result.resolver_contact_bit == 2u &&
               later_result.contact_flags_after == 1u &&
               std::fabs(later_result.position[2] + 2.51f) < 0.0001f &&
               later_result.position[1] == after_terrain[1],
           "later pass consumes the post-terrain candidate and reports bit two");

    later.center_world[2] = 100.0f;
    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               0x21u, 0x67u, &later_result) && !later_result.contact &&
               later_result.contact_flags_after == 0x20u &&
               later_result.resolver_contact_bit == 0u &&
               later_result.position == after_terrain,
           "later pass clears inherited contact bit one before traversal");

    expect(awl::resolve_type1_later_dynamic_object_pass(
               &first, 1, 1, prior, proposed, 1.0f,
               1u, 0x67u, &later_result) && later_result.contact &&
               later_result.resolver_contact_bit == 2u &&
               std::fabs(later_result.position[2] + 1.01f) < 0.0001f,
           "later type-1 contact does not inherit first-pass reversion");

    awl::CollisionDynamicPassObject null_entry = later;
    null_entry.identity = 0;
    null_entry.center_world = {5.0f, 0.0f, -1.0f};
    const std::array<awl::CollisionDynamicPassObject, 2> entries{
        null_entry, later};
    expect(awl::resolve_type1_later_dynamic_object_pass(
               entries.data(), entries.size(), 1, prior, after_terrain,
               1.0f, 0u, 0x67u, &later_result) && !later_result.contact &&
               later_result.queried_objects == 1 &&
               later_result.position == after_terrain,
           "later list skips null object entries");

    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               0x21u, 4u, &later_result) && !later_result.contact &&
               later_result.queried_objects == 0 &&
               later_result.contact_flags_after == 0x21u,
           "later pass does not clear flags when its gate is disabled");

    later.center_world = {5.0f, 0.0f, -1.0f};
    expect(awl::resolve_type1_later_dynamic_object_pass(
               &later, 1, 1, prior, after_terrain, 1.0f,
               0u, 0x12u, &later_result) && later_result.contact &&
               later_result.resolver_contact_bit == 2u,
           "first-pass alternate bit does not block the later pass");
}

void test_third_dynamic_object_pass() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject type1;
    type1.identity = 1;
    type1.enabled = true;
    type1.category = 1;
    type1.collision_flags = 2u;
    type1.data = bytes.data();
    type1.size = bytes.size();
    type1.world_to_object = identity;
    type1.object_to_world = identity;
    type1.center_local = {5.0f, 0.0f, 0.0f};
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.5f};
    awl::CollisionDynamicPassAdjustment result;
    expect(awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 0, 0, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result) && result.contact && result.queried_objects == 1 &&
               result.contact_count == 1 &&
               result.contact_flags_after == 5u &&
               result.resolver_contact_bit == 8u &&
               std::fabs(result.position[2] + 1.01f) < 0.0001f,
           "third-list type-1 contact carries flags and reports resolver bit eight");

    awl::CollisionDynamicPassObject self = type1;
    self.identity = 99;
    awl::CollisionDynamicPassObject disabled = type1;
    disabled.identity = 2;
    disabled.enabled = false;
    awl::CollisionDynamicPassObject wrong_category = type1;
    wrong_category.identity = 3;
    wrong_category.category = 7;
    awl::CollisionDynamicPassObject circle = type1;
    circle.identity = 4;
    circle.collision_flags = 1u;
    circle.center_world = {5.0f, 0.0f, 0.0f};
    circle.radius = 0.5f;
    const std::array<awl::CollisionDynamicPassObject, 5> ordered{
        self, disabled, wrong_category, type1, circle};
    expect(awl::resolve_type1_third_dynamic_object_pass(
               ordered.data(), ordered.size(), 99, 0, 1, prior, proposed,
               1.0f, 4u, 0x8u, &result) && result.contact &&
               result.queried_objects == 2 && result.contact_count == 2 &&
               std::fabs(result.position[2] + 1.51f) < 0.0001f,
           "third-list traversal filters entries and gives each contact the prior response");
    expect(awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 0, 2u, 1, prior, proposed, 1.0f, 4u, 0u,
               &result) && !result.contact && result.position == proposed &&
               result.queried_objects == 0,
           "disabled third-list gate leaves the proposal untouched");
    expect(awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 0, 2u, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result) && result.contact &&
               result.resolver_contact_bit == 8u,
           "an absent source selects the ordinary third-list branch");
    expect(!awl::resolve_type1_third_dynamic_object_pass(
               &type1, 1, 99, 2u, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result) && result.position == std::array<float, 3>{},
           "source flag two selects the still-unsupported alternate branch");
    expect(!awl::resolve_type1_third_dynamic_object_pass(
               nullptr, 1, 0, 0, 1, prior, proposed, 1.0f, 4u, 0x8u,
               &result),
           "a nonempty third list requires object data");
}

void test_category1_static_and_movement_candidate() {
    std::vector<uint8_t> terrain = make_sample_leaf();
    const std::array<float, 3> prior{2.0f, 7.0f, 2.0f};
    const std::array<float, 3> proposed{2.0f, 50.0f, 2.0f};
    awl::CollisionCategory1StaticFlags flags;
    expect(awl::category1_static_surface_mask(0x67u, flags) == 0xC1u,
           "movement flags and clear runtime bytes produce the traced mask");
    flags.state_299a4 = true;
    flags.state_299a5 = true;
    flags.state_299a6 = true;
    flags.state_299a8 = true;
    flags.state_299a9 = true;
    flags.state_299af = true;
    flags.secondary_3f3 = true;
    flags.secondary_3f1 = true;
    expect(awl::category1_static_surface_mask(0x340u, flags) == 0x1FBFu,
           "each observed runtime byte contributes its DOL surface-mask bit");

    awl::CollisionCategory1StaticAdjustment static_result;
    expect(awl::resolve_type1_category1_static_contact(
               nullptr, 0, flags, 0x67u, prior, proposed, 0.3f, 0u,
               &static_result) && !static_result.slot_present &&
               !static_result.contact && static_result.position == proposed,
           "absent slot two copies the proposal without static contact");
    expect(!awl::resolve_type1_category1_static_contact(
               nullptr, terrain.size(), flags, 0x67u, prior, proposed,
               0.3f, 0u, &static_result),
           "nonempty missing static asset is rejected");
    std::vector<uint8_t> mode_zero_static = terrain;
    mode_zero_static[6] = 0;
    expect(awl::resolve_type1_category1_static_contact(
               mode_zero_static.data(), mode_zero_static.size(),
               flags, 0x67u, prior, proposed, 0.3f, 4u,
               &static_result) && static_result.slot_present &&
               !static_result.contact &&
               static_result.narrow_phase.used_containment_shortcut &&
               static_result.position == proposed,
           "slot-two mode zero uses the type-1 containment path");
    mode_zero_static[6] = 2;
    expect(!awl::resolve_type1_category1_static_contact(
               mode_zero_static.data(), mode_zero_static.size(),
               flags, 0x67u, prior, proposed, 0.3f, 4u,
               &static_result) &&
               static_result.position == std::array<float, 3>{},
           "unverified slot-two mode is rejected");

    awl::CollisionCategory1MovementQuery query;
    query.terrain_data = terrain.data();
    query.terrain_size = terrain.size();
    query.moving_radius = 0.3f;
    awl::CollisionCategory1MovementAdjustment result;
    expect(awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               result.resolver_contact_bits == 0u &&
               !result.final_height_resampled &&
               !result.static_contact.slot_present,
           "clear movement candidate composes terrain and absent object stages");

    awl::CollisionDynamicPassObject first;
    first.identity = 1;
    first.enabled = true;
    first.category = 1;
    first.collision_flags = 1u;
    first.center_world = {2.0f, 0.0f, 2.0f};
    first.radius = 0.5f;
    awl::CollisionDynamicPassObject later = first;
    later.identity = 2;
    later.center_world[2] = 2.8f;
    query.first_objects = &first;
    query.first_object_count = 1;
    query.later_objects = &later;
    query.later_object_count = 1;
    expect(awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.first_pass.contact && result.later_pass.contact &&
               result.resolver_contact_bits == 6u &&
               result.final_height_resampled &&
               std::fabs(result.position[2] - 3.61f) < 0.0002f,
           "two object lists run around terrain and trigger final height lookup");
    awl::CollisionSurfaceSample final_surface;
    expect(awl::sample_type1_collision_surface(
               terrain.data(), terrain.size(), result.position[0],
               result.position[2], &final_surface) &&
               std::fabs(result.position[1] - final_surface.height) < 0.0001f,
           "final candidate height matches an independent surface query");

    query.first_objects = nullptr;
    query.first_object_count = 0;
    query.later_objects = nullptr;
    query.later_object_count = 0;
    query.static_data = terrain.data();
    query.static_size = terrain.size();
    query.static_flags.state_299a9 = true;
    const std::array<float, 3> static_prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> static_proposed{5.0f, 50.0f, 0.5f};
    expect(awl::resolve_type1_category1_movement_candidate(
               query, static_prior, static_proposed, &result) &&
               result.static_contact.slot_present &&
               result.static_contact.contact &&
               result.resolver_contact_bits == 1u &&
               result.final_height_resampled &&
               std::fabs(result.position[2] + 0.31f) < 0.0001f,
           "slot-two static contact sets bit one and resamples final height");
    awl::CollisionEdgeSample final_edge;
    expect(awl::project_type1_collision_to_edge(
               terrain.data(), terrain.size(), result.position[0],
               result.position[2], &final_edge) &&
               std::fabs(result.position[1] - final_edge.position[1]) < 0.0001f,
           "static response outside terrain gets edge-projected final height");

    mode_zero_static[6] = 0;
    query.static_data = mode_zero_static.data();
    query.static_size = mode_zero_static.size();
    expect(awl::resolve_type1_category1_movement_candidate(
               query, static_prior, static_proposed, &result) &&
               result.static_contact.contact &&
               result.resolver_contact_bits == 1u &&
               result.final_height_resampled &&
               std::fabs(result.position[2] + 0.31f) < 0.0001f,
           "mode-zero slot-two wall contact composes and resamples height");
    query.static_data = terrain.data();
    query.static_size = terrain.size();

    const std::array<float, 3> inside_static_prior{5.0f, 7.0f, 2.0f};
    expect(awl::resolve_type1_category1_movement_candidate(
               query, inside_static_prior, static_proposed, &result) &&
               !result.static_contact.contact &&
               result.static_contact.narrow_phase.used_containment_shortcut &&
               result.resolver_contact_bits == 0u &&
               !result.final_height_resampled,
           "player contact flag four keeps a matching prior surface clear");

    terrain[6] = 0;
    expect(!awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.position == std::array<float, 3>{},
           "unsupported terrain mode rejects the composed candidate");
    terrain[6] = 1;
    expect(!awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, nullptr),
           "null composed result is rejected");
    query.terrain_data = terrain.data();
    query.moving_radius = -0.3f;
    expect(!awl::resolve_type1_category1_movement_candidate(
               query, prior, proposed, &result) &&
               result.position == std::array<float, 3>{},
           "invalid moving radius rejects the full candidate");
}

void test_world_map_packed_saved_conditions() {
    const std::array<uint8_t, 2> three_bit_values{0xD1, 0x6A};
    awl::WorldMapPackedSavedValues packed{
        three_bit_values.data(), three_bit_values.size(), 3};
    uint32_t value = 99;
    expect(awl::read_world_map_packed_saved_value(packed, 0, &value) &&
               value == 1 &&
               awl::read_world_map_packed_saved_value(packed, 1, &value) &&
               value == 2 &&
               awl::read_world_map_packed_saved_value(packed, 2, &value) &&
               value == 3 &&
               awl::read_world_map_packed_saved_value(packed, 3, &value) &&
               value == 5 &&
               awl::read_world_map_packed_saved_value(packed, 4, &value) &&
               value == 6,
           "packed values use low-bit-first order and cross byte boundaries");
    expect(!awl::read_world_map_packed_saved_value(packed, 5, &value) &&
               value == 0,
           "incomplete packed element is rejected and output cleared");
    packed.width_bits = 33;
    expect(!awl::read_world_map_packed_saved_value(packed, 0, &value) &&
               !awl::read_world_map_packed_saved_value(packed, 0, nullptr),
           "unsupported packed width and null output are rejected");
    packed.width_bits = 3;
    expect(!awl::read_world_map_packed_saved_value(
               packed, std::numeric_limits<size_t>::max(), &value),
           "packed index arithmetic cannot wrap");
    const std::array<uint8_t, 4> full_width{0x12, 0x34, 0x56, 0x78};
    const awl::WorldMapPackedSavedValues full{
        full_width.data(), full_width.size(), 32};
    expect(awl::read_world_map_packed_saved_value(full, 0, &value) &&
               value == 0x78563412u,
           "a 32-bit packed element preserves its low-bit-first byte order");
    packed.width_bits = 0;
    expect(!awl::read_world_map_packed_saved_value(packed, 0, &value),
           "zero-width packed input is unsupported");

    std::array<uint8_t, 47> later_saved{};
    later_saved[38] = 0x20; // Index 0x135, bit 5.
    later_saved[46] = 0x01; // Index 0x170, bit 0.
    const std::array<uint8_t, 2> room_saved{0xC1, 0xC7};
    const awl::WorldMapPackedSavedValues later{
        later_saved.data(), later_saved.size(), 1};
    const awl::WorldMapPackedSavedValues room{
        room_saved.data(), room_saved.size(), 1};
    awl::WorldMapRoomConditionInputs inputs;
    inputs.phase_index = 2;
    inputs.state_299a7 = 1;
    expect(awl::decode_world_map_room_saved_conditions(later, room, &inputs) &&
               inputs.phase_index == 2 && inputs.state_299a7 == 1 &&
               inputs.value_120a4_135_nonzero &&
               inputs.value_120a4_170_nonzero,
           "saved-condition decoding preserves other caller-supplied state");
    awl::WorldMapRoomStaticConditions conditions;
    expect(awl::evaluate_world_map_room_static_conditions(inputs, &conditions) &&
               conditions == awl::WorldMapRoomStaticConditions{
                   true, true, true, true, false, true, true,
                   true, true, true, true, true, true, true},
           "packed table bits feed all ten traced room predicates");
    const auto before = inputs;
    const awl::WorldMapPackedSavedValues short_room{room_saved.data(), 1, 1};
    expect(!awl::decode_world_map_room_saved_conditions(
               later, short_room, &inputs) &&
               inputs.values_14ad4_nonzero == before.values_14ad4_nonzero &&
               inputs.value_120a4_135_nonzero == before.value_120a4_135_nonzero &&
               !awl::decode_world_map_room_saved_conditions(later, room,
                                                             nullptr),
           "truncated tables fail without partially changing condition inputs");
}

void test_world_map_room_condition_evaluator() {
    awl::WorldMapRoomConditionInputs inputs;
    awl::WorldMapRoomStaticConditions conditions;
    expect(awl::evaluate_world_map_room_static_conditions(inputs, &conditions) &&
               conditions == awl::WorldMapRoomStaticConditions{
                   true, false, true, true, false, false, false,
                   false, false, false, false, false, false, false},
           "phase zero and clear state produce the traced room predicates");
    inputs.phase_index = 2;
    inputs.state_299af = 1;
    inputs.state_299a7 = 2;
    inputs.counters_118bc_to_118be = {-1, 0, 0};
    inputs.value_120a4_135_nonzero = true;
    expect(awl::evaluate_world_map_room_static_conditions(inputs, &conditions) &&
               !conditions[0] && conditions[1] && !conditions[2] &&
               !conditions[3] && !conditions[4] && conditions[5],
           "later phase, nonzero state byte, and signed nonpositive counters");
    inputs.counters_118bc_to_118be[2] = 1;
    inputs.value_120a4_170_nonzero = true;
    constexpr std::array<size_t, 8> value_indices{
        0, 6, 7, 8, 9, 10, 14, 15};
    for (size_t row = 0; row < value_indices.size(); ++row) {
        inputs.values_14ad4_nonzero[value_indices[row]] = true;
        expect(awl::evaluate_world_map_room_static_conditions(
                   inputs, &conditions) &&
                   conditions[6 + row] && conditions[2] &&
                   conditions[3] && conditions[4],
               "each indexed saved value enables its ordered room predicate");
        inputs.values_14ad4_nonzero[value_indices[row]] = false;
    }
    inputs.phase_index = 6;
    conditions.fill(true);
    expect(!awl::evaluate_world_map_room_static_conditions(inputs,
                                                           &conditions) &&
               conditions == awl::WorldMapRoomStaticConditions{} &&
               !awl::evaluate_world_map_room_static_conditions(inputs,
                                                                nullptr),
           "unsupported phase and null condition output fail cleanly");
}

void test_world_map_room_static_stage() {
    using Status = awl::WorldMapRoomCollisionStatus;
    constexpr std::array<uint32_t, 14> expected_ids{
        0xA1u, 0x9Eu, 0xECu, 0xEBu, 0x8Du, 0x8Au, 0x11u,
        0x17u, 0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Fu, 0x20u};
    expect(awl::world_map_room_static_condition_ids() == expected_ids,
           "room static predicate order matches the DOL table");
    awl::WorldMapRoomStaticConditions conditions{};
    expect(awl::world_map_room_static_surface_mask(conditions, 0) == 0u &&
               awl::world_map_room_static_surface_mask(conditions, 0x200u) ==
                   0x8000u,
           "room static mask starts empty and carries the resolver high bit");
    conditions.fill(true);
    expect(awl::world_map_room_static_surface_mask(conditions, 0) ==
               0x3FFFu &&
               awl::world_map_room_static_surface_mask(conditions, 0x400u) ==
                   0xBFFFu,
           "all 14 DOL condition rows and the resolver high bit compose");
    constexpr std::array<uint32_t, 14> expected_bits{
        0x0001u, 0x0002u, 0x0004u, 0x2000u, 0x1000u,
        0x0800u, 0x0400u, 0x0200u, 0x0040u, 0x0080u,
        0x0010u, 0x0100u, 0x0008u, 0x0020u};
    for (size_t row = 0; row < expected_bits.size(); ++row) {
        conditions = {};
        conditions[row] = true;
        expect(awl::world_map_room_static_surface_mask(conditions, 0) ==
                   expected_bits[row],
               "each room condition row selects its verified DOL bit");
    }
    conditions = {};
    conditions[13] = true; // DOL row 13: condition 0x20 -> surface bit 0x20.
    expect(awl::world_map_room_static_surface_mask(conditions, 0) == 0x20u,
           "the final DOL condition row selects surface bit 0x20");

    std::vector<uint8_t> col = make_sample_leaf();
    col[6] = 0;
    awl::WorldMapCollisionArchive archive;
    awl::WorldMapCollisionRecordView record;
    expect(archive.parse(make_record_arc(col)) && archive.lookup(0, &record),
           "sample room COL is owned by a validated ARC record");
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    awl::WorldMapRoomStaticAdjustment result;
    expect(awl::resolve_type1_room_static_contact(
               40, Status::Found, &record, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               result.record_present && result.contact &&
               result.surface_mask == 0x20u &&
               result.narrow_phase.first_edge_contact &&
               std::fabs(result.position[2] + 1.01f) < 0.0001f,
           "mapped room category applies the selected type-1 edge response");
    expect(awl::resolve_type1_room_static_contact(
               40, Status::Found, &record, conditions, 1u, 1u, 0u,
               prior, proposed, 1.0f, &result) &&
               result.contact && !result.reverted_horizontal_to_prior &&
               std::fabs(result.position[2] + 1.01f) < 0.0001f,
           "room static query clears carried bit one before narrow phase");
    expect(awl::resolve_type1_room_static_contact(
               40, Status::Found, &record, conditions, 1u, 0u, 5u,
               prior, proposed, 1.0f, &result) &&
               result.contact && result.reverted_horizontal_to_prior &&
               result.position[0] == prior[0] &&
               result.position[2] == prior[2],
           "prior contact bits one and four restore horizontal position");
    expect(awl::resolve_type1_room_static_contact(
               4, Status::NoMapping, nullptr, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !result.record_present && !result.contact &&
               result.position == proposed,
           "unmapped room category copies the proposal");
    expect(awl::resolve_type1_room_static_contact(
               40, Status::MissingRecord, nullptr, conditions, 0u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !result.record_present && result.position == proposed,
           "disabled room static gate does not query the record");
    expect(!awl::resolve_type1_room_static_contact(
               40, Status::MissingRecord, nullptr, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   40, Status::Found, nullptr, conditions, 1u, 0u, 0u,
                   prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   1, Status::Found, &record, conditions, 1u, 0u, 0u,
                   prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   40, Status::Found, &record, conditions, 1u, 0u, 0u,
                   prior, proposed, -1.0f, &result),
           "missing records, category one, and invalid radius are rejected");
    expect(!awl::resolve_type1_room_static_contact(
               4, Status::NoMapping, &record, conditions, 1u, 0u, 0u,
               prior, proposed, 1.0f, &result) &&
               !awl::resolve_type1_room_static_contact(
                   40, Status::Found, &record, conditions, 1u, 0u, 0u,
                   prior, proposed, 1.0f, nullptr),
           "an inconsistent lookup result and null output are rejected");
}

void test_world_map_directional_contact_search() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    put_be16(bytes, 8 + 0x34, 1u);
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::WorldMapContactObject object;
    object.enabled = true;
    object.category = 1;
    object.collision_flags = 2u;
    object.data = bytes.data();
    object.size = bytes.size();
    object.contact_query.world_to_object = identity;
    object.contact_query.object_to_world = identity;
    object.contact_query.object_center_local = {5.0f, 0.0f, 0.0f};
    object.contact_query.object_radius = 10.0f;
    object.world_position = {20.0f, 30.0f, 40.0f};
    object.heading_axis = {0.0f, 0.0f, -1.0f};
    object.metadata = 42u;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 9.0f, -0.1f};
    const std::array<float, 3> fallback_axis{0.0f, 0.0f, 1.0f};
    awl::WorldMapContactResult result;
    expect(awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               result.matched && result.direction_code == 0 &&
               result.metadata == 42u &&
               result.world_position == object.world_position &&
               result.heading_axis == object.heading_axis &&
               result.queried_objects == 1 &&
               result.contacting_objects == 1,
           "first qualifying type-1 edge returns its direction and metadata");

    const std::array<float, 3> vertical_only{5.0f, 20.0f, -2.0f};
    expect(awl::query_world_map_directional_contact(
               &object, 1, 1, prior, vertical_only, fallback_axis,
               &result) && !result.matched && result.direction_code == 7 &&
               result.queried_objects == 0 &&
               result.world_position == vertical_only &&
               result.heading_axis == fallback_axis,
           "zero horizontal motion returns the caller's fallback without search");

    awl::WorldMapContactObject disabled = object;
    disabled.enabled = false;
    awl::WorldMapContactObject wrong_category = object;
    wrong_category.category = 7;
    awl::WorldMapContactObject circle_only = object;
    circle_only.collision_flags = 1u;
    const std::array<awl::WorldMapContactObject, 4> filtered{
        disabled, wrong_category, circle_only, object};
    expect(awl::query_world_map_directional_contact(
               filtered.data(), filtered.size(), 1, prior, proposed,
               fallback_axis, &result) && result.matched &&
               result.queried_objects == 1 &&
               result.contacting_objects == 1,
           "search skips disabled, wrong-category, and circle-only objects");

    awl::WorldMapContactObject wrong_heading = object;
    wrong_heading.heading_axis = {0.0f, 0.0f, 1.0f};
    wrong_heading.metadata = 99u;
    const std::array<awl::WorldMapContactObject, 2> ordered{
        wrong_heading, object};
    expect(awl::query_world_map_directional_contact(
               ordered.data(), ordered.size(), 1, prior, proposed,
               fallback_axis, &result) && result.matched &&
               result.metadata == 42u && result.queried_objects == 2 &&
               result.contacting_objects == 2,
           "a contact without matching heading leaves the next object eligible");

    expect(awl::query_world_map_directional_contact(
               &wrong_heading, 1, 1, prior, proposed, fallback_axis,
               &result) && !result.matched && result.direction_code == 7 &&
               result.metadata == 0u && result.world_position == proposed &&
               result.heading_axis == fallback_axis,
           "no qualifying heading uses the caller's code-seven fallback");

    bytes[6] = 0;
    expect(awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               result.matched && result.direction_code == 0 &&
               result.metadata == 42u,
           "type-1 mode zero supports the directional contact path");
    bytes[6] = 2;
    expect(!awl::query_world_map_directional_contact(
               &object, 1, 1, prior, proposed, fallback_axis, &result) &&
               !result.matched && result.world_position ==
                   std::array<float, 3>{},
           "unverified directional-contact mode is rejected");
    bytes[6] = 1;
    expect(!awl::query_world_map_directional_contact(
               nullptr, 1, 1, prior, proposed, fallback_axis, &result),
           "missing nonempty object list is rejected");
}

uint32_t camera_float_bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void test_world_map_camera_followup() {
    awl::WorldMapCameraFollowupState previous;
    previous.position = {1.0f, 2.0f, 3.0f};
    previous.field_18 = 9.0f;
    previous.yaw = 0.75f;
    previous.bounds_min = {10.0f, 11.0f, 12.0f};
    previous.bounds_max = {20.0f, 21.0f, 22.0f};
    previous.flag_98 = true;
    awl::WorldMapCameraFollowup result;

    struct RegionCase {
        std::array<float, 3> lower;
        std::array<float, 3> upper;
        float yaw;
        std::array<float, 3> bounds_min;
        std::array<float, 3> bounds_max;
    };
    // Rectangle limits and seven-float profile rows from FUN_8001D0E8
    // and 0x8029FE10 in the verified DOL, independent of the helper table.
    const RegionCase regions[] = {
        {{171.0f, -7.0f, 111.0f}, {180.0f, 50.0f, 118.0f},
         0.0f, {173.5f, 0.0f, 120.5f}, {177.6f, 100.0f, 122.3f}},
        {{192.0f, -7.0f, 111.0f}, {199.0f, 50.0f, 120.0f},
         0.0f, {194.5f, 0.0f, 119.6f}, {196.6f, 100.0f, 123.6f}},
        {{216.0f, -7.0f, 112.0f}, {229.0f, 50.0f, 130.0f},
         4.71238899f, {211.1f, 0.0f, 115.2f},
         {219.6f, 100.0f, 125.8f}},
    };
    for (int32_t index = 0; index < 3; ++index) {
        const RegionCase& region = regions[index];
        for (const std::array<float, 3>& corner :
             {region.lower, region.upper}) {
            expect(awl::plan_world_map_camera_followup(
                       previous, 1, corner, 0, 127, &result) &&
                       result.region_index == index &&
                       result.state.position == corner &&
                       camera_float_bits(result.state.field_18) ==
                           0xBF8192C0u &&
                       result.state.yaw == region.yaw &&
                       result.state.bounds_min == region.bounds_min &&
                       result.state.bounds_max == region.bounds_max &&
                       !result.state.flag_98 && result.state.flag_99 &&
                       !result.yaw_adjustment_called &&
                       !result.yaw_adjustment_written,
                   "inclusive camera region endpoints select the DOL profile");
        }
        const std::array<float, 3> below_x{
            region.lower[0] - 0.01f, 0.0f, region.lower[2]};
        const std::array<float, 3> above_z{
            region.upper[0], 0.0f, region.upper[2] + 0.01f};
        for (const std::array<float, 3>& outside : {below_x, above_z}) {
            expect(awl::plan_world_map_camera_followup(
                       previous, 1, outside, 1, 127, &result) &&
                       result.region_index == -1 &&
                       camera_float_bits(result.state.field_18) ==
                           0xBE17E9D8u &&
                       result.state.yaw == previous.yaw &&
                       result.state.bounds_min == previous.bounds_min &&
                       result.state.bounds_max == previous.bounds_max &&
                       result.state.flag_98 && !result.state.flag_99 &&
                       !result.yaw_adjustment_called &&
                       !result.yaw_adjustment_written,
                   "camera region exterior preserves yaw and bounds");
        }
    }
    const std::array<float, 3> outside{181.0f, 17.0f, 111.0f};
    expect(awl::plan_world_map_camera_followup(
               previous, 1, outside, 0, 64, &result) &&
               result.region_index == -1 && result.yaw_adjustment_called &&
               result.yaw_adjustment_written &&
               result.state.position == outside &&
               result.state.yaw == 0.8125f,
           "outside region squares positive signed PAD input and adds yaw");
    expect(awl::plan_world_map_camera_followup(
               previous, 1, outside, 0, -64, &result) &&
               result.yaw_adjustment_called &&
               result.yaw_adjustment_written &&
               result.state.yaw == 0.6875f,
           "negative signed PAD input subtracts the squared yaw amount");
    expect(awl::plan_world_map_camera_followup(
               previous, 1, outside, 0, -128, &result) &&
               result.state.yaw == 0.5f,
           "minimum signed PAD byte produces a negative 0.25-radian increment");
    awl::WorldMapCameraFollowupState disabled_yaw = previous;
    disabled_yaw.flag_98 = false;
    expect(awl::plan_world_map_camera_followup(
               disabled_yaw, 1, outside, 0, 64, &result) &&
               result.yaw_adjustment_called &&
               !result.yaw_adjustment_written &&
               result.state.yaw == disabled_yaw.yaw &&
               result.state.flag_98,
           "yaw adjustment uses the prior flag before the outside branch enables it");
    disabled_yaw = result.state;
    expect(awl::plan_world_map_camera_followup(
               disabled_yaw, 1, outside, 0, 64, &result) &&
               result.yaw_adjustment_written &&
               result.state.yaw == 0.8125f,
           "next outside frame applies yaw after the flag was enabled");
    expect(awl::plan_world_map_camera_followup(
               previous, 2, regions[0].lower, 0, 64, &result) &&
               result.region_index == -1 && !result.yaw_adjustment_called &&
               result.state.position == regions[0].lower &&
               result.state.field_18 == previous.field_18 &&
               result.state.yaw == previous.yaw &&
               result.state.bounds_min == previous.bounds_min &&
               result.state.bounds_max == previous.bounds_max &&
               result.state.flag_98 == previous.flag_98 &&
               result.state.flag_99 == previous.flag_99,
           "other camera categories copy position without profile writes");
    const awl::WorldMapCameraFollowup saved = result;
    auto bad_position = outside;
    bad_position[1] = std::numeric_limits<float>::infinity();
    expect(!awl::plan_world_map_camera_followup(
               previous, 1, bad_position, 0, 64, &result) &&
               result.state.position == saved.state.position &&
               result.region_index == saved.region_index,
           "nonfinite camera position is rejected without output mutation");
    expect(!awl::plan_world_map_camera_followup(
               previous, 1, outside, 0, 64, nullptr),
           "missing camera output is rejected");
}

void test_world_map_camera_target() {
    awl::WorldMapCameraTargetQuery query;
    query.camera.position = {2.0f, 3.0f, 4.0f};
    query.origin_offset_0c = {1.0f, -2.0f, 3.0f};
    query.distance_30 = 10.0f;
    awl::WorldMapCameraTarget target;
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               target.raw == std::array<float, 3>{3.0f, 1.0f, 17.0f} &&
               target.bounded == target.raw && !target.clamped,
           "zero-angle target adds camera position, offset, and forward distance");

    constexpr float kRightAngle = 1.57079632679f;
    query.camera.yaw = kRightAngle;
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               std::fabs(target.raw[0] - 13.0f) < 0.00001f &&
               target.raw[1] == 1.0f &&
               std::fabs(target.raw[2] - 7.0f) < 0.00001f,
           "positive yaw rotates forward distance toward positive X");
    query.camera.yaw = 0.0f;
    query.pitch_offset_8c = kRightAngle;
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               target.raw[0] == 3.0f &&
               std::fabs(target.raw[1] + 9.0f) < 0.00001f &&
               std::fabs(target.raw[2] - 7.0f) < 0.00001f,
           "positive pitch rotates forward distance toward negative Y");
    query.camera.field_18 = -0.5f;
    query.pitch_offset_8c = 0.5f;
    query.camera.yaw = -0.5f;
    query.yaw_offset_90 = 0.5f;
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               target.raw == std::array<float, 3>{3.0f, 1.0f, 17.0f},
           "camera base angles and supplied angle offsets combine before rotation");
    query.camera.field_18 = 0.0f;
    query.pitch_offset_8c = 0.0f;
    query.camera.yaw = 0.0f;
    query.yaw_offset_90 = 0.0f;
    query.camera.flag_99 = true;
    query.camera.bounds_min = {0.0f, 2.0f, 8.0f};
    query.camera.bounds_max = {4.0f, 5.0f, 12.0f};
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               target.raw == std::array<float, 3>{3.0f, 1.0f, 17.0f} &&
               target.bounded ==
                   std::array<float, 3>{3.0f, 2.0f, 12.0f} &&
               target.clamped,
           "camera bounds clamp lower Y and upper Z after target rotation");
    query.camera.bounds_min[0] = 5.0f;
    query.camera.bounds_max[0] = 4.0f;
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               target.bounded[0] == 4.0f,
           "ordered lower-then-upper comparison is retained for supplied bounds");

    awl::WorldMapCameraFollowupState before_region;
    awl::WorldMapCameraFollowup region;
    const std::array<float, 3> region_position{175.0f, 0.0f, 115.0f};
    expect(awl::plan_world_map_camera_followup(
               before_region, 1, region_position, 1, 0, &region),
           "camera region profile is available to the target calculation");
    query = {};
    query.camera = region.state;
    query.distance_30 = 20.0f;
    expect(awl::calculate_world_map_camera_target(query, &target) &&
               target.raw[2] > region.state.bounds_max[2] &&
               target.bounded[0] == 175.0f &&
               target.bounded[2] == region.state.bounds_max[2] &&
               target.clamped,
           "region profile bounds constrain its rotated camera target");
    const awl::WorldMapCameraTarget saved = target;
    query.distance_30 = std::numeric_limits<float>::infinity();
    expect(!awl::calculate_world_map_camera_target(query, &target) &&
               target.raw == saved.raw && target.bounded == saved.bounded &&
               target.clamped == saved.clamped,
           "nonfinite camera distance fails without changing output");
    expect(!awl::calculate_world_map_camera_target(query, nullptr),
           "missing camera target output is rejected");
}

void test_world_map_camera_view() {
    awl::WorldMapCameraViewQuery query;
    query.target.distance_30 = 2.0f;
    query.up_vector_24 = {0.0f, 1.0f, 0.0f};
    awl::WorldMapCameraView view;
    expect(awl::calculate_world_map_camera_view(query, &view) &&
               view.target.bounded ==
                   std::array<float, 3>{0.0f, 0.0f, 2.0f} &&
               view.second_point ==
                   std::array<float, 3>{0.0f, 0.0f, -2.0f} &&
               view.rotated_up ==
                   std::array<float, 3>{0.0f, 1.0f, 0.0f} &&
               view.matrix_50 == std::array<float, 12>{
                   1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, -2.0f},
           "zero-angle camera view produces identity axes and target translation");

    constexpr float kRightAngle = 1.57079632679f;
    query.target.camera.yaw = kRightAngle;
    expect(awl::calculate_world_map_camera_view(query, &view) &&
               std::fabs(view.target.bounded[0] - 2.0f) < 0.00001f &&
               std::fabs(view.second_point[0] + 2.0f) < 0.00001f &&
               std::fabs(view.matrix_50[2] + 1.0f) < 0.00001f &&
               std::fabs(view.matrix_50[8] - 1.0f) < 0.00001f &&
               std::fabs(view.matrix_50[11] + 2.0f) < 0.00001f,
           "positive yaw turns the view forward toward positive X");
    query.target.camera.yaw = 0.0f;
    query.yaw_offset_48 = kRightAngle;
    expect(awl::calculate_world_map_camera_view(query, &view) &&
               std::fabs(view.target.bounded[2] - 2.0f) < 0.00001f &&
               std::fabs(view.matrix_50[8] - 1.0f) < 0.00001f &&
               std::fabs(view.matrix_50[3] - 2.0f) < 0.00001f,
           "second-point yaw offset changes view while preserving base target");
    query.yaw_offset_48 = 0.0f;
    query.pitch_offset_44 = 0.78539816339f;
    expect(awl::calculate_world_map_camera_view(query, &view) &&
               std::fabs(view.matrix_50[9] + 0.70710678f) < 0.00001f &&
               std::fabs(view.matrix_50[10] - 0.70710678f) < 0.00001f,
           "second-point pitch offset tilts the view forward axis");
    query.pitch_offset_44 = 0.0f;
    query.target.camera.field_18 = kRightAngle;
    expect(awl::calculate_world_map_camera_view(query, &view) &&
               std::fabs(view.rotated_up[2] - 1.0f) < 0.00001f &&
               std::fabs(view.matrix_50[9] + 1.0f) < 0.00001f,
           "base pitch rotates the supplied up vector and view forward axis");
    query.target.camera.field_18 = 0.0f;

    awl::WorldMapCameraFollowup region;
    awl::WorldMapCameraFollowupState before_region;
    expect(awl::plan_world_map_camera_followup(
               before_region, 1, {175.0f, 0.0f, 115.0f}, 1, 0, &region),
           "region profile is available to camera view");
    query.target.camera = region.state;
    query.target.distance_30 = 20.0f;
    expect(awl::calculate_world_map_camera_view(query, &view) &&
               view.target.clamped &&
               view.target.bounded[2] == region.state.bounds_max[2] &&
               std::isfinite(view.matrix_50[11]),
           "bounded region target feeds the completed view calculation");

    query = {};
    query.target.distance_30 = 0.0f;
    query.up_vector_24 = {0.0f, 1.0f, 0.0f};
    const awl::WorldMapCameraView saved = view;
    expect(!awl::calculate_world_map_camera_view(query, &view) &&
               view.matrix_50 == saved.matrix_50 &&
               view.target.bounded == saved.target.bounded,
           "zero camera distance rejects a degenerate view atomically");
    query.target.distance_30 = 2.0f;
    query.up_vector_24 = {0.0f, 0.0f, 1.0f};
    expect(!awl::calculate_world_map_camera_view(query, &view) &&
               view.matrix_50 == saved.matrix_50,
           "up vector parallel to forward rejects a degenerate view");
    expect(!awl::calculate_world_map_camera_view(query, nullptr),
           "missing camera view output is rejected");
}

void test_world_map_camera_initial_profile() {
    awl::WorldMapCameraInitialProfile profile;
    expect(awl::make_world_map_camera_initial_profile(&profile) &&
               profile.view_query.target.camera.position ==
                   std::array<float, 3>{0.0f, 0.0f, 0.0f} &&
               profile.view_query.target.origin_offset_0c ==
                   std::array<float, 3>{0.0f, 1.5f, 0.0f} &&
               camera_float_bits(profile.view_query.target.camera.field_18) ==
                   0xBE17E9D8u &&
               camera_float_bits(profile.view_query.target.camera.yaw) ==
                   0x4096CBE4u &&
               profile.view_query.up_vector_24 ==
                   std::array<float, 3>{0.0f, 1.0f, 0.0f} &&
               profile.view_query.target.distance_30 == 12.0f &&
               profile.view_query.target.camera.flag_98 &&
               !profile.view_query.target.camera.flag_99 &&
               profile.mode_104 == 0,
           "world-map constructor profile copies the verified camera fields");
    expect(camera_float_bits(profile.field_34) == 0x3FAAAAABu &&
               camera_float_bits(profile.field_38) == 0x41F1999Au &&
               camera_float_bits(profile.field_3c) == 0x3E99999Au &&
               camera_float_bits(profile.field_40) == 0x44800000u,
           "constructor preserves the four DOL projection inputs");
    expect(std::fabs(profile.initial_view.target.bounded[0] +
                     11.86819037f) < 0.00001f &&
               std::fabs(profile.initial_view.target.bounded[1] -
                         3.27371287f) < 0.00001f &&
               std::fabs(profile.initial_view.target.bounded[2]) < 0.00001f &&
               std::fabs(profile.initial_view.matrix_50[2] - 1.0f) <
                   0.00001f &&
               std::fabs(profile.initial_view.matrix_50[8] +
                         0.989015864f) < 0.00001f,
           "initial view faces the DOL profile direction before player setup");

    awl::WorldMapCameraFollowup followup;
    expect(awl::plan_world_map_camera_followup(
               profile.view_query.target.camera, 1,
               {120.0f, 6.0f, 168.0f}, 0, 64, &followup) &&
               followup.yaw_adjustment_called &&
               followup.yaw_adjustment_written &&
               std::fabs(followup.state.yaw -
                         (profile.view_query.target.camera.yaw + 0.0625f)) <
                   0.00001f,
           "constructor flag enables the first outside-region yaw write");
    expect(!awl::make_world_map_camera_initial_profile(nullptr),
           "missing initial profile output is rejected");
}

struct PlayerCameraHeightScript {
    int calls = 0;
    int fail_on_call = 0;
    std::array<std::array<float, 3>, 4> points{};
};

bool player_camera_height(const std::array<float, 3>& point,
                          float* height, void* context) {
    auto* script = static_cast<PlayerCameraHeightScript*>(context);
    if (script == nullptr || height == nullptr ||
        !std::isfinite(point[0]) || !std::isfinite(point[2])) {
        return false;
    }
    ++script->calls;
    if (script->calls <= 4) {
        script->points[script->calls - 1] = point;
    }
    if (script->calls == script->fail_on_call) {
        return false;
    }
    *height = 0.0f;
    return true;
}

void test_world_map_player_camera_placement() {
    awl::WorldMapCameraInitialProfile profile;
    expect(awl::make_world_map_camera_initial_profile(&profile),
           "player camera placement starts from the verified profile");
    awl::WorldMapPlayerCameraPlacementQuery query;
    query.initial_camera = profile.view_query;
    query.collision_category = 1;
    query.player_position = {175.0f, 0.0f, 115.0f};
    PlayerCameraHeightScript script;
    awl::WorldMapPlayerCameraPlacement result;
    expect(awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               result.first_followup.region_index == 0 &&
               !result.second_update_called && !result.second_yaw_written &&
               script.calls == 2 && result.final_camera.target.camera.yaw == 0.0f &&
               result.final_camera.target.camera.position == query.player_position,
           "region camera placement performs one two-height update");

    query.player_position = {120.0f, 0.0f, 168.0f};
    query.global_byte_3f1 = 1;
    query.scene_mode = 4;
    query.heading_x = 1.0f;
    query.heading_z = 0.0f;
    script = {};
    expect(awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               result.first_followup.region_index == -1 &&
               result.second_update_called && result.second_yaw_written &&
               script.calls == 4 &&
               script.points[0] != script.points[2] &&
               std::fabs(result.final_camera.target.camera.yaw -
                         1.57079632679f) < 0.000001f &&
               result.first_followup.state.yaw ==
                   profile.view_query.target.camera.yaw,
           "outside camera mode four applies heading yaw after first update");
    query.scene_mode = 6;
    query.heading_x = -1.0f;
    script = {};
    expect(awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               script.calls == 4 &&
               std::fabs(result.final_camera.target.camera.yaw +
                         1.57079632679f) < 0.000001f,
           "mode six shares the heading-derived yaw branch");
    query.scene_mode = 0;
    query.fallback_yaw = 0.75f;
    script = {};
    expect(awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               script.calls == 4 &&
               result.final_camera.target.camera.yaw == 0.75f,
           "other scene modes read the supplied fallback yaw");
    query.initial_camera.target.pitch_offset_8c = 0.1f;
    query.initial_camera.target.yaw_offset_90 = 0.2f;
    script = {};
    expect(awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               script.calls == 4 &&
               result.final_camera.target.pitch_offset_8c == 0.0f &&
               result.final_camera.target.yaw_offset_90 == 0.0f,
           "first post-update clears temporary offsets before the second pass");
    query.initial_camera.target.pitch_offset_8c = 0.0f;
    query.initial_camera.target.yaw_offset_90 = 0.0f;
    query.collision_category = 2;
    query.initial_camera.target.camera.flag_98 = false;
    script = {};
    expect(awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               script.calls == 4 && result.second_update_called &&
               !result.second_yaw_written &&
               result.final_camera.target.camera.yaw ==
                   profile.view_query.target.camera.yaw,
           "negative region result still updates twice but a clear flag blocks yaw");
    const awl::WorldMapPlayerCameraPlacement saved = result;
    script = {0, 3};
    expect(!awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               script.calls == 3 &&
               result.final_camera.target.camera.yaw ==
                   saved.final_camera.target.camera.yaw &&
               result.second_update_called == saved.second_update_called,
           "failed second height pass leaves placement output unchanged");
    query.scene_mode = 4;
    query.heading_x = std::numeric_limits<float>::infinity();
    script = {};
    expect(!awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, &result) &&
               script.calls == 2,
           "invalid heading is rejected before the conditional second update");
    expect(!awl::calculate_world_map_player_camera_placement(
               query, player_camera_height, &script, nullptr),
           "missing player camera placement output is rejected");
}

void test_world_map_player_message_camera_update() {
    awl::WorldMapCameraInitialProfile profile;
    expect(awl::make_world_map_camera_initial_profile(&profile),
           "message camera check starts from the verified profile");
    awl::WorldMapPlayerCameraMessageQuery query;
    query.previous_camera = profile.view_query;
    query.collision_category = 1;
    query.message = {3, {-1.0f, 0.0f, -5.2f},
                     {0.0f, 0.0f, 1.0f}, 0};
    PlayerCameraHeightScript script;
    awl::WorldMapPlayerCameraMessageResult result;
    expect(awl::calculate_world_map_player_message_camera_update(
               query, nullptr, nullptr, &result) &&
               !result.update_called &&
               result.final_camera.target.camera.position ==
                   profile.view_query.target.camera.position &&
               result.final_camera.target.camera.yaw ==
                   profile.view_query.target.camera.yaw,
           "fixed message's clear camera byte skips collision and camera writes");

    query.message.camera_update_requested = 1;
    query.message.position = {175.0f, 0.0f, 115.0f};
    expect(awl::calculate_world_map_player_message_camera_update(
               query, player_camera_height, &script, &result) &&
               result.update_called && script.calls == 2 &&
               result.followup.region_index == 0 &&
               !result.final_camera.target.camera.flag_98 &&
               result.final_camera.target.camera.flag_99 &&
               result.final_camera.target.camera.yaw == 0.0f &&
               result.final_camera.target.camera.position ==
                   query.message.position,
           "requested region message performs exactly one two-height update");

    query.message.position = {120.0f, 0.0f, 168.0f};
    query.pad_byte_8e = 64;
    script = {};
    expect(awl::calculate_world_map_player_message_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 && result.followup.region_index == -1 &&
               result.followup.yaw_adjustment_written &&
               result.final_camera.target.camera.flag_98 &&
               std::fabs(result.final_camera.target.camera.yaw -
                         (profile.view_query.target.camera.yaw + 0.0625f)) <
                   0.000001f,
           "outside requested message applies prior-flag signed PAD yaw once");
    query.collision_category = 2;
    query.message.position = {175.0f, 0.0f, 115.0f};
    script = {};
    expect(awl::calculate_world_map_player_message_camera_update(
               query, player_camera_height, &script, &result) &&
               result.followup.region_index == -1 && script.calls == 2 &&
               result.final_camera.target.camera.field_18 ==
                   profile.view_query.target.camera.field_18,
           "message scene type does not replace the player collision category");

    const awl::WorldMapPlayerCameraMessageResult saved = result;
    script = {0, 2};
    expect(!awl::calculate_world_map_player_message_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 &&
               result.final_camera.target.camera.yaw ==
                   saved.final_camera.target.camera.yaw &&
               result.update_called == saved.update_called,
           "failed requested height query leaves message camera output unchanged");
    expect(!awl::calculate_world_map_player_message_camera_update(
               query, nullptr, nullptr, &result) &&
               !awl::calculate_world_map_player_message_camera_update(
                   query, player_camera_height, &script, nullptr),
           "requested camera work requires a height source and output");
}

void test_world_map_player_movement_camera_update() {
    awl::WorldMapPlayerMovementCameraQuery query;
    query.previous_camera.target.camera.position = {2.0f, 0.0f, 2.0f};
    query.previous_camera.target.distance_30 = 2.0f;
    query.previous_camera.up_vector_24 = {0.0f, 1.0f, 0.0f};
    query.resolved_position = {2.0f, 0.0f, 2.0f};
    query.collision_category = 2;
    PlayerCameraHeightScript script;
    awl::WorldMapPlayerMovementCameraResult result;
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 &&
               result.followup.region_index == -1 &&
               result.final_camera.target.camera.position ==
                   query.resolved_position &&
               !result.heading_yaw_branch_taken &&
               !result.target_on_positive_side_188 &&
               std::fabs(result.movement_plane_constant_180 - 2.0f) <
                   0.000001f,
           "movement camera follows resolved position and marks forward target negative");
    query.previous_camera.target.origin_offset_0c = {0.0f, 0.0f, -4.0f};
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 && result.target_on_positive_side_188 &&
               result.update.final_target[2] < query.resolved_position[2],
           "movement camera marks a target behind the player plane positive");
    query.previous_camera.target.origin_offset_0c = {0.0f, 0.0f, -2.0f};
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               result.update.final_target[2] == query.resolved_position[2] &&
               result.target_on_positive_side_188,
           "movement plane counts an exactly coplanar target as positive");
    query.previous_camera.target.origin_offset_0c = {};
    query.previous_camera.target.camera.flag_98 = true;
    query.global_control_word_8034157c = 0x01000000u;
    query.heading_x = 1.0f;
    query.heading_z = 0.0f;
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 && result.heading_yaw_branch_taken &&
               result.heading_yaw_written &&
               std::fabs(result.final_camera.target.camera.yaw +
                         1.57079632679f) < 0.000001f,
           "movement heading branch writes opposite-heading yaw before follow-up");
    query.global_control_word_8034157c = 0x00800000u;
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 && !result.heading_yaw_branch_taken &&
               result.final_camera.target.camera.yaw == 0.0f,
           "adjacent control bit does not request the movement heading yaw");
    query.global_control_word_8034157c = 0x01000000u;
    awl::WorldMapPlayerMovementCameraQuery outside_query = query;
    outside_query.collision_category = 1;
    outside_query.resolved_position = {120.0f, 0.0f, 168.0f};
    outside_query.pad_byte_8e = 64;
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               outside_query, player_camera_height, &script, &result) &&
               script.calls == 2 && result.followup.region_index == -1 &&
               result.heading_yaw_written &&
               result.followup.yaw_adjustment_written &&
               std::fabs(result.final_camera.target.camera.yaw -
                         (-1.57079632679f + 0.0625f)) < 0.000001f,
           "outside follow-up adds signed PAD yaw after heading yaw write");
    query.global_byte_3f1 = 1;
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 && !result.heading_yaw_branch_taken &&
               result.final_camera.target.camera.yaw == 0.0f,
           "global byte blocks the optional heading yaw branch");
    query.global_byte_3f1 = 0;
    query.previous_camera.target.camera.flag_98 = false;
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               result.heading_yaw_branch_taken &&
               !result.heading_yaw_written &&
               result.final_camera.target.camera.yaw == 0.0f,
           "clear camera flag blocks yaw write without skipping the branch");
    query.collision_category = 1;
    query.resolved_position = {175.0f, 0.0f, 115.0f};
    query.previous_camera.target.camera.flag_98 = true;
    script = {};
    expect(awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 && result.followup.region_index == 0 &&
               result.final_camera.target.camera.yaw == 0.0f &&
               result.final_camera.target.camera.flag_99,
           "region profile follows the optional heading branch in DOL order");

    const awl::WorldMapPlayerMovementCameraResult saved = result;
    script = {0, 2};
    expect(!awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 2 &&
               result.final_camera.target.camera.yaw ==
                   saved.final_camera.target.camera.yaw &&
               result.target_on_positive_side_188 ==
                   saved.target_on_positive_side_188,
           "failed movement camera height query preserves output");
    query.heading_x = std::numeric_limits<float>::infinity();
    query.collision_category = 2;
    script = {};
    expect(!awl::calculate_world_map_player_movement_camera_update(
               query, player_camera_height, &script, &result) &&
               script.calls == 0,
           "invalid requested heading fails before height sampling");
    expect(!awl::calculate_world_map_player_movement_camera_update(
               query, nullptr, nullptr, &result) &&
               !awl::calculate_world_map_player_movement_camera_update(
                   query, player_camera_height, &script, nullptr),
           "movement camera requires a height source and output");
}

struct CameraHeightScript {
    std::array<float, 2> heights{};
    std::array<std::array<float, 3>, 2> positions{};
    int calls = 0;
    int fail_on_call = 0;
};

bool scripted_camera_height(const std::array<float, 3>& point,
                            float* height, void* context) {
    auto* script = static_cast<CameraHeightScript*>(context);
    if (script == nullptr || height == nullptr || script->calls >= 2) {
        return false;
    }
    script->positions[script->calls] = point;
    ++script->calls;
    if (script->calls == script->fail_on_call) {
        return false;
    }
    *height = script->heights[script->calls - 1];
    return true;
}

void test_world_map_camera_post_update() {
    awl::WorldMapCameraViewQuery query;
    query.target.distance_30 = 2.0f;
    query.up_vector_24 = {0.0f, 1.0f, 0.0f};
    CameraHeightScript heights;
    heights.heights = {0.0f, -1.0f};
    awl::WorldMapCameraPostUpdate result;
    expect(awl::calculate_world_map_camera_post_update(
               query, scripted_camera_height, &heights, &result) &&
               heights.calls == 2 &&
               heights.positions[0] ==
                   std::array<float, 3>{0.0f, 0.0f, 2.0f} &&
               heights.positions[1] == heights.positions[0] &&
               result.temporary_pitch_offset_8c == 0.0f &&
               result.final_plane.normal ==
                   std::array<float, 3>{0.0f, 0.0f, -1.0f} &&
               result.final_plane.constant == 2.0f &&
               !result.terrain_clamped &&
               result.final_target == heights.positions[1] &&
               result.final_matrix_50 == result.pitched_view.matrix_50,
           "two ordered height queries preserve an unobstructed camera view");

    query.target.yaw_offset_90 = 1.57079632679f;
    heights = {};
    heights.heights = {0.0f, -1.0f};
    expect(awl::calculate_world_map_camera_post_update(
               query, scripted_camera_height, &heights, &result) &&
               heights.calls == 2 &&
               std::fabs(heights.positions[0][0] - 2.0f) < 0.00001f &&
               std::fabs(heights.positions[1][2] - 2.0f) < 0.00001f &&
               result.final_plane.normal ==
                   std::array<float, 3>{0.0f, 0.0f, -1.0f},
           "second camera rebuild clears the earlier yaw offset before sampling");
    query.target.yaw_offset_90 = 0.0f;

    heights = {};
    heights.heights = {2.0f, 2.0f};
    expect(awl::calculate_world_map_camera_post_update(
               query, scripted_camera_height, &heights, &result) &&
               heights.calls == 2 &&
               heights.positions[0] ==
                   std::array<float, 3>{0.0f, 0.0f, 2.0f} &&
               std::fabs(result.temporary_pitch_offset_8c +
                         0.78539816339f) < 0.00001f &&
               heights.positions[1] == result.pitched_view.target.bounded &&
               std::fabs(heights.positions[1][1] - 1.41421356f) < 0.00001f &&
               result.terrain_clamped && result.final_target[1] == 2.0f &&
               std::fabs(result.final_matrix_50[9] - 0.81649658f) <
                   0.00001f &&
               std::fabs(result.final_matrix_50[10] - 0.57735027f) <
                   0.00001f &&
               std::fabs(result.final_plane.constant - 1.41421356f) <
                   0.00001f,
           "terrain pitch changes the second query and Y clamp rebuilds view");

    const awl::WorldMapCameraPostUpdate saved = result;
    heights = {};
    heights.fail_on_call = 1;
    expect(!awl::calculate_world_map_camera_post_update(
               query, scripted_camera_height, &heights, &result) &&
               heights.calls == 1 &&
               result.final_matrix_50 == saved.final_matrix_50,
           "first terrain query failure leaves camera output unchanged");
    heights = {};
    heights.heights = {0.0f, 0.0f};
    heights.fail_on_call = 2;
    expect(!awl::calculate_world_map_camera_post_update(
               query, scripted_camera_height, &heights, &result) &&
               heights.calls == 2 &&
               result.final_matrix_50 == saved.final_matrix_50,
           "second terrain query failure leaves camera output unchanged");
    heights = {};
    heights.heights[0] = std::numeric_limits<float>::infinity();
    expect(!awl::calculate_world_map_camera_post_update(
               query, scripted_camera_height, &heights, &result) &&
               result.final_matrix_50 == saved.final_matrix_50,
           "nonfinite terrain height is rejected");
    expect(!awl::calculate_world_map_camera_post_update(
               query, nullptr, nullptr, &result) &&
               !awl::calculate_world_map_camera_post_update(
                   query, scripted_camera_height, &heights, nullptr),
           "missing terrain sampler or camera result is rejected");
}

void test_world_map_camera_collision_height() {
    std::vector<uint8_t> camera_col = make_sample_leaf();
    camera_col[6] = 0;
    awl::CollisionHeightSample height;
    expect(awl::sample_type1_collision_height(
               camera_col.data(), camera_col.size(), 2.0f, 2.0f,
               &height) &&
               height.height == 6.0f && !height.used_edge_fallback,
           "camera slot-1 containment interpolates its triangle height");
    expect(awl::sample_type1_collision_height(
               camera_col.data(), camera_col.size(), -1.0f, 2.0f,
               &height) &&
               height.height == 4.0f && height.used_edge_fallback,
           "camera slot-1 miss samples the nearest edge's height");

    awl::WorldMapCameraViewQuery query;
    query.target.camera.position = {2.0f, 0.0f, 0.0f};
    query.target.distance_30 = 2.0f;
    query.up_vector_24 = {0.0f, 1.0f, 0.0f};
    awl::WorldMapCameraPostUpdate result;
    expect(awl::calculate_world_map_camera_post_update_from_collision(
               query, camera_col.data(), camera_col.size(), &result) &&
               result.first_view.target.bounded ==
                   std::array<float, 3>{2.0f, 0.0f, 2.0f} &&
               std::fabs(result.temporary_pitch_offset_8c +
                         1.249045772f) < 0.00001f &&
               result.terrain_clamped &&
               std::fabs(result.final_target[1] -
                         (2.0f + 2.0f * result.pitched_view.target.bounded[2])) <
                   0.00001f,
           "camera post-update uses slot-1 heights for both ordered queries");

    const awl::WorldMapCameraPostUpdate saved = result;
    camera_col[6] = 1;
    expect(!awl::calculate_world_map_camera_post_update_from_collision(
               query, camera_col.data(), camera_col.size(), &result) &&
               result.final_target == saved.final_target,
           "movement terrain is rejected as the camera collision source");
    camera_col[6] = 2;
    expect(!awl::sample_type1_collision_height(
               camera_col.data(), camera_col.size(), 2.0f, 2.0f,
               &height),
           "unsupported collision height mode is rejected");
    camera_col = make_single_leaf();
    camera_col[6] = 0;
    expect(!awl::sample_type1_collision_height(
               camera_col.data(), camera_col.size(), 2.0f, 2.0f,
               &height) &&
               !awl::calculate_world_map_camera_post_update_from_collision(
                   query, camera_col.data(), camera_col.size(), &result) &&
               result.final_target == saved.final_target,
           "empty camera leaf fails without a silent zero-height fallback");
}

void test_world_map_movement_candidate_sequence() {
    std::vector<uint8_t> terrain = make_sample_leaf();
    awl::WorldMapMovementQuery query;
    query.pad.stick_x = 80;
    query.current_position = {2.0f, 7.0f, 2.0f};
    query.current_axis = {0.0f, 0.0f, 1.0f};
    query.collision.terrain_data = terrain.data();
    query.collision.terrain_size = terrain.size();
    awl::WorldMapMovementCandidate result;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               result.movement_enabled &&
               std::fabs(result.steering.current_speed - 0.03f) < 0.0001f &&
               result.proposed_position[0] > query.current_position[0] &&
               result.proposed_position[1] == query.current_position[1] &&
               !result.directional_contact.matched &&
               result.directional_contact.direction_code == 7 &&
               result.directional_contact.world_position ==
                   result.proposed_position &&
               result.resolved_position[1] != result.proposed_position[1] &&
               result.collision.resolver_contact_bits == 0u,
           "enabled movement runs steering, proposal, contact fallback, and terrain in order");
    awl::CollisionSurfaceSample surface;
    expect(awl::sample_type1_collision_surface(
               terrain.data(), terrain.size(), result.resolved_position[0],
               result.resolved_position[2], &surface) &&
               std::fabs(result.resolved_position[1] - surface.height) <
                   0.0001f,
           "composed movement candidate height matches independent terrain sampling");
    awl::WorldMapMovementQuery camera_query = query;
    awl::WorldMapCameraFollowupState supplied_camera;
    supplied_camera.yaw = 0.25f;
    awl::WorldMapCameraFollowup camera_followup;
    const bool outside_camera_valid =
        awl::plan_world_map_camera_followup(
            supplied_camera, 1, camera_query.current_position, 1, 0,
            &camera_followup);
    expect(outside_camera_valid && camera_followup.region_index == -1 &&
               camera_followup.state.flag_98 &&
               camera_followup.state.yaw == 0.25f,
           "outside camera region supplies the movement yaw-write flag");
    camera_query.camera_yaw_radians = camera_followup.state.yaw;
    camera_query.camera_yaw_commit_enabled = camera_followup.state.flag_98;
    awl::WorldMapMovementCandidate first_camera_frame;
    awl::WorldMapMovementCandidate second_camera_frame;
    const float first_expected_yaw =
        0.25f - 4.0f * 0.03f * 0.017453292f;
    const bool first_camera_valid =
        awl::calculate_world_map_movement_candidate(
            camera_query, &first_camera_frame);
    expect(first_camera_valid && first_camera_frame.camera_yaw_written &&
               std::fabs(first_camera_frame.camera_yaw_after_proposal -
                         first_expected_yaw) < 0.00001f,
           "supplied camera flag reports the pre-collision yaw write");
    if (first_camera_valid) {
        supplied_camera = camera_followup.state;
        supplied_camera.yaw = first_camera_frame.camera_yaw_after_proposal;
        const bool second_followup_valid =
            awl::plan_world_map_camera_followup(
                supplied_camera, 1, first_camera_frame.resolved_position,
                1, 0, &camera_followup);
        expect(second_followup_valid && camera_followup.state.flag_98 &&
                   camera_followup.state.yaw == first_expected_yaw,
               "outside camera follow-up carries yaw and flag to the next frame");
        camera_query.camera_yaw_radians = camera_followup.state.yaw;
        camera_query.camera_yaw_commit_enabled = camera_followup.state.flag_98;
        camera_query.current_position = first_camera_frame.resolved_position;
        camera_query.steering = first_camera_frame.steering;
        const bool second_camera_valid =
            awl::calculate_world_map_movement_candidate(
                camera_query, &second_camera_frame);
        const float second_expected_yaw =
            first_expected_yaw - 4.0f * 0.06f * 0.017453292f;
        expect(second_camera_valid && second_camera_frame.camera_yaw_written &&
                   std::fabs(second_camera_frame.camera_yaw_after_proposal -
                             second_expected_yaw) < 0.00001f &&
                   second_camera_frame.proposed_position !=
                       first_camera_frame.proposed_position,
               "reported yaw feeds the next supplied movement frame");
    }
    camera_query.state_680 = 0;
    expect(awl::calculate_world_map_movement_candidate(
               camera_query, &second_camera_frame) &&
               !second_camera_frame.movement_enabled &&
               !second_camera_frame.camera_yaw_written &&
               second_camera_frame.camera_yaw_after_proposal ==
                   camera_query.camera_yaw_radians,
           "blocked movement leaves the supplied camera yaw untouched");
    camera_query.state_680 = -1;
    camera_query.state_58c = 1;
    expect(awl::calculate_world_map_movement_candidate(
               camera_query, &second_camera_frame) &&
               !second_camera_frame.movement_enabled &&
               !second_camera_frame.camera_yaw_written &&
               second_camera_frame.camera_yaw_after_proposal ==
                   camera_query.camera_yaw_radians,
           "second movement guard also leaves camera yaw untouched");
    awl::WorldMapScenePositionUpdate scene_update;
    expect(awl::plan_world_map_scene_position_update(
               0, 0, result.resolved_position, &scene_update) &&
               scene_update.position == result.resolved_position &&
               scene_update.next_bucket == 0 &&
               !scene_update.relink_required,
           "collision-adjusted position retains the constructor's bucket-zero scene type");

    awl::CollisionDynamicPassObject circle;
    circle.identity = 2;
    circle.enabled = true;
    circle.category = 1;
    circle.collision_flags = 1u;
    circle.center_world = result.proposed_position;
    circle.radius = 0.5f;
    awl::WorldMapCollisionRegistry circle_registry;
    awl::WorldMapRegisteredCollisionObject circle_entry;
    circle_entry.collision = circle;
    expect(circle_registry.register_object(
               awl::WorldMapCollisionList::First, circle_entry),
           "circle entry registers in the first collision list");
    awl::WorldMapRegisteredCollisionObject player_source = circle_entry;
    player_source.collision.identity = 99;
    expect(circle_registry.register_object(
               awl::WorldMapCollisionList::Third, player_source),
           "player source object can register in the separate third list");
    awl::WorldMapCollisionSnapshot circle_snapshot =
        circle_registry.snapshot();
    expect(circle_snapshot.first_resolver.size() == 1 &&
               circle_snapshot.third_objects.size() == 1 &&
               circle_snapshot.third_objects[0].collision.identity == 99,
           "third-list source does not enter the player's first or later passes");
    query.collision.first_objects = circle_snapshot.first_resolver.data();
    query.collision.first_object_count = circle_snapshot.first_resolver.size();
    query.collision.source_identity = 99;
    query.collision.moving_radius = -99.0f;
    query.directional_objects = circle_snapshot.first_directional.data();
    query.directional_object_count = circle_snapshot.first_directional.size();
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               result.movement_enabled && result.collision.first_pass.contact &&
               result.collision.resolver_contact_bits == 4u &&
               result.directional_contact.queried_objects == 0 &&
               std::fabs(result.collision.first_pass.position[2] -
                         result.proposed_position[2] - 0.81f) < 0.0002f,
           "first-list circle uses the player's fixed radius while directional search skips it");
    circle_snapshot.first_directional[0].category = 2;
    expect(!awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled,
           "inconsistent first-list views are rejected before movement work");
    circle_snapshot.first_directional[0].category = 1;

    put_be16(terrain, 8 + 0x34, 1u);
    constexpr awl::CollisionAffineTransform identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    awl::CollisionDynamicPassObject wall;
    wall.identity = 3;
    wall.enabled = true;
    wall.category = 1;
    wall.collision_flags = 2u;
    wall.data = terrain.data();
    wall.size = terrain.size();
    wall.world_to_object = identity;
    wall.object_to_world = identity;
    wall.center_local = {5.0f, 0.0f, 0.0f};
    wall.radius = 10.0f;
    awl::WorldMapCollisionRegistry wall_registry;
    awl::WorldMapRegisteredCollisionObject wall_entry;
    wall_entry.collision = wall;
    wall_entry.world_position = {20.0f, 30.0f, 40.0f};
    wall_entry.heading_axis = {0.0f, 0.0f, -1.0f};
    wall_entry.metadata = 42u;
    expect(wall_registry.register_object(
               awl::WorldMapCollisionList::First, wall_entry),
           "type-one entry registers in the first collision list");
    const awl::WorldMapCollisionSnapshot wall_snapshot =
        wall_registry.snapshot();
    awl::WorldMapMovementQuery wall_query;
    wall_query.pad.stick_y = -80;
    wall_query.current_position = {5.0f, 7.0f, -0.1f};
    wall_query.current_axis = {0.0f, 0.0f, 1.0f};
    wall_query.directional_objects = wall_snapshot.first_directional.data();
    wall_query.directional_object_count =
        wall_snapshot.first_directional.size();
    wall_query.collision.terrain_data = terrain.data();
    wall_query.collision.terrain_size = terrain.size();
    wall_query.collision.first_objects = wall_snapshot.first_resolver.data();
    wall_query.collision.first_object_count =
        wall_snapshot.first_resolver.size();
    expect(awl::calculate_world_map_movement_candidate(
               wall_query, &result) && result.movement_enabled &&
               result.directional_contact.matched &&
               result.directional_contact.direction_code == 0 &&
               result.directional_contact.metadata == 42u &&
               result.collision.first_pass.queried_objects == 1,
           "same supplied type-one entry reaches direction metadata and collision pass");

    query.state_680 = 0;
    query.collision.terrain_data = nullptr;
    query.collision.terrain_size = 0;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled &&
               result.resolved_position == query.current_position &&
               result.proposed_position == query.current_position &&
               result.steering.current_speed == 0.0f &&
               result.directional_contact.queried_objects == 0,
           "blocked world-map state avoids steering and collision queries");

    query.state_680 = -1;
    query.state_58c = 1;
    expect(awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled &&
               result.resolved_position == query.current_position,
           "nonzero second state guard also blocks the movement sequence");

    query.state_58c = 0;
    expect(!awl::calculate_world_map_movement_candidate(query, &result) &&
               !result.movement_enabled &&
               result.resolved_position == std::array<float, 3>{},
           "unsupported enabled collision input rejects the whole candidate");
    expect(!awl::calculate_world_map_movement_candidate(query, nullptr),
           "missing movement result is rejected");
}

void test_world_map_movement_contact_tail() {
    const std::array<float, 3> prior{1.0f, 2.0f, 3.0f};
    const std::array<float, 3> resolved{4.0f, 5.0f, 6.0f};
    std::array<awl::WorldMapMovementContactSlotOutcome, 2> slots{};
    awl::WorldMapMovementContactTail result;
    expect(awl::plan_world_map_movement_contact_tail(
               2, prior, resolved, slots, &result) &&
               result.recorded_category == 2 &&
               result.recorded_prior == prior &&
               result.recorded_resolved == resolved &&
               result.polygon_queries == 0 && result.state_requests == 0 &&
               result.accepted_slot == -1 &&
               !result.movement_reset_requested,
           "contact state records both positions before non-player probe skip");
    expect(awl::plan_world_map_movement_contact_tail(
               1, prior, resolved, slots, &result) &&
               result.polygon_queries == 2 && result.state_requests == 0 &&
               result.accepted_slot == -1,
           "category one checks both slots when neither polygon contacts");
    slots[0].state_request_accepted = true;
    slots[1] = {true, true};
    expect(awl::plan_world_map_movement_contact_tail(
               1, prior, resolved, slots, &result) &&
               result.polygon_queries == 2 && result.state_requests == 1 &&
               result.accepted_slot == 1 && result.movement_reset_requested,
           "slot one requests state only after its polygon contact");
    slots[0] = {true, false};
    expect(awl::plan_world_map_movement_contact_tail(
               1, prior, resolved, slots, &result) &&
               result.polygon_queries == 2 && result.state_requests == 2 &&
               result.accepted_slot == 1 && result.movement_reset_requested,
           "rejected slot-zero request allows the slot-one request");
    slots[0] = {true, true};
    expect(awl::plan_world_map_movement_contact_tail(
               1, prior, resolved, slots, &result) &&
               result.polygon_queries == 1 && result.state_requests == 1 &&
               result.accepted_slot == 0 && result.movement_reset_requested,
           "accepted slot zero stops before consulting slot one");
    const awl::WorldMapMovementContactTail saved = result;
    const std::array<float, 3> invalid{
        std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
    expect(!awl::plan_world_map_movement_contact_tail(
               1, invalid, resolved, slots, &result) &&
               !awl::plan_world_map_movement_contact_tail(
                   1, prior, invalid, slots, &result) &&
               !awl::plan_world_map_movement_contact_tail(
                   1, prior, resolved, slots, nullptr) &&
               result.recorded_prior == saved.recorded_prior &&
               result.accepted_slot == saved.accepted_slot,
           "invalid contact-tail inputs preserve the prior output");
}

void test_world_map_polygon_contact() {
    // Explicit closing vertex: the DOL walks consecutive pairs only.
    const std::vector<std::array<float, 3>> square{
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
        {2.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 2.0f},
        {0.0f, 0.0f, 0.0f}};
    const std::array<float, 3> inside{1.0f, 5.0f, 1.0f};
    const std::array<float, 3> left{-1.0f, -5.0f, 1.0f};
    const std::array<float, 3> right{3.0f, 0.0f, 1.0f};
    bool contact = false;
    expect(awl::query_world_map_polygon_contact(0, square, inside, right,
                                                 &contact) && contact,
           "mode-zero ray counts one right-edge crossing from inside");
    expect(awl::query_world_map_polygon_contact(0, square, left, inside,
                                                 &contact) && !contact &&
               awl::query_world_map_polygon_contact(0, square, right, inside,
                                                     &contact) && !contact,
           "mode-zero parity excludes points on either outside side");
    auto open_square = square;
    open_square.pop_back();
    expect(awl::query_world_map_polygon_contact(0, open_square, left, inside,
                                                 &contact) && contact,
           "the missing closing edge is not invented");

    expect(awl::query_world_map_polygon_contact(1, square, left, inside,
                                                 &contact) && contact &&
               awl::query_world_map_polygon_contact(2, square, left, inside,
                                                     &contact) && contact &&
               awl::query_world_map_polygon_contact(3, square, left, inside,
                                                     &contact) && !contact,
           "entry across the left edge has negative orientation");
    expect(awl::query_world_map_polygon_contact(2, square, inside, left,
                                                 &contact) && !contact &&
               awl::query_world_map_polygon_contact(3, square, inside, left,
                                                     &contact) && contact,
           "exit across the left edge has nonnegative orientation");
    const std::array<float, 3> on_left{0.0f, 0.0f, 1.0f};
    expect(awl::query_world_map_polygon_contact(1, square, left, on_left,
                                                 &contact) && !contact &&
               awl::query_world_map_polygon_contact(1, square, on_left, inside,
                                                     &contact) && contact,
           "segment ends are exclusive and starts are inclusive");
    const std::vector<std::array<float, 3>> end_edge{
        {10000.0f, 0.0f, 0.0f}, {10000.0f, 0.0f, 2.0f}};
    const std::array<float, 3> origin{0.0f, 0.0f, 1.0f};
    expect(awl::query_world_map_polygon_contact(0, end_edge, origin, inside,
                                                 &contact) && !contact,
           "mode-zero 10000-unit ray excludes its endpoint");
    const std::vector<std::array<float, 3>> parallel_edge{
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    expect(awl::query_world_map_polygon_contact(
               1, parallel_edge, {0.0f, 0.0f, 1.0f},
               {2.0f, 0.0f, 1.0f}, &contact) && !contact,
           "parallel XZ segments do not contact");

    std::array<awl::WorldMapMovementContactSlotOutcome, 2> slots{};
    expect(awl::query_world_map_polygon_contact(3, square, left, inside,
                                                 &slots[0].polygon_contact) &&
               awl::query_world_map_polygon_contact(
                   2, square, left, inside, &slots[1].polygon_contact),
           "supplied slot polygons produce contact outcomes");
    slots[0].state_request_accepted = true;
    slots[1].state_request_accepted = true;
    awl::WorldMapMovementContactTail tail;
    expect(awl::plan_world_map_movement_contact_tail(
               1, left, inside, slots, &tail) &&
               tail.polygon_queries == 2 && tail.state_requests == 1 &&
               tail.accepted_slot == 1,
           "polygon result gates the ordered two-slot action decision");

    contact = true;
    auto invalid_vertices = square;
    invalid_vertices[1][0] = std::numeric_limits<float>::infinity();
    const float huge = std::numeric_limits<float>::max();
    const std::vector<std::array<float, 3>> overflowing_edge{
        {huge, 0.0f, 0.0f}, {-huge, 0.0f, 2.0f}};
    expect(!awl::query_world_map_polygon_contact(4, square, inside, right,
                                                  &contact) &&
               !awl::query_world_map_polygon_contact(0, {}, inside, right,
                                                      &contact) &&
               !awl::query_world_map_polygon_contact(
                   0, invalid_vertices, inside, right, &contact) &&
               !awl::query_world_map_polygon_contact(
                   1, overflowing_edge, left, inside, &contact) &&
               !awl::query_world_map_polygon_contact(0, square, inside,
                                                      right, nullptr) &&
               contact,
           "unsupported or invalid polygon queries preserve output");
}

std::vector<uint8_t> make_trigger_spl_fixture() {
    constexpr size_t groups = 61;
    std::vector<uint8_t> bytes(8 + groups * 4, 0);
    put_be32(bytes, 0, 0xF0F0E1ECu);
    put_be32(bytes, 4, static_cast<uint32_t>(groups));
    size_t player_group = 0;
    for (size_t group = 0; group < groups; ++group) {
        const size_t offset = bytes.size();
        put_be32(bytes, 8 + group * 4, static_cast<uint32_t>(offset));
        const uint32_t count = group == 55 ? 2u : 0u;
        bytes.resize(offset + 4 + count * 4, 0);
        put_be32(bytes, offset, count);
        if (group == 55) {
            player_group = offset;
        }
    }
    const std::array<std::array<float, 3>, 5> square{{
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f},
        {2.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 2.0f},
        {0.0f, 0.0f, 0.0f}}};
    const size_t first = bytes.size();
    put_be32(bytes, player_group + 4, static_cast<uint32_t>(first));
    bytes.resize(first + 8 + square.size() * 12, 0);
    put_be32(bytes, first, 0);
    put_be32(bytes, first + 4, static_cast<uint32_t>(square.size()));
    for (size_t index = 0; index < square.size(); ++index) {
        for (size_t axis = 0; axis < 3; ++axis) {
            put_be_float(bytes, first + 8 + index * 12 + axis * 4,
                         square[index][axis]);
        }
    }
    const size_t second = bytes.size();
    put_be32(bytes, player_group + 8, static_cast<uint32_t>(second));
    bytes.resize(second + 8 + 2 * 12, 0);
    put_be32(bytes, second, 3);
    put_be32(bytes, second + 4, 2);
    put_be_float(bytes, second + 8, 0.0f);
    put_be_float(bytes, second + 16, 2.0f);
    put_be_float(bytes, second + 20, 0.0f);
    put_be_float(bytes, second + 28, 0.0f);
    return bytes;
}

struct TriggerRequestProbe {
    std::array<bool, 2> accepted{};
    std::array<int32_t, 2> called_slots{-1, -1};
    int32_t calls = 0;
    int32_t fail_slot = -1;
};

bool request_trigger_state(int32_t slot, void* context, bool* accepted) {
    auto* probe = static_cast<TriggerRequestProbe*>(context);
    if (probe == nullptr || accepted == nullptr || slot < 0 || slot > 1 ||
        probe->calls >= 2) {
        return false;
    }
    probe->called_slots[static_cast<size_t>(probe->calls++)] = slot;
    if (slot == probe->fail_slot) {
        return false;
    }
    *accepted = probe->accepted[static_cast<size_t>(slot)];
    return true;
}

void test_world_map_trigger_asset() {
    const auto bytes = make_trigger_spl_fixture();
    awl::WorldMapTriggerAsset asset;
    const std::array<float, 3> inside{1.0f, 0.0f, 1.0f};
    const std::array<float, 3> outside{-1.0f, 0.0f, 1.0f};
    bool contact = false;
    expect(asset.load_from_bytes(bytes.data(), bytes.size()) &&
               asset.loaded() && asset.category1_polygon(0) != nullptr &&
               asset.category1_polygon(0)->mode == 0 &&
               asset.category1_polygon(0)->vertices.size() == 5 &&
               asset.category1_polygon(1) != nullptr &&
               asset.category1_polygon(1)->mode == 3 &&
               asset.category1_polygon(1)->vertices.size() == 2 &&
               asset.category1_polygon(2) == nullptr,
           "SPL resolves the DOL category-one group and both slots");
    expect(asset.query_category1_contact(0, inside, outside, &contact) &&
               contact &&
               asset.query_category1_contact(0, outside, inside, &contact) &&
               !contact,
           "decoded mode-zero polygon separates inside and outside");
    expect(asset.query_category1_contact(
               1, {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}, &contact) &&
               contact &&
               asset.query_category1_contact(
                   1, {-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, &contact) &&
               !contact,
           "decoded mode-three segment preserves crossing direction");
    TriggerRequestProbe requests;
    requests.accepted = {true, true};
    awl::WorldMapMovementContactTail tail;
    expect(asset.evaluate_movement_contact_tail(
               1, inside, outside, request_trigger_state, &requests,
               &tail) &&
               tail.polygon_queries == 1 && tail.state_requests == 1 &&
               tail.accepted_slot == 0 && tail.recorded_prior == inside &&
               tail.recorded_resolved == outside && requests.calls == 1 &&
               requests.called_slots[0] == 0,
           "accepted first SPL contact stops before querying the second slot");
    auto overflow_second_slot = bytes;
    constexpr size_t first_polygon = 8 + 61 * 4 + 61 * 4 + 8;
    constexpr size_t second_polygon = first_polygon + 8 + 5 * 12;
    const float huge_coordinate = std::numeric_limits<float>::max();
    put_be_float(overflow_second_slot, second_polygon + 8,
                 huge_coordinate);
    put_be_float(overflow_second_slot, second_polygon + 20,
                 -huge_coordinate);
    awl::WorldMapTriggerAsset second_slot_hazard;
    requests = {};
    requests.accepted[0] = true;
    expect(second_slot_hazard.load_from_bytes(
               overflow_second_slot.data(), overflow_second_slot.size()) &&
               second_slot_hazard.evaluate_movement_contact_tail(
                   1, inside, outside, request_trigger_state, &requests,
                   &tail) &&
               tail.accepted_slot == 0 && requests.calls == 1,
           "first acceptance never evaluates an unusable second-slot query");
    requests = {};
    requests.accepted = {false, true};
    expect(asset.evaluate_movement_contact_tail(
               1, inside, outside, request_trigger_state, &requests,
               &tail) &&
               tail.polygon_queries == 2 && tail.state_requests == 2 &&
               tail.accepted_slot == 1 && requests.calls == 2 &&
               requests.called_slots == std::array<int32_t, 2>{0, 1},
           "rejected first request reaches the second SPL polygon and request");
    requests = {};
    const std::array<float, 3> right{3.0f, 0.0f, 1.0f};
    expect(asset.evaluate_movement_contact_tail(
               1, right, right, request_trigger_state, &requests,
               &tail) &&
               tail.polygon_queries == 2 && tail.state_requests == 0 &&
               tail.accepted_slot == -1 && requests.calls == 0,
           "noncontact skips both state requests");
    expect(asset.evaluate_movement_contact_tail(
               2, inside, outside, nullptr, nullptr, &tail) &&
               tail.recorded_category == 2 && tail.polygon_queries == 0,
           "non-player category records state without a trigger provider");
    requests = {};
    requests.fail_slot = 1;
    const auto saved_tail = tail;
    expect(!asset.evaluate_movement_contact_tail(
               1, inside, outside, request_trigger_state, &requests,
               &tail) &&
               requests.called_slots == std::array<int32_t, 2>{0, 1} &&
               tail.recorded_category == saved_tail.recorded_category &&
               tail.recorded_resolved == saved_tail.recorded_resolved,
           "failed second request preserves output after ordered callbacks");
    requests = {};
    const std::array<float, 3> invalid{
        std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
    expect(!asset.evaluate_movement_contact_tail(
               1, invalid, outside, request_trigger_state, &requests,
               &tail) &&
               !asset.evaluate_movement_contact_tail(
                   1, inside, outside, nullptr, nullptr, &tail) &&
               requests.calls == 0 &&
               tail.recorded_category == saved_tail.recorded_category,
           "invalid positions or missing state provider cannot trigger actions");

    auto broken = bytes;
    broken[0] ^= 1;
    expect(!asset.load_from_bytes(broken.data(), broken.size()) &&
               !asset.loaded() && asset.category1_polygon(0) == nullptr &&
               !asset.evaluate_movement_contact_tail(
                   1, inside, outside, request_trigger_state, &requests,
                   &tail) &&
               tail.recorded_category == saved_tail.recorded_category,
           "bad SPL marker clears a prior successful load");
    broken = bytes;
    put_be32(broken, 8 + 55 * 4, static_cast<uint32_t>(broken.size() + 4));
    expect(!asset.load_from_bytes(broken.data(), broken.size()),
           "out-of-order group offset is rejected");
    broken = bytes;
    const size_t player_group = 8 + 61 * 4 + 55 * 4;
    put_be32(broken, player_group + 4, static_cast<uint32_t>(broken.size()));
    expect(!asset.load_from_bytes(broken.data(), broken.size()),
           "polygon pointer beyond its packed location is rejected");
    broken = bytes;
    const size_t first = player_group + 12 + 5 * 4;
    put_be32(broken, first, 4);
    expect(!asset.load_from_bytes(broken.data(), broken.size()),
           "unsupported polygon mode is rejected");
    broken = bytes;
    put_be32(broken, first + 4, 0xffffffffu);
    expect(!asset.load_from_bytes(broken.data(), broken.size()),
           "oversized vertex count is rejected before reading vertices");
    broken = bytes;
    put_be32(broken, first + 8, 0x7f800000u);
    expect(!asset.load_from_bytes(broken.data(), broken.size()),
           "nonfinite vertex is rejected");
    broken = bytes;
    broken.pop_back();
    contact = true;
    expect(!asset.load_from_bytes(broken.data(), broken.size()) &&
               !asset.load_from_bytes(nullptr, 0) &&
               !asset.query_category1_contact(0, inside, outside, &contact) &&
               contact,
           "truncated or missing SPL leaves no usable contact source");
}

void test_world_map_scene_position_bucket_decision() {
    constexpr float third_x_threshold = 152.97621f;
    uint32_t threshold_bits = 0;
    std::memcpy(&threshold_bits, &third_x_threshold, sizeof(threshold_bits));
    expect(threshold_bits == 0x4318F9E9u,
           "native third X threshold retains the verified DOL float bits");
    constexpr float x_positions[4] = {0.0f, 54.0f, 99.0f, 152.97621f};
    constexpr float z_positions[4] = {0.0f, 64.0f, 130.0f, 194.0f};
    constexpr uint8_t expected[4][4] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16}};
    for (size_t x = 0; x < 4; ++x) {
        for (size_t z = 0; z < 4; ++z) {
            const std::array<float, 3> position{
                x_positions[x], 500.0f, z_positions[z]};
            awl::WorldMapScenePositionUpdate update;
            expect(awl::plan_world_map_scene_position_update(
                       1, expected[x][z], position, &update) &&
                       update.next_bucket == expected[x][z] &&
                       !update.relink_required &&
                       update.position == position,
                   "verified sixteen-entry scene table uses X-major and Z-minor bins");
        }
    }
    for (size_t index = 1; index < 4; ++index) {
        const std::array<float, 3> before_x{
            std::nextafter(x_positions[index],
                           -std::numeric_limits<float>::infinity()),
            0.0f, 0.0f};
        const std::array<float, 3> before_z{
            0.0f, 0.0f,
            std::nextafter(z_positions[index],
                           -std::numeric_limits<float>::infinity())};
        awl::WorldMapScenePositionUpdate boundary;
        expect(awl::plan_world_map_scene_position_update(
                   1, expected[index - 1][0], before_x, &boundary) &&
                   boundary.next_bucket == expected[index - 1][0],
               "one float below each X threshold stays in the lower bin");
        expect(awl::plan_world_map_scene_position_update(
                   1, expected[0][index - 1], before_z, &boundary) &&
                   boundary.next_bucket == expected[0][index - 1],
               "one float below each Z threshold stays in the lower bin");
    }

    awl::WorldMapScenePositionUpdate update;
    const std::array<float, 3> below_third_x{
        std::nextafter(152.97621f, -std::numeric_limits<float>::infinity()),
        -10.0f, 194.0f};
    expect(awl::plan_world_map_scene_position_update(
               2, 12, below_third_x, &update) &&
               update.next_bucket == 12 && !update.relink_required,
           "one float below the third X threshold stays in the preceding bin");
    const std::array<float, 3> crossing{152.97621f, -10.0f, 194.0f};
    expect(awl::plan_world_map_scene_position_update(
               2, 12, crossing, &update) && update.next_bucket == 16 &&
               update.relink_required && update.position == crossing,
           "crossing a threshold plans a relink while retaining the full position");
    expect(awl::plan_world_map_scene_position_update(
               1, 0, crossing, &update) && update.relink_required &&
               update.previous_bucket == 0 && update.next_bucket == 16,
           "switching from bucket zero plans entry to the selected grid bucket");
    expect(awl::plan_world_map_scene_position_update(
               1, -1, crossing, &update) && update.relink_required &&
               update.previous_bucket == -1 && update.next_bucket == 16,
           "constructor key minus one plans first entry into a spatial bucket");
    expect(awl::plan_world_map_scene_position_update(
               0, 0, crossing, &update) && update.next_bucket == 0 &&
               !update.relink_required && update.position == crossing,
           "constructor's initial type zero keeps bucket zero across position bins");
    expect(awl::plan_world_map_scene_position_update(
               0, 16, crossing, &update) && update.next_bucket == 0 &&
               update.relink_required,
           "changing back to scene type zero plans a move to bucket zero");
    for (int32_t type = 3; type <= 44; ++type) {
        expect(awl::plan_world_map_scene_position_update(
                   type, 0, crossing, &update) &&
                   update.next_bucket == static_cast<uint8_t>(type + 14) &&
                   update.relink_required,
               "verified fixed scene-type table maps types three through forty-four");
    }

    const std::array<float, 3> nonfinite{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    expect(!awl::plan_world_map_scene_position_update(
               1, 0, nonfinite, &update) && update.next_bucket == 0,
           "nonfinite scene position is rejected without an update plan");
    expect(!awl::plan_world_map_scene_position_update(
               45, 0, crossing, &update) && update.next_bucket == 0,
           "scene type beyond verified table is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               -1, 0, crossing, &update),
           "negative scene type is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               1, 59, crossing, &update),
           "prior bucket beyond verified range is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               1, -2, crossing, &update),
           "prior bucket below constructor key is rejected");
    expect(!awl::plan_world_map_scene_position_update(
               1, 0, crossing, nullptr),
           "missing scene position result is rejected");
}

void test_world_map_scene_bucket_registry() {
    awl::WorldMapSceneBucketRegistry registry;
    const std::array<float, 3> start{10.0f, 3.0f, 10.0f};
    const std::array<float, 3> same_bucket{20.0f, 4.0f, 20.0f};
    const std::array<float, 3> across_x{54.0f, 5.0f, 10.0f};
    expect(registry.register_object(11, 1, start) &&
               registry.register_object(22, 2, start) &&
               registry.register_object(33, 1, across_x),
           "scene objects register in the constructor's unpositioned bucket");
    expect(registry.size(-1) == 3 && registry.snapshot(-1)[0].identity == 33 &&
               registry.snapshot(-1)[1].identity == 22 &&
               registry.snapshot(-1)[2].identity == 11,
           "constructor bucket insertion keeps newest object first");

    awl::WorldMapScenePositionUpdate update;
    expect(registry.update_position(11, start, &update) &&
               update.relink_required && update.previous_bucket == -1 &&
               update.next_bucket == 1 && registry.size(-1) == 2 &&
               registry.update_position(22, start, &update) &&
               registry.update_position(33, across_x, &update) &&
               registry.size(-1) == 0 &&
               registry.size(1) == 2 && registry.size(5) == 1 &&
               registry.snapshot(1)[0].identity == 22 &&
               registry.snapshot(1)[1].identity == 11,
           "first position writes move objects into computed spatial buckets");

    expect(registry.update_position(11, same_bucket, &update) &&
               !update.relink_required && update.next_bucket == 1 &&
               registry.snapshot(1)[0].identity == 22 &&
               registry.snapshot(1)[1].position == same_bucket,
           "same-bucket position write preserves list order");
    expect(registry.update_position(11, across_x, &update) &&
               update.relink_required && update.previous_bucket == 1 &&
               update.next_bucket == 5 && registry.size(1) == 1 &&
               registry.snapshot(5)[0].identity == 11 &&
               registry.snapshot(5)[1].identity == 33,
           "crossing a bin detaches and inserts at the new bucket head");
    expect(registry.update_position(22, across_x, &update) &&
               registry.snapshot(5)[0].identity == 22 &&
               registry.snapshot(5)[1].identity == 11,
           "a later crossing moves ahead of existing destination entries");

    expect(registry.register_object(44, 0, start) &&
               registry.size(-1) == 1 &&
               registry.update_position(44, across_x, &update) &&
               update.relink_required && update.previous_bucket == -1 &&
               registry.update_position(44, start, &update) &&
               !update.relink_required && registry.size(0) == 1 &&
               registry.snapshot(0)[0].position == start,
           "type zero first enters bucket zero, then retains it across XYZ changes");
    expect(registry.register_object(55, 44, start) &&
               registry.update_position(55, start, &update) &&
               registry.size(58) == 1,
           "fixed scene type forty-four enters the final supported bucket");

    const std::array<float, 3> nonfinite{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    const auto before = registry.snapshot(5);
    expect(!registry.register_object(11, 1, start) &&
               !registry.register_object(0, 1, start) &&
               !registry.register_object(66, 45, start) &&
               !registry.update_position(11, nonfinite, &update) &&
               !registry.update_position(99, start, &update) &&
               !registry.update_position(11, start, nullptr) &&
               registry.snapshot(5).size() == before.size() &&
               registry.snapshot(5)[1].position == before[1].position &&
               update.next_bucket == 0,
           "invalid registration and position updates leave buckets unchanged");
    expect(registry.unregister_object(11) &&
               !registry.unregister_object(11) && registry.size(5) == 2 &&
               registry.snapshot(5)[0].identity == 22,
           "unregister removes exactly one scene node");
    registry.clear();
    expect(registry.size(-1) == 0 && registry.size(0) == 0 &&
               registry.size(1) == 0 &&
               registry.size(5) == 0 && registry.size(58) == 0 &&
               registry.snapshot(59).empty() &&
               registry.snapshot(-2).empty(),
           "clear removes all buckets and unknown bucket queries stay empty");
}

void test_world_map_player_scene_message_1f() {
    awl::WorldMapSceneBucketRegistry registry;
    const std::array<float, 3> start{10.0f, 3.0f, 10.0f};
    const std::array<float, 3> next{54.0f, 4.0f, 130.0f};
    awl::WorldMapScenePositionUpdate update;
    awl::WorldMapPlayerScenePose pose;
    // The constructor registers the node before applying the saved scene
    // type/position. Another node checks ordering on same-key writes.
    expect(registry.register_object(11, 0, start) &&
               registry.register_object(22, 1, start) &&
               registry.update_position(22, start, &update) &&
               registry.update_type_and_position(11, 2, start, &update) &&
               update.previous_bucket == -1 && update.next_bucket == 1 &&
               registry.snapshot(1)[0].scene_type == 2,
           "constructor's saved scene type and position place player node");

    const awl::WorldMapPlayerSceneMessage1F same_key{
        1, {20.0f, 5.0f, 20.0f}, {0.0f, 0.0f, -1.0f}};
    expect(awl::apply_world_map_player_scene_message_1f(
               11, same_key, &registry, &pose, &update) &&
               !update.relink_required && update.next_bucket == 1 &&
               pose.scene_type == 1 && pose.position == same_key.position &&
               pose.heading == same_key.heading &&
               registry.snapshot(1)[0].identity == 11 &&
               registry.snapshot(1)[0].scene_type == 1 &&
               registry.snapshot(1)[1].identity == 22,
           "message updates type, position, and heading without needless relink");

    const awl::WorldMapPlayerSceneMessage1F cross_key{
        2, next, {1.0f, 0.0f, 0.0f}};
    expect(awl::apply_world_map_player_scene_message_1f(
               11, cross_key, &registry, &pose, &update) &&
               update.relink_required && update.previous_bucket == 1 &&
               update.next_bucket == 7 && registry.size(1) == 1 &&
               registry.snapshot(7)[0].identity == 11 &&
               registry.snapshot(7)[0].position == next &&
               pose.heading == cross_key.heading,
           "message crossing exact X/Z thresholds relinks player scene node");

    const awl::WorldMapPlayerSceneMessage1F type_zero{
        0, next, {0.0f, 0.0f, 1.0f}};
    expect(awl::apply_world_map_player_scene_message_1f(
               11, type_zero, &registry, &pose, &update) &&
               update.relink_required && update.previous_bucket == 7 &&
               update.next_bucket == 0 && registry.size(7) == 0 &&
               registry.snapshot(0)[0].scene_type == 0,
           "type-zero message returns player node to bucket zero");

    const auto saved_pose = pose;
    const auto saved_bucket = registry.snapshot(0);
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::apply_world_map_player_scene_message_1f(
               11, {45, next, {1.0f, 0.0f, 0.0f}},
               &registry, &pose, &update) &&
               !awl::apply_world_map_player_scene_message_1f(
                   11, {1, {nan, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
                   &registry, &pose, &update) &&
               !awl::apply_world_map_player_scene_message_1f(
                   11, {1, next, {nan, 0.0f, 0.0f}},
                   &registry, &pose, &update) &&
               !awl::apply_world_map_player_scene_message_1f(
                   99, same_key, &registry, &pose, &update) &&
               !awl::apply_world_map_player_scene_message_1f(
                   11, same_key, nullptr, &pose, &update) &&
               !awl::apply_world_map_player_scene_message_1f(
                   11, same_key, &registry, nullptr, &update) &&
               !awl::apply_world_map_player_scene_message_1f(
                   11, same_key, &registry, &pose, nullptr) &&
               pose.scene_type == saved_pose.scene_type &&
               pose.position == saved_pose.position &&
               pose.heading == saved_pose.heading &&
               registry.snapshot(0)[0].scene_type == saved_bucket[0].scene_type &&
               registry.snapshot(0)[0].position == saved_bucket[0].position &&
               update.next_bucket == 0,
           "unsupported or incomplete messages preserve player pose and bucket");
}

void test_world_map_player_fixed_scene_message_1f() {
    awl::WorldMapSceneBucketRegistry registry;
    awl::WorldMapPlayerScenePose pose;
    awl::WorldMapScenePositionUpdate update;
    const std::array<float, 3> start{120.0f, 5.0f, 168.0f};
    expect(registry.register_object(1, 0, start) &&
               registry.update_position(1, start, &update) &&
               registry.size(0) == 1,
           "player ID one starts in the constructor's type-zero bucket");
    expect(awl::apply_world_map_player_fixed_scene_message_1f(
               &registry, &pose, &update) &&
               update.previous_bucket == 0 && update.next_bucket == 17 &&
               update.relink_required && registry.size(0) == 0 &&
               registry.size(17) == 1 &&
               registry.snapshot(17)[0].identity == 1 &&
               registry.snapshot(17)[0].scene_type == 3 &&
               pose.scene_type == 3 &&
               pose.position == std::array<float, 3>{-1.0f, 0.0f, -5.2f} &&
               pose.heading == std::array<float, 3>{0.0f, 0.0f, 1.0f},
           "verified fixed message moves player ID one to scene type three");
    uint32_t z_bits = 0;
    std::memcpy(&z_bits, &pose.position[2], sizeof(z_bits));
    expect(z_bits == 0xc0a66666u &&
               awl::apply_world_map_player_fixed_scene_message_1f(
                   &registry, &pose, &update) &&
               !update.relink_required && registry.size(17) == 1,
           "fixed payload keeps its DOL Z float and does not relink on repeat");

    awl::WorldMapSceneBucketRegistry wrong_recipient;
    const auto saved_pose = pose;
    expect(wrong_recipient.register_object(2, 0, start) &&
               !awl::apply_world_map_player_fixed_scene_message_1f(
                   &wrong_recipient, &pose, &update) &&
               wrong_recipient.size(-1) == 1 &&
               wrong_recipient.snapshot(-1)[0].scene_type == 0 &&
               pose.scene_type == saved_pose.scene_type &&
               pose.position == saved_pose.position &&
               !awl::apply_world_map_player_fixed_scene_message_1f(
                   nullptr, &pose, &update) &&
               !awl::apply_world_map_player_fixed_scene_message_1f(
                   &registry, nullptr, &update) &&
               !awl::apply_world_map_player_fixed_scene_message_1f(
                   &registry, &pose, nullptr) && update.next_bucket == 0,
           "missing player ID or output rejects the fixed message atomically");
}

void test_world_map_scene_mode_request() {
    awl::WorldMapSceneModeRequestState state;
    state.previous_mode_5c = 4;
    state.state_64 = 8;
    expect(awl::apply_world_map_scene_mode_request(&state, 3, 0) &&
               state.mode_58 == 3 && state.scene_byte_78 == 0 &&
               state.state_64 == 8 && state.global_flag_59af == 1 &&
               state.global_flag_59b0 == 0,
           "step-zero mode three requests the mode without classified flag");

    expect(awl::apply_world_map_scene_mode_request(&state, 6, 7) &&
               state.mode_58 == 6 && state.scene_byte_78 == 7 &&
               state.state_64 == 8 && state.global_flag_59b0 == 1,
           "classified request and previous mode set the second global flag");
    state.global_flag_59b0 = 0;
    state.previous_mode_5c = 3;
    expect(awl::apply_world_map_scene_mode_request(&state, 14, 1) &&
               state.global_flag_59b0 == 0,
           "unclassified previous mode cannot set the second flag");

    state.previous_mode_5c = 14;
    state.state_64 = 0;
    expect(awl::apply_world_map_scene_mode_request(&state, 4, 2) &&
               state.state_64 == 1 && state.global_flag_59b0 == 1,
           "mode four sets state 64 and matches previous mode fourteen");
    state.state_64 = 0;
    state.global_flag_59b0 = 0;
    expect(awl::apply_world_map_scene_mode_request(&state, 13, 3) &&
               state.state_64 == 1 && state.global_flag_59b0 == 0,
           "mode thirteen sets state 64 but is not a classified flag mode");
    state.global_flag_59b0 = 1;
    expect(awl::apply_world_map_scene_mode_request(&state, 99, 0) &&
               state.state_64 == 1 && state.global_flag_59b0 == 1,
           "other modes preserve sticky scene and global state");
    const auto saved = state;
    expect(!awl::apply_world_map_scene_mode_request(nullptr, 4, 0) &&
               state.mode_58 == saved.mode_58 &&
               state.global_flag_59af == saved.global_flag_59af &&
               state.global_flag_59b0 == saved.global_flag_59b0,
           "absent scene-mode state rejects the request");
}

void test_world_map_player_sequence_reset() {
    awl::WorldMapPlayerFixedTransitionStepState state;
    state.scene_byte_79 = 7;
    expect(state.sequence_step_4574 == 2 &&
               awl::reset_world_map_player_sequence_step(&state) &&
               state.sequence_step_4574 == 0 && state.scene_byte_79 == 7,
           "constructor step two resets to zero without touching scene byte");
    state.sequence_step_4574 = 5;
    expect(awl::reset_world_map_player_sequence_step(&state) &&
               state.sequence_step_4574 == 0,
           "sequence step five also resets to zero");
    state.sequence_step_4574 = 1;
    expect(!awl::reset_world_map_player_sequence_step(&state) &&
               state.sequence_step_4574 == 1 &&
               !awl::reset_world_map_player_sequence_step(nullptr),
           "other steps and absent state cannot reset the sequence");
}

void test_world_map_player_fixed_transition_step() {
    awl::WorldMapSceneBucketRegistry registry;
    awl::WorldMapPlayerScenePose pose;
    awl::WorldMapScenePositionUpdate update;
    awl::WorldMapPlayerFixedTransitionStepState state;
    state.sequence_step_4574 = 1;
    state.scene_byte_79 = 7;
    const std::array<float, 3> start{120.0f, 5.0f, 168.0f};
    expect(registry.register_object(1, 0, start) &&
               registry.update_position(1, start, &update) &&
               awl::apply_world_map_player_fixed_transition_step(
                   &state, &registry, &pose, &update) &&
               state.sequence_step_4574 == 2 && state.scene_byte_79 == 0 &&
               update.previous_bucket == 0 && update.next_bucket == 17 &&
               registry.snapshot(17)[0].position == pose.position,
           "sequence step one delivers fixed player pose and advances to two");
    const auto saved_pose = pose;
    expect(!awl::apply_world_map_player_fixed_transition_step(
               &state, &registry, &pose, &update) &&
               state.sequence_step_4574 == 2 && update.next_bucket == 0 &&
               pose.position == saved_pose.position,
           "step two cannot repeat the step-one transition");

    state.sequence_step_4574 = 0;
    expect(!awl::apply_world_map_player_fixed_transition_step(
               &state, &registry, &pose, &update) &&
               state.sequence_step_4574 == 0 &&
               registry.snapshot(17)[0].position == saved_pose.position,
           "step zero keeps its separate scene setup path");

    state.sequence_step_4574 = 1;
    state.scene_byte_79 = 9;
    state.state_680 = 0;
    expect(!awl::apply_world_map_player_fixed_transition_step(
               &state, &registry, &pose, &update) &&
               state.sequence_step_4574 == 1 && state.scene_byte_79 == 9,
           "pending state 680 takes a different, unsupported sequence path");
    state.state_680 = -1;
    state.state_58c = 1;
    expect(!awl::apply_world_map_player_fixed_transition_step(
               &state, &registry, &pose, &update) &&
               state.sequence_step_4574 == 1 && state.scene_byte_79 == 9,
           "nonzero state 58c blocks the sequence update");
    state.state_58c = 0;
    awl::WorldMapSceneBucketRegistry missing_player;
    expect(!awl::apply_world_map_player_fixed_transition_step(
               &state, &missing_player, &pose, &update) &&
               state.sequence_step_4574 == 1 && state.scene_byte_79 == 9 &&
               pose.position == saved_pose.position &&
               registry.snapshot(17)[0].position == saved_pose.position &&
               !awl::apply_world_map_player_fixed_transition_step(
                   nullptr, &registry, &pose, &update) &&
               !awl::apply_world_map_player_fixed_transition_step(
                   &state, &registry, &pose, nullptr),
           "missing player or output cannot half-advance the sequence");
}

void test_world_map_collision_mode_flags() {
    constexpr uint32_t expected[5] = {
        0x67u, 0xd4u, 0x16fu, 0x16fu, 0x14fu};
    for (int32_t mode = 0; mode < 5; ++mode) {
        uint32_t flags = 0;
        expect(awl::world_map_collision_flags_for_mode(mode, &flags) &&
                   flags == expected[mode] &&
                   ((flags & 0x8u) != 0) == (mode >= 2),
               "verified mode flags select the third-list resolver gate");
    }
    uint32_t flags = 99;
    expect(!awl::world_map_collision_flags_for_mode(-1, &flags) &&
               flags == 0 &&
               !awl::world_map_collision_flags_for_mode(5, &flags) &&
               flags == 0 &&
               !awl::world_map_collision_flags_for_mode(0, nullptr),
           "unsupported modes and missing output are rejected");
}

void test_world_map_scene_first_collision_registration() {
    constexpr std::array<int32_t, 5> expected_ids{
        0x2c, 0x2d, 0x2e, 0x2f, 0x30};
    constexpr std::array<int32_t, 5> expected_constructor_ids{
        0x3d, 0x41, 0x3e, 0x3f, 0x40};
    constexpr std::array<std::array<float, 3>, 5> expected_selected_positions{{
        {263.0f, 4.0f, 179.0f}, {277.0f, 4.0f, 218.0f},
        {185.0f, 4.0f, 237.0f}, {75.0f, 4.0f, 113.5f},
        {279.0f, 27.0f, 116.0f},
    }};
    constexpr std::array<float, 5> expected_radii{
        0.3f, 0.3f, 0.3f, 0.9f, 0.3f};
    const auto initial = awl::world_map_first_actor_selection(std::nullopt);
    bool selections_match = initial.actor_id == expected_ids[0] &&
                            initial.base_constructor_id ==
                                expected_constructor_ids[0];
    for (uint32_t draw = 0; draw < 10; ++draw) {
        const size_t index = draw % 5u;
        const auto selected = awl::world_map_first_actor_selection(draw);
        std::array<float, 3> position{};
        selections_match = selections_match &&
            selected.actor_id == expected_ids[index] &&
            selected.base_constructor_id == expected_constructor_ids[index] &&
            awl::world_map_first_actor_initial_position(selected.actor_id,
                                                        4.0f, &position) &&
            position == expected_selected_positions[index] &&
            awl::world_map_first_actor_circle_spec(selected.actor_id).radius ==
                expected_radii[index];
    }
    const auto wrapped = awl::world_map_first_actor_selection(
        std::numeric_limits<uint32_t>::max() - 1u);
    expect(selections_match && wrapped.actor_id == 0x30 &&
               wrapped.base_constructor_id == 0x40,
           "initial and refreshed actor choices feed five verified spawn/circle paths");

    const auto check_spec = [](int32_t id, int32_t mode, float radius) {
        const auto actual = awl::world_map_first_actor_circle_spec(id);
        return actual.mode == mode && actual.radius == radius;
    };
    expect(check_spec(0x15, 1, 0.6f) && check_spec(0x23, 1, 0.6f) &&
               check_spec(0x25, 1, 0.3f) && check_spec(0x27, 2, 0.3f) &&
               check_spec(0x2c, 2, 0.3f) && check_spec(0x2e, 2, 0.3f) &&
               check_spec(0x2f, 2, 0.9f) && check_spec(0x30, 2, 0.3f) &&
               check_spec(0x31, 1, 0.5f),
           "first actor collision setup follows the DOL ID branches and radii");

    const std::array<std::array<float, 3>, 6> expected_positions{{
        {263.0f, 9.0f, 179.0f}, {277.0f, 9.0f, 218.0f},
        {185.0f, 9.0f, 237.0f}, {75.0f, 9.0f, 113.5f},
        {279.0f, 27.0f, 116.0f}, {3.0f, 9.0f, 3.0f},
    }};
    bool positions_match = true;
    for (size_t i = 0; i < expected_positions.size(); ++i) {
        const int32_t id = i == 5 ? 0x31 : 0x2c + static_cast<int32_t>(i);
        std::array<float, 3> position{};
        positions_match = positions_match &&
            awl::world_map_first_actor_initial_position(id, 9.0f,
                                                        &position) &&
            position == expected_positions[i];
    }
    std::array<float, 3> position{99.0f, 98.0f, 97.0f};
    const auto unchanged = position;
    expect(positions_match &&
               awl::world_map_first_actor_initial_position(0x2b, 9.0f,
                                                           &position) &&
               position == expected_positions[5] &&
               awl::world_map_first_actor_initial_position(0x30,
                                                           std::nullopt,
                                                           &position) &&
               position == expected_positions[4],
           "actor IDs select DOL XZ seeds and the height-query exception");
    position = unchanged;
    expect(!awl::world_map_first_actor_initial_position(0x2f,
                                                        std::nullopt,
                                                        &position) &&
               position == unchanged &&
               !awl::world_map_first_actor_initial_position(
                   0x2f, std::numeric_limits<float>::quiet_NaN(),
                   &position) &&
               position == unchanged &&
               !awl::world_map_first_actor_initial_position(0x2f, 9.0f,
                                                            nullptr),
           "missing or invalid required height leaves the output unchanged");

    awl::WorldMapSceneFirstCollisionObjects scene;
    scene.conditional_a.emplace();
    scene.conditional_a->collision.identity = 101;
    scene.conditional_a->collision.category = 2;
    scene.conditional_a->collision.enabled = true;
    scene.conditional_b.emplace();
    scene.conditional_b->collision.identity = 102;
    scene.conditional_b->collision.category = 1;
    scene.category1_actor.collision.identity = 103;
    scene.category1_actor.collision.category = 99;
    scene.category1_actor.collision.collision_flags = 2u;
    scene.category1_actor.world_position = {5.0f, 4.0f, 0.0f};
    scene.category1_actor_id =
        awl::world_map_first_actor_selection(3u).actor_id;
    scene.category1_actor_height_query_result = 4.0f;
    awl::WorldMapCollisionRegistry registry;
    awl::WorldMapRegisteredCollisionObject unrelated;
    unrelated.collision.identity = 80;
    expect(registry.register_object(awl::WorldMapCollisionList::Later, unrelated) &&
               awl::register_world_map_scene_first_collision_objects(
                   scene, &registry),
           "scene constructor first-list objects register from supplied presence");
    auto snapshot = registry.snapshot();
    const auto& first = snapshot.first_resolver;
    expect(first.size() == 3 && first[0].identity == 103 &&
               first[1].identity == 102 && first[2].identity == 101 &&
               first[0].enabled && first[0].category == 1 &&
               first[0].collision_flags == 1u && first[0].radius == 0.9f &&
               first[0].center_world ==
                   std::array<float, 3>{75.0f, 4.0f, 113.5f} &&
               first[0].data == nullptr && first[0].size == 0 &&
               snapshot.first_directional.size() == 3 &&
               snapshot.later_resolver.size() == 1 &&
               snapshot.later_resolver[0].identity == 80,
           "known category-1 circle heads the first list without altering other lists");
    if (first.size() == 3) {
        awl::CollisionDynamicPassAdjustment contact;
        const std::array<float, 3> prior{75.0f, 7.0f, 115.5f};
        const std::array<float, 3> proposal{75.0f, 7.0f, 114.5f};
        expect(awl::resolve_type1_first_dynamic_object_pass(
                   first.data(), first.size(), 999, 1, prior, proposal,
                   0.3f, 4u, 0x67u, &contact) && contact.contact &&
                   contact.queried_objects == 1 &&
                   std::fabs(contact.position[2] - 114.71f) < 0.0001f,
               "registered category-1 actor contributes a circle contact");
    }

    const auto before = registry.snapshot();
    scene.conditional_b->collision.identity = 101;
    expect(!awl::register_world_map_scene_first_collision_objects(
               scene, &registry) &&
               registry.snapshot().first_resolver.size() ==
                   before.first_resolver.size() &&
               registry.snapshot().first_resolver.front().identity == 103,
           "duplicate scene identities reject without changing list order");
    const std::array<float, 3> moved_position{8.0f, 4.0f, 0.0f};
    const std::array<float, 3> moved_heading{1.0f, 0.0f, 0.0f};
    expect(registry.update_circle_object(awl::WorldMapCollisionList::First,
                                         103, moved_position, moved_heading,
                                         true),
           "linked category-1 circle accepts a supplied live pose");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 3 &&
               snapshot.first_resolver[0].identity == 103 &&
               snapshot.first_resolver[1].identity == 102 &&
               snapshot.first_resolver[2].identity == 101 &&
               snapshot.first_resolver[0].center_world == moved_position &&
               snapshot.first_directional[0].world_position == moved_position &&
               snapshot.first_directional[0].heading_axis == moved_heading,
           "pose refresh changes both views without relinking the list");
    if (snapshot.first_resolver.size() == 3) {
        awl::CollisionDynamicPassAdjustment moved_contact;
        const std::array<float, 3> prior{8.0f, 7.0f, 2.0f};
        const std::array<float, 3> proposal{8.0f, 7.0f, 1.0f};
        expect(awl::resolve_type1_first_dynamic_object_pass(
                   snapshot.first_resolver.data(),
                   snapshot.first_resolver.size(), 999, 1, prior, proposal,
                   0.3f, 4u, 0x67u, &moved_contact) &&
                   moved_contact.contact &&
                   std::fabs(moved_contact.position[2] - 1.21f) < 0.0001f,
               "first-list contact follows the refreshed circle center");
    }
    const auto invalid_pose = std::array<float, 3>{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    expect(!registry.update_circle_object(awl::WorldMapCollisionList::First,
                                           103, invalid_pose, moved_heading,
                                           true) &&
               !registry.update_circle_object(awl::WorldMapCollisionList::First,
                                              101, moved_position,
                                              moved_heading, true) &&
               registry.snapshot().first_resolver[0].center_world ==
                   moved_position,
           "invalid pose or non-circle object leaves the first list intact");
    expect(registry.update_circle_object(awl::WorldMapCollisionList::First,
                                         103, moved_position, moved_heading,
                                         false) &&
               !registry.snapshot().first_resolver[0].enabled,
           "inactive circle remains linked in the first list");
    const auto inactive_snapshot = registry.snapshot();
    awl::CollisionDynamicPassAdjustment inactive_contact;
    const std::array<float, 3> prior{8.0f, 7.0f, 2.0f};
    const std::array<float, 3> proposal{8.0f, 7.0f, 1.0f};
    expect(awl::resolve_type1_first_dynamic_object_pass(
               inactive_snapshot.first_resolver.data(),
               inactive_snapshot.first_resolver.size(), 999, 1, prior,
               proposal, 0.3f, 4u, 0x67u, &inactive_contact) &&
               !inactive_contact.contact &&
               inactive_contact.position == proposal,
           "inactive linked circle does not produce contact");
    expect(registry.unregister_object(103) &&
               registry.snapshot().first_resolver.size() == 2 &&
               registry.snapshot().first_resolver[0].identity == 102,
           "actor destruction unlinks the inactive circle");
    scene.conditional_b.reset();
    scene.conditional_a.reset();
    scene.category1_actor.collision.identity = 104;
    scene.category1_actor_id = 0x2c;
    awl::WorldMapCollisionRegistry only_actor;
    expect(awl::register_world_map_scene_first_collision_objects(
               scene, &only_actor) &&
               only_actor.snapshot().first_resolver.size() == 1 &&
               only_actor.snapshot().first_resolver.front().identity == 104 &&
               only_actor.snapshot().first_resolver.front().center_world ==
                   std::array<float, 3>{263.0f, 4.0f, 179.0f} &&
               only_actor.snapshot().first_resolver.front().radius == 0.3f,
           "absent conditional actors leave the required category-1 actor");
    scene.category1_actor.collision.identity = 105;
    scene.category1_actor_id = 0x30;
    scene.category1_actor_height_query_result.reset();
    awl::WorldMapCollisionRegistry fixed_height_actor;
    expect(awl::register_world_map_scene_first_collision_objects(
               scene, &fixed_height_actor) &&
               fixed_height_actor.snapshot().first_resolver.size() == 1 &&
               fixed_height_actor.snapshot().first_resolver.front()
                       .center_world ==
                   std::array<float, 3>{279.0f, 27.0f, 116.0f},
           "ID 0x30 registers at its fixed DOL height without a query result");
    scene.category1_actor_id = 0x2c;
    scene.category1_actor_height_query_result =
        std::numeric_limits<float>::quiet_NaN();
    expect(!awl::register_world_map_scene_first_collision_objects(
               scene, &fixed_height_actor) &&
               fixed_height_actor.snapshot().first_resolver.size() == 1 &&
               fixed_height_actor.snapshot().first_resolver.front().identity ==
                   105 &&
               !awl::register_world_map_scene_first_collision_objects(
                   scene, nullptr),
           "invalid scene height and missing registry are rejected");
}

void test_world_map_first_actor_step() {
    awl::WorldMapFirstActorStepState state;
    state.actor_id = 0x2c;
    state.moving = true;
    state.timer = 17;
    state.proposal = {2.0f, 7.0f, 2.0f};
    state.target = {3.0f, 7.0f, 2.0f};
    state.heading = {1.0f, 0.0f, 0.0f};

    awl::WorldMapFirstActorStepProposal proposed;
    bool speeds_match = true;
    constexpr std::array<float, 5> expected_steps{
        0.03f, 0.06f, 0.04f, 0.02f, 0.01f};
    for (int32_t id = 0x2c; id <= 0x30; ++id) {
        state.actor_id = id;
        speeds_match = speeds_match &&
            awl::propose_world_map_first_actor_step(state, &proposed) &&
            proposed.collision_required && !proposed.target_reached &&
            proposed.state.moving && proposed.state.timer == 17 &&
            std::fabs(proposed.state.proposal[0] -
                      (2.0f + expected_steps[static_cast<size_t>(id - 0x2c)])) <
                0.000001f &&
            proposed.state.proposal[1] == 7.0f &&
            proposed.state.proposal[2] == 2.0f;
    }
    expect(speeds_match,
           "all five actor IDs use their distinct verified speed-table entries");

    state.actor_id = 0x2e;
    state.selector_d4 = 2;
    state.selector_d8 = 2;
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               std::fabs(proposed.state.proposal[0] - 2.06f) < 0.000001f,
           "the paired selector state multiplies the step by 1.5");
    state.selector_d8 = 1;
    state.target = {2.01f, 7.0f, 2.0f};
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               proposed.collision_required && proposed.target_reached &&
               !proposed.state.moving && proposed.state.timer == 0 &&
               proposed.state.proposal == state.proposal,
           "reaching the target clears movement but still requires collision");
    const auto reached = proposed;
    state.target = {2.0f, 7.07f, 2.0f};
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               !proposed.target_reached && proposed.state.moving,
           "target distance includes Y even though contact stopping compares XZ");

    state.target = {3.0f, 7.0f, 2.0f};
    expect(awl::propose_world_map_first_actor_step(state, &proposed),
           "active proposal can be passed to the supplied collision stage");
    auto terrain = make_sample_leaf();
    constexpr uint32_t vertices = 8 + 0x34 + 8;
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        put_be_s16(terrain, vertices + vertex * 8 + 2, 6);
    }
    awl::WorldMapFirstActorStepResult finished;
    expect(awl::finalize_world_map_first_actor_step(
               reached, reached.state.proposal, terrain.data(),
               terrain.size(), &finished) &&
               !finished.state.moving && finished.state.timer == 0 &&
               finished.state.proposal[1] == 6.0f,
           "target-reached branch still resamples the terrain height");
    expect(awl::finalize_world_map_first_actor_step(
               proposed, proposed.state.proposal, terrain.data(),
               terrain.size(), &finished) &&
               !finished.collision_altered_horizontal &&
               finished.state.moving && finished.state.timer == 17 &&
               finished.state.proposal[1] == 6.0f,
           "unchanged collision XZ keeps motion and resamples terrain Y");
    auto adjusted = proposed.state.proposal;
    adjusted[0] += 0.1f;
    expect(awl::finalize_world_map_first_actor_step(
               proposed, adjusted, terrain.data(), terrain.size(),
               &finished) &&
               finished.collision_altered_horizontal &&
               !finished.state.moving && finished.state.timer == 0 &&
               finished.state.proposal[0] == adjusted[0] &&
               finished.state.proposal[1] == 6.0f,
           "horizontal collision change stops motion after terrain resampling");

    const auto prior = finished;
    expect(!awl::finalize_world_map_first_actor_step(
               proposed, adjusted, nullptr, 0, &finished) &&
               finished.state.proposal == prior.state.proposal &&
               finished.state.timer == prior.state.timer &&
               !awl::finalize_world_map_first_actor_step(
                   proposed,
                   {std::numeric_limits<float>::quiet_NaN(), 7.0f, 2.0f},
                   terrain.data(), terrain.size(), &finished) &&
               finished.state.proposal == prior.state.proposal &&
               !awl::finalize_world_map_first_actor_step(
                   proposed, adjusted, terrain.data(), terrain.size(),
                   nullptr),
           "missing terrain, invalid collision, or output rejects atomically");
    state.actor_id = 0x31;
    const auto prior_proposal = proposed;
    expect(!awl::propose_world_map_first_actor_step(state, &proposed) &&
               proposed.state.proposal == prior_proposal.state.proposal &&
               proposed.state.timer == prior_proposal.state.timer,
           "unsupported constructor ID leaves the proposal unchanged");
    state.actor_id = 0x2c;
    state.target[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::propose_world_map_first_actor_step(state, &proposed) &&
               proposed.state.proposal == prior_proposal.state.proposal &&
               !awl::propose_world_map_first_actor_step(state, nullptr),
           "invalid target and missing output reject atomically");
    state.moving = false;
    expect(awl::propose_world_map_first_actor_step(state, &proposed) &&
               !proposed.collision_required &&
               awl::finalize_world_map_first_actor_step(
                   proposed, {}, nullptr, 0, &finished) &&
               !finished.state.moving && finished.state.timer == state.timer &&
               finished.state.proposal == state.proposal,
           "inactive moving branch preserves actor state without terrain access");
}

void test_world_map_first_actor_resolved_step() {
    auto terrain = make_sample_leaf();
    constexpr uint32_t vertices = 8 + 0x34 + 8;
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        put_be_s16(terrain, vertices + vertex * 8 + 2, 6);
    }
    auto static_col = terrain;
    static_col[6] = 0;
    awl::CollisionCategory1MovementQuery query;
    query.terrain_data = terrain.data();
    query.terrain_size = terrain.size();
    query.static_data = static_col.data();
    query.static_size = static_col.size();
    query.source_identity = 10;
    awl::CollisionDynamicPassObject source;
    source.identity = 10;
    source.enabled = true;
    source.category = 1;
    source.collision_flags = 1u;
    source.center_world = {2.0f, 6.0f, 2.0f};
    source.radius = 0.3f;
    query.first_objects = &source;
    query.first_object_count = 1;
    awl::CollisionDynamicPassObject third = source;
    third.identity = 20;
    third.center_world = {2.5f, 6.0f, 2.0f};
    third.radius = 0.4f;
    query.third_objects = &third;
    query.third_object_count = 1;

    awl::WorldMapFirstActorStepState actor;
    actor.actor_id = 0x2c;
    actor.moving = true;
    actor.timer = 17;
    actor.proposal = {2.0f, 7.0f, 2.0f};
    actor.target = {3.0f, 7.0f, 2.0f};
    actor.heading = {1.0f, 0.0f, 0.0f};
    awl::WorldMapFirstActorResolvedStep resolved;
    expect(awl::calculate_world_map_first_actor_step(
               0x2c, actor, query, &resolved) &&
               !resolved.collision.first_pass.contact &&
               resolved.collision.third_pass.contact &&
               resolved.collision.resolver_contact_bits == 8u &&
               resolved.collision.static_contact.surface_mask == 0x4c1u &&
               !resolved.collision.final_height_resampled &&
               resolved.finished.collision_altered_horizontal &&
               !resolved.finished.state.moving &&
               resolved.finished.state.timer == 0 &&
               std::fabs(resolved.finished.state.proposal[0] - 1.79f) <
                   0.0002f &&
               resolved.finished.state.proposal[1] == 6.0f,
           "mode-two actor uses third-list circle contact before terrain and stops");
    query.moving_radius = 0.3f;
    awl::CollisionCategory1MovementAdjustment player_mode;
    expect(awl::resolve_type1_category1_movement_candidate(
               query, actor.proposal, {2.03f, 7.0f, 2.0f},
               &player_mode) &&
               !player_mode.third_pass.contact &&
               (player_mode.resolver_contact_bits & 8u) == 0u &&
               player_mode.static_contact.surface_mask == 0xc1u,
           "player mode zero still skips the actor's third-list branch");

    actor.actor_id = 0x2f;
    expect(awl::calculate_world_map_first_actor_step(
               0x2f, actor, query, &resolved) &&
               std::fabs(resolved.finished.state.proposal[0] - 1.19f) <
                   0.0002f,
           "ID 0x2F selects its larger 0.9 collision radius");
    const auto prior = resolved;
    expect(!awl::calculate_world_map_first_actor_step(
               0x2c, actor, query, &resolved) &&
               resolved.finished.state.proposal ==
                   prior.finished.state.proposal &&
               !awl::calculate_world_map_first_actor_step(
                   0x31, actor, query, &resolved) &&
               !awl::calculate_world_map_first_actor_step(
                   0x2f, actor, query, nullptr),
           "mismatched actor ID and missing output reject without changing state");
    query.third_objects = nullptr;
    expect(!awl::calculate_world_map_first_actor_step(
               0x2f, actor, query, &resolved) &&
               resolved.finished.state.proposal ==
                   prior.finished.state.proposal,
           "missing third-list data rejects the composed actor candidate");
    actor.moving = false;
    query.terrain_data = nullptr;
    expect(awl::calculate_world_map_first_actor_step(
               0x2f, actor, query, &resolved) &&
               !resolved.proposed.collision_required &&
               resolved.finished.state.proposal == actor.proposal &&
               resolved.finished.state.timer == actor.timer,
           "inactive actor bypasses mode-two collision and terrain access");
}

void test_world_map_first_actor_target_selection() {
    std::array<awl::WorldMapFirstActorTargetCandidate, 37> candidates{};
    for (size_t order = 0; order < candidates.size(); ++order) {
        candidates[order].index = static_cast<int32_t>(order + 1);
        candidates[order].position = {100.0f, 0.0f, 0.0f};
    }
    candidates[0].index = 7; // The first DOL table slot is runtime supplied.
    candidates[0].category = 1;
    candidates[0].position = {5.0f, 0.0f, 0.0f};
    candidates[0].relationship_score = 10;
    awl::WorldMapFirstActorTargetState state;
    state.actor_id = 0x2c;
    state.active = true;
    state.mode = 0;
    state.action_timer = 9;
    state.moving = true;
    awl::WorldMapFirstActorTargetDecision chosen;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 0u, &chosen) &&
               chosen.selector_ran && chosen.position_write_allowed &&
               chosen.first_target_index == 7 &&
               chosen.second_target_index == -1 &&
               chosen.first_score == 10u && !chosen.second_score &&
               chosen.state.mode == 1 && chosen.previous_mode == 0 &&
               chosen.handler == awl::WorldMapFirstActorHandler::FirstTarget &&
               chosen.handler_target_index == 7 &&
               chosen.handler_score == 10u &&
               chosen.state.action_timer == 7 && !chosen.state.moving,
           "six-unit scan chooses the first target and rising mode dispatches");

    candidates[1].category = 1;
    candidates[1].position = {2.0f, 0.0f, 0.0f};
    candidates[1].relationship_score = 20;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 13u, &chosen) &&
               chosen.first_target_index == 7 &&
               chosen.second_target_index == 2 &&
               chosen.state.mode == 2 &&
               chosen.handler == awl::WorldMapFirstActorHandler::SecondTarget &&
               chosen.handler_target_index == 2 &&
               chosen.handler_score == 20u &&
               chosen.state.action_timer == 13,
           "three-unit target takes mode two even when six-unit score is better");
    candidates[2].category = 1;
    candidates[2].position = {3.0f, 0.0f, 0.0f};
    candidates[2].relationship_score = 0;
    candidates[3].category = 1;
    candidates[3].position = {6.0f, 0.0f, 0.0f};
    candidates[3].relationship_score = 0;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 0u, &chosen) &&
               chosen.first_target_index == 3 &&
               chosen.second_target_index == 2,
           "exact three distance is excluded from the near scan");
    candidates[2].category = 0;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 0u, &chosen) &&
               chosen.first_target_index == 7 &&
               chosen.second_target_index == 2,
           "exact six distance is excluded from the wider scan");
    candidates[3].category = 0;
    candidates[1].position = {4.0f, 0.0f, 0.0f};
    candidates[1].relationship_score = 10;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 0u, &chosen) &&
               chosen.first_target_index == 7 &&
               chosen.second_target_index == -1,
           "equal relationship scores retain the earlier DOL table entry");

    candidates[0].relationship_score = 50;
    candidates[1].relationship_score = 50;
    state.fallback_timer = 2;
    state.action_timer = 0;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 14u, &chosen) &&
               chosen.first_target_index == -1 &&
               chosen.second_target_index == -1 &&
               chosen.state.mode == 1 &&
               chosen.handler == awl::WorldMapFirstActorHandler::FirstTarget &&
               chosen.handler_target_index == -1 &&
               !chosen.handler_score && chosen.state.action_timer == 7,
           "score 50 is excluded; positive fallback timer still selects mode one");
    state.mode = 2;
    state.action_timer = 5;
    state.fallback_timer = 0;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, std::nullopt, &chosen) &&
               chosen.previous_mode == 2 && chosen.state.mode == 0 &&
               chosen.state.action_timer == 5 && chosen.state.moving &&
               chosen.handler == awl::WorldMapFirstActorHandler::None,
           "lower mode waits while the existing action timer is nonzero");
    state.mode = 0;
    state.action_timer = 1;
    state.fallback_timer = 1;
    state.update_flags = 4u;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, 6u, &chosen) &&
               chosen.state.fallback_timer == 0 &&
               chosen.state.mode == 0 &&
               chosen.handler == awl::WorldMapFirstActorHandler::Idle &&
               chosen.state.action_timer == 13 && !chosen.state.moving,
           "update flag four decrements positive timers before selecting idle");

    state.active = false;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, std::nullopt, &chosen) &&
               !chosen.selector_ran && !chosen.position_write_allowed &&
               chosen.state.action_timer == 1 &&
               chosen.state.fallback_timer == 1,
           "inactive actor skips timer and target work");
    state.active = true;
    state.update_flags = 0x804u;
    expect(awl::select_world_map_first_actor_target(
               state, candidates, std::nullopt, &chosen) &&
               !chosen.selector_ran && chosen.position_write_allowed &&
               chosen.state.action_timer == 1 &&
               chosen.state.fallback_timer == 1,
           "update flag 0x800 bypasses selector and timer decrement");

    state.update_flags = 0;
    state.action_timer = 0;
    const auto prior = chosen;
    expect(!awl::select_world_map_first_actor_target(
               state, candidates, std::nullopt, &chosen) &&
               chosen.state.action_timer == prior.state.action_timer &&
               chosen.selector_ran == prior.selector_ran,
           "required dispatch RNG is not invented");
    candidates[1].index = 99;
    expect(!awl::select_world_map_first_actor_target(
               state, candidates, 0u, &chosen) &&
               chosen.state.action_timer == prior.state.action_timer,
           "unexpected fixed table index rejects atomically");
    candidates[1].index = 2;
    state.actor_id = 0x31;
    expect(!awl::select_world_map_first_actor_target(
               state, candidates, 0u, &chosen) &&
               !awl::select_world_map_first_actor_target(
                   state, candidates, 0u, nullptr),
           "unsupported actor ID and missing output are rejected");
}

void test_world_map_first_actor_action_choice() {
    awl::WorldMapFirstActorTargetDecision selected;
    selected.selector_ran = true;
    selected.handler = awl::WorldMapFirstActorHandler::Idle;
    selected.state.actor_id = 0x2c;
    awl::WorldMapFirstActorActionChoice chosen;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 0u, std::nullopt, &chosen) &&
               chosen.route == awl::WorldMapFirstActorActionRoute::WeightedTable &&
               chosen.table_address == 0x80255fbcu && chosen.table_row == 0 &&
               chosen.action_code == 0 && chosen.action_parameter == 0 &&
               chosen.animation_group == 0,
           "variant-zero idle table uses the DOL's inclusive first row");
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 10u, std::nullopt, &chosen) &&
               chosen.table_row == 0 && chosen.action_code == 0,
           "weighted draw equal to a cumulative bound keeps the earlier row");
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_row == 6 && chosen.action_code == 8,
           "last nonzero idle row covers draw 99");

    selected.state.actor_id = 0x2d;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 40u, std::nullopt, &chosen) &&
               chosen.table_address == 0x80255d7cu &&
               chosen.action_code == 2 && chosen.animation_group == 0,
           "variant-one idle action uses its own DOL table");
    selected.state.actor_id = 0x2e;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 50u, std::nullopt, &chosen) &&
               chosen.table_address == 0x8025595cu &&
               chosen.action_code == 3,
           "variant-two idle action uses its own DOL table");
    selected.state.actor_id = 0x2f;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 3, 99u, 49u, &chosen) &&
               chosen.table_address == 0x80255adcu &&
               chosen.action_code == 11 && chosen.action_parameter == 1 &&
               chosen.animation_group == 5,
           "variant-three prior action three can override weighted idle action");
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 3, 99u, 50u, &chosen) &&
               chosen.action_code == 10 && chosen.animation_group == 4,
           "idle override excludes RNG draw 50");
    selected.state.actor_id = 0x30;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x80255e9cu &&
               chosen.action_code == 1 && chosen.action_parameter == 1 &&
               chosen.animation_group == 4,
           "variant-four action one maps to animation group four");

    selected.handler = awl::WorldMapFirstActorHandler::FirstTarget;
    selected.handler_target_index = 7;
    selected.handler_score = 0u;
    selected.state.actor_id = 0x2c;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, std::nullopt, std::nullopt, &chosen) &&
               chosen.route == awl::WorldMapFirstActorActionRoute::Call8015BDA8 &&
               !chosen.action_code && chosen.table_address == 0,
           "variant-zero first target defers to its untranslated motion routine");
    selected.state.actor_id = 0x2d;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, std::nullopt, std::nullopt, &chosen) &&
               chosen.route == awl::WorldMapFirstActorActionRoute::Call8015B460,
           "zero score selects the variant-one turn routine");
    selected.handler_score = 99u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, std::nullopt, std::nullopt, &chosen) &&
               chosen.route == awl::WorldMapFirstActorActionRoute::Call8015B460,
           "score 99 remains on the variant-one turn route");
    selected.handler_score = 100u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x80255ddcu &&
               chosen.action_code == 5,
           "score above 99 selects the variant-one weighted table");
    selected.state.actor_id = 0x2f;
    selected.handler_score = 61u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x80255bfcu &&
               chosen.action_code == 9,
           "variant-three first target uses the middle threshold table");
    selected.handler_score = 100u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x80255c5cu &&
               chosen.action_code == 10,
           "variant-three first target uses the high threshold table");
    expect(awl::choose_world_map_first_actor_action(
               selected, 10, 0, 0u, std::nullopt, &chosen) &&
               chosen.table_address == 0x80255b3cu &&
               chosen.table_row == 0 && chosen.action_code == 0,
           "action ten selects the sparse table, including its zero-weight row");

    selected.handler = awl::WorldMapFirstActorHandler::SecondTarget;
    selected.state.actor_id = 0x2c;
    selected.handler_score = 0u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x8025613cu &&
               chosen.action_code == 8,
           "zero-score second target uses the variant-zero near table");
    selected.handler_score = 60u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x8025613cu,
           "score 60 remains on the variant-zero low near table");
    selected.handler_score = 61u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x8025619cu,
           "score above 60 selects the variant-zero middle near table");
    selected.handler_score = 100u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 99u, std::nullopt, &chosen) &&
               chosen.table_address == 0x802561fcu &&
               chosen.action_code == 8,
           "score above 99 selects the variant-zero high near table");
    selected.state.actor_id = 0x2e;
    selected.handler_score = 0u;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, std::nullopt, std::nullopt, &chosen) &&
               chosen.route == awl::WorldMapFirstActorActionRoute::Call8015BDA8,
           "variant-two zero-score near target defers to motion routine");
    selected.state.actor_id = 0x2f;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, std::nullopt, std::nullopt, &chosen) &&
               chosen.route == awl::WorldMapFirstActorActionRoute::Call8015B460,
           "variant-three zero-score near target defers to turn routine");

    selected.handler = awl::WorldMapFirstActorHandler::FirstTarget;
    selected.handler_score.reset();
    const auto prior = chosen;
    expect(!awl::choose_world_map_first_actor_action(
               selected, 0, 0, 0u, std::nullopt, &chosen) &&
               chosen.route == prior.route && chosen.table_address == prior.table_address,
           "fallback target without a verified score is rejected atomically");
    selected.handler = awl::WorldMapFirstActorHandler::Idle;
    expect(!awl::choose_world_map_first_actor_action(
               selected, 0, 3, 0u, std::nullopt, &chosen) &&
               !awl::choose_world_map_first_actor_action(
                   selected, 0, 0, std::nullopt, std::nullopt, &chosen) &&
               !awl::choose_world_map_first_actor_action(
                   selected, 0, 0, 0u, std::nullopt, nullptr),
           "required RNG and output are not invented");
}

void test_world_map_first_actor_heading() {
    const auto near = [](float actual, float expected, float tolerance) {
        return std::fabs(actual - expected) < tolerance;
    };
    awl::WorldMapFirstActorHeadingState state;
    state.actor_id = 0x2c;
    state.action_code = 0;
    state.position = {263.0f, 20.0f, 179.0f};
    state.target = {1.0f, 2.0f, 3.0f};
    state.heading = {0.0f, 0.0f, -1.0f};
    state.facing_degrees = 180;
    state.moving = true;
    awl::WorldMapFirstActorHeadingResult built;
    expect(awl::build_world_map_first_actor_heading(
               state, std::nullopt, &built) &&
               !built.target_rebuilt && !built.state.moving &&
               built.previous_facing_degrees == 180 &&
               built.state.target == state.target &&
               built.state.heading == state.heading,
           "nonmoving action copies prior angle and clears moving state");

    state.action_code = 1;
    expect(awl::build_world_map_first_actor_heading(state, 0u, &built) &&
               built.target_rebuilt && built.used_random_angle &&
               built.state.moving &&
               near(built.state.target[0], 263.0f, 0.001f) &&
               near(built.state.target[1], 20.0f, 0.001f) &&
               near(built.state.target[2], 183.0f, 0.001f) &&
               near(built.state.heading[2], 1.0f, 0.001f) &&
               built.state.facing_degrees == 0,
           "variant-zero action one builds a four-unit target at angle zero");
    awl::WorldMapFirstActorTargetDecision selected;
    selected.selector_ran = true;
    selected.handler = awl::WorldMapFirstActorHandler::Idle;
    selected.state.actor_id = 0x2c;
    awl::WorldMapFirstActorActionChoice action;
    expect(awl::choose_world_map_first_actor_action(
               selected, 0, 0, 11u, std::nullopt, &action) &&
               action.action_code == 1,
           "supplied idle decision can choose movement action one");
    if (action.action_code) {
        state.action_code = *action.action_code;
        expect(awl::build_world_map_first_actor_heading(state, 90u, &built) &&
                   built.target_rebuilt &&
                   near(built.state.target[0], 267.0f, 0.001f),
               "selected action one feeds the bounded target and heading path");
    }
    state.actor_id = 0x2d;
    state.position = {277.0f, 20.0f, 218.0f};
    expect(awl::build_world_map_first_actor_heading(state, 90u, &built) &&
               near(built.state.target[0], 279.0f, 0.001f) &&
               near(built.state.target[2], 218.0f, 0.001f) &&
               near(built.state.heading[0], 1.0f, 0.001f),
           "variant-one uses its two-unit radius and X-facing rotation");
    state.actor_id = 0x2e;
    state.action_code = 2;
    state.position = {185.0f, 13.0f, 237.0f};
    expect(awl::build_world_map_first_actor_heading(state, 180u, &built) &&
               near(built.state.target[2], 234.0f, 0.001f) &&
               near(built.state.heading[2], -1.0f, 0.001f),
           "variant-two action two also builds a target");
    state.actor_id = 0x2f;
    state.action_code = 1;
    state.position = {75.0f, 0.0f, 113.5f};
    expect(awl::build_world_map_first_actor_heading(state, 270u, &built) &&
               near(built.state.target[0], 74.0f, 0.001f) &&
               near(built.state.target[2], 113.5f, 0.001f),
           "variant-three uses its one-unit radius");
    state.actor_id = 0x30;
    state.position = {279.0f, 27.0f, 116.0f};
    expect(awl::build_world_map_first_actor_heading(
               state, std::nullopt, &built) && !built.target_rebuilt,
           "variant-four has no target-building action branch");

    state.actor_id = 0x2c;
    state.position = {263.0f, 20.0f, 159.0f};
    expect(awl::build_world_map_first_actor_heading(
               state, std::nullopt, &built) &&
               built.target_rebuilt && !built.used_random_angle &&
               near(built.state.target[2], 163.0f, 0.001f) &&
               built.state.facing_degrees == 0,
           "outside 15 units target direction points toward the anchor");
    state.position = {283.0f, 20.0f, 179.0f};
    expect(awl::build_world_map_first_actor_heading(
               state, std::nullopt, &built) &&
               !built.used_random_angle &&
               near(built.state.target[0], 279.0f, 0.001f) &&
               near(built.state.heading[0], -1.0f, 0.001f),
           "far-anchor negative angle wraps to a westward heading");
    state.position = {263.0f, 36.0f, 179.0f};
    expect(awl::build_world_map_first_actor_heading(
               state, std::nullopt, &built) &&
               !built.used_random_angle &&
               near(built.state.target[1], 36.0f, 0.001f),
           "anchor distance includes vertical separation while target keeps Y");
    state.position = {263.0f, 20.0f, 164.0f};
    expect(awl::build_world_map_first_actor_heading(state, 180u, &built) &&
               built.used_random_angle &&
               near(built.state.target[2], 160.0f, 0.001f),
           "exactly 15 units uses the random-angle path");
    const auto prior = built;
    expect(!awl::build_world_map_first_actor_heading(
               state, std::nullopt, &built) &&
               built.state.target == prior.state.target &&
               built.used_random_angle == prior.used_random_angle,
           "near-anchor path rejects a missing RNG word atomically");
    state.position[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::build_world_map_first_actor_heading(state, 0u, &built),
           "nonfinite actor position is rejected");
    state.position[0] = 263.0f;
    state.actor_id = 0x31;
    expect(!awl::build_world_map_first_actor_heading(state, 0u, &built) &&
               !awl::build_world_map_first_actor_heading(
                   state, 0u, nullptr),
           "unsupported actor and missing result are rejected");
}

void test_world_map_fixed_collision_registration() {
    const auto keys = awl::world_map_fixed_collision_record_keys();
    constexpr std::array<uint32_t, 25> expected_indices{
        18, 22, 17, 21, 16, 20, 15, 19,
        27, 31, 26, 30, 25, 29, 24, 28,
        35, 39, 34, 38, 33, 37, 32, 36, 23};
    bool mapped = true;
    for (size_t i = 0; i < keys.size(); ++i) {
        mapped = mapped && keys[i].archive_index == expected_indices[i] &&
                 keys[i].category == (i == 24 ? 13 : 7);
    }
    expect(mapped, "fixed objects use the verified roomobj indices and categories");
    awl::WorldMapCollisionRecordPools unloaded;
    std::array<awl::WorldMapRegisteredCollisionObject, 25> unbound{};
    unbound[0].collision.identity = 123;
    unbound[0].collision.category = 9;
    expect(!awl::bind_world_map_fixed_collision_records(unloaded, &unbound) &&
               unbound[0].collision.identity == 123 &&
               unbound[0].collision.category == 9 &&
               !awl::bind_world_map_fixed_collision_records(unloaded, nullptr),
           "missing archive or output leaves fixed objects untouched");

    awl::WorldMapFixedCollisionState state;
    state.direct_enabled = {1, 0, 2, 0, 0, 0, 0, 0};
    state.selected_variant = {0, 1, 2, 0, 1, 2, 0, 1};
    state.state_299ae = 3;
    constexpr std::array<bool, 25> expected{
        true, false, true, false, false, false, false, false,
        true, false, false, true, false, false, true, false,
        false, true, false, false, true, false, false, true,
        true};
    expect(awl::world_map_fixed_collision_activation(state) == expected,
           "fixed later-list activation uses direct bytes and exact variant equality");

    std::array<awl::WorldMapRegisteredCollisionObject, 25> objects{};
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i].collision.identity = 100 + i;
        objects[i].collision.category = 1;
        objects[i].collision.collision_flags = 1u;
    }
    awl::WorldMapCollisionRegistry registry;
    awl::WorldMapRegisteredCollisionObject unrelated;
    unrelated.collision.identity = 50;
    unrelated.collision.enabled = true;
    expect(registry.register_object(awl::WorldMapCollisionList::First,
                                    unrelated),
           "unrelated first-list record is present before fixed registration");
    expect(awl::register_world_map_fixed_collision_objects(
               state, objects, &registry),
           "25 fixed objects register from caller-supplied state and records");
    const auto snapshot = registry.snapshot();
    bool ordered = snapshot.later_resolver.size() == 25;
    for (size_t i = 0; ordered && i < 25; ++i) {
        const auto& record = snapshot.later_resolver[i];
        ordered = record.identity == 124 - i &&
                  record.enabled == expected[24 - i] &&
                  record.collision_flags == 3u;
    }
    expect(ordered && snapshot.first_resolver.size() == 1 &&
               snapshot.first_resolver[0].identity == 50,
           "head insertion reverses fixed construction order and leaves other lists intact");

    awl::WorldMapFixedCollisionState updated;
    updated.selected_variant.fill(2);
    expect(awl::register_world_map_fixed_collision_objects(
               updated, objects, &registry) &&
               registry.snapshot().later_resolver.size() == 25 &&
               registry.snapshot().later_resolver.front().identity == 124 &&
               !registry.snapshot().later_resolver.front().enabled &&
               !registry.snapshot().later_resolver.back().enabled,
           "reloading fixed records updates active state without duplicating nodes");

    const auto before = registry.snapshot();
    objects[13].collision.identity = objects[0].collision.identity;
    expect(!awl::register_world_map_fixed_collision_objects(
               state, objects, &registry) &&
               registry.snapshot().later_resolver.size() ==
                   before.later_resolver.size() &&
               registry.snapshot().later_resolver.front().identity ==
                   before.later_resolver.front().identity,
           "duplicate fixed identities fail without changing the registry");
    objects[13].collision.identity = 0;
    expect(!awl::register_world_map_fixed_collision_objects(
               state, objects, &registry) &&
               !awl::register_world_map_fixed_collision_objects(
                   state, objects, nullptr),
           "missing identity and registry are rejected");
}

void test_world_map_collision_registry() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::WorldMapRegisteredCollisionObject first;
    first.collision.identity = 11;
    first.collision.enabled = true;
    first.collision.category = 1;
    first.collision.collision_flags = 2u;
    first.collision.data = bytes.data();
    first.collision.size = bytes.size();
    first.collision.radius = 4.0f;
    first.collision.center_local = {1.0f, 2.0f, 3.0f};
    first.world_position = {4.0f, 5.0f, 6.0f};
    first.heading_axis = {0.0f, 0.0f, -1.0f};
    first.metadata = 41u;
    awl::WorldMapRegisteredCollisionObject second;
    second.collision.identity = 22;
    second.collision.enabled = true;
    second.collision.category = 1;
    second.collision.collision_flags = 1u;
    awl::WorldMapRegisteredCollisionObject later;
    later.collision.identity = 33;
    later.collision.enabled = true;
    later.collision.category = 1;
    later.collision.collision_flags = 1u;
    awl::WorldMapRegisteredCollisionObject third = later;
    third.collision.identity = 44;
    third.collision.center_world = {5.0f, 0.0f, 0.0f};
    third.collision.radius = 0.5f;
    awl::WorldMapCollisionRegistry registry;
    expect(registry.register_object(awl::WorldMapCollisionList::First, first) &&
               registry.register_object(awl::WorldMapCollisionList::First,
                                        second) &&
               registry.register_object(awl::WorldMapCollisionList::Later,
                                        later) &&
               registry.register_object(awl::WorldMapCollisionList::Third,
                                        third),
           "collision objects register into three distinct lists");
    awl::WorldMapCollisionSnapshot snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 2 &&
               snapshot.first_directional.size() == 2 &&
               snapshot.first_resolver[0].identity == 22 &&
               snapshot.first_resolver[1].identity == 11 &&
               snapshot.later_resolver.size() == 1 &&
               snapshot.later_resolver[0].identity == 33 &&
               snapshot.third_resolver.size() == 1 &&
               snapshot.third_resolver[0].identity == 44 &&
               snapshot.third_objects.size() == 1 &&
               snapshot.third_objects[0].collision.identity == 44,
           "first, later, and third lists retain separate traversal order");
    awl::CollisionDynamicPassAdjustment third_contact;
    expect(awl::resolve_type1_third_dynamic_object_pass(
               snapshot.third_resolver.data(), snapshot.third_resolver.size(),
               99, 0, 1, {5.0f, 7.0f, -2.0f}, {5.0f, 9.0f, -0.5f},
               1.0f, 4u, 0x8u, &third_contact) && third_contact.contact &&
               third_contact.resolver_contact_bit == 8u &&
               std::fabs(third_contact.position[2] + 1.51f) < 0.0001f,
           "the third-list snapshot supplies its own ordered contact pass");
    expect(snapshot.first_directional[1].metadata == 41u &&
               snapshot.first_directional[1].world_position ==
                   first.world_position &&
               snapshot.first_directional[1].contact_query.object_radius ==
                   first.collision.radius &&
               snapshot.first_directional[1].data == bytes.data(),
           "one registered entry supplies matching directional and resolver views");

    first.metadata = 42u;
    expect(registry.register_object(awl::WorldMapCollisionList::First, first),
           "registering an existing object moves its node to the front");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 2 &&
               snapshot.first_resolver[0].identity == 11 &&
               snapshot.first_directional[0].metadata == 42u,
           "re-registration replaces the record without duplicating it");

    expect(registry.register_object(awl::WorldMapCollisionList::Third, second),
           "one node can move from first to third list");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 1 &&
               snapshot.third_resolver.size() == 2 &&
               snapshot.third_resolver[0].identity == 22 &&
               snapshot.third_objects.size() == 2 &&
               snapshot.third_objects[0].collision.identity == 22 &&
               snapshot.third_objects[1].collision.identity == 44,
           "cross-list move removes prior membership and leads the third list");
    expect(registry.register_object(awl::WorldMapCollisionList::Later, second),
           "the same node can move from third to later list");
    snapshot = registry.snapshot();
    expect(snapshot.first_resolver.size() == 1 &&
               snapshot.first_resolver[0].identity == 11 &&
               snapshot.later_resolver.size() == 2 &&
               snapshot.later_resolver[0].identity == 22 &&
               snapshot.later_resolver[1].identity == 33,
           "cross-list move removes the old membership and inserts at the new front");
    expect(registry.unregister_object(11) &&
               !registry.unregister_object(11) &&
               registry.size(awl::WorldMapCollisionList::First) == 0,
           "unregister removes one known node only once");
    registry.clear(awl::WorldMapCollisionList::Later);
    expect(registry.size(awl::WorldMapCollisionList::Later) == 0 &&
               registry.snapshot().later_resolver.empty(),
           "clearing a list removes every member");
    registry.clear(awl::WorldMapCollisionList::Third);
    expect(registry.size(awl::WorldMapCollisionList::Third) == 0 &&
               registry.snapshot().third_objects.empty() &&
               registry.snapshot().third_resolver.empty(),
           "clearing the third list does not restore moved nodes");
    first.collision.identity = 0;
    expect(!registry.register_object(awl::WorldMapCollisionList::First, first) &&
               !registry.register_object(
                   static_cast<awl::WorldMapCollisionList>(3), second) &&
               registry.size(awl::WorldMapCollisionList::First) == 0,
           "null identity and unknown list are rejected without mutation");
}

void test_radius_edge_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusEdgeAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               !adjustment.contact && adjustment.position == proposed,
           "unmarked triangle edge does not produce radius contact");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment),
           "marked edge radius query succeeds");
    expect(adjustment.contact && adjustment.surface_flags == 0x2000 &&
               adjustment.triangle_index == 0 && adjustment.edge_index == 0 &&
               adjustment.position[0] == 5.0f &&
               adjustment.position[1] == 7.0f &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "edge pushes the candidate to radius plus 0.01 on its allowed side");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, {5.0f, 7.0f, 0.5f},
               1.0f, &adjustment) &&
               adjustment.contact &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "edge response handles a candidate that crossed the plane");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), {5.0f, 7.0f, 2.0f}, proposed,
               1.0f, &adjustment) && !adjustment.contact,
           "prior point on the other side does not produce edge contact");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, {10.0f, 7.0f, -0.5f},
               1.0f, &adjustment) && !adjustment.contact,
           "edge segment excludes its final endpoint");
    expect(awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 0.5f,
               &adjustment) && !adjustment.contact,
           "candidate exactly at radius has no edge contact");

    bytes[6] = 0;
    expect(!awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment),
           "other collision mode is rejected by the bounded edge helper");
    expect(adjustment.position == std::array<float, 3>{} &&
               !adjustment.contact,
           "failed edge query clears its output");
    bytes[6] = 1;
    expect(!awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative edge radius is rejected");
    expect(!awl::adjust_type1_collision_radius_edge(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null edge query output is rejected");
}

void test_radius_pass_sequence() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionRadiusPassesAdjustment adjustment;
    const std::array<float, 3> prior{5.0f, 7.0f, -2.0f};
    const std::array<float, 3> proposed{5.0f, 7.0f, -0.5f};
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               !adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 1 && adjustment.position == proposed,
           "no radius contact stops after one edge-vertex pass");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] + 1.01f) < 0.0001f,
           "one edge contact is followed by a clear second pass");

    const std::array<float, 3> blocked_prior{0.0f, 7.0f, 2.0f};
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), blocked_prior,
               {0.0f, 7.0f, 0.0f}, 1.0f, &adjustment) &&
               adjustment.contact && adjustment.reverted_to_prior &&
               adjustment.pass_count == 3 &&
               adjustment.position == blocked_prior,
           "persistent vertex contact restores the prior position after three passes");

    bytes = make_one_level_sample_tree();
    constexpr uint32_t payload = 8 + 0x34 * 5;
    put_be16(bytes, payload, 0x2000);
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 7.0f, 10.5f}, 1.0f, &adjustment) &&
               adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[2] - 8.99f) < 0.0001f,
           "edge response crossing into an empty leaf keeps the initial leaf for the next pass");

    put_be_s16(bytes, payload + 8, 11);
    put_be_s16(bytes, payload + 12, 11);
    expect(awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), {10.1f, 7.0f, 11.0f},
               {10.1f, 7.0f, 11.0f}, 1.0f, &adjustment) &&
               adjustment.contact && !adjustment.reverted_to_prior &&
               adjustment.pass_count == 2 &&
               std::fabs(adjustment.position[0] - 9.99f) < 0.0001f,
           "vertex response crossing into an empty leaf keeps the initial leaf for the next pass");
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative sequence radius is rejected");
    expect(!awl::adjust_type1_collision_radius_passes(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null sequence output is rejected");
}

void test_terrain_radius_adjustment() {
    std::vector<uint8_t> bytes = make_sample_leaf();
    awl::CollisionTerrainRadiusAdjustment adjustment;
    const std::array<float, 3> prior{2.0f, 7.0f, 2.0f};
    const std::array<float, 3> proposed{2.0f, 50.0f, 2.0f};
    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 0.0f,
               &adjustment) &&
               adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               adjustment.surface_flags == 0x20 &&
               !adjustment.terrain_contact &&
               !adjustment.initial_edge_fallback &&
               !adjustment.final_edge_fallback &&
               !adjustment.radius_contact && adjustment.radius_pass_count == 0,
           "zero radius keeps the primary height and skips radius passes");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               adjustment.position == std::array<float, 3>{2.0f, 6.0f, 2.0f} &&
               !adjustment.terrain_contact && !adjustment.radius_contact &&
               adjustment.radius_pass_count == 1,
           "clear radius pass keeps the first sampled height");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, {6.0f, 50.0f, -3.0f},
               0.0f, &adjustment) &&
               adjustment.position == std::array<float, 3>{6.0f, 6.0f, 0.0f} &&
               adjustment.terrain_contact && !adjustment.radius_contact &&
               adjustment.initial_edge_fallback &&
               !adjustment.final_edge_fallback,
           "initial miss projects onto the selected leaf edge");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, {6.0f, 50.0f, -3.0f},
               1.0f, &adjustment) && adjustment.terrain_contact &&
               adjustment.initial_edge_fallback && !adjustment.radius_contact &&
               adjustment.radius_pass_count == 1,
           "initial edge fallback remains terrain contact after a clear radius pass");

    put_be16(bytes, 8 + 0x34, 0x2000);
    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), {0.5f, 7.0f, 0.0f},
               {0.5f, 50.0f, 0.0f}, 1.0f, &adjustment) &&
               adjustment.terrain_contact && adjustment.radius_contact &&
               !adjustment.reverted_to_prior &&
               adjustment.radius_pass_count == 2 &&
               std::fabs(adjustment.position[0] - 1.01f) < 0.0001f &&
               std::fabs(adjustment.position[1] - 1.01f) < 0.0001f &&
               !adjustment.final_edge_fallback,
           "vertex contact resamples height at the adjusted X/Z");

    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), {0.0f, 7.0f, 2.0f},
               {0.0f, 50.0f, 0.0f}, 1.0f, &adjustment) &&
               adjustment.terrain_contact && adjustment.radius_contact &&
               adjustment.reverted_to_prior &&
               adjustment.radius_pass_count == 3 &&
               adjustment.position == std::array<float, 3>{0.0f, 4.0f, 2.0f},
           "third-pass revert resamples terrain height at the prior X/Z");

    bytes = make_one_level_sample_tree();
    constexpr uint32_t payload = 8 + 0x34 * 5;
    put_be16(bytes, payload, 0x2000);
    expect(awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), {15.0f, 7.0f, 9.0f},
               {15.0f, 50.0f, 10.5f}, 1.0f, &adjustment) &&
               adjustment.terrain_contact && adjustment.radius_contact &&
               adjustment.radius_pass_count == 2 &&
               adjustment.final_edge_fallback &&
               adjustment.position == std::array<float, 3>{15.0f, 5.0f, 10.0f},
           "post-contact height fallback stays in the original leaf after crossing");

    bytes = make_single_leaf();
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment) &&
               adjustment.position == std::array<float, 3>{} &&
               !adjustment.terrain_contact,
           "empty selected leaf fails and clears its output");
    bytes = make_sample_leaf();
    bytes[6] = 0;
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               &adjustment),
           "untranslated radius mode is rejected");
    bytes[6] = 1;
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, -1.0f,
               &adjustment),
           "negative radius is rejected");
    expect(!awl::adjust_type1_collision_terrain_with_radius(
               bytes.data(), bytes.size(), prior, proposed, 1.0f,
               nullptr),
           "null terrain radius output is rejected");
}

bool inspect_local_asset(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "Unable to open collision asset: %s\n", path);
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    awl::CollisionTreeAnalysis analysis;
    if (!analyze(bytes, analysis)) {
        std::fprintf(stderr, "Collision asset validation failed: %s\n", path);
        return false;
    }
    std::printf("Collision asset validated: %s size=%zu nodes=%zu leaves=%zu "
                "triangles=%zu vertices=%zu depth=%u header=%u/%u/%u/%u\n",
                path, bytes.size(), analysis.node_count, analysis.leaf_count,
                analysis.triangle_count, analysis.vertex_count,
                analysis.max_depth, analysis.format, analysis.header_byte_5,
                analysis.header_byte_6, analysis.header_byte_7);
    const float sample_x = (analysis.root_min[0] + analysis.root_max[0]) * 0.5f;
    const float sample_z = (analysis.root_min[2] + analysis.root_max[2]) * 0.5f;
    awl::CollisionSurfaceSample sample;
    if (awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                            sample_x, sample_z, &sample)) {
        std::printf("Primary surface sample: x=%.3f z=%.3f y=%.3f flags=0x%04X "
                    "leaf=%u triangle=%u\n",
                    sample_x, sample_z, sample.height, sample.surface_flags,
                    sample.leaf_offset, sample.triangle_index);
    } else {
        std::printf("No primary surface sample at root center: x=%.3f z=%.3f\n",
                    sample_x, sample_z);
    }
    awl::CollisionEdgeSample edge_sample;
    const float outside_x = analysis.root_min[0] - 1.0f;
    if (!awl::project_type1_collision_to_edge(
            bytes.data(), bytes.size(), outside_x, sample_z, &edge_sample)) {
        std::fprintf(stderr, "No edge fallback near root boundary: %s\n", path);
        return false;
    }
    std::printf("Edge fallback sample: query=(%.3f, %.3f) "
                "position=(%.3f, %.3f, %.3f) flags=0x%04X "
                "leaf=%u triangle=%u edge=%u\n",
                outside_x, sample_z, edge_sample.position[0],
                edge_sample.position[1], edge_sample.position[2],
                edge_sample.surface_flags, edge_sample.leaf_offset,
                edge_sample.triangle_index, edge_sample.edge_index);
    awl::CollisionTerrainAdjustment adjustment;
    if (!awl::adjust_type1_collision_terrain_height(
            bytes.data(), bytes.size(), {outside_x, 100.0f, sample_z},
            &adjustment) ||
        !adjustment.used_edge_fallback ||
        adjustment.position != edge_sample.position ||
        adjustment.surface_flags != edge_sample.surface_flags) {
        std::fprintf(stderr, "Terrain height adjustment failed: %s\n", path);
        return false;
    }
    awl::CollisionResolverHeightAdjustment resolver_height;
    if (!awl::resample_type1_collision_resolver_height(
            bytes.data(), bytes.size(),
            {outside_x, 100.0f, sample_z}, &resolver_height) ||
        !resolver_height.used_edge_fallback ||
        resolver_height.position !=
            std::array<float, 3>{outside_x, edge_sample.position[1], sample_z}) {
        std::fprintf(stderr, "Resolver height resample failed: %s\n", path);
        return false;
    }
    awl::CollisionRadiusVertexAdjustment vertex_adjustment;
    if (!awl::adjust_type1_collision_radius_vertex(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z}, 0.3f,
            &vertex_adjustment) ||
        !std::isfinite(vertex_adjustment.position[0]) ||
        !std::isfinite(vertex_adjustment.position[1]) ||
        !std::isfinite(vertex_adjustment.position[2])) {
        std::fprintf(stderr, "Radius vertex query failed: %s\n", path);
        return false;
    }
    awl::CollisionRadiusEdgeAdjustment edge_adjustment;
    if (!awl::adjust_type1_collision_radius_edge(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z - 1.0f},
            {sample_x, sample.height, sample_z}, 0.3f,
            &edge_adjustment) ||
        !std::isfinite(edge_adjustment.position[0]) ||
        !std::isfinite(edge_adjustment.position[1]) ||
        !std::isfinite(edge_adjustment.position[2])) {
        std::fprintf(stderr, "Radius edge query failed: %s\n", path);
        return false;
    }
    awl::CollisionRadiusPassesAdjustment passes;
    if (!awl::adjust_type1_collision_radius_passes(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z - 1.0f},
            {sample_x, sample.height, sample_z}, 0.3f,
            &passes) ||
        !std::isfinite(passes.position[0]) ||
        !std::isfinite(passes.position[1]) ||
        !std::isfinite(passes.position[2])) {
        std::fprintf(stderr, "Radius pass sequence failed: %s\n", path);
        return false;
    }
    awl::CollisionTerrainRadiusAdjustment terrain_radius;
    if (!awl::adjust_type1_collision_terrain_with_radius(
            bytes.data(), bytes.size(),
            {sample_x, sample.height, sample_z},
            {sample_x, sample.height, sample_z}, 0.3f,
            &terrain_radius) ||
        !std::isfinite(terrain_radius.position[0]) ||
        !std::isfinite(terrain_radius.position[1]) ||
        !std::isfinite(terrain_radius.position[2])) {
        std::fprintf(stderr, "Terrain radius branch failed: %s\n", path);
        return false;
    }
    awl::CollisionCategory1MovementQuery movement_query;
    movement_query.terrain_data = bytes.data();
    movement_query.terrain_size = bytes.size();
    movement_query.moving_radius = 0.3f;
    awl::CollisionCategory1MovementAdjustment movement;
    const std::array<float, 3> route_point{
        sample_x, sample.height, sample_z};
    if (!awl::resolve_type1_category1_movement_candidate(
            movement_query, route_point, route_point, &movement) ||
        movement.position != terrain_radius.position ||
        movement.resolver_contact_bits !=
            (terrain_radius.terrain_contact ? 1u : 0u)) {
        std::fprintf(stderr, "Category-1 candidate composition failed: %s\n",
                     path);
        return false;
    }
    awl::WorldMapMovementQuery sequence;
    sequence.pad.stick_x = 80;
    sequence.current_position = route_point;
    sequence.current_axis = {0.0f, 0.0f, 1.0f};
    sequence.collision.terrain_data = bytes.data();
    sequence.collision.terrain_size = bytes.size();
    awl::WorldMapMovementCandidate candidate;
    awl::CollisionSurfaceSample moved_surface;
    if (!awl::calculate_world_map_movement_candidate(
            sequence, &candidate) || !candidate.movement_enabled ||
        candidate.proposed_position[0] <= route_point[0] ||
        !awl::sample_type1_collision_surface(
            bytes.data(), bytes.size(), candidate.resolved_position[0],
            candidate.resolved_position[2], &moved_surface) ||
        std::fabs(candidate.resolved_position[1] - moved_surface.height) >
            0.0001f) {
        std::fprintf(stderr, "Movement sequence at local sample failed: %s\n",
                     path);
        return false;
    }
    return true;
}

bool check_local_trigger_asset(const char* disc_root) {
    awl_memory_init();
    awl::filesystem_init();
    awl::WorldMapTriggerAsset asset;
    bool valid = awl::filesystem_mount("/", disc_root) && asset.load();
    const auto* region = asset.category1_polygon(0);
    const auto* crossing = asset.category1_polygon(1);
    valid = valid && region != nullptr && crossing != nullptr &&
            region->mode == 0 && region->vertices.size() == 5 &&
            crossing->mode == 3 && crossing->vertices.size() == 2 &&
            region->vertices.front() == region->vertices.back();
    if (valid) {
        float min_x = region->vertices.front()[0];
        float max_x = min_x;
        float min_z = region->vertices.front()[2];
        float max_z = min_z;
        for (const auto& vertex : region->vertices) {
            min_x = std::min(min_x, vertex[0]);
            max_x = std::max(max_x, vertex[0]);
            min_z = std::min(min_z, vertex[2]);
            max_z = std::max(max_z, vertex[2]);
        }
        const std::array<float, 3> center{
            (min_x + max_x) * 0.5f, 0.0f, (min_z + max_z) * 0.5f};
        const std::array<float, 3> outside{min_x - 1.0f, 0.0f, center[2]};
        bool contact = false;
        valid = max_x > min_x && max_z > min_z &&
                asset.query_category1_contact(0, center, center, &contact) &&
                contact &&
                asset.query_category1_contact(0, outside, outside, &contact) &&
                !contact;
        TriggerRequestProbe requests;
        requests.accepted[0] = true;
        awl::WorldMapMovementContactTail tail;
        valid = valid && asset.evaluate_movement_contact_tail(
                             1, center, outside, request_trigger_state,
                             &requests, &tail) &&
                tail.accepted_slot == 0 && tail.polygon_queries == 1 &&
                requests.calls == 1 && requests.called_slots[0] == 0;
        const auto& a = crossing->vertices[0];
        const auto& b = crossing->vertices[1];
        const float edge_x = b[0] - a[0];
        const float edge_z = b[2] - a[2];
        const float length = std::sqrt(edge_x * edge_x + edge_z * edge_z);
        if (valid && length > 0.0f && std::isfinite(length)) {
            const float move_x = edge_z / length;
            const float move_z = -edge_x / length;
            const std::array<float, 3> prior{
                (a[0] + b[0]) * 0.5f - move_x * 0.5f, 0.0f,
                (a[2] + b[2]) * 0.5f - move_z * 0.5f};
            const std::array<float, 3> resolved{
                prior[0] + move_x, 0.0f, prior[2] + move_z};
            valid = asset.query_category1_contact(
                        1, prior, resolved, &contact) && contact &&
                    asset.query_category1_contact(
                        1, resolved, prior, &contact) && !contact;
        } else {
            valid = false;
        }
    }
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    if (!valid) {
        std::fprintf(stderr, "Local trigger.spl validation failed\n");
    } else {
        std::puts("Local trigger.spl: two category-1 polygons and ordered tail probe passed.");
    }
    return valid;
}

bool check_local_event_conditions(const char* disc_root) {
    awl_memory_init();
    awl::filesystem_init();
    bool valid = awl::filesystem_mount("/", disc_root);
    awl::WorldMapEventConditions conditions;
    awl::WorldMapGlobalActionArchive actions;
    valid = valid && actions.load() && actions.node_count() == 106;
    // Diagnostic bank choice; live descriptor/group/model ownership is absent.
    std::ifstream animation_input(std::filesystem::path(disc_root) / "files" / "boy_0.anm.arc", std::ios::binary);
    std::vector<uint8_t> animation_bytes((std::istreambuf_iterator<char>(animation_input)), {});
    awl::WorldMapAnimationBank animation_bank;
    valid = valid && animation_bank.parse(200, std::move(animation_bytes));
    std::array<uint8_t, 128> unset_saved_bytes{};
    const awl::WorldMapPackedSavedValues unset_saved{
        unset_saved_bytes.data(), unset_saved_bytes.size(), 1};
    for (uint32_t phase = 0; valid && phase < 6; ++phase) {
        valid = conditions.load_phase(phase) && conditions.loaded() &&
                conditions.phase() == static_cast<int32_t>(phase) &&
                conditions.entry_count() == 10;
        size_t payload_bytes = 0;
        for (size_t entry_index = 0;
             valid && entry_index < conditions.entry_count(); ++entry_index) {
            awl::WorldMapEventConditionEntry entry;
            valid = conditions.entry(entry_index, &entry) &&
                    entry.data != nullptr && entry.size != 0 &&
                    entry.name != nullptr && entry.name[0] != 0;
            if (valid) {
                payload_bytes += entry.size;
            }
        }
        for (int32_t slot = 0; valid && slot < 2; ++slot) {
            std::vector<awl::WorldMapMovementRequestRecord> selected;
            valid = conditions.select_movement_request_records(
                        slot, unset_saved, &selected) &&
                    selected.size() ==
                        (slot == 0 ? 1u : (phase == 0 ? 0u : 2u));
            if (valid && slot == 0) {
                awl::WorldMapMovementRecordDecision decision;
                valid = awl::evaluate_world_map_movement_record(
                            selected[0], 0, &decision) ==
                            awl::WorldMapMovementRecordStatus::
                                EligibleForStateRequest &&
                        decision.action_index ==
                            selected[0].saved_condition_id &&
                        decision.decoder_flag == 1 &&
                        awl::evaluate_world_map_movement_record(
                            selected[0], 1, &decision) ==
                            awl::WorldMapMovementRecordStatus::Rejected;
            }
            if (valid && slot == 1 && !selected.empty()) {
                awl::WorldMapMovementRecordDecision decision;
                awl::WorldMapMovementEvaluationState time;
                time.has_time_state = true;
                valid = awl::evaluate_world_map_movement_record(
                            selected[0], 0, &decision) ==
                        awl::WorldMapMovementRecordStatus::
                            RequiresUntranslatedPredicate;
                time.clock_ticks = 19u * 36000u;
                valid = valid &&
                    awl::evaluate_world_map_movement_record(
                        selected[0], time, &decision) ==
                        awl::WorldMapMovementRecordStatus::
                            EligibleForStateRequest &&
                    decision.action_index == 0x178u &&
                    awl::evaluate_world_map_movement_record(
                        selected[1], time, &decision) ==
                        awl::WorldMapMovementRecordStatus::Rejected;
                time.clock_ticks = 2u * 36000u;
                valid = valid &&
                    awl::evaluate_world_map_movement_record(
                        selected[0], time, &decision) ==
                        awl::WorldMapMovementRecordStatus::Rejected &&
                    awl::evaluate_world_map_movement_record(
                        selected[1], time, &decision) ==
                        awl::WorldMapMovementRecordStatus::
                            EligibleForStateRequest;
                time.clock_ticks = 12u * 36000u;
                valid = valid &&
                    awl::evaluate_world_map_movement_record(
                        selected[0], time, &decision) ==
                        awl::WorldMapMovementRecordStatus::Rejected &&
                    awl::evaluate_world_map_movement_record(
                        selected[1], time, &decision) ==
                        awl::WorldMapMovementRecordStatus::Rejected;
            }
        }
        if (valid) {
            awl::WorldMapMovementEvaluationState state;
            state.has_time_state = true;
            awl::WorldMapMovementRequestPreparation prepared;
            std::vector<uint8_t> action_bytes;
            auto check_script = [&animation_bank](const std::vector<uint8_t>& bytes,
                                   uint32_t code_count, uint32_t string_count) {
                awl::WorldMapActionScript script;
                awl::WorldMapActionScriptState reset;
                awl::WorldMapActionInstruction instruction;
                bool valid_script = script.parse(bytes) &&
                       script.instruction_count() == code_count &&
                       script.string_count() == string_count && script.options() == 7 &&
                       script.instruction(0, &instruction) &&
                       script.instruction(code_count - 1, &instruction) &&
                       !script.instruction(code_count, &instruction) &&
                       script.initialize_state(0, &reset) && reset.state_4 == 1 &&
                       reset.instruction_index_0c == 0 &&
                       reset.instruction_count_10 == code_count &&
                       reset.string_count_14 == string_count && reset.options_1b4 == 7;
                using Step = awl::WorldMapActionStepStatus;
                awl::WorldMapActionStep boundary;
                Step status = Step::Advanced;
                size_t steps = 0;
                while (valid_script && steps < 256) {
                    status = script.step(&reset, &boundary);
                    if (status != Step::Advanced) {
                        break;
                    }
                    ++steps;
                }
                // Independently walked DOL-derived prefix expectations. Only
                // counts/digests are public; actual instructions stay local.
                const size_t expected_steps = code_count == 1105 ? 12u : 1u;
                const uint8_t expected_depth = code_count == 1105 ? 0u : 1u;
                const uint64_t expected_digest = code_count == 1105
                    ? 0x54b882e344b5dce9ull : 0x74b4429a2fdd70e5ull;
                uint64_t digest = 14695981039346656037ull;
                auto hash_word = [&digest](uint32_t value) {
                    for (const uint32_t shift : {24u, 16u, 8u, 0u}) {
                        digest = (digest ^ ((value >> shift) & 255u)) * 1099511628211ull;
                    }
                };
                for (const uint32_t value : reset.stack_20) hash_word(value);
                for (const uint32_t value : reset.variables_1b8) hash_word(value);
                const auto before_callback = reset;
                valid_script = valid_script && status == Step::RequiresCallback &&
                    steps == expected_steps && reset.instruction_index_0c == expected_steps &&
                    boundary.instruction_index == expected_steps && boundary.opcode == 0x25 &&
                    reset.stack_depth_1b0 == expected_depth && digest == expected_digest &&
                    script.step(&reset, &boundary) == Step::RequiresCallback &&
                    same_action_state(reset, before_callback);
                auto after_callback = before_callback;
                ++after_callback.instruction_index_0c;
                after_callback.stack_depth_1b0 = 0;
                valid_script = valid_script &&
                    script.step_world_map(&reset, &boundary) ==
                        (code_count == 1105 ? Step::Yielded : Step::Advanced) &&
                    boundary.effective_operand == (code_count == 1105 ? 0u : 66u) &&
                    boundary.callback_arguments[0] == 0 &&
                    same_action_state(reset, after_callback);
                // Command zero ended its pass. This is an explicit later
                // supplied pass, without claiming the live scheduler is wired.
                steps = 0;
                while (valid_script && steps < 256) {
                    status = script.step_world_map(&reset, &boundary);
                    if (status != Step::Advanced) {
                        break;
                    }
                    ++steps;
                }
                const uint32_t next_pc = code_count == 1105 ? 18u : 5u;
                const uint32_t next_command = code_count == 1105 ? 4u : 65u;
                const uint8_t next_depth = code_count == 1105 ? 4u : 3u;
                const uint64_t next_digest = code_count == 1105
                    ? 0xed879127a00137d9ull : 0x17d004121cb12425ull;
                digest = 14695981039346656037ull;
                for (const uint32_t value : reset.stack_20) hash_word(value);
                for (const uint32_t value : reset.variables_1b8) hash_word(value);
                const auto next_boundary = reset;
                valid_script = valid_script && status == Step::RequiresCallback &&
                    reset.state_4 == 1 && reset.instruction_index_0c == next_pc &&
                    boundary.effective_operand == next_command &&
                    reset.stack_depth_1b0 == next_depth && digest == next_digest &&
                    script.step_world_map(&reset, &boundary) == Step::RequiresCallback &&
                    same_action_state(reset, next_boundary);
                awl::WorldMapCommand4Snapshot manager;
                awl::WorldMapCommandPreparation command;
                valid_script = valid_script && script.prepare_world_map_command(
                    reset, &manager, &command) ==
                        awl::WorldMapCommandPreparationStatus::Prepared &&
                    same_action_state(reset, next_boundary) &&
                    command.after_arguments.instruction_index_0c == next_pc + 1 &&
                    command.after_arguments.stack_depth_1b0 == (code_count == 1105 ? 1u : 0u);
                digest = 14695981039346656037ull;
                for (const uint32_t value : command.instruction.callback_arguments) hash_word(value);
                const uint64_t expected_argument_digest = code_count == 1105
                    ? 0xb46eaf0a6bd16bebull : 0x49deeaf2e16b477aull;
                valid_script = valid_script && digest == expected_argument_digest &&
                    command.effect == (code_count == 1105
                        ? awl::WorldMapCommandEffect::BeginRequest4
                        : awl::WorldMapCommandEffect::LevelTransition65) &&
                    !command.has_stack_result;
                // The real script prefix, with explicitly supplied manager
                // fields. This does not claim a live request was started.
                auto unresolved_request = next_boundary;
                valid_script = valid_script && script.step_world_map_request(
                    &unresolved_request, &manager, &boundary) == Step::RequiresCallback &&
                    same_action_state(unresolved_request, next_boundary) &&
                    manager.key_530 == UINT32_MAX && manager.key_534 == UINT32_MAX;
                if (valid_script && code_count == 1105) {
                    // The verified command-4 prefix selects a real MES entry.
                    // Only opaque bytes are owned; presentation remains blocked.
                    awl::WorldMapMessageKey message_key;
                    awl::WorldMapMessageBank message_bank;
                    awl::WorldMapMessageBounds message_bounds;
                    awl::WorldMapMessageStream message_stream;
                    awl::WorldMapSelectionRows message_rows;
                    awl::WorldMapStagedMessage staged_message;
                    awl::WorldMapPresentationWindow presentation_window;
                    awl::WorldMapPresentationResume presentation_resume;
                    awl::WorldMapPresentationAdvance presentation_advance;
                    std::vector<uint8_t> message_bytes;
                    valid_script = awl::resolve_world_map_message_key(
                        command.instruction.callback_arguments[0],
                        command.instruction.callback_arguments[1], &message_key) &&
                        message_key.bank == 5 && message_key.index == 71 &&
                        message_bank.load(message_key.bank) && message_bank.entry_count() == 107 &&
                        message_bank.entry_bounds(message_key.index, &message_bounds) &&
                        message_bounds.offset == 5428 && message_bounds.size == 76 &&
                        message_bank.copy_entry(message_key.index, &message_bytes) && message_bytes.size() == 76 &&
                        message_bank.scan_entry(message_key.index, &message_stream) ==
                            awl::WorldMapMessageStreamStatus::Decoded &&
                        message_stream.consumed_bytes == 73 && message_stream.tokens.size() == 41 &&
                        message_bank.stage_entry(message_key.index, {}, 73, &staged_message) ==
                            awl::WorldMapMessageStagingStatus::Prepared &&
                        staged_message.consumed_bytes == 73 && staged_message.context_expansions == 0 &&
                        staged_message.numeric_expansions == 0 &&
                        staged_message.bytes.size() == 73 &&
                        std::memcmp(staged_message.bytes.data(), message_bytes.data(), 73) == 0 &&
                        awl::prepare_world_map_selection_rows(staged_message.bytes.data(), staged_message.bytes.size(),
                                                             2, &message_rows) ==
                            awl::WorldMapSelectionRowsStatus::Prepared &&
                        message_rows.row_count == 2 && message_rows.max_width_units == 20 &&
                        message_rows.byte_budget == 74 && message_rows.aligned_storage_size == 76 &&
                        message_rows.stop_token_offset == 72 && message_rows.bytes.size() == 73 &&
                        message_rows.width == 528 && message_rows.height == 64 &&
                        awl::count_world_map_presentation_window(staged_message.bytes.data(), staged_message.bytes.size(),
                            0, &presentation_window) == awl::WorldMapPresentationDataStatus::Prepared &&
                        presentation_window.units == 38 && presentation_window.passes[0].units == 18 &&
                        presentation_window.passes[0].stop_offset == 33 && presentation_window.passes[0].next_offset == 34 &&
                        presentation_window.passes[1].units == 20 && presentation_window.passes[1].next_offset == 72 &&
                        presentation_window.passes[2].units == 0 && presentation_window.passes[2].next_offset == 72 &&
                        awl::resume_world_map_presentation_data(staged_message.bytes.data(), staged_message.bytes.size(),
                            0, &presentation_resume) == awl::WorldMapPresentationDataStatus::Prepared &&
                        !presentation_resume.skipped_separator && presentation_resume.start_offset == 0 &&
                        presentation_resume.window.units == 38 &&
                        awl::advance_world_map_presentation_data(staged_message.bytes.data(), staged_message.bytes.size(),
                            0, 38, &presentation_advance) == awl::WorldMapPresentationDataStatus::Prepared &&
                        presentation_advance.start_offset == 34 && presentation_advance.dropped_units == 18 &&
                        presentation_advance.remaining_revealed_units == 20 && presentation_advance.window.units == 20;
                    // Supplied fast reading with no actor/resource is a data
                    // rehearsal. Matching input stops before feedback; it does
                    // not execute or complete the parent request.
                    awl::WorldMapPresentationState presentation;
                    presentation.display_offset_1c = presentation.resume_offset_20 = 0;
                    presentation.window_units_28 = presentation_window.units;
                    presentation.fast_36 = presentation.lock_fast_37 = 1;
                    awl::WorldMapPresentationStep presentation_step;
                    // A separate supplied actor-bearing rehearsal stops at
                    // command dispatch before the tag-0x30 member change.
                    auto actor_presentation = presentation;
                    actor_presentation.manager_resource_60 = 0;
                    awl::WorldMapPresentationStep actor_progression;
                    awl::WorldMapPresentationActorSnapshot actor_snapshot;
                    actor_snapshot.registrations = std::vector<awl::WorldMapPresentationActorRegistration>{{0, 11}};
                    awl::WorldMapPresentationActorStep actor_step;
                    valid_script = valid_script && awl::advance_world_map_presentation(staged_message.bytes.data(),
                        staged_message.bytes.size(), &actor_presentation, 1, 0, 0, &actor_progression) ==
                            awl::WorldMapPresentationStatus::RequiresActorEffects && actor_progression.actor_mode == 0u &&
                        actor_progression.after.revealed_units_24 == 38 && actor_progression.after.resume_offset_20 == 72 &&
                        actor_progression.phase_after_effects == awl::WorldMapPresentationPhase::InputWait &&
                        awl::prepare_world_map_presentation_actor(actor_presentation.manager_resource_60,
                            *actor_progression.actor_mode, actor_snapshot, &actor_step) ==
                                awl::WorldMapPresentationActorStatus::RequiresActorCalls &&
                        actor_step.call_count == 2 && actor_step.calls[0].kind ==
                            awl::WorldMapPresentationActorCallKind::DirectCommand &&
                        actor_step.calls[0].arguments == std::array<uint32_t, 5>{0, 0, 0, 0, 0} &&
                        actor_step.calls[1].kind == awl::WorldMapPresentationActorCallKind::Toggle &&
                        actor_step.calls[1].arguments[0] == 0 &&
                        actor_presentation.phase == awl::WorldMapPresentationPhase::Reading &&
                        actor_presentation.resume_offset_20 == 0 && actor_presentation.revealed_units_24 == 0;
                    // Constructor selection is still missing. A separately
                    // supplied descriptor/binding reaches first model setup,
                    // preserving both actual animation and the later toggle.
                    awl::WorldMapActorAnimationState actor_animation;
                    actor_animation.current_descriptor_0 = actor_animation.base_descriptor_4 = 1;
                    actor_animation.completed_20 = actor_animation.flag_21 = 1;
                    actor_animation.restart_count_2c = 7;
                    actor_animation.model_identity_30 = 100;
                    awl::WorldMapActorAnimationStep animation_step;
                    awl::WorldMapActorPresentationToggleState actor_toggle{0x3a, 1, 9, 3.0f};
                    valid_script = valid_script && awl::prepare_world_map_actor_animation_start(actor_animation, 2,
                        std::nullopt, std::nullopt, &animation_step) == awl::WorldMapActorAnimationStatus::RequiresDescriptor &&
                        awl::prepare_world_map_actor_animation_start(actor_animation, 2,
                            awl::WorldMapActorAnimationDescriptor{2, 0, 0, 0}, awl::WorldMapActorAnimationGroup{0, 200},
                            &animation_step) == awl::WorldMapActorAnimationStatus::RequiresModelSetup &&
                        animation_step.primary_setup && animation_step.primary_setup->model_identity == 100 &&
                        animation_step.primary_setup->bank_identity == 200 && animation_step.primary_setup->clip_index == 0 &&
                        animation_step.primary_setup->blend_count == 0 &&
                        animation_step.after.current_descriptor_0 == 2 && animation_step.after.base_descriptor_4 == 2 &&
                        animation_step.after.completed_20 == 0 && animation_step.after.flag_21 == 0 &&
                        animation_step.after.restart_count_2c == 0 && actor_animation.base_descriptor_4 == 1 &&
                        actor_animation.completed_20 == 1 && actor_animation.flag_21 == 1 && actor_animation.restart_count_2c == 7 &&
                        actor_toggle.flag_158 == 1 && actor_toggle.value_150 == 9 &&
                        actor_presentation.phase == awl::WorldMapPresentationPhase::Reading;
                    // A supplied model binding and owned bank now prepare the
                    // complete first channel helper. Remaining initializer
                    // effects still prevent descriptor/presentation acceptance.
                    awl::WorldMapAnimationChannelState channel{7, 9, 2, 0, 0, 3, 2};
                    awl::WorldMapAnimationPlayback target;
                    target.rate_4 = 5; target.link_14 = 6; target.value_18 = 7;
                    const std::vector<awl::WorldMapAnimationPlaybackRecord> playback{{1, {}}, {2, target}};
                    awl::WorldMapAnimationChannelStep channel_step;
                    awl::WorldMapAnimationClip selected_clip;
                    valid_script = valid_script && animation_step.primary_setup &&
                        awl::prepare_world_map_animation_channel(channel, playback, *animation_step.primary_setup,
                            std::nullopt, &animation_bank, &channel_step) ==
                                awl::WorldMapAnimationChannelStatus::RequiresModelBinding &&
                        awl::prepare_world_map_animation_channel(channel, playback, *animation_step.primary_setup,
                            awl::WorldMapAnimationModelBinding{100, 1}, &animation_bank, &channel_step) ==
                                awl::WorldMapAnimationChannelStatus::Prepared &&
                        animation_bank.resolve(0, &selected_clip) &&
                        channel_step.branch == awl::WorldMapAnimationChannelBranch::NoClip &&
                        channel_step.after.elapsed_0 == 0 && channel_step.after.duration_4 == 0 &&
                        channel_step.after.mode_18 == 0 && channel_step.after.blend_14 == 3 &&
                        channel_step.records_after[1].state.clip_10 &&
                        channel_step.records_after[1].state.clip_10->offset == selected_clip.reference.offset &&
                        channel_step.records_after[1].state.limit_c == selected_clip.parameter_zero &&
                        channel_step.records_after[1].state.rate_4 == 5 &&
                        channel_step.records_after[1].state.link_14 == 6 && channel_step.records_after[1].state.value_18 == 7 &&
                        channel.elapsed_0 == 7 && channel.mode_18 == 2 && !playback[1].state.clip_10 &&
                        actor_animation.base_descriptor_4 == 1 && actor_toggle.flag_158 == 1 &&
                        actor_presentation.phase == awl::WorldMapPresentationPhase::Reading;
                    // A separately supplied descriptor with absent optional
                    // features/secondary model reaches primary link clearing.
                    // Its supplied proposal is never accepted as a live action.
                    awl::WorldMapAnimationInitializerState initializer;
                    initializer.animation = actor_animation; initializer.primary = channel;
                    initializer.records = playback; initializer.has_optional_bindings = true;
                    awl::WorldMapAnimationInitializerStep initializer_step;
                    const uint32_t absent_features = 63u | (127u << 11) | (255u << 19);
                    valid_script = valid_script && awl::prepare_world_map_animation_initializer(initializer, 2,
                        awl::WorldMapActorAnimationDescriptor{2, 0, absent_features, 0},
                        awl::WorldMapActorAnimationGroup{0, 200}, awl::WorldMapAnimationModelBinding{100, 1},
                        &animation_bank, {}, &initializer_step) == awl::WorldMapAnimationInitializerStatus::RequiresModelHierarchy &&
                        initializer_step.hierarchy_model == 100 && initializer_step.after.animation.flag_21 == 0 &&
                        initializer_step.after.records[0].state.clip_10 &&
                        initializer_step.after.records[0].state.clip_10->offset == selected_clip.reference.offset &&
                        initializer_step.after.records[0].state.link_14 == 0 &&
                        initializer.animation.base_descriptor_4 == 1 && initializer.primary.elapsed_0 == 7 &&
                        !initializer.records[0].state.clip_10 && actor_toggle.flag_158 == 1 &&
                        actor_presentation.phase == awl::WorldMapPresentationPhase::Reading;
                    initializer.model_links = awl::WorldMapModelLinkState{{
                        {100,99,0,{300,0,0,0},{1,2,3,4}}, {300,100,4,{400,0,0,0},{}},
                        {400,300,0,{}, {}}}};
                    valid_script = valid_script && awl::prepare_world_map_animation_initializer(initializer, 2,
                        awl::WorldMapActorAnimationDescriptor{2, 0, absent_features, 0},
                        awl::WorldMapActorAnimationGroup{0, 200}, awl::WorldMapAnimationModelBinding{100, 1},
                        &animation_bank, {}, &initializer_step) == awl::WorldMapAnimationInitializerStatus::Prepared &&
                        initializer_step.hierarchy && initializer_step.hierarchy->writes.size() == 4 &&
                        initializer_step.after.model_links->nodes[0].children_15c[0] == 0 &&
                        initializer_step.after.model_links->nodes[1].parent_150 == 0 &&
                        initializer_step.after.model_links->nodes[2].parent_150 == 0 &&
                        initializer_step.after.model_links->nodes[0].parent_150 == 99 &&
                        initializer.animation.base_descriptor_4 == 1 && initializer.primary.elapsed_0 == 7 &&
                        !initializer.records[0].state.clip_10 && initializer.model_links->nodes[0].children_15c[0] == 300 &&
                        actor_toggle.flag_158 == 1 && actor_presentation.phase == awl::WorldMapPresentationPhase::Reading;
                    valid_script = valid_script && awl::advance_world_map_presentation(staged_message.bytes.data(),
                        staged_message.bytes.size(), &presentation, 1, 0, 0, &presentation_step) ==
                            awl::WorldMapPresentationStatus::Advanced &&
                        presentation.phase == awl::WorldMapPresentationPhase::InputWait && presentation.resume_offset_20 == 72 &&
                        presentation.revealed_units_24 == 38 && presentation_step.visited_tokens == 40 &&
                        awl::advance_world_map_presentation(staged_message.bytes.data(), staged_message.bytes.size(),
                            &presentation, 2, 0, 0, &presentation_step) == awl::WorldMapPresentationStatus::Advanced &&
                        presentation.phase == awl::WorldMapPresentationPhase::InputWait &&
                        awl::advance_world_map_presentation(staged_message.bytes.data(), staged_message.bytes.size(),
                            &presentation, 2, 0x100, 0, &presentation_step) == awl::WorldMapPresentationStatus::RequiresFeedback &&
                        presentation_step.feedback_id == 3 && presentation_step.phase_after_effects == awl::WorldMapPresentationPhase::Reading &&
                        presentation.phase == awl::WorldMapPresentationPhase::InputWait && presentation.revealed_units_24 == 38;
                    awl::WorldMapFeedbackRegistry feedback_registry;
                    awl::WorldMapFeedbackStep feedback_step;
                    awl::WorldMapFeedbackRequest feedback_request;
                    uint64_t feedback_identity = 0;
                    valid_script = valid_script && presentation_step.feedback_id &&
                        feedback_registry.create_request(1, *presentation_step.feedback_id) &&
                        feedback_registry.link_request(1, 1) &&
                        feedback_registry.find_request(1, 3, std::nullopt, &feedback_identity) && feedback_identity == 1 &&
                        feedback_registry.start_request(1, {255, 255, 0}, std::nullopt, &feedback_step) ==
                            awl::WorldMapFeedbackStatus::RequiresPlayback &&
                        feedback_step.operation == awl::WorldMapFeedbackOperation::Start && feedback_step.feedback_id == 3 &&
                        feedback_registry.request(1, &feedback_request) && feedback_request.result_18 == UINT32_MAX &&
                        presentation.phase == awl::WorldMapPresentationPhase::InputWait && presentation.revealed_units_24 == 38;
                    constexpr uint64_t expected_request_digests[] = {
                        0xa491d22d6bd9d926ull, 0xcd23c6ae945521afull, 0x936962df319cd5b6ull};
                    for (unsigned scenario = 0; scenario < 3; ++scenario) {
                        auto supplied = next_boundary;
                        manager = {};
                        manager.has_manager_state = true;
                        manager.manager_state_48 = scenario == 2 ? 0u : 3u;
                        manager.manager_result_1c0 = 2;
                        if (scenario != 0) {
                            manager.key_530 = command.instruction.callback_arguments[0];
                            manager.key_534 = command.instruction.callback_arguments[1];
                        }
                        valid_script = valid_script && script.step_world_map_request(
                            &supplied, &manager, &boundary) == Step::Advanced &&
                            supplied.state_4 == 1 && supplied.instruction_index_0c == 19 &&
                            supplied.stack_depth_1b0 == 2 && manager.manager_result_1c0 == 2 &&
                            manager.manager_state_48 == (scenario == 2 ? 0u : 3u) &&
                            manager.key_530 == (scenario == 2 ? UINT32_MAX :
                                command.instruction.callback_arguments[0]) &&
                            manager.key_534 == (scenario == 2 ? UINT32_MAX :
                                command.instruction.callback_arguments[1]);
                        digest = 14695981039346656037ull;
                        for (const uint32_t value : supplied.stack_20) hash_word(value);
                        for (const uint32_t value : supplied.variables_1b8) hash_word(value);
                        valid_script = valid_script && digest == expected_request_digests[scenario];
                    }
                    // Supplied selection rows/input, not decoded presentation
                    // resources or accepted feedback. The proposal's result
                    // is used only as a supplied manager snapshot below.
                    awl::WorldMapSelectionState selection;
                    selection.phase = awl::WorldMapSelectionPhase::Choosing;
                    selection.choice_count_2c = 3;
                    selection.choice_index_28 = 1;
                    selection.has_active_transition = true;
                    selection.active_state_20 = 2;
                    selection.clock_duration_34 = 4;
                    awl::WorldMapSelectionStep selection_plan;
                    valid_script = valid_script && awl::advance_world_map_selection(
                        &selection, 0x100, 0x20000, 100, &selection_plan) ==
                            awl::WorldMapSelectionStatus::RequiresFeedback &&
                        selection.phase == awl::WorldMapSelectionPhase::Choosing &&
                        selection.choice_index_28 == 1 && selection.result_4 == UINT32_MAX &&
                        selection_plan.after.result_4 == 2 && selection_plan.feedback_count == 2 &&
                        selection_plan.feedback_ids[0] == 1 && selection_plan.feedback_ids[1] == 3;
                    awl::WorldMapRequestTransitionState closing;
                    closing.manager_state_48 = 3;
                    closing.manager_result_1c0 = selection_plan.after.result_4;
                    closing.has_active_transition = true;
                    closing.clock_duration_34 = 4;
                    valid_script = valid_script && awl::close_world_map_request_transition(
                        &closing, 100) == awl::WorldMapRequestTransitionStatus::Advanced;
                    const auto before_release = closing;
                    valid_script = valid_script && awl::advance_world_map_request_transition(
                        &closing, 104) == awl::WorldMapRequestTransitionStatus::RequiresResourceRelease &&
                        same_request_transition(closing, before_release);
                    manager.key_530 = command.instruction.callback_arguments[0];
                    manager.key_534 = command.instruction.callback_arguments[1];
                    manager.manager_state_48 = closing.manager_state_48;
                    manager.manager_result_1c0 = closing.manager_result_1c0;
                    auto waiting_for_release = next_boundary;
                    valid_script = valid_script && script.step_world_map_request(
                        &waiting_for_release, &manager, &boundary) == Step::Advanced &&
                        waiting_for_release.instruction_index_0c == 19 &&
                        waiting_for_release.stack_depth_1b0 == 2;
                    digest = 14695981039346656037ull;
                    for (const uint32_t value : waiting_for_release.stack_20) hash_word(value);
                    for (const uint32_t value : waiting_for_release.variables_1b8) hash_word(value);
                    valid_script = valid_script && digest == expected_request_digests[1];
                }
                return valid_script;
            };
            using Request = awl::WorldMapMovementRequestStatus;
            valid = conditions.prepare_movement_request(
                        0, unset_saved, state, -1, 0, &prepared) ==
                        Request::ReadyForActionPath &&
                    prepared.action_index == 0x18bu &&
                    prepared.record_index == 0 && prepared.decoder_flag == 1 &&
                    prepared.use_global_action_list &&
                    prepared.action_list_index == 95 &&
                    actions.decode_prepared_request(
                        prepared, 65536, &action_bytes) &&
                    action_bytes.size() == 8876 &&
                    check_script(action_bytes, 1105, 0);
            if (valid && phase == 0) {
                uint64_t digest = 14695981039346656037ull;
                for (const uint8_t byte : action_bytes) {
                    digest = (digest ^ byte) * 1099511628211ull;
                }
                std::printf("Global action node 95: %zu decoded bytes, 1105 instructions; command 0 yields at PC 12, later pass stops at command 4/PC 18; FNV64 %016llx\n",
                            action_bytes.size(), static_cast<unsigned long long>(digest));
            }
            valid = valid && conditions.prepare_movement_request(
                        0, unset_saved, state, 0, 0, &prepared) ==
                        Request::BlockedByOwnerState;
            state.state_11cec = 1;
            valid = valid && conditions.prepare_movement_request(
                        0, unset_saved, state, -1, 0, &prepared) ==
                        Request::NoEligibleRecord;
            state.state_11cec = 0;
            state.clock_ticks = 19u * 36000u;
            valid = valid && conditions.prepare_movement_request(
                        1, unset_saved, state, -1, 0, &prepared) ==
                        (phase == 0 ? Request::NoEligibleRecord
                                    : Request::ReadyForActionPath);
            if (valid && phase != 0) {
                valid = prepared.action_index == 0x178u &&
                        prepared.record_index == 1 &&
                        prepared.use_global_action_list &&
                        prepared.action_list_index == 76 &&
                        actions.decode_prepared_request(
                            prepared, 65536, &action_bytes) &&
                        action_bytes.size() == 7069 &&
                        check_script(action_bytes, 871, 3);
                if (valid && phase == 1) {
                    uint64_t digest = 14695981039346656037ull;
                    for (const uint8_t byte : action_bytes) {
                        digest = (digest ^ byte) * 1099511628211ull;
                    }
                    std::printf("Global action node 76: %zu decoded bytes, 871 instructions; command 66 consumes its argument at PC 1, stops at command 65/PC 5; FNV64 %016llx\n",
                                action_bytes.size(), static_cast<unsigned long long>(digest));
                }
                state.clock_ticks = 2u * 36000u;
                valid = valid && conditions.prepare_movement_request(
                            1, unset_saved, state, -1, 0, &prepared) ==
                            Request::ReadyForActionPath &&
                        prepared.record_index == 2 &&
                        prepared.action_list_index == 76 &&
                        actions.decode_prepared_request(
                            prepared, 65536, &action_bytes) &&
                        action_bytes.size() == 7069 &&
                        check_script(action_bytes, 871, 3);
                state.clock_ticks = 12u * 36000u;
                valid = valid && conditions.prepare_movement_request(
                            1, unset_saved, state, -1, 0, &prepared) ==
                            Request::NoEligibleRecord;
                state.has_time_state = false;
                valid = valid && conditions.prepare_movement_request(
                            1, unset_saved, state, -1, 0, &prepared) ==
                            Request::RequiresUntranslatedPredicate;
            }
        }
        if (valid) {
            std::printf("Event conditions phase %u: %zu entries, %zu payload bytes; callbacks, command preparation, and supplied request results verified\n",
                        phase, conditions.entry_count(), payload_bytes);
        }
    }
    valid = valid && !conditions.load_phase(6) && !conditions.loaded();
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    if (!valid) {
        std::fprintf(stderr, "Local event condition archive validation failed\n");
    }
    return valid;
}

bool check_local_camera_collision(const char* disc_root) {
    const std::filesystem::path path =
        std::filesystem::path(disc_root) /
        std::filesystem::path(awl::kWorldMapCameraCollisionPath + 1);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "Unable to open local camera collision asset\n");
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    awl::CollisionTreeAnalysis analysis;
    if (!analyze(bytes, analysis) || analysis.header_byte_6 != 0) {
        std::fprintf(stderr, "Unsupported local camera collision asset\n");
        return false;
    }
    const float x = (analysis.root_min[0] + analysis.root_max[0]) * 0.5f;
    const float z = (analysis.root_min[2] + analysis.root_max[2]) * 0.5f;
    awl::CollisionSurfaceSample primary;
    if (!awl::sample_type1_collision_surface(bytes.data(), bytes.size(),
                                             x, z, &primary)) {
        std::fprintf(stderr, "Camera collision center has no surface\n");
        return false;
    }
    awl::WorldMapCameraViewQuery query;
    query.target.camera.position = {x, primary.height, z - 2.0f};
    query.target.distance_30 = 2.0f;
    query.up_vector_24 = {0.0f, 1.0f, 0.0f};
    awl::WorldMapCameraPostUpdate result;
    awl_memory_init();
    awl::filesystem_init();
    awl::WorldMapCameraCollisionAsset camera_asset;
    const bool mounted = awl::filesystem_mount("/", disc_root);
    const bool updated = mounted && camera_asset.load() &&
                         camera_asset.calculate_post_update(query, &result);
    awl::WorldMapCameraInitialProfile profile;
    awl::WorldMapCameraPostUpdate profiled_result;
    awl::WorldMapPlayerCameraPlacement placement;
    bool profiled_update = updated &&
                           awl::make_world_map_camera_initial_profile(&profile);
    if (profiled_update) {
        for (size_t axis = 0; axis < 3; ++axis) {
            const float desired = axis == 0 ? x :
                                  axis == 1 ? primary.height : z;
            profile.view_query.target.camera.position[axis] =
                desired - profile.initial_view.target.bounded[axis];
        }
        profiled_update = camera_asset.calculate_post_update(
            profile.view_query, &profiled_result);
    }
    awl::WorldMapPlayerCameraPlacementQuery placement_query;
    placement_query.initial_camera = query;
    placement_query.player_position = query.target.camera.position;
    placement_query.collision_category = 2;
    const bool placed = profiled_update &&
                        camera_asset.calculate_player_placement(
                            placement_query, &placement);
    awl::WorldMapPlayerCameraMessageQuery message_query;
    message_query.previous_camera = query;
    message_query.collision_category = 2;
    message_query.message.position = query.target.camera.position;
    message_query.message.camera_update_requested = 1;
    awl::WorldMapPlayerCameraMessageResult message_result;
    const bool message_updated = placed &&
                                 camera_asset.calculate_player_message_update(
                                     message_query, &message_result);
    awl::WorldMapPlayerMovementCameraQuery movement_query;
    movement_query.previous_camera = query;
    movement_query.resolved_position = query.target.camera.position;
    movement_query.collision_category = 2;
    awl::WorldMapPlayerMovementCameraResult movement_result;
    const bool movement_updated = message_updated &&
                                  camera_asset.calculate_player_movement_update(
                                      movement_query, &movement_result);
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    if (!updated ||
        std::fabs(result.final_target[1] - primary.height) > 0.0001f ||
        std::fabs(result.final_target[0] - x) > 0.0001f ||
        std::fabs(result.final_target[2] - z) > 0.0001f ||
        !profiled_update ||
        !placed || !placement.second_update_called ||
        placement.second_yaw_written ||
        !message_updated || !message_result.update_called ||
        std::fabs(message_result.update.final_target[0] - x) > 0.0001f ||
        std::fabs(message_result.update.final_target[2] - z) > 0.0001f ||
        !movement_updated || movement_result.target_on_positive_side_188 ||
        std::fabs(movement_result.update.final_target[0] - x) > 0.0001f ||
        std::fabs(movement_result.update.final_target[2] - z) > 0.0001f ||
        std::fabs(placement.final_update.final_target[0] - x) > 0.0001f ||
        std::fabs(placement.final_update.final_target[2] - z) > 0.0001f ||
        std::fabs(profiled_result.first_view.target.bounded[0] - x) >
            0.0001f ||
        std::fabs(profiled_result.first_view.target.bounded[2] - z) >
            0.0001f) {
        std::fprintf(stderr, "Local camera collision post-update failed\n");
        return false;
    }
    std::printf("Local camera collision route validated: nodes=%zu leaves=%zu "
                "triangles=%zu\n", analysis.node_count, analysis.leaf_count,
                analysis.triangle_count);
    return true;
}

// Development-only replay of supplied clear routes. The fixture supplies an
// empty dynamic-object list and scene type zero; it is not a game-owned player.
enum class LocalSlopeExpectation {
    None,
    Uphill,
    Downhill,
};

bool replay_player_route(const uint8_t* terrain, size_t terrain_size,
                         const uint8_t* static_objects, size_t static_size,
                         float start_x, float start_z, int held_frames,
                         float required_end_x,
                         LocalSlopeExpectation slope = LocalSlopeExpectation::None) {
    const bool downhill = slope == LocalSlopeExpectation::Downhill;
    awl::CollisionSurfaceSample spawn_surface;
    if (!awl::sample_type1_collision_surface(
            terrain, terrain_size, start_x, start_z, &spawn_surface)) {
        return false;
    }
    if (slope != LocalSlopeExpectation::None) {
        awl::CollisionSurfaceSample endpoint_surface;
        if (!awl::sample_type1_collision_surface(
                terrain, terrain_size, required_end_x, start_z,
                &endpoint_surface)) {
            return false;
        }
        const float sampled_rise =
            endpoint_surface.height - spawn_surface.height;
        if ((slope == LocalSlopeExpectation::Uphill &&
             sampled_rise <= 1.5f) ||
            (downhill && sampled_rise >= -1.5f)) {
            return false;
        }
    }
    std::array<float, 3> position{start_x, spawn_surface.height, start_z};
    awl::WorldMapSteeringState steering;
    awl::WorldMapSceneBucketRegistry scene;
    awl::WorldMapScenePositionUpdate scene_update;
    if (!scene.register_object(1, 0, position) ||
        !scene.update_position(1, position, &scene_update) ||
        scene_update.next_bucket != 0) {
        return false;
    }

    awl::NativeInputAccumulator native;
    awl::PadAdapter adapter;
    awl::HsdPadFilter filter;
    native.reset(true);
    const awl::NativeKey direction_key =
        downhill ? awl::NativeKey::A : awl::NativeKey::D;
    native.set_key(direction_key, true);
    float previous_x = position[0];
    bool crossed_seam = false;
    bool changed_height = false;
    float lowest_height_step = 0.0f;
    float highest_height_step = 0.0f;
    size_t triangle_changes = 0;
    size_t rising_frames = 0;
    size_t falling_frames = 0;
    size_t static_contacts = 0;
    auto previous_surface = spawn_surface;
    for (int frame = 0; frame < held_frames + 8; ++frame) {
        if (frame == held_frames) {
            native.set_key(direction_key, false);
        }
        native.begin_frame();
        adapter.begin_frame(native.frame());
        filter.begin_frame(adapter.frame().sample);
        awl::WorldMapMovementQuery query;
        query.pad = filter.frame();
        query.current_position = position;
        query.current_axis = {0.0f, 0.0f, 1.0f};
        query.steering = steering;
        query.collision.terrain_data = terrain;
        query.collision.terrain_size = terrain_size;
        query.collision.static_data = static_objects;
        query.collision.static_size = static_size;
        awl::WorldMapMovementCandidate candidate;
        if (!awl::calculate_world_map_movement_candidate(query, &candidate) ||
            !candidate.movement_enabled ||
            !scene.update_position(1, candidate.resolved_position,
                                   &scene_update) ||
            scene_update.next_bucket != 0) {
            return false;
        }
        // The fixture has no polygon-trigger provider. The DOL still
        // records prior/resolved positions and checks both category-1 slots.
        const std::array<awl::WorldMapMovementContactSlotOutcome, 2> slots{};
        awl::WorldMapMovementContactTail contact_tail;
        if (!awl::plan_world_map_movement_contact_tail(
                1, query.current_position, candidate.resolved_position,
                slots, &contact_tail) ||
            contact_tail.recorded_prior != query.current_position ||
            contact_tail.recorded_resolved != candidate.resolved_position ||
            contact_tail.polygon_queries != 2 ||
            contact_tail.state_requests != 0) {
            return false;
        }
        steering = candidate.steering;
        position = candidate.resolved_position;
        awl::CollisionSurfaceSample surface;
        if (!awl::sample_type1_collision_surface(
                terrain, terrain_size, position[0], position[2], &surface) ||
            std::fabs(position[1] - surface.height) > 0.0002f ||
            (downhill ? position[0] > previous_x + 0.0002f
                      : position[0] + 0.0002f < previous_x)) {
            return false;
        }
        crossed_seam = crossed_seam || position[0] > 125.0f;
        changed_height = changed_height ||
                         std::fabs(position[1] - spawn_surface.height) > 0.5f;
        const float height_step = surface.height - previous_surface.height;
        lowest_height_step = std::min(lowest_height_step, height_step);
        highest_height_step = std::max(highest_height_step, height_step);
        rising_frames += height_step > 0.002f;
        falling_frames += height_step < -0.002f;
        triangle_changes +=
            surface.leaf_offset != previous_surface.leaf_offset ||
            surface.triangle_index != previous_surface.triangle_index;
        static_contacts += candidate.collision.static_contact.contact;
        previous_surface = surface;
        if (frame >= held_frames + 6 &&
            position[0] != previous_x) {
            return false;
        }
        previous_x = position[0];
    }
    const auto snapshot = scene.snapshot(0);
    const float actual_rise = position[1] - spawn_surface.height;
    bool slope_valid = true;
    if (slope == LocalSlopeExpectation::Uphill) {
        slope_valid = actual_rise > 1.5f && lowest_height_step >= -0.0002f &&
                      rising_frames >= 60 && triangle_changes >= 5 &&
                      static_contacts == 0;
    } else if (downhill) {
        slope_valid = actual_rise < -1.5f && highest_height_step <= 0.0002f &&
                      falling_frames >= 60 && triangle_changes >= 5 &&
                      static_contacts == 0;
    }
    if (slope != LocalSlopeExpectation::None) {
        std::printf("slope replay start=(%.0f,%.0f) end=(%.3f,%.3f) "
                    "rise=%.3f min-step=%.4f max-step=%.4f "
                    "triangle-changes=%zu rising=%zu falling=%zu "
                    "static-contacts=%zu\n",
                    start_x, start_z, position[0], position[2],
                    actual_rise, lowest_height_step,
                    highest_height_step, triangle_changes, rising_frames,
                    falling_frames, static_contacts);
    }
    const bool end_reached = downhill ? position[0] <= required_end_x
                                      : position[0] >= required_end_x;
    return end_reached &&
           (downhill || required_end_x <= 125.0f || crossed_seam) &&
           changed_height && slope_valid &&
           steering.current_speed == 0.0f && snapshot.size() == 1 &&
           snapshot[0].position == position;
}

void test_synthetic_player_route_replay() {
    const auto terrain = make_sample_leaf();
    expect(replay_player_route(terrain.data(), terrain.size(), nullptr, 0,
                               2.0f, 2.0f, 12, 3.5f),
           "filtered keyboard movement, terrain, scene position, and neutral stop compose across frames");
}

bool replay_local_player_route(const char* disc_root) {
    awl_memory_init();
    awl::filesystem_init();
    bool valid = awl::filesystem_mount("/", disc_root);
    for (uint32_t phase = 0; valid && phase < 6; ++phase) {
        for (int alternate = 0; valid && alternate < 2; ++alternate) {
            awl::WorldMapCollisionAssets assets;
            valid = assets.load(phase, alternate != 0) &&
                    replay_player_route(
                        assets.terrain_bytes().data(),
                        assets.terrain_bytes().size(),
                        assets.static_bytes().data(),
                        assets.static_bytes().size(),
                        120.0f, 168.0f, 48, 128.0f) &&
                    replay_player_route(
                        assets.terrain_bytes().data(),
                        assets.terrain_bytes().size(),
                        assets.static_bytes().data(),
                        assets.static_bytes().size(),
                        115.0f, 160.0f, 72, 126.0f,
                        LocalSlopeExpectation::Uphill) &&
                    replay_player_route(
                        assets.terrain_bytes().data(),
                        assets.terrain_bytes().size(),
                        assets.static_bytes().data(),
                        assets.static_bytes().size(),
                        128.0f, 160.0f, 72, 116.0f,
                        LocalSlopeExpectation::Downhill);
            if (valid) {
                std::printf("Player routes passed: phase=%u terrain=%s "
                            "seam start=120,168; "
                            "slope uphill/downhill start=115,160/128,160\n",
                            phase, assets.paths().terrain);
            }
        }
    }
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    return valid;
}

void test_world_map_collision_asset_provider() {
    constexpr uint32_t unit = 34560000u;
    constexpr std::array<uint32_t, 7> prefix_units{
        0u, 1u, 3u, 6u, 8u, 9u, 10u};
    for (uint32_t prefix = 0; prefix < prefix_units.size(); ++prefix) {
        uint32_t counter = 0;
        awl::WorldMapCollisionSelection from_counter;
        expect(awl::compose_world_map_collision_counter(
                   {prefix, 0u, {}}, &counter) &&
                   counter == prefix_units[prefix] * unit &&
                   awl::derive_world_map_collision_selection(
                       {counter, 0u}, &from_counter) &&
                   from_counter.phase_index == std::min(prefix, 5u),
               "DOL table-prefix counter selects its bounded phase");
    }
    uint32_t composed = 0;
    expect(awl::compose_world_map_collision_counter(
               {0u, 0u, {1u, 1u, 1u, 1u, 1u}}, &composed) &&
               composed == 9540610u,
           "counter subunit weights match the five DOL multipliers");
    expect(awl::compose_world_map_collision_counter(
               {0u, 0u, {0u, 0u, 5u, 0u, 0u}}, &composed) &&
               composed == 180000u &&
               awl::compose_world_map_collision_counter(
                   {0u, 1u, {}}, &composed) && composed == unit &&
               awl::compose_world_map_collision_counter(
                   {0u, 0u, {4u, 0u, 0u, 0u, 0u}}, &composed) &&
               composed == unit,
           "initial finer value and a full-unit carry retain raw word semantics");
    expect(awl::compose_world_map_collision_counter(
               {0u, std::numeric_limits<uint32_t>::max(), {}}, &composed) &&
               composed == 0u - unit,
           "counter composition keeps the target's 32-bit wrap");
    composed = 123u;
    expect(!awl::compose_world_map_collision_counter(
               {7u, 0u, {}}, &composed) && composed == 0u &&
               !awl::compose_world_map_collision_counter(
                   {0u, 0u, {}}, nullptr),
           "unverified table prefix and null counter output are rejected");
    awl::WorldMapCollisionPhaseSetup setup;
    expect(awl::plan_world_map_collision_phase_setup(
               0u, 0, false, &setup) &&
               setup.counter_word == unit && setup.phase_index == 1u &&
               setup.phase_initializer_required && !setup.terminal,
           "mode-zero setup advances to the next phase boundary");
    expect(awl::plan_world_map_collision_phase_setup(
               2u * unit + 180000u, 0, false, &setup) &&
               setup.counter_word == 3u * unit && setup.phase_index == 2u &&
               setup.phase_initializer_required,
           "mode-zero setup drops finer fields at the next boundary");
    expect(awl::plan_world_map_collision_phase_setup(
               3u * unit + 180000u, 1, false, &setup) &&
               setup.counter_word == 3u * unit && setup.phase_index == 2u &&
               setup.phase_initializer_required,
           "mode-one setup resets the current phase to its start");
    expect(awl::plan_world_map_collision_phase_setup(
               3u * unit + 180000u, 2, false, &setup) &&
               setup.counter_word == 3u * unit + 180000u &&
               setup.phase_index == 2u && !setup.phase_initializer_required,
           "other setup modes preserve the supplied counter");
    expect(awl::plan_world_map_collision_phase_setup(
               9u * unit, 0, false, &setup) &&
               setup.counter_word == 10u * unit && setup.terminal &&
               !setup.phase_initializer_required &&
               awl::plan_world_map_collision_phase_setup(
                   10u * unit, 1, false, &setup) && setup.terminal &&
               !setup.phase_initializer_required,
           "sixth table boundary exits before scene setup");
    expect(awl::plan_world_map_collision_phase_setup(
               0u, 0, true, &setup) && setup.blocked &&
               setup.counter_word == 0u && !setup.terminal &&
               !setup.phase_initializer_required &&
               !awl::plan_world_map_collision_phase_setup(
                   0u, 0, false, nullptr),
           "state guard blocks counter stage and null output is rejected");
    struct PhaseCase {
        uint32_t counter;
        uint32_t phase;
    };
    constexpr std::array<PhaseCase, 12> phase_cases{{
        {0u, 0u}, {unit - 1u, 0u}, {unit, 1u},
        {3u * unit - 1u, 1u}, {3u * unit, 2u},
        {6u * unit - 1u, 2u}, {6u * unit, 3u},
        {8u * unit - 1u, 3u}, {8u * unit, 4u},
        {9u * unit - 1u, 4u}, {9u * unit, 5u},
        {std::numeric_limits<uint32_t>::max(), 5u},
    }};
    awl::WorldMapCollisionSelection selection;
    for (const PhaseCase& sample : phase_cases) {
        expect(awl::derive_world_map_collision_selection(
                   {sample.counter, 0u}, &selection) &&
                   selection.phase_index == sample.phase &&
                   !selection.alternate_terrain,
               "raw counter selects the ordered DOL phase boundary");
    }
    expect(awl::derive_world_map_collision_selection(
               {6u * unit, 1u}, &selection) &&
               selection.phase_index == 3u && selection.alternate_terrain,
           "raw state byte selects the alternate terrain asset");
    expect(!awl::derive_world_map_collision_selection({0u, 2u}, &selection) &&
               selection.phase_index == 0u && !selection.alternate_terrain &&
               !awl::derive_world_map_collision_selection({0u, 0u}, nullptr),
           "unsupported terrain selector and null output are rejected");
    constexpr const char* expected_static[6] = {
        "/files/mapobj.col", "/files/mapobj2.col",
        "/files/mapobj3.col", "/files/mapobj4.col",
        "/files/mapobj5.col", "/files/mapobj5.col"};
    awl::WorldMapCollisionAssetPaths paths;
    for (uint32_t phase = 0; phase < 6; ++phase) {
        expect(awl::select_world_map_collision_asset_paths(
                   phase, false, &paths) &&
                   std::strcmp(paths.terrain, "/files/jimen-move.col") == 0 &&
                   std::strcmp(paths.static_objects, expected_static[phase]) == 0,
               "phase selects the DOL terrain and static COL paths");
        expect(awl::select_world_map_collision_asset_paths(
                   phase, true, &paths) &&
                   std::strcmp(paths.terrain, "/files/jimen1-move.col") == 0,
               "terrain switch selects the alternate movement COL");
    }
    expect(!awl::select_world_map_collision_asset_paths(6, false, &paths) &&
               paths.terrain == nullptr && paths.static_objects == nullptr,
           "unsupported phase is rejected and clears output paths");
    expect(!awl::select_world_map_collision_asset_paths(0, false, nullptr),
           "null selector output is rejected");

    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("awl-collision-provider-" + std::to_string(nonce));
    std::error_code error;
    const bool created = fs::create_directory(root, error);
    expect(created && !error, "collision fixture directory is created");
    if (!created || error) {
        return;
    }
    const fs::path files = root / "files";
    fs::create_directory(files, error);
    expect(!error, "collision fixture files directory is created");
    if (error) {
        fs::remove(root, error);
        return;
    }
    const auto write = [](const fs::path& path,
                          const std::vector<uint8_t>& bytes) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    };
    const fs::path terrain_path = files / "jimen-move.col";
    const fs::path alternate_terrain_path = files / "jimen1-move.col";
    const fs::path static_path = files / "mapobj.col";
    const fs::path camera_path = files / "jimen-camera.col";
    const std::vector<uint8_t> terrain = make_sample_leaf();
    std::vector<uint8_t> spawn_terrain = make_sample_leaf();
    constexpr uint32_t spawn_payload = 8 + 0x34;
    initialize_sample_payload(spawn_terrain, 8, spawn_payload, 70, 110);
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        put_be_s16(spawn_terrain, spawn_payload + 8 + vertex * 8 + 2, 6);
    }
    std::vector<uint8_t> static_asset = terrain;
    static_asset[6] = 0;
    expect(write(terrain_path, terrain) &&
               write(alternate_terrain_path, spawn_terrain) &&
               write(static_path, static_asset) &&
               write(camera_path, static_asset),
           "collision fixtures are written");

    awl_memory_init();
    awl::filesystem_init();
    const std::string native_root = root.string();
    const bool mounted = awl::filesystem_mount("/", native_root.c_str());
    expect(mounted, "collision fixture mount succeeds");
    if (mounted) {
        awl::WorldMapCameraCollisionAsset camera_asset;
        awl::WorldMapCameraViewQuery camera_query;
        camera_query.target.camera.position = {2.0f, 0.0f, 0.0f};
        camera_query.target.distance_30 = 2.0f;
        camera_query.up_vector_24 = {0.0f, 1.0f, 0.0f};
        awl::WorldMapCameraPostUpdate camera_result;
        awl::WorldMapPlayerCameraPlacementQuery placement_query;
        placement_query.initial_camera = camera_query;
        placement_query.player_position = camera_query.target.camera.position;
        placement_query.collision_category = 2;
        awl::WorldMapPlayerCameraPlacement placement_result;
        awl::WorldMapPlayerCameraMessageQuery message_query;
        message_query.previous_camera = camera_query;
        message_query.collision_category = 2;
        message_query.message.position = camera_query.target.camera.position;
        message_query.message.camera_update_requested = 1;
        awl::WorldMapPlayerCameraMessageResult message_result;
        awl::WorldMapPlayerMovementCameraQuery movement_query;
        movement_query.previous_camera = camera_query;
        movement_query.resolved_position = camera_query.target.camera.position;
        movement_query.collision_category = 2;
        awl::WorldMapPlayerMovementCameraResult movement_result;
        expect(camera_asset.load() && camera_asset.loaded() &&
                   camera_asset.size() == static_asset.size() &&
                   camera_asset.calculate_post_update(camera_query,
                                                      &camera_result) &&
                   camera_asset.calculate_player_placement(
                       placement_query, &placement_result) &&
                   camera_asset.calculate_player_message_update(
                       message_query, &message_result) &&
                   camera_asset.calculate_player_movement_update(
                       movement_query, &movement_result) &&
                   message_result.update_called &&
                   placement_result.second_update_called &&
                   camera_result.terrain_clamped &&
                   camera_result.first_view.target.bounded ==
                       std::array<float, 3>{2.0f, 0.0f, 2.0f},
               "mounted slot-1 camera asset supplies the two height queries");
        const auto saved_camera_target = camera_result.final_target;
        fs::remove(camera_path, error);
        expect(!camera_asset.load() && !camera_asset.loaded() &&
                   !camera_asset.calculate_post_update(camera_query,
                                                       &camera_result) &&
                   !camera_asset.calculate_player_placement(
                       placement_query, &placement_result) &&
                   !camera_asset.calculate_player_message_update(
                       message_query, &message_result) &&
                   !camera_asset.calculate_player_movement_update(
                       movement_query, &movement_result) &&
                   message_result.update_called &&
                   placement_result.second_update_called &&
                   camera_result.final_target == saved_camera_target,
               "missing camera file clears the owner without changing output");
        message_query.message.camera_update_requested = 0;
        expect(camera_asset.calculate_player_message_update(
                   message_query, &message_result) &&
                   !message_result.update_called &&
                   message_result.final_camera.target.camera.position ==
                       camera_query.target.camera.position,
               "clear message camera byte skips unloaded collision owner");
        expect(write(camera_path, {0, 1, 2}) && !camera_asset.load() &&
                   !camera_asset.loaded(),
               "malformed camera collision file is rejected");
        expect(write(camera_path, terrain) && !camera_asset.load() &&
                   !camera_asset.loaded(),
               "movement-mode COL cannot replace the fixed camera asset");
        expect(write(camera_path, static_asset) && camera_asset.load(),
               "valid camera asset can reload after failure");
        camera_asset.clear();
        expect(!camera_asset.loaded() &&
                   !camera_asset.calculate_post_update(camera_query,
                                                       &camera_result),
               "cleared camera owner cannot serve a camera update");

        awl::WorldMapCollisionAssets assets;
        awl::CollisionCategory1MovementQuery query;
        query.moving_radius = 0.3f;
        expect(assets.load(0, false) && assets.bind(&query) &&
                   query.terrain_data == assets.terrain_bytes().data() &&
                   query.terrain_size == terrain.size() &&
                   query.static_data == assets.static_bytes().data() &&
                   query.static_size == static_asset.size() &&
                   query.moving_radius == 0.3f &&
                   assets.terrain_analysis().header_byte_6 == 1 &&
                   assets.static_analysis().header_byte_6 == 0,
               "provider owns and binds both validated collision assets");
        awl::CollisionCategory1MovementAdjustment result;
        expect(awl::resolve_type1_category1_movement_candidate(
                   query, {2.0f, 7.0f, 2.0f}, {2.0f, 50.0f, 2.0f},
                   &result) && result.static_contact.slot_present &&
                   !result.static_contact.contact &&
                   result.position == std::array<float, 3>{2.0f, 6.0f, 2.0f},
               "bound mode-zero static data yields a clear full candidate");
        expect(!assets.bind(nullptr), "null query binding is rejected");
        expect(!assets.load_from_source_state({0u, 2u}) &&
                   assets.terrain_bytes().empty() &&
                   assets.static_bytes().empty() &&
                   assets.paths().terrain == nullptr &&
                   assets.load_from_source_state({0u, 0u}),
               "invalid raw state clears owned assets and valid state reloads");

        awl::WorldMapSceneFirstCollisionObjects scene;
        scene.category1_actor.collision.identity = 103;
        scene.category1_actor_id =
            awl::world_map_first_actor_selection(3u).actor_id;
        scene.category1_actor_height_query_result = 99.0f;
        awl::WorldMapCollisionRegistry scene_registry;
        expect(assets.load(0, true) &&
                   awl::register_world_map_scene_first_collision_objects_from_assets(
                       scene, assets, &scene_registry) &&
                   scene_registry.snapshot().first_resolver.size() == 1 &&
                   scene_registry.snapshot().first_resolver[0].center_world ==
                       std::array<float, 3>{75.0f, 6.0f, 113.5f} &&
                   scene_registry.snapshot().first_resolver[0].radius == 0.9f,
               "validated slot-10 terrain supplies the actor's spawn height");
        scene.category1_actor.collision.identity = 104;
        scene.category1_actor_id = 0x30;
        expect(awl::register_world_map_scene_first_collision_objects_from_assets(
                   scene, assets, &scene_registry) &&
                   scene_registry.snapshot().first_resolver.size() == 2 &&
                   scene_registry.snapshot().first_resolver[0].center_world ==
                       std::array<float, 3>{279.0f, 27.0f, 116.0f},
               "fixed-height actor skips the terrain height query");
        scene.category1_actor_id = 0x2f;
        expect(write(alternate_terrain_path, make_single_leaf()) &&
                   assets.load(0, true) &&
                   !awl::register_world_map_scene_first_collision_objects_from_assets(
                       scene, assets, &scene_registry) &&
                   scene_registry.snapshot().first_resolver.size() == 2 &&
                   !awl::register_world_map_scene_first_collision_objects_from_assets(
                       scene, assets, nullptr),
               "empty selected leaf rejects spawn without changing the list");
        assets.clear();
        expect(!awl::register_world_map_scene_first_collision_objects_from_assets(
                   scene, assets, &scene_registry) &&
                   assets.load(0, false),
               "unloaded terrain cannot supply a spawn height");

        fs::remove(static_path, error);
        expect(!assets.load(0, false) && !assets.bind(&query) &&
                   assets.paths().terrain == nullptr &&
                   query.terrain_data == nullptr && query.terrain_size == 0 &&
                   query.static_data == nullptr && query.static_size == 0,
               "missing static asset fails without retaining a partial pair");
        expect(write(static_path, {0, 1, 2}),
               "malformed static fixture is written");
        expect(!assets.load(0, false) && !assets.bind(&query),
               "malformed static asset is rejected");
        expect(write(static_path, terrain),
               "wrong-mode static fixture is written");
        expect(!assets.load(0, false) && !assets.bind(&query),
               "wrong static collision mode is rejected");
        expect(write(static_path, static_asset),
               "valid static fixture is restored");
        fs::remove(terrain_path, error);
        expect(!assets.load(0, false) && !assets.bind(&query),
               "missing terrain asset is rejected");
        expect(!assets.load(6, false) && !assets.bind(&query),
               "unsupported phase cannot bind stale bytes");
    }
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    fs::remove(terrain_path, error);
    fs::remove(alternate_terrain_path, error);
    fs::remove(static_path, error);
    fs::remove(camera_path, error);
    fs::remove(files, error);
    fs::remove(root, error);
    expect(!error, "collision fixture directory is cleaned up");
}

struct LocalWallProbe {
    float mid_x = 0.0f;
    float mid_z = 0.0f;
    float normal_x = 0.0f;
    float normal_z = 0.0f;
    uint32_t leaf_offset = 0;
    uint32_t triangle_index = 0;
    uint8_t edge_index = 0;
};

bool probe_local_static_wall(const awl::WorldMapCollisionAssets& assets,
                             awl::CollisionCategory1StaticAdjustment* result,
                             LocalWallProbe* geometry,
                             float seed_x,
                             float seed_z) {
    const auto& bytes = assets.static_bytes();
    const auto& analysis = assets.static_analysis();
    awl::CollisionEdgeSample edge;
    if (!awl::project_type1_collision_to_edge(
            bytes.data(), bytes.size(), seed_x,
            seed_z, &edge) || (edge.surface_flags & 0xC1u) == 0 ||
        edge.edge_index >= 3 || edge.leaf_offset > bytes.size() ||
        bytes.size() - edge.leaf_offset < 0x34) {
        return false;
    }
    const auto be16 = [&bytes](size_t offset) {
        return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
                                     bytes[offset + 1]);
    };
    const auto be32 = [&bytes](size_t offset) {
        return (static_cast<uint32_t>(bytes[offset]) << 24) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
               bytes[offset + 3];
    };
    const size_t triangles = edge.leaf_offset + be32(edge.leaf_offset + 0x28);
    const size_t vertices = edge.leaf_offset + be32(edge.leaf_offset + 0x2c);
    if (triangles > bytes.size() ||
        edge.triangle_index >= (bytes.size() - triangles) / 8 ||
        vertices > bytes.size()) {
        return false;
    }
    const size_t record = triangles + static_cast<size_t>(edge.triangle_index) * 8;
    const uint16_t start_index = be16(record + 2 + edge.edge_index * 2);
    const uint16_t end_index = be16(record + 2 + ((edge.edge_index + 1) % 3) * 2);
    if (start_index >= (bytes.size() - vertices) / 8 ||
        end_index >= (bytes.size() - vertices) / 8) {
        return false;
    }
    const auto signed_coordinate = [&be16](size_t offset) {
        const int32_t raw = be16(offset);
        return raw >= 32768 ? raw - 65536 : raw;
    };
    const size_t start = vertices + static_cast<size_t>(start_index) * 8;
    const size_t end = vertices + static_cast<size_t>(end_index) * 8;
    const float x0 = signed_coordinate(start) * analysis.coordinate_scale;
    const float z0 = signed_coordinate(start + 4) * analysis.coordinate_scale;
    const float x1 = signed_coordinate(end) * analysis.coordinate_scale;
    const float z1 = signed_coordinate(end + 4) * analysis.coordinate_scale;
    const float dx = x1 - x0;
    const float dz = z1 - z0;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(length) || length == 0.0f) {
        return false;
    }
    // FUN_8017C918 uses (0, -1, 0) for this edge plane. Construct a
    // positive-side prior point and a candidate just across the plane.
    const float nx = dz / length;
    const float nz = -dx / length;
    const float mid_x = (x0 + x1) * 0.5f;
    const float mid_z = (z0 + z1) * 0.5f;
    const std::array<float, 3> prior{mid_x + nx, 0.0f, mid_z + nz};
    const std::array<float, 3> proposed{
        mid_x - nx * 0.1f, 0.0f, mid_z - nz * 0.1f};
    const awl::CollisionCategory1StaticFlags flags;
    if (!awl::resolve_type1_category1_static_contact(
            bytes.data(), bytes.size(), flags, 0x67u, prior, proposed,
            0.3f, 0u, result) || !result->slot_present || !result->contact ||
        !result->narrow_phase.first_edge_contact ||
        !std::isfinite(result->position[0]) ||
        !std::isfinite(result->position[1]) ||
        !std::isfinite(result->position[2])) {
        return false;
    }
    const float signed_distance =
        (result->position[0] - mid_x) * nx +
        (result->position[2] - mid_z) * nz;
    if (signed_distance < 0.309f) {
        return false;
    }
    awl::CollisionCategory1MovementQuery query;
    query.moving_radius = 0.3f;
    if (!assets.bind(&query)) {
        return false;
    }
    awl::CollisionCategory1MovementAdjustment composed;
    if (!awl::resolve_type1_category1_movement_candidate(
            query, prior, proposed, &composed) ||
        !composed.static_contact.contact ||
        (composed.resolver_contact_bits & 1u) == 0 ||
        !composed.final_height_resampled ||
        !std::isfinite(composed.position[0]) ||
        !std::isfinite(composed.position[1]) ||
        !std::isfinite(composed.position[2])) {
        return false;
    }
    awl::CollisionSurfaceSample terrain_surface;
    bool height_matches = false;
    if (awl::sample_type1_collision_surface(
            assets.terrain_bytes().data(), assets.terrain_bytes().size(),
            composed.position[0], composed.position[2], &terrain_surface)) {
        height_matches =
            std::fabs(composed.position[1] - terrain_surface.height) < 0.0002f;
    } else {
        awl::CollisionEdgeSample terrain_edge;
        height_matches = awl::project_type1_collision_to_edge(
                             assets.terrain_bytes().data(),
                             assets.terrain_bytes().size(),
                             composed.position[0], composed.position[2],
                             &terrain_edge) &&
                         std::fabs(composed.position[1] -
                                   terrain_edge.position[1]) < 0.0002f;
    }
    if (!height_matches) {
        return false;
    }
    if (geometry != nullptr) {
        *geometry = {mid_x, mid_z, nx, nz,
                     edge.leaf_offset, edge.triangle_index, edge.edge_index};
    }
    return true;
}

bool replay_local_static_wall_route(
    const awl::WorldMapCollisionAssets& assets,
    const awl::DevelopmentWallRoute& wall,
    awl::DevelopmentWallRouteSide side) {
    std::array<float, 3> position = wall.start;
    awl::NativeInputAccumulator native;
    awl::PadAdapter adapter;
    awl::HsdPadFilter filter;
    awl::WorldMapSteeringState steering;
    native.reset(true);
    native.set_key(awl::NativeKey::D, true);
    const bool diagonal = side != awl::DevelopmentWallRouteSide::MinZ;
    native.set_key(awl::NativeKey::S, diagonal);
    bool saw_contact = false;
    bool approached_wall = false;
    bool saw_neutral_fallback_reprojection = false;
    float previous_distance = wall.signed_distance(position);
    awl::CollisionSurfaceSample start_surface;
    const bool start_contained = awl::sample_type1_collision_surface(
        assets.terrain_bytes().data(), assets.terrain_bytes().size(),
        position[0], position[2], &start_surface);
    if (start_contained != (side != awl::DevelopmentWallRouteSide::MinZ)) {
        return false;
    }
    for (int frame = 0; frame < 28; ++frame) {
        if (frame == 20) {
            native.set_key(awl::NativeKey::D, false);
            native.set_key(awl::NativeKey::S, false);
        }
        native.begin_frame();
        adapter.begin_frame(native.frame());
        filter.begin_frame(adapter.frame().sample);
        awl::WorldMapMovementQuery query;
        query.pad = filter.frame();
        query.current_position = position;
        query.current_axis = {0.0f, 0.0f, 1.0f};
        query.steering = steering;
        if (!assets.bind(&query.collision)) {
            return false;
        }
        awl::WorldMapMovementCandidate candidate;
        if (!awl::calculate_world_map_movement_candidate(query, &candidate) ||
            !candidate.movement_enabled ||
            !std::isfinite(candidate.resolved_position[0]) ||
            !std::isfinite(candidate.resolved_position[1]) ||
            !std::isfinite(candidate.resolved_position[2])) {
            return false;
        }
        if (frame >= 26) {
            if (candidate.steering.current_speed != 0.0f ||
                candidate.proposed_position != position) {
                return false;
            }
            if (side == awl::DevelopmentWallRouteSide::MinZ) {
                saw_neutral_fallback_reprojection =
                    saw_neutral_fallback_reprojection ||
                    (candidate.collision.terrain.initial_edge_fallback &&
                     candidate.collision.static_contact.contact &&
                     candidate.collision.terrain.position != position &&
                     candidate.collision.static_contact.position !=
                         candidate.collision.terrain.position &&
                     candidate.resolved_position != position);
            } else if (candidate.resolved_position != position) {
                return false;
            }
        }
        position = candidate.resolved_position;
        steering = candidate.steering;
        awl::CollisionSurfaceSample surface;
        const bool sampled_surface = awl::sample_type1_collision_surface(
            assets.terrain_bytes().data(), assets.terrain_bytes().size(),
            position[0], position[2], &surface);
        bool height_matches = false;
        if (sampled_surface) {
            height_matches =
                std::fabs(position[1] - surface.height) <= 0.0002f;
        } else {
            awl::CollisionEdgeSample edge;
            height_matches = awl::project_type1_collision_to_edge(
                assets.terrain_bytes().data(), assets.terrain_bytes().size(),
                position[0], position[2], &edge) &&
                std::fabs(position[1] - edge.position[1]) <= 0.0002f;
        }
        if (!height_matches ||
            (candidate.collision.static_contact.contact &&
             (candidate.collision.resolver_contact_bits & 1u) == 0)) {
            return false;
        }
        const float distance = wall.signed_distance(position);
        if (!std::isfinite(distance) || distance < 0.309f) {
            return false;
        }
        approached_wall = approached_wall || distance < previous_distance - 0.02f;
        saw_contact = saw_contact || candidate.collision.static_contact.contact;
        previous_distance = distance;
    }
    std::printf("Local wall route %u: final distance=%.3f contact=%d approach=%d\n",
                static_cast<unsigned>(side), previous_distance, saw_contact ? 1 : 0,
                approached_wall ? 1 : 0);
    const bool expected_distance =
        side == awl::DevelopmentWallRouteSide::MaxZ
            ? previous_distance > 0.6f && previous_distance < 0.9f
            : previous_distance < 0.6f;
    return approached_wall && saw_contact && expected_distance &&
           (side != awl::DevelopmentWallRouteSide::MinZ ||
            saw_neutral_fallback_reprojection) &&
           steering.current_speed == 0.0f;
}

bool replay_local_first_actor_route(
    const awl::WorldMapCollisionAssets& assets) {
    awl::WorldMapSceneFirstCollisionObjects scene;
    scene.category1_actor.collision.identity = 103;
    scene.category1_actor_id =
        awl::world_map_first_actor_selection(3u).actor_id;
    awl::WorldMapCollisionRegistry registry;
    if (!awl::register_world_map_scene_first_collision_objects_from_assets(
            scene, assets, &registry)) {
        return false;
    }
    const awl::WorldMapCollisionSnapshot snapshot = registry.snapshot();
    if (snapshot.first_resolver.size() != 1 ||
        snapshot.first_directional.size() != 1 ||
        snapshot.first_resolver[0].category != 1 ||
        snapshot.first_resolver[0].radius != 0.9f) {
        return false;
    }
    const auto center = snapshot.first_resolver[0].center_world;
    std::array<float, 3> position{center[0] - 2.0f, 0.0f, center[2]};
    awl::CollisionSurfaceSample spawn;
    if (!awl::sample_type1_collision_surface(
            assets.terrain_bytes().data(), assets.terrain_bytes().size(),
            position[0], position[2], &spawn)) {
        return false;
    }
    position[1] = spawn.height;
    awl::NativeInputAccumulator native;
    awl::PadAdapter adapter;
    awl::HsdPadFilter filter;
    awl::WorldMapSteeringState steering;
    native.reset(true);
    native.set_key(awl::NativeKey::D, true);
    bool saw_contact = false;
    float last_distance = 2.0f;
    float minimum_distance = 2.0f;
    bool passed_center_x = false;
    for (int frame = 0; frame < 128; ++frame) {
        if (frame == 120) {
            native.set_key(awl::NativeKey::D, false);
        }
        native.begin_frame();
        adapter.begin_frame(native.frame());
        filter.begin_frame(adapter.frame().sample);
        awl::WorldMapMovementQuery query;
        query.pad = filter.frame();
        query.current_position = position;
        query.current_axis = {0.0f, 0.0f, 1.0f};
        query.steering = steering;
        query.directional_objects = snapshot.first_directional.data();
        query.directional_object_count = snapshot.first_directional.size();
        query.collision.first_objects = snapshot.first_resolver.data();
        query.collision.first_object_count = snapshot.first_resolver.size();
        if (!assets.bind(&query.collision)) {
            return false;
        }
        awl::WorldMapMovementCandidate candidate;
        if (!awl::calculate_world_map_movement_candidate(query, &candidate) ||
            !candidate.movement_enabled) {
            return false;
        }
        if (candidate.collision.first_pass.contact &&
            (candidate.collision.resolver_contact_bits & 4u) == 0) {
            return false;
        }
        position = candidate.resolved_position;
        steering = candidate.steering;
        const float dx = position[0] - center[0];
        const float dz = position[2] - center[2];
        last_distance = std::sqrt(dx * dx + dz * dz);
        minimum_distance = std::min(minimum_distance, last_distance);
        passed_center_x = passed_center_x || dx > 0.5f;
        if (!std::isfinite(last_distance) || last_distance < 1.199f ||
            !std::isfinite(position[1])) {
            return false;
        }
        awl::CollisionSurfaceSample surface;
        if (!awl::sample_type1_collision_surface(
                assets.terrain_bytes().data(), assets.terrain_bytes().size(),
                position[0], position[2], &surface) ||
            std::fabs(position[1] - surface.height) > 0.0002f) {
            return false;
        }
        saw_contact = saw_contact || candidate.collision.first_pass.contact;
    }
    std::printf("Local first actor route: final distance=%.3f minimum=%.3f crossed center X=%d contact=%d\n",
                last_distance, minimum_distance, passed_center_x ? 1 : 0,
                saw_contact ? 1 : 0);
    return saw_contact && passed_center_x &&
           minimum_distance >= 1.199f && minimum_distance < 1.22f &&
           last_distance > 2.0f && steering.current_speed == 0.0f;
}

bool inspect_local_catalog(const char* disc_root) {
    awl_memory_init();
    awl::filesystem_init();
    bool valid = awl::filesystem_mount("/", disc_root);
    if (valid) {
        awl::WorldMapCollisionRecordPools pools;
        valid = pools.load() && pools.record_count(0) == 53 &&
                pools.record_count(2) == 21 && pools.record_count(1) == 54;
        if (valid) {
            std::array<awl::WorldMapRegisteredCollisionObject, 25> fixed{};
            const auto keys = awl::world_map_fixed_collision_record_keys();
            valid = awl::bind_world_map_fixed_collision_records(pools, &fixed);
            for (size_t i = 0; valid && i < fixed.size(); ++i) {
                awl::WorldMapCollisionRecordView record;
                valid = pools.lookup(1, keys[i].archive_index, &record) &&
                        fixed[i].collision.category == keys[i].category &&
                        fixed[i].collision.data == record.data &&
                        fixed[i].collision.size == record.size &&
                        fixed[i].collision.center_local == record.center_local;
            }
        }
        if (valid) {
            std::vector<awl::WorldMapMapseCollisionMatch> matches;
            valid = pools.find_mapse_matches(2, &matches) &&
                    matches.size() == 6;
            for (size_t row = 0; valid && row < matches.size(); ++row) {
                valid = matches[row].record_index == row + 2 &&
                        matches[row].table_flag == 1 &&
                        matches[row].record.data != nullptr;
            }
            valid = valid && pools.find_mapse_matches(11, &matches) &&
                    matches.size() == 1 && matches[0].record_index == 16 &&
                    matches[0].table_flag == 2 &&
                    pools.find_mapse_matches(0, &matches) &&
                    matches.size() == 1 && matches[0].record_index == 0 &&
                    matches[0].table_flag == 3 &&
                    pools.find_mapse_matches(16, &matches) && matches.empty() &&
                    !pools.find_mapse_matches(0, nullptr);
        }
        for (uint32_t phase = 0; valid && phase < 6; ++phase) {
            awl::WorldMapRoomCollisionState state;
            state.phase_index = phase;
            awl::WorldMapCollisionRecordView room;
            valid = pools.lookup_room_object(40, state, &room) ==
                        awl::WorldMapRoomCollisionStatus::Found &&
                    room.data != nullptr && room.analysis != nullptr &&
                    room.analysis->header_byte_6 == 0;
            if (valid) {
                awl::WorldMapRoomConditionInputs inputs;
                inputs.phase_index = phase;
                awl::WorldMapRoomStaticConditions conditions;
                awl::WorldMapRoomStaticAdjustment result;
                const std::array<float, 3> point = room.center_local;
                valid = awl::evaluate_world_map_room_static_conditions(
                            inputs, &conditions) &&
                        awl::resolve_type1_room_static_contact(
                            40, awl::WorldMapRoomCollisionStatus::Found,
                            &room, conditions, 1u, 0u, 0u, point, point, 0.3f,
                            &result) &&
                        result.record_present &&
                        std::isfinite(result.position[0]) &&
                        std::isfinite(result.position[1]) &&
                        std::isfinite(result.position[2]);
            }
        }
        if (valid) {
            awl::WorldMapRoomCollisionState state;
            awl::WorldMapCollisionRecordView room;
            valid = pools.lookup_room_object(4, state, &room) ==
                        awl::WorldMapRoomCollisionStatus::NoMapping &&
                    room.data == nullptr;
        }
        for (const int group : {0, 1, 2}) {
            for (uint32_t index = 0;
                 valid && index < pools.record_count(group); ++index) {
                awl::WorldMapCollisionRecordView view;
                awl::CollisionTreeAnalysis checked;
                valid = pools.lookup(group, index, &view) &&
                        view.data != nullptr && view.analysis != nullptr &&
                        view.analysis->node_count == 1 &&
                        std::isfinite(view.center_local[0]) &&
                        std::isfinite(view.center_local[1]) &&
                        std::isfinite(view.center_local[2]) &&
                        std::isfinite(view.radius_local) &&
                        view.radius_local > 0.0f &&
                        awl::analyze_type1_collision_asset(
                            view.data, view.size, &checked) &&
                        checked.header_byte_6 == 0 &&
                        checked.node_count == view.analysis->node_count;
            }
            awl::WorldMapCollisionRecordView missing;
            valid = valid && !pools.lookup(
                group, static_cast<uint32_t>(pools.record_count(group)),
                &missing) && missing.data == nullptr;
        }
        if (!valid) {
            std::fprintf(stderr, "COL archive catalog validation failed\n");
        } else {
            std::printf("COL archive records: maperase=%zu mapse=%zu roomobj=%zu\n",
                        pools.record_count(0), pools.record_count(2),
                        pools.record_count(1));
        }
    }
    for (uint32_t phase = 0; valid && phase < 6; ++phase) {
        for (int alternate = 0; alternate < 2; ++alternate) {
            awl::WorldMapCollisionAssets assets;
            uint32_t counter = 0;
            awl::WorldMapCollisionSelection selected;
            valid = awl::compose_world_map_collision_counter(
                        {phase, 0u, {}}, &counter);
            const awl::WorldMapCollisionSourceState derived_source{
                counter, static_cast<uint8_t>(alternate)};
            valid = valid && awl::derive_world_map_collision_selection(
                        derived_source, &selected) &&
                    selected.phase_index == phase &&
                    selected.alternate_terrain == (alternate != 0) &&
                    assets.load_from_source_state(derived_source) &&
                    std::strcmp(assets.paths().terrain,
                                alternate != 0 ? "/files/jimen1-move.col"
                                               : "/files/jimen-move.col") == 0;
            awl::WorldMapRoomCollisionState room_state;
            room_state.phase_index = selected.phase_index;
            awl::WorldMapRoomCollisionSelection room_selection;
            awl::WorldMapRoomConditionInputs condition_inputs;
            condition_inputs.phase_index = selected.phase_index;
            awl::WorldMapRoomStaticConditions conditions;
            valid = valid &&
                    awl::select_world_map_room_collision_record(
                        40u, room_state, &room_selection) ==
                        awl::WorldMapRoomCollisionStatus::Found &&
                    room_selection.remapped_id ==
                        (phase == 0 ? 40u : 0x4cu + phase) &&
                    awl::evaluate_world_map_room_static_conditions(
                        condition_inputs, &conditions) &&
                    conditions[2] == (phase == 0) &&
                    conditions[3] == (phase == 0);
            awl::WorldMapCollisionPhaseSetup same_phase;
            awl::WorldMapCollisionPhaseSetup next_phase;
            valid = valid &&
                    awl::plan_world_map_collision_phase_setup(
                        counter, 1, false, &same_phase) &&
                    same_phase.phase_initializer_required &&
                    same_phase.phase_index == phase &&
                    same_phase.counter_word == counter &&
                    awl::plan_world_map_collision_phase_setup(
                        counter, 0, false, &next_phase);
            if (valid && phase < 5) {
                awl::WorldMapCollisionAssets advanced_assets;
                valid = next_phase.phase_initializer_required &&
                        !next_phase.terminal &&
                        next_phase.phase_index == phase + 1u &&
                        advanced_assets.load_from_source_state(
                            {next_phase.counter_word,
                             static_cast<uint8_t>(alternate)}) &&
                        advanced_assets.paths().terrain != nullptr;
            } else if (valid) {
                valid = next_phase.terminal &&
                        !next_phase.phase_initializer_required;
            }
            if (!valid) {
                std::fprintf(stderr, "COL catalog failed at phase %u terrain %d\n",
                             phase, alternate);
                break;
            }
            awl::CollisionSurfaceSample surface;
            awl::CollisionCategory1MovementQuery query;
            query.moving_radius = 0.3f;
            awl::CollisionCategory1MovementAdjustment result;
            const bool sampled = awl::sample_type1_collision_surface(
                assets.terrain_bytes().data(), assets.terrain_bytes().size(),
                120.0f, 168.0f, &surface);
            valid = sampled && assets.bind(&query) &&
                awl::resolve_type1_category1_movement_candidate(
                    query, {120.0f, surface.height, 168.0f},
                    {120.1f, surface.height, 168.0f}, &result) &&
                std::isfinite(result.position[0]) &&
                std::isfinite(result.position[1]) &&
                std::isfinite(result.position[2]);
            if (!valid) {
                std::fprintf(stderr,
                             "COL movement candidate failed at phase %u terrain %d\n",
                             phase, alternate);
                break;
            }
            if (phase == 0) {
                constexpr std::array<std::array<float, 2>, 5> seed_xz{{
                    {263.0f, 179.0f}, {277.0f, 218.0f},
                    {185.0f, 237.0f}, {75.0f, 113.5f},
                    {279.0f, 116.0f},
                }};
                for (uint32_t draw = 0; valid && draw < 5; ++draw) {
                    awl::WorldMapSceneFirstCollisionObjects scene;
                    scene.category1_actor.collision.identity = 1;
                    scene.category1_actor_id =
                        awl::world_map_first_actor_selection(draw).actor_id;
                    awl::WorldMapCollisionRegistry registry;
                    valid = awl::register_world_map_scene_first_collision_objects_from_assets(
                                scene, assets, &registry) &&
                            registry.snapshot().first_resolver.size() == 1;
                    if (valid) {
                        const auto center =
                            registry.snapshot().first_resolver[0].center_world;
                        valid = center[0] == seed_xz[draw][0] &&
                                std::isfinite(center[1]) &&
                                center[2] == seed_xz[draw][1];
                        awl::WorldMapFirstActorStepState actor;
                        actor.actor_id = scene.category1_actor_id;
                        actor.moving = true;
                        actor.timer = 1;
                        actor.proposal = center;
                        actor.target = {center[0] + 1.0f, center[1], center[2]};
                        actor.heading = {1.0f, 0.0f, 0.0f};
                        const auto actor_snapshot = registry.snapshot();
                        query.source_identity = 1;
                        query.first_objects = actor_snapshot.first_resolver.data();
                        query.first_object_count =
                            actor_snapshot.first_resolver.size();
                        awl::WorldMapFirstActorResolvedStep finished;
                        valid = valid &&
                            awl::calculate_world_map_first_actor_step(
                                scene.category1_actor_id, actor, query,
                                &finished) &&
                            finished.collision.first_pass.queried_objects == 0 &&
                            finished.collision.third_pass.queried_objects == 0 &&
                            finished.collision.static_contact.slot_present &&
                            std::isfinite(finished.finished.state.proposal[0]) &&
                            std::isfinite(finished.finished.state.proposal[1]) &&
                            std::isfinite(finished.finished.state.proposal[2]);
                        std::printf("COL terrain %d actor ID 0x%02X spawn probe: y=%.3f\n",
                                    alternate, scene.category1_actor_id,
                                    center[1]);
                    }
                }
                if (!valid) {
                    std::fprintf(stderr,
                                 "COL actor spawn probes failed at terrain %d\n",
                                 alternate);
                    break;
                }
            }
            const auto& analysis = assets.static_analysis();
            const float center_x =
                (analysis.root_min[0] + analysis.root_max[0]) * 0.5f;
            const float center_z =
                (analysis.root_min[2] + analysis.root_max[2]) * 0.5f;
            const std::array<std::array<float, 2>, 3> wall_seeds{{
                {analysis.root_min[0] - 1.0f, center_z},
                {center_x, analysis.root_min[2] - 1.0f},
                {center_x, analysis.root_max[2] + 1.0f},
            }};
            const std::array<awl::DevelopmentWallRouteSide, 3> wall_sides{{
                awl::DevelopmentWallRouteSide::MinX,
                awl::DevelopmentWallRouteSide::MinZ,
                awl::DevelopmentWallRouteSide::MaxZ,
            }};
            awl::CollisionCategory1StaticAdjustment wall;
            std::array<LocalWallProbe, 3> sampled_walls{};
            for (size_t side = 0; valid && side < wall_sides.size(); ++side) {
                LocalWallProbe geometry;
                valid = probe_local_static_wall(
                    assets, &wall, &geometry,
                    wall_seeds[side][0], wall_seeds[side][1]);
                for (size_t prior = 0; valid && prior < side; ++prior) {
                    const auto& other = sampled_walls[prior];
                    valid = geometry.leaf_offset != other.leaf_offset ||
                            geometry.triangle_index != other.triangle_index ||
                            geometry.edge_index != other.edge_index;
                }
                if (valid) {
                    sampled_walls[side] = geometry;
                }
                if (valid) {
                    awl::DevelopmentWallRoute route;
                    valid = awl::derive_development_wall_route(
                                assets, &route, wall_sides[side]) &&
                            std::fabs(route.mid_x - geometry.mid_x) < 0.0002f &&
                            std::fabs(route.mid_z - geometry.mid_z) < 0.0002f &&
                            std::fabs(route.normal_x - geometry.normal_x) < 0.0002f &&
                            std::fabs(route.normal_z - geometry.normal_z) < 0.0002f &&
                            std::fabs(route.signed_distance(route.start) - 1.0f) < 0.0002f;
                    if (valid) {
                        valid = replay_local_static_wall_route(
                            assets, route, wall_sides[side]);
                    }
                }
                if (!valid) {
                    std::fprintf(stderr,
                                 "COL wall route %zu failed at phase %u terrain %d\n",
                                 side, phase, alternate);
                }
            }
            if (!valid) {
                break;
            }
            if (phase == 0 && !replay_local_first_actor_route(assets)) {
                std::fprintf(stderr,
                             "COL first actor route failed at terrain %d\n",
                             alternate);
                valid = false;
                break;
            }
            std::printf("COL phase %u terrain %d: %s (%zu bytes), %s (%zu bytes), candidate=(%.3f, %.3f, %.3f), static wall contact=%d\n",
                        phase, alternate, assets.paths().terrain,
                        assets.terrain_bytes().size(),
                        assets.paths().static_objects,
                        assets.static_bytes().size(), result.position[0],
                        result.position[1], result.position[2],
                        wall.contact ? 1 : 0);
        }
    }
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    return valid;
}

} // namespace

int main(int argc, char** argv) {
    test_world_map_collision_archive();
    test_world_map_event_conditions();
    test_world_map_action_asset();
    test_world_map_action_script();
    test_world_map_action_execution();
    test_world_map_action_callbacks();
    test_world_map_command_preparation();
    test_world_map_request_consumption();
    test_world_map_request_transition();
    test_world_map_room_collision_mapping();
    test_world_map_packed_saved_conditions();
    test_world_map_room_condition_evaluator();
    test_world_map_room_static_stage();
    test_world_map_archive_rejects_untranslated_tree_shape();
    test_valid_structures();
    test_rejects_unsupported_or_truncated_files();
    test_rejects_invalid_offsets();
    test_primary_surface_sample();
    test_nearest_edge_fallback();
    test_terrain_height_adjustment();
    test_resolver_height_resampling();
    test_dynamic_contact_broad_phase();
    test_dynamic_contact_edge_adjustment();
    test_dynamic_contact_vertex_adjustment();
    test_dynamic_contact_narrow_phase();
    test_dynamic_object_contact();
    test_first_dynamic_object_pass();
    test_later_dynamic_object_pass();
    test_third_dynamic_object_pass();
    test_category1_static_and_movement_candidate();
    test_world_map_directional_contact_search();
    test_world_map_camera_followup();
    test_world_map_camera_target();
    test_world_map_camera_view();
    test_world_map_camera_initial_profile();
    test_world_map_player_camera_placement();
    test_world_map_player_message_camera_update();
    test_world_map_player_movement_camera_update();
    test_world_map_camera_post_update();
    test_world_map_camera_collision_height();
    test_world_map_movement_candidate_sequence();
    test_world_map_movement_contact_tail();
    test_world_map_polygon_contact();
    test_world_map_trigger_asset();
    test_synthetic_player_route_replay();
    test_world_map_scene_position_bucket_decision();
    test_world_map_scene_bucket_registry();
    test_world_map_player_scene_message_1f();
    test_world_map_player_fixed_scene_message_1f();
    test_world_map_scene_mode_request();
    test_world_map_player_sequence_reset();
    test_world_map_player_fixed_transition_step();
    test_world_map_collision_mode_flags();
    test_world_map_scene_first_collision_registration();
    test_world_map_first_actor_step();
    test_world_map_first_actor_resolved_step();
    test_world_map_first_actor_target_selection();
    test_world_map_first_actor_action_choice();
    test_world_map_first_actor_heading();
    test_world_map_fixed_collision_registration();
    test_world_map_collision_registry();
    test_radius_vertex_adjustment();
    test_radius_edge_adjustment();
    test_radius_pass_sequence();
    test_terrain_radius_adjustment();
    test_world_map_collision_asset_provider();

    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--catalog-local") == 0) {
            if (++index >= argc || !inspect_local_catalog(argv[index])) {
                ++failures;
            }
        } else if (std::strcmp(argv[index], "--replay-local") == 0) {
            if (++index >= argc || !replay_local_player_route(argv[index])) {
                ++failures;
            }
        } else if (std::strcmp(argv[index], "--camera-local") == 0) {
            if (++index >= argc || !check_local_camera_collision(argv[index])) {
                ++failures;
            }
        } else if (std::strcmp(argv[index], "--trigger-local") == 0) {
            if (++index >= argc || !check_local_trigger_asset(argv[index])) {
                ++failures;
            }
        } else if (std::strcmp(argv[index], "--event-conditions-local") == 0) {
            if (++index >= argc ||
                !check_local_event_conditions(argv[index])) {
                ++failures;
            }
        } else if (!inspect_local_asset(argv[index])) {
            ++failures;
        }
    }
    if (failures == 0) {
        std::puts("Collision asset tests passed.");
    }
    return failures == 0 ? 0 : 1;
}
