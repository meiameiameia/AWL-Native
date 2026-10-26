// A software reference renderer for the GX IR (gx_ir.h): a Backend that draws every command into a CPU copy of the EFB and keeps
// the images of the XFB copies. It is slow and approximate (no fog, no indirect texturing, mip level 0 only, no logic ops,
// nearest/bilinear sampling, no dither) and exists to answer "what does this IR stream look like" without a GPU, to cross-check
// the DX11 renderer, and to test the IR end to end. Not part of the game's runtime.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gx/gx_ir.h"

namespace awl::gx {

class SoftBackend : public Backend {
public:
    static constexpr uint32_t kWidth = 640, kHeight = 528;
    struct Image { uint32_t w = 0, h = 0; std::vector<uint32_t> px; };   // R | G << 8 | B << 16 | A << 24

    SoftBackend();
    void draw(const DrawCommand& d) override;
    void efbCopy(const EfbCopyCommand& c) override;

    const std::vector<Image>& xfbImages() const { return xfb_; }     // one per copy to the XFB, in order
    uint64_t trianglesDrawn() const { return tris_; }
    uint64_t pixelsWritten() const { return pixels_; }
    std::map<std::string, uint64_t>& notes() { return notes_; }       // approximations hit while drawing
    void keepXfb(size_t n) { keep_ = n; }                              // keep only the last n XFB images (default 4)

private:
    struct Vtx { float p[4]; float c[2][4]; float t[8][3]; };
    void rasterize(const PipelineState& s, const Vtx* v);
    void shade(const PipelineState& s, const float bary[3], const Vtx* v, float depth, int x, int y);
    void sampleMap(const PipelineState& s, uint32_t map, float u, float v, float out[4]) const;
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
    std::vector<Image> xfb_;
    std::map<uint32_t, Image> copies_;
    uint64_t tris_ = 0, pixels_ = 0;
    size_t keep_ = 4;
    std::map<std::string, uint64_t> notes_;
};

bool writePngRgba(const std::string& path, uint32_t w, uint32_t h, const uint32_t* rgba);   // gx_record.cpp

}  // namespace awl::gx
