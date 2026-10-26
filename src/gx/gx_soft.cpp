#include "gx/gx_soft.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace awl::gx {

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

bool compare(CompareFunc f, float a, float b) {
    switch (f) {
        case CompareFunc::Never: return false;
        case CompareFunc::Less: return a < b;
        case CompareFunc::Equal: return a == b;
        case CompareFunc::LEqual: return a <= b;
        case CompareFunc::Greater: return a > b;
        case CompareFunc::NotEqual: return a != b;
        case CompareFunc::GEqual: return a >= b;
        default: return true;
    }
}

uint32_t packColor(const float c[4]) {
    auto q = [](float v) { return static_cast<uint32_t>(clamp01(v) * 255.0f + 0.5f); };
    return q(c[0]) | q(c[1]) << 8 | q(c[2]) << 16 | q(c[3]) << 24;
}

}  // namespace

SoftBackend::SoftBackend() : color_(size_t(kWidth) * kHeight, 0xFF000000u), depth_(size_t(kWidth) * kHeight, 1.0f) {}

void SoftBackend::sampleMap(const PipelineState& s, uint32_t map, float u, float v, float out[4]) const {
    const TexBinding& b = s.maps[map];
    const uint32_t* px = nullptr;
    uint32_t w = b.width, h = b.height;
    if (b.image) {
        px = b.image->rgba.data();
        w = b.image->width;
        h = b.image->height;
    } else if (b.efbCopyId) {
        auto it = copies_.find(b.efbCopyId);
        if (it == copies_.end()) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
        px = it->second.px.data();
        w = it->second.w;
        h = it->second.h;
    }
    if (!px || !w || !h) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    auto wrap = [](int i, int n, WrapMode m) {
        if (m == WrapMode::Repeat) { i %= n; return i < 0 ? i + n : i; }
        if (m == WrapMode::Mirror) { const int p = 2 * n; i %= p; if (i < 0) i += p; return i < n ? i : p - 1 - i; }
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    };
    auto fetch = [&](int x, int y, float c[4]) {
        const uint32_t p = px[size_t(wrap(y, int(h), b.sampler.wrapT)) * w + wrap(x, int(w), b.sampler.wrapS)];
        for (int k = 0; k < 4; ++k) c[k] = ((p >> (8 * k)) & 255) / 255.0f;
    };
    const float x = u * float(w), y = v * float(h);
    if (!b.sampler.magLinear) { fetch(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), out); return; }
    const float fx = x - 0.5f, fy = y - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    float c00[4], c10[4], c01[4], c11[4];
    fetch(x0, y0, c00); fetch(x0 + 1, y0, c10); fetch(x0, y0 + 1, c01); fetch(x0 + 1, y0 + 1, c11);
    for (int k = 0; k < 4; ++k) out[k] = (c00[k] * (1 - ax) + c10[k] * ax) * (1 - ay) + (c01[k] * (1 - ax) + c11[k] * ax) * ay;
}

void SoftBackend::shade(const PipelineState& s, const float bary[3], const Vtx* v, float depthValue, int x, int y) {
    // perspective-correct attributes: bary already holds l_i / w_i normalised
    float col[2][4] = {};
    for (int ch = 0; ch < 2; ++ch)
        for (int k = 0; k < 4; ++k) col[ch][k] = bary[0] * v[0].c[ch][k] + bary[1] * v[1].c[ch][k] + bary[2] * v[2].c[ch][k];
    float tc[8][3] = {};
    for (uint32_t t = 0; t < s.numTexCoords; ++t)
        for (int k = 0; k < 3; ++k) tc[t][k] = bary[0] * v[0].t[t][k] + bary[1] * v[1].t[t][k] + bary[2] * v[2].t[t][k];

    float reg[4][4];
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) reg[r][k] = s.tev.reg[r][k] / 255.0f;
    auto konstSel = [&](uint32_t sel, bool alpha, float c[4]) {
        c[0] = c[1] = c[2] = c[3] = 1.0f;
        if (sel < 8) { c[0] = c[1] = c[2] = c[3] = (8 - sel) / 8.0f; return; }
        if (!alpha && sel >= 0x0C && sel <= 0x0F) { for (int k = 0; k < 4; ++k) c[k] = s.tev.konst[sel - 0x0C][k] / 255.0f; return; }
        if (sel >= 0x10 && sel <= 0x1F) { const uint32_t comp = (sel - 0x10) / 4, which = (sel - 0x10) % 4; c[0] = c[1] = c[2] = c[3] = s.tev.konst[which][comp] / 255.0f; return; }
    };
    for (uint32_t i = 0; i < s.tev.numStages; ++i) {
        const TevStage& st = s.tev.stages[i];
        float tex[4] = {0, 0, 0, 0}, ras[4] = {0, 0, 0, 0};
        if (st.texMap != kNoMap && st.texCoord != kNoMap && s.maps[st.texMap].valid) {
            const TexCoordScale& ts = s.texScale[st.texCoord];
            const float q = tc[st.texCoord][2] != 0.0f ? tc[st.texCoord][2] : 1.0f;
            const float W = s.maps[st.texMap].width ? float(s.maps[st.texMap].width) : 1.0f, H = s.maps[st.texMap].height ? float(s.maps[st.texMap].height) : 1.0f;
            const float u = tc[st.texCoord][0] / q * float(ts.sSize) / W, vv = tc[st.texCoord][1] / q * float(ts.tSize) / H;
            sampleMap(s, st.texMap, u, vv, tex);
            float sw[4];
            for (int k = 0; k < 4; ++k) sw[k] = tex[s.tev.swap[st.texSwap & 3][k] & 3];
            std::memcpy(tex, sw, sizeof tex);
        }
        if (st.ras == RasColor::Color0A0) { for (int k = 0; k < 4; ++k) ras[k] = col[0][k] / 255.0f; }
        else if (st.ras == RasColor::Color1A1) { for (int k = 0; k < 4; ++k) ras[k] = col[1][k] / 255.0f; }
        {
            float sw[4];
            for (int k = 0; k < 4; ++k) sw[k] = ras[s.tev.swap[st.rasSwap & 3][k] & 3];
            std::memcpy(ras, sw, sizeof ras);
        }
        float kc[4], ka[4];
        konstSel(st.konstColor, false, kc);
        konstSel(st.konstAlpha, true, ka);
        // color
        float in[16][3];
        for (int k = 0; k < 3; ++k) {
            in[0][k] = reg[0][k]; in[1][k] = reg[0][3]; in[2][k] = reg[1][k]; in[3][k] = reg[1][3]; in[4][k] = reg[2][k]; in[5][k] = reg[2][3];
            in[6][k] = reg[3][k]; in[7][k] = reg[3][3]; in[8][k] = tex[k]; in[9][k] = tex[3]; in[10][k] = ras[k]; in[11][k] = ras[3];
            in[12][k] = 1.0f; in[13][k] = 0.5f; in[14][k] = kc[k]; in[15][k] = 0.0f;
        }
        float out[3];
        {
            const TevCombine& c = st.color;
            if (static_cast<int>(c.op) >= 8) ++notes_["TEV compare-mode stage (treated as add)"];
            const float bias = c.bias == TevBias::PlusHalf ? 0.5f : c.bias == TevBias::MinusHalf ? -0.5f : 0.0f;
            const float scale = c.scale == TevScale::X2 ? 2.0f : c.scale == TevScale::X4 ? 4.0f : c.scale == TevScale::Half ? 0.5f : 1.0f;
            for (int k = 0; k < 3; ++k) {
                const float a = in[c.in[0] & 15][k], b = in[c.in[1] & 15][k], cc = in[c.in[2] & 15][k], d = in[c.in[3] & 15][k];
                const float mix = (1.0f - cc) * a + cc * b;
                float r = ((c.op == TevOp::Sub ? d - mix : d + mix) + bias) * scale;
                out[k] = c.clamp ? clamp01(r) : r;
            }
        }
        float ain[8];
        ain[0] = reg[0][3]; ain[1] = reg[1][3]; ain[2] = reg[2][3]; ain[3] = reg[3][3]; ain[4] = tex[3]; ain[5] = ras[3]; ain[6] = ka[3]; ain[7] = 0.0f;
        float aout;
        {
            const TevCombine& c = st.alpha;
            const float bias = c.bias == TevBias::PlusHalf ? 0.5f : c.bias == TevBias::MinusHalf ? -0.5f : 0.0f;
            const float scale = c.scale == TevScale::X2 ? 2.0f : c.scale == TevScale::X4 ? 4.0f : c.scale == TevScale::Half ? 0.5f : 1.0f;
            const float a = ain[c.in[0] & 7], b = ain[c.in[1] & 7], cc = ain[c.in[2] & 7], d = ain[c.in[3] & 7];
            const float mix = (1.0f - cc) * a + cc * b;
            const float r = ((c.op == TevOp::Sub ? d - mix : d + mix) + bias) * scale;
            aout = c.clamp ? clamp01(r) : r;
        }
        const int dc = static_cast<int>(st.color.dest) & 3, da = static_cast<int>(st.alpha.dest) & 3;
        for (int k = 0; k < 3; ++k) reg[dc][k] = out[k];
        reg[da][3] = aout;
    }
    float result[4] = {reg[0][0], reg[0][1], reg[0][2], reg[0][3]};
    // alpha test
    {
        const AlphaCompare& ac = s.alphaCompare;
        const float a8 = std::floor(clamp01(result[3]) * 255.0f + 0.5f);
        const bool t0 = compare(ac.comp0, a8, ac.ref0), t1 = compare(ac.comp1, a8, ac.ref1);
        const bool pass = ac.logic == AlphaLogic::And ? (t0 && t1) : ac.logic == AlphaLogic::Or ? (t0 || t1) : ac.logic == AlphaLogic::Xor ? (t0 != t1) : (t0 == t1);
        if (!pass) return;
    }
    const size_t idx = size_t(y) * kWidth + x;
    if (s.depth.test && !compare(s.depth.func, depthValue, depth_[idx])) return;
    if (s.depth.write) depth_[idx] = depthValue;
    if (s.fog.type != FogType::None) ++notes_["fog not applied"];
    const uint32_t dstPx = color_[idx];
    float dst[4];
    for (int k = 0; k < 4; ++k) dst[k] = ((dstPx >> (8 * k)) & 255) / 255.0f;
    float res[4];
    for (int k = 0; k < 4; ++k) res[k] = clamp01(result[k]);
    if (s.blend.mode == BlendMode::Blend) {
        auto factor = [&](BlendFactor f, bool isSrcFactor, int comp) {
            switch (f) {
                case BlendFactor::Zero: return 0.0f;
                case BlendFactor::One: return 1.0f;
                case BlendFactor::OtherColor: return isSrcFactor ? dst[comp] : res[comp];
                case BlendFactor::InvOtherColor: return 1.0f - (isSrcFactor ? dst[comp] : res[comp]);
                case BlendFactor::SrcAlpha: return res[3];
                case BlendFactor::InvSrcAlpha: return 1.0f - res[3];
                case BlendFactor::DstAlpha: return dst[3];
                default: return 1.0f - dst[3];
            }
        };
        float blended[4];
        for (int k = 0; k < 4; ++k) blended[k] = clamp01(res[k] * factor(s.blend.src, true, k) + dst[k] * factor(s.blend.dst, false, k));
        for (int k = 0; k < 4; ++k) res[k] = blended[k];
    } else if (s.blend.mode == BlendMode::Subtract || s.blend.mode == BlendMode::Logic) {
        ++notes_["subtract/logic blend treated as copy"];
    }
    float outc[4];
    for (int k = 0; k < 3; ++k) outc[k] = s.blend.colorUpdate ? res[k] : dst[k];
    outc[3] = s.blend.alphaUpdate ? res[3] : dst[3];
    color_[idx] = packColor(outc);
    ++pixels_;
}

void SoftBackend::rasterize(const PipelineState& s, const Vtx* vt) {
    const Vtx* v[3] = {&vt[0], &vt[1], &vt[2]};
    float X[3], Y[3], Z[3], IW[3];
    for (int i = 0; i < 3; ++i) {
        IW[i] = 1.0f / v[i]->p[3];
        X[i] = v[i]->p[0] * IW[i] * s.viewport.scale[0] + s.viewport.offset[0] - 342.0f;
        Y[i] = v[i]->p[1] * IW[i] * s.viewport.scale[1] + s.viewport.offset[1] - 342.0f;
        Z[i] = (v[i]->p[2] * IW[i] * s.viewport.scale[2] + s.viewport.offset[2]) / 16777215.0f;
    }
    const float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0.0f || !std::isfinite(area)) return;
    const int x0 = std::max<int>(std::max(0, s.scissor.x0), static_cast<int>(std::floor(std::min({X[0], X[1], X[2]}))));
    const int x1 = std::min<int>(std::min<int>(kWidth - 1, s.scissor.x1), static_cast<int>(std::ceil(std::max({X[0], X[1], X[2]}))));
    const int y0 = std::max<int>(std::max(0, s.scissor.y0), static_cast<int>(std::floor(std::min({Y[0], Y[1], Y[2]}))));
    const int y1 = std::min<int>(std::min<int>(kHeight - 1, s.scissor.y1), static_cast<int>(std::ceil(std::max({Y[0], Y[1], Y[2]}))));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float l0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
            const float l1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
            const float l2 = 1.0f - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const float depthValue = l0 * Z[0] + l1 * Z[1] + l2 * Z[2];
            if (depthValue < 0.0f || depthValue > 1.0f) continue;   // outside the near/far planes
            const float p0 = l0 * IW[0], p1 = l1 * IW[1], p2 = l2 * IW[2], sum = p0 + p1 + p2;
            const float bary[3] = {p0 / sum, p1 / sum, p2 / sum};
            shade(s, bary, vt, depthValue, x, y);
        }
    }
}

void SoftBackend::draw(const DrawCommand& d) {
    if (d.primitive != Primitive::Triangles) { ++notes_["lines/points not drawn"]; return; }
    const PipelineState& s = *d.state;
    auto make = [&](const Vertex& v) {
        Vtx o;
        std::memcpy(o.p, v.pos, sizeof o.p);
        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < 4; ++k) o.c[ch][k] = v.color[ch][k];
        std::memcpy(o.t, v.tex, sizeof o.t);
        return o;
    };
    constexpr float kEpsW = 1e-3f;
    for (uint32_t i = 0; i + 3 <= d.indexCount; i += 3) {
        Vtx tri[3] = {make(d.vertices[d.indices[i]]), make(d.vertices[d.indices[i + 1]]), make(d.vertices[d.indices[i + 2]])};
        // clip against w >= eps (Sutherland-Hodgman, attributes interpolated linearly in clip space)
        std::vector<Vtx> poly(tri, tri + 3), next;
        for (size_t k = 0; k < poly.size(); ++k) {
            const Vtx& p = poly[k];
            const Vtx& q = poly[(k + 1) % poly.size()];
            const bool pin = p.p[3] >= kEpsW, qin = q.p[3] >= kEpsW;
            if (pin) next.push_back(p);
            if (pin != qin) {
                const float t = (kEpsW - p.p[3]) / (q.p[3] - p.p[3]);
                Vtx m;
                const float* pf = reinterpret_cast<const float*>(&p);
                const float* qf = reinterpret_cast<const float*>(&q);
                float* mf = reinterpret_cast<float*>(&m);
                for (size_t f = 0; f < sizeof(Vtx) / sizeof(float); ++f) mf[f] = pf[f] + (qf[f] - pf[f]) * t;
                next.push_back(m);
            }
        }
        if (next.size() < 3) continue;
        ++tris_;
        for (size_t k = 1; k + 1 < next.size(); ++k) {
            Vtx t3[3] = {next[0], next[k], next[k + 1]};
            // rasterize with these three vertices (attributes are read from t3)
            rasterize(s, t3);
        }
    }
}

void SoftBackend::efbCopy(const EfbCopyCommand& c) {
    Image img;
    img.w = c.destWidth;
    img.h = c.destHeight;
    img.px.assign(size_t(img.w) * img.h, 0);
    for (uint32_t y = 0; y < img.h; ++y) {
        for (uint32_t x = 0; x < img.w; ++x) {
            const uint32_t sx = c.srcX + x * c.srcWidth / img.w, sy = c.srcY + y * c.srcHeight / img.h;
            if (sx < kWidth && sy < kHeight) img.px[size_t(y) * img.w + x] = color_[size_t(sy) * kWidth + sx] | 0xFF000000u;
        }
    }
    if (c.target == CopyTarget::Xfb) {
        xfb_.push_back(std::move(img));
        if (xfb_.size() > keep_) xfb_.erase(xfb_.begin());
    } else {
        copies_[c.id] = std::move(img);
        if (copies_.size() > 64) copies_.erase(copies_.begin());
    }
    if (c.clear.color || c.clear.alpha || c.clear.z) {
        const uint32_t cc = c.clear.rgba[0] | c.clear.rgba[1] << 8 | c.clear.rgba[2] << 16 | uint32_t(c.clear.rgba[3]) << 24;
        const float zc = c.clear.z24 / 16777215.0f;
        for (uint32_t y = c.srcY; y < c.srcY + c.srcHeight && y < kHeight; ++y)
            for (uint32_t x = c.srcX; x < c.srcX + c.srcWidth && x < kWidth; ++x) {
                if (c.clear.color || c.clear.alpha) color_[size_t(y) * kWidth + x] = cc;
                if (c.clear.z) depth_[size_t(y) * kWidth + x] = zc;
            }
    }
}

}  // namespace awl::gx
