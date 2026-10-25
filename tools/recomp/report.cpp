#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "codegen.h"
#include "resolve.h"

namespace awl::recomp {

namespace {

std::set<uint32_t> reachableFromEntry(const Program& prog, const Discovery& d) {
    std::set<uint32_t> seen;
    std::vector<uint32_t> work{prog.dol->entry};
    while (!work.empty()) {
        const uint32_t s = work.back();
        work.pop_back();
        if (!seen.insert(s).second) continue;
        auto it = d.funcs.find(s);
        if (it == d.funcs.end()) continue;
        for (uint32_t c : it->second.calls) work.push_back(c);
        for (uint32_t c : it->second.tailCalls) work.push_back(c);
        if (it->second.fallsInto) work.push_back(it->second.fallsInto);
    }
    return seen;
}

}  // namespace

int reportDiscovery(const Program& prog, const Discovery& d, const std::vector<RelImage>& rels, const DiscoveryInput& in) {
    const DiscoveryStats& s = d.stats;
    std::printf("== function discovery ==\n");
    std::printf("text bytes: %zu (%zu words)\n", s.textBytes, s.textBytes / 4);
    std::printf("functions: %zu\n", s.functions);
    std::printf("bytes assigned to functions: %zu (%.2f%%)\n", s.assignedBytes, 100.0 * s.assignedBytes / s.textBytes);
    std::printf("padding (nop/zero) outside functions: %zu bytes (%.2f%%)\n", s.paddingBytes, 100.0 * s.paddingBytes / s.textBytes);
    std::printf("unassigned non-padding bytes: %zu (%.2f%%) in %zu ranges\n", s.unassignedBytes, 100.0 * s.unassignedBytes / s.textBytes,
                d.unassignedRanges.size());
    std::printf("words owned by more than one function: %zu; seeds landing inside another body: %zu\n", s.overlapWords, d.ambiguousSeeds.size());
    std::printf("functions by seed kind (a function may have several):\n");
    for (const auto& [k, v] : s.bySeed) std::printf("  %-14s %zu\n", k.c_str(), v);
    std::printf("data-section text pointers: %zu distinct; already-known/accepted %zu; inside an existing body (case labels etc.) %zu; rejected %zu\n",
                s.dataCandidates, s.dataAccepted, s.dataInsideBody, s.dataRejected);
    std::printf("code-constant text addresses (lis+addi/ori): %zu distinct; accepted as new function starts: %zu\n", s.constCandidates, s.constAccepted);
    std::printf("tail-call edges: %zu, fall-through entry chains: %zu, gap-heuristic seeds: %zu\n", s.tailCallEdges, s.fallThroughEdges, s.gapSeeds);
    size_t relFound = 0, relIndependent = 0;
    for (uint32_t t : in.relTargets) {
        auto it = d.funcs.find(t);
        if (it == d.funcs.end()) continue;
        ++relFound;
        if (it->second.seeds & (SeedBl | SeedData | SeedConst)) ++relIndependent;
    }
    std::printf("REL references into DOL text: %zu distinct addresses; %zu are discovered function starts, %zu of those also found independently from bl/data/code constants\n", in.relTargets.size(), relFound, relIndependent);
    (void)rels;

    std::printf("\nlargest unassigned ranges:\n");
    auto ranges = d.unassignedRanges;
    std::sort(ranges.begin(), ranges.end(), [](auto& a, auto& b) { return a.second - a.first > b.second - b.first; });
    for (size_t i = 0; i < ranges.size() && i < 12; ++i)
        std::printf("  0x%08x..0x%08x (%u bytes)\n", ranges[i].first, ranges[i].second, ranges[i].second - ranges[i].first);
    std::printf("overlap samples (word, owner already):\n");
    for (size_t i = 0; i < d.overlaps.size() && i < 8; ++i) std::printf("  0x%08x owned by 0x%08x\n", d.overlaps[i].first, d.overlaps[i].second);
    {
        std::vector<std::pair<size_t, std::pair<uint32_t, uint32_t>>> top;
        for (const auto& [k, v] : d.overlapPairs) top.push_back({v, k});
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::printf("function pairs sharing words: %zu; largest:", top.size());
        for (size_t i = 0; i < top.size() && i < 10; ++i) std::printf(" (0x%08x,0x%08x):%zu", top[i].second.first, top[i].second.second, top[i].first);
        std::printf("\n");
    }
    {
        std::map<std::string, std::pair<size_t, size_t>> bySeed;   // seed class -> (functions, shared words)
        for (const auto& [a, f] : d.funcs) {
            if (!f.sharedWords) continue;
            const char* cls = (f.seeds & (SeedEntry | SeedBl | SeedRel)) ? "strong (entry/bl/rel)" : (f.seeds & SeedData) ? "data-pointer" : "gap-heuristic/tail";
            ++bySeed[cls].first;
            bySeed[cls].second += f.sharedWords;
        }
        for (const auto& [k, v] : bySeed) std::printf("functions sharing words by seed class: %-24s %zu functions, %zu words\n", k.c_str(), v.first, v.second);
    }
    std::printf("seeds inside another body (first 8):");
    for (size_t i = 0; i < d.ambiguousSeeds.size() && i < 8; ++i) std::printf(" 0x%08x", d.ambiguousSeeds[i]);
    std::printf("\n");
    std::printf("undecodable words inside function bodies: %zu, outside: %zu\n", d.badDecodeAddrsInside.size(), d.badDecodeAddrsOutside.size());
    for (size_t i = 0; i < d.badDecodeAddrsInside.size() && i < 12; ++i)
        std::printf("  inside 0x%08x raw 0x%08x\n", d.badDecodeAddrsInside[i], prog.at(d.badDecodeAddrsInside[i])->raw);

    std::printf("\n== indirect control flow ==\n");
    std::map<IndirectKind, std::vector<const IndirectJump*>> byKind;
    for (const auto& j : d.bctrUnique) byKind[j.kind].push_back(&j);
    size_t totalBctr = d.bctrUnique.size();
    size_t resolved = 0;
    for (const auto& [k, v] : byKind) {
        std::printf("  %-34s %zu   e.g.", indirectKindName(k), v.size());
        for (size_t i = 0; i < v.size() && i < 4; ++i) std::printf(" 0x%08x", v[i]->addr);
        std::printf("\n");
        if (k != IndirectKind::Unresolved && k != IndirectKind::Conditional) resolved += v.size();
    }
    std::printf("  bctr (non-linking) words in text: %zu; resolved (table/const/tail): %zu; unresolved or conditional: %zu\n",
                totalBctr, resolved, totalBctr - resolved);
    size_t totalTableEntries = 0;
    for (const auto& j : d.bctrUnique) totalTableEntries += j.targets.size();
    std::printf("  jump-table entries resolved: %zu; bctr words outside every discovered function: %zu\n", totalTableEntries, d.bctrUnreached);
    std::printf("  unresolved bctr detail:\n");
    for (const auto& j : d.bctrUnique) {
        if (j.kind != IndirectKind::Unresolved && j.kind != IndirectKind::Conditional) continue;
        std::printf("    0x%08x (function 0x%08x)  %s\n", j.addr, j.funcStart, indirectKindName(j.kind));
    }
    std::printf("  functions containing undecodable words (data-in-text suspects): %zu\n", d.suspectData.size());
    for (size_t i = 0; i < d.suspectData.size() && i < 6; ++i) std::printf("    0x%08x\n", d.suspectData[i]);
    std::printf("  bctrl/blrl (indirect calls, runtime lookup): %zu\n", d.totalBctrl);

    std::printf("\n== platform evidence (functions with hardware-touching instructions) ==\n");
    size_t priv = 0, mmio = 0, either = 0;
    std::map<std::string, size_t> block;
    for (const auto& [a, f] : d.funcs) {
        if (f.platform.privileged) ++priv;
        if (!f.platform.mmio.empty()) ++mmio;
        if (f.platform.any()) ++either;
        for (const auto& [b, n] : f.platform.mmio) { (void)n; ++block[b]; }
    }
    std::printf("  functions with unmodelled privileged instructions (rfi/sc): %zu; with constant MMIO addressing: %zu; either: %zu\n", priv, mmio, either);
    for (const auto& [b, n] : block) std::printf("    MMIO block %-12s %zu functions\n", b.c_str(), n);

    const auto reach = reachableFromEntry(prog, d);
    size_t reachBytes = 0;
    for (uint32_t a : reach) { auto it = d.funcs.find(a); if (it != d.funcs.end()) reachBytes += it->second.body.size() * 4; }
    std::printf("\n== static call graph from entry ==\n  functions reachable via bl/b/fall-through: %zu (%zu bytes)\n", reach.size(), reachBytes);
    return 0;
}

}  // namespace awl::recomp

namespace awl::recomp {

// Classifies every function whose body shares words with another: who owns each shared word, whether the function
// is wholly inside another body, which seeds and inbound references it has.
int reportOverlaps(const Program& prog, const Discovery& d) {
    (void)prog;
    std::map<uint32_t, std::vector<uint32_t>> owners;   // word -> function starts containing it
    for (const auto& [s, f] : d.funcs) for (uint32_t a : f.body) owners[a].push_back(s);
    std::map<uint32_t, std::vector<uint32_t>> bl, tail, fall;   // callee -> callers
    for (const auto& [s, f] : d.funcs) {
        for (uint32_t t : f.calls) bl[t].push_back(s);
        for (uint32_t t : f.tailCalls) tail[t].push_back(s);
        if (f.fallsInto) fall[f.fallsInto].push_back(s);
    }
    struct Row { uint32_t start; size_t words, shared; bool startShared, contained; };
    std::vector<Row> rows;
    for (const auto& [s, f] : d.funcs) {
        size_t shared = 0;
        for (uint32_t a : f.body) if (owners[a].size() > 1) ++shared;
        if (!shared) continue;
        rows.push_back({s, f.body.size(), shared, owners[s].size() > 1, shared == f.body.size()});
    }
    std::printf("functions with shared words: %zu\n", rows.size());
    const char* names[] = {"entry", "bl", "rel", "data", "tail", "gap", "fall", "jtab", "const"};
    std::map<std::string, std::pair<size_t, size_t>> classes;   // class -> (functions, words)
    for (const Row& r : rows) {
        const Function& f = d.funcs.at(r.start);
        const bool hasBl = bl.count(r.start) && !bl[r.start].empty();
        const bool hasRel = (f.seeds & SeedRel) != 0;
        const bool hasTail = tail.count(r.start) != 0;
        const bool hasFall = fall.count(r.start) != 0;
        std::string cls = std::string(r.contained ? "contained" : r.startShared ? "start-shared" : "tail-shared");
        cls += hasBl || hasRel ? " +call" : "";
        cls += hasTail ? " +tailcall" : "";
        cls += hasFall ? " +fallthrough" : "";
        if (!(hasBl || hasRel || hasTail || hasFall)) {
            cls += " seeds:";
            for (int b = 0; b < 9; ++b) if (f.seeds & (1u << b)) { cls += " "; cls += names[b]; }
        }
        classes[cls].first++;
        classes[cls].second += r.shared;
    }
    for (const auto& [c, v] : classes) std::printf("  %-70s %4zu functions %6zu shared words\n", c.c_str(), v.first, v.second);
    const Resolution res = resolveFunctions(prog, d);
    std::map<DropReason, std::pair<size_t, size_t>> byReason;
    for (const auto& [s, why] : res.dropped) { byReason[why].first++; byReason[why].second += d.funcs.at(s).body.size(); }
    std::printf("\nresolution: %zu functions dropped\n", res.dropped.size());
    for (const auto& [why, v] : byReason) std::printf("  %-24s %4zu functions %6zu words\n", dropReasonName(why), v.first, v.second);
    std::printf("  words reached only by dropped functions: %zu\n", res.exclusiveWordsDropped);
    std::printf("  functions sharing words: %zu (%zu words) before, %zu (%zu words) after\n", res.overlapFunctionsBefore,
                res.overlapWordsBefore, res.overlapFunctionsAfter, res.overlapWordsAfter);
    std::printf("remaining sharing functions (each emits its own copy of the shared code):\n");
    for (uint32_t s : res.remainingShared) {
        const Function& f = d.funcs.at(s);
        std::printf("  0x%08X seeds 0x%03X words %zu\n", s, f.seeds, f.body.size());
    }
    std::printf("\nlargest (first 40 by shared words):\n");
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.shared > b.shared; });
    for (size_t i = 0; i < rows.size() && i < 40; ++i) {
        const Row& r = rows[i];
        const Function& f = d.funcs.at(r.start);
        std::printf("  0x%08X words %5zu shared %5zu seeds 0x%03X %s %s other owners:", r.start, r.words, r.shared, f.seeds,
                    r.contained ? "CONTAINED" : "partial", r.startShared ? "start-inside" : "");
        std::set<uint32_t> oth;
        for (uint32_t a : f.body) for (uint32_t o : owners[a]) if (o != r.start) oth.insert(o);
        int n = 0;
        for (uint32_t o : oth) { if (n++ < 3) std::printf(" 0x%08X", o); }
        std::printf("%s callers bl=%zu tail=%zu fall=%zu\n", oth.size() > 3 ? " ..." : "", bl.count(r.start) ? bl[r.start].size() : 0,
                    tail.count(r.start) ? tail[r.start].size() : 0, fall.count(r.start) ? fall[r.start].size() : 0);
    }
    return 0;
}

}  // namespace awl::recomp

namespace awl::recomp {

// Functions that never return: no return instruction in the body, no indirect tail transfer, and every tail call and
// fall-through leads to a function that never returns (fixpoint over the call graph).
std::set<uint32_t> findNoReturn(const Program& prog, const Discovery& d) {
    std::set<uint32_t> nr;
    for (const auto& [s, f] : d.funcs) {
        bool returns = false;
        for (uint32_t a : f.body) {
            const Insn& i = *prog.at(a);
            if ((i.op == Op::bclr && !i.lk()) || (i.op == Op::bcctr && !i.lk() && isIndirectJump(i) && false)) returns = true;
        }
        if (!returns && f.badDecodes == 0) nr.insert(s);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = nr.begin(); it != nr.end();) {
            const Function& f = d.funcs.at(*it);
            bool ok = true;
            for (uint32_t t : f.tailCalls) if (!nr.count(t)) ok = false;
            if (f.fallsInto && !nr.count(f.fallsInto)) ok = false;
            for (const auto& j : d.indirect)
                if (j.funcStart == *it && (j.kind == IndirectKind::IndirectTailCall || j.kind == IndirectKind::Unresolved)) ok = false;
            if (!ok) { it = nr.erase(it); changed = true; } else ++it;
        }
    }
    return nr;
}

int reportNoReturn(const Program& prog, const Discovery& d) {
    const std::set<uint32_t> nr = findNoReturn(prog, d);
    std::printf("functions that never return: %zu of %zu\n", nr.size(), d.funcs.size());
    size_t sites = 0, intoEntry = 0, intoPad = 0, intoOther = 0;
    for (const auto& [s, f] : d.funcs) {
        for (uint32_t a : f.body) {
            const Insn& i = *prog.at(a);
            if (!(i.op == Op::b && i.lk()) || !nr.count(i.branchTarget())) continue;
            ++sites;
            const uint32_t nxt = a + 4;
            const Insn* n = prog.at(nxt);
            if (!n) ++intoPad;
            else if (d.funcs.count(nxt)) ++intoEntry;
            else if (n->raw == 0 || n->raw == 0x60000000) ++intoPad;
            else ++intoOther;
        }
    }
    std::printf("bl sites to them: %zu; the next word is a function entry %zu, padding or end of text %zu, other code %zu\n", sites, intoEntry, intoPad, intoOther);
    size_t shown = 0;
    for (uint32_t s : nr) {
        if (shown++ >= 40) break;
        size_t callers = 0;
        for (const auto& [o, f] : d.funcs) callers += std::count(f.calls.begin(), f.calls.end(), s);
        std::printf("  0x%08X words %zu bl callers %zu seeds 0x%03X\n", s, d.funcs.at(s).body.size(), callers, d.funcs.at(s).seeds);
    }
    return 0;
}

}  // namespace awl::recomp
