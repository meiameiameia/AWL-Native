// Direct3D 11 device and offscreen render targets. The device comes from the hardware adapter and falls back to WARP
// (the software rasteriser) when no hardware device can be created or when asked for; tests and headless captures use
// this layer without a window.
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "gfx/gfx_image.h"

namespace awl::gfx {

using Microsoft::WRL::ComPtr;

struct DeviceOptions {
    bool forceWarp = false;     // never use the hardware adapter (deterministic captures on any machine)
    bool allowWarp = true;      // fall back to WARP when the hardware device cannot be created
    bool debugLayer = false;    // request the D3D11 debug layer; ignored when the SDK layers are not installed
};

class Device {
public:
    bool create(const DeviceOptions& opts, std::string& err);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }
    IDXGIFactory2* factory() const { return factory_.Get(); }
    bool isWarp() const { return warp_; }
    const std::string& adapterName() const { return adapterName_; }
    D3D_FEATURE_LEVEL featureLevel() const { return level_; }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIFactory2> factory_;
    std::string adapterName_;
    bool warp_ = false;
    D3D_FEATURE_LEVEL level_ = D3D_FEATURE_LEVEL_11_0;
};

// A texture that can be both drawn into and sampled, with CPU readback. Format R8G8B8A8_UNORM.
class OffscreenTarget {
public:
    bool create(Device& dev, uint32_t width, uint32_t height, std::string& err);
    bool readback(Device& dev, Image& out, std::string& err);

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    ID3D11Texture2D* texture() const { return tex_.Get(); }
    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }

private:
    ComPtr<ID3D11Texture2D> tex_, staging_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t w_ = 0, h_ = 0;
};

// An RGBA8 image uploaded for sampling; reallocated when the size changes.
class ImageTexture {
public:
    bool update(Device& dev, const Image& img, std::string& err);
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    ComPtr<ID3D11Texture2D> tex_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t w_ = 0, h_ = 0;
};

std::string hresultText(long hr);

}  // namespace awl::gfx
