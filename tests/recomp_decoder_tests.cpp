// Decoder tests for the static-recompilation spike.
// Every word below is assembled by hand from the instruction formats in the PowerPC Programming
// Environments Manual (and the Gekko User's Manual for the paired-single extensions): the builders place
// the primary opcode, register fields and extended opcode into their architectural bit positions
// (IBM bit 0 = most significant), independently of the decoder under test.
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>

#include "ppc.h"

using namespace awl::recomp;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

// D-form: opcode | rT(6-10) | rA(11-15) | imm16
constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}
// X/XO/XL-form: opcode | f1(6-10) | f2(11-15) | f3(16-20) | xo(21-30) | rc(31)
constexpr uint32_t X(uint32_t op, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t xo, uint32_t rc = 0) {
    return (op << 26) | (f1 << 21) | (f2 << 16) | (f3 << 11) | (xo << 1) | rc;
}
// A-form: opcode | frD | frA | frB | frC(21-25) | xo(26-30) | rc
constexpr uint32_t A(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c, uint32_t xo, uint32_t rc = 0) {
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | rc;
}
// M-form: opcode | rS | rA | sh/rB | mb(21-25) | me(26-30) | rc
constexpr uint32_t M(uint32_t op, uint32_t s, uint32_t a, uint32_t sh, uint32_t mb, uint32_t me, uint32_t rc = 0) {
    return (op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc;
}
// mfspr/mtspr/mftb carry the 10-bit SPR number as two 5-bit halves with the halves swapped: bits 11-15 hold
// the low half (n & 31) and bits 16-20 the high half (n >> 5); the X() builder takes them as f2 and f3.

struct Case {
    uint32_t word;
    Op op;
};

const Case kCases[] = {
    // --- integer arithmetic (primary opcodes 7, 8, 12-15 and 31 XO-form) ---
    {D(14, 3, 4, -8), Op::addi},        {D(15, 3, 0, 0x8000), Op::addis},   {D(12, 5, 6, 7), Op::addic},
    {D(13, 5, 6, 7), Op::addic_rc},     {D(8, 5, 6, 7), Op::subfic},        {D(7, 5, 6, 7), Op::mulli},
    {X(31, 3, 4, 5, 266), Op::add},     {X(31, 3, 4, 5, 10), Op::addc},     {X(31, 3, 4, 5, 138), Op::adde},
    {X(31, 3, 4, 0, 234), Op::addme},   {X(31, 3, 4, 0, 202), Op::addze},   {X(31, 3, 4, 5, 40), Op::subf},
    {X(31, 3, 4, 5, 8), Op::subfc},     {X(31, 3, 4, 5, 136), Op::subfe},   {X(31, 3, 4, 0, 232), Op::subfme},
    {X(31, 3, 4, 0, 200), Op::subfze},  {X(31, 3, 4, 0, 104), Op::neg},     {X(31, 3, 4, 5, 235), Op::mullw},
    {X(31, 3, 4, 5, 75), Op::mulhw},    {X(31, 3, 4, 5, 11), Op::mulhwu},   {X(31, 3, 4, 5, 491), Op::divw},
    {X(31, 3, 4, 5, 459), Op::divwu},
    // --- compare ---
    {D(11, 1 << 2, 4, -1), Op::cmpi},   {D(10, 1 << 2, 4, 5), Op::cmpli},        {X(31, 1 << 2, 4, 5, 0), Op::cmp},
    {X(31, 1 << 2, 4, 5, 32), Op::cmpl},
    // --- logical / shift / rotate ---
    {D(24, 3, 4, 0x1234), Op::ori},     {D(25, 3, 4, 0x1234), Op::oris},    {D(26, 3, 4, 0x1234), Op::xori},
    {D(27, 3, 4, 0x1234), Op::xoris},   {D(28, 3, 4, 0x1234), Op::andi_rc}, {D(29, 3, 4, 0x1234), Op::andis_rc},
    {X(31, 3, 4, 5, 28), Op::and_},     {X(31, 3, 4, 5, 60), Op::andc},     {X(31, 3, 4, 5, 444), Op::or_},
    {X(31, 3, 4, 5, 412), Op::orc},     {X(31, 3, 4, 5, 316), Op::xor_},    {X(31, 3, 4, 5, 476), Op::nand},
    {X(31, 3, 4, 5, 124), Op::nor},     {X(31, 3, 4, 5, 284), Op::eqv},     {X(31, 3, 4, 0, 954), Op::extsb},
    {X(31, 3, 4, 0, 922), Op::extsh},   {X(31, 3, 4, 0, 26), Op::cntlzw},   {X(31, 3, 4, 5, 24), Op::slw},
    {X(31, 3, 4, 5, 536), Op::srw},     {X(31, 3, 4, 5, 792), Op::sraw},    {X(31, 3, 4, 7, 824), Op::srawi},
    {M(21, 3, 4, 5, 6, 7), Op::rlwinm}, {M(20, 3, 4, 5, 6, 7), Op::rlwimi}, {M(23, 3, 4, 5, 6, 7), Op::rlwnm},
    // --- condition register ---
    {X(19, 3, 4, 5, 257), Op::crand},   {X(19, 3, 4, 5, 129), Op::crandc},  {X(19, 3, 4, 5, 289), Op::creqv},
    {X(19, 3, 4, 5, 225), Op::crnand},  {X(19, 3, 4, 5, 33), Op::crnor},    {X(19, 3, 4, 5, 449), Op::cror},
    {X(19, 3, 4, 5, 417), Op::crorc},   {X(19, 3, 4, 5, 193), Op::crxor},   {X(19, 2 << 2, 5 << 2, 0, 0), Op::mcrf},
    {X(31, 3, 0, 0, 19), Op::mfcr},     {X(31, 3, 0, 0, 144), Op::mtcrf},   {X(31, 1 << 2, 0, 0, 512), Op::mcrxr},
    // --- branches ---
    {(18u << 26) | 0x10, Op::b},        {(16u << 26) | (12u << 21) | (2u << 16) | 0x20, Op::bc},
    {X(19, 20, 0, 0, 16), Op::bclr},    {X(19, 20, 0, 0, 528), Op::bcctr},  {(17u << 26) | 2, Op::sc},
    {X(19, 0, 0, 0, 50), Op::rfi},      {X(19, 0, 0, 0, 150), Op::isync},
    // --- loads ---
    {D(34, 3, 4, 8), Op::lbz},          {D(35, 3, 4, 8), Op::lbzu},         {D(40, 3, 4, 8), Op::lhz},
    {D(41, 3, 4, 8), Op::lhzu},         {D(42, 3, 4, 8), Op::lha},          {D(43, 3, 4, 8), Op::lhau},
    {D(32, 3, 4, 8), Op::lwz},          {D(33, 3, 4, 8), Op::lwzu},         {D(46, 3, 4, 8), Op::lmw},
    {X(31, 3, 4, 5, 87), Op::lbzx},     {X(31, 3, 4, 5, 119), Op::lbzux},   {X(31, 3, 4, 5, 279), Op::lhzx},
    {X(31, 3, 4, 5, 311), Op::lhzux},   {X(31, 3, 4, 5, 343), Op::lhax},    {X(31, 3, 4, 5, 375), Op::lhaux},
    {X(31, 3, 4, 5, 790), Op::lhbrx},   {X(31, 3, 4, 5, 23), Op::lwzx},     {X(31, 3, 4, 5, 55), Op::lwzux},
    {X(31, 3, 4, 5, 534), Op::lwbrx},   {X(31, 3, 4, 5, 20), Op::lwarx},    {X(31, 3, 4, 8, 597), Op::lswi},
    {X(31, 3, 4, 5, 533), Op::lswx},    {X(31, 3, 4, 5, 310), Op::eciwx},
    // --- stores ---
    {D(38, 3, 4, 8), Op::stb},          {D(39, 3, 4, 8), Op::stbu},         {D(44, 3, 4, 8), Op::sth},
    {D(45, 3, 4, 8), Op::sthu},         {D(36, 3, 4, 8), Op::stw},          {D(37, 3, 4, 8), Op::stwu},
    {D(47, 3, 4, 8), Op::stmw},         {X(31, 3, 4, 5, 215), Op::stbx},    {X(31, 3, 4, 5, 247), Op::stbux},
    {X(31, 3, 4, 5, 407), Op::sthx},    {X(31, 3, 4, 5, 439), Op::sthux},   {X(31, 3, 4, 5, 918), Op::sthbrx},
    {X(31, 3, 4, 5, 151), Op::stwx},    {X(31, 3, 4, 5, 183), Op::stwux},   {X(31, 3, 4, 5, 662), Op::stwbrx},
    {X(31, 3, 4, 5, 150, 1), Op::stwcx_rc}, {X(31, 3, 4, 8, 725), Op::stswi}, {X(31, 3, 4, 5, 661), Op::stswx},
    {X(31, 3, 4, 5, 438), Op::ecowx},
    // --- cache / sync / system ---
    {X(31, 0, 4, 5, 86), Op::dcbf},     {X(31, 0, 4, 5, 470), Op::dcbi},    {X(31, 0, 4, 5, 54), Op::dcbst},
    {X(31, 0, 4, 5, 278), Op::dcbt},    {X(31, 0, 4, 5, 246), Op::dcbtst},  {X(31, 0, 4, 5, 1014), Op::dcbz},
    {X(4, 0, 4, 5, 1014), Op::dcbz_l},  {X(31, 0, 4, 5, 982), Op::icbi},    {X(31, 0, 0, 0, 598), Op::sync},
    {X(31, 0, 0, 0, 854), Op::eieio},   {X(31, 0, 0, 5, 306), Op::tlbie},   {X(31, 0, 0, 0, 566), Op::tlbsync},
    {X(31, 3, 0, 0, 83), Op::mfmsr},    {X(31, 3, 0, 0, 146), Op::mtmsr},   {X(31, 3, 1, 0, 595), Op::mfsr},
    {X(31, 3, 1, 0, 210), Op::mtsr},    {X(31, 3, 0, 5, 659), Op::mfsrin},  {X(31, 3, 0, 5, 242), Op::mtsrin},
    {X(31, 3, 8 & 31, 8 >> 5, 339), Op::mfspr}, {X(31, 3, 9 & 31, 9 >> 5, 467), Op::mtspr},
    {X(31, 3, 268 & 31, 268 >> 5, 371), Op::mftb},
    {X(31, 4, 3, 5, 4), Op::tw},        {D(3, 4, 3, 5), Op::twi},
    // --- floating point loads/stores ---
    {D(48, 1, 4, 8), Op::lfs},          {D(49, 1, 4, 8), Op::lfsu},         {D(50, 1, 4, 8), Op::lfd},
    {D(51, 1, 4, 8), Op::lfdu},         {D(52, 1, 4, 8), Op::stfs},         {D(53, 1, 4, 8), Op::stfsu},
    {D(54, 1, 4, 8), Op::stfd},         {D(55, 1, 4, 8), Op::stfdu},        {X(31, 1, 4, 5, 535), Op::lfsx},
    {X(31, 1, 4, 5, 567), Op::lfsux},   {X(31, 1, 4, 5, 599), Op::lfdx},    {X(31, 1, 4, 5, 631), Op::lfdux},
    {X(31, 1, 4, 5, 663), Op::stfsx},   {X(31, 1, 4, 5, 695), Op::stfsux},  {X(31, 1, 4, 5, 727), Op::stfdx},
    {X(31, 1, 4, 5, 759), Op::stfdux},  {X(31, 1, 4, 5, 983), Op::stfiwx},
    // --- floating point arithmetic (opcode 63 double, 59 single) ---
    {A(63, 1, 2, 3, 0, 21), Op::fadd},  {A(59, 1, 2, 3, 0, 21), Op::fadds}, {A(63, 1, 2, 3, 0, 20), Op::fsub},
    {A(59, 1, 2, 3, 0, 20), Op::fsubs}, {A(63, 1, 2, 0, 4, 25), Op::fmul},  {A(59, 1, 2, 0, 4, 25), Op::fmuls},
    {A(63, 1, 2, 3, 0, 18), Op::fdiv},  {A(59, 1, 2, 3, 0, 18), Op::fdivs}, {A(63, 1, 2, 3, 4, 29), Op::fmadd},
    {A(59, 1, 2, 3, 4, 29), Op::fmadds}, {A(63, 1, 2, 3, 4, 28), Op::fmsub}, {A(59, 1, 2, 3, 4, 28), Op::fmsubs},
    {A(63, 1, 2, 3, 4, 31), Op::fnmadd}, {A(59, 1, 2, 3, 4, 31), Op::fnmadds}, {A(63, 1, 2, 3, 4, 30), Op::fnmsub},
    {A(59, 1, 2, 3, 4, 30), Op::fnmsubs}, {A(63, 1, 2, 3, 4, 23), Op::fsel}, {A(59, 1, 0, 3, 0, 24), Op::fres},
    {A(63, 1, 0, 3, 0, 26), Op::frsqrte}, {X(63, 1, 0, 3, 12), Op::frsp},   {X(63, 1, 0, 3, 14), Op::fctiw},
    {X(63, 1, 0, 3, 15), Op::fctiwz},   {X(63, 1 << 2, 2, 3, 0), Op::fcmpu}, {X(63, 1 << 2, 2, 3, 32), Op::fcmpo},
    {X(63, 1, 0, 3, 72), Op::fmr},      {X(63, 1, 0, 3, 40), Op::fneg},     {X(63, 1, 0, 3, 264), Op::fabs},
    {X(63, 1, 0, 3, 136), Op::fnabs},   {X(63, 1, 0, 0, 583), Op::mffs},    {X(63, 0, 0, 3, 711) | (0xFFu << 17), Op::mtfsf},
    {X(63, 1 << 2, 0, 5 << 1, 134), Op::mtfsfi}, {X(63, 3, 0, 0, 70), Op::mtfsb0}, {X(63, 3, 0, 0, 38), Op::mtfsb1},
    {X(63, 1 << 2, 2 << 2, 0, 64), Op::mcrfs},
    // --- paired singles: opcode 4 ---
    {A(4, 1, 2, 3, 0, 21), Op::ps_add}, {A(4, 1, 2, 3, 0, 20), Op::ps_sub}, {A(4, 1, 2, 0, 4, 25), Op::ps_mul},
    {A(4, 1, 2, 3, 0, 18), Op::ps_div}, {A(4, 1, 2, 3, 4, 29), Op::ps_madd}, {A(4, 1, 2, 3, 4, 28), Op::ps_msub},
    {A(4, 1, 2, 3, 4, 31), Op::ps_nmadd}, {A(4, 1, 2, 3, 4, 30), Op::ps_nmsub}, {A(4, 1, 2, 0, 4, 12), Op::ps_muls0},
    {A(4, 1, 2, 0, 4, 13), Op::ps_muls1}, {A(4, 1, 2, 3, 4, 14), Op::ps_madds0}, {A(4, 1, 2, 3, 4, 15), Op::ps_madds1},
    {A(4, 1, 2, 3, 4, 10), Op::ps_sum0}, {A(4, 1, 2, 3, 4, 11), Op::ps_sum1}, {A(4, 1, 2, 3, 4, 23), Op::ps_sel},
    {A(4, 1, 0, 3, 0, 24), Op::ps_res}, {A(4, 1, 0, 3, 0, 26), Op::ps_rsqrte}, {X(4, 1, 2, 3, 528), Op::ps_merge00},
    {X(4, 1, 2, 3, 560), Op::ps_merge01}, {X(4, 1, 2, 3, 592), Op::ps_merge10}, {X(4, 1, 2, 3, 624), Op::ps_merge11},
    {X(4, 1, 0, 3, 72), Op::ps_mr},     {X(4, 1, 0, 3, 40), Op::ps_neg},    {X(4, 1, 0, 3, 264), Op::ps_abs},
    {X(4, 1, 0, 3, 136), Op::ps_nabs},  {X(4, 1 << 2, 2, 3, 0), Op::ps_cmpu0}, {X(4, 1 << 2, 2, 3, 32), Op::ps_cmpo0},
    {X(4, 1 << 2, 2, 3, 64), Op::ps_cmpu1}, {X(4, 1 << 2, 2, 3, 96), Op::ps_cmpo1},
    // psq D-form (opcodes 56, 57, 60, 61) and indexed X-form (opcode 4, 6-bit xo 6/7/38/39)
    {(56u << 26) | (1u << 21) | (3u << 16), Op::psq_l},  {(57u << 26) | (1u << 21) | (3u << 16), Op::psq_lu},
    {(60u << 26) | (1u << 21) | (3u << 16), Op::psq_st}, {(61u << 26) | (1u << 21) | (3u << 16), Op::psq_stu},
    {X(4, 1, 3, 4, 6), Op::psq_lx},     {X(4, 1, 3, 4, 7), Op::psq_stx},    {X(4, 1, 3, 4, 38), Op::psq_lux},
    {X(4, 1, 3, 4, 39), Op::psq_stux},
};

void testEveryOpHasAnEncoding() {
    std::set<Op> seen;
    for (const Case& c : kCases) {
        const Insn i = decode(c.word, 0x80003000);
        if (i.op != c.op) {
            std::fprintf(stderr, "FAIL word %08x decoded as %s, expected %s\n", c.word, opName(i.op), opName(c.op));
            ++g_failures;
        }
        ++g_checks;
        seen.insert(c.op);
    }
    for (uint16_t k = 1; k < static_cast<uint16_t>(Op::Count); ++k) {
        const Op op = static_cast<Op>(k);
        if (!seen.count(op)) {
            std::fprintf(stderr, "FAIL no hand-assembled case covers %s\n", opName(op));
            ++g_failures;
        }
        ++g_checks;
    }
}

void testWellKnownEncodings() {
    // Fixed words every PowerPC programmer knows from the architecture manuals.
    CHECK(decode(0x4E800020, 0).op == Op::bclr && isReturn(decode(0x4E800020, 0)));       // blr
    CHECK(decode(0x4E800420, 0).op == Op::bcctr && isIndirectJump(decode(0x4E800420, 0))); // bctr
    CHECK(decode(0x4E800421, 0).op == Op::bcctr && isCall(decode(0x4E800421, 0)));         // bctrl
    CHECK(decode(0x60000000, 0).op == Op::ori);                                            // nop = ori 0,0,0
    CHECK(disassemble(decode(0x60000000, 0)) == "ori r0,r0,0x0");
    CHECK(decode(0x7C0802A6, 0).op == Op::mfspr && decode(0x7C0802A6, 0).spr() == 8);       // mflr r0
    CHECK(decode(0x7C0803A6, 0).op == Op::mtspr && decode(0x7C0803A6, 0).spr() == 8);       // mtlr r0
    CHECK(decode(0x7C0903A6, 0).op == Op::mtspr && decode(0x7C0903A6, 0).spr() == 9);       // mtctr r0
    CHECK(decode(0x9421FFF0, 0).op == Op::stwu && decode(0x9421FFF0, 0).simm() == -16);     // stwu r1,-16(r1)
    CHECK(decode(0x3C608000, 0).op == Op::addis && decode(0x3C608000, 0).rA() == 0);        // lis r3,0x8000
    CHECK(decode(0x7C0004AC, 0).op == Op::sync);
    CHECK(decode(0x7C0006AC, 0).op == Op::eieio);
    CHECK(decode(0x4C00012C, 0).op == Op::isync);
    CHECK(decode(0x00000000, 0).op == Op::invalid);   // zero word is not an instruction
}

void testInvalid() {
    CHECK(decode(1u << 26, 0).op == Op::invalid);              // primary opcodes 1, 2, 5, 6, 9, 22 are unused
    CHECK(decode(2u << 26, 0).op == Op::invalid);
    CHECK(decode(5u << 26, 0).op == Op::invalid);
    CHECK(decode(9u << 26, 0).op == Op::invalid);
    CHECK(decode(22u << 26, 0).op == Op::invalid);
    CHECK(decode(X(31, 0, 0, 0, 1), 0).op == Op::invalid);     // opcode 31 extended opcode 1 is unassigned
    CHECK(decode(X(19, 0, 0, 0, 2), 0).op == Op::invalid);
    CHECK(decode(17u << 26, 0).op == Op::invalid);             // sc requires bit 30 set
    CHECK(decode(58u << 26, 0).op == Op::invalid);             // 64-bit load group does not exist on Gekko
    CHECK(decode(62u << 26, 0).op == Op::invalid);
    CHECK(decode(X(63, 0, 0, 0, 1), 0).op == Op::invalid);
    CHECK(decode(X(4, 0, 0, 0, 2), 0).op == Op::invalid);
}

void testFields() {
    const Insn addi = decode(D(14, 3, 4, -8), 0x80003000);
    CHECK(addi.rD() == 3 && addi.rA() == 4 && addi.simm() == -8 && addi.uimm() == 0xFFF8);

    const Insn rlw = decode(M(21, 5, 6, 7, 8, 9, 1), 0);
    CHECK(rlw.rS() == 5 && rlw.rA() == 6 && rlw.sh() == 7 && rlw.mb() == 8 && rlw.me() == 9 && rlw.rc());

    const Insn mfspr = decode(X(31, 3, 912 & 31, 912 >> 5, 339), 0);
    CHECK(mfspr.rD() == 3 && mfspr.spr() == 912 && std::string(sprName(912)) == "GQR0");
    CHECK(std::string(sprName(1008)) == "HID0" && std::string(sprName(1009)) == "HID1" && std::string(sprName(920)) == "HID2");
    CHECK(sprName(777) == nullptr);

    // Branch displacements: LI is a signed 24-bit word offset; BD a signed 14-bit word offset.
    const Insn b = decode((18u << 26) | 0x10, 0x80003000);
    CHECK(b.branchTarget() == 0x80003010 && !b.lk() && !b.aa());
    const Insn bl = decode((18u << 26) | (static_cast<uint32_t>(-0x20) & 0x03FFFFFCu) | 1, 0x80003000);
    CHECK(bl.branchTarget() == 0x80002FE0 && bl.lk() && isCall(bl));
    const Insn ba = decode((18u << 26) | 0x100 | 2, 0x80003000);
    CHECK(ba.aa() && ba.branchTarget() == 0x100);
    const Insn bc = decode((16u << 26) | (12u << 21) | (2u << 16) | static_cast<uint32_t>(-8 & 0xFFFC), 0x80003000);
    CHECK(bc.bo() == 12 && bc.bi() == 2 && bc.branchTarget() == 0x80002FF8 && isConditionalBranch(bc));
    const Insn bdnz = decode((16u << 26) | (16u << 21) | 0x8, 0x1000);
    CHECK(bdnz.bo() == 16 && bdnz.branchTarget() == 0x1008);   // decrement-and-branch form, no CR test

    // XO-form OE bit (bit 21) and Rc.
    const Insn addo = decode(X(31, 3, 4, 5, 266, 1) | 0x400, 0);
    CHECK(addo.op == Op::add && addo.oe() && addo.rc() && mnemonic(addo) == "addo.");
    CHECK(mnemonic(decode(X(31, 3, 4, 5, 266), 0)) == "add");

    // Paired-single quantised load: W (bit 16), I (bits 17-19), 12-bit signed displacement.
    const Insn psq = decode((56u << 26) | (1u << 21) | (3u << 16) | (1u << 15) | (2u << 12) | (static_cast<uint32_t>(-16) & 0xFFF), 0);
    CHECK(psq.op == Op::psq_l && psq.rD() == 1 && psq.rA() == 3 && psq.psqW() == 1 && psq.psqI() == 2 && psq.psqD() == -16);
    // psq_stx: W is bit 21, I is bits 22-24.
    const Insn psqx = decode(X(4, 1, 3, 4, 7) | (1u << 10) | (5u << 7), 0);
    CHECK(psqx.op == Op::psq_stx && psqx.psqWx() == 1 && psqx.psqIx() == 5 && psqx.rB() == 4);

    // mtcrf CRM is bits 12-19; mtfsf FM is bits 7-14.
    CHECK(decode(X(31, 3, 0, 0, 144) | (0x81u << 12), 0).crm() == 0x81);
    CHECK(decode(X(63, 0, 0, 3, 711) | (0xA5u << 17), 0).fm() == 0xA5);
}

void testClassification() {
    CHECK(isUnconditionalBranch(decode(18u << 26, 0)));
    CHECK(!isUnconditionalBranch(decode((18u << 26) | 1, 0)));
    CHECK(isConditionalBranch(decode(X(19, 12, 2, 0, 16), 0)));   // beqlr
    CHECK(!isReturn(decode(X(19, 12, 2, 0, 16), 0)));
    CHECK(isSupervisor(decode(X(31, 3, 0, 0, 146), 0)));            // mtmsr
    CHECK(isSupervisor(decode(X(31, 3, 1008 & 31, 1008 >> 5, 467), 0)));   // mtspr HID0
    CHECK(!isSupervisor(decode(X(31, 3, 8 & 31, 8 >> 5, 467), 0)));        // mtlr is user level
    CHECK(!isSupervisor(decode(X(31, 3, 912 & 31, 912 >> 5, 467), 0)));    // GQR write
    CHECK(isModelledSupervisor(decode(X(31, 3, 0, 0, 146), 0)));
    CHECK(!isModelledSupervisor(decode(X(19, 0, 0, 0, 50), 0)));                      // rfi is not modelled
}

}  // namespace

int main() {
    testEveryOpHasAnEncoding();
    testWellKnownEncodings();
    testInvalid();
    testFields();
    testClassification();
    std::printf("recomp decoder tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
