#include "awl/world_map_gpl_metadata.h"
#include "world_map_flat_archive.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapGplMetadataStatus;
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}
} // namespace
WorldMapGplMetadataStatus prepare_world_map_gpl_metadata(
    const std::vector<uint8_t>& bytes, WorldMapGplSkinPolicy policy, WorldMapGplMetadata* out) {
    if ((policy != WorldMapGplSkinPolicy::Reject && policy != WorldMapGplSkinPolicy::RetainMetadata) || out == nullptr || bytes.size() > UINT32_MAX || bytes.size() < 20) return Status::InvalidInput;
    const auto word = [&](uint32_t offset) { return detail::archive_be32(bytes.data() + offset); };
    if (word(0) != 0x005bbc61u || word(4) != 0 || word(8) != 0) return Status::UnsupportedLayout;
    const uint32_t count = word(12), table = word(16);
    if (count == 0 || count > 65535 || table < 20 || table % 4 != 0 ||
        !detail::archive_range(bytes.size(), table, uint64_t(count)*8)) return Status::InvalidInput;
    try {
        const uint64_t table_end = uint64_t(table) + uint64_t(count)*8;
        WorldMapGplMetadata metadata;
        std::vector<uint32_t> starts;
        uint32_t names = static_cast<uint32_t>(bytes.size());
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t offset = word(table + i*8), name = word(table + i*8 + 4);
            if (offset < table_end || offset % 4 != 0 || name < table_end ||
                !detail::archive_range(bytes.size(), offset, 20) || !detail::archive_range(bytes.size(), name, 1)) return Status::InvalidInput;
            const void* end = std::memchr(bytes.data() + name, 0, bytes.size() - name);
            if (end == nullptr || bytes[name] == 0) return Status::InvalidInput;
            starts.push_back(offset); names = (std::min)(names, name);
            WorldMapGplSectionMetadata section; section.offset = offset; section.name_offset = name;
            metadata.sections.push_back(std::move(section));
        }
        std::sort(starts.begin(), starts.end());
        for (size_t i = 0; i < starts.size(); ++i) {
            if (starts[i] >= names || (i != 0 && starts[i] == starts[i-1])) return Status::UnsupportedLayout;
        }
        for (auto& section : metadata.sections) {
            const auto next = std::upper_bound(starts.begin(), starts.end(), section.offset);
            const uint32_t end = next == starts.end() ? names : *next;
            const uint32_t size = end - section.offset;
            if (size < 20) return Status::InvalidInput;
            auto section_word = [&](uint32_t offset) { return word(section.offset + offset); };
            for (uint32_t i = 0; i < 5; ++i) {
                const uint32_t sub = section_word(i*4);
                if (sub != 0 && (sub < 20 || !detail::archive_range(size, sub, 1))) return Status::InvalidInput;
                section.sub_offsets[i] = sub == 0 ? 0 : section.offset + sub;
            }
            const uint32_t position = section_word(0);
            if (position != 0) {
                if (!detail::archive_range(size, position, 8)) return Status::InvalidInput;
                if (policy == WorldMapGplSkinPolicy::Reject && bytes[section.offset + position + 7] == 6 && section_word(position) != 0) return Status::RequiresSkin;
                WorldMapGplPositionMetadata attribute;
                attribute.header_offset = section.offset + position;
                attribute.count = be16(bytes.data() + attribute.header_offset + 4);
                attribute.format = bytes[attribute.header_offset + 6];
                attribute.component_count = bytes[attribute.header_offset + 7];
                section.position = attribute;
            }
            const uint32_t material = section_word(16);
            if (material == 0) return Status::UnsupportedLayout;
            if (!detail::archive_range(size, material, 12)) return Status::InvalidInput;
            section.material_offset = section.offset + material;
            const uint32_t commands = section_word(material + 4);
            const uint16_t command_count = be16(bytes.data() + section.material_offset + 8);
            if (command_count != 0 && (commands < 20 || !detail::archive_range(size, commands, uint64_t(command_count)*16))) return Status::InvalidInput;
            if (command_count != 0 && overlaps(commands,uint64_t(command_count)*16,material,12)) return Status::UnsupportedLayout;
            // Original relocation writes the attribute header/array pointers.
            // Aliasing them with command headers could alter cache filtering.
            std::vector<std::pair<uint32_t,uint32_t>> headers{{material,12}};
            const uint32_t arrays = section_word(8);
            if (arrays != 0 && size < 21) return Status::InvalidInput;
            const uint32_t section_header = arrays != 0 ? 21u : 20u;
            if (overlaps(material,12,0,section_header) ||
                (command_count != 0 && overlaps(commands,uint64_t(command_count)*16,0,section_header))) return Status::UnsupportedLayout;
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t sub = section_word(i*4);
                if (sub == 0) continue;
                const uint32_t length = i == 2 ? uint32_t(bytes[section.offset + 20])*16u : 8u;
                if (!detail::archive_range(size, sub, length)) return Status::InvalidInput;
                if (length == 0) continue;
                if (overlaps(sub,length,0,section_header) ||
                    (command_count != 0 && overlaps(sub,length,commands,uint64_t(command_count)*16))) return Status::UnsupportedLayout;
                for (const auto& header : headers) if (overlaps(sub,length,header.first,header.second)) return Status::UnsupportedLayout;
                headers.emplace_back(sub,length);
            }
            if (section.position) {
                const uint32_t data = section_word(position);
                if (data != 0) {
                    if (!detail::archive_range(size, data, 1)) return Status::InvalidInput;
                    section.position->data_offset = section.offset + data;
                }
            }
            for (uint32_t i = 0; i < command_count; ++i) {
                const auto* header = bytes.data() + section.offset + commands + i*16;
                if (header[0] < 1 || header[0] > 3) return Status::UnsupportedLayout;
                section.commands.push_back({header[0],header[1]});
            }
        }
        *out = std::move(metadata); return Status::Prepared;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
