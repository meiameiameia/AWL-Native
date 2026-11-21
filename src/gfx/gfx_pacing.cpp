#include "gfx/gfx_pacing.h"

#include <windows.h>

#include <cmath>

namespace awl::gfx {

int64_t RetracePacer::dueOffset(int64_t k) const {
    // k * 1001 / 60000 seconds, split so that nothing overflows for any realistic k and frequency.
    const int64_t n = k * 1001;
    const int64_t secs = n / 60000, rem = n % 60000;
    return secs * freq_ + rem * freq_ / 60000;
}

int64_t RetracePacer::onRetrace(int64_t now) {
    if (!started_) start(now);
    ++count_;
    const int64_t due = anchor_ + dueOffset(count_);
    if (now > due + maxLag_ * periodTicks()) {   // far behind: restart the schedule here instead of catching up
        ++reanchors_;
        start(now);
        return now;
    }
    return due;
}

bool SpeedMeter::onRetrace(int64_t now) {
    if (!started_) { start_ = now; count_ = 0; started_ = true; return false; }
    if (++count_ < window_) return false;
    const double wallSeconds = static_cast<double>(now - start_) / static_cast<double>(freq_);
    const double guestSeconds = static_cast<double>(count_) * 1001.0 / 60000.0;   // one retrace = 1001/60000 s
    percent_ = wallSeconds > 0 ? static_cast<int>(100.0 * guestSeconds / wallSeconds + 0.5) : 0;
    start_ = now;
    count_ = 0;
    return true;
}

std::string windowTitle(const std::string& base, int speedPercent, bool paused) {
    std::string t = paused ? "PAUSED - click this window to continue - " + base : base;
    if (!paused && speedPercent >= 0) t += " - speed " + std::to_string(speedPercent) + "% of real time";
    t += " - Esc quit, F11 fullscreen, F1 help text";
    return t;
}

std::string displayLine(int hz) {
    if (hz <= 0) return std::string();
    const double ratio = hz / (60000.0 / 1001.0);
    const std::string head = "DISPLAY " + std::to_string(hz) + " HZ: ";
    if (ratio < 0.98) return head + "BELOW 59.94 HZ, THE GAME CANNOT RUN AT FULL SPEED";
    const int whole = static_cast<int>(ratio + 0.03);
    if (std::fabs(ratio - whole) <= 0.03) {
        if (whole <= 1) return head + "EVERY GAME FRAME SHOWN FOR 1 REFRESH";
        return head + "EVERY GAME FRAME SHOWN FOR " + std::to_string(whole) + " REFRESHES";
    }
    return head + "GAME FRAMES SHOWN FOR " + std::to_string(whole) + " OR " + std::to_string(whole + 1) + " REFRESHES";
}

std::vector<std::string> overlayLines(int speedPercent, int displayHz) {
    std::vector<std::string> l;
    if (speedPercent < 0) l.push_back("SPEED: MEASURING...");
    else if (speedPercent < 95) l.push_back("SPEED " + std::to_string(speedPercent) + "% OF REAL TIME - SLOWER THAN THE ORIGINAL (EXPECTED FOR NOW)");
    else l.push_back("SPEED " + std::to_string(speedPercent) + "% OF REAL TIME");
    const std::string d = displayLine(displayHz);
    if (!d.empty()) l.push_back(d);
    l.push_back("ARROWS = D-PAD   WASD = STICK   SPACE = A   BACKSPACE = B   ENTER = START");
    l.push_back("X, TAB, Z, Q, E = X, Y, Z, L, R");
    l.push_back("F1 HIDE/SHOW THIS TEXT   F11 FULLSCREEN   ESC QUIT");
    return l;
}

std::vector<std::string> pausedLines() { return {"PAUSED", "CLICK THIS WINDOW TO CONTINUE"}; }

int64_t hostClockNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t hostClockFrequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

void hostSleepUntil(int64_t deadline) {
    const int64_t freq = hostClockFrequency();
    for (;;) {
        const int64_t left = deadline - hostClockNow();
        if (left <= 0) return;
        const int64_t ms = left * 1000 / freq;
        if (ms > 2) Sleep(static_cast<DWORD>(ms - 1));
        else YieldProcessor();
    }
}

}  // namespace awl::gfx
