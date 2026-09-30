#include "awl/world_map_message_asset.h"
#include "awl/filesystem.h"
#include "awl/memory.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (24 - i * 8));
}

std::vector<uint8_t> fixture() {
    // Invented opaque payloads: indexed A,B,A,C, with A aliased. No game text.
    std::vector<uint8_t> bytes(36, 0);
    put32(bytes, 0, 0xcdc3b0b0u);
    put32(bytes, 4, 4);
    put32(bytes, 8, 24);
    put32(bytes, 12, 28);
    put32(bytes, 16, 24);
    put32(bytes, 20, 32);
    for (size_t i = 24; i < 36; ++i) bytes[i] = static_cast<uint8_t>(i + 90);
    return bytes;
}

void test_keys() {
    struct Case { uint32_t bank, index; uint8_t resolved; uint32_t row; };
    const Case cases[] = {
        {5, 71, 5, 71}, {59, 0, 59, 0}, {59, 334, 59, 334},
        {59, 335, 60, 0}, {59, 669, 60, 334}, {59, 670, 61, 0},
        {59, 1004, 61, 334}, {59, 1005, 62, 0}, {59, 1324, 62, 319},
        {59, 1325, 62, 320}, {59, UINT32_MAX, 62, UINT32_MAX - 1005},
        {60, 335, 60, 335}, {63, 70, 63, 70}, {0x13b, 335, 60, 0},
        {0x100, UINT32_MAX, 0, UINT32_MAX},
    };
    for (const auto& c : cases) {
        awl::WorldMapMessageKey key;
        expect(awl::resolve_world_map_message_key(c.bank, c.index, &key) &&
                   key.bank == c.resolved && key.index == c.row,
               "bank byte conversion and unsigned item remap boundary");
    }
    for (uint32_t invalid : {64u, 255u, UINT32_MAX}) {
        awl::WorldMapMessageKey key{5, 71};
        expect(!awl::resolve_world_map_message_key(invalid, 0, &key) &&
                   key.bank == 5 && key.index == 71, "unsupported bank preserves key");
    }
    expect(!awl::resolve_world_map_message_key(0, 0, nullptr), "missing key output rejected");
    expect(awl::world_map_message_bank_path(64) == nullptr &&
               awl::world_map_message_bank_path(256) == nullptr,
           "resolved path rejects unsupported bank without truncation");
    expect(std::strcmp(awl::world_map_message_bank_path(5), "/files/david.mes") == 0 &&
               std::strcmp(awl::world_map_message_bank_path(59), "/files/itemdoc.mes") == 0 &&
               std::strcmp(awl::world_map_message_bank_path(62), "/files/itemdoc03.mes") == 0 &&
               std::strcmp(awl::world_map_message_bank_path(63), "/files/line.mes") == 0,
           "bounded verified bank catalog routes");
}

void test_container() {
    awl::WorldMapMessageBank bank;
    auto source = fixture();
    expect(bank.parse(source) && bank.loaded() && bank.entry_count() == 4 && bank.byte_size() == 36,
           "complete synthetic index owns opaque bytes");
    source[24] = 0;
    std::vector<uint8_t> copy;
    expect(bank.copy_entry(0, &copy) && copy == std::vector<uint8_t>({114, 115, 116, 117}),
           "owned bytes independent from caller vector");
    copy[0] = 0;
    expect(bank.copy_entry(2, &copy) && copy == std::vector<uint8_t>({114, 115, 116, 117}),
           "alias preserves bytes and copied output cannot mutate bank");
    const uint32_t expected_offsets[] = {24, 28, 24, 32};
    for (uint32_t i = 0; i < 4; ++i) {
        awl::WorldMapMessageBounds bounds;
        expect(bank.entry_bounds(i, &bounds) && bounds.offset == expected_offsets[i] && bounds.size == 4,
               "next distinct physical offset bounds indexed/aliased payload");
    }
    copy = {17};
    awl::WorldMapMessageBounds bounds{71, 99};
    expect(!bank.copy_entry(4, &copy) && copy == std::vector<uint8_t>({17}) &&
               !bank.entry_bounds(UINT32_MAX, &bounds) && bounds.offset == 71 && bounds.size == 99,
           "out-of-range lookups preserve outputs");
    expect(!bank.entry_bounds(0, nullptr) && !bank.copy_entry(0, nullptr), "missing outputs rejected");
    auto reordered = fixture();
    put32(reordered, 8, 32); put32(reordered, 12, 24); put32(reordered, 16, 28);
    expect(bank.parse(reordered) && bank.entry_bounds(0, &bounds) && bounds.offset == 32 && bounds.size == 4 &&
               bank.entry_bounds(1, &bounds) && bounds.offset == 24 && bounds.size == 4,
           "physical bounds do not reorder logical indices");
    auto padding = fixture(); padding.resize(39, 27);
    expect(bank.parse(padding) && bank.entry_bounds(3, &bounds) && bounds.size == 7,
           "last extent includes opaque file tail rather than invented token length");
    auto one = std::vector<uint8_t>(13, 77);
    put32(one, 0, 0xcdc3b0b0u); put32(one, 4, 1); put32(one, 8, 12);
    expect(bank.parse(one) && bank.copy_entry(0, &copy) && copy == std::vector<uint8_t>({77}),
           "minimal nonempty bank does not require invented text terminator");
    for (size_t length = 0; length <= 24; ++length) {
        auto truncated = fixture(); truncated.resize(length);
        expect(bank.parse(fixture()) && !bank.parse(truncated) && !bank.loaded() && bank.entry_count() == 0,
               "truncated headers/index/payload clear previous ownership");
    }
    struct BadWord { size_t at; uint32_t value; };
    for (const BadWord c : {BadWord{0, 0}, {4, 0}, {4, UINT32_MAX}, {8, 20},
                           {8, 25}, {12, 36}, {20, UINT32_MAX}}) {
        auto bad = fixture(); put32(bad, c.at, c.value);
        expect(bank.parse(fixture()) && !bank.parse(bad) && !bank.loaded(),
               "bad marker/count/metadata/alignment/late offsets rejected before access");
    }
    bank.clear();
    copy = {17};
    expect(!bank.copy_entry(0, &copy) && copy == std::vector<uint8_t>({17}), "cleared bank cannot yield stale bytes");
}

bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

void test_native_load() {
    const auto root = std::filesystem::temp_directory_path() /
        ("awl-message-bank-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto files = root / "files";
    // Require a new root before writing or cleaning up any fixture paths.
    const bool created = std::filesystem::create_directory(root);
    expect(created, "create task-owned fixture root");
    if (!created) return;
    expect(std::filesystem::create_directory(files), "create fixture files directory");
    awl_memory_init();
    awl::filesystem_init();
    const auto root_text = root.string();
    expect(awl::filesystem_mount("/", root_text.c_str()), "mount synthetic native fixture root");
    awl::WorldMapMessageBank bank;
    const auto path = files / "david.mes";
    expect(write_file(path, fixture()) && bank.load(5) && bank.entry_count() == 4,
           "native catalog loader owns supported file");
    expect(write_file(path, {1, 2, 3}) && !bank.load(5) && !bank.loaded(), "malformed reload clears prior bank");
    expect(write_file(path, fixture()) && bank.load(5) && !bank.load(64) && !bank.loaded(),
           "unsupported resolved bank clears prior data");
    expect(bank.load(5) && !bank.load(2) && !bank.loaded(), "missing different bank cannot reuse previous bytes");
    expect(bank.load(5) && std::filesystem::remove(path) && !bank.load(5) && !bank.loaded(),
           "missing reload rejects stale ownership");
    awl::filesystem_shutdown();
    expect(!bank.load(5) && !bank.loaded(), "unmounted load fails");
    awl_memory_shutdown();
    // Only the exact task-created file and its now-empty directories are removed.
    std::filesystem::remove(path);
    std::filesystem::remove(files);
    std::filesystem::remove(root);
}

bool check_local(const char* root) {
    awl_memory_init();
    awl::filesystem_init();
    bool valid = awl::filesystem_mount("/", root);
    uint64_t digest = 14695981039346656037ull;
    uint64_t entries = 0;
    auto hash_byte = [&](uint8_t byte) { digest = (digest ^ byte) * 1099511628211ull; };
    auto hash_word = [&](uint32_t word) {
        for (unsigned shift : {24u, 16u, 8u, 0u}) hash_byte(static_cast<uint8_t>(word >> shift));
    };
    awl::WorldMapMessageBank bank;
    for (uint32_t id = 0; valid && id < 64; ++id) {
        valid = bank.load(id);
        if (!valid) break;
        hash_word(id); hash_word(static_cast<uint32_t>(bank.entry_count()));
        hash_word(static_cast<uint32_t>(bank.byte_size()));
        entries += bank.entry_count();
        for (uint32_t i = 0; valid && i < bank.entry_count(); ++i) {
            awl::WorldMapMessageBounds bounds;
            std::vector<uint8_t> bytes;
            valid = bank.entry_bounds(i, &bounds) && bank.copy_entry(i, &bytes) && !bytes.empty();
            if (!valid) break;
            hash_word(i); hash_word(bounds.offset); hash_word(static_cast<uint32_t>(bounds.size));
            for (uint8_t byte : bytes) hash_byte(byte);
        }
        std::printf("Message bank %u: %zu entries, %zu bytes; opaque bounds checked\n",
                    id, bank.entry_count(), bank.byte_size());
    }
    std::printf("Message catalog entries=%llu, digest=%016llx\n",
                static_cast<unsigned long long>(entries), static_cast<unsigned long long>(digest));
    // Independent mapped-DOL catalog / direct-file Python probe supplies these
    // expectations. This checks opaque ownership only, not token semantics.
    valid = valid && entries == 14983 && digest == 0xaa12c3c9f29b06c2ull;
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    return valid;
}

} // namespace

int main(int argc, char** argv) {
    test_keys();
    test_container();
    test_native_load();
    if (argc == 3 && std::strcmp(argv[1], "--messages-local") == 0) {
        expect(check_local(argv[2]), "local complete catalog matches independent metadata/byte digest");
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: world_map_message_asset_tests [--messages-local disc]\n");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
