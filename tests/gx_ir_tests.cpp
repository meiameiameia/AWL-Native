// Compile and layout checks of the GX IR header (src/gx/gx_ir.h), and a minimal recording backend exercising the interface.
// The expectations are properties of the documented conventions (sizes and encodings from YAGCD/libogc), not of the core.
#include <cstdio>
#include <string>
#include <vector>

#include "gx/gx_ir.h"

using namespace awl::gx;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Recorder : Backend {
    std::vector<std::string> log;
    void draw(const DrawCommand& d) override { log.push_back("draw " + std::to_string(d.indexCount)); }
    void efbCopy(const EfbCopyCommand& c) override { log.push_back("copy " + std::to_string(c.id)); }
    void frameBoundary(uint32_t r) override { log.push_back("frame " + std::to_string(r)); }
};

}  // namespace

int main() {
    check(kIrVersion == 2, "IR version constant");
    // Vertex: clip position (4 floats), two RGBA8 colors, eight (s,t,q) triples.
    check(sizeof(Vertex) == 16 + 8 + 96, "Vertex is 120 bytes");
    // Enumerations carry the hardware encodings (libogc gx.h): GX_TEV_KCSEL, TEV inputs, compare ops.
    check(static_cast<int>(TevColorIn::Zero) == 15 && static_cast<int>(TevColorIn::TexC) == 8 && static_cast<int>(TevColorIn::RasC) == 10, "TEV color input encoding");
    check(static_cast<int>(TevAlphaIn::Zero) == 7 && static_cast<int>(TevAlphaIn::TexA) == 4, "TEV alpha input encoding");
    check(static_cast<int>(TevOp::CompR8GT) == 8 && static_cast<int>(TevOp::CompRGB8EQ) == 15, "TEV compare op encoding");
    check(static_cast<int>(CompareFunc::LEqual) == 3 && static_cast<int>(CompareFunc::Always) == 7, "compare function encoding");
    check(static_cast<int>(FogType::Linear) == 2 && static_cast<int>(FogType::RevExp2) == 7, "fog type encoding");
    check(static_cast<int>(LogicOp::Copy) == 3 && static_cast<int>(LogicOp::Set) == 15, "logic op encoding");
    check(static_cast<int>(BlendFactor::InvDstAlpha) == 7, "blend factor encoding");

    // The backend interface is usable through a base pointer and delivers commands in call order.
    Recorder rec;
    Backend& b = rec;
    PipelineState state{};
    Vertex v[3]{};
    const uint16_t idx[3] = {0, 1, 2};
    DrawCommand d{1, 1, &state, Primitive::Triangles, v, 3, idx, 3};
    EfbCopyCommand c{};
    c.id = 7;
    b.frameBoundary(5);
    b.draw(d);
    b.efbCopy(c);
    b.sync(SyncKind::DrawDone, 0);
    check(rec.log.size() == 3 && rec.log[0] == "frame 5" && rec.log[1] == "draw 3" && rec.log[2] == "copy 7", "commands arrive in call order");
    if (failures) return 1;
    std::printf("gx_ir_tests: all checks passed\n");
    return 0;
}
