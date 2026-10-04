#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {
enum class WorldMapSkinMetadataStatus { Prepared, UnsupportedLayout, InvalidInput, AllocationFailure };
enum class WorldMapSkinReferenceSpace { Asset, OutputBuffer };
struct WorldMapSkinRelocation {
    uint32_t location = 0, target = 0; // Relative to the SKN/file or output base.
    WorldMapSkinReferenceSpace space = WorldMapSkinReferenceSpace::Asset;
};
struct WorldMapSkinMetadata {
    std::array<uint16_t,3> counts{}; // Header +0/+2/+4; strides 40/74/44 hex.
    std::array<std::optional<uint32_t>,3> tables;
    uint8_t control_6 = 0, marker_7 = 0;
    uint32_t word_14 = 0, word_18 = 0; // +14 becomes output-relative only if +18 != 0.
    std::optional<uint32_t> pointer_1c;
    std::optional<uint32_t> output_buffer_bound;
    std::vector<WorldMapSkinRelocation> relocations; // Exact CB9C store order.
};
// CD1C -> CB9C with r4=0, as immutable references rather than PPC pointer
// stores. Zero *record* pointers relocate to the asset/output base, whereas
// zero root pointers remain null. Bounds the three complete record tables
// and reached asset target bytes; header/table overlap rejects. When given,
// output_bound checks offsets only, not execution spans. Matrices, indices,
// weights, count semantics and complete pointed-to payloads remain opaque.
// Marker FF (already relocated) rejects. Does not run the reached C668 locked
// cache/OS setup or a skinning backend. Every failure preserves out.
[[nodiscard]] WorldMapSkinMetadataStatus prepare_world_map_skin_metadata(
    const uint8_t* data, size_t size, std::optional<uint32_t> output_bound, WorldMapSkinMetadata* out);
} // namespace awl
