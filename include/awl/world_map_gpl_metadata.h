#pragma once

#include "awl/world_map_model_feature.h"

#include <array>
#include <optional>
#include <vector>

namespace awl {
enum class WorldMapGplMetadataStatus { Prepared, RequiresSkin, UnsupportedLayout, InvalidInput, AllocationFailure };
enum class WorldMapGplSkinPolicy { Reject, RetainMetadata };
struct WorldMapGplPositionMetadata {
    uint32_t header_offset = 0; // File-relative.
    std::optional<uint32_t> data_offset; // Null serialized data remains null.
    uint16_t count = 0;
    uint8_t format = 0, component_count = 0;
};
struct WorldMapGplSectionMetadata {
    uint32_t offset = 0, name_offset = 0, material_offset = 0;
    std::vector<WorldMapModelFeatureCommand> commands;
    std::array<uint32_t,5> sub_offsets{}; // File-relative, zero remains null.
    std::optional<WorldMapGplPositionMetadata> position;
};
struct WorldMapGplMetadata { std::vector<WorldMapGplSectionMetadata> sections; };
// Shared 3DB8/4078 immutable section/command-header metadata. Only command
// types 1/2/3, distinct sections, trailing names and zero root +4/+8 are
// supported. RetainMetadata admits component-count 6 without executing it;
// Reject preserves the held-item reader's reached nonnull skin stop.
// Bounds section/position/command metadata, not complete array/display-list
// payloads or every relocation reached by 3DB8. No
// command compilation, geometry topology, GX state or drawing is accepted.
[[nodiscard]] WorldMapGplMetadataStatus prepare_world_map_gpl_metadata(
    const std::vector<uint8_t>& bytes, WorldMapGplSkinPolicy policy, WorldMapGplMetadata* out);
} // namespace awl
