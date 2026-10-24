#include "ppc.h"

#include <cstdio>

namespace awl::recomp {

namespace {

const char* const kOpNames[] = {
#define X(n) #n,
    AWL_PPC_OPS(X)
#undef X
};

Insn make(uint32_t raw, uint32_t addr, Op op) {
    Insn i;
    i.raw = raw;
    i.addr = addr;
    i.op = op;
    return i;
}

Op decodeOp4(uint32_t raw) {
    const uint32_t xo10 = (raw >> 1) & 0x3ff;
    switch (xo10) {
        case 0: return Op::ps_cmpu0;
        case 32: return Op::ps_cmpo0;
        case 64: return Op::ps_cmpu1;
        case 96: return Op::ps_cmpo1;
        case 40: return Op::ps_neg;
        case 72: return Op::ps_mr;
        case 136: return Op::ps_nabs;
        case 264: return Op::ps_abs;
        case 528: return Op::ps_merge00;
        case 560: return Op::ps_merge01;
        case 592: return Op::ps_merge10;
        case 624: return Op::ps_merge11;
        case 1014: return Op::dcbz_l;
        default: break;
    }
    switch ((raw >> 1) & 0x3f) {  // psq indexed forms use a 6-bit extended opcode (bits 25-30)
        case 6: return Op::psq_lx;
        case 7: return Op::psq_stx;
        case 38: return Op::psq_lux;
        case 39: return Op::psq_stux;
        default: break;
    }
    switch ((raw >> 1) & 0x1f) {  // A-form: 5-bit extended opcode, frC occupies bits 21-25
        case 10: return Op::ps_sum0;
        case 11: return Op::ps_sum1;
        case 12: return Op::ps_muls0;
        case 13: return Op::ps_muls1;
        case 14: return Op::ps_madds0;
        case 15: return Op::ps_madds1;
        case 18: return Op::ps_div;
        case 20: return Op::ps_sub;
        case 21: return Op::ps_add;
        case 23: return Op::ps_sel;
        case 24: return Op::ps_res;
        case 25: return Op::ps_mul;
        case 26: return Op::ps_rsqrte;
        case 28: return Op::ps_msub;
        case 29: return Op::ps_madd;
        case 30: return Op::ps_nmsub;
        case 31: return Op::ps_nmadd;
        default: break;
    }
    return Op::invalid;
}

Op decodeOp19(uint32_t raw) {
    switch ((raw >> 1) & 0x3ff) {
        case 0: return Op::mcrf;
        case 16: return Op::bclr;
        case 33: return Op::crnor;
        case 50: return Op::rfi;
        case 129: return Op::crandc;
        case 150: return Op::isync;
        case 193: return Op::crxor;
        case 225: return Op::crnand;
        case 257: return Op::crand;
        case 289: return Op::creqv;
        case 417: return Op::crorc;
        case 449: return Op::cror;
        case 528: return Op::bcctr;
        default: return Op::invalid;
    }
}

Op decodeOp31(uint32_t raw) {
    // XO-form arithmetic: the OE bit (bit 21) is part of the extended opcode field, so match on 9 bits.
    switch ((raw >> 1) & 0x1ff) {
        case 8: return Op::subfc;
        case 10: return Op::addc;
        case 11: return Op::mulhwu;
        case 40: return Op::subf;
        case 75: return Op::mulhw;
        case 104: return Op::neg;
        case 136: return Op::subfe;
        case 138: return Op::adde;
        case 200: return Op::subfze;
        case 202: return Op::addze;
        case 232: return Op::subfme;
        case 234: return Op::addme;
        case 235: return Op::mullw;
        case 266: return Op::add;
        case 459: return Op::divwu;
        case 491: return Op::divw;
        default: break;
    }
    switch ((raw >> 1) & 0x3ff) {
        case 0: return Op::cmp;
        case 4: return Op::tw;
        case 19: return Op::mfcr;
        case 20: return Op::lwarx;
        case 23: return Op::lwzx;
        case 24: return Op::slw;
        case 26: return Op::cntlzw;
        case 28: return Op::and_;
        case 32: return Op::cmpl;
        case 54: return Op::dcbst;
        case 55: return Op::lwzux;
        case 60: return Op::andc;
        case 83: return Op::mfmsr;
        case 86: return Op::dcbf;
        case 87: return Op::lbzx;
        case 119: return Op::lbzux;
        case 124: return Op::nor;
        case 144: return Op::mtcrf;
        case 146: return Op::mtmsr;
        case 150: return Op::stwcx_rc;
        case 151: return Op::stwx;
        case 183: return Op::stwux;
        case 210: return Op::mtsr;
        case 215: return Op::stbx;
        case 242: return Op::mtsrin;
        case 246: return Op::dcbtst;
        case 247: return Op::stbux;
        case 278: return Op::dcbt;
        case 279: return Op::lhzx;
        case 284: return Op::eqv;
        case 306: return Op::tlbie;
        case 310: return Op::eciwx;
        case 311: return Op::lhzux;
        case 316: return Op::xor_;
        case 339: return Op::mfspr;
        case 343: return Op::lhax;
        case 371: return Op::mftb;
        case 375: return Op::lhaux;
        case 407: return Op::sthx;
        case 412: return Op::orc;
        case 438: return Op::ecowx;
        case 439: return Op::sthux;
        case 444: return Op::or_;
        case 467: return Op::mtspr;
        case 470: return Op::dcbi;
        case 476: return Op::nand;
        case 512: return Op::mcrxr;
        case 533: return Op::lswx;
        case 534: return Op::lwbrx;
        case 535: return Op::lfsx;
        case 536: return Op::srw;
        case 566: return Op::tlbsync;
        case 567: return Op::lfsux;
        case 595: return Op::mfsr;
        case 597: return Op::lswi;
        case 598: return Op::sync;
        case 599: return Op::lfdx;
        case 631: return Op::lfdux;
        case 659: return Op::mfsrin;
        case 661: return Op::stswx;
        case 662: return Op::stwbrx;
        case 663: return Op::stfsx;
        case 695: return Op::stfsux;
        case 725: return Op::stswi;
        case 727: return Op::stfdx;
        case 759: return Op::stfdux;
        case 790: return Op::lhbrx;
        case 792: return Op::sraw;
        case 824: return Op::srawi;
        case 854: return Op::eieio;
        case 918: return Op::sthbrx;
        case 922: return Op::extsh;
        case 954: return Op::extsb;
        case 982: return Op::icbi;
        case 983: return Op::stfiwx;
        case 1014: return Op::dcbz;
        default: return Op::invalid;
    }
}

Op decodeOp59(uint32_t raw) {
    switch ((raw >> 1) & 0x1f) {
        case 18: return Op::fdivs;
        case 20: return Op::fsubs;
        case 21: return Op::fadds;
        case 24: return Op::fres;
        case 25: return Op::fmuls;
        case 28: return Op::fmsubs;
        case 29: return Op::fmadds;
        case 30: return Op::fnmsubs;
        case 31: return Op::fnmadds;
        default: return Op::invalid;
    }
}

Op decodeOp63(uint32_t raw) {
    switch ((raw >> 1) & 0x1f) {
        case 18: return Op::fdiv;
        case 20: return Op::fsub;
        case 21: return Op::fadd;
        case 23: return Op::fsel;
        case 25: return Op::fmul;
        case 26: return Op::frsqrte;
        case 28: return Op::fmsub;
        case 29: return Op::fmadd;
        case 30: return Op::fnmsub;
        case 31: return Op::fnmadd;
        default: break;
    }
    switch ((raw >> 1) & 0x3ff) {
        case 0: return Op::fcmpu;
        case 12: return Op::frsp;
        case 14: return Op::fctiw;
        case 15: return Op::fctiwz;
        case 32: return Op::fcmpo;
        case 38: return Op::mtfsb1;
        case 40: return Op::fneg;
        case 64: return Op::mcrfs;
        case 70: return Op::mtfsb0;
        case 72: return Op::fmr;
        case 134: return Op::mtfsfi;
        case 136: return Op::fnabs;
        case 264: return Op::fabs;
        case 583: return Op::mffs;
        case 711: return Op::mtfsf;
        default: return Op::invalid;
    }
}

bool hasRc(Op op) {
    switch (op) {
        case Op::add: case Op::addc: case Op::adde: case Op::addme: case Op::addze:
        case Op::and_: case Op::andc: case Op::cntlzw: case Op::divw: case Op::divwu:
        case Op::eqv: case Op::extsb: case Op::extsh:
        case Op::fabs: case Op::fadd: case Op::fadds: case Op::fctiw: case Op::fctiwz:
        case Op::fdiv: case Op::fdivs: case Op::fmadd: case Op::fmadds: case Op::fmr:
        case Op::fmsub: case Op::fmsubs: case Op::fmul: case Op::fmuls: case Op::fnabs:
        case Op::fneg: case Op::fnmadd: case Op::fnmadds: case Op::fnmsub: case Op::fnmsubs:
        case Op::fres: case Op::frsp: case Op::frsqrte: case Op::fsel: case Op::fsub: case Op::fsubs:
        case Op::mffs: case Op::mtfsb0: case Op::mtfsb1: case Op::mtfsf: case Op::mtfsfi:
        case Op::mulhw: case Op::mulhwu: case Op::mullw: case Op::nand: case Op::neg: case Op::nor:
        case Op::or_: case Op::orc: case Op::rlwimi: case Op::rlwinm: case Op::rlwnm:
        case Op::slw: case Op::sraw: case Op::srawi: case Op::srw:
        case Op::subf: case Op::subfc: case Op::subfe: case Op::subfme: case Op::subfze: case Op::xor_:
        case Op::ps_abs: case Op::ps_add: case Op::ps_div: case Op::ps_madd: case Op::ps_madds0:
        case Op::ps_madds1: case Op::ps_merge00: case Op::ps_merge01: case Op::ps_merge10:
        case Op::ps_merge11: case Op::ps_mr: case Op::ps_msub: case Op::ps_mul: case Op::ps_muls0:
        case Op::ps_muls1: case Op::ps_nabs: case Op::ps_neg: case Op::ps_nmadd: case Op::ps_nmsub:
        case Op::ps_res: case Op::ps_rsqrte: case Op::ps_sel: case Op::ps_sub: case Op::ps_sum0:
        case Op::ps_sum1:
            return true;
        default:
            return false;
    }
}

bool hasOe(Op op) {
    switch (op) {
        case Op::add: case Op::addc: case Op::adde: case Op::addme: case Op::addze:
        case Op::divw: case Op::divwu: case Op::mullw: case Op::neg: case Op::subf:
        case Op::subfc: case Op::subfe: case Op::subfme: case Op::subfze:
            return true;
        default:
            return false;
    }
}

}  // namespace

int32_t Insn::branchDisp() const {
    if (op == Op::b) return static_cast<int32_t>(raw & 0x03fffffc) << 6 >> 6;
    return static_cast<int32_t>(static_cast<int16_t>(raw & 0xfffc));
}

uint32_t Insn::branchTarget() const {
    const int32_t d = branchDisp();
    return aa() ? static_cast<uint32_t>(d) : addr + static_cast<uint32_t>(d);
}

Insn decode(uint32_t raw, uint32_t addr) {
    Op op = Op::invalid;
    switch (raw >> 26) {
        case 3: op = Op::twi; break;
        case 4: op = decodeOp4(raw); break;
        case 7: op = Op::mulli; break;
        case 8: op = Op::subfic; break;
        case 10: op = Op::cmpli; break;
        case 11: op = Op::cmpi; break;
        case 12: op = Op::addic; break;
        case 13: op = Op::addic_rc; break;
        case 14: op = Op::addi; break;
        case 15: op = Op::addis; break;
        case 16: op = Op::bc; break;
        case 17: op = (raw & 2) ? Op::sc : Op::invalid; break;
        case 18: op = Op::b; break;
        case 19: op = decodeOp19(raw); break;
        case 20: op = Op::rlwimi; break;
        case 21: op = Op::rlwinm; break;
        case 23: op = Op::rlwnm; break;
        case 24: op = Op::ori; break;
        case 25: op = Op::oris; break;
        case 26: op = Op::xori; break;
        case 27: op = Op::xoris; break;
        case 28: op = Op::andi_rc; break;
        case 29: op = Op::andis_rc; break;
        case 31: op = decodeOp31(raw); break;
        case 32: op = Op::lwz; break;
        case 33: op = Op::lwzu; break;
        case 34: op = Op::lbz; break;
        case 35: op = Op::lbzu; break;
        case 36: op = Op::stw; break;
        case 37: op = Op::stwu; break;
        case 38: op = Op::stb; break;
        case 39: op = Op::stbu; break;
        case 40: op = Op::lhz; break;
        case 41: op = Op::lhzu; break;
        case 42: op = Op::lha; break;
        case 43: op = Op::lhau; break;
        case 44: op = Op::sth; break;
        case 45: op = Op::sthu; break;
        case 46: op = Op::lmw; break;
        case 47: op = Op::stmw; break;
        case 48: op = Op::lfs; break;
        case 49: op = Op::lfsu; break;
        case 50: op = Op::lfd; break;
        case 51: op = Op::lfdu; break;
        case 52: op = Op::stfs; break;
        case 53: op = Op::stfsu; break;
        case 54: op = Op::stfd; break;
        case 55: op = Op::stfdu; break;
        case 56: op = Op::psq_l; break;
        case 57: op = Op::psq_lu; break;
        case 59: op = decodeOp59(raw); break;
        case 60: op = Op::psq_st; break;
        case 61: op = Op::psq_stu; break;
        case 63: op = decodeOp63(raw); break;
        default: break;
    }
    return make(raw, addr, op);
}

const char* opName(Op op) { return kOpNames[static_cast<size_t>(op)]; }

std::string mnemonic(const Insn& i) {
    std::string m = opName(i.op);
    if (!m.empty() && m.back() == '_') m.pop_back();           // and_/or_/xor_ -> and/or/xor
    if (i.op == Op::addic_rc) return "addic.";
    if (i.op == Op::andi_rc) return "andi.";
    if (i.op == Op::andis_rc) return "andis.";
    if (i.op == Op::stwcx_rc) return "stwcx.";
    if (hasOe(i.op) && i.oe()) m += "o";
    if (hasRc(i.op) && i.rc()) m += ".";
    if (i.op == Op::b || i.op == Op::bc) {
        if (i.lk()) m += "l";
        if (i.aa()) m += "a";
    } else if (i.op == Op::bclr || i.op == Op::bcctr) {
        if (i.lk()) m += "l";
    }
    return m;
}

const char* sprName(uint32_t spr) {
    switch (spr) {
        case 1: return "XER";
        case 8: return "LR";
        case 9: return "CTR";
        case 18: return "DSISR";
        case 19: return "DAR";
        case 22: return "DEC";
        case 25: return "SDR1";
        case 26: return "SRR0";
        case 27: return "SRR1";
        case 268: return "TBL";
        case 269: return "TBU";
        case 272: return "SPRG0";
        case 273: return "SPRG1";
        case 274: return "SPRG2";
        case 275: return "SPRG3";
        case 282: return "EAR";
        case 284: return "TBL_W";
        case 285: return "TBU_W";
        case 287: return "PVR";
        case 528: return "IBAT0U"; case 529: return "IBAT0L"; case 530: return "IBAT1U"; case 531: return "IBAT1L";
        case 532: return "IBAT2U"; case 533: return "IBAT2L"; case 534: return "IBAT3U"; case 535: return "IBAT3L";
        case 536: return "DBAT0U"; case 537: return "DBAT0L"; case 538: return "DBAT1U"; case 539: return "DBAT1L";
        case 540: return "DBAT2U"; case 541: return "DBAT2L"; case 542: return "DBAT3U"; case 543: return "DBAT3L";
        case 560: return "IBAT4U"; case 561: return "IBAT4L"; case 562: return "IBAT5U"; case 563: return "IBAT5L";
        case 564: return "IBAT6U"; case 565: return "IBAT6L"; case 566: return "IBAT7U"; case 567: return "IBAT7L";
        case 568: return "DBAT4U"; case 569: return "DBAT4L"; case 570: return "DBAT5U"; case 571: return "DBAT5L";
        case 572: return "DBAT6U"; case 573: return "DBAT6L"; case 574: return "DBAT7U"; case 575: return "DBAT7L";
        case 912: return "GQR0"; case 913: return "GQR1"; case 914: return "GQR2"; case 915: return "GQR3";
        case 916: return "GQR4"; case 917: return "GQR5"; case 918: return "GQR6"; case 919: return "GQR7";
        case 920: return "HID2";
        case 921: return "WPAR";
        case 922: return "DMA_U";
        case 923: return "DMA_L";
        case 936: return "UMMCR0"; case 937: return "UPMC1"; case 938: return "UPMC2";
        case 939: return "USIA"; case 940: return "UMMCR1"; case 941: return "UPMC3"; case 942: return "UPMC4";
        case 952: return "MMCR0"; case 953: return "PMC1"; case 954: return "PMC2"; case 955: return "SIA";
        case 956: return "MMCR1"; case 957: return "PMC3"; case 958: return "PMC4";
        case 959: return "SDA";
        case 1008: return "HID0";
        case 1009: return "HID1";
        case 1010: return "IABR";
        case 1013: return "DABR";
        case 1017: return "L2CR";
        case 1019: return "ICTC";
        case 1020: return "THRM1"; case 1021: return "THRM2"; case 1022: return "THRM3";
        default: return nullptr;
    }
}

namespace {

std::string r(uint32_t n) { return "r" + std::to_string(n); }
std::string f(uint32_t n) { return "f" + std::to_string(n); }
std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%x", v); return b; }
std::string sdec(int32_t v) { return std::to_string(v); }
std::string memD(const Insn& i, bool fp) {
    return (fp ? f(i.rD()) : r(i.rD())) + "," + sdec(i.simm()) + "(" + r(i.rA()) + ")";
}
std::string idx(const Insn& i, bool fp) {
    return (fp ? f(i.rD()) : r(i.rD())) + "," + r(i.rA()) + "," + r(i.rB());
}

}  // namespace

std::string disassemble(const Insn& i) {
    std::string m = mnemonic(i);
    std::string o;
    switch (i.op) {
        case Op::addi: case Op::addic: case Op::addic_rc: case Op::addis: case Op::mulli: case Op::subfic:
            o = r(i.rD()) + "," + r(i.rA()) + "," + sdec(i.simm()); break;
        case Op::ori: case Op::oris: case Op::xori: case Op::xoris: case Op::andi_rc: case Op::andis_rc:
            o = r(i.rA()) + "," + r(i.rS()) + "," + hex(i.uimm()); break;
        case Op::cmpi: o = "cr" + std::to_string(i.crfD()) + "," + r(i.rA()) + "," + sdec(i.simm()); break;
        case Op::cmpli: o = "cr" + std::to_string(i.crfD()) + "," + r(i.rA()) + "," + hex(i.uimm()); break;
        case Op::cmp: case Op::cmpl: o = "cr" + std::to_string(i.crfD()) + "," + r(i.rA()) + "," + r(i.rB()); break;
        case Op::twi: o = std::to_string(i.rD()) + "," + r(i.rA()) + "," + sdec(i.simm()); break;
        case Op::tw: o = std::to_string(i.rD()) + "," + r(i.rA()) + "," + r(i.rB()); break;
        case Op::lbz: case Op::lbzu: case Op::lhz: case Op::lhzu: case Op::lha: case Op::lhau:
        case Op::lwz: case Op::lwzu: case Op::lmw: case Op::stb: case Op::stbu: case Op::sth:
        case Op::sthu: case Op::stw: case Op::stwu: case Op::stmw:
            o = memD(i, false); break;
        case Op::lfs: case Op::lfsu: case Op::lfd: case Op::lfdu: case Op::stfs: case Op::stfsu:
        case Op::stfd: case Op::stfdu:
            o = memD(i, true); break;
        case Op::psq_l: case Op::psq_lu: case Op::psq_st: case Op::psq_stu:
            o = f(i.rD()) + "," + sdec(i.psqD()) + "(" + r(i.rA()) + ")," + std::to_string(i.psqW()) + "," + std::to_string(i.psqI()); break;
        case Op::psq_lx: case Op::psq_lux: case Op::psq_stx: case Op::psq_stux:
            o = idx(i, true) + "," + std::to_string(i.psqWx()) + "," + std::to_string(i.psqIx()); break;
        case Op::lbzx: case Op::lbzux: case Op::lhzx: case Op::lhzux: case Op::lhax: case Op::lhaux:
        case Op::lhbrx: case Op::lwarx: case Op::lwbrx: case Op::lwzx: case Op::lwzux: case Op::stbx:
        case Op::stbux: case Op::sthx: case Op::sthux: case Op::sthbrx: case Op::stwbrx: case Op::stwcx_rc:
        case Op::stwx: case Op::stwux: case Op::eciwx: case Op::ecowx: case Op::lswx: case Op::stswx:
            o = idx(i, false); break;
        case Op::lfsx: case Op::lfsux: case Op::lfdx: case Op::lfdux: case Op::stfsx: case Op::stfsux:
        case Op::stfdx: case Op::stfdux: case Op::stfiwx:
            o = idx(i, true); break;
        case Op::lswi: case Op::stswi: o = r(i.rD()) + "," + r(i.rA()) + "," + std::to_string(i.rB()); break;
        case Op::add: case Op::addc: case Op::adde: case Op::divw: case Op::divwu: case Op::mulhw:
        case Op::mulhwu: case Op::mullw: case Op::subf: case Op::subfc: case Op::subfe:
            o = r(i.rD()) + "," + r(i.rA()) + "," + r(i.rB()); break;
        case Op::addme: case Op::addze: case Op::neg: case Op::subfme: case Op::subfze:
            o = r(i.rD()) + "," + r(i.rA()); break;
        case Op::and_: case Op::andc: case Op::eqv: case Op::nand: case Op::nor: case Op::or_:
        case Op::orc: case Op::xor_: case Op::slw: case Op::srw: case Op::sraw:
            o = r(i.rA()) + "," + r(i.rS()) + "," + r(i.rB()); break;
        case Op::cntlzw: case Op::extsb: case Op::extsh: o = r(i.rA()) + "," + r(i.rS()); break;
        case Op::srawi: o = r(i.rA()) + "," + r(i.rS()) + "," + std::to_string(i.sh()); break;
        case Op::rlwinm: case Op::rlwimi:
            o = r(i.rA()) + "," + r(i.rS()) + "," + std::to_string(i.sh()) + "," + std::to_string(i.mb()) + "," + std::to_string(i.me()); break;
        case Op::rlwnm:
            o = r(i.rA()) + "," + r(i.rS()) + "," + r(i.rB()) + "," + std::to_string(i.mb()) + "," + std::to_string(i.me()); break;
        case Op::b: o = hex(i.branchTarget()); break;
        case Op::bc: o = std::to_string(i.bo()) + "," + std::to_string(i.bi()) + "," + hex(i.branchTarget()); break;
        case Op::bclr: case Op::bcctr: o = std::to_string(i.bo()) + "," + std::to_string(i.bi()); break;
        case Op::crand: case Op::crandc: case Op::creqv: case Op::crnand: case Op::crnor: case Op::cror:
        case Op::crorc: case Op::crxor:
            o = std::to_string(i.crbD()) + "," + std::to_string(i.crbA()) + "," + std::to_string(i.crbB()); break;
        case Op::mcrf: case Op::mcrfs: o = "cr" + std::to_string(i.crfD()) + ",cr" + std::to_string(i.crfS()); break;
        case Op::mcrxr: o = "cr" + std::to_string(i.crfD()); break;
        case Op::mfcr: case Op::mfmsr: o = r(i.rD()); break;
        case Op::mtmsr: o = r(i.rS()); break;
        case Op::mtcrf: o = hex(i.crm()) + "," + r(i.rS()); break;
        case Op::mfspr: case Op::mftb: {
            const uint32_t s = i.spr();
            const char* n = sprName(s);
            o = r(i.rD()) + "," + (n ? std::string(n) : std::to_string(s)); break;
        }
        case Op::mtspr: {
            const uint32_t s = i.spr();
            const char* n = sprName(s);
            o = (n ? std::string(n) : std::to_string(s)) + "," + r(i.rS()); break;
        }
        case Op::mfsr: o = r(i.rD()) + "," + std::to_string(i.sr()); break;
        case Op::mtsr: o = std::to_string(i.sr()) + "," + r(i.rS()); break;
        case Op::mfsrin: o = r(i.rD()) + "," + r(i.rB()); break;
        case Op::mtsrin: o = r(i.rS()) + "," + r(i.rB()); break;
        case Op::dcbf: case Op::dcbi: case Op::dcbst: case Op::dcbt: case Op::dcbtst: case Op::dcbz:
        case Op::dcbz_l: case Op::icbi:
            o = r(i.rA()) + "," + r(i.rB()); break;
        case Op::tlbie: o = r(i.rB()); break;
        case Op::fadd: case Op::fadds: case Op::fdiv: case Op::fdivs: case Op::fsub: case Op::fsubs:
        case Op::ps_add: case Op::ps_div: case Op::ps_sub: case Op::ps_merge00: case Op::ps_merge01:
        case Op::ps_merge10: case Op::ps_merge11:
            o = f(i.rD()) + "," + f(i.rA()) + "," + f(i.rB()); break;
        case Op::fmul: case Op::fmuls: case Op::ps_mul: case Op::ps_muls0: case Op::ps_muls1:
            o = f(i.rD()) + "," + f(i.rA()) + "," + f(i.frC()); break;
        case Op::fmadd: case Op::fmadds: case Op::fmsub: case Op::fmsubs: case Op::fnmadd: case Op::fnmadds:
        case Op::fnmsub: case Op::fnmsubs: case Op::fsel: case Op::ps_madd: case Op::ps_madds0:
        case Op::ps_madds1: case Op::ps_msub: case Op::ps_nmadd: case Op::ps_nmsub: case Op::ps_sel:
        case Op::ps_sum0: case Op::ps_sum1:
            o = f(i.rD()) + "," + f(i.rA()) + "," + f(i.frC()) + "," + f(i.rB()); break;
        case Op::fabs: case Op::fnabs: case Op::fneg: case Op::fmr: case Op::fres: case Op::frsp:
        case Op::frsqrte: case Op::fctiw: case Op::fctiwz: case Op::ps_abs: case Op::ps_nabs:
        case Op::ps_neg: case Op::ps_mr: case Op::ps_res: case Op::ps_rsqrte:
            o = f(i.rD()) + "," + f(i.rB()); break;
        case Op::fcmpo: case Op::fcmpu: case Op::ps_cmpo0: case Op::ps_cmpo1: case Op::ps_cmpu0: case Op::ps_cmpu1:
            o = "cr" + std::to_string(i.crfD()) + "," + f(i.rA()) + "," + f(i.rB()); break;
        case Op::mffs: o = f(i.rD()); break;
        case Op::mtfsf: o = hex(i.fm()) + "," + f(i.rB()); break;
        case Op::mtfsfi: o = "cr" + std::to_string(i.crfD()) + "," + std::to_string((i.raw >> 12) & 15); break;
        case Op::mtfsb0: case Op::mtfsb1: o = std::to_string(i.crbD()); break;
        default: break;
    }
    if (i.op == Op::invalid) {
        char b[32];
        std::snprintf(b, sizeof b, ".long 0x%08x", i.raw);
        return b;
    }
    return o.empty() ? m : m + " " + o;
}

bool isUnconditionalBranch(const Insn& i) { return i.op == Op::b && !i.lk(); }
bool isCall(const Insn& i) {
    return (i.op == Op::b || i.op == Op::bc || i.op == Op::bclr || i.op == Op::bcctr) && i.lk();
}
bool isReturn(const Insn& i) { return i.op == Op::bclr && !i.lk() && (i.bo() & 0x14) == 0x14; }
bool isIndirectJump(const Insn& i) { return i.op == Op::bcctr && !i.lk() && (i.bo() & 0x14) == 0x14; }
bool isConditionalBranch(const Insn& i) {
    if (i.lk()) return false;
    if (i.op == Op::bc) return true;
    return (i.op == Op::bclr || i.op == Op::bcctr) && (i.bo() & 0x14) != 0x14;
}

bool isSupervisor(const Insn& i) {
    switch (i.op) {
        case Op::mfmsr: case Op::mtmsr: case Op::mfsr: case Op::mtsr: case Op::mfsrin: case Op::mtsrin:
        case Op::rfi: case Op::tlbie: case Op::tlbsync: case Op::sc: case Op::dcbi:
            return true;
        case Op::mfspr: case Op::mtspr: {
            const uint32_t s = i.spr();
            // User-level SPRs: XER, LR, CTR, the user performance monitor SPRs and the GQRs/WPAR/DMA
            // registers (which the SDK also reaches from game code).
            if (s == 1 || s == 8 || s == 9) return false;
            if (s >= 912 && s <= 923) return false;
            if (s >= 936 && s <= 943) return false;
            return true;
        }
        default:
            return false;
    }
}

bool isModelledSupervisor(const Insn& i) {
    return isSupervisor(i) && i.op != Op::rfi && i.op != Op::sc;
}

}  // namespace awl::recomp
