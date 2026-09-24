#include "awl/collision_asset.h"

#include <array>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace awl {

namespace {

constexpr uint32_t kCollisionFileMarker = 0xE7E3F1F4u;
constexpr uint8_t kSupportedFormat = 1;
constexpr uint32_t kRootOffset = 8;
constexpr uint32_t kNodeSize = 0x34;
constexpr std::array<uint32_t, 4> kChildFields = {0x0c, 0x10, 0x14, 0x18};
constexpr std::array<uint32_t, 3> kNodeRelativeFields = {0x28, 0x2c, 0x30};

uint32_t read_be32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

bool contains_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

} // namespace

bool analyze_type1_collision_asset(const uint8_t* data,
                                   size_t size,
                                   CollisionTreeAnalysis* analysis) {
    if (analysis != nullptr) {
        *analysis = {};
    }
    if (data == nullptr || analysis == nullptr ||
        !contains_range(size, kRootOffset, kNodeSize) ||
        read_be32(data) != kCollisionFileMarker ||
        data[4] != kSupportedFormat) {
        return false;
    }

    struct PendingNode {
        uint32_t offset;
        uint32_t depth;
    };

    std::vector<PendingNode> pending{{kRootOffset, 0}};
    std::unordered_set<uint32_t> visited;
    size_t leaf_count = 0;
    uint32_t max_depth = 0;

    while (!pending.empty()) {
        const PendingNode node = pending.back();
        pending.pop_back();

        if ((node.offset & 3u) != 0 ||
            !contains_range(size, node.offset, kNodeSize) ||
            !visited.insert(node.offset).second) {
            return false;
        }
        if (node.depth > max_depth) {
            max_depth = node.depth;
        }

        for (const uint32_t field : kNodeRelativeFields) {
            const uint32_t relative = read_be32(data + node.offset + field);
            const uint64_t target = static_cast<uint64_t>(node.offset) + relative;
            // One-past-the-end is valid for an empty payload range.
            if (target > size) {
                return false;
            }
        }

        const uint32_t first_child =
            read_be32(data + node.offset + kChildFields.front());
        if (first_child == 0) {
            ++leaf_count;
            continue;
        }

        if (node.depth == UINT32_MAX) {
            return false;
        }
        for (const uint32_t field : kChildFields) {
            const uint32_t child = read_be32(data + node.offset + field);
            if (child == 0 || (child & 3u) != 0 ||
                !contains_range(size, child, kNodeSize)) {
                return false;
            }
            pending.push_back({child, node.depth + 1});
        }
    }

    analysis->format = data[4];
    analysis->header_byte_5 = data[5];
    analysis->header_byte_6 = data[6];
    analysis->header_byte_7 = data[7];
    analysis->node_count = visited.size();
    analysis->leaf_count = leaf_count;
    analysis->max_depth = max_depth;
    return true;
}

} // namespace awl
