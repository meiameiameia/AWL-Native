#pragma once
#include <cstdint>

namespace awl {
struct SimulationSteps {
    uint32_t count = 0;
    uint64_t discarded_counter_ticks = 0;
};

// Native fixed-step scheduler, independent of Present. The world-map DOL
// interval is two retraces. 30/1 is the rehearsal's nominal native rate;
// the exact VI wall-clock rate still needs a running-game comparison.
// Integer counter arithmetic also supports a supplied rational rate.
class SimulationClock {
public:
    [[nodiscard]] bool initialize(uint64_t counter_frequency,
                                  uint32_t rate_numerator = 30,
                                  uint32_t rate_denominator = 1);
    // Focus loss clears pending time. First activation establishes a fresh
    // baseline. A stall admits at most 100 ms; excess time is reported.
    // Invalid calls preserve clock and output. No allocation or waiting.
    [[nodiscard]] bool advance(uint64_t elapsed_counter_ticks, bool active,
                               SimulationSteps* steps);
private:
    uint64_t frequency_ = 0;
    uint64_t divisor_ = 0;
    uint64_t remainder_ = 0;
    uint32_t numerator_ = 0;
    bool active_ = false;
};
} // namespace awl
