// TEV-to-HLSL pixel shader generation: the renderer-internal description of a pixel-pipeline configuration and the
// generator that turns it into HLSL. Design and the rules behind each field: docs/research/gfx-dx11.md (sections 4-6).
//
// SKELETON (milestone 1). The key below is the renderer's own; milestone 2 fills it from the GX core's IR once
// src/gx/gx_ir.h is frozen. What the generator handles today: up to 16 arithmetic stages (inputs, bias, scale, clamp,
// output register), konst colour/alpha selection, swap tables, texture and raster colour sources, the alpha compare.
// Everything else a key can express is rejected with a named reason: nothing is silently ignored.
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace awl::gfx::tev {

// Names and values follow libogc gx.h (GX_CC_*, GX_CA_*, GX_TB_*, GX_CS_*, GX_TEV_*) because these are the values the
// BP registers carry.
enum ColorIn : uint8_t { CC_CPREV, CC_APREV, CC_C0, CC_A0, CC_C1, CC_A1, CC_C2, CC_A2, CC_TEXC, CC_TEXA, CC_RASC, CC_RASA, CC_ONE, CC_HALF, CC_KONST, CC_ZERO };
enum AlphaIn : uint8_t { CA_APREV, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO };
enum Bias : uint8_t { BIAS_ZERO, BIAS_PLUS_HALF, BIAS_MINUS_HALF };
enum Scale : uint8_t { SCALE_1, SCALE_2, SCALE_4, SCALE_HALF };
enum Dest : uint8_t { DEST_PREV, DEST_REG0, DEST_REG1, DEST_REG2 };
// Compare modes (bias == 3 in the register): result = d + (cmp(a, b) ? c : 0), the compared quantity per mode.
enum Compare : uint8_t { CMP_NONE, CMP_R8_GT, CMP_R8_EQ, CMP_GR16_GT, CMP_GR16_EQ, CMP_BGR24_GT, CMP_BGR24_EQ, CMP_RGB8_GT, CMP_RGB8_EQ };
enum RasChannel : uint8_t { RAS_COLOR0, RAS_COLOR1, RAS_ALPHA_BUMP = 5, RAS_ALPHA_BUMP_N = 6, RAS_ZERO = 7 };
constexpr uint8_t kNone = 0xFF;

struct Stage {
    uint8_t colA = CC_ZERO, colB = CC_ZERO, colC = CC_ZERO, colD = CC_ZERO;
    uint8_t colSub = 0, colBias = BIAS_ZERO, colScale = SCALE_1, colClamp = 1, colDest = DEST_PREV, colCmp = CMP_NONE;
    uint8_t alpA = CA_ZERO, alpB = CA_ZERO, alpC = CA_ZERO, alpD = CA_ZERO;
    uint8_t alpSub = 0, alpBias = BIAS_ZERO, alpScale = SCALE_1, alpClamp = 1, alpDest = DEST_PREV, alpCmp = CMP_NONE;
    uint8_t texMap = kNone, texCoord = kNone;   // TEV order: which texture (0-7) sampled with which coordinate set (0-7)
    uint8_t rasChannel = RAS_ZERO;
    uint8_t kColorSel = 0, kAlphaSel = 0;       // GX_TEV_KCSEL_* / KASEL_* (0-7 fractions, 12-15 K0-K3, 16-31 single channels)
    uint8_t rasSwap = 0, texSwap = 0;           // index of the swap table applied to the raster / texture colour
    uint8_t indStage = kNone;                   // indirect texturing is not generated yet: any value other than kNone is rejected
};

struct SwapTable { uint8_t r = 0, g = 1, b = 2, a = 3; };   // source channel (0=R 1=G 2=B 3=A) for each output channel

// Plain bytes only (no padding): memcmp / hash compare keys.
struct PixelShaderKey {
    uint8_t numStages = 1;
    Stage stages[16];
    SwapTable swaps[4];
    uint8_t projectiveMask = 0;   // bit n: coordinate set n carries q and is divided per pixel
    // alpha compare: comparison codes GX_NEVER..GX_ALWAYS = 0..7, logic AND/OR/XOR/XNOR = 0..3
    uint8_t alphaComp0 = 7, alphaComp1 = 7, alphaLogic = 0;
    uint8_t fogType = 0;          // 0 = none; any other value is rejected for now
    uint8_t efbFormat = 0;        // 0 = RGB8_Z24; others are rejected for now (precision rules: design note section 7)
    uint8_t reserved = 0;          // must be 0
};
static_assert(std::is_trivially_copyable<PixelShaderKey>::value, "key must be plain bytes");

std::string keyBytes(const PixelShaderKey& key);
uint64_t keyHash(const PixelShaderKey& key);   // FNV-1a over keyBytes

struct HlslResult {
    bool ok = false;
    std::string hlsl;
    std::string unsupported;   // the first reason the key cannot be generated; empty when ok
};

// Pixel shader entry point is `main`. Constant buffer b0 ("TevConstants"): kcolor[4], tevreg[4], alphaRef (x, y as 0-255).
// Textures t0-t7 / samplers s0-s7. Input layout: see vertexShaderHlsl().
HlslResult generatePixelShader(const PixelShaderKey& key);

// Layout of the constant buffer, for the C++ side to fill.
struct TevConstants {
    float kcolor[4][4];   // K0-K3, RGBA in 0..1
    float tevreg[4][4];   // PREV, REG0, REG1, REG2 initial values
    float alphaRef[4];    // x = ref0, y = ref1 (0..255), zw unused
};
static_assert(sizeof(TevConstants) == 144, "must match the cbuffer in the generated HLSL");

// Vertex shader for post-transform vertices (clip-space position, two colours, eight 3-component coordinate sets)
// with the GX depth-range mapping. Constant buffer b0: depthRange.x = near, .y = far (0..1).
const char* vertexShaderHlsl();
struct VertexConstants { float depthRange[4]; };
struct Vertex {
    float pos[4];
    float col0[4], col1[4];
    float uv[8][3];
};

}  // namespace awl::gfx::tev
