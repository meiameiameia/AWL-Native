// Pacing maths and behaviour, without a window. The expected values are computed by hand from the NTSC field rate:
// 60000/1001 Hz, i.e. 60000 fields last exactly 1001 seconds.
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
    testHostClock();
    testVideoOutputPacing();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    std::printf("gfx_pacing_tests: all checks passed\n");
    return 0;
}
