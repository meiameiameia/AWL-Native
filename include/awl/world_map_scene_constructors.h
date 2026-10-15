#pragma once
#include "awl/world_map_camera.h"
#include "awl/world_map_collision_assets.h"
#include "awl/world_map_collision_registry.h"
#include <memory>

namespace awl {
// Native projection of D250 ->885BC/88AC0 and F7A4 construction. These are
// current saved/secondary inputs, not completed constructor observations or a
// save-file parser. Later producers must update this same retained owner.
struct WorldMapSceneConstructorState {
    WorldMapSceneConstructorState() noexcept {fixed.selected_variant.fill(3);}
    WorldMapFixedCollisionState fixed;
    int32_t actor_a_29a64=-1,actor_b_29a60=-1,actor_3f8=0x2c;
    uint8_t byte_299aa=0,byte_299b0=0,secondary_49=0,secondary_3f4=0;
    // Current 17D3E8 seed at the reached mandatory heading boundary. No seed
    // is inferred across untranslated earlier constructor/resource callbacks.
    std::optional<uint32_t> heading_rng_seed;
};
struct WorldMapSceneEntryRequest {
    bool requested=false;
    int32_t kind=0,from=0,to=0;
};
// 1456C..14600, including decimal modes 28/29/30 (hex 1C/1D/1E).
[[nodiscard]] WorldMapSceneEntryRequest world_map_scene_entry_request(const WorldMapSceneModeRequestState&) noexcept;
// Complete 17D3E8: two wrapping LCG steps, one combined output word.
[[nodiscard]] uint32_t advance_world_map_scene_rng(uint32_t* state) noexcept;
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
    RequiresOptionalConstructor,RequiresHeadingRng,TerrainIncomplete,UnsupportedNumerics,AllocationFailure
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
    std::array<float,3> zero_{},forward_{};
    WorldMapCameraViewQuery camera_;
    float radians_per_degree_=0;
    int32_t rng_min_=0,rng_max_=0;
    bool verified_=false;
};
} // namespace awl
