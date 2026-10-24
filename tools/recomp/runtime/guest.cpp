#include "guest.h"

#include <xmmintrin.h>

namespace awl::rt {

void stop(Ctx& c, StopKind k, uint32_t pc, uint32_t addr, const char* what) {
    c.pc = pc;
    throw Stop{k, pc, addr, what};
}

void platformBoundary(Ctx& c, uint32_t fnAddr, const char* evidence) {
    if (c.skipPlatform) {
        c.boundaries.push_back(BoundaryHit{fnAddr, evidence, c.icount, c.sp ? c.cs[c.sp - 1].site : 0});
        c.r[3] = 0;   // census only: the routine is skipped and reports 0
        return;
    }
    stop(c, StopKind::Platform, fnAddr, fnAddr, evidence);
}

void limitReached(Ctx& c) { stop(c, StopKind::InstructionLimit, c.pc, 0, "instruction limit reached"); }

uint8_t* memFault(Ctx& c, uint32_t ea) {
    if (c.skipPlatform) {
        c.faults.push_back(BoundaryHit{ea, (ea >= 0xCC000000u && ea < 0xCE000000u) ? "MMIO access" : "unmapped access", c.icount, c.pc});
        std::memset(c.scratch, 0, sizeof c.scratch);
        return c.scratch;
    }
    if (ea >= 0xCC000000u && ea < 0xCE000000u) stop(c, StopKind::Mmio, c.pc, ea, "access to the hardware register window 0xCC000000");
    stop(c, StopKind::Fault, c.pc, ea, "access outside MEM1 and the locked cache");
}

GuestFn lookupFunction(uint32_t addr) {
    uint32_t lo = 0, hi = kFunctionCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (kFunctions[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < kFunctionCount && kFunctions[lo].addr == addr) ? kFunctions[lo].fn : nullptr;
}

void callIndirect(Ctx& c, uint32_t site, uint32_t target) {
    GuestFn fn = lookupFunction(target);
    if (!fn) stop(c, StopKind::BadIndirect, site, target, "indirect call target is not a recompiled function entry");
    pushFrame(c, site, target);
    fn(c);
    popFrame(c);
}

void tailIndirect(Ctx& c, uint32_t site, uint32_t target) {
    GuestFn fn = lookupFunction(target);
    if (!fn) stop(c, StopKind::BadIndirect, site, target, "indirect tail-call target is not a recompiled function entry");
    fn(c);
}

void unresolvedJump(Ctx& c, uint32_t site) { stop(c, StopKind::UnresolvedJump, site, c.ctr, "bctr with no statically resolved targets"); }
void badJump(Ctx& c, uint32_t site, uint32_t target) { stop(c, StopKind::BadIndirect, site, target, "jump-table index outside the resolved targets"); }
void unimplemented(Ctx& c, uint32_t site, const char* what) { stop(c, StopKind::Unimplemented, site, 0, what); }
void privileged(Ctx& c, uint32_t site, const char* what) { stop(c, StopKind::Privileged, site, 0, what); }
void badReturn(Ctx& c, uint32_t site) { stop(c, StopKind::BadReturn, site, c.lr, "blr with LR different from the shadow-stack return address"); }

uint32_t mfspr(Ctx& c, uint32_t, uint32_t n) {
    switch (n) {
        case 1: return xerValue(c);
        case 8: return c.lr;
        case 9: return c.ctr;
        case 268: return static_cast<uint32_t>(timeBase(c));
        case 269: return static_cast<uint32_t>(timeBase(c) >> 32);
        case 287: return 0x00083214u;   // Gekko processor version register value used by GameCube
        case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: return c.gqr[n - 912];
        default: return n < 1024 ? c.spr[n] : 0;
    }
}

void mtspr(Ctx& c, uint32_t, uint32_t n, uint32_t v) {
    switch (n) {
        case 1: setXer(c, v); return;
        case 8: c.lr = v; return;
        case 9: c.ctr = v; return;
        case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c.gqr[n - 912] = v; return;
        default: if (n < 1024) c.spr[n] = v; return;
    }
}

void syncHostFp(uint32_t fpscr) {
    static const unsigned rc[4] = {0, 3, 2, 1};   // PPC RN 0 nearest, 1 zero, 2 +inf, 3 -inf -> MXCSR RC field
    unsigned csr = 0x1F80u | (rc[fpscr & 3] << 13);
    if (fpscr & 4) csr |= 0x8040u;                // NI: flush denormal results (FTZ) and inputs (DAZ) to zero
    _mm_setcsr(csr);
}

}  // namespace awl::rt
