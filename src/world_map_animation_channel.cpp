#include "awl/world_map_animation_channel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace awl {
namespace {
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
bool bounded(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}
float as_float(uint32_t word) { float f; std::memcpy(&f, &word, sizeof(f)); return f; }
} // namespace

void WorldMapAnimationBank::clear() {
    identity_ = 0; archive_ = false; bytes_.clear(); entries_.clear();
}
bool WorldMapAnimationBank::parse(uint64_t identity, std::vector<uint8_t> bytes) {
    clear();
    if (identity == 0 || bytes.size() < 8 || bytes.size() > UINT32_MAX) return false;
    std::vector<Entry> entries;
    const bool archive = be32(bytes.data()) == 0x55aa382du;
    if (!archive) {
        // Section payload offsets stay opaque; bound the whole record table.
        if (!bounded(bytes.size(), 8, uint64_t(be16(bytes.data() + 4)) * 16)) return false;
        entries.push_back({0, static_cast<uint32_t>(bytes.size())});
    } else {
        if (bytes.size() < 0x20) return false;
        const uint32_t nodes = be32(bytes.data() + 4), metadata = be32(bytes.data() + 8);
        const uint32_t data_start = be32(bytes.data() + 12);
        if (nodes < 0x20 || !bounded(bytes.size(), nodes, metadata) ||
            uint64_t(nodes) + metadata > data_start || data_start > bytes.size() ||
            !bounded(bytes.size(), nodes, 12)) return false;
        const uint32_t count = be32(bytes.data() + nodes + 8);
        const uint64_t names = uint64_t(nodes) + uint64_t(count) * 12;
        const uint64_t end = uint64_t(nodes) + metadata;
        if (count < 2 || names >= end || !bounded(bytes.size(), nodes, uint64_t(count) * 12) ||
            be32(bytes.data() + nodes) != 0x01000000u || be32(bytes.data() + nodes + 4) != 0) return false;
        std::vector<std::pair<uint64_t, uint64_t>> extents;
        for (uint32_t i = 1; i < count; ++i) {
            const auto* node = bytes.data() + nodes + size_t(i) * 12;
            const uint32_t tag = be32(node), offset = be32(node + 4), size = be32(node + 8);
            const uint64_t name = names + (tag & 0xffffffu);
            if ((tag >> 24) != 0 || name >= end || offset < data_start || size == 0 ||
                !bounded(bytes.size(), offset, size)) return false;
            const auto* first = bytes.data() + name;
            const void* terminator = std::memchr(first, 0, static_cast<size_t>(end - name));
            if (terminator == nullptr || terminator == first) return false;
            entries.push_back({offset, size});
            extents.emplace_back(offset, uint64_t(offset) + size);
        }
        std::sort(extents.begin(), extents.end());
        for (size_t i = 1; i < extents.size(); ++i) {
            if (extents[i].first < extents[i - 1].second) return false;
        }
    }
    bytes_ = std::move(bytes); entries_ = std::move(entries);
    archive_ = archive; identity_ = identity;
    return true;
}
bool WorldMapAnimationBank::resolve(uint32_t index, WorldMapAnimationClip* out) const {
    if (out == nullptr || !loaded()) return false;
    const size_t selected = archive_ ? index & 0xffffu : 0;
    if (selected >= entries_.size()) return false;
    const auto entry = entries_[selected];
    if (entry.size < 8) return false;
    const auto* data = bytes_.data() + entry.offset;
    const uint32_t count = be16(data + 4);
    if (!bounded(entry.size, 8, uint64_t(count) * 16)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* record = data + 8 + size_t(i) * 16;
        if (be16(record + 10) == 0) {
            const float scalar = as_float(be32(record));
            if (!std::isfinite(scalar)) return false;
            *out = {{identity_, entry.offset}, entry.size, scalar};
            return true;
        }
    }
    return false;
}

WorldMapAnimationChannelStatus prepare_world_map_animation_channel(
    const WorldMapAnimationChannelState& channel,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationChannelStep* out) {
    using Status = WorldMapAnimationChannelStatus;
    if (out == nullptr || &channel == &out->after || &records == &out->records_after ||
        setup.model_identity == 0 || setup.bank_identity == 0 || !std::isfinite(setup.start_value)) return Status::InvalidInput;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].identity == 0) return Status::InvalidInput;
        for (size_t j = 0; j < i; ++j) if (records[i].identity == records[j].identity) return Status::InvalidInput;
    }
    WorldMapAnimationChannelStep step;
    step.after = channel; step.records_after = records;
    auto stop = [&](Status status, uint64_t required = 0) {
        step.after = channel; step.records_after = records; step.required_record = required;
        *out = step; return status;
    };
    if (!model) return stop(Status::RequiresModelBinding);
    if (model->model_identity != setup.model_identity || model->playback_178 == 0) return Status::InvalidInput;
    auto find = [&](uint64_t identity) -> WorldMapAnimationPlayback* {
        for (auto& record : step.records_after) if (record.identity == identity) return &record.state;
        return nullptr;
    };
    auto* model_state = find(model->playback_178);
    if (model_state == nullptr) return stop(Status::RequiresPlaybackRecord, model->playback_178);
    // Nonzero clip pointer is a boolean in FUN_8019FE54, then low-byte tested.
    if (!model_state->clip_10) {
        step.branch = WorldMapAnimationChannelBranch::NoClip;
        step.after.mode_18 = 0;
    } else {
        // The reached record copies execute in order, including aliasing.
        bool unsafe_copy = false;
        auto copy = [&](uint64_t source, uint64_t destination) -> std::optional<uint64_t> {
            const auto* from = find(source);
            if (from == nullptr) return source;
            auto* to = find(destination);
            if (to == nullptr) return destination;
            // NaN conversion/payload behavior of PPC lfs/stfs is outside this
            // finite snapshot translation. Unread/retained fields stay opaque.
            if (!std::isfinite(from->position_0) || !std::isfinite(from->rate_4) || !std::isfinite(from->limit_c)) {
                unsafe_copy = true;
                return std::nullopt;
            }
            *to = *from; to->word_14 = 0; to->value_18 = 0.0f;
            return std::nullopt;
        };
        std::optional<uint64_t> missing;
        if (channel.elapsed_0 >= channel.duration_4) {
            step.branch = WorldMapAnimationChannelBranch::Completed;
            missing = copy(model->playback_178, channel.previous_c);
            step.after.mode_18 = 1;
        } else if (channel.mode_18 == 0) {
            step.branch = WorldMapAnimationChannelBranch::First;
            missing = copy(model->playback_178, channel.previous_c);
            step.after.mode_18 = 1;
        } else {
            step.branch = WorldMapAnimationChannelBranch::Interrupted;
            missing = copy(channel.previous_c, channel.older_10);
            if (!missing && !unsafe_copy) missing = copy(channel.target_8, channel.previous_c);
            // PPC fsubs converts each unsigned word to binary32 before fdivs.
            step.after.blend_14 = static_cast<float>(channel.elapsed_0) / static_cast<float>(channel.duration_4);
            step.after.mode_18 = 2;
        }
        if (unsafe_copy) return Status::InvalidInput;
        if (missing) return *missing == 0 ? Status::InvalidInput : stop(Status::RequiresPlaybackRecord, *missing);
    }
    if (bank == nullptr || !bank->loaded()) return stop(Status::RequiresBank);
    if (bank->identity() != setup.bank_identity) return Status::InvalidInput;
    WorldMapAnimationClip clip;
    if (!bank->resolve(setup.clip_index, &clip)) return Status::InvalidInput;
    if (channel.target_8 == 0) return Status::InvalidInput;
    auto* target = find(channel.target_8);
    if (target == nullptr) return stop(Status::RequiresPlaybackRecord, channel.target_8);
    // FUN_8019FE08 leaves +4/+14/+18 alone (including earlier aliased copies).
    target->clip_10 = clip.reference; target->position_0 = setup.start_value;
    target->word_8 = 0; target->limit_c = clip.parameter_zero;
    step.after.elapsed_0 = 0; step.after.duration_4 = setup.blend_count;
    step.clip = clip;
    *out = std::move(step);
    return Status::Prepared;
}

WorldMapAnimationChannelStatus advance_world_map_animation_channel(
    WorldMapAnimationChannelState* channel,
    std::vector<WorldMapAnimationPlaybackRecord>* records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationChannelStep* out) {
    using Status = WorldMapAnimationChannelStatus;
    if (channel == nullptr || records == nullptr) return Status::InvalidInput;
    const auto status = prepare_world_map_animation_channel(*channel, *records, setup, model, bank, out);
    if (status != Status::Prepared) return status;
    *channel = out->after; *records = out->records_after;
    return Status::Advanced;
}
} // namespace awl
