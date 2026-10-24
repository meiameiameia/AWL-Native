// awl-hle-scan: evidence tool for the SDK identification in docs/research/hle-platform.md.
//   awl-hle-scan facts    --dol main.dol --rel-dir disc/files    one line per discovered function
//   awl-hle-scan rel-refs --dol main.dol --rel-dir disc/files    one line per DOL address that a REL calls or points to
// The game files are read at run time only (paths come from the arguments or AWL_RECOMP_DOL / AWL_RECOMP_REL_DIR);
// nothing derived from them is embedded in the binary.
//
// facts line: F <start> <size in words> | <hw reg>:<R|W|RW>... | priv: <supervisor kinds> | low: <lowmem addr><R|W|RW>...
//             | calls: <bl targets> | tail: <tail-call targets> | ind:<bctrl/blrl count>
// Hardware registers (0xCC000000-0xCDFFFFFF) and OS low memory (0x80000000-0x800030FF) are listed only when the
// base register holds a constant built by lis/addi/ori inside the function; accesses through pointers kept in
// variables (for example the GX library's register pointers) are invisible here and show up at run time instead.
// Volatile registers are forgotten at every call, so a pointer returned by a callee is never mistaken for a constant.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "analysis.h"
#include "image.h"
#include "ppc.h"

using namespace awl::recomp;

namespace {

const char* envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

bool isLoad(Op o) {
    switch (o) {
        case Op::lbz: case Op::lhz: case Op::lha: case Op::lwz: case Op::lfs: case Op::lfd:
        case Op::lbzu: case Op::lhzu: case Op::lwzu: return true;
        default: return false;
    }
}

bool isStore(Op o) {
    switch (o) {
        case Op::stb: case Op::sth: case Op::stw: case Op::stfs: case Op::stfd:
        case Op::stbu: case Op::sthu: case Op::stwu: return true;
        default: return false;
    }
}

std::string rwText(const std::string& seen) {
    std::string out;
    for (char c : seen) if (out.find(c) == std::string::npos) out.push_back(c);
    return out;
}

using Constants = std::map<uint32_t, std::optional<uint32_t>>;

std::optional<uint32_t> constantOf(const Constants& v, uint32_t reg) {
    const auto it = v.find(reg);
    return it == v.end() ? std::nullopt : it->second;
}

// Updates the straight-line constant state for one instruction (lis/addi/ori chains only).
void step(Constants& v, const Insn& i) {
    std::optional<uint32_t> value;
    uint32_t dst = 99;
    if (i.op == Op::addis) {
        dst = i.rD();
        if (i.rA() == 0) value = static_cast<uint32_t>(i.simm()) << 16;
        else if (const auto base = constantOf(v, i.rA())) value = *base + (static_cast<uint32_t>(i.simm()) << 16);
    } else if (i.op == Op::addi) {
        dst = i.rD();
        if (i.rA() == 0) value = static_cast<uint32_t>(i.simm());
        else if (const auto base = constantOf(v, i.rA())) value = *base + static_cast<uint32_t>(i.simm());
    } else if (i.op == Op::ori) {
        dst = i.rA();
        if (const auto src = constantOf(v, i.rS())) value = *src | i.uimm();
    }
    const bool call = (i.op == Op::b || i.op == Op::bc || i.op == Op::bcctr || i.op == Op::bclr) && i.lk();
    if (call) {
        for (uint32_t r : {0u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u}) v[r] = std::nullopt;
    }
    for (uint32_t r = 0; r < 32; ++r) {
        if (writesGpr(i, r)) v[r] = (r == dst) ? value : std::nullopt;
    }
}

void printFacts(const Program& prog, const Discovery& disc) {
    for (const auto& [start, f] : disc.funcs) {
        Constants constants;
        std::map<uint32_t, std::string> hw;
        std::map<uint32_t, std::string> lowmem;
        for (uint32_t a : f.body) {
            const Insn& i = *prog.at(a);
            if (isLoad(i.op) || isStore(i.op)) {
                std::optional<uint32_t> base = i.rA() == 0 ? std::optional<uint32_t>(0) : constantOf(constants, i.rA());
                if (base) {
                    const uint32_t ea = *base + static_cast<uint32_t>(i.simm());
                    const char rw = isStore(i.op) ? 'W' : 'R';
                    if (ea >= 0xCC000000u && ea < 0xCE000000u) hw[ea].push_back(rw);
                    else if (ea >= 0x80000000u && ea < 0x80003100u) lowmem[ea].push_back(rw);
                }
            }
            step(constants, i);
        }
        const std::set<uint32_t> calls(f.calls.begin(), f.calls.end());
        std::printf("F %08X %u |", start, static_cast<unsigned>((f.end() - start) / 4));
        for (const auto& [ea, rw] : hw) std::printf(" %08X:%s", ea, rwText(rw).c_str());
        std::printf(" | priv:");
        for (const auto& k : f.platform.privilegedKinds) std::printf(" %s,", k.c_str());
        std::printf(" | low:");
        for (const auto& [ea, rw] : lowmem) std::printf(" %04X%s", ea & 0xFFFFu, rwText(rw).c_str());
        std::printf(" | calls:");
        for (uint32_t c : calls) std::printf(" %08X", c);
        std::printf(" | tail:");
        for (uint32_t c : f.tailCalls) std::printf(" %08X", c);
        std::printf(" | ind:%u\n", f.indirectCalls);
    }
}

constexpr uint8_t kRelocRel24 = 10;   // R_PPC_REL24, see relocTypeName in image.cpp

void printRelRefs(const std::vector<RelImage>& rels) {
    struct Counts { unsigned calls = 0, other = 0; };
    std::map<uint32_t, std::map<size_t, Counts>> refs;
    for (size_t m = 0; m < rels.size(); ++m) {
        for (const auto& imp : rels[m].imports) {
            if (imp.moduleId != 0) continue;
            for (const auto& rl : imp.relocs) {
                Counts& c = refs[rl.addend][m];
                if (rl.type == kRelocRel24) ++c.calls; else ++c.other;
            }
        }
    }
    for (const auto& [target, perRel] : refs) {
        std::printf("R %08X", target);
        for (const auto& [m, c] : perRel) std::printf(" wlp%zu:%u/%u", m, c.calls, c.other);
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string command = argc > 1 ? argv[1] : "";
    std::string dolPath = envOr("AWL_RECOMP_DOL", "");
    std::string relDir = envOr("AWL_RECOMP_REL_DIR", "");
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--rel-dir" && i + 1 < argc) relDir = argv[++i];
    }
    if ((command != "facts" && command != "rel-refs") || dolPath.empty() || relDir.empty()) {
        std::fprintf(stderr, "usage: awl-hle-scan facts|rel-refs --dol <main.dol> --rel-dir <dir with wlp*.rel>\n");
        return 2;
    }
    DolImage dol;
    std::string err;
    if (!loadDol(dolPath, dol, err)) {
        std::fprintf(stderr, "DOL: %s\n", err.c_str());
        return 1;
    }
    std::vector<RelImage> rels;
    for (const char* n : {"wlp0.rel", "wlp1.rel", "wlp2.rel"}) {
        RelImage r;
        if (!loadRel(relDir + "/" + n, r, err)) {
            std::fprintf(stderr, "%s: %s\n", n, err.c_str());
            return 1;
        }
        rels.push_back(std::move(r));
    }
    if (command == "rel-refs") {
        printRelRefs(rels);
        return 0;
    }
    DiscoveryInput in;
    for (const auto& r : rels) {
        for (const auto& imp : r.imports) {
            if (imp.moduleId != 0) continue;
            for (const auto& rl : imp.relocs) {
                if (dol.isText(rl.addend) && (rl.addend & 3) == 0) in.relTargets.push_back(rl.addend);
            }
        }
    }
    const Program prog = buildProgram(dol);
    const Discovery disc = discoverFunctions(prog, in);
    printFacts(prog, disc);
    return 0;
}
