#include "awl/world_map_skin_metadata.h"
#include "world_map_flat_archive.h"

#include <new>
#include <utility>

namespace awl {
namespace {
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return as != 0 && bs != 0 && a < b + bs && b < a + as;
}
} // namespace
WorldMapSkinMetadataStatus prepare_world_map_skin_metadata(
    const uint8_t* data, size_t size, std::optional<uint32_t> output_bound, WorldMapSkinMetadata* out) {
    using Status = WorldMapSkinMetadataStatus;
    if (!data || !out || size < 32 || size > UINT32_MAX) return Status::InvalidInput;
    if (data[7] == 0xff) return Status::UnsupportedLayout;
    const auto word = [&](uint32_t at) { return detail::archive_be32(data + at); };
    constexpr uint32_t strides[] = {0x40,0x74,0x44};
    try {
        WorldMapSkinMetadata metadata;
        metadata.control_6 = data[6]; metadata.marker_7 = data[7];
        metadata.word_14 = word(0x14); metadata.word_18 = word(0x18);
        metadata.output_buffer_bound = output_bound;
        for (uint32_t i = 0; i < 3; ++i) {
            metadata.counts[i] = be16(data + i*2);
            const uint32_t offset = word(8 + i*4);
            const uint64_t length = uint64_t(metadata.counts[i])*strides[i];
            if (offset == 0) {
                if (length != 0) return Status::InvalidInput;
            } else {
                if (offset < 32 || !detail::archive_range(size, offset, length == 0 ? 1 : length)) return Status::InvalidInput;
                metadata.tables[i] = offset;
            }
            for (uint32_t prior = 0; prior < i; ++prior) {
                if (metadata.tables[prior] && overlaps(offset,length,*metadata.tables[prior],uint64_t(metadata.counts[prior])*strides[prior]))
                    return Status::UnsupportedLayout;
            }
        }
        // Root conditional fixups precede every unconditional record fixup.
        const auto add = [&](uint32_t location, WorldMapSkinReferenceSpace space) {
            const uint32_t target = word(location);
            if (space == WorldMapSkinReferenceSpace::Asset) {
                if (!detail::archive_range(size,target,1)) return false;
            } else if (output_bound && target > *output_bound) return false;
            metadata.relocations.push_back({location,target,space}); return true;
        };
        for (uint32_t i = 0; i < 3; ++i) {
            if (metadata.tables[i] && !add(8 + i*4,WorldMapSkinReferenceSpace::Asset)) return Status::InvalidInput;
        }
        if (metadata.word_18 != 0 && !add(0x14,WorldMapSkinReferenceSpace::OutputBuffer)) return Status::InvalidInput;
        if (word(0x1c) != 0) {
            if (!add(0x1c,WorldMapSkinReferenceSpace::Asset)) return Status::InvalidInput;
            metadata.pointer_1c = word(0x1c);
        }
        constexpr WorldMapSkinReferenceSpace asset = WorldMapSkinReferenceSpace::Asset;
        constexpr WorldMapSkinReferenceSpace buffer = WorldMapSkinReferenceSpace::OutputBuffer;
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t record = 0; record < metadata.counts[i]; ++record) {
                const uint32_t at = *metadata.tables[i] + record*strides[i];
                if (i == 0) {
                    if (!add(at + 0x30,asset) || !add(at + 0x34,buffer)) return Status::InvalidInput;
                } else if (i == 1) {
                    if (!add(at + 0x60,asset) || !add(at + 0x64,asset) || !add(at + 0x68,buffer)) return Status::InvalidInput;
                } else {
                    if (!add(at + 0x30,asset) || !add(at + 0x3c,asset) || !add(at + 0x34,asset) || !add(at + 0x38,buffer))
                        return Status::InvalidInput;
                }
            }
        }
        *out = std::move(metadata); return Status::Prepared;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
