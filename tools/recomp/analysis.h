// Static analysis over the decoded DOL text: function discovery, indirect branch classification
// and platform-boundary evidence. Nothing here embeds game bytes; everything is derived at run time.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "image.h"
#include "ppc.h"

namespace awl::recomp {

struct TextSeg {
    uint32_t base = 0;
    uint32_t sectionIndex = 0;
    std::vector<Insn> insns;
    uint32_t end() const { return base + static_cast<uint32_t>(insns.size()) * 4; }
};

struct Program {
    const DolImage* dol = nullptr;
    std::vector<TextSeg> segs;
    const Insn* at(uint32_t addr) const;                 // nullptr outside text or misaligned
    bool inText(uint32_t addr) const { return at(addr) != nullptr; }
    int64_t wordIndex(uint32_t addr) const;              // global index across segments, or -1
    size_t totalWords() const;
};

Program buildProgram(const DolImage& dol);

enum SeedKind : uint32_t {
    SeedEntry = 1,
    SeedBl = 2,        // target of bl
    SeedRel = 4,       // referenced by a REL relocation against module 0
    SeedData = 8,      // text address found in a data section
    SeedTail = 16,     // target of a tail-call style b
    SeedGap = 32,      // prologue/gap heuristic
    SeedFall = 64,     // reached by fall-through from another function (entry chain)
    SeedJumpTable = 128,
    SeedConst = 256    // text address built from lis+addi/ori immediates in code (function-pointer literal)
};

enum class IndirectKind {
    JumpTableBounded,    // table length taken from a preceding cmplwi/bgt (or bge) bound check
    JumpTableScanned,    // no bound check found; length found by scanning valid text pointers
    ConstTailJump,       // mtctr from a lis/ori style constant: direct tail jump
    IndirectTailCall,    // epilogue before bctr: tail call through a function pointer or vtable
    Conditional,         // conditional bcctr
    Unresolved
};

struct IndirectJump {
    uint32_t addr = 0;
    uint32_t funcStart = 0;
    IndirectKind kind = IndirectKind::Unresolved;
    uint32_t table = 0;
    uint32_t count = 0;
    uint32_t constTarget = 0;
    std::vector<uint32_t> targets;
};

struct PlatformEvidence {
    uint32_t privileged = 0;     // supervisor instructions the runtime cannot model (rfi, sc)
    uint32_t supervisorState = 0;   // supervisor state moves the runtime models as storage (MSR, SR, BAT/HID/L2CR SPRs)
    std::map<std::string, uint32_t> mmio;   // hardware block name -> access count
    uint32_t lowmem = 0;         // absolute accesses to the OS low-memory area 0x80000000-0x800030FF
    std::vector<std::string> privilegedKinds;
    bool any() const { return privileged != 0 || !mmio.empty(); }
};

struct Function {
    uint32_t start = 0;
    uint32_t seeds = 0;
    std::vector<uint32_t> body;            // sorted instruction addresses reached from start
    std::vector<uint32_t> calls;           // direct bl targets
    std::vector<uint32_t> tailCalls;       // b targets that leave the function
    uint32_t fallsInto = 0;                // non-zero when execution runs into another entry
    uint32_t indirectCalls = 0;            // bctrl/blrl count
    uint32_t sharedWords = 0;              // words of this body already owned by an earlier function
    uint32_t badDecodes = 0;
    std::vector<uint32_t> badDecodeAddrs;
    PlatformEvidence platform;
    uint32_t end() const { return body.empty() ? start + 4 : body.back() + 4; }
};

struct DiscoveryStats {
    size_t functions = 0;
    std::map<std::string, size_t> bySeed;     // seed kind name -> count of functions having it
    size_t textBytes = 0;
    size_t assignedBytes = 0;                 // union of function bodies
    size_t paddingBytes = 0;                  // nop/zero words between functions
    size_t overlapWords = 0;                  // words owned by more than one function
    size_t unassignedBytes = 0;               // text bytes in no function and not padding
    size_t dataCandidates = 0;                // text-pointing words found in data sections
    size_t dataAccepted = 0;
    size_t dataInsideBody = 0;                // pointers into an existing function body (case labels etc.)
    size_t dataRejected = 0;
    size_t tailCallEdges = 0;
    size_t fallThroughEdges = 0;
    size_t gapSeeds = 0;
    size_t constCandidates = 0;               // distinct text addresses built by lis+addi/ori pairs
    size_t constAccepted = 0;                 // of those, new function starts (not already known/inside a body)
};

struct Discovery {
    std::map<uint32_t, Function> funcs;
    std::vector<IndirectJump> indirect;
    std::map<std::pair<uint32_t, uint32_t>, size_t> overlapPairs;   // (walking function, earlier owner) -> shared words
    std::vector<uint32_t> ambiguousSeeds;     // seeds that landed inside an existing body (not at its start)
    std::vector<std::pair<uint32_t, uint32_t>> overlaps;   // first overlapping word, owner pair starts
    std::vector<std::pair<uint32_t, uint32_t>> unassignedRanges;  // [start,end) not in any function
    std::vector<uint32_t> badDecodeAddrsOutside;          // invalid words outside every function
    std::vector<uint32_t> badDecodeAddrsInside;
    std::vector<IndirectJump> bctrUnique;     // one classification per non-linking bcctr word in text
    size_t bctrUnreached = 0;                 // of those, not inside any discovered function body
    std::vector<uint32_t> suspectData;        // functions whose body contains undecodable words (data-in-text)
    size_t totalBctrl = 0;
    DiscoveryStats stats;

    const Function* containing(uint32_t addr) const;       // function whose body holds addr
};

struct DiscoveryInput {
    std::vector<uint32_t> relTargets;       // module-0 addresses referenced from RELs
    std::vector<uint32_t> extraEntries;     // additional entry points (REL prolog/epilog/unresolved)
};

Discovery discoverFunctions(const Program& prog, const DiscoveryInput& in);

// Resolves a single bctr (exposed for tests).
IndirectJump classifyBctr(const Program& prog, uint32_t funcStart, uint32_t bctrAddr);

const char* indirectKindName(IndirectKind k);

// True when the instruction (conservatively) modifies general register `reg`.
bool writesGpr(const Insn& i, uint32_t reg);

// Names a hardware block for a constant physical/uncached address, or nullptr outside 0xCC000000-0xCDFFFFFF.
const char* mmioBlockName(uint32_t addr);

}  // namespace awl::recomp
