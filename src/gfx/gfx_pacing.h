// Real-time pacing of the guest's video retraces. The guest time base is instruction-based and stays that way: pacing
// only decides when the host lets the guest continue, never what the guest computes.
//
// Retrace k (counted from the anchor) is due at anchor + k * 1001/60000 s (NTSC field rate 59.94 Hz, the same
// 675,675 time-base ticks per field that the VI model uses). All arithmetic is integer, in clock ticks of the
// caller's high-resolution counter.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace awl::gfx {

class RetracePacer {
public:
    // frequency: ticks per second of the clock passed to the other calls (QueryPerformanceFrequency on Windows).
    // maxLagRetraces: when the host is more than this many retraces behind schedule it re-anchors instead of running
    // faster than real time to catch up (a debugger break, a long stall, a window drag).
    explicit RetracePacer(int64_t frequency, int64_t maxLagRetraces = 4) : freq_(frequency), maxLag_(maxLagRetraces) {}

    // Anchors the schedule: the next retrace is due one field period after `now`.
    void start(int64_t now) { anchor_ = now; count_ = 0; started_ = true; }
    bool started() const { return started_; }

    // Called when a guest retrace occurs. Returns the clock value at which the host should let the guest continue; it is
    // <= now when the host is on time or late and must not sleep.
    int64_t onRetrace(int64_t now);

    // Pause / resume (focus loss): after resume the schedule restarts from `now`, so there is no catch-up burst.
    void pause() { paused_ = true; }
    void resume(int64_t now) { paused_ = false; start(now); }
    bool paused() const { return paused_; }

    int64_t periodTicks() const { return dueOffset(1); }
    // Offset of retrace k from the anchor, in clock ticks (exact floor of k * 1001 * freq / 60000).
    int64_t dueOffset(int64_t k) const;
    uint64_t reanchors() const { return reanchors_; }
    int64_t retraces() const { return count_; }

private:
    int64_t freq_, maxLag_;
    int64_t anchor_ = 0, count_ = 0;
    bool started_ = false, paused_ = false;
    uint64_t reanchors_ = 0;
};

// How fast the guest runs relative to real time, measured over windows of retraces: 100% = 59.94 retraces per second of wall time.
// A pause (focus loss) must call reset() so that it does not count as slowness.
class SpeedMeter {
public:
    explicit SpeedMeter(int64_t frequency, int64_t windowRetraces = 30) : freq_(frequency), window_(windowRetraces) {}
    void reset() { count_ = 0; started_ = false; }
    // Returns true when a new reading is available (percent()).
    bool onRetrace(int64_t now);
    int percent() const { return percent_; }

private:
    int64_t freq_, window_, start_ = 0, count_ = 0;
    bool started_ = false;
    int percent_ = 0;
};

// Window title: product name, speed and the main keys, kept short so that a narrow title bar still shows the state. speedPercent < 0
// leaves the speed out. paused puts "PAUSED - click this window to continue" first, where truncation cannot hide it.
std::string windowTitle(const std::string& base, int speedPercent, bool paused = false);

// How the display's refresh rate relates to the game's 59.94 Hz, in the overlay's capital letters: e.g. 60 Hz -> every game frame
// shown for exactly one refresh; 144 Hz -> each shown for 2 or 3 refreshes (whole refreshes only, never torn); below 59.94 the game
// cannot keep full speed. hz <= 0 (unknown) gives an empty string.
std::string displayLine(int hz);

// The help text drawn in the top-left corner of the picture (visible in fullscreen too): speed first (with a note when the game
// runs slower than the original), the display line when the refresh rate is known (displayHz > 0), then the controls.
// speedPercent < 0 = not measured yet.
std::vector<std::string> overlayLines(int speedPercent, int displayHz = 0);

// The big notice drawn in the middle of the last picture while the game is paused (focus lost); shown even when F1 hid the help text.
std::vector<std::string> pausedLines();

// Host clock helpers (QueryPerformanceCounter). sleepUntil sleeps coarsely and spins for the last stretch.
int64_t hostClockNow();
int64_t hostClockFrequency();
void hostSleepUntil(int64_t deadline);

}  // namespace awl::gfx
