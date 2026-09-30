#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace awl::detail {
struct FlatArchiveEntry { uint32_t offset = 0, size = 0; };
inline uint32_t archive_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline bool archive_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}
// Existing bounded flat-U8 contract shared by animation and model banks.
// Returns file nodes in serialized order, excluding root node zero. Nested
// directories, empty/overlapping extents and malformed names are unsupported.
inline bool parse_flat_archive(const std::vector<uint8_t>& bytes, std::vector<FlatArchiveEntry>* out) {
    if (out == nullptr || bytes.size() < 0x20 || bytes.size() > UINT32_MAX || archive_be32(bytes.data()) != 0x55aa382du) return false;
    const uint32_t nodes = archive_be32(bytes.data() + 4), metadata = archive_be32(bytes.data() + 8);
    const uint32_t data_start = archive_be32(bytes.data() + 12);
    if (nodes < 0x20 || !archive_range(bytes.size(), nodes, metadata) ||
        uint64_t(nodes) + metadata > data_start || data_start > bytes.size() || !archive_range(bytes.size(), nodes, 12)) return false;
    const uint32_t count = archive_be32(bytes.data() + nodes + 8);
    const uint64_t names = uint64_t(nodes) + uint64_t(count) * 12, end = uint64_t(nodes) + metadata;
    if (count < 2 || names >= end || !archive_range(bytes.size(), nodes, uint64_t(count) * 12) ||
        archive_be32(bytes.data() + nodes) != 0x01000000u || archive_be32(bytes.data() + nodes + 4) != 0) return false;
    std::vector<FlatArchiveEntry> entries;
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    for (uint32_t i = 1; i < count; ++i) {
        const auto* node = bytes.data() + nodes + size_t(i) * 12;
        const uint32_t tag = archive_be32(node), offset = archive_be32(node + 4), size = archive_be32(node + 8);
        const uint64_t name = names + (tag & 0xffffffu);
        if ((tag >> 24) != 0 || name >= end || offset < data_start || size == 0 || !archive_range(bytes.size(), offset, size)) return false;
        const auto* first = bytes.data() + name;
        const void* terminator = std::memchr(first, 0, static_cast<size_t>(end - name));
        if (terminator == nullptr || terminator == first) return false;
        entries.push_back({offset, size}); extents.emplace_back(offset, uint64_t(offset) + size);
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i) if (extents[i].first < extents[i - 1].second) return false;
    *out = std::move(entries); return true;
}
} // namespace awl::detail
