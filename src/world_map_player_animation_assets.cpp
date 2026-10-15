#include "awl/world_map_player_animation_assets.h"
#include "awl/filesystem.h"

#include <new>
#include <fstream>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
bool read_bank(const char* path, std::vector<uint8_t>* out) {
    void* data = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file(path, &data, &size)) return false;
    const std::unique_ptr<void,void(*)(void*)> guard(data, filesystem_free_file_data);
    if (size > UINT32_MAX) return false;
    const auto* first = static_cast<const uint8_t*>(data);
    out->assign(first, first + size);
    return true;
}
template<class Bank> uint64_t identity(const Bank& bank) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&bank));
}
} // namespace

WorldMapPlayerAnimationAssetsStatus load_world_map_player_animation_assets(
    std::shared_ptr<const WorldMapPlayerAnimationAssets>* out) {
    using Status = WorldMapPlayerAnimationAssetsStatus;
    if (out == nullptr) return Status::InvalidInput;
    if (*out && (*out)->group_index()==0) return Status::Loaded;
    try {
        auto assets = std::shared_ptr<WorldMapPlayerAnimationAssets>(new WorldMapPlayerAnimationAssets);
        std::vector<uint8_t> bytes;
        if (!read_bank("/files/boy_0.anm.arc", &bytes)) return Status::ReadFailure;
        if (!assets->primary_.parse(identity(assets->primary_), std::move(bytes)) ||
            !assets->primary_.is_archive()) return Status::UnsupportedLayout;
        if (!read_bank("/files/boy_0_subanm.arc", &bytes)) return Status::ReadFailure;
        if (!assets->secondary_.parse(identity(assets->secondary_), std::move(bytes)) ||
            !assets->secondary_.is_archive()) return Status::UnsupportedLayout;
        if (!read_bank("/files/boy_0_subact.arc", &bytes)) return Status::ReadFailure;
        if (!assets->models_.parse(identity(assets->models_), std::move(bytes)))
            return Status::UnsupportedLayout;
        *out = std::move(assets);
        return Status::Loaded;
    } catch (const std::bad_alloc&) {
        return Status::AllocationFailure;
    }
}

WorldMapPlayerAnimationAssetsStatus load_world_map_optional_animation_assets(
    uint32_t group,std::shared_ptr<const WorldMapPlayerAnimationAssets>* out) {
    using S=WorldMapPlayerAnimationAssetsStatus;
    if(!out)return S::InvalidInput;
    if(group!=65 && group!=66)return S::RequiresGroup;
    if(*out && (*out)->group_index()==group)return S::Loaded;
    try {
        auto assets=std::shared_ptr<WorldMapPlayerAnimationAssets>(new WorldMapPlayerAnimationAssets);assets->group_=group;
        std::vector<uint8_t> bytes;
        if(!read_bank(group==65?"/files/dog1.anm.arc":"/files/cat.anm.arc",&bytes))return S::ReadFailure;
        if(!assets->primary_.parse(identity(assets->primary_),std::move(bytes)) || !assets->primary_.is_archive())return S::UnsupportedLayout;
        // 28104's +4/+8 file-size gates skip the verified empty secondary
        // files for groups 65/66. Reached descriptors use index 127 (none).
        // Keep both providers absent; a future secondary request must stop.
        for(const char* path:group==65?std::array<const char*,2>{"/files/dog1_subanm.arc","/files/dog1_subact.arc"}:
            std::array<const char*,2>{"/files/cat_subanm.arc","/files/cat_subact.arc"}){
            char native[1024];if(!filesystem_resolve_path(path,native,sizeof(native)))return S::ReadFailure;
            std::ifstream input(native,std::ios::binary|std::ios::ate);if(!input || input.tellg()<0)return S::ReadFailure;
            if(input.tellg()!=0)return S::UnsupportedLayout;
        }
        *out=std::move(assets);return S::Loaded;
    }catch(const std::bad_alloc&){return S::AllocationFailure;}
}

WorldMapPlayerAnimationAssetsStatus bind_world_map_player_animation_group(
    uint32_t descriptor_word,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    WorldMapPlayerAnimationGroupBinding* out) {
    using Status = WorldMapPlayerAnimationAssetsStatus;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerAnimationGroupBinding>);
    if (out == nullptr) return Status::InvalidInput;
    const uint32_t group = descriptor_word >> 25;
    if(group!=0 && group!=65 && group!=66)return Status::RequiresGroup;
    if (!assets) return Status::RequiresAssets;
    if (group != assets->group_index()) return Status::RequiresGroup;
    WorldMapPlayerAnimationGroupBinding binding;
    binding.assets = assets;
    binding.primary = {group, assets->primary_animations().identity()};
    binding.secondary = {group, assets->secondary_models().identity()};
    binding.secondary_animation_bank_identity = assets->secondary_animations().identity();
    *out = std::move(binding);
    return Status::Bound;
}
} // namespace awl
