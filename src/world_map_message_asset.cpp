#include "awl/world_map_message_asset.h"

#include "awl/filesystem.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace awl {
namespace {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// FUN_800FB540 indexes name pairs at 0x802517E0. Native disc ownership
// replaces named-cache acquisition; this is not the original cache lifetime.
constexpr const char* bank_paths[] = {
    "/files/badog.mes", "/files/bahn.mes", "/files/boy.mes", "/files/carter.mes",
    "/files/daryl.mes", "/files/david.mes", "/files/ebony.mes", "/files/flat.mes",
    "/files/flora.mes", "/files/garfun.mes", "/files/ghali.mes", "/files/godey.mes",
    "/files/gurat.mes", "/files/gustafa.mes", "/files/hugh.mes", "/files/kate.mes",
    "/files/kesaran.mes", "/files/kris.mes", "/files/lumina.mes", "/files/mash.mes",
    "/files/moi.mes", "/files/mukumuku.mes", "/files/muumuu.mes", "/files/nami.mes",
    "/files/nina.mes", "/files/pasaran.mes", "/files/rock.mes", "/files/romana.mes",
    "/files/rou.mes", "/files/san.mes", "/files/sebastian.mes", "/files/sepilia.mes",
    "/files/son.mes", "/files/suary.mes", "/files/tay.mes", "/files/takakura.mes",
    "/files/tsurutan.mes", "/files/vesta.mes", "/files/sepilia_wife.mes", "/files/muumuu_wife.mes",
    "/files/nami_wife.mes", "/files/console.mes", "/files/other.mes", "/files/son_appeal.mes",
    "/files/son_talk.mes", "/files/son_baby.mes", "/files/son_diary.mes", "/files/son_npccom.mes",
    "/files/son_other.mes", "/files/event.mes", "/files/debug.mes", "/files/system.mes",
    "/files/minigame.mes", "/files/tv.mes", "/files/tv_vd0.mes", "/files/tv_vd1.mes",
    "/files/tv_vd2.mes", "/files/tv_vd3.mes", "/files/tv_vd4.mes", "/files/itemdoc.mes",
    "/files/itemdoc01.mes", "/files/itemdoc02.mes", "/files/itemdoc03.mes", "/files/line.mes",
};
static_assert(sizeof(bank_paths) / sizeof(bank_paths[0]) == 64);

} // namespace

bool resolve_world_map_message_key(
    uint32_t bank_word, uint32_t index_word, WorldMapMessageKey* out) {
    if (out == nullptr) return false;
    WorldMapMessageKey key{static_cast<uint8_t>(bank_word), index_word};
    if (key.bank >= 64) return false;
    if (key.bank == 59) {
        if (key.index > 1004) {
            key.bank = 62;
            key.index -= 1005;
        } else if (key.index > 669) {
            key.bank = 61;
            key.index -= 670;
        } else if (key.index > 334) {
            key.bank = 60;
            key.index -= 335;
        }
    }
    *out = key;
    return true;
}

const char* world_map_message_bank_path(uint32_t bank) {
    return bank < 64 ? bank_paths[bank] : nullptr;
}

void WorldMapMessageBank::clear() {
    bytes_.clear();
    entries_.clear();
}

bool WorldMapMessageBank::parse(std::vector<uint8_t> bytes) {
    clear();
    if (bytes.size() < 12 || bytes.size() > std::numeric_limits<uint32_t>::max() ||
        be32(bytes.data()) != 0xcdc3b0b0u) return false;
    const uint32_t count = be32(bytes.data() + 4);
    const uint64_t table_end = 8ull + static_cast<uint64_t>(count) * 4;
    if (count == 0 || table_end >= bytes.size()) return false;
    std::vector<uint32_t> offsets(count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t offset = be32(bytes.data() + 8 + i * 4);
        if (offset < table_end || offset >= bytes.size() || (offset & 3u) != 0) return false;
        offsets[i] = offset;
    }
    auto sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<WorldMapMessageBounds> entries(count);
    for (size_t i = 0; i < count; ++i) {
        const auto next = std::upper_bound(sorted.begin(), sorted.end(), offsets[i]);
        const size_t end = next == sorted.end() ? bytes.size() : *next;
        entries[i] = {offsets[i], end - offsets[i]};
    }
    bytes_.swap(bytes);
    entries_.swap(entries);
    return true;
}

bool WorldMapMessageBank::load(uint32_t resolved_bank) {
    clear();
    const char* path = world_map_message_bank_path(resolved_bank);
    if (path == nullptr) return false;
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file(path, &raw, &size)) return false;
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(raw, &filesystem_free_file_data);
    const auto* first = static_cast<const uint8_t*>(raw);
    return first != nullptr && parse(std::vector<uint8_t>(first, first + size));
}

bool WorldMapMessageBank::entry_bounds(uint32_t index, WorldMapMessageBounds* out) const {
    if (out == nullptr || index >= entries_.size()) return false;
    *out = entries_[index];
    return true;
}

bool WorldMapMessageBank::copy_entry(uint32_t index, std::vector<uint8_t>* out) const {
    if (out == nullptr || index >= entries_.size()) return false;
    const auto& entry = entries_[index];
    std::vector<uint8_t> copied(bytes_.data() + entry.offset, bytes_.data() + entry.offset + entry.size);
    out->swap(copied);
    return true;
}

WorldMapMessageStreamStatus WorldMapMessageBank::scan_entry(
    uint32_t index, WorldMapMessageStream* out) const {
    if (out == nullptr || index >= entries_.size()) return WorldMapMessageStreamStatus::InvalidInput;
    const auto& entry = entries_[index];
    return scan_world_map_message_stream(bytes_.data() + entry.offset, entry.size, out);
}

WorldMapSelectionRowsStatus WorldMapMessageBank::prepare_selection_rows(
    uint32_t index, uint32_t row_count, WorldMapSelectionRows* out) const {
    if (out == nullptr || index >= entries_.size()) return WorldMapSelectionRowsStatus::InvalidInput;
    const auto& entry = entries_[index];
    return prepare_world_map_selection_rows(bytes_.data() + entry.offset, entry.size, row_count, out);
}

} // namespace awl
