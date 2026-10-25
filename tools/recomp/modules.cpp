#include "modules.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <set>

#include "runtime/guest.h"

namespace awl::recomp {

static_assert(kMaxRelModules == rt::kMaxRelModules && kMaxRelSections == rt::kMaxRelSections, "generator and runtime module limits must agree");

namespace {

constexpr uint32_t kSyntheticStride = 0x00400000;
constexpr uint32_t kSyntheticFirst = 0x80400000;

bool isHaLoType(uint8_t t) { return t >= 3 && t <= 6; }

}  // namespace

uint32_t relSyntheticBase(uint32_t moduleId) { return kSyntheticFirst + (moduleId - 1) * kSyntheticStride; }

bool RelModule::locate(uint32_t addr, uint32_t& section, uint32_t& offset) const {
    for (size_t i = 0; i < layout.sectionAddr.size(); ++i) {
        const uint32_t base = layout.sectionAddr[i];
        if (base == 0 || addr < base || addr - base >= rel->sections[i].length) continue;
        section = static_cast<uint32_t>(i);
        offset = addr - base;
        return true;
    }
    return false;
}

std::vector<uint32_t> RelModule::headerEntries() const {
    std::vector<uint32_t> e;
    if (rel->prolog) e.push_back(layout.sectionAddr[rel->prologSection] + rel->prolog);
    if (rel->epilog) e.push_back(layout.sectionAddr[rel->epilogSection] + rel->epilog);
    if (rel->unresolved) e.push_back(layout.sectionAddr[rel->unresolvedSection] + rel->unresolved);
    return e;
}

const RelModule* RelSet::moduleFor(uint32_t synthAddr) const {
    for (const auto& m : modules) {
        if (synthAddr >= m->layout.base && synthAddr < m->layout.end) return m.get();
    }
    return nullptr;
}

const RelModule* RelSet::byId(uint32_t id) const {
    for (const auto& m : modules) if (m->rel->id == id) return m.get();
    return nullptr;
}

bool prepareRels(const DolImage& dol, const std::vector<RelImage>& rels, RelSet& out, std::string& error) {
    out.modules.clear();
    std::set<uint32_t> ids;
    std::vector<const RelImage*> sorted;
    for (const RelImage& r : rels) {
        if (r.id == 0 || r.id >= kMaxRelModules) {
            error = r.name + ": module id " + std::to_string(r.id) + " is outside 1.." + std::to_string(kMaxRelModules - 1);
            return false;
        }
        if (r.sections.size() > kMaxRelSections) {
            error = r.name + ": " + std::to_string(r.sections.size()) + " sections exceed the supported " + std::to_string(kMaxRelSections);
            return false;
        }
        if (!ids.insert(r.id).second) {
            error = r.name + ": duplicate module id " + std::to_string(r.id);
            return false;
        }
        sorted.push_back(&r);
    }
    std::sort(sorted.begin(), sorted.end(), [](const RelImage* a, const RelImage* b) { return a->id < b->id; });

    std::vector<RelLayout> layouts;
    for (const RelImage* r : sorted) {
        layouts.push_back(layoutRel(*r, relSyntheticBase(r->id)));
        if (layouts.back().end - layouts.back().base > kSyntheticStride) {
            error = r->name + ": module image larger than the synthetic stride";
            return false;
        }
    }
    for (size_t k = 0; k < sorted.size(); ++k) {
        const RelImage& r = *sorted[k];
        auto m = std::make_unique<RelModule>();
        m->rel = &r;
        m->layout = layouts[k];
        m->img = std::make_unique<DolImage>();
        RelLinkStats st;
        if (!linkRel(r, layouts, dol, *m->img, st, error)) {
            error = r.name + ": " + error;
            return false;
        }
        if (st.unresolved || st.rangeErrors || st.unknownTypes) {
            error = r.name + ": linking at the synthetic layout left " + std::to_string(st.unresolved) + " unresolved relocations, " +
                    std::to_string(st.rangeErrors) + " branch range errors and " + std::to_string(st.unknownTypes) + " unknown relocation types";
            return false;
        }
        // Relocations whose value depends on a module's load address: the immediate of an address-forming
        // instruction. The halfword relocated is the second half of its instruction word.
        for (const auto& imp : r.imports) {
            for (const RelReloc& rl : imp.relocs) {
                if (imp.moduleId == 0) continue;   // absolute DOL address: already exact in the analysed image
                if (rl.type == 1 || rl.type == 10) continue;   // ADDR32 patches data in guest memory; REL24 becomes a direct call
                if (!isHaLoType(rl.type)) {
                    error = r.name + ": relocation type " + std::to_string(rl.type) + " against module " + std::to_string(imp.moduleId) +
                            " in section " + std::to_string(rl.fromSection) + " is not supported by the generator";
                    return false;
                }
                const uint32_t insnAddr = m->layout.sectionAddr[rl.fromSection] + (rl.fromOffset & ~3u);
                if (!r.sections[rl.fromSection].exec) continue;   // address halves in data are patched by OSLink in memory
                DynReloc d;
                d.type = rl.type;
                d.module = imp.moduleId;
                d.section = rl.section;
                d.addend = rl.addend;
                m->dyn[insnAddr] = d;
            }
        }
        m->prog = std::make_unique<Program>(buildProgram(*m->img));
        DiscoveryInput in;
        in.extraEntries = m->headerEntries();
        m->disc = discoverFunctions(*m->prog, in);
        out.modules.push_back(std::move(m));
    }
    return true;
}

namespace {

bool findString(const DolImage& dol, const char* text, uint32_t& addr) {
    const size_t n = std::strlen(text);
    for (const DolSection& s : dol.sections) {
        if (s.text) continue;
        const uint8_t* p = dol.file.data() + s.fileOffset;
        for (uint32_t off = 0; off + n < s.size; ++off) {
            if (std::memcmp(p + off, text, n) != 0) continue;
            if (off != 0 && p[off - 1] != 0) continue;   // must start a string
            addr = s.addr + off;
            return true;
        }
    }
    return false;
}

// Functions whose body forms the constant `target` in a register (addis/addi/ori from r0, in program order).
std::vector<uint32_t> functionsForming(const Program& prog, const Discovery& d, uint32_t target) {
    std::vector<uint32_t> hits;
    for (const auto& [start, f] : d.funcs) {
        std::array<std::optional<uint32_t>, 32> k;
        bool found = false;
        for (uint32_t a : f.body) {
            const Insn& i = *prog.at(a);
            std::optional<uint32_t> val;
            uint32_t dest = 0;
            auto src = [&](uint32_t reg) -> std::optional<uint32_t> {
                if (reg == 0) return 0u;
                return k[reg];
            };
            if (i.op == Op::addis) { if (auto s = src(i.rA())) { val = *s + (static_cast<uint32_t>(i.simm()) << 16); dest = i.rD(); } }
            else if (i.op == Op::addi) { if (auto s = src(i.rA())) { val = *s + static_cast<uint32_t>(i.simm()); dest = i.rD(); } }
            else if (i.op == Op::ori) { if (auto s = k[i.rS()]) { val = *s | i.uimm(); dest = i.rA(); } }
            for (uint32_t r = 0; r < 32; ++r) if (writesGpr(i, r)) k[r].reset();
            if (val) {
                k[dest] = *val;
                if (*val == target) found = true;
            }
        }
        if (found) hits.push_back(start);
    }
    return hits;
}

std::vector<uint32_t> callersOf(const Discovery& d, uint32_t callee) {
    std::vector<uint32_t> out;
    for (const auto& [start, f] : d.funcs) {
        if (std::find(f.calls.begin(), f.calls.end(), callee) != f.calls.end() ||
            std::find(f.tailCalls.begin(), f.tailCalls.end(), callee) != f.tailCalls.end())
            out.push_back(start);
    }
    return out;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

}  // namespace

bool findOsLink(const Program& prog, const Discovery& d, const DolImage& dol, OsLinkEvidence& out, std::string& error) {
    uint32_t linkStr = 0, unlinkStr = 0;
    if (!findString(dol, "OSLink: unknown relocation type", linkStr) || !findString(dol, "OSUnlink: unknown relocation type", unlinkStr)) {
        error = "the SDK's \"OSLink: unknown relocation type\" / \"OSUnlink: unknown relocation type\" strings were not found in the DOL";
        return false;
    }
    const auto relocLink = functionsForming(prog, d, linkStr);
    const auto relocUnlink = functionsForming(prog, d, unlinkStr);
    if (relocLink.size() != 1 || relocUnlink.size() != 1) {
        error = "expected exactly one function referencing each OSLink/OSUnlink error string, found " + std::to_string(relocLink.size()) +
                " and " + std::to_string(relocUnlink.size());
        return false;
    }
    out.relocateLink = relocLink[0];
    out.relocateUnlink = relocUnlink[0];
    const auto linkers = callersOf(d, out.relocateLink);
    if (linkers.size() != 1) {
        error = "expected exactly one caller of the OSLink relocation routine " + hex(out.relocateLink) + ", found " + std::to_string(linkers.size());
        return false;
    }
    out.linkInternal = linkers[0];
    const auto osLinks = callersOf(d, out.linkInternal);
    if (osLinks.size() != 1) {
        error = "expected exactly one caller of the SDK's Link routine " + hex(out.linkInternal) + " (OSLink), found " + std::to_string(osLinks.size());
        return false;
    }
    out.osLink = osLinks[0];
    const auto unlinkers = callersOf(d, out.relocateUnlink);
    if (unlinkers.size() != 1) {
        error = "expected exactly one caller of the OSUnlink relocation routine " + hex(out.relocateUnlink) + " (OSUnlink), found " + std::to_string(unlinkers.size());
        return false;
    }
    out.osUnlink = unlinkers[0];
    return true;
}

}  // namespace awl::recomp
