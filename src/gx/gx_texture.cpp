// GX core: texture decoding to RGBA8, the TLUT, and the decode cache. Tile layouts and formats: YAGCD chapter 15 (texture
// formats) and libogc gx.c (__GetImageTileCount, GX_InitTexObj tile types, GX_InitTlutRegion). The CMPR interpolation
// (5/8, 3/8) follows the repository's earlier TPL decoder (src/tpl.cpp), which was checked against the game's textures.
#include <algorithm>
#include <cstring>

#include "gx/gx_core.h"

namespace awl::gx {

uint64_t gxHash64(const uint8_t* p, size_t n, uint64_t seed) {
    constexpr uint64_t k1 = 0x9E3779B97F4A7C15ull, k2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t h = seed ^ (n * k1);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h ^= w * k2;
        h = ((h << 31) | (h >> 33)) * k1;
    }
    uint64_t tail = 0;
    std::memcpy(&tail, p + i, n - i);
    h ^= tail * k2;
    h ^= h >> 29;
    h *= k1;
    h ^= h >> 32;
    return h;
}

namespace {

constexpr uint64_t kCacheBudgetBytes = 192ull << 20;
constexpr uint64_t kSnapshotBudgetBytes = 192ull << 20;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
uint32_t e4(uint32_t v) { return v * 17; }
uint32_t e5(uint32_t v) { return (v << 3) | (v >> 2); }
uint32_t e6(uint32_t v) { return (v << 2) | (v >> 4); }
uint32_t e3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

uint32_t rgb565(uint32_t v) { return pack(e5(v >> 11), e6((v >> 5) & 63), e5(v & 31), 255); }
uint32_t rgb5a3(uint32_t v) {
    if (v & 0x8000) return pack(e5((v >> 10) & 31), e5((v >> 5) & 31), e5(v & 31), 255);
    return pack(e4((v >> 8) & 15), e4((v >> 4) & 15), e4(v & 15), e3((v >> 12) & 7));
}
uint32_t ia8(uint32_t v) { const uint32_t a = v >> 8, i = v & 0xFF; return pack(i, i, i, a); }   // alpha in the first byte

struct TileGeom { uint32_t w, h, bytes; };
bool tileGeom(uint32_t fmt, TileGeom& g) {
    switch (fmt) {
        case 0: case 8: g = {8, 8, 32}; return true;                    // I4, C4
        case 1: case 2: case 9: g = {8, 4, 32}; return true;            // I8, IA4, C8
        case 3: case 4: case 5: case 10: g = {4, 4, 32}; return true;   // IA8, RGB565, RGB5A3, C14X2
        case 6: g = {4, 4, 64}; return true;                            // RGBA8: AR then GB cache line
        case 14: g = {8, 8, 32}; return true;                           // CMPR
        default: return false;
    }
}
uint32_t levelBytes(uint32_t fmt, uint32_t w, uint32_t h) {
    TileGeom g{};
    tileGeom(fmt, g);
    return ((w + g.w - 1) / g.w) * ((h + g.h - 1) / g.h) * g.bytes;
}

uint32_t lerp58(uint32_t a, uint32_t b) { return (a * 5 + b * 3) >> 3; }

void decodeCmprSub(uint32_t* out, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, const uint8_t* b) {
    const uint32_t c0 = be16(b), c1 = be16(b + 2);
    const uint32_t k0 = rgb565(c0), k1 = rgb565(c1);
    auto ch = [](uint32_t c, int s) { return (c >> s) & 0xFF; };
    uint32_t col[4] = {k0, k1, 0, 0};
    if (c0 > c1) {
        col[2] = pack(lerp58(ch(k0, 0), ch(k1, 0)), lerp58(ch(k0, 8), ch(k1, 8)), lerp58(ch(k0, 16), ch(k1, 16)), 255);
        col[3] = pack(lerp58(ch(k1, 0), ch(k0, 0)), lerp58(ch(k1, 8), ch(k0, 8)), lerp58(ch(k1, 16), ch(k0, 16)), 255);
    } else {
        col[2] = pack((ch(k0, 0) + ch(k1, 0)) / 2, (ch(k0, 8) + ch(k1, 8)) / 2, (ch(k0, 16) + ch(k1, 16)) / 2, 255);
        col[3] = col[2] & 0x00FFFFFFu;
    }
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            if (bx + x >= w || by + y >= h) continue;
            out[(by + y) * w + bx + x] = col[(b[4 + y] >> (6 - 2 * x)) & 3];
        }
}

}  // namespace

// Decodes one mip level into out[w*h]; palette is null for non-indexed formats.
static void decodeLevel(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, const uint16_t* pal, uint32_t palFmt, uint32_t* out) {
    TileGeom g{};
    tileGeom(fmt, g);
    auto palColor = [&](uint32_t idx) {
        const uint32_t e = pal[idx];
        return palFmt == 0 ? ia8(e) : palFmt == 1 ? rgb565(e) : rgb5a3(e);
    };
    for (uint32_t ty = 0; ty < h; ty += g.h) {
        for (uint32_t tx = 0; tx < w; tx += g.w) {
            if (fmt == 14) {
                for (uint32_t s = 0; s < 4; ++s) decodeCmprSub(out, w, h, tx + (s & 1) * 4, ty + (s >> 1) * 4, src + s * 8);
                src += g.bytes;
                continue;
            }
            for (uint32_t y = 0; y < g.h; ++y) {
                for (uint32_t x = 0; x < g.w; ++x) {
                    uint32_t px = 0;
                    switch (fmt) {
                        case 0: { const uint32_t n = (src[y * 4 + x / 2] >> ((x & 1) ? 0 : 4)) & 15, i = e4(n); px = pack(i, i, i, i); break; }
                        case 1: { const uint32_t i = src[y * 8 + x]; px = pack(i, i, i, i); break; }
                        case 2: { const uint32_t v = src[y * 8 + x], i = e4(v & 15); px = pack(i, i, i, e4(v >> 4)); break; }
                        case 3: px = ia8(be16(src + y * 8 + x * 2)); break;
                        case 4: px = rgb565(be16(src + y * 8 + x * 2)); break;
                        case 5: px = rgb5a3(be16(src + y * 8 + x * 2)); break;
                        case 6: { const uint32_t t = (y * 4 + x) * 2; px = pack(src[t + 1], src[32 + t], src[32 + t + 1], src[t]); break; }
                        case 8: px = palColor((src[y * 4 + x / 2] >> ((x & 1) ? 0 : 4)) & 15); break;
                        case 9: px = palColor(src[y * 8 + x]); break;
                        case 10: px = palColor(be16(src + y * 8 + x * 2) & 0x3FFF); break;
                        default: break;
                    }
                    if (tx + x < w && ty + y < h) out[(ty + y) * w + tx + x] = px;
                }
            }
            src += g.bytes;
        }
    }
}

void Core::tlutLoad() {
    const uint32_t phys = (bp_[0x64] & 0x1FFFFF) << 5, offset = bp_[0x65] & 0x3FF, entries = ((bp_[0x65] >> 10) & 0x3FF) * 16;
    if (offset * 256 + entries > tlutRam_.size()) fail("TLUT load beyond the TMEM palette area", 0x65);
    const uint8_t* src = mem_.read(phys, entries * 2);
    if (!src) fail("TLUT load reads outside memory", 0x64);
    for (uint32_t i = 0; i < entries; ++i) tlutRam_[offset * 256 + i] = be16(src + 2 * i);
    ++tlutEpoch_;
}

std::shared_ptr<const Texture> Core::loadTexture(const TexRequest& req) {
    TileGeom g{};
    if (!tileGeom(req.format, g)) fail("texture format not supported: " + std::to_string(req.format), 0x88);
    if (req.width == 0 || req.height == 0) fail("texture with a zero dimension", 0x88);
    const bool indexed = req.format == 8 || req.format == 9 || req.format == 10;
    struct Lvl { uint32_t w, h, bytes, off; };
    Lvl lv[11];
    uint32_t total = 0, n = 0;
    for (uint32_t l = 0; l < req.levels && l < 11; ++l, ++n) {
        const uint32_t w = std::max(1u, req.width >> l), h = std::max(1u, req.height >> l);
        lv[l] = {w, h, levelBytes(req.format, w, h), total};
        total += lv[l].bytes;
    }
    const uint8_t* raw = mem_.read(req.addr, total);
    if (!raw) fail("texture data outside memory at " + std::to_string(req.addr), 0x94);
    const uint16_t* pal = nullptr;
    uint32_t palCount = 0;
    if (indexed) {
        palCount = req.format == 8 ? 16 : req.format == 9 ? 256 : 16384;
        if (req.tlutOffset * 256 + palCount > tlutRam_.size()) fail("palette beyond the TMEM palette area", 0x98);
        pal = &tlutRam_[req.tlutOffset * 256];
    }
    // Exact change detection without rehashing: the raw bytes (and palette) of the last decode of this texture binding are
    // kept, and a byte-for-byte comparison (an order of magnitude cheaper than the hash) shows whether they are unchanged;
    // only a binding seen for the first time, or whose bytes changed, is hashed. The key stays a content hash.
    const uint32_t palBytes = indexed ? palCount * 2 : 0;
    const uint32_t tuple[8] = {req.addr, req.width, req.height, req.format, n, req.tlutOffset, req.tlutFormat, total};
    const uint64_t tupleKey = gxHash64(reinterpret_cast<const uint8_t*>(tuple), sizeof tuple, 0x534E4150ull);
    uint64_t key = 0;
    Snapshot& snap = snapshots_[tupleKey];
    const bool sameBinding = snap.valid && std::memcmp(snap.tuple, tuple, sizeof tuple) == 0 && snap.raw.size() == total && snap.pal.size() == palBytes;
    if (trustEpoch_ && sameBinding && snap.epoch == texGeneration_ && snap.tlutEpoch == tlutEpoch_) {
        // Verified since the last texture-cache invalidate: the SDK invalidates after changing texture memory, so the bytes
        // are not read again (a change without an invalidate is not observed, as on hardware).
        key = snap.key;
        ++stats_.textureTrusted;
    } else if (sameBinding && std::memcmp(snap.raw.data(), raw, total) == 0 && (palBytes == 0 || std::memcmp(snap.pal.data(), pal, palBytes) == 0)) {
        key = snap.key;
        snap.epoch = texGeneration_;
        snap.tlutEpoch = tlutEpoch_;
        stats_.textureBytesCompared += total + palBytes;
    } else {
        stats_.textureBytesHashed += total;
        key = gxHash64(raw, total, 0x41574C47ull);
        key = gxHash64(reinterpret_cast<const uint8_t*>(&req.format), 4, key);
        const uint32_t dims[3] = {req.width, req.height, n};
        key = gxHash64(reinterpret_cast<const uint8_t*>(dims), sizeof dims, key);
        if (indexed) {
            key = gxHash64(reinterpret_cast<const uint8_t*>(pal), palCount * 2, key);
            key = gxHash64(reinterpret_cast<const uint8_t*>(&req.tlutFormat), 4, key);
        }
        snapBytes_ -= snap.raw.size() + snap.pal.size();
        snap.valid = true;
        std::memcpy(snap.tuple, tuple, sizeof tuple);
        snap.raw.assign(raw, raw + total);
        snap.pal.resize(palBytes);
        if (palBytes) std::memcpy(snap.pal.data(), pal, palBytes);
        snap.key = key;
        snap.epoch = texGeneration_;
        snap.tlutEpoch = tlutEpoch_;
        snapBytes_ += snap.raw.size() + snap.pal.size();
        if (snapBytes_ > kSnapshotBudgetBytes) {          // simple bound: forget every other snapshot
            for (auto it2 = snapshots_.begin(); it2 != snapshots_.end();) {
                if (&it2->second == &snap) { ++it2; continue; }
                snapBytes_ -= it2->second.raw.size() + it2->second.pal.size();
                it2 = snapshots_.erase(it2);
            }
        }
    }    auto it = texCache_.find(key);
    if (it != texCache_.end()) {
        ++stats_.textureCacheHits;
        texLru_.remove(key);
        texLru_.push_front(key);
        return it->second.tex;
    }
    auto tex = std::make_shared<Texture>();
    tex->key = key;
    tex->width = static_cast<uint16_t>(req.width);
    tex->height = static_cast<uint16_t>(req.height);
    tex->sourceFormat = static_cast<uint8_t>(req.format);
    tex->levelCount = static_cast<uint8_t>(n);
    uint32_t texels = 0;
    for (uint32_t l = 0; l < n; ++l) {
        tex->levels[l] = {static_cast<uint16_t>(lv[l].w), static_cast<uint16_t>(lv[l].h), texels};
        texels += lv[l].w * lv[l].h;
    }
    tex->rgba.assign(texels, 0);
    for (uint32_t l = 0; l < n; ++l) decodeLevel(req.format, raw + lv[l].off, lv[l].w, lv[l].h, pal, req.tlutFormat, tex->rgba.data() + tex->levels[l].offsetTexels);
    ++stats_.textureDecodes;
    const uint64_t bytes = uint64_t(texels) * 4;
    texCache_[key] = {tex, bytes};
    texLru_.push_front(key);
    texCacheBytes_ += bytes;
    while (texCacheBytes_ > kCacheBudgetBytes && texLru_.size() > 1) {
        const uint64_t victim = texLru_.back();
        texLru_.pop_back();
        auto v = texCache_.find(victim);
        if (v != texCache_.end()) { texCacheBytes_ -= v->second.bytes; texCache_.erase(v); }
    }
    return tex;
}

}  // namespace awl::gx
