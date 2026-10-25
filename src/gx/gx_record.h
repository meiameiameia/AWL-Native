// Backends that need no GPU: the text-dump recorder (headless per-frame IR dumps under build/) and the hub that lets
// several backends (recorder, DX11 renderer) receive the same command sequence.
#pragma once

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "gx/gx_ir.h"

namespace awl::gx {

// One line per field, deterministic text (no pointers, no host time). Used by the recorder and by tests.
std::string describePipeline(const PipelineState& s);
std::string describeDraw(const DrawCommand& d, size_t maxVertices);
std::string describeCopy(const EfbCopyCommand& c);

// Writes <dir>/frame_<retrace>.txt for the retrace intervals [first, first + count) in which commands arrive.
class RecordingBackend : public Backend {
public:
    RecordingBackend(const std::string& dir, uint32_t first, uint32_t count);
    ~RecordingBackend() override;
    void draw(const DrawCommand& d) override;
    void efbCopy(const EfbCopyCommand& c) override;
    void sync(SyncKind kind, uint16_t token) override;
    void frameBoundary(uint32_t retrace) override;
    uint32_t framesWritten() const { return frames_; }

private:
    void close();
    std::string dir_;
    uint32_t first_, count_, frames_ = 0, lastState_ = 0;
    FILE* f_ = nullptr;
    std::set<uint64_t> dumpedTextures_;   // level 0 of each texture the dumped draws use is written once as tex_<key>.png
};

// Forwards every command to the registered backends in registration order. Registration is not thread safe: do it before the
// run starts. The platform's GX model delivers to this hub; the renderer registers itself with add().
class BackendHub : public Backend {
public:
    void add(Backend* b) { list_.push_back(b); }
    void remove(Backend* b);
    bool empty() const { return list_.empty(); }
    void draw(const DrawCommand& d) override { for (Backend* b : list_) b->draw(d); }
    void efbCopy(const EfbCopyCommand& c) override { for (Backend* b : list_) b->efbCopy(c); }
    void sync(SyncKind k, uint16_t t) override { for (Backend* b : list_) b->sync(k, t); }
    void frameBoundary(uint32_t r) override { for (Backend* b : list_) b->frameBoundary(r); }

private:
    std::vector<Backend*> list_;
};
BackendHub& backendHub();   // process-wide hub used by src/hle/dev_gx.cpp

}  // namespace awl::gx
