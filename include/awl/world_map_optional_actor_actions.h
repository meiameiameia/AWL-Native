#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>

namespace awl {
enum class WorldMapOptionalActionKind { AExplicit,BExplicit,BTable };
enum class WorldMapOptionalModelOperation { Appearance,Animation,SetFlag15c,BindSecondary };
struct WorldMapOptionalModelCommand {
    WorldMapOptionalModelOperation operation=WorldMapOptionalModelOperation::Appearance;
    uint32_t index=0,variant=0;
    float rate=0;
};
struct WorldMapOptionalActionState {
    int32_t current=0,previous=0;
    uint32_t deadline=0;
    uint8_t restart_latch=0;
};
// Consumed source descriptors, initial duration rows and scalar operands.
// The mounted scene provider owns/validates them; invented fixtures may supply
// this value directly without claiming target identity.
struct WorldMapOptionalActionTables {
    std::array<uint32_t,34> a_initializers{};
    std::array<uint32_t,42> b_initializers{};
    std::array<std::array<uint32_t,2>,42> b_durations{};
    std::array<bool,42> b_duration_known{};
    std::array<float,5> speeds{},model_scales{}; // IDs 27..2B
    std::array<float,2> rate_divisors{};
    std::array<float,3> night_position{},forward{};
    float night_threshold=0;
};
struct WorldMapOptionalActionQuery {
    WorldMapOptionalActionKind kind=WorldMapOptionalActionKind::AExplicit;
    int32_t actor_id=0,requested=0;
    WorldMapOptionalActionState before;
    uint32_t minimum=0,maximum=0;
    std::optional<uint32_t> phase_raw,rng_before;
    std::optional<int32_t> held_scene_type;
    std::optional<std::array<float,3>> held_position;
};
struct WorldMapOptionalActionPlan {
    WorldMapOptionalActionKind kind=WorldMapOptionalActionKind::AExplicit;
    int32_t actor_id=0;
    uint64_t actor_identity=0; // bound by the constructor projection
    WorldMapOptionalActionState after;
    std::optional<uint32_t> rng_after;
    bool initialized=false,collision_enabled=true,bind_secondary_model=false;
    // True requests the indicated write; false leaves the saved field alone.
    bool record_enabled=false,special_flag_one=false,clear_accumulator=false;
    std::optional<int32_t> internal_scene_type;
    std::optional<std::array<float,3>> position,heading;
    std::optional<std::array<float,3>> record_position; // Internal record only; no 107A4/10830 scene write.
    std::optional<float> facing_threshold;
    std::optional<float> requested_speed;
    std::optional<int32_t> heading_degrees;
    std::array<WorldMapOptionalModelCommand,3> model_commands{};
    uint32_t model_command_count=0;
    // Commands are retained pending model/resource execution. Preparing these
    // CPU fields never certifies that model setup or actor AI has run.
    bool model_work_pending() const noexcept {return model_command_count!=0;}
};
enum class WorldMapOptionalActionStatus {
    Prepared,InvalidInput,RequiresPhase,RequiresRng,RequiresHeldType,
    UnsupportedAction,UnsupportedInitializer,UnsupportedNumerics,RequiresHeldPosition
};
// 15F5B8 /1680C0 /16816C plus the supported 162918/162AD0 and
// 162B10/16312C and 168BB0/169E7C/16B834 initialization effects. Atomic, allocation-free;
// model commands remain pending. Requires nearest rounding for reached math.
[[nodiscard]] WorldMapOptionalActionStatus prepare_world_map_optional_actor_action(
    const WorldMapOptionalActionTables&,const WorldMapOptionalActionQuery&,
    WorldMapOptionalActionPlan*) noexcept;
struct WorldMapOptionalBWeightedRow {
    uint32_t count=0;
    std::array<uint8_t,12> weights{};
    std::array<std::array<uint32_t,3>,12> actions{}; // action/min/max, not durations read here
};
struct WorldMapOptionalBWeightedSelection {
    uint32_t action=0,minimum=0,maximum=0,rng_after=0,index=0;
};
// Complete 16D08C, used by CDBC/CF2C/CFDC (11/10/12 entries).
// Current weights, action triples and r13-60A4 remain explicit inputs. Draws
// even for a constant choice; strict signed cumulative comparison and final
// fallback are preserved. This does not execute the chosen initializer.
[[nodiscard]] bool prepare_world_map_optional_b_weighted_selection(
    const WorldMapOptionalBWeightedRow&,uint32_t upper,uint32_t rng_before,
    WorldMapOptionalBWeightedSelection*) noexcept;
} // namespace awl
