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

#include "gx/gx_common.h"
#include "gx/gx_ir.h"
#include "gx/gx_vertex.h"

namespace awl::gx {

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
    uint64_t textureDecodes = 0, textureCacheHits = 0, textureBytesHashed = 0, textureBytesCompared = 0, textureTrusted = 0;
    uint64_t pipelineRebuilds = 0, culledTriangles = 0, displayLists = 0, batchedDraws = 0;
    double busySeconds = 0;       // wall time spent inside feed()
    double pipelineSeconds = 0;   // of which: pipeline rebuilds (texture binding, hashing and decoding included)
    double vertexSeconds = 0;     // of which: vertex loading, XF and primitive conversion
    double backendSeconds = 0;    // of which: time inside the backend (draw and copy calls)
};

// 64-bit content hash used for texture keys and copy-destination checks.
uint64_t gxHash64(const uint8_t* p, size_t n, uint64_t seed);

class Core {
public:
    Core(const GuestMemory& mem, Backend& backend, CoreHost& host);
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Bytes in write-gather order; commands may span calls. Throws GxError.
    void feed(const uint8_t* data, size_t n);
    // Retrace counter of the VI model; the backend's frameBoundary is called when it changes before the next command.
    void setRetrace(uint32_t retrace) { retrace_ = retrace; }
    void setObserver(StreamObserver* o) { obs_ = o; }
    // Wall-time accounting in CoreStats (busy/pipeline/vertex/backend seconds): off by default because the clock reads cost
    // about as much as a small draw.
    void setProfiling(bool on) { profile_ = on; }
    void setTextureTrust(bool on) { trustEpoch_ = on; }   // false: compare the bytes of every texture binding at every draw (diagnostic)
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

    // ---- gx_draw.cpp: draw commands (vertex stage in gx_vertex.cpp) ----
    bool draw(uint8_t op, const uint8_t* d, size_t n, size_t& used);

    // ---- gx_pipeline.cpp: BP to IR state, EFB copies ----
    void rebuildPipeline();
    void buildTev(TevState& t) const;
    void bindMap(uint32_t map, TexBinding& b);
    void efbCopy();

    // ---- gx_texture.cpp: texture decoding and cache ----
    struct TexRequest {
        uint32_t addr, width, height, format, levels, tlutOffset, tlutFormat;   // addr bit 31: the bytes come from TMEM (a preloaded texture), the low bits are the TMEM byte offset
        uint32_t tmemOdd = 0;   // preloaded RGBA8: byte offset of the odd bank (the GB lines)
    };
    std::shared_ptr<const Texture> loadTexture(const TexRequest& req);
    void tlutLoad();
    void texPreload();

    // ---- state ----
    const GuestMemory& mem_;
    Backend& backend_;
    CoreHost& host_;
    CpState cp_;
    XfState xf_;
    VertexEngine engine_{mem_, cp_, xf_};
    uint32_t bp_[256] = {};
    uint32_t bpMask_ = 0xFFFFFF;
    uint32_t tevColor_[2][4][2] = {};       // [type: 0 color, 1 konst][slot][L, H] BP 0xE0-0xE7 data (bits 0-22)
    std::vector<uint8_t> pending_;
    uint64_t offset_ = 0;                   // stream offset of the command being processed
    uint64_t streamBase_ = 0;               // stream offset of pending_[0]
    uint32_t retrace_ = 0, frameSeen_ = 0xFFFFFFFFu;
    CoreStats stats_;
    StreamObserver* obs_ = nullptr;
    bool profile_ = false;
    bool trustEpoch_ = true;

    std::shared_ptr<const PipelineState> pipe_;
    uint32_t stateId_ = 0;
    bool dirty_ = true;
    uint32_t drawSeq_ = 0, copySeq_ = 0;
    uint32_t texGeneration_ = 0;            // incremented by TX_INVALIDATE / preload: the texture cache's validity epoch

    // Texture decode cache, keyed by the content hash.
    struct CacheEntry { std::shared_ptr<const Texture> tex; uint64_t bytes; };
    std::unordered_map<uint64_t, CacheEntry> texCache_;
    std::list<uint64_t> texLru_;
    uint64_t texCacheBytes_ = 0;
    // Raw bytes of the last decode per texture binding, for the exact unchanged-content check.
    struct Snapshot { bool valid = false; uint32_t tuple[9] = {}; std::vector<uint8_t> raw; std::vector<uint16_t> pal; uint64_t key = 0; uint32_t epoch = 0, tlutEpoch = 0; };
    std::unordered_map<uint64_t, Snapshot> snapshots_;
    uint64_t snapBytes_ = 0;
    // Memoised bindings per map (valid while the registers and the epoch are unchanged).
    struct MapMemo { uint32_t regs[8]; uint32_t epoch; uint32_t tlutEpoch; TexBinding binding; bool valid = false; };
    MapMemo memo_[8];
    std::vector<uint16_t> tlutRam_;         // TMEM TLUT area as 16-bit entries (offset unit: 256 entries)
    uint32_t tlutEpoch_ = 0;
    std::vector<uint8_t> tmem_;             // 1 MiB texture memory: only what GXPreLoad*Texture (BP 0x60-0x63) copied in is modelled; cached textures are read from RAM

    // EFB copies to textures that later binds may alias (convention 8).
    struct CopyRecord { uint32_t id, addr, bytes, format, width, height; uint64_t memHash; bool warned; };
    std::vector<CopyRecord> copies_;
    friend struct CoreTestAccess;
};

}  // namespace awl::gx
