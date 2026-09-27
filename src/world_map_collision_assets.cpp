#include "awl/world_map_collision_assets.h"

#include "awl/filesystem.h"
#include "awl/platform.h"

#include <array>
#include <memory>

namespace awl {

namespace {

bool read_supported_col(const char* path,
                        uint8_t required_header_byte_6,
                        std::vector<uint8_t>* bytes,
                        CollisionTreeAnalysis* analysis) {
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file(path, &raw, &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    const auto* first = static_cast<const uint8_t*>(raw);
    bytes->assign(first, first + size);
    if (!analyze_type1_collision_asset(bytes->data(), bytes->size(), analysis)) {
        AWL_LOG_ERROR("Unsupported or malformed collision asset: %s", path);
        return false;
    }
    if (analysis->header_byte_6 != required_header_byte_6) {
        AWL_LOG_ERROR("Unexpected collision mode in %s: %u", path,
                      static_cast<unsigned>(analysis->header_byte_6));
        return false;
    }
    return true;
}

} // namespace

bool derive_world_map_collision_selection(
    const WorldMapCollisionSourceState& source,
    WorldMapCollisionSelection* selection) {
    if (selection != nullptr) {
        *selection = {};
    }
    if (selection == nullptr || source.terrain_variant_byte > 1) {
        return false;
    }
    // FUN_8000FFB0's unsigned division, followed by FUN_8000FEE8's
    // six ordered unsigned subtract-and-compare thresholds at 0x8023DF70.
    uint32_t remaining = source.phase_counter_word / 34560000u;
    constexpr std::array<uint32_t, 6> thresholds{1u, 2u, 3u, 2u, 1u, 1u};
    uint32_t phase = 0;
    for (const uint32_t threshold : thresholds) {
        if (remaining < threshold) {
            break;
        }
        remaining -= threshold;
        ++phase;
    }
    selection->phase_index = phase < 6 ? phase : 5;
    selection->alternate_terrain = source.terrain_variant_byte == 1;
    return true;
}

bool select_world_map_collision_asset_paths(
    uint32_t phase_index,
    bool alternate_terrain,
    WorldMapCollisionAssetPaths* paths) {
    if (paths != nullptr) {
        *paths = {};
    }
    if (paths == nullptr || phase_index > 5) {
        return false;
    }
    // 0x80299B50's six entries; entries 4 and 5 share mapobj5.col.
    constexpr std::array<const char*, 6> static_paths{
        "/files/mapobj.col", "/files/mapobj2.col", "/files/mapobj3.col",
        "/files/mapobj4.col", "/files/mapobj5.col", "/files/mapobj5.col"};
    paths->terrain = alternate_terrain ? "/files/jimen1-move.col"
                                       : "/files/jimen-move.col";
    paths->static_objects = static_paths[phase_index];
    return true;
}

bool WorldMapCollisionAssets::load(uint32_t phase_index,
                                   bool alternate_terrain) {
    clear();
    WorldMapCollisionAssetPaths selected;
    if (!select_world_map_collision_asset_paths(
            phase_index, alternate_terrain, &selected)) {
        return false;
    }
    std::vector<uint8_t> terrain;
    std::vector<uint8_t> static_objects;
    CollisionTreeAnalysis terrain_analysis;
    CollisionTreeAnalysis static_analysis;
    if (!read_supported_col(selected.terrain, 1, &terrain, &terrain_analysis) ||
        !read_supported_col(selected.static_objects, 0, &static_objects,
                            &static_analysis)) {
        return false;
    }
    paths_ = selected;
    terrain_.swap(terrain);
    static_objects_.swap(static_objects);
    terrain_analysis_ = terrain_analysis;
    static_analysis_ = static_analysis;
    return true;
}

bool WorldMapCollisionAssets::load_from_source_state(
    const WorldMapCollisionSourceState& source) {
    WorldMapCollisionSelection selection;
    if (!derive_world_map_collision_selection(source, &selection)) {
        clear();
        return false;
    }
    return load(selection.phase_index, selection.alternate_terrain);
}

void WorldMapCollisionAssets::clear() {
    paths_ = {};
    terrain_.clear();
    static_objects_.clear();
    terrain_analysis_ = {};
    static_analysis_ = {};
}

bool WorldMapCollisionAssets::bind(
    CollisionCategory1MovementQuery* query) const {
    if (query == nullptr) {
        return false;
    }
    if (terrain_.empty() || static_objects_.empty()) {
        query->terrain_data = nullptr;
        query->terrain_size = 0;
        query->static_data = nullptr;
        query->static_size = 0;
        return false;
    }
    query->terrain_data = terrain_.data();
    query->terrain_size = terrain_.size();
    query->static_data = static_objects_.data();
    query->static_size = static_objects_.size();
    return true;
}

} // namespace awl
