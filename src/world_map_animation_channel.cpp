#include "awl/world_map_animation_channel.h"
#include "world_map_flat_archive.h"

#include <algorithm>
#include <cmath>
#include <cfenv>
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
bool supported_float(float f) { return std::isfinite(f) && (f == 0 || std::isnormal(f)); }
int32_t signed_half(const uint8_t* p) { const int32_t v = be16(p); return v >= 32768 ? v - 65536 : v; }
// 801B8F0C: shortest-sign spherical mix, with separately rounded float
// products/sums. Native double acos/sin replace the original math library;
// they are not an instruction-equivalence claim for those library bodies.
bool spherical_mix(const float* a, const float* b, float t, float* out) noexcept {
    float products[4];
    for (unsigned i = 0; i < 4; ++i) {
        products[i] = a[i] * b[i];
        if (!supported_float(products[i])) return false;
    }
    float dot = products[0] + products[1];
    if (!supported_float(dot)) return false;
    dot = products[2] + dot;
    if (!supported_float(dot)) return false;
    dot = products[3] + dot;
    if (!supported_float(dot)) return false;
    const float sign = dot < 0 ? -1.0f : 1.0f;
    if (dot < 0) dot = -dot;
    float left = 1.0f - t, right = sign * t;
    // Exact float bits at 8034BEF0. Do not normalize or clamp the dot.
    if (dot <= as_float(0x3f7fff58u)) {
        const float angle = static_cast<float>(std::acos(static_cast<double>(dot)));
        const float denominator = static_cast<float>(std::sin(static_cast<double>(angle)));
        const float left_angle = left * angle, right_angle = t * angle;
        if (!supported_float(angle) || !supported_float(denominator) || denominator == 0 ||
            !supported_float(left_angle) || !supported_float(right_angle)) return false;
        const float left_sine = static_cast<float>(std::sin(static_cast<double>(left_angle)));
        const float right_sine = static_cast<float>(std::sin(static_cast<double>(right_angle)));
        if (!supported_float(left_sine) || !supported_float(right_sine)) return false;
        left = left_sine / denominator;
        const float ratio = right_sine / denominator;
        if (!supported_float(ratio)) return false;
        right = sign * ratio;
    }
    if (!supported_float(left) || !supported_float(right)) return false;
    for (unsigned i = 0; i < 4; ++i) {
        const float lp = left * a[i], rp = right * b[i];
        if (!supported_float(lp) || !supported_float(rp)) return false;
        out[i] = lp + rp;
        if (!supported_float(out[i])) return false;
    }
    return true;
}
WorldMapAnimationKeyStatus select_keys(const uint8_t* data, uint32_t size,
    const uint8_t* section, uint64_t table_end, float time, WorldMapAnimationKeyInterval* out) noexcept {
    using Status = WorldMapAnimationKeyStatus;
    const uint32_t count = be16(section + 8);
    if (count < 2) return Status::NoKeys; // 0DDC does not read layout/time here.
    const uint8_t encoding = section[12] >> 4, flags = section[13], modes = section[14], components = section[15];
    if (encoding > 4) return Status::UnsupportedLayout;
    const uint32_t width = encoding < 2 ? 1u : encoding < 4 ? 2u : 4u;
    const uint32_t vector_size = width * 3;
    const uint32_t quaternion_size = (4u - unsigned((flags >> 5) & 1) - unsigned((flags >> 6) & 1) - unsigned((flags >> 7) & 1)) * 2;
    uint64_t start = be32(section + 4);
    if (start < table_end) return Status::UnsupportedLayout;
    if (components & 1) start += vector_size;
    if (components & 2) start += vector_size;
    if (components & 8) start += quaternion_size;
    else if (components & 4) start += vector_size;
    if (components & 16) start += width * 12u;
    uint32_t stride = 2;
    auto vector_track = [&](uint8_t mode) {
        stride += vector_size;
        if (mode == 2 || mode == 3) stride += vector_size * 2;
        if (mode == 3) stride += 4;
    };
    if (flags & 1) vector_track(modes & 3);
    if (flags & 2) vector_track((modes >> 2) & 3);
    if (flags & 8) {
        stride += quaternion_size;
        const unsigned mode = (modes >> 4) & 7;
        // 0B34 uses section width for controls but fixed S16-size base
        // quaternion values. They coincide only for two-byte encodings.
        const uint32_t controls_size = (quaternion_size / 2) * width;
        if (mode == 4 || mode == 5) stride += controls_size * 2;
        if (mode == 5) stride += 4;
        if (mode == 7) stride += controls_size;
    } else if (flags & 4) vector_track((modes >> 4) & 7);
    if (flags & 16) stride += width * 12u;
    if (start % 2 != 0 || stride % 2 != 0 || !bounded(size,start,uint64_t(count)*stride)) return Status::UnsupportedLayout;
    if (!supported_float(time) || std::fegetround() != FE_TONEAREST) return Status::UnsupportedNumerics;
    uint32_t first = 0;
    for (; first + 2 < count; ++first) {
        const auto* next = data + size_t(start) + size_t(first + 1) * stride;
        if (time <= static_cast<float>(signed_half(next))) break;
    }
    const uint32_t first_offset = uint32_t(start) + first * stride, second_offset = first_offset + stride;
    const float duration = static_cast<float>(signed_half(data + second_offset)) - static_cast<float>(signed_half(data + first_offset));
    const float elapsed = time - static_cast<float>(signed_half(data + first_offset));
    if (!supported_float(elapsed)) return Status::UnsupportedNumerics;
    float fraction = 0;
    if (duration > 0 && elapsed > 0) fraction = elapsed >= duration ? 1.0f : elapsed / duration;
    if (!supported_float(fraction)) return Status::UnsupportedNumerics;
    *out = {uint16_t(first),uint16_t(first + 1),first_offset,second_offset,stride,fraction};
    return Status::Selected;
}
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
        std::vector<detail::FlatArchiveEntry> files;
        if (!detail::parse_flat_archive(bytes, &files)) return false;
        for (const auto& file : files) entries.push_back({file.offset, file.size});
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

WorldMapAnimationPoseStatus WorldMapAnimationBank::sample_constant_pose(
    WorldMapAnimationClipReference clip, uint32_t node,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) const noexcept {
    return sample_pose_impl(clip,node,std::nullopt,settings,prior,out);
}
WorldMapAnimationPoseStatus WorldMapAnimationBank::sample_pose(
    WorldMapAnimationClipReference clip, uint32_t node, float time,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) const noexcept {
    return sample_pose_impl(clip,node,time,settings,prior,out);
}
WorldMapAnimationKeyStatus WorldMapAnimationBank::select_key_interval(
    WorldMapAnimationClipReference clip, uint32_t node, float time, WorldMapAnimationKeyInterval* out) const noexcept {
    using Status = WorldMapAnimationKeyStatus;
    if (out == nullptr || !loaded() || clip.bank_identity != identity_) return Status::InvalidInput;
    const Entry* entry = nullptr;
    for (const auto& candidate : entries_) if (candidate.offset == clip.offset) { entry = &candidate; break; }
    if (entry == nullptr) return Status::InvalidInput;
    const auto* data = bytes_.data() + entry->offset;
    if (entry->size < 8) return Status::UnsupportedLayout;
    const uint32_t count = be16(data + 4);
    if (!bounded(entry->size,8,uint64_t(count)*16)) return Status::UnsupportedLayout;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* section = data + 8 + size_t(i)*16;
        if (be16(section+10) == (node & 0xffffu)) return select_keys(data,entry->size,section,8+uint64_t(count)*16,time,out);
    }
    return Status::NoPose;
}
WorldMapAnimationPoseStatus WorldMapAnimationBank::sample_pose_impl(
    WorldMapAnimationClipReference clip, uint32_t node, std::optional<float> time,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) const noexcept {
    using Status = WorldMapAnimationPoseStatus;
    if (out == nullptr || !loaded() || clip.bank_identity != identity_) return Status::InvalidInput;
    const Entry* entry = nullptr;
    for (const auto& candidate : entries_) if (candidate.offset == clip.offset) { entry = &candidate; break; }
    if (entry == nullptr) return Status::InvalidInput;
    const auto* data = bytes_.data() + entry->offset;
    if (entry->size < 8) return Status::UnsupportedLayout;
    const uint32_t count = be16(data + 4);
    const uint64_t table_end = 8 + uint64_t(count) * 16;
    if (!bounded(entry->size, 8, uint64_t(count) * 16)) return Status::UnsupportedLayout;
    const uint8_t* section = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* candidate = data + 8 + size_t(i) * 16;
        if (be16(candidate + 10) == (node & 0xffffu)) { section = candidate; break; }
    }
    if (section == nullptr) return Status::NoPose;
    const uint8_t flags = section[13], components = section[15];
    if ((flags & 31) != 0 && !time) return Status::RequiresKeyedTracks;
    if (flags & 0x14) return Status::RequiresKeyedTracks; // Euler/matrix paths.
    auto pose = prior;
    pose[0] &= 0x00ffffffu; // 0264's single-byte clear, not a whole word clear.
    uint64_t cursor = be32(section + 4); // 0734 relocates relative to clip base.
    auto decode = [&](uint8_t encoding, float scale, unsigned n, float* values) {
        if (encoding > 4) return Status::UnsupportedLayout;
        const unsigned width = encoding < 2 ? 1u : encoding < 4 ? 2u : 4u;
        if (cursor < table_end || (cursor % width) != 0 || !bounded(entry->size, cursor, uint64_t(n) * width))
            return Status::UnsupportedLayout;
        if (std::fegetround() != FE_TONEAREST || (encoding != 4 && !supported_float(scale)))
            return Status::UnsupportedNumerics;
        for (unsigned i = 0; i < n; ++i) {
            const auto* p = data + size_t(cursor) + i * width;
            if (encoding == 4) values[i] = as_float(be32(p));
            else {
                int32_t value = width == 1 ? p[0] : be16(p);
                if (encoding == 1 && value >= 128) value -= 256;
                if (encoding == 3 && value >= 32768) value -= 65536;
                values[i] = static_cast<float>(value) * scale;
            }
            if (!supported_float(values[i])) return Status::UnsupportedNumerics;
        }
        cursor += uint64_t(n) * width;
        return Status::Sampled;
    };
    auto store = [&](unsigned at, const float* values, unsigned n, uint32_t flag) {
        pose[0] |= flag << 24;
        for (unsigned i = 0; i < n; ++i) std::memcpy(&pose[at + i], values + i, 4);
    };
    const uint8_t encoding = section[12] >> 4;
    const float scale = std::ldexp(1.0f, -int(section[12] & 15));
    float values[4]{};
    if ((components & 2) && !(flags & 2)) {
        const auto status = decode(encoding, scale, 3, values);
        if (status != Status::Sampled) return status;
        store(1, values, 3, 1);
    }
    if ((components & 8) && !(flags & 8)) {
        // 1C1C omits XYZ lanes flagged by bits 5/6/7, but always reads W.
        const unsigned n = 4u - unsigned((flags >> 5) & 1) - unsigned((flags >> 6) & 1) - unsigned((flags >> 7) & 1);
        const uint64_t start = cursor;
        const auto status = decode(settings.quaternion_encoding, settings.quaternion_scale, n, values);
        if (status != Status::Sampled) return status;
        float quaternion[4]{};
        unsigned source = 0;
        for (unsigned i = 0; i < 3; ++i) if ((flags & (32u << i)) == 0) quaternion[i] = values[source++];
        quaternion[3] = values[source];
        store(4, quaternion, 4, 4);
        // The original helper returns count*2 regardless of encoding. Retain
        // this cursor rule even for supplied globals other than default S16.
        cursor = start + n * 2u;
    }
    if ((components & 1) && !(flags & 1)) {
        const auto status = decode(encoding, scale, 3, values);
        if (status != Status::Sampled) return status;
        store(8, values, 3, 8);
    }
    if (flags & 31) {
        WorldMapAnimationKeyInterval interval;
        const auto selected = select_keys(data,entry->size,section,table_end,*time,&interval);
        using KeyStatus = WorldMapAnimationKeyStatus;
        if (selected == KeyStatus::UnsupportedLayout) return Status::UnsupportedLayout;
        if (selected == KeyStatus::UnsupportedNumerics) return Status::UnsupportedNumerics;
        if (selected == KeyStatus::Selected) {
            // 0F5C advances key value cursors in scale/quaternion/translation
            // order. Controls begin after ALL keyed base components (0D0C).
            uint64_t first = uint64_t(interval.first_offset)+2, second = uint64_t(interval.second_offset)+2;
            const unsigned width = encoding < 2 ? 1u : encoding < 4 ? 2u : 4u;
            const unsigned quaternion_count = 4u - unsigned((flags >> 5) & 1) - unsigned((flags >> 6) & 1) - unsigned((flags >> 7) & 1);
            const uint64_t base_size = ((flags & 1) ? width * 3u : 0u) +
                ((flags & 2) ? width * 3u : 0u) + ((flags & 8) ? quaternion_count * 2u : 0u);
            const uint64_t first_control = first + base_size, second_control = second + base_size;
            auto vector = [&](unsigned at, uint32_t flag, uint8_t mode) {
                if (mode > 1) return Status::RequiresInterpolation;
                float a[3]{}, b[3]{};
                cursor = first; auto status = decode(encoding,scale,3,a); first = cursor;
                if (status != Status::Sampled) return status;
                cursor = second; status = decode(encoding,scale,3,b); second = cursor;
                if (status != Status::Sampled) return status;
                if (mode == 1) {
                    const float complement = 1.0f - interval.fraction;
                    for (unsigned i = 0; i < 3; ++i) {
                        // 1D88 has two separately rounded products, then add.
                        const float right = interval.fraction * b[i], left = complement * a[i];
                        if (!supported_float(right) || !supported_float(left)) return Status::UnsupportedNumerics;
                        a[i] = right + left;
                        if (!supported_float(a[i])) return Status::UnsupportedNumerics;
                    }
                }
                store(at,a,3,flag); return Status::Sampled;
            };
            if (flags & 2) {
                const auto status = vector(1,1,(section[14]>>2)&3); if (status != Status::Sampled) return status;
            }
            if (flags & 8) {
                float a[4]{}, b[4]{}, quaternion[4]{};
                auto read_quaternion = [&](uint64_t start, float* q) {
                    cursor = start; float compressed[4]{};
                    const auto status = decode(settings.quaternion_encoding,settings.quaternion_scale,quaternion_count,compressed);
                    if (status != Status::Sampled) return status;
                    unsigned source = 0;
                    for (unsigned i = 0; i < 3; ++i) q[i] = (flags & (32u << i)) ? 0.0f : compressed[source++];
                    q[3] = compressed[source];
                    return Status::Sampled;
                };
                auto status = read_quaternion(first,a); if (status != Status::Sampled) return status;
                status = read_quaternion(second,b); if (status != Status::Sampled) return status;
                first += quaternion_count * 2u; second += quaternion_count * 2u; // 1C1C return rule.
                const unsigned mode = (section[14] >> 4) & 7;
                if (mode == 0) std::copy(a,a+4,quaternion);
                else if (mode == 6) {
                    if (!spherical_mix(a,b,interval.fraction,quaternion)) return Status::UnsupportedNumerics;
                } else if (mode == 4 || mode == 7) {
                    float c0[4]{}, c1[4]{}, endpoints[4]{}, controls[4]{};
                    status = read_quaternion(first_control + (mode == 4 ? quaternion_count * 2u : 0u),c0);
                    if (status != Status::Sampled) return status;
                    status = read_quaternion(second_control,c1); if (status != Status::Sampled) return status;
                    // 801B90A4: mix endpoints and controls independently,
                    // then mix those results at (2*t)*(1-t), without normalization.
                    const float twice = 2.0f * interval.fraction, complement = 1.0f - interval.fraction;
                    const float curve_fraction = twice * complement;
                    if (!spherical_mix(a,b,interval.fraction,endpoints) ||
                        !spherical_mix(c0,c1,interval.fraction,controls) ||
                        !spherical_mix(endpoints,controls,curve_fraction,quaternion)) return Status::UnsupportedNumerics;
                } else return Status::RequiresInterpolation; // Mode 5 needs its separate remap settings.
                store(4,quaternion,4,4);
            }
            if (flags & 1) {
                const auto status = vector(8,8,section[14]&3); if (status != Status::Sampled) return status;
            }
        }
    }
    *out = pose;
    return Status::Sampled;
}

WorldMapAnimationPoseStatus sample_world_map_animation_pose(
    const WorldMapAnimationPlayback& playback, uint32_t node,
    const WorldMapAnimationBank* bank, const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) noexcept {
    using Status = WorldMapAnimationPoseStatus;
    if (out == nullptr) return Status::InvalidInput;
    if (!playback.clip_10) return Status::NoPose;
    if (bank == nullptr || !bank->loaded()) return Status::RequiresBank;
    WorldMapAnimationPose pose;
    const auto status = bank->sample_pose(*playback.clip_10, node, playback.position_0, settings, prior, &pose);
    if (status != Status::Sampled) return status;
    if (playback.link_14 != 0) return Status::RequiresBlend;
    *out = pose;
    return Status::Sampled;
}

WorldMapAnimationPoseStatus blend_world_map_animation_poses(
    const WorldMapAnimationPose* first, const WorldMapAnimationPose* second,
    float weight, const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) noexcept {
    using Status = WorldMapAnimationPoseStatus;
    if (out == nullptr) return Status::InvalidInput;
    const uint32_t first_flags = first == nullptr ? 0u : (*first)[0] >> 24;
    const uint32_t second_flags = second == nullptr ? 0u : (*second)[0] >> 24;
    const uint32_t flags = (first_flags | second_flags) & 13u;
    auto pose = prior; pose[0] &= 0x00ffffffu;
    // 23A0 skips unused components, including weight arithmetic.
    if (flags != 0 && (!supported_float(weight) || std::fegetround() != FE_TONEAREST))
        return Status::UnsupportedNumerics;
    for (uint32_t flag : {1u,4u,8u}) {
        if (!(flags & flag)) continue;
        const unsigned at = flag == 1 ? 1u : flag == 4 ? 4u : 8u;
        const unsigned n = flag == 4 ? 4u : 3u;
        float a[4]{}, b[4]{}, values[4]{};
        if (flag == 1) { std::fill(a,a+3,1.0f); std::fill(b,b+3,1.0f); }
        if (flag == 4) a[3] = b[3] = 1.0f;
        for (unsigned i = 0; i < n; ++i) {
            if (first_flags & flag) a[i] = as_float((*first)[at+i]);
            if (second_flags & flag) b[i] = as_float((*second)[at+i]);
            if (!supported_float(a[i]) || !supported_float(b[i])) return Status::UnsupportedNumerics;
        }
        if (flag == 4) {
            if (!spherical_mix(a,b,weight,values)) return Status::UnsupportedNumerics;
        } else {
            const float complement = 1.0f - weight;
            if (!supported_float(complement)) return Status::UnsupportedNumerics;
            for (unsigned i = 0; i < n; ++i) {
                const float left = a[i] * complement, right = b[i] * weight;
                if (!supported_float(left) || !supported_float(right)) return Status::UnsupportedNumerics;
                values[i] = left + right;
                if (!supported_float(values[i])) return Status::UnsupportedNumerics;
            }
        }
        pose[0] |= flag << 24;
        for (unsigned i = 0; i < n; ++i) std::memcpy(&pose[at+i],values+i,4);
    }
    *out = pose;
    return Status::Sampled;
}

namespace {
template<class Playback,class Record,class Weight>
WorldMapAnimationPoseStatus sample_blended_pose(
    const Playback& playback, uint32_t node,
    const std::vector<Record>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out,Weight&& weight) noexcept {
    using Status = WorldMapAnimationPoseStatus;
    if (out == nullptr) return Status::InvalidInput;
    auto sample = [&](const Playback& p, const WorldMapAnimationPose& input,
        WorldMapAnimationPose* result) {
        if (!p.clip_10) return Status::NoPose;
        const WorldMapAnimationBank* bank = nullptr;
        for (const auto* candidate : banks) {
            if (candidate != nullptr && candidate->loaded() && candidate->identity() == p.clip_10->bank_identity) {
                if (bank != nullptr) return Status::InvalidInput; // Ambiguous owner identity.
                bank = candidate;
            }
        }
        if (bank == nullptr) return Status::RequiresBank;
        return bank->sample_pose(*p.clip_10,node,p.position_0,settings,input,result);
    };
    WorldMapAnimationPose pose;
    auto status = sample(playback,prior,&pose);
    if (status != Status::Sampled) return status;
    const auto* current = &playback;
    size_t visited = 0;
    while (current->link_14 != 0) {
        // Any walk visiting more records than supplied must repeat one.
        // The DOL recursively unrolls nine records per FF8C call; iteration
        // preserves that order without risking an unbounded native stack.
        const Playback* next = nullptr;
        for (const auto& record : records) if (record.identity == current->link_14) {
            if (next != nullptr) return Status::InvalidInput;
            next = &record.state;
        }
        if (next == nullptr) return Status::RequiresPlaybackRecord;
        if (visited++ >= records.size()) return Status::CyclicBlend;
        WorldMapAnimationPose sampled{}; // Unwritten scratch words are not read by 23A0.
        status = sample(*next,sampled,&sampled);
        if (status == Status::NoPose) break; // FF8C stops before blending/following that record.
        if (status != Status::Sampled) return status;
        const auto blend_weight=weight(*current);
        if (!blend_weight && (((pose[0]|sampled[0])>>24)&13u)) return Status::RequiresBlendWeight;
        // With no flagged components 23A0 does not use weight arithmetic. This
        // scratch value neither writes a record nor establishes unknown +18.
        status = blend_world_map_animation_poses(&pose,&sampled,blend_weight.value_or(0.0f),pose,&pose);
        if (status != Status::Sampled) return status;
        current = next;
    }
    *out = pose;
    return Status::Sampled;
}
} // namespace
WorldMapAnimationPoseStatus sample_world_map_blended_animation_pose(
    const WorldMapAnimationPlayback& playback,uint32_t node,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior,WorldMapAnimationPose* out) noexcept {
    return sample_blended_pose(playback,node,records,banks,settings,prior,out,
        [](const auto& record) {return std::optional<float>(record.value_18);});
}
WorldMapAnimationPoseStatus sample_world_map_partial_blended_animation_pose(
    const WorldMapAnimationPartialPlayback& playback,uint32_t node,
    const std::vector<WorldMapAnimationPartialPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior,WorldMapAnimationPose* out) noexcept {
    return sample_blended_pose(playback,node,records,banks,settings,prior,out,
        [](const auto& record) {return record.value_18;});
}

std::optional<WorldMapAnimationPlayback> WorldMapAnimationPartialPlayback::complete() const noexcept {
    if(!word_8 || !limit_c || !value_18)return std::nullopt;
    return WorldMapAnimationPlayback{position_0,rate_4,*word_8,*limit_c,clip_10,link_14,*value_18};
}
WorldMapAnimationPartialPlayback partial_world_map_animation_playback(const WorldMapAnimationPlayback& p) noexcept {
    return {p.position_0,p.rate_4,p.word_8,p.limit_c,p.clip_10,p.link_14,p.value_18};
}
WorldMapAnimationChannelStatus prepare_world_map_partial_animation_channel(
    const WorldMapAnimationChannelState& channel,
    const std::vector<WorldMapAnimationPartialPlaybackRecord>& records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationPartialChannelStep* out) {
    using Status = WorldMapAnimationChannelStatus;
    if (out == nullptr || &channel == &out->after || &records == &out->records_after ||
        setup.model_identity == 0 || setup.bank_identity == 0 || !std::isfinite(setup.start_value)) return Status::InvalidInput;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].identity == 0) return Status::InvalidInput;
        for (size_t j = 0; j < i; ++j) if (records[i].identity == records[j].identity) return Status::InvalidInput;
    }
    WorldMapAnimationPartialChannelStep step;
    step.after = channel; step.records_after = records;
    auto stop = [&](Status status, uint64_t required = 0) {
        step.after = channel; step.records_after = records; step.required_record = required;
        *out = step; return status;
    };
    if (!model) return stop(Status::RequiresModelBinding);
    if (model->model_identity != setup.model_identity || model->playback_178 == 0) return Status::InvalidInput;
    auto find = [&](uint64_t identity) -> WorldMapAnimationPartialPlayback* {
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
        uint32_t missing_fields = 0;
        auto copy = [&](uint64_t source, uint64_t destination) -> std::optional<uint64_t> {
            const auto* from = find(source);
            if (from == nullptr) return source;
            auto* to = find(destination);
            if (to == nullptr) return destination;
            // NaN conversion/payload behavior of PPC lfs/stfs is outside this
            // finite snapshot translation. Unread/retained fields stay opaque.
            if (!std::isfinite(from->position_0) || !std::isfinite(from->rate_4)) {
                unsafe_copy = true;
                return std::nullopt;
            }
            if(!from->word_8){missing_fields=1;return source;}
            if(!from->limit_c){missing_fields=2;return source;}
            if(!std::isfinite(*from->limit_c)){unsafe_copy=true;return std::nullopt;}
            *to = *from; to->link_14 = 0; to->value_18 = 0.0f;
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
        if(missing_fields){step.required_fields=missing_fields;return stop(Status::RequiresPlaybackFields,*missing);}
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

WorldMapAnimationChannelStatus prepare_world_map_animation_channel(
    const WorldMapAnimationChannelState& channel,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationChannelStep* out) {
    using Status=WorldMapAnimationChannelStatus;
    if(out==nullptr || &channel==&out->after || &records==&out->records_after)return Status::InvalidInput;
    std::vector<WorldMapAnimationPartialPlaybackRecord> partial;
    partial.reserve(records.size());
    for(const auto& record:records)partial.push_back({record.identity,partial_world_map_animation_playback(record.state)});
    WorldMapAnimationPartialChannelStep prepared;
    const auto status=prepare_world_map_partial_animation_channel(channel,partial,setup,model,bank,&prepared);
    if(status==Status::InvalidInput)return status;
    WorldMapAnimationChannelStep step;
    step.after=prepared.after;step.branch=prepared.branch;step.clip=prepared.clip;step.required_record=prepared.required_record;
    step.records_after.reserve(prepared.records_after.size());
    for(const auto& record:prepared.records_after){
        const auto complete=record.state.complete();if(!complete)return Status::InvalidInput;
        step.records_after.push_back({record.identity,*complete});
    }
    *out=std::move(step);return status;
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
