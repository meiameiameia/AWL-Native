// Pacing maths and behaviour, without a window. The expected values are computed by hand from the NTSC field rate:
// 60000/1001 Hz, i.e. 60000 fields last exactly 1001 seconds.
#include <algorithm>
#include <cstdio>

#include "gfx/gfx_output.h"
#include "gfx/gfx_pacing.h"

using namespace awl::gfx;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void testSchedule() {
    const int64_t freq = 10000000;   // 10 MHz counter (the usual QueryPerformanceFrequency)
    RetracePacer p(freq);
    // one field: 1001/60000 s = 166,833.33 ticks -> floor
    CHECK(p.dueOffset(1) == 166833);
    CHECK(p.periodTicks() == 166833);
    // 60000 fields = 1001 s exactly
    CHECK(p.dueOffset(60000) == 1001 * freq);
    CHECK(p.dueOffset(120000) == 2002 * freq);
    // ten million fields (about 46 hours): 1.001e10 / 60000 s = 166833 s + 20000/60000 s
    CHECK(p.dueOffset(10000000) == 1668333333333ll);
    // a 1 GHz counter keeps full resolution: 1001e9 / 60000 = 16,683,333.33
    RetracePacer g(1000000000);
    CHECK(g.dueOffset(1) == 16683333);
    CHECK(g.dueOffset(60000) == 1001ll * 1000000000);
    // the schedule never drifts: the due time of retrace k does not depend on how the earlier ones were timed
    RetracePacer a(freq), b(freq);
    a.start(1000); b.start(1000);
    int64_t last = 0;
    for (int i = 0; i < 100; ++i) last = a.onRetrace(1000 + i);   // always early: returns the schedule
    CHECK(last == 1000 + p.dueOffset(100));
    CHECK(b.onRetrace(5000) == 1000 + p.dueOffset(1));
}

static void testOnTimeLateAndReanchor() {
    const int64_t freq = 10000000;
    RetracePacer q(freq);
    q.start(0);
    const int64_t due1 = q.onRetrace(0);
    CHECK(due1 == q.dueOffset(1));
    // 2 periods late: still within the allowed lag, no catch-up burst needed beyond running without sleeping
    const int64_t due2 = q.onRetrace(due1 + 2 * q.periodTicks());
    CHECK(due2 == q.dueOffset(2));
    CHECK(due2 < due1 + 2 * q.periodTicks());         // due time is in the past: caller does not sleep
    CHECK(q.reanchors() == 0);
    // more than maxLag (4) periods late: re-anchor at now instead of running fast to catch up
    const int64_t now = 100 * q.periodTicks();
    CHECK(q.onRetrace(now) == now);
    CHECK(q.reanchors() == 1);
    CHECK(q.onRetrace(now) == now + q.dueOffset(1));  // the next retrace is one period after the re-anchor
}

static void testPauseResume() {
    const int64_t freq = 10000000;
    RetracePacer p(freq);
    p.start(0);
    p.onRetrace(0);
    p.pause();
    CHECK(p.paused());
    p.resume(50 * freq);                              // 50 seconds later (focus returns)
    CHECK(!p.paused());
    CHECK(p.onRetrace(50 * freq) == 50 * freq + p.dueOffset(1));   // no burst of 3000 retraces to catch up
    CHECK(p.reanchors() == 0);
}

static void testSpeedMeter() {
    const int64_t freq = 10000000;
    // 30 retraces last 30 * 1001/60000 s = 0.5005 s of guest time. The first call only starts the window.
    SpeedMeter full(freq);
    CHECK(!full.onRetrace(0));
    for (int i = 1; i < 30; ++i) CHECK(!full.onRetrace(freq / 100 * i));
    CHECK(full.onRetrace(5005000));                        // 0.5005 s of wall time: 100%
    CHECK(full.percent() == 100);
    // half speed: the same 30 retraces take 1.001 s
    SpeedMeter half(freq);
    half.onRetrace(1000);
    bool got = false;
    for (int i = 1; i <= 30; ++i) got = half.onRetrace(1000 + 10010000 * i / 30);
    CHECK(got && half.percent() == 50);
    // the next window is measured from the end of the previous one; a pause (reset) discards the partial window
    for (int i = 1; i <= 30; ++i) got = half.onRetrace(1000 + 10010000 + 2002000 * i / 30);   // 0.2002 s for 0.5005 s of guest time: 250%
    CHECK(got && half.percent() == 250);
    half.reset();
    CHECK(!half.onRetrace(999999999));                     // starts a new window, no reading
    // titles: product name, controls, speed
    const std::string t = windowTitle("Game", 31);
    CHECK(t.find("Game") == 0 && t.find("Esc quit") != std::string::npos && t.find("F11 fullscreen") != std::string::npos);
    CHECK(t.find("speed 31% of real time") != std::string::npos);
    CHECK(windowTitle("Game", -1).find("speed") == std::string::npos);
    // paused: the state comes first so that a narrow title bar cannot cut it off; speed is left out while paused
    const std::string p = windowTitle("Game", 100, true);
    CHECK(p.find("PAUSED - click this window to continue") == 0 && p.find("Game") != std::string::npos && p.find("speed") == std::string::npos);
    // not paused: the speed comes before the key list, and the whole title stays short enough for a 640 pixel window (about 75 characters fit)
    CHECK(t.find("speed") < t.find("Esc quit"));
    CHECK(windowTitle("Harvest Moon: A Wonderful Life (native)", 100).find("speed 100%") < 80);
}

static void testDisplayLine() {
    CHECK(displayLine(0).empty() && displayLine(-5).empty());
    CHECK(displayLine(60).find("DISPLAY 60 HZ") == 0 && displayLine(60).find("1 REFRESH") != std::string::npos);
    CHECK(displayLine(59).find("1 REFRESH") != std::string::npos);                            // 59.94 reported as 59
    CHECK(displayLine(120).find("2 REFRESHES") != std::string::npos && displayLine(120).find(" OR ") == std::string::npos);
    CHECK(displayLine(180).find("3 REFRESHES") != std::string::npos);
    CHECK(displayLine(144).find("2 OR 3 REFRESHES") != std::string::npos);                    // 144 / 59.94 = 2.40
    CHECK(displayLine(75).find("1 OR 2 REFRESHES") != std::string::npos);                     // 75 / 59.94 = 1.25
    CHECK(displayLine(50).find("CANNOT RUN AT FULL SPEED") != std::string::npos);
    // the overlay carries it right after the speed line, only when the rate is known
    CHECK(overlayLines(100, 144)[1] == displayLine(144));
    CHECK(overlayLines(100, 0).size() + 1 == overlayLines(100, 144).size());
    const std::vector<std::string> pz = pausedLines();
    CHECK(pz.size() == 2 && pz[0] == "PAUSED" && pz[1].find("CLICK THIS WINDOW") != std::string::npos);
}

static void testOverlay() {
    // text: speed first, a slow-game note below 95%, the controls from the brief
    const std::vector<std::string> slow = overlayLines(31);
    CHECK(slow.size() >= 3 && slow[0].find("SPEED 31% OF REAL TIME") == 0 && slow[0].find("SLOWER THAN THE ORIGINAL") != std::string::npos);
    const std::vector<std::string> fast = overlayLines(100);
    CHECK(fast[0].find("SPEED 100% OF REAL TIME") == 0 && fast[0].find("SLOWER") == std::string::npos);
    CHECK(overlayLines(-1)[0].find("MEASURING") != std::string::npos);
    std::string all;
    for (const std::string& l : slow) all += l + ' ';
    for (const char* k : {"ARROWS = D-PAD", "WASD = STICK", "SPACE = A", "BACKSPACE = B", "ENTER = START", "F11 FULLSCREEN", "ESC QUIT"}) CHECK(all.find(k) != std::string::npos);
    // drawing: a panel changes only its own box; text pixels are white, the box behind is darkened; lower case = upper case; unknown = blank
    Image base;
    base.resize(200, 100);
    base.fill(200, 100, 50);
    Image img = base;
    drawTextPanel(img, 4, 4, 1, {"Ab 1%"});
    const uint32_t boxW = 6 * 5 + 6, boxH = 10 + 6;
    bool whiteSeen = false, darkSeen = false, outsideChanged = false;
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x) {
            const bool inside = x >= 4 && x < 4 + boxW && y >= 4 && y < 4 + boxH;
            const bool same = std::equal(img.px(x, y), img.px(x, y) + 4, base.px(x, y));
            if (!inside && !same) outsideChanged = true;
            if (inside && img.px(x, y)[0] == 255 && img.px(x, y)[1] == 255) whiteSeen = true;
            if (inside && img.px(x, y)[0] == 70) darkSeen = true;
        }
    CHECK(whiteSeen && darkSeen && !outsideChanged);
    Image lower = base, upper = base;
    drawText(lower, 10, 10, 2, "hello", 255, 255, 255);
    drawText(upper, 10, 10, 2, "HELLO", 255, 255, 255);
    CHECK(lower == upper && !(lower == base));
    Image blank = base;
    drawText(blank, 10, 10, 1, "~ #", 255, 255, 255);
    CHECK(blank == base);
    drawText(blank, 190, 95, 3, "WWW", 255, 255, 255);   // partly outside the image: clipped, no crash
    CHECK(textWidth("abc", 2) == 36);
}

static void testPausedImage() {
    Image last;
    last.resize(640, 448);
    last.fill(200, 100, 50);
    const Image p = makePausedImage(last);
    CHECK(p.width == 640 && p.height == 448);
    CHECK(p.px(0, 0)[0] == 100 && p.px(0, 0)[1] == 50 && p.px(0, 0)[2] == 25);   // dimmed to half brightness
    // the notice is in the middle: white text pixels on a darkened box, and nothing in the top rows
    bool white = false, topChanged = false;
    for (uint32_t y = 0; y < 448; ++y)
        for (uint32_t x = 0; x < 640; ++x) {
            const uint8_t* d = p.px(x, y);
            if (y >= 100 && y <= 350 && d[0] == 255 && d[1] == 255 && d[2] == 255) white = true;
            if (y < 40 && !(d[0] == 100 && d[1] == 50 && d[2] == 25)) topChanged = true;
        }
    CHECK(white && !topChanged);
    CHECK(last.px(0, 0)[0] == 200);                                              // the input is not touched
    const Image blank = makePausedImage(Image());                                // no picture yet: black 640 x 448 with the notice
    CHECK(blank.width == 640 && blank.height == 448 && blank.px(0, 0)[0] == 0);
    last.resize(1280, 896);
    last.fill(10, 10, 10);
    const Image big = makePausedImage(last);                                     // twice the size: twice the text
    CHECK(big.width == 1280 && big.height == 896);
}

static void testHostClock() {
    const int64_t f = hostClockFrequency();
    CHECK(f > 0);
    const int64_t t0 = hostClockNow();
    hostSleepUntil(t0 + f / 100);                     // 10 ms
    const int64_t t1 = hostClockNow();
    CHECK(t1 - t0 >= f / 100);
    CHECK(t1 - t0 < f / 2);
    hostSleepUntil(t0);                               // a deadline in the past returns immediately
}

static void testVideoOutputPacing() {
    // Four retraces without a window: each waits for its due time, so at least four field periods pass.
    OutputOptions o;
    o.pace = true;
    VideoOutput v;
    std::string err;
    CHECK(v.init(o, err));
    const int64_t t0 = hostClockNow();
    for (int i = 0; i < 4; ++i) CHECK(v.onRetrace(nullptr) == RetraceAction::Continue);
    const double ms = double(hostClockNow() - t0) * 1000.0 / double(hostClockFrequency());
    std::printf("gfx_pacing_tests: 4 paced retraces took %.2f ms (ideal 66.73)\n", ms);
    CHECK(ms >= 4 * 16.6833 - 1.0);
    CHECK(ms < 1000.0);
}

int main() {
    testSchedule();
    testOnTimeLateAndReanchor();
    testPauseResume();
    testSpeedMeter();
    testOverlay();
    testDisplayLine();
    testPausedImage();
    testHostClock();
    testVideoOutputPacing();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    std::printf("gfx_pacing_tests: all checks passed\n");
    return 0;
}
