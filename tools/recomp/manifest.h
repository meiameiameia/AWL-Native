// Reader for tools/recomp/hle_manifest.txt: the functions the platform track replaces with native
// handlers. The format is documented in the manifest's header. The generator emits rt::hleCall for each
// entry instead of the recompiled body.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "analysis.h"

namespace awl::recomp {

struct HleEntry {
    uint32_t addr = 0;
    std::string name;
    std::string evidence;
    int line = 0;          // 1-based manifest line, for diagnostics
};

// Parses manifest text. On any malformed line or duplicate address returns false and fills `errors`
// with one "<source>:<line>: <message>" string per problem (parsing continues to report all of them).
// An empty manifest (comments and blank lines only) is valid and yields no entries. Entries are returned
// sorted by address.
bool parseHleManifest(const std::string& text, const std::string& source, std::vector<HleEntry>& out,
                      std::vector<std::string>& errors);

// Rejects entries whose address is not a discovered DOL function start.
bool validateHleManifest(const std::vector<HleEntry>& entries, const Discovery& d, const std::string& source,
                         std::vector<std::string>& errors);

}  // namespace awl::recomp
