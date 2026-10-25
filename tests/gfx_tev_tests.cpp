// TEV shader generation: key identity, loud rejection of what is not generated yet, compilation through the cache, and
// small offscreen draws whose expected pixels are worked out by hand from the GX combiner equation
//     out = (d (+|-) ((1 - c) * a + c * b) + bias) * scale
// (libogc gx.h, GX_SetTevColorOp). No window is opened.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gfx/gfx_output.h"
#include "gfx/gfx_shader_cache.h"

using namespace awl::gfx;
using namespace awl::gfx::tev;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// ---- key and generator rules -----------------------------------------------------------------------------------------
static PixelShaderKey passRaster() {   // PREV = raster colour and alpha
    PixelShaderKey k;
    k.numStages = 1;
    Stage& s = k.stages[0];
    s.colD = CC_RASC;
    s.alpD = CA_RASA;
    s.rasChannel = RAS_COLOR0;
    return k;
}

static void testKeys() {
    PixelShaderKey a = passRaster(), b = passRaster();
    CHECK(keyBytes(a) == keyBytes(b) && keyHash(a) == keyHash(b));
    CHECK(keyBytes(a).size() == sizeof(PixelShaderKey));
    b.stages[0].colScale = SCALE_2;
    CHECK(keyBytes(a) != keyBytes(b) && keyHash(a) != keyHash(b));
    PixelShaderKey c = passRaster();
    c.alphaComp0 = 4;
    CHECK(keyBytes(a) != keyBytes(c));
}

static bool rejects(const PixelShaderKey& k, const char* word) {
    const HlslResult r = generatePixelShader(k);
    if (r.ok || r.unsupported.find(word) == std::string::npos) {
        std::fprintf(stderr, "expected rejection mentioning '%s', got ok=%d reason='%s'\n", word, r.ok, r.unsupported.c_str());
        return false;
    }
    return true;
}

static void testRejections() {
    PixelShaderKey k = passRaster();
    CHECK(generatePixelShader(k).ok);
    k = passRaster(); k.fogType = 2;                          CHECK(rejects(k, "fog"));
    k = passRaster(); k.efbFormat = 1;                        CHECK(rejects(k, "EFB pixel format"));
    k = passRaster(); k.stages[0].colCmp = CMP_R8_GT;         CHECK(rejects(k, "compare"));
    k = passRaster(); k.stages[0].alpCmp = CMP_RGB8_EQ;       CHECK(rejects(k, "compare"));
    k = passRaster(); k.stages[0].indStage = 0;               CHECK(rejects(k, "indirect"));
    k = passRaster(); k.stages[0].rasChannel = RAS_ALPHA_BUMP; CHECK(rejects(k, "bump"));
    k = passRaster(); k.numStages = 0;                        CHECK(rejects(k, "stage count"));
    k = passRaster(); k.numStages = 17;                       CHECK(rejects(k, "stage count"));
    k = passRaster(); k.stages[0].colA = CC_TEXC;             CHECK(rejects(k, "no texture"));
    k = passRaster(); k.stages[0].texMap = 0;                 CHECK(rejects(k, "both"));
    k = passRaster(); k.stages[0].kColorSel = 9;              CHECK(rejects(k, "KCSEL"));
    k = passRaster(); k.alphaComp0 = 9;                       CHECK(rejects(k, "alpha compare"));
    k = passRaster(); k.reserved = 1;                         CHECK(rejects(k, "reserved"));
    // sixteen stages are fine
    k = passRaster(); k.numStages = 16;
    CHECK(generatePixelShader(k).ok);
}

// ---- draw fixture ----------------------------------------------------------------------------------------------------
struct Fixture {
    Device dev;
    OffscreenTarget target;
    PixelShaderCache cache;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb, vcb, pcb;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> point;
    std::string err;

    bool init() {
        DeviceOptions o;
        if (!dev.create(o, err) || !target.create(dev, 4, 4, err)) return false;
        ComPtr<ID3DBlob> code;
        if (!compileHlsl(vertexShaderHlsl(), "main", "vs_5_0", code, err)) return false;
        ID3D11Device* d = dev.device();
        if (FAILED(d->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, vs.GetAddressOf()))) { err = "vs"; return false; }
        std::vector<D3D11_INPUT_ELEMENT_DESC> el = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, col0), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, col1), D3D11_INPUT_PER_VERTEX_DATA, 0}};
        for (UINT i = 0; i < 8; ++i)
            el.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, uv) + i * 12), D3D11_INPUT_PER_VERTEX_DATA, 0});
        if (FAILED(d->CreateInputLayout(el.data(), static_cast<UINT>(el.size()), code->GetBufferPointer(), code->GetBufferSize(), layout.GetAddressOf()))) { err = "layout"; return false; }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(Vertex) * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(d->CreateBuffer(&bd, nullptr, vb.GetAddressOf()))) { err = "vb"; return false; }
        bd.ByteWidth = sizeof(VertexConstants); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(d->CreateBuffer(&bd, nullptr, vcb.GetAddressOf()))) { err = "vcb"; return false; }
        bd.ByteWidth = sizeof(TevConstants);
        if (FAILED(d->CreateBuffer(&bd, nullptr, pcb.GetAddressOf()))) { err = "pcb"; return false; }
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        d->CreateRasterizerState(&rd, raster.GetAddressOf());
        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        d->CreateBlendState(&bld, blend.GetAddressOf());
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        d->CreateDepthStencilState(&dsd, depth.GetAddressOf());
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        d->CreateSamplerState(&sd, point.GetAddressOf());
        return true;
    }

    // Clears the 4x4 target to (0,0,255,255) and draws a full-target quad. col0 is the raster colour (0-255 per channel),
    // uv0 is (0.5, 0.5); `tex` (optional) is bound to texture 0.
    bool draw(const PixelShaderKey& key, const float k0[4], const float tevreg0[4], const uint8_t col0[4], float alphaRef0, ID3D11ShaderResourceView* tex, Image& out, float alphaRef1 = 0.0f) {
        ID3D11PixelShader* ps = cache.get(dev, key, err);
        if (!ps) { std::fprintf(stderr, "shader: %s\n", err.c_str()); return false; }
        Vertex v[4] = {};
        const float xy[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
        for (int i = 0; i < 4; ++i) {
            v[i].pos[0] = xy[i][0]; v[i].pos[1] = xy[i][1]; v[i].pos[2] = -1.0f; v[i].pos[3] = 1.0f;   // z = -w: the GX near plane
            for (int c = 0; c < 4; ++c) v[i].col0[c] = col0[c] / 255.0f;
            for (int u = 0; u < 8; ++u) { v[i].uv[u][0] = 0.5f; v[i].uv[u][1] = 0.5f; v[i].uv[u][2] = 1.0f; }
        }
        ID3D11DeviceContext* c = dev.context();
        c->UpdateSubresource(vb.Get(), 0, nullptr, v, 0, 0);
        VertexConstants vc = {{0.0f, 1.0f, 0, 0}};
        c->UpdateSubresource(vcb.Get(), 0, nullptr, &vc, 0, 0);
        TevConstants pc = {};
        std::memcpy(pc.kcolor[0], k0, 16);
        std::memcpy(pc.tevreg[0], tevreg0, 16);
        pc.alphaRef[0] = alphaRef0;
        pc.alphaRef[1] = alphaRef1;
        c->UpdateSubresource(pcb.Get(), 0, nullptr, &pc, 0, 0);
        const float clear[4] = {0, 0, 1, 1};
        c->ClearRenderTargetView(target.rtv(), clear);
        D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
        ID3D11RenderTargetView* rtv = target.rtv();
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->RSSetViewports(1, &vp);
        c->RSSetState(raster.Get());
        c->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFFu);
        c->OMSetDepthStencilState(depth.Get(), 0);
        c->IASetInputLayout(layout.Get());
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        UINT stride = sizeof(Vertex), offset = 0;
        ID3D11Buffer* vbp = vb.Get();
        c->IASetVertexBuffers(0, 1, &vbp, &stride, &offset);
        c->VSSetShader(vs.Get(), nullptr, 0);
        ID3D11Buffer* b = vcb.Get();
        c->VSSetConstantBuffers(0, 1, &b);
        c->PSSetShader(ps, nullptr, 0);
        b = pcb.Get();
        c->PSSetConstantBuffers(0, 1, &b);
        ID3D11SamplerState* smp = point.Get();
        c->PSSetSamplers(0, 1, &smp);
        c->PSSetShaderResources(0, 1, &tex);
        c->Draw(4, 0);
        ID3D11ShaderResourceView* none = nullptr;
        c->PSSetShaderResources(0, 1, &none);
        return target.readback(dev, out, err);
    }
};

static bool px(const Image& img, int r, int g, int b, int a) {
    const uint8_t* p = img.px(2, 2);
    if (std::abs(p[0] - r) > 1 || std::abs(p[1] - g) > 1 || std::abs(p[2] - b) > 1 || std::abs(p[3] - a) > 1) {
        std::fprintf(stderr, "pixel (%d,%d,%d,%d), expected (%d,%d,%d,%d)\n", p[0], p[1], p[2], p[3], r, g, b, a);
        return false;
    }
    return true;
}

static void testDraws() {
    Fixture f;
    if (!f.init()) { std::fprintf(stderr, "fixture: %s\n", f.err.c_str()); ++g_failures; return; }
    std::printf("gfx_tev_tests: %s device %s\n", f.dev.isWarp() ? "WARP" : "hardware", f.dev.adapterName().c_str());
    const float zero4[4] = {0, 0, 0, 0};
    const float k0[4] = {128 / 255.0f, 64 / 255.0f, 255 / 255.0f, 1.0f};
    const uint8_t ras[4] = {200, 100, 50, 100};
    Image out;

    // PASSCLR: d = RASC, alpha d = RASA -> the raster colour unchanged
    {
        PixelShaderKey k = passRaster();
        CHECK(f.draw(k, k0, zero4, ras, 0, nullptr, out));
        CHECK(px(out, 200, 100, 50, 100));
    }
    // MODULATE with a konst: lerp(0, ras, kc) = ras * kc. 200*128/255 = 100.39, 100*64/255 = 25.10, 50*255/255 = 50
    {
        PixelShaderKey k = passRaster();
        Stage& s = k.stages[0];
        s.colA = CC_ZERO; s.colB = CC_RASC; s.colC = CC_KONST; s.colD = CC_ZERO;
        s.kColorSel = 12;   // GX_TEV_KCSEL_K0
        CHECK(f.draw(k, k0, zero4, ras, 0, nullptr, out));
        CHECK(px(out, 100, 25, 50, 100));
    }
    // bias and scale with clamping: (ras - 0.5) * 2. R: (0.78431-0.5)*2 = 0.56863 -> 145; G, B negative -> clamped to 0
    {
        PixelShaderKey k = passRaster();
        Stage& s = k.stages[0];
        s.colBias = BIAS_MINUS_HALF; s.colScale = SCALE_2;
        CHECK(f.draw(k, k0, zero4, ras, 0, nullptr, out));
        CHECK(px(out, 145, 0, 0, 100));
    }
    // subtract: 1 - ras = (55, 155, 205); alpha scale 4 on a quarter of 0.5 (konst 1/8 * 4 = 0.5 -> 128)
    {
        PixelShaderKey k = passRaster();
        Stage& s = k.stages[0];
        s.colA = CC_ZERO; s.colB = CC_RASC; s.colC = CC_ONE; s.colD = CC_ONE; s.colSub = 1;
        s.alpD = CA_KONST; s.kAlphaSel = 7; s.alpScale = SCALE_4;   // GX_TEV_KASEL_1_8
        CHECK(f.draw(k, k0, zero4, ras, 0, nullptr, out));
        CHECK(px(out, 55, 155, 205, 128));
    }
    // two stages through a register: stage 0 writes REG0 = ras (colour only), stage 1 PREV = REG0 * HALF = ras / 2 (alpha from stage 0's PREV = 0)
    {
        PixelShaderKey k = passRaster();
        k.numStages = 2;
        Stage& s0 = k.stages[0];
        s0.colDest = DEST_REG0; s0.alpD = CA_ZERO; s0.alpDest = DEST_REG0;
        Stage& s1 = k.stages[1];
        s1.colA = CC_ZERO; s1.colB = CC_C0; s1.colC = CC_HALF; s1.colD = CC_ZERO;
        s1.alpD = CA_ZERO;
        CHECK(f.draw(k, k0, zero4, ras, 0, nullptr, out));
        CHECK(px(out, 100, 50, 25, 0));
    }
    // alpha compare GREATER 128: alpha 100 fails -> pixel discarded, target keeps its clear colour (0,0,255,255)
    {
        PixelShaderKey k = passRaster();
        k.alphaComp0 = 4; k.alphaComp1 = 7; k.alphaLogic = 0;   // GX_GREATER, GX_ALWAYS, GX_AOP_AND
        CHECK(f.draw(k, k0, zero4, ras, 128.0f, nullptr, out));
        CHECK(px(out, 0, 0, 255, 255));
        const uint8_t bright[4] = {200, 100, 50, 200};
        CHECK(f.draw(k, k0, zero4, bright, 128.0f, nullptr, out));
        CHECK(px(out, 200, 100, 50, 200));
        // OR with EQUAL 100: alpha 100 fails GREATER 128 but passes EQUAL 100
        k.alphaComp1 = 2; k.alphaLogic = 1;
        CHECK(f.draw(k, k0, zero4, ras, 128.0f, nullptr, out, 100.0f));
        CHECK(px(out, 200, 100, 50, 100));
        // XOR: exactly one of the two tests true passes, both true discards
        k.alphaLogic = 2;
        CHECK(f.draw(k, k0, zero4, ras, 128.0f, nullptr, out, 100.0f));
        CHECK(px(out, 200, 100, 50, 100));
        CHECK(f.draw(k, k0, zero4, ras, 50.0f, nullptr, out, 100.0f));   // 100 > 50 true XOR 100 == 100 true -> discarded
        CHECK(px(out, 0, 0, 255, 255));
        // XNOR: both tests equal passes, differing discards
        k.alphaLogic = 3;
        CHECK(f.draw(k, k0, zero4, ras, 50.0f, nullptr, out, 100.0f));   // true, true -> pass
        CHECK(px(out, 200, 100, 50, 100));
        CHECK(f.draw(k, k0, zero4, ras, 128.0f, nullptr, out, 7.0f));    // false, false -> pass
        CHECK(px(out, 200, 100, 50, 100));
        CHECK(f.draw(k, k0, zero4, ras, 128.0f, nullptr, out, 100.0f));  // false, true -> discarded
        CHECK(px(out, 0, 0, 255, 255));
    }
    // texture: a 1x1 texel (255,128,0,255) through swap table 1 = (B,G,R,A) -> (0,128,255,255), modulated by the raster colour white
    {
        Image t;
        t.resize(2, 2);
        t.fill(255, 128, 0, 255);
        ImageTexture tex;
        std::string e;
        CHECK(tex.update(f.dev, t, e));
        PixelShaderKey k;
        k.numStages = 1;
        k.swaps[1].r = 2; k.swaps[1].g = 1; k.swaps[1].b = 0; k.swaps[1].a = 3;
        Stage& s = k.stages[0];
        s.texMap = 0; s.texCoord = 0; s.texSwap = 1; s.rasChannel = RAS_COLOR0;
        s.colA = CC_ZERO; s.colB = CC_TEXC; s.colC = CC_RASC; s.colD = CC_ZERO;   // tex * ras
        s.alpA = CA_ZERO; s.alpB = CA_TEXA; s.alpC = CA_RASA; s.alpD = CA_ZERO;
        const uint8_t white[4] = {255, 255, 255, 255};
        CHECK(f.draw(k, k0, zero4, white, 0, tex.srv(), out));
        CHECK(px(out, 0, 128, 255, 255));
        const uint8_t half[4] = {128, 128, 128, 128};   // 128/255 = 0.50196: tex * 0.50196 = (0, 64.25, 128) alpha 128.25
        CHECK(f.draw(k, k0, zero4, half, 0, tex.srv(), out));
        CHECK(px(out, 0, 64, 128, 128));
    }
    // cache: every distinct key compiled once, repeated keys hit
    const PixelShaderCache::Stats& st = f.cache.stats();
    CHECK(st.compiled == f.cache.size() && st.rejected == 0);
    CHECK(st.hits > 0 && st.lookups == st.hits + st.compiled);
    // an unsupported key is cached as a failure with its reason
    PixelShaderKey bad = passRaster();
    bad.fogType = 4;
    std::string e;
    CHECK(f.cache.get(f.dev, bad, e) == nullptr && e.find("fog") != std::string::npos);
    e.clear();
    CHECK(f.cache.get(f.dev, bad, e) == nullptr && e.find("fog") != std::string::npos);
    CHECK(f.cache.stats().rejected == 1);
}

int main() {
    testKeys();
    testRejections();
    testDraws();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    std::printf("gfx_tev_tests: all checks passed\n");
    return 0;
}
