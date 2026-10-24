#include "guest.h"

#include <unordered_map>
#include <xmmintrin.h>

#include "hle_abi.h"

namespace awl::rt {

namespace {

constexpr uint32_t kMmioBase = 0xCC000000u;
constexpr uint32_t kMmioEnd = 0xCC010000u;   // exclusive: the modelled hardware window is 0xCC000000-0xCC00FFFF

struct HleEntry {
    const char* name;
    HleHandler handler;
};

// Process-wide platform hooks. Registration happens at startup before guest code runs, so these are plain
// statics; the table is function-local so static initialisation order cannot bite code that registers early.
std::unordered_map<uint32_t, HleEntry>& hleTable() {
    static std::unordered_map<uint32_t, HleEntry> t;
    return t;
}
const HleName* g_hleNames = nullptr;
uint32_t g_hleNameCount = 0;
MmioRead g_mmioRead = nullptr;
MmioWrite g_mmioWrite = nullptr;
TimeBaseSource g_timeBase = nullptr;

const char* hleNameFor(uint32_t addr) {
    for (uint32_t i = 0; i < g_hleNameCount; ++i)
        if (g_hleNames[i].addr == addr) return g_hleNames[i].name;
    return nullptr;
}

uint64_t sizeMask(uint32_t size) { return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1; }

}  // namespace

void stop(Ctx& c, StopKind k, uint32_t pc, uint32_t addr, const char* what) {
    c.pc = pc;
    throw Stop{k, pc, addr, what};
}

void stopNoCtx(StopKind k, uint32_t pc, uint32_t addr, const char* what, const char* name) {
    throw Stop{k, pc, addr, what, name};
}

void platformBoundary(Ctx& c, uint32_t fnAddr, const char* evidence) {
    Machine& m = *c.m;
    if (m.skipPlatform) {
        m.boundaries.push_back(BoundaryHit{fnAddr, evidence, m.icount, c.sp ? c.cs[c.sp - 1].site : 0});
        c.r[3] = 0;   // census only: the routine is skipped and reports 0
        return;
    }
    stop(c, StopKind::Platform, fnAddr, fnAddr, evidence);
}

void limitReached(Ctx& c) { stop(c, StopKind::InstructionLimit, c.pc, 0, "instruction limit reached"); }

uint8_t* memFault(Ctx& c, uint32_t ea) {
    Machine& m = *c.m;
    const bool mmio = ea >= kMmioBase && ea < 0xCE000000u;
    if (m.skipPlatform) {
        m.faults.push_back(BoundaryHit{ea, mmio ? "MMIO access" : "unmapped access", m.icount, c.pc});
        std::memset(c.scratch, 0, sizeof c.scratch);
        return c.scratch;
    }
    if (mmio) {
        if (g_mmioRead) stop(c, StopKind::Mmio, c.pc, ea, "access to the hardware register area outside the modelled window 0xCC000000-0xCC00FFFF");
        stop(c, StopKind::Mmio, c.pc, ea, "access to the hardware register window 0xCC000000 (no MMIO model installed)");
    }
    stop(c, StopKind::Fault, c.pc, ea, "access outside MEM1 and the locked cache");
}

uint64_t loadSlow(Ctx& c, uint32_t ea, uint32_t size) {
    if (g_mmioRead && ea >= kMmioBase && ea <= kMmioEnd - size) return g_mmioRead(c, ea, size) & sizeMask(size);
    memFault(c, ea);   // stops, or (census) records the access and falls through to read zero
    return 0;
}

void storeSlow(Ctx& c, uint32_t ea, uint32_t size, uint64_t value) {
    if (g_mmioWrite && ea >= kMmioBase && ea <= kMmioEnd - size) { g_mmioWrite(c, ea, size, value & sizeMask(size)); return; }
    memFault(c, ea);   // stops, or (census) records the access and drops the store
}

GuestFn lookupFunction(const Machine& m, uint32_t addr) {
    uint32_t lo = 0, hi = m.functionCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (m.functions[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < m.functionCount && m.functions[lo].addr == addr) ? m.functions[lo].fn : nullptr;
}

void callIndirect(Ctx& c, uint32_t site, uint32_t target) {
    GuestFn fn = lookupFunction(*c.m, target);
    if (!fn) stop(c, StopKind::BadIndirect, site, target, "indirect call target is not a recompiled function entry");
    pushFrame(c, site, target);
    fn(c);
    popFrame(c);
}

void tailIndirect(Ctx& c, uint32_t site, uint32_t target) {
    GuestFn fn = lookupFunction(*c.m, target);
    if (!fn) stop(c, StopKind::BadIndirect, site, target, "indirect tail-call target is not a recompiled function entry");
    fn(c);
}

void unresolvedJump(Ctx& c, uint32_t site) { stop(c, StopKind::UnresolvedJump, site, c.ctr, "bctr with no statically resolved targets"); }
void badJump(Ctx& c, uint32_t site, uint32_t target) { stop(c, StopKind::BadIndirect, site, target, "jump-table index outside the resolved targets"); }
void unimplemented(Ctx& c, uint32_t site, const char* what) { stop(c, StopKind::Unimplemented, site, 0, what); }
void privileged(Ctx& c, uint32_t site, const char* what) { stop(c, StopKind::Privileged, site, 0, what); }
void badReturn(Ctx& c, uint32_t site) { stop(c, StopKind::BadReturn, site, c.lr, "blr with LR different from the shadow-stack return address"); }

uint64_t timeBase(Ctx& c) { return g_timeBase ? g_timeBase(c) : c.m->icount / 4; }

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

// ---- hle_abi.h -------------------------------------------------------------------------------------------

void declareHleNames(const HleName* table, uint32_t count) {
    g_hleNames = table;
    g_hleNameCount = count;
}

void hleRegister(uint32_t guestAddr, const char* name, HleHandler handler) {
    if (!handler) stopNoCtx(StopKind::Contract, guestAddr, guestAddr, "hleRegister with a null handler", name);
    const auto ins = hleTable().emplace(guestAddr, HleEntry{name, handler});
    if (!ins.second)
        stopNoCtx(StopKind::Contract, guestAddr, guestAddr, "hleRegister: this address is already registered (name shown is the existing registration)", ins.first->second.name);
}

void hleCall(Ctx& c, uint32_t guestAddr) {
    const auto& t = hleTable();
    const auto it = t.find(guestAddr);
    if (it == t.end()) {
        c.pc = guestAddr;
        throw Stop{StopKind::HleUnregistered, guestAddr, guestAddr, "function replaced by a native handler (HLE) has no registered handler", hleNameFor(guestAddr)};
    }
    at(c, guestAddr, 1);
    const uint32_t lr = c.lr;
    it->second.handler(c);
    c.lr = lr;   // the replaced function returns with LR as it entered (blr)
}

void mmioInstall(MmioRead read, MmioWrite write) {
    if ((read == nullptr) != (write == nullptr))
        stopNoCtx(StopKind::Contract, 0, 0, "mmioInstall needs both a read and a write model (or neither, to uninstall)", nullptr);
    g_mmioRead = read;
    g_mmioWrite = write;
}

void timeBaseInstall(TimeBaseSource source) { g_timeBase = source; }

Ctx* createThreadContext(Ctx& parent) { return new Ctx(*parent.m); }

void destroyThreadContext(Ctx* c) { delete c; }

void callGuest(Ctx& c, uint32_t guestFn) {
    if (!lookupFunction(*c.m, guestFn)) stop(c, StopKind::BadIndirect, c.pc, guestFn, "callGuest target is not a recompiled function entry");
    const uint32_t lr = c.lr, pc = c.pc;
    callIndirect(c, pc, guestFn);   // frame site = the current guest pc (the replaced function calling back)
    c.lr = lr;
    c.pc = pc;
}

}  // namespace awl::rt
