#include "awl/world_map_player_actor_prepare.h"
#include <cfenv>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
using G = awl::WorldMapPlayerActorGateStatus;
using V = awl::WorldMapPlayerActorVisibilityStatus;
int failures = 0;
void expect(bool yes, const char* message) { if (!yes) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
void hash(uint64_t& digest, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) { digest ^= (value >> (24-i*8)) & 255; digest *= 1099511628211ull; }
}
uint32_t bits(float value) { uint32_t word; std::memcpy(&word, &value, 4); return word; }
awl::WorldMapPlayerActorVisibilityOwner broad_view() {
    awl::WorldMapPlayerActorVisibilityOwner owner;
    owner.view = awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};
    for (size_t i = 0; i < 6; ++i) {
        awl::WorldMapPlayerActorPlane plane;
        plane.point[i/2] = i%2 ? -10.0f : 10.0f;
        plane.normal[i/2] = i%2 ? -1.0f : 1.0f;
        owner.planes[i] = plane;
    }
    return owner;
}
void script_comparisons() {
    constexpr uint32_t widths[]{0,1,7,8,9,16,31,32}, active[]{0,1,0x80000000,0xffffffff};
    uint64_t digest = 14695981039346656037ull;
    for (uint32_t seed = 0; seed < 512; ++seed) {
        std::optional<awl::WorldMapPlayerActorScriptOwner> script(std::in_place);
        script->flag_width_4 = widths[seed%8]; script->flag_bytes.resize(4);
        for (uint32_t i = 0; i < 4; ++i) script->flag_bytes[i] = seed/8%3 == 0 ? uint8_t(seed*37+i*71) : 0;
        script->records.emplace(); const auto count = seed/24%4;
        for (uint32_t i = 0; i < count; ++i)
            script->records->push_back({i || seed/96%3 == 0 ? 0 : 17, active[(seed/3+i)%4]});
        const auto result = awl::prepare_world_map_player_state29_actor_gate(uint8_t(0), script);
        expect(result == G::Ready || result == G::BlockedByScript, "complete script observation produces a predicate");
        hash(digest,seed); hash(digest,*script->flag_width_4);
        for (auto byte : script->flag_bytes) hash(digest,byte);
        hash(digest,count);
        for (const auto& record : *script->records) { hash(digest,uint32_t(record.actor_code_d0)); hash(digest,*record.active_word_78); }
        hash(digest,result == G::BlockedByScript ? 1u : 0u);
    }
    expect(digest == 0x37234C70008E952Eull, "512 original flag-reader/first-match script predicate comparisons");
    std::cout << "PLAYER_ACTOR_SCRIPT_COMPARISONS 512 " << std::hex << digest << std::dec << '\n';
    digest = 14695981039346656037ull;
    for (uint32_t seed = 0; seed < 64; ++seed) {
        constexpr uint8_t values[]{0,1,128,255}; const auto saved = values[seed%4];
        const uint32_t control = seed*0x10204081u;
        const auto result = awl::prepare_world_map_player_state29_actor_gate(saved, {});
        expect(result == (saved ? G::RequiresAuxiliaryCallback : G::RequiresScriptOwner), "normal state29 auxiliary decision precedes script ownership");
        // Entry's control store is a reference observation, not a mutation
        // acknowledged by this side-effect-free gate helper.
        for (auto word : {seed,uint32_t(saved),control,saved ? 0x80031618u : 0x80109120u,control}) hash(digest,word);
    }
    expect(digest == 0x9EA41A0255B7EF95ull, "64 original normal-state29 pre-script entry comparisons");
    std::cout << "PLAYER_STATE29_ACTOR_ENTRY_COMPARISONS 64 " << std::hex << digest << std::dec << '\n';
}
void script_dependencies() {
    std::optional<awl::WorldMapPlayerActorScriptOwner> script;
    expect(awl::prepare_world_map_player_state29_actor_gate({},script) == G::RequiresSavedFlag, "unknown saved flag stops first");
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::RequiresScriptOwner, "clear saved flag still needs independent script owner");
    script.emplace();
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::RequiresFlagWidth, "unknown bit-array width stops");
    script->flag_width_4 = 33;
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::UnsupportedFlagWidth, "unsupported width does not invent clear flags");
    script->flag_width_4 = 9; script->flag_bytes = {1};
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::RequiresFlagBytes, "nonzero flag prefix does not bypass a later reached byte");
    script->flag_bytes.push_back(0);
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::BlockedByScript, "nonzero complete flags bypass unknown list");
    script->flag_width_4 = 0; script->flag_bytes.clear();
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::RequiresScriptList, "zero width reads no bytes and reaches list");
    script->records.emplace(); script->records->push_back({17,{}});
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::Ready, "unmatched record's unknown active word is unread");
    script->records->push_back({0,{}});
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::RequiresRecordState, "first matched active word must be known");
    script->records->back().active_word_78 = 0; script->records->push_back({0,1});
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::Ready, "first matching inactive record wins over later active duplicate");
    (*script->records)[1].active_word_78 = 0x80000000;
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::BlockedByScript, "entire active word is tested, not its low byte");
    script->flag_width_4 = 1; script->flag_bytes = {0xfe};
    script->records->clear();
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(0),script) == G::Ready, "unused high bits of final byte do not block");
    expect(awl::prepare_world_map_player_state29_actor_gate(uint8_t(255),script) == G::RequiresAuxiliaryCallback, "nonzero saved flag cannot bypass untranslated auxiliary virtual");
}
void visibility_comparisons() {
    constexpr awl::WorldMapModelMatrix views[]{
        {1,0,0,0,0,1,0,0,0,0,1,0}, {0,0,1,3,0,1,0,-2,-1,0,0,1},
        {2,0,0,-4,0,-1,0,2,0,0,.5f,0}, {.25f,-.5f,.75f,.125f,.5f,1,-.25f,-.375f,-1,.25f,.5f,1.25f}};
    constexpr float radii[]{0,.5f,1,2,4};
    uint64_t digest = 14695981039346656037ull; uint32_t routes[7]{};
    for (uint32_t seed = 0; seed < 512; ++seed) {
        awl::WorldMapPlayerActorSphere sphere{{float(int(seed%17)-8)*.25f,float(int(seed/17%11)-5)*.25f,float(int(seed%7)-3)*.5f},radii[seed%5]};
        awl::WorldMapPlayerActorVisibilityOwner owner; owner.view = views[seed/5%4];
        for (uint32_t i = 0; i < 6; ++i) {
            awl::WorldMapPlayerActorPlane plane; const float sign = i%2 ? -1.0f : 1.0f;
            plane.point[i/2] = float((i%2 ? -1 : 1)*int(seed/20%5))*.5f; plane.normal[i/2] = sign; owner.planes[i] = plane;
        }
        const auto result = awl::prepare_world_map_player_actor_visibility(sphere,owner);
        expect(result.status == V::Visible || result.status == V::Culled, "complete supported visibility observations produce a decision");
        const auto index = result.plane_index.value_or(6); if (index < 7) ++routes[index];
        hash(digest,seed); for (auto value : sphere.center) hash(digest,bits(value)); hash(digest,bits(sphere.radius));
        for (auto value : *owner.view) hash(digest,bits(value));
        for (const auto& plane : owner.planes) {
            for (auto value : plane->point) hash(digest,bits(value)); for (auto value : plane->normal) hash(digest,bits(value));
        }
        hash(digest,result.status == V::Visible ? 1u : 0u); hash(digest,index);
    }
    expect(digest == 0x2AA43E09C41C2678ull, "512 original view/subtraction/dot/six-plane comparisons");
    constexpr uint32_t expected[]{124,113,12,18,20,1,224};
    for (size_t i = 0; i < 7; ++i) expect(routes[i] == expected[i], "reference cases cover every first rejection and all-plane visibility");
    std::cout << "PLAYER_ACTOR_VISIBILITY_COMPARISONS 512 " << std::hex << digest << std::dec << '\n';
}
void visibility_dependencies() {
    auto owner = broad_view(); awl::WorldMapPlayerActorSphere sphere{{0,0,0},1};
    const auto check = [&](V status, std::optional<uint32_t> plane = {}) {
        const auto result = awl::prepare_world_map_player_actor_visibility(sphere,owner);
        expect(result.status == status && result.plane_index == plane, "visibility dependency/boundary preserves exact stop reason and index");
    };
    check(V::Visible); sphere.center[0] = 11; check(V::Culled,0); // Tangency rejects.
    sphere.center[0] = std::nextafter(11.0f,0.0f); check(V::Visible);
    sphere.center[0] = std::nextafter(11.0f,20.0f); check(V::Culled,0);
    owner.planes[1].reset(); check(V::Culled,0); // Unreached plane stays unread.
    sphere.center[0] = 0; check(V::RequiresPlane,1); owner = broad_view();
    owner.planes[5]->normal[0] = std::numeric_limits<float>::quiet_NaN(); check(V::InvalidInput,5);
    owner = broad_view(); owner.planes[5]->normal[0] = std::numeric_limits<float>::denorm_min(); check(V::UnsupportedNumerics,5);
    owner = broad_view(); (*owner.view)[0] = std::numeric_limits<float>::max(); sphere.center[0] = 2; check(V::UnsupportedNumerics);
    owner = broad_view(); sphere.center[0] = std::numeric_limits<float>::denorm_min(); check(V::UnsupportedNumerics);
    sphere.center[0] = 0; sphere.radius = -1; check(V::InvalidInput); sphere.radius = 1;
    owner.view.reset(); check(V::RequiresView); owner = broad_view();
    const auto rounding = std::fegetround(); expect(std::fesetround(FE_UPWARD) == 0,"set unsupported visibility rounding");
    check(V::UnsupportedNumerics); expect(std::fegetround() == FE_UPWARD,"visibility does not change caller rounding");
    expect(std::fesetround(rounding) == 0,"restore visibility rounding");
    // An independent cancellation example distinguishes the original
    // (m00*x + m02*z) + (m01*y + translation) order from a serial dot.
    owner = broad_view(); owner.view = awl::WorldMapModelMatrix{16777216,1,-16777216,0,0,1,0,0,0,0,1,0};
    sphere = {{1,1,1},0}; owner.planes[0] = awl::WorldMapPlayerActorPlane{{.5f,0,0},{1,0,0}};
    check(V::Culled,0);
}
}
int main() {
    script_comparisons(); script_dependencies(); visibility_comparisons(); visibility_dependencies();
    return failures ? 1 : 0;
}
