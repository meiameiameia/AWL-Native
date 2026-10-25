#include "resolve.h"

#include <algorithm>
#include <set>

namespace awl::recomp {

namespace {

constexpr uint32_t kReferenced = SeedEntry | SeedBl | SeedRel | SeedTail | SeedFall | SeedConst;

// How many words of each function are held by more than one of `alive`.
void countOverlap(const Discovery& d, const std::set<uint32_t>& alive, size_t& functions, size_t& words, std::vector<uint32_t>* who) {
    std::map<uint32_t, uint32_t> owners;
    for (uint32_t s : alive) for (uint32_t a : d.funcs.at(s).body) ++owners[a];
    functions = 0;
    words = 0;
    for (uint32_t s : alive) {
        size_t shared = 0;
        for (uint32_t a : d.funcs.at(s).body) if (owners[a] > 1) ++shared;
        if (shared) {
            ++functions;
            words += shared;
            if (who) who->push_back(s);
        }
    }
}

// True when the instructions from `start` onwards, with no branch, call, return or exception instruction in between,
// reach an undecodable word.
bool straightLineReachesInvalid(const Program& prog, uint32_t start) {
    for (uint32_t a = start;; a += 4) {
        const Insn* i = prog.at(a);
        if (!i) return false;
        if (i->op == Op::invalid) return true;
        if (i->op == Op::b || i->op == Op::bc || i->op == Op::bclr || i->op == Op::bcctr || i->op == Op::rfi || i->op == Op::sc ||
            i->op == Op::tw || i->op == Op::twi)
            return false;
    }
}

}  // namespace

const char* dropReasonName(DropReason r) {
    switch (r) {
        case DropReason::DataInText: return "data in text";
        case DropReason::JumpTableLabel: return "jump-table label";
        case DropReason::UnreferencedFragment: return "unreferenced fragment";
    }
    return "?";
}

Resolution resolveFunctions(const Program& prog, const Discovery& d) {
    Resolution res;
    // Inbound edges between discovered functions (bl, tail branch, fall-through).
    std::set<uint32_t> inbound;
    for (const auto& [s, f] : d.funcs) {
        for (uint32_t t : f.calls) inbound.insert(t);
        for (uint32_t t : f.tailCalls) inbound.insert(t);
        if (f.fallsInto) inbound.insert(f.fallsInto);
    }
    // Who contains each address, and every jump table (address range and its case labels).
    std::map<uint32_t, std::vector<uint32_t>> owners;
    for (const auto& [s, f] : d.funcs) for (uint32_t a : f.body) owners[a].push_back(s);
    struct Table { uint32_t begin, end; };
    std::vector<Table> tables;
    std::set<uint32_t> labels;
    for (const IndirectJump& j : d.indirect) {
        if (j.kind != IndirectKind::JumpTableBounded && j.kind != IndirectKind::JumpTableScanned) continue;
        tables.push_back({j.table, j.table + 4 * static_cast<uint32_t>(std::max<size_t>(j.count, j.targets.size()))});
        for (uint32_t t : j.targets) labels.insert(t);
    }

    // Candidates for the jump-table-label rule need every data word that points at them.
    std::map<uint32_t, std::vector<uint32_t>> dataRefs;   // function start -> addresses of data words equal to it
    for (const auto& [s, f] : d.funcs) {
        if ((f.seeds & kReferenced) == 0 && (f.seeds & SeedData) && labels.count(s)) dataRefs[s];
    }
    if (!dataRefs.empty()) {
        for (const DolSection& sec : prog.dol->sections) {
            if (sec.text) continue;
            for (uint32_t off = 0; off + 4 <= sec.size; off += 4) {
                const uint32_t v = be32(&prog.dol->file[sec.fileOffset + off]);
                const auto it = dataRefs.find(v);
                if (it != dataRefs.end()) it->second.push_back(sec.addr + off);
            }
        }
    }
    auto inTable = [&](uint32_t addr) {
        for (const Table& t : tables) if (addr >= t.begin && addr < t.end) return true;
        return false;
    };

    for (const auto& [s, f] : d.funcs) {
        // 1. Data in text: the walk runs into an undecodable word and nothing references the start; or, for a start
        //    only a code constant (lis+addi) names, straight-line code from the entry reaches an undecodable word, so
        //    executing it from the entry would trap on that word. That is a table in the text segment that code reads
        //    as data, not a function.
        if (f.badDecodes && (f.seeds & kReferenced) == 0) {
            res.dropped[s] = DropReason::DataInText;
            continue;
        }
        if (f.badDecodes && (f.seeds & (kReferenced & ~SeedConst)) == 0 && straightLineReachesInvalid(prog, s)) {
            res.dropped[s] = DropReason::DataInText;
            continue;
        }
        const bool hasInbound = inbound.count(s) != 0;
        // 2. A case label: seeded only through data words that are all entries of jump tables, and the label lies in
        //    the body of another function (the one containing the switch).
        if ((f.seeds & kReferenced) == 0 && (f.seeds & SeedData) && !hasInbound && labels.count(s)) {
            bool inOther = false;
            for (uint32_t o : owners[s]) if (o != s) inOther = true;
            bool allInTables = !dataRefs[s].empty();
            for (uint32_t a : dataRefs[s]) allInTables = allInTables && inTable(a);
            if (inOther && allInTables) {
                res.dropped[s] = DropReason::JumpTableLabel;
                continue;
            }
        }
        // 3. A heuristic fragment nothing refers to that duplicates code other functions reach.
        if (f.seeds == SeedGap && !hasInbound) {
            size_t shared = 0;
            for (uint32_t a : f.body) if (owners[a].size() > 1) ++shared;
            if (shared) res.dropped[s] = DropReason::UnreferencedFragment;
        }
    }

    std::set<uint32_t> all, alive;
    for (const auto& [s, f] : d.funcs) {
        (void)f;
        all.insert(s);
        if (!res.isDropped(s)) alive.insert(s);
    }
    countOverlap(d, all, res.overlapFunctionsBefore, res.overlapWordsBefore, nullptr);
    countOverlap(d, alive, res.overlapFunctionsAfter, res.overlapWordsAfter, &res.remainingShared);
    // Words that only dropped functions reached.
    std::set<uint32_t> covered;
    for (uint32_t s : alive) for (uint32_t a : d.funcs.at(s).body) covered.insert(a);
    for (const auto& [s, why] : res.dropped) {
        (void)why;
        for (uint32_t a : d.funcs.at(s).body) if (!covered.count(a)) { covered.insert(a); ++res.exclusiveWordsDropped; }
    }
    return res;
}

}  // namespace awl::recomp
