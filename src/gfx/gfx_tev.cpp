#include "gfx/gfx_tev.h"

#include <cstring>

namespace awl::gfx::tev {

std::string keyBytes(const PixelShaderKey& key) { return std::string(reinterpret_cast<const char*>(&key), sizeof key); }

uint64_t keyHash(const PixelShaderKey& key) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&key);
    for (size_t i = 0; i < sizeof key; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

namespace {

const char* kFractions[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
const char kChannel[4] = {'r', 'g', 'b', 'a'};
const char* kRegName[4] = {"r_prev", "r_reg0", "r_reg1", "r_reg2"};

struct Gen {
    std::string out;
    std::string why;   // first unsupported reason

    void fail(const std::string& s) { if (why.empty()) why = s; }
    void line(const std::string& s) { out += "    " + s + "\n"; }

    // Constant colour selected by a KCSEL value; empty and a recorded failure for values the BP cannot encode.
    std::string kColor(uint8_t sel) {
        if (sel < 8) return std::string("float3(") + kFractions[sel] + "," + kFractions[sel] + "," + kFractions[sel] + ")";
        if (sel >= 12 && sel <= 15) return "kcolor[" + std::to_string(sel - 12) + "].rgb";
        if (sel >= 16) return "kcolor[" + std::to_string((sel - 16) & 3) + "]." + std::string(3, kChannel[(sel - 16) >> 2]);
        fail("konst colour selector " + std::to_string(sel) + " is not a valid GX_TEV_KCSEL value");
        return "float3(0,0,0)";
    }
    std::string kAlpha(uint8_t sel) {
        if (sel < 8) return kFractions[sel];
        if (sel >= 16) return "kcolor[" + std::to_string((sel - 16) & 3) + "]." + std::string(1, kChannel[(sel - 16) >> 2]);
        fail("konst alpha selector " + std::to_string(sel) + " is not a valid GX_TEV_KASEL value");
        return "0.0";
    }

    std::string colorInput(uint8_t in, bool texOk) {
        switch (in) {
            case CC_CPREV: return "r_prev.rgb";
            case CC_APREV: return "r_prev.aaa";
            case CC_C0: return "r_reg0.rgb";
            case CC_A0: return "r_reg0.aaa";
            case CC_C1: return "r_reg1.rgb";
            case CC_A1: return "r_reg1.aaa";
            case CC_C2: return "r_reg2.rgb";
            case CC_A2: return "r_reg2.aaa";
            case CC_TEXC: if (!texOk) fail("a stage reads the texture colour but has no texture bound"); return "tex.rgb";
            case CC_TEXA: if (!texOk) fail("a stage reads the texture alpha but has no texture bound"); return "tex.aaa";
            case CC_RASC: return "ras.rgb";
            case CC_RASA: return "ras.aaa";
            case CC_ONE: return "float3(1,1,1)";
            case CC_HALF: return "float3(0.5,0.5,0.5)";
            case CC_KONST: return "kc";
            case CC_ZERO: return "float3(0,0,0)";
            default: fail("colour input " + std::to_string(in) + " out of range"); return "float3(0,0,0)";
        }
    }
    std::string alphaInput(uint8_t in, bool texOk) {
        switch (in) {
            case CA_APREV: return "r_prev.a";
            case CA_A0: return "r_reg0.a";
            case CA_A1: return "r_reg1.a";
            case CA_A2: return "r_reg2.a";
            case CA_TEXA: if (!texOk) fail("a stage reads the texture alpha but has no texture bound"); return "tex.a";
            case CA_RASA: return "ras.a";
            case CA_KONST: return "ka";
            case CA_ZERO: return "0.0";
            default: fail("alpha input " + std::to_string(in) + " out of range"); return "0.0";
        }
    }
    static const char* scaleText(uint8_t s) { return s == SCALE_1 ? "1.0" : s == SCALE_2 ? "2.0" : s == SCALE_4 ? "4.0" : "0.5"; }
    static const char* biasText(uint8_t b) { return b == BIAS_ZERO ? "0.0" : b == BIAS_PLUS_HALF ? "0.5" : "-0.5"; }

    // Four channels of `src` selected through a swap table.
    static std::string swizzle(const std::string& src, const SwapTable& t) {
        const uint8_t c[4] = {t.r, t.g, t.b, t.a};
        std::string s = "float4(";
        for (int i = 0; i < 4; ++i) { if (i) s += ","; s += src + "." + kChannel[c[i] & 3]; }
        return s + ")";
    }
};

const char* compareText(uint8_t c) {
    switch (c) {
        case 1: return "<";
        case 2: return "==";
        case 3: return "<=";
        case 4: return ">";
        case 5: return "!=";
        case 6: return ">=";
        default: return nullptr;   // 0 NEVER, 7 ALWAYS handled by the caller
    }
}

std::string alphaCompare(uint8_t code, const char* ref) {
    if (code == 0) return "false";
    if (code == 7) return "true";
    return std::string("(a8 ") + compareText(code) + " (uint)" + ref + ")";
}

}  // namespace

HlslResult generatePixelShader(const PixelShaderKey& key) {
    HlslResult res;
    Gen g;
    if (key.numStages < 1 || key.numStages > 16) { res.unsupported = "stage count " + std::to_string(key.numStages) + " outside 1-16"; return res; }
    if (key.fogType != 0 && (key.fogType < 2 || key.fogType > 7 || key.fogType == 3)) { res.unsupported = "fog type " + std::to_string(key.fogType) + " is not a GX fog type"; return res; }
    if (key.fogType != 0 && key.fogOrtho) { res.unsupported = "fog with an orthographic projection (parameters not verified against a stream)"; return res; }
    if (key.reserved != 0) { res.unsupported = "reserved key byte is not zero"; return res; }
    if (key.efbFormat != 0) { res.unsupported = "EFB pixel format " + std::to_string(key.efbFormat) + " is not emulated yet (only RGB8)"; return res; }

    g.out =
        "cbuffer TevConstants : register(b0) {\n"
        "    float4 kcolor[4];\n"
        "    float4 tevreg[4];\n"
        "    float4 alphaRef;\n"
        "    float4 stageScale[16];\n"
        "    float4 fogParam;\n"
        "    float4 fogColor;\n"
        "};\n"
        "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1); Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
        "Texture2D tex4 : register(t4); Texture2D tex5 : register(t5); Texture2D tex6 : register(t6); Texture2D tex7 : register(t7);\n"
        "SamplerState smp0 : register(s0); SamplerState smp1 : register(s1); SamplerState smp2 : register(s2); SamplerState smp3 : register(s3);\n"
        "SamplerState smp4 : register(s4); SamplerState smp5 : register(s5); SamplerState smp6 : register(s6); SamplerState smp7 : register(s7);\n"
        "struct PsIn {\n"
        "    float4 pos : SV_Position; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float3 uv0 : TEXCOORD0; float3 uv1 : TEXCOORD1; float3 uv2 : TEXCOORD2; float3 uv3 : TEXCOORD3;\n"
        "    float3 uv4 : TEXCOORD4; float3 uv5 : TEXCOORD5; float3 uv6 : TEXCOORD6; float3 uv7 : TEXCOORD7;\n"
        "};\n"
        "float4 main(PsIn i) : SV_Target0 {\n";
    g.line("float4 r_prev = tevreg[0]; float4 r_reg0 = tevreg[1]; float4 r_reg1 = tevreg[2]; float4 r_reg2 = tevreg[3];");

    for (uint32_t n = 0; n < key.numStages && g.why.empty(); ++n) {
        const Stage& s = key.stages[n];
        const std::string tag = "stage " + std::to_string(n) + ": ";
        if (s.indStage != kNone) { g.fail(tag + "indirect texturing is not generated yet"); break; }
        if (s.alpCmp != CMP_NONE && s.alpCmp != CMP_RGB8_GT && s.alpCmp != CMP_RGB8_EQ) { g.fail(tag + "alpha compare mode other than A8 (R8/GR16/BGR24 forms on alpha are not defined)"); break; }
        if (s.colCmp > CMP_RGB8_EQ || s.alpCmp > CMP_RGB8_EQ) { g.fail(tag + "compare mode out of range"); break; }
        if (s.rasChannel == RAS_ALPHA_BUMP || s.rasChannel == RAS_ALPHA_BUMP_N) { g.fail(tag + "alpha-bump raster source needs indirect texturing"); break; }
        if (s.rasChannel > RAS_COLOR1 && s.rasChannel != RAS_ZERO) { g.fail(tag + "raster channel " + std::to_string(s.rasChannel) + " is not valid"); break; }
        if (s.rasSwap > 3 || s.texSwap > 3) { g.fail(tag + "swap table index out of range"); break; }
        const bool texOk = s.texMap != kNone && s.texCoord != kNone;
        if ((s.texMap == kNone) != (s.texCoord == kNone)) { g.fail(tag + "texture map and coordinate set must both be present or both absent"); break; }
        if (texOk && (s.texMap > 7 || s.texCoord > 7)) { g.fail(tag + "texture map or coordinate set out of range"); break; }
        if (s.colDest > 3 || s.alpDest > 3 || s.colScale > 3 || s.alpScale > 3 || s.colBias > 2 || s.alpBias > 2) { g.fail(tag + "register encoding out of range"); break; }

        g.line("{   // " + tag.substr(0, tag.size() - 2));
        if (texOk) {
            const std::string m = std::to_string(s.texMap), c = std::to_string(s.texCoord);
            const bool proj = (key.projectiveMask >> s.texCoord) & 1;
            g.line("    float2 st = " + std::string(proj ? "i.uv" + c + ".xy / i.uv" + c + ".z" : "i.uv" + c + ".xy") + " * stageScale[" + std::to_string(n) + "].xy;");
            g.line("    float4 tex_raw = tex" + m + ".Sample(smp" + m + ", st);");
            g.line("    float4 tex = " + Gen::swizzle("tex_raw", key.swaps[s.texSwap]) + ";");
        } else {
            g.line("    float4 tex = float4(0,0,0,0);");
        }
        const char* rasSrc = s.rasChannel == RAS_COLOR0 ? "i.col0" : s.rasChannel == RAS_COLOR1 ? "i.col1" : "float4(0,0,0,0)";
        g.line(std::string("    float4 ras_raw = ") + rasSrc + ";");
        g.line("    float4 ras = " + Gen::swizzle("ras_raw", key.swaps[s.rasSwap]) + ";");
        g.line("    float3 kc = " + g.kColor(s.kColorSel) + ";");
        g.line("    float ka = " + g.kAlpha(s.kAlphaSel) + ";");

        // colour: out = (d (+|-) ((1-c)*a + c*b) + bias) * scale
        g.line("    float3 ca = saturate(" + g.colorInput(s.colA, texOk) + "), cb = saturate(" + g.colorInput(s.colB, texOk) + "), cc = saturate(" +
               g.colorInput(s.colC, texOk) + "), cd = " + g.colorInput(s.colD, texOk) + ";");
        if (s.colCmp == CMP_NONE) {
            g.line(std::string("    float3 cres = (cd ") + (s.colSub ? "-" : "+") + " lerp(ca, cb, cc) + " + Gen::biasText(s.colBias) + ") * " + Gen::scaleText(s.colScale) + ";");
        } else {
            // compare modes: out = d + (a CMP b ? c : 0) on the 8-bit inputs; bias and scale do not apply
            const bool eq = s.colCmp == CMP_R8_EQ || s.colCmp == CMP_GR16_EQ || s.colCmp == CMP_BGR24_EQ || s.colCmp == CMP_RGB8_EQ;
            const char* op = eq ? "==" : ">";
            g.line("    uint3 ia = (uint3)(ca * 255.0 + 0.5), ib = (uint3)(cb * 255.0 + 0.5);");
            if (s.colCmp == CMP_RGB8_GT || s.colCmp == CMP_RGB8_EQ) {
                g.line(std::string("    bool3 cmp = ia ") + op + " ib;");
                g.line("    float3 cres = cd + float3(cmp.x ? cc.x : 0.0, cmp.y ? cc.y : 0.0, cmp.z ? cc.z : 0.0);");
            } else {
                const char* va = (s.colCmp == CMP_R8_GT || s.colCmp == CMP_R8_EQ) ? "ia.r" : (s.colCmp == CMP_GR16_GT || s.colCmp == CMP_GR16_EQ) ? "((ia.g << 8) | ia.r)" : "((ia.b << 16) | (ia.g << 8) | ia.r)";
                const char* vb = (s.colCmp == CMP_R8_GT || s.colCmp == CMP_R8_EQ) ? "ib.r" : (s.colCmp == CMP_GR16_GT || s.colCmp == CMP_GR16_EQ) ? "((ib.g << 8) | ib.r)" : "((ib.b << 16) | (ib.g << 8) | ib.r)";
                g.line(std::string("    bool cmp = ") + va + " " + op + " " + vb + ";");
                g.line("    float3 cres = cd + (cmp ? cc : float3(0.0, 0.0, 0.0));");
            }
        }
        g.line(s.colClamp ? "    cres = saturate(cres);" : "    cres = clamp(cres, -1024.0/255.0, 1023.0/255.0);");
        // alpha, same form
        g.line("    float aa = saturate(" + g.alphaInput(s.alpA, texOk) + "), ab = saturate(" + g.alphaInput(s.alpB, texOk) + "), ac = saturate(" +
               g.alphaInput(s.alpC, texOk) + "), ad = " + g.alphaInput(s.alpD, texOk) + ";");
        if (s.alpCmp == CMP_NONE) {
            g.line(std::string("    float ares = (ad ") + (s.alpSub ? "-" : "+") + " lerp(aa, ab, ac) + " + Gen::biasText(s.alpBias) + ") * " + Gen::scaleText(s.alpScale) + ";");
        } else {
            g.line("    uint a8a = (uint)(aa * 255.0 + 0.5), a8b = (uint)(ab * 255.0 + 0.5);");
            g.line(std::string("    float ares = ad + ((a8a ") + (s.alpCmp == CMP_RGB8_EQ ? "==" : ">") + " a8b) ? ac : 0.0);");
        }
        g.line(s.alpClamp ? "    ares = saturate(ares);" : "    ares = clamp(ares, -1024.0/255.0, 1023.0/255.0);");
        g.line(std::string("    ") + kRegName[s.colDest] + ".rgb = cres; " + kRegName[s.alpDest] + ".a = ares;");
        g.line("}");
    }
    if (!g.why.empty()) { res.unsupported = g.why; return res; }

    // Fog (GX_SetFog): ze = a / (b - z) with z the screen depth in [0, 1], f = clamp(ze - c, 0, 1), then the type's curve, and the colour
    // moves towards the fog colour by that amount. fogParam = (a, b, c), fogColor in 0..1 (libogc GX_SetFog derives a, b, c).
    if (key.fogType != 0) {
        g.line("float fz = saturate(fogParam.x / (fogParam.y - i.pos.z) - fogParam.z);");
        const char* curve = key.fogType == 2 ? "fz" : key.fogType == 4 ? "1.0 - exp2(-8.0 * fz)" : key.fogType == 5 ? "1.0 - exp2(-8.0 * fz * fz)" :
                            key.fogType == 6 ? "exp2(-8.0 * (1.0 - fz))" : "exp2(-8.0 * (1.0 - fz) * (1.0 - fz))";
        g.line(std::string("float fogAmount = ") + curve + ";");
        g.line("r_prev.rgb = lerp(r_prev.rgb, fogColor.rgb, fogAmount);");
    }

    // Alpha compare on the 8-bit result alpha: (comp0(a, ref0) logic comp1(a, ref1)); a failed test discards the pixel.
    const bool alwaysTrue = key.alphaComp0 == 7 && key.alphaComp1 == 7 && key.alphaLogic == 0;
    if (key.alphaComp0 > 7 || key.alphaComp1 > 7 || key.alphaLogic > 3) { res.unsupported = "alpha compare encoding out of range"; return res; }
    if (!alwaysTrue) {
        static const char* logic[4] = {"&&", "||", "!=", "=="};
        g.line("uint a8 = (uint)(saturate(r_prev.a) * 255.0 + 0.5);");
        g.line("bool atest = " + alphaCompare(key.alphaComp0, "alphaRef.x") + " " + logic[key.alphaLogic] + " " + alphaCompare(key.alphaComp1, "alphaRef.y") + ";");
        g.line("if (!atest) discard;");
    }
    g.line("return saturate(r_prev);");
    g.out += "}\n";
    res.ok = true;
    res.hlsl = std::move(g.out);
    return res;
}

const char* vertexShaderHlsl() {
    return R"(
struct VtxIn {
    float4 pos : POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float3 uv0 : TEXCOORD0; float3 uv1 : TEXCOORD1; float3 uv2 : TEXCOORD2; float3 uv3 : TEXCOORD3;
    float3 uv4 : TEXCOORD4; float3 uv5 : TEXCOORD5; float3 uv6 : TEXCOORD6; float3 uv7 : TEXCOORD7;
};
struct VtxOut {
    float4 pos : SV_Position; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float3 uv0 : TEXCOORD0; float3 uv1 : TEXCOORD1; float3 uv2 : TEXCOORD2; float3 uv3 : TEXCOORD3;
    float3 uv4 : TEXCOORD4; float3 uv5 : TEXCOORD5; float3 uv6 : TEXCOORD6; float3 uv7 : TEXCOORD7;
};
VtxOut main(VtxIn v) {
    VtxOut o;
    // GX clip space has z in [-w, 0] (near -> -w, far -> 0); D3D wants [0, w]. The viewport maps it to [near, far].
    o.pos = float4(v.pos.x, v.pos.y, v.pos.z + v.pos.w, v.pos.w);
    o.col0 = v.col0; o.col1 = v.col1;
    o.uv0 = v.uv0; o.uv1 = v.uv1; o.uv2 = v.uv2; o.uv3 = v.uv3; o.uv4 = v.uv4; o.uv5 = v.uv5; o.uv6 = v.uv6; o.uv7 = v.uv7;
    return o;
}
)";
}

}  // namespace awl::gfx::tev
