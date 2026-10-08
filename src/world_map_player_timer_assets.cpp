#include "awl/world_map_player_timer_assets.h"
#include "awl/filesystem.h"
#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapPlayerTimerAssetsStatus;
constexpr size_t max_tam_size = 32 * 1024 * 1024;
uint32_t word(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
bool range(size_t size, uint64_t at, uint64_t length) { return at <= size && length <= size - at; }
bool valid_bank(WorldMapPlayerTimerBank bank) { return bank == WorldMapPlayerTimerBank::Eyes || bank == WorldMapPlayerTimerBank::Mouth; }
bool read(const char* path, std::vector<uint8_t>* bytes) {
    void* data = nullptr; size_t size = 0;
    if (!filesystem_read_entire_file(path, &data, &size)) return false;
    const std::unique_ptr<void,void(*)(void*)> guard(data, filesystem_free_file_data);
    if (size > max_tam_size) return false;
    const auto* first = static_cast<const uint8_t*>(data); bytes->assign(first, first + size); return true;
}
} // namespace
bool WorldMapPlayerTimerAssets::parse(const std::vector<uint8_t>& bytes, Table* out) {
    // The two verified raw files share this marker; already relocated FFFF
    // input is not a file-relative table. No resource body format is inferred.
    if (bytes.size() < 8 || bytes.size() > max_tam_size || word(bytes.data()) != 0xF4E1EEEDu) return false;
    const uint32_t count = word(bytes.data() + 4);
    const uint64_t header_end = 8 + uint64_t(count) * 8;
    if (count == 0 || !range(bytes.size(), 0, header_end)) return false;
    Table next; next.bytes = bytes;
    std::vector<std::pair<uint32_t,uint32_t>> arrays;
    std::vector<uint32_t> offsets;
    uint64_t metadata_end = header_end;
    uint64_t reference_count = 0;
    for (uint32_t row = 0; row < count; ++row) {
        const auto* entry = bytes.data() + 8 + size_t(row) * 8;
        const uint32_t columns = word(entry), array = word(entry + 4);
        const uint64_t length = uint64_t(columns) * 4;
        if (columns == 0 || (array & 3) != 0 || array < header_end || !range(bytes.size(), array, length)) return false;
        arrays.emplace_back(array, static_cast<uint32_t>(uint64_t(array) + length));
        metadata_end = std::max(metadata_end, uint64_t(array) + length);
        next.rows.push_back({static_cast<uint32_t>(reference_count), columns});
        reference_count += columns;
        if (reference_count > bytes.size() / 4) return false;
    }
    std::sort(arrays.begin(), arrays.end());
    for (size_t i = 1; i < arrays.size(); ++i) if (arrays[i].first < arrays[i - 1].second) return false;
    offsets.reserve(static_cast<size_t>(reference_count));
    for (uint32_t row = 0; row < count; ++row) {
        const uint32_t array = word(bytes.data() + 12 + size_t(row) * 8);
        for (uint32_t column = 0; column < next.rows[row].count; ++column) {
            const uint32_t offset = word(bytes.data() + array + size_t(column) * 4);
            if ((offset & 3) != 0 || offset < metadata_end || !range(bytes.size(), offset, 4)) return false;
            offsets.push_back(offset);
        }
    }
    auto physical = offsets;
    std::sort(physical.begin(), physical.end()); physical.erase(std::unique(physical.begin(), physical.end()), physical.end());
    for (uint32_t offset : offsets) {
        const auto after = std::upper_bound(physical.begin(), physical.end(), offset);
        const uint32_t end = after == physical.end() ? static_cast<uint32_t>(bytes.size()) : *after;
        next.resources.push_back({offset, end - offset});
    }
    *out = std::move(next); return true;
}
size_t WorldMapPlayerTimerAssets::row_count(WorldMapPlayerTimerBank bank) const {
    return valid_bank(bank) ? tables_[static_cast<size_t>(bank)].rows.size() : 0;
}
size_t WorldMapPlayerTimerAssets::column_count(WorldMapPlayerTimerBank bank, uint32_t row) const {
    if (!valid_bank(bank)) return 0;
    const auto& rows = tables_[static_cast<size_t>(bank)].rows;
    return row < rows.size() ? rows[row].count : 0;
}
uint64_t WorldMapPlayerTimerAssets::table_identity(WorldMapPlayerTimerBank bank) const {
    return valid_bank(bank) ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tables_[static_cast<size_t>(bank)])) : 0;
}
bool WorldMapPlayerTimerAssets::resource(WorldMapPlayerTimerBank bank, uint32_t row, uint32_t column, WorldMapPlayerTimerResourceView* out) const {
    if (!out || !valid_bank(bank)) return false;
    const auto& table = tables_[static_cast<size_t>(bank)];
    if (row >= table.rows.size() || column >= table.rows[row].count) return false;
    const auto& resource = table.resources[size_t(table.rows[row].first) + column];
    const auto* data = table.bytes.data() + resource.offset;
    *out = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)), resource.offset, resource.size, data}; return true;
}
Status decode_world_map_player_timer_assets(const std::vector<uint8_t>& eyes, const std::vector<uint8_t>& mouth,
    std::shared_ptr<const WorldMapPlayerTimerAssets>* out) {
    if (!out) return Status::InvalidInput;
    try {
        auto next = std::shared_ptr<WorldMapPlayerTimerAssets>(new WorldMapPlayerTimerAssets);
        if (!WorldMapPlayerTimerAssets::parse(eyes, &next->tables_[0]) || !WorldMapPlayerTimerAssets::parse(mouth, &next->tables_[1])) return Status::UnsupportedLayout;
        *out = std::move(next); return Status::Decoded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
Status load_world_map_player_timer_assets(std::shared_ptr<const WorldMapPlayerTimerAssets>* out) {
    if (!out) return Status::InvalidInput;
    if (*out) return Status::Loaded;
    try {
        auto next = std::shared_ptr<WorldMapPlayerTimerAssets>(new WorldMapPlayerTimerAssets);
        std::vector<uint8_t> bytes;
        if (!read("/files/char_com_eye.tam", &bytes)) return Status::ReadFailure;
        if (!WorldMapPlayerTimerAssets::parse(bytes, &next->tables_[0])) return Status::UnsupportedLayout;
        if (!read("/files/char_com_mouth.tam", &bytes)) return Status::ReadFailure;
        if (!WorldMapPlayerTimerAssets::parse(bytes, &next->tables_[1])) return Status::UnsupportedLayout;
        *out = std::move(next); return Status::Loaded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
WorldMapPlayerTimerBindingResult bind_world_map_player_timer_assets(
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& assets, uint32_t type_0, uint32_t type_4,
    const std::optional<uint32_t>& clock, WorldMapPlayerTimerBinding* out) {
    using Bank = WorldMapPlayerTimerBank;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerTimerBinding>);
    if (!out || type_0 < 0x3A || type_0 > INT32_MAX || type_4 < 0x24 || type_4 > INT32_MAX) return {};
    if (!assets) return {Status::RequiresAssets};
    WorldMapPlayerTimerResourceView first, second;
    if (!assets->resource(Bank::Eyes, type_0 - 0x3A, 0, &first)) return {Status::RequiresRow, Bank::Eyes, type_0 - 0x3A};
    if (!clock) return {Status::RequiresClock, Bank::Eyes, type_0 - 0x3A};
    if (!assets->resource(Bank::Mouth, type_4 - 0x24, 0, &second)) return {Status::RequiresRow, Bank::Mouth, type_4 - 0x24};
    try {
        WorldMapPlayerTimerBinding next; next.assets = assets;
        auto& obs = next.observations; obs.clock = clock;
        obs.fallback_table_38 = assets->table_identity(Bank::Eyes); obs.fallback_table_24 = assets->table_identity(Bank::Mouth);
        obs.rows = {{*obs.fallback_table_38, type_0 - 0x3A, 0, first.identity}, {*obs.fallback_table_24, type_4 - 0x24, 0, second.identity}};
        *out = std::move(next); return {Status::Bound};
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
} // namespace awl
