#include "awl/world_map_update_inputs.h"
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool yes, const char* why) {
    if (!yes) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
void hash(uint64_t& digest, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) { digest ^= (word >> (24 - i * 8)) & 255; digest *= 1099511628211ull; }
}
bool equal(const awl::WorldMapControlWord& a, const awl::WorldMapControlWord& b) {
    return a.loop_count == b.loop_count && a.producer_word == b.producer_word && a.dispatch_word == b.dispatch_word;
}
void control_checks() {
    using S = awl::WorldMapControlStatus;
    awl::WorldMapControlSources sources;
    awl::WorldMapSceneModeRequestState scene;
    awl::WorldMapControlWord out{7,8,9};
    const auto prior = out;
    const auto stopped = [&](S status) {
        expect(awl::prepare_world_map_control_word(0, sources, scene, &out) == status && equal(out, prior),
            "unknown reached source preserves the complete proposal");
    };
    stopped(S::RequiresPhaseOwner);
    sources.phase.emplace(); stopped(S::RequiresPhaseByte8);
    sources.phase->byte_8 = uint8_t{1}; sources.phase->byte_a = uint8_t{255}; stopped(S::RequiresPhaseRaw);
    sources.phase->raw_word_0 = 0; stopped(S::RequiresStageOwner);
    sources.stage_20 = 0; sources.phase->byte_a.reset(); stopped(S::RequiresPhaseByteA);
    expect(awl::prepare_world_map_control_word(0, sources, scene, nullptr) == S::InvalidInput, "null proposal rejects");
    sources.phase = awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{0}}; sources.stage_20 = 4;
    scene.global_flag_59b0 = 255; scene.global_flag_59af = 2;
    expect(awl::prepare_world_map_control_word(6, sources, scene, &out) == S::Prepared && out.producer_word == 0x2981 &&
        out.dispatch_word == 0x2981 && scene.global_flag_59b0 == 255 && scene.global_flag_59af == 2,
        "disabled phase needs no raw word, combines nonzero events and leaves supplied request state untouched");
    sources.phase = awl::WorldMapControlPhase{uint8_t{255},0u,uint8_t{0}}; sources.stage_20 = INT32_MIN; scene = {};
    expect(awl::prepare_world_map_control_word(0, sources, scene, &out) == S::Prepared && out.producer_word == 0x1f && out.dispatch_word == 0xf,
        "nonboolean phase byte enables midnight flags; main-loop mask removes only bit 0x10");
    sources.phase->raw_word_0 = 11;
    expect(awl::prepare_world_map_control_word(1, sources, scene, &out) == S::Prepared && out.dispatch_word == 0,
        "active nonboundary control does not invent the status skip bit");

    uint64_t digest = 14695981039346656037ull; size_t cases = 0;
    // Invented field values, compared with actual DOL producer/getter/mask
    // instructions. The ignored probe has no callback hook in this matrix.
    for (uint32_t raw : {0u,1u,9u,10u,599u,600u,5999u,6000u,35999u,36000u,431999u,432000u,863999u,864000u,UINT32_MAX})
        for (uint32_t loop : {0u,1u,5u,6u,UINT32_MAX})
            for (uint8_t on : {uint8_t{0},uint8_t{1}})
                for (uint8_t override_byte : {uint8_t{0},uint8_t{1}})
                    for (int32_t stage : {0,3,4,5,6})
                        for (uint32_t ones = 0; ones < 4; ++ones) {
                            sources = {awl::WorldMapControlPhase{on,raw,override_byte},stage};
                            scene.global_flag_59b0 = (ones & 1) ? 255 : 0;
                            scene.global_flag_59af = (ones & 2) ? 2 : 0;
                            expect(awl::prepare_world_map_control_word(loop, sources, scene, &out) == S::Prepared && out.loop_count == loop,
                                "complete supplied producer inputs prepare");
                            for (uint32_t word : {raw,loop,uint32_t(on),uint32_t(override_byte),uint32_t(stage),ones,out.producer_word,out.dispatch_word}) hash(digest, word);
                            ++cases;
                        }
    expect(cases == 6000 && digest == 0x23E24AFFA6D91EADull, "6000 producer/getter/main-mask cases match mapped instructions");
    std::cout << "UPDATE_CONTROL_COMPARISONS " << cases << " digest " << std::hex << digest << std::dec << '\n';
}
void group_checks() {
    using S = awl::WorldMapSceneFilterStatus;
    uint32_t mask = 0xabcdef;
    expect(awl::prepare_world_map_dispatch_groups(4, {}, &mask) == S::RequiresSceneByte79 && mask == 0xabcdef,
        "mode four needs its reached scene byte and preserves output");
    expect(awl::prepare_world_map_dispatch_groups(0, {}, &mask) == S::Prepared && mask == 0x1fe0,
        "first predicate never reads unknown scene byte");
    expect(awl::prepare_world_map_dispatch_groups(8, {}, &mask) == S::Prepared && mask == 0x1be0,
        "second predicate never reads unknown scene byte");
    expect(awl::prepare_world_map_dispatch_groups(INT32_MIN, uint8_t{0}, &mask) == S::Prepared && mask == 0,
        "signed outlying mode follows original fallback rather than an invented valid range");
    expect(awl::prepare_world_map_dispatch_groups(INT32_MAX, uint8_t{255}, &mask) == S::Prepared && mask == 0xb20,
        "nonzero fallback byte blocks groups five, eight, nine and eleven");
    expect(awl::prepare_world_map_dispatch_groups(4, uint8_t{0}, nullptr) == S::InvalidInput, "null group output rejects");
    uint64_t digest = 14695981039346656037ull; size_t cases = 0;
    for (int32_t mode = -1; mode < 47; ++mode) for (uint8_t flag : {uint8_t{0},uint8_t{1},uint8_t{255}}) {
        mask = 0xffffff;
        expect(awl::prepare_world_map_dispatch_groups(mode, flag, &mask) == S::Prepared, "complete scene group inputs prepare");
        for (uint32_t word : {uint32_t(mode),uint32_t(flag),mask}) hash(digest, word);
        ++cases;
    }
    expect(cases == 144 && digest == 0x3A824B58AC4EF2F8ull, "144 complete clear/mode/group-write cases match mapped instructions");
    std::cout << "UPDATE_GROUP_COMPARISONS " << cases << " digest " << std::hex << digest << std::dec << '\n';
}
void owner_checks() {
    using C = awl::WorldMapControlStatus; using F = awl::WorldMapSceneFilterStatus; using D = awl::WorldMapDispatchStatus;
    awl::WorldMapUpdateInputs missing({});
    expect(missing.capture_control() == C::RequiresClock && missing.dispatch(0x5001).status == D::RequiresControl, "clock owner cannot be guessed");
    auto clock = std::make_shared<awl::GameClock>(); const auto lifetime = std::weak_ptr<awl::GameClock>(clock);
    awl::WorldMapUpdateInputs owner(clock);
    expect(owner.blocked_groups() == 0 && !owner.control() && owner.scene().previous_mode_5c == -1 &&
        owner.capture_control() == C::RequiresPhaseOwner, "fresh owner retains constructor fields without inventing phase or stage");
    owner.request_scene_mode(4, 9);
    expect(owner.scene().global_flag_59af == 1 && owner.scene().global_flag_59b0 == 0 && owner.blocked_groups() == 0,
        "request sets real event fields without prematurely rebuilding dispatch groups");
    awl::WorldMapSceneModeCommit commit{true,true,77,88};
    expect(owner.commit_scene_fields(0x1234, {}, &commit) == F::RequiresSceneByte79 && owner.scene().mode_58 == 4 &&
        owner.scene().previous_mode_5c == -1 && owner.scene().state_64 == 1 && owner.scene().transition_flags_7c == 0 &&
        owner.blocked_groups() == 0 && commit.mode == 77 && commit.scene_type == 88 && commit.swapped,
        "late unknown scene byte rolls back pending/current fields, filter and output");
    expect(owner.commit_scene_fields(0x1234, uint8_t{255}, &commit) == F::CommittedFields && commit.transition && !commit.swapped && commit.mode == 4 &&
        owner.scene().mode_58 == -1 && owner.scene().transition_flags_7c == 0x1234 && owner.blocked_groups() == 0xb20,
        "field commit selects mode-four blocked groups from nonzero +79");
    owner.request_scene_mode(6, 0);
    expect(owner.scene().global_flag_59b0 == 1 && owner.scene().global_flag_59af == 1, "classified prior/current request latches both event bytes");
    owner.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},{}},5});
    expect(owner.capture_control() == C::RequiresPhaseByteA && !owner.control() && owner.scene().global_flag_59b0 == 1 && owner.scene().global_flag_59af == 1,
        "late unknown override cannot consume either owned event");
    owner.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{255}},5});
    expect(owner.capture_control() == C::Captured && owner.control()->producer_word == 0x800 && owner.control()->dispatch_word == 0x800 &&
        owner.scene().global_flag_59b0 == 0 && owner.scene().global_flag_59af == 0,
        "override replaces accumulated events but successful capture still consumes both");
    expect(owner.dispatch(0x5001).status == D::Filtered && !owner.dispatch(0x5001).control_word &&
        owner.dispatch(0).control_word == 0x800u, "group five filters without exposing callback control; other groups keep the same captured word");
    owner.request_scene_mode(6, 1);
    owner.observe_control_sources({awl::WorldMapControlPhase{uint8_t{1},11u,uint8_t{0}},5});
    expect(owner.capture_control() == C::AlreadyCaptured && owner.control()->dispatch_word == 0x800 && owner.scene().global_flag_59af == 1 &&
        owner.scene().global_flag_59b0 == 1, "same-loop recapture cannot replace control or consume a newer request");
    expect(owner.commit_scene_fields(0x55, uint8_t{0}, &commit) == F::CommittedFields && owner.blocked_groups() == 0 && owner.dispatch(0x5001).control_word == 0x800u,
        "mid-loop scene commit changes filter while retaining the earlier captured control");
    expect(owner.commit_scene_fields(0, {}, &commit) == F::Unchanged && !commit.transition && owner.blocked_groups() == 0 &&
        owner.scene().transition_flags_7c == 0x55, "no pending request preserves mask/flags without requiring +79");
    expect(clock->advance() && owner.dispatch(0x5001).status == D::RequiresControl && !owner.dispatch(0x5001).control_word,
        "advanced shared clock makes prior control unusable for dispatch");
    expect(owner.capture_control() == C::Captured && owner.control()->loop_count == 1 && owner.control()->dispatch_word == 0x2140 &&
        owner.scene().global_flag_59af == 0 && owner.scene().global_flag_59b0 == 0,
        "next loop captures late phase/stage observations and newer requests exactly once");
    for (uint32_t group = 0; group < 24; ++group)
        expect(owner.dispatch((group << 12) | 0xfff).status == D::Ready && owner.dispatch(group << 12).control_word == 0x2140u,
            "all constructor-supported priority keys preserve low-word independence and shared control");
    for (uint32_t priority : {0x18000u,0x1ffffu,0x80000000u,UINT32_MAX})
        expect(owner.dispatch(priority).status == D::UnsupportedPriority && !owner.dispatch(priority).control_word,
            "out-of-storage unsigned keys stop before any invalid shift or bitmap read");
    owner.request_scene_mode(0, 0);
    expect(owner.commit_scene_fields(1, {}, &commit) == F::CommittedFields && owner.blocked_groups() == 0x1fe0 && owner.dispatch(0x5001).status == D::Filtered,
        "mode predicate commits without unrelated byte observation");
    owner.request_scene_mode(-1, 0);
    expect(owner.commit_scene_fields(0, uint8_t{0}, &commit) == F::Unchanged && owner.blocked_groups() == 0x1fe0,
        "a no-transition commit never clears a previously blocked mask");
    const auto prior = *owner.control();
    expect(clock->advance(), "test advances only its owned clock");
    owner.observe_control_sources({});
    expect(owner.capture_control() == C::RequiresPhaseOwner && equal(*owner.control(), prior) && owner.scene().global_flag_59af == 1 &&
        owner.dispatch(0x5001).status == D::RequiresControl, "failed next-loop capture preserves last diagnostics and pending event without dispatching stale control");
    owner.observe_control_sources({awl::WorldMapControlPhase{uint8_t{0},{},uint8_t{0}},0});
    expect(owner.capture_control() == C::Captured && owner.control()->dispatch_word == 0x2800 && owner.scene().global_flag_59af == 0,
        "successful retry consumes the preserved event once");
    owner.request_scene_mode(-2, 0);
    expect(owner.commit_scene_fields(2, uint8_t{0}, &commit) == F::CommittedFields && commit.swapped && commit.mode == 6 && owner.blocked_groups() == 0,
        "swap selects the restored committed mode for the new mask");
    expect(owner.commit_scene_fields(0, {}, nullptr) == F::InvalidInput && owner.blocked_groups() == 0, "null commit output preserves owner");
    clock.reset();
    expect(!lifetime.expired() && owner.game_clock() && owner.dispatch(0x5001).control_word == 0x2800u,
        "input owner retains the shared clock after caller handle release");
}
} // namespace
int main() {
    control_checks(); group_checks(); owner_checks();
    return failures ? 1 : 0;
}
