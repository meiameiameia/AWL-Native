#include "awl/world_map_held_item_assets.h"
#include "awl/filesystem.h"
#include "world_map_flat_archive.h"

#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapHeldItemAssetsStatus;
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
    switch (prepare_world_map_gpl_metadata(bytes, WorldMapGplSkinPolicy::Reject, out)) {
    case WorldMapGplMetadataStatus::Prepared: return Status::Prepared;
    case WorldMapGplMetadataStatus::RequiresSkin: return Status::RequiresSkin;
    case WorldMapGplMetadataStatus::UnsupportedLayout: return Status::UnsupportedLayout;
    case WorldMapGplMetadataStatus::InvalidInput: return Status::InvalidInput;
    case WorldMapGplMetadataStatus::AllocationFailure: return Status::AllocationFailure;
    default: return Status::InvalidInput;
    }
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
