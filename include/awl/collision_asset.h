#pragma once

#include <cstddef>
#include <cstdint>

namespace awl {

struct CollisionTreeAnalysis {
    uint8_t format = 0;
    uint8_t header_byte_5 = 0;
    uint8_t header_byte_6 = 0;
    uint8_t header_byte_7 = 0;
    size_t node_count = 0;
    size_t leaf_count = 0;
    uint32_t max_depth = 0;
};

// Validates the relocatable type-1 tree structure used by the verified
// movement collision assets. Leaf payload semantics are not decoded yet.
[[nodiscard]] bool analyze_type1_collision_asset(
    const uint8_t* data,
    size_t size,
    CollisionTreeAnalysis* analysis);

} // namespace awl
