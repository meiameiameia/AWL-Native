#include "awl/world_map_update_inputs.h"
#include <utility>

namespace awl {
WorldMapControlStatus prepare_world_map_control_word(
    uint32_t loop_count, const WorldMapControlSources& sources,
    const WorldMapSceneModeRequestState& scene, WorldMapControlWord* out) noexcept {
    using S = WorldMapControlStatus;
    if (!out) return S::InvalidInput;
    if (!sources.phase) return S::RequiresPhaseOwner;
    const auto& phase = *sources.phase;
    if (!phase.byte_8) return S::RequiresPhaseByte8;
    uint32_t word = loop_count % 6u == 0 ? 1u : 0u;
    if (*phase.byte_8 == 0) {
        word |= 0x800u;
    } else {
        if (!phase.raw_word_0) return S::RequiresPhaseRaw;
        const auto raw = *phase.raw_word_0;
        if ((raw / 10u) % 60u == 0) {
            word |= 4u;
            const auto minute = (raw / 600u) % 60u;
            if (minute % 10u == 0) word |= 2u;
            if (minute == 0) {
                word |= 8u;
                const auto hour = (raw / 36000u) % 24u;
                if (hour == 0) word |= 0x10u;
                if (hour == 12) word |= 0x20u;
            }
        }
    }
    if (!sources.stage_20) return S::RequiresStageOwner;
    if (*sources.stage_20 == 3 || *sources.stage_20 == 5) word |= 0x40u;
    else if (*sources.stage_20 == 4) word |= 0x80u;
    if (scene.global_flag_59b0 != 0) word |= 0x100u;
    if (scene.global_flag_59af != 0) word |= 0x2000u;
    // The override is last, after the original has consumed both event bytes.
    if (!phase.byte_a) return S::RequiresPhaseByteA;
    if (*phase.byte_a != 0) word = 0x800u;
    *out = {loop_count, word, word & ~0x10u};
    return S::Prepared;
}

WorldMapSceneFilterStatus prepare_world_map_dispatch_groups(
    int32_t mode, std::optional<uint8_t> scene_byte_79, uint32_t* out) noexcept {
    using S = WorldMapSceneFilterStatus;
    if (!out) return S::InvalidInput;
    uint32_t groups = 0;
    // Signed comparisons in 80012774, then 800127DC.
    if ((mode >= 0 && mode <= 2) || mode == 15 || (mode >= 17 && mode <= 28) ||
        (mode >= 31 && mode <= 41) || mode == 44 || mode == 45) {
        groups = 0x1fe0u; // 5,6,7,8,9,10,11,12
    } else if (mode >= 7 && mode <= 9) {
        groups = 0x1be0u; // 5,6,7,8,9,11,12
    } else {
        if (!scene_byte_79) return S::RequiresSceneByte79;
        if (*scene_byte_79 != 0) groups = 0xb20u; // 5,8,9,11
    }
    *out = groups;
    return S::Prepared;
}

WorldMapUpdateInputs::WorldMapUpdateInputs(std::shared_ptr<GameClock> clock) noexcept
    : clock_(std::move(clock)) {}

void WorldMapUpdateInputs::request_scene_mode(int32_t mode, uint8_t scene_byte) noexcept {
    // The existing helper rejects only a null state; this member is nonnull.
    static_cast<void>(apply_world_map_scene_mode_request(&scene_, mode, scene_byte));
}

WorldMapSceneFilterStatus WorldMapUpdateInputs::commit_scene_fields(
    uint32_t transition_flags, std::optional<uint8_t> scene_byte_79,
    WorldMapSceneModeCommit* out) noexcept {
    using S = WorldMapSceneFilterStatus;
    if (!out) return S::InvalidInput;
    auto next = scene_;
    WorldMapSceneModeCommit commit;
    if (!commit_world_map_scene_mode_fields(&next, transition_flags, &commit)) return S::InvalidInput;
    uint32_t groups = blocked_groups_;
    if (commit.transition) {
        const auto status = prepare_world_map_dispatch_groups(commit.mode, scene_byte_79, &groups);
        if (status != S::Prepared) return status;
    }
    scene_ = next;
    blocked_groups_ = groups;
    *out = commit;
    return commit.transition ? S::CommittedFields : S::Unchanged;
}

WorldMapControlStatus WorldMapUpdateInputs::capture_control() noexcept {
    using S = WorldMapControlStatus;
    if (!clock_) return S::RequiresClock;
    const auto loop = clock_->state().loop_count;
    if (control_ && control_->loop_count == loop) return S::AlreadyCaptured;
    WorldMapControlWord word;
    const auto status = prepare_world_map_control_word(loop, sources_, scene_, &word);
    if (status != S::Prepared) return status;
    control_ = word;
    scene_.global_flag_59b0 = 0;
    scene_.global_flag_59af = 0;
    return S::Captured;
}

WorldMapDispatchInput WorldMapUpdateInputs::dispatch(uint32_t priority) const noexcept {
    using S = WorldMapDispatchStatus;
    if (!clock_ || !control_ || control_->loop_count != clock_->state().loop_count) return {};
    const auto group = priority >> 12;
    if (group >= 24) return {S::UnsupportedPriority, {}};
    if ((blocked_groups_ & (1u << group)) != 0) return {S::Filtered, {}};
    return {S::Ready, control_->dispatch_word};
}
} // namespace awl
