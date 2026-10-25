// REL (relocatable module) support for the generator: lays the modules out at synthetic bases for analysis,
// records the relocation sites whose value depends on the module's load address, and finds the SDK's
// OSLink/OSUnlink in the DOL from evidence. Nothing here embeds game bytes or addresses.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "analysis.h"
#include "image.h"

namespace awl::recomp {

constexpr uint32_t kMaxRelModules = 8;     // module ids 1..7 (0 is the DOL); equals rt::kMaxRelModules
constexpr uint32_t kMaxRelSections = 32;

// A relocation whose result depends on where the target module is loaded: the 16-bit immediate of an
// instruction (lis/addi/ori/loads/stores) that holds a piece of a module-section address. Relocations against
// the DOL (module 0) are absolute and are patched into the analysed image instead.
struct DynReloc {
    uint8_t type = 0;          // R_PPC_ADDR16 (3), _LO (4), _HI (5) or _HA (6)
    uint32_t module = 0;       // target module id
    uint32_t section = 0;      // target section
    uint32_t addend = 0;
};

// One REL prepared for generation. Analysis and code generation run on the module linked at a synthetic base;
// the generated code never embeds that base: addresses are expressed through the module's section bases, which
// the runtime learns when the SDK's OSLink succeeds.
struct RelModule {
    const RelImage* rel = nullptr;
    RelLayout layout;                          // synthetic section addresses
    std::unique_ptr<DolImage> img;             // the module linked at the synthetic layout
    std::unique_ptr<Program> prog;
    Discovery disc;
    std::map<uint32_t, DynReloc> dyn;          // by synthetic address of the relocated instruction
    // Synthetic address -> section and offset within it; false outside every section of this module.
    bool locate(uint32_t addr, uint32_t& section, uint32_t& offset) const;
    // Entry points taken from the REL header (prolog, epilog, unresolved), as synthetic addresses.
    std::vector<uint32_t> headerEntries() const;
};

struct RelSet {
    std::vector<std::unique_ptr<RelModule>> modules;   // sorted by module id
    const RelModule* moduleFor(uint32_t synthAddr) const;
    const RelModule* byId(uint32_t id) const;
};

// Synthetic base used for a module id during analysis (never visible in generated code except as a label).
uint32_t relSyntheticBase(uint32_t moduleId);

// Links every module at its synthetic base, discovers its functions and collects its dynamic relocations.
// Fails (with a message) on duplicate or out-of-range ids, unresolved imports, branch-range errors, unknown
// relocation types and relocations the generator cannot express.
bool prepareRels(const DolImage& dol, const std::vector<RelImage>& rels, RelSet& out, std::string& error);

struct OsLinkEvidence {
    uint32_t osLink = 0;          // OSLink(info, bss): the only caller of Link
    uint32_t osUnlink = 0;        // OSUnlink(info): the only caller of the unlink-relocation routine
    uint32_t linkInternal = 0;    // function that calls the relocation routine holding the "OSLink:" string
    uint32_t relocateLink = 0;    // references "OSLink: unknown relocation type"
    uint32_t relocateUnlink = 0;  // references "OSUnlink: unknown relocation type"
};

// Identifies OSLink/OSUnlink in the DOL: the functions referencing the SDK's "OSLink: unknown relocation type"
// and "OSUnlink: unknown relocation type" error strings, then their unique callers in the call graph.
bool findOsLink(const Program& prog, const Discovery& d, const DolImage& dol, OsLinkEvidence& out, std::string& error);

}  // namespace awl::recomp
