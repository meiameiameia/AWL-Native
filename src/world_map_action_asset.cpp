#include "awl/world_map_action_asset.h"

#include "awl/filesystem.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace awl {
namespace {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool in_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

} // namespace

bool decode_world_map_action_clz(
    const uint8_t* data, size_t size, size_t output_limit,
    std::vector<uint8_t>* out) {
    if (out == nullptr || data == nullptr || size < 16 ||
        std::memcmp(data, "CLZ\0", 4) != 0) {
        return false;
    }
    const uint32_t total = be32(data + 4);
    // The target entries have one block, zero stride, and a block output
    // count equal to the total. Other block layouts are not yet supported.
    if (total == 0 || total > output_limit || be32(data + 8) != 0 ||
        be32(data + 12) != total) {
        return false;
    }
    std::vector<uint8_t> decoded;
    decoded.reserve(total);
    size_t cursor = 16;
    uint32_t flags = 0;
    while (decoded.size() < total) {
        // FUN_80178450 consumes flag bits from low to high, with one
        // meaning a match and zero a literal. 0xFF00 tracks eight tokens.
        flags >>= 1;
        if ((flags & 0xff00u) == 0) {
            if (cursor == size) {
                return false;
            }
            flags = static_cast<uint32_t>(data[cursor++]) | 0xff00u;
        }
        if ((flags & 1u) == 0) {
            if (cursor == size) {
                return false;
            }
            decoded.push_back(data[cursor++]);
        } else {
            if (!in_range(size, cursor, 2)) {
                return false;
            }
            const uint32_t low = data[cursor++];
            const uint32_t high = data[cursor++];
            const size_t distance = 4096u - (low | ((high & 0xf0u) << 4));
            const size_t length = (high & 0x0fu) + 3u;
            if (distance > decoded.size() || length > total - decoded.size()) {
                return false;
            }
            // Byte-at-a-time copying preserves overlapping matches.
            for (size_t i = 0; i < length; ++i) {
                decoded.push_back(decoded[decoded.size() - distance]);
            }
        }
    }
    if (cursor != size) {
        return false;
    }
    out->swap(decoded);
    return true;
}

void WorldMapGlobalActionArchive::clear() {
    entries_.clear();
    bytes_.clear();
}

bool WorldMapGlobalActionArchive::parse(std::vector<uint8_t> bytes) {
    clear();
    if (bytes.size() < 0x20 || be32(bytes.data()) != 0x55aa382du) {
        return false;
    }
    const uint32_t nodes = be32(bytes.data() + 4);
    const uint32_t metadata_size = be32(bytes.data() + 8);
    const uint32_t data_start = be32(bytes.data() + 12);
    if (nodes < 0x20 || !in_range(bytes.size(), nodes, metadata_size) ||
        static_cast<uint64_t>(nodes) + metadata_size > data_start ||
        !in_range(bytes.size(), nodes, 12)) {
        return false;
    }
    const uint32_t count = be32(bytes.data() + nodes + 8);
    const uint64_t names = static_cast<uint64_t>(nodes) +
                           static_cast<uint64_t>(count) * 12;
    const uint64_t names_end = static_cast<uint64_t>(nodes) + metadata_size;
    if (count < 2 || names >= names_end || data_start > bytes.size() ||
        !in_range(bytes.size(), nodes, static_cast<uint64_t>(count) * 12) ||
        be32(bytes.data() + nodes) != 0x01000000u ||
        be32(bytes.data() + nodes + 4) != 0) {
        return false;
    }
    std::vector<Entry> entries(count);
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    for (uint32_t index = 1; index < count; ++index) {
        const size_t node = nodes + static_cast<size_t>(index) * 12;
        const uint32_t tag = be32(bytes.data() + node);
        const uint32_t offset = be32(bytes.data() + node + 4);
        const uint32_t size = be32(bytes.data() + node + 8);
        const uint64_t name = names + (tag & 0x00ffffffu);
        // Common.arc is flat. Reject directories and unknown node types.
        if ((tag >> 24) != 0 || name >= names_end || offset < data_start ||
            size == 0 || !in_range(bytes.size(), offset, size)) {
            return false;
        }
        const void* end = std::memchr(bytes.data() + name, 0,
                                     static_cast<size_t>(names_end - name));
        if (end == nullptr || end == bytes.data() + name) {
            return false;
        }
        entries[index] = {offset, size};
        extents.emplace_back(offset, static_cast<uint64_t>(offset) + size);
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i) {
        if (extents[i].first < extents[i - 1].second) {
            return false;
        }
    }
    bytes_.swap(bytes);
    entries_.swap(entries);
    return true;
}

bool WorldMapGlobalActionArchive::load() {
    clear();
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file("/files/Common.arc", &raw, &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    const auto* first = static_cast<const uint8_t*>(raw);
    return first != nullptr && parse(std::vector<uint8_t>(first, first + size));
}

bool WorldMapGlobalActionArchive::decode_prepared_request(
    const WorldMapMovementRequestPreparation& request,
    size_t output_limit, std::vector<uint8_t>* out) const {
    if (!loaded() || !request.use_global_action_list ||
        request.action_index < 300u || request.action_index >= 0x3ffu ||
        request.decoder_flag > 1 || request.action_list_index <= 0 ||
        request.action_list_index != static_cast<int32_t>(request.action_index - 300u) ||
        static_cast<size_t>(request.action_list_index) >= entries_.size()) {
        return false;
    }
    const Entry& entry = entries_[static_cast<size_t>(request.action_list_index)];
    return decode_world_map_action_clz(bytes_.data() + entry.offset,
                                      entry.size, output_limit, out);
}

} // namespace awl
