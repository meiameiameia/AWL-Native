#include "awl/world_map_player_model_auxiliary.h"
#include "world_map_flat_archive.h"

#include <new>
#include <utility>

namespace awl {
namespace {
bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return as != 0 && bs != 0 && a < b + bs && b < a + as;
}
} // namespace
WorldMapPlayerModelAuxiliaryStatus prepare_world_map_player_model_auxiliary(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,
    std::unique_ptr<WorldMapPlayerModelAuxiliaryMetadata>* out) {
    using Status = WorldMapPlayerModelAuxiliaryStatus;
    if (!out) return Status::InvalidInput;
    if (!assets) return Status::RequiresAssets;
    try {
        auto metadata = std::unique_ptr<WorldMapPlayerModelAuxiliaryMetadata>(new WorldMapPlayerModelAuxiliaryMetadata);
        metadata->assets_ = assets;
        WorldMapPlayerModelAssetView gpl, skin;
        if (!assets->resource(2, &gpl) || !assets->resource(3, &skin)) return Status::UnsupportedLayout;
        const std::vector<uint8_t> bytes(gpl.data,gpl.data + gpl.size);
        const auto gpl_status = prepare_world_map_gpl_metadata(bytes,WorldMapGplSkinPolicy::RetainMetadata,&metadata->gpl_);
        if (gpl_status == WorldMapGplMetadataStatus::AllocationFailure) return Status::AllocationFailure;
        if (gpl_status != WorldMapGplMetadataStatus::Prepared) return Status::UnsupportedLayout;
        for (size_t i = 0; i < metadata->gpl_.sections.size(); ++i) {
            const auto& position = metadata->gpl_.sections[i].position;
            if (!position || position->component_count != 6) continue;
            metadata->buffer_size_ = (uint32_t(position->count)*12u + 31u) & ~31u;
            if (position->data_offset) {
                // A later skin write would use this complete allocated extent.
                // Require the selected raw output region to be within its GPL
                // section, excluding trailing names and following sections.
                uint32_t end = gpl.size;
                for (const auto& section : metadata->gpl_.sections) {
                    if (section.name_offset < end) end = section.name_offset;
                    if (section.offset > metadata->gpl_.sections[i].offset && section.offset < end) end = section.offset;
                }
                if (!detail::archive_range(end,*position->data_offset,metadata->buffer_size_)) return Status::UnsupportedLayout;
                const auto& section = metadata->gpl_.sections[i];
                const uint32_t data = *position->data_offset, size = metadata->buffer_size_;
                if (overlaps(data,size,section.offset,section.sub_offsets[2] != 0 ? 21u : 20u)) return Status::UnsupportedLayout;
                for (size_t header = 0; header < section.sub_offsets.size(); ++header) {
                    if (section.sub_offsets[header] == 0) continue;
                    const uint32_t length = header == 4 ? 12u : header == 2 ? uint32_t(bytes[section.offset + 20])*16u : 8u;
                    if (overlaps(data,size,section.sub_offsets[header],length)) return Status::UnsupportedLayout;
                }
                const uint32_t commands = detail::archive_be32(bytes.data() + section.material_offset + 4);
                if (overlaps(data,size,uint64_t(section.offset) + commands,uint64_t(section.commands.size())*16))
                    return Status::UnsupportedLayout;
                metadata->target_ = WorldMapPlayerSkinTarget{static_cast<uint32_t>(i),
                    {gpl.reference.bank_identity,gpl.reference.offset + *position->data_offset},metadata->buffer_size_};
            }
            break;
        }
        const auto skin_status = prepare_world_map_skin_metadata(skin.data,skin.size,
            metadata->target_ ? std::optional<uint32_t>(metadata->buffer_size_) : std::nullopt,&metadata->skin_);
        if (skin_status == WorldMapSkinMetadataStatus::AllocationFailure) return Status::AllocationFailure;
        if (skin_status != WorldMapSkinMetadataStatus::Prepared) return Status::UnsupportedLayout;
        *out = std::move(metadata); return Status::Prepared;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
