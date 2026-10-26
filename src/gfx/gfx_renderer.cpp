#include "gfx/gfx_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <cstring>

#include "gfx/gfx_tev.h"
#include <windows.h>

namespace awl::gfx {

namespace {

const char* kCopyHlsl = R"(
Texture2D efb : register(t0);
cbuffer C : register(b0) {
    int4 src;      // x, y, w, h of the EFB rectangle
    int4 dst;      // w, h of the result, first and last EFB row the filter may read
    float4 f0;     // vertical filter taps 0-3 (each already divided by 64)
    float4 f1;     // taps 4-6, w = 1: 2x2 box average instead (half-scale texture copies)
};
struct VOut { float4 pos : SV_Position; };
VOut vs(uint id : SV_VertexID) {
    VOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    return o;
}
float4 ps(VOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float4 acc;
    if (f1.w > 0.5) {
        int sx = src.x + 2 * p.x, sy = src.y + 2 * p.y;
        acc = (efb.Load(int3(sx, sy, 0)) + efb.Load(int3(sx + 1, sy, 0)) + efb.Load(int3(sx, sy + 1, 0)) + efb.Load(int3(sx + 1, sy + 1, 0))) * 0.25;
    } else {
        int sx = src.x + ((2 * p.x + 1) * src.z) / (2 * dst.x);
        int sy = src.y + ((2 * p.y + 1) * src.w) / (2 * dst.y);
        float c[7] = { f0.x, f0.y, f0.z, f0.w, f1.x, f1.y, f1.z };
        acc = float4(0, 0, 0, 0);
        [unroll] for (int k = 0; k < 7; ++k) {
            int y = clamp(sy + k - 3, dst.z, dst.w);
            acc += c[k] * efb.Load(int3(sx, y, 0));
        }
    }
    return acc;
}
)";

const char* kClearHlsl = R"(
cbuffer C : register(b0) { float4 color; float4 z; };
struct VOut { float4 pos : SV_Position; };
VOut vs(uint id : SV_VertexID) {
    VOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, z.x, 1.0);
    return o;
}
float4 ps(VOut i) : SV_Target { return color; }
)";

D3D11_COMPARISON_FUNC comparison(gx::CompareFunc f) {
    switch (f) {
        case gx::CompareFunc::Never: return D3D11_COMPARISON_NEVER;
        case gx::CompareFunc::Less: return D3D11_COMPARISON_LESS;
        case gx::CompareFunc::Equal: return D3D11_COMPARISON_EQUAL;
        case gx::CompareFunc::LEqual: return D3D11_COMPARISON_LESS_EQUAL;
        case gx::CompareFunc::Greater: return D3D11_COMPARISON_GREATER;
        case gx::CompareFunc::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
        case gx::CompareFunc::GEqual: return D3D11_COMPARISON_GREATER_EQUAL;
        default: return D3D11_COMPARISON_ALWAYS;
    }
}

D3D11_BLEND factor(gx::BlendFactor f, bool isSource) {
    switch (f) {
        case gx::BlendFactor::Zero: return D3D11_BLEND_ZERO;
        case gx::BlendFactor::One: return D3D11_BLEND_ONE;
        case gx::BlendFactor::OtherColor: return isSource ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
        case gx::BlendFactor::InvOtherColor: return isSource ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
        case gx::BlendFactor::SrcAlpha: return D3D11_BLEND_SRC_ALPHA;
        case gx::BlendFactor::InvSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
        case gx::BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
        default: return D3D11_BLEND_INV_DEST_ALPHA;
    }
}
// The alpha channel blends with the colour factors replaced by their alpha counterparts (D3D forbids colour factors there).
D3D11_BLEND alphaFactor(D3D11_BLEND f) {
    switch (f) {
        case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
        case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
        default: return f;
    }
}

template <class T> std::string bytesOf(const T& v) { return std::string(reinterpret_cast<const char*>(&v), sizeof v); }

// IR pipeline state -> pixel shader key; returns false with the reason when the state is not expressible yet.
bool buildKey(const gx::PipelineState& s, tev::PixelShaderKey& k, std::string& why) {
    using namespace tev;
    if (s.tev.numStages < 1 || s.tev.numStages > 16) { why = "TEV stage count outside 1-16"; return false; }
    k.numStages = s.tev.numStages;
    for (uint32_t i = 0; i < s.tev.numStages; ++i) {
        const gx::TevStage& t = s.tev.stages[i];
        Stage& o = k.stages[i];
        o.colA = t.color.in[0]; o.colB = t.color.in[1]; o.colC = t.color.in[2]; o.colD = t.color.in[3];
        o.colSub = t.color.op == gx::TevOp::Sub; o.colBias = static_cast<uint8_t>(t.color.bias); o.colScale = static_cast<uint8_t>(t.color.scale);
        o.colClamp = t.color.clamp; o.colDest = static_cast<uint8_t>(t.color.dest);
        o.colCmp = static_cast<uint8_t>(t.color.op >= gx::TevOp::CompR8GT ? static_cast<uint8_t>(t.color.op) - 7 : 0);
        o.alpA = t.alpha.in[0]; o.alpB = t.alpha.in[1]; o.alpC = t.alpha.in[2]; o.alpD = t.alpha.in[3];
        o.alpSub = t.alpha.op == gx::TevOp::Sub; o.alpBias = static_cast<uint8_t>(t.alpha.bias); o.alpScale = static_cast<uint8_t>(t.alpha.scale);
        o.alpClamp = t.alpha.clamp; o.alpDest = static_cast<uint8_t>(t.alpha.dest);
        o.alpCmp = static_cast<uint8_t>(t.alpha.op >= gx::TevOp::CompR8GT ? static_cast<uint8_t>(t.alpha.op) - 7 : 0);
        o.texMap = t.texMap; o.texCoord = t.texCoord;
        o.rasChannel = static_cast<uint8_t>(t.ras);
        o.kColorSel = t.konstColor; o.kAlphaSel = t.konstAlpha;
        o.rasSwap = t.rasSwap; o.texSwap = t.texSwap;
        o.indStage = t.indirect.indStage;
        if (t.indirect.indStage != gx::kNoMap) {
            o.indFormat = t.indirect.format; o.indBias = t.indirect.biasMask; o.indMatrix = t.indirect.matrix;
            o.indWrapS = t.indirect.wrapS; o.indWrapT = t.indirect.wrapT; o.indAddPrev = t.indirect.addPrevious; o.indAlpha = t.indirect.alphaSelect;
        }
    }
    for (uint32_t i = 0; i < 4 && i < s.tev.numIndirect; ++i) {
        k.ind[i].texMap = s.tev.indirectStage[i].texMap; k.ind[i].texCoord = s.tev.indirectStage[i].texCoord;
        k.ind[i].scaleS = s.tev.indirectStage[i].scaleS; k.ind[i].scaleT = s.tev.indirectStage[i].scaleT;
    }
    for (int t = 0; t < 4; ++t) { k.swaps[t].r = s.tev.swap[t][0]; k.swaps[t].g = s.tev.swap[t][1]; k.swaps[t].b = s.tev.swap[t][2]; k.swaps[t].a = s.tev.swap[t][3]; }
    k.projectiveMask = 0xFF;   // IR convention 4: the texture unit always divides by q (q = 1 for ST texgens)
    static const bool noAlpha = std::getenv("AWL_GFX_DEBUG_NOALPHA") != nullptr;   // debugging aid: ignore the alpha test
    k.alphaComp0 = static_cast<uint8_t>(s.alphaCompare.comp0);
    if (noAlpha) { k.alphaComp0 = k.alphaComp1 = 7; k.alphaLogic = 0; }
    if (!noAlpha) { k.alphaComp1 = static_cast<uint8_t>(s.alphaCompare.comp1); k.alphaLogic = static_cast<uint8_t>(s.alphaCompare.logic); }
    k.fogType = static_cast<uint8_t>(s.fog.type);
    k.fogOrtho = s.fog.orthographic;
    k.efbFormat = static_cast<uint8_t>(s.efbFormat);
    return true;
}

}  // namespace

// Debugging aid: number of EFB pixels whose colour is not opaque black.
static size_t countLit(Device& dev, ID3D11Texture2D* tex, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev.device()->CreateTexture2D(&d, nullptr, st.GetAddressOf()))) return 0;
    dev.context()->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev.context()->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return 0;
    size_t lit = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) { const uint8_t* p = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + x * 4; if (p[0] | p[1] | p[2]) ++lit; }
    dev.context()->Unmap(st.Get(), 0);
    return lit;
}

void Renderer::report(const std::string& reason) {
    uint64_t& n = unsupported_[reason];
    if (n++ == 0) std::fprintf(stderr, "gfx: not rendered exactly: %s (counted, reported once)\n", reason.c_str());
}

void Renderer::printSummary() const {
    std::printf("renderer: %llu draws (%llu skipped), %llu triangles, %llu EFB copies (%llu approximated), %llu textures uploaded (%.1f MB, %llu evicted); "
                "shaders %zu compiled, %llu rejected, %llu cache hits\n",
                (unsigned long long)stats_.draws, (unsigned long long)stats_.drawsSkipped, (unsigned long long)stats_.triangles,
                (unsigned long long)stats_.copies, (unsigned long long)stats_.copiesApproximated, (unsigned long long)stats_.texturesUploaded,
                stats_.textureBytes / 1048576.0, (unsigned long long)stats_.texturesEvicted, shaders_.size(), (unsigned long long)shaders_.stats().rejected, (unsigned long long)shaders_.stats().hits);
    for (const auto& u : unsupported_) std::printf("renderer: not exact: %s x %llu\n", u.first.c_str(), (unsigned long long)u.second);
}

bool Renderer::createGpu(Gpu& g, uint32_t w, uint32_t h, bool renderTarget) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(dev_->device()->CreateTexture2D(&d, nullptr, g.tex.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dev_->device()->CreateShaderResourceView(g.tex.Get(), nullptr, g.srv.ReleaseAndGetAddressOf()))) return false;
    g.w = w; g.h = h;
    return true;
}

bool Renderer::init(Device& dev, std::string& err) {
    dev_ = &dev;
    ID3D11Device* d = dev.device();
    if (!createGpu(efb_, kEfbWidth, kEfbHeight, true)) { err = "EFB colour target"; return false; }
    if (FAILED(d->CreateRenderTargetView(efb_.tex.Get(), nullptr, efbRtv_.GetAddressOf()))) { err = "EFB RTV"; return false; }
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = kEfbWidth; dd.Height = kEfbHeight; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(d->CreateTexture2D(&dd, nullptr, depth_.GetAddressOf()))) { err = "EFB depth target"; return false; }
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(d->CreateDepthStencilView(depth_.Get(), &dv, dsv_.GetAddressOf()))) { err = "EFB DSV"; return false; }

    ComPtr<ID3DBlob> code;
    if (!compileHlsl(tev::vertexShaderHlsl(), "main", "vs_5_0", code, err)) return false;
    if (FAILED(d->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, vs_.GetAddressOf()))) { err = "vertex shader"; return false; }
    std::vector<D3D11_INPUT_ELEMENT_DESC> el = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(tev::Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(tev::Vertex, col0), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(tev::Vertex, col1), D3D11_INPUT_PER_VERTEX_DATA, 0}};
    for (UINT i = 0; i < 8; ++i)
        el.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(tev::Vertex, uv) + i * 12), D3D11_INPUT_PER_VERTEX_DATA, 0});
    if (FAILED(d->CreateInputLayout(el.data(), static_cast<UINT>(el.size()), code->GetBufferPointer(), code->GetBufferSize(), layout_.GetAddressOf()))) { err = "input layout"; return false; }

    ComPtr<ID3DBlob> c2;
    if (!compileHlsl(kCopyHlsl, "vs", "vs_5_0", c2, err)) return false;
    if (FAILED(d->CreateVertexShader(c2->GetBufferPointer(), c2->GetBufferSize(), nullptr, copyVs_.GetAddressOf()))) { err = "copy vs"; return false; }
    if (!compileHlsl(kCopyHlsl, "ps", "ps_5_0", c2, err)) return false;
    if (FAILED(d->CreatePixelShader(c2->GetBufferPointer(), c2->GetBufferSize(), nullptr, copyPs_.GetAddressOf()))) { err = "copy ps"; return false; }
    if (!compileHlsl(kClearHlsl, "vs", "vs_5_0", c2, err)) return false;
    if (FAILED(d->CreateVertexShader(c2->GetBufferPointer(), c2->GetBufferSize(), nullptr, clearVs_.GetAddressOf()))) { err = "clear vs"; return false; }
    if (!compileHlsl(kClearHlsl, "ps", "ps_5_0", c2, err)) return false;
    if (FAILED(d->CreatePixelShader(c2->GetBufferPointer(), c2->GetBufferSize(), nullptr, clearPs_.GetAddressOf()))) { err = "clear ps"; return false; }

    auto cbuffer = [&](ComPtr<ID3D11Buffer>& b, UINT bytes) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (bytes + 15) & ~15u; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(d->CreateBuffer(&bd, nullptr, b.GetAddressOf()));
    };
    if (!cbuffer(tevCb_, sizeof(tev::TevConstants)) || !cbuffer(copyCb_, 64) || !cbuffer(clearCb_, 32)) { err = "constant buffers"; return false; }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;   // culling is done by the GX core (IR convention 6)
    rd.ScissorEnable = TRUE;
    d->CreateRasterizerState(&rd, raster_.GetAddressOf());
    rd.ScissorEnable = FALSE;
    d->CreateRasterizerState(&rd, rasterNoScissor_.GetAddressOf());
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d->CreateBlendState(&bl, noBlend_.GetAddressOf());
    D3D11_DEPTH_STENCIL_DESC ds = {};
    d->CreateDepthStencilState(&ds, noDepth_.GetAddressOf());

    // Defined initial EFB contents: opaque black, far depth.
    const float black[4] = {0, 0, 0, 1};
    dev.context()->ClearRenderTargetView(efbRtv_.Get(), black);
    dev.context()->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return true;
}

// Appends to a dynamic buffer used as a ring: NO_OVERWRITE while there is room, DISCARD (a fresh buffer behind the scenes) when it wraps.
bool Renderer::upload(ComPtr<ID3D11Buffer>& buf, uint32_t& capacity, uint32_t& cursor, uint32_t bind, const void* data, uint32_t bytes, uint32_t& offset) {
    if (!buf || bytes > capacity) {
        capacity = std::max<uint32_t>(bytes, std::max<uint32_t>(capacity * 2, 4u << 20));
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = capacity; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = bind; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->device()->CreateBuffer(&bd, nullptr, buf.ReleaseAndGetAddressOf()))) return false;
        cursor = capacity;   // forces the discard path below
    }
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (cursor + bytes > capacity) { mode = D3D11_MAP_WRITE_DISCARD; cursor = 0; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev_->context()->Map(buf.Get(), 0, mode, 0, &m))) return false;
    std::memcpy(static_cast<uint8_t*>(m.pData) + cursor, data, bytes);
    dev_->context()->Unmap(buf.Get(), 0);
    offset = cursor;
    cursor += (bytes + 15) & ~15u;
    return true;
}

bool Renderer::uploadTexture(const gx::Texture& t, Gpu& out) {
    uint32_t levels = std::max<uint32_t>(1, t.levelCount);
    // The mip chain must be the D3D one (each level half the previous, minimum 1); otherwise only level 0 is used.
    for (uint32_t l = 1; l < levels; ++l)
        if (t.levels[l].width != std::max(1, t.levels[l - 1].width / 2) || t.levels[l].height != std::max(1, t.levels[l - 1].height / 2)) { levels = 1; break; }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = t.width; d.Height = t.height; d.MipLevels = levels; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init[11] = {};
    for (uint32_t l = 0; l < levels; ++l) {
        init[l].pSysMem = t.rgba.data() + t.levels[l].offsetTexels;
        init[l].SysMemPitch = t.levels[l].width * 4u;
    }
    if (FAILED(dev_->device()->CreateTexture2D(&d, init, out.tex.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dev_->device()->CreateShaderResourceView(out.tex.Get(), nullptr, out.srv.ReleaseAndGetAddressOf()))) return false;
    out.w = t.width; out.h = t.height;
    out.bytes = t.rgba.size() * 4;
    ++stats_.texturesUploaded;
    stats_.textureBytes += t.rgba.size() * 4;
    return true;
}

ID3D11ShaderResourceView* Renderer::mapSrv(const gx::TexBinding& b, std::string& why) {
    if (!b.valid) { why = "a TEV stage samples a texture map that is not bound"; return nullptr; }
    if (b.efbCopyId) {
        const auto it = copyTextures_.find(b.efbCopyId);
        if (it == copyTextures_.end()) { why = "texture map references an EFB copy that no longer exists"; return nullptr; }
        return it->second.srv.Get();
    }
    if (!b.image) { why = "texture map has neither an image nor an EFB copy"; return nullptr; }
    const auto it = textures_.find(b.image->key);
    if (it != textures_.end()) { it->second.lastUse = ++useClock_; return it->second.srv.Get(); }
    Gpu g;
    if (!uploadTexture(*b.image, g)) { why = "texture upload failed"; return nullptr; }
    g.lastUse = ++useClock_;
    textureBytes_ += g.bytes;
    return textures_.emplace(b.image->key, std::move(g)).first->second.srv.Get();
}

ID3D11SamplerState* Renderer::sampler(const gx::Sampler& s) {
    struct K { uint8_t ws, wt, minf, mag; float bias, lo, hi; } k = {static_cast<uint8_t>(s.wrapS), static_cast<uint8_t>(s.wrapT), static_cast<uint8_t>(s.minFilter), s.magLinear, s.lodBias, s.minLod, s.maxLod};
    const std::string key = bytesOf(k);
    const auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second.Get();
    auto addr = [](gx::WrapMode m) { return m == gx::WrapMode::Clamp ? D3D11_TEXTURE_ADDRESS_CLAMP : m == gx::WrapMode::Repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_MIRROR; };
    bool minLinear = false, mipLinear = false;
    switch (s.minFilter) {
        case gx::FilterMode::Near: case gx::FilterMode::NearMipNear: break;
        case gx::FilterMode::Linear: case gx::FilterMode::LinMipNear: minLinear = true; break;
        case gx::FilterMode::NearMipLin: mipLinear = true; break;
        case gx::FilterMode::LinMipLin: minLinear = mipLinear = true; break;
    }
    const bool mipped = s.minFilter >= gx::FilterMode::NearMipNear;
    D3D11_SAMPLER_DESC d = {};
    d.Filter = D3D11_ENCODE_BASIC_FILTER(minLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT, s.magLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                         mipLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT, D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    d.AddressU = addr(s.wrapS); d.AddressV = addr(s.wrapT); d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MipLODBias = s.lodBias;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MinLOD = mipped ? s.minLod : 0.0f;
    d.MaxLOD = mipped ? std::max(s.maxLod, d.MinLOD) : 0.0f;   // without mip filtering the base level is always used
    ComPtr<ID3D11SamplerState> st;
    dev_->device()->CreateSamplerState(&d, st.GetAddressOf());
    return samplers_.emplace(key, st).first->second.Get();
}

ID3D11BlendState* Renderer::blendState(const gx::BlendState& b) {
    struct K { uint8_t mode, src, dst, op, color, alpha; } k = {static_cast<uint8_t>(b.mode), static_cast<uint8_t>(b.src), static_cast<uint8_t>(b.dst), static_cast<uint8_t>(b.logicOp), b.colorUpdate, b.alphaUpdate};
    if (b.mode == gx::BlendMode::None || b.mode == gx::BlendMode::Logic) { k.src = k.dst = 0; }
    if (b.mode != gx::BlendMode::Logic) k.op = 0;
    const std::string key = bytesOf(k);
    const auto it = blends_.find(key);
    if (it != blends_.end()) return it->second.Get();
    D3D11_BLEND_DESC d = {};
    D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
    r.RenderTargetWriteMask = static_cast<UINT8>((b.colorUpdate ? D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
                                                 (b.alphaUpdate ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    if (b.mode == gx::BlendMode::Logic && b.logicOp == gx::LogicOp::NoOp) r.RenderTargetWriteMask = 0;
    if (b.mode == gx::BlendMode::Blend) {
        r.BlendEnable = TRUE;
        r.SrcBlend = factor(b.src, true); r.DestBlend = factor(b.dst, false);
        r.SrcBlendAlpha = alphaFactor(r.SrcBlend); r.DestBlendAlpha = alphaFactor(r.DestBlend);
        r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else if (b.mode == gx::BlendMode::Subtract) {
        r.BlendEnable = TRUE;
        r.SrcBlend = r.DestBlend = r.SrcBlendAlpha = r.DestBlendAlpha = D3D11_BLEND_ONE;
        r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT;   // destination minus source, clamped at 0
    }
    ComPtr<ID3D11BlendState> st;
    dev_->device()->CreateBlendState(&d, st.GetAddressOf());
    return blends_.emplace(key, st).first->second.Get();
}

ID3D11DepthStencilState* Renderer::depthState(const gx::DepthState& s) {
    struct K { uint8_t test, func, write; } k = {s.test, static_cast<uint8_t>(s.func), s.write};
    if (!s.test) k = {0, 0, 0};
    const std::string key = bytesOf(k);
    const auto it = depths_.find(key);
    if (it != depths_.end()) return it->second.Get();
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = s.test;   // with the compare off the pixel engine neither tests nor updates depth (verify against captures)
    d.DepthFunc = comparison(s.func);
    d.DepthWriteMask = s.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    ComPtr<ID3D11DepthStencilState> st;
    dev_->device()->CreateDepthStencilState(&d, st.GetAddressOf());
    return depths_.emplace(key, st).first->second.Get();
}

// Keeps the texture cache within a byte budget (least recently used first) and the copy-texture table within a count. Called between
// draws, so no texture of the draw being built can disappear.
void Renderer::trimCaches() {
    constexpr uint64_t kBudget = 512ull << 20;
    if (textureBytes_ > kBudget) {
        std::vector<std::pair<uint64_t, uint64_t>> order;   // (lastUse, key)
        for (const auto& e : textures_) order.push_back({e.second.lastUse, e.first});
        std::sort(order.begin(), order.end());
        for (const auto& o : order) {
            if (textureBytes_ <= kBudget / 4 * 3) break;
            textureBytes_ -= textures_[o.second].bytes;
            textures_.erase(o.second);
            ++stats_.texturesEvicted;
        }
    }
    if (copyTextures_.size() > 512) {
        std::vector<uint32_t> ids;
        for (const auto& e : copyTextures_) ids.push_back(e.first);
        std::sort(ids.begin(), ids.end());
        for (size_t i = 0; i + 256 < ids.size(); ++i) copyTextures_.erase(ids[i]);
    }
}

void Renderer::draw(const gx::DrawCommand& dc) {
    ++stats_.draws;
    trimCaches();
    depthDirty_ = true;
    const gx::PipelineState& s = *dc.state;
    auto skip = [&](const std::string& why) { report(why); ++stats_.drawsSkipped; };
    if (!dc.indexCount || !dc.vertexCount) return;
    if (dc.vertexCount > 65535) { skip("draw with more than 65535 vertices"); return; }
    if (s.viewport.width <= 0 || s.viewport.height <= 0) { skip("empty viewport"); return; }
    if (s.blend.mode == gx::BlendMode::Logic && s.blend.logicOp != gx::LogicOp::Copy && s.blend.logicOp != gx::LogicOp::NoOp) { skip("blend logic op other than copy/no-op"); return; }
    if (s.depth.compareBeforeTexture && s.depth.test && (s.alphaCompare.comp0 != gx::CompareFunc::Always || s.alphaCompare.comp1 != gx::CompareFunc::Always)) {
        skip("early depth test together with an alpha test (depth is updated before the alpha test on hardware)");
        return;
    }
    if (s.efbFormat != gx::PixelFormat::RGB8_Z24) { skip("EFB pixel format other than RGB8_Z24"); return; }
    if (s.fog.type != gx::FogType::None && s.fog.rangeAdjustEnable) { skip("fog range adjustment (GX_SetFogRangeAdj)"); return; }

    tev::PixelShaderKey key;
    std::string why;
    if (!buildKey(s, key, why)) { skip(why); return; }
    ID3D11PixelShader* ps = shaders_.get(*dev_, key, why);
    if (!ps) { skip(why.substr(0, why.find('\n'))); return; }

    // Textures and per-stage coordinate scale.
    tev::TevConstants cb = {};
    for (auto& sc : cb.stageScale) { sc[0] = 1.0f; sc[1] = 1.0f; sc[2] = 1.0f; sc[3] = 1.0f; }
    ID3D11ShaderResourceView* srvs[8] = {};
    ID3D11SamplerState* smps[8] = {};
    bool mapDone[8] = {};
    auto bindMap = [&](uint32_t m) -> bool {
        if (m > 7) { skip("texture map out of range"); return false; }
        if (mapDone[m]) return true;
        srvs[m] = mapSrv(s.maps[m], why);
        if (!srvs[m]) { skip(why); return false; }
        smps[m] = sampler(s.maps[m].sampler);
        mapDone[m] = true;
        return true;
    };
    for (uint32_t i = 0; i < s.tev.numStages; ++i) {
        const gx::TevStage& t = s.tev.stages[i];
        float* sc = cb.stageScale[i];
        if (t.texCoord != gx::kNoMap) {
            if (t.texCoord > 7) { skip("texture coordinate set out of range"); return; }
            const gx::TexCoordScale& ts = s.texScale[t.texCoord];
            if (ts.cylWrapS || ts.cylWrapT) { skip("cylindrical texture coordinate wrapping"); return; }
            if (ts.rangeBiasS || ts.rangeBiasT) report("texture coordinate range bias (ignored: coordinates are used unbiased)");
            sc[0] = ts.sSize; sc[1] = ts.tSize;
        }
        if (t.texMap != gx::kNoMap) {
            if (!bindMap(t.texMap)) return;
            const gx::TexBinding& b = s.maps[t.texMap];
            if (b.width) sc[2] = 1.0f / b.width;
            if (b.height) sc[3] = 1.0f / b.height;
        }
    }
    for (uint32_t k = 0; k < s.tev.numIndirect && k < 4; ++k) {
        const gx::IndirectStage& is = s.tev.indirectStage[k];
        if (is.texMap > 7 || is.texCoord > 7) continue;   // an indirect lookup no stage uses may name anything
        bool used = false;
        for (uint32_t i = 0; i < s.tev.numStages; ++i) used |= s.tev.stages[i].indirect.indStage == k;
        if (!used) continue;
        if (!bindMap(is.texMap)) return;
        const gx::TexCoordScale& ts = s.texScale[is.texCoord];
        const gx::TexBinding& b = s.maps[is.texMap];
        if (b.width) cb.indScale[k][0] = static_cast<float>(ts.sSize) / static_cast<float>(1u << is.scaleS) / b.width;
        if (b.height) cb.indScale[k][1] = static_cast<float>(ts.tSize) / static_cast<float>(1u << is.scaleT) / b.height;
    }
    for (int m = 0; m < 3; ++m)
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col) cb.indMtx[2 * m + row][col] = s.tev.indirectMatrix[m][row][col];
    bool anyIndirect = false;
    for (uint32_t i = 0; i < s.tev.numStages; ++i) anyIndirect |= s.tev.stages[i].indirect.indStage != gx::kNoMap;
    if (anyIndirect) report("indirect texturing offsets use the matrix / 256 in texels (scale not verified against hardware)");
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { cb.tevreg[r][c] = s.tev.reg[r][c] / 255.0f; cb.kcolor[r][c] = s.tev.konst[r][c] / 255.0f; }
    cb.fogParam[0] = s.fog.a; cb.fogParam[1] = s.fog.b; cb.fogParam[2] = s.fog.c;
    for (int c = 0; c < 3; ++c) cb.fogColor[c] = s.fog.color[c] / 255.0f;
    cb.alphaRef[0] = s.alphaCompare.ref0;
    cb.alphaRef[1] = s.alphaCompare.ref1;

    // Vertices and indices to draw: triangles as they are; points become screen-aligned squares, lines quads along the line
    // (size in 1/6 pixel, EFB pixels), expanded here in clip space.
    verts_.clear();
    idx_.clear();
    auto convert = [&](const gx::Vertex& v) {
        tev::Vertex o;
        std::memcpy(o.pos, v.pos, sizeof o.pos);
        for (int c = 0; c < 4; ++c) { o.col0[c] = v.color[0][c] / 255.0f; o.col1[c] = v.color[1][c] / 255.0f; }
        std::memcpy(o.uv, v.tex, sizeof o.uv);
        return o;
    };
    const float vpW = s.viewport.width, vpH = s.viewport.height;
    static const uint16_t kQuadIdx[6] = {0, 1, 2, 2, 1, 3};
    static const float kTexOffset[8] = {0.0f, 1.0f / 16, 1.0f / 8, 1.0f / 4, 0.5f, 1.0f, 0.0f, 0.0f};   // GX_TO_ZERO .. GX_TO_ONE
    if (dc.primitive == gx::Primitive::Triangles) {
        verts_.reserve(dc.vertexCount);
        for (uint32_t i = 0; i < dc.vertexCount; ++i) verts_.push_back(convert(dc.vertices[i]));
        idx_.assign(dc.indices, dc.indices + dc.indexCount);
    } else if (dc.primitive == gx::Primitive::Points) {
        const float half = s.pointSize / 6.0f * 0.5f;
        const float off = kTexOffset[s.pointTexOffset & 7];
        for (uint32_t k = 0; k < dc.indexCount; ++k) {
            const gx::Vertex& v = dc.vertices[dc.indices[k]];
            if (!(v.pos[3] > 0.0f)) continue;   // behind the eye: no sprite
            const float dx = half * 2.0f / vpW * v.pos[3], dy = half * 2.0f / vpH * v.pos[3];
            const uint16_t base = static_cast<uint16_t>(verts_.size());
            for (int corner = 0; corner < 4; ++corner) {   // TL, TR, BL, BR
                tev::Vertex o = convert(v);
                o.pos[0] += (corner & 1) ? dx : -dx;
                o.pos[1] += (corner & 2) ? -dy : dy;
                for (int t = 0; t < 8; ++t) {
                    if (!s.texScale[t].pointOffset) continue;
                    if (corner & 1) o.uv[t][0] += off;
                    if (corner & 2) o.uv[t][1] += off;
                }
                verts_.push_back(o);
            }
            for (uint16_t i : kQuadIdx) idx_.push_back(static_cast<uint16_t>(base + i));
        }
        report("point sprites: texture coordinate offset applied in normalized coordinate units (not verified against hardware)");
    } else {
        const float half = s.lineWidth / 6.0f * 0.5f;
        for (uint32_t k = 0; k + 1 < dc.indexCount; k += 2) {
            const gx::Vertex& a0 = dc.vertices[dc.indices[k]];
            const gx::Vertex& a1 = dc.vertices[dc.indices[k + 1]];
            if (!(a0.pos[3] > 0.0f) || !(a1.pos[3] > 0.0f)) continue;
            const float x0 = a0.pos[0] / a0.pos[3] * vpW * 0.5f, y0 = a0.pos[1] / a0.pos[3] * vpH * 0.5f;
            const float x1 = a1.pos[0] / a1.pos[3] * vpW * 0.5f, y1 = a1.pos[1] / a1.pos[3] * vpH * 0.5f;
            const float lx = x1 - x0, ly = y1 - y0, len = std::sqrt(lx * lx + ly * ly);
            if (len < 1e-6f) continue;
            const float px = -ly / len * half, py = lx / len * half;   // half-width perpendicular, in pixels
            const uint16_t base = static_cast<uint16_t>(verts_.size());
            const gx::Vertex* ends[4] = {&a0, &a0, &a1, &a1};
            const float sign[4] = {1, -1, 1, -1};
            for (int c = 0; c < 4; ++c) {
                tev::Vertex o = convert(*ends[c]);
                o.pos[0] += sign[c] * px * 2.0f / vpW * ends[c]->pos[3];
                o.pos[1] += sign[c] * py * 2.0f / vpH * ends[c]->pos[3];
                verts_.push_back(o);
            }
            for (uint16_t i : kQuadIdx) idx_.push_back(static_cast<uint16_t>(base + i));
        }
        if (s.lineTexOffset) report("line texture coordinate offsets (ignored)");
    }
    if (verts_.size() > 65535) { skip("primitive expansion produced more than 65535 vertices"); return; }
    if (idx_.empty()) return;
    uint32_t vbOffset = 0, ibOffset = 0;
    if (!upload(vb_, vbCap_, vbCursor_, D3D11_BIND_VERTEX_BUFFER, verts_.data(), static_cast<uint32_t>(verts_.size() * sizeof(tev::Vertex)), vbOffset) ||
        !upload(ib_, ibCap_, ibCursor_, D3D11_BIND_INDEX_BUFFER, idx_.data(), static_cast<uint32_t>(idx_.size() * 2), ibOffset)) { skip("vertex/index buffer upload failed"); return; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev_->context()->Map(tevCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { skip("constant buffer upload failed"); return; }
    std::memcpy(m.pData, &cb, sizeof cb);
    dev_->context()->Unmap(tevCb_.Get(), 0);

    if (const char* dd = std::getenv("AWL_GFX_DUMPDRAW")) {   // debugging aid: write one draw's clip-space vertices and indices to <file>
        if (dc.seq == static_cast<uint32_t>(std::strtoul(dd, nullptr, 0)) && std::getenv("AWL_GFX_DUMPDRAW_FILE")) {
            if (FILE* f = std::fopen(std::getenv("AWL_GFX_DUMPDRAW_FILE"), "wb")) {
                const uint32_t nVerts = static_cast<uint32_t>(verts_.size()), nIdx = static_cast<uint32_t>(idx_.size());
                std::fwrite(&nVerts, 4, 1, f); std::fwrite(&nIdx, 4, 1, f);
                for (const tev::Vertex& v : verts_) std::fwrite(v.pos, 4, 4, f);
                std::fwrite(idx_.data(), 2, nIdx, f);
                std::fclose(f);
            }
        }
    }
    ID3D11DeviceContext* c = dev_->context();
    ID3D11RenderTargetView* rtv = efbRtv_.Get();
    c->OMSetRenderTargets(1, &rtv, dsv_.Get());
    // D3D11 silently ignores a viewport whose depth range leaves [0, 1] and keeps the previous one. The XF registers give a far value of
    // 16777216 / 16777215 (just above 1.0), so the range is clamped to what the depth buffer can hold.
    const auto unit = [](float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; };
    D3D11_VIEWPORT vp = {s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height, unit(s.viewport.nearZ), unit(s.viewport.farZ)};
    c->RSSetViewports(1, &vp);
    if (std::getenv("AWL_GFX_LITPROBE") && dc.seq == static_cast<uint32_t>(std::strtoul(std::getenv("AWL_GFX_LITPROBE"), nullptr, 0))) {
        UINT n1 = 1; D3D11_VIEWPORT g1[1] = {}; c->RSGetViewports(&n1, g1);
        std::fprintf(stderr, "fiber=%p thread=%lu ", GetCurrentFiber(), GetCurrentThreadId());
        std::fprintf(stderr, "vpset want(%.4f,%.4f,%.4f,%.4f z %.6f-%.6f) got n=%u (%.4f,%.4f,%.4f,%.4f z %.6f-%.6f)\n", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth, n1,
                     g1[0].TopLeftX, g1[0].TopLeftY, g1[0].Width, g1[0].Height, g1[0].MinDepth, g1[0].MaxDepth);
    }
    if (std::getenv("AWL_GFX_VPCHECK")) {   // debugging aid: does the viewport we set stick?
        UINT n1 = 1; D3D11_VIEWPORT g1[1] = {}; c->RSGetViewports(&n1, g1);
        const bool ok = g1[0].Width == vp.Width && g1[0].Height == vp.Height && g1[0].MinDepth == vp.MinDepth;
        static uint64_t good = 0, bad = 0;
        (ok ? good : bad)++;
        static void* lastFiber = nullptr; static bool lastOk = true;
        if (GetCurrentFiber() != lastFiber || ok != lastOk || (good + bad) % 100000 == 0) {
            std::fprintf(stderr, "vpcheck seq=%u fiber=%p ok=%d (good %llu bad %llu) want %.0fx%.0f z%.4f got %.0fx%.0f z%.4f\n", dc.seq, GetCurrentFiber(), ok, (unsigned long long)good, (unsigned long long)bad,
                         vp.Width, vp.Height, vp.MinDepth, g1[0].Width, g1[0].Height, g1[0].MinDepth);
            lastFiber = GetCurrentFiber(); lastOk = ok;
        }
    }
    D3D11_RECT sc = {s.scissor.x0, s.scissor.y0, s.scissor.x1 + 1, s.scissor.y1 + 1};
    c->RSSetScissorRects(1, &sc);
    static const bool noScissor = std::getenv("AWL_GFX_DEBUG_NOSCISSOR") != nullptr;   // debugging aid: no scissor
    c->RSSetState(noScissor ? rasterNoScissor_.Get() : raster_.Get());
    c->OMSetBlendState(blendState(s.blend), nullptr, 0xFFFFFFFFu);
    static const bool noDepth = std::getenv("AWL_GFX_DEBUG_NODEPTH") != nullptr;   // debugging aid: depth test off
    if (noDepth) c->OMSetDepthStencilState(noDepth_.Get(), 0); else c->OMSetDepthStencilState(depthState(s.depth), 0);
    c->IASetInputLayout(layout_.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(tev::Vertex), offset = vbOffset;
    ID3D11Buffer* vb = vb_.Get();
    c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    c->IASetIndexBuffer(ib_.Get(), DXGI_FORMAT_R16_UINT, ibOffset);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    static const bool solid = std::getenv("AWL_GFX_DEBUG_SOLID") != nullptr;   // debugging aid: every pixel solid red
    if (solid) {
        if (!solidPs_) {
            ComPtr<ID3DBlob> solidCode; std::string e;
            if (compileHlsl("float4 main() : SV_Target0 { return float4(1,0,0,1); }", "main", "ps_5_0", solidCode, e))
                dev_->device()->CreatePixelShader(solidCode->GetBufferPointer(), solidCode->GetBufferSize(), nullptr, solidPs_.GetAddressOf());
        }
        c->PSSetShader(solidPs_.Get(), nullptr, 0);
    } else {
        c->PSSetShader(ps, nullptr, 0);
    }
    ID3D11Buffer* cbuf = tevCb_.Get();
    c->PSSetConstantBuffers(0, 1, &cbuf);
    c->PSSetShaderResources(0, 8, srvs);
    c->PSSetSamplers(0, 8, smps);
    if (frameStats_ && !frameQuery_) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION, 0};
        dev_->device()->CreateQuery(&qd, frameQuery_.GetAddressOf());
        if (frameQuery_) { c->Begin(frameQuery_.Get()); frameDraws_ = 0; frameFirstSeq_ = dc.seq; }
    }
    ++frameDraws_;
    ComPtr<ID3D11Query> occ;
    if (occFrom_ && dc.seq >= occFrom_ && dc.seq < occFrom_ + occCount_) {   // debugging aid (AWL_GFX_OCCLUSION=<first seq>,<count>)
        D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION, 0};
        dev_->device()->CreateQuery(&qd, occ.GetAddressOf());
        if (occ) c->Begin(occ.Get());
    }
    const bool probe = std::getenv("AWL_GFX_LITPROBE") && dc.seq == static_cast<uint32_t>(std::strtoul(std::getenv("AWL_GFX_LITPROBE"), nullptr, 0));
    if (probe) {   // the D3D11 state actually bound for this draw
        UINT nvp = 1; D3D11_VIEWPORT gv[1] = {}; c->RSGetViewports(&nvp, gv);
        UINT nsc = 1; D3D11_RECT gs[1] = {}; c->RSGetScissorRects(&nsc, gs);
        ComPtr<ID3D11RasterizerState> rs; c->RSGetState(rs.GetAddressOf());
        D3D11_RASTERIZER_DESC rdesc = {}; if (rs) rs->GetDesc(&rdesc);
        ID3D11RenderTargetView* grtv[1] = {}; ID3D11DepthStencilView* gdsv = nullptr; c->OMGetRenderTargets(1, grtv, &gdsv);
        D3D11_RENDER_TARGET_VIEW_DESC rv = {}; if (grtv[0]) grtv[0]->GetDesc(&rv);
        ComPtr<ID3D11Resource> rres; if (grtv[0]) grtv[0]->GetResource(rres.GetAddressOf());
        ComPtr<ID3D11Texture2D> rt2; if (rres) rres.As(&rt2);
        D3D11_TEXTURE2D_DESC td = {}; if (rt2) rt2->GetDesc(&td);
        ComPtr<ID3D11BlendState> bs; FLOAT bf[4]; UINT sm; c->OMGetBlendState(bs.GetAddressOf(), bf, &sm);
        D3D11_BLEND_DESC bd = {}; if (bs) bs->GetDesc(&bd);
        ComPtr<ID3D11DepthStencilState> dss; UINT sref; c->OMGetDepthStencilState(dss.GetAddressOf(), &sref);
        D3D11_DEPTH_STENCIL_DESC dd = {}; if (dss) dss->GetDesc(&dd);
        std::fprintf(stderr, "d3dstate viewport(%.1f,%.1f,%.1f,%.1f z %.4f-%.4f) scissor(%ld,%ld,%ld,%ld) raster{fill %d cull %d clip %d scissor %d} rtv %ux%u dsv %p blend{en %d mask %u} depth{en %d write %d func %d} samplemask %08X\n",
                     gv[0].TopLeftX, gv[0].TopLeftY, gv[0].Width, gv[0].Height, gv[0].MinDepth, gv[0].MaxDepth, gs[0].left, gs[0].top, gs[0].right, gs[0].bottom,
                     rdesc.FillMode, rdesc.CullMode, rdesc.DepthClipEnable, rdesc.ScissorEnable, td.Width, td.Height, static_cast<void*>(gdsv), bd.RenderTarget[0].BlendEnable,
                     bd.RenderTarget[0].RenderTargetWriteMask, dd.DepthEnable, dd.DepthWriteMask, dd.DepthFunc, sm);
        if (grtv[0]) grtv[0]->Release();
        if (gdsv) gdsv->Release();
    }
    const size_t litBefore = probe ? countLit(*dev_, efb_.tex.Get(), kEfbWidth, kEfbHeight) : 0;
    c->DrawIndexed(static_cast<UINT>(idx_.size()), 0, 0);
    if (probe) std::fprintf(stderr, "litprobe seq=%u before=%zu after=%zu\n", dc.seq, litBefore, countLit(*dev_, efb_.tex.Get(), kEfbWidth, kEfbHeight));
    if (occ) {
        c->End(occ.Get());
        UINT64 samples = 0;
        while (c->GetData(occ.Get(), &samples, sizeof samples, 0) == S_FALSE) {}
        float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f, bz0 = 1e30f, bz1 = -1e30f;
        for (const tev::Vertex& v : verts_) {
            if (!(v.pos[3] > 0)) continue;
            bx0 = std::min(bx0, v.pos[0] / v.pos[3]); bx1 = std::max(bx1, v.pos[0] / v.pos[3]);
            by0 = std::min(by0, v.pos[1] / v.pos[3]); by1 = std::max(by1, v.pos[1] / v.pos[3]);
            bz0 = std::min(bz0, v.pos[2] / v.pos[3]); bz1 = std::max(bz1, v.pos[2] / v.pos[3]);
        }
        std::fprintf(stderr, "ndcbox x[%.2f,%.2f] y[%.2f,%.2f] z[%.4f,%.4f] vp(%.0f,%.0f,%.0f,%.0f) sc(%d,%d,%d,%d) ", bx0, bx1, by0, by1, bz0, bz1, s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height, s.scissor.x0, s.scissor.y0, s.scissor.x1, s.scissor.y1);
        double area = 0; uint32_t inside = 0, tris = 0;
        for (size_t t = 0; t + 2 < idx_.size(); t += 3) {
            const tev::Vertex* v[3] = {&verts_[idx_[t]], &verts_[idx_[t + 1]], &verts_[idx_[t + 2]]};
            bool ok = true; double px[3], py[3];
            for (int k = 0; k < 3; ++k) {
                const float w = v[k]->pos[3], zz = v[k]->pos[2] + w;
                ok = ok && w > 0 && std::fabs(v[k]->pos[0]) <= w && std::fabs(v[k]->pos[1]) <= w && zz >= 0 && zz <= w;
                px[k] = v[k]->pos[0] / w * vpW * 0.5; py[k] = v[k]->pos[1] / w * vpH * 0.5;
            }
            ++tris;
            if (ok) { ++inside; area += std::fabs((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0])) * 0.5; }
        }
        uint64_t cover = 0;   // pixel centres inside each triangle (overlaps counted), window coordinates y down
        for (size_t t = 0; t + 2 < idx_.size(); t += 3) {
            double x[3], y[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                const tev::Vertex& v = verts_[idx_[t + k]];
                if (!(v.pos[3] > 0)) { ok = false; break; }
                x[k] = s.viewport.x + (v.pos[0] / v.pos[3] * 0.5 + 0.5) * s.viewport.width;
                y[k] = s.viewport.y + (0.5 - v.pos[1] / v.pos[3] * 0.5) * s.viewport.height;
            }
            if (!ok) continue;
            const double a2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            if (std::fabs(a2) < 1e-9) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({x[0], x[1], x[2]}))), x1 = std::min(639, (int)std::ceil(std::max({x[0], x[1], x[2]})));
            const int y0 = std::max(0, (int)std::floor(std::min({y[0], y[1], y[2]}))), y1 = std::min(447, (int)std::ceil(std::max({y[0], y[1], y[2]})));
            for (int py = y0; py <= y1; ++py)
                for (int px = x0; px <= x1; ++px) {
                    const double cx = px + 0.5, cy = py + 0.5;
                    const double w0 = (x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy);
                    const double w1 = (x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy);
                    const double w2 = (x[0] - cx) * (y[1] - cy) - (x[1] - cx) * (y[0] - cy);
                    if ((a2 > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) || (a2 < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) ++cover;
                }
        }
        std::fprintf(stderr, "tris=%u inside=%u area=%.1f cpucover=%llu ", tris, inside, area, (unsigned long long)cover);
        std::fprintf(stderr, "map0w=%u ", s.maps[0].width);
        std::fprintf(stderr, "vpraw scale(%.2f,%.2f,%.1f) offset(%.2f,%.2f,%.1f) ", s.viewport.scale[0], s.viewport.scale[1], s.viewport.scale[2], s.viewport.offset[0], s.viewport.offset[1], s.viewport.offset[2]);
        std::fprintf(stderr, "occlusion seq=%u prim=%d verts=%zu idx=%zu samples=%llu depth=%d/%d write=%d blend=%d stages=%u map0=%d\n", dc.seq, static_cast<int>(dc.primitive),
                     verts_.size(), idx_.size(), static_cast<unsigned long long>(samples), s.depth.test, static_cast<int>(s.depth.func), s.depth.write, static_cast<int>(s.blend.mode),
                     s.tev.numStages, s.maps[0].valid);
    }
    ID3D11ShaderResourceView* none[8] = {};
    c->PSSetShaderResources(0, 8, none);
    stats_.triangles += idx_.size() / 3;
}

void Renderer::clearRect(const gx::EfbCopyCommand& cc) {
    const gx::ClearSpec& cl = cc.clear;
    if (!cl.color && !cl.alpha && !cl.z) return;
    depthDirty_ = true;
    ID3D11DeviceContext* c = dev_->context();
    struct { float color[4]; float z[4]; } cb = {{cl.rgba[0] / 255.0f, cl.rgba[1] / 255.0f, cl.rgba[2] / 255.0f, cl.rgba[3] / 255.0f}, {cl.z24 / 16777215.0f, 0, 0, 0}};
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c->Map(clearCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, &cb, sizeof cb);
    c->Unmap(clearCb_.Get(), 0);
    gx::BlendState bs = {};
    bs.mode = gx::BlendMode::None; bs.colorUpdate = cl.color; bs.alphaUpdate = cl.alpha;
    gx::DepthState ds = {};
    ds.test = cl.z; ds.func = gx::CompareFunc::Always; ds.write = cl.z;
    ID3D11RenderTargetView* rtv = efbRtv_.Get();
    c->OMSetRenderTargets(1, &rtv, dsv_.Get());
    D3D11_VIEWPORT vp = {static_cast<float>(cc.srcX), static_cast<float>(cc.srcY), static_cast<float>(cc.srcWidth), static_cast<float>(cc.srcHeight), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    c->RSSetState(rasterNoScissor_.Get());
    c->OMSetBlendState(blendState(bs), nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(depthState(ds), 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(clearVs_.Get(), nullptr, 0);
    c->PSSetShader(clearPs_.Get(), nullptr, 0);
    ID3D11Buffer* cbuf = clearCb_.Get();
    c->VSSetConstantBuffers(0, 1, &cbuf);
    c->Draw(3, 0);
}

void Renderer::efbCopy(const gx::EfbCopyCommand& cc) {
    ++stats_.copies;
    if (frameStats_ && frameQuery_ && cc.target == gx::CopyTarget::Xfb) {   // AWL_GFX_FRAMESTATS=1: samples passed by every draw since the previous XFB copy
        dev_->context()->End(frameQuery_.Get());
        UINT64 samples = 0;
        while (dev_->context()->GetData(frameQuery_.Get(), &samples, sizeof samples, 0) == S_FALSE) {}
        std::fprintf(stderr, "framestats copy=%u firstseq=%u draws=%u samples=%llu\n", cc.id, frameFirstSeq_, frameDraws_, static_cast<unsigned long long>(samples));
        frameQuery_.Reset();
    }
    if (cc.efbFormat != gx::PixelFormat::RGB8_Z24) { report("EFB copy from a pixel format other than RGB8_Z24"); }
    if (!cc.srcWidth || !cc.srcHeight || !cc.destWidth || !cc.destHeight || cc.srcX + cc.srcWidth > kEfbWidth || cc.srcY + cc.srcHeight > kEfbHeight ||
        cc.destWidth > 2048 || cc.destHeight > 2048) {
        report("EFB copy rectangle outside the EFB or empty");
        return;
    }
    bool approx = false;
    if (cc.target == gx::CopyTarget::Xfb) {
        if (cc.gamma != 0) { report("EFB copy gamma other than 1.0 (not applied)"); approx = true; }
    } else {
        if (cc.texFormat != 6) { report("EFB copy to a texture in a format other than RGBA8 (stored as RGBA8, bit depth not reduced)"); approx = true; }
        if (cc.mipmap) { report("EFB copy to a texture with mipmap generation (base level only)"); approx = true; }
        if (cc.intensity) { report("EFB copy to an intensity texture (colour kept)"); approx = true; }
    }
    if (approx) ++stats_.copiesApproximated;

    // Destination image.
    Gpu* dst = nullptr;
    if (cc.target == gx::CopyTarget::Xfb) {
        XfbEntry* e = nullptr;
        for (XfbEntry& x : xfb_) if (x.addr == cc.destAddr) e = &x;
        if (!e) { xfb_.push_back(XfbEntry()); e = &xfb_.back(); e->addr = cc.destAddr; }
        if (!e->gpu.tex || e->gpu.w != cc.destWidth || e->gpu.h != cc.destHeight)
            if (!createGpu(e->gpu, cc.destWidth, cc.destHeight, true)) { report("EFB copy destination allocation failed"); return; }
        e->seq = ++xfbSeq_;
        dst = &e->gpu;
    } else {
        Gpu& g = copyTextures_[cc.id];
        if (!createGpu(g, cc.destWidth, cc.destHeight, true)) { report("EFB copy destination allocation failed"); return; }
        dst = &g;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    dev_->device()->CreateRenderTargetView(dst->tex.Get(), nullptr, rtv.GetAddressOf());

    // The filter: the vertical taps apply only when the copy is not scaled; a half-scale texture copy averages 2x2 texels.
    const bool box = cc.target == gx::CopyTarget::Texture && cc.halfScale;
    float taps[7] = {0, 0, 0, 1, 0, 0, 0};
    if (cc.target == gx::CopyTarget::Xfb && copyFilter_) {
        int sum = 0;
        for (int i = 0; i < 7; ++i) sum += cc.vfilter[i];
        if (sum > 0) for (int i = 0; i < 7; ++i) taps[i] = cc.vfilter[i] / static_cast<float>(sum);
    }
    struct { int32_t src[4]; int32_t dst[4]; float f0[4]; float f1[4]; } cb = {
        {static_cast<int32_t>(cc.srcX), static_cast<int32_t>(cc.srcY), static_cast<int32_t>(cc.srcWidth), static_cast<int32_t>(cc.srcHeight)},
        {static_cast<int32_t>(cc.destWidth), static_cast<int32_t>(cc.destHeight), cc.clampTop ? static_cast<int32_t>(cc.srcY) : 0,
         cc.clampBottom ? static_cast<int32_t>(cc.srcY + cc.srcHeight - 1) : static_cast<int32_t>(kEfbHeight - 1)},
        {taps[0], taps[1], taps[2], taps[3]}, {taps[4], taps[5], taps[6], box ? 1.0f : 0.0f}};
    ID3D11DeviceContext* c = dev_->context();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c->Map(copyCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { report("copy constant buffer upload failed"); return; }
    std::memcpy(m.pData, &cb, sizeof cb);
    c->Unmap(copyCb_.Get(), 0);
    ID3D11RenderTargetView* r = rtv.Get();
    c->OMSetRenderTargets(1, &r, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(cc.destWidth), static_cast<float>(cc.destHeight), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    c->RSSetState(rasterNoScissor_.Get());
    c->OMSetBlendState(noBlend_.Get(), nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(noDepth_.Get(), 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(copyVs_.Get(), nullptr, 0);
    c->PSSetShader(copyPs_.Get(), nullptr, 0);
    ID3D11Buffer* cbuf = copyCb_.Get();
    c->PSSetConstantBuffers(0, 1, &cbuf);
    ID3D11ShaderResourceView* srv = efb_.srv.Get();
    c->PSSetShaderResources(0, 1, &srv);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);

    clearRect(cc);
}

bool Renderer::xfbImage(uint32_t addr, Image& out, std::string& why) {
    const XfbEntry* best = nullptr;
    for (const XfbEntry& e : xfb_) {
        const bool exact = e.addr == addr;
        const bool inside = addr > e.addr && addr < e.addr + e.gpu.w * 2u * e.gpu.h;   // 16-bit pixels in the XFB
        if ((exact || inside) && (!best || (exact && best->addr != addr) || (exact == (best->addr == addr) && e.seq > best->seq))) best = &e;
    }
    if (!best) { why = "no EFB copy wrote the XFB address"; return false; }
    D3D11_TEXTURE2D_DESC d = {};
    best->gpu.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev_->device()->CreateTexture2D(&d, nullptr, staging.GetAddressOf()))) { why = "readback texture"; return false; }
    dev_->context()->CopyResource(staging.Get(), best->gpu.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev_->context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { why = "readback map"; return false; }
    out.resize(d.Width, d.Height);
    for (uint32_t y = 0; y < d.Height; ++y) std::memcpy(out.px(0, y), static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * 4);
    dev_->context()->Unmap(staging.Get(), 0);
    return true;
}

bool Renderer::peekDepth(uint32_t x, uint32_t y, uint32_t& z24, std::string& err) {
    if (x >= kEfbWidth || y >= kEfbHeight) { err = "EFB depth peek outside the EFB"; return false; }
    if (!depthStaging_) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = kEfbWidth; d.Height = kEfbHeight; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R24G8_TYPELESS;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->device()->CreateTexture2D(&d, nullptr, depthStaging_.GetAddressOf()))) { err = "depth readback texture"; return false; }
        depthDirty_ = true;
    }
    // Depth-stencil resources can only be copied whole; the copy is reused until the next draw or clear changes the depth buffer.
    if (depthDirty_) {
        dev_->context()->CopyResource(depthStaging_.Get(), depth_.Get());
        depthDirty_ = false;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev_->context()->Map(depthStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) { err = "depth readback map"; return false; }
    z24 = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4) & 0x00FFFFFFu;
    dev_->context()->Unmap(depthStaging_.Get(), 0);
    return true;
}

bool RendererSource::produce(const FrameDesc& frame, Image& out, std::string& err) {
    std::string why;
    if (r_.xfbImage(frame.xfbAddr, out, why)) return true;
    ++missing_;
    if (std::find(warned_.begin(), warned_.end(), frame.xfbAddr) == warned_.end()) {
        warned_.push_back(frame.xfbAddr);
        std::fprintf(stderr, "gfx: XFB 0x%08X shown from the diagnostic frame: %s\n", frame.xfbAddr, why.c_str());
    }
    return diag_.produce(frame, out, err);
}

}  // namespace awl::gfx
