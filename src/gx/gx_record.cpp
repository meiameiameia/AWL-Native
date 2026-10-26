#include "gx/gx_record.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <filesystem>

namespace awl::gx {

namespace {

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const char* kCmp[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
const char* kBlendMode[] = {"none", "blend", "subtract", "logic"};
const char* kFactor[] = {"0", "1", "othercolor", "1-othercolor", "srcalpha", "1-srcalpha", "dstalpha", "1-dstalpha"};
const char* kColIn[] = {"CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2", "TEXC", "TEXA", "RASC", "RASA", "ONE", "HALF", "KONST", "ZERO"};
const char* kAlphaIn[] = {"APREV", "A0", "A1", "A2", "TEXA", "RASA", "KONST", "ZERO"};
const char* kDest[] = {"prev", "reg0", "reg1", "reg2"};
const char* kScale[] = {"x1", "x2", "x4", "/2"};
const char* kBias[] = {"0", "+0.5", "-0.5"};
const char* kWrap[] = {"clamp", "repeat", "mirror"};
const char* kFilter[] = {"near", "linear", "near_mip_near", "lin_mip_near", "near_mip_lin", "lin_mip_lin"};
const char* kFmt[] = {"RGB8_Z24", "RGBA6_Z24", "RGB565_Z16", "Z24"};

std::string combine(const TevCombine& c, bool alpha) {
    const char* const* names = alpha ? kAlphaIn : kColIn;
    std::string s = fmt("(%s,%s,%s,%s) ", names[c.in[0] & (alpha ? 7 : 15)], names[c.in[1] & (alpha ? 7 : 15)], names[c.in[2] & (alpha ? 7 : 15)], names[c.in[3] & (alpha ? 7 : 15)]);
    if (static_cast<int>(c.op) >= 8) s += fmt("compare%d", static_cast<int>(c.op));
    else s += fmt("%s bias%s scale%s", c.op == TevOp::Add ? "add" : "sub", kBias[static_cast<int>(c.bias) % 3], kScale[static_cast<int>(c.scale) & 3]);
    s += fmt(" %s -> %s", c.clamp ? "clamp" : "noclamp", kDest[static_cast<int>(c.dest) & 3]);
    return s;
}

// Minimal PNG writer (stored deflate blocks) for texture dumps; the IR texels are R,G,B,A bytes, rows top to bottom.
uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
void put32be(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i))); }
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32be(out, crc32(body.data(), body.size()));
}
bool writePng(const std::string& path, uint32_t w, uint32_t h, const uint32_t* rgba) {
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t p = rgba[y * w + x];
            raw.push_back(uint8_t(p));
            raw.push_back(uint8_t(p >> 8));
            raw.push_back(uint8_t(p >> 16));
            raw.push_back(uint8_t(p >> 24));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    put32be(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

}  // namespace

bool writePngRgba(const std::string& path, uint32_t w, uint32_t h, const uint32_t* rgba) { return writePng(path, w, h, rgba); }

std::string describePipeline(const PipelineState& s) {
    std::string o;
    o += fmt("  efb=%s colors=%u texcoords=%u cull=%u\n", kFmt[static_cast<int>(s.efbFormat) & 3], s.numColorChannels, s.numTexCoords, s.cullMode);
    o += fmt("  viewport x=%g y=%g w=%g h=%g z=[%g,%g]\n", s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height, s.viewport.nearZ, s.viewport.farZ);
    o += fmt("  scissor (%d,%d)-(%d,%d)\n", s.scissor.x0, s.scissor.y0, s.scissor.x1, s.scissor.y1);
    o += fmt("  depth test=%d func=%s write=%d early=%d\n", s.depth.test, kCmp[static_cast<int>(s.depth.func) & 7], s.depth.write, s.depth.compareBeforeTexture);
    o += fmt("  blend mode=%s src=%s dst=%s logic=%d color=%d alpha=%d dither=%d dstalpha=%d(%u)\n", kBlendMode[static_cast<int>(s.blend.mode) & 3], kFactor[static_cast<int>(s.blend.src) & 7],
             kFactor[static_cast<int>(s.blend.dst) & 7], static_cast<int>(s.blend.logicOp), s.blend.colorUpdate, s.blend.alphaUpdate, s.blend.dither, s.blend.dstAlphaEnable, s.blend.dstAlpha);
    o += fmt("  alpha (a %s %u) logic%d (a %s %u)\n", kCmp[static_cast<int>(s.alphaCompare.comp0) & 7], s.alphaCompare.ref0, static_cast<int>(s.alphaCompare.logic),
             kCmp[static_cast<int>(s.alphaCompare.comp1) & 7], s.alphaCompare.ref1);
    o += fmt("  fog type=%d ortho=%d a=%g b=%g c=%g color=%02X%02X%02X\n", static_cast<int>(s.fog.type), s.fog.orthographic, s.fog.a, s.fog.b, s.fog.c, s.fog.color[0], s.fog.color[1], s.fog.color[2]);
    o += fmt("  tev stages=%u indirect=%u\n", s.tev.numStages, s.tev.numIndirect);
    for (uint32_t i = 0; i < s.tev.numStages; ++i) {
        const TevStage& t = s.tev.stages[i];
        o += fmt("    [%u] map=%d coord=%d ras=%d kc=%u ka=%u swap=%u,%u color %s | alpha %s", i, t.texMap == kNoMap ? -1 : t.texMap, t.texCoord == kNoMap ? -1 : t.texCoord, static_cast<int>(t.ras),
                 t.konstColor, t.konstAlpha, t.rasSwap, t.texSwap, combine(t.color, false).c_str(), combine(t.alpha, true).c_str());
        if (t.indirect.indStage != kNoMap) o += fmt(" | ind stage=%u fmt=%u bias=%u mtx=%u wrap=%u,%u prev=%d", t.indirect.indStage, t.indirect.format, t.indirect.biasMask, t.indirect.matrix, t.indirect.wrapS, t.indirect.wrapT, t.indirect.addPrevious);
        o += "\n";
    }
    for (int c = 0; c < 4; ++c)
        o += fmt("    reg%d=%d,%d,%d,%d konst%d=%u,%u,%u,%u\n", c, s.tev.reg[c][0], s.tev.reg[c][1], s.tev.reg[c][2], s.tev.reg[c][3], c, s.tev.konst[c][0], s.tev.konst[c][1], s.tev.konst[c][2], s.tev.konst[c][3]);
    for (int m = 0; m < 8; ++m) {
        const TexBinding& b = s.maps[m];
        if (!b.valid) continue;
        o += fmt("  map%d %ux%u wrap=%s/%s min=%s mag=%s lodbias=%g lod=[%g,%g]", m, b.width, b.height, kWrap[static_cast<int>(b.sampler.wrapS) % 3], kWrap[static_cast<int>(b.sampler.wrapT) % 3],
                 kFilter[static_cast<int>(b.sampler.minFilter) % 6], b.sampler.magLinear ? "linear" : "near", b.sampler.lodBias, b.sampler.minLod, b.sampler.maxLod);
        if (b.image) o += fmt(" texture key=%016llX fmt=%u levels=%u\n", static_cast<unsigned long long>(b.image->key), b.image->sourceFormat, b.image->levelCount);
        else o += fmt(" efbcopy=%u\n", b.efbCopyId);
    }
    for (int i = 0; i < s.numTexCoords; ++i) o += fmt("  texscale%d s=%u t=%u\n", i, s.texScale[i].sSize, s.texScale[i].tSize);
    return o;
}

std::string describeDraw(const DrawCommand& d, size_t maxVertices) {
    static const char* kPrim[] = {"triangles", "lines", "points"};
    std::string o = fmt("draw seq=%u state=%u %s vertices=%u indices=%u\n", d.seq, d.stateId, kPrim[static_cast<int>(d.primitive) % 3], d.vertexCount, d.indexCount);
    const size_t n = std::min<size_t>(d.vertexCount, maxVertices);
    for (size_t i = 0; i < n; ++i) {
        const Vertex& v = d.vertices[i];
        o += fmt("  v%zu pos=%g,%g,%g,%g c0=%02X%02X%02X%02X c1=%02X%02X%02X%02X", i, v.pos[0], v.pos[1], v.pos[2], v.pos[3], v.color[0][0], v.color[0][1], v.color[0][2], v.color[0][3], v.color[1][0],
                 v.color[1][1], v.color[1][2], v.color[1][3]);
        for (int t = 0; t < d.state->numTexCoords; ++t) o += fmt(" t%d=%g,%g,%g", t, v.tex[t][0], v.tex[t][1], v.tex[t][2]);
        o += "\n";
    }
    if (n < d.vertexCount) o += fmt("  ... %zu more vertices\n", d.vertexCount - n);
    o += "  idx";
    for (uint32_t i = 0; i < d.indexCount && i < 3 * maxVertices; ++i) o += fmt(" %u", d.indices[i]);
    o += "\n";
    return o;
}

std::string describeCopy(const EfbCopyCommand& c) {
    std::string o = fmt("copy id=%u %s src=(%u,%u %ux%u) dest=0x%08X %ux%u efb=%s", c.id, c.target == CopyTarget::Xfb ? "xfb" : "texture", c.srcX, c.srcY, c.srcWidth, c.srcHeight, c.destAddr,
                        c.destWidth, c.destHeight, kFmt[static_cast<int>(c.efbFormat) & 3]);
    if (c.target == CopyTarget::Xfb) {
        o += fmt(" yscale=%g gamma=%u vfilter=%u,%u,%u,%u,%u,%u,%u sample=%06X field=%d", c.yScale, c.gamma, c.vfilter[0], c.vfilter[1], c.vfilter[2], c.vfilter[3], c.vfilter[4], c.vfilter[5], c.vfilter[6],
                  c.sampleBits, c.frameToField);
    } else {
        o += fmt(" texfmt=0x%02X half=%d mip=%d intensity=%d clamp=%d%d", c.texFormat, c.halfScale, c.mipmap, c.intensity, c.clampTop, c.clampBottom);
    }
    o += fmt(" clear color=%d alpha=%d z=%d rgba=%02X%02X%02X%02X z24=%06X\n", c.clear.color, c.clear.alpha, c.clear.z, c.clear.rgba[0], c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3], c.clear.z24);
    return o;
}

RecordingBackend::RecordingBackend(const std::string& dir, uint32_t first, uint32_t count) : dir_(dir), first_(first), count_(count) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}
RecordingBackend::~RecordingBackend() { close(); }

void RecordingBackend::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

void RecordingBackend::frameBoundary(uint32_t retrace) {
    close();
    lastState_ = 0;
    if (retrace < first_ || retrace - first_ >= count_) return;
    const std::string path = dir_ + "/" + fmt("frame_%05u.txt", retrace);
    f_ = std::fopen(path.c_str(), "w");
    if (f_) {
        std::fprintf(f_, "frame %u (IR version %u)\n", retrace, kIrVersion);
        ++frames_;
    }
}

void RecordingBackend::draw(const DrawCommand& d) {
    if (!f_) return;
    if (d.stateId != lastState_) {
        std::fprintf(f_, "state %u\n%s", d.stateId, describePipeline(*d.state).c_str());
        lastState_ = d.stateId;
    }
    for (const TexBinding& m : d.state->maps) {
        if (!m.valid || !m.image || !dumpedTextures_.insert(m.image->key).second) continue;
        char name[64];
        std::snprintf(name, sizeof name, "/tex_%016llX.png", static_cast<unsigned long long>(m.image->key));
        writePng(dir_ + name, m.image->width, m.image->height, m.image->rgba.data());
    }
    std::fputs(describeDraw(d, 64).c_str(), f_);
}
void RecordingBackend::efbCopy(const EfbCopyCommand& c) {
    if (f_) std::fputs(describeCopy(c).c_str(), f_);
}
void RecordingBackend::sync(SyncKind kind, uint16_t token) {
    if (f_) std::fprintf(f_, "sync %s %u\n", kind == SyncKind::DrawDone ? "done" : kind == SyncKind::Token ? "token" : "token-int", token);
}

FrameLogBackend::FrameLogBackend(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    f_ = std::fopen(path.c_str(), "w");
    if (f_) std::fputs("retrace draws vertices inView% meanLitRGB(color0 draws) copies xfbCopies\n", f_);
}
FrameLogBackend::~FrameLogBackend() {
    flush();
    if (f_) std::fclose(f_);
}
void FrameLogBackend::draw(const DrawCommand& d) {
    ++draws_;
    vertices_ += d.vertexCount;
    for (uint32_t i = 0; i < d.vertexCount; ++i) {
        const float* c = d.vertices[i].pos;
        if (c[3] > 0 && c[2] <= 0 && c[2] >= -c[3] && c[0] >= -c[3] && c[0] <= c[3] && c[1] >= -c[3] && c[1] <= c[3]) ++inView_;
    }
    if (d.state->numColorChannels && d.state->tev.stages[0].ras == RasColor::Color0A0) {
        for (uint32_t i = 0; i < d.vertexCount; ++i)
            for (int k = 0; k < 3; ++k) litSum_[k] += d.vertices[i].color[0][k];
        litVertices_ += d.vertexCount;
    }
}
void FrameLogBackend::efbCopy(const EfbCopyCommand& c) {
    ++copies_;
    if (c.target == CopyTarget::Xfb) ++xfbCopies_;
}
void FrameLogBackend::flush() {
    if (f_ && open_ && (draws_ || copies_)) {
        const double n = litVertices_ ? double(litVertices_) : 1.0;
        std::fprintf(f_, "%u %llu %llu %.1f %.0f,%.0f,%.0f %llu %llu\n", retrace_, static_cast<unsigned long long>(draws_), static_cast<unsigned long long>(vertices_),
                     vertices_ ? 100.0 * double(inView_) / double(vertices_) : 0.0, litSum_[0] / n, litSum_[1] / n, litSum_[2] / n, static_cast<unsigned long long>(copies_),
                     static_cast<unsigned long long>(xfbCopies_));
        std::fflush(f_);
    }
    draws_ = vertices_ = inView_ = litVertices_ = copies_ = xfbCopies_ = 0;
    litSum_[0] = litSum_[1] = litSum_[2] = 0;
}
void FrameLogBackend::frameBoundary(uint32_t retrace) {
    flush();
    retrace_ = retrace;
    open_ = true;
}

StateLogBackend::StateLogBackend(const std::string& path, uint32_t first, uint32_t count) : first_(first), count_(count) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    f_ = std::fopen(path.c_str(), "w");
}
StateLogBackend::~StateLogBackend() {
    flushRun();
    if (f_) std::fclose(f_);
}
void StateLogBackend::flushRun() {
    if (f_ && runDraws_) {
        std::fprintf(f_, "  x%llu draws, %llu vertices (%llu inside the clip volume): %s\n", static_cast<unsigned long long>(runDraws_), static_cast<unsigned long long>(runVertices_),
                     static_cast<unsigned long long>(runIn_), runKey_.c_str());
    }
    runDraws_ = runVertices_ = runIn_ = 0;
    runKey_.clear();
}
void StateLogBackend::frameBoundary(uint32_t retrace) {
    flushRun();
    retrace_ = retrace;
    active_ = retrace >= first_ && retrace - first_ < count_;
    if (active_ && f_) std::fprintf(f_, "== retrace %u\n", retrace);
}
void StateLogBackend::draw(const DrawCommand& d) {
    if (!active_ || !f_) return;
    const PipelineState& s = *d.state;
    std::string key = fmt("viewport %g,%g %gx%g z[%g,%g] scissor (%d,%d)-(%d,%d) texcoords %u cull %u depth %d/%s/%d blend %s", s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height, s.viewport.nearZ,
                          s.viewport.farZ, s.scissor.x0, s.scissor.y0, s.scissor.x1, s.scissor.y1, s.numTexCoords, s.cullMode, s.depth.test, kCmp[static_cast<int>(s.depth.func) & 7], s.depth.write,
                          kBlendMode[static_cast<int>(s.blend.mode) & 3]);
    for (int m = 0; m < 8; ++m)
        if (s.maps[m].valid) key += fmt(" map%d %ux%u %s", m, s.maps[m].width, s.maps[m].height, s.maps[m].image ? "ram" : fmt("efbcopy%u", s.maps[m].efbCopyId).c_str());
    key += fmt(" tev%u texscale0 %ux%u", s.tev.numStages, s.texScale[0].sSize, s.texScale[0].tSize);
    if (key != runKey_) {
        flushRun();
        runKey_ = key;
    }
    ++runDraws_;
    runVertices_ += d.vertexCount;
    for (uint32_t i = 0; i < d.vertexCount; ++i) {
        const float* c = d.vertices[i].pos;
        if (c[3] > 0 && c[2] <= 0 && c[2] >= -c[3] && c[0] >= -c[3] && c[0] <= c[3] && c[1] >= -c[3] && c[1] <= c[3]) ++runIn_;
    }
}
void StateLogBackend::efbCopy(const EfbCopyCommand& c) {
    if (!active_ || !f_) return;
    flushRun();
    std::fputs(describeCopy(c).c_str(), f_);
}

void BackendHub::remove(Backend* b) { list_.erase(std::remove(list_.begin(), list_.end(), b), list_.end()); }

BackendHub& backendHub() {
    static BackendHub hub;
    return hub;
}

}  // namespace awl::gx
