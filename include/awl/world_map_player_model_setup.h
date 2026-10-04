#pragma once

#include "awl/world_map_player_model_auxiliary.h"
#include "awl/world_map_model_initialization.h"

namespace awl {
struct WorldMapPlayerCommandPlan {
    WorldMapModelResourceReference header;
    uint8_t type = 0, channel = 0;
    uint32_t word_4 = 0; // Descriptor/configuration bits, not an asset pointer.
    std::optional<WorldMapModelResourceReference> display_list;
    uint32_t display_list_size = 0;
    uint32_t table_offset = 0, packet_budget = 0;
    // 4078 initializes command record +4/+8/+C/+10 to zero. Later GX
    // compilation is pending; a packet budget is NOT a compiled packet.
    std::optional<WorldMapPlayerModelTextureBinding> texture;
};
struct WorldMapPlayerCommandSectionPlan {
    uint32_t table_offset = 0, commands_offset = 0, packet_budget = 0;
    // Section record +0/+4 are unwritten until 4A2C emits its GX setup.
    std::vector<WorldMapPlayerCommandPlan> commands;
};
struct WorldMapPlayerCommandTextureWrite {
    uint32_t section = 0, command = 0;
    WorldMapPlayerModelTextureBinding texture;
};
struct WorldMapPlayerFeaturePlan {
    std::optional<uint32_t> node; // Null selects the root feature +14.
    uint16_t group = 0;
    uint32_t storage_offset = 0;
    WorldMapModelResourceReference resource;
    // 2708 receives a NONNULL arena: flags +38 = 0, no private cache.
    // 2A4C stores buffer +34 = 0, identity matrix, bytes +3C/+3D = 1/0.
    // DD44 subsequently binds every feature to the SAME final four-slot
    // cache. Only its channel words are initialized to FFFFFFFF.
    uint32_t cache_offset = 0;
    WorldMapModelMatrix matrix{1,0,0,0,0,1,0,0,0,0,1,0};
    uint32_t buffer_34 = 0, flags_38 = 0;
    uint8_t enabled_3c = 1, byte_3d = 0;
};
enum class WorldMapPlayerModelSetupStatus {
    PreparedPlan, RequiresAssets, RequiresResourcePreparation, SingularMatrix,
    UnsupportedLayout, InvalidInput, AllocationFailure,
};
// Owns a checked construction plan, NOT a live/constructed primary model.
// CF3C and D064 sizing, 4078 table initialization/budgets, D7B0/2708
// feature bindings, E7C4 order, DD44 shared cache and ordered 53C0 texture
// selections are supported. Actual GX packets, skin backend/evaluation,
// wrapper publication, holder binding and frame execution remain pending.
class WorldMapPlayerModelSetupPlan {
public:
    WorldMapPlayerModelSetupPlan(const WorldMapPlayerModelSetupPlan&) = delete;
    WorldMapPlayerModelSetupPlan& operator=(const WorldMapPlayerModelSetupPlan&) = delete;
    const WorldMapPlayerModelSelection& selection() const { return selection_; }
    const WorldMapPlayerModelAuxiliaryMetadata& auxiliary() const { return *auxiliary_; }
    const WorldMapModelCore& core_before_features() const { return core_; }
    const std::vector<WorldMapPlayerCommandSectionPlan>& command_sections() const { return commands_; }
    const std::vector<WorldMapPlayerCommandTextureWrite>& texture_writes() const { return texture_writes_; }
    const std::vector<WorldMapPlayerFeaturePlan>& features() const { return features_; }
    const std::vector<uint32_t>& feature_node_order() const { return feature_node_order_; }
    const std::vector<WorldMapModelCoreAllocation>& storage_requests() const { return storage_; }
    // Null when DD44 reached no feature and therefore initialized no cache.
    // Every cache payload +4 remains unknown, separate from command textures.
    const std::optional<std::array<uint32_t,4>>& cache_channels() const { return cache_channels_; }
    uint32_t command_allocation_size() const { return command_allocation_size_; }
    uint32_t command_packets_offset() const { return command_packets_offset_; }
    uint32_t feature_storage_size() const { return feature_storage_size_; }
    uint32_t model_allocation_size() const { return model_allocation_size_; } // CF3C, no skin-buffer addition.
    uint32_t preallocated_size() const { return preallocated_size_; } // D064: distinct alignment + skin size.
private:
    WorldMapPlayerModelSetupPlan() = default;
    friend WorldMapPlayerModelSetupStatus prepare_world_map_player_model_setup(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&, std::unique_ptr<WorldMapPlayerModelSetupPlan>*);
    WorldMapPlayerModelSelection selection_;
    std::unique_ptr<WorldMapPlayerModelAuxiliaryMetadata> auxiliary_;
    WorldMapModelCore core_;
    std::vector<WorldMapPlayerCommandSectionPlan> commands_;
    std::vector<WorldMapPlayerCommandTextureWrite> texture_writes_;
    std::vector<WorldMapPlayerFeaturePlan> features_;
    std::vector<uint32_t> feature_node_order_;
    std::vector<WorldMapModelCoreAllocation> storage_;
    std::optional<std::array<uint32_t,4>> cache_channels_;
    uint32_t command_allocation_size_ = 0, command_packets_offset_ = 0;
    uint32_t feature_storage_size_ = 0, model_allocation_size_ = 0, preallocated_size_ = 0;
};
// Uses actual retained phase-selected providers; no caller-invented bindings.
// Invalid/unsupported/allocation failures preserve the previous output owner.
// All command +8 extents are bounded but their GX topology stays opaque.
// This explicit PreparedPlan result cannot be supplied as a native model.
[[nodiscard]] WorldMapPlayerModelSetupStatus prepare_world_map_player_model_setup(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,
    std::unique_ptr<WorldMapPlayerModelSetupPlan>* out);
} // namespace awl
