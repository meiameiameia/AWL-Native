#include "analysis.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <set>
#include <unordered_map>

namespace awl::recomp {

// ---------------------------------------------------------------------------------------------
// Program
// ---------------------------------------------------------------------------------------------

const Insn* Program::at(uint32_t addr) const {
    if (addr & 3) return nullptr;
    for (const auto& s : segs) {
        if (addr >= s.base && addr < s.end()) return &s.insns[(addr - s.base) / 4];
    }
    return nullptr;
}

int64_t Program::wordIndex(uint32_t addr) const {
    if (addr & 3) return -1;
    int64_t acc = 0;
    for (const auto& s : segs) {
        if (addr >= s.base && addr < s.end()) return acc + (addr - s.base) / 4;
        acc += static_cast<int64_t>(s.insns.size());
    }
    return -1;
}

size_t Program::totalWords() const {
    size_t n = 0;
    for (const auto& s : segs) n += s.insns.size();
    return n;
}

Program buildProgram(const DolImage& dol) {
    Program p;
    p.dol = &dol;
    for (const auto& s : dol.sections) {
        if (!s.text) continue;
        TextSeg seg;
        seg.base = s.addr;
        seg.sectionIndex = static_cast<uint32_t>(s.index);
        seg.insns.reserve(s.size / 4);
        for (uint32_t off = 0; off < s.size; off += 4) {
            const uint32_t raw = be32(&dol.file[s.fileOffset + off]);
            seg.insns.push_back(decode(raw, s.addr + off));
        }
        p.segs.push_back(std::move(seg));
    }
    std::sort(p.segs.begin(), p.segs.end(), [](const TextSeg& a, const TextSeg& b) { return a.base < b.base; });
    return p;
}

const char* indirectKindName(IndirectKind k) {
    switch (k) {
        case IndirectKind::JumpTableBounded: return "jump-table (bounded by compare)";
        case IndirectKind::JumpTableScanned: return "jump-table (length scanned)";
        case IndirectKind::ConstTailJump: return "constant tail jump";
        case IndirectKind::IndirectTailCall: return "indirect tail call";
        case IndirectKind::Conditional: return "conditional bcctr";
        case IndirectKind::Unresolved: return "unresolved";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------------------------

bool writesGpr(const Insn& i, uint32_t reg) {
    switch (i.op) {
        case Op::addi: case Op::addic: case Op::addic_rc: case Op::addis: case Op::addme: case Op::addze:
        case Op::add: case Op::addc: case Op::adde: case Op::divw: case Op::divwu: case Op::mulhw:
        case Op::mulhwu: case Op::mulli: case Op::mullw: case Op::neg: case Op::subf: case Op::subfc:
        case Op::subfe: case Op::subfic: case Op::subfme: case Op::subfze:
        case Op::lbz: case Op::lbzx: case Op::lha: case Op::lhax: case Op::lhbrx: case Op::lhz: case Op::lhzx:
        case Op::lwarx: case Op::lwbrx: case Op::lwz: case Op::lwzx: case Op::mfcr: case Op::mfmsr:
        case Op::mfspr: case Op::mftb: case Op::mfsr: case Op::mfsrin: case Op::eciwx:
            return i.rD() == reg;
        case Op::lbzu: case Op::lbzux: case Op::lhau: case Op::lhaux: case Op::lhzu: case Op::lhzux:
        case Op::lwzu: case Op::lwzux:
            return i.rD() == reg || i.rA() == reg;
        case Op::stbu: case Op::stbux: case Op::sthu: case Op::sthux: case Op::stwu: case Op::stwux:
        case Op::stfsu: case Op::stfsux: case Op::stfdu: case Op::stfdux: case Op::lfsu: case Op::lfsux:
        case Op::lfdu: case Op::lfdux: case Op::psq_lu: case Op::psq_stu: case Op::psq_lux: case Op::psq_stux:
            return i.rA() == reg;
        case Op::and_: case Op::andc: case Op::andi_rc: case Op::andis_rc: case Op::cntlzw: case Op::eqv:
        case Op::extsb: case Op::extsh: case Op::nand: case Op::nor: case Op::or_: case Op::orc:
        case Op::ori: case Op::oris: case Op::rlwimi: case Op::rlwinm: case Op::rlwnm: case Op::slw:
        case Op::sraw: case Op::srawi: case Op::srw: case Op::xor_: case Op::xori: case Op::xoris:
            return i.rA() == reg;
        case Op::lmw: return reg >= i.rD();
        case Op::lswi: case Op::lswx: return true;   // conservative
        case Op::stwcx_rc: return false;
        default: return false;
    }
}

namespace {

bool isNopOrZero(const Insn& i) { return i.raw == 0 || i.raw == 0x60000000; }

bool isMtctr(const Insn& i) { return i.op == Op::mtspr && i.spr() == 9; }
bool isMtlr(const Insn& i) { return i.op == Op::mtspr && i.spr() == 8; }

// Backward constant propagation within a straight-line window. Returns the value register `reg` holds
// immediately before `addr`, if it was built from lis/addi/ori style immediates.
std::optional<uint32_t> constValue(const Program& p, uint32_t reg, uint32_t addr, uint32_t floor, int depth = 0) {
    if (depth > 4) return std::nullopt;
    for (uint32_t a = addr; a > floor;) {
        a -= 4;
        const Insn* i = p.at(a);
        if (!i) return std::nullopt;
        if (isCall(*i)) return std::nullopt;
        if (!writesGpr(*i, reg)) continue;
        switch (i->op) {
            case Op::addis:
                if (i->rA() == 0) return static_cast<uint32_t>(i->simm()) << 16;
                if (auto v = constValue(p, i->rA(), a, floor, depth + 1)) return *v + (static_cast<uint32_t>(i->simm()) << 16);
                return std::nullopt;
            case Op::addi:
                if (i->rA() == 0) return static_cast<uint32_t>(i->simm());
                if (auto v = constValue(p, i->rA(), a, floor, depth + 1)) return *v + static_cast<uint32_t>(i->simm());
                return std::nullopt;
            case Op::ori:
                if (auto v = constValue(p, i->rS(), a, floor, depth + 1)) return *v | i->uimm();
                return std::nullopt;
            case Op::oris:
                if (auto v = constValue(p, i->rS(), a, floor, depth + 1)) return *v | (i->uimm() << 16);
                return std::nullopt;
            case Op::or_:
                if (i->rS() == i->rB() && !i->rc()) return constValue(p, i->rS(), a, floor, depth + 1);
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }
    return std::nullopt;
}

bool textPointer(const Program& p, uint32_t v) { return (v & 3) == 0 && p.inText(v); }

bool hasEpilogueBefore(const Program& p, uint32_t addr, uint32_t floor) {
    const uint32_t lo = addr > floor + 24 * 4 ? addr - 24 * 4 : floor;
    for (uint32_t a = addr; a > lo;) {
        a -= 4;
        const Insn* i = p.at(a);
        if (!i) break;
        if (isMtlr(*i)) return true;
        if (i->op == Op::addi && i->rD() == 1 && i->rA() == 1 && i->simm() > 0) return true;
    }
    return false;
}

}  // namespace

const char* mmioBlockName(uint32_t addr) {
    if (addr < 0xCC000000u || addr >= 0xCE000000u) return nullptr;
    if (addr >= 0xCD000000u) return "HW-mirror-CD";
    const uint32_t off = addr - 0xCC000000u;
    if (off < 0x1000) return "CP";
    if (off < 0x2000) return "PE";
    if (off < 0x3000) return "VI";
    if (off < 0x4000) return "PI";
    if (off < 0x5000) return "MI";
    if (off < 0x6000) return "DSP";
    if (off < 0x6400) return "DI";
    if (off < 0x6800) return "SI";
    if (off < 0x6C00) return "EXI";
    if (off < 0x7000) return "AI";
    if (off >= 0x8000 && off < 0x8004) return "GXFIFO";
    return "HW-other";
}

// ---------------------------------------------------------------------------------------------
// Indirect branches
// ---------------------------------------------------------------------------------------------

IndirectJump classifyBctr(const Program& p, uint32_t funcStart, uint32_t bctrAddr) {
    IndirectJump r;
    r.addr = bctrAddr;
    r.funcStart = funcStart;
    const Insn* b = p.at(bctrAddr);
    if (!b || b->op != Op::bcctr) return r;
    if (!isIndirectJump(*b)) {
        r.kind = IndirectKind::Conditional;
        return r;
    }
    const uint32_t floor = std::max<uint32_t>(funcStart, bctrAddr > 0x100 ? bctrAddr - 0x100 : 0);
    // 1. nearest mtctr
    const Insn* mt = nullptr;
    uint32_t mtAddr = 0;
    for (uint32_t a = bctrAddr; a > floor && bctrAddr - a < 12 * 4;) {
        a -= 4;
        const Insn* i = p.at(a);
        if (i && isMtctr(*i)) { mt = i; mtAddr = a; break; }
    }
    if (!mt) return r;
    const uint32_t rC = mt->rS();
    // 2. definition of the counter source
    const Insn* def = nullptr;
    uint32_t defAddr = 0;
    for (uint32_t a = mtAddr; a > floor;) {
        a -= 4;
        const Insn* i = p.at(a);
        if (!i) break;
        if (writesGpr(*i, rC)) { def = i; defAddr = a; break; }
    }
    if (!def) return r;
    if (auto cv = constValue(p, rC, mtAddr, floor)) {
        if (textPointer(p, *cv)) {
            r.kind = IndirectKind::ConstTailJump;
            r.constTarget = *cv;
            return r;
        }
    }
    if (def->op == Op::lwzx) {
        std::optional<uint32_t> table;
        for (int pass = 0; pass < 2 && !table; ++pass) {
            const uint32_t baseReg = pass == 0 ? def->rA() : def->rB();
            auto cv = constValue(p, baseReg, defAddr, floor);
            if (cv && p.dol->findSection(*cv) && !p.dol->isText(*cv)) table = *cv;
        }
        if (table) {
            // Bound from the nearest preceding cmplwi + conditional branch.
            uint32_t bound = 0;
            bool bounded = false;
            const uint32_t lo = defAddr > floor + 48 * 4 ? defAddr - 48 * 4 : floor;
            for (uint32_t a = defAddr; a > lo && !bounded;) {
                a -= 4;
                const Insn* c = p.at(a);
                if (!c || c->op != Op::cmpli) continue;
                for (uint32_t n = a + 4; n < defAddr; n += 4) {
                    const Insn* br = p.at(n);
                    if (br && br->op == Op::bc && !br->lk() && (br->bi() >> 2) == c->crfD()) {
                        const bool taken = (br->bo() & 8) != 0;
                        if (taken && (br->bi() & 3) == 1) { bound = c->uimm() + 1; bounded = true; }      // bgt default
                        else if (!taken && (br->bi() & 3) == 0) { bound = c->uimm(); bounded = true; }   // bge default
                        break;
                    }
                }
            }
            auto entryValid = [&](uint32_t idx, uint32_t& target) {
                uint32_t v = 0;
                if (!p.dol->readWord(*table + idx * 4, v) || !textPointer(p, v)) return false;
                const int64_t d = static_cast<int64_t>(v) - bctrAddr;
                if (d > 0x40000 || d < -0x40000) return false;
                target = v;
                return true;
            };
            std::vector<uint32_t> targets;
            bool ok = bounded && bound > 0 && bound <= 4096;
            if (ok) {
                for (uint32_t k = 0; k < bound && ok; ++k) {
                    uint32_t t = 0;
                    if (entryValid(k, t)) targets.push_back(t); else ok = false;
                }
            }
            if (ok) {
                r.kind = IndirectKind::JumpTableBounded;
            } else {
                targets.clear();
                for (uint32_t k = 0; k < 1024; ++k) {
                    uint32_t t = 0;
                    if (!entryValid(k, t)) break;
                    targets.push_back(t);
                }
                if (targets.size() >= 2) r.kind = IndirectKind::JumpTableScanned;
            }
            if (r.kind == IndirectKind::JumpTableBounded || r.kind == IndirectKind::JumpTableScanned) {
                r.table = *table;
                r.count = static_cast<uint32_t>(targets.size());
                r.targets = std::move(targets);
                return r;
            }
        }
    }
    // A counter value loaded from memory (vtable slot, pointer-to-member thunk) is a runtime-dispatched tail
    // jump: resolved by the guest-address lookup rather than at generation time.
    const bool loaded = def->op == Op::lwz || def->op == Op::lwzx || def->op == Op::lwzu || def->op == Op::lwzux;
    if (loaded || hasEpilogueBefore(p, bctrAddr, floor)) {
        r.kind = IndirectKind::IndirectTailCall;
        return r;
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------------------------

const Function* Discovery::containing(uint32_t addr) const {
    // Linear in the number of functions with start <= addr; bodies may be sparse, so test membership.
    auto it = funcs.upper_bound(addr);
    for (int tries = 0; it != funcs.begin() && tries < 64; ++tries) {
        --it;
        if (std::binary_search(it->second.body.begin(), it->second.body.end(), addr)) return &it->second;
    }
    return nullptr;
}

namespace {

constexpr uint32_t kStrongSeeds = SeedEntry | SeedBl | SeedRel;

bool prologueAt(const Program& p, uint32_t a) {
    const Insn* i = p.at(a);
    if (!i) return false;
    return i->raw == 0x7C0802A6u || (i->op == Op::stwu && i->rD() == 1 && i->rA() == 1);
}

class Discoverer {
public:
    Discoverer(const Program& p, Discovery& d) : p_(p), d_(d), owner_(p.totalWords(), 0), stamp_(p.totalWords(), 0) {}

    void addSeed(uint32_t addr, uint32_t kind) {
        if (!p_.inText(addr)) return;
        auto [it, inserted] = seeds_.try_emplace(addr, 0);
        it->second |= kind;
        if (inserted) pending_.push_back(addr);
    }

    void run() {
        while (!pending_.empty()) {
            const uint32_t a = pending_.back();
            pending_.pop_back();
            if (!d_.funcs.count(a)) walk(a);
        }
    }

    const std::map<uint32_t, uint32_t>& seeds() const { return seeds_; }
    uint32_t seedMask(uint32_t a) const { auto it = seeds_.find(a); return it == seeds_.end() ? 0 : it->second; }
    uint32_t ownerOf(uint32_t addr) const { int64_t i = p_.wordIndex(addr); return i < 0 ? 0 : owner_[i]; }
    size_t overlapWords() const { return overlapWords_; }
    const std::vector<std::pair<uint32_t, uint32_t>>& overlaps() const { return overlaps_; }
    const std::map<std::pair<uint32_t, uint32_t>, size_t>& pairs() const { return pairs_; }

private:
    void walk(uint32_t start) {
        Function f;
        f.start = start;
        f.seeds = seeds_[start];
        ++walkId_;
        std::vector<uint32_t> stack{start};
        std::set<uint32_t> calls, tails;
        while (!stack.empty()) {
            uint32_t a = stack.back();
            stack.pop_back();
            for (;;) {
                const int64_t wi = p_.wordIndex(a);
                if (wi < 0) break;
                if (stamp_[wi] == walkId_) break;
                stamp_[wi] = walkId_;
                const Insn& i = *p_.at(a);
                f.body.push_back(a);
                if (owner_[wi] == 0) owner_[wi] = start;
                else if (owner_[wi] != start) { ++f.sharedWords; ++overlapWords_; ++pairs_[{start, owner_[wi]}]; if (overlaps_.size() < 64) overlaps_.push_back({a, owner_[wi]}); }
                if (i.op == Op::invalid) {
                    f.badDecodeAddrs.push_back(a);
                    ++f.badDecodes;
                    break;
                }
                bool terminal = false;
                if (i.op == Op::b) {
                    const uint32_t t = i.branchTarget();
                    if (i.lk()) {
                        calls.insert(t);
                    } else {
                        terminal = true;
                        const bool known = t != start && seeds_.count(t) != 0;
                        const bool far = t < start || t > start + 0x40000 || !p_.inText(t);
                        const bool prologue = t != start && prologueAt(p_, t);
                        if (known || far || prologue) {
                            tails.insert(t);
                            if (!known) addSeed(t, SeedTail);
                        } else {
                            stack.push_back(t);
                        }
                    }
                } else if (i.op == Op::bc) {
                    const uint32_t t = i.branchTarget();
                    if (i.lk()) calls.insert(t);
                    else if (p_.inText(t)) stack.push_back(t);
                } else if (i.op == Op::bclr) {
                    if (i.lk()) ++f.indirectCalls;
                    else if (isReturn(i)) terminal = true;
                } else if (i.op == Op::bcctr) {
                    if (i.lk()) {
                        ++f.indirectCalls;
                    } else if (isIndirectJump(i)) {
                        terminal = true;
                        IndirectJump j = classifyBctr(p_, start, a);
                        if (j.kind == IndirectKind::ConstTailJump) {
                            tails.insert(j.constTarget);
                            addSeed(j.constTarget, SeedTail);
                        } else {
                            for (uint32_t t : j.targets) stack.push_back(t);
                        }
                        d_.indirect.push_back(std::move(j));
                    } else {
                        IndirectJump j;
                        j.addr = a;
                        j.funcStart = start;
                        j.kind = IndirectKind::Conditional;
                        d_.indirect.push_back(std::move(j));
                    }
                } else if (i.op == Op::rfi) {
                    terminal = true;
                }
                if (terminal) break;
                a += 4;
                const uint32_t m = seedMask(a);
                if ((m & kStrongSeeds) && a != start) {
                    f.fallsInto = a;
                    ++d_.stats.fallThroughEdges;
                    break;
                }
            }
        }
        std::sort(f.body.begin(), f.body.end());
        f.calls.assign(calls.begin(), calls.end());
        f.tailCalls.assign(tails.begin(), tails.end());
        d_.stats.tailCallEdges += tails.size();
        d_.funcs[start] = std::move(f);
    }

    const Program& p_;
    Discovery& d_;
    std::vector<uint32_t> owner_;
    std::vector<uint32_t> stamp_;
    uint32_t walkId_ = 0;
    std::map<uint32_t, uint32_t> seeds_;
    std::vector<uint32_t> pending_;
    size_t overlapWords_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> overlaps_;
    std::map<std::pair<uint32_t, uint32_t>, size_t> pairs_;
};

bool looksLikeEntry(const Program& p, uint32_t a) {
    const Insn* i = p.at(a);
    if (!i || i->op == Op::invalid) return false;
    if (i->raw == 0x7C0802A6u) return true;                         // mflr r0
    if (i->op == Op::stwu && i->rD() == 1 && i->rA() == 1) return true;
    const Insn* prev = p.at(a - 4);
    if (!prev) return true;                                         // first word of a segment
    if (isNopOrZero(*prev) || prev->op == Op::invalid) return true;
    if (isReturn(*prev) || isUnconditionalBranch(*prev) || isIndirectJump(*prev) || prev->op == Op::rfi) return true;
    return false;
}

PlatformEvidence scanPlatform(const Program& p, const Function& f) {
    PlatformEvidence ev;
    std::set<std::string> kinds;
    for (uint32_t a : f.body) {
        const Insn& i = *p.at(a);
        if (isSupervisor(i)) {
            if (isModelledSupervisor(i)) ++ev.supervisorState; else ++ev.privileged;
            std::string k = mnemonic(i);
            if (i.op == Op::mfspr || i.op == Op::mtspr) {
                const char* n = sprName(i.spr());
                k += std::string(" ") + (n ? n : std::to_string(i.spr()).c_str());
            }
            kinds.insert(k);
        }
        switch (i.op) {
            case Op::lbz: case Op::lhz: case Op::lha: case Op::lwz: case Op::stb: case Op::sth: case Op::stw:
            case Op::lfs: case Op::lfd: case Op::stfs: case Op::stfd:
            case Op::lbzu: case Op::lhzu: case Op::lwzu: case Op::stbu: case Op::sthu: case Op::stwu: {
                std::optional<uint32_t> base = i.rA() == 0 ? std::optional<uint32_t>(0)
                                                            : constValue(p, i.rA(), a, f.start);
                if (!base) break;
                const uint32_t ea = *base + static_cast<uint32_t>(i.simm());
                if (const char* name = mmioBlockName(ea)) ++ev.mmio[name];
                else if (ea >= 0x80000000u && ea < 0x80003100u) ++ev.lowmem;
                break;
            }
            default: break;
        }
    }
    ev.privilegedKinds.assign(kinds.begin(), kinds.end());
    return ev;
}

const char* seedName(uint32_t bit) {
    switch (bit) {
        case SeedEntry: return "entry";
        case SeedBl: return "bl-target";
        case SeedRel: return "rel-reference";
        case SeedData: return "data-pointer";
        case SeedTail: return "tail-branch";
        case SeedGap: return "gap-heuristic";
        case SeedFall: return "fall-through";
        case SeedJumpTable: return "jump-table";
        case SeedConst: return "code-constant";
    }
    return "?";
}

}  // namespace

Discovery discoverFunctions(const Program& prog, const DiscoveryInput& in) {
    Discovery d;
    Discoverer disc(prog, d);
    const DolImage& dol = *prog.dol;

    disc.addSeed(dol.entry, SeedEntry);
    for (const auto& seg : prog.segs) {
        for (const Insn& i : seg.insns) {
            if (i.op == Op::b && i.lk()) disc.addSeed(i.branchTarget(), SeedBl);
            if (i.op == Op::bc && i.lk()) disc.addSeed(i.branchTarget(), SeedBl);
            if (isCall(i) && (i.op == Op::bcctr || i.op == Op::bclr)) ++d.totalBctrl;
        }
    }
    for (uint32_t t : in.relTargets) disc.addSeed(t, SeedRel);
    for (uint32_t t : in.extraEntries) disc.addSeed(t, SeedEntry);

    // Data pointers.
    std::vector<uint32_t> candidates;
    for (const auto& s : dol.sections) {
        if (s.text) continue;
        for (uint32_t off = 0; off + 4 <= s.size; off += 4) {
            const uint32_t v = be32(&dol.file[s.fileOffset + off]);
            if (textPointer(prog, v)) candidates.push_back(v);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    d.stats.dataCandidates = candidates.size();

    // Function-pointer literals built in code: addis rX,0,hi followed shortly by addi/ori rY,rX,lo.
    std::set<uint32_t> constSet;
    for (const auto& seg : prog.segs) {
        for (size_t k = 0; k < seg.insns.size(); ++k) {
            const Insn& hi = seg.insns[k];
            if (hi.op != Op::addis || hi.rA() != 0) continue;
            for (size_t n = k + 1; n < seg.insns.size() && n <= k + 6; ++n) {
                const Insn& lo = seg.insns[n];
                if ((lo.op == Op::addi || lo.op == Op::ori) && lo.rA() == hi.rD() && (lo.op == Op::addi || lo.rS() == hi.rD())) {
                    const uint32_t src = lo.op == Op::addi ? lo.rA() : lo.rS();
                    if (src != hi.rD()) continue;
                    const uint32_t v = (static_cast<uint32_t>(hi.simm()) << 16) +
                                       (lo.op == Op::addi ? static_cast<uint32_t>(lo.simm()) : lo.uimm());
                    if (textPointer(prog, v)) constSet.insert(v);
                    break;
                }
                if (writesGpr(lo, hi.rD())) break;
            }
        }
    }
    std::vector<uint32_t> constCandidates(constSet.begin(), constSet.end());
    d.stats.constCandidates = constCandidates.size();

    disc.run();
    std::set<uint32_t> resolvedCandidates;
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        for (uint32_t c : candidates) {
            if (resolvedCandidates.count(c)) continue;
            if (d.funcs.count(c) || disc.seedMask(c)) {   // already a function: just note the data reference
                disc.addSeed(c, SeedData);
                resolvedCandidates.insert(c);
                continue;
            }
            if (disc.ownerOf(c) != 0) {
                ++d.stats.dataInsideBody;
                resolvedCandidates.insert(c);
                continue;
            }
            if (looksLikeEntry(prog, c)) {
                disc.addSeed(c, SeedData);
                disc.run();   // walk now so later candidates inside this body (case labels) are recognised
                ++d.stats.dataAccepted;
                changed = true;
            } else {
                ++d.stats.dataRejected;
            }
            resolvedCandidates.insert(c);
        }
        for (uint32_t c : constCandidates) {
            if (d.funcs.count(c) || disc.seedMask(c)) { disc.addSeed(c, SeedConst); continue; }
            if (disc.ownerOf(c) != 0) continue;
            if (looksLikeEntry(prog, c)) {
                disc.addSeed(c, SeedConst);
                disc.run();
                ++d.stats.constAccepted;
                changed = true;
            }
        }
        if (changed) disc.run();
        // Gap heuristic: first non-padding word of each uncovered run.
        size_t gaps = 0;
        for (const auto& seg : prog.segs) {
            uint32_t a = seg.base;
            while (a < seg.end()) {
                if (disc.ownerOf(a) != 0 || isNopOrZero(*prog.at(a))) { a += 4; continue; }
                // a is the first uncovered non-padding word of a run
                const Insn* prev = prog.at(a - 4);
                const bool boundary = !prev || disc.ownerOf(a - 4) != 0 || isNopOrZero(*prev);
                if (boundary && prog.at(a)->op != Op::invalid) {
                    disc.addSeed(a, SeedGap);
                    ++gaps;
                    disc.run();
                } else {
                    a += 4;
                    continue;
                }
                // walk() claimed some words; move past the claimed region
                while (a < seg.end() && disc.ownerOf(a) != 0) a += 4;
            }
        }
        d.stats.gapSeeds += gaps;
        if (!changed && gaps == 0) break;
    }

    // Per-function platform evidence and stats.
    d.overlaps = disc.overlaps();
    d.overlapPairs = disc.pairs();
    d.stats.overlapWords = disc.overlapWords();
    d.stats.functions = d.funcs.size();
    for (auto& [start, f] : d.funcs) {
        f.seeds = disc.seedMask(start);
        for (uint32_t bit = 1; bit <= SeedConst; bit <<= 1) {
            if (f.seeds & bit) ++d.stats.bySeed[seedName(bit)];
        }
        f.platform = scanPlatform(prog, f);
        if (disc.ownerOf(start) != start) d.ambiguousSeeds.push_back(start);
    }
    {
        auto rank = [](IndirectKind k) { return k == IndirectKind::Unresolved || k == IndirectKind::Conditional ? 1 : 0; };
        std::map<uint32_t, IndirectJump> best;
        for (const auto& j : d.indirect) {
            auto it = best.find(j.addr);
            if (it == best.end() || rank(j.kind) < rank(it->second.kind)) best[j.addr] = j;
        }
        for (const auto& seg : prog.segs) {
            for (const Insn& i : seg.insns) {
                if (i.op != Op::bcctr || i.lk() || best.count(i.addr)) continue;
                const uint32_t own = disc.ownerOf(i.addr);
                IndirectJump j = classifyBctr(prog, own ? own : i.addr - 0x100, i.addr);
                if (!own) ++d.bctrUnreached;
                best[i.addr] = j;
            }
        }
        for (auto& [a, j] : best) d.bctrUnique.push_back(std::move(j));
        for (const auto& [a, f] : d.funcs) if (f.badDecodes) d.suspectData.push_back(a);
    }
    for (const auto& seg : prog.segs) {
        d.stats.textBytes += seg.insns.size() * 4;
        uint32_t runStart = 0;
        bool inRun = false;
        for (const Insn& i : seg.insns) {
            const bool owned = disc.ownerOf(i.addr) != 0;
            if (owned) {
                d.stats.assignedBytes += 4;
                if (i.op == Op::invalid) d.badDecodeAddrsInside.push_back(i.addr);
            } else if (isNopOrZero(i)) {
                d.stats.paddingBytes += 4;
            } else {
                d.stats.unassignedBytes += 4;
                if (i.op == Op::invalid) d.badDecodeAddrsOutside.push_back(i.addr);
            }
            const bool gap = !owned && !isNopOrZero(i);
            if (gap && !inRun) { runStart = i.addr; inRun = true; }
            if (!gap && inRun) { d.unassignedRanges.push_back({runStart, i.addr}); inRun = false; }
        }
        if (inRun) d.unassignedRanges.push_back({runStart, seg.end()});
    }
    return d;
}

}  // namespace awl::recomp
