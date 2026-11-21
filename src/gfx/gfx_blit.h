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

// Point keeps a 1:1 blit bit exact (captures). Linear is plain bilinear. Sharp is "sharp bilinear" for upscaling to a non-integer
// size: each source pixel is held flat across most of its footprint and blended only over about one destination pixel at the edge,
// so the picture is crisp at any window size and never shimmers (below 1x it is plain bilinear). Window presentation only.
enum class BlitFilter { Point, Linear, Sharp };

class Blitter {
public:
    bool init(Device& dev, std::string& err);
    // Clears the target to opaque black and draws `src` (srcW x srcH texels) into `dest` (target pixels) with the chosen filter.
    void blit(Device& dev, ID3D11RenderTargetView* target, uint32_t targetW, uint32_t targetH, ID3D11ShaderResourceView* src,
              uint32_t srcW, uint32_t srcH, const Rect& dest, BlitFilter filter);
    // Same without a source size: Linear or Point only.
    void blit(Device& dev, ID3D11RenderTargetView* target, uint32_t targetW, uint32_t targetH, ID3D11ShaderResourceView* src,
              const Rect& dest, bool linear) {
        blit(dev, target, targetW, targetH, src, 0, 0, dest, linear ? BlitFilter::Linear : BlitFilter::Point);
    }

private:
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, psSharp_;
    ComPtr<ID3D11Buffer> sharpParams_;
    ComPtr<ID3D11SamplerState> point_, linear_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11DepthStencilState> depth_;
};

// Compiles HLSL; on failure `err` holds the compiler messages. Used by the blitter and the shader cache.
bool compileHlsl(const std::string& source, const char* entry, const char* profile, ComPtr<ID3DBlob>& code, std::string& err);

}  // namespace awl::gfx
