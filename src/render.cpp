#include "awl/render.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace awl {
namespace {

bool require_dx_success(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) {
        return true;
    }
    std::cerr << "[ERROR] " << operation << " failed with HRESULT 0x"
              << std::hex << static_cast<uint32_t>(result) << std::dec << std::endl;
    return false;
}

bool compile_shader_blob(const char* label, const char* source, const char* target,
                         ComPtr<ID3DBlob>* out_blob) {
    if (!label || !source || !target || !out_blob) {
        return false;
    }

    ComPtr<ID3DBlob> error_blob;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), label, nullptr, nullptr, "main", target,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, out_blob->ReleaseAndGetAddressOf(),
        error_blob.GetAddressOf());
    if (SUCCEEDED(result)) {
        return true;
    }

    require_dx_success(result, label);
    if (error_blob && error_blob->GetBufferPointer() && error_blob->GetBufferSize() != 0) {
        std::cerr.write(static_cast<const char*>(error_blob->GetBufferPointer()),
                        static_cast<std::streamsize>(error_blob->GetBufferSize()));
        std::cerr << std::endl;
    }
    return false;
}

bool map_gx_wrap_mode(uint32_t gx_wrap, D3D11_TEXTURE_ADDRESS_MODE* out_mode) {
    if (!out_mode) {
        return false;
    }
    switch (gx_wrap) {
        case 0:
            *out_mode = D3D11_TEXTURE_ADDRESS_CLAMP;
            return true;
        case 1:
            *out_mode = D3D11_TEXTURE_ADDRESS_WRAP;
            return true;
        case 2:
            *out_mode = D3D11_TEXTURE_ADDRESS_MIRROR;
            return true;
        default:
            return false;
    }
}

D3D11_FILTER make_basic_filter(bool min_linear, bool mag_linear,
                               bool mip_linear) {
    if (min_linear) {
        if (mag_linear) {
            return mip_linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                              : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        }
        return mip_linear ? D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR
                          : D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    }
    if (mag_linear) {
        return mip_linear ? D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR
                          : D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    }
    return mip_linear ? D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR
                      : D3D11_FILTER_MIN_MAG_MIP_POINT;
}

bool map_gx_filter(uint32_t min_filter, uint32_t mag_filter,
                   D3D11_FILTER* out_filter, bool* out_uses_mips) {
    if (!out_filter || !out_uses_mips || min_filter > 5 || mag_filter > 1) {
        return false;
    }

    const bool min_linear = min_filter == 1 || min_filter == 3 || min_filter == 5;
    const bool mag_linear = mag_filter == 1;
    const bool mip_linear = min_filter == 4 || min_filter == 5;
    *out_uses_mips = min_filter >= 2;
    *out_filter = make_basic_filter(min_linear, mag_linear, mip_linear);
    return true;
}

} // namespace

struct Vertex {
    float x, y, z;
    float r, g, b; // Used by triangle
    float u, v;    // Used by quad
};

struct ConstantBuffer {
    XMMATRIX view_projection;
    XMFLOAT4 material_color;
};

// Simple vertex shader (HLSL) for both triangle and quad (ignores UVs if not provided by layout)
static const char* vertex_shader_source = R"(
cbuffer CBuf : register(b0) {
    matrix view_projection;
};

struct VSInput {
    float3 position : POSITION;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
};
VSOutput main(VSInput input)
{
    VSOutput output;
    // Apply camera transform to all vertices (for 2D stuff it's an ortho or identity anyway, but let's just use the matrix)
    output.position = mul(float4(input.position, 1.0), view_projection);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
)";

// Simple pixel shader (HLSL) with texture support
static const char* pixel_shader_source = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    return tex.Sample(sam, input.uv);
}
)";

// Dedicated debug mesh vertex shader with GPL COLOR0 support.
static const char* debug_vertex_shader_source = R"(
cbuffer CBuf : register(b0) {
    matrix view_projection;
};
struct VSInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), view_projection);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

// Dedicated debug pixel shader (uses real UVs)
static const char* debug_pixel_shader_source = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);
cbuffer CBuf : register(b0) {
    matrix view_projection;
    float4 material_color;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
float4 main(PSInput input) : SV_TARGET
{
    return tex.Sample(sam, input.uv) * input.color * material_color;
}
)";

// Fallback color pixel shader (HLSL)
static const char* color_pixel_shader_source = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    return float4(input.color, 1.0);
}
)";

// Solid color pixel shader
static const char* solid_pixel_shader_source = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    return float4(0.0, 1.0, 0.0, 1.0); // Bright green
}
)";

// UV color pixel shader
static const char* uv_color_pixel_shader_source = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    return float4(frac(input.uv.x), frac(input.uv.y), 0.0, 1.0);
}
)";

// Force opaque pixel shader
static const char* force_opaque_pixel_shader_source = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};
float4 main(PSInput input) : SV_TARGET
{
    float4 color = tex.Sample(sam, input.uv);
    return float4(color.rgb, 1.0);
}
)";

// Debug alpha pixel shader
static const char* debug_alpha_pixel_shader_source = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};
float4 main(PSInput input) : SV_TARGET
{
    float a = tex.Sample(sam, input.uv).a;
    return float4(a, a, a, 1.0);
}
)";

// Debug transparency pixel shader
static const char* debug_transparency_pixel_shader_source = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};
float4 main(PSInput input) : SV_TARGET
{
    float4 color = tex.Sample(sam, input.uv);
    if (color.a < 0.05) {
        return float4(1.0, 0.0, 1.0, 1.0); // Magenta
    }
    return float4(color.rgb, 1.0);
}
)";

RenderContext::RenderContext()
    : hwnd_(nullptr), device_(nullptr), context_(nullptr),
      swap_chain_(nullptr), render_target_view_(nullptr), depth_stencil_view_(nullptr),
      back_buffer_width_(0), back_buffer_height_(0), frame_count_(0), initialized_(false),
      vertex_shader_(nullptr), debug_vertex_shader_(nullptr), pixel_shader_(nullptr),
      debug_pixel_shader_(nullptr), color_pixel_shader_(nullptr), solid_pixel_shader_(nullptr),
      uv_color_pixel_shader_(nullptr), force_opaque_pixel_shader_(nullptr),
      debug_alpha_pixel_shader_(nullptr), debug_transparency_pixel_shader_(nullptr),
      input_layout_(nullptr), debug_input_layout_(nullptr),
      rs_no_cull_(nullptr), rs_wireframe_(nullptr), constant_buffer_(nullptr),
      vertex_buffer_(nullptr),
      dev_texture_(nullptr), dev_srv_(nullptr), dev_sampler_(nullptr), point_sampler_(nullptr),
      quad_vb_(nullptr), blend_state_(nullptr), depth_stencil_state_(nullptr),
      debug_mesh_vb_(nullptr), debug_mesh_ib_(nullptr), debug_mesh_index_count_(0), flip_v_(false) {

    cam_eye_[0] = 0.0f; cam_eye_[1] = 4.0f; cam_eye_[2] = 6.0f;
    cam_at_[0] = 0.0f; cam_at_[1] = 0.0f; cam_at_[2] = 0.0f;
    debug_material_color_[0] = 1.0f;
    debug_material_color_[1] = 1.0f;
    debug_material_color_[2] = 1.0f;
    debug_material_color_[3] = 1.0f;
}

RenderContext::~RenderContext() {
    shutdown();
}

bool RenderContext::init(HWND hwnd) {
    shutdown();
    hwnd_ = hwnd;
    if (!hwnd_) {
        std::cerr << "[ERROR] RenderContext::init received a null window." << std::endl;
        return false;
    }

    if (!init_d3d(hwnd) || !init_pipeline()) {
        shutdown();
        return false;
    }

    initialized_ = true;
    std::cout << "Render context initialized successfully" << std::endl;
    return true;
}

bool RenderContext::init_d3d(HWND hwnd) {
    RECT rect = {};
    if (!GetClientRect(hwnd, &rect)) {
        std::cerr << "[ERROR] GetClientRect failed during renderer initialization."
                  << std::endl;
        return false;
    }
    const UINT width = static_cast<UINT>(rect.right - rect.left);
    const UINT height = static_cast<UINT>(rect.bottom - rect.top);
    if (width == 0 || height == 0) {
        std::cerr << "[ERROR] Renderer cannot initialize a zero-sized back buffer."
                  << std::endl;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;

    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        feature_levels, 1, D3D11_SDK_VERSION, &scd,
        &swap_chain_, &device_, &feature_level, &context_
    );

    if (!require_dx_success(hr, "D3D11CreateDeviceAndSwapChain")) {
        return false;
    }

    return rebuild_render_target(width, height, false);
}

bool RenderContext::rebuild_render_target(uint32_t width, uint32_t height,
                                          bool resize_buffers) {
    if (!device_ || !context_ || !swap_chain_ || width == 0 || height == 0) {
        return false;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    if (render_target_view_) {
        render_target_view_->Release();
        render_target_view_ = nullptr;
    }
    if (depth_stencil_view_) {
        depth_stencil_view_->Release();
        depth_stencil_view_ = nullptr;
    }

    if (resize_buffers) {
        const HRESULT resize_result =
            swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (!require_dx_success(resize_result, "IDXGISwapChain::ResizeBuffers")) {
            return false;
        }
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = swap_chain_->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(back_buffer.GetAddressOf()));
    if (!require_dx_success(result, "IDXGISwapChain::GetBuffer")) {
        return false;
    }

    ComPtr<ID3D11RenderTargetView> render_target;
    result = device_->CreateRenderTargetView(
        back_buffer.Get(), nullptr, render_target.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateRenderTargetView")) {
        return false;
    }

    D3D11_TEXTURE2D_DESC depth_descriptor = {};
    depth_descriptor.Width = width;
    depth_descriptor.Height = height;
    depth_descriptor.MipLevels = 1;
    depth_descriptor.ArraySize = 1;
    depth_descriptor.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_descriptor.SampleDesc.Count = 1;
    depth_descriptor.Usage = D3D11_USAGE_DEFAULT;
    depth_descriptor.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ComPtr<ID3D11Texture2D> depth_texture;
    result = device_->CreateTexture2D(
        &depth_descriptor, nullptr, depth_texture.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateTexture2D(depth)")) {
        return false;
    }

    ComPtr<ID3D11DepthStencilView> depth_stencil_view;
    result = device_->CreateDepthStencilView(
        depth_texture.Get(), nullptr, depth_stencil_view.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateDepthStencilView")) {
        return false;
    }

    render_target_view_ = render_target.Detach();
    depth_stencil_view_ = depth_stencil_view.Detach();
    context_->OMSetRenderTargets(1, &render_target_view_, depth_stencil_view_);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<FLOAT>(width);
    viewport.Height = static_cast<FLOAT>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    back_buffer_width_ = width;
    back_buffer_height_ = height;
    if (resize_buffers) {
        std::cout << "[INFO] DX11 back buffer resized to " << width << 'x' << height
                  << std::endl;
    }
    return true;
}

bool RenderContext::capture_back_buffer_bmp(const char* native_path) {
    if (!native_path || native_path[0] == '\0' || !device_ || !context_ ||
        !swap_chain_) {
        return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = swap_chain_->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(back_buffer.GetAddressOf()));
    if (!require_dx_success(result, "Get back buffer for capture")) {
        return false;
    }

    D3D11_TEXTURE2D_DESC descriptor = {};
    back_buffer->GetDesc(&descriptor);
    if (descriptor.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        descriptor.SampleDesc.Count != 1 || descriptor.Width == 0 ||
        descriptor.Height == 0 ||
        descriptor.Width >
            static_cast<uint32_t>((std::numeric_limits<LONG>::max)()) ||
        descriptor.Height >
            static_cast<uint32_t>((std::numeric_limits<LONG>::max)())) {
        std::cerr << "[ERROR] Unsupported back-buffer layout for BMP capture."
                  << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_descriptor = descriptor;
    staging_descriptor.Usage = D3D11_USAGE_STAGING;
    staging_descriptor.BindFlags = 0;
    staging_descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_descriptor.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    result = device_->CreateTexture2D(&staging_descriptor, nullptr,
                                      staging.GetAddressOf());
    if (!require_dx_success(result, "Create staging texture for capture")) {
        return false;
    }

    context_->CopyResource(staging.Get(), back_buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    result = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (!require_dx_success(result, "Map staging texture for capture")) {
        return false;
    }

    const uint64_t row_bytes = static_cast<uint64_t>(descriptor.Width) * 4;
    const uint64_t pixel_bytes = row_bytes * descriptor.Height;
    const uint64_t file_bytes = sizeof(BITMAPFILEHEADER) +
                                sizeof(BITMAPINFOHEADER) + pixel_bytes;
    if (row_bytes > (std::numeric_limits<DWORD>::max)() ||
        pixel_bytes > (std::numeric_limits<DWORD>::max)() ||
        file_bytes > (std::numeric_limits<DWORD>::max)()) {
        context_->Unmap(staging.Get(), 0);
        return false;
    }

    HANDLE output = CreateFileA(native_path, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        std::cerr << "[ERROR] Unable to create frame capture " << native_path
                  << " (Win32 error " << GetLastError() << ")." << std::endl;
        context_->Unmap(staging.Get(), 0);
        return false;
    }

    BITMAPFILEHEADER file_header = {};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = static_cast<DWORD>(file_bytes);

    BITMAPINFOHEADER info_header = {};
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = static_cast<LONG>(descriptor.Width);
    info_header.biHeight = -static_cast<LONG>(descriptor.Height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = static_cast<DWORD>(pixel_bytes);

    const auto write_bytes = [output](const void* data, DWORD size) {
        DWORD written = 0;
        return WriteFile(output, data, size, &written, nullptr) != FALSE &&
               written == size;
    };

    bool success = write_bytes(
                       &file_header, static_cast<DWORD>(sizeof(file_header))) &&
                   write_bytes(
                       &info_header, static_cast<DWORD>(sizeof(info_header)));
    std::vector<uint8_t> bgra_row;
    try {
        bgra_row.resize(static_cast<size_t>(row_bytes));
    } catch (const std::bad_alloc&) {
        success = false;
    } catch (const std::length_error&) {
        success = false;
    }

    for (uint32_t y = 0; success && y < descriptor.Height; ++y) {
        const auto* rgba = static_cast<const uint8_t*>(mapped.pData) +
                           static_cast<size_t>(y) * mapped.RowPitch;
        for (uint32_t x = 0; x < descriptor.Width; ++x) {
            const size_t pixel = static_cast<size_t>(x) * 4;
            bgra_row[pixel] = rgba[pixel + 2];
            bgra_row[pixel + 1] = rgba[pixel + 1];
            bgra_row[pixel + 2] = rgba[pixel];
            bgra_row[pixel + 3] = 0xFF;
        }
        success = write_bytes(bgra_row.data(), static_cast<DWORD>(row_bytes));
    }

    CloseHandle(output);
    context_->Unmap(staging.Get(), 0);
    if (!success) {
        std::cerr << "[ERROR] Failed while writing frame capture " << native_path
                  << "." << std::endl;
        return false;
    }

    std::cout << "[INFO] Captured DX11 back buffer to " << native_path
              << std::endl;
    return true;
}

bool RenderContext::init_pipeline() {
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> debug_vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> debug_ps_blob;
    ComPtr<ID3DBlob> color_ps_blob;
    ComPtr<ID3DBlob> solid_ps_blob;
    ComPtr<ID3DBlob> uv_ps_blob;
    ComPtr<ID3DBlob> opaque_ps_blob;
    ComPtr<ID3DBlob> alpha_ps_blob;
    ComPtr<ID3DBlob> transparency_ps_blob;

    if (!compile_shader_blob("main vertex shader", vertex_shader_source, "vs_4_0", &vs_blob) ||
        !compile_shader_blob("debug vertex shader", debug_vertex_shader_source, "vs_4_0", &debug_vs_blob) ||
        !compile_shader_blob("texture pixel shader", pixel_shader_source, "ps_4_0", &ps_blob) ||
        !compile_shader_blob("debug texture pixel shader", debug_pixel_shader_source, "ps_4_0", &debug_ps_blob) ||
        !compile_shader_blob("color pixel shader", color_pixel_shader_source, "ps_4_0", &color_ps_blob) ||
        !compile_shader_blob("solid pixel shader", solid_pixel_shader_source, "ps_4_0", &solid_ps_blob) ||
        !compile_shader_blob("UV pixel shader", uv_color_pixel_shader_source, "ps_4_0", &uv_ps_blob) ||
        !compile_shader_blob("opaque pixel shader", force_opaque_pixel_shader_source, "ps_4_0", &opaque_ps_blob) ||
        !compile_shader_blob("alpha pixel shader", debug_alpha_pixel_shader_source, "ps_4_0", &alpha_ps_blob) ||
        !compile_shader_blob("transparency pixel shader", debug_transparency_pixel_shader_source, "ps_4_0", &transparency_ps_blob)) {
        return false;
    }

    HRESULT result = device_->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreateVertexShader(main)")) return false;
    result = device_->CreateVertexShader(
        debug_vs_blob->GetBufferPointer(), debug_vs_blob->GetBufferSize(), nullptr,
        &debug_vertex_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreateVertexShader(debug)")) return false;
    result = device_->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(texture)")) return false;
    result = device_->CreatePixelShader(
        debug_ps_blob->GetBufferPointer(), debug_ps_blob->GetBufferSize(), nullptr,
        &debug_pixel_shader_);
    if (!require_dx_success(
            result, "ID3D11Device::CreatePixelShader(debug texture)")) return false;
    result = device_->CreatePixelShader(
        color_ps_blob->GetBufferPointer(), color_ps_blob->GetBufferSize(), nullptr,
        &color_pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(color)")) return false;
    result = device_->CreatePixelShader(
        solid_ps_blob->GetBufferPointer(), solid_ps_blob->GetBufferSize(), nullptr,
        &solid_pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(solid)")) return false;
    result = device_->CreatePixelShader(
        uv_ps_blob->GetBufferPointer(), uv_ps_blob->GetBufferSize(), nullptr,
        &uv_color_pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(UV)")) return false;
    result = device_->CreatePixelShader(
        opaque_ps_blob->GetBufferPointer(), opaque_ps_blob->GetBufferSize(), nullptr,
        &force_opaque_pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(opaque)")) return false;
    result = device_->CreatePixelShader(
        alpha_ps_blob->GetBufferPointer(), alpha_ps_blob->GetBufferSize(), nullptr,
        &debug_alpha_pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(alpha)")) return false;
    result = device_->CreatePixelShader(
        transparency_ps_blob->GetBufferPointer(), transparency_ps_blob->GetBufferSize(),
        nullptr, &debug_transparency_pixel_shader_);
    if (!require_dx_success(result, "ID3D11Device::CreatePixelShader(transparency)")) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    result = device_->CreateInputLayout(
        layout, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout_);
    if (!require_dx_success(result, "ID3D11Device::CreateInputLayout(main)")) return false;
    
    D3D11_INPUT_ELEMENT_DESC debug_layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    result = device_->CreateInputLayout(
        debug_layout, 3, debug_vs_blob->GetBufferPointer(), debug_vs_blob->GetBufferSize(),
        &debug_input_layout_);
    if (!require_dx_success(result, "ID3D11Device::CreateInputLayout(debug)")) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    result = device_->CreateRasterizerState(&rd, &rs_no_cull_);
    if (!require_dx_success(result, "ID3D11Device::CreateRasterizerState(solid)")) return false;

    D3D11_RASTERIZER_DESC wd = {};
    wd.FillMode = D3D11_FILL_WIREFRAME;
    wd.CullMode = D3D11_CULL_NONE;
    result = device_->CreateRasterizerState(&wd, &rs_wireframe_);
    if (!require_dx_success(result, "ID3D11Device::CreateRasterizerState(wireframe)")) return false;

    // Constant Buffer for Camera
    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.ByteWidth = sizeof(ConstantBuffer);
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = device_->CreateBuffer(&cb_desc, nullptr, &constant_buffer_);
    if (!require_dx_success(result, "ID3D11Device::CreateBuffer(constants)")) return false;

    // Triangle VB
    Vertex tri_verts[] = {
        { 0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f },
        {-0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f },
        { 0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f }
    };
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(tri_verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = tri_verts;
    result = device_->CreateBuffer(&bd, &init_data, &vertex_buffer_);
    if (!require_dx_success(result, "ID3D11Device::CreateBuffer(fallback triangle)")) return false;

    // Quad VB (centered, preserve a rough aspect ratio for 184x48)
    // 184 / 48 = 3.83. Let's make it width=1.0, height=0.26
    Vertex quad_verts[] = {
        { -1.0f,  0.26f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f, 0.0f }, // Top left
        {  1.0f,  0.26f, 0.0f,  0.0f, 0.0f, 0.0f,  1.0f, 0.0f }, // Top right
        { -1.0f, -0.26f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f, 1.0f }, // Bottom left
        {  1.0f, -0.26f, 0.0f,  0.0f, 0.0f, 0.0f,  1.0f, 1.0f }  // Bottom right
    };
    bd.ByteWidth = sizeof(quad_verts);
    init_data.pSysMem = quad_verts;
    result = device_->CreateBuffer(&bd, &init_data, &quad_vb_);
    if (!require_dx_success(result, "ID3D11Device::CreateBuffer(texture quad)")) return false;

    // The main world pass calls GXSetBlendMode(GX_BM_BLEND,
    // GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR).
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    result = device_->CreateBlendState(&blend_desc, &blend_state_);
    if (!require_dx_success(result, "ID3D11Device::CreateBlendState")) return false;

    // The same pass restores GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE)
    // before dispatching the terrain draw task.
    D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
    depth_stencil_desc.DepthEnable = TRUE;
    depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_stencil_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    result = device_->CreateDepthStencilState(
        &depth_stencil_desc, &depth_stencil_state_);
    if (!require_dx_success(
            result, "ID3D11Device::CreateDepthStencilState(target pass)")) {
        return false;
    }

    // Sampler State
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    result = device_->CreateSamplerState(&samp_desc, &dev_sampler_);
    if (!require_dx_success(result, "ID3D11Device::CreateSamplerState(linear)")) return false;

    D3D11_SAMPLER_DESC pt_samp_desc = samp_desc;
    pt_samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    result = device_->CreateSamplerState(&pt_samp_desc, &point_sampler_);
    if (!require_dx_success(result, "ID3D11Device::CreateSamplerState(point)")) return false;

    return true;
}

bool RenderContext::create_texture_from_rgba8(const uint8_t* pixels, size_t pixel_bytes,
                                              uint32_t width, uint32_t height) {
    size_t required_bytes = 0;
    if (!initialized_ || !device_ || !pixels ||
        width > (std::numeric_limits<uint16_t>::max)() ||
        height > (std::numeric_limits<uint16_t>::max)() ||
        !validate_rgba8_texture_upload(
            width, height, pixel_bytes, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
            &required_bytes)) {
        std::cerr << "[ERROR] Rejected invalid RGBA8 texture upload." << std::endl;
        return false;
    }

    TplDecodedTexture texture;
    texture.header.width = static_cast<uint16_t>(width);
    texture.header.height = static_cast<uint16_t>(height);
    texture.header.wrap_s = 0;
    texture.header.wrap_t = 0;
    texture.header.min_filter = 1;
    texture.header.mag_filter = 1;
    TplDecodedMipLevel level;
    level.width = width;
    level.height = height;
    try {
        level.rgba8.assign(pixels, pixels + required_bytes);
        texture.mip_levels.push_back(std::move(level));
    } catch (const std::bad_alloc&) {
        std::cerr << "[ERROR] Unable to stage RGBA8 texture pixels." << std::endl;
        return false;
    } catch (const std::length_error&) {
        std::cerr << "[ERROR] RGBA8 texture size is invalid." << std::endl;
        return false;
    }
    return create_texture_from_decoded_levels(texture);
}

bool RenderContext::create_texture_from_decoded_tpl(
    const TplDecodedTexture& texture) {
    return create_texture_from_decoded_levels(texture);
}

bool RenderContext::create_debug_texture_from_decoded_tpl(
    uint32_t texture_slot, const TplDecodedTexture& texture) {
    constexpr uint32_t kMaxDebugTextureSlots = 64;
    if (texture_slot >= kMaxDebugTextureSlots ||
        !create_texture_from_decoded_levels(texture)) {
        return false;
    }

    try {
        const size_t required_slots = static_cast<size_t>(texture_slot) + 1;
        if (required_slots > debug_textures_.size()) {
            // Reserve every allocation before changing any vector size. A
            // failed allocation therefore cannot leave the three resource
            // arrays with mismatched lengths, and replacing a lower slot
            // never truncates or leaks resources in higher slots.
            debug_textures_.reserve(required_slots);
            debug_texture_srvs_.reserve(required_slots);
            debug_texture_samplers_.reserve(required_slots);
            debug_textures_.resize(required_slots, nullptr);
            debug_texture_srvs_.resize(required_slots, nullptr);
            debug_texture_samplers_.resize(required_slots, nullptr);
        }
    } catch (const std::bad_alloc&) {
        cleanup_texture();
        return false;
    } catch (const std::length_error&) {
        cleanup_texture();
        return false;
    }

    if (debug_texture_samplers_[texture_slot]) {
        debug_texture_samplers_[texture_slot]->Release();
    }
    if (debug_texture_srvs_[texture_slot]) {
        debug_texture_srvs_[texture_slot]->Release();
    }
    if (debug_textures_[texture_slot]) {
        debug_textures_[texture_slot]->Release();
    }
    debug_textures_[texture_slot] = dev_texture_;
    debug_texture_srvs_[texture_slot] = dev_srv_;
    debug_texture_samplers_[texture_slot] = dev_sampler_;
    dev_texture_ = nullptr;
    dev_srv_ = nullptr;
    dev_sampler_ = nullptr;
    return true;
}

bool RenderContext::create_texture_from_decoded_levels(
    const TplDecodedTexture& texture) {
    if (!initialized_ || !device_ || texture.mip_levels.empty() ||
        texture.mip_levels.size() !=
            static_cast<size_t>(texture.header.max_lod) + 1 ||
        texture.header.min_lod > texture.header.max_lod ||
        texture.header.edge_lod != 0 || texture.header.unpacked != 0 ||
        !std::isfinite(texture.header.lod_bias) ||
        texture.header.lod_bias < D3D11_MIP_LOD_BIAS_MIN ||
        texture.header.lod_bias > D3D11_MIP_LOD_BIAS_MAX) {
        std::cerr << "[ERROR] Rejected unsupported decoded TPL state." << std::endl;
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc = {};
    bool uses_mips = false;
    if (!map_gx_wrap_mode(texture.header.wrap_s, &sampler_desc.AddressU) ||
        !map_gx_wrap_mode(texture.header.wrap_t, &sampler_desc.AddressV) ||
        !map_gx_filter(texture.header.min_filter, texture.header.mag_filter,
                       &sampler_desc.Filter, &uses_mips)) {
        std::cerr << "[ERROR] Rejected unsupported GX sampler state." << std::endl;
        return false;
    }
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MipLODBias = texture.header.lod_bias;
    sampler_desc.MaxAnisotropy = 1;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = uses_mips ? static_cast<float>(texture.header.min_lod) : 0.0f;
    sampler_desc.MaxLOD = uses_mips ? static_cast<float>(texture.header.max_lod) : 0.0f;

    std::vector<D3D11_SUBRESOURCE_DATA> subresources;
    try {
        subresources.resize(texture.mip_levels.size());
    } catch (const std::bad_alloc&) {
        std::cerr << "[ERROR] Unable to allocate DX11 mip descriptors." << std::endl;
        return false;
    } catch (const std::length_error&) {
        std::cerr << "[ERROR] DX11 mip descriptor count is invalid." << std::endl;
        return false;
    }

    uint32_t expected_width = texture.header.width;
    uint32_t expected_height = texture.header.height;
    for (size_t index = 0; index < texture.mip_levels.size(); ++index) {
        const TplDecodedMipLevel& level = texture.mip_levels[index];
        size_t required_bytes = 0;
        if (level.width != expected_width || level.height != expected_height ||
            !validate_rgba8_texture_upload(
                level.width, level.height, level.rgba8.size(),
                D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, &required_bytes)) {
            std::cerr << "[ERROR] Rejected invalid RGBA8 mip level " << index
                      << "." << std::endl;
            return false;
        }
        subresources[index].pSysMem = level.rgba8.data();
        subresources[index].SysMemPitch = level.width * 4;
        expected_width = (std::max)(1U, expected_width / 2);
        expected_height = (std::max)(1U, expected_height / 2);
    }

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = texture.header.width;
    tex_desc.Height = texture.header.height;
    tex_desc.MipLevels = static_cast<UINT>(texture.mip_levels.size());
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> gpu_texture;
    HRESULT result =
        device_->CreateTexture2D(&tex_desc, subresources.data(),
                                 gpu_texture.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateTexture2D")) {
        return false;
    }

    ComPtr<ID3D11ShaderResourceView> shader_resource;
    result = device_->CreateShaderResourceView(
        gpu_texture.Get(), nullptr, shader_resource.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateShaderResourceView")) {
        return false;
    }

    ComPtr<ID3D11SamplerState> sampler;
    result = device_->CreateSamplerState(&sampler_desc, sampler.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateSamplerState(TPL)")) {
        return false;
    }

    cleanup_texture();
    dev_texture_ = gpu_texture.Detach();
    dev_srv_ = shader_resource.Detach();
    dev_sampler_ = sampler.Detach();
    cached_texture_pixels_ = texture.mip_levels.front().rgba8;
    cached_texture_width_ = texture.header.width;
    cached_texture_height_ = texture.header.height;

    std::cout << "DX11 texture created with " << texture.mip_levels.size()
              << " mip level(s); GX sampler translated as wrap=("
              << texture.header.wrap_s << ',' << texture.header.wrap_t
              << "), filter=(" << texture.header.min_filter << ','
              << texture.header.mag_filter << "), LOD="
              << static_cast<uint32_t>(texture.header.min_lod) << ".."
              << static_cast<uint32_t>(texture.header.max_lod) << ", bias="
              << texture.header.lod_bias << "." << std::endl;
    return true;
}

bool RenderContext::create_debug_mesh(const DebugTexturedVertex* vertices, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count) {
    size_t vertex_bytes = 0;
    size_t index_bytes = 0;
    if (!initialized_ || !device_ ||
        !validate_debug_mesh_upload(vertices, vertex_count, indices, index_count,
                                    &vertex_bytes, &index_bytes)) {
        std::cerr << "[ERROR] Rejected invalid debug mesh upload." << std::endl;
        return false;
    }

    // Optional flip V mapping and Bounds calculation
    float min_x = 999999.0f, min_y = 999999.0f, min_z = 999999.0f;
    float max_x = -999999.0f, max_y = -999999.0f, max_z = -999999.0f;
    float min_u = 999999.0f, max_u = -999999.0f;
    float min_v = 999999.0f, max_v = -999999.0f;

    std::vector<DebugTexturedVertex> processed_verts;
    try {
        processed_verts.assign(vertices, vertices + vertex_count);
    } catch (const std::bad_alloc&) {
        std::cerr << "[ERROR] Unable to allocate processed debug vertices." << std::endl;
        return false;
    } catch (const std::length_error&) {
        std::cerr << "[ERROR] Debug vertex count is invalid." << std::endl;
        return false;
    }
    for (uint32_t i = 0; i < vertex_count; ++i) {
        if (flip_v_) {
            processed_verts[i].v = 1.0f - processed_verts[i].v;
        }

        min_x = min(min_x, processed_verts[i].x);
        min_y = min(min_y, processed_verts[i].y);
        min_z = min(min_z, processed_verts[i].z);
        
        max_x = max(max_x, processed_verts[i].x);
        max_y = max(max_y, processed_verts[i].y);
        max_z = max(max_z, processed_verts[i].z);

        min_u = min(min_u, vertices[i].u);
        max_u = max(max_u, vertices[i].u);
        min_v = min(min_v, vertices[i].v);
        max_v = max(max_v, vertices[i].v);
    }
    
    // CPU-side texture sampling diagnostic
    if (!cached_texture_pixels_.empty() && cached_texture_width_ > 0 && cached_texture_height_ > 0) {
        std::cout << "[INFO] CPU-side texture sampling diagnostic for first 10 UVs:" << std::endl;
        for (uint32_t i = 0; i < min((uint32_t)10, vertex_count); ++i) {
            float u = processed_verts[i].u;
            float v = processed_verts[i].v;
            
            // Handle wrap/clamp (assuming wrap for fractional part)
            float frac_u = u - floorf(u);
            float frac_v = v - floorf(v);
            
            uint32_t tx = static_cast<uint32_t>(frac_u * cached_texture_width_);
            uint32_t ty = static_cast<uint32_t>(frac_v * cached_texture_height_);
            
            // Clamp strictly to bounds to prevent out-of-bounds
            tx = min(tx, cached_texture_width_ - 1);
            ty = min(ty, cached_texture_height_ - 1);
            
            uint32_t pixel_idx = (ty * cached_texture_width_ + tx) * 4;
            if (pixel_idx + 3 < cached_texture_pixels_.size()) {
                uint8_t r = cached_texture_pixels_[pixel_idx + 0];
                uint8_t g = cached_texture_pixels_[pixel_idx + 1];
                uint8_t b = cached_texture_pixels_[pixel_idx + 2];
                uint8_t a = cached_texture_pixels_[pixel_idx + 3];
                std::cout << "GPU UV " << i << ": u=" << u << " v=" << v 
                          << " -> pixel=(" << tx << "," << ty << ") RGBA=(" 
                          << (int)r << "," << (int)g << "," << (int)b << "," << (int)a << ")" << std::endl;
            }
        }
    }
    
    float cx = (min_x + max_x) * 0.5f;
    float cy = (min_y + max_y) * 0.5f;
    float cz = (min_z + max_z) * 0.5f;
    
    float dx = max_x - min_x;
    float dy = max_y - min_y;
    float dz = max_z - min_z;
    float radius = max(max(dx, dy), dz);
    if (radius < 1.0f) radius = 10.0f; // fallback

    bool normalize_placement = (getenv("AWL_NORMALIZED_PLACEMENT") != nullptr && strcmp(getenv("AWL_NORMALIZED_PLACEMENT"), "1") == 0);
    if (normalize_placement) {
        std::cout << "[INFO] Using normalized debug mesh placement for visual validation" << std::endl;
        float scale = 1.0f / (radius * 0.5f);
        for (uint32_t i = 0; i < vertex_count; ++i) {
            processed_verts[i].x = (processed_verts[i].x - cx) * scale;
            processed_verts[i].y = (processed_verts[i].y - cy) * scale;
            processed_verts[i].z = (processed_verts[i].z - cz) * scale;
        }
        // Recompute bounds
        min_x = -2.0f; max_x = 2.0f;
        min_y = -2.0f; max_y = 2.0f;
        min_z = -2.0f; max_z = 2.0f;
        cx = 0.0f; cy = 0.0f; cz = 0.0f;
        radius = 2.0f;
    }
    
    std::cout << "[INFO] UV Bounds before flip: U[" << min_u << ", " << max_u << "] V[" << min_v << ", " << max_v << "]" << std::endl;
    std::cout << "[INFO] flip_v_ enabled: " << (flip_v_ ? "true" : "false") << std::endl;

    for (uint32_t i = 0; i < min((uint32_t)10, vertex_count); ++i) {
        std::cout << "[INFO] GPU vertex " << i << ": pos=(" 
                  << processed_verts[i].x << "," << processed_verts[i].y << "," << processed_verts[i].z 
                  << "), uv=(" << processed_verts[i].u << "," << processed_verts[i].v << ")" << std::endl;
    }

    std::cout << "[INFO] Mesh bounds: [" << min_x << ", " << min_y << ", " << min_z << "] to [" << max_x << ", " << max_y << ", " << max_z << "]" << std::endl;
    std::cout << "[INFO] Mesh center: " << cx << ", " << cy << ", " << cz << " (Radius: " << radius << ")" << std::endl;

    // Position camera dynamically based on radius
    const float new_cam_at[3] = {cx, cy, cz};
    const float new_cam_eye[3] = {
        cx, cy + (radius * 0.8f), cz + (radius * 1.5f)};
    
    std::cout << "[INFO] Camera Eye: " << new_cam_eye[0] << ", " << new_cam_eye[1]
              << ", " << new_cam_eye[2] << std::endl;
    std::cout << "[INFO] Camera At:  " << new_cam_at[0] << ", " << new_cam_at[1]
              << ", " << new_cam_at[2] << std::endl;
    std::cout << "[INFO] Camera source: diagnostic mesh auto-fit; gameplay camera "
                 "state is not translated yet."
              << std::endl;

    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.Usage = D3D11_USAGE_DEFAULT;
    vb_desc.ByteWidth = static_cast<UINT>(vertex_bytes);
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = processed_verts.data();
    
    ComPtr<ID3D11Buffer> vertex_buffer;
    HRESULT result =
        device_->CreateBuffer(&vb_desc, &vb_data, vertex_buffer.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateBuffer(debug vertices)")) {
        return false;
    }
    std::cout << "[INFO] Debug Vertex buffer created." << std::endl;

    D3D11_BUFFER_DESC ib_desc = {};
    ib_desc.Usage = D3D11_USAGE_DEFAULT;
    ib_desc.ByteWidth = static_cast<UINT>(index_bytes);
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA ib_data = {};
    ib_data.pSysMem = indices;
    
    ComPtr<ID3D11Buffer> index_buffer;
    result = device_->CreateBuffer(&ib_desc, &ib_data, index_buffer.GetAddressOf());
    if (!require_dx_success(result, "ID3D11Device::CreateBuffer(debug indices)")) {
        return false;
    }
    std::cout << "[INFO] Debug Index buffer created." << std::endl;

    cleanup_debug_mesh();
    debug_mesh_vb_ = vertex_buffer.Detach();
    debug_mesh_ib_ = index_buffer.Detach();
    debug_mesh_index_count_ = index_count;
    for (size_t i = 0; i < 3; ++i) {
        cam_at_[i] = new_cam_at[i];
        cam_eye_[i] = new_cam_eye[i];
    }
    std::cout << "[INFO] Render mode: real-UV GPL textured mesh" << std::endl;
    return true;
}

bool RenderContext::create_debug_mesh_batches(
    const DebugTexturedVertex* vertices, uint32_t vertex_count,
    const uint32_t* indices, uint32_t index_count,
    const DebugDrawBatch* batches, uint32_t batch_count) {
    if (debug_texture_srvs_.size() >
            (std::numeric_limits<uint32_t>::max)() ||
        !validate_debug_draw_batches(
            batches, batch_count, index_count,
            static_cast<uint32_t>(debug_texture_srvs_.size())) ||
        !create_debug_mesh(vertices, vertex_count, indices, index_count)) {
        return false;
    }

    for (uint32_t index = 0; index < batch_count; ++index) {
        const uint32_t texture_slot = batches[index].texture_slot;
        if (!debug_textures_[texture_slot] ||
            !debug_texture_srvs_[texture_slot] ||
            !debug_texture_samplers_[texture_slot]) {
            cleanup_debug_mesh();
            return false;
        }
    }
    try {
        debug_draw_batches_.assign(batches, batches + batch_count);
    } catch (const std::bad_alloc&) {
        cleanup_debug_mesh();
        return false;
    } catch (const std::length_error&) {
        cleanup_debug_mesh();
        return false;
    }
    std::cout << "[INFO] Ordered debug draw batch count: "
              << debug_draw_batches_.size() << std::endl;
    return true;
}

void RenderContext::set_debug_material_color_rgba8(
    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    constexpr float kByteToFloat = 1.0f / 255.0f;
    debug_material_color_[0] = static_cast<float>(red) * kByteToFloat;
    debug_material_color_[1] = static_cast<float>(green) * kByteToFloat;
    debug_material_color_[2] = static_cast<float>(blue) * kByteToFloat;
    debug_material_color_[3] = static_cast<float>(alpha) * kByteToFloat;
    std::cout << "DX11 target material color set to RGBA=("
              << static_cast<uint32_t>(red) << ','
              << static_cast<uint32_t>(green) << ','
              << static_cast<uint32_t>(blue) << ','
              << static_cast<uint32_t>(alpha) << ")." << std::endl;
}

void RenderContext::cleanup_debug_mesh() {
    if (debug_mesh_vb_) { debug_mesh_vb_->Release(); debug_mesh_vb_ = nullptr; }
    if (debug_mesh_ib_) { debug_mesh_ib_->Release(); debug_mesh_ib_ = nullptr; }
    debug_mesh_index_count_ = 0;
    debug_draw_batches_.clear();
}

void RenderContext::cleanup_debug_textures() {
    for (ID3D11SamplerState* sampler : debug_texture_samplers_) {
        if (sampler) {
            sampler->Release();
        }
    }
    for (ID3D11ShaderResourceView* srv : debug_texture_srvs_) {
        if (srv) {
            srv->Release();
        }
    }
    for (ID3D11Texture2D* texture : debug_textures_) {
        if (texture) {
            texture->Release();
        }
    }
    debug_texture_samplers_.clear();
    debug_texture_srvs_.clear();
    debug_textures_.clear();
}

void RenderContext::cleanup_texture() {
    if (dev_srv_) { dev_srv_->Release(); dev_srv_ = nullptr; }
    if (dev_texture_) { dev_texture_->Release(); dev_texture_ = nullptr; }
    if (dev_sampler_) { dev_sampler_->Release(); dev_sampler_ = nullptr; }
    cached_texture_pixels_.clear();
    cached_texture_width_ = 0;
    cached_texture_height_ = 0;
}

void RenderContext::shutdown() {
    initialized_ = false;
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }

    cleanup_texture();
    cleanup_debug_textures();
    if (point_sampler_) { point_sampler_->Release(); point_sampler_ = nullptr; }
    if (quad_vb_) { quad_vb_->Release(); quad_vb_ = nullptr; }
    cleanup_debug_mesh();
    if (blend_state_) { blend_state_->Release(); blend_state_ = nullptr; }
    if (depth_stencil_state_) {
        depth_stencil_state_->Release();
        depth_stencil_state_ = nullptr;
    }
    if (rs_no_cull_) { rs_no_cull_->Release(); rs_no_cull_ = nullptr; }
    if (rs_wireframe_) { rs_wireframe_->Release(); rs_wireframe_ = nullptr; }
    if (constant_buffer_) { constant_buffer_->Release(); constant_buffer_ = nullptr; }
    
    if (input_layout_) { input_layout_->Release(); input_layout_ = nullptr; }
    if (debug_input_layout_) { debug_input_layout_->Release(); debug_input_layout_ = nullptr; }
    if (pixel_shader_) { pixel_shader_->Release(); pixel_shader_ = nullptr; }
    if (debug_pixel_shader_) { debug_pixel_shader_->Release(); debug_pixel_shader_ = nullptr; }
    if (color_pixel_shader_) { color_pixel_shader_->Release(); color_pixel_shader_ = nullptr; }
    if (solid_pixel_shader_) { solid_pixel_shader_->Release(); solid_pixel_shader_ = nullptr; }
    if (uv_color_pixel_shader_) { uv_color_pixel_shader_->Release(); uv_color_pixel_shader_ = nullptr; }
    if (force_opaque_pixel_shader_) { force_opaque_pixel_shader_->Release(); force_opaque_pixel_shader_ = nullptr; }
    if (debug_alpha_pixel_shader_) { debug_alpha_pixel_shader_->Release(); debug_alpha_pixel_shader_ = nullptr; }
    if (debug_transparency_pixel_shader_) { debug_transparency_pixel_shader_->Release(); debug_transparency_pixel_shader_ = nullptr; }
    if (vertex_shader_) { vertex_shader_->Release(); vertex_shader_ = nullptr; }
    if (debug_vertex_shader_) { debug_vertex_shader_->Release(); debug_vertex_shader_ = nullptr; }
    if (vertex_buffer_) { vertex_buffer_->Release(); vertex_buffer_ = nullptr; }
    
    if (depth_stencil_view_) {
        depth_stencil_view_->Release();
        depth_stencil_view_ = nullptr;
    }
    if (render_target_view_) { render_target_view_->Release(); render_target_view_ = nullptr; }
    if (swap_chain_) { swap_chain_->Release(); swap_chain_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    hwnd_ = nullptr;
    back_buffer_width_ = 0;
    back_buffer_height_ = 0;
    frame_count_ = 0;
}

bool RenderContext::render() {
    if (!initialized_ || !hwnd_ || !device_ || !context_ || !swap_chain_ ||
        !render_target_view_ || !depth_stencil_view_ || !depth_stencil_state_) {
        std::cerr << "[ERROR] Render requested without a complete DX11 context."
                  << std::endl;
        return false;
    }

    RECT client_rect = {};
    if (!GetClientRect(hwnd_, &client_rect)) {
        std::cerr << "[ERROR] GetClientRect failed during rendering." << std::endl;
        return false;
    }
    const uint32_t client_width =
        static_cast<uint32_t>(client_rect.right - client_rect.left);
    const uint32_t client_height =
        static_cast<uint32_t>(client_rect.bottom - client_rect.top);
    if (client_width == 0 || client_height == 0) {
        return true;
    }
    if ((client_width != back_buffer_width_ || client_height != back_buffer_height_) &&
        !rebuild_render_target(client_width, client_height, true)) {
        return false;
    }

    // Dark blue background to make alpha obvious
    float clear_color[4] = { 0.05f, 0.1f, 0.2f, 1.0f };
    context_->ClearRenderTargetView(render_target_view_, clear_color);
    context_->ClearDepthStencilView(
        depth_stencil_view_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    uint64_t& frame_count = frame_count_;
    bool log_frames = (getenv("AWL_FRAME_LOGS") != nullptr && strcmp(getenv("AWL_FRAME_LOGS"), "1") == 0);
    
    if (frame_count == 0) {
        std::cout << "[INFO] Target pass depth state: enabled, LEQUAL, writes enabled"
                  << std::endl;
        std::cout << "[INFO] Culling disabled for real-UV debug mesh" << std::endl;
    }

    context_->IASetInputLayout(input_layout_);
    context_->VSSetShader(vertex_shader_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constant_buffer_);

    float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context_->OMSetBlendState(blend_state_, blend_factor, 0xffffffff);
    context_->OMSetDepthStencilState(depth_stencil_state_, 0);

    UINT stride = sizeof(Vertex);
    UINT debug_stride = sizeof(DebugTexturedVertex);
    UINT offset = 0;
    
    // Hard fallback modes check
    bool render_debug_mesh = false;
    const bool uses_ordered_batches = !debug_draw_batches_.empty();
    bool ordered_textures_valid = uses_ordered_batches;
    if (uses_ordered_batches) {
        for (const DebugDrawBatch& batch : debug_draw_batches_) {
            if (batch.texture_slot >= debug_texture_srvs_.size() ||
                batch.texture_slot >= debug_texture_samplers_.size() ||
                !debug_texture_srvs_[batch.texture_slot] ||
                !debug_texture_samplers_[batch.texture_slot]) {
                ordered_textures_valid = false;
                break;
            }
        }
    }
    if (debug_mesh_vb_ && debug_mesh_ib_) {
        if (frame_count == 0) {
            std::cout << "[INFO] real-UV render branch reached" << std::endl;
            std::cout << "[INFO]   vertex buffer valid: " << (debug_mesh_vb_ ? "yes" : "no") << std::endl;
            std::cout << "[INFO]   index buffer valid: " << (debug_mesh_ib_ ? "yes" : "no") << std::endl;
            std::cout << "[INFO]   vertex shader valid: " << (debug_vertex_shader_ ? "yes" : "no") << std::endl;
            std::cout << "[INFO]   pixel shader valid: " << (debug_pixel_shader_ ? "yes" : "no") << std::endl;
            std::cout << "[INFO]   input layout valid: " << (debug_input_layout_ ? "yes" : "no") << std::endl;
            std::cout << "[INFO]   constant buffer valid: " << (constant_buffer_ ? "yes" : "no") << std::endl;
            std::cout << "[INFO]   texture SRV valid: "
                      << ((dev_srv_ || ordered_textures_valid) ? "yes" : "no")
                      << std::endl;
            std::cout << "[INFO]   sampler state valid: "
                      << ((dev_sampler_ || ordered_textures_valid) ? "yes" : "no")
                      << std::endl;
            std::cout << "[INFO]   target alpha blend state: SRC_ALPHA / INV_SRC_ALPHA"
                      << std::endl;
            std::cout << "[INFO]   target depth state: LEQUAL / write"
                      << std::endl;
            std::cout << "[INFO]   index count: " << debug_mesh_index_count_ << std::endl;
            if (uses_ordered_batches) {
                std::cout << "[INFO]   ordered draw batches: "
                          << debug_draw_batches_.size() << std::endl;
            }
        }

        if (!debug_input_layout_ || !debug_vertex_shader_ || !debug_pixel_shader_ ||
            !constant_buffer_ ||
            debug_stride == 0 || debug_mesh_index_count_ == 0 ||
            (uses_ordered_batches && !ordered_textures_valid)) {
            std::cerr << "[ERROR] Required debug mesh pipeline is incomplete."
                      << std::endl;
            return false;
        }
        render_debug_mesh = true;
    }

    if (render_debug_mesh) {
        // Camera positioned based on dynamic mesh bounds
        XMVECTOR eye = XMVectorSet(cam_eye_[0], cam_eye_[1], cam_eye_[2], 0.0f);
        XMVECTOR at = XMVectorSet(cam_at_[0], cam_at_[1], cam_at_[2], 0.0f);
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
        
        RECT rect;
        GetClientRect(hwnd_, &rect);
        float width = static_cast<float>(rect.right - rect.left);
        float height = static_cast<float>(rect.bottom - rect.top);
        float aspect = width / (height > 0 ? height : 1.0f);
        
        float fov = XM_PIDIV4;
        float near_plane = 0.1f;
        float far_plane = 10000.0f;
        XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, near_plane, far_plane); // High far plane for terrain bounds
        XMMATRIX view_proj = XMMatrixMultiply(view, proj);
        
        if (frame_count == 0) {
            std::cout << "[INFO] Camera FOV: " << fov << ", Aspect: " << aspect 
                      << ", Near: " << near_plane << ", Far: " << far_plane << std::endl;
        }

        ConstantBuffer cb = {};
        cb.view_projection = XMMatrixTranspose(view_proj);
        cb.material_color = XMFLOAT4(
            debug_material_color_[0], debug_material_color_[1],
            debug_material_color_[2], debug_material_color_[3]);
        context_->UpdateSubresource(constant_buffer_, 0, nullptr, &cb, 0, 0);

        // Use the debug layout and debug shaders
        context_->IASetInputLayout(debug_input_layout_);
        context_->VSSetShader(debug_vertex_shader_, nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, &constant_buffer_);

        bool force_solid = (getenv("AWL_FORCE_SOLID_MESH") != nullptr && strcmp(getenv("AWL_FORCE_SOLID_MESH"), "1") == 0);
        bool uv_color = (getenv("AWL_DEBUG_UV_COLOR") != nullptr && strcmp(getenv("AWL_DEBUG_UV_COLOR"), "1") == 0);
        bool force_opaque = (getenv("AWL_TEXTURE_FORCE_OPAQUE") != nullptr && strcmp(getenv("AWL_TEXTURE_FORCE_OPAQUE"), "1") == 0);
        bool debug_alpha = (getenv("AWL_DEBUG_TEXTURE_ALPHA") != nullptr && strcmp(getenv("AWL_DEBUG_TEXTURE_ALPHA"), "1") == 0);
        bool debug_transparency = (getenv("AWL_DEBUG_TEXTURE_TRANSPARENCY") != nullptr && strcmp(getenv("AWL_DEBUG_TEXTURE_TRANSPARENCY"), "1") == 0);
        bool draw_sanity = (getenv("AWL_DRAW_SANITY_TRIANGLE") != nullptr && strcmp(getenv("AWL_DRAW_SANITY_TRIANGLE"), "1") == 0);
        bool wireframe = (getenv("AWL_WIREFRAME") != nullptr && strcmp(getenv("AWL_WIREFRAME"), "1") == 0);
        bool point_sampler = (getenv("AWL_POINT_SAMPLER") != nullptr && strcmp(getenv("AWL_POINT_SAMPLER"), "1") == 0);
        
        if (wireframe) {
            context_->RSSetState(rs_wireframe_);
        } else {
            context_->RSSetState(rs_no_cull_); // Disable culling
        }
        
        if (force_solid) {
            if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: solid" << std::endl;
            if (frame_count == 0) std::cout << "[INFO] Using constant solid-color shader for GPL mesh" << std::endl;
            context_->PSSetShader(solid_pixel_shader_, nullptr, 0);
        } else if (uv_color) {
            if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: UV debug" << std::endl;
            context_->PSSetShader(uv_color_pixel_shader_, nullptr, 0);
        } else if (dev_srv_ || uses_ordered_batches) {
            if (force_opaque) {
                if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: texture sample (force opaque)" << std::endl;
                if (frame_count == 0) std::cout << "[INFO] Using texture RGB with forced alpha=1" << std::endl;
                context_->PSSetShader(force_opaque_pixel_shader_, nullptr, 0);
            } else if (debug_alpha) {
                if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: texture sample (debug alpha)" << std::endl;
                context_->PSSetShader(debug_alpha_pixel_shader_, nullptr, 0);
            } else if (debug_transparency) {
                if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: texture sample (debug transparency)" << std::endl;
                context_->PSSetShader(debug_transparency_pixel_shader_, nullptr, 0);
            } else {
                if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: texture sample" << std::endl;
                context_->PSSetShader(debug_pixel_shader_, nullptr, 0);
            }
            if (!uses_ordered_batches) {
                context_->PSSetShaderResources(0, 1, &dev_srv_);
                if (point_sampler) {
                    if (frame_count == 0) std::cout << "[INFO] Using point sampler" << std::endl;
                    context_->PSSetSamplers(0, 1, &point_sampler_);
                } else {
                    context_->PSSetSamplers(0, 1, &dev_sampler_);
                }
            }
        } else {
            if (frame_count == 0) std::cout << "[INFO] Active pixel shader mode: solid (fallback)" << std::endl;
            context_->PSSetShader(solid_pixel_shader_, nullptr, 0);
        }
        
        context_->IASetVertexBuffers(0, 1, &debug_mesh_vb_, &debug_stride, &offset);
        context_->IASetIndexBuffer(debug_mesh_ib_, DXGI_FORMAT_R32_UINT, 0);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        
        if (uses_ordered_batches) {
            for (size_t batch_index = 0;
                 batch_index < debug_draw_batches_.size(); ++batch_index) {
                const DebugDrawBatch& batch = debug_draw_batches_[batch_index];
                ID3D11ShaderResourceView* batch_srv =
                    debug_texture_srvs_[batch.texture_slot];
                ID3D11SamplerState* batch_sampler = point_sampler
                    ? point_sampler_
                    : debug_texture_samplers_[batch.texture_slot];
                constexpr float kByteToFloat = 1.0f / 255.0f;
                cb.material_color = XMFLOAT4(
                    static_cast<float>(batch.material_red) * kByteToFloat,
                    static_cast<float>(batch.material_green) * kByteToFloat,
                    static_cast<float>(batch.material_blue) * kByteToFloat,
                    static_cast<float>(batch.material_alpha) * kByteToFloat);
                context_->UpdateSubresource(
                    constant_buffer_, 0, nullptr, &cb, 0, 0);
                context_->PSSetShaderResources(0, 1, &batch_srv);
                context_->PSSetSamplers(0, 1, &batch_sampler);
                if (log_frames && frame_count < 10) {
                    std::cout << "Calling ordered DrawIndexed batch "
                              << batch_index << ": first_index="
                              << batch.first_index << " index_count="
                              << batch.index_count << " texture_slot="
                              << batch.texture_slot << " material_rgba=("
                              << static_cast<uint32_t>(batch.material_red) << ','
                              << static_cast<uint32_t>(batch.material_green) << ','
                              << static_cast<uint32_t>(batch.material_blue) << ','
                              << static_cast<uint32_t>(batch.material_alpha) << ')'
                              << std::endl;
                }
                context_->DrawIndexed(
                    batch.index_count, batch.first_index, 0);
            }
        } else {
            if (log_frames && frame_count < 10) std::cout << "Calling DrawIndexed for real-UV mesh: index_count=" << debug_mesh_index_count_ << std::endl;
            context_->DrawIndexed(debug_mesh_index_count_, 0, 0);
            if (log_frames && frame_count < 10) std::cout << "DrawIndexed completed for real-UV mesh" << std::endl;
        }
        
        if (draw_sanity) {
            // Draw a known fallback triangle in the SAME branch to test pipeline validity
            context_->IASetInputLayout(input_layout_);
            context_->VSSetShader(vertex_shader_, nullptr, 0);
            context_->PSSetShader(color_pixel_shader_, nullptr, 0);
            cb.view_projection = XMMatrixIdentity();
            context_->UpdateSubresource(constant_buffer_, 0, nullptr, &cb, 0, 0);
            context_->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
            context_->Draw(3, 0);
        }
        
        if (!force_solid && !uv_color && (dev_srv_ || uses_ordered_batches)) {
            ID3D11ShaderResourceView* null_srv = nullptr;
            context_->PSSetShaderResources(0, 1, &null_srv);
        }
        
        // Restore default rasterizer state
        context_->RSSetState(nullptr);
    } else {
        bool mesh_only = (getenv("AWL_MESH_ONLY") != nullptr && strcmp(getenv("AWL_MESH_ONLY"), "1") == 0);
        if (!mesh_only && dev_srv_) {
        context_->IASetInputLayout(input_layout_);
        context_->VSSetShader(vertex_shader_, nullptr, 0);
        ConstantBuffer cb = {};
        cb.view_projection = XMMatrixIdentity();
        cb.material_color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        context_->UpdateSubresource(constant_buffer_, 0, nullptr, &cb, 0, 0);

        // Render Textured Quad
        context_->PSSetShader(pixel_shader_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &dev_srv_);
        context_->PSSetSamplers(0, 1, &dev_sampler_);
        
        context_->IASetVertexBuffers(0, 1, &quad_vb_, &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->Draw(4, 0);
        
        // Unbind SRV to be clean
        ID3D11ShaderResourceView* null_srv = nullptr;
        context_->PSSetShaderResources(0, 1, &null_srv);
    } else if (!mesh_only) {
        ConstantBuffer cb = {};
        cb.view_projection = XMMatrixIdentity();
        cb.material_color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        context_->UpdateSubresource(constant_buffer_, 0, nullptr, &cb, 0, 0);

        // Fallback Triangle
        context_->PSSetShader(color_pixel_shader_, nullptr, 0);
        context_->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->Draw(3, 0);
    }
    }

    const char* capture_path = getenv("AWL_CAPTURE_FRAME");
    if (frame_count == 0 && capture_path && capture_path[0] != '\0' &&
        !capture_back_buffer_bmp(capture_path)) {
        return false;
    }

    if (log_frames && frame_count < 10) std::cout << "Frame " << frame_count << ": after render" << std::endl;
    HRESULT hr = swap_chain_->Present(1, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        if (log_frames && frame_count < 10) {
            std::cout << "Frame " << frame_count
                      << ": present occluded; frame not counted" << std::endl;
        }
        Sleep(16);
        return true;
    }
    if (FAILED(hr)) {
        require_dx_success(hr, "IDXGISwapChain::Present");
        if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && device_) {
            require_dx_success(device_->GetDeviceRemovedReason(),
                               "ID3D11Device::GetDeviceRemovedReason");
        }
        return false;
    }
    if (log_frames && frame_count < 10) std::cout << "Frame " << frame_count << ": after present" << std::endl;
    frame_count++;
    return true;
}

} // namespace awl
