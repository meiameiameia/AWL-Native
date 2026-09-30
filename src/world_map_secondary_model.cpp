#include "awl/world_map_secondary_model.h"
#include "world_map_flat_archive.h"

#include <cmath>
#include <utility>

namespace awl {
namespace {
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
} // namespace
void WorldMapModelBank::clear() { identity_ = 0; entries_.clear(); bytes_.clear(); }
bool WorldMapModelBank::parse(uint64_t identity, std::vector<uint8_t> bytes) {
    clear();
    if (identity == 0) return false;
    std::vector<detail::FlatArchiveEntry> files;
    if (!detail::parse_flat_archive(bytes, &files)) return false;
    for (const auto& file : files) entries_.push_back({file.offset, file.size});
    bytes_ = std::move(bytes); identity_ = identity; return true;
}
bool WorldMapModelBank::resolve(uint32_t index, WorldMapModelResource* out) const {
    if (out == nullptr || !loaded() || index == 0 || index > INT32_MAX || index > entries_.size()) return false;
    const auto entry = entries_[size_t(index) - 1];
    if (entry.size < 0x20) return false;
    const auto* data = bytes_.data() + entry.offset;
    if (detail::archive_be32(data) != 0x007b7960u) return false;
    const uint16_t count = be16(data + 6), field = be16(data + 0x14);
    if (!detail::archive_range(entry.size, 0x20, uint64_t(count) * 0x1c)) return false;
    const uint32_t core = 0x198u + uint32_t(count) * (field == 0xffffu ? 0x18u : 0x78u);
    *out = {{identity_, entry.offset}, entry.size, count, field, core, core + 0x20u};
    return true;
}
WorldMapSecondarySetupStatus prepare_world_map_secondary_model_setup(
    uint32_t index, uint64_t bank_identity, const WorldMapModelBank* bank,
    uint64_t model_identity, const std::optional<WorldMapSecondaryModelRecord>& model,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::optional<uint64_t>& arena, WorldMapSecondarySetupStep* out) {
    using Status = WorldMapSecondarySetupStatus;
    if (out == nullptr || bank_identity == 0) return Status::InvalidInput;
    WorldMapSecondarySetupStep step;
    auto finish = [&](Status status) { *out = std::move(step); return status; };
    if (bank == nullptr || !bank->loaded()) return finish(Status::RequiresBank);
    if (bank->identity() != bank_identity) return Status::InvalidInput;
    WorldMapModelResource resource;
    if (!bank->resolve(index, &resource)) return Status::InvalidInput;
    step.resource = resource;
    if (model_identity != 0) {
        if (!model) { step.required_record = model_identity; return finish(Status::RequiresModel); }
        if (model->identity != model_identity) return Status::InvalidInput;
        step.retain_playback = model->count_4 == resource.count_6;
        if (step.retain_playback) {
            if (model->playback_178 == 0) return Status::InvalidInput;
            const WorldMapAnimationPlayback* match = nullptr;
            for (const auto& record : records) if (record.identity == model->playback_178) {
                if (match != nullptr) return Status::InvalidInput;
                match = &record.state;
            }
            if (match == nullptr) { step.required_record = model->playback_178; return finish(Status::RequiresPlayback); }
            if (!std::isfinite(match->position_0) || !std::isfinite(match->rate_4) || !std::isfinite(match->limit_c) ||
                !std::isfinite(match->value_18)) return Status::InvalidInput;
            step.saved_playback = *match; // FUN_801A040C would later copy ALL seven fields without blend reset.
        }
    }
    if (!arena) return finish(Status::RequiresArena);
    WorldMapSecondaryConstructionRequest request;
    request.resource = resource; request.arena_identity = *arena;
    if (*arena == 0) { request.allocation_size = resource.allocation_size; request.result_flags_174 = 4; }
    step.construction = request;
    return finish(Status::RequiresConstruction);
}
WorldMapSecondaryReleaseStatus prepare_world_map_secondary_model_release(
    uint64_t model_identity, const std::optional<WorldMapSecondaryModelRecord>& model,
    const std::optional<WorldMapSecondaryFeatureRecord>& feature, WorldMapSecondaryReleaseStep* out) {
    using Status = WorldMapSecondaryReleaseStatus;
    if (out == nullptr || &model == &out->model_after || &feature == &out->feature_after) return Status::InvalidInput;
    WorldMapSecondaryReleaseStep step;
    step.wrapper_after = model_identity; step.model_after = model; step.feature_after = feature;
    auto finish = [&](Status status) { *out = std::move(step); return status; };
    auto call = [&](WorldMapSecondaryReleaseKind kind, uint64_t owner, uint64_t resource, uint32_t offset = 0) {
        step.feature_after = feature;
        step.required_call = WorldMapSecondaryReleaseCall{kind, owner, resource, offset};
        return finish(Status::RequiresBackend);
    };
    if (model_identity == 0) return finish(Status::Prepared);
    if (!model) { step.required_record = model_identity; return finish(Status::RequiresModel); }
    if (model->identity != model_identity) return Status::InvalidInput;
    if ((model->flags_174 & 2u) && model->resource_0 != 0) return call(WorldMapSecondaryReleaseKind::Asset, model_identity, model->resource_0);
    if ((model->flags_174 & 1u) && model->auxiliary_c != 0) return call(WorldMapSecondaryReleaseKind::Auxiliary, model_identity, model->auxiliary_c, 0xc);
    if (model->feature_14 != 0) {
        if (!feature) { step.required_record = model->feature_14; return finish(Status::RequiresFeature); }
        if (feature->identity != model->feature_14) return Status::InvalidInput;
        if (feature->buffer_34 != 0) {
            if (feature->flags_38 & 1u) return call(WorldMapSecondaryReleaseKind::Allocation, feature->identity, feature->buffer_34);
            step.feature_after->buffer_34 = 0;
        }
    }
    if (model->flags_174 & 4u) {
        if (model->allocation_10 != 0) return call(WorldMapSecondaryReleaseKind::Allocation, model_identity, model->allocation_10);
        return call(WorldMapSecondaryReleaseKind::Allocation, model_identity, model_identity);
    }
    step.wrapper_after = 0;
    return finish(Status::Prepared);
}
} // namespace awl
