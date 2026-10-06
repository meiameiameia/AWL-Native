#pragma once
#include "awl/world_map_player_topology.h"
#include "awl/world_map_player_skin_work.h"

namespace awl {
struct WorldMapPlayerVertex {
    std::array<float,3> position{},normal{};
    std::array<float,2> uv{};
};
enum class WorldMapPlayerGeometryStatus {
    PreparedGeometry, DecodedVertices, RequiresTopology, UnsupportedLayout,
    InvalidInput, AllocationFailure,
};
struct WorldMapPlayerGeometryResult {
    WorldMapPlayerGeometryStatus status=WorldMapPlayerGeometryStatus::InvalidInput;
    uint32_t section=0,command=0;
    std::optional<WorldMapPlayerTopologyStatus> topology_failure;
};
// Persistent CPU geometry layout and last decoded vertices. Each batch has one
// vertex per ordered topology reference; triangle indices remain batch-local.
// Only signed16 XYZ/normal/ST is supported. Position/UV use their VAT fractions;
// normals use GX's fixed 14-bit fraction, without length normalization.
// The selected interleaved XYZ/normal arrays read retained skin output; other
// arrays read immutable GPL bytes. No root/world transform is applied here.
// No feature scheduling, lighting, GPU objects, drawing or gameplay acceptance.
class WorldMapPlayerGeometry {
public:
    WorldMapPlayerGeometry(const WorldMapPlayerGeometry&)=delete;
    WorldMapPlayerGeometry& operator=(const WorldMapPlayerGeometry&)=delete;
    const WorldMapPlayerTopology& topology() const {return topology_;}
    const std::vector<std::vector<WorldMapPlayerVertex>>& vertices() const {return vertices_;}
    // Same retained asset owner and exact output extent are required. All
    // batches stage before publication; every failure preserves this geometry.
    [[nodiscard]] WorldMapPlayerGeometryResult decode(
        const WorldMapPlayerSkinWork& work,const std::vector<uint8_t>& vertex_output);
private:
    WorldMapPlayerGeometry()=default;
    struct Binding {uint32_t offset=0;uint8_t stride=0,fraction=0;uint16_t count=0;bool skinned=false;};
    friend WorldMapPlayerGeometryResult prepare_world_map_player_geometry(
        const WorldMapPlayerSkinWork&,std::unique_ptr<WorldMapPlayerGeometry>*);
    WorldMapPlayerTopology topology_;
    WorldMapPlayerModelAssetView gpl_;
    std::vector<std::array<Binding,3>> bindings_;
    std::vector<std::vector<WorldMapPlayerVertex>> vertices_;
    uint32_t output_size_=0;
};
// Prepares all topology and bindings once, then decodes initial_output().
// Unsupported layout/topology or allocation failure preserves the prior owner.
[[nodiscard]] WorldMapPlayerGeometryResult prepare_world_map_player_geometry(
    const WorldMapPlayerSkinWork& work,std::unique_ptr<WorldMapPlayerGeometry>* out);
} // namespace awl
