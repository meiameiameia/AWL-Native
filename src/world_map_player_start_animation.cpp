#include "awl/world_map_player_start_animation.h"
#include "awl/disc_identity.h"
#include "world_map_dol_view.h"
#include "awl/filesystem.h"
#include <fstream>
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapPlayerStartAnimationStatus;
using detail::max_dol_size;
using detail::DolView;
using detail::dol_word;
} // namespace

uint32_t classify_world_map_player_start_animation(uint32_t item, uint32_t action, uint8_t type) {
    if (type == 1 || type == 2 || type == 4 || item == 0x4FF) return 3;
    if (item != 0) return 1;
    return action != 0 ? 2 : 0;
}

bool WorldMapPlayerStartAnimationTables::decode(const uint8_t* data, size_t size) {
    DolView view;
    if (!view.initialize(data,size)) return false;
    const auto* empty = view.read(0x80282DF8,1);
    if(!empty)return false;
    for(uint32_t selector=0;selector<4;++selector){
        const auto jump_address=0x8029E4B8+selector*4,row_address=0x80249230+selector*8;
        // Selector one remains mandatory. Older synthetic startup-only profiles
        // may omit other entire rows; partially mapped rows/witnesses reject.
        if(selector!=1 && !view.read(jump_address,1) && !view.read(row_address,1))continue;
        const auto* jump=view.read(jump_address,4);const auto* row=view.read(row_address,8);
        if(!jump || dol_word(jump)!=0x800289A8 || !row)return false;
        std::array<WorldMapPlayerStartAnimationSelection,4> entries;
        std::array<std::optional<uint16_t>,4> rates;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint16_t index = static_cast<uint16_t>((uint16_t(row[i * 2]) << 8) | row[i * 2 + 1]);
            const auto* slot = view.read(0x8029BECC + uint32_t(index) * 4,4);
            if (!slot) return false;
            const uint32_t address = dol_word(slot);
            if (address == 0 || (address & 3) != 0) return false;
            const auto* descriptor = view.read(address,12);
            if (!descriptor) return false;
            entries[i] = {i,index,{address,dol_word(descriptor),dol_word(descriptor + 4),dol_word(descriptor + 8)}};
            // Native resource bound, not an inferred original table length.
            // Require an explicit terminal within 256 twelve-byte records.
            for (uint32_t record=0;record<256;++record) {
                const uint64_t key=uint64_t(address)+uint64_t(record)*12;
                if (key>UINT32_MAX) return false;
                const auto* words=view.read(static_cast<uint32_t>(key),12);
                if (!words) return false;
                sequences_[selector][i].push_back({key,dol_word(words),dol_word(words+4),dol_word(words+8)});
                if ((dol_word(words)&1023u)==1023) break;
            }
            if ((sequences_[selector][i].back().word_0&1023u)!=1023) return false;
            const uint32_t code = entries[i].descriptor.word_8;
            // Preserve unsupported negative descriptors for the later status
            // consumer; they cannot authorize a read before a rate table.
            if (code <= INT32_MAX) {
                const uint64_t rate_address = code < 64 ? 0x802929E0ull + uint64_t(code) * 2 :
                    0x80292A14ull + uint64_t(code - 64) * 2;
                if (rate_address > UINT32_MAX) return false;
                const auto* rate = view.read(static_cast<uint32_t>(rate_address),2);
                if (!rate) return false;
                rates[i] = static_cast<uint16_t>((uint16_t(rate[0]) << 8) | rate[1]);
            }
        }
        entries_[selector]=entries;status_rates_[selector]=rates;
    }
    constexpr uint32_t addresses[]{0x80346B00,0x80346B04,0x80346B10,0x80346B14,0x80346B20,
        0x80346B24,0x80346B28,0x80346B2C,0x80346B30,0x80346B34,0x80346B38,0x80346B3C,0x80346B40};
    static_assert(sizeof(addresses) / sizeof(addresses[0]) == static_cast<size_t>(WorldMapPlayerStatusGlobal::Count));
    for (size_t i = 0; i < status_globals_.size(); ++i) {
        const auto* word = view.read(addresses[i],4);
        if (!word) return false;
        status_globals_[i] = dol_word(word);
    }
    constexpr uint32_t condition_addresses[]{0x80346ae0,0x80346ae4,0x80346ae8,0x80346aec,0x80346af0,0x80346afc,0x80346af8};
    for(size_t i=0;i<condition_factors_.globals.size();++i)if(view.read(condition_addresses[i],1)){
        const auto* value=view.read(condition_addresses[i],4);if(!value)return false;condition_factors_.globals[i]=dol_word(value);}
    const auto* reset = view.read(0x8034A360,12);
    if (!reset) return false;
    status_reset_ = {dol_word(reset),dol_word(reset + 4),dol_word(reset + 8)};
    empty_item_type_ = *empty;
    return true;
}

bool WorldMapPlayerStartAnimationTables::sequence_record(uint64_t base,uint64_t current,WorldMapActorAnimationDescriptor* out) const noexcept {
    if (!out) return false;
    for(size_t s=0;s<entries_.size();++s)if(entries_[s])for(size_t i=0;i<4;++i)if((*entries_[s])[i].descriptor.identity==base){
        for (const auto& record:sequences_[s][i]) if (record.identity==current) {*out=record;return true;}
        return false;
    }
    return false;
}
WorldMapPlayerStatusFactors WorldMapPlayerStartAnimationTables::status_factors(
    const WorldMapActorAnimationDescriptor& current) const noexcept {
    WorldMapPlayerStatusFactors result;
    for (size_t i = 0; i < result.globals.size(); ++i) result.globals[i] = status_globals_[i];
    for(size_t s=0;s<entries_.size();++s)if(entries_[s])for(size_t i=0;i<4;++i){
        const auto& known = (*entries_[s])[i].descriptor;
        if (known.identity == current.identity && known.word_0 == current.word_0 &&
            known.word_4 == current.word_4 && known.word_8 == current.word_8 && status_rates_[s][i]) {
            result.rate = WorldMapPlayerStatusRate{current.word_8,*status_rates_[s][i]};
            return result;
        }
    }
    return result;
}

WorldMapPlayerStatusCode WorldMapPlayerStartAnimationTables::prepare_fresh_status(
    std::optional<uint32_t> phase_raw, WorldMapPlayerStatusState* out) const noexcept {
    if (!out) return WorldMapPlayerStatusCode::InvalidInput;
    WorldMapPlayerStatusState next;
    const auto status = construct_world_map_player_status(phase_raw, &next);
    if (status != WorldMapPlayerStatusCode::Prepared) return status;
    return reset_world_map_player_status(next, status_reset_, out);
}

Status WorldMapPlayerStartAnimationTables::select(const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type, WorldMapPlayerStartAnimationSelection* out) const {
    if (!out) return Status::InvalidInput;
    if (command.selector > 3) return Status::UnsupportedCommand;
    if (item_type && item_type->item != command.item) return Status::InvalidInput;
    if (command.item != 0 && !item_type) return Status::RequiresItemType;
    if (command.item == 0 && item_type && item_type->type != empty_item_type_) return Status::InvalidInput;
    const uint8_t type = command.item == 0 ? empty_item_type_ : item_type->type;
    if(!entries_[command.selector])return Status::RequiresSelectorTables;
    *out = (*entries_[command.selector])[classify_world_map_player_start_animation(command.item,command.action,type)];
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
