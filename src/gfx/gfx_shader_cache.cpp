#include "gfx/gfx_shader_cache.h"

namespace awl::gfx {

ID3D11PixelShader* PixelShaderCache::get(Device& dev, const tev::PixelShaderKey& key, std::string& err) {
    ++stats_.lookups;
    const std::string bytes = tev::keyBytes(key);
    auto it = map_.find(bytes);
    if (it != map_.end()) {
        ++stats_.hits;
        if (!it->second.ps) err = it->second.error;
        return it->second.ps.Get();
    }
    Entry e;
    const tev::HlslResult gen = tev::generatePixelShader(key);
    if (!gen.ok) {
        e.error = "unsupported pixel pipeline: " + gen.unsupported;
        ++stats_.rejected;
    } else {
        ComPtr<ID3DBlob> code;
        if (!compileHlsl(gen.hlsl, "main", "ps_5_0", code, e.error)) {
            e.error += "\n--- generated HLSL ---\n" + gen.hlsl;
            ++stats_.rejected;
        } else {
            const HRESULT hr = dev.device()->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, e.ps.GetAddressOf());
            if (FAILED(hr)) { e.error = "CreatePixelShader: " + hresultText(hr); ++stats_.rejected; }
            else ++stats_.compiled;
        }
    }
    auto res = map_.emplace(bytes, std::move(e));
    if (!res.first->second.ps) err = res.first->second.error;
    return res.first->second.ps.Get();
}

}  // namespace awl::gfx
