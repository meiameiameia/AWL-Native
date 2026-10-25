// Headless tests of the DX11 device layer: device creation (hardware, WARP), offscreen render + readback, the
// letterbox maths, BMP round trips, the diagnostic frame and frame capture through VideoOutput. No window is opened.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "gfx/gfx_output.h"

using namespace awl::gfx;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static Image gradient(uint32_t w, uint32_t h) {
    Image img;
    img.resize(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = img.px(x, y);
            p[0] = static_cast<uint8_t>(x * 7 + 1); p[1] = static_cast<uint8_t>(y * 11 + 2); p[2] = static_cast<uint8_t>(x * y + 3); p[3] = 255;
        }
    return img;
}

static void testLetterbox() {
    // 4:3 picture in a 16:9 client: full height, pillarbox. 720 * 4/3 = 960, centred in 1280.
    CHECK(letterbox(640, 448, 4, 3, 1280, 720) == (Rect{160, 0, 960, 720}));
    // exact aspect: fills
    CHECK(letterbox(640, 448, 4, 3, 800, 600) == (Rect{0, 0, 800, 600}));
    // taller client: letterbox. 400 wide -> 300 high, centred at y=50
    CHECK(letterbox(640, 480, 4, 3, 400, 400) == (Rect{0, 50, 400, 300}));
    // source aspect when no display aspect is given: 2:1 source in a square client
    CHECK(letterbox(200, 100, 0, 0, 100, 100) == (Rect{0, 25, 100, 50}));
    // degenerate sizes give an empty rectangle
    CHECK(letterbox(0, 0, 4, 3, 100, 100).w == 0);
    CHECK(letterbox(640, 480, 4, 3, 0, 0).w == 0);
}

static void testBmp() {
    const Image img = gradient(37, 23);   // width not a multiple of 4: exercises row padding
    const std::vector<uint8_t> bytes = encodeBmp(img);
    CHECK(bytes.size() == 54 + ((37 * 3 + 3) / 4 * 4) * 23u);
    CHECK(bytes[0] == 'B' && bytes[1] == 'M');
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "awl_gfx_test_bmp";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "a.bmp").string();
    std::string err;
    CHECK(writeBmp(path, img, err));
    Image back;
    CHECK(readBmp(path, back, err));
    CHECK(back == img);
    CHECK(slurp(path) == bytes);
    // bottom-up storage: the first pixel row in the file is the image's last row, stored as BGR
    CHECK(bytes[54 + 0] == img.px(0, 22)[2] && bytes[54 + 1] == img.px(0, 22)[1] && bytes[54 + 2] == img.px(0, 22)[0]);
    CHECK(!readBmp((dir / "missing.bmp").string(), back, err));
    std::filesystem::remove_all(dir);
}

static void testDiagnosticFrame() {
    Image a, b, c;
    drawDiagnosticFrame(a, 640, 448, 17);
    drawDiagnosticFrame(b, 640, 448, 17);
    drawDiagnosticFrame(c, 640, 448, 18);
    CHECK(a == b);          // deterministic
    CHECK(!(a == c));       // the latch count is visible
    CHECK(a.width == 640 && a.height == 448);
    CHECK(a.px(639, 447)[0] == 0xA0 && a.px(639, 447)[1] == 0x10 && a.px(639, 447)[2] == 0xA0);   // background colour
    // The first glyph ('1', scale 8) puts its top-row centre pixel (column 2 of 5) at x = 16 + 2*8, y = 16: white.
    CHECK(a.px(16 + 2 * 8 + 3, 16 + 3)[0] == 255 && a.px(16 + 2 * 8 + 3, 16 + 3)[1] == 255);
}

static void testDeviceAndOffscreen(bool forceWarp) {
    Device dev;
    DeviceOptions o;
    o.forceWarp = forceWarp;
    std::string err;
    CHECK(dev.create(o, err));
    if (!err.empty()) std::fprintf(stderr, "device: %s\n", err.c_str());
    CHECK(dev.device() != nullptr && dev.context() != nullptr && dev.factory() != nullptr);
    CHECK(dev.featureLevel() >= D3D_FEATURE_LEVEL_11_0);
    if (forceWarp) CHECK(dev.isWarp());
    std::printf("gfx_device_tests: %s device: %s\n", forceWarp ? "forced WARP" : (dev.isWarp() ? "WARP fallback" : "hardware"), dev.adapterName().c_str());

    // 1:1 blit through the real pipeline reproduces the image bit for bit (point sampling, no filtering).
    Blitter blit;
    CHECK(blit.init(dev, err));
    OffscreenTarget target;
    const Image src = gradient(37, 23);
    CHECK(target.create(dev, src.width, src.height, err));
    ImageTexture tex;
    CHECK(tex.update(dev, src, err));
    blit.blit(dev, target.rtv(), src.width, src.height, tex.srv(), Rect{0, 0, 37, 23}, false);
    Image out;
    CHECK(target.readback(dev, out, err));
    CHECK(out == src);

    // letterboxed blit of a solid colour: black bars, picture in the middle (64x64 target, 4:3 -> 64x48 at y=8)
    Image solid;
    solid.resize(16, 12);
    solid.fill(200, 100, 50);
    OffscreenTarget t2;
    CHECK(t2.create(dev, 64, 64, err));
    ImageTexture tex2;
    CHECK(tex2.update(dev, solid, err));
    const Rect r = letterbox(16, 12, 4, 3, 64, 64);
    CHECK(r == (Rect{0, 8, 64, 48}));
    blit.blit(dev, t2.rtv(), 64, 64, tex2.srv(), r, true);
    Image out2;
    CHECK(t2.readback(dev, out2, err));
    auto closeTo = [](const uint8_t* p, int r0, int g0, int b0) { return std::abs(p[0] - r0) <= 1 && std::abs(p[1] - g0) <= 1 && std::abs(p[2] - b0) <= 1; };
    CHECK(closeTo(out2.px(5, 3), 0, 0, 0) && closeTo(out2.px(63, 7), 0, 0, 0));
    CHECK(closeTo(out2.px(0, 8), 200, 100, 50) && closeTo(out2.px(32, 32), 200, 100, 50) && closeTo(out2.px(63, 55), 200, 100, 50));
    CHECK(closeTo(out2.px(5, 60), 0, 0, 0) && closeTo(out2.px(63, 56), 0, 0, 0));
    CHECK(out2.px(32, 32)[3] == 255);
}

static void testCapture() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "awl_gfx_test_capture";
    std::filesystem::remove_all(dir);
    auto run = [&](const std::filesystem::path& sub, bool warp) {
        OutputOptions o;
        o.captureFrames = 3;
        o.captureDir = (dir / sub).string();
        o.forceWarp = warp;
        VideoOutput v;
        std::string err;
        CHECK(v.init(o, err));
        CHECK(v.input() == nullptr);   // no window: no input source
        for (uint64_t i = 1; i <= 6; ++i) {
            FrameDesc f;
            f.width = 96; f.height = 64; f.strideBytes = 192; f.latchIndex = i;
            const bool latched = i != 2 && i != 5;   // retraces 2 and 5 show no new latch
            CHECK(v.onRetrace(latched ? &f : nullptr) == RetraceAction::Continue);
        }
        CHECK(v.presented() == 4);     // latched retraces: 1, 3, 4, 6
        CHECK(v.captured() == 3);      // only the first three are written
    };
    run("a", false);
    run("b", true);   // software device: same bytes
    for (int i = 0; i < 3; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "frame_%04d.bmp", i);
        const std::vector<uint8_t> a = slurp((dir / "a" / name).string()), b = slurp((dir / "b" / name).string());
        CHECK(!a.empty() && a == b);
        Image img;
        std::string err;
        CHECK(readBmp((dir / "a" / name).string(), img, err));
        // frames 0..2 are the latches 1, 3, 4 of the run above
        const uint64_t latch[3] = {1, 3, 4};
        Image expect;
        drawDiagnosticFrame(expect, 96, 64, latch[i]);
        CHECK(img == expect);
    }
    CHECK(!std::filesystem::exists(dir / "a" / "frame_0003.bmp"));
    std::filesystem::remove_all(dir);

    // a capture folder that cannot be created is a loud error
    OutputOptions bad;
    bad.captureFrames = 1;
    bad.captureDir = "";
    VideoOutput v;
    std::string err;
    CHECK(!v.init(bad, err) && !err.empty());

    // no window, no capture, no pacing: frames are counted and nothing else happens
    OutputOptions none;
    VideoOutput w;
    CHECK(w.init(none, err));
    FrameDesc f;
    f.width = 640; f.height = 448;
    CHECK(w.onRetrace(&f) == RetraceAction::Continue && w.presented() == 1 && w.captured() == 0);
}

int main() {
    testLetterbox();
    testBmp();
    testDiagnosticFrame();
    testDeviceAndOffscreen(false);
    testDeviceAndOffscreen(true);
    testCapture();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    std::printf("gfx_device_tests: all checks passed\n");
    return 0;
}
