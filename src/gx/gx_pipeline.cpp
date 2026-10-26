// GX core: BP/XF register state -> IR pipeline state, texture binding, EFB copies. Register layouts: YAGCD chapter 5 and
// libogc gx.c (the SDK functions that write them); see docs/research/gx-core.md section 5 for the per-field sources.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "gx/gx_core.h"

namespace awl::gx {

namespace {

uint32_t bitsOf(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
float fl(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
int sext(uint32_t v, uint32_t n) { return static_cast<int>(v << (32 - n)) >> (32 - n); }

// libogc _gx2HWFiltConv: GX filter -> hardware field NEAR 0, NEAR_MIP_NEAR 1, NEAR_MIP_LIN 2, LINEAR 4, LIN_MIP_NEAR 5, LIN_MIP_LIN 6.
FilterMode minFilterFromHw(uint32_t hw) {
    switch (hw) {
        case 0: return FilterMode::Near;
        case 1: return FilterMode::NearMipNear;
        case 2: return FilterMode::NearMipLin;
        case 4: return FilterMode::Linear;
        case 5: return FilterMode::LinMipNear;
        case 6: return FilterMode::LinMipLin;
        default: return FilterMode::Linear;
    }
}
bool usesMips(FilterMode f) { return f != FilterMode::Near && f != FilterMode::Linear; }

TevCombine decodeCombine(uint32_t v, bool alpha) {
    TevCombine c{};
    if (alpha) { c.in[0] = uint8_t(bitsOf(v, 13, 3)); c.in[1] = uint8_t(bitsOf(v, 10, 3)); c.in[2] = uint8_t(bitsOf(v, 7, 3)); c.in[3] = uint8_t(bitsOf(v, 4, 3)); }
    else { c.in[0] = uint8_t(bitsOf(v, 12, 4)); c.in[1] = uint8_t(bitsOf(v, 8, 4)); c.in[2] = uint8_t(bitsOf(v, 4, 4)); c.in[3] = uint8_t(bitsOf(v, 0, 4)); }
    const uint32_t bias = bitsOf(v, 16, 2), sub = bitsOf(v, 18, 1), scale = bitsOf(v, 20, 2);
    if (bias == 3) {                       // compare mode (libogc GX_SetTevColorOp: scale = tevop >> 1, op = tevop & 1)
        c.op = static_cast<TevOp>(8 + (scale << 1 | sub));
        c.bias = TevBias::Zero;
        c.scale = TevScale::X1;
    } else {
        c.op = sub ? TevOp::Sub : TevOp::Add;
        c.bias = static_cast<TevBias>(bias);
        c.scale = static_cast<TevScale>(scale);
    }
    c.clamp = bitsOf(v, 19, 1) != 0;
    c.dest = static_cast<TevDest>(bitsOf(v, 22, 2));
    return c;
}

// Bytes of a copy-to-texture destination (libogc __GetImageTileCount): tiles x 32 bytes, RGBA8 and Z24X8 use two planes.
uint32_t copyBytes(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t xs, ys;
    switch (fmt) {
        case 0x00: case 0x02: case 0x20: case 0x22: case 0x30: xs = 3; ys = 3; break;
        case 0x11: case 0x01: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x39: case 0x3A: xs = 3; ys = 2; break;
        default: xs = 2; ys = 2; break;
    }
    const uint32_t planes = (fmt == 0x06 || fmt == 0x16) ? 2 : 1;
    return ((w + (1u << xs) - 1) >> xs) * ((h + (1u << ys) - 1) >> ys) * planes * 32;
}

}  // namespace

void Core::buildTev(TevState& t) const {
    const uint32_t gm = bp_[0x00];
    t.numStages = static_cast<uint8_t>(bitsOf(gm, 10, 4) + 1);
    t.numIndirect = static_cast<uint8_t>(bitsOf(gm, 16, 3));
    if (t.numIndirect > 4) throw GxError("GEN_MODE: more than 4 indirect stages", offset_, 0x00);
    for (uint32_t s = 0; s < t.numStages; ++s) {
        TevStage& st = t.stages[s];
        const uint32_t tref = bp_[0x28 + s / 2], sh = (s & 1) ? 12 : 0;
        const bool en = bitsOf(tref, sh + 6, 1) != 0;
        st.texMap = en ? static_cast<uint8_t>(bitsOf(tref, sh, 3)) : kNoMap;
        st.texCoord = en ? static_cast<uint8_t>(bitsOf(tref, sh + 3, 3)) : kNoMap;
        const uint32_t cc = bitsOf(tref, sh + 7, 3);
        if (cc == 2 || cc == 3 || cc == 4) throw GxError("TEV stage with an undefined raster color channel", offset_, 0x28 + s / 2);
        st.ras = static_cast<RasColor>(cc);
        const uint32_t ce = bp_[0xC0 + 2 * s], ae = bp_[0xC1 + 2 * s], ks = bp_[0xF6 + s / 2];
        st.color = decodeCombine(ce, false);
        st.alpha = decodeCombine(ae, true);
        st.rasSwap = static_cast<uint8_t>(bitsOf(ae, 0, 2));
        st.texSwap = static_cast<uint8_t>(bitsOf(ae, 2, 2));
        st.konstColor = static_cast<uint8_t>((s & 1) ? bitsOf(ks, 14, 5) : bitsOf(ks, 4, 5));
        st.konstAlpha = static_cast<uint8_t>((s & 1) ? bitsOf(ks, 19, 5) : bitsOf(ks, 9, 5));
        const uint32_t ic = bp_[0x10 + s];
        if (t.numIndirect != 0 && ic != 0) {
            IndirectOp& io = st.indirect;
            io.indStage = static_cast<uint8_t>(bitsOf(ic, 0, 2));
            io.format = static_cast<uint8_t>(bitsOf(ic, 2, 2));
            io.biasMask = static_cast<uint8_t>(bitsOf(ic, 4, 3));
            io.alphaSelect = static_cast<uint8_t>(bitsOf(ic, 7, 2));
            io.matrix = static_cast<uint8_t>(bitsOf(ic, 9, 4));
            io.wrapS = static_cast<uint8_t>(bitsOf(ic, 13, 3));
            io.wrapT = static_cast<uint8_t>(bitsOf(ic, 16, 3));
            io.useOriginalLod = bitsOf(ic, 19, 1) != 0;
            io.addPrevious = bitsOf(ic, 20, 1) != 0;
        } else {
            st.indirect.indStage = kNoMap;
        }
    }
    for (uint32_t slot = 0; slot < 4; ++slot) {
        const uint32_t cl = tevColor_[0][slot][0], ch = tevColor_[0][slot][1];
        t.reg[slot][0] = static_cast<int16_t>(sext(bitsOf(cl, 0, 11), 11));
        t.reg[slot][3] = static_cast<int16_t>(sext(bitsOf(cl, 12, 11), 11));
        t.reg[slot][1] = static_cast<int16_t>(sext(bitsOf(ch, 12, 11), 11));
        t.reg[slot][2] = static_cast<int16_t>(sext(bitsOf(ch, 0, 11), 11));
        const uint32_t kl = tevColor_[1][slot][0], kh = tevColor_[1][slot][1];
        t.konst[slot][0] = static_cast<uint8_t>(bitsOf(kl, 0, 8));
        t.konst[slot][3] = static_cast<uint8_t>(bitsOf(kl, 12, 8));
        t.konst[slot][1] = static_cast<uint8_t>(bitsOf(kh, 12, 8));
        t.konst[slot][2] = static_cast<uint8_t>(bitsOf(kh, 0, 8));
        // Swap table i: KSEL(2i) = r, g; KSEL(2i+1) = b, a (libogc GX_SetTevSwapModeTable)
        t.swap[slot][0] = static_cast<uint8_t>(bitsOf(bp_[0xF6 + 2 * slot], 0, 2));
        t.swap[slot][1] = static_cast<uint8_t>(bitsOf(bp_[0xF6 + 2 * slot], 2, 2));
        t.swap[slot][2] = static_cast<uint8_t>(bitsOf(bp_[0xF7 + 2 * slot], 0, 2));
        t.swap[slot][3] = static_cast<uint8_t>(bitsOf(bp_[0xF7 + 2 * slot], 2, 2));
    }
    const uint32_t iref = bp_[0x27];
    for (uint32_t i = 0; i < 4; ++i) {
        t.indirectStage[i].texMap = static_cast<uint8_t>(bitsOf(iref, 6 * i, 3));
        t.indirectStage[i].texCoord = static_cast<uint8_t>(bitsOf(iref, 6 * i + 3, 3));
        const uint32_t ss = bp_[0x25 + i / 2];
        t.indirectStage[i].scaleS = static_cast<uint8_t>(bitsOf(ss, (i & 1) * 8, 4));
        t.indirectStage[i].scaleT = static_cast<uint8_t>(bitsOf(ss, (i & 1) * 8 + 4, 4));
    }
    for (uint32_t m = 0; m < 3; ++m) {      // IND_MTXA/B/C of matrix m: elements /1024, exponent = scale bits - 17 (libogc GX_SetIndTexMatrix)
        const uint32_t a = bp_[0x06 + 3 * m], b = bp_[0x07 + 3 * m], c = bp_[0x08 + 3 * m];
        const int exp = static_cast<int>(bitsOf(a, 22, 2) | bitsOf(b, 22, 2) << 2 | bitsOf(c, 22, 2) << 4) - 17;
        const uint32_t regs[3] = {a, b, c};
        for (int col = 0; col < 3; ++col) {
            t.indirectMatrix[m][0][col] = std::ldexp(static_cast<float>(sext(bitsOf(regs[col], 0, 11), 11)) / 1024.0f, exp);
            t.indirectMatrix[m][1][col] = std::ldexp(static_cast<float>(sext(bitsOf(regs[col], 11, 11), 11)) / 1024.0f, exp);
        }
    }
}

void Core::bindMap(uint32_t map, TexBinding& out) {
    const uint32_t base = map < 4 ? 0x80 + map : 0xA0 + (map - 4);
    const uint32_t regs[8] = {bp_[base], bp_[base + 4], bp_[base + 8], bp_[base + 12], bp_[base + 16], bp_[base + 20], bp_[base + 24], 0};
    MapMemo& m = memo_[map];
    if (trustEpoch_ && m.valid && std::memcmp(m.regs, regs, sizeof regs) == 0 && m.epoch == texGeneration_ && m.tlutEpoch == tlutEpoch_) {
        out = m.binding;
        return;
    }
    TexBinding b{};
    b.valid = true;
    const uint32_t mode0 = regs[0], mode1 = regs[1], img0 = regs[2], img3 = regs[5], tlut = regs[6];
    b.width = static_cast<uint16_t>(bitsOf(img0, 0, 10) + 1);
    b.height = static_cast<uint16_t>(bitsOf(img0, 10, 10) + 1);
    const uint32_t format = bitsOf(img0, 20, 4);
    Sampler& s = b.sampler;
    s.wrapS = static_cast<WrapMode>(bitsOf(mode0, 0, 2));
    s.wrapT = static_cast<WrapMode>(bitsOf(mode0, 2, 2));
    if (static_cast<int>(s.wrapS) > 2 || static_cast<int>(s.wrapT) > 2) fail("texture wrap mode 3 is undefined", base);
    s.magLinear = bitsOf(mode0, 4, 1) != 0;
    s.minFilter = minFilterFromHw(bitsOf(mode0, 5, 3));
    s.edgeLod = bitsOf(mode0, 8, 1) == 0;             // libogc stores !edgelod in bit 8 (the "diagonal LOD" field)
    s.lodBias = static_cast<float>(sext(bitsOf(mode0, 9, 8), 8)) / 32.0f;
    s.maxAnisotropy = static_cast<uint8_t>(1u << bitsOf(mode0, 19, 2));
    s.biasClamp = bitsOf(mode0, 21, 1) != 0;
    s.minLod = static_cast<float>(bitsOf(mode1, 0, 8)) / 16.0f;
    s.maxLod = static_cast<float>(bitsOf(mode1, 8, 8)) / 16.0f;

    const uint32_t addr = bitsOf(img3, 0, 24) << 5;
    uint32_t levels = 1;
    if (usesMips(s.minFilter)) {
        uint32_t possible = 1;
        for (uint32_t d = std::max<uint32_t>(b.width, b.height); d > 1; d >>= 1) ++possible;
        levels = std::min<uint32_t>({possible, static_cast<uint32_t>(std::ceil(s.maxLod)) + 1, 11u});
    }
    // Aliasing of an earlier EFB copy (convention 8): same start address, destination bytes unchanged since the copy.
    bool aliased = false;
    for (CopyRecord& rec : copies_) {
        if (rec.addr != addr) continue;
        const uint8_t* now = mem_.read(rec.addr, rec.bytes);
        if (now && gxHash64(now, rec.bytes, 0x4350ull) == rec.memHash) {
            b.efbCopyId = rec.id;
            aliased = true;
        } else if (!rec.warned) {
            rec.warned = true;
            char msg[160];
            std::snprintf(msg, sizeof msg, "gx: texture bound at EFB-copy destination 0x%08X after the memory changed: reading RAM, not the copy (copy %u)", addr, rec.id);
            host_.log(msg);
        }
        break;
    }
    if (!aliased) {
        TexRequest req{addr, b.width, b.height, format, levels, bitsOf(tlut, 0, 10), bitsOf(tlut, 10, 2)};
        b.image = loadTexture(req);
    }
    m.valid = true;
    std::memcpy(m.regs, regs, sizeof regs);
    m.epoch = texGeneration_;
    m.tlutEpoch = tlutEpoch_;
    m.binding = b;
    out = b;
}

void Core::rebuildPipeline() {
    ++stats_.pipelineRebuilds;
    auto sp = std::make_shared<PipelineState>();
    PipelineState& s = *sp;
    const uint32_t pc = bp_[0x43];
    if (bitsOf(pc, 0, 3) > 3) fail("EFB pixel format Y8/U8/V8/YUV420 is not supported", 0x43);
    if (bitsOf(bp_[0xF5], 2, 2) != 0) fail("z-texture (TEV_ZENV) is not supported", 0xF5);
    s.efbFormat = static_cast<PixelFormat>(bitsOf(pc, 0, 3));
    s.numColorChannels = static_cast<uint8_t>(std::min<uint32_t>(xf_.reg[0x09], 2));
    s.numTexCoords = static_cast<uint8_t>(std::min<uint32_t>(xf_.reg[0x3F], 8));

    Viewport& vp = s.viewport;
    for (int i = 0; i < 3; ++i) { vp.scale[i] = fl(xf_.reg[0x1A + i]); vp.offset[i] = fl(xf_.reg[0x1D + i]); }
    vp.width = 2.0f * vp.scale[0];
    vp.height = -2.0f * vp.scale[1];
    vp.x = vp.offset[0] - 342.0f - vp.scale[0];
    vp.y = vp.offset[1] - 342.0f + vp.scale[1];
    vp.farZ = vp.offset[2] / 16777215.0f;
    vp.nearZ = vp.farZ - vp.scale[2] / 16777215.0f;

    const int xo = static_cast<int>(bitsOf(bp_[0x59], 0, 10)) * 2, yo = static_cast<int>(bitsOf(bp_[0x59], 10, 10)) * 2;
    s.scissor.x0 = std::max(0, static_cast<int>(bitsOf(bp_[0x20], 12, 12)) - xo);
    s.scissor.y0 = std::max(0, static_cast<int>(bitsOf(bp_[0x20], 0, 12)) - yo);
    s.scissor.x1 = std::min(639, static_cast<int>(bitsOf(bp_[0x21], 12, 12)) - xo);
    s.scissor.y1 = std::min(527, static_cast<int>(bitsOf(bp_[0x21], 0, 12)) - yo);

    const uint32_t zm = bp_[0x40];
    s.depth = {bitsOf(zm, 0, 1) != 0, static_cast<CompareFunc>(bitsOf(zm, 1, 3)), bitsOf(zm, 4, 1) != 0, bitsOf(pc, 6, 1) != 0, static_cast<ZFormat>(bitsOf(pc, 3, 2))};

    const uint32_t cm = bp_[0x41];
    BlendState& bl = s.blend;
    bl.mode = bitsOf(cm, 1, 1) ? BlendMode::Logic : bitsOf(cm, 0, 1) ? (bitsOf(cm, 11, 1) ? BlendMode::Subtract : BlendMode::Blend) : BlendMode::None;
    bl.src = static_cast<BlendFactor>(bitsOf(cm, 8, 3));
    bl.dst = static_cast<BlendFactor>(bitsOf(cm, 5, 3));
    bl.logicOp = static_cast<LogicOp>(bitsOf(cm, 12, 4));
    bl.colorUpdate = bitsOf(cm, 3, 1) != 0;
    bl.alphaUpdate = bitsOf(cm, 4, 1) != 0;
    bl.dither = bitsOf(cm, 2, 1) != 0;
    bl.dstAlphaEnable = bitsOf(bp_[0x42], 8, 1) != 0;
    bl.dstAlpha = static_cast<uint8_t>(bitsOf(bp_[0x42], 0, 8));

    const uint32_t af = bp_[0xF3];
    s.alphaCompare = {static_cast<CompareFunc>(bitsOf(af, 16, 3)), static_cast<CompareFunc>(bitsOf(af, 19, 3)), static_cast<uint8_t>(bitsOf(af, 0, 8)), static_cast<uint8_t>(bitsOf(af, 8, 8)),
                      static_cast<AlphaLogic>(bitsOf(af, 22, 2))};

    // Fog: inverse of libogc GX_SetFog (A as the top 20 bits of a float, C likewise, B = mantissa/2^23 * 2^(shift-1)).
    FogState& fg = s.fog;
    const uint32_t f3 = bp_[0xF1];
    fg.type = static_cast<FogType>(bitsOf(f3, 21, 3));
    fg.orthographic = bitsOf(f3, 20, 1) != 0;
    const uint32_t shift = bitsOf(bp_[0xF0], 0, 5);
    fg.a = std::ldexp(fl(bitsOf(bp_[0xEE], 0, 20) << 12), static_cast<int>(shift));
    fg.b = std::ldexp(static_cast<float>(bitsOf(bp_[0xEF], 0, 24)) / 8388608.0f, static_cast<int>(shift) - 1);
    fg.c = fl(bitsOf(f3, 0, 20) << 12);
    fg.color[0] = static_cast<uint8_t>(bitsOf(bp_[0xF2], 16, 8));
    fg.color[1] = static_cast<uint8_t>(bitsOf(bp_[0xF2], 8, 8));
    fg.color[2] = static_cast<uint8_t>(bitsOf(bp_[0xF2], 0, 8));
    fg.rangeAdjustEnable = bitsOf(bp_[0xE8], 10, 1) != 0;
    fg.rangeAdjustCenter = static_cast<uint16_t>(bitsOf(bp_[0xE8], 0, 10) - 342u);
    for (int i = 0; i < 5; ++i) {
        fg.rangeAdjustTable[2 * i] = static_cast<uint16_t>(bitsOf(bp_[0xE9 + i], 0, 12));
        fg.rangeAdjustTable[2 * i + 1] = static_cast<uint16_t>(bitsOf(bp_[0xE9 + i], 12, 12));
    }

    buildTev(s.tev);
    // Texture maps referenced by the TEV stages and the indirect stages.
    bool used[8] = {};
    for (uint32_t i = 0; i < s.tev.numStages; ++i)
        if (s.tev.stages[i].texMap != kNoMap) used[s.tev.stages[i].texMap] = true;
    for (uint32_t i = 0; i < s.tev.numIndirect; ++i) used[s.tev.indirectStage[i].texMap] = true;
    for (uint32_t m = 0; m < 8; ++m)
        if (used[m]) bindMap(m, s.maps[m]);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t ss = bp_[0x30 + 2 * i], ts = bp_[0x31 + 2 * i];
        s.texScale[i] = {static_cast<uint16_t>(bitsOf(ss, 0, 16) + 1), static_cast<uint16_t>(bitsOf(ts, 0, 16) + 1), bitsOf(ss, 16, 1) != 0, bitsOf(ts, 16, 1) != 0,
                         bitsOf(ss, 17, 1) != 0, bitsOf(ts, 17, 1) != 0, bitsOf(ss, 18, 1) != 0, bitsOf(ss, 19, 1) != 0};
    }
    s.lineWidth = static_cast<uint8_t>(bitsOf(bp_[0x22], 0, 8));
    s.pointSize = static_cast<uint8_t>(bitsOf(bp_[0x22], 8, 8));
    s.lineTexOffset = static_cast<uint8_t>(bitsOf(bp_[0x22], 16, 3));
    s.pointTexOffset = static_cast<uint8_t>(bitsOf(bp_[0x22], 19, 3));
    s.cullMode = static_cast<uint8_t>(bitsOf(bp_[0x00], 14, 2));

    pipe_ = sp;
    ++stateId_;
    dirty_ = false;
}

void Core::efbCopy() {
    const uint32_t v = bp_[0x52];
    EfbCopyCommand c{};
    const bool toXfb = bitsOf(v, 14, 1) != 0;
    c.id = ++copySeq_;
    c.target = toXfb ? CopyTarget::Xfb : CopyTarget::Texture;
    c.srcX = bitsOf(bp_[0x49], 0, 10);
    c.srcY = bitsOf(bp_[0x49], 10, 10);
    c.srcWidth = bitsOf(bp_[0x4A], 0, 10) + 1;
    c.srcHeight = bitsOf(bp_[0x4A], 10, 10) + 1;
    c.destAddr = bitsOf(bp_[0x4B], 0, 24) << 5;
    const uint32_t pf = bitsOf(bp_[0x43], 0, 3);
    if (pf > 3) fail("EFB copy from a Y8/U8/V8/YUV420 EFB is not supported", 0x52);
    c.efbFormat = static_cast<PixelFormat>(pf);
    c.clampTop = bitsOf(v, 0, 1) != 0;
    c.clampBottom = bitsOf(v, 1, 1) != 0;
    c.sampleBits = bp_[0x01];
    for (int i = 0; i < 4; ++i) c.vfilter[i] = static_cast<uint8_t>(bitsOf(bp_[0x53], 6 * i, 6));
    for (int i = 0; i < 3; ++i) c.vfilter[4 + i] = static_cast<uint8_t>(bitsOf(bp_[0x54], 6 * i, 6));
    // Clear: libogc forces z update on and blending off around a clearing copy; color and alpha follow their update masks.
    c.clear.color = bitsOf(v, 11, 1) && bitsOf(bp_[0x41], 3, 1);
    c.clear.alpha = bitsOf(v, 11, 1) && bitsOf(bp_[0x41], 4, 1);
    c.clear.z = bitsOf(v, 11, 1) && bitsOf(bp_[0x40], 4, 1);
    c.clear.rgba[3] = static_cast<uint8_t>(bitsOf(bp_[0x4F], 8, 8));
    c.clear.rgba[0] = static_cast<uint8_t>(bitsOf(bp_[0x4F], 0, 8));
    c.clear.rgba[1] = static_cast<uint8_t>(bitsOf(bp_[0x50], 8, 8));
    c.clear.rgba[2] = static_cast<uint8_t>(bitsOf(bp_[0x50], 0, 8));
    c.clear.z24 = bitsOf(bp_[0x51], 0, 24);
    if (toXfb) {
        const uint32_t g = bitsOf(v, 7, 2);
        if (g == 3) fail("display copy gamma 3 is undefined", 0x52);
        c.gamma = static_cast<uint8_t>(g);
        const uint32_t ys = bitsOf(bp_[0x4E], 0, 9);
        c.yScale = (bitsOf(v, 10, 1) && ys) ? 256.0f / static_cast<float>(ys) : 1.0f;
        c.destWidth = c.srcWidth;
        c.destHeight = static_cast<uint32_t>(std::lround(c.srcHeight * c.yScale));
        c.frameToField = bitsOf(v, 12, 2) != 0;
    } else {
        // Destination format (libogc GX_SetTexCopyDst): bits 3 and 4-6 hold the low nibble, bit 15 the intensity class; a
        // z-texture copy is signalled by the EFB format being forced to Z24 for the copy.
        const uint32_t lfmt = bitsOf(v, 4, 3) | bitsOf(v, 3, 1) << 3;
        const bool cls = bitsOf(v, 15, 1) != 0, z = pf == 3;
        uint32_t fmt = 0xFFFF;
        if (z) {
            switch (lfmt) { case 0: fmt = 0x30; break; case 1: fmt = 0x11; break; case 6: fmt = 0x16; break; case 9: fmt = 0x39; break; case 10: fmt = 0x3A; break; case 11: fmt = 0x13; break; case 12: fmt = 0x3C; break; default: break; }
        } else if (cls) {
            switch (lfmt) { case 0: fmt = 0x0; break; case 1: fmt = 0x1; break; case 2: fmt = 0x2; break; case 3: fmt = 0x3; break; case 6: fmt = 0x26; break; default: break; }
        } else {
            switch (lfmt) { case 0: fmt = 0x20; break; case 2: fmt = 0x22; break; case 3: fmt = 0x23; break; case 4: fmt = 0x4; break; case 5: fmt = 0x5; break; case 6: fmt = 0x6; break;
                            case 7: fmt = 0x27; break; case 8: fmt = 0x28; break; case 9: fmt = 0x29; break; case 10: fmt = 0x2A; break; case 11: fmt = 0x2B; break; case 12: fmt = 0x2C; break; default: break; }
        }
        if (fmt == 0xFFFF) fail("EFB copy to texture with an undefined destination format", 0x52);
        c.texFormat = static_cast<uint8_t>(fmt);
        c.intensity = cls;
        c.mipmap = bitsOf(v, 9, 1) != 0;
        c.halfScale = c.mipmap;
        c.destWidth = c.halfScale ? std::max(1u, c.srcWidth / 2) : c.srcWidth;
        c.destHeight = c.halfScale ? std::max(1u, c.srcHeight / 2) : c.srcHeight;
        // Remember the destination so later texture binds can alias it (it does not write guest memory).
        const uint32_t bytes = copyBytes(fmt, c.destWidth, c.destHeight);
        const uint8_t* mem = mem_.read(c.destAddr, bytes);
        if (!mem) fail("EFB copy destination outside memory", 0x4B);
        copies_.erase(std::remove_if(copies_.begin(), copies_.end(), [&](const CopyRecord& r) { return r.addr == c.destAddr; }), copies_.end());
        if (copies_.size() >= 64) copies_.erase(copies_.begin());
        copies_.push_back({c.id, c.destAddr, bytes, fmt, c.destWidth, c.destHeight, gxHash64(mem, bytes, 0x4350ull), false});
        dirty_ = true;
        ++texGeneration_;
    }
    ++stats_.copies;
    backend_.efbCopy(c);
    host_.efbCopyIssued(c.target);
}

}  // namespace awl::gx
