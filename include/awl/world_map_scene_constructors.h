#pragma once
#include "awl/world_map_camera.h"
#include "awl/world_map_collision_assets.h"
#include "awl/world_map_collision_registry.h"
#include "awl/game_rng.h"
#include <memory>

namespace awl {
// Consumed fields of saved 14D58/14DEC records. Restoration and later writers
// populate these inputs; absence is not a constructor-default observation.
struct WorldMapOptionalActorSaved {
    int32_t scene_type=1,action=0;
    std::array<float,3> position{},heading{0,0,1};
    uint32_t wake_minutes=0,sleep_minutes=0;
    uint8_t special_14e6c=0;
};
struct WorldMapOptionalActorProjection {
    WorldMapSceneObject scene;
    WorldMapRegisteredCollisionObject collision;
};
// Native projection of D250 ->885BC/88AC0 and F7A4 construction. These are
// current saved/secondary inputs, not completed constructor observations or a
// save-file parser. Later producers must update this same retained owner.
struct WorldMapSceneConstructorState {
    WorldMapSceneConstructorState() noexcept {fixed.selected_variant.fill(3);}
    WorldMapFixedCollisionState fixed;
    int32_t actor_a_29a64=-1,actor_b_29a60=-1,actor_3f8=0x2c;
    uint8_t byte_299aa=0,byte_299b0=0,secondary_49=0,secondary_3f4=0;
    std::optional<WorldMapOptionalActorSaved> optional_a,optional_b;
    std::optional<int32_t> held_constructor_11cc8;
    // Retain the actual shared process owner. Never reset it at scene entry.
    std::shared_ptr<GameRng> rng;
};
struct WorldMapSceneEntryRequest {
    bool requested=false;
    int32_t kind=0,from=0,to=0;
};
// 1456C..14600, including decimal modes 28/29/30 (hex 1C/1D/1E).
[[nodiscard]] WorldMapSceneEntryRequest world_map_scene_entry_request(const WorldMapSceneModeRequestState&) noexcept;
// Complete 16C800, including unsigned hour offsets and inclusive bounds.
[[nodiscard]] bool world_map_optional_actor_b_awake(uint32_t phase_raw,
    uint32_t wake_minutes,uint32_t sleep_minutes) noexcept;
struct WorldMapSceneConstructorProjection {
    std::array<WorldMapSceneObject,5> spatial;
    WorldMapPlayerContextMetadata mandatory_context;
    std::array<float,3> mandatory_heading{};
    WorldMapCameraViewQuery camera;
    std::optional<uint32_t> rng_after;
    uint8_t secondary_3f4=0;
};
enum class WorldMapSceneConstructorStatus {
    Loaded,Decoded,Prepared,InvalidInput,ReadFailure,WrongDol,UnsupportedLayout,
    RequiresOptionalConstructor,RequiresHeadingRng,TerrainIncomplete,UnsupportedNumerics,AllocationFailure,
    Absent,RequiresOptionalFields,RequiresOptionalAction,RequiresRngLoop,UnsupportedSceneType
};
// Immutable consumed data from the verified DOL, including the static source
// descriptors used to initialize the four mode-zero dispatch rows. No raw DOL
// bytes or Ghidra output are retained. Full actor models/AI are not constructed.
class WorldMapSceneConstructorTables {
public:
    bool target_verified() const noexcept {return verified_;}
    [[nodiscard]] static WorldMapSceneConstructorStatus decode(const uint8_t*,size_t,
        std::shared_ptr<const WorldMapSceneConstructorTables>*);
    [[nodiscard]] static WorldMapSceneConstructorStatus load(std::shared_ptr<const WorldMapSceneConstructorTables>*);
    [[nodiscard]] WorldMapSceneConstructorStatus prepare(const WorldMapCollisionAssets&,
        const WorldMapSceneConstructorState&,const std::array<uint64_t,5>& spatial_keys,
        WorldMapSceneConstructorProjection*) const noexcept;
    const WorldMapCameraViewQuery& camera() const noexcept {return camera_;}
    // Consumed collision/context projection of 15DFF8 / 166988. Supported
    // no-action tails only; selected action callbacks stop. No models/AI run.
    [[nodiscard]] WorldMapSceneConstructorStatus prepare_optional(bool actor_b,
        const WorldMapCollisionAssets&,const WorldMapSceneConstructorState&,
        uint32_t phase_raw,uint64_t key,WorldMapOptionalActorProjection*) const noexcept;
    // Assembly uses these separately at the two post-player construction
    // points. The combined adapter above is only an isolated preparation API.
    [[nodiscard]] WorldMapSceneConstructorStatus prepare_spatial(const WorldMapCollisionAssets&,
        const WorldMapSceneConstructorState&,const std::array<uint64_t,5>&,
        WorldMapSceneConstructorProjection*) const noexcept;
    [[nodiscard]] WorldMapSceneConstructorStatus prepare_mandatory(const WorldMapSceneConstructorState&,
        WorldMapSceneConstructorProjection*) const noexcept;
private:
    WorldMapSceneConstructorTables()=default;
    bool decode_fields(const uint8_t*,size_t) noexcept;
    std::array<std::array<float,3>,4> positions_{},headings_{};
    std::array<float,5> thresholds_{},ranges_{};
    std::array<float,2> optional_thresholds_{};
    std::array<float,3> zero_{},forward_{};
    WorldMapCameraViewQuery camera_;
    float radians_per_degree_=0;
    int32_t rng_min_=0,rng_max_=0;
    bool verified_=false;
};
} // namespace awl
