#include "awl/world_map_player_status.h"
#include <cfenv>
#include <cmath>

namespace awl {
namespace {
using G = WorldMapPlayerStatusGlobal;
using S = WorldMapPlayerStatusCode;
int64_t signed_word(uint32_t value) noexcept {
    return value <= INT32_MAX ? int64_t(value) : int64_t(value) - 0x100000000ll;
}
uint32_t divide(uint32_t numerator, uint32_t denominator) noexcept {
    // Denominators are positive/nonzero, including C7E8's factor * 20000
    // with the verified elapsed factor bounded to [100,150].
    return static_cast<uint32_t>(signed_word(numerator) / denominator);
}
uint32_t clamp(uint32_t value, uint32_t minimum, uint32_t maximum) noexcept {
    if (signed_word(value) < signed_word(minimum)) return minimum;
    if (signed_word(maximum) < signed_word(value)) return maximum;
    return value;
}
class Calculation {
public:
    explicit Calculation(const WorldMapPlayerStatusInputs& source) : inputs(source) {}
    WorldMapPlayerStatusStep step;
    bool get(G key, uint32_t& value) noexcept {
        const auto& observed = inputs.globals[static_cast<size_t>(key)];
        if (!observed) { step.status = S::RequiresGlobal; step.required_global = key; return false; }
        value = *observed; return true;
    }
    bool elapsed(const WorldMapPlayerStatusState& state, uint32_t& factor) noexcept {
        if (!inputs.phase_raw) { step.status = S::RequiresPhaseRaw; return false; }
        step.status = prepare_world_map_player_status_elapsed(state.word_12c, *inputs.phase_raw, &factor);
        return step.status == S::Prepared;
    }
    bool cc14(const WorldMapPlayerStatusState& s, uint32_t& out) noexcept {
        uint32_t a, b;
        if (!get(G::Word7ec0, a)) return false;
        out = 100;
        if (signed_word(s.word_4) <= signed_word(a)) {
            if (!get(G::Word7ebc, b)) return false;
            out = divide(0u - b * s.word_4, 100000) + 500;
        }
        return true;
    }
    bool cca4(const WorldMapPlayerStatusState& s, uint32_t& out) noexcept {
        uint32_t a, b;
        if (!get(G::Word7e9c, a)) return false;
        if (signed_word(s.word_20 * a) < 2499975) {
            if (!get(G::Word7e98, b)) return false;
            out = divide(s.word_20 * b * 100, 2499975) + 100;
        } else {
            if (!get(G::Word7e94, a)) return false;
            if (signed_word(s.word_20 * a) <= 7499925) {
                if (!get(G::Word7e90, b)) return false;
                out = divide(b * s.word_20 * 50 + 0x990000u - 0x69e4u, 49999);
            } else {
                if (!get(G::Word7e8c, b)) return false;
                out = divide(s.word_20 * b - 2499975, 24999);
            }
        }
        return true;
    }
    bool cd30(const WorldMapPlayerStatusState& s, uint32_t& out) noexcept {
        uint32_t a, b;
        if (!get(G::Word7e88, a)) return false;
        if (signed_word(s.word_20 * a) < 2499975) {
            out = divide(0u - s.word_20 * 50 + 2499975, 24999);
        } else {
            if (!get(G::Word7e84, a)) return false;
            if (signed_word(s.word_20 * a) < 7499925) {
                if (!get(G::Word7e80, b)) return false;
                out = divide(b * s.word_20 * 50, 49999);
            } else out = divide(0u - s.word_20 * 50 + 7499925, 24999);
        }
        return true;
    }
private:
    const WorldMapPlayerStatusInputs& inputs;
};
} // namespace
WorldMapPlayerStatusCode prepare_world_map_player_status_elapsed(
    uint32_t saved_raw, uint32_t phase_raw, uint32_t* out) noexcept {
    if (!out) return S::InvalidInput;
    // 10294 wraps before the unsigned minute/second getters. CDB8 saturates
    // 216000 - elapsedSeconds at zero, then takes the curve at >= 57600.
    const uint32_t seconds = (saved_raw - phase_raw) / 10;
    const uint32_t remaining = seconds < 216000 ? 216000 - seconds : 0;
    if (remaining < 57600) { *out = 100; return S::Prepared; }
    if (std::fegetround() != FE_TONEAREST) return S::UnsupportedNumerics;
    // 8004CE50..CEC4: retain each original single-precision boundary.
    // The native double sine is an equivalent for this finite integer-output
    // domain, verified against the original reduction and kernel instructions.
    const float offset = static_cast<float>(remaining - 57600);
    const float scaled = offset * 180.0f;
    const float degrees = scaled / 21600.0f;
    const float shifted = degrees - 90.0f;
    const float radians = 0x1.1df46ap-6f * shifted; // DOL 8034A36C: 3C8EFA35
    const float sine = static_cast<float>(std::sin(static_cast<double>(radians)));
    const float product = 50.0f * sine;
    const float half = product * 0.5f;
    const float result = 125.0f + half;
    if (!std::isfinite(result) || result < 100.0f || result > 150.0f) return S::UnsupportedNumerics;
    *out = static_cast<uint32_t>(result);
    return S::Prepared;
}
WorldMapPlayerStatusStep prepare_world_map_player_status(
    const WorldMapPlayerStatusState& before, uint32_t code,
    const WorldMapPlayerStatusInputs& inputs) noexcept {
    Calculation c(inputs);
    auto next = before;
    uint32_t value, a, b, elapsed;
    if (!c.cc14(next, value)) return c.step;
    // Keep both low-word multiplies and unsigned divides in C87C.
    next.word_14 = clamp(next.word_14 - ((value * 10u * 20u) / 100u) / 10u, next.word_1c, next.word_18);
    if (signed_word(code) < 0) { c.step.status = S::UnsupportedRateIndex; return c.step; }
    if (!inputs.rate) { c.step.status = S::RequiresRate; return c.step; }
    if (inputs.rate->descriptor_word_8 != code) { c.step.status = S::RequiresRate; return c.step; }
    if (code >= 64) {
        next.word_154 = inputs.rate->value;
        if (!c.cca4(next, a)) return c.step;
        if (!c.get(G::Word7eb0, b)) return c.step;
        value = 100;
        if (signed_word(next.word_14) <= signed_word(b)) {
            if (!c.get(G::Word7eac, b)) return c.step;
            value = divide((b - next.word_14) * 200u, 1350000) + 100;
        }
        if (!c.elapsed(next, elapsed)) return c.step;
        value += a + elapsed;
        if (next.word_160 == 1 || next.word_160 == 2) {
            const auto byte = next.word_160 == 1 ? next.byte_2e : next.byte_32;
            const uint32_t factor = (0xd7au - uint32_t(byte) * 0x28au) >> 2;
            value = (value * factor) / 100u;
        }
        next.word_4 = clamp(next.word_4 - (value * (next.word_154 * 400u)) / 3000000u, next.word_c, next.word_8);
    } else {
        next.word_150 = inputs.rate->value;
        // C7E8 calls CDB8 BEFORE either integer getter.
        if (!c.elapsed(next, elapsed)) return c.step;
        if (!c.cd30(next, a) || !c.get(G::Word7ea0, b)) return c.step;
        value = divide(next.word_14 * b, 1350000) + 20;
        next.word_4 = clamp(next.word_4 + divide(next.word_150 * 200u * (value + a), elapsed * 20000u), next.word_c, next.word_8);
    }
    c.step.status = S::Prepared;
    c.step.after = next;
    return c.step;
}
} // namespace awl
