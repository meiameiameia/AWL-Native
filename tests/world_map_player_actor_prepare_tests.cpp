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
std::array<uint32_t,6> bounds_words(const awl::WorldMapPlayerActorBounds& bounds) {
    return {bits(bounds.sphere.center[0]),bits(bounds.sphere.center[1]),bits(bounds.sphere.center[2]),
        bits(bounds.sphere.radius),bits(bounds.height_10),bits(bounds.horizontal_radius_14)};
}
void normal_bounds_checks() {
    using B=awl::WorldMapPlayerActorBoundsStatus;
    constexpr float positions[]{0,-0.0f,1,-1,.1f,-.1f,120.125f,168.375f,8388607.5f,8388608,8388609,16777215,16777216,16777218,33554432,-16777216};
    uint64_t digest=14695981039346656037ull;
    for(uint32_t seed=0;seed<1024;++seed){const std::array<float,3> point{positions[seed%16],positions[seed/16%16],positions[(seed*7+seed/16)%16]};
        awl::WorldMapPlayerActorBounds bounds;
        expect(awl::prepare_world_map_player_normal_actor_bounds(point,&bounds)==B::Prepared,"normal bounds prepare finite supported positions");
        hash(digest,seed);for(auto value:point)hash(digest,bits(value));for(auto word:bounds_words(bounds))hash(digest,word);
    }
    expect(digest==0x67B275E9F29CA1DDull,"1024 original bounds constructions match the declared native reciprocal-sqrt seed profile");
    std::cout<<"PLAYER_NORMAL_BOUNDS_COMPARISONS 1024 "<<std::hex<<digest<<std::dec<<'\n';
    awl::WorldMapPlayerActorBounds bounds;
    expect(awl::prepare_world_map_player_normal_actor_bounds({0,0,0},&bounds)==B::Prepared &&
        bounds_words(bounds)==std::array<uint32_t,6>{0,0x3f800000,0,0x3fddb3d7,0x40000000,0x3fb504f3},
        "normal unit half-extents yield expected center, height and independently mapped native-profile radii");
    expect(awl::prepare_world_map_player_normal_actor_bounds({33554432,33554432,33554432},&bounds)==B::Prepared &&
        bounds.sphere.center==std::array<float,3>{33554432,33554432,33554432} && bounds.sphere.radius==0 && bounds.height_10==0 && bounds.horizontal_radius_14==0,
        "rounded endpoints may collapse all extents; normal bounds cannot use a fixed radius or center offset");
    bounds.sphere.center={10,20,30};
    expect(awl::prepare_world_map_player_normal_actor_bounds(bounds.sphere.center,&bounds)==B::Prepared &&
        bounds.sphere.center==std::array<float,3>{10,21,30},"normal position may alias output center without partial writes");
    const auto before=bounds_words(bounds);
    expect(awl::prepare_world_map_player_normal_actor_bounds({0,std::numeric_limits<float>::infinity(),0},&bounds)==B::InvalidInput && bounds_words(bounds)==before,
        "nonfinite bounds input preserves all prior output words");
    expect(awl::prepare_world_map_player_normal_actor_bounds({0,std::numeric_limits<float>::denorm_min(),0},&bounds)==B::UnsupportedNumerics && bounds_words(bounds)==before,
        "subnormal position rejects without publishing bounds");
    expect(awl::prepare_world_map_player_normal_actor_bounds({0,0,0},nullptr)==B::InvalidInput,"null bounds output rejects");
    const auto rounding=std::fegetround();expect(std::fesetround(FE_UPWARD)==0,"set bounds rounding failure");
    expect(awl::prepare_world_map_player_normal_actor_bounds({0,0,0},&bounds)==B::UnsupportedNumerics && bounds_words(bounds)==before,
        "unsupported rounding preserves prior bounds");expect(std::fesetround(rounding)==0,"restore bounds rounding");
}
void screen_comparisons() {
    using S=awl::WorldMapPlayerActorScreenStatus;
    constexpr awl::WorldMapModelMatrix views[]{
        {1,0,0,0,0,1,0,0,0,0,1,0},{0,0,1,3,0,1,0,-2,-1,0,0,1},
        {2,0,0,-4,0,-1,0,2,0,0,.5f,0},{.25f,-.5f,.75f,.125f,.5f,1,-.25f,-.375f,-1,.25f,.5f,1.25f}};
    constexpr float radii[]{0,.5f,1,2,4},extents[]{0,.5f,1,2,4,-1};
    uint64_t digest=14695981039346656037ull;uint32_t routes[3]{};
    for(uint32_t seed=0;seed<512;++seed){awl::WorldMapPlayerActorSphere sphere{{float(int(seed%17)-8)*.25f,float(int(seed/17%11)-5)*.25f,float(int(seed%7)-3)*.5f},radii[seed%5]};
        awl::WorldMapPlayerActorScreenOwner owner;owner.view=views[seed/5%4];owner.half_extents={extents[seed/20%6],extents[seed/120%6]};
        const auto result=awl::prepare_world_map_player_actor_screen(sphere,owner);const auto axis=result.axis.value_or(2);
        expect(result.status==S::Inside || result.status==S::Outside,"supported screen observations produce an effect predicate");
        if(axis<3)++routes[axis];hash(digest,seed);for(auto value:sphere.center)hash(digest,bits(value));hash(digest,bits(sphere.radius));
        for(auto value:*owner.view)hash(digest,bits(value));for(const auto& value:owner.half_extents)hash(digest,bits(*value));
        hash(digest,result.status==S::Inside?1u:0u);hash(digest,axis);
    }
    expect(digest==0xAFAD9EA15ECC46B1ull && routes[0]==239 && routes[1]==48 && routes[2]==225,
        "512 original separate-view/bounds-getter X/Y comparisons include each outside branch and inside results");
    std::cout<<"PLAYER_ACTOR_SCREEN_COMPARISONS 512 "<<std::hex<<digest<<std::dec<<'\n';
}
void screen_dependencies() {
    using S=awl::WorldMapPlayerActorScreenStatus;
    awl::WorldMapPlayerActorSphere sphere{{0,0,100000},1};awl::WorldMapPlayerActorScreenOwner owner;
    const auto check=[&](S status,std::optional<uint32_t> axis={}){const auto result=awl::prepare_world_map_player_actor_screen(sphere,owner);
        expect(result.status==status && result.axis==axis,"screen boundary/dependency retains ordered stop reason");};
    check(S::RequiresView);owner.view=awl::WorldMapModelMatrix{1,0,0,0,0,1,0,0,0,0,1,0};check(S::RequiresExtent,0);
    owner.half_extents[0]=10.0f;check(S::RequiresExtent,1);owner.half_extents[1]=10.0f;check(S::Inside); // Z is not a screen extent test.
    sphere.center[0]=-11;check(S::Outside,0);sphere.center[0]=std::nextafter(-11.0f,0.0f);check(S::Inside);
    sphere.center[0]=11;check(S::Outside,0);sphere.center[0]=std::nextafter(11.0f,0.0f);check(S::Inside);
    sphere.center[0]=0;sphere.center[1]=-11;check(S::Outside,1);sphere.center[1]=11;check(S::Outside,1);
    sphere.center[0]=11;owner.half_extents[1].reset();check(S::Outside,0);sphere.center[0]=0;check(S::RequiresExtent,1);
    owner.half_extents[1]=std::numeric_limits<float>::quiet_NaN();check(S::InvalidInput,1);
    owner.half_extents[1]=std::numeric_limits<float>::denorm_min();check(S::UnsupportedNumerics,1);
    owner.half_extents[1]=10.0f;sphere.center[1]=0;(*owner.view)[3]=20;check(S::Outside,0); // Separate view affects the decision.
    (*owner.view)[3]=0;sphere.radius=-1;check(S::InvalidInput);sphere.radius=1;
    const auto rounding=std::fegetround();expect(std::fesetround(FE_DOWNWARD)==0,"set screen rounding failure");
    check(S::UnsupportedNumerics);expect(std::fesetround(rounding)==0,"restore screen rounding");
}
}
int main() {
    script_comparisons(); script_dependencies(); visibility_comparisons(); visibility_dependencies();
    normal_bounds_checks();screen_comparisons();screen_dependencies();
    return failures ? 1 : 0;
}
