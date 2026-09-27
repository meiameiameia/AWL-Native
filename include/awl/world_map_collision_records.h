#pragma once

#include "awl/collision_asset.h"

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
};

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

private:
    WorldMapCollisionArchive maperase_;
    WorldMapCollisionArchive mapse_;
    WorldMapCollisionArchive roomobj_;
};

} // namespace awl
