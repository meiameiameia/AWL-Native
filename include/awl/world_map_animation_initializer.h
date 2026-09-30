#pragma once

#include "awl/world_map_animation_channel.h"
#include "awl/world_map_model_links.h"

namespace awl {

struct WorldMapAnimationFeatureTimer {
    uint64_t resource_0 = 0;
    uint32_t clock_4 = 0;
    float value_8 = 0, rate_c = 0;
};
struct WorldMapAnimationFeature38 {
    uint32_t type_0 = 0, type_4 = 0, index_8 = UINT32_MAX;
    uint64_t table_c = 0;
    WorldMapAnimationFeatureTimer first_14, second_24;
};
struct WorldMapAnimationFeatureRow {
    uint64_t table_identity = 0;
    uint32_t index = 0, column = 0;
    uint64_t resource_identity = 0; // Zero is an observed null entry.
};
struct WorldMapAnimationFeature3c {
    uint64_t resource_0 = 0;
    float value_4 = 0, value_8 = 0;
};
struct WorldMapAnimationFeature3cLookup {
    uint32_t index = 0;
    uint64_t resource_identity = 0;
};
struct WorldMapAnimationInitializerObservations {
    std::optional<uint32_t> clock;
    std::optional<uint64_t> fallback_table_38, fallback_table_24;
    std::vector<WorldMapAnimationFeatureRow> rows;
    std::optional<WorldMapAnimationFeature3cLookup> feature_3c;
};
enum class WorldMapAnimationFeatureStatus { Prepared, RequiresTable, RequiresRow, RequiresClock, InvalidInput };
struct WorldMapAnimationFeatureStep {
    WorldMapAnimationFeature38 after;
    std::optional<WorldMapAnimationFeatureRow> required_row;
};
// FUN_8002F494 and FUN_8017E11C using keyed observations of the two
// pointer lookups, not a guessed table parser. Failure/missing evidence is
// atomic; unused rows/tables/clocks are not read. Duplicate reached rows fail.
[[nodiscard]] WorldMapAnimationFeatureStatus prepare_world_map_animation_feature38(
    const WorldMapAnimationFeature38& state, uint32_t index,
    const WorldMapAnimationInitializerObservations& observations, WorldMapAnimationFeatureStep* out);

// Ordered FUN_801A6C4C/6C28/6C70 and FUN_801A6A14 metadata effects.
// Sets target loop low byte, rate, start position (+0 for rate >=0;
// limit minus the verified binary32 epsilon for negative rate), then copies
// and links model playback in branch order. No pose/track evaluation occurs.
// Nonfinite reached arithmetic/copy fields, missing/invalid keys and output aliases
// fail or block atomically. Only whole-record aliases are supported.
[[nodiscard]] WorldMapAnimationChannelStatus prepare_world_map_animation_channel_settings(
    const WorldMapAnimationChannelState& channel,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    uint64_t model_identity, const std::optional<WorldMapAnimationModelBinding>& model,
    uint32_t loop, float rate, WorldMapAnimationChannelStep* out);

struct WorldMapAnimationInitializerState {
    WorldMapActorAnimationState animation;
    WorldMapAnimationChannelState primary;
    std::vector<WorldMapAnimationPlaybackRecord> records;
    bool has_optional_bindings = false;
    std::optional<WorldMapAnimationFeature38> feature_38; // Absent means observed null, when bindings are known.
    std::optional<WorldMapAnimationFeature3c> feature_3c;
    uint64_t secondary_model_c0 = 0;
    std::optional<WorldMapModelLinkState> model_links; // Missing means unknown, not empty.
};
enum class WorldMapAnimationInitializerStatus {
    Unchanged, RequiresDescriptor, RequiresBinding, RequiresModelSetup,
    RequiresOptionalBindings, RequiresFeatureTable, RequiresFeatureRow,
    RequiresFeature3cLookup, RequiresClock, RequiresSecondarySetup,
    RequiresSecondaryRelease, RequiresModelHierarchy, InvalidInput,
    Prepared,
};
struct WorldMapAnimationInitializerStep {
    // Supplied-state proposal or unaccepted ordered prefix, never resumable
    // or a live parent acknowledgement. Reprepare from the original state
    // when evidence arrives; no arbitrary hierarchy success is accepted.
    WorldMapAnimationInitializerState after;
    std::optional<WorldMapActorAnimationStep> start;
    std::optional<WorldMapAnimationChannelStep> setup, settings;
    std::optional<WorldMapAnimationFeatureRow> required_row;
    std::optional<uint32_t> feature_3c_index, secondary_index;
    bool loop = false;
    float rate = 0;
    uint64_t hierarchy_model = 0;
    std::optional<WorldMapModelLinkStep> hierarchy;
};
// FUN_8017D660 -> FUN_8017DB28. Composes the first channel, optional
// feature metadata, observed absent secondary model, clocks/rate/model
// metadata and FUN_8017DF68's primary link clearing from supplied nodes.
// Prepared means this no-secondary initializer proposal is complete, not
// executed as a live actor/presentation effect. Present secondary setup/release
// and unknown/reached missing model links remain explicit stops. No frame
// update. Input and out.after must not alias; InvalidInput preserves output.
[[nodiscard]] WorldMapAnimationInitializerStatus prepare_world_map_animation_initializer(
    const WorldMapAnimationInitializerState& state, uint64_t requested,
    const std::optional<WorldMapActorAnimationDescriptor>& descriptor,
    const std::optional<WorldMapActorAnimationGroup>& group,
    const std::optional<WorldMapAnimationModelBinding>& model, const WorldMapAnimationBank* bank,
    const WorldMapAnimationInitializerObservations& observations, WorldMapAnimationInitializerStep* out);

} // namespace awl
