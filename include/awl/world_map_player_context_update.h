#pragma once
#include "awl/world_map_player_context_labels.h"

namespace awl {
// Projected fields written by 43368..43774, over explicitly supplied prior
// state. Defaults do not establish the full player/secondary/UI constructor.
struct WorldMapPlayerContextUpdateState {
    WorldMapPlayerContextIconState ui;
    std::array<uint32_t,4> labels_128{}; // primary,zero,tertiary,secondary
    uint8_t byte_1350=0,secondary_byte_574=0;
    uint32_t secondary_word_564=0;
    std::array<uint32_t,3> target_1388{}; // owner code, two virtual payload words
};
struct WorldMapPlayerContextUpdateInput {
    int32_t scene_type=0,current_key=0,held_item=0,action=0;
    WorldMapPlayerContextVector position{},facing{};
    std::optional<WorldMapPlayerContextItemRow> held_row;
    std::optional<uint32_t> tracked_14ac;
    // Resolved D4368 then D4F44 words, reached only for nonzero +14AC.
    std::array<std::optional<uint32_t>,2> tracked_comparison;
    std::optional<uint8_t> scene_predicate_1728e4,secondary_byte_3f1;
    // Only reached saved-record/availability/terrain observations are used.
    // This facade derives source/value/scene/pose/query mode and target fields.
    WorldMapPlayerContextLabelInput label_observations;
};
struct WorldMapPlayerContextUpdateSources {
    const WorldMapPlayerContextTables* queries=nullptr;
    const WorldMapPlayerContextLabelTables* labels=nullptr;
    const WorldMapSceneBucketRegistry* registry=nullptr;
    const WorldMapPlayerContextIconSources* icons=nullptr;
};
enum class WorldMapPlayerContextUpdateStatus {
    Prepared,RequiresHeldRow,RequiresHeldType,RequiresSearchProfile,QueryIncomplete,
    RequiresTrackedTarget,RequiresTrackedComparison,RequiresTransition,LabelsIncomplete,
    RequiresScenePredicate,RequiresSecondaryByte,IconsIncomplete,RequiresContextAction,InvalidInput
};
struct WorldMapPlayerContextUpdateStep {
    WorldMapPlayerContextUpdateStatus status=WorldMapPlayerContextUpdateStatus::InvalidInput;
    uint32_t query_count=0;
    std::array<uint32_t,2> query_masks{};
    std::optional<WorldMapPlayerContextStep> query;
    std::optional<WorldMapPlayerContextResult> target;
    std::optional<WorldMapPlayerContextLabelStep> labels;
    std::optional<WorldMapPlayerContextIconStep> icons;
    std::optional<uint32_t> requested_state,requested_action_kind;
    std::optional<uint32_t> transition_context_138c;
};
// 1CF3C's complete inclusive rectangle predicate; no terrain sampling.
// The verified profile owns its four constants; scene != 1 skips coordinates.
// Original !LT branches also pass unordered coordinates; Y is never read.
[[nodiscard]] bool world_map_player_context_region(const WorldMapPlayerContextSearchProfile&,
    int32_t scene_type,const WorldMapPlayerContextVector&) noexcept;
// Common context path 43368..43774, stopping before button dispatch. Includes
// ordered scene-registry searches, item/action descriptors, label overrides,
// icon setup and player/secondary writes. Prior entry guards and scene-one
// action prefix are separate prerequisites. Reached state-99 transition or
// kind-2D action creation stops explicitly; neither is fabricated/consumed.
// Every stop preserves *out. Diagnostics describe staging only. No allocation,
// clock advance, registry writes, GPU work or full third-callback receipt.
[[nodiscard]] WorldMapPlayerContextUpdateStep prepare_world_map_player_context_update(
    const WorldMapPlayerContextUpdateSources&,const WorldMapPlayerContextUpdateState&,
    const WorldMapPlayerContextUpdateInput&,WorldMapPlayerContextUpdateState* out) noexcept;
} // namespace awl
