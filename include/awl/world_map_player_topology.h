#pragma once
#include "awl/world_map_player_draw_commands.h"

namespace awl {
struct WorldMapPlayerVertexReference { uint16_t position=0,normal=0,uv=0; };
struct WorldMapPlayerPrimitive { uint8_t opcode=0; uint32_t first_reference=0,count=0; };
struct WorldMapPlayerTopologyBatch {
    uint32_t section=0,command=0;
    WorldMapModelResourceReference display_list;
    uint32_t display_list_size=0,nop_bytes=0;
    WorldMapPlayerTextureParameters texture;
    uint32_t descriptor_word=0;
    std::vector<WorldMapPlayerVertexReference> references;
    std::vector<WorldMapPlayerPrimitive> primitives;
    std::vector<uint32_t> triangle_indices; // Into this batch's ordered references.
};
struct WorldMapPlayerTopology {
    std::shared_ptr<const WorldMapPlayerModelAssets> assets;
    std::vector<WorldMapPlayerTopologyBatch> batches; // Serialized section/command order.
};
enum class WorldMapPlayerTopologyStatus {
    PreparedTopology,RequiresDescriptor,RequiresTexture,RequiresTev,
    UnsupportedLayout,InvalidInput,AllocationFailure,
};
struct WorldMapPlayerTopologyResult {
    WorldMapPlayerTopologyStatus status=WorldMapPlayerTopologyStatus::InvalidInput;
    uint32_t section=0,command=0; // Reached failing section/command when available.
};
// Complete bounded CPU topology for indexed P/N/Tex0 profiles (8/16-bit,
// VAT0). Preserves GX quads/triangles/strips/fans and NOP padding; rejects
// register/list commands, matrix/direct/NBT/color/multi-UV layouts explicitly.
// Type 1's attached range uses prior texture, then installs its texture.
// Type 2 installs descriptors before its range; type 3 installs TEV before
// its range. Each section must establish its own known prerequisite state.
// Every index is bounded by its retained array; first range agrees with the
// material header. Failures preserve *out and all borrowed drawing state.
// Static enabled-path replay; runtime draw gates and caches are not modeled.
// Retains providers, not a command owner. No attribute decoding, skinned
// output binding, lighting, feature ordering, GPU submission or game acceptance.
[[nodiscard]] WorldMapPlayerTopologyResult prepare_world_map_player_topology(
    const WorldMapPlayerDrawCommands& drawing,WorldMapPlayerTopology* out);
} // namespace awl
