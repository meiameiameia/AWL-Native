// awl-gx-survey: reports which GX commands, registers and formats the game uses, from a command-stream dump captured by
// awl-host (AWL_GX_CAPTURE, see src/gx/gx_dump.h). Findings: docs/research/gx-core.md.
//   awl-gx-survey <dump> [--out report.txt] [--frames]
// Register numbers and field layouts: YAGCD chapter 5 (CP/XF/BP registers), libogc gx.h/gx.c (the SDK's own register
// writes). The tool decodes only; it draws nothing and reads no guest memory (indexed data is counted by index, not read).
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gx/gx_dump.h"

namespace {

using namespace awl::gx::dump;

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
float asFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// ---- Histograms --------------------------------------------------------------------------------------------------
struct Hist {
    std::map<std::string, uint64_t> m;
    void add(const std::string& k, uint64_t n = 1) { m[k] += n; }
    void print(FILE* out, const char* title, size_t limit = 0) const {
        std::fprintf(out, "\n%s (%zu distinct)\n", title, m.size());
        std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        size_t n = 0;
        for (const auto& e : v) {
            if (limit && n++ >= limit) { std::fprintf(out, "  ... %zu more\n", v.size() - limit); break; }
            std::fprintf(out, "  %9llu  %s\n", static_cast<unsigned long long>(e.second), e.first.c_str());
        }
    }
};

// ---- Names -------------------------------------------------------------------------------------------------------
const char* posFmt(uint32_t f) { static const char* n[] = {"u8", "s8", "u16", "s16", "f32", "?5", "?6", "?7"}; return n[f & 7]; }
const char* colFmt(uint32_t f) { static const char* n[] = {"rgb565", "rgb888", "rgbx8888", "rgba4444", "rgba6666", "rgba8888", "?6", "?7"}; return n[f & 7]; }
const char* attrMode(uint32_t m) { static const char* n[] = {"none", "direct", "idx8", "idx16"}; return n[m & 3]; }
const char* cmpName(uint32_t c) { static const char* n[] = {"never", "less", "equal", "lequal", "greater", "nequal", "gequal", "always"}; return n[c & 7]; }
const char* texFmt(uint32_t f) {
    switch (f) {
        case 0: return "I4"; case 1: return "I8"; case 2: return "IA4"; case 3: return "IA8"; case 4: return "RGB565"; case 5: return "RGB5A3";
        case 6: return "RGBA8"; case 8: return "C4"; case 9: return "C8"; case 10: return "C14X2"; case 14: return "CMPR";
        default: return "?";
    }
}
const char* tlutFmt(uint32_t f) { static const char* n[] = {"IA8", "RGB565", "RGB5A3", "?3"}; return n[f & 3]; }
const char* pixFmt(uint32_t f) { static const char* n[] = {"RGB8_Z24", "RGBA6_Z24", "RGB565_Z16", "Z24", "Y8", "U8", "V8", "YUV420"}; return n[f & 7]; }
const char* wrapName(uint32_t w) { static const char* n[] = {"clamp", "repeat", "mirror", "?3"}; return n[w & 3]; }
const char* filtName(uint32_t f) { static const char* n[] = {"near", "linear", "near_mip_near", "lin_mip_near", "near_mip_lin", "lin_mip_lin", "?6", "?7"}; return n[f & 7]; }
const char* blendFactor(uint32_t f) { static const char* n[] = {"0", "1", "srcclr", "1-srcclr", "srcalpha", "1-srcalpha", "dstalpha", "1-dstalpha"}; return n[f & 7]; }
const char* logicOp(uint32_t o) {
    static const char* n[] = {"clear", "and", "revand", "copy", "invand", "noop", "xor", "or", "nor", "equiv", "inv", "revor", "invcopy", "invor", "nand", "set"};
    return n[o & 15];
}
const char* tevColIn(uint32_t i) {
    static const char* n[] = {"CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2", "TEXC", "TEXA", "RASC", "RASA", "ONE", "HALF", "KONST", "ZERO"};
    return n[i & 15];
}
const char* tevAlphaIn(uint32_t i) { static const char* n[] = {"APREV", "A0", "A1", "A2", "TEXA", "RASA", "KONST", "ZERO"}; return n[i & 7]; }
const char* tevDest(uint32_t d) { static const char* n[] = {"prev", "reg0", "reg1", "reg2"}; return n[d & 3]; }
const char* tevScale(uint32_t s) { static const char* n[] = {"x1", "x2", "x4", "/2"}; return n[s & 3]; }
const char* tevBias(uint32_t b) { static const char* n[] = {"0", "+0.5", "-0.5", "compare"}; return n[b & 3]; }

std::string bpName(uint32_t r) {
    if (r == 0x00) return "GEN_MODE";
    if (r >= 0x01 && r <= 0x04) return fmt("COPY_FILTER_%02X", r);
    if (r >= 0x06 && r <= 0x0E) return fmt("IND_MTX%c%u", "ABC"[(r - 6) % 3], (r - 6) / 3);
    if (r == 0x0F) return "IND_IMASK";
    if (r >= 0x10 && r <= 0x1F) return fmt("IND_CMD%X", r - 0x10);
    if (r == 0x20) return "SU_SCIS0";
    if (r == 0x21) return "SU_SCIS1";
    if (r == 0x22) return "SU_LPSIZE";
    if (r == 0x23 || r == 0x24) return fmt("PERF_%02X", r);
    if (r == 0x25 || r == 0x26) return fmt("RAS1_SS%u", r - 0x25);
    if (r == 0x27) return "RAS1_IREF";
    if (r >= 0x28 && r <= 0x2F) return fmt("RAS1_TREF%u", r - 0x28);
    if (r >= 0x30 && r <= 0x3F) return fmt("SU_%cSIZE%u", (r & 1) ? 'T' : 'S', (r - 0x30) / 2);
    switch (r) {
        case 0x40: return "PE_ZMODE"; case 0x41: return "PE_CMODE0"; case 0x42: return "PE_CMODE1"; case 0x43: return "PE_CONTROL";
        case 0x44: return "PE_FIELD_MASK"; case 0x45: return "PE_DONE"; case 0x46: return "PE_REFRESH"; case 0x47: return "PE_TOKEN";
        case 0x48: return "PE_TOKEN_INT"; case 0x49: return "EFB_COPY_SRC_XY"; case 0x4A: return "EFB_COPY_SRC_SIZE";
        case 0x4B: return "EFB_COPY_DST_ADDR"; case 0x4D: return "EFB_COPY_DST_STRIDE"; case 0x4E: return "EFB_COPY_YSCALE";
        case 0x4F: return "EFB_CLEAR_AR"; case 0x50: return "EFB_CLEAR_GB"; case 0x51: return "EFB_CLEAR_Z"; case 0x52: return "EFB_COPY_EXEC";
        case 0x53: case 0x54: return fmt("COPY_FILTER_%02X", r); case 0x55: case 0x56: return fmt("BBOX_%02X", r);
        case 0x58: return "REVBITS"; case 0x59: return "SCISSOR_OFFSET";
        case 0x64: return "TX_LOADTLUT0"; case 0x65: return "TX_LOADTLUT1"; case 0x66: return "TX_INVALIDATE";
        case 0x67: return "PERF1"; case 0x68: return "FIELD_MODE"; case 0x69: return "BUS_CLOCK";
        case 0xE8: return "FOG_RANGE"; case 0xF2: return "TEV_FOG_COLOR"; case 0xF3: return "TEV_ALPHAFUNC";
        case 0xF4: return "TEV_ZENV0"; case 0xF5: return "TEV_ZENV1"; case 0xFE: return "BP_MASK";
        default: break;
    }
    if (r >= 0x60 && r <= 0x63) return fmt("TX_PRELOAD_%02X", r);
    if ((r >= 0x80 && r <= 0xBB) && (r & 0x1F) < 0x1C) {
        const uint32_t base = r >= 0xA0 ? 0xA0 : 0x80, k = (r - base) / 4, map = (r - base) % 4 + (r >= 0xA0 ? 4 : 0);
        static const char* n[] = {"TX_MODE0", "TX_MODE1", "TX_IMAGE0", "TX_IMAGE1", "TX_IMAGE2", "TX_IMAGE3", "TX_TLUT"};
        if (k < 7) return fmt("%s_%u", n[k], map);
    }
    if (r >= 0xC0 && r <= 0xDF) return fmt("TEV_%s_ENV%u", (r & 1) ? "ALPHA" : "COLOR", (r - 0xC0) / 2);
    if (r >= 0xE0 && r <= 0xE7) return fmt("TEV_REG%u%c", (r - 0xE0) / 2, (r & 1) ? 'H' : 'L');
    if (r >= 0xE9 && r <= 0xED) return fmt("FOG_RANGE_ADJ_%02X", r);
    if (r >= 0xEE && r <= 0xF1) return fmt("TEV_FOG_PARAM%u", r - 0xEE);
    if (r >= 0xF6 && r <= 0xFD) return fmt("TEV_KSEL%u", r - 0xF6);
    return fmt("BP_%02X", r);
}

// ---- Decoders -----------------------------------------------------------------------------------------------------
std::string decodeColorEnv(uint32_t v) {
    return fmt("color d=%s c=%s b=%s a=%s %s bias=%s scale=%s %s dest=%s", tevColIn(bits(v, 0, 4)), tevColIn(bits(v, 4, 4)), tevColIn(bits(v, 8, 4)),
               tevColIn(bits(v, 12, 4)), bits(v, 18, 1) ? "sub" : "add", tevBias(bits(v, 16, 2)), tevScale(bits(v, 20, 2)),
               bits(v, 19, 1) ? "clamp" : "noclamp", tevDest(bits(v, 22, 2)));
}
std::string decodeAlphaEnv(uint32_t v) {
    return fmt("alpha d=%s c=%s b=%s a=%s %s bias=%s scale=%s %s dest=%s rswap=%u tswap=%u", tevAlphaIn(bits(v, 4, 3)), tevAlphaIn(bits(v, 7, 3)),
               tevAlphaIn(bits(v, 10, 3)), tevAlphaIn(bits(v, 13, 3)), bits(v, 18, 1) ? "sub" : "add", tevBias(bits(v, 16, 2)), tevScale(bits(v, 20, 2)),
               bits(v, 19, 1) ? "clamp" : "noclamp", tevDest(bits(v, 22, 2)), bits(v, 0, 2), bits(v, 2, 2));
}
std::string decodeLightChan(uint32_t v) {
    const uint32_t mask = bits(v, 2, 4) | bits(v, 11, 4) << 4;
    const char* attn = !bits(v, 9, 1) ? "none" : bits(v, 10, 1) ? "spot" : "spec";
    return fmt("mat=%s light=%s mask=%02X amb=%s diff=%s attn=%s", bits(v, 0, 1) ? "vtx" : "reg", bits(v, 1, 1) ? "on" : "off", mask,
               bits(v, 6, 1) ? "vtx" : "reg", bits(v, 7, 2) == 0 ? "none" : bits(v, 7, 2) == 1 ? "sign" : "clamp", attn);
}
std::string decodeTexGen(uint32_t v) {
    static const char* src[] = {"pos", "nrm", "color", "binrmT", "binrmB", "tex0", "tex1", "tex2", "tex3", "tex4", "tex5", "tex6", "tex7"};
    static const char* type[] = {"regular", "emboss", "srtg-c0", "srtg-c1"};
    const uint32_t s = bits(v, 7, 5), t = bits(v, 4, 3);
    return fmt("proj=%s form=%s type=%s src=%s%s", bits(v, 1, 1) ? "STQ" : "ST", bits(v, 2, 2) == 0 ? "AB11" : bits(v, 2, 2) == 1 ? "ABC1" : "?",
               t < 4 ? type[t] : "?", s < 13 ? src[s] : "?", t == 1 ? fmt(" embossSrc=%u embossLight=%u", bits(v, 12, 3), bits(v, 15, 3)).c_str() : "");
}

// ---- Vertex layout -----------------------------------------------------------------------------------------------
struct Attr {
    uint32_t mode = 0, cnt = 0, fmt = 0, shift = 0;
    uint32_t bytes = 0;      // bytes per vertex
    uint32_t nidx = 1;       // indices per vertex when indexed (3 for NBT3)
};
struct Layout {
    bool pnmtx = false, texmtx[8] = {};
    Attr pos, nrm, col[2], tex[8];
    uint32_t size = 0;
    uint32_t vat = 0;
    std::string desc;
};

uint32_t compBytes(uint32_t f) { return f <= 1 ? 1 : f <= 3 ? 2 : 4; }
uint32_t colBytes(uint32_t f) { return f == 0 || f == 3 ? 2 : f == 1 || f == 4 ? 3 : 4; }
uint32_t idxBytes(uint32_t mode) { return mode == 2 ? 1 : 2; }

struct CpState {
    uint32_t vcdLo = 0, vcdHi = 0, vat[3][8] = {}, arrBase[16] = {}, arrStride[16] = {}, matA = 0, matB = 0;
};

Layout buildLayout(const CpState& cp, uint32_t vat) {
    Layout L;
    L.vat = vat;
    const uint32_t a = cp.vat[0][vat], b = cp.vat[1][vat], c = cp.vat[2][vat];
    L.pnmtx = bits(cp.vcdLo, 0, 1);
    if (L.pnmtx) L.size += 1;
    for (uint32_t t = 0; t < 8; ++t) {
        L.texmtx[t] = bits(cp.vcdLo, 1 + t, 1);
        if (L.texmtx[t]) L.size += 1;
    }
    auto finish = [&](Attr& at, uint32_t directBytes) {
        if (at.mode == 1) at.bytes = directBytes;
        else if (at.mode >= 2) at.bytes = idxBytes(at.mode) * at.nidx;
        else at.bytes = 0;
        L.size += at.bytes;
    };
    L.pos = {bits(cp.vcdLo, 9, 2), bits(a, 0, 1), bits(a, 1, 3), bits(a, 4, 5), 0, 1};
    finish(L.pos, (L.pos.cnt ? 3 : 2) * compBytes(L.pos.fmt));
    L.nrm = {bits(cp.vcdLo, 11, 2), bits(a, 9, 1), bits(a, 10, 3), 0, 0, 1};
    if (L.nrm.mode >= 2 && L.nrm.cnt && bits(a, 31, 1)) L.nrm.nidx = 3;
    finish(L.nrm, (L.nrm.cnt ? 9 : 3) * (L.nrm.fmt == 1 ? 1 : L.nrm.fmt == 3 ? 2 : 4));
    L.col[0] = {bits(cp.vcdLo, 13, 2), bits(a, 13, 1), bits(a, 14, 3), 0, 0, 1};
    finish(L.col[0], colBytes(L.col[0].fmt));
    L.col[1] = {bits(cp.vcdLo, 15, 2), bits(a, 17, 1), bits(a, 18, 3), 0, 0, 1};
    finish(L.col[1], colBytes(L.col[1].fmt));
    // Texture coordinate fields: VAT A holds tex0; B holds tex1-3 and the count/format of tex4; C holds tex4's shift and tex5-7.
    // (YAGCD VAT A/B/C layout; the shift of tex4 is split over bit 31 of B and bits 0-3 of C.)
    struct TexField { uint32_t cntLo, fmtLo, shiftLo; const uint32_t* w; };
    const TexField fields[8] = {{21, 22, 25, &a}, {0, 1, 4, &b}, {9, 10, 13, &b}, {18, 19, 22, &b}, {27, 28, 0, &b}, {4, 5, 8, &c}, {13, 14, 17, &c}, {22, 23, 26, &c}};
    for (uint32_t t = 0; t < 8; ++t) {
        const TexField& f = fields[t];
        L.tex[t] = {bits(cp.vcdHi, 2 * t, 2), bits(*f.w, f.cntLo, 1), bits(*f.w, f.fmtLo, 3), t == 4 ? (bits(b, 31, 1) | bits(c, 0, 4) << 1) : bits(*f.w, f.shiftLo, 5), 0, 1};
        finish(L.tex[t], (L.tex[t].cnt ? 2 : 1) * compBytes(L.tex[t].fmt));
    }
    // Description.
    std::string d = fmt("vat%u:", vat);
    if (L.pnmtx) d += " pnmtx";
    for (uint32_t t = 0; t < 8; ++t)
        if (L.texmtx[t]) d += fmt(" tmtx%u", t);
    d += fmt(" pos[%s %s %s", attrMode(L.pos.mode), L.pos.cnt ? "xyz" : "xy", posFmt(L.pos.fmt));
    if (L.pos.mode && L.pos.fmt < 4) d += fmt(" >>%u", L.pos.shift);
    d += "]";
    if (L.nrm.mode) d += fmt(" nrm[%s %s %s%s]", attrMode(L.nrm.mode), L.nrm.cnt ? "nbt" : "n", posFmt(L.nrm.fmt), L.nrm.nidx == 3 ? " idx3" : "");
    for (int i = 0; i < 2; ++i)
        if (L.col[i].mode) d += fmt(" c%d[%s %s %s]", i, attrMode(L.col[i].mode), L.col[i].cnt ? "rgba" : "rgb", colFmt(L.col[i].fmt));
    for (uint32_t t = 0; t < 8; ++t)
        if (L.tex[t].mode) {
            d += fmt(" t%u[%s %s %s", t, attrMode(L.tex[t].mode), L.tex[t].cnt ? "st" : "s", posFmt(L.tex[t].fmt));
            if (L.tex[t].fmt < 4) d += fmt(" >>%u", L.tex[t].shift);
            d += "]";
        }
    L.desc = d;
    return L;
}

// ---- The survey --------------------------------------------------------------------------------------------------
struct Survey {
    // Stream state.
    CpState cp;
    uint32_t bp[256] = {};
    uint32_t bpMask = 0xFFFFFF;
    uint32_t xfReg[0x60] = {};
    bool xfRegSet[0x60] = {};

    // Statistics.
    Hist cmds, cpRegs, cpRegValues, xfDirect, xfRegWrites, xfIndexed, bpRegs, prims, vertexFormats, arrays, mtxIdx;
    Hist genModes, lightChans, texGens, drawStates, tevStages, tevFunctions, indirect, alphaFunc, zmodes, blends, peControl, fog;
    Hist textures, texModes, texTLUT, tlutLoads, efbCopies, efbCopyKinds, scissors, viewports, projections, lineSizes, texScale, misc, unknownBp;
    Hist tevSignatures, efbAlias, frames, textureCount, swapTables, copyFilters, projAtDraw;
    std::map<uint32_t, uint64_t> pnmtxVerts, texmtxVerts[8];
    std::set<uint32_t> copyDsts;               // addr>>5 of EFB copies to textures
    std::set<std::string> usedTexKeys;
    uint64_t draws = 0, verts = 0, fifoBytes = 0, dlBytes = 0, calls = 0, frameCount = 0;
    uint32_t maxIdx[16] = {};
    std::set<uint32_t> arrBasesSeen[16];
    std::set<uint32_t> arrStridesSeen[16];

    // Per-frame accounting.
    struct Frame { uint32_t retrace = 0; uint64_t draws = 0, verts = 0, copies = 0, xfbCopies = 0, texCopies = 0, clears = 0, bytes = 0, tevDistinct = 0; };
    std::vector<Frame> frameList;
    std::set<std::string> frameTev;

    uint64_t offset = 0;                       // position in the (flattened) command stream, for messages
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> dlQueue;
    bool failed = false;
    std::string failMsg;

    void fail(const char* what, uint32_t op) {
        if (failed) return;
        failed = true;
        failMsg = fmt("stream offset %llu (frame %u): %s (opcode/register 0x%02X)", static_cast<unsigned long long>(offset), frameList.empty() ? 0 : frameList.back().retrace, what, op);
    }

    // ---- Commands ----
    void cpLoad(uint8_t reg, uint32_t v) {
        cpRegs.add(fmt("CP 0x%02X", reg));
        if (reg == 0x30) cp.matA = v;
        else if (reg == 0x40) cp.matB = v;
        else if (reg == 0x50) cp.vcdLo = v;
        else if (reg == 0x60) cp.vcdHi = v;
        else if (reg >= 0x70 && reg < 0x78) cp.vat[0][reg - 0x70] = v;
        else if (reg >= 0x80 && reg < 0x88) cp.vat[1][reg - 0x80] = v;
        else if (reg >= 0x90 && reg < 0x98) cp.vat[2][reg - 0x90] = v;
        else if (reg >= 0xA0 && reg < 0xB0) { cp.arrBase[reg - 0xA0] = v; arrBasesSeen[reg - 0xA0].insert(v & 0x03FFFFFF); }
        else if (reg >= 0xB0 && reg < 0xC0) { cp.arrStride[reg - 0xB0] = v; arrStridesSeen[reg - 0xB0].insert(v & 0xFF); }
        else if (reg == 0x20) misc.add("CP 0x20 (unknown register; value " + fmt("0x%X", v) + ")");
        else fail("CP register not decoded", reg);
        if (reg == 0x30 || reg == 0x40) cpRegValues.add(fmt("CP 0x%02X matrix-index default 0x%08X", reg, v));
    }

    void xfLoad(uint32_t addr, uint32_t count, const uint8_t* words) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t a = addr + i, v = be32(words + 4 * i);
            if (a >= 0x1000 && a < 0x1060) {
                xfReg[a - 0x1000] = v;
                xfRegSet[a - 0x1000] = true;
                xfRegWrites.add(fmt("XF 0x%04X", a));
            }
        }
        if (addr < 0x100) xfDirect.add(fmt("position/texture matrices: addr 0x%03X, %u words (matrix idx %u..)", addr, count, addr / 4));
        else if (addr >= 0x400 && addr < 0x460) xfDirect.add(fmt("normal matrices: addr 0x%03X, %u words", addr, count));
        else if (addr >= 0x500 && addr < 0x600) xfDirect.add(fmt("post-transform matrices: addr 0x%03X, %u words", addr, count));
        else if (addr >= 0x600 && addr < 0x680) xfDirect.add(fmt("lights: addr 0x%03X, %u words (light %u)", addr, count, (addr - 0x600) / 16));
        else if (addr >= 0x1000 && addr < 0x1060) {
            if (addr == 0x101A && count == 6) viewports.add(fmt("scale %g %g %g offset %g %g %g", asFloat(xfReg[0x1A]), asFloat(xfReg[0x1B]), asFloat(xfReg[0x1C]), asFloat(xfReg[0x1D]),
                                                                  asFloat(xfReg[0x1E]), asFloat(xfReg[0x1F])));
            if (addr == 0x1020 && count == 7)
                projections.add(fmt("%s p=%g %g %g %g %g %g", xfReg[0x26] ? "ortho" : "perspective", asFloat(xfReg[0x20]), asFloat(xfReg[0x21]), asFloat(xfReg[0x22]), asFloat(xfReg[0x23]),
                                    asFloat(xfReg[0x24]), asFloat(xfReg[0x25])));
            else if (addr == 0x1020 && count == 6) projections.add("projection params written without the ortho flag (6 words)");
        } else if (addr + count > 0x1060 || (addr >= 0x680 && addr < 0x1000) || (addr >= 0x460 && addr < 0x500) || (addr >= 0x100 && addr < 0x400))
            fail("XF load outside the modelled memory map", 0x10);
        else
            xfDirect.add(fmt("other: addr 0x%04X, %u words", addr, count));
    }

    void bpLoad(uint32_t word) {
        const uint32_t reg = word >> 24, data = word & 0xFFFFFF;
        if (reg == 0xFE) {
            bpMask = data;
            bpRegs.add("BP_MASK");
            misc.add(fmt("BP mask write 0x%06X", data));
            return;
        }
        const uint32_t mask = bpMask;
        bpMask = 0xFFFFFF;
        const uint32_t nv = (bp[reg] & ~mask & 0xFFFFFF) | (data & mask);
        bp[reg] = nv;
        bpRegs.add(bpName(reg));
        if (mask != 0xFFFFFF) misc.add(fmt("masked BP write %s mask 0x%06X", bpName(reg).c_str(), mask));
        switch (reg) {
            case 0x45: misc.add("PE_DONE (finish)"); break;
            case 0x47: misc.add("PE_TOKEN (no interrupt)"); break;
            case 0x48: misc.add("PE_TOKEN_INT (token with interrupt)"); break;
            case 0x52: efbCopy(nv); break;
            case 0x64: tlutLoads.add(fmt("LOADTLUT0 (main memory address register)")); break;
            case 0x65: tlutLoads.add(fmt("LOADTLUT1 count=%u x16 entries, tmem offset 0x%X", bits(nv, 10, 10), bits(nv, 0, 10))); break;
            case 0x66: misc.add("TX_INVALIDATE (texture cache)"); break;
            default: break;
        }
        const bool known = reg <= 0x0F || (reg >= 0x10 && reg <= 0x2F) || (reg >= 0x30 && reg <= 0x3F) || (reg >= 0x40 && reg <= 0x59) ||
                           (reg >= 0x60 && reg <= 0x69) || (reg >= 0x80 && reg <= 0xBB) || (reg >= 0xC0 && reg <= 0xFD);
        if (!known) unknownBp.add(fmt("%s data 0x%06X", bpName(reg).c_str(), data));
    }

    void efbCopy(uint32_t v) {
        const bool toXfb = bits(v, 14, 1), clear = bits(v, 11, 1);
        const uint32_t texFmt4 = bits(v, 4, 3) | (bits(v, 3, 1) << 3);
        Frame& f = frameList.back();
        ++f.copies;
        if (toXfb) ++f.xfbCopies; else ++f.texCopies;
        if (clear) ++f.clears;
        const uint32_t sx = bits(bp[0x49], 0, 10), sy = bits(bp[0x49], 10, 10), w = bits(bp[0x4A], 0, 10) + 1, h = bits(bp[0x4A], 10, 10) + 1;
        const uint32_t pf = bits(bp[0x43], 0, 3);
        std::string k = fmt("%s src=(%u,%u) %ux%u", toXfb ? "to-XFB" : "to-texture", sx, sy, w, h);
        if (toXfb) k += fmt(" gamma=%u frame2field=%u yscale=%u clamp=%u", bits(v, 7, 2), bits(v, 12, 2), bits(bp[0x4E], 0, 9), bits(v, 0, 2));
        else {
            k += fmt(" dstfmt=0x%X halfscale=%u mip=%u classbits=%u intensity/yuv=%u clamp=%u stride=%u", texFmt4, bits(v, 10, 1), bits(v, 9, 1), bits(v, 15, 2), bits(v, 2, 1), bits(v, 0, 2),
                     bits(bp[0x4D], 0, 10));
            copyDsts.insert(bp[0x4B] & 0xFFFFFF);
        }
        k += fmt(" clear=%u efb=%s zfmt=%u", clear, pixFmt(pf), bits(bp[0x43], 3, 3));
        if (clear) k += fmt(" clearAR=%06X GB=%04X Z=%06X zmode=%s/%s/update%u", bp[0x4F], bp[0x50], bp[0x51], bits(bp[0x40], 0, 1) ? "ztest" : "noztest",
                            cmpName(bits(bp[0x40], 1, 3)), bits(bp[0x40], 4, 1));
        copyFilters.add(fmt("copy filter regs 01-04: %06X %06X %06X %06X, vertical 53/54: %06X %06X", bp[1], bp[2], bp[3], bp[4], bp[0x53], bp[0x54]));
        efbCopies.add(k);
        efbCopyKinds.add(fmt("%s clear=%u", toXfb ? "to-XFB" : "to-texture", clear));
    }

    // Texture map bound state.
    struct TexBinding { uint32_t mode0, mode1, img0, img1, img2, img3, tlut; };
    TexBinding texmap(uint32_t n) const {
        const uint32_t base = n < 4 ? 0x80 + n : 0xA0 + (n - 4);
        return {bp[base], bp[base + 4], bp[base + 8], bp[base + 12], bp[base + 16], bp[base + 20], bp[base + 24]};
    }
    std::string describeTex(uint32_t n) const {
        const TexBinding t = texmap(n);
        const uint32_t f = bits(t.img0, 20, 4), w = bits(t.img0, 0, 10) + 1, h = bits(t.img0, 10, 10) + 1;
        std::string s = fmt("%s %ux%u", texFmt(f), w, h);
        if (f >= 8 && f <= 10) s += fmt(" tlutfmt=%s", tlutFmt(bits(t.tlut, 10, 2)));
        const uint32_t minf = bits(t.mode0, 5, 3);
        s += fmt(" wrap=%s/%s min=%s mag=%s", wrapName(bits(t.mode0, 0, 2)), wrapName(bits(t.mode0, 2, 2)), filtName(minf), bits(t.mode0, 4, 1) ? "linear" : "near");
        if (minf >= 2) s += fmt(" maxlod=%u", bits(t.mode1, 8, 8) / 16);
        if (bits(t.mode0, 9, 10)) s += " lodbias";
        if (bits(t.mode0, 19, 2)) s += " aniso";
        return s;
    }

    void drawState(uint32_t op, uint32_t count) {
        Frame& fr = frameList.back();
        ++fr.draws;
        fr.verts += count;
        const uint32_t gm = bp[0x00], ntex = bits(gm, 0, 4), ncol = bits(gm, 4, 5), ntev = bits(gm, 10, 4) + 1, nind = bits(gm, 16, 3);
        genModes.add(fmt("ntexgen=%u ncolchan=%u ntev=%u nind=%u cull=%u zfreeze=%u msaa=%u", ntex, ncol, ntev, nind, bits(gm, 14, 2), bits(gm, 19, 1), bits(gm, 9, 1)));
        tevStages.add(fmt("%u stages", ntev));
        // XF side.
        std::string xf = fmt("numcolors=%u numtexgens=%u", xfReg[0x09], xfReg[0x3F]);
        for (uint32_t i = 0; i < xfReg[0x09] && i < 2; ++i)
            xf += fmt(" | color%u: %s | alpha%u: %s", i, decodeLightChan(xfReg[0x0E + i]).c_str(), i, decodeLightChan(xfReg[0x10 + i]).c_str());
        lightChans.add(xf);
        projAtDraw.add(fmt("%s projection at draw", xfReg[0x26] ? "orthographic" : "perspective"));
        for (uint32_t i = 0; i < xfReg[0x3F] && i < 8; ++i)
            texGens.add(fmt("texgen%u: %s | post matrix base=%u normalize=%u (dual tex %s)", i, decodeTexGen(xfReg[0x40 + i]).c_str(), bits(xfReg[0x50 + i], 0, 6), bits(xfReg[0x50 + i], 8, 1),
                            xfReg[0x12] ? "on" : "off"));
        // Pixel pipeline.
        const uint32_t zm = bp[0x40], cm = bp[0x41], pc = bp[0x43];
        zmodes.add(fmt("ztest=%s func=%s zwrite=%u", bits(zm, 0, 1) ? "on" : "off", cmpName(bits(zm, 1, 3)), bits(zm, 4, 1)));
        blends.add(fmt("blend=%u logic=%u dither=%u colorUpdate=%u alphaUpdate=%u src=%s dst=%s subtract=%u logicop=%s constAlpha=%s(%u)", bits(cm, 0, 1), bits(cm, 1, 1), bits(cm, 2, 1), bits(cm, 3, 1),
                         bits(cm, 4, 1), blendFactor(bits(cm, 8, 3)), blendFactor(bits(cm, 5, 3)), bits(cm, 11, 1), logicOp(bits(cm, 12, 4)), bits(bp[0x42], 8, 1) ? "on" : "off",
                         bits(bp[0x42], 0, 8)));
        peControl.add(fmt("efb=%s zformat=%u zcompare=%s", pixFmt(bits(pc, 0, 3)), bits(pc, 3, 3), bits(pc, 6, 1) ? "before-tex" : "after-tex"));
        const uint32_t afv = bp[0xF3];
        alphaFunc.add(fmt("(a %s %u) %s (a %s %u)", cmpName(bits(afv, 16, 3)), bits(afv, 0, 8), bits(afv, 22, 2) == 0 ? "AND" : bits(afv, 22, 2) == 1 ? "OR" : bits(afv, 22, 2) == 2 ? "XOR" : "XNOR",
                         cmpName(bits(afv, 19, 3)), bits(afv, 8, 8)));
        fog.add(fmt("fog type=%u proj=%s", bits(bp[0xF1], 21, 3), bits(bp[0xF1], 20, 1) ? "ortho" : "persp"));
        scissors.add(fmt("scissor tl=(%d,%d) br=(%d,%d) offset=(%u,%u)", int(bits(bp[0x20], 12, 12)) - 342, int(bits(bp[0x20], 0, 12)) - 342, int(bits(bp[0x21], 12, 12)) - 342, int(bits(bp[0x21], 0, 12)) - 342,
                         int(bits(bp[0x59], 0, 10)) * 2 - 342, int(bits(bp[0x59], 10, 10)) * 2 - 342));
        if (bp[0x22]) lineSizes.add(fmt("line width=%u (1/6 px) point size=%u (1/6 px) line offset=%u point offset=%u aspect=%u", bits(bp[0x22], 0, 8), bits(bp[0x22], 8, 8), bits(bp[0x22], 16, 3),
                                        bits(bp[0x22], 19, 3), bits(bp[0x22], 22, 1)));
        for (uint32_t i = 0; i < 8; ++i) {
            const uint32_t s = bp[0x30 + 2 * i], t = bp[0x31 + 2 * i];
            if ((s | t) && i < xfReg[0x3F]) texScale.add(fmt("coord%u: s=%u t=%u wraps=%u wrapt=%u bias(s)=%u cylwrap(s)=%u", i, bits(s, 0, 16) + 1, bits(t, 0, 16) + 1, bits(s, 16, 1), bits(t, 16, 1), bits(s, 17, 1), bits(s, 18, 1)));
        }
        // TEV signature and per-stage functions.
        std::string sig = fmt("ntev=%u ntex=%u ncol=%u nind=%u", ntev, ntex, ncol, nind);
        std::set<uint32_t> maps;
        for (uint32_t s = 0; s < ntev; ++s) {
            const uint32_t tref = bp[0x28 + s / 2], sh = (s & 1) ? 12 : 0;
            const uint32_t map = bits(tref, sh, 3), coord = bits(tref, sh + 3, 3), en = bits(tref, sh + 6, 1), cc = bits(tref, sh + 7, 3);
            const uint32_t ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
            const uint32_t ks = bp[0xF6 + s / 2], kcsel = (s & 1) ? bits(ks, 14, 5) : bits(ks, 4, 5), kasel = (s & 1) ? bits(ks, 19, 5) : bits(ks, 9, 5);
            std::string st = fmt("[%u] tex=%s coord=%u chan=%u | %s | %s | kc=%u ka=%u", s, en ? fmt("map%u", map).c_str() : "-", coord, cc, decodeColorEnv(ce).c_str(), decodeAlphaEnv(ae).c_str(), kcsel, kasel);
            sig += " " + st;
            tevFunctions.add(fmt("%s | %s", decodeColorEnv(ce).c_str(), decodeAlphaEnv(ae).c_str()));
            if (en) maps.insert(map);
            if (bits(ce, 16, 2) == 3 || bits(ae, 16, 2) == 3) misc.add("TEV compare-mode stage (bias = compare) in use");
            if (bits(ae, 0, 2) || bits(ae, 2, 2)) misc.add("TEV swap table selected (rswap/tswap non-zero)");
        }
        // Swap tables: KSEL(2i) holds r,g and KSEL(2i+1) holds b,a of table i (libogc GX_SetTevSwapModeTable).
        for (uint32_t i = 0; i < 4; ++i)
            swapTables.add(fmt("swap table %u: r=%u g=%u b=%u a=%u", i, bits(bp[0xF6 + 2 * i], 0, 2), bits(bp[0xF6 + 2 * i], 2, 2), bits(bp[0xF7 + 2 * i], 0, 2), bits(bp[0xF7 + 2 * i], 2, 2)));
        tevSignatures.add(sig);
        if (frameTev.insert(sig).second) ++fr.tevDistinct;
        // Indirect.
        if (nind) {
            indirect.add(fmt("draw with %u indirect stage(s), iref=0x%06X", nind, bp[0x27]));
            for (uint32_t s = 0; s < ntev; ++s) {
                const uint32_t ic = bp[0x10 + s];
                if (ic) indirect.add(fmt("IND_CMD%u: bt=%u fmt=%u bias=%u bs=%u mtx=%u wrap s=%u t=%u useprev=%u addprev=%u", s, bits(ic, 0, 2), bits(ic, 2, 2), bits(ic, 4, 3), bits(ic, 7, 2),
                                         bits(ic, 9, 4), bits(ic, 13, 3), bits(ic, 16, 3), bits(ic, 19, 1), bits(ic, 20, 1)));
            }
            for (uint32_t i = 0; i < nind; ++i) maps.insert(bits(bp[0x27], 6 * i, 3));
        }
        // Textures bound for the draw.
        for (uint32_t m : maps) {
            const std::string d = describeTex(m);
            textures.add(d);
            const TexBinding t = texmap(m);
            const std::string key = fmt("%s addr=%06X", d.c_str(), bits(t.img3, 0, 24));
            usedTexKeys.insert(key);
            const bool alias = copyDsts.count(bits(t.img3, 0, 24)) != 0;
            efbAlias.add(alias ? "texture source = an earlier EFB copy destination" : "texture from other memory");
            texModes.add(fmt("tmem even=0x%X (%u x %u cache) odd=0x%X", bits(t.img1, 0, 15), bits(t.img1, 15, 1), bits(t.img1, 18, 3), bits(t.img2, 0, 15)));
        }
        textureCount.add(fmt("%zu texture map(s) per draw", maps.size()));
        // Vertex.
        (void)op;
    }

    // ---- Stream parser ----
    void draw(uint8_t op, const uint8_t* d, size_t& i, size_t n, bool& need) {
        if (n - i < 3) { need = true; return; }
        const uint32_t count = uint32_t(d[i + 1]) << 8 | d[i + 2], vat = op & 7;
        const Layout L = buildLayout(cp, vat);
        if (L.size == 0) { fail("draw with an empty vertex descriptor", op); return; }
        const size_t total = 3 + size_t(count) * L.size;
        if (n - i < total) { need = true; return; }
        // Primitive and format statistics.
        const uint32_t prim = op & 0xF8;
        static const char* pn[] = {"quads(0x80)", "?0x88", "triangles(0x90)", "strip(0x98)", "fan(0xA0)", "lines(0xA8)", "linestrip(0xB0)", "points(0xB8)"};
        prims.add(fmt("%s vat%u", pn[(prim - 0x80) / 8], vat), 1);
        vertexFormats.add(fmt("%s  [vertex %u bytes]", L.desc.c_str(), L.size));
        ++draws;
        verts += count;
        drawState(op, count);
        // Per-vertex scan: matrix indices and array indices.
        const uint8_t* v = d + i + 3;
        for (uint32_t k = 0; k < count; ++k, v += L.size) {
            const uint8_t* p = v;
            if (L.pnmtx) { ++pnmtxVerts[*p]; ++p; }
            for (uint32_t t = 0; t < 8; ++t)
                if (L.texmtx[t]) { ++texmtxVerts[t][*p]; ++p; }
            auto idx = [&](const Attr& a, uint32_t array) {
                if (a.mode == 0) return;
                if (a.mode == 1) { p += a.bytes; return; }
                for (uint32_t j = 0; j < a.nidx; ++j) {
                    const uint32_t ix = a.mode == 2 ? p[0] : uint32_t(p[0]) << 8 | p[1];
                    maxIdx[array] = std::max(maxIdx[array], ix);
                    p += idxBytes(a.mode);
                }
            };
            idx(L.pos, 0);
            idx(L.nrm, 1);
            idx(L.col[0], 2);
            idx(L.col[1], 3);
            for (uint32_t t = 0; t < 8; ++t) idx(L.tex[t], 4 + t);
        }
        for (uint32_t a = 0; a < 12; ++a) {
            const Attr* at = a == 0 ? &L.pos : a == 1 ? &L.nrm : a < 4 ? &L.col[a - 2] : &L.tex[a - 4];
            if (at->mode >= 2) arrays.add(fmt("array %s used indexed (%s), stride %u", a == 0 ? "pos" : a == 1 ? "nrm" : a < 4 ? fmt("col%u", a - 2).c_str() : fmt("tex%u", a - 4).c_str(),
                                              attrMode(at->mode), cp.arrStride[a]), count);
        }
        i += total;
    }

    // Returns the bytes consumed; false in `incomplete` when the buffer ends inside a command.
    size_t parse(const uint8_t* d, size_t n, int depth, bool& incomplete) {
        size_t i = 0;
        incomplete = false;
        while (i < n && !failed) {
            const uint8_t op = d[i];
            size_t need = 1;
            bool more = false;
            const uint64_t at = offset + i;
            (void)at;
            if (op == 0x00) {
                cmds.add("NOP 0x00");
            } else if (op == 0x48) {
                cmds.add("INVALIDATE_VERTEX_CACHE 0x48");
            } else if (op == 0x08) {
                need = 6;
                if (n - i < need) { more = true; } else { cmds.add("LOAD_CP_REG 0x08"); cpLoad(d[i + 1], be32(d + i + 2)); }
            } else if (op == 0x10) {
                if (n - i < 5) { more = true; }
                else {
                    const uint32_t hdr = be32(d + i + 1), count = (hdr >> 16) + 1, addr = hdr & 0xFFFF;
                    need = 5 + size_t(count) * 4;
                    if (n - i < need) more = true;
                    else { cmds.add("LOAD_XF_REG 0x10"); xfLoad(addr, count, d + i + 5); }
                }
            } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
                need = 5;
                if (n - i < need) more = true;
                else {
                    const uint32_t w = be32(d + i + 1), index = w >> 16, len = bits(w, 12, 4) + 1, addr = w & 0xFFF;
                    const uint32_t arr = 12 + ((op - 0x20) >> 3);
                    cmds.add(fmt("LOAD_INDX_%c 0x%02X", 'A' + ((op - 0x20) >> 3), op));
                    xfIndexed.add(fmt("indexed XF load %c: addr 0x%03X, %u words, array stride %u", 'A' + ((op - 0x20) >> 3), addr, len, cp.arrStride[arr]));
                    maxIdx[arr] = std::max(maxIdx[arr], index);
                }
            } else if (op == 0x40) {
                need = 9;
                if (n - i < need) more = true;
                else {
                    cmds.add("CALL_DISPLAY_LIST 0x40");
                    const uint32_t addr = be32(d + i + 1), size = be32(d + i + 5);
                    ++calls;
                    if (depth >= 4) fail("display list nested deeper than 4", op);
                    else if (dlQueue.empty()) fail("display list record missing from the dump", op);
                    else {
                        auto rec = std::move(dlQueue.front());
                        dlQueue.pop_front();
                        if (rec.first != addr || rec.second.size() != size) fail("display list record does not match the call", op);
                        else {
                            dlBytes += size;
                            bool inc = false;
                            const size_t used = parse(rec.second.data(), rec.second.size(), depth + 1, inc);
                            if (!failed && (inc || used != size)) fail("display list ends inside a command", op);
                        }
                    }
                }
            } else if (op == 0x61) {
                need = 5;
                if (n - i < need) more = true;
                else { cmds.add("LOAD_BP_REG 0x61"); bpLoad(be32(d + i + 1)); }
            } else if ((op & 0xC0) == 0x80 && op != 0x88 && (op & 0x38) != 0x08) {
                bool m = false;
                const size_t before = i;
                draw(op, d, i, n, m);
                if (m) more = true;
                else if (!failed) { cmds.add(fmt("DRAW 0x%02X", op & 0xF8)); continue; }
                i = before;
            } else {
                fail("unknown command opcode", op);
            }
            if (more) { incomplete = true; break; }
            if (failed) break;
            i += need;
        }
        return i;
    }

    std::vector<uint8_t> pending;

    bool run(const std::string& path, std::string& err) {
        Reader r;
        if (!r.open(path, err)) return false;
        Record rec;
        while (r.next(rec, err)) {
            switch (rec.type) {
                case Rec::Frame:
                    frameList.push_back(Frame{rec.addr});
                    frameTev.clear();
                    ++frameCount;
                    break;
                case Rec::DisplayList:
                    dlQueue.emplace_back(rec.addr, std::move(rec.data));
                    break;
                case Rec::Prelude: {
                    if (frameList.empty()) frameList.push_back(Frame{0});
                    bool inc = false;
                    parse(rec.data.data(), rec.data.size(), 0, inc);
                    break;
                }
                case Rec::Fifo: {
                    if (frameList.empty()) frameList.push_back(Frame{0});
                    frameList.back().bytes += rec.data.size();
                    fifoBytes += rec.data.size();
                    pending.insert(pending.end(), rec.data.begin(), rec.data.end());
                    bool inc = false;
                    const size_t used = parse(pending.data(), pending.size(), 0, inc);
                    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(used));
                    offset += used;
                    break;
                }
            }
            if (failed) { err = failMsg; return false; }
        }
        return err.empty();
    }

    void report(FILE* out, bool perFrame) {
        std::fprintf(out, "GX survey: %llu frames, %llu draws, %llu vertices, %llu FIFO bytes, %llu display-list calls (%llu bytes)\n", static_cast<unsigned long long>(frameCount),
                     static_cast<unsigned long long>(draws), static_cast<unsigned long long>(verts), static_cast<unsigned long long>(fifoBytes), static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(dlBytes));
        cmds.print(out, "== Commands");
        prims.print(out, "== Primitive types");
        vertexFormats.print(out, "== Vertex formats (per draw)");
        arrays.print(out, "== Indexed attribute arrays (vertices)");
        std::fprintf(out, "\n== Array registers: distinct bases / strides, largest index used\n");
        static const char* an[] = {"pos", "nrm", "col0", "col1", "tex0", "tex1", "tex2", "tex3", "tex4", "tex5", "tex6", "tex7", "xf-A(pos mtx)", "xf-B(nrm mtx)", "xf-C(tex mtx)", "xf-D(light)"};
        for (int a = 0; a < 16; ++a)
            if (!arrBasesSeen[a].empty() || !arrStridesSeen[a].empty()) {
                std::fprintf(out, "  %-14s bases=%zu strides:", an[a], arrBasesSeen[a].size());
                for (uint32_t s : arrStridesSeen[a]) std::fprintf(out, " %u", s);
                std::fprintf(out, "  max index %u\n", maxIdx[a]);
            }
        std::fprintf(out, "\n== Per-vertex matrix indices (position/normal matrix index byte)\n");
        for (const auto& e : pnmtxVerts) std::fprintf(out, "  pnmtx %u : %llu vertices\n", e.first, static_cast<unsigned long long>(e.second));
        for (int t = 0; t < 8; ++t)
            for (const auto& e : texmtxVerts[t]) std::fprintf(out, "  tex%d mtx idx %u : %llu vertices\n", t, e.first, static_cast<unsigned long long>(e.second));
        cpRegs.print(out, "== CP register writes");
        cpRegValues.print(out, "== CP matrix-index defaults");
        xfDirect.print(out, "== XF memory loads (direct)", 80);
        xfIndexed.print(out, "== XF indexed loads");
        xfRegWrites.print(out, "== XF register writes");
        viewports.print(out, "== Viewports (XF 0x101A scale/offset), as loaded", 40);
        projections.print(out, "== Projections (loads)", 40);
        projAtDraw.print(out, "== Projection in effect at draw");
        lightChans.print(out, "== XF colour/lighting channels at draw", 40);
        texGens.print(out, "== Texgen units at draw", 60);
        genModes.print(out, "== BP GEN_MODE at draw");
        tevStages.print(out, "== TEV stage counts at draw");
        tevFunctions.print(out, "== Distinct TEV stage functions (colour|alpha)", 100);
        std::fprintf(out, "\n== Distinct TEV configurations (whole-pipeline signatures): %zu\n", tevSignatures.m.size());
        tevSignatures.print(out, "== TEV configurations by draws", 60);
        swapTables.print(out, "== TEV swap tables at draw");
        indirect.print(out, "== Indirect texturing", 40);
        alphaFunc.print(out, "== Alpha compare at draw");
        zmodes.print(out, "== Z mode at draw");
        blends.print(out, "== Blend / colour update at draw");
        peControl.print(out, "== EFB pixel format / z-compare location at draw");
        fog.print(out, "== Fog at draw");
        scissors.print(out, "== Scissor at draw", 40);
        lineSizes.print(out, "== Line / point sizes at draw");
        texScale.print(out, "== Texcoord scale (SU_SSIZE/TSIZE) at draw", 40);
        textures.print(out, "== Textures bound by draws (format size wrap filter)", 200);
        textureCount.print(out, "== Textures per draw");
        texModes.print(out, "== TMEM configuration of bound textures", 20);
        efbAlias.print(out, "== Texture sources");
        std::fprintf(out, "\n== Distinct textures (format size address) referenced: %zu\n", usedTexKeys.size());
        tlutLoads.print(out, "== TLUT loads");
        efbCopyKinds.print(out, "== EFB copies by kind");
        efbCopies.print(out, "== EFB copies in detail", 80);
        copyFilters.print(out, "== Copy filter registers at copies");
        bpRegs.print(out, "== BP register writes");
        unknownBp.print(out, "== BP registers outside the documented map", 40);
        misc.print(out, "== Miscellaneous events", 60);
        // Frames.
        uint64_t withDraws = 0, maxDraws = 0, maxVerts = 0, maxTev = 0;
        for (const Frame& f : frameList) {
            if (f.draws) ++withDraws;
            maxDraws = std::max(maxDraws, f.draws);
            maxVerts = std::max(maxVerts, f.verts);
            maxTev = std::max(maxTev, f.tevDistinct);
        }
        std::fprintf(out, "\n== Frames: %zu in the dump, %llu with draws; max draws/frame %llu, max vertices/frame %llu, max distinct TEV configs/frame %llu\n", frameList.size(),
                     static_cast<unsigned long long>(withDraws), static_cast<unsigned long long>(maxDraws), static_cast<unsigned long long>(maxVerts), static_cast<unsigned long long>(maxTev));
        if (perFrame) {
            std::fprintf(out, "retrace draws vertices copies(xfb/tex) clears tevcfgs bytes\n");
            for (const Frame& f : frameList)
                std::fprintf(out, "%6u %6llu %8llu %llu/%llu %llu %llu %llu\n", f.retrace, static_cast<unsigned long long>(f.draws), static_cast<unsigned long long>(f.verts),
                             static_cast<unsigned long long>(f.xfbCopies), static_cast<unsigned long long>(f.texCopies), static_cast<unsigned long long>(f.clears),
                             static_cast<unsigned long long>(f.tevDistinct), static_cast<unsigned long long>(f.bytes));
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string in, outPath;
    bool perFrame = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--frames") perFrame = true;
        else if (in.empty()) in = a;
        else { std::fprintf(stderr, "unexpected argument %s\n", a.c_str()); return 2; }
    }
    if (in.empty()) {
        std::fprintf(stderr, "usage: awl-gx-survey <dump> [--out report.txt] [--frames]\n");
        return 2;
    }
    auto s = std::make_unique<Survey>();
    std::string err;
    const bool ok = s->run(in, err);
    FILE* out = stdout;
    if (!outPath.empty()) {
        out = std::fopen(outPath.c_str(), "w");
        if (!out) { std::fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    }
    s->report(out, perFrame);
    if (out != stdout) std::fclose(out);
    if (!ok) {
        std::fprintf(stderr, "SURVEY STOPPED: %s\n", err.c_str());
        return 3;
    }
    return 0;
}
