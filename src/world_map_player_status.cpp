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
WorldMapPlayerStatusConditionStep prepare_world_map_player_status_conditions(
    const WorldMapPlayerStatusState& before,const WorldMapPlayerStatusConditionFactors& factors,
    std::optional<uint32_t> phase,uint32_t action) noexcept {
    using C=WorldMapPlayerStatusConditionCode;using CG=WorldMapPlayerStatusConditionGlobal;
    WorldMapPlayerStatusConditionStep result;auto state=before;
    const auto finish=[&](C code){result.status=code;if(code==C::Prepared || code==C::TransitionRequired)result.after=state;return result;};
    const auto field=[&](const auto& value,uint32_t offset){if(value)return true;result.status=C::RequiresField;result.required_field=offset;return false;};
    const auto ratio=[&](CG key,uint32_t value,uint32_t divisor,int64_t* out){const auto& coefficient=factors.globals[static_cast<size_t>(key)];
        if(!coefficient){result.status=C::RequiresGlobal;result.required_global=key;return false;}*out=signed_word(value * *coefficient)/divisor;return true;};
    const auto transition=[&](uint32_t state_id){result.requested_state=state_id;return finish(C::TransitionRequired);};
    if(!phase)return finish(C::RequiresPhaseRaw);
    result.gate=uint8_t{0};if(*phase>=state.word_12c)return transition(action?0x1fu:0x21u);
    result.gate=uint8_t{1};if(!field(state.word_134,0x134))return result;
    if(*phase>=*state.word_134){uint32_t elapsed=0;
        if(prepare_world_map_player_status_elapsed(state.word_12c,*phase,&elapsed)!=WorldMapPlayerStatusCode::Prepared)return finish(C::UnsupportedNumerics);
        if(elapsed==150){if(!field(state.byte_15c,0x15c))return result;if(!*state.byte_15c)state.byte_15c=uint8_t{1};}
        else state.byte_15c=uint8_t{0};
        if(state.byte_15c==uint8_t{1})return transition(0x1b);
    }
    int64_t value=0;result.gate=uint8_t{2};if(!ratio(CG::Word7ee0,state.word_4,100000,&value))return result;
    if(value<=10)return transition(action?0x23u:0x25u);
    result.gate=uint8_t{3};if(!field(state.byte_158,0x158))return result;
    if(!*state.byte_158){if(!ratio(CG::Word7edc,state.word_4,100000,&value))return result;if(value<=20){state.byte_158=uint8_t{1};return transition(action?0x1cu:0x1eu);}}
    else {if(!ratio(CG::Word7ed8,state.word_4,100000,&value))return result;if(value>=25)state.byte_158=uint8_t{0};}
    result.gate=uint8_t{4};if(!field(state.byte_159,0x159))return result;
    if(!*state.byte_159){if(!ratio(CG::Word7ed4,state.word_4,100000,&value))return result;if(value<=40){state.byte_159=uint8_t{1};return transition(action?0x1cu:0x1eu);}}
    else {if(!ratio(CG::Word7ed0,state.word_4,100000,&value))return result;if(value>=45)state.byte_159=uint8_t{0};}
    result.gate=uint8_t{5};if(!ratio(CG::Word7ec4,state.word_14,1350000,&value))return result;
    if(value<=10){state.byte_15b=uint8_t{1};if(!field(state.byte_15a,0x15a))return result;
        bool request=!*state.byte_15a;
        if(!request){if(!field(state.word_13c,0x13c))return result;request=*phase>=*state.word_13c;}
        if(request){state.word_13c=*phase+36000u;return transition(0x1b);}
    }else state.byte_15a=uint8_t{0};
    result.gate=uint8_t{6};if(!ratio(CG::Word7ec8,state.word_14,1350000,&value))return result;
    if(value<=20){if(!field(state.byte_15b,0x15b))return result;if(!*state.byte_15b)return transition(0x1b);}
    else state.byte_15b=uint8_t{0};
    result.gate.reset();return finish(C::Prepared);
}
WorldMapPlayerStatusCode construct_world_map_player_status(
    std::optional<uint32_t> phase_raw, WorldMapPlayerStatusState* out) noexcept {
    if (!out) return S::InvalidInput;
    if (!phase_raw) return S::RequiresPhaseRaw;
    WorldMapPlayerStatusState next;
    next.word_4 = next.word_8 = 100000; next.word_c = 1;
    next.word_14 = next.word_18 = 1350000; next.word_1c = 100;
    next.word_20 = 50000; next.word_24 = 99999; next.word_28 = 0;
    next.byte_2e = next.byte_32 = 1;
    next.word_12c = *phase_raw + 2160000u;
    next.word_134 = *phase_raw + 576000u; next.word_13c = *phase_raw;
    next.byte_14c=next.byte_158=next.byte_159=next.byte_15a=next.byte_15b=next.byte_15c=uint8_t{0};
    next.word_150 = next.word_154 = 100; next.word_160 = 0;
    *out = next;
    return S::Prepared;
}

WorldMapPlayerStatusCode reset_world_map_player_status(
    const WorldMapPlayerStatusState& before, const WorldMapPlayerStatusResetValues& values,
    WorldMapPlayerStatusState* out) noexcept {
    if (!out) return S::InvalidInput;
    auto next = before;
    next.word_4 = clamp(values.word_4, next.word_c, next.word_8);
    next.word_20 = clamp(values.word_20, next.word_28, next.word_24);
    next.word_14 = clamp(values.word_14, next.word_1c, next.word_18);
    *out = next;
    return S::Prepared;
}

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
