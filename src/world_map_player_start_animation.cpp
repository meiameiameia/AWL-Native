#include "awl/world_map_player_start_animation.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include <fstream>
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapPlayerStartAnimationStatus;
constexpr size_t max_dol_size = 32 * 1024 * 1024;
uint32_t word(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
struct Section { uint32_t offset = 0, address = 0, size = 0; };
class DolView {
public:
    bool initialize(const uint8_t* data, size_t size) {
        if (!data || size < 0x100 || size > max_dol_size) return false;
        data_ = data;
        for (size_t i = 0; i < sections_.size(); ++i) {
            const Section section{word(data + i * 4),word(data + 0x48 + i * 4),word(data + 0x90 + i * 4)};
            if (section.size == 0) continue;
            if (section.offset < 0x100 || section.offset > size || section.size > size - section.offset ||
                uint64_t(section.address) + section.size > uint64_t(UINT32_MAX) + 1) return false;
            for (size_t j = 0; j < i; ++j) {
                const auto& prior = sections_[j];
                if (prior.size == 0) continue;
                if ((uint64_t(section.address) < uint64_t(prior.address) + prior.size &&
                     uint64_t(prior.address) < uint64_t(section.address) + section.size) ||
                    (uint64_t(section.offset) < uint64_t(prior.offset) + prior.size &&
                     uint64_t(prior.offset) < uint64_t(section.offset) + section.size)) return false;
            }
            sections_[i] = section;
        }
        return true;
    }
    const uint8_t* read(uint32_t address, size_t size) const {
        for (const auto& section : sections_) {
            if (section.size != 0 && address >= section.address &&
                uint64_t(address) - section.address + size <= section.size)
                return data_ + section.offset + (address - section.address);
        }
        return nullptr;
    }
private:
    const uint8_t* data_ = nullptr;
    std::array<Section,18> sections_{};
};
} // namespace

uint32_t classify_world_map_player_start_animation(uint32_t item, uint32_t action, uint8_t type) {
    if (type == 1 || type == 2 || type == 4 || item == 0x4FF) return 3;
    if (item != 0) return 1;
    return action != 0 ? 2 : 0;
}

bool WorldMapPlayerStartAnimationTables::decode(const uint8_t* data, size_t size) {
    DolView view;
    if (!view.initialize(data,size)) return false;
    const auto* jump = view.read(0x8029E4BC,4);
    const auto* row = view.read(0x80249238,8);
    const auto* empty = view.read(0x80282DF8,1);
    if (!jump || word(jump) != 0x800289A8 || !row || !empty) return false;
    std::array<WorldMapPlayerStartAnimationSelection,4> entries;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint16_t index = static_cast<uint16_t>((uint16_t(row[i * 2]) << 8) | row[i * 2 + 1]);
        const auto* slot = view.read(0x8029BECC + uint32_t(index) * 4,4);
        if (!slot) return false;
        const uint32_t address = word(slot);
        if (address == 0 || (address & 3) != 0) return false;
        const auto* descriptor = view.read(address,12);
        if (!descriptor) return false;
        entries[i] = {i,index,{address,word(descriptor),word(descriptor + 4),word(descriptor + 8)}};
    }
    entries_ = entries; empty_item_type_ = *empty;
    return true;
}

Status WorldMapPlayerStartAnimationTables::select(const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type, WorldMapPlayerStartAnimationSelection* out) const {
    if (!out) return Status::InvalidInput;
    if (command.selector != 1) return Status::UnsupportedCommand;
    if (item_type && item_type->item != command.item) return Status::InvalidInput;
    if (command.item != 0 && !item_type) return Status::RequiresItemType;
    if (command.item == 0 && item_type && item_type->type != empty_item_type_) return Status::InvalidInput;
    const uint8_t type = command.item == 0 ? empty_item_type_ : item_type->type;
    *out = entries_[classify_world_map_player_start_animation(command.item,command.action,type)];
    return Status::Selected;
}

Status decode_world_map_player_start_animation_tables(const uint8_t* data, size_t size,
    std::shared_ptr<const WorldMapPlayerStartAnimationTables>* out) {
    if (!data || !out) return Status::InvalidInput;
    try {
        auto next = std::shared_ptr<WorldMapPlayerStartAnimationTables>(new WorldMapPlayerStartAnimationTables);
        if (!next->decode(data,size)) return Status::UnsupportedLayout;
        *out = std::move(next);
        return Status::Decoded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}

Status load_world_map_player_start_animation_tables(std::shared_ptr<const WorldMapPlayerStartAnimationTables>* out) {
    if (!out) return Status::InvalidInput;
    if (*out && (*out)->target_verified()) return Status::Loaded;
    try {
        char path[1024];
        if (!filesystem_resolve_path("/sys/main.dol",path,sizeof(path))) return Status::ReadFailure;
        std::ifstream input(path,std::ios::binary | std::ios::ate);
        if (!input) return Status::ReadFailure;
        const auto size = input.tellg();
        if (size < 0 || size > static_cast<std::streamoff>(max_dol_size)) return Status::ReadFailure;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        input.seekg(0);
        if (!input || (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) ||
            input.peek() != std::char_traits<char>::eof() || input.bad()) return Status::ReadFailure;
        Sha1 sha;
        if (!sha1_bytes(bytes.data(),bytes.size(),&sha)) return Status::ReadFailure;
        if (sha != kTargetDolSha1) return Status::WrongDol;
        auto next = std::shared_ptr<WorldMapPlayerStartAnimationTables>(new WorldMapPlayerStartAnimationTables);
        if (!next->decode(bytes.data(),bytes.size())) return Status::UnsupportedLayout;
        next->target_verified_ = true; *out = std::move(next);
        return Status::Loaded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}

Status prepare_world_map_player_start_animation(
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
    const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type,
    const WorldMapAnimationInitializerState& state,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationInitializerObservations& observations, WorldMapPlayerStartAnimationStep* out) {
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerStartAnimationStep>);
    if (!out || (out->initializer && &state == &out->initializer->after)) return Status::InvalidInput;
    try {
        WorldMapPlayerStartAnimationStep next; next.tables = tables;
        auto finish = [&](Status status) { *out = std::move(next); return status; };
        if (!next.tables) return finish(Status::RequiresTables);
        WorldMapPlayerStartAnimationSelection selection;
        const auto selected = next.tables->select(command,item_type,&selection);
        if (selected == Status::InvalidInput) return selected;
        if (selected != Status::Selected) return finish(selected);
        next.selection = selection;
        const auto& descriptor = selection.descriptor;
        const bool unchanged = state.animation.base_descriptor_4 == descriptor.identity;
        WorldMapPlayerAnimationGroupBinding binding;
        WorldMapAnimationInitializerObservations supplied;
        if (!unchanged) {
            const auto bound = bind_world_map_player_animation_group(descriptor.word_0,assets,&binding);
            if (bound == WorldMapPlayerAnimationAssetsStatus::RequiresAssets) return finish(Status::RequiresAssets);
            if (bound == WorldMapPlayerAnimationAssetsStatus::RequiresGroup) return finish(Status::RequiresGroup);
            if (bound != WorldMapPlayerAnimationAssetsStatus::Bound) return Status::InvalidInput;
            next.binding = binding;
            supplied = observations;
            // This provider owns the selected banks; generic caller bank
            // observations are replaced, not read as reached dependencies.
            supplied.secondary_group = binding.secondary;
            supplied.model_bank = &binding.assets->secondary_models();
        }
        WorldMapAnimationInitializerStep initializer;
        const auto status = prepare_world_map_animation_initializer(state,descriptor.identity,descriptor,
            unchanged ? std::optional<WorldMapActorAnimationGroup>{} : binding.primary, model,
            unchanged ? nullptr : &binding.assets->primary_animations(),supplied,&initializer);
        if (status == WorldMapAnimationInitializerStatus::InvalidInput) return Status::InvalidInput;
        next.initializer_status = status; next.initializer = std::move(initializer);
        if (status == WorldMapAnimationInitializerStatus::Unchanged) return finish(Status::Unchanged);
        return finish(status == WorldMapAnimationInitializerStatus::Prepared ? Status::Prepared : Status::InitializerIncomplete);
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
WorldMapPlayerStartAnimationStatus advance_world_map_player_start_animation(
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
    const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type,
    WorldMapNativeAnimationInitializerMetadata* metadata, WorldMapNativeAnimationChannel* channel,
    const std::vector<WorldMapNativeModel*>& models,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    const WorldMapAnimationInitializerObservations& observations, WorldMapPlayerStartNativeAnimationStep* out) {
    using Status = WorldMapPlayerStartAnimationStatus;
    using Native = WorldMapNativeAnimationInitializerStatus;
    static_assert(std::is_nothrow_move_assignable_v<WorldMapPlayerStartNativeAnimationStep>);
    if (!out || !metadata || metadata == &out->initializer.after) return Status::InvalidInput;
    try {
        WorldMapPlayerStartNativeAnimationStep next; next.tables = tables; next.initializer.after = *metadata;
        if (!tables) return Status::RequiresTables;
        WorldMapPlayerStartAnimationSelection selection;
        const auto selected = tables->select(command, item_type, &selection);
        if (selected != Status::Selected) return selected;
        next.selection = selection;
        const bool unchanged = metadata->animation.base_descriptor_4 == selection.descriptor.identity;
        WorldMapPlayerAnimationGroupBinding binding;
        if (!unchanged) {
            const auto group = selection.descriptor.word_0 >> 25;
            if (group != 0) { *out = std::move(next); return Status::RequiresGroup; }
            const auto status = bind_world_map_player_animation_group(selection.descriptor.word_0, assets, &binding);
            if (status == WorldMapPlayerAnimationAssetsStatus::RequiresAssets) { *out = std::move(next); return Status::RequiresAssets; }
            if (status != WorldMapPlayerAnimationAssetsStatus::Bound) return Status::InvalidInput;
            next.binding = binding;
        }
        const auto status = advance_world_map_native_animation_initializer(metadata, channel, models,
            selection.descriptor.identity, selection.descriptor,
            unchanged ? std::optional<WorldMapActorAnimationGroup>{} : binding.primary,
            unchanged ? nullptr : &binding.assets->primary_animations(), observations, &next.initializer);
        if (status == Native::InvalidInput) return Status::InvalidInput;
        if (status == Native::AllocationFailure) return Status::AllocationFailure;
        *out = std::move(next);
        if (status == Native::Advanced) return Status::Advanced;
        if (status == Native::Unchanged) return Status::Unchanged;
        return Status::InitializerIncomplete;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
