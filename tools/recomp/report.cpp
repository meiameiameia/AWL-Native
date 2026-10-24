#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "codegen.h"

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
