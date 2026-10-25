#include "codegen.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "modules.h"
#include "resolve.h"
#include "sha256.h"
#include "runtime/guest.h"

namespace awl::recomp {

namespace {

std::string S(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string fname(uint32_t a) { return S("f_%08X", a); }

struct GenStats {
    size_t functions = 0, stubs = 0, hle = 0, mmioRecompiled = 0, instructions = 0, bytes = 0;
    size_t midFunctionTails = 0, unsupportedInsns = 0, unresolvedBctr = 0, jumpTables = 0, fellOff = 0, noReturnSites = 0;
    std::map<std::string, size_t> unsupportedKinds;
    std::vector<std::string> errors;   // generation errors; any of them fails the run
    std::vector<std::string> notes;    // places that became loud stops (untranslated, falls off, unresolved)
};

// Where the function being emitted lives. DOL functions use their real addresses; REL functions are analysed at
// a synthetic base and every address they use is formed from the module's section bases (rb is the base of the
// function's own section, loaded once at entry), so the generated code works wherever OSLink places the module.
struct ModuleView {
    const RelSet* rels = nullptr;
    const RelModule* mod = nullptr;   // null for a DOL function
    const std::set<uint32_t>* noReturn = nullptr;   // functions that never return (a bl to one is followed by a stop)
};

std::string relSymbol(const RelModule& m, uint32_t synthAddr) {
    uint32_t sec = 0, off = 0;
    m.locate(synthAddr, sec, off);
    return S("m%u_s%u_%08X", m.rel->id, sec, off);
}

class FnEmitter {
public:
    FnEmitter(const Program& p, const Discovery& d, const std::set<uint32_t>& gen, const Function& f, GenStats& st,
              ModuleView mv = {}, std::string nameOverride = {})
        : p_(p), d_(d), gen_(gen), f_(f), st_(st), rels_(mv.rels), mod_(mv.mod), noRet_(mv.noReturn), nameOverride_(std::move(nameOverride)),
          body_(f.body.begin(), f.body.end()) {
        if (mod_) {
            uint32_t off = 0;
            mod_->locate(f.start, fnSec_, off);
        }
    }

    std::string emit() {
        collectLabels();
        std::string out;
        out += S("void %s(Ctx& c) {\n", (nameOverride_.empty() ? sym(f_.start) : nameOverride_).c_str());
        out += "    uint32_t ea = 0;\n    (void)ea;\n";
        if (mod_) out += S("    const uint32_t rb = c.m->relBase[%u][%u];\n    (void)rb;\n", mod_->rel->id, fnSec_);
        body_text_.clear();
        for (size_t k = 0; k < f_.body.size(); ++k) {
            const uint32_t a = f_.body[k];
            const Insn& i = *p_.at(a);
            if (labels_.count(a)) {
                flush();
                line(S("L_%08X:;", a), true);
            }
            emitInsn(i);
            ++st_.instructions;
            if (fallsThrough(i)) {
                const uint32_t nxt = a + 4;
                if (k + 1 < f_.body.size() && f_.body[k + 1] == nxt) continue;
                flush();
                if (nxt == f_.fallsInto && gen_.count(nxt)) { checkpoint(a); line(S("return %s(c);", sym(nxt).c_str())); }
                else { ++st_.fellOff; note("falls off", a); line(S("rt::unimplemented(c, %s, \"control flow leaves the discovered function body\");", ADDR(a).c_str())); }
            }
        }
        out += body_text_;
        out += "}\n\n";
        return out;
    }

private:
    void line(const std::string& s, bool noIndent = false) {
        body_text_ += noIndent ? "  " : "    ";
        body_text_ += s;
        body_text_ += '\n';
    }
    void flush() {
        if (pend_) { line(S("rt::tick(c, %d);", pend_)); pend_ = 0; }
    }
    // Emit the pc/icount update for an instruction that can trap or call (counted including itself).
    void trapPoint(const Insn& i) {
        line(S("rt::at(c, %s, %d);", ADDR(i.addr).c_str(), pend_));
        pend_ = 0;
    }

    // Preemption checkpoint (pending instructions must already be flushed): records the pc and compares the
    // retired count against the deadline/limit. Emitted at every return, tail transfer and backward branch;
    // calls already pass through rt::at.
    void checkpoint(uint32_t addr) { line(S("rt::at(c, %s, 0);", ADDR(addr).c_str())); }

    static bool fallsThrough(const Insn& i) {
        if (i.op == Op::b) return i.lk();
        if (i.op == Op::bclr) return i.lk() || !isReturn(i);
        if (i.op == Op::bcctr) return i.lk() || !isIndirectJump(i);
        if (i.op == Op::rfi) return false;
        return true;
    }

    void collectLabels() {
        labels_.clear();
        for (uint32_t a : f_.body) {
            const Insn& i = *p_.at(a);
            if ((i.op == Op::b && !i.lk()) || (i.op == Op::bc && !i.lk())) {
                const uint32_t t = i.branchTarget();
                if (body_.count(t) && !(i.op == Op::b && t == f_.start && false)) labels_.insert(t);
            }
            if (i.op == Op::bcctr && isIndirectJump(i)) {
                if (const IndirectJump* j = jumpAt(a)) {
                    if (j->kind == IndirectKind::JumpTableBounded || j->kind == IndirectKind::JumpTableScanned)
                        for (uint32_t t : j->targets) if (body_.count(t)) labels_.insert(t);
                }
            }
        }
    }

    // A bctr word is classified by address: functions that share a tail all dispatch it the same way.
    const IndirectJump* jumpAt(uint32_t a) const {
        const auto it = std::lower_bound(d_.bctrUnique.begin(), d_.bctrUnique.end(), a, [](const IndirectJump& j, uint32_t addr) { return j.addr < addr; });
        return it != d_.bctrUnique.end() && it->addr == a ? &*it : nullptr;
    }

    std::string R(uint32_t n) const { return S("c.r[%u]", n); }
    std::string RA0(uint32_t n) const { return n == 0 ? "0u" : S("c.r[%u]", n); }
    static std::string U(int32_t v) { return S("0x%Xu", static_cast<uint32_t>(v)); }
    std::string F0(uint32_t n) const { return S("c.f[%u].ps0", n); }

    std::string eaD(const Insn& i) const {   // d(rA)
        if (dyn_) return i.rA() == 0 ? simmExpr(i) : S("%s + %s", R(i.rA()).c_str(), simmExpr(i).c_str());
        if (i.rA() == 0) return U(i.simm());
        if (i.simm() == 0) return R(i.rA());
        return S("%s + %s", R(i.rA()).c_str(), U(i.simm()).c_str());
    }
    std::string eaX(const Insn& i) const { return i.rA() == 0 ? R(i.rB()) : S("%s + %s", R(i.rA()).c_str(), R(i.rB()).c_str()); }

    // Branch-condition code. Returns the C++ condition (empty = always) and emits CTR decrement if needed.
    std::string condition(const Insn& i) {
        const uint32_t bo = i.bo(), bi = i.bi();
        std::string ctr, crc;
        if (!(bo & 4)) {
            line("--c.ctr;");
            ctr = (bo & 2) ? "c.ctr == 0" : "c.ctr != 0";
        }
        if (!(bo & 0x10)) crc = S("%srt::crBit(c, %u)", (bo & 8) ? "" : "!", bi);
        if (ctr.empty()) return crc;
        if (crc.empty()) return ctr;
        return ctr + " && " + crc;
    }

    void rc(const Insn& i, uint32_t reg) {
        if (i.rc()) line(S("rt::cr0(c, %s);", R(reg).c_str()));
    }
    void rc1(const Insn& i) {
        if (i.rc()) line("rt::cr1(c);");
    }

    // Records a place where generation could not produce plain code, for the generator report.
    void note(const char* what, uint32_t addr) {
        st_.notes.push_back(S("%s at 0x%08X in %s (seeds 0x%03X)", what, addr, (nameOverride_.empty() ? sym(f_.start) : nameOverride_).c_str(), f_.seeds));
    }

    void unsupported(const Insn& i, const char* why) {
        note(opName(i.op), i.addr);
        ++st_.unsupportedInsns;
        ++st_.unsupportedKinds[opName(i.op)];
        flush();
        line(S("rt::unimplemented(c, %s, \"%s\");", ADDR(i.addr).c_str(), why));
    }

    void tailCall(const Insn& i, uint32_t target) {
        flush();
        if (gen_.count(target)) {
            checkpoint(i.addr);
            line(S("return %s(c);", sym(target).c_str()));
        } else {
            ++st_.midFunctionTails; note("tail branch to non-function", i.addr);
            line(S("rt::unimplemented(c, %s, \"tail branch to an address that is not a recompiled function entry\");", ADDR(i.addr).c_str()));
        }
    }

    void emitInsn(const Insn& i);

    // ---- addresses and symbols ------------------------------------------------------------------------
    // C++ expression for a guest address used by the code being emitted. REL code never embeds linked
    // addresses: positions inside a module are section bases (loaded by OSLink) plus fixed offsets.
    std::string ADDR(uint32_t addr) const {
        if (rels_) {
            if (const RelModule* m = rels_->moduleFor(addr)) {
                uint32_t sec = 0, off = 0;
                if (m->locate(addr, sec, off)) {
                    if (m == mod_ && sec == fnSec_) return S("(rb + 0x%Xu)", off);
                    return S("(c.m->relBase[%u][%u] + 0x%Xu)", m->rel->id, sec, off);
                }
            }
        }
        return S("0x%08X", addr);
    }
    std::string sym(uint32_t addr) const {
        if (rels_) if (const RelModule* m = rels_->moduleFor(addr)) return relSymbol(*m, addr);
        return fname(addr);
    }

    // The 16-bit piece of a module address that an ADDR16_xx relocation places in an instruction immediate.
    std::string dynPiece(const DynReloc& d) const {
        const std::string s = S("(c.m->relBase[%u][%u] + 0x%Xu)", d.module, d.section, d.addend);
        switch (d.type) {
            case 6: return S("((%s + 0x8000u) >> 16)", s.c_str());   // _HA
            case 5: return S("(%s >> 16)", s.c_str());               // _HI
            default: return S("(%s & 0xFFFFu)", s.c_str());          // ADDR16, _LO
        }
    }
    std::string simmExpr(const Insn& i) const { return dyn_ ? S("rt::sext16(%s)", dynPiece(*dyn_).c_str()) : U(i.simm()); }
    static bool dynAware(Op op) {
        switch (op) {
            case Op::addi: case Op::addis: case Op::ori: case Op::oris:
            case Op::lwz: case Op::lbz: case Op::lhz: case Op::lha: case Op::lwzu: case Op::lbzu: case Op::lhzu: case Op::lhau:
            case Op::stw: case Op::stb: case Op::sth: case Op::stwu: case Op::stbu: case Op::sthu: case Op::lmw: case Op::stmw:
            case Op::lfs: case Op::lfd: case Op::lfsu: case Op::lfdu: case Op::stfs: case Op::stfd: case Op::stfsu: case Op::stfdu:
                return true;
            default:
                return false;
        }
    }

    const Program& p_;
    const Discovery& d_;
    const std::set<uint32_t>& gen_;
    const Function& f_;
    GenStats& st_;
    const RelSet* rels_;
    const RelModule* mod_;
    const std::set<uint32_t>* noRet_;
    std::string nameOverride_;
    uint32_t fnSec_ = 0;
    const DynReloc* dyn_ = nullptr;   // relocation on the instruction being emitted, if any
    std::set<uint32_t> body_;
    std::set<uint32_t> labels_;
    std::string body_text_;
    int pend_ = 0;
};

void FnEmitter::emitInsn(const Insn& i) {
    ++pend_;
    const uint32_t D = i.rD(), A = i.rA(), B = i.rB();
    const bool oe = i.oe();
    dyn_ = nullptr;
    if (mod_) {
        const auto it = mod_->dyn.find(i.addr);
        if (it != mod_->dyn.end()) {
            dyn_ = &it->second;
            if (!dynAware(i.op)) {
                st_.errors.push_back(S("%s: relocated immediate on %s at 0x%08X, which the generator cannot express", sym(f_.start).c_str(), opName(i.op), i.addr));
                dyn_ = nullptr;
            }
        }
    }
    switch (i.op) {
        // ---- integer arithmetic ----
        case Op::addi: line(A == 0 ? S("%s = %s;", R(D).c_str(), simmExpr(i).c_str())
                                   : S("%s = %s + %s;", R(D).c_str(), R(A).c_str(), simmExpr(i).c_str())); break;
        case Op::addis:
            if (dyn_) {
                const std::string hi = S("(%s << 16)", dynPiece(*dyn_).c_str());
                line(A == 0 ? S("%s = %s;", R(D).c_str(), hi.c_str()) : S("%s = %s + %s;", R(D).c_str(), R(A).c_str(), hi.c_str()));
            } else {
                line(A == 0 ? S("%s = %s;", R(D).c_str(), U(i.simm() * 65536).c_str())
                            : S("%s = %s + %s;", R(D).c_str(), R(A).c_str(), U(i.simm() * 65536).c_str()));
            }
            break;
        case Op::addic: line(S("%s = rt::addx<true, false>(c, %s, %s, 0);", R(D).c_str(), R(A).c_str(), U(i.simm()).c_str())); break;
        case Op::addic_rc: line(S("%s = rt::addx<true, false>(c, %s, %s, 0);", R(D).c_str(), R(A).c_str(), U(i.simm()).c_str()));
            line(S("rt::cr0(c, %s);", R(D).c_str())); break;
        case Op::subfic: line(S("%s = rt::addx<true, false>(c, ~%s, %s, 1);", R(D).c_str(), R(A).c_str(), U(i.simm()).c_str())); break;
        case Op::mulli: line(S("%s = static_cast<uint32_t>(static_cast<int32_t>(%s) * %d);", R(D).c_str(), R(A).c_str(), i.simm())); break;
        case Op::add: line(S("%s = rt::addx<false, %s>(c, %s, %s, 0);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::addc: line(S("%s = rt::addx<true, %s>(c, %s, %s, 0);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::adde: line(S("%s = rt::addx<true, %s>(c, %s, %s, c.ca);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::addme: line(S("%s = rt::addx<true, %s>(c, %s, 0xFFFFFFFFu, c.ca);", R(D).c_str(), oe ? "true" : "false", R(A).c_str())); rc(i, D); break;
        case Op::addze: line(S("%s = rt::addx<true, %s>(c, %s, 0, c.ca);", R(D).c_str(), oe ? "true" : "false", R(A).c_str())); rc(i, D); break;
        case Op::subf: line(S("%s = rt::addx<false, %s>(c, ~%s, %s, 1);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::subfc: line(S("%s = rt::addx<true, %s>(c, ~%s, %s, 1);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::subfe: line(S("%s = rt::addx<true, %s>(c, ~%s, %s, c.ca);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::subfme: line(S("%s = rt::addx<true, %s>(c, ~%s, 0xFFFFFFFFu, c.ca);", R(D).c_str(), oe ? "true" : "false", R(A).c_str())); rc(i, D); break;
        case Op::subfze: line(S("%s = rt::addx<true, %s>(c, ~%s, 0, c.ca);", R(D).c_str(), oe ? "true" : "false", R(A).c_str())); rc(i, D); break;
        case Op::neg: line(S("%s = rt::addx<false, %s>(c, ~%s, 0, 1);", R(D).c_str(), oe ? "true" : "false", R(A).c_str())); rc(i, D); break;
        case Op::mullw: line(S("%s = rt::mullw<%s>(c, %s, %s);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::mulhw: line(S("%s = rt::mulhw(%s, %s);", R(D).c_str(), R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::mulhwu: line(S("%s = rt::mulhwu(%s, %s);", R(D).c_str(), R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::divw: line(S("%s = rt::divw<%s>(c, %s, %s);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        case Op::divwu: line(S("%s = rt::divwu<%s>(c, %s, %s);", R(D).c_str(), oe ? "true" : "false", R(A).c_str(), R(B).c_str())); rc(i, D); break;
        // ---- compares ----
        case Op::cmpi: line(S("rt::cmps(c, %u, %s, %s);", i.crfD(), R(A).c_str(), U(i.simm()).c_str())); break;
        case Op::cmpli: line(S("rt::cmpu(c, %u, %s, 0x%Xu);", i.crfD(), R(A).c_str(), i.uimm())); break;
        case Op::cmp: line(S("rt::cmps(c, %u, %s, %s);", i.crfD(), R(A).c_str(), R(B).c_str())); break;
        case Op::cmpl: line(S("rt::cmpu(c, %u, %s, %s);", i.crfD(), R(A).c_str(), R(B).c_str())); break;
        // ---- logical ----
        case Op::ori: line(dyn_ ? S("%s = %s | %s;", R(A).c_str(), R(i.rS()).c_str(), dynPiece(*dyn_).c_str())
                                : S("%s = %s | 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm())); break;
        case Op::oris: line(dyn_ ? S("%s = %s | (%s << 16);", R(A).c_str(), R(i.rS()).c_str(), dynPiece(*dyn_).c_str())
                                 : S("%s = %s | 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm() << 16)); break;
        case Op::xori: line(S("%s = %s ^ 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm())); break;
        case Op::xoris: line(S("%s = %s ^ 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm() << 16)); break;
        case Op::andi_rc: line(S("%s = %s & 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm())); line(S("rt::cr0(c, %s);", R(A).c_str())); break;
        case Op::andis_rc: line(S("%s = %s & 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm() << 16)); line(S("rt::cr0(c, %s);", R(A).c_str())); break;
        case Op::and_: line(S("%s = %s & %s;", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::andc: line(S("%s = %s & ~%s;", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::or_: line(S("%s = %s | %s;", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::orc: line(S("%s = %s | ~%s;", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::xor_: line(S("%s = %s ^ %s;", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::nand: line(S("%s = ~(%s & %s);", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::nor: line(S("%s = ~(%s | %s);", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::eqv: line(S("%s = ~(%s ^ %s);", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::extsb: line(S("%s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(%s)));", R(A).c_str(), R(i.rS()).c_str())); rc(i, A); break;
        case Op::extsh: line(S("%s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(%s)));", R(A).c_str(), R(i.rS()).c_str())); rc(i, A); break;
        case Op::cntlzw: line(S("%s = rt::cntlzw(%s);", R(A).c_str(), R(i.rS()).c_str())); rc(i, A); break;
        case Op::slw: line(S("%s = rt::slw(%s, %s);", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::srw: line(S("%s = rt::srw(%s, %s);", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::sraw: line(S("%s = rt::sraw(c, %s, %s);", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str())); rc(i, A); break;
        case Op::srawi: line(S("%s = rt::sraw(c, %s, %u);", R(A).c_str(), R(i.rS()).c_str(), i.sh())); rc(i, A); break;
        case Op::rlwinm: line(S("%s = rt::rotl(%s, %u) & 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.sh(), rt::mask(i.mb(), i.me()))); rc(i, A); break;
        case Op::rlwnm: line(S("%s = rt::rotl(%s, %s) & 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), R(B).c_str(), rt::mask(i.mb(), i.me()))); rc(i, A); break;
        case Op::rlwimi: line(S("%s = (rt::rotl(%s, %u) & 0x%Xu) | (%s & 0x%Xu);", R(A).c_str(), R(i.rS()).c_str(), i.sh(),
                                rt::mask(i.mb(), i.me()), R(A).c_str(), ~rt::mask(i.mb(), i.me()))); rc(i, A); break;
        // ---- condition register ----
        case Op::crand: line(S("rt::setCrBit(c, %u, rt::crBit(c, %u) & rt::crBit(c, %u));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::crandc: line(S("rt::setCrBit(c, %u, rt::crBit(c, %u) & ~rt::crBit(c, %u));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::creqv: line(S("rt::setCrBit(c, %u, ~(rt::crBit(c, %u) ^ rt::crBit(c, %u)));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::crnand: line(S("rt::setCrBit(c, %u, ~(rt::crBit(c, %u) & rt::crBit(c, %u)));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::crnor: line(S("rt::setCrBit(c, %u, ~(rt::crBit(c, %u) | rt::crBit(c, %u)));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::cror: line(S("rt::setCrBit(c, %u, rt::crBit(c, %u) | rt::crBit(c, %u));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::crorc: line(S("rt::setCrBit(c, %u, rt::crBit(c, %u) | ~rt::crBit(c, %u));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::crxor: line(S("rt::setCrBit(c, %u, rt::crBit(c, %u) ^ rt::crBit(c, %u));", i.crbD(), i.crbA(), i.crbB())); break;
        case Op::mcrf: line(S("rt::setCr(c, %u, rt::getCr(c, %u));", i.crfD(), i.crfS())); break;
        case Op::mcrxr: line(S("rt::setCr(c, %u, (c.so << 3) | (c.ov << 2) | (c.ca << 1));", i.crfD())); line("c.so = c.ov = c.ca = 0;"); break;
        case Op::mfcr: line(S("%s = rt::mfcr(c);", R(D).c_str())); break;
        case Op::mtcrf: line(S("rt::mtcrf(c, 0x%X, %s);", i.crm(), R(i.rS()).c_str())); break;
        // ---- loads ----
        case Op::lwz: case Op::lbz: case Op::lhz: case Op::lha: {
            trapPoint(i);
            const char* fn = i.op == Op::lwz ? "ld32" : i.op == Op::lbz ? "ld8" : i.op == Op::lhz ? "ld16" : "ld16s";
            line(S("%s = rt::%s(c, %s);", R(D).c_str(), fn, eaD(i).c_str()));
            break;
        }
        case Op::lwzu: case Op::lbzu: case Op::lhzu: case Op::lhau: {
            trapPoint(i);
            const char* fn = i.op == Op::lwzu ? "ld32" : i.op == Op::lbzu ? "ld8" : i.op == Op::lhzu ? "ld16" : "ld16s";
            line(S("ea = %s;", eaD(i).c_str()));
            line(S("%s = rt::%s(c, ea);", R(D).c_str(), fn));
            line(S("%s = ea;", R(A).c_str()));
            break;
        }
        case Op::lwzx: case Op::lbzx: case Op::lhzx: case Op::lhax: case Op::lhbrx: case Op::lwbrx: {
            trapPoint(i);
            const char* fn = i.op == Op::lwzx ? "ld32" : i.op == Op::lbzx ? "ld8" : i.op == Op::lhzx ? "ld16" : i.op == Op::lhax ? "ld16s" : i.op == Op::lhbrx ? "ld16r" : "ld32r";
            line(S("%s = rt::%s(c, %s);", R(D).c_str(), fn, eaX(i).c_str()));
            break;
        }
        case Op::lwzux: case Op::lbzux: case Op::lhzux: case Op::lhaux: {
            trapPoint(i);
            const char* fn = i.op == Op::lwzux ? "ld32" : i.op == Op::lbzux ? "ld8" : i.op == Op::lhzux ? "ld16" : "ld16s";
            line(S("ea = %s;", eaX(i).c_str()));
            line(S("%s = rt::%s(c, ea);", R(D).c_str(), fn));
            line(S("%s = ea;", R(A).c_str()));
            break;
        }
        case Op::lmw: trapPoint(i); line(S("rt::lmw(c, %u, %s);", D, eaD(i).c_str())); break;
        case Op::lswi: trapPoint(i); line(S("rt::lsw(c, %u, %s, %u);", D, A == 0 ? "0u" : R(A).c_str(), B == 0 ? 32u : B)); break;
        case Op::lswx: trapPoint(i); line(S("rt::lsw(c, %u, %s, c.xerbc);", D, eaX(i).c_str())); break;
        case Op::lwarx: trapPoint(i); line(S("%s = rt::lwarx(c, %s);", R(D).c_str(), eaX(i).c_str())); break;
        // ---- stores ----
        case Op::stw: case Op::stb: case Op::sth: {
            trapPoint(i);
            const char* fn = i.op == Op::stw ? "st32" : i.op == Op::stb ? "st8" : "st16";
            line(S("rt::%s(c, %s, %s);", fn, eaD(i).c_str(), R(i.rS()).c_str()));
            break;
        }
        case Op::stwu: case Op::stbu: case Op::sthu: {
            trapPoint(i);
            const char* fn = i.op == Op::stwu ? "st32" : i.op == Op::stbu ? "st8" : "st16";
            line(S("ea = %s;", eaD(i).c_str()));
            line(S("rt::%s(c, ea, %s);", fn, R(i.rS()).c_str()));
            line(S("%s = ea;", R(A).c_str()));
            break;
        }
        case Op::stwx: case Op::stbx: case Op::sthx: case Op::sthbrx: case Op::stwbrx: {
            trapPoint(i);
            const char* fn = i.op == Op::stwx ? "st32" : i.op == Op::stbx ? "st8" : i.op == Op::sthx ? "st16" : i.op == Op::sthbrx ? "st16r" : "st32r";
            line(S("rt::%s(c, %s, %s);", fn, eaX(i).c_str(), R(i.rS()).c_str()));
            break;
        }
        case Op::stwux: case Op::stbux: case Op::sthux: {
            trapPoint(i);
            const char* fn = i.op == Op::stwux ? "st32" : i.op == Op::stbux ? "st8" : "st16";
            line(S("ea = %s;", eaX(i).c_str()));
            line(S("rt::%s(c, ea, %s);", fn, R(i.rS()).c_str()));
            line(S("%s = ea;", R(A).c_str()));
            break;
        }
        case Op::stmw: trapPoint(i); line(S("rt::stmw(c, %u, %s);", i.rS(), eaD(i).c_str())); break;
        case Op::stswi: trapPoint(i); line(S("rt::stsw(c, %u, %s, %u);", i.rS(), A == 0 ? "0u" : R(A).c_str(), B == 0 ? 32u : B)); break;
        case Op::stswx: trapPoint(i); line(S("rt::stsw(c, %u, %s, c.xerbc);", i.rS(), eaX(i).c_str())); break;
        case Op::stwcx_rc: trapPoint(i); line(S("rt::stwcx(c, %s, %s);", eaX(i).c_str(), R(i.rS()).c_str())); break;
        case Op::dcbz: case Op::dcbz_l: trapPoint(i); line(S("rt::dcbz(c, %s);", eaX(i).c_str())); break;
        case Op::dcbf: case Op::dcbst: case Op::dcbt: case Op::dcbtst: case Op::icbi: case Op::sync: case Op::isync: case Op::eieio:
            break;   // no host effect in a single-threaded, coherent memory model
        // ---- special registers ----
        case Op::mfspr: {
            const uint32_t s = i.spr();
            if (s == 8) line(S("%s = c.lr;", R(D).c_str()));
            else if (s == 9) line(S("%s = c.ctr;", R(D).c_str()));
            else { trapPoint(i); line(S("%s = rt::mfspr(c, %s, %u);", R(D).c_str(), ADDR(i.addr).c_str(), s)); }
            break;
        }
        case Op::mtspr: {
            const uint32_t s = i.spr();
            if (s == 8) line(S("c.lr = %s;", R(i.rS()).c_str()));
            else if (s == 9) line(S("c.ctr = %s;", R(i.rS()).c_str()));
            else { trapPoint(i); line(S("rt::mtspr(c, %s, %u, %s);", ADDR(i.addr).c_str(), s, R(i.rS()).c_str())); }
            break;
        }
        case Op::mftb: line(S("%s = rt::mftb(c, %u);", R(D).c_str(), i.spr())); break;
        case Op::mfmsr: line(S("%s = c.msr;", R(D).c_str())); break;
        case Op::mtmsr: line(S("c.msr = %s;", R(i.rS()).c_str())); break;
        case Op::mfsr: line(S("%s = c.sr[%u];", R(D).c_str(), i.sr())); break;
        case Op::mtsr: line(S("c.sr[%u] = %s;", i.sr(), R(i.rS()).c_str())); break;
        case Op::mfsrin: line(S("%s = c.sr[%s >> 28];", R(D).c_str(), R(B).c_str())); break;
        case Op::mtsrin: line(S("c.sr[%s >> 28] = %s;", R(B).c_str(), R(i.rS()).c_str())); break;
        case Op::tlbie: case Op::tlbsync: case Op::dcbi: break;   // no TLB or cache model in the spike runtime
        case Op::rfi: case Op::sc: case Op::eciwx: case Op::ecowx:
            flush(); line(S("rt::privileged(c, %s, \"%s\");", ADDR(i.addr).c_str(), opName(i.op))); break;
        // tw/twi: trap when any selected comparison of rA with rB (or the sign-extended immediate) holds. TO bits:
        // 16 signed <, 8 signed >, 4 equal, 2 unsigned <, 1 unsigned >. A taken trap raises a program exception, which
        // the runtime does not model: it stops loudly. A trap that is not taken is a no-op.
        case Op::tw: case Op::twi:
            trapPoint(i);
            line(S("if (rt::trapCond(%u, %s, %s)) rt::privileged(c, %s, \"trap instruction (tw/twi) taken: the program exception is not modelled\");",
                   D, R(A).c_str(), i.op == Op::tw ? R(B).c_str() : U(i.simm()).c_str(), ADDR(i.addr).c_str()));
            break;
        // ---- floating point ----
        case Op::lfs: trapPoint(i); line(S("rt::lfs(c, %u, %s);", D, eaD(i).c_str())); break;
        case Op::lfd: trapPoint(i); line(S("rt::lfd(c, %u, %s);", D, eaD(i).c_str())); break;
        case Op::lfsu: case Op::lfdu: trapPoint(i); line(S("ea = %s;", eaD(i).c_str()));
            line(S("rt::%s(c, %u, ea);", i.op == Op::lfsu ? "lfs" : "lfd", D)); line(S("%s = ea;", R(A).c_str())); break;
        case Op::lfsx: trapPoint(i); line(S("rt::lfs(c, %u, %s);", D, eaX(i).c_str())); break;
        case Op::lfdx: trapPoint(i); line(S("rt::lfd(c, %u, %s);", D, eaX(i).c_str())); break;
        case Op::lfsux: case Op::lfdux: trapPoint(i); line(S("ea = %s;", eaX(i).c_str()));
            line(S("rt::%s(c, %u, ea);", i.op == Op::lfsux ? "lfs" : "lfd", D)); line(S("%s = ea;", R(A).c_str())); break;
        case Op::stfs: trapPoint(i); line(S("rt::stfs(c, %u, %s);", D, eaD(i).c_str())); break;
        case Op::stfd: trapPoint(i); line(S("rt::stfd(c, %u, %s);", D, eaD(i).c_str())); break;
        case Op::stfsu: case Op::stfdu: trapPoint(i); line(S("ea = %s;", eaD(i).c_str()));
            line(S("rt::%s(c, %u, ea);", i.op == Op::stfsu ? "stfs" : "stfd", D)); line(S("%s = ea;", R(A).c_str())); break;
        case Op::stfsx: trapPoint(i); line(S("rt::stfs(c, %u, %s);", D, eaX(i).c_str())); break;
        case Op::stfdx: trapPoint(i); line(S("rt::stfd(c, %u, %s);", D, eaX(i).c_str())); break;
        case Op::stfiwx: trapPoint(i); line(S("rt::stfiwx(c, %u, %s);", D, eaX(i).c_str())); break;
        case Op::stfsux: case Op::stfdux: trapPoint(i); line(S("ea = %s;", eaX(i).c_str()));
            line(S("rt::%s(c, %u, ea);", i.op == Op::stfsux ? "stfs" : "stfd", D)); line(S("%s = ea;", R(A).c_str())); break;
        case Op::psq_l: case Op::psq_lu: trapPoint(i);
            line(S("ea = %s;", A == 0 ? U(i.psqD()).c_str() : S("%s + %s", R(A).c_str(), U(i.psqD()).c_str()).c_str()));
            line(S("rt::psqL(c, %u, ea, %u, %u);", D, i.psqW(), i.psqI()));
            if (i.op == Op::psq_lu) line(S("%s = ea;", R(A).c_str()));
            break;
        case Op::psq_st: case Op::psq_stu: trapPoint(i);
            line(S("ea = %s;", A == 0 ? U(i.psqD()).c_str() : S("%s + %s", R(A).c_str(), U(i.psqD()).c_str()).c_str()));
            line(S("rt::psqSt(c, %u, ea, %u, %u);", D, i.psqW(), i.psqI()));
            if (i.op == Op::psq_stu) line(S("%s = ea;", R(A).c_str()));
            break;
        case Op::psq_lx: case Op::psq_lux: trapPoint(i);
            line(S("ea = %s;", eaX(i).c_str()));
            line(S("rt::psqL(c, %u, ea, %u, %u);", D, i.psqWx(), i.psqIx()));
            if (i.op == Op::psq_lux) line(S("%s = ea;", R(A).c_str()));
            break;
        case Op::psq_stx: case Op::psq_stux: trapPoint(i);
            line(S("ea = %s;", eaX(i).c_str()));
            line(S("rt::psqSt(c, %u, ea, %u, %u);", D, i.psqWx(), i.psqIx()));
            if (i.op == Op::psq_stux) line(S("%s = ea;", R(A).c_str()));
            break;
        case Op::fadd: line(S("rt::setD(c, %u, %s + %s);", D, F0(A).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fsub: line(S("rt::setD(c, %u, %s - %s);", D, F0(A).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmul: line(S("rt::setD(c, %u, %s * %s);", D, F0(A).c_str(), F0(i.frC()).c_str())); rc1(i); break;
        case Op::fdiv: line(S("rt::setD(c, %u, %s / %s);", D, F0(A).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fadds: line(S("rt::setS(c, %u, rt::fround(%s + %s));", D, F0(A).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fsubs: line(S("rt::setS(c, %u, rt::fround(%s - %s));", D, F0(A).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmuls: line(S("rt::setS(c, %u, rt::fround(%s * %s));", D, F0(A).c_str(), F0(i.frC()).c_str())); rc1(i); break;
        case Op::fdivs: line(S("rt::setS(c, %u, rt::fround(%s / %s));", D, F0(A).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmadd: line(S("rt::setD(c, %u, std::fma(%s, %s, %s));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmsub: line(S("rt::setD(c, %u, std::fma(%s, %s, -%s));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fnmadd: line(S("rt::setD(c, %u, -std::fma(%s, %s, %s));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fnmsub: line(S("rt::setD(c, %u, -std::fma(%s, %s, -%s));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmadds: line(S("rt::setS(c, %u, rt::fround(std::fma(%s, %s, %s)));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmsubs: line(S("rt::setS(c, %u, rt::fround(std::fma(%s, %s, -%s)));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fnmadds: line(S("rt::setS(c, %u, -rt::fround(std::fma(%s, %s, %s)));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fnmsubs: line(S("rt::setS(c, %u, -rt::fround(std::fma(%s, %s, -%s)));", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fsel: line(S("rt::setD(c, %u, %s >= 0.0 ? %s : %s);", D, F0(A).c_str(), F0(i.frC()).c_str(), F0(B).c_str())); rc1(i); break;
        case Op::fmr: line(S("rt::setD(c, %u, %s);", D, F0(B).c_str())); rc1(i); break;
        case Op::fneg: line(S("rt::setD(c, %u, -%s);", D, F0(B).c_str())); rc1(i); break;
        case Op::fabs: line(S("rt::setD(c, %u, std::fabs(%s));", D, F0(B).c_str())); rc1(i); break;
        case Op::fnabs: line(S("rt::setD(c, %u, -std::fabs(%s));", D, F0(B).c_str())); rc1(i); break;
        case Op::frsp: line(S("rt::setS(c, %u, rt::fround(%s));", D, F0(B).c_str())); rc1(i); break;
        case Op::fres: line(S("rt::setS(c, %u, rt::fresPlaceholder(%s));", D, F0(B).c_str())); rc1(i); break;
        case Op::frsqrte: line(S("rt::setD(c, %u, rt::frsqrtePlaceholder(%s));", D, F0(B).c_str())); rc1(i); break;
        case Op::fctiw: line(S("rt::setFctiw(c, %u, rt::fctiw(%s, false));", D, F0(B).c_str())); rc1(i); break;
        case Op::fctiwz: line(S("rt::setFctiw(c, %u, rt::fctiw(%s, true));", D, F0(B).c_str())); rc1(i); break;
        case Op::fcmpu: case Op::fcmpo: line(S("rt::fcmp(c, %u, %s, %s);", i.crfD(), F0(A).c_str(), F0(B).c_str())); break;
        case Op::mffs: line(S("rt::mffs(c, %u);", D)); rc1(i); break;
        case Op::mtfsf: line(S("rt::mtfsf(c, 0x%X, %u);", i.fm(), B)); rc1(i); break;
        case Op::mtfsfi: line(S("rt::mtfsfi(c, %u, %u);", i.crfD(), (i.raw >> 12) & 15)); rc1(i); break;
        case Op::mtfsb0: line(S("rt::mtfsb(c, %u, 0);", i.crbD())); rc1(i); break;
        case Op::mtfsb1: line(S("rt::mtfsb(c, %u, 1);", i.crbD())); rc1(i); break;
        case Op::mcrfs: line(S("rt::mcrfs(c, %u, %u);", i.crfD(), i.crfS())); break;
        case Op::ps_add: case Op::ps_sub: case Op::ps_div: case Op::ps_merge00: case Op::ps_merge01: case Op::ps_merge10:
        case Op::ps_merge11:
            line(S("rt::%s(c, %u, %u, %u, 0);", opName(i.op), D, A, B)); rc1(i); break;
        case Op::ps_mul: case Op::ps_muls0: case Op::ps_muls1:
            line(S("rt::%s(c, %u, %u, 0, %u);", opName(i.op), D, A, i.frC())); rc1(i); break;
        case Op::ps_madd: case Op::ps_msub: case Op::ps_nmadd: case Op::ps_nmsub: case Op::ps_madds0: case Op::ps_madds1:
        case Op::ps_sum0: case Op::ps_sum1: case Op::ps_sel:
            line(S("rt::%s(c, %u, %u, %u, %u);", opName(i.op), D, A, B, i.frC())); rc1(i); break;
        case Op::ps_mr: case Op::ps_neg: case Op::ps_abs: case Op::ps_nabs: case Op::ps_res: case Op::ps_rsqrte:
            line(S("rt::%s(c, %u, 0, %u, 0);", opName(i.op), D, B)); rc1(i); break;
        case Op::ps_cmpu0: case Op::ps_cmpo0: line(S("rt::fcmp(c, %u, c.f[%u].ps0, c.f[%u].ps0);", i.crfD(), A, B)); break;
        case Op::ps_cmpu1: case Op::ps_cmpo1: line(S("rt::fcmp(c, %u, c.f[%u].ps1, c.f[%u].ps1);", i.crfD(), A, B)); break;
        // ---- branches ----
        case Op::b: {
            const uint32_t t = i.branchTarget();
            if (i.lk()) {
                trapPoint(i);
                if (gen_.count(t)) {
                    line(S("AWL_CALL(c, %s, %s, %s);", ADDR(i.addr).c_str(), ADDR(t).c_str(), sym(t).c_str()));
                    // The callee never returns; if it does (a replaced or mis-analysed function), stop instead of running
                    // whatever follows the call.
                    if (noRet_ && noRet_->count(t)) { ++st_.noReturnSites; line(S("rt::returnedFromNoReturn(c, %s, %s);", ADDR(i.addr).c_str(), ADDR(t).c_str())); }
                }
                else { ++st_.unsupportedInsns; ++st_.unsupportedKinds["bl-to-non-function"]; note("bl to non-function", i.addr); line(S("rt::unimplemented(c, %s, \"call to an address that is not a recompiled function\");", ADDR(i.addr).c_str())); }
            } else if (body_.count(t) && !(t != f_.start && gen_.count(t))) {
                flush();
                if (t <= i.addr) checkpoint(i.addr);   // backward branch
                line(S("goto L_%08X;", t));
            } else {
                tailCall(i, t);
            }
            break;
        }
        case Op::bc: {
            const uint32_t t = i.branchTarget();
            flush();
            const std::string cond = condition(i);
            if (i.lk()) {
                if (t == i.addr + 4) { line(S("c.lr = %s;", ADDR(i.addr + 4).c_str())); break; }   // bcl 20,31,$+4: read the pc
                unsupported(i, "conditional call");
                break;
            }
            const std::string cnd = cond.empty() ? "true" : cond;
            if (body_.count(t) && !(t != f_.start && gen_.count(t))) {
                if (t <= i.addr) line(S("if (%s) { rt::at(c, %s, 0); goto L_%08X; }", cnd.c_str(), ADDR(i.addr).c_str(), t));   // backward branch
                else line(S("if (%s) goto L_%08X;", cnd.c_str(), t));
            } else if (gen_.count(t)) {
                line(S("if (%s) { rt::at(c, %s, 0); return %s(c); }", cnd.c_str(), ADDR(i.addr).c_str(), sym(t).c_str()));
            } else {
                ++st_.midFunctionTails; note("branch out of body", i.addr);
                line(S("if (%s) rt::unimplemented(c, %s, \"conditional branch out of the function body\");", cnd.c_str(), i.addr));
            }
            break;
        }
        case Op::bclr: {
            if (i.lk()) {
                trapPoint(i);
                const std::string cond = condition(i);
                if (!cond.empty()) { unsupported(i, "conditional blrl"); break; }
                line("{ const uint32_t target = c.lr;");
                line(S("  rt::callIndirect(c, %s, target); }", ADDR(i.addr).c_str()));
            } else {
                flush();
                const std::string cond = condition(i);
                if (cond.empty()) { checkpoint(i.addr); line(S("rt::ret(c, %s);", ADDR(i.addr).c_str())); line("return;"); }
                else line(S("if (%s) { rt::at(c, %s, 0); rt::ret(c, %s); return; }", cond.c_str(), ADDR(i.addr).c_str(), ADDR(i.addr).c_str()));
            }
            break;
        }
        case Op::bcctr: {
            if (i.lk()) {
                trapPoint(i);
                const std::string cond = condition(i);
                if (!cond.empty()) { unsupported(i, "conditional bctrl"); break; }
                line(S("rt::callIndirect(c, %s, c.ctr);", ADDR(i.addr).c_str()));
            } else if (isIndirectJump(i)) {
                flush();
                const IndirectJump* j = jumpAt(i.addr);
                if (!j || j->kind == IndirectKind::Unresolved || j->kind == IndirectKind::Conditional) {
                    ++st_.unresolvedBctr; note("unresolved bctr", i.addr);
                    line(S("rt::unresolvedJump(c, %s);", ADDR(i.addr).c_str()));
                } else if (j->kind == IndirectKind::JumpTableBounded || j->kind == IndirectKind::JumpTableScanned) {
                    ++st_.jumpTables;
                    checkpoint(i.addr);   // targets may be backward
                    // In a REL the table holds linked addresses: dispatch on the offset within this section.
                    line(mod_ ? "switch (c.ctr - rb) {" : "switch (c.ctr) {");
                    std::set<uint32_t> done;
                    for (uint32_t t : j->targets) {
                        if (!done.insert(t).second) continue;
                        if (mod_) {
                            uint32_t sec = 0, off = 0;
                            if (!mod_->locate(t, sec, off) || sec != fnSec_) { unsupported(i, "jump-table target outside the function's section"); continue; }
                            line(S("case 0x%Xu: goto L_%08X;", off, t), true);
                        } else {
                            line(S("case 0x%08X: goto L_%08X;", t, t), true);
                        }
                    }
                    line(S("default: rt::badJump(c, %s, c.ctr);", ADDR(i.addr).c_str()), true);
                    line("}");
                } else if (j->kind == IndirectKind::ConstTailJump) {
                    tailCall(i, j->constTarget);
                } else {   // IndirectTailCall
                    checkpoint(i.addr);
                    line(S("rt::tailIndirect(c, %s, c.ctr);", ADDR(i.addr).c_str()));
                    line("return;");
                }
            } else {
                unsupported(i, "conditional bcctr");
            }
            break;
        }
        case Op::invalid:
            unsupported(i, "undecodable word");
            break;
        default:
            unsupported(i, "instruction not handled by the generator");
            break;
    }
}

// Writes generated files only when their content differs from what is on disk, so untouched files keep their
// timestamps and the host build skips them.
struct OutputWriter {
    explicit OutputWriter(std::string d) : dir(std::move(d)) {}
    void write(const std::string& name, const std::string& content) {
        digests[name] = Sha256::of(content);
        const auto path = std::filesystem::path(dir) / name;
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (old == content) { ++unchanged; return; }
            }
        }
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        o << content;
        ++written;
    }
    // Removes generated sources from earlier runs that are no longer part of the output (gen_*.cpp only).
    size_t removeStale(const std::vector<std::string>& keep) {
        size_t n = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("gen_", 0) != 0 || e.path().extension() != ".cpp") continue;
            if (std::find(keep.begin(), keep.end(), name) != keep.end()) continue;
            std::filesystem::remove(e.path(), ec);
            ++n;
        }
        return n;
    }
    std::string dir;
    size_t written = 0, unchanged = 0;
    std::map<std::string, std::string> digests;   // SHA-256 of every generated file, by name
    // The regression digest: one line per file ("<sha256>  <name>", sorted by name) and a final "output <sha256>" line
    // that hashes the lines above, so one value covers the whole generated output.
    std::string digestFile() const {
        std::string t = "# SHA-256 of each generated file; the last line hashes the lines above\n";
        for (const auto& [n, h] : digests) t += h + "  " + n + "\n";
        return t + "output " + Sha256::of(t) + "\n";
    }
};

std::set<uint32_t> bootClosure(const Program& prog, const Discovery& d, const std::set<uint32_t>& hle) {
    std::set<uint32_t> seen;
    std::vector<uint32_t> work{prog.dol->entry};
    while (!work.empty()) {
        const uint32_t s = work.back();
        work.pop_back();
        if (!seen.insert(s).second) continue;
        auto it = d.funcs.find(s);
        if (it == d.funcs.end()) continue;
        const Function& f = it->second;
        // Privileged functions (rfi/sc) without an HLE handler are emitted as boundary stubs and HLE functions are
        // replaced; both are leaves. MMIO-only functions are recompiled, so the walk continues into them.
        if (f.platform.privileged != 0 || hle.count(s)) continue;
        for (uint32_t t : f.calls) work.push_back(t);
        for (uint32_t t : f.tailCalls) work.push_back(t);
        if (f.fallsInto) work.push_back(f.fallsInto);
        for (const auto& j : d.indirect) {
            if (j.funcStart == s && j.kind == IndirectKind::ConstTailJump) work.push_back(j.constTarget);
        }
    }
    return seen;
}

std::string hex8(uint32_t v) { return S("0x%08X", v); }

std::string evidenceText(const Function& f) {
    std::string e;
    for (const auto& k : f.platform.privilegedKinds) { if (!e.empty()) e += "; "; e += k; }
    for (const auto& [b, n] : f.platform.mmio) { if (!e.empty()) e += "; "; e += "MMIO " + b + " x" + std::to_string(n); }
    return e;
}

}  // namespace

int generateCode(const Program& prog, const Discovery& d, const std::vector<HleEntry>& hle, const RelSet* rels,
                 const OsLinkEvidence* os, const std::string& outDir, bool full) {
    namespace fs = std::filesystem;
    std::set<uint32_t> hleAddrs;
    std::map<uint32_t, const HleEntry*> hleByAddr;
    for (const HleEntry& e : hle) { hleAddrs.insert(e.addr); hleByAddr[e.addr] = &e; }
    // Discovery ambiguities are resolved first: functions that are not functions (data, case labels, unreferenced
    // duplicates of code other functions reach) are neither emitted nor dispatchable (see resolve.h).
    const Resolution res = resolveFunctions(prog, d);
    for (const HleEntry& e : hle) {
        if (res.isDropped(e.addr)) {
            std::fprintf(stderr, "HLE manifest entry 0x%08X (%s) is not a function: discovery dropped it (%s)\n", e.addr, e.name.c_str(),
                         dropReasonName(res.dropped.at(e.addr)));
            return 1;
        }
    }
    std::set<uint32_t> gen;
    if (full) {
        for (const auto& [a, f] : d.funcs) { (void)f; if (!res.isDropped(a)) gen.insert(a); }
    } else {
        gen = bootClosure(prog, d, hleAddrs);
        for (auto it = gen.begin(); it != gen.end();) {
            if (!d.funcs.count(*it) || res.isDropped(*it)) it = gen.erase(it); else ++it;
        }
    }
    // REL modules are generated with the whole DOL only (the boot closure never reaches them).
    const bool withRels = full && rels && !rels->modules.empty();
    if (withRels && !os) {
        std::fprintf(stderr, "RELs were given but OSLink/OSUnlink were not identified\n");
        return 1;
    }
    std::set<uint32_t> genAll = gen;   // every function start the emitter may call: the DOL's and the modules' (synthetic)
    std::vector<std::vector<uint32_t>> relFns;   // per module: the functions to emit, by synthetic address
    std::vector<Resolution> relRes;
    if (withRels) {
        for (const auto& m : rels->modules) {
            relRes.push_back(resolveFunctions(*m->prog, m->disc));
            relFns.emplace_back();
            for (const auto& [a, f] : m->disc.funcs) {
                (void)f;
                if (relRes.back().isDropped(a)) continue;
                relFns.back().push_back(a);
                genAll.insert(a);
            }
        }
    }
    // Functions that never return (not the replaced ones, whose native handlers decide how they end).
    std::set<uint32_t> noRet;
    for (uint32_t a : findNoReturn(prog, d))
        if (gen.count(a) && !hleAddrs.count(a)) noRet.insert(a);
    if (withRels)
        for (size_t mi = 0; mi < rels->modules.size(); ++mi)
            for (uint32_t a : findNoReturn(*rels->modules[mi]->prog, rels->modules[mi]->disc))
                if (genAll.count(a)) noRet.insert(a);
    fs::create_directories(outDir);
    GenStats st;
    std::vector<std::string> files;
    OutputWriter out(outDir);
    // Chunks are address windows (a function belongs to the window holding its start), so a change to one
    // function can only alter its own chunk file, and file names do not depend on what else was generated.
    constexpr uint32_t kChunkWindow = 0x8000;
    const char* prelude =
        "// Generated by awl-recomp from the local DOL. Derived from game bytes: keep out of version control.\n"
        "#include \"gen_decls.h\"\n#include <cmath>\nnamespace rt = awl::rt;\nusing namespace awl::rt;\n\n";
    std::string chunk;
    std::string chunkName;
    auto flushChunk = [&]() {
        if (chunk.empty()) return;
        out.write(chunkName, prelude + chunk);
        files.push_back(chunkName);
        chunk.clear();
    };
    std::string decls = "#pragma once\n#include \"guest.h\"\n#include \"hle_abi.h\"\n";
    for (uint32_t a : gen) decls += S("void %s(awl::rt::Ctx& c);\n", fname(a).c_str());
    if (withRels)
        for (size_t mi = 0; mi < rels->modules.size(); ++mi)
            for (uint32_t a : relFns[mi]) decls += S("void %s(awl::rt::Ctx& c);\n", relSymbol(*rels->modules[mi], a).c_str());
    out.write("gen_decls.h", decls);
    for (uint32_t a : gen) {
        const Function& f = d.funcs.at(a);
        ++st.functions;
        const std::string name = S("gen_%08X.cpp", a & ~(kChunkWindow - 1));
        if (!chunk.empty() && name != chunkName) flushChunk();
        chunkName = name;
        if (hleByAddr.count(a)) {
            ++st.hle;
            chunk += S("void %s(Ctx& c) { rt::hleCall(c, 0x%08X); }\n\n", fname(a).c_str(), a);
        } else if (f.platform.privileged != 0 && !(a == prog.dol->entry)) {
            ++st.stubs;
            chunk += S("void %s(Ctx& c) { rt::platformBoundary(c, 0x%08X, \"%s\"); }\n\n", fname(a).c_str(), a, evidenceText(f).c_str());
        } else {
            if (f.platform.any()) {
                ++st.mmioRecompiled;
                chunk += S("// platform evidence: %s\n", evidenceText(f).c_str());
            }
            // The SDK OSLink/OSUnlink stay recompiled; after a successful call the generated wrapper tells the
            // runtime about the module (see rt::moduleLinked), which registers its functions for dispatch.
            const bool wrapLink = withRels && a == os->osLink, wrapUnlink = withRels && a == os->osUnlink;
            if (wrapLink || wrapUnlink) {
                FnEmitter e(prog, d, gen, f, st, ModuleView{nullptr, nullptr, &noRet}, fname(a) + "_impl");
                chunk += e.emit();
                chunk += S("// %s: module functions are registered once the recompiled SDK function reports success.\n", wrapLink ? "OSLink" : "OSUnlink");
                chunk += S("void %s(Ctx& c) {\n    const uint32_t info = c.r[3];\n    %s_impl(c);\n    if (c.r[3] != 0) rt::%s(c, info);\n}\n\n",
                           fname(a).c_str(), fname(a).c_str(), wrapLink ? "moduleLinked" : "moduleUnlinked");
            } else {
                FnEmitter e(prog, d, gen, f, st, ModuleView{nullptr, nullptr, &noRet});
                chunk += e.emit();
            }
        }
    }
    flushChunk();
    // REL module functions, chunked by section-relative address window within each module.
    size_t relFunctions = 0;
    if (withRels) {
        for (size_t mi = 0; mi < rels->modules.size(); ++mi) {
            const auto& m = rels->modules[mi];
            for (uint32_t a : relFns[mi]) {
                const Function& f = m->disc.funcs.at(a);
                ++relFunctions;
                uint32_t sec = 0, off = 0;
                m->locate(a, sec, off);
                const std::string name = S("gen_m%u_%08X.cpp", m->rel->id, (a - m->layout.base) & ~(kChunkWindow - 1));
                if (!chunk.empty() && name != chunkName) flushChunk();
                chunkName = name;
                if (f.platform.privileged != 0) {
                    st.errors.push_back(S("%s: privileged instruction in a REL function (module %u section %u offset 0x%X)", relSymbol(*m, a).c_str(), m->rel->id, sec, off));
                    continue;
                }
                if (f.platform.any()) {
                    ++st.mmioRecompiled;
                    chunk += S("// platform evidence: %s\n", evidenceText(f).c_str());
                }
                FnEmitter e(*m->prog, m->disc, genAll, f, st, ModuleView{rels, m.get(), &noRet});
                chunk += e.emit();
            }
            flushChunk();
        }
    }
    // Dispatch table
    {
        std::string t = "#include \"gen_decls.h\"\nnamespace awl::rt {\nconst FnEntry kFunctions[] = {\n";
        for (uint32_t a : gen) t += S("    {0x%08X, %s},\n", a, fname(a).c_str());
        t += S("};\nconst uint32_t kFunctionCount = %zu;\n", gen.size());
        // Names of the HLE-replaced functions, so an unregistered handler is reported by name.
        size_t named = 0;
        std::string names;
        for (uint32_t a : gen) {
            const auto it = hleByAddr.find(a);
            if (it == hleByAddr.end()) continue;
            names += S("    {0x%08X, \"%s\"},\n", a, it->second->name.c_str());
            ++named;
        }
        if (named) {
            t += "namespace {\nconst HleName kHleNames[] = {\n" + names;
            t += S("};\nstruct HleNamesDeclarer { HleNamesDeclarer() { declareHleNames(kHleNames, %zu); } } g_hleNamesDeclarer;\n}\n", named);
        }
        if (withRels) {
            // The module tables live in gen_rel_dispatch.cpp; calling into it from here keeps that object linked when
            // the generated code is a static library.
            t += "void declareGeneratedRelModules();\nnamespace {\nstruct RelModulesDeclarer { RelModulesDeclarer() { declareGeneratedRelModules(); } } g_relModulesDeclarer;\n}\n";
        }
        t += "}\n";
        out.write("gen_dispatch.cpp", t);
        files.push_back("gen_dispatch.cpp");
    }
    // REL module tables: section lengths and the (section, offset) of every function, declared at start-up.
    if (withRels) {
        std::string t = "#include \"gen_decls.h\"\nnamespace awl::rt {\nnamespace {\n";
        for (size_t mi = 0; mi < rels->modules.size(); ++mi) {
            const auto& m = rels->modules[mi];
            t += S("const uint32_t kRel%uLengths[] = {", m->rel->id);
            for (const RelSection& s : m->rel->sections) t += S("%u, ", s.length);
            t += "};\n";
            t += S("const RelFnDecl kRel%uFns[] = {\n", m->rel->id);
            for (uint32_t a : relFns[mi]) {
                uint32_t sec = 0, off = 0;
                m->locate(a, sec, off);
                t += S("    {%u, 0x%X, %s},\n", sec, off, relSymbol(*m, a).c_str());
            }
            t += "};\n";
        }
        t += "const RelModuleDecl kRelModules[] = {\n";
        for (size_t mi = 0; mi < rels->modules.size(); ++mi) {
            const auto& m = rels->modules[mi];
            t += S("    {%u, %zu, kRel%uLengths, kRel%uFns, %zu},\n", m->rel->id, m->rel->sections.size(), m->rel->id, m->rel->id, relFns[mi].size());
        }
        t += S("};\n}\nvoid declareGeneratedRelModules() { declareRelModules(kRelModules, %zu); }\n}\n", rels->modules.size());
        out.write("gen_rel_dispatch.cpp", t);
        files.push_back("gen_rel_dispatch.cpp");
    }
    {
        std::string cm = "set(AWL_RECOMP_GEN_SOURCES\n";
        for (const auto& f : files) cm += "    \"${AWL_RECOMP_GEN_DIR}/" + f + "\"\n";
        cm += ")\n";
        out.write("sources.cmake", cm);
    }
    const std::string digestText = out.digestFile();
    out.write("gen_digest.txt", digestText);
    out.digests.erase("gen_digest.txt");
    const size_t removed = out.removeStale(files);
    std::printf("generated %zu functions (%zu platform stubs, %zu HLE calls, %zu MMIO-only recompiled), %zu instructions into %zu files under %s\n",
                st.functions + relFunctions, st.stubs, st.hle, st.mmioRecompiled, st.instructions, files.size(), outDir.c_str());
    if (withRels) {
        std::printf("  REL modules: %zu, %zu functions; OSLink %s, OSUnlink %s (wrapped to register module functions)\n", rels->modules.size(),
                    relFunctions, hex8(os->osLink).c_str(), hex8(os->osUnlink).c_str());
    }
    std::printf("  files written %zu, unchanged %zu, stale removed %zu\n", out.written, out.unchanged, removed);
    std::printf("  output digest: sha256 %s (%zu files)\n", digestText.substr(digestText.rfind("output ") + 7, 64).c_str(), out.digests.size());
    std::printf("  jump tables emitted: %zu; unresolved bctr: %zu; tails/branches to non-function targets: %zu; bodies that fall off: %zu\n",
                st.jumpTables, st.unresolvedBctr, st.midFunctionTails, st.fellOff);
    std::printf("  functions that never return: %zu; calls to them followed by a stop: %zu\n", noRet.size(), st.noReturnSites);
    std::printf("  untranslated instructions: %zu\n", st.unsupportedInsns);
    for (const auto& [k, v] : st.unsupportedKinds) std::printf("    %-24s %zu\n", k.c_str(), v);
    auto describe = [](const char* what, const Resolution& r) {
        size_t byReason[3] = {0, 0, 0};
        for (const auto& [s, why] : r.dropped) { (void)s; ++byReason[static_cast<int>(why)]; }
        std::printf("  resolution (%s): dropped %zu (%zu data in text, %zu jump-table labels, %zu unreferenced fragments); functions sharing words "
                    "%zu (%zu words) before, %zu (%zu words) after, each emitting its own copy\n",
                    what, r.dropped.size(), byReason[0], byReason[1], byReason[2], r.overlapFunctionsBefore, r.overlapWordsBefore,
                    r.overlapFunctionsAfter, r.overlapWordsAfter);
    };
    describe("DOL", res);
    for (size_t mi = 0; mi < relRes.size(); ++mi) describe(rels->modules[mi]->rel->name.c_str(), relRes[mi]);
    for (const auto& n : st.notes) std::printf("  note: %s\n", n.c_str());
    for (const auto& e : st.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
    return st.errors.empty() ? 0 : 1;
}

int generateCode(const Program& prog, const Discovery& d, const std::vector<HleEntry>& hle, const std::string& outDir, bool full) {
    return generateCode(prog, d, hle, nullptr, nullptr, outDir, full);
}

}  // namespace awl::recomp
