#include "awl/world_map_trigger_asset.h"

#include "awl/filesystem.h"
#include "awl/platform.h"
#include "awl/world_map_movement.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace awl {

namespace {

constexpr uint32_t kMarker = 0xF0F0E1ECu;
constexpr size_t kGroupCount = 61;
constexpr size_t kCategory1Group = 55;

uint32_t read_be32(const uint8_t* bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

float read_be_float(const uint8_t* bytes, size_t offset) {
    const uint32_t bits = read_be32(bytes, offset);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits), "SPL requires 32-bit float");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct PolygonReference {
    size_t offset = 0;
    size_t group = 0;
    size_t slot = 0;
};

} // namespace

void WorldMapTriggerAsset::clear() {
    category1_polygons_ = {};
    loaded_ = false;
}

bool WorldMapTriggerAsset::load_from_bytes(const uint8_t* bytes,
                                            size_t size) {
    clear();
    constexpr size_t header_size = 8 + kGroupCount * 4;
    if (bytes == nullptr || size < header_size ||
        read_be32(bytes, 0) != kMarker ||
        read_be32(bytes, 4) != kGroupCount) {
        return false;
    }

    // FUN_8019AAB0 relocates the root offsets, then each group's polygon
    // offsets. The verified file packs both offset tables contiguously.
    std::vector<PolygonReference> polygons;
    size_t group_cursor = header_size;
    for (size_t group = 0; group < kGroupCount; ++group) {
        if (group_cursor > size - 4 ||
            read_be32(bytes, 8 + group * 4) != group_cursor) {
            return false;
        }
        const uint32_t count = read_be32(bytes, group_cursor);
        if (count > (size - group_cursor - 4) / 4 ||
            (group == kCategory1Group && count != 2)) {
            return false;
        }
        for (size_t slot = 0; slot < count; ++slot) {
            polygons.push_back({read_be32(bytes, group_cursor + 4 + slot * 4),
                                group, slot});
        }
        group_cursor += 4 + static_cast<size_t>(count) * 4;
    }

    std::array<WorldMapTriggerPolygon, 2> parsed{};
    size_t polygon_cursor = group_cursor;
    for (const PolygonReference& reference : polygons) {
        if (polygon_cursor > size || size - polygon_cursor < 8 ||
            reference.offset != polygon_cursor) {
            return false;
        }
        const uint32_t mode = read_be32(bytes, polygon_cursor);
        const uint32_t vertex_count = read_be32(bytes, polygon_cursor + 4);
        if (mode > 3 || vertex_count < 2 ||
            vertex_count > (size - polygon_cursor - 8) / 12) {
            return false;
        }
        WorldMapTriggerPolygon polygon;
        polygon.mode = mode;
        polygon.vertices.reserve(vertex_count);
        for (size_t vertex = 0; vertex < vertex_count; ++vertex) {
            const size_t offset = polygon_cursor + 8 + vertex * 12;
            const std::array<float, 3> point{
                read_be_float(bytes, offset),
                read_be_float(bytes, offset + 4),
                read_be_float(bytes, offset + 8)};
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
                !std::isfinite(point[2])) {
                return false;
            }
            polygon.vertices.push_back(point);
        }
        if (reference.group == kCategory1Group) {
            parsed[reference.slot] = std::move(polygon);
        }
        polygon_cursor += 8 + static_cast<size_t>(vertex_count) * 12;
    }
    if (polygon_cursor != size) {
        return false;
    }
    category1_polygons_ = std::move(parsed);
    loaded_ = true;
    return true;
}

bool WorldMapTriggerAsset::load() {
    clear();
    void* raw = nullptr;
    size_t size = 0;
    constexpr const char* path = "/files/trigger.spl";
    if (!filesystem_read_entire_file(path, &raw, &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    if (!load_from_bytes(static_cast<const uint8_t*>(raw), size)) {
        AWL_LOG_ERROR("Unsupported or malformed trigger asset: %s", path);
        return false;
    }
    return true;
}

const WorldMapTriggerPolygon* WorldMapTriggerAsset::category1_polygon(
    size_t slot) const {
    return loaded_ && slot < category1_polygons_.size()
               ? &category1_polygons_[slot]
               : nullptr;
}

bool WorldMapTriggerAsset::query_category1_contact(
    size_t slot, const std::array<float, 3>& prior_position,
    const std::array<float, 3>& resolved_position, bool* contact) const {
    const WorldMapTriggerPolygon* polygon = category1_polygon(slot);
    return polygon != nullptr &&
           query_world_map_polygon_contact(
               static_cast<int32_t>(polygon->mode), polygon->vertices,
               prior_position, resolved_position, contact);
}

bool WorldMapTriggerAsset::evaluate_movement_contact_tail(
    int32_t collision_category,
    const std::array<float, 3>& prior_position,
    const std::array<float, 3>& resolved_position,
    WorldMapTriggerStateRequest request_state,
    void* request_context,
    WorldMapMovementContactTail* output) const {
    if (output == nullptr ||
        (collision_category == 1 && (!loaded_ || request_state == nullptr))) {
        return false;
    }
    std::array<WorldMapMovementContactSlotOutcome, 2> outcomes{};
    if (collision_category == 1) {
        for (int32_t slot = 0; slot < 2; ++slot) {
            auto& outcome = outcomes[static_cast<size_t>(slot)];
            if (!query_category1_contact(
                    static_cast<size_t>(slot), prior_position,
                    resolved_position, &outcome.polygon_contact)) {
                return false;
            }
            if (!outcome.polygon_contact) {
                continue;
            }
            if (!request_state(slot, request_context,
                               &outcome.state_request_accepted)) {
                return false;
            }
            if (outcome.state_request_accepted) {
                break;
            }
        }
    }
    return plan_world_map_movement_contact_tail(
        collision_category, prior_position, resolved_position,
        outcomes, output);
}

} // namespace awl
