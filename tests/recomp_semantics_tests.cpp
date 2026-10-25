// Semantics tests for the guest runtime used by recompiled code.
// Expected values are derived by hand from the PowerPC architecture manuals (integer, XER/CR, rotate/mask,
// FPSCR rounding), the Gekko User's Manual (paired singles, GQR layout) or exact arithmetic noted per case;
// none of them is copied from the implementation's output.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <xmmintrin.h>
#include <memory>
#include <vector>

#include "guest.h"

namespace awl::rt {
// The runtime links against the generated dispatch table; the tests supply a tiny one.
namespace {
int g_calledA = 0;
void fnA(Ctx& c) { ++g_calledA; c.r[3] = 0xA; }
void fnB(Ctx& c) { c.r[3] = 0xB; }
}  // namespace
const FnEntry kFunctions[] = {{0x80001000, fnA}, {0x80002000, fnB}};
const uint32_t kFunctionCount = 2;
}  // namespace awl::rt

using namespace awl::rt;

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

struct Harness {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kMem1Size, 0);
    std::vector<uint8_t> lc = std::vector<uint8_t>(kLockedCacheSize, 0);
    Machine machine;
    std::unique_ptr<Ctx> c;
    Harness() {
        machine.mem = mem.data();
        machine.lc = lc.data();
        c = std::make_unique<Ctx>(machine);
        setFpscr(*c, 0);
    }
    uint8_t* at(uint32_t guest) { return mem.data() + (guest & 0x0FFFFFFF); }
};

bool throwsKind(Ctx& c, StopKind kind, void (*fn)(Ctx&)) {
    try {
        fn(c);
    } catch (const Stop& s) {
        return s.kind == kind;
    }
    return false;
}

void testIntegerArithmetic() {
    Harness h;
    Ctx& c = *h.c;
    // 0xFFFFFFFF + 1 = 0 with carry out; no signed overflow (-1 + 1 = 0).
    CHECK((addx<true, true>(c, 0xFFFFFFFF, 1, 0)) == 0 && c.ca == 1 && c.ov == 0 && c.so == 0);
    // 0x7FFFFFFF + 1 overflows signed range: result 0x80000000, OV=1 and SO latches, CA=0.
    CHECK((addx<true, true>(c, 0x7FFFFFFF, 1, 0)) == 0x80000000 && c.ca == 0 && c.ov == 1 && c.so == 1);
    // A later non-overflowing op clears OV but SO stays sticky.
    CHECK((addx<true, true>(c, 1, 1, 0)) == 2 && c.ov == 0 && c.so == 1);
    c.so = 0;
    // adde: 0xFFFFFFFF + 0 + CA(1) = 0, carry out 1.
    CHECK((addx<true, false>(c, 0xFFFFFFFF, 0, 1)) == 0 && c.ca == 1);
    // subfc rD = rB - rA = 5 - 3 = 2 with no borrow -> CA=1 (implemented as ~rA + rB + 1).
    CHECK((addx<true, false>(c, ~3u, 5, 1)) == 2 && c.ca == 1);
    // 3 - 5 = -2 with borrow -> CA=0.
    CHECK((addx<true, false>(c, ~5u, 3, 1)) == 0xFFFFFFFE && c.ca == 0);
    // neg(0x80000000) overflows (result 0x80000000, OV=1).
    CHECK((addx<false, true>(c, ~0x80000000u, 0, 1)) == 0x80000000 && c.ov == 1);
    c.so = 0; c.ov = 0;
    // mullw: 0x10000 * 0x10000 = 2^32 does not fit in 32 bits: low word 0, OV=1.
    CHECK(mullw<true>(c, 0x10000, 0x10000) == 0 && c.ov == 1);
    c.so = 0; c.ov = 0;
    CHECK(mullw<false>(c, 0xFFFFFFFF, 3) == 0xFFFFFFFD);   // -1 * 3 = -3
    // mulhw / mulhwu high words: (-1)*(-1) = 1 -> hi 0; 0xFFFFFFFF*0xFFFFFFFF = 0xFFFFFFFE00000001 -> hi 0xFFFFFFFE.
    CHECK(mulhw(0xFFFFFFFF, 0xFFFFFFFF) == 0);
    CHECK(mulhwu(0xFFFFFFFF, 0xFFFFFFFF) == 0xFFFFFFFE);
    CHECK(mulhw(0x80000000, 2) == 0xFFFFFFFF);              // -2^31 * 2 = -2^32 -> hi word -1
    // divw / divwu in the defined range.
    CHECK(divw<false>(c, 0xFFFFFFF9, 2) == 0xFFFFFFFD);     // -7 / 2 truncates toward zero = -3
    CHECK(divwu<false>(c, 7, 2) == 3);
    // Defined-overflow handling used by the runtime (architecturally undefined result, OV set).
    CHECK(divwu<true>(c, 5, 0) == 0 && c.ov == 1);
}

void testRecordAndCompare() {
    Harness h;
    Ctx& c = *h.c;
    cr0(c, 0x80000000);     // negative -> LT
    CHECK(getCr(c, 0) == 8);
    cr0(c, 5);              // positive -> GT
    CHECK(getCr(c, 0) == 4);
    cr0(c, 0);              // zero -> EQ
    CHECK(getCr(c, 0) == 2);
    c.so = 1;
    cr0(c, 0);              // SO copied into bit 3 of the field
    CHECK(getCr(c, 0) == 3);
    c.so = 0;
    cmps(c, 3, 0xFFFFFFFF, 1);   // signed: -1 < 1
    CHECK(getCr(c, 3) == 8);
    cmpu(c, 3, 0xFFFFFFFF, 1);   // unsigned: 0xFFFFFFFF > 1
    CHECK(getCr(c, 3) == 4);
    // Field numbering: CR0 is the most significant nibble, CR7 the least.
    c.cr = 0;
    setCr(c, 0, 0xA);
    setCr(c, 7, 0x5);
    CHECK(c.cr == 0xA0000005u);
    CHECK(crBit(c, 0) == 1 && crBit(c, 1) == 0 && crBit(c, 2) == 1 && crBit(c, 31) == 1);
    setCrBit(c, 1, 1);
    CHECK(c.cr == 0xE0000005u);
    // mtcrf with CRM 0x81 updates fields 0 and 7 only.
    c.cr = 0;
    mtcrf(c, 0x81, 0xFFFFFFFF);
    CHECK(c.cr == 0xF000000Fu);
}

void testRotateMaskShift() {
    Harness h;
    Ctx& c = *h.c;
    // MASK(mb,me): bits mb..me inclusive in IBM numbering; mb>me wraps around.
    static_assert(mask(0, 31) == 0xFFFFFFFFu, "full mask");
    static_assert(mask(24, 31) == 0x000000FFu, "low byte");
    static_assert(mask(0, 0) == 0x80000000u, "bit 0");
    static_assert(mask(16, 23) == 0x0000FF00u, "byte 2");
    static_assert(mask(28, 3) == 0xF000000Fu, "wrapped mask");
    static_assert(mask(31, 0) == 0x80000001u, "wrapped two-bit mask");
    CHECK(rotl(0x80000001, 1) == 0x00000003);
    CHECK(rotl(0x12345678, 0) == 0x12345678);
    CHECK(rotl(0x12345678, 36) == rotl(0x12345678, 4));   // only the low 5 bits count
    // rlwinm r,r,8,0,31 style rotate then mask: (0x12345678 rotl 8) = 0x34567812.
    CHECK((rotl(0x12345678, 8) & mask(0, 31)) == 0x34567812);
    // slw / srw use a 6-bit shift amount: >= 32 gives 0.
    CHECK(slw(1, 31) == 0x80000000 && slw(1, 32) == 0 && slw(1, 63) == 0);
    CHECK(srw(0x80000000, 31) == 1 && srw(0x80000000, 32) == 0);
    // sraw: negative value shifted right fills with sign; CA set only if a 1 bit is shifted out of a negative.
    CHECK(sraw(c, 0xFFFFFFF0, 4) == 0xFFFFFFFF && c.ca == 0);     // -16 >> 4 = -1, no bits lost
    CHECK(sraw(c, 0xFFFFFFF1, 4) == 0xFFFFFFFF && c.ca == 1);     // -15 >> 4 = -1, bit lost
    CHECK(sraw(c, 0x00000010, 4) == 1 && c.ca == 0);
    CHECK(sraw(c, 0x80000000, 32) == 0xFFFFFFFF && c.ca == 1);    // shift >= 32 of a negative
    CHECK(sraw(c, 0x7FFFFFFF, 40) == 0 && c.ca == 0);
    CHECK(cntlzw(0) == 32 && cntlzw(1) == 31 && cntlzw(0x80000000) == 0 && cntlzw(0x00010000) == 15);
}

void testMemory() {
    Harness h;
    Ctx& c = *h.c;
    st32(c, 0x80001000, 0x11223344);
    CHECK(h.at(0x80001000)[0] == 0x11 && h.at(0x80001000)[1] == 0x22 && h.at(0x80001000)[2] == 0x33 && h.at(0x80001000)[3] == 0x44);
    CHECK(ld32(c, 0x80001000) == 0x11223344);
    CHECK(ld16(c, 0x80001002) == 0x3344 && ld8(c, 0x80001001) == 0x22);
    st16(c, 0x80001010, 0x8001);
    CHECK(ld16s(c, 0x80001010) == 0xFFFF8001);             // lha sign-extends
    CHECK(ld16(c, 0x80001010) == 0x8001);                  // lhz zero-extends
    CHECK(ld32r(c, 0x80001000) == 0x44332211);             // lwbrx byte-reverses
    CHECK(ld16r(c, 0x80001000) == 0x2211);                 // lhbrx
    st32r(c, 0x80001020, 0x11223344);                      // stwbrx: memory holds the reversed bytes
    CHECK(h.at(0x80001020)[0] == 0x44 && h.at(0x80001020)[3] == 0x11);
    // The uncached 0xC0000000 view reaches the same bytes as the cached 0x80000000 view.
    CHECK(ld32(c, 0xC0001000) == 0x11223344);
    // Unaligned access is permitted.
    CHECK(ld32(c, 0x80001001) == 0x22334400u + h.at(0x80001004)[0]);
    // lmw/stmw: r29..r31 stored consecutively.
    c.r[29] = 1; c.r[30] = 2; c.r[31] = 3;
    stmw(c, 29, 0x80001100);
    CHECK(ld32(c, 0x80001100) == 1 && ld32(c, 0x80001104) == 2 && ld32(c, 0x80001108) == 3);
    c.r[29] = c.r[30] = c.r[31] = 0;
    lmw(c, 30, 0x80001104);
    CHECK(c.r[29] == 0 && c.r[30] == 2 && c.r[31] == 3);
    // Locked cache window.
    st32(c, 0xE0000010, 0xCAFEBABE);
    CHECK(h.lc[0x10] == 0xCA && ld32(c, 0xE0000010) == 0xCAFEBABE);
    // dcbz clears the aligned 32-byte block that contains the address.
    for (uint32_t i = 0; i < 64; ++i) h.at(0x80002000)[i] = 0xFF;
    dcbz(c, 0x80002025);
    CHECK(h.at(0x80002000)[0x1F] == 0xFF && h.at(0x80002000)[0x20] == 0 && h.at(0x80002000)[0x3F] == 0 );
    // lwarx / stwcx.: success when the reservation is intact, failure afterwards; CR0[EQ] reports it.
    st32(c, 0x80003000, 7);
    CHECK(lwarx(c, 0x80003000) == 7);
    stwcx(c, 0x80003000, 9);
    CHECK(ld32(c, 0x80003000) == 9 && getCr(c, 0) == 2);
    stwcx(c, 0x80003000, 11);                // reservation consumed by the previous stwcx.
    CHECK(ld32(c, 0x80003000) == 9 && getCr(c, 0) == 0);
    lwarx(c, 0x80003000);
    dcbz(c, 0x80003000);                     // clearing the reserved line cancels the reservation
    stwcx(c, 0x80003000, 5);
    CHECK(getCr(c, 0) == 0);
    // Faults: hardware register window and out-of-range addresses stop with distinct kinds.
    c.pc = 0x80004000;
    try { ld32(c, 0xCC006800); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::Mmio && s.addr == 0xCC006800); }
    try { ld32(c, 0x81800000); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::Fault); }
    try { ld32(c, 0x817FFFFD); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::Fault); }   // straddles the end
    try { ld32(c, 0x00000058); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::Fault); }
    CHECK(ld8(c, 0x817FFFFF) == 0);          // last byte of MEM1 is valid
}

void testSprAndTimeBase() {
    Harness h;
    Ctx& c = *h.c;
    mtspr(c, 0, 1, 0xE000007F);               // XER: SO, OV, CA and byte count
    CHECK(c.so == 1 && c.ov == 1 && c.ca == 1 && c.xerbc == 0x7F && mfspr(c, 0, 1) == 0xE000007F);
    mtspr(c, 0, 913, 0x00040004);             // GQR1
    CHECK(c.gqr[1] == 0x00040004 && mfspr(c, 0, 913) == 0x00040004);
    mtspr(c, 0, 1008, 0x8000C000);            // HID0 is storage-only in the spike runtime
    CHECK(mfspr(c, 0, 1008) == 0x8000C000);
    CHECK(mfspr(c, 0, 287) == 0x00083214);    // PVR: the runtime's chosen constant (public emulator docs; unverified on hardware)
    c.m->icount = 4000;                          // 4 instructions per time-base tick (documented assumption)
    CHECK(mftb(c, 268) == 1000 && mftb(c, 269) == 0);
    c.m->icount = (uint64_t{5} << 32) * 4;
    CHECK(mftb(c, 269) == 5);
}

double roundingProbe(uint32_t rn) {
    Harness h;
    setFpscr(*h.c, rn);
    volatile double x = 1.0 + 3.0 * std::ldexp(1.0, -25);   // 1 + 0.75 * 2^-23: between 1 and 1 + 2^-23
    const double r = fround(x);
    setFpscr(*h.c, 0);
    return r;
}

void testFloatingPoint() {
    const double up = 1.0 + std::ldexp(1.0, -23);
    // frsp honours FPSCR[RN]: 00 nearest, 01 toward zero, 10 toward +inf, 11 toward -inf.
    CHECK(roundingProbe(0) == up);          // 0.75 ulp above 1.0 -> nearest is 1 + 2^-23
    CHECK(roundingProbe(1) == 1.0);
    CHECK(roundingProbe(2) == up);
    CHECK(roundingProbe(3) == 1.0);

    Harness h;
    Ctx& c = *h.c;
    // fctiw uses the current rounding mode; fctiwz always truncates.  2.5 and -2.5 are exact halves.
    CHECK(fctiw(2.5, false) == 2);                          // ties to even
    CHECK(fctiw(3.5, false) == 4);
    CHECK(fctiw(-2.5, false) == 0xFFFFFFFE);
    CHECK(fctiw(-2.9, true) == 0xFFFFFFFE);                 // truncate toward zero -> -2
    setFpscr(c, 2);
    CHECK(fctiw(2.1, false) == 3);                          // toward +inf
    setFpscr(c, 3);
    CHECK(fctiw(2.9, false) == 2);                          // toward -inf
    CHECK(fctiw(-2.1, false) == 0xFFFFFFFD);
    setFpscr(c, 0);
    // Saturation: positive overflow gives 0x7FFFFFFF, negative 0x80000000; NaN gives 0x80000000.
    CHECK(fctiw(3e9, true) == 0x7FFFFFFF);
    CHECK(fctiw(-3e9, true) == 0x80000000);
    CHECK(fctiw(std::nan(""), true) == 0x80000000);

    // Compare result bits: LT=8, GT=4, EQ=2, UN=1; unordered when either operand is NaN.
    fcmp(c, 2, 1.0, 2.0);
    CHECK(getCr(c, 2) == 8);
    fcmp(c, 2, 2.0, 1.0);
    CHECK(getCr(c, 2) == 4);
    fcmp(c, 2, 1.0, 1.0);
    CHECK(getCr(c, 2) == 2);
    fcmp(c, 2, std::nan(""), 1.0);
    CHECK(getCr(c, 2) == 1);

    // mtfsf / mffs round trip; FEX and VX (bits 1,2 IBM = 0x40000000, 0x20000000) are not writable.
    setFpscr(c, 0);                          // drop the FPCC bits left by the compares above
    c.f[4].ps0 = bitsToDouble(0xFFFFFFFFull);
    mtfsf(c, 0x01, 4);                       // FM bit 7 = field 7 (the low nibble: RN and NI)
    CHECK(c.fpscr == 0xF);
    mffs(c, 5);
    CHECK((doubleToBits(c.f[5].ps0) & 0xFFFFFFFF) == 0xF);
    mtfsb(c, 30, 0);                          // clear FPSCR bit 30 (IBM numbering) = RN low bit
    CHECK(c.fpscr == 0xD);
    setFpscr(c, 0);

    // Non-IEEE mode: flush-to-zero.  A denormal times 1.0 is exact in IEEE mode and 0 in non-IEEE mode.
    volatile double denorm = std::ldexp(1.0, -1074);
    volatile double one = 1.0;
    setFpscr(c, 0);
    CHECK(denorm * one == std::ldexp(1.0, -1074));
    setFpscr(c, 4);                           // NI
    CHECK(denorm * one == 0.0);
    setFpscr(c, 0);

    // lfs converts a single to double and fills both halves; stfs rounds back to single.
    st32(c, 0x80001000, 0x3FC00000);          // 1.5f
    lfs(c, 7, 0x80001000);
    CHECK(c.f[7].ps0 == 1.5 && c.f[7].ps1 == 1.5);
    c.f[8].ps0 = 2.25;
    stfs(c, 8, 0x80001010);
    CHECK(ld32(c, 0x80001010) == 0x40100000);
    stfd(c, 8, 0x80001020);
    CHECK(ld64(c, 0x80001020) == 0x4002000000000000ull);   // 2.25 as a double
    stfiwx(c, 4, 0x80001030);                 // low word of the register image
    CHECK(ld32(c, 0x80001030) == 0xFFFFFFFF);
}

void testPairedSingles() {
    Harness h;
    Ctx& c = *h.c;
    // Fused multiply-add keeps the full product: a = 1 + 2^-13, c = 1 - 2^-13, product = 1 - 2^-26 exactly.
    // Rounding the product to single first would give 1.0 and a sum of 0; a fused op gives -2^-26.
    c.f[1].ps0 = c.f[1].ps1 = 1.0 + std::ldexp(1.0, -13);
    c.f[2].ps0 = c.f[2].ps1 = 1.0 - std::ldexp(1.0, -13);
    c.f[3].ps0 = c.f[3].ps1 = -1.0;
    ps_madd(c, 4, 1, 3, 2);                  // f4 = f1 * f2 + f3
    CHECK(c.f[4].ps0 == -std::ldexp(1.0, -26) && c.f[4].ps1 == -std::ldexp(1.0, -26));
    ps_nmsub(c, 5, 1, 3, 2);                 // -(f1*f2 - f3) = -(2 - 2^-26); 2^-26 is below half a single ulp at 2 -> -2.0
    CHECK(c.f[5].ps0 == -2.0);

    // ps_sum0: ps0 = a.ps0 + b.ps1, ps1 = c.ps1;  ps_sum1: ps0 = c.ps0, ps1 = a.ps0 + b.ps1.
    c.f[10] = {1.0, 2.0}; c.f[11] = {10.0, 20.0}; c.f[12] = {100.0, 200.0};
    ps_sum0(c, 13, 10, 11, 12);
    CHECK(c.f[13].ps0 == 21.0 && c.f[13].ps1 == 200.0);
    ps_sum1(c, 14, 10, 11, 12);
    CHECK(c.f[14].ps0 == 100.0 && c.f[14].ps1 == 21.0);
    // merges: 01 = (a.ps0, b.ps1), 10 = (a.ps1, b.ps0)
    ps_merge01(c, 15, 10, 11, 0);
    CHECK(c.f[15].ps0 == 1.0 && c.f[15].ps1 == 20.0);
    ps_merge10(c, 16, 10, 11, 0);
    CHECK(c.f[16].ps0 == 2.0 && c.f[16].ps1 == 10.0);
    // muls0 multiplies both lanes by c.ps0; muls1 by c.ps1.
    ps_muls0(c, 17, 10, 0, 11);
    CHECK(c.f[17].ps0 == 10.0 && c.f[17].ps1 == 20.0);
    ps_muls1(c, 18, 10, 0, 11);
    CHECK(c.f[18].ps0 == 20.0 && c.f[18].ps1 == 40.0);
    // ps_sel: lane value is c when a >= 0 (including -0.0), else b. NaN selects b.
    c.f[20] = {-0.0, -1.0}; c.f[21] = {5.0, 5.0}; c.f[22] = {7.0, 7.0};
    ps_sel(c, 23, 20, 21, 22);
    CHECK(c.f[23].ps0 == 7.0 && c.f[23].ps1 == 5.0);
    // In-place ops must read both lanes before writing (d == a).
    c.f[24] = {1.0, 2.0};
    ps_merge10(c, 24, 24, 24, 0);
    CHECK(c.f[24].ps0 == 2.0 && c.f[24].ps1 == 1.0);

    // GQR layout (Gekko manual): LD_TYPE bits 16-18, LD_SCALE bits 24-29, ST_TYPE bits 0-2, ST_SCALE bits 8-13.
    // Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16.  Dequantised value = raw * 2^-LD_SCALE.
    st8(c, 0x80001000, 0x80); st8(c, 0x80001001, 0x40);
    c.gqr[2] = (4u << 16);                           // u8, scale 0
    psqL(c, 1, 0x80001000, 0, 2);
    CHECK(c.f[1].ps0 == 128.0 && c.f[1].ps1 == 64.0);
    c.gqr[2] = (4u << 16) | (7u << 24);              // u8, scale 7 -> 128/128, 64/128
    psqL(c, 1, 0x80001000, 0, 2);
    CHECK(c.f[1].ps0 == 1.0 && c.f[1].ps1 == 0.5);
    psqL(c, 1, 0x80001000, 1, 2);                    // W=1: a single value, ps1 forced to 1.0
    CHECK(c.f[1].ps0 == 1.0 && c.f[1].ps1 == 1.0);
    c.gqr[2] = (6u << 16) | (62u << 24);             // s8, scale field 62 = -2 (6-bit signed) -> multiply by 4
    st8(c, 0x80001000, 0xFE);                        // -2
    psqL(c, 1, 0x80001000, 1, 2);
    CHECK(c.f[1].ps0 == -8.0);
    st16(c, 0x80001000, 0xFFFF);
    c.gqr[2] = (7u << 16);                           // s16
    psqL(c, 1, 0x80001000, 1, 2);
    CHECK(c.f[1].ps0 == -1.0);
    c.gqr[2] = 0;                                    // float
    st32(c, 0x80001000, 0x3F800000); st32(c, 0x80001004, 0xC0000000);   // 1.0f, -2.0f
    psqL(c, 1, 0x80001000, 0, 2);
    CHECK(c.f[1].ps0 == 1.0 && c.f[1].ps1 == -2.0);

    // Quantising stores: value * 2^ST_SCALE, truncated toward zero, saturated to the type's range.
    c.f[6] = {0.5, -3.7};
    c.gqr[3] = 7u | (8u << 8);                       // s16, scale 8: 0.5*256 = 128 ; -3.7*256 = -947.2 -> -947
    psqSt(c, 6, 0x80001100, 0, 3);
    CHECK(ld16(c, 0x80001100) == 0x0080 && ld16(c, 0x80001102) == static_cast<uint16_t>(-947));
    c.f[6] = {1000.0, -1.0};
    c.gqr[3] = 4u;                                   // u8, scale 0: saturate high to 255, negative to 0
    psqSt(c, 6, 0x80001110, 0, 3);
    CHECK(ld8(c, 0x80001110) == 255 && ld8(c, 0x80001111) == 0);
    c.f[6] = {-3.7, 0.0};
    c.gqr[3] = 6u;                                   // s8: truncation toward zero gives -3
    psqSt(c, 6, 0x80001120, 1, 3);                   // W=1 stores one element
    CHECK(ld8(c, 0x80001120) == 0xFD && ld8(c, 0x80001121) == 0);
    c.f[6] = {1.5, -2.0};
    c.gqr[3] = 0;                                    // float
    psqSt(c, 6, 0x80001130, 0, 3);
    CHECK(ld32(c, 0x80001130) == 0x3FC00000 && ld32(c, 0x80001134) == 0xC0000000);
}

void fnReturnsWrongLr(Ctx& c) { c.lr = 0x1234; }

void testCallPlumbing() {
    Harness h;
    Ctx& c = *h.c;
    // callIndirect resolves a guest address through the dispatch table and tracks the shadow stack.
    callIndirect(c, 0x80000100, 0x80001000);
    CHECK(c.r[3] == 0xA && g_calledA == 1 && c.sp == 0 && c.lr == 0x80000104);
    try { callIndirect(c, 0x80000200, 0x80001004); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::BadIndirect && s.addr == 0x80001004); }
    // blr must find LR equal to the shadow-stack return address.
    pushFrame(c, 0x80000300, 0x80002000);
    ret(c, 0x80002010);                      // LR was set by pushFrame -> consistent
    fnReturnsWrongLr(c);
    try { ret(c, 0x80002010); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::BadReturn); }
    popFrame(c);
    // Platform boundaries throw by default and are recorded and skipped in census mode.
    try { platformBoundary(c, 0x80005000, "test"); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::Platform && s.pc == 0x80005000); }
    c.m->skipPlatform = true;
    platformBoundary(c, 0x80005000, "test");
    CHECK(c.m->boundaries.size() == 1 && c.m->boundaries[0].addr == 0x80005000);
    c.m->skipPlatform = false;
    // The instruction limit is checked at trap points.
    c.m->setLimit(10);
    c.m->icount = 5;
    at(c, 0x80006000, 3);
    CHECK(c.m->icount == 8 && c.pc == 0x80006000);
    try { at(c, 0x80006004, 5); CHECK(false); } catch (const Stop& s) { CHECK(s.kind == StopKind::InstructionLimit); }
}

// Locked-cache DMA, Gekko User's Manual register layout (PowerPC bit numbering, bit 0 = MSB):
//   DMA_U (SPR 922): bits 0-26 MEM_ADDR, bits 27-31 DMA_LEN[2:6]   -> value bits 31..5 and 4..0
//   DMA_L (SPR 923): bits 0-26 LC_ADDR, bit 27 DMA_LD (0x10), bits 28-29 DMA_LEN[0:1] (value << 2), bit 30 DMA_T (0x02),
//                    bit 31 DMA_F (0x01). Length in 32-byte blocks, 0 meaning 128. LD = 1 loads memory into the cache.
// The SDK's LCLoadBlocks/LCStoreBlocks pass flags 0x12 (load + trigger) and 0x02 (store + trigger).
void fillPattern(uint8_t* p, uint32_t n, uint8_t seed) { for (uint32_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(seed + i * 7); }

void testLockedCacheDma() {
    Harness h;
    Ctx& c = *h.c;
    // Load 2 blocks (64 bytes): memory 0x80001000 -> cache 0xE0000040. LEN = 2 = 0b0000010: LEN[2:6] = 0, LEN[0:1] = 2.
    fillPattern(h.at(0x80001000), 64, 0x10);
    fillPattern(h.lc.data() + 0x80, 4, 0xEE);   // a neighbour that must stay untouched
    mtspr(c, 0x80006000, 922, 0x80001000);
    mtspr(c, 0x80006004, 923, 0xE0000040 | 0x10 | (2u << 2) | 0x02);
    bool same = true;
    for (uint32_t i = 0; i < 64; ++i) same = same && h.lc[0x40 + i] == static_cast<uint8_t>(0x10 + i * 7);
    CHECK(same);
    CHECK(h.lc[0x3F] == 0 && h.lc[0x80] == 0xEE);   // bytes outside [0x40, 0x80) untouched
    CHECK(mfspr(c, 0, 923) == (0xE0000040u | 0x10 | (2u << 2)));   // DMA_T reads back clear: the transfer completed
    CHECK(mfspr(c, 0, 922) == 0x80001000u);

    // Store: cache 0xE0000100 (1 block) -> memory 0x80002020 (LD clear, T set).
    fillPattern(h.lc.data() + 0x100, 32, 0x55);
    mtspr(c, 0, 922, 0x80002020);
    mtspr(c, 0, 923, 0xE0000100 | (1u << 2) | 0x02);
    same = true;
    for (uint32_t i = 0; i < 32; ++i) same = same && h.mem[0x2020 + i] == static_cast<uint8_t>(0x55 + i * 7);
    CHECK(same && h.mem[0x201F] == 0 && h.mem[0x2040] == 0);

    // Length 0 means 128 blocks = 4096 bytes. The 0xC0000000 mirror of memory is also valid.
    fillPattern(h.at(0x80003000), 4096, 0x21);
    h.mem[0x3000 + 4096] = 0x99;
    mtspr(c, 0, 922, 0xC0003000);
    mtspr(c, 0, 923, 0xE0000000 | 0x10 | 0x02);
    CHECK(h.lc[0] == 0x21 && h.lc[4095] == static_cast<uint8_t>(0x21 + 4095 * 7) && h.lc[4096] == 0);

    // A 6-bit-spanning length: 33 blocks = 0b00100001 -> DMA_LEN[2:6] = 0b01000 (8) in DMA_U, DMA_LEN[0:1] = 1 in DMA_L.
    fillPattern(h.at(0x80004000), 33 * 32, 0x33);
    mtspr(c, 0, 922, 0x80004000 | 8);
    mtspr(c, 0, 923, 0xE0000000 | 0x10 | (1u << 2) | 0x02);
    CHECK(h.lc[0] == 0x33 && h.lc[33 * 32 - 1] == static_cast<uint8_t>(0x33 + (33 * 32 - 1) * 7) && h.lc[33 * 32] == static_cast<uint8_t>(0x21 + 33 * 32 * 7));   // the byte after is still the earlier 4 KB load

    // Without DMA_T nothing is transferred; the register just holds the value.
    std::memset(h.lc.data(), 0, 64);
    mtspr(c, 0, 922, 0x80001000);
    mtspr(c, 0, 923, 0xE0000000 | 0x10 | (1u << 2));
    CHECK(h.lc[0] == 0 && mfspr(c, 0, 923) == (0xE0000000u | 0x10 | (1u << 2)));

    // Out-of-range transfers stop loudly with the mtspr pc and the offending address, and move nothing.
    fillPattern(h.at(0x80005000), 64, 0x44);
    std::memset(h.lc.data(), 0, 128);
    Stop s{};
    bool stopped = false;
    try { mtspr(c, 0x80006100, 922, 0x80005000); mtspr(c, 0x80006104, 923, 0xE0003FE0 | 0x10 | (2u << 2) | 0x02); }   // 64 bytes at the last block
    catch (const Stop& e) { stopped = true; s = e; }
    CHECK(stopped && s.kind == StopKind::Fault && s.pc == 0x80006104 && s.addr == 0xE0003FE0 && h.lc[0x3FE0] == 0);
    stopped = false;
    try { mtspr(c, 0, 922, 0x817FFFE0); mtspr(c, 0x80006108, 923, 0xE0000000 | 0x10 | (2u << 2) | 0x02); }          // straddles the end of MEM1
    catch (const Stop& e) { stopped = true; s = e; }
    CHECK(stopped && s.kind == StopKind::Fault && s.addr == 0x817FFFE0);
    stopped = false;
    try { mtspr(c, 0, 922, 0x00001000); mtspr(c, 0x8000610C, 923, 0xE0000000 | 0x10 | (1u << 2) | 0x02); }          // not a memory address
    catch (const Stop& e) { stopped = true; s = e; }
    CHECK(stopped && s.kind == StopKind::Fault && s.addr == 0x00001000);
    stopped = false;
    try { mtspr(c, 0, 922, 0x80005000); mtspr(c, 0x80006110, 923, 0x80000000 | 0x10 | (1u << 2) | 0x02); }          // cache address not in 0xE0000000
    catch (const Stop& e) { stopped = true; s = e; }
    CHECK(stopped && s.kind == StopKind::Fault && s.addr == 0x80000000);
    stopped = false;
    try { mtspr(c, 0, 922, 0x80005000); mtspr(c, 0x80006114, 923, 0xE0000000 | 0x10 | 0x02); mtspr(c, 0, 923, 0xE0003800 | 0x10 | 0x02); }   // 128 blocks (4 KB) from 0xE0003800 run past the 16 KB cache
    catch (const Stop& e) { stopped = true; s = e; }
    CHECK(stopped && s.addr == 0xE0003800);
}

// tw/twi trap conditions (PowerPC architecture book, TO field: 16 signed <, 8 signed >, 4 equal, 2 unsigned <, 1 unsigned >).
void testTrapCondition() {
    CHECK(trapCond(31, 5, 5) && trapCond(31, 1, 2) && trapCond(31, 0xFFFFFFFF, 0));   // 31: every outcome traps (twi 31,r0,0)
    CHECK(!trapCond(0, 5, 5) && !trapCond(0, 1, 2));                                  // 0: never traps
    CHECK(trapCond(16, 0xFFFFFFFF, 0) && !trapCond(16, 0, 0xFFFFFFFF));               // signed <: -1 < 0
    CHECK(!trapCond(2, 0xFFFFFFFF, 0) && trapCond(2, 0, 0xFFFFFFFF));                 // unsigned <: 0xFFFFFFFF is not < 0
    CHECK(!trapCond(8, 0xFFFFFFFF, 0) && trapCond(8, 3, 0xFFFFFFFF));                 // signed >
    CHECK(trapCond(1, 0xFFFFFFFF, 0) && !trapCond(1, 0, 0xFFFFFFFF));                 // unsigned >
    CHECK(trapCond(4, 7, 7) && !trapCond(4, 7, 8));                                   // equal
    CHECK(trapCond(24, 7, 3) && !trapCond(24, 3, 3));                                 // signed < or >: i.e. not equal
}

// ---- FPSCR access sites of the DOL (SDK PPCMffpscr, PPCMtfpscr, PPCSetFpNonIEEEMode, FPU init and context load/save) ----
// The sequences below are the ones the DOL executes (names from the SDK source for the same code shape): mtfsf 0xff
// with the value passed through a stack double, mffs read back through the stack, mtfsb1 29 (NI), mtfsf 0xff,f0 with
// f0 = 0.0 (reset) and the context load/save pair mtfsf/mffs.
void testFpscrSites() {
    Harness h;
    Ctx& c = *h.c;
    const uint32_t sp = 0x80002000;
    auto hostRc = [] { return (_mm_getcsr() >> 13) & 3u; };
    auto hostFlush = [] { return (_mm_getcsr() & 0x8040u) == 0x8040u; };

    // PPCMtfpscr(value): stw r4(0) -> 16(r1); stw r3 -> 20(r1); lfd f31,16(r1); mtfsf 0xff,f31.
    // value 0xA0000FF5: VX (0x20000000) is a derived bit and not writable, RN = 01 (toward zero), NI = 1.
    st32(c, sp + 16, 0);
    st32(c, sp + 20, 0xA0000FF5);
    lfd(c, 31, sp + 16);
    mtfsf(c, 0xFF, 31);
    CHECK(c.fpscr == 0x80000FF5);
    CHECK(hostRc() == 3 && hostFlush());       // RN 01 (toward zero) is MXCSR RC 11; NI selects flush-to-zero

    // PPCMffpscr(): mffs f31; stfd f31,8(r1); lwz r3,12(r1): the low word of the register image is the FPSCR.
    mffs(c, 31);
    stfd(c, 31, sp + 8);
    CHECK(ld32(c, sp + 12) == 0x80000FF5);

    // FPU init: f0 = 0.0 then mtfsf 0xff,f0 clears the whole register and the host environment (nearest, no flush).
    c.f[0].ps0 = 0.0;
    mtfsf(c, 0xFF, 0);
    CHECK(c.fpscr == 0 && hostRc() == 0 && !hostFlush());

    // PPCSetFpNonIEEEMode: mtfsb1 29 sets NI (IBM bit 29 = value bit 2) and nothing else.
    mtfsb(c, 29, 1);
    CHECK(c.fpscr == 4 && hostFlush() && hostRc() == 0);

    // OSLoadContext / OSSaveContext: the saved double round-trips through memory (lfd/mtfsf, then mffs/stfd).
    st64(c, sp + 64, 0xFFF8000000000002ull);   // what an earlier mffs stored: RN = 10 (toward +inf)
    lfd(c, 0, sp + 64);
    mtfsf(c, 0xFF, 0);
    CHECK(c.fpscr == 2 && hostRc() == 2 && !hostFlush());
    mffs(c, 0);
    stfd(c, 0, sp + 72);
    CHECK((ld64(c, sp + 72) & 0xFFFFFFFFull) == 2);
    setFpscr(c, 0);
}

// ---- Fused single-precision multiply-add -----------------------------------------------------------------------
// fmadds, fmsubs, fnmadds, fnmsubs and the ps_ forms round a*c+b once to single precision. Computing the fused
// result in double and then converting to single rounds twice, and for a fused operation (unlike +, -, *, /, sqrt,
// where a 53-bit intermediate suffices: Figueroa, "When is double rounding innocuous?", 1995, needs 2p+2 = 50 bits)
// that can pick the wrong float. fmaSingle rounds the exact result to odd in double first (Boldo and Melquiond,
// "When double rounding is odd", 2005: round-to-odd to p+2 or more bits followed by round-to-nearest to p bits equals
// the single correct rounding), which is exact here because the intermediate has 53 >= 24 + 2 bits.
void testFusedSingle() {
    Harness h;
    Ctx& c = *h.c;
    const double u12 = std::ldexp(1.0, -12), u23 = std::ldexp(1.0, -23), u24 = std::ldexp(1.0, -24), u36 = std::ldexp(1.0, -36);
    // a = 2^-12 + 2^-24 and c = 2^-12 - 2^-24 + 2^-36 are exact singles (13 and 24 significant bits) and
    //   a*c = 2^-24 * (1 + 2^-12)(1 - 2^-12 + 2^-24) = 2^-24 * (1 + 2^-36) = 2^-24 + 2^-60 exactly.
    // With b = 1 the exact sum is 1 + 2^-24 + 2^-60: just above the midpoint 1 + 2^-24 between the singles 1 and 1 + 2^-23,
    // so the correctly rounded single is 1 + 2^-23. Rounding to double first lands exactly on the midpoint (2^-60 is far
    // below half a double ulp, 2^-53), and the tie then goes to the even single 1.0: wrong.
    const double a = u12 + u24, cc = u12 - u24 + u36;
    CHECK(static_cast<double>(static_cast<float>(a)) == a && static_cast<double>(static_cast<float>(cc)) == cc);   // inputs are singles
    CHECK(fround(std::fma(a, cc, 1.0)) == 1.0);               // the double-rounded result, shown to be the wrong float
    CHECK(fmaSingle(a, cc, 1.0) == 1.0 + u23);                // single rounding of the exact sum
    // The mirror image (exact result just below the midpoint above -1): -a*c + (-1) rounds to -(1 + 2^-23).
    CHECK(fmaSingle(-a, cc, -1.0) == -(1.0 + u23));
    // fmsubs a*c - b with b = -1 is the same exact sum; fnmadds/fnmsubs negate the single-rounded value.
    CHECK(fmaSingle(a, cc, -(-1.0)) == 1.0 + u23);
    CHECK(-fmaSingle(a, cc, 1.0) == -(1.0 + u23));
    // Exact ties still go to even: 2^-12 * 2^-12 + 1 = 1 + 2^-24 is the midpoint of 1 (even) and 1 + 2^-23 (odd);
    // 3*2^-12 * 2^-12 + 1 = 1 + 3*2^-24 is the midpoint of 1 + 2^-23 (odd) and 1 + 2^-22 (even).
    CHECK(fmaSingle(u12, u12, 1.0) == 1.0);
    CHECK(fmaSingle(3 * u12, u12, 1.0) == 1.0 + 2 * u23);
    // Exact results are untouched, and special values pass through the single conversion.
    CHECK(fmaSingle(1.5, 2.0, 0.25) == 3.25);
    CHECK(std::isinf(fmaSingle(std::ldexp(1.0, 100), std::ldexp(1.0, 100), 0.0)));      // 2^200 overflows single
    CHECK(std::isinf(fmaSingle(INFINITY, 1.0, 1.0)) && std::isnan(fmaSingle(std::nan(""), 1.0, 1.0)));
    CHECK(std::isnan(fmaSingle(INFINITY, 0.0, 1.0)));
    // Single denormals: 2^-150 is half the smallest denormal 2^-149 (tie to even = 0); just above it rounds up to 2^-149.
    CHECK(fmaSingle(std::ldexp(1.0, -75), std::ldexp(1.0, -75), 0.0) == 0.0);
    CHECK(fmaSingle(std::ldexp(1.0, -75), std::ldexp(1.0 + u23, -75), 0.0) == std::ldexp(1.0, -149));
    // The host rounding mode is left as the guest set it, and directed modes still round once to the right float.
    setFpscr(c, 2);                                           // toward +inf: 1 + 2^-24 + 2^-60 rounds up to 1 + 2^-23
    CHECK(fmaSingle(a, cc, 1.0) == 1.0 + u23 && fmaSingle(a, cc, -1.0 * 1.0 + 2.0) == 1.0 + u23);
    CHECK(((_mm_getcsr() >> 13) & 3u) == 2);
    setFpscr(c, 3);                                           // toward -inf: rounds down to 1.0
    CHECK(fmaSingle(a, cc, 1.0) == 1.0 && ((_mm_getcsr() >> 13) & 3u) == 1);
    setFpscr(c, 0);

    // The paired-single forms use the same rounding in both lanes.
    c.f[1].ps0 = a;   c.f[1].ps1 = -a;
    c.f[2].ps0 = cc;  c.f[2].ps1 = cc;
    c.f[3].ps0 = 1.0; c.f[3].ps1 = -1.0;
    ps_madd(c, 4, 1, 3, 2);                                   // d = a*c + b per lane
    CHECK(c.f[4].ps0 == 1.0 + u23 && c.f[4].ps1 == -(1.0 + u23));
    ps_nmadd(c, 5, 1, 3, 2);
    CHECK(c.f[5].ps0 == -(1.0 + u23) && c.f[5].ps1 == 1.0 + u23);
    ps_msub(c, 6, 1, 3, 2);                                   // a*c - b
    CHECK(c.f[6].ps0 == fmaSingle(a, cc, -1.0) && c.f[6].ps1 == fmaSingle(-a, cc, 1.0));
    ps_madds0(c, 7, 1, 3, 2);                                 // lane 1 also uses c.ps0
    CHECK(c.f[7].ps1 == fmaSingle(-a, cc, -1.0));
}

}  // namespace

int main() {
    testLockedCacheDma();
    testTrapCondition();
    testFpscrSites();
    testFusedSingle();
    testIntegerArithmetic();
    testRecordAndCompare();
    testRotateMaskShift();
    testMemory();
    testSprAndTimeBase();
    testFloatingPoint();
    testPairedSingles();
    testCallPlumbing();
    std::printf("recomp semantics tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
