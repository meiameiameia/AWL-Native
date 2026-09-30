#include "awl/world_map_animation_initializer.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace awl {
namespace {
bool signed_at_least(uint32_t word, uint32_t minimum) { return word <= INT32_MAX && word >= minimum; }
float reset_epsilon() {
    const uint32_t word = 0x38d1b717u; // Mapped 8034BD58, not 1.0.
    float value; std::memcpy(&value, &word, 4); return value;
}
} // namespace

WorldMapAnimationFeatureStatus prepare_world_map_animation_feature38(
    const WorldMapAnimationFeature38& state, uint32_t index,
    const WorldMapAnimationInitializerObservations& observations, WorldMapAnimationFeatureStep* out) {
    using Status = WorldMapAnimationFeatureStatus;
    if (out == nullptr || &state == &out->after) return Status::InvalidInput;
    WorldMapAnimationFeatureStep step; step.after = state; step.after.index_8 = index;
    auto stop = [&](Status status) { step.after = state; *out = step; return status; };
    auto set = [&](WorldMapAnimationFeatureTimer& timer, const std::optional<uint64_t>& table,
                   uint32_t row_index, uint32_t column) {
        if (!table) return Status::RequiresTable;
        if (*table == 0) return Status::InvalidInput;
        step.required_row = WorldMapAnimationFeatureRow{*table, row_index, column, 0};
        const WorldMapAnimationFeatureRow* match = nullptr;
        for (const auto& row : observations.rows) {
            if (row.table_identity == *table && row.index == row_index && row.column == column) {
                if (match != nullptr) return Status::InvalidInput;
                match = &row;
            }
        }
        if (match == nullptr) return Status::RequiresRow;
        if (!observations.clock) return Status::RequiresClock;
        timer = {match->resource_identity, *observations.clock, 0.0f, 1.0f};
        return Status::Prepared;
    };
    Status status = Status::Prepared;
    if (index <= INT32_MAX && state.table_c != 0) {
        status = set(step.after.first_14, state.table_c, index, 0);
        if (status == Status::Prepared) status = set(step.after.second_24, state.table_c, index, 1);
    } else if (state.index_8 != UINT32_MAX) {
        if (signed_at_least(state.type_0, 0x3a)) {
            status = set(step.after.first_14, observations.fallback_table_38, state.type_0 - 0x3a, 0);
        }
        if (status == Status::Prepared && signed_at_least(state.type_4, 0x24)) {
            status = set(step.after.second_24, observations.fallback_table_24, state.type_4 - 0x24, 0);
        }
    }
    if (status == Status::InvalidInput) return status;
    if (status != Status::Prepared) return stop(status);
    step.required_row.reset(); *out = step;
    return Status::Prepared;
}

WorldMapAnimationChannelStatus prepare_world_map_animation_channel_settings(
    const WorldMapAnimationChannelState& channel, const std::vector<WorldMapAnimationPlaybackRecord>& records,
    uint64_t model_identity, const std::optional<WorldMapAnimationModelBinding>& model,
    uint32_t loop, float rate, WorldMapAnimationChannelStep* out) {
    using Status = WorldMapAnimationChannelStatus;
    if (out == nullptr || &channel == &out->after || &records == &out->records_after || !std::isfinite(rate)) return Status::InvalidInput;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].identity == 0) return Status::InvalidInput;
        for (size_t j = 0; j < i; ++j) if (records[i].identity == records[j].identity) return Status::InvalidInput;
    }
    WorldMapAnimationChannelStep step; step.after = channel; step.records_after = records;
    auto stop = [&](Status status, uint64_t required = 0) {
        step.records_after = records; step.required_record = required; *out = step; return status;
    };
    auto find = [&](uint64_t identity) -> WorldMapAnimationPlayback* {
        for (auto& record : step.records_after) if (record.identity == identity) return &record.state;
        return nullptr;
    };
    if (channel.target_8 == 0) return Status::InvalidInput;
    auto* target = find(channel.target_8);
    if (!target) return stop(Status::RequiresPlaybackRecord, channel.target_8);
    target->word_8 = loop & 0xffu; target->rate_4 = rate;
    if (rate >= 0) target->position_0 = 0.0f;
    else {
        if (!std::isfinite(target->limit_c)) return Status::InvalidInput;
        target->position_0 = target->limit_c - reset_epsilon();
        if (!std::isfinite(target->position_0)) return Status::InvalidInput;
    }
    // Unsupported signed modes with an incomplete clock perform no model call.
    if (channel.elapsed_0 < channel.duration_4 && channel.mode_18 > 2) { *out = step; return Status::Prepared; }
    if (!model) return stop(Status::RequiresModelBinding);
    if (model_identity == 0 || model->model_identity != model_identity || model->playback_178 == 0) return Status::InvalidInput;
    auto copy = [&](uint64_t source, uint64_t destination) {
        if (source == 0 || destination == 0) return Status::InvalidInput;
        const auto* from = find(source);
        if (!from) { step.required_record = source; return Status::RequiresPlaybackRecord; }
        auto* to = find(destination);
        if (!to) { step.required_record = destination; return Status::RequiresPlaybackRecord; }
        if (!std::isfinite(from->position_0) || !std::isfinite(from->rate_4) || !std::isfinite(from->limit_c)) return Status::InvalidInput;
        *to = *from; to->link_14 = 0; to->value_18 = 0.0f;
        return Status::Prepared;
    };
    Status status;
    if (channel.elapsed_0 >= channel.duration_4 || channel.mode_18 == 0) {
        step.branch = channel.elapsed_0 >= channel.duration_4 ? WorldMapAnimationChannelBranch::Completed : WorldMapAnimationChannelBranch::NoClip;
        status = copy(channel.target_8, model->playback_178);
        // Mode-zero FUN_801A02FC sees the link just cleared by this copy.
    } else {
        const float ratio = static_cast<float>(channel.elapsed_0) / static_cast<float>(channel.duration_4);
        if (channel.mode_18 == 1) {
            step.branch = WorldMapAnimationChannelBranch::First;
            status = copy(channel.previous_c, model->playback_178);
            if (status == Status::Prepared) {
                auto* playback = find(model->playback_178);
                playback->link_14 = channel.target_8; playback->value_18 = ratio;
            }
        } else {
            step.branch = WorldMapAnimationChannelBranch::Interrupted;
            if (!std::isfinite(channel.blend_14)) return Status::InvalidInput;
            status = copy(channel.older_10, model->playback_178);
            if (status == Status::Prepared) {
                auto* playback = find(model->playback_178);
                playback->link_14 = channel.previous_c; playback->value_18 = channel.blend_14;
                auto* previous = find(channel.previous_c);
                if (channel.previous_c == 0) return Status::InvalidInput;
                if (!previous) return stop(Status::RequiresPlaybackRecord, channel.previous_c);
                previous->link_14 = channel.target_8; previous->value_18 = ratio;
            }
        }
    }
    if (status == Status::InvalidInput) return status;
    if (status != Status::Prepared) return stop(status, step.required_record);
    *out = std::move(step); return Status::Prepared;
}

WorldMapAnimationInitializerStatus prepare_world_map_animation_initializer(
    const WorldMapAnimationInitializerState& state, uint64_t requested,
    const std::optional<WorldMapActorAnimationDescriptor>& descriptor,
    const std::optional<WorldMapActorAnimationGroup>& group,
    const std::optional<WorldMapAnimationModelBinding>& model, const WorldMapAnimationBank* bank,
    const WorldMapAnimationInitializerObservations& observations, WorldMapAnimationInitializerStep* out) {
    using Status = WorldMapAnimationInitializerStatus;
    if (out == nullptr || &state == &out->after) return Status::InvalidInput;
    WorldMapAnimationInitializerStep step; step.after = state;
    auto finish = [&](Status status) { *out = std::move(step); return status; };
    WorldMapActorAnimationStep start;
    const auto begin = prepare_world_map_actor_animation_start(state.animation, requested, descriptor, group, &start);
    if (begin == WorldMapActorAnimationStatus::InvalidInput) return Status::InvalidInput;
    step.start = start; step.after.animation = start.after;
    if (begin == WorldMapActorAnimationStatus::Unchanged) return finish(Status::Unchanged);
    if (begin == WorldMapActorAnimationStatus::RequiresDescriptor) return finish(Status::RequiresDescriptor);
    if (begin == WorldMapActorAnimationStatus::RequiresBinding) return finish(Status::RequiresBinding);
    WorldMapAnimationChannelStep setup;
    const auto channel_status = prepare_world_map_animation_channel(state.primary, state.records, *start.primary_setup, model, bank, &setup);
    if (channel_status == WorldMapAnimationChannelStatus::InvalidInput) return Status::InvalidInput;
    step.setup = setup;
    if (channel_status != WorldMapAnimationChannelStatus::Prepared) return finish(Status::RequiresModelSetup);
    step.after.primary = setup.after; step.after.records = setup.records_after;
    if (!state.has_optional_bindings) return finish(Status::RequiresOptionalBindings);
    const auto& fields = *start.fields;
    if (state.feature_38) {
        WorldMapAnimationFeatureStep feature;
        const auto status = prepare_world_map_animation_feature38(*state.feature_38, fields.feature_38_index.value_or(UINT32_MAX), observations, &feature);
        if (status == WorldMapAnimationFeatureStatus::InvalidInput) return Status::InvalidInput;
        step.required_row = feature.required_row;
        if (status == WorldMapAnimationFeatureStatus::RequiresTable) return finish(Status::RequiresFeatureTable);
        if (status == WorldMapAnimationFeatureStatus::RequiresRow) return finish(Status::RequiresFeatureRow);
        if (status == WorldMapAnimationFeatureStatus::RequiresClock) return finish(Status::RequiresClock);
        step.after.feature_38 = feature.after;
    }
    if (state.feature_3c) {
        uint64_t resource = 0;
        // FUN_8014C104 special index 0x26 returns null without table access.
        if (fields.feature_3c_index && *fields.feature_3c_index != 0x26) {
            step.feature_3c_index = fields.feature_3c_index;
            if (!observations.feature_3c) return finish(Status::RequiresFeature3cLookup);
            if (observations.feature_3c->index != *fields.feature_3c_index) return Status::InvalidInput;
            resource = observations.feature_3c->resource_identity;
        }
        if (resource != state.feature_3c->resource_0) step.after.feature_3c = WorldMapAnimationFeature3c{resource, 0, 0};
    }
    if (fields.secondary_index) { step.secondary_index = fields.secondary_index; return finish(Status::RequiresSecondarySetup); }
    if (state.secondary_model_c0 != 0) return finish(Status::RequiresSecondaryRelease);
    auto& animation = step.after.animation;
    switch (fields.mode_10) {
    case 0: step.loop = false; break;
    case 1: step.loop = true; break;
    case 2:
        if (!observations.clock) return finish(Status::RequiresClock);
        animation.deadline_10 = *observations.clock + (fields.value_14 == 0 ? animation.default_duration_c : fields.value_14 * 10u);
        step.loop = true; break;
    case 3:
        animation.completed_count_1c = 0;
        animation.count_18 = fields.value_14 == 0 ? animation.default_count_14 : fields.value_14;
        step.loop = animation.count_18 > 1; break;
    }
    step.rate = fields.flag_24 ? -1.0f : 1.0f;
    if (fields.flag_12) {
        if (!std::isfinite(animation.speed_8)) return Status::InvalidInput;
        step.rate *= animation.speed_8;
    }
    WorldMapAnimationChannelStep settings;
    const auto status = prepare_world_map_animation_channel_settings(step.after.primary, step.after.records,
        animation.model_identity_30, model, step.loop ? 1u : 0u, step.rate, &settings);
    if (status == WorldMapAnimationChannelStatus::InvalidInput) return Status::InvalidInput;
    step.settings = settings;
    if (status != WorldMapAnimationChannelStatus::Prepared) return finish(Status::RequiresModelSetup);
    step.after.records = settings.records_after;
    animation.flag_21 = fields.flag_13 ? 1 : 0;
    step.hierarchy_model = animation.model_identity_30;
    return finish(Status::RequiresModelHierarchy);
}
} // namespace awl
