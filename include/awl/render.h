#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "awl/render_validation.h"
#include "awl/tpl.h"

namespace awl {

class RenderContext {
public:
    RenderContext();
    ~RenderContext();
    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;
    RenderContext(RenderContext&&) = delete;
    RenderContext& operator=(RenderContext&&) = delete;

    bool init(HWND hwnd);
    void shutdown();
    
    // Renders the scene (currently either the test triangle or a textured quad)
    bool render();
    uint64_t presented_frame_count() const { return frame_count_; }

    // Dev preview feature: upload a decoded RGBA8 buffer to DX11 for rendering
    bool create_texture_from_rgba8(const uint8_t* pixels, size_t pixel_bytes,
                                   uint32_t width, uint32_t height);

    // Target material path: uploads the complete decoded TPL mip chain and
    // translates its GX wrap/filter/LOD fields to a DX11 sampler.
    bool create_texture_from_decoded_tpl(const TplDecodedTexture& texture);

    // Ordered GPL path: uploads one texture resource into a batch-visible slot.
    bool create_debug_texture_from_decoded_tpl(
        uint32_t texture_slot, const TplDecodedTexture& texture);

    // Supplies the constant raster color for the verified target GPL TEV path.
    void set_debug_material_color_rgba8(uint8_t red, uint8_t green,
                                        uint8_t blue, uint8_t alpha);

    using DebugTexturedVertex = awl::DebugTexturedVertex;

    // Debug mesh feature: upload extracted GPL topology
    bool create_debug_mesh(const DebugTexturedVertex* vertices, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count);
    bool create_debug_mesh_batches(
        const DebugTexturedVertex* vertices, uint32_t vertex_count,
        const uint32_t* indices, uint32_t index_count,
        const DebugDrawBatch* batches, uint32_t batch_count);

private:
    bool init_d3d(HWND hwnd);
    bool init_pipeline();
    bool rebuild_render_target(uint32_t width, uint32_t height, bool resize_buffers);
    bool capture_back_buffer_bmp(const char* native_path);
    bool create_texture_from_decoded_levels(const TplDecodedTexture& texture);
    void cleanup_texture();
    void cleanup_debug_textures();
    void cleanup_debug_mesh();

    HWND hwnd_;
    
    // Core DX11 Device Interfaces
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    IDXGISwapChain* swap_chain_;
    ID3D11RenderTargetView* render_target_view_;
    ID3D11DepthStencilView* depth_stencil_view_;
    uint32_t back_buffer_width_;
    uint32_t back_buffer_height_;
    uint64_t frame_count_;
    bool initialized_;

    // Pipeline State Objects
    ID3D11VertexShader* vertex_shader_;
    ID3D11VertexShader* debug_vertex_shader_;
    ID3D11PixelShader* pixel_shader_;
    ID3D11PixelShader* debug_pixel_shader_;
    ID3D11PixelShader* color_pixel_shader_;
    ID3D11PixelShader* solid_pixel_shader_;
    ID3D11PixelShader* uv_color_pixel_shader_;
    ID3D11PixelShader* force_opaque_pixel_shader_;
    ID3D11PixelShader* debug_alpha_pixel_shader_;
    ID3D11PixelShader* debug_transparency_pixel_shader_;
    ID3D11InputLayout* input_layout_;
    ID3D11InputLayout* debug_input_layout_;
    ID3D11RasterizerState* rs_no_cull_;
    ID3D11RasterizerState* rs_wireframe_;
    ID3D11Buffer* constant_buffer_;
    
    // Triangle fallback resources
    ID3D11Buffer* vertex_buffer_;

    // Texture preview resources
    ID3D11Texture2D* dev_texture_;
    ID3D11ShaderResourceView* dev_srv_;
    ID3D11SamplerState* dev_sampler_;
    ID3D11SamplerState* point_sampler_;
    ID3D11Buffer* quad_vb_;
    ID3D11BlendState* blend_state_;
    ID3D11DepthStencilState* depth_stencil_state_;

    // Debug Mesh resources
    ID3D11Buffer* debug_mesh_vb_;
    ID3D11Buffer* debug_mesh_ib_;
    uint32_t debug_mesh_index_count_;
    std::vector<ID3D11Texture2D*> debug_textures_;
    std::vector<ID3D11ShaderResourceView*> debug_texture_srvs_;
    std::vector<ID3D11SamplerState*> debug_texture_samplers_;
    std::vector<DebugDrawBatch> debug_draw_batches_;
    
    // Cached pixels for CPU sampling diagnostic
    std::vector<uint8_t> cached_texture_pixels_;
    uint32_t cached_texture_width_ = 0;
    uint32_t cached_texture_height_ = 0;
    
    // Camera state
    float cam_eye_[3];
    float cam_at_[3];

    float debug_material_color_[4];
    
    // Dev toggle
    bool flip_v_;
};

} // namespace awl
