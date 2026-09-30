#include "awl/world_map_secondary_model.h"
#include "world_map_flat_archive.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace awl {
namespace {
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
float as_float(uint32_t word) { float value; std::memcpy(&value, &word, sizeof(value)); return value; }
float nmsub(float a, float b, float c) { return -std::fma(a,b,-c); } // Negate AFTER rounding, including signed zero.
} // namespace
WorldMapModelPreparationStatus prepare_world_map_model_pose(
    const std::array<uint32_t, 13>& pose, WorldMapModelMatrix* out) {
    using Status = WorldMapModelPreparationStatus;
    if (out == nullptr) return Status::InvalidInput;
    const uint32_t flags = pose[0] >> 24;
    WorldMapModelMatrix matrix{1,0,0,0, 0,1,0,0, 0,0,1,0};
    if (flags & 0x10) {
        for (size_t i = 0; i < matrix.size(); ++i) matrix[i] = as_float(pose[i + 1]);
    } else {
        if (flags & 4) {
            const float x = as_float(pose[4]), y = as_float(pose[5]);
            const float z = as_float(pose[6]), w = as_float(pose[7]);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) return Status::InvalidInput;
            // FUN_801B83B0 paired-single products/sums, in their observed order.
            const float xx = x*x, yy = y*y, zz = z*z, zw = z*w;
            const float yw = y*w, xw = x*w;
            const float xz2 = std::fma(z,z,xx), yw2 = std::fma(w,w,yy);
            const float norm = xz2 + yw2;
            if (!std::isfinite(norm) || norm <= 0) return Status::InvalidInput;
            const float factor = 2.0f / norm; // Documented native numerical difference.
            if (!std::isfinite(factor)) return Status::InvalidInput;
            const float xy_plus = std::fma(x,y,zw), xy_minus = std::fma(x,y,-zw);
            const float xz_plus = std::fma(x,z,yw), yz_plus = std::fma(y,z,xw);
            const float xz_minus = nmsub(yw,2.0f,xz_plus);
            const float yz_minus = nmsub(xw,2.0f,yz_plus);
            matrix = {nmsub(zz+yy,factor,1.0f), xy_minus*factor, xz_plus*factor, 0,
                xy_plus*factor, nmsub(xz2,factor,1.0f), yz_minus*factor, 0,
                xz_minus*factor, yz_plus*factor, nmsub(xx+yy,factor,1.0f), 0};
        } else if (flags & 2) {
            // Zero rotations take the original identity branch. Preserve the
            // explicit stop for reached nonzero rotations/trigonometric math.
            for (size_t i = 4; i <= 6; ++i) {
                const float angle = as_float(pose[i]);
                if (!std::isfinite(angle)) return Status::InvalidInput;
                if (angle != 0) return Status::RequiresEulerRotation;
            }
        }
        if (flags & 1) {
            for (size_t column = 0; column < 3; ++column) {
                const float scale = as_float(pose[column + 1]);
                if (!std::isfinite(scale)) return Status::InvalidInput;
                for (size_t row = 0; row < 3; ++row) matrix[row*4 + column] *= scale;
            }
        }
        if (flags & 8) for (size_t row = 0; row < 3; ++row) matrix[row*4 + 3] = as_float(pose[row + 8]);
    }
    if (!std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); })) return Status::InvalidInput;
    *out = matrix;
    return Status::Prepared;
}
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
WorldMapModelPreparationStatus WorldMapModelBank::prepare(uint32_t index, WorldMapModelPreparationStep* out) const {
    using Status = WorldMapModelPreparationStatus;
    if (out == nullptr) return Status::InvalidInput;
    WorldMapPreparedModelResource result;
    if (!resolve(index, &result.metadata)) return Status::InvalidInput;
    const auto& meta = result.metadata;
    const auto* data = bytes_.data() + meta.reference.offset;
    auto word = [&](uint32_t offset) { return detail::archive_be32(data + offset); };
    auto reference = [&](uint32_t offset, std::optional<WorldMapModelResourceReference>& target) {
        if (offset == 0) return true;
        if (!detail::archive_range(meta.size, offset, 1) || uint64_t(meta.reference.offset) + offset > UINT32_MAX) return false;
        target = WorldMapModelResourceReference{identity_, meta.reference.offset + offset};
        return true;
    };
    result.word_18 = word(0x18); result.word_1c = word(0x1c);
    if ((result.word_18 != 0 && !reference(result.word_1c, result.pointer_1c)) ||
        !reference(word(0xc), result.pointer_c) || !reference(word(0x10), result.pointer_10)) return Status::InvalidInput;
    const uint32_t table_end = 0x20u + uint32_t(meta.count_6)*0x1cu;
    // Preflight layout before conversion. Original overwrites each pose in
    // place; repeating or overlapping that span is deliberately unsupported.
    std::vector<uint32_t> spans;
    for (uint32_t i = 0; i < meta.count_6; ++i) {
        const uint32_t offset = word(0x20u + i*0x1cu);
        if (offset == 0) continue;
        if (!detail::archive_range(meta.size, offset, 0x34)) return Status::InvalidInput;
        if (offset < table_end || offset%4 != 0) return Status::UnsupportedLayout;
        spans.push_back(offset);
    }
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); ++i) if (spans[i] - spans[i-1] < 0x34u) return Status::UnsupportedLayout;
    result.records.reserve(meta.count_6);
    for (uint32_t i = 0; i < meta.count_6; ++i) {
        const uint32_t start = 0x20u + i*0x1cu;
        WorldMapPreparedModelRecord record;
        if (!reference(word(start), record.pointers[0])) return Status::InvalidInput;
        if (record.pointers[0]) {
            std::array<uint32_t, 13> pose;
            for (uint32_t j = 0; j < pose.size(); ++j) pose[j] = word(word(start) + j*4);
            WorldMapModelMatrix matrix;
            const auto status = prepare_world_map_model_pose(pose, &matrix);
            if (status == Status::RequiresEulerRotation) {
                *out = {std::nullopt, i}; return status;
            }
            if (status != Status::Prepared) return status;
            record.matrix = matrix;
        }
        for (uint32_t j = 1; j < record.pointers.size(); ++j)
            if (!reference(word(start + j*4), record.pointers[j])) return Status::InvalidInput;
        record.word_14 = word(start + 0x14); record.word_18 = word(start + 0x18);
        result.records.push_back(std::move(record));
    }
    *out = {std::move(result), 0};
    return Status::Prepared;
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
    const auto preparation = bank->prepare(index, &request.preparation);
    if (preparation == WorldMapModelPreparationStatus::InvalidInput) return Status::InvalidInput;
    request.preparation_status = preparation;
    step.construction = std::move(request);
    if (preparation != WorldMapModelPreparationStatus::Prepared) return finish(Status::RequiresResourcePreparation);
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
