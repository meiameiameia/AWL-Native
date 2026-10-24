// C++ emission for the static-recompilation spike. Output goes under an ignored build directory only.
#pragma once

#include <string>
#include <vector>

#include "analysis.h"
#include "image.h"
#include "manifest.h"

namespace awl::recomp {

// Prints the discovery/indirect-branch/platform-evidence report to stdout.
int reportDiscovery(const Program& prog, const Discovery& d, const std::vector<RelImage>& rels, const DiscoveryInput& in);

// Emits C++ for the functions reachable from the entry point (boot path) or for every discovered
// function (`full`). Every manifest entry (already validated against `d`) is emitted as a call to
// rt::hleCall instead of its recompiled body, and is not traversed when computing the boot closure.
// Returns a process exit code.
int generateCode(const Program& prog, const Discovery& d, const std::vector<HleEntry>& hle, const std::string& outDir,
                 bool full);

}  // namespace awl::recomp
