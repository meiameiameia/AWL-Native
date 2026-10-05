#include "awl/world_map_animation_channel.h"

#include <array>
#include <algorithm>
#include <cfenv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {
using Bank = awl::WorldMapAnimationBank;
using Playback = awl::WorldMapAnimationPlayback;
using Record = awl::WorldMapAnimationPlaybackRecord;
using Channel = awl::WorldMapAnimationChannelState;
using Step = awl::WorldMapAnimationChannelStep;
using Status = awl::WorldMapAnimationChannelStatus;
using Branch = awl::WorldMapAnimationChannelBranch;
using Setup = awl::WorldMapActorAnimationSetup;
using Binding = awl::WorldMapAnimationModelBinding;
int failures = 0;
void expect(bool yes, const char* message) {
    if (!yes) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
uint32_t bits(float value) { uint32_t word; std::memcpy(&word, &value, 4); return word; }
void put(std::vector<uint8_t>& bytes, size_t offset, uint32_t word) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(word >> (24 - 8 * i));
}
void hash(uint64_t& digest, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) { digest ^= static_cast<uint8_t>(word >> (24 - i * 8)); digest *= 1099511628211ull; }
}
std::vector<uint8_t> raw() {
    // Invented table: IDs 7,0,0. The first zero scalar is -2, not the final 9.
    std::vector<uint8_t> bytes(56);
    put(bytes, 0, 0x12345678); put(bytes, 4, 0x00030000);
    put(bytes, 8, bits(4)); put(bytes, 16, 7);
    put(bytes, 24, bits(-2)); put(bytes, 32, 0);
    put(bytes, 40, bits(9)); put(bytes, 48, 0);
    return bytes;
}
std::vector<uint8_t> archive() {
    // Invented flat U8, with two raw fixtures and no game bytes/names.
    std::vector<uint8_t> bytes(240);
    put(bytes, 0, 0x55aa382d); put(bytes, 4, 32); put(bytes, 8, 48); put(bytes, 12, 96);
    put(bytes, 32, 0x01000000); put(bytes, 40, 3);
    put(bytes, 44, 1); put(bytes, 48, 96); put(bytes, 52, 56);
    put(bytes, 56, 3); put(bytes, 60, 160); put(bytes, 64, 56);
    bytes[69] = 'a'; bytes[71] = 'b';
    const auto clip = raw();
    std::copy(clip.begin(), clip.end(), bytes.begin() + 96);
    std::copy(clip.begin(), clip.end(), bytes.begin() + 160);
    put(bytes, 184, bits(6));
    return bytes;
}
Playback playback(uint32_t seed, bool active = true) {
    Playback p;
    p.position_0 = static_cast<float>(seed); p.rate_4 = static_cast<float>(seed + 1);
    p.word_8 = seed + 2; p.limit_c = static_cast<float>(seed + 3);
    if (active) p.clip_10 = awl::WorldMapAnimationClipReference{77, seed + 4};
    p.link_14 = seed + 5; p.value_18 = static_cast<float>(seed + 6);
    return p;
}
std::vector<Record> records(bool active = true) {
    return {{1, playback(10, active)}, {2, playback(20)}, {3, playback(30)}, {4, playback(40)}};
}
bool same(const Playback& a, const Playback& b) {
    return bits(a.position_0) == bits(b.position_0) && bits(a.rate_4) == bits(b.rate_4) && a.word_8 == b.word_8 &&
        bits(a.limit_c) == bits(b.limit_c) && bool(a.clip_10) == bool(b.clip_10) &&
        (!a.clip_10 || (a.clip_10->bank_identity == b.clip_10->bank_identity && a.clip_10->offset == b.clip_10->offset)) &&
        a.link_14 == b.link_14 && bits(a.value_18) == bits(b.value_18);
}
void test_bank() {
    Bank bank; awl::WorldMapAnimationClip clip;
    expect(bank.parse(200, raw()) && !bank.is_archive() && bank.clip_count() == 1 &&
        bank.resolve(UINT32_MAX, &clip) && clip.reference.bank_identity == 200 && clip.reference.offset == 0 &&
        clip.parameter_zero == -2, "raw ignores clip index and selects the first section zero, preserving a finite negative value");
    expect(bank.parse(200, archive()) && bank.is_archive() && bank.clip_count() == 2 &&
        bank.resolve(0x10001, &clip) && clip.reference.offset == 160 && clip.parameter_zero == 6,
        "archive takes low 16 bits plus root offset, not full index or named-file order");
    const auto saved = clip;
    expect(!bank.resolve(2, &clip) && !bank.resolve(UINT32_MAX, &clip) && !bank.resolve(0, nullptr) &&
        clip.reference.offset == saved.reference.offset, "out-of-range/null lookups preserve output");
    for (unsigned fault = 0; fault < 8; ++fault) {
        auto bytes = archive();
        switch (fault) {
        case 0: put(bytes, 40, UINT32_MAX); break;
        case 1: put(bytes, 44, 0x01000001); break;
        case 2: put(bytes, 48, 80); break;
        case 3: put(bytes, 52, UINT32_MAX); break;
        case 4: put(bytes, 60, 100); break;
        case 5: put(bytes, 44, 100); break;
        case 6: bytes[70] = bytes[72] = bytes[73] = bytes[74] = bytes[75] = bytes[76] = bytes[77] = bytes[78] = bytes[79] = 'z'; break;
        case 7: put(bytes, 8, 12); break;
        }
        expect(!bank.parse(200, bytes) && !bank.loaded(), "unsupported or malformed archive clears owned data");
    }
    auto bytes = raw(); bytes.pop_back();
    expect(!bank.parse(200, bytes) && !bank.parse(0, raw()), "truncated table and null bank identity fail");
    bytes = raw(); put(bytes, 32, 8); put(bytes, 48, 9);
    expect(bank.parse(200, bytes) && !bank.resolve(0, &clip), "missing section zero is rejected");
    bytes = raw(); put(bytes, 24, 0x7fc00000);
    expect(bank.parse(200, bytes) && !bank.resolve(0, &clip), "nonfinite reached scalar is explicitly unsupported");
    bytes = raw(); put(bytes, 8, 0x7fc00000);
    expect(bank.parse(200, bytes) && bank.resolve(0, &clip), "unused section scalar remains opaque");
}
void test_channel() {
    Bank bank; expect(bank.parse(200, raw()), "synthetic bank loads");
    const Setup setup{100, 200, 99, 10, 123, -0.0f}; // r6 is unused in this helper.
    const Binding model{100, 1};
    Channel channel{1, 2, 2, 3, 4, -3.0f, 1};
    auto pool = records(); Step step;
    expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
        step.branch == Branch::Interrupted && step.after.mode_18 == 2 && step.after.blend_14 == 0.5f &&
        step.after.elapsed_0 == 0 && step.after.duration_4 == 10 &&
        step.records_after[3].state.position_0 == 30 && step.records_after[2].state.position_0 == 20 &&
        step.records_after[2].state.link_14 == 0 && bits(step.records_after[2].state.value_18) == 0 &&
        bits(step.records_after[1].state.position_0) == 0x80000000 && step.records_after[1].state.rate_4 == 21 &&
        step.records_after[1].state.word_8 == 0 && step.records_after[1].state.limit_c == -2 &&
        step.records_after[1].state.link_14 == 25 && step.records_after[1].state.value_18 == 26 &&
        same(pool[1].state, playback(20)), "interrupted blend copies previous to older before target to previous, then initializes only four target fields");
    expect(awl::advance_world_map_animation_channel(&channel, &pool, setup, model, &bank, &step) == Status::Advanced &&
        channel.mode_18 == 2 && pool[1].state.clip_10->bank_identity == 200, "complete helper advances supplied snapshots");
    for (uint32_t mode : {0u, 1u, 99u}) {
        channel = {2, 2, 2, 3, 0, -3.0f, mode}; pool = records();
        expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
            step.branch == Branch::Completed && step.after.mode_18 == 1 && step.after.blend_14 == -3 &&
            step.records_after[2].state.position_0 == 10, "unsigned completed gate precedes prior mode and does not read unused older record");
    }
    channel = {0, 2, 2, 3, 0, -3.0f, 0};
    expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
        step.branch == Branch::First && step.after.blend_14 == -3, "first transition copies model and preserves blend scalar");
    pool = records(false); channel = {0, 0, 2, 0, 0, -3.0f, 99};
    expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
        step.branch == Branch::NoClip && step.after.mode_18 == 0 && step.after.blend_14 == -3 &&
        same(step.records_after[2].state, pool[2].state), "no model clip skips clock gates and all copies, preserving unused records");
    channel = {1, 2, 2, 3, 4, -3.0f, 1}; pool = records();
    expect(awl::advance_world_map_animation_channel(&channel, &pool, setup, std::nullopt, &bank, &step) == Status::RequiresModelBinding &&
        channel.elapsed_0 == 1 && same(pool[1].state, playback(20)), "unknown model binding cannot advance supplied state");
    expect(awl::advance_world_map_animation_channel(&channel, &pool, setup, model, nullptr, &step) == Status::RequiresBank &&
        step.after.elapsed_0 == 1 && same(step.records_after[2].state, pool[2].state) && same(pool[3].state, playback(40)),
        "missing bank rolls back proposed copies and keeps parent state pending");
    auto missing = pool; missing.pop_back();
    expect(awl::prepare_world_map_animation_channel(channel, missing, setup, model, &bank, &step) == Status::RequiresPlaybackRecord &&
        step.required_record == 4 && same(step.records_after[2].state, missing[2].state), "first reached missing playback record is explicit and atomic");
    missing = pool; missing.erase(missing.begin() + 2);
    expect(awl::prepare_world_map_animation_channel(channel, missing, setup, model, &bank, &step) == Status::RequiresPlaybackRecord &&
        step.required_record == 3, "missing first copy source blocks before any later record lookup");
    missing = pool; missing.erase(missing.begin() + 1);
    expect(awl::prepare_world_map_animation_channel(channel, missing, setup, model, &bank, &step) == Status::RequiresPlaybackRecord &&
        step.required_record == 2 && same(step.records_after[2].state, playback(40)), "missing second copy source rolls back the earlier older-record copy");
    missing = pool; missing.erase(missing.begin());
    expect(awl::prepare_world_map_animation_channel(channel, missing, setup, model, &bank, &step) == Status::RequiresPlaybackRecord &&
        step.required_record == 1, "model record lookup precedes channel record reads");
    const auto saved = step;
    auto null_pointer = channel; null_pointer.older_10 = 0;
    auto duplicate = pool; duplicate.push_back(pool[0]);
    Bank wrong; expect(wrong.parse(201, raw()), "other bank loads");
    expect(awl::prepare_world_map_animation_channel(null_pointer, pool, setup, model, &bank, &step) == Status::InvalidInput &&
        awl::prepare_world_map_animation_channel(channel, duplicate, setup, model, &bank, &step) == Status::InvalidInput &&
        awl::prepare_world_map_animation_channel(channel, pool, setup, Binding{101, 1}, &bank, &step) == Status::InvalidInput &&
        awl::prepare_world_map_animation_channel(channel, pool, setup, model, &wrong, &step) == Status::InvalidInput &&
        step.required_record == saved.required_record, "null reached record, duplicate ownership and mismatched keys fail without output changes");
    expect(awl::prepare_world_map_animation_channel(step.after, pool, setup, model, &bank, &step) == Status::InvalidInput &&
        awl::prepare_world_map_animation_channel(channel, step.records_after, setup, model, &bank, &step) == Status::InvalidInput &&
        awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, nullptr) == Status::InvalidInput &&
        awl::advance_world_map_animation_channel(nullptr, &pool, setup, model, &bank, &step) == Status::InvalidInput,
        "aliasing output and null arguments are rejected");
    auto nonfinite = setup; nonfinite.start_value = std::numeric_limits<float>::infinity();
    expect(awl::prepare_world_map_animation_channel(channel, pool, nonfinite, model, &bank, &step) == Status::InvalidInput,
        "nonfinite setup value is explicitly unsupported");
    auto unsafe = pool; unsafe[2].state.rate_4 = std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_animation_channel(channel, unsafe, setup, model, &bank, &step) == Status::InvalidInput,
        "reached nonfinite copied field is explicitly unsupported");
    unsafe = records(false); unsafe[0].state.rate_4 = std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_animation_channel(channel, unsafe, setup, model, &bank, &step) == Status::Prepared,
        "unused model scalar is not read by the no-clip gate");
    pool = records(); pool[2].state.link_14 = UINT32_MAX; pool[2].state.value_18 = std::numeric_limits<float>::quiet_NaN();
    expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
        step.records_after[3].state.link_14 == 0 && bits(step.records_after[3].state.value_18) == 0,
        "copy reset fields do not validate unused source values");
    Bank high_bank; expect(high_bank.parse(0x1000000c8ull, raw()), "high-width bank identity loads");
    auto high_setup = setup; high_setup.model_identity = 0x100000064ull; high_setup.bank_identity = high_bank.identity();
    expect(awl::prepare_world_map_animation_channel(channel, pool, high_setup, Binding{high_setup.model_identity,1},
        &high_bank, &step) == Status::Prepared && step.records_after[1].state.clip_10->bank_identity == high_bank.identity(),
        "native model/bank identities retain all 64 bits");
}

void hash_playback(uint64_t& digest, const Record& record) {
    const auto& p = record.state;
    for (uint32_t word : {static_cast<uint32_t>(record.identity), bits(p.position_0), bits(p.rate_4), p.word_8,
        bits(p.limit_c), p.clip_10 ? static_cast<uint32_t>(p.clip_10->bank_identity) : 0u,
        p.clip_10 ? p.clip_10->offset : 0u, static_cast<uint32_t>(p.link_14), bits(p.value_18)}) hash(digest, word);
}
void test_partial_channel() {
    Bank bank;expect(bank.parse(200,raw()),"partial channel bank loads");
    const Setup setup{100,200,0,10,0,0};const Binding binding{100,1};
    auto pool=[](bool active){std::vector<awl::WorldMapAnimationPartialPlaybackRecord> result;
        for(const auto& record:records(active))result.push_back({record.identity,awl::partial_world_map_animation_playback(record.state)});
        return result;};
    auto partial=pool(false);awl::WorldMapAnimationPartialChannelStep step;
    partial[0].state.word_8.reset();partial[0].state.limit_c.reset();partial[0].state.value_18.reset();
    partial[1].state.word_8.reset();partial[1].state.limit_c.reset();partial[1].state.value_18.reset();
    const Channel no_clip{0,0,2,0,0,-3.0f,99};
    expect(awl::prepare_world_map_partial_animation_channel(no_clip,partial,setup,binding,&bank,&step)==Status::Prepared &&
        step.branch==Branch::NoClip && !step.records_after[0].state.word_8 && !step.records_after[0].state.limit_c &&
        step.records_after[1].state.word_8==0u && step.records_after[1].state.limit_c==-2 &&
        step.records_after[1].state.rate_4==21 && step.records_after[1].state.link_14==25 &&
        !step.records_after[1].state.value_18 && !step.records_after[1].state.complete(),
        "no-clip reads no unknown model fields; target initialization preserves unknown weight and known rate/link");
    partial=pool(true);partial[0].state.word_8.reset();partial[0].state.limit_c.reset();
    const Channel completed{0,0,2,3,4,-3.0f,2};
    expect(awl::prepare_world_map_partial_animation_channel(completed,partial,setup,binding,&bank,&step)==Status::RequiresPlaybackFields &&
        step.required_record==1 && step.required_fields==1 && step.after.elapsed_0==0 &&
        step.records_after[2].state.position_0==30,"first reached unknown source word blocks before limit and preserves copies");
    partial[0].state.word_8=12;
    expect(awl::prepare_world_map_partial_animation_channel(completed,partial,setup,binding,&bank,&step)==Status::RequiresPlaybackFields &&
        step.required_record==1 && step.required_fields==2,"known source word exposes the later unknown source limit");
    partial=pool(true);partial[1].state.word_8.reset();
    const Channel interrupted{1,2,2,3,4,-3.0f,1};
    expect(awl::prepare_world_map_partial_animation_channel(interrupted,partial,setup,binding,&bank,&step)==Status::RequiresPlaybackFields &&
        step.required_record==2 && step.required_fields==1 && step.records_after[3].state.position_0==40 &&
        step.after.blend_14==-3,"late unknown copy source rolls back the earlier previous-to-older copy");
    partial=pool(true);partial[1].state.value_18.reset();partial[2].state.value_18.reset();
    expect(awl::prepare_world_map_partial_animation_channel(interrupted,partial,setup,binding,&bank,&step)==Status::Prepared &&
        step.records_after[3].state.value_18==0 && step.records_after[2].state.value_18==0 &&
        !step.records_after[1].state.value_18 && step.records_after[2].state.complete(),
        "FECC resets unknown source weights without reading them while FE08 retains target unknown weight");
    partial=pool(true);partial[0].state.value_18.reset();
    const Channel all_alias{0,0,1,1,1,-3.0f,2};
    expect(awl::prepare_world_map_partial_animation_channel(all_alias,partial,setup,binding,&bank,&step)==Status::Prepared &&
        step.records_after[0].state.complete() && step.records_after[0].state.value_18==0 &&
        step.records_after[0].state.link_14==0 && step.records_after[0].state.limit_c==-2,
        "self-copy establishes reset weight before aliased target initialization without reading old unknown weight");
    const auto saved=step.required_record;partial[0].state.limit_c=std::numeric_limits<float>::infinity();
    expect(awl::prepare_world_map_partial_animation_channel(completed,partial,setup,binding,&bank,&step)==Status::InvalidInput &&
        step.required_record==saved,"reached known nonfinite copy limit rejects without output mutation");
}
void test_matrix() {
    Bank bank; expect(bank.parse(200, raw()), "matrix bank loads");
    const std::array<std::array<uint32_t, 2>, 8> clocks{{{0,0}, {0,1}, {1,2}, {2,2}, {3,2},
        {0x7ffffffe,0x7fffffff}, {0xfffffffe,UINT32_MAX}, {UINT32_MAX,0}}};
    const std::array<std::array<uint64_t, 4>, 6> layouts{{{1,2,3,4}, {1,2,2,4}, {1,2,3,3},
        {1,2,3,2}, {1,1,3,4}, {1,1,1,1}}};
    uint64_t digest = 14695981039346656037ull; unsigned cases = 0;
    for (uint32_t active : {0u,1u}) for (const auto& clock : clocks) for (uint32_t mode : {0u,1u,2u,UINT32_MAX}) {
        for (const auto& layout : layouts) for (uint32_t blend : {0u,1u,10u,UINT32_MAX}) {
            const auto pool = records(active != 0);
            const Channel channel{clock[0],clock[1],layout[1],layout[2],layout[3],-3.0f,mode};
            const Setup setup{100,200,UINT32_MAX,blend,0,-0.0f}; Step step;
            const auto status = awl::prepare_world_map_animation_channel(channel,pool,setup,Binding{100,layout[0]},&bank,&step);
            expect(status == Status::Prepared && step.branch && same(pool[0].state,playback(10,active != 0)), "matrix preparation preserves input");
            if (status != Status::Prepared || !step.branch) continue;
            for (uint32_t word : {active,clock[0],clock[1],mode,static_cast<uint32_t>(layout[1]),
                static_cast<uint32_t>(layout[2]),static_cast<uint32_t>(layout[3]),blend,static_cast<uint32_t>(*step.branch),
                step.after.elapsed_0,step.after.duration_4,bits(*step.after.blend_14),step.after.mode_18}) hash(digest,word);
            for (const auto& record : step.records_after) hash_playback(digest,record);
            ++cases;
        }
    }
    std::cout << "CHANNEL_MATRIX " << cases << ' ' << std::hex << digest << std::dec << '\n';
    // Expected digest is independently obtained from the mapped DOL listing.
    expect(cases == 1536 && digest == 0xab9e66bb9f113eadull, "channel matrix matches independent instruction probe");
}
awl::WorldMapAnimationPose prior_pose() {
    awl::WorldMapAnimationPose pose{};
    for (uint32_t i = 0; i < pose.size(); ++i) pose[i] = 0xa5a50000u + i;
    return pose;
}
std::vector<uint8_t> constant_fixture(uint32_t seed) {
    // Invented values only. Each of the five numeric encodings, three
    // exponents, eight omitted-axis masks and eight component sets is reached.
    const uint8_t encoding = uint8_t(seed % 5), exponent = std::array<uint8_t,3>{0,7,15}[(seed / 5) % 3];
    const uint8_t omitted = uint8_t((seed / 15) % 8), components = uint8_t((seed / 120) % 8);
    size_t cursor = encoding < 2 && (components & 3) == 3 ? 65 : 64;
    std::vector<uint8_t> bytes(256);
    put(bytes, 4, 0x00010000); put(bytes, 12, uint32_t(cursor));
    bytes[19] = 7; bytes[20] = uint8_t((encoding << 4) | exponent); bytes[21] = uint8_t(omitted << 5);
    bytes[23] = uint8_t((components & 1 ? 2 : 0) | (components & 2 ? 8 : 0) | (components & 4 ? 1 : 0));
    uint32_t state = seed + 1;
    auto component = [&](uint8_t kind) {
        state = state * 1664525u + 1013904223u;
        const unsigned width = kind < 2 ? 1u : kind < 4 ? 2u : 4u;
        const uint32_t value = kind < 4 ? state >> (32 - width * 8) :
            bits(float(int32_t((state >> 16) % 2049) - 1024) / 64.0f);
        for (unsigned i = 0; i < width; ++i) bytes[cursor++] = uint8_t(value >> ((width - i - 1) * 8));
    };
    if (components & 1) for (unsigned i = 0; i < 3; ++i) component(encoding);
    if (components & 2) for (unsigned i = 0; i < 4; ++i) if (i == 3 || (omitted & (1u << i)) == 0) component(3);
    if (components & 4) for (unsigned i = 0; i < 3; ++i) component(encoding);
    return bytes;
}
void test_constant_poses() {
    using PoseStatus = awl::WorldMapAnimationPoseStatus;
    const auto prior = prior_pose(); const awl::WorldMapAnimationPoseSettings settings;
    Bank bank; Playback playback; playback.clip_10 = awl::WorldMapAnimationClipReference{200,0};
    uint64_t digest = 14695981039346656037ull; unsigned cases = 0, unaligned = 0;
    for (uint32_t seed = 0; seed < 960; ++seed) {
        expect(bank.parse(200, constant_fixture(seed)), "constant fixture parses");
        auto pose = prior;
        const auto status = awl::sample_world_map_animation_pose(playback,0x10007,&bank,settings,prior,&pose);
        const uint32_t omitted = (seed / 15) % 8;
        const unsigned quaternion_count = 4 - unsigned(omitted & 1) - unsigned((omitted >> 1) & 1) - unsigned((omitted >> 2) & 1);
        if (seed % 5 == 4 && ((seed / 120) % 8 & 6) == 6 && quaternion_count % 2) {
            expect(status == PoseStatus::UnsupportedLayout && pose == prior, "unaligned float payload rejects atomically");
            ++unaligned; continue;
        }
        expect(status == PoseStatus::Sampled, "constant components sample with node low-halfword lookup");
        hash(digest, seed); for (auto w : pose) hash(digest, w); ++cases;
        auto alias = prior;
        expect(awl::sample_world_map_animation_pose(playback,7,&bank,settings,alias,&alias) == status && alias == pose,
            "prior/output alias preserves the same component writes");
        expect((pose[0] & 0xffffffu) == (prior[0] & 0xffffffu) && pose[11] == prior[11] && pose[12] == prior[12],
            "flag clear and reached setters preserve unused bytes and tail words");
    }
    std::cout << "CONSTANT_POSES " << cases << ' ' << std::hex << digest << std::dec << '\n';
    expect(cases == 936 && unaligned == 24, "complete encoding/component matrix retains alignment boundary");
    // Independently executed raw relocation/lookup/sampler/setter bodies.
    expect(digest == 0x7ff3c94708598a27ull, "constant poses match instruction comparison");
    auto bytes = constant_fixture(840); // All components, U8; quaternion is S16.
    expect(bank.parse(200, bytes), "failure fixture parses"); auto pose = prior;
    playback.clip_10.reset(); playback.link_14 = 99; playback.position_0 = std::numeric_limits<float>::quiet_NaN();
    expect(awl::sample_world_map_animation_pose(playback,7,nullptr,settings,prior,&pose) == PoseStatus::NoPose && pose == prior,
        "null clip does not reach bank, time, loop or blend");
    playback.clip_10 = awl::WorldMapAnimationClipReference{200,0};
    expect(awl::sample_world_map_animation_pose(playback,8,&bank,settings,prior,&pose) == PoseStatus::NoPose && pose == prior,
        "missing node exits before blend");
    expect(awl::sample_world_map_animation_pose(playback,7,nullptr,settings,prior,&pose) == PoseStatus::RequiresBank,
        "reached clip needs a retained bank");
    expect(awl::sample_world_map_animation_pose(playback,7,&bank,settings,prior,&pose) == PoseStatus::RequiresBlend && pose == prior,
        "reached blend never publishes an unblended pose"); playback.link_14 = 0;
    expect(awl::sample_world_map_animation_pose(playback,7,&bank,settings,prior,&pose) == PoseStatus::Sampled,
        "constant components do not evaluate copied time");
    bytes[21] |= 8; expect(bank.parse(200,bytes), "keyed fixture parses"); pose = prior;
    expect(awl::sample_world_map_animation_pose(playback,7,&bank,settings,prior,&pose) == PoseStatus::RequiresKeyedTracks && pose == prior,
        "keyed tracks stop explicitly before partial constant publication");
    for (unsigned fault = 0; fault < 7; ++fault) {
        bytes = constant_fixture(844); // Float32 scale/translation and S16 quaternion.
        switch (fault) {
        case 0: put(bytes,12,UINT32_MAX); break;
        case 1: put(bytes,12,8); break;
        case 2: bytes.resize(66); break;
        case 3: bytes[20] = 0x50; break;
        case 4: put(bytes,64,0x7fc00000); break;
        case 5: put(bytes,64,1); break;
        case 6: put(bytes,12,65); break;
        }
        expect(bank.parse(200,bytes), "malformed payload remains opaque at metadata parse"); pose = prior;
        const auto status = bank.sample_constant_pose(*playback.clip_10,7,settings,prior,&pose);
        expect(status == (fault == 4 || fault == 5 ? PoseStatus::UnsupportedNumerics : PoseStatus::UnsupportedLayout) && pose == prior,
            "out-of-span/header/truncated/unknown/unaligned/nonfinite/subnormal reached payload rejects atomically");
    }
    bytes = constant_fixture(840); expect(bank.parse(200,bytes), "rounding fixture parses"); pose = prior;
    const int rounding = std::fegetround(); std::fesetround(FE_DOWNWARD);
    const auto rounding_status = bank.sample_constant_pose(*playback.clip_10,7,settings,prior,&pose);
    std::fesetround(rounding);
    expect(rounding_status == PoseStatus::UnsupportedNumerics && pose == prior, "unsupported rounding preserves output");
    expect(bank.sample_constant_pose({201,0},7,settings,prior,&pose) == PoseStatus::InvalidInput &&
        bank.sample_constant_pose({200,1},7,settings,prior,&pose) == PoseStatus::InvalidInput &&
        bank.sample_constant_pose({200,0},7,settings,prior,nullptr) == PoseStatus::InvalidInput,
        "foreign identity, interior clip pointer and null output reject");
    // Duplicate IDs keep the first even if it has no components or invalid
    // opaque payload. No scalar-zero lookup is required for pose sampling.
    bytes = constant_fixture(840); put(bytes,4,0x00020000);
    std::copy_n(bytes.begin()+8,16,bytes.begin()+24); bytes[23] = 0; put(bytes,12,UINT32_MAX);
    expect(bank.parse(200,bytes), "duplicate fixture parses"); pose = prior;
    expect(bank.sample_constant_pose({200,0},7,settings,prior,&pose) == PoseStatus::Sampled && pose[0] == (prior[0]&0xffffffu) &&
        std::equal(pose.begin()+1,pose.end(),prior.begin()+1), "first matching empty section wins without reading payload");
    bytes.assign(256,0); put(bytes,4,0x00010000); put(bytes,12,64);
    bytes[19]=7; bytes[20]=0x3e; bytes[21]=0xe0; bytes[23]=9;
    put(bytes,64,0x3f800000); put(bytes,68,0x0100ff00);
    expect(bank.parse(200,bytes), "supplied quaternion globals fixture parses");
    uint64_t settings_digest = 14695981039346656037ull;
    for (uint8_t encoding = 0; encoding < 5; ++encoding) for (float scale : {0.0f,0x1p-14f,1.0f,-0x1p-7f,2.0f}) {
        pose = prior;
        expect(bank.sample_constant_pose({200,0},7,{encoding,scale},prior,&pose) == PoseStatus::Sampled,
            "supplied quaternion globals preserve fixed count*2 cursor and omitted-axis zero writes");
        hash(settings_digest,encoding); hash(settings_digest,bits(scale)); for (auto w : pose) hash(settings_digest,w);
    }
    std::cout << "QUATERNION_SETTINGS " << std::hex << settings_digest << std::dec << '\n';
    expect(settings_digest == 0x54650a558df26b55ull,"supplied quaternion settings match independent instruction comparison");
    pose=prior;
    expect(bank.sample_constant_pose({200,0},7,{5,1},prior,&pose) == PoseStatus::UnsupportedLayout && pose==prior,
        "unknown reached quaternion encoding preserves pose");
    expect(bank.sample_constant_pose({200,0},7,{3,std::numeric_limits<float>::infinity()},prior,&pose) == PoseStatus::UnsupportedNumerics && pose==prior,
        "nonfinite reached quaternion scale preserves pose");
    expect(bank.sample_constant_pose({200,0},7,{4,std::numeric_limits<float>::infinity()},prior,&pose) == PoseStatus::Sampled,
        "float quaternion decoding leaves unused scale opaque");
    bytes=archive(); put(bytes,108,56); bytes[116]=0; bytes[117]=0; bytes[119]=2;
    expect(bank.parse(200,bytes),"archive payload boundary fixture parses"); pose=prior;
    expect(bank.sample_constant_pose({200,96},7,settings,prior,&pose)==PoseStatus::UnsupportedLayout && pose==prior,
        "payload cannot cross selected archive entry into padding or another clip");
    put(bytes,100,0xffff0000); expect(bank.parse(200,bytes),"archive section count stays opaque at parse");
    expect(bank.sample_constant_pose({200,96},7,settings,prior,&pose)==PoseStatus::UnsupportedLayout && pose==prior,
        "malformed selected archive table rejects before node lookup");
}
void local_bank(const std::filesystem::path& disc) {
    std::ifstream input(disc / "files" / "boy_0.anm.arc", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    Bank bank;
    expect(bank.parse(200, bytes) && bank.is_archive() && bank.clip_count() == 126,
        "local supplied boy animation bank validates as a flat archive");
    if (!bank.loaded()) return;
    uint64_t digest = 14695981039346656037ull;
    for (uint32_t i = 0; i < bank.clip_count(); ++i) {
        awl::WorldMapAnimationClip clip, alias;
        expect(bank.resolve(i, &clip) && bank.resolve(i + 0x10000, &alias) &&
            clip.reference.offset == alias.reference.offset, "all local nodes resolve with low-word aliasing");
        for (uint32_t word : {i,clip.reference.offset,clip.size,bits(clip.parameter_zero)}) hash(digest,word);
    }
    std::cout << "LOCAL_ANIMATION_BANK " << bank.clip_count() << ' ' << std::hex << digest << std::dec << '\n';
    expect(digest == 0xbd04bfab3a7b2993ull, "all local extents and section-zero scalars match independent metadata walk");
    using PoseStatus = awl::WorldMapAnimationPoseStatus;
    const auto prior = prior_pose(); const awl::WorldMapAnimationPoseSettings settings;
    uint64_t pose_digest = 14695981039346656037ull; unsigned sampled = 0, keyed = 0;
    for (uint32_t i = 0; i < bank.clip_count(); ++i) {
        awl::WorldMapAnimationClip clip;
        if (!bank.resolve(i,&clip)) { expect(false,"local pose clip resolves"); continue; }
        const size_t base = clip.reference.offset;
        if (base > bytes.size() || clip.size > bytes.size()-base || clip.size < 8) {
            expect(false,"local clip span remains bounded"); continue;
        }
        const unsigned count = (unsigned(bytes[base+4])<<8)|bytes[base+5];
        if (count > (clip.size-8)/16) { expect(false,"local section table bounded"); continue; }
        Playback p; p.clip_10 = clip.reference;
        for (unsigned j = 0; j < count; ++j) {
            const size_t record = base+8+size_t(j)*16;
            const uint32_t node = (uint32_t(bytes[record+10])<<8)|bytes[record+11];
            auto pose = prior;
            const auto status = awl::sample_world_map_animation_pose(p,node,&bank,settings,prior,&pose);
            if (bytes[record+13]&31) {
                expect(status == PoseStatus::RequiresKeyedTracks && pose == prior,"every keyed local node stops explicitly"); ++keyed;
            } else {
                expect(status == PoseStatus::Sampled,"every local constant node samples");
                hash(pose_digest,i); hash(pose_digest,node); for (auto w : pose) hash(pose_digest,w); ++sampled;
            }
        }
    }
    std::cout << "LOCAL_CONSTANT_POSES " << sampled << " keyed " << keyed << ' ' << std::hex << pose_digest << std::dec << '\n';
    expect(sampled == 3649 && keyed == 3287 && pose_digest == 0x78df62f236ac030eull,
        "all primary constant poses match independently executed DOL bodies; keyed coverage remains explicit");
}
} // namespace
int main(int argc, char** argv) {
    test_bank(); test_channel(); test_partial_channel(); test_matrix(); test_constant_poses();
    if (argc == 3 && std::string(argv[1]) == "--animation-bank-local") local_bank(argv[2]);
    else if (argc != 1) expect(false, "usage: --animation-bank-local <disc>");
    return failures == 0 ? 0 : 1;
}
