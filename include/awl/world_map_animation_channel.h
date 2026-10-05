#pragma once

#include "awl/world_map_actor_animation.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {

struct WorldMapAnimationClipReference {
    uint64_t bank_identity = 0;
    uint32_t offset = 0;
};

struct WorldMapAnimationClip {
    WorldMapAnimationClipReference reference;
    uint32_t size = 0;
    float parameter_zero = 0;
};

using WorldMapAnimationPose = std::array<uint32_t, 13>;
enum class WorldMapAnimationPoseStatus {
    Sampled, NoPose, RequiresBank, RequiresKeyedTracks, RequiresBlend,
    UnsupportedLayout, UnsupportedNumerics, InvalidInput, RequiresInterpolation,
    RequiresPlaybackRecord, CyclicBlend, RequiresBlendWeight,
};
enum class WorldMapAnimationKeyStatus {
    Selected, NoKeys, NoPose, UnsupportedLayout, UnsupportedNumerics, InvalidInput,
};
struct WorldMapAnimationKeyInterval {
    uint16_t first = 0, second = 0;
    uint32_t first_offset = 0, second_offset = 0, stride = 0;
    float fraction = 0;
};
// Supplied snapshot of the quaternion globals at 803489F8 / 803489F0.
// Defaults are their verified DOL initial values, not live global ownership.
struct WorldMapAnimationPoseSettings {
    uint8_t quaternion_encoding = 3;
    float quaternion_scale = 0x1p-14f;
};

// Owns a raw clip or a flat U8 bank. Only node extents, the bounded 16-byte
// section table and first section ID zero's scalar are supported. Constant
// node components, step/linear vectors and quaternion modes 0/4/6/7 can
// be sampled. Spherical math uses the native double acos/sin library.
class WorldMapAnimationBank {
public:
    [[nodiscard]] bool parse(uint64_t identity, std::vector<uint8_t> bytes);
    void clear();
    [[nodiscard]] bool loaded() const { return identity_ != 0; }
    [[nodiscard]] uint64_t identity() const { return identity_; }
    [[nodiscard]] bool is_archive() const { return archive_; }
    [[nodiscard]] size_t clip_count() const { return entries_.size(); }
    // FUN_801A0A88: raw clips ignore the index; U8 uses (index & 0xFFFF)+1.
    // FUN_801A0AF4 scans in order; duplicate ID zero keeps the first match.
    // Missing/nonfinite scalar, out-of-range node and malformed table fail
    // without changing output. A successful lookup is not clip playback.
    [[nodiscard]] bool resolve(uint32_t index, WorldMapAnimationClip* out) const;
    // Bounded 0AF4 -> 1194/0F5C constant-component path, before 01F8 blending.
    // First matching low-halfword node ID wins. Unwritten pose words retain
    // prior bytes; success clears only the high flag byte before setters.
    // All non-Sampled results preserve output, including prior/output alias.
    [[nodiscard]] WorldMapAnimationPoseStatus sample_constant_pose(
        WorldMapAnimationClipReference clip, uint32_t node,
        const WorldMapAnimationPoseSettings& settings,
        const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) const noexcept;
    // 0DDC/0B34 key layout and ordered pair selection, then 1CEC clamped
    // fraction. Offsets are clip-relative. Payload values remain opaque;
    // selection does not establish interpolation support or advance time.
    [[nodiscard]] WorldMapAnimationKeyStatus select_key_interval(
        WorldMapAnimationClipReference clip, uint32_t node, float time,
        WorldMapAnimationKeyInterval* out) const noexcept;
    // Constant components, keyed scale/translation modes 0/1 and quaternion
    // modes 0/4/6/7. Quaternion remap mode 5, Euler/matrix and vector curves stop.
    [[nodiscard]] WorldMapAnimationPoseStatus sample_pose(
        WorldMapAnimationClipReference clip, uint32_t node, float time,
        const WorldMapAnimationPoseSettings& settings,
        const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) const noexcept;
    // Retained native bank identities cannot silently acquire new bytes.
    [[nodiscard]] bool same_contents(const WorldMapAnimationBank& other) const {
        return identity_ == other.identity_ && bytes_ == other.bytes_;
    }
private:
    struct Entry { uint32_t offset = 0, size = 0; };
    uint64_t identity_ = 0;
    bool archive_ = false;
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    [[nodiscard]] WorldMapAnimationPoseStatus sample_pose_impl(
        WorldMapAnimationClipReference clip, uint32_t node, std::optional<float> time,
        const WorldMapAnimationPoseSettings& settings,
        const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) const noexcept;
};

// Semantic snapshots, not serialized layouts. Optional clip replaces the
// original nullable pointer. Record identities preserve aliasing/copy order.
struct WorldMapAnimationPlayback {
    float position_0 = 0;
    float rate_4 = 0;
    uint32_t word_8 = 0;
    float limit_c = 0;
    std::optional<WorldMapAnimationClipReference> clip_10;
    uint64_t link_14 = 0; // Stable identity for the blend-record pointer.
    float value_18 = 0;
};
// Bounded 01F8/0264: null clip/missing node returns NoPose. Unsupported
// interpolation or a blend link stops explicitly. Supplied time is used
// only for reached keys; no looping, clock or live model is advanced.
[[nodiscard]] WorldMapAnimationPoseStatus sample_world_map_animation_pose(
    const WorldMapAnimationPlayback& playback, uint32_t node,
    const WorldMapAnimationBank* bank, const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) noexcept;
struct WorldMapAnimationPlaybackRecord {
    uint64_t identity = 0;
    WorldMapAnimationPlayback state;
};
// 23A0 component blend: missing flagged components use scale (1,1,1),
// quaternion (0,0,0,1), translation (0,0,0). Weight is not clamped.
// Only flags 1/4/8 participate; unselected words/padding retain prior bytes.
// Failure preserves output; either input and prior may alias output.
[[nodiscard]] WorldMapAnimationPoseStatus blend_world_map_animation_poses(
    const WorldMapAnimationPose* first, const WorldMapAnimationPose* second,
    float weight, const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) noexcept;
// 01F8/FF8C ordered chain evaluation using supplied immutable bank/record
// snapshots. A missing linked node stops successfully; missing bank/record,
// ambiguous identities, unsupported sampling/numerics and cycles publish
// nothing. No records, clocks, model ownership or bank bytes are mutated.
[[nodiscard]] WorldMapAnimationPoseStatus sample_world_map_blended_animation_pose(
    const WorldMapAnimationPlayback& playback, uint32_t node,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior, WorldMapAnimationPose* out) noexcept;
// FDE8 initializes only position/rate/clip/link. FE08 can subsequently write
// word/limit while leaving weight unknown. Absence means unwritten, not zero.
struct WorldMapAnimationPartialPlayback {
    float position_0 = 0, rate_4 = 0;
    std::optional<uint32_t> word_8;
    std::optional<float> limit_c;
    std::optional<WorldMapAnimationClipReference> clip_10;
    uint64_t link_14 = 0;
    std::optional<float> value_18;
    [[nodiscard]] std::optional<WorldMapAnimationPlayback> complete() const noexcept;
};
[[nodiscard]] WorldMapAnimationPartialPlayback partial_world_map_animation_playback(
    const WorldMapAnimationPlayback& playback) noexcept;
struct WorldMapAnimationPartialPlaybackRecord {
    uint64_t identity = 0;
    WorldMapAnimationPartialPlayback state;
};
// Same reached 01F8/FF8C sequence on partial owner records. Word +8, limit +C
// and rate +4 are not sampler inputs. Unknown +18 blocks only a reached blend
// with flagged components; null clips/missing nodes stop before that read.
// No projection establishes a complete playback or writes unknown fields.
[[nodiscard]] WorldMapAnimationPoseStatus sample_world_map_partial_blended_animation_pose(
    const WorldMapAnimationPartialPlayback& playback,uint32_t node,
    const std::vector<WorldMapAnimationPartialPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const WorldMapAnimationPoseSettings& settings,
    const WorldMapAnimationPose& prior,WorldMapAnimationPose* out) noexcept;
struct WorldMapAnimationChannelState {
    uint32_t elapsed_0 = 0;
    uint32_t duration_4 = 0;
    uint64_t target_8 = 0;
    uint64_t previous_c = 0;
    uint64_t older_10 = 0;
    // FUN_801A67A8 leaves this unwritten; supplied snapshots may establish it.
    std::optional<float> blend_14 = 0.0f;
    uint32_t mode_18 = 0;
};
struct WorldMapAnimationModelBinding {
    uint64_t model_identity = 0;
    uint64_t playback_178 = 0; // FUN_801A0404's observed pointer.
};
enum class WorldMapAnimationChannelBranch { NoClip, Completed, First, Interrupted };
enum class WorldMapAnimationChannelStatus {
    Prepared, Advanced, RequiresModelBinding, RequiresPlaybackRecord, RequiresBank, InvalidInput,
    RequiresPlaybackFields, AllocationFailure, RequiresChannelFields,
};
struct WorldMapAnimationPartialChannelStep {
    WorldMapAnimationChannelState after;
    std::vector<WorldMapAnimationPartialPlaybackRecord> records_after;
    std::optional<WorldMapAnimationChannelBranch> branch;
    std::optional<WorldMapAnimationClip> clip;
    uint64_t required_record = 0;
    // RequiresPlaybackFields: 1 is +8, 2 is +C. RequiresChannelFields: 1 is +14.
    uint32_t required_fields = 0;
};
// The same ordered 6878 helper with explicit unwritten playback fields.
// FECC requires source +8/+C, but resets +14/+18 without reading them.
// FE08 preserves target rate/link/weight, including an unknown weight.
// Stops retain all input records/channel; invalid input preserves output.
[[nodiscard]] WorldMapAnimationChannelStatus prepare_world_map_partial_animation_channel(
    const WorldMapAnimationChannelState& channel,
    const std::vector<WorldMapAnimationPartialPlaybackRecord>& records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationPartialChannelStep* out);
struct WorldMapAnimationChannelStep {
    WorldMapAnimationChannelState after;
    std::vector<WorldMapAnimationPlaybackRecord> records_after;
    std::optional<WorldMapAnimationChannelBranch> branch;
    std::optional<WorldMapAnimationClip> clip;
    uint64_t required_record = 0;
};

// FUN_801A6878, including ordered FUN_8019FECC copies and FUN_8019FE08
// target initialization. Supplies model +178 and reached playback records;
// never creates or discovers a model. The complete helper is prepared
// atomically; missing evidence leaves after/records_after at the input.
// Nonfinite start/section-zero/copied float values are unsupported; unread
// and retained fields stay opaque. Complete record aliases are represented,
// not arbitrary overlapping byte ranges or serialized PPC object layouts.
// No parent descriptor/presentation acknowledgement or frame update exists.
[[nodiscard]] WorldMapAnimationChannelStatus prepare_world_map_animation_channel(
    const WorldMapAnimationChannelState& channel,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationChannelStep* out);
// Applies only the complete isolated helper to supplied snapshots.
[[nodiscard]] WorldMapAnimationChannelStatus advance_world_map_animation_channel(
    WorldMapAnimationChannelState* channel,
    std::vector<WorldMapAnimationPlaybackRecord>* records,
    const WorldMapActorAnimationSetup& setup,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationBank* bank, WorldMapAnimationChannelStep* out);

} // namespace awl
