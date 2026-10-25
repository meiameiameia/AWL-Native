// Resolution of discovery ambiguities before code generation. Discovery walks every seed as its own function, so
// a heuristic seed that lands inside code another function already reaches yields a second, overlapping "function".
// This pass decides, from evidence, which of those are not functions at all. A dropped function is never emitted
// and is not in the dispatch table: its words stay in the functions that own them, direct references to it
// cannot exist (that is what the rules prove), and a computed jump to its address stops loudly with the address.
// Functions that legitimately share a tail (both referenced) are emitted with their own copy of the shared code.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "analysis.h"

namespace awl::recomp {

enum class DropReason {
    DataInText,          // body reaches an undecodable word and has no strong reference: data, not code
    JumpTableLabel,      // a case label of another function's jump table: every reference to it is a table entry
    UnreferencedFragment // seeded only by the gap heuristic, no reference of any kind, overlaps other functions
};

const char* dropReasonName(DropReason r);

struct Resolution {
    std::map<uint32_t, DropReason> dropped;   // function start -> why it is not emitted
    // Statistics over the functions that remain (after dropping) and, for comparison, over all discovered ones.
    size_t overlapFunctionsBefore = 0, overlapWordsBefore = 0;
    size_t overlapFunctionsAfter = 0, overlapWordsAfter = 0;
    size_t exclusiveWordsDropped = 0;         // words only a dropped function reached (no emitted function holds them)
    std::vector<uint32_t> remainingShared;    // functions that still share words (each emits its own copy)
    bool isDropped(uint32_t start) const { return dropped.count(start) != 0; }
};

Resolution resolveFunctions(const Program& prog, const Discovery& d);

}  // namespace awl::recomp
