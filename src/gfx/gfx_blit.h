// Draws an RGBA8 texture into a render target (window back buffer or offscreen), letterboxed to the display aspect.
#pragma once

#include "gfx/gfx_device.h"

namespace awl::gfx {

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// Largest rectangle with the display aspect aspectNum:aspectDen that fits in dstW x dstH, centred. aspectNum == 0 keeps
// the source aspect srcW:srcH (square pixels). Integer arithmetic with truncation, so the result is exact for the tests.
Rect letterbox(uint32_t srcW, uint32_t srcH, uint32_t aspectNum, uint32_t aspectDen, uint32_t dstW, uint32_t dstH);

class Blitter {
public:
    bool init(Device& dev, std::string& err);
    // Clears the target to opaque black and draws `src` into `dest` (target pixels). `linear` selects bilinear
    // filtering; point sampling keeps a 1:1 blit bit exact.
    void blit(Device& dev, ID3D11RenderTargetView* target, uint32_t targetW, uint32_t targetH, ID3D11ShaderResourceView* src,
              const Rect& dest, bool linear);

private:
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> point_, linear_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11DepthStencilState> depth_;
};

// Compiles HLSL; on failure `err` holds the compiler messages. Used by the blitter and the shader cache.
bool compileHlsl(const std::string& source, const char* entry, const char* profile, ComPtr<ID3DBlob>& code, std::string& err);

}  // namespace awl::gfx
