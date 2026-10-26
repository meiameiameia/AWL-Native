// awl-gx-bench: replays a captured GX stream (awl-gx-survey dump) against a MEM1 image through the GX core with a
// checksumming null backend, to time the vertex path and to prove that an optimisation leaves the IR output bit-identical.
//   awl-host --dol main.dol --disc disc --dump-mem build/bench/mem1.bin   (with AWL_GX_CAPTURE/AWL_GX_FRAMES set)
//   awl-gx-bench --mem build/bench/mem1.bin --dump build/bench/title.gxd [--reps 7]
// The memory image is the one at the end of the run, so scenes with static arrays and display lists replay faithfully;
// the checksum is only comparable between builds on the same inputs.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gx/gx_core.h"
#include "gx/gx_dump.h"

using namespace awl::gx;
using namespace awl::gx::dump;

namespace {

struct Memory : GuestMemory {
    std::vector<uint8_t> bytes;
    const uint8_t* read(uint32_t a, uint32_t n) const override { return (uint64_t(a) + n <= bytes.size()) ? bytes.data() + a : nullptr; }
};

struct Sink : Backend {
    uint64_t sum = 0, draws = 0, vertices = 0, indices = 0, copies = 0;
    double seconds = 0;                       // time spent in this backend (its checksumming), subtracted from the core's wall time
    static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull; }
    void draw(const DrawCommand& d) override {
        const auto t0 = std::chrono::steady_clock::now();
        ++draws;
        vertices += d.vertexCount;
        indices += d.indexCount;
        uint64_t h = sum;
        for (uint32_t i = 0; i < d.vertexCount; ++i) {
            uint32_t w[30];
            static_assert(sizeof(Vertex) == 120, "vertex layout");
            std::memcpy(w, &d.vertices[i], sizeof w);
            for (uint32_t x : w) h = mix(h, x);
        }
        for (uint32_t i = 0; i < d.indexCount; ++i) h = mix(h, d.indices[i]);
        h = mix(h, d.stateId);
        sum = h;
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    void efbCopy(const EfbCopyCommand& c) override { ++copies; sum = mix(sum, c.id ^ (c.destAddr << 1)); }
};

struct Host : CoreHost {};

}  // namespace

int main(int argc, char** argv) {
    std::string memPath, dumpPath;
    int reps = 7;
    bool profile = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mem" && i + 1 < argc) memPath = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--reps" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "--profile") profile = true;
    }
    if (memPath.empty() || dumpPath.empty()) {
        std::fprintf(stderr, "usage: awl-gx-bench --mem <MEM1 image> --dump <gx dump> [--reps N]\n");
        return 2;
    }
    Memory mem;
    FILE* f = std::fopen(memPath.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", memPath.c_str()); return 1; }
    std::fseek(f, 0, SEEK_END);
    mem.bytes.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(mem.bytes.data(), 1, mem.bytes.size(), f) != mem.bytes.size()) return 1;
    std::fclose(f);

    std::vector<uint8_t> prelude, stream;
    uint32_t frames = 0;
    Reader r;
    std::string err;
    if (!r.open(dumpPath, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    Record rec;
    while (r.next(rec, err)) {
        if (rec.type == Rec::Prelude) prelude.insert(prelude.end(), rec.data.begin(), rec.data.end());
        else if (rec.type == Rec::Fifo) stream.insert(stream.end(), rec.data.begin(), rec.data.end());
        else if (rec.type == Rec::Frame) ++frames;
    }
    if (!err.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

    // One core for all passes (texture snapshots, plans and arrays stay warm like in a running game); pass 0 starts from the
    // recorded state and its checksum is the one comparable between builds, the other passes are timed.
    std::vector<double> perVertex, perFrame;
    uint64_t checksum = 0, lastChecksum = 0, vertices = 0, draws = 0;
    Sink sink;
    Host host;
    Core core(mem, sink, host);
    core.setProfiling(profile);       // --profile: per-stage seconds from the core's own clocks (their reads add roughly 3 ns per vertex)
    try {
        core.feed(prelude.data(), prelude.size());
        for (int rep = 0; rep < reps + 1; ++rep) {
            sink.sum = 0;
            sink.seconds = 0;
            const CoreStats base = core.stats();
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < stream.size(); i += 32) core.feed(stream.data() + i, std::min<size_t>(32, stream.size() - i));
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const CoreStats s = core.stats();
            const double back = profile ? s.backendSeconds - base.backendSeconds : sink.seconds;
            const double vtx = profile ? s.vertexSeconds - base.vertexSeconds : wall - back, pipe = s.pipelineSeconds - base.pipelineSeconds;
            vertices = s.vertices - base.vertices;
            draws = s.draws - base.draws;
            if (rep == 0) checksum = sink.sum;
            lastChecksum = sink.sum;
            if (rep > 0) {
                perVertex.push_back(1e9 * vtx / double(vertices));
                perFrame.push_back(1e3 * (wall - sink.seconds) / std::max<uint32_t>(1, frames));
            }
            if (rep == 1) std::printf("pass: %llu draws, %llu vertices; vertex stage %.1f ms, pipeline %.1f ms, backend %.1f ms, feed wall %.1f ms\n", static_cast<unsigned long long>(draws),
                                      static_cast<unsigned long long>(vertices), vtx * 1e3, pipe * 1e3, back * 1e3, wall * 1e3);
        }
    } catch (const GxError& e) {
        std::fprintf(stderr, "core error: %s (offset %llu)\n", e.what(), static_cast<unsigned long long>(e.streamOffset));
        return 3;
    }
    (void)lastChecksum;    std::sort(perVertex.begin(), perVertex.end());
    std::sort(perFrame.begin(), perFrame.end());
    std::printf("frames %u; median %.1f ns per vertex (min %.1f), %.2f ms per frame excluding the backend (min %.2f); checksum %016llX\n", frames, perVertex[perVertex.size() / 2], perVertex.front(),
                perFrame[perFrame.size() / 2], perFrame.front(), static_cast<unsigned long long>(checksum));
    return 0;
}
