// GX core: draw commands. The vertex stage (loading, XF, primitive conversion, culling) is in gx_vertex.cpp; this function
// frames it: it sizes the command, brings the pipeline state up to date, runs the engine and delivers the IR command.
#include <chrono>

#include "gx/gx_core.h"

namespace awl::gx {

bool Core::draw(uint8_t op, const uint8_t* d, size_t n, size_t& used) {
    if (n < 3) return false;
    const uint32_t count = uint32_t(d[1]) << 8 | d[2];
    const uint32_t size = engine_.vertexSize(op & 7);
    if (size == 0) fail("GX draw with an empty vertex descriptor", op);
    const size_t total = 3 + size_t(count) * size;
    if (n < total) return false;
    used = total;
    ++stats_.draws;
    stats_.vertices += count;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tA = profile_ ? Clock::now() : Clock::time_point();
    if (dirty_) rebuildPipeline();
    const Clock::time_point tB = profile_ ? Clock::now() : Clock::time_point();
    VertexEngine::Result res;
    try {
        engine_.run(op, d + 3, count, pipe_->cullMode, res);
    } catch (const GxError& e) {
        throw GxError(e.what(), offset_, e.code);
    }
    stats_.culledTriangles += res.culledTriangles;
    if (res.batched) ++stats_.batchedDraws;
    if (res.primitive == Primitive::Triangles) stats_.triangles += res.indexCount / 3;
    const Clock::time_point tC = profile_ ? Clock::now() : Clock::time_point();
    if (profile_) {
        stats_.pipelineSeconds += std::chrono::duration<double>(tB - tA).count();
        stats_.vertexSeconds += std::chrono::duration<double>(tC - tB).count();
    }
    if (res.indexCount == 0) return true;
    DrawCommand cmd{drawSeq_++, stateId_, pipe_.get(), res.primitive, res.vertices, res.vertexCount, res.indices, res.indexCount};
    backend_.draw(cmd);
    if (profile_) stats_.backendSeconds += std::chrono::duration<double>(Clock::now() - tC).count();
    return true;
}

}  // namespace awl::gx
