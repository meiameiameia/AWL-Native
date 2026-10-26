// The DirectX 11 renderer: a gx::Backend that turns the GX core's IR (src/gx/gx_ir.h) into D3D11 draws on an EFB-sized
// render target, executes EFB copies (to XFB images and to textures, with the vertical filter and the clear), and answers
// EFB depth peeks. Design and the rules behind each mapping: docs/research/gfx-dx11.md.
//
// Anything in the IR that is not rendered exactly is reported by name to stderr the first time it is seen and counted
// (unsupported()); a draw with such a state is skipped, a copy with an approximated format is performed as RGBA8 and
// counted. Nothing is ignored silently.
#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>

#include "gfx/gfx_blit.h"
#include "gfx/gfx_device.h"
#include "gfx/gfx_output.h"
#include "gfx/gfx_shader_cache.h"
#include "gfx/gfx_tev.h"
#include "gx/gx_ir.h"

namespace awl::gfx {

class Renderer : public gx::Backend {
public:
    static constexpr uint32_t kEfbWidth = 640, kEfbHeight = 528;

    bool init(Device& dev, std::string& err);
    void setOcclusionDebug(uint32_t firstSeq, uint32_t count) { occFrom_ = firstSeq; occCount_ = count; }

    // gx::Backend
    void draw(const gx::DrawCommand& d) override;
    void efbCopy(const gx::EfbCopyCommand& c) override;

    // The image the latest copy to the XFB wrote at `addr` (or the copy whose destination range contains it).
    bool xfbImage(uint32_t addr, Image& out, std::string& why);
    // One pixel of the EFB depth buffer, 24 bits (0xFFFFFF = far plane). Flushes nothing: D3D orders it after the draws.
    bool peekDepth(uint32_t x, uint32_t y, uint32_t& z24, std::string& err);

    struct Stats {
        uint64_t draws = 0, drawsSkipped = 0, triangles = 0, copies = 0, copiesApproximated = 0;
        uint64_t texturesUploaded = 0, textureBytes = 0, texturesEvicted = 0;
    };
    const Stats& stats() const { return stats_; }
    const PixelShaderCache::Stats& shaderStats() const { return shaders_.stats(); }
    // Every distinct reason something was not rendered exactly, with its count (deterministic order).
    const std::map<std::string, uint64_t>& unsupported() const { return unsupported_; }
    void printSummary() const;

private:
    struct Gpu {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t w = 0, h = 0;
        uint64_t bytes = 0, lastUse = 0;
    };
    struct XfbEntry { Gpu gpu; uint32_t addr = 0; uint64_t seq = 0; };

    void report(const std::string& reason);
    void trimCaches();
    bool uploadTexture(const gx::Texture& t, Gpu& out);
    ID3D11ShaderResourceView* mapSrv(const gx::TexBinding& b, std::string& why);
    ID3D11SamplerState* sampler(const gx::Sampler& s);
    ID3D11BlendState* blendState(const gx::BlendState& b);
    ID3D11DepthStencilState* depthState(const gx::DepthState& d);
    bool upload(ComPtr<ID3D11Buffer>& buf, uint32_t& capacity, uint32_t& cursor, uint32_t bind, const void* data, uint32_t bytes, uint32_t& offset);
    bool createGpu(Gpu& g, uint32_t w, uint32_t h, bool renderTarget);
    void clearRect(const gx::EfbCopyCommand& c);

    Device* dev_ = nullptr;
    PixelShaderCache shaders_;
    Stats stats_;
    std::map<std::string, uint64_t> unsupported_;

    Gpu efb_;
    ComPtr<ID3D11RenderTargetView> efbRtv_;
    ComPtr<ID3D11Texture2D> depth_, depthStaging_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    bool depthDirty_ = true;

    ComPtr<ID3D11VertexShader> vs_, copyVs_, clearVs_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11PixelShader> copyPs_, clearPs_, solidPs_;
    ComPtr<ID3D11Buffer> vb_, ib_, tevCb_, copyCb_, clearCb_;
    uint32_t vbCap_ = 0, ibCap_ = 0, vbCursor_ = 0, ibCursor_ = 0;
    std::vector<tev::Vertex> verts_;
    std::vector<uint16_t> idx_;
    ComPtr<ID3D11RasterizerState> raster_, rasterNoScissor_;
    ComPtr<ID3D11BlendState> noBlend_;
    ComPtr<ID3D11DepthStencilState> noDepth_;

    std::unordered_map<uint64_t, Gpu> textures_;       // by Texture::key
    std::unordered_map<uint32_t, Gpu> copyTextures_;   // by EfbCopyCommand::id (copies to textures)
    std::vector<XfbEntry> xfb_;
    uint64_t xfbSeq_ = 0, useClock_ = 0, textureBytes_ = 0;
    ComPtr<ID3D11Query> frameQuery_;
    uint32_t frameDraws_ = 0, frameFirstSeq_ = 0;
    bool frameStats_ = std::getenv("AWL_GFX_FRAMESTATS") != nullptr;
    uint32_t occFrom_ = 0, occCount_ = 0;   // AWL_GFX_OCCLUSION=<first draw seq>,<count>: print the samples each draw passes
    std::map<std::string, ComPtr<ID3D11SamplerState>> samplers_;
    std::map<std::string, ComPtr<ID3D11BlendState>> blends_;
    std::map<std::string, ComPtr<ID3D11DepthStencilState>> depths_;
};

// Presents what the renderer produced for the latched XFB address; falls back to the diagnostic frame (reported once per
// address) when no copy wrote there.
class RendererSource : public FrameSource {
public:
    explicit RendererSource(Renderer& r) : r_(r) {}
    bool produce(const FrameDesc& frame, Image& out, std::string& err) override;
    uint64_t missing() const { return missing_; }

private:
    Renderer& r_;
    DiagnosticSource diag_;
    uint64_t missing_ = 0;
    std::vector<uint32_t> warned_;
};

}  // namespace awl::gfx
