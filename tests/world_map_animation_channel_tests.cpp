#include "awl/world_map_animation_channel.h"

#include <array>
#include <algorithm>
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
    Channel channel{1, 2, 2, 3, 4, -3, 1};
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
        channel = {2, 2, 2, 3, 0, -3, mode}; pool = records();
        expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
            step.branch == Branch::Completed && step.after.mode_18 == 1 && step.after.blend_14 == -3 &&
            step.records_after[2].state.position_0 == 10, "unsigned completed gate precedes prior mode and does not read unused older record");
    }
    channel = {0, 2, 2, 3, 0, -3, 0};
    expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
        step.branch == Branch::First && step.after.blend_14 == -3, "first transition copies model and preserves blend scalar");
    pool = records(false); channel = {0, 0, 2, 0, 0, -3, 99};
    expect(awl::prepare_world_map_animation_channel(channel, pool, setup, model, &bank, &step) == Status::Prepared &&
        step.branch == Branch::NoClip && step.after.mode_18 == 0 && step.after.blend_14 == -3 &&
        same(step.records_after[2].state, pool[2].state), "no model clip skips clock gates and all copies, preserving unused records");
    channel = {1, 2, 2, 3, 4, -3, 1}; pool = records();
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
    const Channel no_clip{0,0,2,0,0,-3,99};
    expect(awl::prepare_world_map_partial_animation_channel(no_clip,partial,setup,binding,&bank,&step)==Status::Prepared &&
        step.branch==Branch::NoClip && !step.records_after[0].state.word_8 && !step.records_after[0].state.limit_c &&
        step.records_after[1].state.word_8==0u && step.records_after[1].state.limit_c==-2 &&
        step.records_after[1].state.rate_4==21 && step.records_after[1].state.link_14==25 &&
        !step.records_after[1].state.value_18 && !step.records_after[1].state.complete(),
        "no-clip reads no unknown model fields; target initialization preserves unknown weight and known rate/link");
    partial=pool(true);partial[0].state.word_8.reset();partial[0].state.limit_c.reset();
    const Channel completed{0,0,2,3,4,-3,2};
    expect(awl::prepare_world_map_partial_animation_channel(completed,partial,setup,binding,&bank,&step)==Status::RequiresPlaybackFields &&
        step.required_record==1 && step.required_fields==1 && step.after.elapsed_0==0 &&
        step.records_after[2].state.position_0==30,"first reached unknown source word blocks before limit and preserves copies");
    partial[0].state.word_8=12;
    expect(awl::prepare_world_map_partial_animation_channel(completed,partial,setup,binding,&bank,&step)==Status::RequiresPlaybackFields &&
        step.required_record==1 && step.required_fields==2,"known source word exposes the later unknown source limit");
    partial=pool(true);partial[1].state.word_8.reset();
    const Channel interrupted{1,2,2,3,4,-3,1};
    expect(awl::prepare_world_map_partial_animation_channel(interrupted,partial,setup,binding,&bank,&step)==Status::RequiresPlaybackFields &&
        step.required_record==2 && step.required_fields==1 && step.records_after[3].state.position_0==40 &&
        step.after.blend_14==-3,"late unknown copy source rolls back the earlier previous-to-older copy");
    partial=pool(true);partial[1].state.value_18.reset();partial[2].state.value_18.reset();
    expect(awl::prepare_world_map_partial_animation_channel(interrupted,partial,setup,binding,&bank,&step)==Status::Prepared &&
        step.records_after[3].state.value_18==0 && step.records_after[2].state.value_18==0 &&
        !step.records_after[1].state.value_18 && step.records_after[2].state.complete(),
        "FECC resets unknown source weights without reading them while FE08 retains target unknown weight");
    partial=pool(true);partial[0].state.value_18.reset();
    const Channel all_alias{0,0,1,1,1,-3,2};
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
            const Channel channel{clock[0],clock[1],layout[1],layout[2],layout[3],-3,mode};
            const Setup setup{100,200,UINT32_MAX,blend,0,-0.0f}; Step step;
            const auto status = awl::prepare_world_map_animation_channel(channel,pool,setup,Binding{100,layout[0]},&bank,&step);
            expect(status == Status::Prepared && step.branch && same(pool[0].state,playback(10,active != 0)), "matrix preparation preserves input");
            if (status != Status::Prepared || !step.branch) continue;
            for (uint32_t word : {active,clock[0],clock[1],mode,static_cast<uint32_t>(layout[1]),
                static_cast<uint32_t>(layout[2]),static_cast<uint32_t>(layout[3]),blend,static_cast<uint32_t>(*step.branch),
                step.after.elapsed_0,step.after.duration_4,bits(step.after.blend_14),step.after.mode_18}) hash(digest,word);
            for (const auto& record : step.records_after) hash_playback(digest,record);
            ++cases;
        }
    }
    std::cout << "CHANNEL_MATRIX " << cases << ' ' << std::hex << digest << std::dec << '\n';
    // Expected digest is independently obtained from the mapped DOL listing.
    expect(cases == 1536 && digest == 0xab9e66bb9f113eadull, "channel matrix matches independent instruction probe");
}
void local_bank(const std::filesystem::path& disc) {
    std::ifstream input(disc / "files" / "boy_0.anm.arc", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    Bank bank;
    expect(bank.parse(200, std::move(bytes)) && bank.is_archive() && bank.clip_count() == 126,
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
}
} // namespace
int main(int argc, char** argv) {
    test_bank(); test_channel(); test_partial_channel(); test_matrix();
    if (argc == 3 && std::string(argv[1]) == "--animation-bank-local") local_bank(argv[2]);
    else if (argc != 1) expect(false, "usage: --animation-bank-local <disc>");
    return failures == 0 ? 0 : 1;
}
