// Renderer tests on hand-built IR scenes (src/gx/gx_ir.h), offscreen, no window. Expected pixels are worked out by hand from the
// GX equations: TEV out = (d + (1-c)*a + c*b + bias) * scale, blend src*sf + dst*df, GX depth = near + (z/w + 1) * (far - near),
// and the XFB copy filter out(y) = sum over k of coef[k] * in(y + k - 3) / 64.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gfx/gfx_renderer.h"

using namespace awl;
using namespace awl::gfx;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

gx::PipelineState baseState() {
    gx::PipelineState s{};
    s.efbFormat = gx::PixelFormat::RGB8_Z24;
    s.numColorChannels = 1;
    s.numTexCoords = 1;
    s.viewport = {0, 0, 640, 448, 0.0f, 1.0f, {0, 0, 0}, {0, 0, 0}};
    s.scissor = {0, 0, 639, 447};
    s.depth = {false, gx::CompareFunc::Always, false, false, gx::ZFormat::Linear};
    s.blend = {gx::BlendMode::None, gx::BlendFactor::One, gx::BlendFactor::Zero, gx::LogicOp::Copy, true, true, false, false, 0};
    s.alphaCompare = {gx::CompareFunc::Always, gx::CompareFunc::Always, 0, 0, gx::AlphaLogic::And};
    s.fog.type = gx::FogType::None;
    s.tev.numStages = 1;
    for (auto& st : s.tev.stages) {
        st.texMap = gx::kNoMap; st.texCoord = gx::kNoMap;
        st.ras = gx::RasColor::Color0A0;
        st.indirect.indStage = gx::kNoMap;
        for (int i = 0; i < 4; ++i) { st.color.in[i] = static_cast<uint8_t>(gx::TevColorIn::Zero); st.alpha.in[i] = static_cast<uint8_t>(gx::TevAlphaIn::Zero); }
        st.color.op = st.alpha.op = gx::TevOp::Add;
        st.color.clamp = st.alpha.clamp = true;
    }
    for (int t = 0; t < 4; ++t) for (int c = 0; c < 4; ++c) s.tev.swap[t][c] = static_cast<uint8_t>(c);
    // stage 0: PREV = raster colour / alpha
    s.tev.stages[0].color.in[3] = static_cast<uint8_t>(gx::TevColorIn::RasC);
    s.tev.stages[0].alpha.in[3] = static_cast<uint8_t>(gx::TevAlphaIn::RasA);
    for (auto& ts : s.texScale) { ts.sSize = 1; ts.tSize = 1; }
    return s;
}

struct Quad {
    std::vector<gx::Vertex> v;
    std::vector<uint16_t> idx = {0, 1, 2, 2, 1, 3};
    // clip-space rectangle x0..x1, y0..y1 (y up) at z/w = zw (w = 1), vertex colour rgba, texture coordinates 0..1
    Quad(float x0, float y0, float x1, float y1, float zw, const uint8_t rgba[4]) : v(4) {
        const float xy[4][2] = {{x0, y1}, {x1, y1}, {x0, y0}, {x1, y0}};   // top-left, top-right, bottom-left, bottom-right
        const float st[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        for (int i = 0; i < 4; ++i) {
            gx::Vertex& q = v[i];
            q = gx::Vertex{};
            q.pos[0] = xy[i][0]; q.pos[1] = xy[i][1]; q.pos[2] = zw; q.pos[3] = 1.0f;
            for (int c = 0; c < 4; ++c) q.color[0][c] = rgba[c];
            q.tex[0][0] = st[i][0]; q.tex[0][1] = st[i][1]; q.tex[0][2] = 1.0f;
            for (int t = 1; t < 8; ++t) q.tex[t][2] = 1.0f;
        }
    }
    gx::DrawCommand cmd(const gx::PipelineState& s) const {
        gx::DrawCommand d{};
        d.state = &s;
        d.primitive = gx::Primitive::Triangles;
        d.vertices = v.data(); d.vertexCount = 4;
        d.indices = idx.data(); d.indexCount = 6;
        return d;
    }
};

gx::EfbCopyCommand xfbCopy(uint32_t id, uint32_t addr, bool filter) {
    gx::EfbCopyCommand c{};
    c.id = id;
    c.target = gx::CopyTarget::Xfb;
    c.srcX = 0; c.srcY = 0; c.srcWidth = 640; c.srcHeight = 448;
    c.destAddr = addr; c.destWidth = 640; c.destHeight = 448;
    c.efbFormat = gx::PixelFormat::RGB8_Z24;
    c.yScale = 1.0f;
    const uint8_t id7[7] = {0, 0, 0, 64, 0, 0, 0}, game[7] = {8, 8, 10, 12, 10, 8, 8};
    for (int i = 0; i < 7; ++i) c.vfilter[i] = filter ? game[i] : id7[i];
    c.clampTop = c.clampBottom = true;
    return c;
}

bool near3(const Image& img, int x, int y, int r, int g, int b, int tol = 1) {
    const uint8_t* p = img.px(x, y);
    const bool ok = std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
    if (!ok) std::fprintf(stderr, "pixel (%d,%d) = (%d,%d,%d), expected (%d,%d,%d)\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}

}  // namespace

int main() {
    Device dev;
    std::string err;
    if (!dev.create(DeviceOptions(), err)) { std::fprintf(stderr, "device: %s\n", err.c_str()); return 1; }
    std::printf("gfx_renderer_tests: %s %s\n", dev.isWarp() ? "WARP" : "hardware", dev.adapterName().c_str());
    Renderer r;
    if (!r.init(dev, err)) { std::fprintf(stderr, "renderer: %s\n", err.c_str()); return 1; }
    Image img;
    std::string why;

    // 1. A red quad over the left half of the viewport. Clip x in [-1, 0] covers window x 0..319; clip y in [0, 1] is the top half.
    {
        const uint8_t red[4] = {255, 0, 0, 255};
        gx::PipelineState s = baseState();
        Quad q(-1, 0, 0, 1, -1.0f, red);
        const gx::DrawCommand d = q.cmd(s);
        r.draw(d);
        r.efbCopy(xfbCopy(1, 0x1000, false));
        CHECK(r.xfbImage(0x1000, img, why));
        CHECK(img.width == 640 && img.height == 448);
        CHECK(near3(img, 100, 50, 255, 0, 0));       // inside: left, top half
        CHECK(near3(img, 319, 223, 255, 0, 0));      // last pixel inside
        CHECK(near3(img, 320, 50, 0, 0, 0));         // right half untouched (initial EFB: black)
        CHECK(near3(img, 100, 224, 0, 0, 0));        // bottom half untouched
    }

    // 2. Blending: a 50% green quad over the red area. GX_BL_SRCALPHA / INVSRCALPHA with alpha 128/255 = 0.50196:
    //    R = 0 * 0.50196 + 255 * 0.49804 = 127, G = 255 * 0.50196 = 128, B = 0.
    {
        const uint8_t green[4] = {0, 255, 0, 128};
        gx::PipelineState s = baseState();
        s.blend.mode = gx::BlendMode::Blend;
        s.blend.src = gx::BlendFactor::SrcAlpha;
        s.blend.dst = gx::BlendFactor::InvSrcAlpha;
        Quad q(-1, 0, 0, 1, -1.0f, green);
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(2, 0x2000, false));
        CHECK(r.xfbImage(0x2000, img, why));
        CHECK(near3(img, 100, 50, 127, 128, 0));
        CHECK(near3(img, 400, 50, 0, 0, 0));
    }

    // 3. Depth. Clear everything first (copy with colour + z clear: black, far), then draw a green quad at z/w = -0.5 (depth 0.5
    //    with the range [0, 1]) with LEqual + write, then a blue quad behind it at z/w = -0.25 (depth 0.75), which must fail.
    {
        gx::EfbCopyCommand c = xfbCopy(3, 0x3000, false);
        c.clear.color = c.clear.alpha = c.clear.z = true;
        c.clear.rgba[3] = 255;
        c.clear.z24 = 0xFFFFFF;
        r.efbCopy(c);
        uint32_t z = 0;
        CHECK(r.peekDepth(100, 50, z, why) && z == 0xFFFFFF);
        const uint8_t green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
        gx::PipelineState s = baseState();
        s.depth = {true, gx::CompareFunc::LEqual, true, false, gx::ZFormat::Linear};
        Quad a(-1, -1, 1, 1, -0.5f, green), b(-1, -1, 1, 1, -0.25f, blue);
        r.draw(a.cmd(s));
        r.draw(b.cmd(s));
        r.efbCopy(xfbCopy(4, 0x4000, false));
        CHECK(r.xfbImage(0x4000, img, why));
        CHECK(near3(img, 100, 50, 0, 255, 0));       // the nearer quad stays
        CHECK(r.peekDepth(100, 50, z, why));
        CHECK(std::abs(static_cast<int>(z) - 8388608) <= 2);   // 0.5 * 16777215
        // a nearer quad (z/w = -0.75, depth 0.25) passes
        Quad n(-1, -1, 1, 1, -0.75f, blue);
        r.draw(n.cmd(s));
        CHECK(r.peekDepth(100, 50, z, why));
        CHECK(std::abs(static_cast<int>(z) - 4194304) <= 2);   // 0.25 * 16777215 = 4194303.75
        // the depth range comes from the viewport: [0.5, 1] maps z/w = -0.5 to 0.5 + 0.5 * 0.5 = 0.75... applied to the next clear
        c = xfbCopy(5, 0x5000, false);
        c.clear.z = true; c.clear.z24 = 0xFFFFFF;
        r.efbCopy(c);
        gx::PipelineState s2 = s;
        s2.viewport.nearZ = 0.5f; s2.viewport.farZ = 1.0f;
        Quad h(-1, -1, 1, 1, -0.5f, green);
        r.draw(h.cmd(s2));
        CHECK(r.peekDepth(100, 50, z, why));
        CHECK(std::abs(static_cast<int>(z) - static_cast<int>(0.75 * 16777215.0 + 0.5)) <= 2);
    }

    // 4. Texture, nearest filtering: a 2x2 image through TEXC * RASC (white raster). Quadrants of the 640x448 quad sample the texels.
    {
        auto tex = std::make_shared<gx::Texture>();
        tex->key = 0x1234; tex->width = tex->height = 2; tex->levelCount = 1;
        tex->levels[0] = {2, 2, 0};
        auto px = [](uint8_t r8, uint8_t g8, uint8_t b8) { return uint32_t(255) << 24 | uint32_t(b8) << 16 | uint32_t(g8) << 8 | r8; };
        tex->rgba = {px(255, 0, 0), px(0, 255, 0), px(0, 0, 255), px(255, 255, 0)};   // TL, TR, BL, BR
        gx::PipelineState s = baseState();
        gx::TevStage& st = s.tev.stages[0];
        st.texMap = 0; st.texCoord = 0;
        st.color.in[0] = static_cast<uint8_t>(gx::TevColorIn::Zero); st.color.in[1] = static_cast<uint8_t>(gx::TevColorIn::TexC);
        st.color.in[2] = static_cast<uint8_t>(gx::TevColorIn::RasC); st.color.in[3] = static_cast<uint8_t>(gx::TevColorIn::Zero);
        st.alpha.in[0] = static_cast<uint8_t>(gx::TevAlphaIn::Zero); st.alpha.in[1] = static_cast<uint8_t>(gx::TevAlphaIn::TexA);
        st.alpha.in[2] = static_cast<uint8_t>(gx::TevAlphaIn::RasA); st.alpha.in[3] = static_cast<uint8_t>(gx::TevAlphaIn::Zero);
        s.maps[0].valid = true; s.maps[0].image = tex; s.maps[0].width = s.maps[0].height = 2;
        s.maps[0].sampler = {gx::WrapMode::Clamp, gx::WrapMode::Clamp, gx::FilterMode::Near, false, 0, 0, 0, 1, false, false};
        s.texScale[0].sSize = s.texScale[0].tSize = 2;
        const uint8_t white[4] = {255, 255, 255, 255};
        Quad q(-1, -1, 1, 1, -1.0f, white);
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(6, 0x6000, false));
        CHECK(r.xfbImage(0x6000, img, why));
        CHECK(near3(img, 100, 50, 255, 0, 0));       // top-left texel
        CHECK(near3(img, 500, 50, 0, 255, 0));       // top-right
        CHECK(near3(img, 100, 300, 0, 0, 255));      // bottom-left
        CHECK(near3(img, 500, 300, 255, 255, 0));    // bottom-right
        // TexCoordScale: sSize/tSize of 1 against a 2x2 texture means s,t in 0..1 cover half of it: the whole quad shows the top-left texel
        s.texScale[0].sSize = s.texScale[0].tSize = 1;
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(7, 0x7000, false));
        CHECK(r.xfbImage(0x7000, img, why));
        CHECK(near3(img, 500, 300, 255, 0, 0));      // s,t in [0, 0.5) -> only texel (0,0)
    }

    // 5. The XFB copy's vertical filter {8,8,10,12,10,8,8}/64 on a white top half (rows 0..223) and black bottom half.
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        gx::EfbCopyCommand c = xfbCopy(8, 0x8000, false);
        c.clear.color = c.clear.alpha = c.clear.z = true; c.clear.rgba[3] = 255; c.clear.z24 = 0xFFFFFF;
        r.efbCopy(c);
        gx::PipelineState s = baseState();
        Quad q(-1, 0, 1, 1, -1.0f, white);
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(9, 0x9000, true));
        CHECK(r.xfbImage(0x9000, img, why));
        CHECK(near3(img, 10, 100, 255, 255, 255));                 // far from the edge: unchanged
        CHECK(near3(img, 10, 220, 255, 255, 255));                 // taps 217..223 all white
        CHECK(near3(img, 10, 221, 223, 223, 223, 1));              // taps 218..223 white = 56/64 * 255 = 223.1
        CHECK(near3(img, 10, 222, 191, 191, 191, 1));              // 48/64 * 255 = 191.25
        CHECK(near3(img, 10, 223, 151, 151, 151, 1));              // 38/64 * 255 = 151.4
        CHECK(near3(img, 10, 224, 104, 104, 104, 1));              // 26/64 * 255 = 103.6
        CHECK(near3(img, 10, 225, 64, 64, 64, 1));                 // 16/64 * 255 = 63.75
        CHECK(near3(img, 10, 226, 32, 32, 32, 1));                 // tap 223 only (k = 0): 8/64 * 255 = 31.9
        CHECK(near3(img, 10, 227, 0, 0, 0));                       // beyond the filter
    }

    // 5b. Fog on a white quad at depth 0.5 (z/w = -0.5), fog colour black. Parameters of GX_SetFog(near 1, far 3, start 1, end 3):
    //     a = far*near / ((far-near)*(end-start)) = 0.75, b = far/(far-near) = 1.5, c = start/(end-start) = 0.5.
    //     ze = a / (b - z) = 0.75 / 1.0 = 0.75, f = ze - c = 0.25; the curves: linear f, exp 1 - 2^(-8f), exp2 1 - 2^(-8f^2),
    //     reverse exp 2^(-8(1-f)), reverse exp2 2^(-8(1-f)^2). Colour = 255 * (1 - amount).
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        struct { gx::FogType t; double amount; } cases[] = {{gx::FogType::Linear, 0.25}, {gx::FogType::Exp, 0.75}, {gx::FogType::Exp2, 1.0 - std::pow(2.0, -0.5)},
                                                           {gx::FogType::RevExp, std::pow(2.0, -6.0)}, {gx::FogType::RevExp2, std::pow(2.0, -4.5)}};
        uint32_t addr = 0xA000;
        for (const auto& fc : cases) {
            gx::PipelineState s = baseState();
            s.fog.type = fc.t; s.fog.a = 0.75f; s.fog.b = 1.5f; s.fog.c = 0.5f;
            s.depth = {true, gx::CompareFunc::Always, false, false, gx::ZFormat::Linear};
            Quad q(-1, -1, 1, 1, -0.5f, white);
            r.draw(q.cmd(s));
            r.efbCopy(xfbCopy(20, addr, false));
            CHECK(r.xfbImage(addr, img, why));
            const int expect = static_cast<int>(std::lround(255.0 * (1.0 - fc.amount)));
            CHECK(near3(img, 100, 100, expect, expect, expect, 2));
            addr += 0x1000;
        }
        // depth 0 (near, z/w = -1): ze = 0.75 / 1.5 = 0.5, f = 0: no fog; depth 1 (z/w = 0): ze = 0.75 / 0.5 = 1.5, f = 1 (clamped): all fog
        gx::PipelineState s = baseState();
        s.fog.type = gx::FogType::Linear; s.fog.a = 0.75f; s.fog.b = 1.5f; s.fog.c = 0.5f;
        Quad nearQ(-1, -1, 0, 1, -1.0f, white), farQ(0, -1, 1, 1, -0.0f, white);
        r.draw(nearQ.cmd(s));
        r.draw(farQ.cmd(s));
        r.efbCopy(xfbCopy(21, 0xF000, false));
        CHECK(r.xfbImage(0xF000, img, why));
        CHECK(near3(img, 100, 100, 255, 255, 255));
        CHECK(near3(img, 500, 100, 0, 0, 0));
    }

    // 5b2. Points and lines expand to quads. A point of size 120 (1/6 pixel units) is a 20 pixel square centred on the vertex; a line of width 12
    //      is 2 pixels thick, centred on the line. Clip (0, 0) is window pixel (320, 224) in the 640x448 viewport.
    {
        gx::EfbCopyCommand c0 = xfbCopy(40, 0xE000, false);
        c0.clear.color = c0.clear.alpha = true; c0.clear.rgba[3] = 255;
        r.efbCopy(c0);
        const uint8_t red[4] = {255, 0, 0, 255};
        gx::PipelineState s = baseState();
        s.pointSize = 120; s.lineWidth = 12;
        Quad pq(0, 0, 0, 0, -1.0f, red);
        gx::DrawCommand pd = pq.cmd(s);
        pd.primitive = gx::Primitive::Points;
        const uint16_t one[1] = {0};
        pd.indices = one; pd.indexCount = 1; pd.vertexCount = 1;
        r.draw(pd);
        Quad lq(-0.5f, 0, 0.5f, 0, -1.0f, red);   // vertices 0 and 1 are (-0.5, 0) and (0.5, 0) (the quad's top edge)
        gx::DrawCommand ld = lq.cmd(s);
        ld.primitive = gx::Primitive::Lines;
        const uint16_t pair[2] = {2, 3};
        ld.indices = pair; ld.indexCount = 2;
        gx::Vertex* lv = const_cast<gx::Vertex*>(lq.v.data());
        for (int i = 2; i < 4; ++i) { lv[i].pos[1] = 0.5f; }   // lift the line away from the point: y = +0.5 -> window row 112
        r.draw(ld);
        r.efbCopy(xfbCopy(41, 0xE100, false));
        CHECK(r.xfbImage(0xE100, img, why));
        CHECK(near3(img, 320, 224, 255, 0, 0) && near3(img, 311, 215, 255, 0, 0) && near3(img, 328, 232, 255, 0, 0));   // inside the 310..329 x 214..233 square
        CHECK(near3(img, 305, 224, 0, 0, 0) && near3(img, 320, 210, 0, 0, 0) && near3(img, 335, 224, 0, 0, 0));
        CHECK(near3(img, 320, 111, 255, 0, 0) && near3(img, 320, 112, 255, 0, 0));     // the 2-pixel line covers rows 111 and 112
        CHECK(near3(img, 320, 110, 0, 0, 0) && near3(img, 320, 113, 0, 0, 0) && near3(img, 100, 111, 0, 0, 0) && near3(img, 500, 111, 0, 0, 0));
    }

    // 5b3. Render to texture, then composite: a 64x32 region (scissor 0,0-63,31) gets a red left half and a green right half, is copied to
    //      a texture (copy id 50), and a full-screen quad samples that copy through the binding's efbCopyId. Clip x in [-1, -0.8] is the
    //      region's 64 pixels (viewport 640 wide), y in [0.857143, 1] its 32 rows. The composite shows red on the left half of the screen.
    {
        gx::EfbCopyCommand c0 = xfbCopy(49, 0xE200, false);
        c0.clear.color = c0.clear.alpha = true; c0.clear.rgba[3] = 255;
        r.efbCopy(c0);
        const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, white[4] = {255, 255, 255, 255};
        gx::PipelineState s = baseState();
        s.scissor = {0, 0, 63, 31};
        const float y0 = 1.0f - 2.0f * 32.0f / 448.0f;
        Quad left(-1.0f, y0, -0.9f, 1.0f, -1.0f, red), right(-0.9f, y0, -0.8f, 1.0f, -1.0f, green);
        r.draw(left.cmd(s));
        r.draw(right.cmd(s));
        gx::EfbCopyCommand tc{};
        tc.id = 50; tc.target = gx::CopyTarget::Texture;
        tc.srcX = 0; tc.srcY = 0; tc.srcWidth = 64; tc.srcHeight = 32;
        tc.destAddr = 0x1000; tc.destWidth = 64; tc.destHeight = 32;
        tc.efbFormat = gx::PixelFormat::RGB8_Z24; tc.texFormat = 6; tc.yScale = 1.0f;
        tc.clear.color = tc.clear.alpha = true; tc.clear.rgba[0] = tc.clear.rgba[1] = tc.clear.rgba[2] = tc.clear.rgba[3] = 255;   // as the game: clear to white
        r.efbCopy(tc);
        gx::PipelineState c = baseState();
        gx::TevStage& st = c.tev.stages[0];
        st.texMap = 0; st.texCoord = 0;
        st.color.in[3] = static_cast<uint8_t>(gx::TevColorIn::TexC);
        st.alpha.in[3] = static_cast<uint8_t>(gx::TevAlphaIn::TexA);
        c.maps[0].valid = true; c.maps[0].efbCopyId = 50; c.maps[0].width = 64; c.maps[0].height = 32;
        c.maps[0].sampler = {gx::WrapMode::Clamp, gx::WrapMode::Clamp, gx::FilterMode::Near, false, 0, 0, 0, 1, false, false};
        c.texScale[0].sSize = 64; c.texScale[0].tSize = 32;
        // The game's viewport depth range as the core decodes it from the XF registers: near 1/1024 and far = 16777216 / 16777215, which is
        // just above 1.0. D3D silently ignores a viewport whose MaxDepth exceeds 1, leaving the 64x32 viewport of the copy pass in place.
        c.viewport.nearZ = 0.000976622f; c.viewport.farZ = 16777216.0f / 16777215.0f;
        Quad full(-1, -1, 1, 1, -1.0f, white);
        r.draw(full.cmd(c));
        r.efbCopy(xfbCopy(51, 0xE300, false));
        CHECK(r.xfbImage(0xE300, img, why));
        CHECK(near3(img, 100, 100, 255, 0, 0) && near3(img, 300, 400, 255, 0, 0));   // left half: the red of the region
        CHECK(near3(img, 400, 100, 0, 255, 0) && near3(img, 600, 400, 0, 255, 0));   // right half: green
        // the clear after the copy whitened the 64x32 region of the EFB itself, and the full-screen composite covers it again
        r.efbCopy(xfbCopy(52, 0xE400, false));
    }

    // 5c. Indirect texturing (the convention the renderer implements; scale not yet verified against hardware): a 4x1 texture sampled
    //     through an indirect lookup. Indirect texel (S, T, U) = (A, B, G) = (192, 128, 0); bias 128 on S and T gives (64, 0, 0); matrix element
    //     4.0 gives offset s = 4 * 64 / 256 = 1 texel. A pixel whose base texel is 0.625 samples texel 1, base 3.125 clamps to texel 3.
    {
        auto px = [](uint8_t r8, uint8_t g8, uint8_t b8, uint8_t a8) { return uint32_t(a8) << 24 | uint32_t(b8) << 16 | uint32_t(g8) << 8 | r8; };
        auto m = std::make_shared<gx::Texture>();
        m->key = 0x5001; m->width = 4; m->height = 1; m->levelCount = 1; m->levels[0] = {4, 1, 0};
        m->rgba = {px(255, 0, 0, 255), px(0, 255, 0, 255), px(0, 0, 255, 255), px(255, 255, 0, 255)};
        auto ind = std::make_shared<gx::Texture>();
        ind->key = 0x5002; ind->width = 1; ind->height = 1; ind->levelCount = 1; ind->levels[0] = {1, 1, 0};
        ind->rgba = {px(0, 0, 128, 192)};
        gx::PipelineState s = baseState();
        gx::TevStage& st = s.tev.stages[0];
        st.texMap = 0; st.texCoord = 0;
        st.color.in[3] = static_cast<uint8_t>(gx::TevColorIn::TexC);
        st.alpha.in[3] = static_cast<uint8_t>(gx::TevAlphaIn::TexA);
        st.indirect = {0, 0, 3, 0, 1, 0, 0, false, false};
        s.tev.numIndirect = 1;
        s.tev.indirectStage[0] = {1, 0, 0, 0};
        s.tev.indirectMatrix[0][0][0] = 4.0f;
        s.maps[0].valid = true; s.maps[0].image = m; s.maps[0].width = 4; s.maps[0].height = 1;
        s.maps[0].sampler = {gx::WrapMode::Clamp, gx::WrapMode::Clamp, gx::FilterMode::Near, false, 0, 0, 0, 1, false, false};
        s.maps[1].valid = true; s.maps[1].image = ind; s.maps[1].width = 1; s.maps[1].height = 1;
        s.maps[1].sampler = s.maps[0].sampler;
        s.texScale[0].sSize = 4; s.texScale[0].tSize = 1;
        const uint8_t white[4] = {255, 255, 255, 255};
        Quad q(-1, -1, 1, 1, -1.0f, white);
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(30, 0xB000, false));
        CHECK(r.xfbImage(0xB000, img, why));
        CHECK(near3(img, 100, 100, 0, 255, 0));      // base 0.625 + 1 -> texel 1
        CHECK(near3(img, 300, 100, 0, 0, 255));      // 1.875 + 1 -> texel 2
        CHECK(near3(img, 500, 100, 255, 255, 0));    // 3.125 + 1 = 4.125 -> clamped to texel 3
        // wrap code 6 zeroes the base coordinate: every pixel samples texel 0 + 1
        s.tev.stages[0].indirect.wrapS = 6; s.tev.stages[0].indirect.wrapT = 6;
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(31, 0xC000, false));
        CHECK(r.xfbImage(0xC000, img, why));
        CHECK(near3(img, 100, 100, 0, 255, 0) && near3(img, 500, 100, 0, 255, 0));
        // matrix 0 (none) gives no offset: the texture appears undistorted
        s.tev.stages[0].indirect.wrapS = s.tev.stages[0].indirect.wrapT = 0; s.tev.stages[0].indirect.matrix = 0;
        r.draw(q.cmd(s));
        r.efbCopy(xfbCopy(32, 0xD000, false));
        CHECK(r.xfbImage(0xD000, img, why));
        CHECK(near3(img, 100, 100, 255, 0, 0) && near3(img, 300, 100, 0, 255, 0) && near3(img, 500, 100, 255, 255, 0));
    }

    // 6. Not rendered exactly means reported, never silently ignored.
    {
        const uint8_t red[4] = {255, 0, 0, 255};
        const uint64_t skippedBefore = r.stats().drawsSkipped;
        gx::PipelineState s = baseState();
        s.fog.type = gx::FogType::Linear; s.fog.rangeAdjustEnable = true;
        Quad q(-1, -1, 1, 1, -1.0f, red);
        r.draw(q.cmd(s));
        gx::PipelineState s4 = baseState();
        s4.fog.type = gx::FogType::Exp; s4.fog.orthographic = true;
        r.draw(q.cmd(s4));
        gx::PipelineState s2 = baseState();
        s2.depth.test = true; s2.depth.compareBeforeTexture = true;
        s2.alphaCompare.comp0 = gx::CompareFunc::Greater;
        r.draw(q.cmd(s2));
        gx::PipelineState s3 = baseState();
        s3.blend.mode = gx::BlendMode::Logic; s3.blend.logicOp = gx::LogicOp::Xor;
        r.draw(q.cmd(s3));
        CHECK(r.stats().drawsSkipped == skippedBefore + 4);
        bool ortho = false, fog = false, lines = false, logic = false;
        for (const auto& u : r.unsupported()) {
            fog |= u.first.find("range adjustment") != std::string::npos;
            ortho |= u.first.find("orthographic") != std::string::npos;
            lines |= u.first.find("early depth test") != std::string::npos;
            logic |= u.first.find("logic op") != std::string::npos;
        }
        CHECK(fog && ortho && lines && logic);
    }

    r.printSummary();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    std::printf("gfx_renderer_tests: all checks passed\n");
    return 0;
}
