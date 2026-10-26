// GX core: the command processor of the GP. It consumes the bytes the CPU writes to the write-gather port (the CP
// command stream, including called display lists), keeps the CP/XF/BP register state, performs vertex loading and the XF
// stage on the CPU, decodes the pixel pipeline and textures, and delivers renderer-independent commands (gx_ir.h) to a
// Backend. Register layouts: YAGCD chapter 5 and libogc gx.h/gx.c (docs/research/gx-core.md lists what each convention rests on).
//
// The core knows nothing about the platform: guest memory comes through GuestMemory, completion services (finish, token,
// copies) are reported to a CoreHost, and unsupported features throw GxError with the stream offset (dev_gx.cpp turns it into a
// stop of the run).
#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "gx/gx_ir.h"

namespace awl::gx {

class GxError : public std::runtime_error {
public:
    GxError(const std::string& what, uint64_t offset, uint32_t code) : std::runtime_error(what), streamOffset(offset), code(code) {}
    uint64_t streamOffset;   // bytes of command stream consumed before the failing command (display-list bytes not counted)
    uint32_t code;           // opcode, register number or address that failed
};

// Guest memory by physical address (0 .. 0x01FFFFFF is MEM1). Returns null when [addr, addr+size) is not readable.
class GuestMemory {
public:
    virtual ~GuestMemory() = default;
    virtual const uint8_t* read(uint32_t physAddr, uint32_t size) const = 0;
};

// Services the platform completes for the stream; all are called in stream order from feed().
class CoreHost {
public:
    virtual ~CoreHost() = default;
    virtual void drawDone() {}                              // BP PE_DONE (GXSetDrawDone / finish)
    virtual void token(uint16_t /*value*/, bool /*interrupt*/) {}   // BP PE_TOKEN / PE_TOKEN_INT
    virtual void efbCopyIssued(CopyTarget) {}               // BP EFB copy trigger
    virtual void log(const char* /*message*/) {}            // diagnostics (once-per-event notes)
};

// Observes the stream as the core consumes it (capture tooling): state-setting commands (CP, direct XF and BP loads, one whole
// command each) and the display lists the stream calls, in call order.
class StreamObserver {
public:
    virtual ~StreamObserver() = default;
    virtual void displayList(uint32_t /*addr*/, uint32_t /*size*/, const uint8_t* /*data*/) {}
    virtual void command(uint8_t /*op*/, const uint8_t* /*data*/, size_t /*n*/) {}
};

struct CoreStats {
    uint64_t commands = 0, draws = 0, vertices = 0, triangles = 0, copies = 0;
    uint64_t textureDecodes = 0, textureCacheHits = 0, textureBytesHashed = 0, textureBytesCompared = 0;
    uint64_t pipelineRebuilds = 0, culledTriangles = 0, displayLists = 0;
    double busySeconds = 0;       // wall time spent inside feed()
    double pipelineSeconds = 0;   // of which: pipeline rebuilds (texture binding, hashing and decoding included)
    double vertexSeconds = 0;     // of which: vertex loading, XF and primitive conversion
    double backendSeconds = 0;    // of which: time inside the backend (draw and copy calls)
};

// ---- Register state (inspectable for tests and tools) -------------------------------------------------------------
struct CpState {
    uint32_t vcdLo = 0, vcdHi = 0;
    uint32_t vat[3][8] = {};              // VAT A, B, C of the 8 vertex formats
    uint32_t arrayBase[16] = {};          // 0xA0-0xAF: pos, nrm, col0, col1, tex0-7, XF index arrays A-D (physical)
    uint32_t arrayStride[16] = {};
    uint32_t matIdxA = 0, matIdxB = 0;    // CP 0x30 / 0x40 (mirrors of XF 0x1018 / 0x1019)
};
struct XfState {
    uint32_t mem[0x800] = {};             // 0x000-0x0FF position/texture matrices, 0x400 normal matrices, 0x500 post matrices, 0x600 lights
    uint32_t reg[0x60] = {};              // XF registers 0x1000-0x105F
};

// 64-bit content hash used for texture keys and copy-destination checks.
uint64_t gxHash64(const uint8_t* p, size_t n, uint64_t seed);

class Core {
public:
    struct LightObj { float color[4], a[3], k[3], pos[3], dir[3]; };   // XF light memory 0x600 + 16 * i, decoded

    Core(const GuestMemory& mem, Backend& backend, CoreHost& host);
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Bytes in write-gather order; commands may span calls. Throws GxError.
    void feed(const uint8_t* data, size_t n);
    // Retrace counter of the VI model; the backend's frameBoundary is called when it changes before the next command.
    void setRetrace(uint32_t retrace) { retrace_ = retrace; }
    void setObserver(StreamObserver* o) { obs_ = o; }
    bool midCommand() const { return !pending_.empty(); }   // a command is split across feed() calls

    const CoreStats& stats() const { return stats_; }
    const CpState& cp() const { return cp_; }
    const XfState& xf() const { return xf_; }
    uint32_t bp(uint32_t reg) const { return bp_[reg & 0xFF]; }

private:
    // ---- gx_core.cpp: stream ----
    size_t parse(const uint8_t* d, size_t n, int depth);
    [[noreturn]] void fail(const std::string& what, uint32_t code) const;
    void cpLoad(uint32_t reg, uint32_t value);
    void xfLoad(uint32_t addr, uint32_t count, const uint8_t* words);
    void xfIndexed(uint32_t op, uint32_t word);
    void bpLoad(uint32_t word);
    void bpWrite(uint32_t reg, uint32_t value);
    void callList(uint32_t addr, uint32_t size, int depth);

    // ---- gx_draw.cpp: vertices, XF, primitives ----
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
    struct TexGenCtx {
        uint32_t type, src, form;
        bool stq, dual, normalize;
        int idx;                 // texture matrix currently cached in m (-1: none)
        float m[12], post[12];
    };
    struct DrawCtx {            // everything that is constant while one draw command runs
        float proj[6];
        bool ortho, lit, nmValid;
        uint32_t numColors, numTexGens;
        uint32_t chan[2][2];    // channel control words [channel][color, alpha]
        LightObj lights[8];
        uint32_t lightLoaded;
        int pnIdx;
        float pm[12], nm[9];
        TexGenCtx tg[8];
        const uint8_t* win[16];
        uint32_t winBytes[16];
        float posScale, texScale[8];
    };
    void beginDraw(const VtxFmt& f);
    const LightObj& light(uint32_t i);
    VtxFmt vertexFormat(uint32_t vat) const;
    bool draw(uint8_t op, const uint8_t* d, size_t n, size_t& used);
    const uint8_t* elem(const AttrFmt& a, uint32_t array, const uint8_t*& p, uint32_t directBytes, uint32_t elemBytes);
    void decodeVertex(const VtxFmt& f, const uint8_t* p, RawVertex& r);
    void transformVertex(const RawVertex& r, Vertex& out);
    void lightChannels(const RawVertex& r, const float vpos[3], const float vnrm[3], uint8_t color[2][4]);
    void texGen(const RawVertex& r, const uint8_t color[2][4], Vertex& out);

    // ---- gx_pipeline.cpp: BP to IR state, EFB copies ----
    void rebuildPipeline();
    void buildTev(TevState& t) const;
    void bindMap(uint32_t map, TexBinding& b);
    void efbCopy();

    // ---- gx_texture.cpp: texture decoding and cache ----
    struct TexRequest {
        uint32_t addr, width, height, format, levels, tlutOffset, tlutFormat;
    };
    std::shared_ptr<const Texture> loadTexture(const TexRequest& req);
    void tlutLoad();

    // ---- state ----
    const GuestMemory& mem_;
    Backend& backend_;
    CoreHost& host_;
    CpState cp_;
    XfState xf_;
    uint32_t bp_[256] = {};
    uint32_t bpMask_ = 0xFFFFFF;
    uint32_t tevColor_[2][4][2] = {};       // [type: 0 color, 1 konst][slot][L, H] BP 0xE0-0xE7 data (bits 0-22)
    std::vector<uint8_t> pending_;
    uint64_t offset_ = 0;                   // stream offset of the command being processed
    uint64_t streamBase_ = 0;               // stream offset of pending_[0]
    uint32_t retrace_ = 0, frameSeen_ = 0xFFFFFFFFu;
    CoreStats stats_;
    StreamObserver* obs_ = nullptr;

    std::shared_ptr<const PipelineState> pipe_;
    uint32_t stateId_ = 0;
    bool dirty_ = true;
    uint32_t drawSeq_ = 0, copySeq_ = 0;
    uint32_t texGeneration_ = 0;            // incremented by TX_INVALIDATE / preload: the texture cache's validity epoch
    std::vector<Vertex> verts_;
    std::vector<uint16_t> idx_;
    DrawCtx dc_{};

    // Texture decode cache, keyed by the content hash.
    struct CacheEntry { std::shared_ptr<const Texture> tex; uint64_t bytes; };
    std::unordered_map<uint64_t, CacheEntry> texCache_;
    std::list<uint64_t> texLru_;
    uint64_t texCacheBytes_ = 0;
    // Raw bytes of the last decode per texture binding, for the exact unchanged-content check.
    struct Snapshot { bool valid = false; uint32_t tuple[8] = {}; std::vector<uint8_t> raw; std::vector<uint16_t> pal; uint64_t key = 0; };
    std::unordered_map<uint64_t, Snapshot> snapshots_;
    uint64_t snapBytes_ = 0;
    // Memoised bindings per map (valid while the registers and the epoch are unchanged).
    struct MapMemo { uint32_t regs[8]; uint32_t epoch; uint32_t tlutEpoch; TexBinding binding; bool valid = false; };
    MapMemo memo_[8];
    std::vector<uint16_t> tlutRam_;         // TMEM TLUT area as 16-bit entries (offset unit: 256 entries)
    uint32_t tlutEpoch_ = 0;

    // EFB copies to textures that later binds may alias (convention 8).
    struct CopyRecord { uint32_t id, addr, bytes, format, width, height; uint64_t memHash; bool warned; };
    std::vector<CopyRecord> copies_;
    friend struct CoreTestAccess;
};

}  // namespace awl::gx
