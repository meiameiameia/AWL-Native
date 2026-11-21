#include "gfx/gfx_blit.h"

#include <d3dcompiler.h>

namespace awl::gfx {

Rect letterbox(uint32_t srcW, uint32_t srcH, uint32_t aspectNum, uint32_t aspectDen, uint32_t dstW, uint32_t dstH) {
    Rect r;
    if (!srcW || !srcH || !dstW || !dstH) return r;
    uint64_t num = aspectNum, den = aspectDen;
    if (!num || !den) { num = srcW; den = srcH; }
    // Width needed for the full height; if it does not fit, fit the width instead.
    uint64_t w = uint64_t(dstH) * num / den, h = dstH;
    if (w > dstW) { w = dstW; h = uint64_t(dstW) * den / num; }
    r.w = static_cast<int32_t>(w);
    r.h = static_cast<int32_t>(h);
    r.x = static_cast<int32_t>((dstW - w) / 2);
    r.y = static_cast<int32_t>((dstH - h) / 2);
    return r;
}

bool compileHlsl(const std::string& source, const char* entry, const char* profile, ComPtr<ID3DBlob>& code, std::string& err) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  code.ReleaseAndGetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        err = std::string("HLSL compile failed (") + entry + "/" + profile + "): " + hresultText(hr);
        if (errors) err += std::string("\n") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }
    return true;
}

namespace {
const char* kBlitHlsl = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vs(uint id : SV_VertexID) {
    VOut o;
    float2 t = float2((id << 1) & 2, id & 2);          // (0,0) (2,0) (0,2): one triangle covering the viewport
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv = t;
    return o;
}
float4 ps(VOut i) : SV_Target { return src.Sample(smp, i.uv); }
cbuffer SharpParams : register(b0) { float2 srcSize; float2 scale; };   // scale = destination pixels per source texel, per axis
float4 psSharp(VOut i) : SV_Target {
    const float2 texel = i.uv * srcSize;
    const float2 base = floor(texel);
    const float2 sc = max(scale, 1.0);
    const float2 range = 0.5 - 0.5 / sc;                  // flat part of the footprint, each side of the texel centre
    const float2 cd = texel - base - 0.5;
    const float2 f = (cd - clamp(cd, -range, range)) * sc + 0.5;
    return src.Sample(smp, (base + f) / srcSize);
}
)";
}  // namespace

bool Blitter::init(Device& dev, std::string& err) {
    ComPtr<ID3DBlob> vsCode, psCode;
    if (!compileHlsl(kBlitHlsl, "vs", "vs_5_0", vsCode, err) || !compileHlsl(kBlitHlsl, "ps", "ps_5_0", psCode, err)) return false;
    ID3D11Device* d = dev.device();
    ComPtr<ID3DBlob> sharpCode;
    if (!compileHlsl(kBlitHlsl, "psSharp", "ps_5_0", sharpCode, err)) return false;
    HRESULT hr = d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = d->CreatePixelShader(sharpCode->GetBufferPointer(), sharpCode->GetBufferSize(), nullptr, psSharp_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d->CreateBuffer(&bd, nullptr, sharpParams_.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) { err = "blit shaders: " + hresultText(hr); return false; }
    D3D11_SAMPLER_DESC s = {};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    hr = d->CreateSamplerState(&s, point_.ReleaseAndGetAddressOf());
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (SUCCEEDED(hr)) hr = d->CreateSamplerState(&s, linear_.ReleaseAndGetAddressOf());
    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = d->CreateRasterizerState(&r, raster_.ReleaseAndGetAddressOf());
    D3D11_BLEND_DESC b = {};
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = d->CreateBlendState(&b, blend_.ReleaseAndGetAddressOf());
    D3D11_DEPTH_STENCIL_DESC ds = {};
    if (SUCCEEDED(hr)) hr = d->CreateDepthStencilState(&ds, depth_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = "blit state objects: " + hresultText(hr); return false; }
    return true;
}

void Blitter::blit(Device& dev, ID3D11RenderTargetView* target, uint32_t targetW, uint32_t targetH, ID3D11ShaderResourceView* src,
                   uint32_t srcW, uint32_t srcH, const Rect& dest, BlitFilter filter) {
    ID3D11DeviceContext* c = dev.context();
    const float black[4] = {0, 0, 0, 1};
    c->ClearRenderTargetView(target, black);
    if (dest.w <= 0 || dest.h <= 0 || !targetW || !targetH) return;
    D3D11_VIEWPORT vp = {static_cast<float>(dest.x), static_cast<float>(dest.y), static_cast<float>(dest.w), static_cast<float>(dest.h), 0.0f, 1.0f};
    c->OMSetRenderTargets(1, &target, nullptr);
    c->RSSetViewports(1, &vp);
    c->RSSetState(raster_.Get());
    c->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(depth_.Get(), 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    const bool sharp = filter == BlitFilter::Sharp && srcW && srcH;
    c->PSSetShader(sharp ? psSharp_.Get() : ps_.Get(), nullptr, 0);
    if (sharp) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(sharpParams_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* p = static_cast<float*>(m.pData);
            p[0] = static_cast<float>(srcW); p[1] = static_cast<float>(srcH);
            p[2] = static_cast<float>(dest.w) / static_cast<float>(srcW); p[3] = static_cast<float>(dest.h) / static_cast<float>(srcH);
            c->Unmap(sharpParams_.Get(), 0);
        }
        ID3D11Buffer* cb = sharpParams_.Get();
        c->PSSetConstantBuffers(0, 1, &cb);
    }
    ID3D11SamplerState* smp = filter == BlitFilter::Point ? point_.Get() : linear_.Get();
    c->PSSetSamplers(0, 1, &smp);
    c->PSSetShaderResources(0, 1, &src);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    if (sharp) { ID3D11Buffer* nb = nullptr; c->PSSetConstantBuffers(0, 1, &nb); }
}

}  // namespace awl::gfx
