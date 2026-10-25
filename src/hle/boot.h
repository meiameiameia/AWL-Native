// Boot environment: what the IPL and the apploader leave in memory before the DOL entry runs (docs/research/hle-platform.md,
// section 3). Inputs are the disc extraction's sys/ files; every assumed value is written to the boot trace.
#pragma once

#include <string>

#include "image.h"
#include "boot_table.h"
#include "platform.h"

namespace awl::hle {

// Zeroes MEM1, loads the DOL sections, builds the low-memory area, BI2 and the FST. Returns false with a message.
bool buildBootEnvironment(Machine& m, const recomp::DolImage& dol, const std::string& discDir, BootLayout& layout, std::string& err);

}  // namespace awl::hle
