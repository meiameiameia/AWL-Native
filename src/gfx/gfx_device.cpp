#include "gfx/gfx_device.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace awl::gfx {

std::string hresultText(long hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

namespace {

std::string narrow(const wchar_t* w) {
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf - 1, nullptr, nullptr);
    return buf;
}

HRESULT tryCreate(D3D_DRIVER_TYPE type, UINT flags, ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx, D3D_FEATURE_LEVEL& level) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, dev.ReleaseAndGetAddressOf(), &level,
                                   ctx.ReleaseAndGetAddressOf());
    if (hr == E_INVALIDARG)   // runtimes without 11.1 reject the level list
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, dev.ReleaseAndGetAddressOf(), &level,
                               ctx.ReleaseAndGetAddressOf());
    return hr;
}

}  // namespace

bool Device::create(const DeviceOptions& opts, std::string& err) {
    UINT flags = 0;
    if (opts.debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = E_FAIL;
    warp_ = false;
    if (!opts.forceWarp) {
        hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, flags, device_, ctx_, level_);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, 0, device_, ctx_, level_);
    }
    if (FAILED(hr)) {
        if (!opts.forceWarp && !opts.allowWarp) { err = "no hardware Direct3D 11 device: " + hresultText(hr); return false; }
        hr = tryCreate(D3D_DRIVER_TYPE_WARP, flags, device_, ctx_, level_);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) hr = tryCreate(D3D_DRIVER_TYPE_WARP, 0, device_, ctx_, level_);
        if (FAILED(hr)) { err = "cannot create a Direct3D 11 device (hardware or WARP): " + hresultText(hr); return false; }
        warp_ = true;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
        DXGI_ADAPTER_DESC d = {};
        if (SUCCEEDED(adapter->GetDesc(&d))) adapterName_ = narrow(d.Description);
        adapter->GetParent(IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
    }
    if (!factory_) {
        hr = CreateDXGIFactory1(IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) { err = "cannot create the DXGI factory: " + hresultText(hr); return false; }
    }
    return true;
}

bool OffscreenTarget::create(Device& dev, uint32_t width, uint32_t height, std::string& err) {
    if (!width || !height || width > 16384 || height > 16384) { err = "offscreen target size out of range"; return false; }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev.device()->CreateTexture2D(&d, nullptr, tex_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = "offscreen texture: " + hresultText(hr); return false; }
    hr = dev.device()->CreateRenderTargetView(tex_.Get(), nullptr, rtv_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = "offscreen RTV: " + hresultText(hr); return false; }
    hr = dev.device()->CreateShaderResourceView(tex_.Get(), nullptr, srv_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = "offscreen SRV: " + hresultText(hr); return false; }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = dev.device()->CreateTexture2D(&d, nullptr, staging_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = "offscreen staging texture: " + hresultText(hr); return false; }
    w_ = width; h_ = height;
    return true;
}

bool OffscreenTarget::readback(Device& dev, Image& out, std::string& err) {
    if (!tex_) { err = "offscreen target not created"; return false; }
    dev.context()->CopyResource(staging_.Get(), tex_.Get());
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = dev.context()->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { err = "offscreen readback map: " + hresultText(hr); return false; }
    out.resize(w_, h_);
    for (uint32_t y = 0; y < h_; ++y) std::memcpy(out.px(0, y), static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w_) * 4);
    dev.context()->Unmap(staging_.Get(), 0);
    return true;
}

bool ImageTexture::update(Device& dev, const Image& img, std::string& err) {
    if (!img.valid()) { err = "ImageTexture: invalid image"; return false; }
    if (!tex_ || w_ != img.width || h_ != img.height) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = img.width; d.Height = img.height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = dev.device()->CreateTexture2D(&d, nullptr, tex_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { err = "image texture: " + hresultText(hr); return false; }
        hr = dev.device()->CreateShaderResourceView(tex_.Get(), nullptr, srv_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { err = "image texture SRV: " + hresultText(hr); return false; }
        w_ = img.width; h_ = img.height;
    }
    dev.context()->UpdateSubresource(tex_.Get(), 0, nullptr, img.rgba.data(), img.width * 4, 0);
    return true;
}

}  // namespace awl::gfx
