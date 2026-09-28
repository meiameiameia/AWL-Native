#pragma once

#include "awl/world_map_movement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapTriggerPolygon {
    uint32_t mode = 0;
    std::vector<std::array<float, 3>> vertices;
};

// Supplies the still-untranslated FUN_8010A9C0(state, 3, slot, 0) result.
// Return false if the request could not be evaluated; write acceptance only
// on success. Called only after that slot's polygon contact succeeds.
using WorldMapTriggerStateRequest = bool (*)(
    int32_t slot, void* context, bool* accepted);

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
    // Executes the DOL's category-1 slot order using this validated asset.
    // The callback supplies the state-request result; player reset is only
    // reported. On failure output is unchanged, but callback side effects
    // already performed by an earlier slot cannot be rolled back.
    [[nodiscard]] bool evaluate_movement_contact_tail(
        int32_t collision_category,
        const std::array<float, 3>& prior_position,
        const std::array<float, 3>& resolved_position,
        WorldMapTriggerStateRequest request_state,
        void* request_context,
        WorldMapMovementContactTail* output) const;

private:
    std::array<WorldMapTriggerPolygon, 2> category1_polygons_{};
    bool loaded_ = false;
};

} // namespace awl
