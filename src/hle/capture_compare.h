// Comparison of the Dolphin captures (build\dolphin-capture\) with the platform's boot assumptions and the host's boot
// trace; produces the markdown report. See docs/research/hle-platform.md, section 10.
#pragma once

#include <string>

#include "boot_table.h"

namespace awl::hle::capture {

struct CompareInputs {
    std::string captureDir = "build/dolphin-capture";
    std::string tracePath = "build/boot-trace.log";
    std::string discDir = "disc";                       // sys/boot.bin, sys/bi2.bin and sys/fst.bin are read for the placement checks
    std::string hostDumpDir = "build/host-dumps";       // optional at-entry-mem1.raw / after-osinit-mem1.raw written by awl-host
    BootValues values = bootValues();                   // the assumptions under test (tests pass their own)
};

// Builds the whole report. Missing files give "not captured" sections; malformed lines are listed; never throws for bad input.
std::string buildReport(const CompareInputs& in);

}  // namespace awl::hle::capture
