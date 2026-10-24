// C++ emission for the static-recompilation spike. Output goes under an ignored build directory only.
#pragma once

#include <string>
#include <vector>

#include "analysis.h"
#include "image.h"

namespace awl::recomp {

// Prints the discovery/indirect-branch/platform-evidence report to stdout.
int reportDiscovery(const Program& prog, const Discovery& d, const std::vector<RelImage>& rels, const DiscoveryInput& in);

// Emits C++ for the functions reachable from the entry point (boot path) or for every discovered
// function (`full`). Returns a process exit code.
int generateCode(const Program& prog, const Discovery& d, const std::string& outDir, bool full);

}  // namespace awl::recomp
