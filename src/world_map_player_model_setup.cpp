#include "awl/world_map_player_model_setup.h"
#include "world_map_flat_archive.h"

#include <algorithm>
#include <new>
#include <utility>

namespace awl {
WorldMapPlayerModelSetupStatus prepare_world_map_player_model_setup(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,
    std::unique_ptr<WorldMapPlayerModelSetupPlan>* out) {
    using Status = WorldMapPlayerModelSetupStatus;
    if (!out) return Status::InvalidInput;
    if (!assets) return Status::RequiresAssets;
    try {
        auto plan = std::unique_ptr<WorldMapPlayerModelSetupPlan>(new WorldMapPlayerModelSetupPlan);
        const auto selected = prepare_world_map_player_model_selection(assets,&plan->selection_);
        if (selected == WorldMapPlayerModelAssetsStatus::AllocationFailure) return Status::AllocationFailure;
        if (selected == WorldMapPlayerModelAssetsStatus::RequiresResourcePreparation) return Status::RequiresResourcePreparation;
        if (selected != WorldMapPlayerModelAssetsStatus::RequiresAuxiliaryConstruction) return Status::UnsupportedLayout;
        const auto auxiliary = prepare_world_map_player_model_auxiliary(plan->selection_.assets,&plan->auxiliary_);
        if (auxiliary == WorldMapPlayerModelAuxiliaryStatus::AllocationFailure) return Status::AllocationFailure;
        if (auxiliary != WorldMapPlayerModelAuxiliaryStatus::Prepared) return Status::UnsupportedLayout;
        WorldMapModelCoreInitialization core;
        const auto initialized = prepare_world_map_model_core(plan->selection_.model,&core);
        if (initialized == WorldMapModelCoreStatus::SingularMatrix) return Status::SingularMatrix;
        if (initialized != WorldMapModelCoreStatus::Prepared || !core.core) return Status::UnsupportedLayout;
        plan->core_ = std::move(*core.core);

        // 4078 first reserves 8 + section_count*16 + command_count*20,
        // then rounds ONCE to 32 before the 4978 per-section packet budgets.
        const auto& gpl = plan->auxiliary_->gpl();
        const auto& view = plan->selection_.gpl;
        uint64_t cursor = 8u + uint64_t(gpl.sections.size())*16u, packets = 0;
        for (size_t i = 0; i < gpl.sections.size(); ++i) {
            const auto& source = gpl.sections[i];
            WorldMapPlayerCommandSectionPlan section;
            section.table_offset = 8u + static_cast<uint32_t>(i)*16u;
            if (cursor > UINT32_MAX) return Status::UnsupportedLayout;
            section.commands_offset = static_cast<uint32_t>(cursor);
            uint32_t end = view.size;
            for (const auto& other : gpl.sections) {
                end = (std::min)(end,other.name_offset);
                if (other.offset > source.offset) end = (std::min)(end,other.offset);
            }
            const uint32_t headers = detail::archive_be32(view.data + source.material_offset + 4);
            section.packet_budget = 0x60; // 4978's initial per-section allowance.
            for (size_t j = 0; j < source.commands.size(); ++j) {
                const uint64_t at = uint64_t(source.offset) + headers + j*16u;
                if (!detail::archive_range(end,at,16) || cursor > UINT32_MAX) return Status::UnsupportedLayout;
                WorldMapPlayerCommandPlan command;
                command.header = {view.reference.bank_identity,view.reference.offset + static_cast<uint32_t>(at)};
                command.type = view.data[at]; command.channel = view.data[at+1];
                command.word_4 = detail::archive_be32(view.data + at + 4);
                const uint32_t display = detail::archive_be32(view.data + at + 8);
                command.display_list_size = detail::archive_be32(view.data + at + 12);
                if (display != 0) {
                    const uint64_t target = uint64_t(source.offset) + display;
                    if (!detail::archive_range(end,target,(std::max)(1u,command.display_list_size))) return Status::UnsupportedLayout;
                    command.display_list = WorldMapModelResourceReference{view.reference.bank_identity,
                        view.reference.offset + static_cast<uint32_t>(target)};
                } else if (command.display_list_size != 0) return Status::UnsupportedLayout;
                command.table_offset = static_cast<uint32_t>(cursor); cursor += 20;
                // 4978: type 1 -> 45, type 2 -> 21, type 3 -> 61,
                // each rounded to 32. Wider command types remain rejected.
                if (command.type < 1 || command.type > 3) return Status::UnsupportedLayout;
                command.packet_budget = command.type == 2 ? 32u : 64u;
                section.packet_budget += command.packet_budget;
                section.commands.push_back(std::move(command));
            }
            packets += section.packet_budget;
            plan->commands_.push_back(std::move(section));
        }
        const uint64_t aligned = (cursor + 31u) & ~uint64_t(31);
        if (aligned + packets > UINT32_MAX) return Status::UnsupportedLayout;
        plan->command_packets_offset_ = static_cast<uint32_t>(aligned);
        plan->command_allocation_size_ = static_cast<uint32_t>(aligned + packets);

        // 53C0/50E8 selects every matching type-one command in serialized
        // section/command order. Byte FF is an ordinary channel; only full
        // FFFFFFFF is 50E8's wildcard, and D41C does not pass that wildcard.
        for (const auto& texture : plan->selection_.textures) {
            for (size_t i = 0; i < plan->commands_.size(); ++i) {
                auto& section = plan->commands_[i];
                for (size_t j = 0; j < section.commands.size(); ++j) {
                    auto& command = section.commands[j];
                    if (command.type != 1 || command.channel != texture.channel) continue;
                    command.texture = texture;
                    plan->texture_writes_.push_back({static_cast<uint32_t>(i),static_cast<uint32_t>(j),texture});
                }
            }
        }

        plan->storage_ = plan->core_.allocations;
        uint64_t model_cursor = plan->core_.consumed_size;
        auto feature = [&](std::optional<uint32_t> node,uint16_t group) {
            if (group >= gpl.sections.size() || model_cursor + 0x50u > UINT32_MAX) return false;
            const auto& section = gpl.sections[group];
            plan->features_.push_back({node,group,static_cast<uint32_t>(model_cursor),
                {view.reference.bank_identity,view.reference.offset + section.offset},0});
            plan->storage_.push_back({static_cast<uint32_t>(model_cursor),0x50});
            model_cursor += 0x50; return true;
        };
        // D748/D878 sizing and D7B0/DAB0 construction walk the same already
        // checked child-before-sibling hierarchy. FFFF omits the feature.
        if (plan->core_.resource.field_14 != 0xffff && !feature(std::nullopt,plan->core_.resource.field_14)) return Status::UnsupportedLayout;
        for (uint32_t i = 0; i < plan->core_.nodes.size(); ++i) {
            const auto& node = plan->core_.nodes[i];
            if (node.value_4 == 0xffff) continue;
            if (!feature(i,static_cast<uint16_t>(node.value_4))) return Status::UnsupportedLayout;
            plan->feature_node_order_.push_back(i);
        }
        // E7C4 stable ascending byte +1 order; equal priorities retain the
        // original node order. Non-feature nodes keep unknown +14 links.
        std::stable_sort(plan->feature_node_order_.begin(),plan->feature_node_order_.end(),[&](uint32_t a,uint32_t b) {
            return plan->core_.nodes[a].order_1 < plan->core_.nodes[b].order_1;
        });
        plan->feature_storage_size_ = static_cast<uint32_t>(model_cursor - plan->core_.consumed_size);
        const uint64_t model_size = uint64_t(plan->core_.resource.core_storage_size) + plan->feature_storage_size_;
        const uint64_t preallocated = ((model_size + 63u) & ~uint64_t(31)) + plan->auxiliary_->skin_buffer_size();
        if (model_cursor + 32u > UINT32_MAX || model_size + 32u > UINT32_MAX || preallocated > UINT32_MAX) return Status::UnsupportedLayout;
        plan->storage_.push_back({static_cast<uint32_t>(model_cursor),32});
        // DD44/36A4 uses one shared final cache, not four slots per feature.
        for (auto& item : plan->features_) item.cache_offset = static_cast<uint32_t>(model_cursor);
        if (!plan->features_.empty()) plan->cache_channels_ = std::array<uint32_t,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
        plan->model_allocation_size_ = static_cast<uint32_t>(model_size + 32u);
        plan->preallocated_size_ = static_cast<uint32_t>(preallocated);
        *out = std::move(plan); return Status::PreparedPlan;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
