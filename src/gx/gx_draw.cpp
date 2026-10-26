// GX core: vertex loading, the XF stage on the CPU (transform, projection, lighting channels, texture-coordinate
// generation) and primitive conversion. Formulas and register layouts: YAGCD chapter 5 (VCD/VAT, XF registers, matrix
// memory) and the libogc API documentation of the lighting and texgen functions (docs/research/gx-core.md, section 3).
//
// Everything that cannot change inside one draw command (matrices, lights, projection, texgen setup, array windows) is
// decoded once into DrawCtx, and per-vertex work reuses it: the title scenes draw 100,000 vertices per frame.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "gx/gx_core.h"

namespace awl::gx {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t bitsOf(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
float fl(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
uint32_t compBytes(uint32_t f) { return f <= 1 ? 1 : f <= 3 ? 2 : 4; }
uint32_t colBytes(uint32_t f) { return f == 0 || f == 3 ? 2 : f == 1 || f == 4 ? 3 : 4; }

// One coordinate component: u8, s8, u16, s16 (divided by 2^shift) or f32.
float readComp(const uint8_t*& p, uint32_t fmt, uint32_t shift, float scale) {
    switch (fmt) {
        case 0: { const float v = float(p[0]) * scale; p += 1; return v; }
        case 1: { const float v = float(static_cast<int8_t>(p[0])) * scale; p += 1; return v; }
        case 2: { const float v = float(uint32_t(p[0]) << 8 | p[1]) * scale; p += 2; return v; }
        case 3: { const float v = float(static_cast<int16_t>(uint16_t(p[0]) << 8 | p[1])) * scale; p += 2; return v; }
        default: { const float v = fl(be32(p)); p += 4; (void)shift; return v; }
    }
}
// Normal components are fixed-point with a fixed scale: s8 -> 1/64, s16 -> 1/16384 (YAGCD VAT, normal format).
float readNrm(const uint8_t*& p, uint32_t fmt) {
    if (fmt == 1) { const float v = float(static_cast<int8_t>(p[0])) / 64.0f; p += 1; return v; }
    if (fmt == 3) { const float v = float(static_cast<int16_t>(uint16_t(p[0]) << 8 | p[1])) / 16384.0f; p += 2; return v; }
    const float v = fl(be32(p));
    p += 4;
    return v;
}
uint8_t rep(uint32_t v, uint32_t bits) { return static_cast<uint8_t>(bits == 4 ? v * 17 : bits == 5 ? (v << 3) | (v >> 2) : bits == 6 ? (v << 2) | (v >> 4) : v); }

void readColor(const uint8_t* p, uint32_t fmt, uint8_t out[4]) {
    switch (fmt) {
        case 0: { const uint32_t v = uint32_t(p[0]) << 8 | p[1]; out[0] = rep(v >> 11, 5); out[1] = rep((v >> 5) & 63, 6); out[2] = rep(v & 31, 5); out[3] = 255; break; }
        case 1: case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
        case 3: { const uint32_t v = uint32_t(p[0]) << 8 | p[1]; out[0] = rep(v >> 12, 4); out[1] = rep((v >> 8) & 15, 4); out[2] = rep((v >> 4) & 15, 4); out[3] = rep(v & 15, 4); break; }
        case 4: { const uint32_t v = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; out[0] = rep(v >> 18, 6); out[1] = rep((v >> 12) & 63, 6); out[2] = rep((v >> 6) & 63, 6); out[3] = rep(v & 63, 6); break; }
        default: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; break;
    }
}

// Face determination uses the determinant of the (x, y, w) rows of the three clip-space vertices: 2 x the signed area
// (counter-clockwise positive, y up) for w = 1, invariant to clipping. The game draws its logo quads clockwise as seen on
// the screen and keeps them with the cull-back state (BP GEN_MODE hardware value 1), so a front face is CLOCKWISE on the
// screen, i.e. a negative determinant (gx_ir.h convention 6, version 2). This is the single place the sign lives.
constexpr double kFrontFaceDeterminantSign = -1.0;

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void norm3(float* v) {
    const float l = std::sqrt(dot3(v, v));
    if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Attenuation x diffuse factor of one light for one channel control word (YAGCD XF channel control: bits 7-8 diffuse
// function, bit 9 attenuation enable, bit 10 spotlight/specular select).
float lightFactor(const Core::LightObj& l, uint32_t ctrl, const float p[3], const float n[3]) {
    float ldir[3] = {l.pos[0] - p[0], l.pos[1] - p[1], l.pos[2] - p[2]};
    const float dist2 = dot3(ldir, ldir), dist = std::sqrt(dist2);
    if (dist > 0.0f) { const float inv = 1.0f / dist; ldir[0] *= inv; ldir[1] *= inv; ldir[2] *= inv; }
    float attn = 1.0f;
    if (bitsOf(ctrl, 9, 1)) {
        if (bitsOf(ctrl, 10, 1)) {                       // spotlight: cosine attenuation over distance attenuation
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

}  // namespace

Core::VtxFmt Core::vertexFormat(uint32_t vat) const {
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

// Decodes what stays constant during one draw: projection, channel controls, texgen setup, texture/post matrices and
// windows into the CP arrays (a window is the whole range an 8- or 16-bit index can reach; null when it does not fit
// in memory, then each element is read and range-checked individually).
void Core::beginDraw(const VtxFmt& f) {
    DrawCtx& d = dc_;
    for (int i = 0; i < 6; ++i) d.proj[i] = fl(xf_.reg[0x20 + i]);
    d.ortho = xf_.reg[0x26] != 0;
    d.numColors = std::min<uint32_t>(xf_.reg[0x09], 2);
    d.numTexGens = std::min<uint32_t>(xf_.reg[0x3F], 8);
    d.lit = false;
    for (uint32_t ch = 0; ch < 2; ++ch) {
        d.chan[ch][0] = xf_.reg[0x0E + ch];
        d.chan[ch][1] = xf_.reg[0x10 + ch];
        if (ch < d.numColors && (bitsOf(d.chan[ch][0], 1, 1) || bitsOf(d.chan[ch][1], 1, 1))) d.lit = true;
    }
    d.lightLoaded = 0;
    d.pnIdx = -1;
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
        t.idx = -1;
        if (t.dual) {
            const uint32_t* P = &xf_.mem[0x500 + bitsOf(post, 0, 6) * 4];
            for (int k = 0; k < 12; ++k) t.post[k] = fl(P[k]);
        }
    }
    auto window = [&](uint32_t array, uint32_t mode) {
        d.win[array] = nullptr;
        d.winBytes[array] = 0;
        if (mode < 2) return;
        const uint32_t span = cp_.arrayStride[array] * (mode == 2 ? 256u : 65536u) + 64;
        d.win[array] = mem_.read(cp_.arrayBase[array], span);
        d.winBytes[array] = d.win[array] ? span : 0;
    };
    window(0, f.pos.mode);
    window(1, f.nrm.mode);
    window(2, f.col[0].mode);
    window(3, f.col[1].mode);
    for (uint32_t t = 0; t < 8; ++t) window(4 + t, f.tex[t].mode);
    // Scale factors of the fixed-point coordinate formats: 2^-shift
    d.posScale = std::ldexp(1.0f, -static_cast<int>(f.pos.shift));
    for (uint32_t t = 0; t < 8; ++t) d.texScale[t] = std::ldexp(1.0f, -static_cast<int>(f.tex[t].shift));
}

const Core::LightObj& Core::light(uint32_t i) {
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
        d.lightLoaded |= 1u << i;
    }
    return d.lights[i];
}

// Returns the element of an attribute: the next bytes of the stream (direct) or the entry of the CP array (indexed).
const uint8_t* Core::elem(const AttrFmt& a, uint32_t array, const uint8_t*& p, uint32_t directBytes, uint32_t elemBytes) {
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

void Core::decodeVertex(const VtxFmt& f, const uint8_t* p, RawVertex& r) {
    r.pnIdx = f.pnmtx ? *p++ : bitsOf(xf_.reg[0x18], 0, 6);
    for (uint32_t t = 0; t < 8; ++t) {
        if (f.texmtx[t]) r.texIdx[t] = *p++;
        else r.texIdx[t] = t < 4 ? bitsOf(xf_.reg[0x18], 6 * (t + 1), 6) : bitsOf(xf_.reg[0x19], 6 * (t - 4), 6);
    }
    r.pos[0] = r.pos[1] = r.pos[2] = 0.0f;
    if (f.pos.mode) {
        const uint32_t n = f.pos.cnt ? 3 : 2, db = n * compBytes(f.pos.fmt);
        const uint8_t* e = elem(f.pos, 0, p, db, db);
        for (uint32_t i = 0; i < n; ++i) r.pos[i] = readComp(e, f.pos.fmt, f.pos.shift, dc_.posScale);
    }
    r.hasNrm = false;
    if (f.nrm.mode) {
        const uint32_t comps = f.nrm.cnt ? 9 : 3, cb = f.nrm.fmt == 1 ? 1 : f.nrm.fmt == 3 ? 2 : 4;
        r.hasNrm = true;
        for (int v = 0; v < 3; ++v) r.nrm[v][0] = r.nrm[v][1] = r.nrm[v][2] = 0.0f;
        if (f.nrm.mode >= 2 && f.nrm.cnt && f.nrmIdx3) {
            for (uint32_t v = 0; v < 3; ++v) {
                const uint8_t* e = elem(f.nrm, 1, p, 3 * cb, 3 * cb);
                for (uint32_t i = 0; i < 3; ++i) r.nrm[v][i] = readNrm(e, f.nrm.fmt);
            }
        } else {
            const uint8_t* e = elem(f.nrm, 1, p, comps * cb, comps * cb);
            for (uint32_t v = 0; v < comps / 3; ++v)
                for (uint32_t i = 0; i < 3; ++i) r.nrm[v][i] = readNrm(e, f.nrm.fmt);
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
        for (uint32_t i = 0; i < n; ++i) r.tex[t][i] = readComp(e, f.tex[t].fmt, f.tex[t].shift, dc_.texScale[t]);
    }
}

void Core::lightChannels(const RawVertex& r, const float vpos[3], const float vnrm[3], uint8_t color[2][4]) {
    DrawCtx& d = dc_;
    static const uint8_t white[4] = {255, 255, 255, 255};
    for (uint32_t ch = 0; ch < d.numColors; ++ch) {
        const uint32_t cc = d.chan[ch][0], ac = d.chan[ch][1];
        const uint32_t ambReg = xf_.reg[0x0A + ch], matReg = xf_.reg[0x0C + ch];
        const uint8_t* vtx = r.hasCol[ch] ? r.col[ch] : white;
        // Attenuation x diffuse of each enabled light for the color control and the alpha control (computed once, shared by the components).
        float fc[8] = {}, fa[8] = {};
        const uint32_t maskC = bitsOf(cc, 1, 1) ? (bitsOf(cc, 2, 4) | bitsOf(cc, 11, 4) << 4) : 0;
        const uint32_t maskA = bitsOf(ac, 1, 1) ? (bitsOf(ac, 2, 4) | bitsOf(ac, 11, 4) << 4) : 0;
        for (uint32_t i = 0; i < 8; ++i) {
            if (maskC & (1u << i)) fc[i] = lightFactor(light(i), cc, vpos, vnrm);
            if (maskA & (1u << i)) fa[i] = (maskA == maskC && ac == cc) ? fc[i] : lightFactor(light(i), ac, vpos, vnrm);
        }
        for (int comp = 0; comp < 4; ++comp) {
            const bool isAlpha = comp == 3;
            const uint32_t ctrl = isAlpha ? ac : cc;
            const int reg = static_cast<int>((matReg >> (24 - 8 * comp)) & 0xFF);
            const int mat = bitsOf(ctrl, 0, 1) ? vtx[comp] : reg;
            if (!bitsOf(ctrl, 1, 1)) { color[ch][comp] = static_cast<uint8_t>(mat); continue; }
            int lacc = bitsOf(ctrl, 6, 1) ? vtx[comp] : static_cast<int>((ambReg >> (24 - 8 * comp)) & 0xFF);
            const uint32_t mask = isAlpha ? maskA : maskC;
            const float* f = isAlpha ? fa : fc;
            for (uint32_t i = 0; i < 8; ++i)
                if (mask & (1u << i)) { const float v = f[i] * d.lights[i].color[comp]; lacc += static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f); }
            lacc = std::clamp(lacc, 0, 255);
            // out = mat * lacc / 255 with the hardware's exact-at-255 rounding: (mat * (l + (l >> 7))) >> 8
            color[ch][comp] = static_cast<uint8_t>((mat * (lacc + (lacc >> 7))) >> 8);
        }
    }
}

void Core::texGen(const RawVertex& r, const uint8_t color[2][4], Vertex& out) {
    DrawCtx& d = dc_;
    for (uint32_t i = 0; i < d.numTexGens; ++i) {
        TexGenCtx& t = d.tg[i];
        float s = 0, tt = 0, q = 1;
        if (t.type == 0) {
            float in[4] = {0, 0, 0, 1};
            if (t.src == 0) { in[0] = r.pos[0]; in[1] = r.pos[1]; in[2] = r.pos[2]; }
            else if (t.src == 1) { in[0] = r.nrm[0][0]; in[1] = r.nrm[0][1]; in[2] = r.nrm[0][2]; }
            else if (t.src == 3) { in[0] = r.nrm[2][0]; in[1] = r.nrm[2][1]; in[2] = r.nrm[2][2]; }   // binormal T = tangent in NBT order
            else if (t.src == 4) { in[0] = r.nrm[1][0]; in[1] = r.nrm[1][1]; in[2] = r.nrm[1][2]; }
            else if (t.src >= 5 && t.src <= 12) { in[0] = r.tex[t.src - 5][0]; in[1] = r.tex[t.src - 5][1]; in[2] = 1.0f; }
            else fail("texgen source row not modelled for the regular type: " + std::to_string(t.src), 0x1040 + i);
            if (t.form == 0) in[2] = 1.0f;            // AB11: (a, b, 1, 1)
            const int idx = static_cast<int>(r.texIdx[i] & 0x3F);
            if (t.idx != idx) {
                const uint32_t* M = &xf_.mem[idx * 4];
                for (int k = 0; k < 12; ++k) t.m[k] = fl(M[k]);
                t.idx = idx;
            }
            const float* m = t.m;
            s = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
            tt = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
            q = t.stq ? m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3] : 1.0f;
            if (t.dual) {
                float v[3] = {s, tt, q};
                if (t.normalize) norm3(v);
                const float* p = t.post;
                s = p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + p[3];
                tt = p[4] * v[0] + p[5] * v[1] + p[6] * v[2] + p[7];
                q = p[8] * v[0] + p[9] * v[1] + p[10] * v[2] + p[11];
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

void Core::transformVertex(const RawVertex& r, Vertex& out) {
    DrawCtx& d = dc_;
    // Position matrix (3x4, rows of 4 words at 4 * index) -> view space.
    const int pn = static_cast<int>(r.pnIdx & 0x3F);
    if (d.pnIdx != pn) {
        const uint32_t* M = &xf_.mem[pn * 4];
        for (int k = 0; k < 12; ++k) d.pm[k] = fl(M[k]);
        d.pnIdx = pn;
        d.nmValid = false;
    }
    const float* m = d.pm;
    float vp[3];
    for (int k = 0; k < 3; ++k) vp[k] = m[4 * k] * r.pos[0] + m[4 * k + 1] * r.pos[1] + m[4 * k + 2] * r.pos[2] + m[4 * k + 3];
    // Projection (XF 0x1020-0x1026; YAGCD): perspective x' = A x + B z, y' = C y + D z, z' = E z + F, w' = -z; orthographic
    // x' = A x + B, y' = C y + D, z' = E z + F, w' = 1.
    const float* P = d.proj;
    if (!d.ortho) {
        out.pos[0] = P[0] * vp[0] + P[1] * vp[2];
        out.pos[1] = P[2] * vp[1] + P[3] * vp[2];
        out.pos[2] = P[4] * vp[2] + P[5];
        out.pos[3] = -vp[2];
    } else {
        out.pos[0] = P[0] * vp[0] + P[1];
        out.pos[1] = P[2] * vp[1] + P[3];
        out.pos[2] = P[4] * vp[2] + P[5];
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
                for (int k = 0; k < 9; ++k) d.nm[k] = fl(xf_.mem[base + k]);
                d.nmValid = true;
            }
            const float* n = d.nm;
            for (int k = 0; k < 3; ++k) vn[k] = n[3 * k] * r.nrm[0][0] + n[3 * k + 1] * r.nrm[0][1] + n[3 * k + 2] * r.nrm[0][2];
            norm3(vn);
        }
        lightChannels(r, vp, vn, out.color);
    }
    if (d.numTexGens != 0) {
        std::memset(out.tex, 0, sizeof out.tex);
        texGen(r, out.color, out);
    } else {
        std::memset(out.tex, 0, sizeof out.tex);
    }
}

bool Core::draw(uint8_t op, const uint8_t* d, size_t n, size_t& used) {
    if (n < 3) return false;
    const uint32_t count = uint32_t(d[1]) << 8 | d[2], vat = op & 7;
    const VtxFmt f = vertexFormat(vat);
    if (f.size == 0) fail("GX draw with an empty vertex descriptor", op);
    const size_t total = 3 + size_t(count) * f.size;
    if (n < total) return false;
    used = total;
    ++stats_.draws;
    stats_.vertices += count;
    const auto tA = std::chrono::steady_clock::now();
    if (dirty_) rebuildPipeline();
    const auto tB = std::chrono::steady_clock::now();
    const uint32_t prim = op & 0xF8;
    verts_.resize(count);
    RawVertex raw;
    beginDraw(f);
    const uint8_t* p = d + 3;
    for (uint32_t k = 0; k < count; ++k, p += f.size) {
        decodeVertex(f, p, raw);
        transformVertex(raw, verts_[k]);
    }
    // Primitive conversion (gx_ir.h convention 5).
    idx_.clear();
    Primitive kind = Primitive::Triangles;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { idx_.push_back(uint16_t(a)); idx_.push_back(uint16_t(b)); idx_.push_back(uint16_t(c)); };
    switch (prim) {
        case 0x80: for (uint32_t i = 0; i + 4 <= count; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); } break;
        case 0x90: for (uint32_t i = 0; i + 3 <= count; i += 3) tri(i, i + 1, i + 2); break;
        case 0x98: for (uint32_t i = 0; i + 3 <= count; ++i) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); } break;
        case 0xA0: for (uint32_t i = 1; i + 2 <= count; ++i) tri(0, i, i + 1); break;
        case 0xA8: kind = Primitive::Lines; for (uint32_t i = 0; i + 2 <= count; i += 2) { idx_.push_back(uint16_t(i)); idx_.push_back(uint16_t(i + 1)); } break;
        case 0xB0: kind = Primitive::Lines; for (uint32_t i = 0; i + 2 <= count; ++i) { idx_.push_back(uint16_t(i)); idx_.push_back(uint16_t(i + 1)); } break;
        case 0xB8: kind = Primitive::Points; for (uint32_t i = 0; i < count; ++i) idx_.push_back(uint16_t(i)); break;
        default: fail("unknown primitive type", op);
    }
    if (kind == Primitive::Triangles) {
        // Face culling by the sign of the clip-space determinant (convention 6); culled triangles are dropped.
        const uint32_t cull = pipe_->cullMode;
        if (cull != 0) {
            size_t w = 0;
            for (size_t i = 0; i + 3 <= idx_.size(); i += 3) {
                const float* a = verts_[idx_[i]].pos;
                const float* b = verts_[idx_[i + 1]].pos;
                const float* c = verts_[idx_[i + 2]].pos;
                const double det = kFrontFaceDeterminantSign *
                                   (double(a[0]) * (double(b[1]) * c[3] - double(c[1]) * b[3]) - double(a[1]) * (double(b[0]) * c[3] - double(c[0]) * b[3]) +
                                    double(a[3]) * (double(b[0]) * c[1] - double(c[0]) * b[1]));
                const bool drop = cull == 3 || (cull == 1 && det <= 0.0) || (cull == 2 && det >= 0.0);
                if (drop) { ++stats_.culledTriangles; continue; }
                idx_[w] = idx_[i]; idx_[w + 1] = idx_[i + 1]; idx_[w + 2] = idx_[i + 2];
                w += 3;
            }
            idx_.resize(w);
        }
        stats_.triangles += idx_.size() / 3;
    }
    const auto tC = std::chrono::steady_clock::now();
    stats_.pipelineSeconds += std::chrono::duration<double>(tB - tA).count();
    stats_.vertexSeconds += std::chrono::duration<double>(tC - tB).count();
    if (idx_.empty()) return true;
    DrawCommand cmd{drawSeq_++, stateId_, pipe_.get(), kind, verts_.data(), count, idx_.data(), static_cast<uint32_t>(idx_.size())};
    backend_.draw(cmd);
    stats_.backendSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - tC).count();
    return true;
}

}  // namespace awl::gx
