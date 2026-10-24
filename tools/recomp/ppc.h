// PowerPC 750CL / Gekko instruction decoder (user + supervisor set, paired singles included).
// Encodings come from the public PowerPC Programming Environments Manual and the Gekko user manual.
#pragma once

#include <cstdint>
#include <string>

namespace awl::recomp {

#define AWL_PPC_OPS(X) \
    X(invalid) \
    X(add) X(addc) X(adde) X(addi) X(addic) X(addic_rc) X(addis) X(addme) X(addze) \
    X(and_) X(andc) X(andi_rc) X(andis_rc) \
    X(b) X(bc) X(bcctr) X(bclr) \
    X(cmp) X(cmpi) X(cmpl) X(cmpli) X(cntlzw) \
    X(crand) X(crandc) X(creqv) X(crnand) X(crnor) X(cror) X(crorc) X(crxor) \
    X(dcbf) X(dcbi) X(dcbst) X(dcbt) X(dcbtst) X(dcbz) X(dcbz_l) \
    X(divw) X(divwu) X(eciwx) X(ecowx) X(eieio) X(eqv) X(extsb) X(extsh) \
    X(fabs) X(fadd) X(fadds) X(fcmpo) X(fcmpu) X(fctiw) X(fctiwz) X(fdiv) X(fdivs) \
    X(fmadd) X(fmadds) X(fmr) X(fmsub) X(fmsubs) X(fmul) X(fmuls) X(fnabs) X(fneg) \
    X(fnmadd) X(fnmadds) X(fnmsub) X(fnmsubs) X(fres) X(frsp) X(frsqrte) X(fsel) \
    X(fsub) X(fsubs) X(icbi) X(isync) \
    X(lbz) X(lbzu) X(lbzux) X(lbzx) X(lfd) X(lfdu) X(lfdux) X(lfdx) \
    X(lfs) X(lfsu) X(lfsux) X(lfsx) X(lha) X(lhau) X(lhaux) X(lhax) X(lhbrx) \
    X(lhz) X(lhzu) X(lhzux) X(lhzx) X(lmw) X(lswi) X(lswx) X(lwarx) X(lwbrx) \
    X(lwz) X(lwzu) X(lwzux) X(lwzx) \
    X(mcrf) X(mcrfs) X(mcrxr) X(mfcr) X(mffs) X(mfmsr) X(mfspr) X(mfsr) X(mfsrin) X(mftb) \
    X(mtcrf) X(mtfsb0) X(mtfsb1) X(mtfsf) X(mtfsfi) X(mtmsr) X(mtspr) X(mtsr) X(mtsrin) \
    X(mulhw) X(mulhwu) X(mulli) X(mullw) X(nand) X(neg) X(nor) X(or_) X(orc) X(ori) X(oris) \
    X(psq_l) X(psq_lu) X(psq_lux) X(psq_lx) X(psq_st) X(psq_stu) X(psq_stux) X(psq_stx) \
    X(ps_abs) X(ps_add) X(ps_cmpo0) X(ps_cmpo1) X(ps_cmpu0) X(ps_cmpu1) X(ps_div) \
    X(ps_madd) X(ps_madds0) X(ps_madds1) X(ps_merge00) X(ps_merge01) X(ps_merge10) X(ps_merge11) \
    X(ps_mr) X(ps_msub) X(ps_mul) X(ps_muls0) X(ps_muls1) X(ps_nabs) X(ps_neg) X(ps_nmadd) \
    X(ps_nmsub) X(ps_res) X(ps_rsqrte) X(ps_sel) X(ps_sub) X(ps_sum0) X(ps_sum1) \
    X(rfi) X(rlwimi) X(rlwinm) X(rlwnm) X(sc) X(slw) X(sraw) X(srawi) X(srw) \
    X(stb) X(stbu) X(stbux) X(stbx) X(stfd) X(stfdu) X(stfdux) X(stfdx) X(stfiwx) \
    X(stfs) X(stfsu) X(stfsux) X(stfsx) X(sth) X(sthbrx) X(sthu) X(sthux) X(sthx) \
    X(stmw) X(stswi) X(stswx) X(stw) X(stwbrx) X(stwcx_rc) X(stwu) X(stwux) X(stwx) \
    X(subf) X(subfc) X(subfe) X(subfic) X(subfme) X(subfze) X(sync) \
    X(tlbie) X(tlbsync) X(tw) X(twi) X(xor_) X(xori) X(xoris)

enum class Op : uint16_t {
#define X(n) n,
    AWL_PPC_OPS(X)
#undef X
    Count
};

struct Insn {
    uint32_t raw = 0;
    uint32_t addr = 0;
    Op op = Op::invalid;

    // Common field extractors (IBM bit numbering mapped to LSB shifts).
    uint32_t rD() const { return (raw >> 21) & 31; }
    uint32_t rS() const { return (raw >> 21) & 31; }
    uint32_t rA() const { return (raw >> 16) & 31; }
    uint32_t rB() const { return (raw >> 11) & 31; }
    uint32_t frC() const { return (raw >> 6) & 31; }
    uint32_t sh() const { return (raw >> 11) & 31; }
    uint32_t mb() const { return (raw >> 6) & 31; }
    uint32_t me() const { return (raw >> 1) & 31; }
    int32_t simm() const { return static_cast<int16_t>(raw & 0xffff); }
    uint32_t uimm() const { return raw & 0xffff; }
    bool rc() const { return (raw & 1) != 0; }
    bool oe() const { return (raw & 0x400) != 0; }
    bool lk() const { return (raw & 1) != 0; }
    bool aa() const { return (raw & 2) != 0; }
    uint32_t crfD() const { return (raw >> 23) & 7; }
    uint32_t crfS() const { return (raw >> 18) & 7; }
    uint32_t crbD() const { return (raw >> 21) & 31; }
    uint32_t crbA() const { return (raw >> 16) & 31; }
    uint32_t crbB() const { return (raw >> 11) & 31; }
    uint32_t bo() const { return (raw >> 21) & 31; }
    uint32_t bi() const { return (raw >> 16) & 31; }
    uint32_t spr() const { return ((raw >> 16) & 31) | (((raw >> 11) & 31) << 5); }
    uint32_t crm() const { return (raw >> 12) & 0xff; }
    uint32_t fm() const { return (raw >> 17) & 0xff; }
    uint32_t sr() const { return (raw >> 16) & 15; }
    // Paired-single quantised load/store fields.
    uint32_t psqW() const { return (raw >> 15) & 1; }       // D-form (psq_l/st)
    uint32_t psqI() const { return (raw >> 12) & 7; }       // D-form
    int32_t psqD() const { return (static_cast<int32_t>(raw & 0xfff) << 20) >> 20; }
    uint32_t psqWx() const { return (raw >> 10) & 1; }      // X-form (psq_lx/stx)
    uint32_t psqIx() const { return (raw >> 7) & 7; }       // X-form

    // Branch helpers. branchTarget() is valid for b and bc only (absolute when aa()).
    int32_t branchDisp() const;
    uint32_t branchTarget() const;
};

Insn decode(uint32_t raw, uint32_t addr);

const char* opName(Op op);               // base mnemonic (lower case, no suffix)
std::string mnemonic(const Insn& i);     // full mnemonic with ./o/a/l suffixes where encoded
std::string disassemble(const Insn& i);  // mnemonic plus operands (canonical, no simplified forms)
const char* sprName(uint32_t spr);       // nullptr when the SPR number is unknown to Gekko

// Branch classification used by the control-flow analysis.
bool isUnconditionalBranch(const Insn& i);   // b (no link)
bool isCall(const Insn& i);                  // b/bc/bclr/bcctr with LK=1
bool isReturn(const Insn& i);                // bclr, BO=always, LK=0
bool isIndirectJump(const Insn& i);          // bcctr, BO=always, LK=0
bool isConditionalBranch(const Insn& i);     // bc or conditional bclr/bcctr with LK=0
bool isSupervisor(const Insn& i);            // privileged instruction
bool isModelledSupervisor(const Insn& i);    // privileged state move the spike runtime keeps in plain storage

}  // namespace awl::recomp
