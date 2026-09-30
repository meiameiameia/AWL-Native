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

bool same_token(const awl::WorldMapMessageToken& a, const awl::WorldMapMessageToken& b) {
    return a.offset == b.offset && a.tag == b.tag && a.byte_count == b.byte_count &&
        a.visitor_slot == b.visitor_slot && a.terminator == b.terminator;
}

bool same_stream(const awl::WorldMapMessageStream& a, const awl::WorldMapMessageStream& b) {
    if (a.consumed_bytes != b.consumed_bytes || a.tokens.size() != b.tokens.size()) return false;
    for (size_t i = 0; i < a.tokens.size(); ++i) if (!same_token(a.tokens[i], b.tokens[i])) return false;
    return true;
}

bool same_rows(const awl::WorldMapSelectionRows& a, const awl::WorldMapSelectionRows& b) {
    return a.row_count == b.row_count && a.max_width_units == b.max_width_units &&
        a.byte_budget == b.byte_budget && a.aligned_storage_size == b.aligned_storage_size &&
        a.stop_token_offset == b.stop_token_offset && a.consumed_bytes == b.consumed_bytes &&
        a.width == b.width && a.height == b.height && a.bytes == b.bytes;
}

void test_selection_rows() {
    using Status = awl::WorldMapSelectionRowsStatus;
    const uint8_t bytes[] = {0x81, 91, 2, 0x16, 0, 0, 0, 0, 1,
                            0x81, 92, 0x82, 93, 0x83, 94, 0, 0x16};
    awl::WorldMapSelectionRows rows;
    expect(awl::prepare_world_map_selection_rows(bytes, sizeof(bytes), 1, &rows) == Status::Prepared &&
               rows.row_count == 1 && rows.max_width_units == 2 && rows.byte_budget == 10 &&
               rows.aligned_storage_size == 12 && rows.stop_token_offset == 8 && rows.consumed_bytes == 9 &&
               rows.width == 96 && rows.height == 32 &&
               rows.bytes == std::vector<uint8_t>({0x81, 91, 2, 0x16, 0, 0, 0, 0, 0}),
           "first row preserves zero arguments, counts tag 2/8x units, and replaces separator with zero");
    expect(awl::prepare_world_map_selection_rows(bytes, sizeof(bytes), 2, &rows) == Status::Prepared &&
               rows.row_count == 2 && rows.max_width_units == 3 && rows.byte_budget == 17 &&
               rows.aligned_storage_size == 20 && rows.stop_token_offset == 15 && rows.consumed_bytes == 16 &&
               rows.width == 120 && rows.height == 64 && rows.bytes.size() == 16 && rows.bytes.back() == 0 &&
               std::memcmp(rows.bytes.data(), bytes, 16) == 0,
           "complete requested rows update widest row and omit malformed trailing data");
    const auto prior = rows;
    expect(awl::prepare_world_map_selection_rows(bytes, sizeof(bytes), 3, &rows) == Status::InsufficientRows &&
               same_rows(rows, prior), "terminator before requested row count preserves output");
    const uint8_t default_byte[] = {0x90, 0x7f, 0};
    expect(awl::prepare_world_map_selection_rows(default_byte, sizeof(default_byte), 1, &rows) == Status::Prepared &&
               rows.max_width_units == 0 && rows.width == 48 && rows.bytes.size() == 3,
           "default byte family contributes bytes without inventing width units");
    const uint8_t blank[] = {1, 1, 0};
    expect(awl::prepare_world_map_selection_rows(blank, sizeof(blank), 3, &rows) == Status::Prepared &&
               rows.row_count == 3 && rows.max_width_units == 0 && rows.byte_budget == 4 &&
               rows.aligned_storage_size == 4 && rows.width == 48 && rows.height == 96,
           "blank rows and zero terminator retain exact budget/alignment boundary");
    const uint8_t empty_row[] = {0};
    expect(awl::prepare_world_map_selection_rows(empty_row, sizeof(empty_row), 1, &rows) == Status::Prepared &&
               rows.bytes == std::vector<uint8_t>({0}) && rows.byte_budget == 2 && rows.aligned_storage_size == 4,
           "single zero token prepares an empty bounded row");
    const auto empty_prior = rows;
    const uint8_t no_end[] = {0x81, 0};
    const uint8_t partial[] = {0x37, 0, 0, 0};
    expect(awl::prepare_world_map_selection_rows(no_end, sizeof(no_end), 1, &rows) == Status::MissingStopToken &&
               same_rows(rows, empty_prior), "argument zero does not finalize a row");
    expect(awl::prepare_world_map_selection_rows(partial, sizeof(partial), 1, &rows) == Status::TruncatedToken &&
               same_rows(rows, empty_prior), "incomplete control token fails without replacing rows");
    for (uint32_t count : {0u, 0x80000000u, UINT32_MAX}) {
        expect(awl::prepare_world_map_selection_rows(bytes, sizeof(bytes), count, &rows) == Status::InvalidInput &&
                   same_rows(rows, empty_prior), "nonpositive/unsupported signed row budget rejected");
    }
    expect(awl::prepare_world_map_selection_rows(bytes, sizeof(bytes), 0x7fffffffu, &rows) == Status::InsufficientRows &&
               same_rows(rows, empty_prior), "maximum supported count still requires actual rows");
    expect(awl::prepare_world_map_selection_rows(nullptr, 0, 1, &rows) == Status::InvalidInput &&
               awl::prepare_world_map_selection_rows(bytes, SIZE_MAX, 1, &rows) == Status::InvalidInput &&
               same_rows(rows, empty_prior) &&
               awl::prepare_world_map_selection_rows(bytes, sizeof(bytes), 1, nullptr) == Status::InvalidInput,
           "missing pointers and overflow-prone input size fail before access");
    expect(awl::prepare_world_map_selection_rows(bytes, 0, 1, &rows) == Status::MissingStopToken &&
               same_rows(rows, empty_prior), "empty span cannot supply a row boundary");
    auto bank_bytes = fixture();
    bank_bytes[24] = 0x81; bank_bytes[25] = 99; bank_bytes[26] = 1;
    bank_bytes[27] = 0x37; // Unread after the supplied row count is reached.
    awl::WorldMapMessageBank bank;
    expect(bank.parse(bank_bytes) && bank.prepare_selection_rows(0, 1, &rows) == Status::Prepared &&
               rows.bytes == std::vector<uint8_t>({0x81, 99, 0}) && rows.byte_budget == 4 &&
               bank.prepare_selection_rows(2, 1, &rows) == Status::Prepared,
           "bank aliases prepare the same row without scanning malformed subsequent data");
    const auto bank_prior = rows;
    expect(bank.prepare_selection_rows(0, 2, &rows) == Status::TruncatedToken && same_rows(rows, bank_prior) &&
               bank.prepare_selection_rows(4, 1, &rows) == Status::InvalidInput && same_rows(rows, bank_prior) &&
               bank.prepare_selection_rows(0, 1, nullptr) == Status::InvalidInput,
           "bank row preparation respects physical extent and preserves invalid outputs");
    bank.clear();
    expect(bank.prepare_selection_rows(0, 1, &rows) == Status::InvalidInput && same_rows(rows, bank_prior),
           "cleared bank cannot prepare stale rows");
}

void test_message_stream() {
    using Status = awl::WorldMapMessageStreamStatus;
    uint64_t digest = 14695981039346656037ull;
    for (unsigned tag = 0; tag < 256; ++tag) {
        const uint8_t bytes[] = {99, 99, static_cast<uint8_t>(tag), 0, 0, 0, 0, 99};
        awl::WorldMapMessageToken token;
        expect(awl::read_world_map_message_token(bytes, sizeof(bytes), 2, &token) == Status::Decoded &&
                   token.offset == 2 && token.tag == tag,
               "each byte dispatches at a nonzero bounded offset");
        for (uint8_t byte : {token.tag, token.visitor_slot, token.byte_count,
                             static_cast<uint8_t>(token.terminator)}) {
            digest = (digest ^ byte) * 1099511628211ull;
        }
        // The independently pinned dispatch digest below validates sizes. Each
        // incomplete prefix must then fail without replacing an earlier output.
        const auto prior = token;
        for (size_t available = 1; available < token.byte_count; ++available) {
            expect(awl::read_world_map_message_token(bytes, 2 + available, 2, &token) == Status::TruncatedToken &&
                       same_token(token, prior), "every partial multi-byte token preserves output");
        }
    }
    // Mapped DOL jump/member/length tables and literal length instructions,
    // independently walked by Python for all 256 input bytes.
    expect(digest == 0xffca4d95c51515d9ull, "all byte dispatch identities/sizes match independent DOL digest");
    const uint8_t bytes[] = {0x16, 0, 0, 0, 0, 0x50, 0, 0, 0, 0x81, 0, 0x41, 0, 0, 0, 0x7f};
    awl::WorldMapMessageStream stream;
    expect(awl::scan_world_map_message_stream(bytes, sizeof(bytes), &stream) == Status::Decoded &&
               stream.consumed_bytes == 15 && stream.tokens.size() == 5 &&
               stream.tokens[0].visitor_slot == 0x30 && stream.tokens[0].byte_count == 5 &&
               stream.tokens[1].offset == 5 && stream.tokens[1].byte_count == 4 &&
               stream.tokens[2].offset == 9 && stream.tokens[2].visitor_slot == 0xbc &&
               stream.tokens[2].byte_count == 2 && stream.tokens[3].offset == 11 &&
               stream.tokens[3].visitor_slot == 0xa0 && stream.tokens[3].byte_count == 3 &&
               stream.tokens.back().offset == 14 && stream.tokens.back().terminator,
           "zero arguments are not terminators and trailing bytes remain outside the stream");
    const auto prior_stream = stream;
    const uint8_t missing[] = {0x81, 0};
    const uint8_t truncated[] = {0x16, 0, 0, 0};
    expect(awl::scan_world_map_message_stream(missing, sizeof(missing), &stream) == Status::MissingTerminator &&
               same_stream(stream, prior_stream), "argument zero cannot satisfy stream termination");
    expect(awl::scan_world_map_message_stream(truncated, sizeof(truncated), &stream) == Status::TruncatedToken &&
               same_stream(stream, prior_stream), "truncated stream preserves previous full result");
    expect(awl::scan_world_map_message_stream(bytes, 0, &stream) == Status::MissingTerminator &&
               same_stream(stream, prior_stream), "empty supplied span cannot yield a terminated stream");
    expect(awl::scan_world_map_message_stream(nullptr, 0, &stream) == Status::InvalidInput &&
               same_stream(stream, prior_stream) &&
               awl::scan_world_map_message_stream(bytes, sizeof(bytes), nullptr) == Status::InvalidInput,
           "invalid scan input/output rejected atomically");
    awl::WorldMapMessageToken token{71, 23, 17, 45, true};
    const auto prior_token = token;
    expect(awl::read_world_map_message_token(nullptr, 0, 0, &token) == Status::InvalidInput &&
               awl::read_world_map_message_token(bytes, sizeof(bytes), sizeof(bytes), &token) == Status::InvalidInput &&
               awl::read_world_map_message_token(bytes, sizeof(bytes), SIZE_MAX, &token) == Status::InvalidInput &&
               same_token(token, prior_token) &&
               awl::read_world_map_message_token(bytes, sizeof(bytes), 0, nullptr) == Status::InvalidInput,
           "invalid offsets, source, and output rejected before access");
    const uint8_t zero[] = {0, 0x16};
    expect(awl::scan_world_map_message_stream(zero, sizeof(zero), &stream) == Status::Decoded &&
               stream.tokens.size() == 1 && stream.consumed_bytes == 1 && stream.tokens[0].terminator,
           "first terminator stops without requiring a valid trailing token");
    auto bank_bytes = fixture();
    bank_bytes[24] = 0x81; bank_bytes[25] = 0; bank_bytes[26] = 0;
    awl::WorldMapMessageBank bank;
    expect(bank.parse(bank_bytes) && bank.scan_entry(0, &stream) == Status::Decoded &&
               stream.consumed_bytes == 3 && stream.tokens.size() == 2 &&
               bank.scan_entry(2, &stream) == Status::Decoded && stream.consumed_bytes == 3,
           "bank scans both logical aliases within the same physical safety extent");
    const auto prior_bank_stream = stream;
    expect(bank.scan_entry(1, &stream) == Status::MissingTerminator && same_stream(stream, prior_bank_stream) &&
               bank.scan_entry(4, &stream) == Status::InvalidInput && same_stream(stream, prior_bank_stream),
           "bank scan cannot read the next entry's terminator or replace output on an invalid index");
    expect(bank.scan_entry(0, nullptr) == Status::InvalidInput, "bank scan requires output");
    bank.clear();
    expect(bank.scan_entry(0, &stream) == Status::InvalidInput && same_stream(stream, prior_bank_stream),
           "cleared bank cannot scan stale entry bytes");
}

bool check_local(const char* root) {
    awl_memory_init();
    awl::filesystem_init();
    bool valid = awl::filesystem_mount("/", root);
    uint64_t digest = 14695981039346656037ull;
    uint64_t stream_digest = 14695981039346656037ull;
    uint64_t row_digest = 14695981039346656037ull;
    uint64_t row_cases = 0;
    uint64_t entries = 0;
    uint64_t token_count = 0;
    uint64_t consumed_bytes = 0;
    auto hash_byte = [&](uint8_t byte) { digest = (digest ^ byte) * 1099511628211ull; };
    auto hash_word = [&](uint32_t word) {
        for (unsigned shift : {24u, 16u, 8u, 0u}) hash_byte(static_cast<uint8_t>(word >> shift));
    };
    auto hash_stream_word = [&](uint32_t word) {
        for (unsigned shift : {24u, 16u, 8u, 0u}) {
            stream_digest = (stream_digest ^ static_cast<uint8_t>(word >> shift)) * 1099511628211ull;
        }
    };
    auto hash_row_word = [&](uint32_t word) {
        for (unsigned shift : {24u, 16u, 8u, 0u}) {
            row_digest = (row_digest ^ static_cast<uint8_t>(word >> shift)) * 1099511628211ull;
        }
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
            awl::WorldMapMessageStream stream;
            valid = bank.scan_entry(i, &stream) == awl::WorldMapMessageStreamStatus::Decoded &&
                !stream.tokens.empty() && stream.tokens.back().terminator && stream.consumed_bytes <= bounds.size;
            if (!valid) break;
            token_count += stream.tokens.size(); consumed_bytes += stream.consumed_bytes;
            hash_stream_word(id); hash_stream_word(i);
            hash_stream_word(static_cast<uint32_t>(stream.consumed_bytes));
            hash_stream_word(static_cast<uint32_t>(stream.tokens.size()));
            for (const auto& token : stream.tokens) {
                hash_stream_word(static_cast<uint32_t>(token.offset)); hash_stream_word(token.tag);
                hash_stream_word(token.visitor_slot); hash_stream_word(token.byte_count);
            }
            uint32_t full_rows = 0;
            for (const auto& token : stream.tokens) if (token.tag == 0 || token.tag == 1) ++full_rows;
            // Supplied diagnostic counts, not original selection activation.
            for (uint32_t wanted : {1u, full_rows}) {
                awl::WorldMapSelectionRows rows;
                valid = bank.prepare_selection_rows(i, wanted, &rows) == awl::WorldMapSelectionRowsStatus::Prepared;
                if (!valid) break;
                hash_row_word(id); hash_row_word(i); hash_row_word(wanted);
                hash_row_word(rows.max_width_units); hash_row_word(rows.byte_budget);
                hash_row_word(rows.aligned_storage_size); hash_row_word(static_cast<uint32_t>(rows.stop_token_offset));
                hash_row_word(static_cast<uint32_t>(rows.consumed_bytes)); hash_row_word(static_cast<uint32_t>(rows.bytes.size()));
                uint32_t width_bits = 0, height_bits = 0;
                std::memcpy(&width_bits, &rows.width, sizeof(width_bits));
                std::memcpy(&height_bits, &rows.height, sizeof(height_bits));
                hash_row_word(width_bits); hash_row_word(height_bits);
                for (uint8_t byte : rows.bytes) row_digest = (row_digest ^ byte) * 1099511628211ull;
                ++row_cases;
                const auto before = rows;
                valid = bank.prepare_selection_rows(i, full_rows + 1, &rows) ==
                    awl::WorldMapSelectionRowsStatus::InsufficientRows && same_rows(rows, before);
                if (!valid) break;
            }
        }
        std::printf("Message bank %u: %zu entries, %zu bytes; bounds and token streams checked\n",
                    id, bank.entry_count(), bank.byte_size());
    }
    std::printf("Message catalog entries=%llu, digest=%016llx\n",
                static_cast<unsigned long long>(entries), static_cast<unsigned long long>(digest));
    // Independent mapped-DOL catalog / direct-file Python probe supplies these
    // expectations. This checks opaque ownership only, not token semantics.
    valid = valid && entries == 14983 && digest == 0xaa12c3c9f29b06c2ull;
    std::printf("Message streams: tokens=%llu, consumed=%llu, digest=%016llx\n",
                static_cast<unsigned long long>(token_count), static_cast<unsigned long long>(consumed_bytes),
                static_cast<unsigned long long>(stream_digest));
    valid = valid && token_count == 902090 && consumed_bytes == 1612076 && stream_digest == 0x508ed55a9c95265bull;
    std::printf("Supplied selection rows: cases=%llu, digest=%016llx\n",
                static_cast<unsigned long long>(row_cases), static_cast<unsigned long long>(row_digest));
    valid = valid && row_cases == 29966 && row_digest == 0x92417279568c8fddull;
    awl::filesystem_shutdown();
    awl_memory_shutdown();
    return valid;
}

} // namespace

int main(int argc, char** argv) {
    test_keys();
    test_container();
    test_native_load();
    test_message_stream();
    test_selection_rows();
    if (argc == 3 && std::strcmp(argv[1], "--messages-local") == 0) {
        expect(check_local(argv[2]), "local complete catalog matches independent metadata/byte digest");
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: world_map_message_asset_tests [--messages-local disc]\n");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
