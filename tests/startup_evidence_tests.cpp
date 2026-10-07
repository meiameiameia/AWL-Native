#include "awl/startup_cli.h"
#include "awl/disc_identity.h"
#include "awl/movement_recording.h"
#include "awl/filesystem.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void word(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void test_cli() {
    awl::StartupOptions options;
    std::string error;
    const auto parse = [&](std::initializer_list<const char*> args) {
        std::vector<const char*> argv(args);
        return awl::parse_startup_options(static_cast<int>(argv.size()), argv.data(), &options, &error);
    };
    expect(parse({"awl"}) && options.mode == awl::StartupMode::Default, "default mode parses");
    expect(parse({"awl", "--help"}) && options.mode == awl::StartupMode::Help, "help parses");
    for (const char* mode : {"--verify-disc", "--target-smoke", "--preview-smoke", "--scene-smoke",
            "--movement-rehearsal", "--movement-rehearsal-smoke", "--movement-wall-rehearsal",
            "--movement-wall-rehearsal-smoke", "--movement-actor-rehearsal", "--movement-actor-rehearsal-smoke"})
        expect(parse({"awl", mode}), "existing single mode parses");
    expect(parse({"awl", "--movement-rehearsal", "--record-movement", "build/route-1.awlr"}) &&
        options.recording_path == "build/route-1.awlr", "seam recording parses");
    expect(parse({"awl", "--movement-rehearsal-smoke", "--record-movement", "build/route-1.awlr"}),
           "bounded neutral recording smoke parses");
    const auto previous = options;
    for (const auto args : {
            std::initializer_list<const char*>{"awl", "--typo"},
            {"awl", "--help", "--help"}, {"awl", "--scene-smoke", "--target-smoke"},
            {"awl", "--record-movement"}, {"awl", "--replay-movement"},
            {"awl", "--replay-movement", "--help"},
            {"awl", "--movement-wall-rehearsal", "--record-movement", "build/a.awlr"},
            {"awl", "--movement-rehearsal", "--record-movement", "disc/a.awlr"},
            {"awl", "--movement-rehearsal", "--record-movement", "build/a.awlr", "--record-movement", "build/b.awlr"}}) {
        expect(!parse(args) && !error.empty() && options.mode == previous.mode &&
            options.recording_path == previous.recording_path, "bad CLI rejects and preserves options");
    }
    expect(parse({"awl", "--replay-movement", "build/a.awlr"}) &&
        options.replay_path == "build/a.awlr", "headless replay parses");
    for (const char* path : {"build/../a.awlr", "build/sub/a.awlr", "build/a.txt", "build/.awlr",
        "disc/a.awlr", "build/CON.awlr", "build/COM1.awlr", "build/LPT9.awlr", "build/a:b.awlr"})
        expect(!awl::valid_movement_recording_path(path), "unsafe/nonlocal output filename rejects");
    expect(awl::valid_movement_recording_path("build\\my-route_2.awlr"), "native path separator accepted");
}

bool test_recording_format(const std::string& filename) {
    // Hand-authored wire fixture, independent of the encoder. One signed axis,
    // reset event, trigger boundary and exact signed-zero position word.
    std::vector<uint8_t> bytes(236, 0);
    const char magic[] = "AWLTICK1";
    std::memcpy(bytes.data(), magic, 8);
    word(bytes, 12, 30); word(bytes, 16, 1);
    std::memcpy(bytes.data() + 20, awl::kTargetGameId.data(), 6);
    std::memcpy(bytes.data() + 28, awl::kTargetDolSha1.data(), 20);
    bytes[48] = 7; bytes[68] = 9;
    word(bytes, 88, 0x80000000); word(bytes, 92, 0x3f800000); word(bytes, 96, 0x40000000);
    word(bytes, 100, 1); word(bytes, 104, 1); word(bytes, 108, 1); word(bytes, 112, 1);
    word(bytes, 116, 0x100); word(bytes, 120, 0x80); word(bytes, 136, 131);
    word(bytes, 144, 0x80000000); // First state word.
    awl::MovementRecording recording;
    expect(awl::decode_movement_recording(bytes.data(), bytes.size(), &recording) &&
        recording.ticks.size() == 1 && recording.ticks[0].reset_before &&
        recording.ticks[0].input.stick_x == -128 && recording.ticks[0].input.trigger_l == 131 &&
        recording.ticks[0].state[0] == 0x80000000u && recording.terrain_sha1[0] == 7,
        "hand-authored recording decodes exact metadata, signed axes and state words");
    std::vector<uint8_t> encoded;
    expect(awl::encode_movement_recording(recording, &encoded) && encoded == bytes,
           "encoding matches independently authored wire bytes");
    for (size_t prefix = 0; prefix < bytes.size(); ++prefix)
        expect(!awl::decode_movement_recording(bytes.data(), prefix, &recording) &&
            recording.ticks.size() == 1 && recording.ticks[0].state[0] == 0x80000000u,
            "every truncated prefix rejects without output publication");
    for (const auto mutation : {
        std::pair<size_t, uint32_t>{8, 1}, {12, 60}, {16, 2}, {100, 0},
        {100, 18001}, {104, 2}, {108, 2}, {112, 2}, {116, 0x80}, {120, 256},
        {136, 256}, {144, 0x7fc00000}, {88, 0x7f800000}}) {
        auto bad = bytes; word(bad, mutation.first, mutation.second);
        expect(!awl::decode_movement_recording(bad.data(), bad.size(), &recording),
               "unsupported profile/rate/count/order/boolean/PAD/nonfinite word rejects");
    }
    for (size_t offset : {size_t(0), size_t(20), size_t(26), size_t(28)}) {
        auto bad = bytes; bad[offset] ^= 1;
        expect(!awl::decode_movement_recording(bad.data(), bad.size(), &recording),
               "magic, target identity and reserved bytes reject");
    }
    auto extra = bytes; extra.push_back(0);
    expect(!awl::decode_movement_recording(extra.data(), extra.size(), &recording), "trailing bytes reject");
    const bool saved = awl::save_movement_recording(filename.c_str(), recording);
    expect(saved, "recording saves to fresh local filename");
    expect(!awl::save_movement_recording(filename.c_str(), recording), "existing recording is never overwritten");
    awl::MovementRecording loaded;
    expect(awl::load_movement_recording(filename.c_str(), &loaded) &&
        loaded.ticks[0].state == recording.ticks[0].state, "on-disk recording round trip agrees");
    expect(!awl::save_movement_recording("disc/forbidden.awlr", recording), "writer rejects an asset directory");
    auto bad = recording;
    bad.spawn[1] = std::numeric_limits<float>::quiet_NaN();
    expect(!awl::encode_movement_recording(bad, &encoded) && encoded == bytes,
           "invalid encode preserves previous bytes");
    recording.ticks.resize(awl::kMaxMovementRecordingTicks, recording.ticks.front());
    expect(awl::encode_movement_recording(recording, &encoded), "ten-minute recording bound is supported");
    recording.ticks.push_back(recording.ticks.front());
    expect(!awl::encode_movement_recording(recording, &encoded), "tick limit rejects excess input");
    return saved;
}

void test_hash_and_identity(const std::filesystem::path& directory) {
    awl::Sha1 digest{};
    constexpr awl::Sha1 empty{0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,
        0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09};
    constexpr awl::Sha1 abc{0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    expect(awl::sha1_bytes(nullptr, 0, &digest) && digest == empty, "CNG matches empty SHA1 vector");
    const uint8_t message[] = {'a','b','c'};
    expect(awl::sha1_bytes(message, 3, &digest) && digest == abc, "CNG matches standard abc SHA1 vector");
    expect(!awl::sha1_bytes(nullptr, 1, &digest) && digest == abc &&
        !awl::sha1_bytes(message, size_t(33) * 1024 * 1024, &digest) && digest == abc,
        "invalid hash span preserves digest");
    std::filesystem::create_directories(directory / "sys");
    awl::filesystem_init();
    expect(awl::filesystem_mount("/", directory.string().c_str()), "synthetic identity fixture mounts");
    expect(awl::verify_mounted_disc_identity().status == awl::DiscIdentityStatus::BootUnreadable,
           "missing boot rejects");
    const auto write_boot = [&](const char* text, size_t length) {
        std::ofstream boot(directory / "sys/boot.bin", std::ios::binary | std::ios::trunc);
        boot.write(text, static_cast<std::streamsize>(length));
    };
    write_boot("GYWEE", 5);
    expect(awl::verify_mounted_disc_identity().status == awl::DiscIdentityStatus::BootUnreadable,
           "short boot rejects");
    write_boot("WRONG!", 6);
    expect(awl::verify_mounted_disc_identity().status == awl::DiscIdentityStatus::WrongGameId,
           "unsupported game ID rejects");
    write_boot("GYWEE9", 6);
    expect(awl::verify_mounted_disc_identity().status == awl::DiscIdentityStatus::DolUnreadable,
           "missing DOL rejects");
    {
        std::ofstream file(directory / "sys/main.dol", std::ios::binary);
        file.write("abc", 3);
    }
    expect(awl::verify_mounted_disc_identity().status == awl::DiscIdentityStatus::WrongDol,
           "synthetic bytes cannot satisfy supported DOL identity");
    expect(awl::sha1_native_file((directory / "sys/main.dol").string().c_str(), &digest) && digest == abc,
           "native file hashing agrees with known vector");
    {
        std::ofstream file(directory / "sys/main.dol", std::ios::binary | std::ios::trunc);
        file.seekp(std::streamoff(33) * 1024 * 1024); file.put('x');
    }
    expect(awl::verify_mounted_disc_identity().status == awl::DiscIdentityStatus::DolUnreadable,
           "oversized DOL rejects before hashing");
    awl::filesystem_shutdown();
}
} // namespace

int main() {
    const std::string unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string recording_path = "build/evidence-test-" + unique + ".awlr";
    const std::filesystem::path fixture = "build/evidence-test-" + unique;
    try {
        if (std::filesystem::exists(recording_path) || !std::filesystem::create_directory(fixture))
            throw std::runtime_error("Synthetic fixture name already exists");
        const auto resolved_fixture = std::filesystem::canonical(fixture);
        if (resolved_fixture.parent_path() != std::filesystem::canonical("build"))
            throw std::runtime_error("Synthetic fixture escaped build/");
        test_cli();
        const bool created_recording = test_recording_format(recording_path);
        test_hash_and_identity(fixture);
        if (created_recording) std::filesystem::remove(recording_path);
        std::filesystem::remove_all(resolved_fixture); // Verified, newly created test directory only.
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Test fixture error: %s\n", error.what()); ++failures;
    }
    if (!failures) std::puts("Startup/evidence tests passed.");
    return failures ? 1 : 0;
}
