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

// FUN_8002057C -> FUN_800213C4's bounded ID remap and static table lookup.
[[nodiscard]] WorldMapRoomCollisionStatus select_world_map_room_collision_record(
    uint32_t object_id,
    const WorldMapRoomCollisionState& state,
    WorldMapRoomCollisionSelection* out);

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
        uint32_t object_id,
        const WorldMapRoomCollisionState& state,
        WorldMapCollisionRecordView* out) const;

private:
    WorldMapCollisionArchive maperase_;
    WorldMapCollisionArchive mapse_;
    WorldMapCollisionArchive roomobj_;
};

} // namespace awl
