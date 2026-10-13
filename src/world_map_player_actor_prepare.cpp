#include "awl/world_map_player_actor_prepare.h"
#include <cfenv>
#include <cmath>

namespace awl {
WorldMapPlayerActorGateStatus prepare_world_map_player_state29_actor_gate(
    std::optional<uint8_t> saved,const std::optional<WorldMapPlayerActorScriptOwner>& script) noexcept {
    return prepare_world_map_player_normal_actor_gate(saved,script);
}
WorldMapPlayerActorGateStatus prepare_world_map_player_normal_actor_gate(
    std::optional<uint8_t> saved, const std::optional<WorldMapPlayerActorScriptOwner>& script) noexcept {
    using S = WorldMapPlayerActorGateStatus;
    if (!saved) return S::RequiresSavedFlag;
    if (*saved) return S::RequiresAuxiliaryCallback;
    if (!script) return S::RequiresScriptOwner;
    if (!script->flag_width_4) return S::RequiresFlagWidth;
    const auto width = *script->flag_width_4;
    if (width > 32) return S::UnsupportedFlagWidth;
    uint32_t flags = 0;
    // Index zero starts at bit zero. Consume each reached byte, including
    // bytes after a nonzero prefix, before 109120 branches on the result.
    for (uint32_t bit = 0; bit < width; bit += 8) {
        const auto byte = bit / 8;
        if (byte >= script->flag_bytes.size()) return S::RequiresFlagBytes;
        const auto count = width - bit < 8 ? width - bit : 8;
        flags |= (uint32_t(script->flag_bytes[byte]) & ((1u << count) - 1u)) << bit;
    }
    if (flags) return S::BlockedByScript;
    if (!script->records) return S::RequiresScriptList;
    for (const auto& record : *script->records) if (record.actor_code_d0 == 0) {
        if (!record.active_word_78) return S::RequiresRecordState;
        return *record.active_word_78 ? S::BlockedByScript : S::Ready;
    }
    return S::Ready;
}

namespace {
bool ordinary(float value) noexcept {
    return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL;
}
bool finite(const std::array<float,3>& values) noexcept {
    for (float value : values) if (!std::isfinite(value)) return false;
    return true;
}
bool ordinary(const std::array<float,3>& values) noexcept {
    for (float value : values) if (!ordinary(value)) return false;
    return true;
}
bool transform_center(const std::array<float,3>& center, const WorldMapModelMatrix& m,
    std::array<float,3>* transformed) noexcept {
    for (size_t row = 0; row < 3; ++row) {
        const float x = m[row*4] * center[0], y = m[row*4+1] * center[1];
        const float xz = std::fma(m[row*4+2], center[2], x);
        const float yt = m[row*4+3] + y;
        (*transformed)[row] = xz + yt;
        if (!ordinary(x) || !ordinary(y) || !ordinary(xz) || !ordinary(yt) || !ordinary((*transformed)[row])) return false;
    }
    return true;
}
bool length(const std::array<float,3>& vector, float* out) noexcept {
    const float x = vector[0]*vector[0], y = vector[1]*vector[1];
    const float zx = std::fma(vector[2],vector[2],x), squared = zx+y;
    if (!ordinary(x) || !ordinary(y) || !ordinary(zx) || !ordinary(squared)) return false;
    if (squared == 0) { *out = squared; return true; }
    const double seed = 1.0/std::sqrt(static_cast<double>(squared));
    const float seed_squared = static_cast<float>(seed*seed), half_seed = static_cast<float>(seed*0.5);
    const float correction = -std::fma(seed_squared,squared,-3.0f);
    const float inverse = correction*half_seed, result = squared*inverse;
    if (!ordinary(seed_squared) || !ordinary(half_seed) || !ordinary(correction) || !ordinary(inverse) || !ordinary(result)) return false;
    *out = result; return true;
}
}
WorldMapPlayerActorBoundsStatus prepare_world_map_player_normal_actor_bounds(
    const std::array<float,3>& position, WorldMapPlayerActorBounds* out) noexcept {
    using S = WorldMapPlayerActorBoundsStatus;
    if (!out || !finite(position)) return S::InvalidInput;
    if (std::fegetround() != FE_TONEAREST || !ordinary(position)) return S::UnsupportedNumerics;
    constexpr float low_offset[]{-1,0,-1}, high_offset[]{1,2,1};
    WorldMapPlayerActorBounds next;
    std::array<float,3> half;
    for (size_t axis = 0; axis < 3; ++axis) {
        float low = position[axis]+low_offset[axis], high = position[axis]+high_offset[axis];
        if (!ordinary(low) || !ordinary(high)) return S::UnsupportedNumerics;
        if (low > high) { const auto prior = low; low = high; high = prior; }
        const float span = high-low;
        half[axis] = span*0.5f; next.sphere.center[axis] = low+half[axis];
        if (!ordinary(span) || !ordinary(half[axis]) || !ordinary(next.sphere.center[axis])) return S::UnsupportedNumerics;
    }
    if (!length(half,&next.sphere.radius)) return S::UnsupportedNumerics;
    next.height_10 = 2.0f*half[1]; half[1] = 0;
    if (!ordinary(next.height_10) || !length(half,&next.horizontal_radius_14)) return S::UnsupportedNumerics;
    *out = next; return S::Prepared;
}
WorldMapPlayerActorVisibilityResult prepare_world_map_player_actor_visibility(
    const WorldMapPlayerActorSphere& sphere, const WorldMapPlayerActorVisibilityOwner& owner) noexcept {
    using S = WorldMapPlayerActorVisibilityStatus;
    if (!std::isfinite(sphere.radius) || sphere.radius < 0 || !finite(sphere.center)) return {};
    if (!owner.view) return {S::RequiresView};
    for (float value : *owner.view) if (!std::isfinite(value)) return {};
    if (std::fegetround() != FE_TONEAREST || !ordinary(sphere.radius) || !ordinary(sphere.center))
        return {S::UnsupportedNumerics};
    for (float value : *owner.view) if (!ordinary(value)) return {S::UnsupportedNumerics};
    std::array<float,3> transformed;
    if (!transform_center(sphere.center,*owner.view,&transformed)) return {S::UnsupportedNumerics};
    for (uint32_t i = 0; i < owner.planes.size(); ++i) {
        if (!owner.planes[i]) return {S::RequiresPlane, i};
        const auto& plane = *owner.planes[i];
        if (!finite(plane.point) || !finite(plane.normal)) return {S::InvalidInput, i};
        if (!ordinary(plane.point) || !ordinary(plane.normal)) return {S::UnsupportedNumerics, i};
        std::array<float,3> delta;
        for (size_t axis = 0; axis < 3; ++axis) delta[axis] = transformed[axis] - plane.point[axis];
        const float y = delta[1] * plane.normal[1], z = delta[2] * plane.normal[2];
        const float xy = std::fma(delta[0], plane.normal[0], y);
        const float dot = xy + z;
        if (!ordinary(delta) || !ordinary(y) || !ordinary(z) || !ordinary(xy) || !ordinary(dot))
            return {S::UnsupportedNumerics, i};
        if (dot >= sphere.radius) return {S::Culled, i};
    }
    return {S::Visible};
}
WorldMapPlayerActorScreenResult prepare_world_map_player_actor_screen(
    const WorldMapPlayerActorSphere& sphere, const WorldMapPlayerActorScreenOwner& owner) noexcept {
    using S = WorldMapPlayerActorScreenStatus;
    if (!std::isfinite(sphere.radius) || sphere.radius < 0 || !finite(sphere.center)) return {};
    if (!owner.view) return {S::RequiresView};
    for (float value : *owner.view) if (!std::isfinite(value)) return {};
    if (std::fegetround() != FE_TONEAREST || !ordinary(sphere.radius) || !ordinary(sphere.center)) return {S::UnsupportedNumerics};
    for (float value : *owner.view) if (!ordinary(value)) return {S::UnsupportedNumerics};
    std::array<float,3> transformed;
    if (!transform_center(sphere.center,*owner.view,&transformed)) return {S::UnsupportedNumerics};
    for (uint32_t axis = 0; axis < 2; ++axis) {
        if (!owner.half_extents[axis]) return {S::RequiresExtent,axis};
        const float extent = *owner.half_extents[axis];
        if (!std::isfinite(extent)) return {S::InvalidInput,axis};
        if (!ordinary(extent)) return {S::UnsupportedNumerics,axis};
        const float upper = transformed[axis]+sphere.radius;
        if (!ordinary(upper)) return {S::UnsupportedNumerics,axis};
        if (upper <= -extent) return {S::Outside,axis};
        const float lower = transformed[axis]-sphere.radius;
        if (!ordinary(lower)) return {S::UnsupportedNumerics,axis};
        if (lower >= extent) return {S::Outside,axis};
    }
    return {S::Inside};
}
} // namespace awl
