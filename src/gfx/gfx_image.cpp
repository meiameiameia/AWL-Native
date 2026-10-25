#include "gfx/gfx_image.h"

#include <cstdio>

namespace awl::gfx {

void Image::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) { rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a; }
}

namespace {
void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }
}  // namespace

std::vector<uint8_t> encodeBmp(const Image& img) {
    const uint32_t rowBytes = (img.width * 3 + 3) & ~3u;
    const uint32_t dataBytes = rowBytes * img.height;
    std::vector<uint8_t> out;
    out.reserve(54 + dataBytes);
    out.push_back('B'); out.push_back('M');
    put32(out, 54 + dataBytes); put32(out, 0); put32(out, 54);
    put32(out, 40); put32(out, img.width); put32(out, img.height);
    put16(out, 1); put16(out, 24); put32(out, 0); put32(out, dataBytes);
    put32(out, 2835); put32(out, 2835); put32(out, 0); put32(out, 0);
    for (uint32_t y = img.height; y-- > 0;) {
        const uint8_t* row = img.px(0, y);
        for (uint32_t x = 0; x < img.width; ++x) { out.push_back(row[x * 4 + 2]); out.push_back(row[x * 4 + 1]); out.push_back(row[x * 4]); }
        for (uint32_t pad = img.width * 3; pad < rowBytes; ++pad) out.push_back(0);
    }
    return out;
}

bool writeBmp(const std::string& path, const Image& img, std::string& err) {
    if (!img.valid()) { err = "writeBmp: invalid image"; return false; }
    const std::vector<uint8_t> bytes = encodeBmp(img);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path + " (does the folder exist?)"; return false; }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) { err = "short write to " + path; return false; }
    return true;
}

bool readBmp(const std::string& path, Image& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot read " + path; return false; }
    std::vector<uint8_t> b;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') { err = path + ": not a BMP"; return false; }
    const uint32_t offset = get32(&b[10]), w = get32(&b[18]), h = get32(&b[22]);
    if (get16(&b[28]) != 24 || get32(&b[30]) != 0 || w == 0 || h == 0 || w > 16384 || h > 16384) {
        err = path + ": only uncompressed 24-bit BMP is supported";
        return false;
    }
    const uint32_t rowBytes = (w * 3 + 3) & ~3u;
    if (b.size() < size_t(offset) + size_t(rowBytes) * h) { err = path + ": truncated"; return false; }
    out.resize(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = &b[offset + size_t(h - 1 - y) * rowBytes];
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* d = out.px(x, y);
            d[0] = src[x * 3 + 2]; d[1] = src[x * 3 + 1]; d[2] = src[x * 3]; d[3] = 255;
        }
    }
    return true;
}

namespace {
// 5x7 digit glyphs, one row per byte (bit 4 = leftmost column).
const uint8_t kDigits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
}  // namespace

void drawDigits(Image& img, uint32_t x0, uint32_t y0, uint32_t scale, uint64_t value, uint8_t r, uint8_t g, uint8_t b) {
    char text[24];
    const int len = std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(value));
    for (int i = 0; i < len; ++i) {
        const uint8_t* glyph = kDigits[text[i] - '0'];
        for (uint32_t row = 0; row < 7; ++row)
            for (uint32_t col = 0; col < 5; ++col) {
                if (!(glyph[row] & (0x10 >> col))) continue;
                for (uint32_t dy = 0; dy < scale; ++dy)
                    for (uint32_t dx = 0; dx < scale; ++dx) {
                        const uint32_t px = x0 + uint32_t(i) * 6 * scale + col * scale + dx, py = y0 + row * scale + dy;
                        if (px >= img.width || py >= img.height) continue;
                        uint8_t* d = img.px(px, py);
                        d[0] = r; d[1] = g; d[2] = b; d[3] = 255;
                    }
            }
    }
}

void drawDiagnosticFrame(Image& out, uint32_t width, uint32_t height, uint64_t latchCount) {
    out.resize(width, height);
    out.fill(0xA0, 0x10, 0xA0);   // magenta: cannot be mistaken for a game picture
    drawDigits(out, 16, 16, 8, latchCount, 255, 255, 255);
}

}  // namespace awl::gfx
