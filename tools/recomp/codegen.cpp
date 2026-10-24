#include "codegen.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

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
    size_t functions = 0, stubs = 0, instructions = 0, bytes = 0;
    size_t midFunctionTails = 0, unsupportedInsns = 0, unresolvedBctr = 0, jumpTables = 0, fellOff = 0;
    std::map<std::string, size_t> unsupportedKinds;
};

class FnEmitter {
public:
    FnEmitter(const Program& p, const Discovery& d, const std::set<uint32_t>& gen, const Function& f, GenStats& st)
        : p_(p), d_(d), gen_(gen), f_(f), st_(st), body_(f.body.begin(), f.body.end()) {}

    std::string emit() {
        collectLabels();
        std::string out;
        out += S("void %s(Ctx& c) {\n", fname(f_.start).c_str());
        out += "    uint32_t ea = 0;\n    (void)ea;\n";
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
                if (nxt == f_.fallsInto && gen_.count(nxt)) line(S("return %s(c);", fname(nxt).c_str()));
                else { ++st_.fellOff; line(S("rt::unimplemented(c, 0x%08X, \"control flow leaves the discovered function body\");", a)); }
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
        if (pend_) { line(S("c.icount += %d;", pend_)); pend_ = 0; }
    }
    // Emit the pc/icount update for an instruction that can trap or call (counted including itself).
    void trapPoint(const Insn& i) {
        line(S("rt::at(c, 0x%08X, %d);", i.addr, pend_));
        pend_ = 0;
    }

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

    const IndirectJump* jumpAt(uint32_t a) const {
        for (const auto& j : d_.indirect) if (j.addr == a && j.funcStart == f_.start) return &j;
        return nullptr;
    }

    std::string R(uint32_t n) const { return S("c.r[%u]", n); }
    std::string RA0(uint32_t n) const { return n == 0 ? "0u" : S("c.r[%u]", n); }
    static std::string U(int32_t v) { return S("0x%Xu", static_cast<uint32_t>(v)); }
    std::string F0(uint32_t n) const { return S("c.f[%u].ps0", n); }

    std::string eaD(const Insn& i) const {   // d(rA)
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

    void unsupported(const Insn& i, const char* why) {
        ++st_.unsupportedInsns;
        ++st_.unsupportedKinds[opName(i.op)];
        flush();
        line(S("rt::unimplemented(c, 0x%08X, \"%s\");", i.addr, why));
    }

    void tailCall(const Insn& i, uint32_t target) {
        flush();
        if (gen_.count(target)) {
            line(S("return %s(c);", fname(target).c_str()));
        } else {
            ++st_.midFunctionTails;
            line(S("rt::unimplemented(c, 0x%08X, \"tail branch to an address that is not a recompiled function entry\");", i.addr));
        }
    }

    void emitInsn(const Insn& i);

    const Program& p_;
    const Discovery& d_;
    const std::set<uint32_t>& gen_;
    const Function& f_;
    GenStats& st_;
    std::set<uint32_t> body_;
    std::set<uint32_t> labels_;
    std::string body_text_;
    int pend_ = 0;
};

void FnEmitter::emitInsn(const Insn& i) {
    ++pend_;
    const uint32_t D = i.rD(), A = i.rA(), B = i.rB();
    const bool oe = i.oe();
    switch (i.op) {
        // ---- integer arithmetic ----
        case Op::addi: line(A == 0 ? S("%s = %s;", R(D).c_str(), U(i.simm()).c_str())
                                   : S("%s = %s + %s;", R(D).c_str(), R(A).c_str(), U(i.simm()).c_str())); break;
        case Op::addis: line(A == 0 ? S("%s = %s;", R(D).c_str(), U(i.simm() * 65536).c_str())
                                    : S("%s = %s + %s;", R(D).c_str(), R(A).c_str(), U(i.simm() * 65536).c_str())); break;
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
        case Op::ori: line(S("%s = %s | 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm())); break;
        case Op::oris: line(S("%s = %s | 0x%Xu;", R(A).c_str(), R(i.rS()).c_str(), i.uimm() << 16)); break;
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
            else { trapPoint(i); line(S("%s = rt::mfspr(c, 0x%08X, %u);", R(D).c_str(), i.addr, s)); }
            break;
        }
        case Op::mtspr: {
            const uint32_t s = i.spr();
            if (s == 8) line(S("c.lr = %s;", R(i.rS()).c_str()));
            else if (s == 9) line(S("c.ctr = %s;", R(i.rS()).c_str()));
            else { trapPoint(i); line(S("rt::mtspr(c, 0x%08X, %u, %s);", i.addr, s, R(i.rS()).c_str())); }
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
            flush(); line(S("rt::privileged(c, 0x%08X, \"%s\");", i.addr, opName(i.op))); break;
        case Op::tw: case Op::twi: unsupported(i, "trap instruction"); break;
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
                if (gen_.count(t)) line(S("AWL_CALL(c, 0x%08X, 0x%08X, %s);", i.addr, t, fname(t).c_str()));
                else { ++st_.unsupportedInsns; ++st_.unsupportedKinds["bl-to-non-function"]; line(S("rt::unimplemented(c, 0x%08X, \"call to an address that is not a recompiled function\");", i.addr)); }
            } else if (body_.count(t) && !(t != f_.start && gen_.count(t))) {
                flush();
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
                if (t == i.addr + 4) { line(S("c.lr = 0x%08X;", i.addr + 4)); break; }   // bcl 20,31,$+4: read the pc
                unsupported(i, "conditional call");
                break;
            }
            const std::string cnd = cond.empty() ? "true" : cond;
            if (body_.count(t) && !(t != f_.start && gen_.count(t))) {
                line(S("if (%s) goto L_%08X;", cnd.c_str(), t));
            } else if (gen_.count(t)) {
                line(S("if (%s) return %s(c);", cnd.c_str(), fname(t).c_str()));
            } else {
                ++st_.midFunctionTails;
                line(S("if (%s) rt::unimplemented(c, 0x%08X, \"conditional branch out of the function body\");", cnd.c_str(), i.addr));
            }
            break;
        }
        case Op::bclr: {
            if (i.lk()) {
                trapPoint(i);
                const std::string cond = condition(i);
                if (!cond.empty()) { unsupported(i, "conditional blrl"); break; }
                line("{ const uint32_t target = c.lr;");
                line(S("  rt::callIndirect(c, 0x%08X, target); }", i.addr));
            } else {
                flush();
                const std::string cond = condition(i);
                if (cond.empty()) { line(S("rt::ret(c, 0x%08X);", i.addr)); line("return;"); }
                else line(S("if (%s) { rt::ret(c, 0x%08X); return; }", cond.c_str(), i.addr));
            }
            break;
        }
        case Op::bcctr: {
            if (i.lk()) {
                trapPoint(i);
                const std::string cond = condition(i);
                if (!cond.empty()) { unsupported(i, "conditional bctrl"); break; }
                line(S("rt::callIndirect(c, 0x%08X, c.ctr);", i.addr));
            } else if (isIndirectJump(i)) {
                flush();
                const IndirectJump* j = jumpAt(i.addr);
                if (!j || j->kind == IndirectKind::Unresolved || j->kind == IndirectKind::Conditional) {
                    ++st_.unresolvedBctr;
                    line(S("rt::unresolvedJump(c, 0x%08X);", i.addr));
                } else if (j->kind == IndirectKind::JumpTableBounded || j->kind == IndirectKind::JumpTableScanned) {
                    ++st_.jumpTables;
                    line("switch (c.ctr) {");
                    std::set<uint32_t> done;
                    for (uint32_t t : j->targets) {
                        if (!done.insert(t).second) continue;
                        line(S("case 0x%08X: goto L_%08X;", t, t), true);
                    }
                    line(S("default: rt::badJump(c, 0x%08X, c.ctr);", i.addr), true);
                    line("}");
                } else if (j->kind == IndirectKind::ConstTailJump) {
                    tailCall(i, j->constTarget);
                } else {   // IndirectTailCall
                    line(S("rt::tailIndirect(c, 0x%08X, c.ctr);", i.addr));
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

std::set<uint32_t> bootClosure(const Program& prog, const Discovery& d) {
    std::set<uint32_t> seen;
    std::vector<uint32_t> work{prog.dol->entry};
    while (!work.empty()) {
        const uint32_t s = work.back();
        work.pop_back();
        if (!seen.insert(s).second) continue;
        auto it = d.funcs.find(s);
        if (it == d.funcs.end()) continue;
        const Function& f = it->second;
        if (f.platform.any()) continue;    // platform boundary: emitted as a stub, not traversed
        for (uint32_t t : f.calls) work.push_back(t);
        for (uint32_t t : f.tailCalls) work.push_back(t);
        if (f.fallsInto) work.push_back(f.fallsInto);
        for (const auto& j : d.indirect) {
            if (j.funcStart == s && j.kind == IndirectKind::ConstTailJump) work.push_back(j.constTarget);
        }
    }
    return seen;
}

std::string evidenceText(const Function& f) {
    std::string e;
    for (const auto& k : f.platform.privilegedKinds) { if (!e.empty()) e += "; "; e += k; }
    for (const auto& [b, n] : f.platform.mmio) { if (!e.empty()) e += "; "; e += "MMIO " + b + " x" + std::to_string(n); }
    return e;
}

}  // namespace

int generateCode(const Program& prog, const Discovery& d, const std::string& outDir, bool full) {
    namespace fs = std::filesystem;
    std::set<uint32_t> gen;
    if (full) {
        for (const auto& [a, f] : d.funcs) { (void)f; gen.insert(a); }
    } else {
        gen = bootClosure(prog, d);
        for (auto it = gen.begin(); it != gen.end();) {
            if (!d.funcs.count(*it)) it = gen.erase(it); else ++it;
        }
    }
    fs::create_directories(outDir);
    GenStats st;
    std::vector<std::string> files;
    std::string chunk;
    int chunkIndex = 0;
    const char* prelude =
        "// Generated by awl-recomp from the local DOL. Derived from game bytes: keep out of version control.\n"
        "#include \"gen_decls.h\"\n#include <cmath>\nnamespace rt = awl::rt;\nusing namespace awl::rt;\n\n";
    auto flushChunk = [&]() {
        if (chunk.empty()) return;
        char name[32];
        std::snprintf(name, sizeof name, "gen_%03d.cpp", chunkIndex++);
        std::ofstream o(fs::path(outDir) / name, std::ios::binary);
        o << prelude << chunk;
        files.push_back(name);
        chunk.clear();
    };
    std::string decls = "#pragma once\n#include \"guest.h\"\n";
    for (uint32_t a : gen) decls += S("void %s(awl::rt::Ctx& c);\n", fname(a).c_str());
    {
        std::ofstream o(fs::path(outDir) / "gen_decls.h", std::ios::binary);
        o << decls;
    }
    for (uint32_t a : gen) {
        const Function& f = d.funcs.at(a);
        ++st.functions;
        if (f.platform.any() && !(a == prog.dol->entry)) {
            ++st.stubs;
            chunk += S("void %s(Ctx& c) { rt::platformBoundary(c, 0x%08X, \"%s\"); }\n\n", fname(a).c_str(), a, evidenceText(f).c_str());
        } else {
            FnEmitter e(prog, d, gen, f, st);
            chunk += e.emit();
        }
        if (chunk.size() > (1u << 20)) flushChunk();
    }
    flushChunk();
    // Dispatch table
    {
        std::string t = "#include \"gen_decls.h\"\nnamespace awl::rt {\nconst FnEntry kFunctions[] = {\n";
        for (uint32_t a : gen) t += S("    {0x%08X, %s},\n", a, fname(a).c_str());
        t += S("};\nconst uint32_t kFunctionCount = %zu;\n}\n", gen.size());
        std::ofstream o(fs::path(outDir) / "gen_dispatch.cpp", std::ios::binary);
        o << t;
        files.push_back("gen_dispatch.cpp");
    }
    {
        std::ofstream o(fs::path(outDir) / "sources.cmake", std::ios::binary);
        o << "set(AWL_RECOMP_GEN_SOURCES\n";
        for (const auto& f : files) o << "    \"${AWL_RECOMP_GEN_DIR}/" << f << "\"\n";
        o << ")\n";
    }
    std::printf("generated %zu functions (%zu platform stubs), %zu instructions into %zu files under %s\n", st.functions, st.stubs,
                st.instructions, files.size(), outDir.c_str());
    std::printf("  jump tables emitted: %zu; unresolved bctr: %zu; tails/branches to non-function targets: %zu; bodies that fall off: %zu\n",
                st.jumpTables, st.unresolvedBctr, st.midFunctionTails, st.fellOff);
    std::printf("  untranslated instructions: %zu\n", st.unsupportedInsns);
    for (const auto& [k, v] : st.unsupportedKinds) std::printf("    %-24s %zu\n", k.c_str(), v);
    return 0;
}

}  // namespace awl::recomp
