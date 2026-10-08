#pragma once
#include "awl/world_map_animation_initializer.h"
#include <array>
#include <memory>

namespace awl {
struct WorldMapPlayerInitialAnimationQuery {
    uint32_t phase = 0;
    uint32_t model_slot = 0;
    uint32_t alternate = UINT32_MAX; // B8F8's signed -1 branch.
};
struct WorldMapPlayerInitialAnimationSelection {
    uint32_t phase = 0;
    uint16_t descriptor_index = 0;
    WorldMapActorAnimationDescriptor descriptor;
    uint32_t type_0 = 0, type_4 = 0;
    uint32_t feature_source_10 = 0;
    uint8_t feature_flag_34 = 0;
    float scale = 1; // 8002E47C's phase-selected catalog row +4.
};
enum class WorldMapPlayerInitialAnimationStatus {
    Decoded, Loaded, Selected, Prepared, RequiresInputs, RequiresPhase,
    UnsupportedModelSlot, UnsupportedAlternate, RequiresTimerTable,
    RequiresTimerRow, RequiresClock, InvalidInput, UnsupportedLayout,
    ReadFailure, WrongDol, AllocationFailure
};
struct WorldMapPlayerInitialAnimationStep {
    WorldMapPlayerInitialAnimationStatus status = WorldMapPlayerInitialAnimationStatus::InvalidInput;
    std::optional<WorldMapPlayerInitialAnimationSelection> selection;
    // Exposed only after BOTH constructor timers have verified reset writes.
    std::optional<WorldMapAnimationFeature38> feature;
    std::optional<WorldMapAnimationFeatureRow> required_row;
};
// Immutable slot-zero initial descriptor, phase-keyed model types and scale.
// Owns static words only; phase, runtime timer tables/clock and live actor
// ownership remain separate. Does not decode other models/alternate branches.
class WorldMapPlayerInitialAnimationInputs {
public:
    WorldMapPlayerInitialAnimationInputs(const WorldMapPlayerInitialAnimationInputs&) = delete;
    WorldMapPlayerInitialAnimationInputs& operator=(const WorldMapPlayerInitialAnimationInputs&) = delete;
    bool target_verified() const { return target_verified_; }
    [[nodiscard]] WorldMapPlayerInitialAnimationStep select(const WorldMapPlayerInitialAnimationQuery& query) const;
private:
    WorldMapPlayerInitialAnimationInputs() = default;
    bool decode(const uint8_t* data, size_t size);
    friend WorldMapPlayerInitialAnimationStatus decode_world_map_player_initial_animation_inputs(
        const uint8_t*, size_t, std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>*);
    friend WorldMapPlayerInitialAnimationStatus load_world_map_player_initial_animation_inputs(
        std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>*);
    WorldMapActorAnimationDescriptor descriptor_;
    uint16_t descriptor_index_ = 0;
    std::array<std::array<uint32_t,2>,6> types_{};
    std::array<float,6> scales_{};
    bool target_verified_ = false;
};
// Synthetic decoder validates all reached extents and both changed timer
// types. Below-threshold/negative rows are unsupported: the complete feature
// adapter cannot invent their constructor-unwritten timers. Stops preserve out.
[[nodiscard]] WorldMapPlayerInitialAnimationStatus decode_world_map_player_initial_animation_inputs(
    const uint8_t* data, size_t size, std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>* out);
// Verifies SHA1 of the exact mounted /sys/main.dol bytes before decoding.
// Retains no raw DOL. Only a verified owner may skip I/O; failures preserve out.
[[nodiscard]] WorldMapPlayerInitialAnimationStatus load_world_map_player_initial_animation_inputs(
    std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>* out);
// F39C -> F3E8: types 13/F, index -1, table null, source slot zero and flag
// zero precede the phase-selected type changes. Reached timer rows/clock are
// supplied, never inferred. Stops expose no placeholder or partial feature.
[[nodiscard]] WorldMapPlayerInitialAnimationStep prepare_world_map_player_initial_animation(
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const WorldMapPlayerInitialAnimationQuery& query,
    const WorldMapAnimationInitializerObservations& observations);
// B8F8 -> E7BC/1FC4: clear only the root flag byte, then write uniform scale
// and flag 1. Other words are retained without interpreting unwritten floats.
// Finite scales (including signed zero/negative) are supported. Stops preserve out.
[[nodiscard]] WorldMapPlayerInitialAnimationStatus prepare_world_map_player_initial_root_pose(
    const WorldMapAnimationPose& before, float scale, WorldMapAnimationPose* out);
} // namespace awl
