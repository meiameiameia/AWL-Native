#pragma once
#include "awl/world_map_optional_actor_actions.h"
#include "awl/world_map_player_animation_holder.h"

namespace awl {
enum class WorldMapOptionalActorModelStatus {
    Constructed,Loaded,InvalidInput,RequiresTables,RequiresClock,ReadFailure,
    WrongDol,UnsupportedLayout,UnsupportedCommand,ModelIncomplete,AnimationIncomplete,
    RequiresTimer,AllocationFailure
};
struct WorldMapOptionalActorModelResult {
    WorldMapOptionalActorModelStatus status=WorldMapOptionalActorModelStatus::InvalidInput;
    std::optional<WorldMapPlayerAnimationHolderResult> holder;
    std::optional<WorldMapAnimationInitializerStatus> initializer;
    uint32_t command=0;
};
// Only reached initial/selected first descriptors and appearance rows. DOL addresses
// are symbolic descriptor keys; no raw DOL or native pointer is serialized.
class WorldMapOptionalActorModelTables {
public:
    [[nodiscard]] static WorldMapOptionalActorModelStatus load(std::shared_ptr<const WorldMapOptionalActorModelTables>*);
    bool descriptor(bool actor_a,uint32_t clip,uint32_t variant,WorldMapActorAnimationDescriptor*) const noexcept;
    bool appearance(int32_t actor_id,uint32_t choice,std::array<int32_t,2>*) const noexcept;
private:
    WorldMapOptionalActorModelTables()=default;
    std::array<WorldMapActorAnimationDescriptor,6> descriptors_{};
    std::array<std::array<std::array<int32_t,2>,2>,5> appearance_{};
};
// Fresh optional wrapper setup plus ordered constructor action commands.
// Retains actual ACT/GPL/SKN/TPL/animation/TAM providers and native CPU models.
// Original supplied arena becomes independently owned native storage; no PPC
// allocation cursor, shared arena, actor AI or DX11 registration is simulated.
class WorldMapOptionalActorModel {
public:
    [[nodiscard]] static WorldMapOptionalActorModelResult create(
        const std::shared_ptr<const WorldMapOptionalActorModelTables>&,
        const WorldMapOptionalActionPlan&,float model_scale,const std::shared_ptr<GameClock>&,
        std::unique_ptr<WorldMapOptionalActorModel>*);
    const WorldMapPlayerAnimationHolder& holder() const noexcept {return *holder_;}
    int32_t actor_id() const noexcept {return actor_id_;}
    uint64_t actor_identity() const noexcept {return actor_identity_;}
    uint64_t secondary_binding() const noexcept {return secondary_binding_;}
    std::optional<uint8_t> flag_15c() const noexcept {return flag_15c_;}
    uint32_t appearance_choice() const noexcept {return appearance_choice_;}
    const std::array<std::optional<WorldMapAnimationFeatureTimer>,2>& timers() const noexcept {return timers_;}
    const std::array<int32_t,2>& appearance_types() const noexcept {return appearance_types_;}
private:
    WorldMapOptionalActorModel()=default;
    WorldMapOptionalActorModelStatus apply_appearance(const WorldMapOptionalActorModelTables&,uint32_t);
    WorldMapOptionalActorModelResult animate(const WorldMapOptionalActorModelTables&,const WorldMapOptionalModelCommand&);
    int32_t actor_id_=0;
    uint64_t actor_identity_=0,secondary_binding_=0;
    std::optional<uint8_t> flag_15c_; // Wrapper byte is unwritten by the supported fresh setup.
    uint32_t appearance_choice_=0;
    std::array<int32_t,2> appearance_types_{};
    std::array<std::optional<WorldMapAnimationFeatureTimer>,2> timers_{};
    std::shared_ptr<GameClock> clock_;
    std::shared_ptr<const WorldMapPlayerTimerAssets> timer_assets_;
    std::shared_ptr<const WorldMapPlayerModelAssets> assets_;
    std::unique_ptr<WorldMapPlayerAnimationHolder> holder_;
};
} // namespace awl
