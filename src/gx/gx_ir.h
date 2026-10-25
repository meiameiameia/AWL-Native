// GX intermediate representation (IR): the interface between the GX core (command processor, XF on the CPU, BP/TEV decode,
// texture decoding; src/gx, this track) and a renderer backend (src/gfx, DirectX 11; or the recording backend). DRAFT 1 for
// the architect's review; once frozen, changes are requested from the architect.
//
// The core turns the CP command stream (FIFO and called display lists) into a sequence of commands, delivered in stream
// order to a Backend:
//   * Draw     - one GX primitive command, already converted to a triangle, line or point list, with post-transform
//                vertices and the complete decoded pixel-pipeline state that applies to it;
//   * EfbCopy  - one EFB copy (to an external frame buffer, XFB, or to a texture), with the optional clear.
// The core performs the whole XF stage (matrices, projection, lighting channels, texture-coordinate generation) so that no
// backend needs CP/XF knowledge. The backend implements the rasterizer, TEV, texture units and pixel engine.
//
// ===== Conventions ==============================================================================================
// Sources. YAGCD = "Yet Another GameCube Documentation" chapter 5 (https://hitmen.c02.at/files/yagcd/yagcd/chap5.html),
// libogc = devkitPro libogc gc/ogc/gx.h and libogc/gx.c (the SDK-compatible API that writes these registers), and the
// game itself (docs/research/gx-core.md, "Observed"). Dolphin (GPL) was read only to confirm understanding; nothing is copied.
//
// 1. Clip space (Vertex::pos). Exactly what the XF unit outputs: (x, y, z, w) = projection x view-space position, before the
//    perspective divide and before any clipping. GX does NOT use the D3D/OpenGL depth convention: the visible volume is
//      -w <= x <= w,   -w <= y <= w,   -w <= z <= 0     (z/w = -1 at the near plane, 0 at the far plane).
//    Evidence: libogc GX_LoadProjectionMtx writes the matrix elements to XF 0x1020-0x1026 unmodified; the perspective
//    projection of libogc (guPerspective) has m22 = -n/(f-n), m23 = -f*n/(f-n), which gives z/w = -1 at z=-n and 0 at z=-f;
//    the game's own orthographic projection (observed: A=2/640, C=-2/448, E=1/1024, F=-1 -> z/w = z/1024 - 1) lies in the
//    same range. YAGCD (XF projection, viewport) gives the formulas. Direct conversion to D3D: z_d3d = z + w (so that
//    z_d3d/w = z/w + 1 in [0,1]) with the D3D viewport depth range set to [Viewport::nearZ, Viewport::farZ]; this equals
//    the hardware mapping below. w can be <= 0; clipping against the volume above is the backend's job (the core does not
//    clip, and does not divide). Triangles are never culled by the backend (see 6).
// 2. Viewport and depth (Viewport). Window coordinates: EFB pixels, origin top-left, x right, y DOWN, one unit = one EFB pixel.
//      x_win = (x/w) * scale.x + offset.x - 342,   y_win = (y/w) * scale.y + offset.y - 342,
//      depth = (z/w) * scale.z + offset.z   (in units of 1/16777215; 0 = near, 1 = far after dividing by 16777215)
//    with libogc GX_SetViewport: scale = (width/2, -height/2, (far-near)*16777215), offset = (x0+width/2+342, y0+height/2+342,
//    far*16777215). The 342 is the hardware's window offset and is already removed from Viewport::x/y. The core delivers both
//    the decoded rectangle/range and the raw XF scale/offset. Hardware depth is a 24-bit integer (EFB Z24); a float depth
//    buffer is an accepted native difference.
// 3. Colors. Vertex::color[0..1] are the lit channel results, RGBA, 8 bits per component (the XF channel output is 8-bit),
//    not premultiplied, no sRGB. Channel i is only meaningful when i < PipelineState::numColorChannels; unused channels are
//    zero. Vertex colors from the stream (RGB565, RGBA4444, RGBA6666, ...) are expanded to 8 bits by bit replication
//    exactly as the hardware does.
// 4. Texture coordinates. Vertex::tex[i] = (s, t, q) of texgen unit i after the texgen matrix, the dual-transform (post)
//    matrix and normalisation, as XF outputs them; q = 1 for 2x4 (ST) texgens. Valid for i < PipelineState::numTexCoords.
//    The units are "normalized": the texture unit multiplies s/q and t/q by TexCoordScale (SU_SSIZE/SU_TSIZE, = the size of the
//    texture in the normal case) to get texel coordinates, with the texel centre at +0.5 and the top-left texel at (0,0).
//    TEV stages select a texcoord set with TevStage::texCoord. (libogc __SetSURegs, GX_SetTexCoordScaleManually; YAGCD
//    SU_SSIZE/SU_TSIZE.) The game's logo draws use s,t in [0,1] with scale == texture size (observed).
// 5. Winding and primitive conversion. The core converts quads, strips and fans to triangle lists in the stream order, with
//    the winding the hardware gives them (strip triangles alternate; the core flips every second triangle of a strip so that
//    all triangles of a primitive have the winding of its first triangle). Lines/line strips become line lists (pairs),
//    points a point list. The vertices are shared through indices (Draw::indices).
// 6. Culling. The core performs face culling (GEN_MODE cull, libogc GX_SetCullMode: hardware 1 = cull back faces, 2 = cull
//    front faces, 3 = cull all, mapping {0,2,1,3}) with the sign of the clip-space determinant of (x, y, w), which is
//    invariant to clipping and valid for w <= 0, and drops culled triangles. Front-facing is counter-clockwise in NDC with x
//    right and y up (libogc documents CCW as the front face). The IR therefore contains only triangles to be drawn, and a
//    backend must disable its own culling. State::cull is informational.
// 7. Textures. Every bound texture is decoded by the core to RGBA8, 4 bytes per texel in R,G,B,A byte order, rows top to
//    bottom, followed by the mip levels present (Texture::levels). I4/I8/IA4/IA8 expand as the hardware does (I replicates
//    into RGBA, alpha = I for I formats), color-index formats are resolved through the TLUT at bind time. Texture::key is a
//    64-bit content hash (format, size, mip count, texel bytes, TLUT bytes and format) that the backend may cache on; two
//    equal keys are the same image. Texture objects are immutable, owned by shared_ptr, and may be retained by the backend.
// 8. EFB copy textures. An EFB copy to a texture does not write guest memory. When a texture map is later bound to memory
//    that an earlier copy-to-texture wrote (same address, covering the bound size, destination bytes unchanged by the CPU
//    since), the binding references that copy by EfbCopyCommand::id instead of carrying texels; the backend converts the
//    copy's pixels to the copy's destination format when it executes the copy and samples them under the binding's
//    sampler. Copies to the XFB write nothing to guest memory either; the VI model finds the copy by its physical address.
// 9. Ordering and lifetime. Commands are delivered synchronously from the emulation thread in the order the GP executes them.
//    Everything a command points to (vertices, indices, the PipelineState) is valid only for the duration of the call
//    except objects held by shared_ptr, which the backend may keep. PipelineState objects are shared between consecutive
//    draws while the state is unchanged: equal Draw::stateId means identical state (a cache key for pipeline objects).
// 10. Failure. The core stops loudly (stream offset and register/command) for anything it does not implement: this header
//    marks each feature Observed (seen in the game's stream by docs/research/gx-core.md), Defined (a documented hardware
//    feature the core will implement but the survey has not seen, so it has no real stream to validate against) or Rejected
//    (the core stops with a diagnostic).
//
// Coverage (v1)                                Status
//   vertex formats: direct/indexed, all comps  Observed: direct s16 xy/xyz, rgba8888, f32 st; the rest Defined
//   lighting channels                          Observed: 1 channel, no lights; lights/specular Defined
//   texgen                                     Observed: ST regular from tex0 with post matrix; STQ, normal/pos source Defined,
//                                              emboss/color texgen Defined (core may reject)
//   TEV 1-16 stages, konst, swap tables        Observed: 1 stage, default swap tables; the rest Defined
//   indirect texturing                         Defined (not observed)
//   alpha compare, z, blend, dither, update    Observed (one config: blend src/inv-src alpha, z lequal); logic ops Defined
//   fog                                        Defined (disabled in the survey)
//   texture formats                            Observed: RGB5A3; I4 I8 IA4 IA8 RGB565 RGBA8 C4 C8 C14X2 CMPR Defined
//   EFB copy to XFB with clear                 Observed (vertical filter on, 2:1 off); copy to texture Defined (not observed)
//   z-texture, YUV/Y8/U8/V8 EFB formats        Rejected
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace awl::gx {

constexpr uint32_t kIrVersion = 1;

// ---- Geometry -----------------------------------------------------------------------------------------------------
struct Vertex {
    float pos[4];            // clip space (x, y, z, w), convention 1
    uint8_t color[2][4];     // lit RGBA of channel 0 and 1, convention 3
    float tex[8][3];         // (s, t, q) of texgen 0..7, convention 4
};                           // 120 bytes

enum class Primitive : uint8_t { Triangles, Lines, Points };

// ---- Fixed-function enumerations (values are the hardware encodings used by libogc) ---------------------------------
enum class CompareFunc : uint8_t { Never, Less, Equal, LEqual, Greater, NotEqual, GEqual, Always };
enum class PixelFormat : uint8_t { RGB8_Z24, RGBA6_Z24, RGB565_Z16, Z24 };   // Y8/U8/V8/YUV420 are Rejected
enum class ZFormat : uint8_t { Linear, Near, Mid, Far };                       // 16-bit Z compression curve (RGB565_Z16 only)
enum class BlendMode : uint8_t { None, Blend, Subtract, Logic };
// For the source factor value 2 means the destination color, for the destination factor it means the source color
// (libogc GX_BL_SRCCLR == GX_BL_DSTCLR).
enum class BlendFactor : uint8_t { Zero, One, OtherColor, InvOtherColor, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha };
enum class LogicOp : uint8_t { Clear, And, RevAnd, Copy, InvAnd, NoOp, Xor, Or, Nor, Equiv, Inv, RevOr, InvCopy, InvOr, Nand, Set };
enum class AlphaLogic : uint8_t { And, Or, Xor, Xnor };

struct Viewport {
    float x, y, width, height;     // EFB pixels, origin top-left, y down (convention 2); 342 offset removed
    float nearZ, farZ;             // 0..1
    float scale[3], offset[3];     // raw XF 0x101A..0x101F (convention 2)
};
struct Scissor { int32_t x0, y0, x1, y1; };   // inclusive EFB pixel rectangle; BP scissor minus the box offset, clamped to the EFB

struct DepthState {
    bool test;                     // PE_ZMODE enable
    CompareFunc func;
    bool write;
    bool compareBeforeTexture;     // PE_CONTROL "z compare location": depth test/update happens before the TEV (early z)
    ZFormat zFormat;
};
struct BlendState {
    BlendMode mode;
    BlendFactor src, dst;          // used for Blend
    LogicOp logicOp;               // used for Logic
    bool colorUpdate, alphaUpdate; // write masks of the EFB color and alpha
    bool dither;
    bool dstAlphaEnable;           // PE_CMODE1: the alpha written to the EFB is dstAlpha instead of the TEV alpha
    uint8_t dstAlpha;
};
struct AlphaCompare {              // pass = (a OP0 ref0) logic (a OP1 ref1); TEV_ALPHAFUNC. Always/Always means no test.
    CompareFunc comp0, comp1;
    uint8_t ref0, ref1;
    AlphaLogic logic;
};

// ---- Fog ----------------------------------------------------------------------------------------------------------
// TEV_FOG_PARAM0-3 / FOG_COLOR and the range adjustment (libogc GX_SetFog, GX_SetFogRangeAdj; YAGCD). Defined, not observed.
enum class FogType : uint8_t { None = 0, Linear = 2, Exp = 4, Exp2 = 5, RevExp = 6, RevExp2 = 7 };   // FSEL field; projection is `orthographic`
struct FogState {
    FogType type;
    bool orthographic;             // FOG_PARAM_3 PROJ bit: fog computed from the orthographic z
    float a, b, c;                 // FOG_PARAM_0, FOG_PARAM_1/2 (b_mag, b_shift) and FOG_PARAM_3 decoded to floats by the core (inverse of libogc GX_SetFog)
    uint8_t color[3];
    bool rangeAdjustEnable;
    uint16_t rangeAdjustCenter;    // EFB x centre (342 removed)
    uint16_t rangeAdjustTable[10]; // 12-bit entries of FOG_RANGE_ADJ 0xE9-0xED
};

// ---- Textures -----------------------------------------------------------------------------------------------------
enum class WrapMode : uint8_t { Clamp, Repeat, Mirror };
// GX_NEAR, GX_LINEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN
enum class FilterMode : uint8_t { Near, Linear, NearMipNear, LinMipNear, NearMipLin, LinMipLin };

struct TextureLevel { uint16_t width, height; uint32_t offsetTexels; };   // offset into Texture::rgba in texels
struct Texture {
    uint64_t key;                  // convention 7
    uint16_t width, height;        // level 0
    uint8_t sourceFormat;          // GX texture format number (GX_TF_*), informational
    uint8_t levelCount;            // >= 1
    TextureLevel levels[11];
    std::vector<uint32_t> rgba;    // texels as bytes R,G,B,A (little-endian uint32 = A<<24|B<<16|G<<8|R), levels back to back
};

struct Sampler {                   // TX_SETMODE0/1
    WrapMode wrapS, wrapT;
    FilterMode minFilter;
    bool magLinear;
    float lodBias;                 // signed, in LOD units (register / 32)
    float minLod, maxLod;          // register / 16
    uint8_t maxAnisotropy;         // 1, 2 or 4
    bool edgeLod, biasClamp;
};
struct TexCoordScale {             // SU_SSIZE/SU_TSIZE of texcoord set i
    uint16_t sSize, tSize;         // register + 1 (normally the texture size); texel = (s/q) * sSize
    bool rangeBiasS, rangeBiasT, cylWrapS, cylWrapT;
    bool lineOffset, pointOffset;
};
struct TexBinding {                // one of the 8 texture maps
    bool valid;
    std::shared_ptr<const Texture> image;   // null when the map is backed by an EFB copy
    uint32_t efbCopyId;            // nonzero: the map reads the destination of this earlier EfbCopyCommand (convention 8)
    uint16_t width, height;        // level 0 size from TX_SETIMAGE0 (also valid for EFB-copy maps)
    Sampler sampler;
};

// ---- TEV ----------------------------------------------------------------------------------------------------------
enum class TevColorIn : uint8_t { PrevC, PrevA, C0, A0, C1, A1, C2, A2, TexC, TexA, RasC, RasA, One, Half, Konst, Zero };
enum class TevAlphaIn : uint8_t { PrevA, A0, A1, A2, TexA, RasA, Konst, Zero };
// out = clamp?(((d op ((1-c)*a + c*b)) + bias) * scale) for Add/Sub; the 8 compare ops (libogc GX_TEV_COMP_*) ignore bias and
// scale: out = d + (a CMP b ? c : 0) on R8, GR16, BGR24 or per-component RGB8 (alpha: A8 compare for the *_RGB8 forms).
enum class TevOp : uint8_t { Add = 0, Sub = 1, CompR8GT = 8, CompR8EQ, CompGR16GT, CompGR16EQ, CompBGR24GT, CompBGR24EQ, CompRGB8GT, CompRGB8EQ };
enum class TevBias : uint8_t { Zero, PlusHalf, MinusHalf };
enum class TevScale : uint8_t { X1, X2, X4, Half };
enum class TevDest : uint8_t { Prev, Reg0, Reg1, Reg2 };

struct TevCombine {
    uint8_t in[4];                 // a, b, c, d as TevColorIn (color) or TevAlphaIn (alpha)
    TevOp op;
    TevBias bias;
    TevScale scale;
    bool clamp;
    TevDest dest;
};
// Rasterized color source of a stage (RAS1_TREF color channel field).
enum class RasColor : uint8_t { Color0A0 = 0, Color1A1 = 1, AlphaBump = 5, AlphaBumpNormalized = 6, Zero = 7 };
constexpr uint8_t kNoMap = 0xFF;

struct IndirectOp {                // IND_CMD of the TEV stage; indStage == kNoMap means the stage has no indirect operation
    uint8_t indStage;              // 0..3, which indirect texture lookup feeds this stage
    uint8_t format;                // 0 = 8-bit, 1 = 5-bit, 2 = 4-bit, 3 = 3-bit offsets
    uint8_t biasMask;              // bit0 = S, bit1 = T, bit2 = U: add the format bias to the component
    uint8_t alphaSelect;           // 0 = off, 1..3 = S/T/U used as bump alpha
    uint8_t matrix;                // 0 none, 1..3 = matrix 0..2, 5..7 = S matrices 0..2, 9..11 = T matrices 0..2
    uint8_t wrapS, wrapT;          // 0 = off, 1..5 = 256, 128, 64, 32, 16, 6 = 0
    bool addPrevious;              // add the previous stage's texcoord
    bool useOriginalLod;           // use the unmodified texcoord for the LOD computation
};
struct TevStage {
    TevCombine color, alpha;
    uint8_t texMap, texCoord;      // kNoMap when the stage samples no texture
    RasColor ras;
    uint8_t konstColor, konstAlpha;   // TEV_KSEL selectors, libogc GX_TEV_KCSEL_*/KASEL_* numbering (0x00..0x1F)
    uint8_t rasSwap, texSwap;      // swap table 0..3 (alpha env rswap/tswap)
    IndirectOp indirect;
};
struct IndirectStage {             // indirect texture lookups (RAS1_IREF, RAS1_SS0/SS1)
    uint8_t texMap, texCoord;
    uint8_t scaleS, scaleT;        // RAS1_SS0/SS1 codes 0..8: the indirect offset is scaled by 1 / (1 << code)
};
struct TevState {
    uint8_t numStages;             // 1..16
    TevStage stages[16];
    int16_t reg[4][4];             // prev, reg0, reg1, reg2 as RGBA, signed 11-bit (-1024..1023)
    uint8_t konst[4][4];           // KONST colors 0..3, RGBA
    uint8_t swap[4][4];            // swap tables: for table t, output component r,g,b,a takes input component swap[t][c] (0=r..3=a)
    uint8_t numIndirect;           // 0..4
    IndirectStage indirectStage[4];
    float indirectMatrix[3][2][3]; // IND_MTXA/B/C: rows (s,t) x columns (3), with the matrix scale exponent applied
};

// ---- Per-draw pipeline state --------------------------------------------------------------------------------------
struct PipelineState {
    PixelFormat efbFormat;
    uint8_t numColorChannels;      // 0..2
    uint8_t numTexCoords;          // 0..8 texgen units
    Viewport viewport;
    Scissor scissor;
    DepthState depth;
    BlendState blend;
    AlphaCompare alphaCompare;
    FogState fog;
    TevState tev;
    TexBinding maps[8];
    TexCoordScale texScale[8];
    uint8_t lineWidth, pointSize;  // 1/6 pixel units (SU_LPSIZE); lines/points are expanded to quads of this size by the backend
    uint8_t lineTexOffset, pointTexOffset;
    uint8_t cullMode;              // GEN_MODE cull as libogc hardware value (informational: culling is already applied, convention 6)
};

// ---- Commands -----------------------------------------------------------------------------------------------------
struct DrawCommand {
    uint32_t seq;                  // running number of the draw, for diagnostics
    uint32_t stateId;              // equal ids = identical PipelineState (convention 9)
    const PipelineState* state;
    Primitive primitive;
    const Vertex* vertices;
    uint32_t vertexCount;
    const uint16_t* indices;       // Triangles: groups of 3, Lines: pairs, Points: singles
    uint32_t indexCount;
};

enum class CopyTarget : uint8_t { Xfb, Texture };
struct ClearSpec {                 // applied after the copy; each component only if its EFB update mask is on (convention in the note)
    bool color, alpha, z;
    uint8_t rgba[4];
    uint32_t z24;
};
struct EfbCopyCommand {
    uint32_t id;                   // 1,2,3,...; referenced by TexBinding::efbCopyId
    CopyTarget target;
    uint32_t srcX, srcY, srcWidth, srcHeight;   // EFB rectangle, pixels
    uint32_t destAddr;             // physical byte address in main memory (XFB or texture buffer)
    uint32_t destWidth, destHeight;// size of the result: XFB: srcWidth x srcHeight*yScale; texture: srcWidth/srcHeight, halved if halfScale
    PixelFormat efbFormat;         // EFB format at the time of the copy
    // XFB copy
    float yScale;                  // 1.0 = none (BP 0x4E: 256/reg)
    uint8_t gamma;                 // 0 = 1.0, 1 = 1.7, 2 = 2.2
    uint8_t vfilter[7];            // vertical (deflicker) filter coefficients, 6 bits each, sum 64; observed {8,8,10,12,10,8,8}
    uint32_t sampleBits;           // BP 0x01-0x04 sample pattern words (observed: default 0x666666)
    bool frameToField;
    // Texture copy
    uint8_t texFormat;             // libogc GX_TF_*/GX_CTF_*/GX_TF_Z* number of the destination format
    bool halfScale, mipmap, clampTop, clampBottom;
    bool intensity;                // convert to intensity for I/IA formats
    ClearSpec clear;
};

// ---- Backend ------------------------------------------------------------------------------------------------------
enum class SyncKind : uint8_t { DrawDone, Token, TokenInterrupt };

class Backend {
public:
    virtual ~Backend() = default;
    virtual void draw(const DrawCommand& d) = 0;
    virtual void efbCopy(const EfbCopyCommand& c) = 0;
    // Optional hooks; the platform model completes the finish/token services itself, a backend may use these as flush points.
    virtual void sync(SyncKind, uint16_t /*token*/) {}
    // Called when the first command of a new retrace interval arrives (retrace counter of the VI model); for per-frame dumps.
    virtual void frameBoundary(uint32_t /*retrace*/) {}
};

}  // namespace awl::gx
