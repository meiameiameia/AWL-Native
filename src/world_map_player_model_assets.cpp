#include "awl/world_map_player_model_assets.h"
#include "awl/filesystem.h"
#include "world_map_flat_archive.h"

#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapPlayerModelAssetsStatus;
template<class T> uint64_t identity(const T& object) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&object));
}
std::optional<size_t> bank_index(WorldMapPlayerTextureBank bank) {
    switch (bank) {
    case WorldMapPlayerTextureBank::Body: return 0;
    case WorldMapPlayerTextureBank::Eyes: return 1;
    case WorldMapPlayerTextureBank::Mouth: return 2;
    default: return std::nullopt;
    }
}
} // namespace

bool WorldMapPlayerModelAssets::resource(uint32_t node, WorldMapPlayerModelAssetView* out) const {
    if (!out || node == 0 || node > entries_.size()) return false;
    const auto& entry = entries_[node - 1];
    *out = {{identity(bytes_),entry.offset},entry.size,bytes_.data() + entry.offset};
    return true;
}
size_t WorldMapPlayerModelAssets::texture_count(WorldMapPlayerTextureBank bank) const {
    const auto index = bank_index(bank);
    return index ? textures_[*index].textures.size() : 0;
}
const TplTexture* WorldMapPlayerModelAssets::texture(WorldMapPlayerTextureBank bank, uint16_t entry) const {
    const auto index = bank_index(bank);
    if (!index || entry >= textures_[*index].textures.size()) return nullptr;
    return &textures_[*index].textures[entry];
}
uint64_t WorldMapPlayerModelAssets::texture_bank_identity(WorldMapPlayerTextureBank bank) const {
    const auto index = bank_index(bank);
    return index ? identity(textures_[*index]) : 0;
}

Status load_world_map_player_model_assets(uint32_t phase, std::shared_ptr<const WorldMapPlayerModelAssets>* out) {
    if (!out) return Status::InvalidInput;
    if (phase >= 6) return Status::RequiresPhase;
    constexpr uint32_t variants[] = {0,0,0,1,1,2};
    constexpr const char* paths[] = {"/files/boy_0.arc","/files/boy_1.arc","/files/boy_2.arc"};
    const uint32_t variant = variants[phase];
    if (*out && (*out)->variant() == variant) return Status::Loaded;
    try {
        auto assets = std::shared_ptr<WorldMapPlayerModelAssets>(new WorldMapPlayerModelAssets);
        assets->variant_ = variant;
        void* data = nullptr; size_t size = 0;
        if (!filesystem_read_entire_file(paths[variant], &data, &size)) return Status::ReadFailure;
        const std::unique_ptr<void,void(*)(void*)> guard(data, filesystem_free_file_data);
        if (size == 0 || size > UINT32_MAX) return Status::UnsupportedLayout;
        const auto* first = static_cast<const uint8_t*>(data);
        assets->bytes_.assign(first, first + size);
        std::vector<detail::FlatArchiveEntry> entries;
        if (!detail::parse_flat_archive(assets->bytes_, &entries) || entries.size() < 6)
            return Status::UnsupportedLayout;
        for (const auto& entry : entries) assets->entries_.push_back({entry.offset,entry.size});
        // Retain heterogeneous node order for the existing full-index ACT API.
        if (!assets->models_.parse(identity(assets->models_), assets->bytes_)) return Status::UnsupportedLayout;
        WorldMapModelResource model;
        if (!assets->models_.resolve(1, &model)) return Status::UnsupportedLayout;
        // SKN 3 and GPL 2 are owned extents; their reached wrapper/construction
        // paths must be translated before the primary model can be published.
        for (size_t i = 0; i < assets->textures_.size(); ++i) {
            const auto& entry = entries[3 + i];
            if (!tpl_load_from_memory(first + entry.offset, entry.size, &assets->textures_[i]))
                return Status::TextureFailure;
        }
        *out = std::move(assets);
        return Status::Loaded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}

Status prepare_world_map_player_model_selection(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets, WorldMapPlayerModelSelection* out) {
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerModelSelection>);
    if (!out) return Status::InvalidInput;
    if (!assets) return Status::RequiresAssets;
    try {
        WorldMapPlayerModelSelection selection;
        selection.assets = assets;
        WorldMapModelPreparationStep preparation;
        const auto status = assets->models().prepare(1, &preparation);
        if (status == WorldMapModelPreparationStatus::RequiresEulerRotation) return Status::RequiresResourcePreparation;
        if (status != WorldMapModelPreparationStatus::Prepared || !preparation.prepared) return Status::UnsupportedLayout;
        selection.model = std::move(*preparation.prepared);
        if (!assets->resource(3, &selection.skin) || !assets->resource(2, &selection.gpl)) return Status::UnsupportedLayout;
        if (assets->file_count() >= 7) {
            WorldMapPlayerModelAssetView animation;
            if (!assets->resource(7, &animation)) return Status::UnsupportedLayout;
            selection.texture_animation = animation;
        }
        selection.textures = {{{0xff,WorldMapPlayerTextureBank::Body,0,0},
            {0,WorldMapPlayerTextureBank::Eyes,0,0},{1,WorldMapPlayerTextureBank::Mouth,0,0},
            {2,WorldMapPlayerTextureBank::Body,1,0}}};
        for (auto& binding : selection.textures) {
            const auto* texture = assets->texture(binding.bank, binding.index);
            if (!texture || !texture->raw_data) return Status::TextureFailure;
            binding.bank_identity = assets->texture_bank_identity(binding.bank);
        }
        *out = std::move(selection);
        return Status::RequiresAuxiliaryConstruction;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
