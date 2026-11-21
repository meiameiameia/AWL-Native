// Pixel shader cache keyed by the TEV / pixel-state configuration. Generation and compilation happen on the first use
// of a key; later uses are a hash lookup. Failures are cached too, so an unsupported configuration is reported once
// with its reason and then fails fast every time it comes back (the caller decides how loud to be).
// The key is the fixed-size PixelShaderKey itself (plain bytes): hashed a word at a time and compared with memcmp, no string is built.
#pragma once

#include <cstdint>
#include <cstring>
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
    // The renderer reused the shader of the previous draw (same pipeline state id): counted as the lookup it replaces.
    void noteReuse() { ++stats_.lookups; ++stats_.hits; }

    struct Stats { uint64_t lookups = 0, hits = 0, compiled = 0, rejected = 0; };
    const Stats& stats() const { return stats_; }
    size_t size() const { return map_.size(); }

private:
    struct KeyHash {
        size_t operator()(const tev::PixelShaderKey& k) const {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
            uint64_t h = 0x9E3779B97F4A7C15ull;
            size_t i = 0;
            for (; i + 8 <= sizeof k; i += 8) {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = (h ^ w) * 0xFF51AFD7ED558CCDull;
                h ^= h >> 32;
            }
            for (; i < sizeof k; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
            return static_cast<size_t>(h);
        }
    };
    struct KeyEq {
        bool operator()(const tev::PixelShaderKey& a, const tev::PixelShaderKey& b) const { return std::memcmp(&a, &b, sizeof a) == 0; }
    };
    struct Entry { ComPtr<ID3D11PixelShader> ps; std::string error; };
    std::unordered_map<tev::PixelShaderKey, Entry, KeyHash, KeyEq> map_;
    Stats stats_;
};

}  // namespace awl::gfx
