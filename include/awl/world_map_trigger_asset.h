#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapTriggerPolygon {
    uint32_t mode = 0;
    std::vector<std::array<float, 3>> vertices;
};

// Owns the validated /files/trigger.spl polygons used by the category-1
// movement tail. The DOL maps category 1 to group 55, which has two slots
// in the verified asset. A returned polygon pointer lasts until clear(),
// load(), load_from_bytes(), or destruction.
class WorldMapTriggerAsset {
public:
    [[nodiscard]] bool load();
    [[nodiscard]] bool load_from_bytes(const uint8_t* bytes, size_t size);
    void clear();

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] const WorldMapTriggerPolygon* category1_polygon(
        size_t slot) const;
    [[nodiscard]] bool query_category1_contact(
        size_t slot, const std::array<float, 3>& prior_position,
        const std::array<float, 3>& resolved_position, bool* contact) const;

private:
    std::array<WorldMapTriggerPolygon, 2> category1_polygons_{};
    bool loaded_ = false;
};

} // namespace awl
