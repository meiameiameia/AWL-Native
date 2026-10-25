// Pixel shader cache keyed by the TEV / pixel-state configuration. Generation and compilation happen on the first use
// of a key; later uses are a hash lookup. Failures are cached too, so an unsupported configuration is reported once
// with its reason and then fails fast every time it comes back (the caller decides how loud to be).
#pragma once

#include <string>
#include <unordered_map>

#include "gfx/gfx_blit.h"
#include "gfx/gfx_device.h"
#include "gfx/gfx_tev.h"

namespace awl::gfx {

class PixelShaderCache {
public:
    // Returns the shader, or nullptr with `err` set (the generator's unsupported reason or the HLSL compiler output).
    ID3D11PixelShader* get(Device& dev, const tev::PixelShaderKey& key, std::string& err);

    struct Stats { uint64_t lookups = 0, hits = 0, compiled = 0, rejected = 0; };
    const Stats& stats() const { return stats_; }
    size_t size() const { return map_.size(); }

private:
    struct Entry { ComPtr<ID3D11PixelShader> ps; std::string error; };
    std::unordered_map<std::string, Entry> map_;
    Stats stats_;
};

}  // namespace awl::gfx
