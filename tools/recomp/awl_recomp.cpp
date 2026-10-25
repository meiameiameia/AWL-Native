// awl-recomp: static-recompilation feasibility tool.
//   awl-recomp <command> [--dol <main.dol>] [--rel-dir <dir with wlp*.rel>] [options]
// The game files are read at run time only; paths come from arguments or AWL_RECOMP_DOL / AWL_RECOMP_REL_DIR.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "analysis.h"
#include "codegen.h"
#include "image.h"
#include "manifest.h"
#include "modules.h"
#include "ppc.h"

using namespace awl::recomp;

namespace {

struct Options {
    std::string command;
    std::string dol;
    std::string relDir;
    std::string out = "build/recomp-gen";
    std::string manifest = "tools/recomp/hle_manifest.txt";
    std::string target;
    uint32_t addr = 0;
    uint32_t count = 16;
    bool full = false;
    uint32_t filesPerChunk = 256;
};

const char* envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

bool parseArgs(int argc, char** argv, Options& o) {
    o.dol = envOr("AWL_RECOMP_DOL", "");
    o.relDir = envOr("AWL_RECOMP_REL_DIR", "");
    if (argc < 2) return false;
    o.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (a == "--dol") next(o.dol);
        else if (a == "--rel-dir") next(o.relDir);
        else if (a == "--out") next(o.out);
        else if (a == "--hle-manifest") next(o.manifest);
        else if (a == "--full") o.full = true;
        else if (a == "--addr" && i + 1 < argc) o.addr = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--count" && i + 1 < argc) o.count = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
    }
    return true;
}

void usage() {
    std::fprintf(stderr,
        "usage: awl-recomp <command> [--dol main.dol] [--rel-dir dir] [options]\n"
        "  info       DOL sections and REL headers, import and relocation statistics\n"
        "  decode     decode every text word; per-mnemonic counts and undecodable words\n"
        "  rel        link the RELs at fixed bases, decode and discover functions in them\n"
        "  discover   function discovery, coverage and indirect-branch classification\n"
        "  disasm     --addr A [--count N]  disassemble text\n"
        "  gen        --out DIR [--full] [--hle-manifest FILE]   emit C++ for the boot path (or the whole DOL with --full);\n"
        "             manifest defaults to tools/recomp/hle_manifest.txt\n");
}

// Loads the game's modules from the directory: every wlp0/1/2.rel that exists (a partial set is allowed so a
// synthetic module can be tested); an existing file that fails to parse, or a directory with none, is an error.
bool loadRels(const Options& o, std::vector<RelImage>& rels) {
    if (o.relDir.empty()) return true;
    for (const char* n : {"wlp0.rel", "wlp1.rel", "wlp2.rel"}) {
        const std::string path = o.relDir + "/" + n;
        if (!std::ifstream(path, std::ios::binary)) continue;
        RelImage r;
        std::string err;
        if (!loadRel(path, r, err)) {
            std::fprintf(stderr, "%s: %s\n", n, err.c_str());
            return false;
        }
        r.name = n;
        rels.push_back(std::move(r));
    }
    if (rels.empty()) {
        std::fprintf(stderr, "no wlp0.rel, wlp1.rel or wlp2.rel found in %s\n", o.relDir.c_str());
        return false;
    }
    return true;
}

// Reads and parses the HLE manifest; prints every problem and returns false when it cannot be used.
bool loadManifest(const std::string& path, std::vector<HleEntry>& hle) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open HLE manifest %s (run from the repository root or pass --hle-manifest)\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::vector<std::string> errors;
    if (!parseHleManifest(ss.str(), path, hle, errors)) {
        for (const auto& e : errors) std::fprintf(stderr, "%s\n", e.c_str());
        return false;
    }
    return true;
}

std::vector<uint32_t> relTargetsIntoDol(const DolImage& dol, const std::vector<RelImage>& rels) {
    std::set<uint32_t> out;
    for (const auto& r : rels) {
        for (const auto& imp : r.imports) {
            if (imp.moduleId != 0) continue;
            for (const auto& rl : imp.relocs) {
                if (dol.isText(rl.addend) && (rl.addend & 3) == 0) out.insert(rl.addend);
            }
        }
    }
    return {out.begin(), out.end()};
}

int cmdInfo(const DolImage& dol, const std::vector<RelImage>& rels) {
    std::printf("DOL: entry 0x%08x, bss 0x%08x size %u\n", dol.entry, dol.bssAddr, dol.bssSize);
    for (const auto& s : dol.sections) {
        std::printf("  %s%-2d  addr 0x%08x  size %8u  file 0x%06x\n", s.text ? "text" : "data", s.text ? s.index : s.index - 7,
                    s.addr, s.size, s.fileOffset);
    }
    for (const auto& r : rels) {
        std::printf("\nREL %s: module id %u, version %u, %u sections, bss %u (align %u), file %zu bytes\n", r.name.c_str(),
                    r.id, r.version, r.numSections, r.bssSize, r.bssAlign, r.file.size());
        std::printf("  prolog sec %u +0x%x, epilog sec %u +0x%x, unresolved sec %u +0x%x, align %u, fixSize %u\n",
                    r.prologSection, r.prolog, r.epilogSection, r.epilog, r.unresolvedSection, r.unresolved, r.align, r.fixSize);
        for (size_t i = 0; i < r.sections.size(); ++i) {
            const auto& s = r.sections[i];
            if (s.length == 0 && s.offset == 0) continue;
            std::printf("  section %2zu  %-5s  file 0x%06x  length %8u\n", i, s.bss() ? "bss" : (s.exec ? "text" : "data"), s.offset, s.length);
        }
        std::map<std::string, size_t> typeCount;
        for (const auto& imp : r.imports) {
            std::printf("  import module %u: %zu relocations\n", imp.moduleId, imp.relocs.size());
            for (const auto& rl : imp.relocs) ++typeCount[std::string(relocTypeName(rl.type)) + (imp.moduleId == 0 ? " (->DOL)" : imp.moduleId == r.id ? " (->self)" : " (->other REL)")];
        }
        std::printf("  control records: SECTION %zu, NOP %zu, MRKREF %zu, END %zu\n", r.sectionRecords, r.nopRecords, r.markRecords, r.endRecords);
        for (const auto& [k, v] : typeCount) std::printf("    %-34s %zu\n", k.c_str(), v);
    }
    return 0;
}

int cmdDecode(const Program& prog) {
    std::map<std::string, size_t> counts, variants;
    std::vector<uint32_t> bad;
    size_t total = 0, ps = 0, psq = 0, bl = 0, bctrl = 0, bctr = 0, blrl = 0;
    for (const auto& seg : prog.segs) {
        for (const Insn& i : seg.insns) {
            ++total;
            if (i.op == Op::invalid) { bad.push_back(i.addr); continue; }
            ++counts[opName(i.op)];
            ++variants[mnemonic(i)];
            const std::string n = opName(i.op);
            if (n.rfind("ps_", 0) == 0) ++ps;
            if (n.rfind("psq_", 0) == 0) ++psq;
            if (i.op == Op::b && i.lk()) ++bl;
            if (i.op == Op::bcctr && i.lk()) ++bctrl;
            if (i.op == Op::bcctr && !i.lk()) ++bctr;
            if (i.op == Op::bclr && i.lk()) ++blrl;
        }
    }
    std::printf("text words: %zu, decoded: %zu, undecodable: %zu\n", total, total - bad.size(), bad.size());
    std::printf("paired-single ALU/merge/cmp ops (ps_*): %zu, psq load/store: %zu, bl: %zu, bctrl: %zu, bctr/bcctr(no link): %zu, blrl: %zu\n",
                ps, psq, bl, bctrl, bctr, blrl);
    std::printf("distinct base mnemonics: %zu, distinct full mnemonics (with ./o/l/a): %zu\n", counts.size(), variants.size());
    std::vector<std::pair<std::string, size_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::printf("\nper-mnemonic counts (base mnemonic):\n");
    for (const auto& [n, c] : v) std::printf("  %-12s %zu\n", n.c_str(), c);
    // Undecodable words: split all-zero padding from other words, then group into runs.
    std::vector<uint32_t> zeros, others;
    for (uint32_t a : bad) (prog.at(a)->raw == 0 ? zeros : others).push_back(a);
    std::printf("\nundecodable words: %zu all-zero (padding/data-in-text), %zu other\n", zeros.size(), others.size());
    size_t shown = 0, runs = 0;
    for (size_t i = 0; i < others.size();) {
        size_t j = i;
        while (j + 1 < others.size() && others[j + 1] == others[j] + 4) ++j;
        ++runs;
        if (shown++ < 40) {
            std::printf("  0x%08x..0x%08x (%zu words) first word 0x%08x\n", others[i], others[j], j - i + 1, prog.at(others[i])->raw);
        }
        i = j + 1;
    }
    std::printf("  %zu runs of non-zero undecodable words\n", runs);
    return 0;
}

void summarize(const char* label, const Program& prog, const Discovery& d) {
    size_t zero = 0, other = 0, ps = 0, psq = 0, bl = 0, bctrl = 0, words = 0;
    for (const auto& seg : prog.segs) {
        for (const Insn& i : seg.insns) {
            ++words;
            if (i.op == Op::invalid) { (i.raw == 0 ? zero : other)++; continue; }
            const std::string n = opName(i.op);
            if (n.rfind("ps_", 0) == 0) ++ps;
            if (n.rfind("psq_", 0) == 0) ++psq;
            if (i.op == Op::b && i.lk()) ++bl;
            if (i.op == Op::bcctr && i.lk()) ++bctrl;
        }
    }
    std::map<IndirectKind, size_t> kinds;
    for (const auto& j : d.bctrUnique) ++kinds[j.kind];
    std::printf("%s: %zu text words, undecodable %zu zero + %zu other, ps_* %zu, psq %zu, bl %zu, bctrl %zu\n", label, words, zero, other, ps, psq, bl, bctrl);
    std::printf("    functions %zu, assigned %.2f%% of %zu text bytes, padding %zu B, unassigned %zu B, shared words %zu\n", d.stats.functions,
                100.0 * d.stats.assignedBytes / d.stats.textBytes, d.stats.textBytes, d.stats.paddingBytes, d.stats.unassignedBytes, d.stats.overlapWords);
    std::printf("    bctr %zu:", d.bctrUnique.size());
    for (const auto& [k, v] : kinds) std::printf("  %s=%zu", indirectKindName(k), v);
    std::printf("\n");
}

int cmdRel(const DolImage& dol, const std::vector<RelImage>& rels) {
    const uint32_t bases[3] = {0x80400000, 0x80800000, 0x80C00000};
    const Program dolProg = buildProgram(dol);
    DiscoveryInput dolIn;
    dolIn.relTargets = relTargetsIntoDol(dol, rels);
    const Discovery dolDisc = discoverFunctions(dolProg, dolIn);
    std::vector<RelLayout> layouts;
    for (size_t i = 0; i < rels.size(); ++i) layouts.push_back(layoutRel(rels[i], bases[i]));
    for (size_t i = 0; i < rels.size(); ++i) {
        const RelImage& r = rels[i];
        DolImage img;
        RelLinkStats st;
        std::string err;
        if (!linkRel(r, layouts, dol, img, st, err)) { std::fprintf(stderr, "%s: %s\n", r.name.c_str(), err.c_str()); return 1; }
        std::printf("\n%s linked at 0x%08x..0x%08x: %zu relocations applied (-> DOL text %zu, DOL data %zu, DOL other %zu, self %zu, other REL %zu), unresolved %zu, branch range errors %zu\n",
                    r.name.c_str(), layouts[i].base, layouts[i].end, st.applied, st.toDolText, st.toDolData, st.toDolOther, st.toSelf, st.toOtherRel,
                    st.unresolved, st.rangeErrors);
        // Relocated branch targets into the DOL must land on discovered function starts.
        std::set<uint32_t> targets;
        size_t miss = 0;
        for (const auto& imp : r.imports) {
            if (imp.moduleId != 0) continue;
            for (const auto& rl : imp.relocs) {
                if (rl.type != 10) continue;   // REL24 calls into the DOL
                targets.insert(rl.addend);
                if (!dolDisc.funcs.count(rl.addend)) ++miss;
            }
        }
        std::printf("    REL24 call targets in the DOL: %zu distinct; %zu relocations whose target is not a discovered function start\n", targets.size(), miss);
        const Program prog = buildProgram(img);
        DiscoveryInput in;
        if (r.prolog) in.extraEntries.push_back(layouts[i].sectionAddr[r.prologSection] + r.prolog);
        if (r.epilog) in.extraEntries.push_back(layouts[i].sectionAddr[r.epilogSection] + r.epilog);
        if (r.unresolved) in.extraEntries.push_back(layouts[i].sectionAddr[r.unresolvedSection] + r.unresolved);
        const Discovery d = discoverFunctions(prog, in);
        summarize(r.name.c_str(), prog, d);
    }
    return 0;
}

int cmdDisasm(const Program& prog, const Options& o) {
    for (uint32_t k = 0; k < o.count; ++k) {
        const Insn* i = prog.at(o.addr + 4 * k);
        if (!i) break;
        std::printf("%08x  %08x  %s\n", i->addr, i->raw, disassemble(*i).c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o) || o.dol.empty()) {
        usage();
        return 2;
    }
    DolImage dol;
    std::string err;
    if (!loadDol(o.dol, dol, err)) {
        std::fprintf(stderr, "DOL: %s\n", err.c_str());
        return 1;
    }
    std::vector<RelImage> rels;
    if (!loadRels(o, rels)) return 1;

    if (o.command == "info") return cmdInfo(dol, rels);
    const Program prog = buildProgram(dol);
    if (o.command == "decode") return cmdDecode(prog);
    if (o.command == "rel") return cmdRel(dol, rels);
    if (o.command == "disasm") return cmdDisasm(prog, o);
    if (o.command == "classify") {
        const IndirectJump j = classifyBctr(prog, o.count ? o.count : o.addr - 0x100, o.addr);
        std::printf("%s table 0x%08x count %u const 0x%08x\n",indirectKindName(j.kind), j.table, j.count, j.constTarget);
        return 0;
    }
    if (o.command == "xref") {
        for (const auto& seg : prog.segs)
            for (const Insn& i : seg.insns)
                if ((i.op == Op::b || i.op == Op::bc) && i.branchTarget() == o.addr)
                    std::printf("%08x  %s\n", i.addr, disassemble(i).c_str());
        for (const auto& sec : dol.sections) {
            if (sec.text) continue;
            for (uint32_t off = 0; off + 4 <= sec.size; off += 4)
                if (be32(&dol.file[sec.fileOffset + off]) == o.addr) std::printf("data %08x\n", sec.addr + off);
        }
        return 0;
    }
    if (o.command == "words") {
        for (uint32_t k = 0; k < o.count; ++k) {
            uint32_t w = 0;
            if (!dol.readWord(o.addr + 4 * k, w)) break;
            std::printf("%08x  %08x\n", o.addr + 4 * k, w);
        }
        return 0;
    }
    if (o.command == "discover" || o.command == "gen") {
        std::vector<HleEntry> hle;
        if (o.command == "gen" && !loadManifest(o.manifest, hle)) return 1;
        DiscoveryInput in;
        in.relTargets = relTargetsIntoDol(dol, rels);
        Discovery d = discoverFunctions(prog, in);
        if (o.command == "discover") return reportDiscovery(prog, d, rels, in);
        std::vector<std::string> errors;
        if (!validateHleManifest(hle, d, o.manifest, errors)) {
            for (const auto& e : errors) std::fprintf(stderr, "%s\n", e.c_str());
            return 1;
        }
        // With the whole DOL, the modules are generated too: each is linked at a synthetic layout for analysis, and
        // the SDK's OSLink/OSUnlink are identified from evidence so their wrappers can register the module code.
        RelSet relSet;
        OsLinkEvidence os;
        const bool withRels = o.full && !rels.empty();
        if (withRels) {
            if (!prepareRels(dol, rels, relSet, err)) { std::fprintf(stderr, "REL: %s\n", err.c_str()); return 1; }
            if (!findOsLink(prog, d, dol, os, err)) { std::fprintf(stderr, "OSLink: %s\n", err.c_str()); return 1; }
        }
        return generateCode(prog, d, hle, withRels ? &relSet : nullptr, withRels ? &os : nullptr, o.out, o.full);
    }
    usage();
    return 2;
}
