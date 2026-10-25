#include "gfx/gfx_pacing.h"

#include <windows.h>

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
