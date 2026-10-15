#pragma once
#include "awl/game_clock.h"
#include "awl/input.h"
#include "awl/world_map_scene_index.h"
#include <memory>
#include <optional>

namespace awl {
struct WorldMapControlPhase {
    std::optional<uint8_t> byte_8;
    // Read only when byte_8 is nonzero; separate from GameClock raw time.
    std::optional<uint32_t> raw_word_0;
    std::optional<uint8_t> byte_a;
};
struct WorldMapControlSources {
    std::optional<WorldMapControlPhase> phase;
    std::optional<int32_t> stage_20;
};
struct WorldMapControlWord {
    uint32_t loop_count = 0;
    uint32_t producer_word = 0; // 80012568, before main-loop filtering
    uint32_t dispatch_word = 0; // 8000AC64 clears bit 0x10
};
enum class WorldMapControlStatus {
    Prepared, Captured, AlreadyCaptured, RequiresClock, RequiresPhaseOwner,
    RequiresPhaseByte8, RequiresPhaseRaw, RequiresStageOwner,
    RequiresPhaseByteA, InvalidInput
};
// 80012568/getters and the main-loop bit clear, in original branch order.
// This proposal does not consume the request state's one-shot bytes.
// Unknown reached fields or null output preserve *out. No allocation.
[[nodiscard]] WorldMapControlStatus prepare_world_map_control_word(
    uint32_t loop_count, const WorldMapControlSources& sources,
    const WorldMapSceneModeRequestState& scene, WorldMapControlWord* out) noexcept;

enum class WorldMapSceneFilterStatus {
    Prepared, CommittedFields, Unchanged, RequiresSceneByte79, InvalidInput
};
// 80177D30's mode predicates/group writes over the constructor's 24 bits.
// Byte +79 is required only after both mode predicates fail. All signed mode
// words follow those predicates; no invented scene-mode range is imposed.
[[nodiscard]] WorldMapSceneFilterStatus prepare_world_map_dispatch_groups(
    int32_t mode, std::optional<uint8_t> scene_byte_79, uint32_t* out) noexcept;

enum class WorldMapDispatchStatus { Ready, Filtered, RequiresControl, UnsupportedPriority };
struct WorldMapDispatchInput {
    WorldMapDispatchStatus status = WorldMapDispatchStatus::RequiresControl;
    std::optional<uint32_t> control_word;
};
struct WorldMapPadSnapshot {
    uint32_t loop_count=0,raw_time=0;
    HsdPadFrame frame;
};
enum class WorldMapPadCaptureStatus {Captured,AlreadyCaptured,RequiresClock,RequiresSample};

// Owns explicit phase/stage observations, existing scene-request fields and
// one-shot bytes, the derived group filter and one captured control per loop.
// Fresh request fields/zero group bits match their constructors/CRT. Phase and
// stage have no invented startup values. The shared clock must outlive use.
// No phase progression, scene construction, list registration/iteration,
// work-queue consumer, rendering or live game-loop scheduling is activated.
class WorldMapUpdateInputs {
public:
    explicit WorldMapUpdateInputs(std::shared_ptr<GameClock> clock) noexcept;
    WorldMapUpdateInputs(const WorldMapUpdateInputs&) = delete;
    WorldMapUpdateInputs& operator=(const WorldMapUpdateInputs&) = delete;
    const std::shared_ptr<GameClock>& game_clock() const noexcept { return clock_; }
    const WorldMapControlSources& sources() const noexcept { return sources_; }
    const WorldMapSceneModeRequestState& scene() const noexcept { return scene_; }
    uint32_t blocked_groups() const noexcept { return blocked_groups_; }
    // May describe the prior loop after the clock advances. Dispatch checks
    // freshness; callers must not use this diagnostic accessor to bypass it.
    const std::optional<WorldMapControlWord>& control() const noexcept { return control_; }
    void observe_control_sources(const WorldMapControlSources& sources) noexcept { sources_ = sources; }
    // Existing 8017767C field effects supply the same owned one-shot bytes
    // consumed by capture_control. A request never rebuilds the group filter.
    void request_scene_mode(int32_t mode, uint8_t scene_byte) noexcept;
    // Compose existing 17755C field commit with 177D30 group selection only.
    // On a transition, scene construction/teardown remains the caller's open
    // dependency. A missing reached +79 rolls back fields, filter and output.
    // No pending request leaves the mask intact and does not read +79.
    [[nodiscard]] WorldMapSceneFilterStatus commit_scene_fields(
        uint32_t transition_flags, std::optional<uint8_t> scene_byte_79,
        WorldMapSceneModeCommit* out) noexcept;
    // Capture before scene commit/primary dispatch, as in the main loop.
    // Successful capture clears both event bytes even if +A replaces the word.
    // Repeated capture in the same loop preserves that word and newer requests.
    // Missing dependencies preserve everything. Later callback stops do not
    // undo capture; retry uses its retained word. This is an input boundary,
    // not acknowledgement that the whole loop or its work queues completed.
    [[nodiscard]] WorldMapControlStatus capture_control() noexcept;
    // Raw first-channel sample, supplied by the existing native PAD adapter
    // (or a recording). No OS polling or invented pressed word. Capture once
    // per shared-clock loop using the existing bounded HSD filter; retries
    // retain edges/countdown. Observation is consumed only on a new capture.
    void observe_pad_sample(const PadSample& sample) noexcept {pad_sample_=sample;}
    [[nodiscard]] WorldMapPadCaptureStatus capture_pad() noexcept;
    // Null for missing/stale capture. PAD and control capture are independent
    // earlier input stages; callback failure never rolls either one back.
    const WorldMapPadSnapshot* pad_for_loop() const noexcept;
    // D018/D260's update-queue pass uses the captured control directly,
    // without primary-list group filtering. Reject absent or stale snapshots.
    [[nodiscard]] WorldMapDispatchInput queue_control() const noexcept;
    // 17CE48/CEB0/D05C: priority >> 12 selects a bit. Unsupported keys >=24
    // stop instead of reading outside the original three-byte storage.
    // Filtered nodes expose no callback control and need no callback inputs.
    [[nodiscard]] WorldMapDispatchInput dispatch(uint32_t priority) const noexcept;
private:
    friend class WorldMapSceneAssembly;
    std::shared_ptr<GameClock> clock_;
    WorldMapControlSources sources_;
    WorldMapSceneModeRequestState scene_;
    uint32_t blocked_groups_ = 0;
    std::optional<WorldMapControlWord> control_;
    std::optional<PadSample> pad_sample_;
    HsdPadFilter pad_filter_;
    std::optional<WorldMapPadSnapshot> pad_;
};
} // namespace awl
