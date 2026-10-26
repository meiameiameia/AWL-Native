// The vertex engine of the GX core: vertex loading from the command stream and the CP arrays, the XF stage (position and
// normal matrices, projection, lighting channels, texture-coordinate generation) and primitive conversion with face culling.
// Formulas and register layouts: YAGCD chapter 5 and the libogc API documentation (docs/research/gx-core.md).
//
// Speed structure: everything that cannot change inside one draw command is decoded once (Plan per vertex format, DrawCtx per
// register epoch); common vertex formats get specialised attribute loaders selected once per format (function pointers to
// template instances), the rest goes through a generic loader; the per-vertex arithmetic uses SSE2 intrinsics that perform the same
// IEEE operations in the same order as the scalar reference, so results are bit-identical to it.
#pragma once

#include <smmintrin.h>   // SSE4.1 baseline

#include <memory>

#ifdef _MSC_VER
#pragma warning(disable : 4324)   // structure padded for the __m128 members
#endif

#include "gx/gx_common.h"
#include "gx/gx_ir.h"

namespace awl::gx {

class VertexEngine {
public:
    struct Result {
        const Vertex* vertices = nullptr;
        uint32_t vertexCount = 0;
        const uint16_t* indices = nullptr;
        uint32_t indexCount = 0;
        Primitive primitive = Primitive::Triangles;
        uint32_t culledTriangles = 0;
        bool batched = false;                       // the 4-wide path ran for this draw
    };

    VertexEngine(const GuestMemory& mem, const CpState& cp, const XfState& xf);

    // Register-write notifications (the engine caches what it decodes from them).
    void cpWritten() { ++cpEpoch_; }
    void xfWritten(uint32_t addr, uint32_t count);

    // Bytes of one vertex of format `vat` (0 = empty descriptor).
    uint32_t vertexSize(uint32_t vat);
    // Loads and transforms `count` vertices of format `op & 7` from `data`, converts them into the primitive `op & 0xF8` and
    // culls (cullMode: GEN_MODE hardware value). Throws GxError (offset 0) for unsupported state.
    void run(uint8_t op, const uint8_t* data, uint32_t count, uint32_t cullMode, Result& out);

    struct AttrFmt { uint32_t mode, cnt, fmt, shift; };
    struct VtxFmt {
        bool pnmtx = false, texmtx[8] = {};
        AttrFmt pos{}, nrm{}, col[2] = {}, tex[8] = {};
        bool nrmIdx3 = false;
        uint32_t size = 0;
    };
    struct RawVertex {
        uint32_t pnIdx, texIdx[8];
        float pos[3], nrm[3][3];                   // normal, binormal, tangent (viewed as n, b, t)
        bool hasNrm;
        uint8_t col[2][4];
        bool hasCol[2];
        float tex[8][2];
    };
    struct Cols { __m128 c0, c1, c2, c3; };         // a 3x4 matrix as columns (rows in lanes 0-2)
    struct LightObj {
        float color[4], a[3], k[3], pos[3], dir[3];
        bool constAttn;                             // a1 = a2 = k1 = k2 = 0
        float attn0;                                // max(0, a0) / k0 when constAttn
    };
    struct TexGenCtx {
        uint32_t type, src, form;
        bool stq, dual, normalize, postIdentity;   // postIdentity: the post matrix is exactly the identity (+0 elements)
        uint32_t postIdx;
        int idx;                                    // texture matrix currently cached in m (-1: none)
        Cols m, post;
    };
    struct ChanCfg {                                // one lighting channel decoded from the XF registers
        uint32_t ctrlC, ctrlA;                      // control words of the color and the alpha half
        bool litC, litA, matVtxC, matVtxA, ambVtxC, ambVtxA, same;   // lighting on, material/ambient from the vertex, both halves identical
        int mat[4], amb[4];                         // material and ambient registers (r, g, b, a)
        uint32_t nC, nA;                            // enabled lights of each half
        uint8_t idxC[8], idxA[8];
    };
    // Four vertices of a fast plan in structure-of-arrays form (lane = vertex): what the specialised loaders write and the batch XF reads.
    struct alignas(16) RawBatch {
        float pos[3][4], nrm[3][4], tex[2][4];
        int32_t col[4][4];
    };
    struct DrawCtx;
    // Loads one attribute of up to four consecutive vertices: p is the attribute in the first vertex, `stride` the vertex size.
    using AttrFn = void (*)(const DrawCtx&, const uint8_t* p, uint32_t stride, uint32_t n, RawBatch&);
    struct Plan {
        uint32_t epoch = 0;
        uint32_t key[29];                           // the register values the plan was built from
        VtxFmt f;
        bool fast = false;
        AttrFn pos = nullptr, nrm = nullptr, col0 = nullptr, tex0 = nullptr;
        uint32_t nrmOff = 0, colOff = 0, texOff = 0;   // byte offsets of the attributes inside a vertex (position at 0)
        const uint8_t* win[12] = {};                // windows into the CP arrays reachable by an 8/16-bit index (null: none)
        uint32_t winBytes[12] = {};
        uint32_t stride[12] = {};
        float posScale = 1, texScale[8] = {};
    };
    struct DrawCtx {
        // CP side (copied from the plan)
        const uint8_t* win[12];
        uint32_t winBytes[12];
        uint32_t stride[12];
        float posScale, texScale[8];
        // XF registers
        __m128 projA, projB;                        // (P0, P2, P4, .) and (P1, P3, P5, .)
        float proj[6];
        bool ortho, lit, nmValid;
        uint32_t numColors, numTexGens;
        ChanCfg chan[2];
        uint32_t pnDefault, texDefault[8];
        TexGenCtx tg[8];
        // lights and matrices
        LightObj lights[8];
        uint32_t lightLoaded;
        int pnIdx;
        Cols pm;
        Cols nmc;                                   // normal matrix columns
        // batch path (4 vertices per SSE register): broadcast matrix elements, valid while the matrix index is unchanged
        struct LightB { __m128 pos[3], attn0, color[4]; bool nonNeg; };   // broadcast copies for the batch path (nonNeg: attn0 >= 0)
        LightB lightb[8];
        __m128 projb[6];
        bool batchOk;                               // fast plan, lights and texgens within what the batch path implements
        __m128 pmb[12], nmb[9], tmb[8][12], postb[8][12];
        int tmbIdx[8], pmbIdx, nmbIdx;
    };

private:
    Plan& plan(uint32_t vat);
    void beginDraw(const Plan& p);
    const LightObj& light(uint32_t i);
    [[noreturn]] void fail(const std::string& what, uint32_t code) const;
    VtxFmt vertexFormat(uint32_t vat) const;
    const uint8_t* elem(const AttrFmt& a, uint32_t array, const uint8_t*& p, uint32_t directBytes, uint32_t elemBytes);
    void decodeGeneric(const VtxFmt& f, const uint8_t* p, RawVertex& r);
    void transform(const RawVertex& r, Vertex& out);
    void transformBatch(const RawBatch& b, bool hasNrm, Vertex* out);
    void lightChannels(const RawVertex& r, const float vpos[3], const float vnrm[3], uint8_t color[2][4]);
    void texGen(const RawVertex& r, const uint8_t color[2][4], Vertex& out);

    const GuestMemory& mem_;
    const CpState& cp_;
    const XfState& xf_;
    bool scalarOnly_ = false;                       // AWL_GX_SCALAR=1: bypass the specialised loaders and the batch path (verification)
    uint32_t cpEpoch_ = 1;
    bool regsDirty_ = true, postDirty_ = true, lightsDirty_ = true;
    Plan plans_[8];
    Plan* fresh_ = nullptr;                         // the plan vertexSize() just validated for the draw about to run
    DrawCtx dc_{};
    std::unique_ptr<Vertex[]> verts_;
    std::unique_ptr<uint16_t[]> idx_;
};

}  // namespace awl::gx
