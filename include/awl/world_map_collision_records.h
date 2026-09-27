#pragma once

#include "awl/collision_asset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace awl {

struct WorldMapCollisionRecordView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const CollisionTreeAnalysis* analysis = nullptr;
    const char* name = nullptr;
    // FUN_80191D54's local bounding sphere for the origin-selected leaf.
    std::array<float, 3> center_local{};
    float radius_local = 0.0f;
};

struct WorldMapMapseCollisionMatch {
    uint32_t record_index = 0;
    uint32_t table_flag = 0;
    WorldMapCollisionRecordView record{};
};

// Only the three state bytes read by FUN_8002057C for room-object remapping.
// Their gameplay meanings are not established; phase_index is the validated
// 0..5 result of FUN_8000FEE8, supplied by the caller.
struct WorldMapRoomCollisionState {
    uint32_t phase_index = 0;
    bool state_299a5 = false;
    bool state_299a6 = false;
    bool state_299ae = false;
};

enum class WorldMapRoomCollisionStatus {
    Found,
    NoMapping,
    UnsupportedInput,
    MissingRecord,
};

struct WorldMapRoomCollisionSelection {
    uint32_t remapped_id = 0;
    uint32_t record_index = 0;
};

// FUN_8002057C -> FUN_800213C4's bounded collision-category remap and lookup.
[[nodiscard]] WorldMapRoomCollisionStatus select_world_map_room_collision_record(
    uint32_t category,
    const WorldMapRoomCollisionState& state,
    WorldMapRoomCollisionSelection* out);

// Predicate results for the 14 ordered condition IDs in DOL table 0x80299698.
// Live FUN_80024EBC condition ownership is not translated.
using WorldMapRoomStaticConditions = std::array<bool, 14>;

[[nodiscard]] std::array<uint32_t, 14> world_map_room_static_condition_ids();

// A caller-owned packed-value table read by FUN_8018972C. The DOL stores the
// element width at +4 and the byte pointer at +0x14; this view adds a length
// so malformed or incomplete native data can be rejected.
struct WorldMapPackedSavedValues {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t width_bits = 0;
};

[[nodiscard]] bool read_world_map_packed_saved_value(
    const WorldMapPackedSavedValues& values,
    size_t index,
    uint32_t* out);

// Inputs read by FUN_80024EBC for the 14 room-mask conditions. Saved values
// can be decoded from caller-owned packed tables below; their live owner and
// update path are not established yet.
struct WorldMapRoomConditionInputs {
    uint32_t phase_index = 0;
    uint8_t state_299a7 = 0;
    uint8_t state_299af = 0;
    std::array<int8_t, 3> counters_118bc_to_118be{};
    bool value_120a4_135_nonzero = false;
    bool value_120a4_170_nonzero = false;
    std::array<bool, 16> values_14ad4_nonzero{};
};

[[nodiscard]] bool decode_world_map_room_saved_conditions(
    const WorldMapPackedSavedValues& values_120a4,
    const WorldMapPackedSavedValues& values_14ad4,
    WorldMapRoomConditionInputs* inputs);

[[nodiscard]] bool evaluate_world_map_room_static_conditions(
    const WorldMapRoomConditionInputs& inputs,
    WorldMapRoomStaticConditions* out);

struct WorldMapRoomStaticAdjustment {
    std::array<float, 3> position{};
    uint32_t surface_mask = 0;
    bool record_present = false;
    bool contact = false;
    bool reverted_horizontal_to_prior = false;
    CollisionDynamicContactAdjustment narrow_phase{};
};

[[nodiscard]] uint32_t world_map_room_static_surface_mask(
    const WorldMapRoomStaticConditions& conditions,
    uint32_t resolver_flags);

// Isolates FUN_8001E498's non-category-1 static stage. The caller supplies
// the lookup outcome and a validated record view; NoMapping copies the
// proposal, while missing/unsupported data fails. This does not own runtime
// condition state or accept a player's position. Carried narrow-phase flags
// and accumulated resolver-contact bits are separate DOL values.
[[nodiscard]] bool resolve_type1_room_static_contact(
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
    WorldMapRoomStaticAdjustment* adjustment);

// Owns one verified ARC and its embedded type-1 COL files. Views borrow the
// archive's storage and are invalidated by clear(), parse(), or destruction.
class WorldMapCollisionArchive {
public:
    WorldMapCollisionArchive() = default;
    WorldMapCollisionArchive(const WorldMapCollisionArchive&) = delete;
    WorldMapCollisionArchive& operator=(const WorldMapCollisionArchive&) = delete;
    WorldMapCollisionArchive(WorldMapCollisionArchive&&) = delete;
    WorldMapCollisionArchive& operator=(WorldMapCollisionArchive&&) = delete;

    [[nodiscard]] bool parse(std::vector<uint8_t> bytes);
    void clear();
    [[nodiscard]] size_t record_count() const { return records_.size(); }
    [[nodiscard]] bool lookup(uint32_t index, WorldMapCollisionRecordView* out) const;

private:
    struct Record {
        uint32_t offset = 0;
        uint32_t size = 0;
        std::string name;
        CollisionTreeAnalysis analysis{};
        std::array<float, 3> center_local{};
        float radius_local = 0.0f;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Record> records_;
};

// FUN_80020E38's group routing: 1=roomobj, 2=mapse, all others=maperase.
// The runtime object list and its index/group values are supplied elsewhere.
class WorldMapCollisionRecordPools {
public:
    [[nodiscard]] bool load();
    void clear();
    [[nodiscard]] bool lookup(int32_t group, uint32_t index,
                              WorldMapCollisionRecordView* out) const;
    [[nodiscard]] size_t record_count(int32_t group) const;
    // FUN_8001D714 visits every matching row, including repeated IDs.
    // A loaded pool returns true with an empty result for an unknown ID.
    // Output views borrow this pool's storage and follow the same lifetime.
    [[nodiscard]] bool find_mapse_matches(
        uint32_t id, std::vector<WorldMapMapseCollisionMatch>* out) const;
    [[nodiscard]] WorldMapRoomCollisionStatus lookup_room_object(
        uint32_t category,
        const WorldMapRoomCollisionState& state,
        WorldMapCollisionRecordView* out) const;

private:
    WorldMapCollisionArchive maperase_;
    WorldMapCollisionArchive mapse_;
    WorldMapCollisionArchive roomobj_;
};

} // namespace awl
