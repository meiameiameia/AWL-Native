// awl-capture-compare: reads the Dolphin captures (build\dolphin-capture\), the host's boot trace and the disc's sys files and
// writes build\dolphin-capture\compare-report.md. Console tool; no game window, no runtime needed.
#include <cstdio>
#include <cstring>
#include <string>

#include "capture_compare.h"

int main(int argc, char** argv) {
    awl::hle::capture::CompareInputs in;
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--capture" && i + 1 < argc) in.captureDir = argv[++i];
        else if (a == "--trace" && i + 1 < argc) in.tracePath = argv[++i];
        else if (a == "--disc" && i + 1 < argc) in.discDir = argv[++i];
        else if (a == "--host-dumps" && i + 1 < argc) in.hostDumpDir = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else {
            std::fprintf(stderr, "usage: awl-capture-compare [--capture build/dolphin-capture] [--trace build/boot-trace.log] [--disc disc]\n"
                                 "                           [--host-dumps build/host-dumps] [--out <capture>/compare-report.md]\n");
            return 2;
        }
    }
    if (out.empty()) out = in.captureDir + "/compare-report.md";
    const std::string report = awl::hle::capture::buildReport(in);
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s (does the folder exist?)\n", out.c_str());
        return 1;
    }
    const size_t n = std::fwrite(report.data(), 1, report.size(), f);
    const bool closed = std::fclose(f) == 0;
    if (n != report.size() || !closed) {
        std::fprintf(stderr, "short write to %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), report.size());
    return 0;
}
