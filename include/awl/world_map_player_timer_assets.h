#pragma once
#include "awl/world_map_animation_initializer.h"
#include <array>
#include <memory>

namespace awl {
enum class WorldMapPlayerTimerBank { Eyes, Mouth };
struct WorldMapPlayerTimerResourceView {
    uint64_t identity = 0;
    uint32_t offset = 0, size = 0;
    const uint8_t* data = nullptr; // Borrowed immutable OPAQUE physical slice.
};
enum class WorldMapPlayerTimerAssetsStatus {
    Decoded, Loaded, Bound, RequiresAssets, RequiresRow, RequiresClock,
    InvalidInput, UnsupportedLayout, ReadFailure, AllocationFailure
};
struct WorldMapPlayerTimerBindingResult {
    WorldMapPlayerTimerAssetsStatus status = WorldMapPlayerTimerAssetsStatus::InvalidInput;
    WorldMapPlayerTimerBank bank = WorldMapPlayerTimerBank::Eyes;
    std::optional<uint32_t> required_row;
};
// Owns raw char_com_eye/mouth TAMs and their two-level row/column offsets.
// Relative offsets replace 824C's packed pointer/magic relocation. Resource
// slices end at the next physical resource or EOF (native safety policy);
// their animation bodies, sampling and texture application are unsupported.
class WorldMapPlayerTimerAssets {
public:
    WorldMapPlayerTimerAssets(const WorldMapPlayerTimerAssets&) = delete;
    WorldMapPlayerTimerAssets& operator=(const WorldMapPlayerTimerAssets&) = delete;
    size_t row_count(WorldMapPlayerTimerBank bank) const;
    size_t column_count(WorldMapPlayerTimerBank bank, uint32_t row) const;
    uint64_t table_identity(WorldMapPlayerTimerBank bank) const;
    // Invalid bank/row/column or output preserves the borrowed output view.
    [[nodiscard]] bool resource(WorldMapPlayerTimerBank bank, uint32_t row, uint32_t column,
        WorldMapPlayerTimerResourceView* out) const;
private:
    WorldMapPlayerTimerAssets() = default;
    struct Table {
        struct Row { uint32_t first = 0, count = 0; };
        struct Resource { uint32_t offset = 0, size = 0; };
        std::vector<uint8_t> bytes;
        std::vector<Row> rows;
        std::vector<Resource> resources;
    };
    static bool parse(const std::vector<uint8_t>& bytes, Table* out);
    friend WorldMapPlayerTimerAssetsStatus decode_world_map_player_timer_assets(
        const std::vector<uint8_t>&, const std::vector<uint8_t>&, std::shared_ptr<const WorldMapPlayerTimerAssets>*);
    friend WorldMapPlayerTimerAssetsStatus load_world_map_player_timer_assets(
        std::shared_ptr<const WorldMapPlayerTimerAssets>*);
    std::array<Table,2> tables_;
};
// Decode both raw TAMs before replacing out. Relocated/unknown markers,
// empty rows, overlapping pointer arrays, metadata/resource overlap,
// misalignment and truncated extents reject explicitly. Resources may alias.
[[nodiscard]] WorldMapPlayerTimerAssetsStatus decode_world_map_player_timer_assets(
    const std::vector<uint8_t>& eyes, const std::vector<uint8_t>& mouth,
    std::shared_ptr<const WorldMapPlayerTimerAssets>* out);
// F2DC's player-table order: eyes, then mouth. Other two animal tables and
// original archive-manager activation/release are outside this owner. Pair
// publication is atomic; a complete retained owner skips file I/O.
[[nodiscard]] WorldMapPlayerTimerAssetsStatus load_world_map_player_timer_assets(
    std::shared_ptr<const WorldMapPlayerTimerAssets>* out);
struct WorldMapPlayerTimerBinding {
    std::shared_ptr<const WorldMapPlayerTimerAssets> assets;
    WorldMapAnimationInitializerObservations observations;
};
// F3E8's changed first/second type resets: resolve first row, require the raw
// clock snapshot, then resolve second. Supplies column zero with retained
// native table/resource keys. Failure or allocation leaves out unchanged.
[[nodiscard]] WorldMapPlayerTimerBindingResult bind_world_map_player_timer_assets(
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& assets,
    uint32_t type_0, uint32_t type_4, const std::optional<uint32_t>& clock,
    WorldMapPlayerTimerBinding* out);
} // namespace awl
