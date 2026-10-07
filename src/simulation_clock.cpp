#include "awl/simulation_clock.h"
#include <algorithm>
#include <limits>

namespace awl {
bool SimulationClock::initialize(uint64_t frequency, uint32_t numerator,
                                  uint32_t denominator) {
    if (frequency < 1000 || numerator == 0 || numerator > 60000 ||
        denominator == 0 || denominator > 1001 ||
        numerator > denominator * 60u ||
        frequency > std::numeric_limits<uint64_t>::max() /
                        (uint64_t(numerator) + denominator)) {
        return false;
    }
    frequency_ = frequency;
    numerator_ = numerator;
    divisor_ = frequency * denominator;
    remainder_ = 0;
    active_ = false;
    return true;
}

bool SimulationClock::advance(uint64_t elapsed, bool active,
                               SimulationSteps* steps) {
    if (!steps || !frequency_) return false;
    SimulationSteps next;
    if (!active || !active_) {
        remainder_ = 0;
        active_ = active;
        next.discarded_counter_ticks = elapsed;
    } else {
        const uint64_t admitted = std::min(elapsed, frequency_ / 10);
        next.discarded_counter_ticks = elapsed - admitted;
        const uint64_t accumulated = remainder_ + admitted * numerator_;
        next.count = static_cast<uint32_t>(accumulated / divisor_);
        remainder_ = accumulated % divisor_;
    }
    *steps = next;
    return true;
}
} // namespace awl
