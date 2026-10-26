// awl-gx-render: replays a captured GX stream against a MEM1 image and writes the XFB copies as PNG files, using the software
// reference renderer (gx_soft.h). The way to see what an IR stream looks like without a GPU.
//   awl-gx-render --mem build/bench/mem1117.bin --dump build/bench/t1117.gxd --out build/render [--keep 4]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gx/gx_core.h"
#include "gx/gx_dump.h"
#include "gx/gx_soft.h"

using namespace awl::gx;
using namespace awl::gx::dump;

namespace {
struct Memory : GuestMemory {
    std::vector<uint8_t> bytes;
    const uint8_t* read(uint32_t a, uint32_t n) const override { return (uint64_t(a) + n <= bytes.size()) ? bytes.data() + a : nullptr; }
};
struct Host : CoreHost {};
}  // namespace

int main(int argc, char** argv) {
    std::string memPath, dumpPath, out = "build/render";
    size_t keep = 4;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mem") memPath = argv[++i];
        else if (a == "--dump") dumpPath = argv[++i];
        else if (a == "--out") out = argv[++i];
        else if (a == "--keep") keep = static_cast<size_t>(std::atoi(argv[++i]));
    }
    if (memPath.empty() || dumpPath.empty()) {
        std::fprintf(stderr, "usage: awl-gx-render --mem <MEM1 image> --dump <gx dump> [--out dir] [--keep N]\n");
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
    Reader r;
    std::string err;
    if (!r.open(dumpPath, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    Record rec;
    while (r.next(rec, err)) {
        if (rec.type == Rec::Prelude) prelude.insert(prelude.end(), rec.data.begin(), rec.data.end());
        else if (rec.type == Rec::Fifo) stream.insert(stream.end(), rec.data.begin(), rec.data.end());
    }
    SoftBackend soft;
    soft.keepXfb(keep);
    Host host;
    Core core(mem, soft, host);
    try {
        core.feed(prelude.data(), prelude.size());
        for (size_t i = 0; i < stream.size(); i += 32) core.feed(stream.data() + i, std::min<size_t>(32, stream.size() - i));
    } catch (const GxError& e) {
        std::fprintf(stderr, "core error: %s (offset %llu)\n", e.what(), static_cast<unsigned long long>(e.streamOffset));
        return 3;
    }
    std::printf("%llu triangles drawn, %llu pixels written, %zu XFB images kept\n", static_cast<unsigned long long>(soft.trianglesDrawn()), static_cast<unsigned long long>(soft.pixelsWritten()), soft.xfbImages().size());
    for (auto& n : soft.notes()) std::printf("note: %s (%llu)\n", n.first.c_str(), static_cast<unsigned long long>(n.second));
    for (size_t i = 0; i < soft.xfbImages().size(); ++i) {
        const SoftBackend::Image& im = soft.xfbImages()[i];
        const std::string path = out + "/xfb_" + std::to_string(i) + ".png";
        std::string cmd;
        writePngRgba(path, im.w, im.h, im.px.data());
        std::printf("wrote %s (%ux%u)\n", path.c_str(), im.w, im.h);
    }
    return 0;
}
