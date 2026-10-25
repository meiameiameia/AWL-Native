// CPU-side images, BMP files and the diagnostic frame. No Direct3D here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace awl::gfx {

// Tightly packed RGBA8, row 0 = top.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width && height && rgba.size() == size_t(width) * height * 4; }
    void resize(uint32_t w, uint32_t h) { width = w; height = h; rgba.assign(size_t(w) * h * 4, 0); }
    uint8_t* px(uint32_t x, uint32_t y) { return &rgba[(size_t(y) * width + x) * 4]; }
    const uint8_t* px(uint32_t x, uint32_t y) const { return &rgba[(size_t(y) * width + x) * 4]; }
    void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    bool operator==(const Image& o) const { return width == o.width && height == o.height && rgba == o.rgba; }
};

// 24-bit uncompressed bottom-up BMP (alpha is dropped on write and read back as 255). The bytes depend only on the pixels.
bool writeBmp(const std::string& path, const Image& img, std::string& err);
bool readBmp(const std::string& path, Image& out, std::string& err);
std::vector<uint8_t> encodeBmp(const Image& img);

// The development indicator shown until the GX renderer produces the real picture: a fixed colour with the count of
// framebuffer latches in large white digits at the top left. Not a game image; never acceptance evidence.
void drawDiagnosticFrame(Image& out, uint32_t width, uint32_t height, uint64_t latchCount);
void drawDigits(Image& img, uint32_t x, uint32_t y, uint32_t scale, uint64_t value, uint8_t r, uint8_t g, uint8_t b);

}  // namespace awl::gfx
