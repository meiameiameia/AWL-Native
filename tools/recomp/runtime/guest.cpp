#include "guest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <xmmintrin.h>

#include "hle_abi.h"

namespace awl::rt {

namespace {

constexpr uint32_t kMmioBase = 0xCC000000u;
constexpr uint32_t kEfbBase = 0xC8000000u;   // EFB access window: CPU peeks and pokes of the embedded framebuffer
constexpr uint32_t kEfbEnd = 0xC9000000u;    // exclusive
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
PreemptHook g_preemptHook = nullptr;
uint64_t g_deadline = ~0ull;   // preemption deadline in retired instructions; ~0 = none
const RelModuleDecl* g_relModules = nullptr;
uint32_t g_relModuleCount = 0;

std::vector<Machine*>& machines() {
    static std::vector<Machine*> v;
    return v;
}

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

#ifdef AWL_STORE_WATCH_ENABLED
#pragma warning(push)
#pragma warning(disable : 4996)   // getenv, sscanf, fopen: diagnostic only
// Store watch (diagnostic, architect-approved). AWL_STORE_WATCH=<hex address>,<hex length> [AWL_STORE_WATCH_FROM=<icount>]
// [AWL_STORE_WATCH_MAX=<lines>] [AWL_STORE_WATCH_LOG=<file>] [AWL_STORE_WATCH_VALUE=<hex> [AWL_STORE_WATCH_MASK=<hex>]: only
// stores whose value & mask equals VALUE]. The address is a physical MEM1 address (0xCC008000 selects the GX write-gather
// port). Consecutive stores of one guest pc to adjacent addresses are merged into one line: "W icount=<first>..<last>
// pc=<trap point> fn=<innermost recompiled function> from=<its call site> ea=<first>..<next> n=<stores> size=<bytes>
// first=<value> last=<value> chain=<call sites, innermost first>". pc is the last trap point (at()) passed, so it names the
// loop, not the exact store.
uint32_t g_storeWatchLo = 0, g_storeWatchLen = 0;
namespace {
struct StoreWatchState {
    FILE* out = stderr;
    uint64_t from = 0, maxLines = 200000, lines = 0, hits = 0, value = 0, valueMask = ~0ull;
    bool filterValue = false;   // AWL_STORE_WATCH_VALUE=<hex>: only stores of exactly this value are logged
    bool open = false;
    uint32_t pc = 0, fn = 0, site = 0, startEa = 0, nextEa = 0, size = 0;
    char chain[128] = {};   // call sites of the innermost frames, innermost first
    uint64_t n = 0, firstIc = 0, lastIc = 0, firstVal = 0, lastVal = 0;
    StoreWatchState() {
        if (const char* w = std::getenv("AWL_STORE_WATCH")) {
            unsigned long long a = 0, l = 0;
            if (std::sscanf(w, "%llx,%llx", &a, &l) == 2) { g_storeWatchLo = static_cast<uint32_t>(a) & 0x0FFFFFFFu; g_storeWatchLen = static_cast<uint32_t>(l); }
        }
        if (const char* f = std::getenv("AWL_STORE_WATCH_FROM")) from = std::strtoull(f, nullptr, 0);
        if (const char* v = std::getenv("AWL_STORE_WATCH_VALUE")) { value = std::strtoull(v, nullptr, 16); filterValue = true; }
        if (const char* v = std::getenv("AWL_STORE_WATCH_MASK")) valueMask = std::strtoull(v, nullptr, 16);
        if (const char* m = std::getenv("AWL_STORE_WATCH_MAX")) maxLines = std::strtoull(m, nullptr, 0);
        if (const char* p = std::getenv("AWL_STORE_WATCH_LOG")) if (FILE* f = std::fopen(p, "w")) out = f;
    }
    void flush() {
        if (!open) return;
        open = false;
        if (lines++ >= maxLines) return;
        std::fprintf(out, "W icount=%llu..%llu pc=%08X fn=%08X from=%08X ea=%08X..%08X n=%llu size=%u first=%llX last=%llX chain=%s\n",
                     (unsigned long long)firstIc, (unsigned long long)lastIc, pc, fn, site, startEa, nextEa, (unsigned long long)n, size,
                     (unsigned long long)firstVal, (unsigned long long)lastVal, chain);
    }
    ~StoreWatchState() { flush(); std::fprintf(out, "W total stores into the range: %llu\n", (unsigned long long)hits); if (out != stderr) std::fclose(out); }
};
StoreWatchState& storeWatch() { static StoreWatchState s; return s; }
struct StoreWatchInit { StoreWatchInit() { storeWatch(); } } g_storeWatchInit;
}  // namespace

void storeWatchHit(Ctx& c, uint32_t ea, uint32_t size, uint64_t value) {
    StoreWatchState& s = storeWatch();
    if (c.m->icount < s.from || (s.filterValue && (value & s.valueMask) != s.value)) return;
    ++s.hits;
    const uint32_t fn = c.sp ? c.cs[c.sp - 1].callee : 0, site = c.sp ? c.cs[c.sp - 1].site : 0;
    if (s.open && s.pc == c.pc && s.fn == fn && s.size == size && s.nextEa == ea) {
        s.nextEa = ea + size;
        ++s.n;
        s.lastIc = c.m->icount;
        s.lastVal = value;
        return;
    }
    s.flush();
    s.open = true;
    {
        char* p = s.chain;
        for (uint32_t i = 0; i < 8 && i < c.sp; ++i) {
            const int w = std::snprintf(p, s.chain + sizeof s.chain - p, "%s%08X", i ? "<" : "", c.cs[c.sp - 1 - i].site);
            if (w <= 0 || p + w >= s.chain + sizeof s.chain) break;
            p += w;
        }
        *p = 0;
    }
    s.pc = c.pc; s.fn = fn; s.site = site; s.startEa = ea; s.nextEa = ea + size; s.size = size; s.n = 1;
    s.firstIc = s.lastIc = c.m->icount; s.firstVal = s.lastVal = value;
}
#pragma warning(pop)
#endif

void registerMachine(Machine& m) { machines().push_back(&m); }

void unregisterMachine(Machine& m) {
    auto& v = machines();
    v.erase(std::remove(v.begin(), v.end(), &m), v.end());
}

void refreshPreemption() {
    // icount >= deadline  <=>  icount > deadline - 1; a deadline of 0 is due immediately.
    const uint64_t due = !g_preemptHook || g_deadline == ~0ull ? ~0ull : (g_deadline ? g_deadline - 1 : 0);
    for (Machine* m : machines()) m->checkAt = std::min(m->limit(), due);
}

void checkpointSlow(Ctx& c) {
    Machine& m = *c.m;
    if (m.icount > m.limit()) limitReached(c);
    // The hook does not nest on the context it is already running on; other contexts that reach a due
    // deadline (a fiber the hook switched to) enter it themselves.
    if (!g_preemptHook || c.inPreempt || m.icount < g_deadline) return;
    struct Guard {
        Ctx& c;
        explicit Guard(Ctx& x) : c(x) { c.inPreempt = true; }
        ~Guard() { c.inPreempt = false; }
    } guard(c);
    g_preemptHook(c);
    if (g_deadline <= m.icount)
        stop(c, StopKind::Contract, c.pc, 0, "preemption hook returned without setting a deadline beyond the retired-instruction count");
}

uint8_t* memFault(Ctx& c, uint32_t ea) {
    Machine& m = *c.m;
    const bool mmio = (ea >= kMmioBase && ea < 0xCE000000u) || (ea >= kEfbBase && ea < kEfbEnd);
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

static bool inHardwareWindow(uint32_t ea, uint32_t size) {
    return (ea >= kMmioBase && ea <= kMmioEnd - size) || (ea >= kEfbBase && ea <= kEfbEnd - size);
}

uint64_t loadSlow(Ctx& c, uint32_t ea, uint32_t size) {
    if (g_mmioRead && inHardwareWindow(ea, size)) return g_mmioRead(c, ea, size) & sizeMask(size);
    memFault(c, ea);   // stops, or (census) records the access and falls through to read zero
    return 0;
}

void storeSlow(Ctx& c, uint32_t ea, uint32_t size, uint64_t value) {
    if (g_mmioWrite && inHardwareWindow(ea, size)) { g_mmioWrite(c, ea, size, value & sizeMask(size)); return; }
    memFault(c, ea);   // stops, or (census) records the access and drops the store
}

GuestFn lookupFunction(const Machine& m, uint32_t addr) {
    uint32_t lo = 0, hi = m.functionCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (m.functions[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < m.functionCount && m.functions[lo].addr == addr) return m.functions[lo].fn;
    // Not in the DOL's table: a function of a linked REL module.
    const auto& dyn = m.dynFunctions;
    const auto it = std::lower_bound(dyn.begin(), dyn.end(), addr, [](const Machine::DynFn& e, uint32_t a) { return e.addr < a; });
    return (it != dyn.end() && it->addr == addr) ? it->fn : nullptr;
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
void returnedFromNoReturn(Ctx& c, uint32_t site, uint32_t callee) { stop(c, StopKind::BadReturn, site, callee, "a function the analysis found never returns has returned"); }
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

// Locked-cache DMA, Gekko User's Manual layout (PowerPC bit numbering, bit 0 = MSB):
//   DMA_U (SPR 922): bits 0-26 MEM_ADDR (32-byte aligned), bits 27-31 DMA_LEN[2:6]
//   DMA_L (SPR 923): bits 0-26 LC_ADDR, bit 27 DMA_LD, bits 28-29 DMA_LEN[0:1], bit 30 DMA_T, bit 31 DMA_F
// Writing DMA_L with DMA_T set starts a transfer of DMA_LEN 32-byte blocks (0 means 128) between memory
// and the locked cache: DMA_LD set loads memory into the cache, clear stores the cache to memory.
// Completion is instantaneous, so DMA_T (and DMA_F, the queue flush) read back clear.
static void lockedCacheDma(Ctx& c, uint32_t pc, uint32_t dmaL) {
    constexpr uint32_t kT = 0x2, kLd = 0x10;
    c.spr[923] = dmaL & ~(kT | 1u);
    if (!(dmaL & kT)) return;
    const uint32_t dmaU = c.spr[922];
    uint32_t blocks = ((dmaU & 0x1Fu) << 2) | ((dmaL >> 2) & 3u);
    if (blocks == 0) blocks = 128;
    const uint32_t bytes = blocks * 32;
    const uint32_t memAddr = dmaU & ~0x1Fu, lcAddr = dmaL & ~0x1Fu;
    const uint32_t memSeg = memAddr >> 28, memOff = memAddr & 0x0FFFFFFFu;
    if (!((memSeg == 0x8 || memSeg == 0xC) && memOff <= kMem1Size && bytes <= kMem1Size - memOff))
        stop(c, StopKind::Fault, pc, memAddr, "locked-cache DMA: memory range outside MEM1");
    if (!((lcAddr >> 28) == 0xE && (lcAddr & 0x0FFFFFFFu) <= kLockedCacheSize && bytes <= kLockedCacheSize - (lcAddr & 0x0FFFFFFFu)))
        stop(c, StopKind::Fault, pc, lcAddr, "locked-cache DMA: cache range outside the 16 KB locked cache at 0xE0000000");
    uint8_t* mem = c.m->mem + memOff;
    uint8_t* lc = c.m->lc + (lcAddr & 0x0FFFFFFFu);
    if (dmaL & kLd) std::memcpy(lc, mem, bytes); else std::memcpy(mem, lc, bytes);
}

void mtspr(Ctx& c, uint32_t pc, uint32_t n, uint32_t v) {
    switch (n) {
        case 1: setXer(c, v); return;
        case 8: c.lr = v; return;
        case 9: c.ctr = v; return;
        case 923: lockedCacheDma(c, pc, v); return;
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
    // Copy the handler out of the table: a handler may switch fibers and never return here, and a container
    // iterator (which debug builds register with the container) must not be left on an abandoned stack.
    HleHandler handler = nullptr;
    {
        const auto& t = hleTable();
        const auto it = t.find(guestAddr);
        if (it != t.end()) handler = it->second.handler;
    }
    if (!handler) {
        c.pc = guestAddr;
        throw Stop{StopKind::HleUnregistered, guestAddr, guestAddr, "function replaced by a native handler (HLE) has no registered handler", hleNameFor(guestAddr)};
    }
    at(c, guestAddr, 1);
    const uint32_t lr = c.lr;
    handler(c);
    c.lr = lr;   // the replaced function returns with LR as it entered (blr)
}

void mmioInstall(MmioRead read, MmioWrite write) {
    if ((read == nullptr) != (write == nullptr))
        stopNoCtx(StopKind::Contract, 0, 0, "mmioInstall needs both a read and a write model (or neither, to uninstall)", nullptr);
    g_mmioRead = read;
    g_mmioWrite = write;
}

void timeBaseInstall(TimeBaseSource source) { g_timeBase = source; }

void declareRelModules(const RelModuleDecl* table, uint32_t count) {
    g_relModules = table;
    g_relModuleCount = count;
}

void moduleLinked(Ctx& c, uint32_t info) {
    Machine& m = *c.m;
    const uint32_t id = ld32(c, info);
    const RelModuleDecl* decl = nullptr;
    for (uint32_t i = 0; i < g_relModuleCount; ++i) if (g_relModules[i].id == id) decl = &g_relModules[i];
    if (!decl || id >= kMaxRelModules)
        stop(c, StopKind::Contract, c.pc, id, "OSLink succeeded for a module id that has no generated code (module id in addr)");
    if (m.relLinked[id]) stop(c, StopKind::Contract, c.pc, id, "OSLink succeeded for a module that is already linked (module id in addr)");
    const uint32_t numSections = ld32(c, info + 0x0C);
    const uint32_t table = ld32(c, info + 0x10);
    if (numSections != decl->numSections)
        stop(c, StopKind::Contract, c.pc, numSections, "linked module has a different section count than the generated code (count in addr)");
    uint32_t base[kMaxRelSections] = {};
    for (uint32_t i = 0; i < numSections; ++i) {
        const uint32_t addr = ld32(c, table + 8 * i) & ~3u;
        const uint32_t length = ld32(c, table + 8 * i + 4);
        if (length != decl->sectionLengths[i])
            stop(c, StopKind::Contract, c.pc, length, "linked module section length differs from the generated code (length in addr)");
        base[i] = length ? addr : 0;
    }
    std::vector<Machine::DynFn> added;
    for (uint32_t i = 0; i < decl->fnCount; ++i) {
        const RelFnDecl& f = decl->fns[i];
        if (f.section >= numSections || base[f.section] == 0)
            stop(c, StopKind::Contract, c.pc, f.section, "generated module function lies in a section the linked module does not have (section in addr)");
        added.push_back({base[f.section] + f.offset, f.fn, id});
    }
    auto byAddr = [](const Machine::DynFn& a, const Machine::DynFn& b) { return a.addr < b.addr; };
    std::sort(added.begin(), added.end(), byAddr);
    std::vector<Machine::DynFn> merged(m.dynFunctions.size() + added.size());
    std::merge(m.dynFunctions.begin(), m.dynFunctions.end(), added.begin(), added.end(), merged.begin(), byAddr);
    for (size_t i = 1; i < merged.size(); ++i)
        if (merged[i - 1].addr == merged[i].addr)
            stop(c, StopKind::Contract, c.pc, merged[i].addr, "module function address collides with an already registered function (address in addr)");
    m.dynFunctions = std::move(merged);
    std::memcpy(m.relBase[id], base, sizeof base);
    m.relLinked[id] = true;
}

void moduleUnlinked(Ctx& c, uint32_t info) {
    Machine& m = *c.m;
    const uint32_t id = ld32(c, info);
    if (id >= kMaxRelModules || !m.relLinked[id])
        stop(c, StopKind::Contract, c.pc, id, "OSUnlink succeeded for a module that was not linked through the generated code (module id in addr)");
    auto& v = m.dynFunctions;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const Machine::DynFn& f) { return f.module == id; }), v.end());
    std::memset(m.relBase[id], 0, sizeof m.relBase[id]);
    m.relLinked[id] = false;
}

void preemptInstall(PreemptHook hook) {
    g_preemptHook = hook;
    refreshPreemption();
}

void preemptDeadlineSet(uint64_t deadline) {
    g_deadline = deadline;
    refreshPreemption();
}

uint64_t retiredInstructions(const Ctx& c) { return c.m->icount; }

void setThreadHostData(Ctx& c, void* data) { c.hostData = data; }

void* threadHostData(Ctx& c) { return c.hostData; }

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
