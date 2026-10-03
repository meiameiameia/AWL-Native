#include "awl/world_map_held_item_assets.h"
#include "awl/filesystem.h"
#include "world_map_flat_archive.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapHeldItemAssetsStatus;
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}
bool read_bytes(const char* path, std::vector<uint8_t>* out) {
    void* data = nullptr; size_t size = 0;
    if (!filesystem_read_entire_file(path, &data, &size)) return false;
    const std::unique_ptr<void,void(*)(void*)> guard(data, filesystem_free_file_data);
    if (size > UINT32_MAX) return false;
    const auto* first = static_cast<const uint8_t*>(data);
    out->assign(first, first + size); return true;
}
size_t bank_index(WorldMapHeldItemFeatureBank bank) {
    return bank == WorldMapHeldItemFeatureBank::Primary ? 0u : 1u;
}
} // namespace
WorldMapHeldItemAssetsStatus prepare_world_map_held_item_gpl_metadata(
    const std::vector<uint8_t>& bytes, WorldMapHeldItemGplMetadata* out) {
    if (out == nullptr || bytes.size() > UINT32_MAX || bytes.size() < 20) return Status::InvalidInput;
    const auto word = [&](uint32_t offset) { return detail::archive_be32(bytes.data() + offset); };
    if (word(0) != 0x005bbc61u || word(4) != 0 || word(8) != 0) return Status::UnsupportedLayout;
    const uint32_t count = word(12), table = word(16);
    if (count == 0 || count > 65535 || table < 20 || table % 4 != 0 ||
        !detail::archive_range(bytes.size(), table, uint64_t(count)*8)) return Status::InvalidInput;
    try {
        const uint64_t table_end = uint64_t(table) + uint64_t(count)*8;
        WorldMapHeldItemGplMetadata metadata;
        std::vector<uint32_t> starts;
        uint32_t names = static_cast<uint32_t>(bytes.size());
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t offset = word(table + i*8), name = word(table + i*8 + 4);
            if (offset < table_end || offset % 4 != 0 || name < table_end ||
                !detail::archive_range(bytes.size(), offset, 20) || !detail::archive_range(bytes.size(), name, 1)) return Status::InvalidInput;
            const void* end = std::memchr(bytes.data() + name, 0, bytes.size() - name);
            if (end == nullptr || bytes[name] == 0) return Status::InvalidInput;
            starts.push_back(offset); names = (std::min)(names, name);
            metadata.sections.push_back({offset,name,0,{}});
        }
        std::sort(starts.begin(), starts.end());
        for (size_t i = 0; i < starts.size(); ++i) {
            if (starts[i] >= names || (i != 0 && starts[i] == starts[i-1])) return Status::UnsupportedLayout;
        }
        for (auto& section : metadata.sections) {
            const auto next = std::upper_bound(starts.begin(), starts.end(), section.offset);
            const uint32_t end = next == starts.end() ? names : *next;
            const uint32_t size = end - section.offset;
            if (size < 20) return Status::InvalidInput;
            auto section_word = [&](uint32_t offset) { return word(section.offset + offset); };
            for (uint32_t i = 0; i < 5; ++i) {
                const uint32_t sub = section_word(i*4);
                if (sub != 0 && (sub < 20 || !detail::archive_range(size, sub, 1))) return Status::InvalidInput;
            }
            const uint32_t position = section_word(0);
            if (position != 0) {
                if (!detail::archive_range(size, position, 8)) return Status::InvalidInput;
                if (bytes[section.offset + position + 7] == 6 && section_word(position) != 0) return Status::RequiresSkin;
            }
            const uint32_t material = section_word(16);
            if (material == 0) return Status::UnsupportedLayout;
            if (!detail::archive_range(size, material, 12)) return Status::InvalidInput;
            section.material_offset = section.offset + material;
            const uint32_t commands = section_word(material + 4);
            const uint16_t command_count = be16(bytes.data() + section.material_offset + 8);
            if (command_count != 0 && (commands < 20 || !detail::archive_range(size, commands, uint64_t(command_count)*16))) return Status::InvalidInput;
            if (command_count != 0 && overlaps(commands,uint64_t(command_count)*16,material,12)) return Status::UnsupportedLayout;
            // Original relocation writes the attribute header/array pointers.
            // Aliasing them with command headers could alter cache filtering.
            std::vector<std::pair<uint32_t,uint32_t>> headers{{material,12}};
            const uint32_t arrays = section_word(8);
            if (arrays != 0 && size < 21) return Status::InvalidInput;
            const uint32_t section_header = arrays != 0 ? 21u : 20u;
            if (overlaps(material,12,0,section_header) ||
                (command_count != 0 && overlaps(commands,uint64_t(command_count)*16,0,section_header))) return Status::UnsupportedLayout;
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t sub = section_word(i*4);
                if (sub == 0) continue;
                const uint32_t length = i == 2 ? uint32_t(bytes[section.offset + 20])*16u : 8u;
                if (!detail::archive_range(size, sub, length)) return Status::InvalidInput;
                if (length == 0) continue;
                if (overlaps(sub,length,0,section_header) ||
                    (command_count != 0 && overlaps(sub,length,commands,uint64_t(command_count)*16))) return Status::UnsupportedLayout;
                for (const auto& header : headers) if (overlaps(sub,length,header.first,header.second)) return Status::UnsupportedLayout;
                headers.emplace_back(sub,length);
            }
            for (uint32_t i = 0; i < command_count; ++i) {
                const auto* header = bytes.data() + section.offset + commands + i*16;
                if (header[0] < 1 || header[0] > 3) return Status::UnsupportedLayout;
                section.commands.push_back({header[0],header[1]});
            }
        }
        *out = std::move(metadata); return Status::Prepared;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
const WorldMapHeldItemGplMetadata& WorldMapHeldItemAssets::metadata(WorldMapHeldItemFeatureBank bank) const {
    return banks_[bank_index(bank)].metadata;
}
size_t WorldMapHeldItemAssets::texture_count(WorldMapHeldItemFeatureBank bank) const {
    return banks_[bank_index(bank)].textures.textures.size();
}
const TplTexture* WorldMapHeldItemAssets::texture(WorldMapHeldItemFeatureBank bank, uint16_t index) const {
    if (bank != WorldMapHeldItemFeatureBank::Primary && bank != WorldMapHeldItemFeatureBank::Alternate) return nullptr;
    const auto& textures = banks_[bank_index(bank)].textures.textures;
    return index < textures.size() ? &textures[index] : nullptr;
}
bool WorldMapHeldItemAssets::resolve(const WorldMapHeldItemFeatureRoute& route, WorldMapHeldItemFeatureObservations* out) const {
    if (out == nullptr || (route.bank != WorldMapHeldItemFeatureBank::Primary && route.bank != WorldMapHeldItemFeatureBank::Alternate)) return false;
    const auto& bank = banks_[bank_index(route.bank)];
    if (route.group >= bank.metadata.sections.size()) return false;
    for (const auto index : route.textures) if (index >= bank.textures.textures.size() ||
        bank.textures.textures[index].image_header_offset == 0 || bank.textures.textures[index].header.unpacked != 0) return false;
    const auto& section = bank.metadata.sections[route.group];
    WorldMapHeldItemFeatureObservations pending = *out;
    pending.resource = WorldMapModelFeatureResourceBinding{route.bank,route.group,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bank.bytes.data() + section.offset)),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&section)),section.commands};
    pending.texture_bank_identity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bank.textures.textures.data()));
    *out = std::move(pending); return true;
}
WorldMapHeldItemAssetsStatus load_world_map_held_item_assets(std::shared_ptr<const WorldMapHeldItemAssets>* out) {
    if (out == nullptr) return Status::InvalidInput;
    if (*out) return Status::Loaded;
    constexpr const char* paths[2][2] = {{"/files/symbol.gpl","/files/symbol.tpl"},{"/files/real.gpl","/files/real.tpl"}};
    try {
        auto assets = std::shared_ptr<WorldMapHeldItemAssets>(new WorldMapHeldItemAssets);
        for (size_t i = 0; i < 2; ++i) {
            auto& bank = assets->banks_[i];
            if (!read_bytes(paths[i][0], &bank.bytes)) return Status::ReadFailure;
            const auto status = prepare_world_map_held_item_gpl_metadata(bank.bytes, &bank.metadata);
            if (status != Status::Prepared) return status;
            if (!tpl_load_from_file(paths[i][1], &bank.textures)) return Status::TextureFailure;
        }
        *out = std::move(assets); return Status::Loaded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
WorldMapModelFeatureStatus advance_world_map_native_held_item_feature_from_assets(
    WorldMapNativeModelFeature* feature, int32_t item_id,
    const std::optional<WorldMapHeldItemFeatureRow>& row, std::optional<uint8_t> baseline,
    std::shared_ptr<const WorldMapHeldItemAssets> assets, WorldMapHeldItemFeatureStep* out) {
    using FeatureStatus = WorldMapModelFeatureStatus;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapHeldItemFeatureStep>);
    static_assert(std::is_nothrow_move_assignable_v<WorldMapModelFeatureObjectState>);
    if (feature == nullptr || out == nullptr || &feature->state_ == &out->after) return FeatureStatus::InvalidInput;
    if (item_id == 0) return advance_world_map_native_held_item_feature(feature,0,{},out);
    try {
        WorldMapHeldItemFeatureObservations observations; observations.row = row; observations.null_item_alternate_group = baseline;
        WorldMapHeldItemFeatureStep pending;
        auto status = prepare_world_map_held_item_feature(feature->state_,item_id,observations,&pending);
        if (status == FeatureStatus::InvalidInput) return status;
        if (status != FeatureStatus::RequiresResource || !assets) { *out = std::move(pending); return status; }
        if (!assets->resolve(*pending.route, &observations)) return FeatureStatus::InvalidInput;
        status = prepare_world_map_held_item_feature(feature->state_,item_id,observations,&pending);
        if (status != FeatureStatus::Prepared) return status;
        auto after = pending.after;
        auto providers = feature->asset_providers_;
        if (std::find(providers.begin(),providers.end(),assets) == providers.end()) providers.push_back(std::move(assets));
        *out = std::move(pending); feature->state_ = std::move(after); feature->asset_providers_.swap(providers);
        return FeatureStatus::Advanced;
    } catch (const std::bad_alloc&) { return FeatureStatus::AllocationFailure; }
}
} // namespace awl
