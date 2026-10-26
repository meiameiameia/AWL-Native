#include "gx/gx_vertex.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace awl::gx {

namespace {

using Eng = VertexEngine;
using AttrFmt = Eng::AttrFmt;
using DrawCtx = Eng::DrawCtx;
using RawVertex = Eng::RawVertex;
using RawBatch = Eng::RawBatch;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t bitsOf(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
float fl(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
uint32_t compBytes(uint32_t f) { return f <= 1 ? 1 : f <= 3 ? 2 : 4; }
uint32_t colBytes(uint32_t f) { return f == 0 || f == 3 ? 2 : f == 1 || f == 4 ? 3 : 4; }

// One coordinate component of format FMT: u8, s8, u16, s16 (times scale = 2^-shift) or f32.
template <int FMT>
inline float comp(const uint8_t*& p, float scale) {
    if constexpr (FMT == 0) { const float v = float(p[0]) * scale; p += 1; return v; }
    else if constexpr (FMT == 1) { const float v = float(static_cast<int8_t>(p[0])) * scale; p += 1; return v; }
    else if constexpr (FMT == 2) { const float v = float(uint32_t(p[0]) << 8 | p[1]) * scale; p += 2; return v; }
    else if constexpr (FMT == 3) { const float v = float(static_cast<int16_t>(uint16_t(p[0]) << 8 | p[1])) * scale; p += 2; return v; }
    else { const float v = fl(be32(p)); p += 4; return v; }
}
float compRuntime(const uint8_t*& p, uint32_t fmt, float scale) {
    switch (fmt) {
        case 0: return comp<0>(p, scale);
        case 1: return comp<1>(p, scale);
        case 2: return comp<2>(p, scale);
        case 3: return comp<3>(p, scale);
        default: return comp<4>(p, scale);
    }
}
// Normal components are fixed-point with a fixed scale: s8 -> 1/64, s16 -> 1/16384 (YAGCD VAT, normal format); the
// multiplications by these powers of two are exact.
template <int FMT>
inline float nrmComp(const uint8_t*& p) {
    if constexpr (FMT == 1) { const float v = float(static_cast<int8_t>(p[0])) * (1.0f / 64.0f); p += 1; return v; }
    else if constexpr (FMT == 3) { const float v = float(static_cast<int16_t>(uint16_t(p[0]) << 8 | p[1])) * (1.0f / 16384.0f); p += 2; return v; }
    else { const float v = fl(be32(p)); p += 4; return v; }
}
float nrmCompRuntime(const uint8_t*& p, uint32_t fmt) {
    if (fmt == 1) return nrmComp<1>(p);
    if (fmt == 3) return nrmComp<3>(p);
    return nrmComp<4>(p);
}
uint8_t rep(uint32_t v, uint32_t bits) { return static_cast<uint8_t>(bits == 4 ? v * 17 : bits == 5 ? (v << 3) | (v >> 2) : bits == 6 ? (v << 2) | (v >> 4) : v); }

template <int FMT>
inline void readColorT(const uint8_t* p, uint8_t out[4]) {
    if constexpr (FMT == 0) { const uint32_t v = uint32_t(p[0]) << 8 | p[1]; out[0] = rep(v >> 11, 5); out[1] = rep((v >> 5) & 63, 6); out[2] = rep(v & 31, 5); out[3] = 255; }
    else if constexpr (FMT == 1 || FMT == 2) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; }
    else if constexpr (FMT == 3) { const uint32_t v = uint32_t(p[0]) << 8 | p[1]; out[0] = rep(v >> 12, 4); out[1] = rep((v >> 8) & 15, 4); out[2] = rep((v >> 4) & 15, 4); out[3] = rep(v & 15, 4); }
    else if constexpr (FMT == 4) { const uint32_t v = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; out[0] = rep(v >> 18, 6); out[1] = rep((v >> 12) & 63, 6); out[2] = rep((v >> 6) & 63, 6); out[3] = rep(v & 63, 6); }
    else { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; }
}
void readColor(const uint8_t* p, uint32_t fmt, uint8_t out[4]) {
    switch (fmt) {
        case 0: readColorT<0>(p, out); break;
        case 1: case 2: readColorT<1>(p, out); break;
        case 3: readColorT<3>(p, out); break;
        case 4: readColorT<4>(p, out); break;
        default: readColorT<5>(p, out); break;
    }
}

// Face determination uses the determinant of the (x, y, w) rows of the three clip-space vertices: 2 x the signed area
// (counter-clockwise positive, y up) for w = 1, invariant to clipping. The game draws its logo quads clockwise as seen on
// the screen and keeps them with the cull-back state (BP GEN_MODE hardware value 1), so a front face is CLOCKWISE on the
// screen, i.e. a negative determinant (gx_ir.h convention 6, version 2). This is the single place the sign lives.
constexpr double kFrontFaceDeterminantSign = -1.0;

// sqrtss without the math-error handling /fp:strict wraps around std::sqrt (same correctly rounded result for x >= 0).
inline float sqrtf1(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void norm3(float* v) {
    const float l = sqrtf1(dot3(v, v));
    if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Attenuation x diffuse of one light for one channel control word (YAGCD XF channel control: bits 7-8 diffuse function,
// bit 9 attenuation enable, bit 10 spotlight/specular select).
float lightFactor(const Eng::LightObj& l, uint32_t ctrl, const float p[3], const float n[3]) {
    float ldir[3] = {l.pos[0] - p[0], l.pos[1] - p[1], l.pos[2] - p[2]};
    const float dist2 = dot3(ldir, ldir), dist = sqrtf1(dist2);
    if (dist > 0.0f) { const float inv = 1.0f / dist; ldir[0] *= inv; ldir[1] *= inv; ldir[2] *= inv; }
    float attn = 1.0f;
    if (bitsOf(ctrl, 9, 1)) {
        if (bitsOf(ctrl, 10, 1) && l.constAttn) {
            attn = l.attn0;                              // constant attenuation (GX_SP_OFF / GX_DA_OFF coefficients): no angle or distance terms
        } else if (bitsOf(ctrl, 10, 1)) {                // spotlight: cosine attenuation over distance attenuation
            const float c = std::max(0.0f, dot3(ldir, l.dir));
            const float num = std::max(0.0f, l.a[0] + l.a[1] * c + l.a[2] * c * c);
            attn = num / (l.k[0] + l.k[1] * dist + l.k[2] * dist2);
        } else {                                         // specular: N.H term in both polynomials
            const float nl = dot3(n, ldir);
            const float s = nl >= 0.0f ? std::max(0.0f, dot3(n, l.dir)) : 0.0f;
            float k[3] = {l.k[0], l.k[1], l.k[2]};
            if (bitsOf(ctrl, 7, 2) != 0) norm3(k);
            attn = std::max(0.0f, l.a[0] + l.a[1] * s + l.a[2] * s * s) / (k[0] + k[1] * s + k[2] * s * s);
        }
    }
    const uint32_t diff = bitsOf(ctrl, 7, 2);
    if (diff == 0) return attn;
    const float nl = dot3(ldir, n);
    return attn * (diff == 1 ? nl : std::max(0.0f, nl));
}

// Matrix of 3 rows x 4 (row-major words) as columns for the SSE products.
Eng::Cols colsFromRows34(const uint32_t* w) {
    float m[12];
    for (int k = 0; k < 12; ++k) m[k] = fl(w[k]);
    return {_mm_set_ps(0, m[8], m[4], m[0]), _mm_set_ps(0, m[9], m[5], m[1]), _mm_set_ps(0, m[10], m[6], m[2]), _mm_set_ps(0, m[11], m[7], m[3])};
}
Eng::Cols colsFromRows33(const uint32_t* w) {
    float m[9];
    for (int k = 0; k < 9; ++k) m[k] = fl(w[k]);
    return {_mm_set_ps(0, m[6], m[3], m[0]), _mm_set_ps(0, m[7], m[4], m[1]), _mm_set_ps(0, m[8], m[5], m[2]), _mm_setzero_ps()};
}
// ((c0 * x + c1 * y) + c2 * z) + c3: the scalar reference's order (m0 x + m1 y + m2 z + m3) in every lane.
inline __m128 apply(const Eng::Cols& m, float x, float y, float z) {
    return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(m.c0, _mm_set1_ps(x)), _mm_mul_ps(m.c1, _mm_set1_ps(y))), _mm_mul_ps(m.c2, _mm_set1_ps(z))), m.c3);
}

// ---- Specialised attribute loaders (selected once per vertex format) -------------------------------------------------
template <int IB>
inline uint32_t readIndex(const uint8_t* p) {
    if constexpr (IB == 1) return p[0];
    else return uint32_t(p[0]) << 8 | p[1];
}
// 16-bit and 32-bit big-endian attributes are converted with SIMD: one unaligned load per element (the window has slack past the
// last element), a byte shuffle for the endianness, then the same int -> float conversion and multiply per component as the
// scalar loaders. Lanes past n repeat the last vertex.
inline __m128i bswap16x8() { return _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14); }
inline __m128i bswap32x4() { return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }
template <int IB, int FMT>
void decPos(const DrawCtx& d, const uint8_t* p, uint32_t stride, uint32_t n, RawBatch& b) {
    if constexpr (FMT == 3) {
        __m128 v[4];
        const __m128 s = _mm_set1_ps(d.posScale);
        const __m128i sw = bswap16x8();
        for (uint32_t lane = 0; lane < 4; ++lane) {
            const uint8_t* e = d.win[0] + d.stride[0] * readIndex<IB>(p + size_t(stride) * std::min(lane, n - 1));
            v[lane] = _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvtepi16_epi32(_mm_shuffle_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(e)), sw))), s);
        }
        _MM_TRANSPOSE4_PS(v[0], v[1], v[2], v[3]);
        _mm_store_ps(b.pos[0], v[0]);
        _mm_store_ps(b.pos[1], v[1]);
        _mm_store_ps(b.pos[2], v[2]);
    } else {
        for (uint32_t lane = 0; lane < n; ++lane, p += stride) {
            const uint8_t* e = d.win[0] + d.stride[0] * readIndex<IB>(p);
            b.pos[0][lane] = comp<FMT>(e, d.posScale);
            b.pos[1][lane] = comp<FMT>(e, d.posScale);
            b.pos[2][lane] = comp<FMT>(e, d.posScale);
        }
    }
}
template <int IB, int FMT>
void decNrm(const DrawCtx& d, const uint8_t* p, uint32_t stride, uint32_t n, RawBatch& b) {
    if constexpr (FMT == 4) {
        __m128 v[4];
        const __m128i sw = bswap32x4();
        for (uint32_t lane = 0; lane < 4; ++lane) {
            const uint8_t* e = d.win[1] + d.stride[1] * readIndex<IB>(p + size_t(stride) * std::min(lane, n - 1));
            v[lane] = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(e)), sw));
        }
        _MM_TRANSPOSE4_PS(v[0], v[1], v[2], v[3]);
        _mm_store_ps(b.nrm[0], v[0]);
        _mm_store_ps(b.nrm[1], v[1]);
        _mm_store_ps(b.nrm[2], v[2]);
    } else {
        for (uint32_t lane = 0; lane < n; ++lane, p += stride) {
            const uint8_t* e = d.win[1] + d.stride[1] * readIndex<IB>(p);
            b.nrm[0][lane] = nrmComp<FMT>(e);
            b.nrm[1][lane] = nrmComp<FMT>(e);
            b.nrm[2][lane] = nrmComp<FMT>(e);
        }
    }
}
template <int IB, int FMT>
void decCol0(const DrawCtx& d, const uint8_t* p, uint32_t stride, uint32_t n, RawBatch& b) {
    for (uint32_t lane = 0; lane < n; ++lane, p += stride) {
        uint8_t c[4];
        readColorT<FMT>(d.win[2] + d.stride[2] * readIndex<IB>(p), c);
        b.col[0][lane] = c[0];
        b.col[1][lane] = c[1];
        b.col[2][lane] = c[2];
        b.col[3][lane] = c[3];
    }
}
template <int IB, int FMT>
void decTex0(const DrawCtx& d, const uint8_t* p, uint32_t stride, uint32_t n, RawBatch& b) {
    if constexpr (FMT == 3) {
        __m128 v[4];
        const __m128 s = _mm_set1_ps(d.texScale[0]);
        const __m128i sw = bswap16x8();
        for (uint32_t lane = 0; lane < 4; ++lane) {
            const uint8_t* e = d.win[4] + d.stride[4] * readIndex<IB>(p + size_t(stride) * std::min(lane, n - 1));
            v[lane] = _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvtepi16_epi32(_mm_shuffle_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(e)), sw))), s);
        }
        _MM_TRANSPOSE4_PS(v[0], v[1], v[2], v[3]);
        _mm_store_ps(b.tex[0], v[0]);
        _mm_store_ps(b.tex[1], v[1]);
    } else {
        for (uint32_t lane = 0; lane < n; ++lane, p += stride) {
            const uint8_t* e = d.win[4] + d.stride[4] * readIndex<IB>(p);
            b.tex[0][lane] = comp<FMT>(e, d.texScale[0]);
            b.tex[1][lane] = comp<FMT>(e, d.texScale[0]);
        }
    }
}
Eng::AttrFn selectPos(const AttrFmt& a) {
    if (a.cnt != 1 || (a.mode != 2 && a.mode != 3)) return nullptr;
    const bool b8 = a.mode == 2;
    if (a.fmt == 3) return b8 ? decPos<1, 3> : decPos<2, 3>;
    if (a.fmt == 4) return b8 ? decPos<1, 4> : decPos<2, 4>;
    return nullptr;
}
Eng::AttrFn selectNrm(const AttrFmt& a) {
    if (a.cnt != 0 || a.mode != 2) return nullptr;
    if (a.fmt == 1) return decNrm<1, 1>;
    if (a.fmt == 3) return decNrm<1, 3>;
    if (a.fmt == 4) return decNrm<1, 4>;
    return nullptr;
}
Eng::AttrFn selectCol(const AttrFmt& a) {
    if (a.mode != 2 && a.mode != 3) return nullptr;
    const bool b8 = a.mode == 2;
    if (a.fmt == 0) return b8 ? decCol0<1, 0> : decCol0<2, 0>;
    if (a.fmt == 3) return b8 ? decCol0<1, 3> : decCol0<2, 3>;
    if (a.fmt == 5) return b8 ? decCol0<1, 5> : decCol0<2, 5>;
    return nullptr;
}
Eng::AttrFn selectTex(const AttrFmt& a) {
    if (a.cnt != 1 || (a.mode != 2 && a.mode != 3)) return nullptr;
    const bool b8 = a.mode == 2;
    if (a.fmt == 1) return b8 ? decTex0<1, 1> : decTex0<2, 1>;
    if (a.fmt == 3) return b8 ? decTex0<1, 3> : decTex0<2, 3>;
    if (a.fmt == 4) return b8 ? decTex0<1, 4> : decTex0<2, 4>;
    return nullptr;
}

}  // namespace

VertexEngine::VertexEngine(const GuestMemory& mem, const CpState& cp, const XfState& xf) : mem_(mem), cp_(cp), xf_(xf), verts_(new Vertex[65536 + 4]), idx_(new uint16_t[3 * 65536 + 16]) {
    std::memset(&dc_, 0, sizeof dc_);
    scalarOnly_ = std::getenv("AWL_GX_SCALAR") != nullptr;
}

void VertexEngine::fail(const std::string& what, uint32_t code) const { throw GxError(what, 0, code); }

void VertexEngine::xfWritten(uint32_t addr, uint32_t count) {
    const uint32_t end = addr + count;
    if (end > 0x1000) regsDirty_ = postDirty_ = true;
    if (addr < 0x600 && end > 0x500) postDirty_ = true;
    if (addr < 0x680 && end > 0x600) lightsDirty_ = true;
}

VertexEngine::VtxFmt VertexEngine::vertexFormat(uint32_t vat) const {
    VtxFmt f;
    const uint32_t a = cp_.vat[0][vat], b = cp_.vat[1][vat], c = cp_.vat[2][vat];
    f.pnmtx = bitsOf(cp_.vcdLo, 0, 1);
    f.size = f.pnmtx ? 1 : 0;
    for (uint32_t t = 0; t < 8; ++t) {
        f.texmtx[t] = bitsOf(cp_.vcdLo, 1 + t, 1);
        if (f.texmtx[t]) ++f.size;
    }
    auto direct = [&](AttrFmt& at, uint32_t directBytes, uint32_t indices) {
        uint32_t bytes = 0;
        if (at.mode == 1) bytes = directBytes;
        else if (at.mode >= 2) bytes = (at.mode == 2 ? 1 : 2) * indices;
        f.size += bytes;
    };
    f.pos = {bitsOf(cp_.vcdLo, 9, 2), bitsOf(a, 0, 1), bitsOf(a, 1, 3), bitsOf(a, 4, 5)};
    direct(f.pos, (f.pos.cnt ? 3 : 2) * compBytes(f.pos.fmt), 1);
    f.nrm = {bitsOf(cp_.vcdLo, 11, 2), bitsOf(a, 9, 1), bitsOf(a, 10, 3), 0};
    f.nrmIdx3 = bitsOf(a, 31, 1) != 0;
    direct(f.nrm, (f.nrm.cnt ? 9 : 3) * (f.nrm.fmt == 1 ? 1 : f.nrm.fmt == 3 ? 2 : 4), (f.nrm.cnt && f.nrmIdx3) ? 3 : 1);
    f.col[0] = {bitsOf(cp_.vcdLo, 13, 2), bitsOf(a, 13, 1), bitsOf(a, 14, 3), 0};
    direct(f.col[0], colBytes(f.col[0].fmt), 1);
    f.col[1] = {bitsOf(cp_.vcdLo, 15, 2), bitsOf(a, 17, 1), bitsOf(a, 18, 3), 0};
    direct(f.col[1], colBytes(f.col[1].fmt), 1);
    // Texcoord fields (YAGCD VAT A/B/C): tex0 in A, tex1-3 and tex4's count/format in B, tex4's shift split over B bit 31 and C bits 0-3.
    struct Field { uint32_t cnt, fmt, shift; const uint32_t* w; };
    const Field fields[8] = {{21, 22, 25, &a}, {0, 1, 4, &b}, {9, 10, 13, &b}, {18, 19, 22, &b}, {27, 28, 0, &b}, {4, 5, 8, &c}, {13, 14, 17, &c}, {22, 23, 26, &c}};
    for (uint32_t t = 0; t < 8; ++t) {
        const Field& fd = fields[t];
        const uint32_t shift = t == 4 ? (bitsOf(b, 31, 1) | bitsOf(c, 0, 4) << 1) : bitsOf(*fd.w, fd.shift, 5);
        f.tex[t] = {bitsOf(cp_.vcdHi, 2 * t, 2), bitsOf(*fd.w, fd.cnt, 1), bitsOf(*fd.w, fd.fmt, 3), shift};
        direct(f.tex[t], (f.tex[t].cnt ? 2 : 1) * compBytes(f.tex[t].fmt), 1);
    }
    return f;
}

// The plan of a vertex format depends on VCD, the VAT of the format and the array registers; it is rebuilt when any of them
// differ from the values it was built from (the SDK rewrites them before nearly every draw, mostly with the same values).
VertexEngine::Plan& VertexEngine::plan(uint32_t vat) {
    Plan& p = plans_[vat];
    uint32_t key[3 + 2 * 12 + 2];
    key[0] = cp_.vat[0][vat];
    key[1] = cp_.vat[1][vat];
    key[2] = cp_.vat[2][vat];
    for (int a = 0; a < 12; ++a) { key[3 + a] = cp_.arrayBase[a]; key[15 + a] = cp_.arrayStride[a]; }
    key[27] = cp_.vcdLo;
    key[28] = cp_.vcdHi;
    if (p.epoch != 0 && std::memcmp(p.key, key, sizeof key) == 0) return p;
    std::memcpy(p.key, key, sizeof key);
    p.epoch = 1;
    p.f = vertexFormat(vat);
    const VtxFmt& f = p.f;
    p.posScale = std::ldexp(1.0f, -static_cast<int>(f.pos.shift));
    for (uint32_t t = 0; t < 8; ++t) p.texScale[t] = std::ldexp(1.0f, -static_cast<int>(f.tex[t].shift));
    auto window = [&](uint32_t array, uint32_t mode) {
        p.win[array] = nullptr;
        p.winBytes[array] = 0;
        p.stride[array] = cp_.arrayStride[array];
        if (mode < 2) return;
        const uint32_t span = cp_.arrayStride[array] * (mode == 2 ? 256u : 65536u) + 64;
        p.win[array] = mem_.read(cp_.arrayBase[array], span);
        p.winBytes[array] = p.win[array] ? span : 0;
    };
    window(0, f.pos.mode);
    window(1, f.nrm.mode);
    window(2, f.col[0].mode);
    window(3, f.col[1].mode);
    for (uint32_t t = 0; t < 8; ++t) window(4 + t, f.tex[t].mode);
    // Specialised loaders for the common shapes: indexed position, optional 8-bit-indexed normal, one indexed color, one indexed
    // texcoord, no per-vertex matrix indices, nothing else.
    p.pos = selectPos(f.pos);
    p.nrm = f.nrm.mode ? selectNrm(f.nrm) : nullptr;
    p.col0 = f.col[0].mode ? selectCol(f.col[0]) : nullptr;
    p.tex0 = f.tex[0].mode ? selectTex(f.tex[0]) : nullptr;
    bool fast = p.pos && !f.pnmtx && (!f.nrm.mode || p.nrm) && (!f.col[0].mode || p.col0) && !f.col[1].mode && (!f.tex[0].mode || p.tex0);
    for (uint32_t t = 0; t < 8; ++t) fast = fast && !f.texmtx[t] && (t == 0 || !f.tex[t].mode);
    fast = fast && p.win[0] && (!f.nrm.mode || p.win[1]) && (!f.col[0].mode || p.win[2]) && (!f.tex[0].mode || p.win[4]);
    // attribute offsets inside the vertex: the position first, then normal, color 0 and texcoord 0 in stream order
    uint32_t off = f.pos.mode == 2 ? 1 : 2;
    p.nrmOff = off;
    if (f.nrm.mode) off += f.nrm.mode == 2 ? 1 : 2;
    p.colOff = off;
    if (f.col[0].mode) off += f.col[0].mode == 2 ? 1 : 2;
    p.texOff = off;
    p.fast = fast && !scalarOnly_;
    return p;
}

uint32_t VertexEngine::vertexSize(uint32_t vat) {
    Plan& p = plan(vat);
    fresh_ = &p;                                    // run() of the same draw reuses it
    return p.f.size;
}

void VertexEngine::beginDraw(const Plan& p) {
    DrawCtx& d = dc_;
    std::memcpy(d.win, p.win, sizeof d.win);
    std::memcpy(d.winBytes, p.winBytes, sizeof d.winBytes);
    std::memcpy(d.stride, p.stride, sizeof d.stride);
    d.posScale = p.posScale;
    std::memcpy(d.texScale, p.texScale, sizeof d.texScale);
    if (regsDirty_) {
        for (int i = 0; i < 6; ++i) d.proj[i] = fl(xf_.reg[0x20 + i]);
        for (int i = 0; i < 6; ++i) d.projb[i] = _mm_set1_ps(d.proj[i]);
        d.projA = _mm_set_ps(0, d.proj[4], d.proj[2], d.proj[0]);
        d.projB = _mm_set_ps(0, d.proj[5], d.proj[3], d.proj[1]);
        d.ortho = xf_.reg[0x26] != 0;
        d.numColors = std::min<uint32_t>(xf_.reg[0x09], 2);
        d.numTexGens = std::min<uint32_t>(xf_.reg[0x3F], 8);
        d.lit = false;
        for (uint32_t ch = 0; ch < 2; ++ch) {
            ChanCfg& c = d.chan[ch];
            c.ctrlC = xf_.reg[0x0E + ch];
            c.ctrlA = xf_.reg[0x10 + ch];
            c.litC = bitsOf(c.ctrlC, 1, 1) != 0;
            c.litA = bitsOf(c.ctrlA, 1, 1) != 0;
            c.matVtxC = bitsOf(c.ctrlC, 0, 1) != 0;
            c.matVtxA = bitsOf(c.ctrlA, 0, 1) != 0;
            c.ambVtxC = bitsOf(c.ctrlC, 6, 1) != 0;
            c.ambVtxA = bitsOf(c.ctrlA, 6, 1) != 0;
            c.same = c.ctrlC == c.ctrlA;
            for (int k = 0; k < 4; ++k) {
                c.mat[k] = static_cast<int>((xf_.reg[0x0C + ch] >> (24 - 8 * k)) & 0xFF);
                c.amb[k] = static_cast<int>((xf_.reg[0x0A + ch] >> (24 - 8 * k)) & 0xFF);
            }
            c.nC = c.nA = 0;
            const uint32_t maskC = c.litC ? (bitsOf(c.ctrlC, 2, 4) | bitsOf(c.ctrlC, 11, 4) << 4) : 0;
            const uint32_t maskA = c.litA ? (bitsOf(c.ctrlA, 2, 4) | bitsOf(c.ctrlA, 11, 4) << 4) : 0;
            for (uint32_t i = 0; i < 8; ++i) {
                if (maskC & (1u << i)) c.idxC[c.nC++] = static_cast<uint8_t>(i);
                if (maskA & (1u << i)) c.idxA[c.nA++] = static_cast<uint8_t>(i);
            }
            if (ch < d.numColors && (c.litC || c.litA)) d.lit = true;
        }
        d.pnDefault = bitsOf(xf_.reg[0x18], 0, 6);
        for (uint32_t t = 0; t < 8; ++t) d.texDefault[t] = t < 4 ? bitsOf(xf_.reg[0x18], 6 * (t + 1), 6) : bitsOf(xf_.reg[0x19], 6 * (t - 4), 6);
        regsDirty_ = false;
    }
    if (postDirty_) {
        const bool dual = bitsOf(xf_.reg[0x12], 0, 1) != 0;
        for (uint32_t i = 0; i < d.numTexGens; ++i) {
            TexGenCtx& t = d.tg[i];
            const uint32_t info = xf_.reg[0x40 + i], post = xf_.reg[0x50 + i];
            t.type = bitsOf(info, 4, 3);
            t.src = bitsOf(info, 7, 5);
            t.form = bitsOf(info, 2, 2);
            t.stq = bitsOf(info, 1, 1) != 0;
            t.dual = dual && t.type == 0;
            t.normalize = bitsOf(post, 8, 1) != 0;
            t.postIdx = bitsOf(post, 0, 6);
            if (t.dual) {
                t.post = colsFromRows34(&xf_.mem[0x500 + t.postIdx * 4]);
                static const uint32_t kIdentity[12] = {0x3F800000u, 0, 0, 0, 0, 0x3F800000u, 0, 0, 0, 0, 0x3F800000u, 0};
                t.postIdentity = std::memcmp(&xf_.mem[0x500 + t.postIdx * 4], kIdentity, sizeof kIdentity) == 0;
                for (int k = 0; k < 12; ++k) d.postb[i][k] = _mm_set1_ps(fl(xf_.mem[0x500 + t.postIdx * 4 + k]));
            }
        }
        postDirty_ = false;
    }
    if (lightsDirty_) {
        d.lightLoaded = 0;
        lightsDirty_ = false;
    }
    d.pnIdx = -1;
    d.nmValid = false;
    d.pmbIdx = d.nmbIdx = -1;
    for (uint32_t i = 0; i < d.numTexGens; ++i) d.tg[i].idx = d.tmbIdx[i] = -1;
    // The batch path handles uniform matrices (fast plans), constant-attenuation lights and regular texgens without normalisation.
    bool ok = p.fast && !scalarOnly_;
    for (uint32_t ch = 0; ok && ch < d.numColors; ++ch) {
        const ChanCfg& c = d.chan[ch];
        for (uint32_t k = 0; k < c.nC; ++k) { const LightObj& l = light(c.idxC[k]); if (bitsOf(c.ctrlC, 9, 1) && !(bitsOf(c.ctrlC, 10, 1) && l.constAttn)) ok = false; }
        for (uint32_t k = 0; k < c.nA; ++k) { const LightObj& l = light(c.idxA[k]); if (bitsOf(c.ctrlA, 9, 1) && !(bitsOf(c.ctrlA, 10, 1) && l.constAttn)) ok = false; }
    }
    for (uint32_t i = 0; ok && i < d.numTexGens; ++i) {
        const TexGenCtx& t = d.tg[i];
        if (t.type != 0 || (t.dual && t.normalize) || !(t.src <= 1 || t.src == 3 || t.src == 4 || (t.src >= 5 && t.src <= 12))) ok = false;
    }
    d.batchOk = ok;
}

const VertexEngine::LightObj& VertexEngine::light(uint32_t i) {
    DrawCtx& d = dc_;
    if (!(d.lightLoaded & (1u << i))) {
        const uint32_t* L = &xf_.mem[0x600 + 16 * i];
        LightObj& l = d.lights[i];
        l.color[0] = float(L[3] >> 24);
        l.color[1] = float((L[3] >> 16) & 0xFF);
        l.color[2] = float((L[3] >> 8) & 0xFF);
        l.color[3] = float(L[3] & 0xFF);
        for (int k = 0; k < 3; ++k) {
            l.a[k] = fl(L[4 + k]);
            l.k[k] = fl(L[7 + k]);
            l.pos[k] = fl(L[10 + k]);
            l.dir[k] = fl(L[13 + k]);
        }
        l.constAttn = l.a[1] == 0.0f && l.a[2] == 0.0f && l.k[1] == 0.0f && l.k[2] == 0.0f;
        l.attn0 = l.constAttn ? std::max(0.0f, l.a[0]) / l.k[0] : 0.0f;
        DrawCtx::LightB& lb = d.lightb[i];
        for (int k = 0; k < 3; ++k) lb.pos[k] = _mm_set1_ps(l.pos[k]);
        lb.attn0 = _mm_set1_ps(l.attn0);
        for (int k = 0; k < 4; ++k) lb.color[k] = _mm_set1_ps(l.color[k]);
        lb.nonNeg = l.attn0 >= 0.0f;
        d.lightLoaded |= 1u << i;
    }
    return d.lights[i];
}

// Returns the element of an attribute: the next bytes of the stream (direct) or the entry of the CP array (indexed).
const uint8_t* VertexEngine::elem(const AttrFmt& a, uint32_t array, const uint8_t*& p, uint32_t directBytes, uint32_t elemBytes) {
    if (a.mode == 1) {
        const uint8_t* e = p;
        p += directBytes;
        return e;
    }
    uint32_t idx;
    if (a.mode == 2) { idx = p[0]; p += 1; }
    else { idx = uint32_t(p[0]) << 8 | p[1]; p += 2; }
    const uint32_t off = cp_.arrayStride[array] * idx;
    if (dc_.win[array] && off + elemBytes <= dc_.winBytes[array]) return dc_.win[array] + off;
    const uint8_t* e = mem_.read(cp_.arrayBase[array] + off, elemBytes);
    if (!e) fail("indexed vertex attribute reads outside memory (array " + std::to_string(array) + ", index " + std::to_string(idx) + ")", 0xA0 + array);
    return e;
}

void VertexEngine::decodeGeneric(const VtxFmt& f, const uint8_t* p, RawVertex& r) {
    DrawCtx& d = dc_;
    r.pnIdx = f.pnmtx ? *p++ : d.pnDefault;
    for (uint32_t t = 0; t < 8; ++t) r.texIdx[t] = f.texmtx[t] ? *p++ : d.texDefault[t];
    r.pos[0] = r.pos[1] = r.pos[2] = 0.0f;
    for (int v = 0; v < 3; ++v) r.nrm[v][0] = r.nrm[v][1] = r.nrm[v][2] = 0.0f;
    if (f.pos.mode) {
        const uint32_t n = f.pos.cnt ? 3 : 2, db = n * compBytes(f.pos.fmt);
        const uint8_t* e = elem(f.pos, 0, p, db, db);
        for (uint32_t i = 0; i < n; ++i) r.pos[i] = compRuntime(e, f.pos.fmt, d.posScale);
    }
    r.hasNrm = false;
    if (f.nrm.mode) {
        const uint32_t comps = f.nrm.cnt ? 9 : 3, cb = f.nrm.fmt == 1 ? 1 : f.nrm.fmt == 3 ? 2 : 4;
        r.hasNrm = true;
        if (f.nrm.mode >= 2 && f.nrm.cnt && f.nrmIdx3) {
            for (uint32_t v = 0; v < 3; ++v) {
                const uint8_t* e = elem(f.nrm, 1, p, 3 * cb, 3 * cb);
                for (uint32_t i = 0; i < 3; ++i) r.nrm[v][i] = nrmCompRuntime(e, f.nrm.fmt);
            }
        } else {
            const uint8_t* e = elem(f.nrm, 1, p, comps * cb, comps * cb);
            for (uint32_t v = 0; v < comps / 3; ++v)
                for (uint32_t i = 0; i < 3; ++i) r.nrm[v][i] = nrmCompRuntime(e, f.nrm.fmt);
        }
    }
    for (uint32_t c = 0; c < 2; ++c) {
        r.hasCol[c] = false;
        if (!f.col[c].mode) continue;
        const uint8_t* e = elem(f.col[c], 2 + c, p, colBytes(f.col[c].fmt), colBytes(f.col[c].fmt));
        readColor(e, f.col[c].fmt, r.col[c]);
        r.hasCol[c] = true;
    }
    for (uint32_t t = 0; t < 8; ++t) {
        r.tex[t][0] = r.tex[t][1] = 0.0f;
        if (!f.tex[t].mode) continue;
        const uint32_t n = f.tex[t].cnt ? 2 : 1, db = n * compBytes(f.tex[t].fmt);
        const uint8_t* e = elem(f.tex[t], 4 + t, p, db, db);
        for (uint32_t i = 0; i < n; ++i) r.tex[t][i] = compRuntime(e, f.tex[t].fmt, d.texScale[t]);
    }
}

void VertexEngine::lightChannels(const RawVertex& r, const float vpos[3], const float vnrm[3], uint8_t color[2][4]) {
    DrawCtx& d = dc_;
    static const uint8_t white[4] = {255, 255, 255, 255};
    for (uint32_t ch = 0; ch < d.numColors; ++ch) {
        const ChanCfg& c = d.chan[ch];
        const uint8_t* vtx = r.hasCol[ch] ? r.col[ch] : white;
        uint8_t* out = color[ch];
        // Attenuation x diffuse of each enabled light for the color control and the alpha control (computed once, shared by the components).
        float fc[8], fa[8];
        for (uint32_t k = 0; k < c.nC; ++k) fc[k] = lightFactor(light(c.idxC[k]), c.ctrlC, vpos, vnrm);
        if (c.same) {
            for (uint32_t k = 0; k < c.nA; ++k) fa[k] = fc[k];
        } else {
            for (uint32_t k = 0; k < c.nA; ++k) fa[k] = lightFactor(light(c.idxA[k]), c.ctrlA, vpos, vnrm);
        }
        for (int comp = 0; comp < 4; ++comp) {
            const bool isAlpha = comp == 3;
            const bool matVtx = isAlpha ? c.matVtxA : c.matVtxC;
            const int mat = matVtx ? vtx[comp] : c.mat[comp];
            if (!(isAlpha ? c.litA : c.litC)) { out[comp] = static_cast<uint8_t>(mat); continue; }
            int lacc = (isAlpha ? c.ambVtxA : c.ambVtxC) ? vtx[comp] : c.amb[comp];
            const uint32_t n = isAlpha ? c.nA : c.nC;
            const uint8_t* idx = isAlpha ? c.idxA : c.idxC;
            const float* f = isAlpha ? fa : fc;
            for (uint32_t k = 0; k < n; ++k) {
                const float v = f[k] * d.lights[idx[k]].color[comp];
                lacc += static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f);
            }
            lacc = std::clamp(lacc, 0, 255);
            // out = mat * lacc / 255 with the hardware's exact-at-255 rounding: (mat * (l + (l >> 7))) >> 8
            out[comp] = static_cast<uint8_t>((mat * (lacc + (lacc >> 7))) >> 8);
        }
    }
}

void VertexEngine::texGen(const RawVertex& r, const uint8_t color[2][4], Vertex& out) {
    DrawCtx& d = dc_;
    for (uint32_t i = 0; i < d.numTexGens; ++i) {
        TexGenCtx& t = d.tg[i];
        float s, tt, q;
        if (t.type == 0) {
            float in0, in1, in2;
            if (t.src == 0) { in0 = r.pos[0]; in1 = r.pos[1]; in2 = r.pos[2]; }
            else if (t.src == 1) { in0 = r.nrm[0][0]; in1 = r.nrm[0][1]; in2 = r.nrm[0][2]; }
            else if (t.src == 3) { in0 = r.nrm[2][0]; in1 = r.nrm[2][1]; in2 = r.nrm[2][2]; }   // binormal T = tangent in NBT order
            else if (t.src == 4) { in0 = r.nrm[1][0]; in1 = r.nrm[1][1]; in2 = r.nrm[1][2]; }
            else if (t.src >= 5 && t.src <= 12) { in0 = r.tex[t.src - 5][0]; in1 = r.tex[t.src - 5][1]; in2 = 1.0f; }
            else fail("texgen source row not modelled for the regular type: " + std::to_string(t.src), 0x1040 + i);
            if (t.form == 0) in2 = 1.0f;              // AB11: (a, b, 1, 1)
            const int idx = static_cast<int>(r.texIdx[i] & 0x3F);
            if (t.idx != idx) {
                t.m = colsFromRows34(&xf_.mem[idx * 4]);
                t.idx = idx;
            }
            // (((m0 in0 + m1 in1) + m2 in2) + m3 in3), in3 = 1, in each lane = rows s, t, q
            const __m128 v = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(t.m.c0, _mm_set1_ps(in0)), _mm_mul_ps(t.m.c1, _mm_set1_ps(in1))), _mm_mul_ps(t.m.c2, _mm_set1_ps(in2))), _mm_mul_ps(t.m.c3, _mm_set1_ps(1.0f)));
            float st[4];
            _mm_storeu_ps(st, v);
            s = st[0];
            tt = st[1];
            q = t.stq ? st[2] : 1.0f;
            if (t.dual) {
                float vv[3] = {s, tt, q};
                if (t.normalize) norm3(vv);
                const __m128 w = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(t.post.c0, _mm_set1_ps(vv[0])), _mm_mul_ps(t.post.c1, _mm_set1_ps(vv[1]))), _mm_mul_ps(t.post.c2, _mm_set1_ps(vv[2]))), t.post.c3);
                _mm_storeu_ps(st, w);
                s = st[0];
                tt = st[1];
                q = st[2];
            }
        } else if (t.type == 2 || t.type == 3) {      // color-to-texcoord: (r, g) of the lit channel
            const uint8_t* c = color[t.type - 2];
            s = c[0] / 255.0f;
            tt = c[1] / 255.0f;
            q = 1.0f;
        } else {
            fail("emboss texgen is not implemented", 0x1040 + i);
        }
        out.tex[i][0] = s;
        out.tex[i][1] = tt;
        out.tex[i][2] = q;
    }
}

void VertexEngine::transform(const RawVertex& r, Vertex& out) {
    DrawCtx& d = dc_;
    // Position matrix (3x4, rows of 4 words at 4 * index) -> view space.
    const int pn = static_cast<int>(r.pnIdx & 0x3F);
    if (d.pnIdx != pn) {
        d.pm = colsFromRows34(&xf_.mem[pn * 4]);
        d.pnIdx = pn;
        d.nmValid = false;
    }
    const __m128 vpv = apply(d.pm, r.pos[0], r.pos[1], r.pos[2]);
    float vp[4];
    _mm_storeu_ps(vp, vpv);
    // Projection (XF 0x1020-0x1026; YAGCD): perspective x' = A x + B z, y' = C y + D z, z' = E z + F, w' = -z; orthographic
    // x' = A x + B, y' = C y + D, z' = E z + F, w' = 1.
    if (!d.ortho) {
        const __m128 a = _mm_mul_ps(d.projA, vpv);
        const __m128 b = _mm_mul_ps(d.projB, _mm_set_ps(0.0f, 1.0f, vp[2], vp[2]));
        _mm_storeu_ps(out.pos, _mm_add_ps(a, b));
        out.pos[3] = -vp[2];
    } else {
        _mm_storeu_ps(out.pos, _mm_add_ps(_mm_mul_ps(d.projA, vpv), d.projB));
        out.pos[3] = 1.0f;
    }
    std::memset(out.color, 0, sizeof out.color);
    if (d.numColors != 0) {
        // Normal in view space (3x3 matrix at 0x400 + 3 * index), normalised for lighting.
        float vn[3] = {0, 0, 0};
        if (d.lit && r.hasNrm) {
            if (!d.nmValid) {
                const uint32_t base = 0x400 + 3 * static_cast<uint32_t>(pn);
                if (base + 9 > 0x460) fail("normal matrix index outside the normal matrix memory", r.pnIdx);
                d.nmc = colsFromRows33(&xf_.mem[base]);
                d.nmValid = true;
            }
            const __m128 nv = _mm_add_ps(_mm_add_ps(_mm_mul_ps(d.nmc.c0, _mm_set1_ps(r.nrm[0][0])), _mm_mul_ps(d.nmc.c1, _mm_set1_ps(r.nrm[0][1]))), _mm_mul_ps(d.nmc.c2, _mm_set1_ps(r.nrm[0][2])));
            float t4[4];
            _mm_storeu_ps(t4, nv);
            vn[0] = t4[0];
            vn[1] = t4[1];
            vn[2] = t4[2];
            norm3(vn);
        }
        lightChannels(r, vp, vn, out.color);
    }
    std::memset(out.tex, 0, sizeof out.tex);
    if (d.numTexGens != 0) texGen(r, out.color, out);
}

namespace {
inline __m128 sel(__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); }
inline __m128 mul(__m128 a, __m128 b) { return _mm_mul_ps(a, b); }
inline __m128 add(__m128 a, __m128 b) { return _mm_add_ps(a, b); }
// ((a0 x + a1 y) + a2 z) + a3 with broadcast matrix elements m[0..3]: the scalar reference's order in every lane.
inline __m128 rowDot(const __m128* m, __m128 x, __m128 y, __m128 z) { return add(add(add(mul(m[0], x), mul(m[1], y)), mul(m[2], z)), m[3]); }
inline void store3(float* dst, __m128 v) {
    _mm_storel_pi(reinterpret_cast<__m64*>(dst), v);
    _mm_store_ss(dst + 2, _mm_movehl_ps(v, v));
}
}  // namespace

// Four vertices at once (all of one draw with uniform matrices): the same IEEE operations in the same order per lane as
// transform(), so the results are bit-identical; lanes past the draw's last vertex carry stale data and are discarded.
void VertexEngine::transformBatch(const RawBatch& b, bool hasNrm, Vertex* out) {
    DrawCtx& d = dc_;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const int pn = static_cast<int>(d.pnDefault);
    if (d.pmbIdx != pn) {
        for (int k = 0; k < 12; ++k) d.pmb[k] = _mm_set1_ps(fl(xf_.mem[pn * 4 + k]));
        d.pmbIdx = pn;
    }
    const __m128 px = _mm_load_ps(b.pos[0]), py = _mm_load_ps(b.pos[1]), pz = _mm_load_ps(b.pos[2]);
    const __m128 vx = rowDot(d.pmb, px, py, pz), vy = rowDot(d.pmb + 4, px, py, pz), vz = rowDot(d.pmb + 8, px, py, pz);
    __m128 ox, oy, oz, ow;
    const __m128* P = d.projb;
    if (!d.ortho) {
        ox = add(mul(P[0], vx), mul(P[1], vz));
        oy = add(mul(P[2], vy), mul(P[3], vz));
        oz = add(mul(P[4], vz), P[5]);
        ow = _mm_xor_ps(vz, _mm_set1_ps(-0.0f));
    } else {
        ox = add(mul(P[0], vx), P[1]);
        oy = add(mul(P[2], vy), P[3]);
        oz = add(mul(P[4], vz), P[5]);
        ow = one;
    }
    {
        __m128 t0 = ox, t1 = oy, t2 = oz, t3 = ow;
        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        _mm_storeu_ps(out[0].pos, t0);
        _mm_storeu_ps(out[1].pos, t1);
        _mm_storeu_ps(out[2].pos, t2);
        _mm_storeu_ps(out[3].pos, t3);
    }
    for (int i = 0; i < 4; ++i) std::memset(out[i].color, 0, sizeof out[i].color);
    if (d.numColors != 0) {
        // View-space normal, normalised (the scalar path normalises only a non-empty result; zero stays zero).
        __m128 nx = zero, ny = zero, nz = zero;
        if (d.lit && hasNrm) {
            if (d.nmbIdx != pn) {
                const uint32_t base = 0x400 + 3 * static_cast<uint32_t>(pn);
                if (base + 9 > 0x460) fail("normal matrix index outside the normal matrix memory", static_cast<uint32_t>(pn));
                for (int k = 0; k < 9; ++k) d.nmb[k] = _mm_set1_ps(fl(xf_.mem[base + k]));
                d.nmbIdx = pn;
            }
            const __m128 ax = _mm_load_ps(b.nrm[0]), ay = _mm_load_ps(b.nrm[1]), az = _mm_load_ps(b.nrm[2]);
            nx = add(add(mul(d.nmb[0], ax), mul(d.nmb[1], ay)), mul(d.nmb[2], az));
            ny = add(add(mul(d.nmb[3], ax), mul(d.nmb[4], ay)), mul(d.nmb[5], az));
            nz = add(add(mul(d.nmb[6], ax), mul(d.nmb[7], ay)), mul(d.nmb[8], az));
            const __m128 l = _mm_sqrt_ps(add(add(mul(nx, nx), mul(ny, ny)), mul(nz, nz)));
            const __m128 m = _mm_cmpgt_ps(l, zero);
            if (_mm_movemask_ps(m) == 0xF) {
                nx = _mm_div_ps(nx, l);
                ny = _mm_div_ps(ny, l);
                nz = _mm_div_ps(nz, l);
            } else {
                nx = sel(m, _mm_div_ps(nx, l), nx);
                ny = sel(m, _mm_div_ps(ny, l), ny);
                nz = sel(m, _mm_div_ps(nz, l), nz);
            }
        }
        const __m128i i0 = _mm_setzero_si128(), i255 = _mm_set1_epi32(255);
        const __m128 half = _mm_set1_ps(0.5f);
        const __m128i shuf = _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
        for (uint32_t ch = 0; ch < d.numColors; ++ch) {
            const ChanCfg& c = d.chan[ch];
            // vertex color lanes (channel 0 only exists in fast plans; the loaders leave 255 when the vertex has no color)
            __m128i vc[4];
            for (int k = 0; k < 4; ++k) vc[k] = ch == 0 ? _mm_load_si128(reinterpret_cast<const __m128i*>(b.col[k])) : _mm_set1_epi32(255);
            // attenuation x diffuse of every enabled light (lanes = vertices)
            bool simpleC[8], simpleA[8];
            auto factor = [&](uint32_t li, uint32_t ctrl, bool& simple) {
                const DrawCtx::LightB& l = d.lightb[li];
                const __m128 dx = _mm_sub_ps(l.pos[0], vx), dy = _mm_sub_ps(l.pos[1], vy), dz = _mm_sub_ps(l.pos[2], vz);
                const __m128 dist = _mm_sqrt_ps(add(add(mul(dx, dx), mul(dy, dy)), mul(dz, dz)));
                const __m128 m = _mm_cmpgt_ps(dist, zero), inv = _mm_div_ps(one, dist);
                __m128 lx, ly, lz;
                if (_mm_movemask_ps(m) == 0xF) {
                    lx = mul(dx, inv);
                    ly = mul(dy, inv);
                    lz = mul(dz, inv);
                } else {
                    lx = sel(m, mul(dx, inv), dx);
                    ly = sel(m, mul(dy, inv), dy);
                    lz = sel(m, mul(dz, inv), dz);
                }
                const bool useAttn = bitsOf(ctrl, 9, 1) != 0;
                const __m128 attn = useAttn ? l.attn0 : one;
                const uint32_t diff = bitsOf(ctrl, 7, 2);
                // products with a non-negative factor and non-negative colors are non-negative: the rounding add needs no sign select
                simple = diff != 1 && (!useAttn || l.nonNeg);
                if (diff == 0) return attn;
                const __m128 nl = add(add(mul(lx, nx), mul(ly, ny)), mul(lz, nz));
                return mul(attn, diff == 1 ? nl : _mm_max_ps(nl, zero));
            };
            __m128 fc[8], fa[8];
            for (uint32_t k = 0; k < c.nC; ++k) fc[k] = factor(c.idxC[k], c.ctrlC, simpleC[k]);
            for (uint32_t k = 0; k < c.nA; ++k) {
                if (c.same) { fa[k] = fc[k]; simpleA[k] = simpleC[k]; }
                else fa[k] = factor(c.idxA[k], c.ctrlA, simpleA[k]);
            }
            __m128i res[4];            for (int comp = 0; comp < 4; ++comp) {
                const bool isAlpha = comp == 3;
                const bool matVtx = isAlpha ? c.matVtxA : c.matVtxC;
                const __m128i mat = matVtx ? vc[comp] : _mm_set1_epi32(c.mat[comp]);
                res[comp] = mat;
                if (isAlpha ? c.litA : c.litC) {
                    __m128i lacc = (isAlpha ? c.ambVtxA : c.ambVtxC) ? vc[comp] : _mm_set1_epi32(c.amb[comp]);
                    const uint32_t n = isAlpha ? c.nA : c.nC;
                    const uint8_t* idx = isAlpha ? c.idxA : c.idxC;
                    const __m128* f = isAlpha ? fa : fc;
                    const bool* simple = isAlpha ? simpleA : simpleC;
                    for (uint32_t k = 0; k < n; ++k) {
                        const __m128 v = mul(f[k], d.lightb[idx[k]].color[comp]);
                        const __m128 adj = simple[k] ? add(v, half) : sel(_mm_cmpge_ps(v, zero), add(v, half), _mm_sub_ps(v, half));
                        lacc = _mm_add_epi32(lacc, _mm_cvttps_epi32(adj));
                    }
                    lacc = _mm_max_epi32(_mm_min_epi32(lacc, i255), i0);
                    // mat <= 255 and lacc + (lacc >> 7) <= 256: the product fits 16 bits, so the cheap 16-bit multiply is exact
                    res[comp] = _mm_srai_epi32(_mm_mullo_epi16(mat, _mm_add_epi32(lacc, _mm_srai_epi32(lacc, 7))), 8);
                }
            }
            // interleave the lanes into one RGBA dword per vertex
            const __m128i bytes = _mm_packus_epi16(_mm_packus_epi32(res[0], res[1]), _mm_packus_epi32(res[2], res[3]));
            alignas(16) uint32_t w[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(w), _mm_shuffle_epi8(bytes, shuf));
            for (int i = 0; i < 4; ++i) std::memcpy(out[i].color[ch], &w[i], 4);
        }
    }
    if (d.numTexGens < 8)
        for (int i = 0; i < 4; ++i) std::memset(out[i].tex[d.numTexGens], 0, sizeof(float) * 3 * (8 - d.numTexGens));
    for (uint32_t ti = 0; ti < d.numTexGens; ++ti) {
        const TexGenCtx& t = d.tg[ti];
        __m128 in0, in1, in2;
        if (t.src == 0) { in0 = px; in1 = py; in2 = pz; }
        else if (t.src == 1) { in0 = _mm_load_ps(b.nrm[0]); in1 = _mm_load_ps(b.nrm[1]); in2 = _mm_load_ps(b.nrm[2]); }
        else if (t.src == 3 || t.src == 4) { in0 = in1 = in2 = zero; }     // binormals: not present in fast plans
        else if (t.src == 5) { in0 = _mm_load_ps(b.tex[0]); in1 = _mm_load_ps(b.tex[1]); in2 = one; }
        else { in0 = in1 = zero; in2 = one; }                              // tex1-7: not present in fast plans
        if (t.form == 0) in2 = one;
        const int idx = static_cast<int>(d.texDefault[ti] & 0x3F);
        if (d.tmbIdx[ti] != idx) {
            for (int k = 0; k < 12; ++k) d.tmb[ti][k] = _mm_set1_ps(fl(xf_.mem[idx * 4 + k]));
            d.tmbIdx[ti] = idx;
        }
        const __m128* m = d.tmb[ti];
        __m128 s = add(add(add(mul(m[0], in0), mul(m[1], in1)), mul(m[2], in2)), mul(m[3], one));
        __m128 tt = add(add(add(mul(m[4], in0), mul(m[5], in1)), mul(m[6], in2)), mul(m[7], one));
        __m128 q = t.stq ? add(add(add(mul(m[8], in0), mul(m[9], in1)), mul(m[10], in2)), mul(m[11], one)) : one;
        if (t.dual) {
            // An identity post matrix maps (s, t, q) to (s + 0, t + 0, q + 0) exactly (the zero products only decide the sign of a
            // zero result, which the final + 0 clears) when all three are finite; otherwise the full product runs.
            const __m128 inf = _mm_set1_ps(std::numeric_limits<float>::infinity());
            const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
            const bool finite = t.postIdentity && _mm_movemask_ps(_mm_and_ps(_mm_and_ps(_mm_cmplt_ps(_mm_and_ps(s, absMask), inf), _mm_cmplt_ps(_mm_and_ps(tt, absMask), inf)), _mm_cmplt_ps(_mm_and_ps(q, absMask), inf))) == 0xF;
            if (finite) {
                s = add(s, zero);
                tt = add(tt, zero);
                q = add(q, zero);
            } else {
                const __m128* p = d.postb[ti];
                const __m128 s2 = add(add(add(mul(p[0], s), mul(p[1], tt)), mul(p[2], q)), p[3]);
                const __m128 t2 = add(add(add(mul(p[4], s), mul(p[5], tt)), mul(p[6], q)), p[7]);
                const __m128 q2 = add(add(add(mul(p[8], s), mul(p[9], tt)), mul(p[10], q)), p[11]);
                s = s2;
                tt = t2;
                q = q2;
            }
        }
        __m128 t0 = s, t1 = tt, t2 = q, t3 = zero;
        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        store3(out[0].tex[ti], t0);
        store3(out[1].tex[ti], t1);
        store3(out[2].tex[ti], t2);
        store3(out[3].tex[ti], t3);
    }
}
void VertexEngine::run(uint8_t op, const uint8_t* data, uint32_t count, uint32_t cullMode, Result& out) {
    Plan& p = fresh_ == &plans_[op & 7] ? *fresh_ : plan(op & 7);
    fresh_ = nullptr;
    beginDraw(p);
    Vertex* vtx = verts_.get();
    const uint32_t size = p.f.size;
    if (p.fast) {
        RawBatch b;
        std::memset(&b, 0, sizeof b);
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 4; ++i) b.col[k][i] = 255;       // "no vertex color" reads as white (channel material source = vertex)
        const DrawCtx& d = dc_;
        const bool hasNrm = p.nrm != nullptr;
        auto load = [&](uint32_t k, uint32_t n) {
            const uint8_t* v = data + size_t(k) * size;
            p.pos(d, v, size, n, b);
            if (p.nrm) p.nrm(d, v + p.nrmOff, size, n, b);
            if (p.col0) p.col0(d, v + p.colOff, size, n, b);
            if (p.tex0) p.tex0(d, v + p.texOff, size, n, b);
        };
        // The indexed arrays are read at random places; ask for the elements of the whole draw ahead of the loaders so that the
        // cache misses overlap instead of serialising behind each vertex.
        {
            const uint32_t pIB = p.f.pos.mode == 2 ? 1 : 2, cIB = p.f.col[0].mode == 2 ? 1 : 2, tIB = p.f.tex[0].mode == 2 ? 1 : 2;
            for (uint32_t k = 0; k < count; ++k) {
                const uint8_t* v = data + size_t(k) * size;
                _mm_prefetch(reinterpret_cast<const char*>(d.win[0] + d.stride[0] * (pIB == 1 ? v[0] : uint32_t(v[0]) << 8 | v[1])), _MM_HINT_T0);
                if (p.nrm) _mm_prefetch(reinterpret_cast<const char*>(d.win[1] + d.stride[1] * v[p.nrmOff]), _MM_HINT_T0);
                if (p.col0) { const uint8_t* q = v + p.colOff; _mm_prefetch(reinterpret_cast<const char*>(d.win[2] + d.stride[2] * (cIB == 1 ? q[0] : uint32_t(q[0]) << 8 | q[1])), _MM_HINT_T0); }
                if (p.tex0) { const uint8_t* q = v + p.texOff; _mm_prefetch(reinterpret_cast<const char*>(d.win[4] + d.stride[4] * (tIB == 1 ? q[0] : uint32_t(q[0]) << 8 | q[1])), _MM_HINT_T0); }
            }
        }
        if (d.batchOk) {
            for (uint32_t k = 0; k < count; k += 4) {
                load(k, std::min<uint32_t>(4, count - k));
                transformBatch(b, hasNrm, vtx + k);
            }
        } else {
            RawVertex r;
            std::memset(&r, 0, sizeof r);
            r.pnIdx = d.pnDefault;
            for (uint32_t t = 0; t < 8; ++t) r.texIdx[t] = d.texDefault[t];
            r.hasNrm = hasNrm;
            r.hasCol[0] = p.col0 != nullptr;
            for (uint32_t k = 0; k < count; ++k) {
                load(k, 1);
                for (int c = 0; c < 3; ++c) { r.pos[c] = b.pos[c][0]; r.nrm[0][c] = b.nrm[c][0]; }
                for (int c = 0; c < 4; ++c) r.col[0][c] = static_cast<uint8_t>(b.col[c][0]);
                r.tex[0][0] = b.tex[0][0];
                r.tex[0][1] = b.tex[1][0];
                transform(r, vtx[k]);
            }
        }    } else {
        RawVertex r;
        for (uint32_t k = 0; k < count; ++k) {
            decodeGeneric(p.f, data + size_t(k) * size, r);
            transform(r, vtx[k]);
        }
    }
    // Primitive conversion (gx_ir.h convention 5) with face culling by the sign of the clip-space determinant (convention 6).
    uint16_t* ix = idx_.get();
    uint32_t n = 0, culled = 0;
    Primitive kind = Primitive::Triangles;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (cullMode != 0) {
            const float* pa = vtx[a].pos;
            const float* pb = vtx[b].pos;
            const float* pc = vtx[c].pos;
            const double det = kFrontFaceDeterminantSign *
                               (double(pa[0]) * (double(pb[1]) * pc[3] - double(pc[1]) * pb[3]) - double(pa[1]) * (double(pb[0]) * pc[3] - double(pc[0]) * pb[3]) +
                                double(pa[3]) * (double(pb[0]) * pc[1] - double(pc[0]) * pb[1]));
            const bool drop = cullMode == 3 || (cullMode == 1 && det <= 0.0) || (cullMode == 2 && det >= 0.0);
            if (drop) { ++culled; return; }
        }
        ix[n] = static_cast<uint16_t>(a);
        ix[n + 1] = static_cast<uint16_t>(b);
        ix[n + 2] = static_cast<uint16_t>(c);
        n += 3;
    };
    switch (op & 0xF8) {
        case 0x80: for (uint32_t i = 0; i + 4 <= count; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); } break;
        case 0x90: for (uint32_t i = 0; i + 3 <= count; i += 3) tri(i, i + 1, i + 2); break;
        case 0x98: for (uint32_t i = 0; i + 3 <= count; ++i) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); } break;
        case 0xA0: for (uint32_t i = 1; i + 2 <= count; ++i) tri(0, i, i + 1); break;
        case 0xA8: kind = Primitive::Lines; for (uint32_t i = 0; i + 2 <= count; i += 2) { ix[n++] = uint16_t(i); ix[n++] = uint16_t(i + 1); } break;
        case 0xB0: kind = Primitive::Lines; for (uint32_t i = 0; i + 2 <= count; ++i) { ix[n++] = uint16_t(i); ix[n++] = uint16_t(i + 1); } break;
        case 0xB8: kind = Primitive::Points; for (uint32_t i = 0; i < count; ++i) ix[n++] = uint16_t(i); break;
        default: fail("unknown primitive type", op);
    }
    out.vertices = vtx;
    out.vertexCount = count;
    out.indices = ix;
    out.indexCount = n;
    out.primitive = kind;
    out.culledTriangles = culled;
    out.batched = p.fast && dc_.batchOk;
}

}  // namespace awl::gx
