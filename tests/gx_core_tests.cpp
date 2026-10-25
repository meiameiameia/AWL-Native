// Tests of the GX core with hand-assembled command streams. Every expectation is derived from the public formats, not from the
// implementation: command and register layouts (YAGCD chapter 5, libogc gx.c), the projection and viewport formulas
// (YAGCD XF registers), the tile layouts of the texture formats (YAGCD chapter 15), and arithmetic done by hand in the
// comments. The face-culling sign is the one expectation that comes from observation: the game's logo quads (clockwise on
// screen) survive the cull-back state.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gx/gx_core.h"
#include "gx/gx_record.h"

using namespace awl::gx;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// ---- Fixtures --------------------------------------------------------------------------------------------------
struct Mem : GuestMemory {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 20, 0);
    const uint8_t* read(uint32_t a, uint32_t n) const override { return (uint64_t(a) + n <= bytes.size()) ? bytes.data() + a : nullptr; }
    void put32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a + i] = uint8_t(v >> (24 - 8 * i)); }
    void putf(uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(a, v); }
    void put16(uint32_t a, uint16_t v) { bytes[a] = uint8_t(v >> 8); bytes[a + 1] = uint8_t(v); }
};

struct Rec : Backend {
    struct Draw { std::vector<Vertex> v; std::vector<uint16_t> idx; PipelineState state; Primitive prim; uint32_t stateId; };
    std::vector<Draw> draws;
    std::vector<EfbCopyCommand> copies;
    std::vector<std::string> order;
    std::vector<uint32_t> frames;
    void draw(const DrawCommand& d) override {
        draws.push_back({std::vector<Vertex>(d.vertices, d.vertices + d.vertexCount), std::vector<uint16_t>(d.indices, d.indices + d.indexCount), *d.state, d.primitive, d.stateId});
        order.push_back("draw");
    }
    void efbCopy(const EfbCopyCommand& c) override { copies.push_back(c); order.push_back("copy"); }
    void frameBoundary(uint32_t r) override { frames.push_back(r); }
};

struct Host : CoreHost {
    int finishes = 0, tokens = 0, ints = 0, copiesSeen = 0;
    uint16_t lastToken = 0;
    std::vector<std::string> logs;
    void drawDone() override { ++finishes; }
    void token(uint16_t v, bool i) override { ++tokens; ints += i; lastToken = v; }
    void efbCopyIssued(CopyTarget) override { ++copiesSeen; }
    void log(const char* m) override { logs.push_back(m); }
};

struct Stream {
    std::vector<uint8_t> b;
    Stream& u8(uint32_t v) { b.push_back(uint8_t(v)); return *this; }
    Stream& u16(uint32_t v) { u8(v >> 8); return u8(v); }
    Stream& u32(uint32_t v) { u16(v >> 16); return u16(v & 0xFFFF); }
    Stream& f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); return u32(v); }
    Stream& cp(uint32_t reg, uint32_t val) { u8(0x08); u8(reg); return u32(val); }
    Stream& bp(uint32_t reg, uint32_t val) { u8(0x61); return u32(reg << 24 | (val & 0xFFFFFF)); }
    Stream& xf(uint32_t addr, const std::vector<uint32_t>& w) { u8(0x10); u32((uint32_t(w.size()) - 1) << 16 | addr); for (uint32_t x : w) u32(x); return *this; }
    Stream& xff(uint32_t addr, const std::vector<float>& f) {
        std::vector<uint32_t> w;
        for (float x : f) { uint32_t v; std::memcpy(&v, &x, 4); w.push_back(v); }
        return xf(addr, w);
    }
    Stream& drawHdr(uint32_t op, uint32_t count) { u8(op); return u16(count); }
};

struct Rig {
    Mem mem;
    Rec rec;
    Host host;
    Core core{mem, rec, host};
    void feed(const Stream& s) { core.feed(s.b.data(), s.b.size()); }
    bool feedFails(const Stream& s, std::string* msg = nullptr, uint64_t* off = nullptr) {
        try { feed(s); } catch (const GxError& e) { if (msg) *msg = e.what(); if (off) *off = e.streamOffset; return true; }
        return false;
    }
};

// The state the SDK establishes for a 640x480 2D pass: orthographic projection x' = 2x/640 - 1, y' = 1 - 2y/480, z' = z/1000 - 1,
// viewport 640x480 at the origin, identity position matrix 0, one color channel taking the vertex color, no texgens,
// no culling, one TEV stage passing the rasterized color.
Stream base2d() {
    Stream s;
    s.xff(0x1020, {2.0f / 640, -1.0f, -2.0f / 480, 1.0f, 0.001f, -1.0f});
    s.xf(0x1026, {1});
    s.xff(0x101A, {320.0f, -240.0f, 16777215.0f, 320.0f + 342.0f, 240.0f + 342.0f, 16777215.0f});
    s.xff(0x0000, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    s.xf(0x1009, {1}).xf(0x100E, {1}).xf(0x1010, {1}).xf(0x103F, {0});
    s.xf(0x1018, {0}).xf(0x1019, {0});
    s.bp(0x00, 1u << 4);                              // GEN_MODE: 0 texgens, 1 color channel, 1 TEV stage, no culling (tests that need it set it)
    s.bp(0x20, (342 << 12) | 342).bp(0x21, ((342 + 639) << 12) | (342 + 479));
    s.bp(0x59, (171u << 10) | 171u);                  // scissor box offset 0: libogc stores (offset + 342) >> 1
    return s;
}

// ---- Tests ----------------------------------------------------------------------------------------------------
void testOrthoQuad() {
    Rig r;
    Stream s = base2d();
    s.bp(0x00, 1u << 14 | 1u << 4);                   // cull hardware value 1 (back faces)
    // Vertex: position direct 2 x s16 (VAT0 A: cnt 0 = xy, fmt 3 = s16 -> 3 << 1 = 6), color direct RGBA8888 (VCD 3 << 13 would be indexed; direct = 1)
    s.cp(0x50, (1u << 9) | (1u << 13)).cp(0x60, 0).cp(0x70, (3u << 1) | (5u << 14));
    s.drawHdr(0x80, 4);
    const int16_t xy[4][2] = {{0, 0}, {640, 0}, {640, 480}, {0, 480}};   // top-left, top-right, bottom-right, bottom-left
    for (int i = 0; i < 4; ++i) { s.u16(uint16_t(xy[i][0])).u16(uint16_t(xy[i][1])).u32(0x11223344 + i); }
    r.feed(s);
    check(r.rec.draws.size() == 1, "quad: one draw");
    if (r.rec.draws.size() != 1) return;
    const auto& d = r.rec.draws[0];
    // x' = 2x/640 - 1 -> -1, 1, 1, -1;   y' = 1 - 2y/480 -> 1, 1, -1, -1;   z' = 0/1000 - 1 = -1;   w' = 1 (YAGCD orthographic)
    const float ex[4] = {-1, 1, 1, -1}, ey[4] = {1, 1, -1, -1};
    for (int i = 0; i < 4; ++i) {
        check(near(d.v[i].pos[0], ex[i]) && near(d.v[i].pos[1], ey[i]) && near(d.v[i].pos[2], -1.0f) && near(d.v[i].pos[3], 1.0f), "quad: clip position");
    }
    check(d.v[1].color[0][0] == 0x11 && d.v[1].color[0][1] == 0x22 && d.v[1].color[0][2] == 0x33 && d.v[1].color[0][3] == 0x45, "quad: RGBA8888 color of vertex 1");
    check(d.idx == std::vector<uint16_t>({0, 1, 2, 0, 2, 3}), "quad: two triangles sharing the 0-2 diagonal");
    check(d.prim == Primitive::Triangles, "quad: triangle list");
    // Viewport from libogc GX_SetViewport: scale (w/2, -h/2), offset (x0 + w/2 + 342, y0 + h/2 + 342)
    check(near(d.state.viewport.x, 0) && near(d.state.viewport.y, 0) && near(d.state.viewport.width, 640) && near(d.state.viewport.height, 480), "viewport rectangle");
    check(near(d.state.viewport.nearZ, 0) && near(d.state.viewport.farZ, 1.0f, 1e-6f), "viewport depth range");
    check(d.state.scissor.x0 == 0 && d.state.scissor.y0 == 0 && d.state.scissor.x1 == 639 && d.state.scissor.y1 == 479, "scissor rectangle (342 removed)");
    check(r.core.stats().culledTriangles == 0, "clockwise quad survives cull-back (observed in the game's logo quads)");
    // Counter-clockwise: dropped by cull-back, kept with cull none.
    Stream s2;
    s2.drawHdr(0x90, 3);
    const int16_t ccw[3][2] = {{0, 0}, {0, 480}, {640, 480}};
    for (auto& p : ccw) s2.u16(uint16_t(p[0])).u16(uint16_t(p[1])).u32(0xFFFFFFFF);
    r.feed(s2);
    check(r.rec.draws.size() == 1 && r.core.stats().culledTriangles == 1, "counter-clockwise triangle culled by cull-back");
    Stream s3;
    s3.bp(0x00, 1u << 4);       // cull none
    s3.drawHdr(0x90, 3);
    for (auto& p : ccw) s3.u16(uint16_t(p[0])).u16(uint16_t(p[1])).u32(0xFFFFFFFF);
    r.feed(s3);
    check(r.rec.draws.size() == 2, "cull none keeps the triangle");
    Stream s4;
    s4.bp(0x00, (2u << 14) | (1u << 4));   // hardware value 2: cull front faces
    s4.drawHdr(0x90, 3);
    for (auto& p : ccw) s4.u16(uint16_t(p[0])).u16(uint16_t(p[1])).u32(0xFFFFFFFF);
    r.feed(s4);
    check(r.rec.draws.size() == 3, "cull-front keeps a back-facing triangle");
}

void testPerspectiveDepthRange() {
    Rig r;
    Stream s = base2d();
    // Perspective with near 1, far 3 (libogc guPerspective form): z' = E z + F with E = -n/(f-n) = -0.5, F = -f n/(f-n) = -1.5; w' = -z.
    s.xff(0x1020, {2.0f, 0.0f, 2.0f, 0.0f, -0.5f, -1.5f});
    s.xf(0x1026, {0});
    s.bp(0x00, 1u << 4);
    s.cp(0x50, 1u << 9).cp(0x70, (1u) | (4u << 1));       // position direct xyz float
    s.drawHdr(0x90, 3);
    const float p[3][3] = {{0, 0, -1}, {0, 0, -3}, {1, 1, -2}};
    for (auto& v : p) s.f32(v[0]).f32(v[1]).f32(v[2]);
    r.feed(s);
    if (r.rec.draws.size() != 1) { check(false, "perspective: draw"); return; }
    const auto& v = r.rec.draws[0].v;
    // near plane: z' = 0.5 - 1.5 = -1 with w' = 1 -> z/w = -1; far plane: z' = 1.5 - 1.5 = 0, w' = 3 -> 0. Clip volume -w <= z <= 0.
    check(near(v[0].pos[2], -1.0f) && near(v[0].pos[3], 1.0f), "near plane maps to z = -w");
    check(near(v[1].pos[2], 0.0f) && near(v[1].pos[3], 3.0f), "far plane maps to z = 0");
    check(near(v[2].pos[0], 2.0f) && near(v[2].pos[1], 2.0f) && near(v[2].pos[3], 2.0f), "perspective x' = A x, y' = C y, w' = -z");
}

void testPrimitives() {
    Rig r;
    Stream s = base2d();
    s.bp(0x00, 1u << 4);
    s.cp(0x50, 1u << 9).cp(0x70, 3u << 1);       // xy s16
    auto verts = [&](Stream& st, int n) { for (int i = 0; i < n; ++i) st.u16(uint16_t(i * 8)).u16(uint16_t(i * 4)); };
    s.drawHdr(0x98, 5); verts(s, 5);             // strip
    s.drawHdr(0xA0, 5); verts(s, 5);             // fan
    s.drawHdr(0x80, 8); verts(s, 8);             // two quads
    s.drawHdr(0xA8, 4); verts(s, 4);             // lines
    s.drawHdr(0xB0, 4); verts(s, 4);             // line strip
    s.drawHdr(0xB8, 3); verts(s, 3);             // points
    s.drawHdr(0x90, 7); verts(s, 7);             // triangles with a leftover vertex
    r.feed(s);
    if (r.rec.draws.size() != 7) { check(false, "primitives: seven draws"); return; }
    // Strip: triangle k uses (k, k+1, k+2); every second one swaps the first two so the winding is constant (YAGCD primitive rules)
    check(r.rec.draws[0].idx == std::vector<uint16_t>({0, 1, 2, 2, 1, 3, 2, 3, 4}), "triangle strip");
    check(r.rec.draws[1].idx == std::vector<uint16_t>({0, 1, 2, 0, 2, 3, 0, 3, 4}), "triangle fan");
    check(r.rec.draws[2].idx == std::vector<uint16_t>({0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}), "two quads");
    check(r.rec.draws[3].prim == Primitive::Lines && r.rec.draws[3].idx == std::vector<uint16_t>({0, 1, 2, 3}), "line list");
    check(r.rec.draws[4].prim == Primitive::Lines && r.rec.draws[4].idx == std::vector<uint16_t>({0, 1, 1, 2, 2, 3}), "line strip");
    check(r.rec.draws[5].prim == Primitive::Points && r.rec.draws[5].idx == std::vector<uint16_t>({0, 1, 2}), "points");
    check(r.rec.draws[6].idx == std::vector<uint16_t>({0, 1, 2, 3, 4, 5}), "triangle list ignores a leftover vertex");
}

void testVertexFormats() {
    Rig r;
    Mem& m = r.mem;
    Stream s = base2d();
    s.bp(0x00, 1u << 4);
    // Position: indexed 8-bit from array 0 at 0x1000, stride 8, 3 x s16 with shift 4 (VAT A: cnt 1, fmt 3, shift 4).
    // Normal: none. Color 0: direct RGB565. Color 1: direct RGBA4444. Tex0: direct 2 x u8 shift 2 (VAT A: cnt 1, fmt 0, shift 2).
    m.put16(0x1000 + 0, uint16_t(int16_t(32))); m.put16(0x1002, uint16_t(int16_t(-48))); m.put16(0x1004, 16);          // entry 0: (2, -3, 1)
    m.put16(0x1008 + 0, 64); m.put16(0x100A, 0); m.put16(0x100C, 0);                                                    // entry 1: (4, 0, 0)
    s.cp(0xA0, 0x1000).cp(0xB0, 8);
    s.cp(0x50, (2u << 9) | (1u << 13) | (1u << 15)).cp(0x60, 1u);
    const uint32_t vatA = 1u | (3u << 1) | (4u << 4) | (0u << 13) | (0u << 14) | (1u << 17) | (3u << 18) | (1u << 21) | (0u << 22) | (2u << 25);
    s.cp(0x70, vatA);
    s.xf(0x1009, {2}).xf(0x100F, {1}).xf(0x1011, {1});
    s.drawHdr(0x90, 3);
    for (int i = 0; i < 3; ++i) {
        s.u8(i == 2 ? 1 : 0);                                 // index (entry 0, entry 0, entry 1)
        s.u16(0xF800);                                        // color0 RGB565: red
        s.u16(0x8421);                                        // color1 RGBA4444: r=8,g=4,b=2,a=1
        s.u8(8).u8(4);                                        // tex0 (u8 shift 2): 2.0, 1.0
    }
    r.feed(s);
    if (r.rec.draws.size() != 1) { check(false, "formats: one draw"); return; }
    const Vertex& v0 = r.rec.draws[0].v[0];
    const Vertex& v2 = r.rec.draws[0].v[2];
    // Ortho: x' = 2x/640 - 1 -> x=2: -0.99375; y=-3: 1 + 6/480 = 1.0125; indexed entry 1 has x=4
    check(near(v0.pos[0], 2.0f * 2 / 640 - 1) && near(v0.pos[1], 1.0f - 2.0f * -3 / 480), "indexed s16 position with shift 4");
    check(near(v2.pos[0], 2.0f * 4 / 640 - 1), "second array entry");
    check(v0.color[0][0] == 255 && v0.color[0][1] == 0 && v0.color[0][2] == 0 && v0.color[0][3] == 255, "RGB565 red -> (255,0,0,255)");
    // RGBA4444 0x8421: nibbles r=8 -> 136, g=4 -> 68, b=2 -> 34, a=1 -> 17 (n * 17)
    check(v0.color[1][0] == 136 && v0.color[1][1] == 68 && v0.color[1][2] == 34 && v0.color[1][3] == 17, "RGBA4444 expansion n*17");
    // Without texgens the stream's tex coordinates are not forwarded; load one texgen to check the u8-with-shift decode
    Stream t;
    t.xf(0x103F, {1});
    t.xf(0x1040, {5u << 7});             // texgen 0: regular, ST, source tex0
    t.xff(0x00F0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    t.xf(0x1018, {60u << 6});            // tex0 matrix index 60 = identity
    t.drawHdr(0x90, 3);
    for (int i = 0; i < 3; ++i) { t.u8(0); t.u16(0xF800).u16(0x8421).u8(8).u8(4); }
    r.feed(t);
    check(r.rec.draws.size() == 2 && near(r.rec.draws[1].v[0].tex[0][0], 2.0f) && near(r.rec.draws[1].v[0].tex[0][1], 1.0f) && near(r.rec.draws[1].v[0].tex[0][2], 1.0f), "u8 texcoords with shift 2 through identity texgen (s,t,q) = (2,1,1)");
}

void testTexGenAndLighting() {
    Rig r;
    Stream s = base2d();
    s.bp(0x00, 1u << 4);
    // pos xyz f32, normal xyz f32, tex0 st f32, all direct
    s.cp(0x50, (1u << 9) | (1u << 11)).cp(0x60, 1u).cp(0x70, 1u | (4u << 1) | (4u << 10) | (1u << 21) | (4u << 22));
    // Texgen 0: regular ST from tex0 through a texture matrix (index 30) that scales by 2; dual transform off.
    s.xf(0x103F, {1}).xf(0x1040, {5u << 7}).xf(0x1012, {0});
    s.xff(0x0078, {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0});
    s.xf(0x1018, {30u << 6});
    // Lighting on color 0: material from register 0xFF8040FF, ambient register black, light 0, diffuse clamp, no attenuation.
    s.xf(0x1009, {1}).xf(0x100C, {0xFF8040FFu}).xf(0x100A, {0x000000FFu});
    s.xf(0x100E, {(1u << 1) | (1u << 2) | (2u << 7)}).xf(0x1010, {0});
    s.xff(0x0400, {1, 0, 0, 0, 1, 0, 0, 0, 1});            // normal matrix 0 = identity
    std::vector<uint32_t> light(16, 0);
    light[3] = 0xFFFFFFFFu;
    float lp[3] = {0, 0, 1.0e6f};                          // a far light along +z: direction toward the light is (0,0,1)
    for (int i = 0; i < 3; ++i) std::memcpy(&light[10 + i], &lp[i], 4);
    s.xf(0x0600, light);
    s.drawHdr(0x90, 3);
    const float nrm[3][3] = {{0, 0, 1}, {0, 0.6f, 0.8f}, {0, 0, -1}};
    for (int i = 0; i < 3; ++i) s.f32(float(i)).f32(0).f32(0).f32(nrm[i][0]).f32(nrm[i][1]).f32(nrm[i][2]).f32(0.25f * float(i + 1)).f32(0.5f);
    r.feed(s);
    if (r.rec.draws.size() != 1) { check(false, "lighting: one draw"); return; }
    const auto& v = r.rec.draws[0].v;
    // N.L = 1: lacc = 0 + 255 -> (mat * (255 + 1)) >> 8 = mat. Alpha channel is unlit: the material register alpha 0xFF.
    check(v[0].color[0][0] == 255 && v[0].color[0][1] == 0x80 && v[0].color[0][2] == 0x40 && v[0].color[0][3] == 255, "fully lit vertex returns the material color");
    // N.L = 0.8: lacc = round(204) = 204; out = mat * 205 >> 8 -> R 255*205>>8 = 204, G 128*205>>8 = 102, B 64*205>>8 = 51 (~ mat * 0.8)
    check(v[1].color[0][0] == 204 && v[1].color[0][1] == 102 && v[1].color[0][2] == 51, "diffuse N.L = 0.8 scales the material by 0.8");
    // N.L = -1 with the clamped diffuse function: no light, ambient (0) only
    check(v[2].color[0][0] == 0 && v[2].color[0][1] == 0 && v[2].color[0][2] == 0, "clamped diffuse gives black for a back-facing normal");
    // Texgen: s = 2u, t = 2v, q = 1 for the ST form
    check(near(v[1].tex[0][0], 1.0f) && near(v[1].tex[0][1], 1.0f) && near(v[1].tex[0][2], 1.0f), "texture matrix scale 2 applied to (0.5, 0.5): (1, 1, 1)");
}

void testBlendAndTev() {
    Rig r;
    Stream s = base2d();
    s.cp(0x50, 1u << 9).cp(0x70, 3u << 1);
    // TEV stage 0: color (a,b,c,d) = (ZERO, TEXC, RASC, ZERO): fields d bits 0-3, c 4-7, b 8-11, a 12-15 -> 0xF8AF? a=F b=8 c=A d=F
    s.bp(0xC0, (0xFu << 12) | (0x8u << 8) | (0xAu << 4) | 0xF);
    // alpha env: d bits 4-6, c 7-9, b 10-12, a 13-15: (a,b,c,d) = (ZERO 7, TEXA 4, RASA 5, ZERO 7), dest prev, clamp bit 19
    s.bp(0xC1, (7u << 13) | (4u << 10) | (5u << 7) | (7u << 4) | (1u << 19));
    // TREF0: map 0, coord 0, enable (bit 6), color channel 0
    s.bp(0x28, 0x40);
    s.bp(0x41, 1u | (1u << 2) | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5));       // blend, dither, color+alpha update, src = srcalpha(4), dst = 1-srcalpha(5)
    s.bp(0x40, 1u | (3u << 1) | (1u << 4));                                            // z test lequal, write
    s.bp(0xF3, 0 | (7u << 16) | (7u << 19) | (0u << 22));                              // alpha compare: always AND always
    s.bp(0x43, 0);                                                                     // RGB8_Z24
    s.bp(0xF6, 0);
    // Texture 0: 1x1 RGBA8 at 0x2000 (tile of 64 bytes: AR pair then GB pair)
    r.mem.bytes[0x2000] = 0x80; r.mem.bytes[0x2001] = 0xFF; r.mem.bytes[0x2020] = 0x40; r.mem.bytes[0x2021] = 0x20;     // A=0x80 R=0xFF, G=0x40 B=0x20
    s.bp(0x88, 0 | (0u << 10) | (6u << 20)).bp(0x94, 0x2000 >> 5).bp(0x80, 0).bp(0x84, 0);
    s.drawHdr(0x90, 3);
    for (int i = 0; i < 3; ++i) s.u16(uint16_t(i)).u16(0);
    r.feed(s);
    if (r.rec.draws.size() != 1) { check(false, "tev: one draw"); return; }
    const PipelineState& p = r.rec.draws[0].state;
    const TevStage& t = p.tev.stages[0];
    check(t.color.in[0] == uint8_t(TevColorIn::Zero) && t.color.in[1] == uint8_t(TevColorIn::TexC) && t.color.in[2] == uint8_t(TevColorIn::RasC) && t.color.in[3] == uint8_t(TevColorIn::Zero), "TEV color inputs (ZERO, TEXC, RASC, ZERO)");
    check(t.alpha.in[1] == uint8_t(TevAlphaIn::TexA) && t.alpha.in[2] == uint8_t(TevAlphaIn::RasA) && t.alpha.clamp, "TEV alpha inputs and clamp");
    check(t.texMap == 0 && t.texCoord == 0 && p.tev.numStages == 1, "TEV stage texture selection");
    check(p.blend.mode == BlendMode::Blend && p.blend.src == BlendFactor::SrcAlpha && p.blend.dst == BlendFactor::InvSrcAlpha && p.blend.dither && p.blend.colorUpdate && p.blend.alphaUpdate, "blend state");
    check(p.depth.test && p.depth.func == CompareFunc::LEqual && p.depth.write, "z mode");
    check(p.alphaCompare.comp0 == CompareFunc::Always && p.alphaCompare.comp1 == CompareFunc::Always, "alpha compare");
    check(p.maps[0].valid && p.maps[0].image && p.maps[0].image->width == 1, "bound texture");
    if (p.maps[0].image) check(p.maps[0].image->rgba[0] == (0xFFu | 0x40u << 8 | 0x20u << 16 | 0x80u << 24), "RGBA8 tile decodes AR then GB planes");
}

void testTextureFormats() {
    Rig r;
    Mem& m = r.mem;
    auto bind = [&](uint32_t fmt, uint32_t w, uint32_t h, uint32_t addr, uint32_t tlutReg = 0, uint32_t mode0 = 0, uint32_t mode1 = 0) {
        Stream s = base2d();
        s.bp(0x28, 0x40).bp(0x88, (w - 1) | (h - 1) << 10 | fmt << 20).bp(0x94, addr >> 5).bp(0x80, mode0).bp(0x84, mode1).bp(0x98, tlutReg);
        s.cp(0x50, 1u << 9).cp(0x70, 3u << 1);
        s.drawHdr(0x90, 3);
        for (int i = 0; i < 3; ++i) s.u16(uint16_t(i)).u16(0);
        r.feed(s);
        return r.rec.draws.back().state.maps[0].image;
    };
    // I4, 8x8 tile of 32 bytes: row y has 4 bytes, high nibble first. Texel (x=1,y=0) = low nibble of byte 0; (x=0,y=1) = high nibble of byte 4.
    m.bytes[0x3000] = 0x1F; m.bytes[0x3004] = 0x70;
    auto t = bind(0, 8, 8, 0x3000);
    check(t && t->rgba[0] == 0x11111111u && t->rgba[1] == 0xFFFFFFFFu && t->rgba[8] == 0x77777777u, "I4: nibbles, replicated into RGBA (n*17)");
    // I8 8x4 tile: byte y*8+x
    m.bytes[0x3100] = 0x40; m.bytes[0x3100 + 9] = 0xA0;
    t = bind(1, 8, 4, 0x3100);
    check(t && t->rgba[0] == 0x40404040u && t->rgba[8 + 1] == 0xA0A0A0A0u, "I8: intensity into all four channels");
    // IA8 4x4 tile: 2 bytes per texel, first byte alpha, second intensity
    m.bytes[0x3200] = 0x80; m.bytes[0x3201] = 0x20;
    t = bind(3, 4, 4, 0x3200);
    check(t && t->rgba[0] == (0x20u | 0x20u << 8 | 0x20u << 16 | 0x80u << 24), "IA8: alpha first, then intensity");
    // IA4 8x4 tile: byte y*8+x, high nibble alpha, low nibble intensity
    m.bytes[0x3300] = 0x8F;
    t = bind(2, 8, 4, 0x3300);
    check(t && t->rgba[0] == (255u | 255u << 8 | 255u << 16 | (8u * 17) << 24), "IA4: alpha nibble then intensity nibble");
    // RGB565 4x4 tile: 0xF800 red, 0x07E0 green at texel 1, 0x001F blue at texel 2
    m.put16(0x3400, 0xF800); m.put16(0x3402, 0x07E0); m.put16(0x3404, 0x001F);
    t = bind(4, 4, 4, 0x3400);
    check(t && t->rgba[0] == 0xFF0000FFu && t->rgba[1] == 0xFF00FF00u && t->rgba[2] == 0xFFFF0000u, "RGB565 red, green, blue");
    // RGB5A3: bit 15 set -> RGB555 opaque; clear -> A3 R4 G4 B4. 0xFC00 = red; 0x7F00 = alpha 7 (255), red 15, rest 0
    m.put16(0x3500, 0xFC00); m.put16(0x3502, 0x7F00); m.put16(0x3504, 0x2F00);
    t = bind(5, 4, 4, 0x3500);
    check(t && t->rgba[0] == 0xFF0000FFu && t->rgba[1] == 0xFF0000FFu && t->rgba[2] == ((255u) | (((2u << 5) | (2u << 2) | (2u >> 1))) << 24), "RGB5A3 both encodings");
    // CMPR: one 8x8 block = four 4x4 sub-blocks of (c0, c1, 4 rows of 2-bit selectors). c0 = 0xF800 (red) > c1 = 0x001F (blue):
    // palette 0 red, 1 blue, 2 = (5 red + 3 blue)/8, 3 = (3 red + 5 blue)/8. Row 0 selectors 0,1,2,3 -> 0b00011011 = 0x1B.
    uint32_t a = 0x3600;
    m.put16(a, 0xF800); m.put16(a + 2, 0x001F); m.bytes[a + 4] = 0x1B;
    t = bind(14, 8, 8, 0x3600);
    check(t && t->rgba[0] == 0xFF0000FFu && t->rgba[1] == 0xFFFF0000u, "CMPR endpoints");
    check(t && ((t->rgba[2] & 0xFF) == (255u * 5 / 8)) && (((t->rgba[2] >> 16) & 0xFF) == (255u * 3 / 8)), "CMPR 5/8-3/8 interpolation (red 159, blue 95)");
    // Color index with a TLUT: C8 8x4 tile, palette RGB565 at offset 0 with entries 0 = red, 1 = green.
    m.put16(0x3800, 0xF800); m.put16(0x3802, 0x07E0);
    m.bytes[0x3700] = 1;
    Stream l;
    l.bp(0x64, 0x3800 >> 5).bp(0x65, 0 | (1u << 10));             // 1 x 16 entries from 0x3800 to palette offset 0
    r.feed(l);
    t = bind(9, 8, 4, 0x3700, (1u << 10) | 0);                     // TX_SETTLUT: RGB565 palette at offset 0
    check(t && t->rgba[0] == 0xFF00FF00u && t->rgba[1] == 0xFF0000FFu, "C8 through an RGB565 TLUT");
    // Mip chain: LIN_MIP_LIN (hardware 6) with max LOD 2.0 on a 4x4 RGB565 texture -> levels 4x4, 2x2, 1x1
    t = bind(4, 4, 4, 0x3400, 0, 6u << 5, 32u << 8);
    check(t && t->levelCount == 3 && t->levels[1].width == 2 && t->levels[2].width == 1, "mip levels follow the filter and max LOD");
    // Cache: the same bytes bound again decode once
    const uint64_t decodes = r.core.stats().textureDecodes;
    bind(4, 4, 4, 0x3400);
    bind(4, 4, 4, 0x3400);
    check(r.core.stats().textureDecodes == decodes, "identical texture content hits the decode cache");
}

void testEfbCopies() {
    Rig r;
    Stream s = base2d();
    // XFB copy with clear: source (0,0) 640x480, destination 0x10000, clear color A=0xFF R=0x10, G=0x20 B=0x30, z 0xFFFFFF.
    s.bp(0x49, 0).bp(0x4A, (479u << 10) | 639u).bp(0x4B, 0x10000 >> 5).bp(0x4D, 640 >> 4);
    s.bp(0x4F, 0xFF10).bp(0x50, 0x2030).bp(0x51, 0xFFFFFF);
    s.bp(0x41, (1u << 3) | (1u << 4)).bp(0x40, (1u) | (7u << 1) | (1u << 4));      // color and alpha update, z update
    s.bp(0x4E, 256);
    s.bp(0x53, 0x30A208).bp(0x54, 0x00820A);
    s.bp(0x52, (1u << 14) | (1u << 11) | 0x3);
    r.feed(s);
    if (r.rec.copies.size() != 1) { check(false, "xfb copy delivered"); return; }
    const EfbCopyCommand& c = r.rec.copies[0];
    check(c.target == CopyTarget::Xfb && c.srcWidth == 640 && c.srcHeight == 480 && c.destAddr == 0x10000 && c.destWidth == 640 && c.destHeight == 480, "XFB copy geometry");
    check(c.clear.color && c.clear.alpha && c.clear.z && c.clear.rgba[0] == 0x10 && c.clear.rgba[1] == 0x20 && c.clear.rgba[2] == 0x30 && c.clear.rgba[3] == 0xFF && c.clear.z24 == 0xFFFFFF, "clear colors");
    check(c.clampTop && c.clampBottom, "copy clamp bits");
    check(c.vfilter[0] == 8 && c.vfilter[1] == 8 && c.vfilter[2] == 10 && c.vfilter[3] == 12 && c.vfilter[4] == 10 && c.vfilter[5] == 8 && c.vfilter[6] == 8, "vertical filter taps (6 bits each, regs 0x53/0x54)");
    check(near(c.yScale, 1.0f), "y scale 256/256");
    check(r.host.copiesSeen == 1, "host sees the copy");
    // finish and token services
    Stream t;
    t.bp(0x45, 2).bp(0x48, 0x1234).bp(0x47, 0x4321);
    r.feed(t);
    check(r.host.finishes == 1 && r.host.tokens == 2 && r.host.ints == 1 && r.host.lastToken == 0x4321, "finish and token services reach the host");
    // Copy to texture: RGB565 format 4 (lfmt 4 -> bits 4-6), 64x64 to 0x20000; then a texture bound there aliases the copy.
    Rig q;
    Stream u = base2d();
    u.bp(0x49, 0).bp(0x4A, (63u << 10) | 63u).bp(0x4B, 0x20000 >> 5).bp(0x43, 0);
    u.bp(0x52, (4u << 4) | (1u << 16));            // not XFB, no clear, format RGB565 (class bit 15 clear)
    u.cp(0x50, 1u << 9).cp(0x70, 3u << 1);
    u.bp(0x28, 0x40).bp(0x88, 63u | (63u << 10) | (4u << 20)).bp(0x94, 0x20000 >> 5);
    u.drawHdr(0x90, 3);
    for (int i = 0; i < 3; ++i) u.u16(uint16_t(i)).u16(0);
    q.feed(u);
    check(q.rec.copies.size() == 1 && q.rec.copies[0].target == CopyTarget::Texture && q.rec.copies[0].texFormat == 4 && q.rec.copies[0].destWidth == 64, "copy to texture (RGB565)");
    check(q.rec.draws.size() == 1 && q.rec.draws[0].state.maps[0].efbCopyId == q.rec.copies[0].id && !q.rec.draws[0].state.maps[0].image, "texture bound at a copy destination references the copy");
    check(q.rec.order.size() == 2 && q.rec.order[0] == "copy", "copy precedes the draw that samples it");
    // CPU rewrites the destination: the next bind reads RAM and logs once
    q.mem.bytes[0x20000] = 0x55;
    Stream v;
    v.bp(0x66, 0);                                      // texture cache invalidate
    v.drawHdr(0x90, 3);
    for (int i = 0; i < 3; ++i) v.u16(uint16_t(i)).u16(0);
    q.feed(v);
    check(q.rec.draws.size() == 2 && q.rec.draws[1].state.maps[0].efbCopyId == 0 && q.rec.draws[1].state.maps[0].image, "changed destination memory falls back to RAM");
    check(q.host.logs.size() == 1, "the fallback is logged once");
    q.feed(v);
    check(q.host.logs.size() == 1, "and not again");
}

void testStreamHandling() {
    Rig r;
    // Commands split over feed() calls
    Stream s;
    s.cp(0x30, 0x1234).bp(0x20, 0x123456);
    r.core.feed(s.b.data(), 3);
    r.core.feed(s.b.data() + 3, s.b.size() - 3);
    check(r.core.cp().matIdxA == 0x1234 && r.core.bp(0x20) == 0x123456, "commands split across feed() calls");
    // BP mask applies to the next write only
    Stream m;
    m.bp(0xFE, 0x00FF00).bp(0x20, 0xAAAAAA).bp(0x20, 0x111111);
    r.feed(m);
    check(r.core.bp(0x20) == 0x111111, "BP mask resets after one write");
    Stream m2;
    m2.bp(0x21, 0xFFFFFF).bp(0xFE, 0x00FF00).bp(0x21, 0x000000);
    r.feed(m2);
    check(r.core.bp(0x21) == 0xFF00FF, "masked BP write keeps the bits outside the mask");
    // Indexed XF load: array C-style index through CP array 12 (pos matrices)
    r.mem.putf(0x5000, 3.0f); r.mem.putf(0x5004, 4.0f);
    Stream x;
    x.cp(0xAC, 0x5000).cp(0xBC, 8);
    x.u8(0x20).u32((1u << 16) | (1u << 12) | 0x0010);             // index 1, 2 words, to XF address 0x10
    r.mem.putf(0x5008, 7.0f); r.mem.putf(0x500C, 8.0f);
    r.feed(x);
    check(r.core.xf().mem[0x10] == 0x40E00000u && r.core.xf().mem[0x11] == 0x41000000u, "indexed XF load reads array base + stride * index");
    // Display list with a BP load; nested list
    r.mem.bytes[0x6000] = 0x61; r.mem.put32(0x6001, 0x22 << 24 | 0x000042);
    Stream dl;
    dl.u8(0x40).u32(0x80006000).u32(5);
    r.feed(dl);
    check(r.core.bp(0x22) == 0x000042, "display list executes its commands");
    // NOP padding and invalidate
    Stream n;
    n.u8(0).u8(0x48).u8(0);
    r.feed(n);
    // Frame boundary
    r.core.setRetrace(7);
    r.feed(n);
    check(!r.rec.frames.empty() && r.rec.frames.back() == 7, "backend receives the frame boundary");
}

void testFailures() {
    std::string msg;
    uint64_t off = 0;
    {
        Rig r;
        Stream s;
        s.u8(0x00).u8(0x00).u8(0x55);
        check(r.feedFails(s, &msg, &off) && off == 2, "unknown opcode stops at its stream offset");
    }
    {
        Rig r;
        Stream s;
        s.bp(0x6B, 0);
        check(r.feedFails(s, &msg), "undocumented BP register stops");
    }
    {
        Rig r;
        Stream s;
        s.cp(0x10, 0);
        check(r.feedFails(s, &msg), "unknown CP register stops");
    }
    {
        Rig r;
        Stream s = base2d();
        s.bp(0xF5, 1u << 2);
        s.cp(0x50, 1u << 9).cp(0x70, 3u << 1).drawHdr(0x90, 3);
        for (int i = 0; i < 3; ++i) s.u16(uint16_t(i)).u16(0);
        check(r.feedFails(s, &msg), "z-texture is rejected");
    }
    {
        Rig r;
        Stream s = base2d();
        s.bp(0x43, 4);
        s.cp(0x50, 1u << 9).cp(0x70, 3u << 1).drawHdr(0x90, 3);
        for (int i = 0; i < 3; ++i) s.u16(uint16_t(i)).u16(0);
        check(r.feedFails(s, &msg), "Y8 EFB format is rejected");
    }
    {
        Rig r;
        Stream s = base2d();
        s.xf(0x103F, {1}).xf(0x1040, {(1u << 4) | (5u << 7)});      // emboss texgen
        s.cp(0x50, 1u << 9).cp(0x60, 1u).cp(0x70, 3u << 1 | (1u << 21) | (4u << 22)).drawHdr(0x90, 3);
        for (int i = 0; i < 3; ++i) s.u16(uint16_t(i)).u16(0).f32(0).f32(0);
        check(r.feedFails(s, &msg), "emboss texgen is rejected");
    }
    {
        Rig r;
        Stream s = base2d();
        s.u8(0x40).u32(0x80700000).u32(16);                         // display list outside the test memory
        check(r.feedFails(s, &msg), "display list outside memory stops");
    }
}

void testRecorderText() {
    Rig r;
    Stream s = base2d();
    s.cp(0x50, 1u << 9).cp(0x70, 3u << 1).drawHdr(0x90, 3);
    for (int i = 0; i < 3; ++i) s.u16(uint16_t(i * 100)).u16(uint16_t(i * 10));
    r.feed(s);
    if (r.rec.draws.empty()) { check(false, "recorder: draw"); return; }
    // The text of one draw is deterministic and independent of pointers
    DrawCommand dc{0, 1, &r.rec.draws[0].state, Primitive::Triangles, r.rec.draws[0].v.data(), 3, r.rec.draws[0].idx.data(), 3};
    const std::string a = describeDraw(dc, 8), b = describeDraw(dc, 8);
    check(a == b && a.find("draw seq=0 state=1 triangles vertices=3 indices=3") == 0, "draw description header");
}

}  // namespace

int main() {
    testOrthoQuad();
    testPerspectiveDepthRange();
    testPrimitives();
    testVertexFormats();
    testTexGenAndLighting();
    testBlendAndTev();
    testTextureFormats();
    testEfbCopies();
    testStreamHandling();
    testFailures();
    testRecorderText();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("gx_core_tests: all checks passed\n");
    return 0;
}
