#include "gfx/gfx_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gfx/gfx_tev.h"

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
    }
    for (int t = 0; t < 4; ++t) { k.swaps[t].r = s.tev.swap[t][0]; k.swaps[t].g = s.tev.swap[t][1]; k.swaps[t].b = s.tev.swap[t][2]; k.swaps[t].a = s.tev.swap[t][3]; }
    k.projectiveMask = 0xFF;   // IR convention 4: the texture unit always divides by q (q = 1 for ST texgens)
    k.alphaComp0 = static_cast<uint8_t>(s.alphaCompare.comp0);
    k.alphaComp1 = static_cast<uint8_t>(s.alphaCompare.comp1);
    k.alphaLogic = static_cast<uint8_t>(s.alphaCompare.logic);
    k.fogType = static_cast<uint8_t>(s.fog.type);
    k.fogOrtho = s.fog.orthographic;
    k.efbFormat = static_cast<uint8_t>(s.efbFormat);
    return true;
}

}  // namespace

void Renderer::report(const std::string& reason) {
    uint64_t& n = unsupported_[reason];
    if (n++ == 0) std::fprintf(stderr, "gfx: not rendered exactly: %s (counted, reported once)\n", reason.c_str());
}

void Renderer::printSummary() const {
    std::printf("renderer: %llu draws (%llu skipped), %llu triangles, %llu EFB copies (%llu approximated), %llu textures uploaded (%.1f MB); "
                "shaders %zu compiled, %llu rejected, %llu cache hits\n",
                (unsigned long long)stats_.draws, (unsigned long long)stats_.drawsSkipped, (unsigned long long)stats_.triangles,
                (unsigned long long)stats_.copies, (unsigned long long)stats_.copiesApproximated, (unsigned long long)stats_.texturesUploaded,
                stats_.textureBytes / 1048576.0, shaders_.size(), (unsigned long long)shaders_.stats().rejected, (unsigned long long)shaders_.stats().hits);
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

bool Renderer::upload(ComPtr<ID3D11Buffer>& buf, uint32_t& capacity, uint32_t bind, const void* data, uint32_t bytes) {
    if (!buf || capacity < bytes) {
        capacity = std::max<uint32_t>(bytes, capacity * 2);
        capacity = std::max<uint32_t>(capacity, 1u << 16);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = capacity; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = bind; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->device()->CreateBuffer(&bd, nullptr, buf.ReleaseAndGetAddressOf()))) return false;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev_->context()->Map(buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    std::memcpy(m.pData, data, bytes);
    dev_->context()->Unmap(buf.Get(), 0);
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
    if (it != textures_.end()) return it->second.srv.Get();
    Gpu g;
    if (!uploadTexture(*b.image, g)) { why = "texture upload failed"; return nullptr; }
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

void Renderer::draw(const gx::DrawCommand& dc) {
    ++stats_.draws;
    depthDirty_ = true;
    const gx::PipelineState& s = *dc.state;
    auto skip = [&](const std::string& why) { report(why); ++stats_.drawsSkipped; };
    if (dc.primitive != gx::Primitive::Triangles) { skip("lines and points are not rendered (wide-primitive expansion not implemented)"); return; }
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
    for (auto& sc : cb.stageScale) { sc[0] = 1.0f; sc[1] = 1.0f; }
    ID3D11ShaderResourceView* srvs[8] = {};
    ID3D11SamplerState* smps[8] = {};
    bool mapDone[8] = {};
    for (uint32_t i = 0; i < s.tev.numStages; ++i) {
        const gx::TevStage& t = s.tev.stages[i];
        if (t.texMap == gx::kNoMap || t.texCoord == gx::kNoMap) continue;
        if (t.texMap > 7 || t.texCoord > 7) { skip("texture map or coordinate set out of range"); return; }
        const gx::TexBinding& b = s.maps[t.texMap];
        const gx::TexCoordScale& ts = s.texScale[t.texCoord];
        if (ts.cylWrapS || ts.cylWrapT) { skip("cylindrical texture coordinate wrapping"); return; }
        if (ts.rangeBiasS || ts.rangeBiasT) { skip("texture coordinate range bias"); return; }
        if (!mapDone[t.texMap]) {
            srvs[t.texMap] = mapSrv(b, why);
            if (!srvs[t.texMap]) { skip(why); return; }
            smps[t.texMap] = sampler(b.sampler);
            mapDone[t.texMap] = true;
        }
        if (b.width) cb.stageScale[i][0] = static_cast<float>(ts.sSize) / b.width;
        if (b.height) cb.stageScale[i][1] = static_cast<float>(ts.tSize) / b.height;
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { cb.tevreg[r][c] = s.tev.reg[r][c] / 255.0f; cb.kcolor[r][c] = s.tev.konst[r][c] / 255.0f; }
    cb.fogParam[0] = s.fog.a; cb.fogParam[1] = s.fog.b; cb.fogParam[2] = s.fog.c;
    for (int c = 0; c < 3; ++c) cb.fogColor[c] = s.fog.color[c] / 255.0f;
    cb.alphaRef[0] = s.alphaCompare.ref0;
    cb.alphaRef[1] = s.alphaCompare.ref1;

    std::vector<tev::Vertex> verts(dc.vertexCount);
    for (uint32_t i = 0; i < dc.vertexCount; ++i) {
        const gx::Vertex& v = dc.vertices[i];
        tev::Vertex& o = verts[i];
        std::memcpy(o.pos, v.pos, sizeof o.pos);
        for (int c = 0; c < 4; ++c) { o.col0[c] = v.color[0][c] / 255.0f; o.col1[c] = v.color[1][c] / 255.0f; }
        std::memcpy(o.uv, v.tex, sizeof o.uv);
    }
    if (!upload(vb_, vbCap_, D3D11_BIND_VERTEX_BUFFER, verts.data(), static_cast<uint32_t>(verts.size() * sizeof(tev::Vertex))) ||
        !upload(ib_, ibCap_, D3D11_BIND_INDEX_BUFFER, dc.indices, dc.indexCount * 2)) { skip("vertex/index buffer upload failed"); return; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dev_->context()->Map(tevCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { skip("constant buffer upload failed"); return; }
    std::memcpy(m.pData, &cb, sizeof cb);
    dev_->context()->Unmap(tevCb_.Get(), 0);

    ID3D11DeviceContext* c = dev_->context();
    ID3D11RenderTargetView* rtv = efbRtv_.Get();
    c->OMSetRenderTargets(1, &rtv, dsv_.Get());
    D3D11_VIEWPORT vp = {s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height, s.viewport.nearZ, s.viewport.farZ};
    c->RSSetViewports(1, &vp);
    D3D11_RECT sc = {s.scissor.x0, s.scissor.y0, s.scissor.x1 + 1, s.scissor.y1 + 1};
    c->RSSetScissorRects(1, &sc);
    c->RSSetState(raster_.Get());
    c->OMSetBlendState(blendState(s.blend), nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(depthState(s.depth), 0);
    c->IASetInputLayout(layout_.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(tev::Vertex), offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    c->IASetIndexBuffer(ib_.Get(), DXGI_FORMAT_R16_UINT, 0);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbuf = tevCb_.Get();
    c->PSSetConstantBuffers(0, 1, &cbuf);
    c->PSSetShaderResources(0, 8, srvs);
    c->PSSetSamplers(0, 8, smps);
    c->DrawIndexed(dc.indexCount, 0, 0);
    ID3D11ShaderResourceView* none[8] = {};
    c->PSSetShaderResources(0, 8, none);
    stats_.triangles += dc.indexCount / 3;
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
    if (cc.target == gx::CopyTarget::Xfb) {
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
