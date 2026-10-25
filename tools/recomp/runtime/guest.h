// Guest runtime for statically recompiled Gekko code.
// Generated functions take a Ctx&, keep guest registers in it, and reach guest memory through the
// big-endian helpers below. Anything the runtime cannot run throws rt::Stop with the guest pc.
// State is split in two: Machine is the shared guest machine (memory, function table, retired-instruction
// counter, limits, diagnostics) and Ctx is the CPU state of one guest thread, which points at its Machine.
// The contract with the platform runtime (HLE handlers, MMIO models, thread contexts, callGuest, time
// base) is declared in hle_abi.h and implemented in guest.cpp.
//
// Build generated code and anything including this header with /fp:strict: the helpers rely on the host
// FP environment (rounding mode, FTZ/DAZ) following the guest FPSCR, so the compiler must not fold or
// reorder FP operations across them.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <vector>

namespace awl::rt {

constexpr uint32_t kMem1Size = 0x01800000;       // 24 MB main memory
constexpr uint32_t kLockedCacheSize = 0x4000;    // 16 KB data cache locked at 0xE0000000
constexpr uint32_t kMaxCallDepth = 8192;
constexpr uint32_t kMaxRelModules = 8;     // module ids 1..7 (id 0 is the DOL)
constexpr uint32_t kMaxRelSections = 32;

struct Fpr {
    double ps0 = 0.0;
    double ps1 = 0.0;
};

enum class StopKind : uint32_t {
    Platform,         // call into a routine classified as hardware/OS dependent
    Mmio,             // load/store to the 0xCC000000 hardware register window
    Privileged,       // supervisor instruction the runtime does not model
    UnresolvedJump,   // bctr whose targets static analysis could not determine
    BadIndirect,      // computed branch/call to an address with no recompiled function
    BadReturn,        // blr whose LR does not match the shadow call stack
    Fault,            // access outside every modelled memory region
    Unimplemented,    // instruction form the generator refuses to translate
    DepthExceeded,
    InstructionLimit,
    HleUnregistered,  // hleCall on an address with no registered handler
    Contract          // misuse of the runtime contract in hle_abi.h (duplicate registration, null handler, ...)
};

struct Stop {
    StopKind kind;
    uint32_t pc;       // guest address of the instruction (or function) that stopped
    uint32_t addr;     // effective address / target involved
    const char* what;  // static description
    const char* name = nullptr;   // function name when one is known (HLE); static storage
};

struct CallFrame {
    uint32_t site;     // address of the bl/bctrl
    uint32_t callee;   // function entered
    uint32_t ret;      // value LR was set to
};

struct BoundaryHit {
    uint32_t addr;
    const char* evidence;
    uint64_t icount;
    uint32_t callSite;
};

struct TraceHit {
    uint32_t callee;
    uint32_t site;
    uint32_t depth;
    uint64_t icount;
    uint32_t args[4];       // r3-r6 at the call
};

struct Ctx;
using GuestFn = void (*)(Ctx&);

struct FnEntry {
    uint32_t addr;
    GuestFn fn;
};

// Provided by the generated dispatch table (sorted by address).
extern const FnEntry kFunctions[];
extern const uint32_t kFunctionCount;

struct Machine;
void registerMachine(Machine& m);     // preemption deadlines apply to every live machine
void unregisterMachine(Machine& m);
void refreshPreemption();             // recompute Machine::checkAt after the limit, hook or deadline changes

// Shared guest machine: one per process, reached by every thread context. Guest threads are cooperative
// fibers (the platform track schedules them), so none of this needs locking.
struct Machine {
    Machine() { registerMachine(*this); refreshPreemption(); }
    ~Machine() { unregisterMachine(*this); }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    uint8_t* mem = nullptr;        // kMem1Size bytes, guest 0x80000000 / 0xC0000000 view
    uint8_t* lc = nullptr;         // kLockedCacheSize bytes at 0xE0000000
    const FnEntry* functions = kFunctions;       // recompiled function entries, sorted by address
    uint32_t functionCount = kFunctionCount;
    uint64_t icount = 0;           // instructions retired by all threads (default time base)
    // The one value checkpoints compare the retired count against: min(instruction limit, deadline - 1),
    // so a single compare serves both. Derived; change the limit with setLimit, the deadline with
    // preemptDeadlineSet.
    uint64_t checkAt = ~0ull;
    uint64_t limit() const { return limit_; }
    void setLimit(uint64_t v) { limit_ = v; refreshPreemption(); }   // stop after this many retired instructions
    bool skipPlatform = false;     // census mode: record platform boundaries and return instead of stopping
    std::vector<BoundaryHit> boundaries;
    std::vector<TraceHit> trace;       // --trace: calls in execution order
    uint32_t traceMaxDepth = 0;        // 0 = tracing off; otherwise record calls up to this depth
    std::vector<BoundaryHit> faults;   // census mode: unmapped/MMIO accesses that were absorbed

    // Linked REL modules. relBase[id][section] is the guest address of a section of the module with that id
    // (0 when absent or not linked); generated REL code forms every address from it. dynFunctions holds the
    // linked modules' recompiled functions at their linked addresses, sorted by address (see moduleLinked).
    struct DynFn { uint32_t addr; GuestFn fn; uint32_t module; };
    uint32_t relBase[kMaxRelModules][kMaxRelSections] = {};
    bool relLinked[kMaxRelModules] = {};
    std::vector<DynFn> dynFunctions;

private:
    uint64_t limit_ = ~0ull;
};

// CPU state of one guest thread. Create the first one from a Machine and further ones with
// createThreadContext; every context keeps its own registers and shadow call stack.
struct Ctx {
    explicit Ctx(Machine& machine) : m(&machine) {}
    Machine* m;
    uint32_t r[32] = {};
    Fpr f[32];
    uint32_t cr = 0;
    uint32_t so = 0, ov = 0, ca = 0, xerbc = 0;
    uint32_t lr = 0, ctr = 0;
    uint32_t fpscr = 0;
    uint32_t gqr[8] = {};
    uint32_t spr[1024] = {};     // storage-only SPRs (HID0/1/2, WPAR, DMA, L2CR, BATs, SRR0/1, SPRG*, ...)
    uint32_t msr = 0;
    uint32_t sr[16] = {};
    uint32_t pc = 0;
    uint32_t reservation = 0;
    bool reservationValid = false;
    uint8_t scratch[16] = {};
    void* hostData = nullptr;    // platform per-thread data (setThreadHostData)
    bool inPreempt = false;      // the preemption hook is running on this context
    uint32_t sp = 0;
    CallFrame cs[kMaxCallDepth];
};

// ---- Stops ---------------------------------------------------------------------------------------
[[noreturn]] void stop(Ctx& c, StopKind k, uint32_t pc, uint32_t addr, const char* what);
void platformBoundary(Ctx& c, uint32_t fnAddr, const char* evidence);   // throws unless c.skipPlatform
[[noreturn]] void limitReached(Ctx& c);
void checkpointSlow(Ctx& c);   // limit stop, or the preemption hook when the deadline is due
uint8_t* memFault(Ctx& c, uint32_t ea);   // throws unless skipPlatform, then returns a zeroed scratch cell
// Stops that have no thread context to blame (registration-time contract violations).
[[noreturn]] void stopNoCtx(StopKind k, uint32_t pc, uint32_t addr, const char* what, const char* name);

// ---- Call plumbing ---------------------------------------------------------------------------------
GuestFn lookupFunction(const Machine& m, uint32_t addr);   // nullptr when addr is not a recompiled function entry

// Names for the functions the generator replaced with hleCall, so an unregistered handler is reported
// by name. Called once from the generated dispatch table's static initialiser; table must outlive the run.
struct HleName {
    uint32_t addr;
    const char* name;
};
void declareHleNames(const HleName* table, uint32_t count);

// Generated REL modules. Each module's functions are known by (section, offset); their guest addresses exist
// only once the SDK's OSLink has placed the module. The generated dispatch table declares one RelModuleDecl per
// module (static initialiser, like declareHleNames); the generated OSLink/OSUnlink wrappers call moduleLinked /
// moduleUnlinked after a successful link/unlink with the OSModuleInfo pointer that was passed in r3.
struct RelFnDecl {
    uint32_t section;
    uint32_t offset;
    GuestFn fn;
};
struct RelModuleDecl {
    uint32_t id;
    uint32_t numSections;
    const uint32_t* sectionLengths;   // numSections entries, as in the REL the code was generated from
    const RelFnDecl* fns;
    uint32_t fnCount;
};
void declareRelModules(const RelModuleDecl* table, uint32_t count);
// Reads the linked module's section table from guest memory (OSModuleInfo: id at +0, section count at +0x0C,
// absolute section-table address at +0x10; entries are {address | exec bit, length}), checks it against the
// generated module, records the section bases and registers the module's functions at their linked addresses.
// Stops loudly for an unknown module id, a section count or length that differs from the generated module, a
// module that is already linked, or a function address that collides with a registered one.
void moduleLinked(Ctx& c, uint32_t info);
// Removes the module's functions and section bases.
void moduleUnlinked(Ctx& c, uint32_t info);

// Retire n instructions on the shared counter (straight-line code between trap points).
inline void tick(Ctx& c, uint32_t n) { c.m->icount += n; }

inline void at(Ctx& c, uint32_t pc, uint32_t n) {
    c.pc = pc;
    Machine& m = *c.m;
    m.icount += n;
    if (m.icount > m.checkAt) checkpointSlow(c);
}

inline void pushFrame(Ctx& c, uint32_t site, uint32_t callee) {
    if (c.sp >= kMaxCallDepth) stop(c, StopKind::DepthExceeded, site, callee, "guest call depth exceeded");
    c.lr = site + 4;
    Machine& m = *c.m;
    if (m.traceMaxDepth && c.sp < m.traceMaxDepth) m.trace.push_back(TraceHit{callee, site, c.sp, m.icount, {c.r[3], c.r[4], c.r[5], c.r[6]}});
    c.cs[c.sp++] = CallFrame{site, callee, site + 4};
}
inline void popFrame(Ctx& c) { --c.sp; }

// bl: LR := site+4, run callee. The shadow stack records (site, callee) for traces and blr validation.
#define AWL_CALL(c, site, callee_addr, fn) \
    do { ::awl::rt::pushFrame((c), (site), (callee_addr)); fn(c); ::awl::rt::popFrame((c)); } while (0)

void callIndirect(Ctx& c, uint32_t site, uint32_t target);                 // bctrl / blrl
void tailIndirect(Ctx& c, uint32_t site, uint32_t target);                 // bctr tail call (no frame)
[[noreturn]] void unresolvedJump(Ctx& c, uint32_t site);
[[noreturn]] void badJump(Ctx& c, uint32_t site, uint32_t target);
[[noreturn]] void unimplemented(Ctx& c, uint32_t site, const char* what);
[[noreturn]] void privileged(Ctx& c, uint32_t site, const char* what);
[[noreturn]] void badReturn(Ctx& c, uint32_t site);
[[noreturn]] void returnedFromNoReturn(Ctx& c, uint32_t site, uint32_t callee);   // a function the analysis found never returns did

// blr: validates LR against the shadow stack. Top-level entries (sp == 0) are unchecked.
inline void ret(Ctx& c, uint32_t site) {
    if (c.sp != 0 && c.lr != c.cs[c.sp - 1].ret) badReturn(c, site);
}

// ---- Memory ----------------------------------------------------------------------------------------
// Host pointer for plain memory (MEM1 and the locked cache), nullptr elsewhere.
inline uint8_t* hostMem(Ctx& c, uint32_t ea, uint32_t n) {
    const uint32_t seg = ea >> 28;
    const uint32_t off = ea & 0x0FFFFFFFu;
    if ((seg == 0x8 || seg == 0xC) && off <= kMem1Size - n) return c.m->mem + off;
    if (seg == 0xE && off <= kLockedCacheSize - n) return c.m->lc + off;
    return nullptr;
}
// For accesses that cannot be routed to a hardware model (dcbz): stops or absorbs outside plain memory.
inline uint8_t* host(Ctx& c, uint32_t ea, uint32_t n) {
    uint8_t* p = hostMem(c, ea, n);
    return p ? p : memFault(c, ea);
}

// Loads and stores outside plain memory: the hardware window 0xCC000000-0xCC00FFFF goes to the installed
// MMIO model (value in guest numeric order); anything else stops (or is absorbed in census mode).
uint64_t loadSlow(Ctx& c, uint32_t ea, uint32_t size);
void storeSlow(Ctx& c, uint32_t ea, uint32_t size, uint64_t value);

inline uint8_t ld8(Ctx& c, uint32_t ea) { const uint8_t* p = hostMem(c, ea, 1); return p ? *p : static_cast<uint8_t>(loadSlow(c, ea, 1)); }
inline uint16_t ld16(Ctx& c, uint32_t ea) {
    const uint8_t* p = hostMem(c, ea, 2);
    if (!p) return static_cast<uint16_t>(loadSlow(c, ea, 2));
    uint16_t v; std::memcpy(&v, p, 2); return _byteswap_ushort(v);
}
inline uint32_t ld32(Ctx& c, uint32_t ea) {
    const uint8_t* p = hostMem(c, ea, 4);
    if (!p) return static_cast<uint32_t>(loadSlow(c, ea, 4));
    uint32_t v; std::memcpy(&v, p, 4); return _byteswap_ulong(v);
}
inline uint64_t ld64(Ctx& c, uint32_t ea) {
    const uint8_t* p = hostMem(c, ea, 8);
    if (!p) return loadSlow(c, ea, 8);
    uint64_t v; std::memcpy(&v, p, 8); return _byteswap_uint64(v);
}
inline void st8(Ctx& c, uint32_t ea, uint32_t v) {
    uint8_t* p = hostMem(c, ea, 1);
    if (p) *p = static_cast<uint8_t>(v); else storeSlow(c, ea, 1, v & 0xFFu);
}
inline void st16(Ctx& c, uint32_t ea, uint32_t v) {
    uint8_t* p = hostMem(c, ea, 2);
    if (!p) { storeSlow(c, ea, 2, v & 0xFFFFu); return; }
    const uint16_t b = _byteswap_ushort(static_cast<uint16_t>(v)); std::memcpy(p, &b, 2);
}
inline void st32(Ctx& c, uint32_t ea, uint32_t v) {
    uint8_t* p = hostMem(c, ea, 4);
    if (!p) { storeSlow(c, ea, 4, v); return; }
    const uint32_t b = _byteswap_ulong(v); std::memcpy(p, &b, 4);
}
inline void st64(Ctx& c, uint32_t ea, uint64_t v) {
    uint8_t* p = hostMem(c, ea, 8);
    if (!p) { storeSlow(c, ea, 8, v); return; }
    const uint64_t b = _byteswap_uint64(v); std::memcpy(p, &b, 8);
}

inline uint32_t ld16s(Ctx& c, uint32_t ea) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ld16(c, ea)))); }
inline uint32_t ld16r(Ctx& c, uint32_t ea) { return _byteswap_ushort(ld16(c, ea)); }
inline uint32_t ld32r(Ctx& c, uint32_t ea) { return _byteswap_ulong(ld32(c, ea)); }
inline void st16r(Ctx& c, uint32_t ea, uint32_t v) { st16(c, ea, _byteswap_ushort(static_cast<uint16_t>(v))); }
inline void st32r(Ctx& c, uint32_t ea, uint32_t v) { st32(c, ea, _byteswap_ulong(v)); }

inline float bitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t floatToBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline double bitsToDouble(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
inline uint64_t doubleToBits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

// lwarx/stwcx. on a single hardware thread: the reservation is lost by any stwcx. and by dcbz on the line.
inline uint32_t lwarx(Ctx& c, uint32_t ea) { c.reservation = ea & ~31u; c.reservationValid = true; return ld32(c, ea); }
inline void stwcx(Ctx& c, uint32_t ea, uint32_t v) {
    const bool ok = c.reservationValid && c.reservation == (ea & ~31u);
    if (ok) st32(c, ea, v);
    c.reservationValid = false;
    c.cr = (c.cr & 0x0FFFFFFFu) | ((ok ? 2u : 0u) << 28) | (c.so << 28);
}
inline void dcbz(Ctx& c, uint32_t ea) {
    std::memset(host(c, ea & ~31u, 32), 0, 32);
    if (c.reservationValid && c.reservation == (ea & ~31u)) c.reservationValid = false;
}

// lmw/stmw/lsw/stsw
inline void lmw(Ctx& c, uint32_t rd, uint32_t ea) { for (uint32_t i = rd; i < 32; ++i, ea += 4) c.r[i] = ld32(c, ea); }
inline void stmw(Ctx& c, uint32_t rs, uint32_t ea) { for (uint32_t i = rs; i < 32; ++i, ea += 4) st32(c, ea, c.r[i]); }
inline void lsw(Ctx& c, uint32_t rd, uint32_t ea, uint32_t n) {
    uint32_t reg = rd, shift = 24;
    if (n) c.r[reg] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        c.r[reg] |= static_cast<uint32_t>(ld8(c, ea + i)) << shift;
        if (shift == 0) { shift = 24; reg = (reg + 1) & 31; if (i + 1 < n) c.r[reg] = 0; } else shift -= 8;
    }
}
inline void stsw(Ctx& c, uint32_t rs, uint32_t ea, uint32_t n) {
    uint32_t reg = rs, shift = 24;
    for (uint32_t i = 0; i < n; ++i) {
        st8(c, ea + i, c.r[reg] >> shift);
        if (shift == 0) { shift = 24; reg = (reg + 1) & 31; } else shift -= 8;
    }
}

// ---- Integer helpers -------------------------------------------------------------------------------
constexpr uint32_t mask(uint32_t mb, uint32_t me) {
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}
inline uint32_t sext16(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }
inline uint32_t rotl(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }

// tw/twi trap condition: TO bit 16 signed less-than, 8 signed greater-than, 4 equal, 2 unsigned less-than, 1 unsigned greater-than.
inline bool trapCond(uint32_t to, uint32_t a, uint32_t b) {
    const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
    return ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) || ((to & 2) && a < b) || ((to & 1) && a > b);
}

inline void setCr(Ctx& c, uint32_t field, uint32_t v) {
    const uint32_t sh = (7 - field) * 4;
    c.cr = (c.cr & ~(0xFu << sh)) | ((v & 0xFu) << sh);
}
inline uint32_t getCr(const Ctx& c, uint32_t field) { return (c.cr >> ((7 - field) * 4)) & 0xFu; }
inline uint32_t crBit(const Ctx& c, uint32_t bit) { return (c.cr >> (31 - bit)) & 1u; }
inline void setCrBit(Ctx& c, uint32_t bit, uint32_t v) {
    c.cr = (c.cr & ~(1u << (31 - bit))) | ((v & 1u) << (31 - bit));
}
inline void cr0(Ctx& c, uint32_t result) {
    const int32_t s = static_cast<int32_t>(result);
    setCr(c, 0, (s < 0 ? 8u : s > 0 ? 4u : 2u) | c.so);
}
inline void cmps(Ctx& c, uint32_t f, uint32_t a, uint32_t b) {
    const int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    setCr(c, f, (x < y ? 8u : x > y ? 4u : 2u) | c.so);
}
inline void cmpu(Ctx& c, uint32_t f, uint32_t a, uint32_t b) {
    setCr(c, f, (a < b ? 8u : a > b ? 4u : 2u) | c.so);
}

// a + b + cin with optional CA / OV update (XO-form semantics; subtraction passes ~a).
template <bool SetCa, bool SetOv>
inline uint32_t addx(Ctx& c, uint32_t a, uint32_t b, uint32_t cin) {
    const uint64_t s = static_cast<uint64_t>(a) + b + cin;
    const uint32_t r = static_cast<uint32_t>(s);
    if (SetCa) c.ca = static_cast<uint32_t>(s >> 32);
    if (SetOv) {
        const uint32_t o = ((a ^ r) & (b ^ r)) >> 31;
        c.ov = o;
        if (o) c.so = 1;
    }
    return r;
}
template <bool SetOv>
inline uint32_t mullw(Ctx& c, uint32_t a, uint32_t b) {
    const int64_t p = static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b);
    if (SetOv) {
        const uint32_t o = (p != static_cast<int32_t>(p)) ? 1u : 0u;
        c.ov = o;
        if (o) c.so = 1;
    }
    return static_cast<uint32_t>(p);
}
inline uint32_t mulhw(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)) >> 32);
}
inline uint32_t mulhwu(uint32_t a, uint32_t b) { return static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32); }
// Division by zero and INT_MIN / -1 are architecturally undefined; the values below follow the common
// hardware-observed results (see docs/research/recomp-feasibility.md, semantics risks).
template <bool SetOv>
inline uint32_t divw(Ctx& c, uint32_t a, uint32_t b) {
    const int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    uint32_t r;
    bool o = false;
    if (y == 0 || (x == INT32_MIN && y == -1)) { r = x < 0 ? 0xFFFFFFFFu : 0u; o = true; }
    else r = static_cast<uint32_t>(x / y);
    if (SetOv) { c.ov = o; if (o) c.so = 1; }
    return r;
}
template <bool SetOv>
inline uint32_t divwu(Ctx& c, uint32_t a, uint32_t b) {
    uint32_t r = 0;
    const bool o = b == 0;
    if (!o) r = a / b;
    if (SetOv) { c.ov = o; if (o) c.so = 1; }
    return r;
}
inline uint32_t cntlzw(uint32_t x) {
    unsigned long i;
    return _BitScanReverse(&i, x) ? 31u - i : 32u;
}
inline uint32_t slw(uint32_t s, uint32_t n) { n &= 63; return n >= 32 ? 0u : s << n; }
inline uint32_t srw(uint32_t s, uint32_t n) { n &= 63; return n >= 32 ? 0u : s >> n; }
inline uint32_t sraw(Ctx& c, uint32_t s, uint32_t n) {
    n &= 63;
    const int32_t v = static_cast<int32_t>(s);
    if (n >= 32) { c.ca = v < 0; return v < 0 ? 0xFFFFFFFFu : 0u; }
    c.ca = (v < 0 && n && (s & ((1u << n) - 1)) != 0) ? 1u : 0u;
    return static_cast<uint32_t>(v >> n);
}
inline uint32_t xerValue(const Ctx& c) { return (c.so << 31) | (c.ov << 30) | (c.ca << 29) | (c.xerbc & 0x7F); }
inline void setXer(Ctx& c, uint32_t v) { c.so = v >> 31; c.ov = (v >> 30) & 1; c.ca = (v >> 29) & 1; c.xerbc = v & 0x7F; }
inline uint32_t mfcr(const Ctx& c) { return c.cr; }
inline void mtcrf(Ctx& c, uint32_t crm, uint32_t v) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < 8; ++i) if (crm & (0x80u >> i)) m |= 0xFu << (28 - 4 * i);
    c.cr = (c.cr & ~m) | (v & m);
}

// ---- SPR / time base -------------------------------------------------------------------------------
// Until the platform installs a source (timeBaseInstall), the time base advances deterministically with
// the machine's retired instructions (assumption: ~3 cycles per instruction, 12 CPU cycles per TB tick ->
// 4 instructions per tick).
uint64_t timeBase(Ctx& c);
uint32_t mfspr(Ctx& c, uint32_t pc, uint32_t n);
void mtspr(Ctx& c, uint32_t pc, uint32_t n, uint32_t v);
inline uint32_t mftb(Ctx& c, uint32_t n) { return n == 269 ? static_cast<uint32_t>(timeBase(c) >> 32) : static_cast<uint32_t>(timeBase(c)); }

// ---- Floating point --------------------------------------------------------------------------------
// FPSCR bits (little-endian numbering of the 32-bit value): RN = bits 0-1, NI = bit 2.
void syncHostFp(uint32_t fpscr);          // rounding mode + flush-to-zero from RN/NI
inline void setFpscr(Ctx& c, uint32_t v) { c.fpscr = v; syncHostFp(v); }
inline void cr1(Ctx& c) { setCr(c, 1, c.fpscr >> 28); }

inline double fround(double x) { return static_cast<double>(static_cast<float>(x)); }

// Host floating-point control word (MXCSR on x86), behind small wrappers so the rest of the runtime does not name the intrinsic.
inline unsigned hostFpGet() { return _mm_getcsr(); }
inline void hostFpSet(unsigned v) { _mm_setcsr(v); }

// Fused multiply-add rounded once to single precision, for fmadds/fmsubs/fnmadds/fnmsubs and the ps_ forms.
// fround(std::fma(a, c, b)) rounds twice (to double, then to single), which can pick the wrong float; for +, -, *, /
// and sqrt a 53-bit intermediate is enough (Figueroa 1995: wider precision q >= 2p + 2, here 53 >= 50), but not for an
// exact a*c + b. Here the exact result is first rounded to ODD in double (the neighbour with an odd last mantissa bit
// when the result is inexact), which keeps the sticky information, and the conversion to single then rounds that value
// to nearest (or in the guest's directed mode): round-to-odd at p + 2 or more bits followed by any rounding to p bits
// equals the single correct rounding (Boldo and Melquiond, "When double rounding is odd", 2005; 53 >= 24 + 2).
// The residual of the sum comes from error-free transformations (two-sum, and fma for the product error), so a, c
// and b must be exactly representable as single, as the architecture requires for these instructions (the result is
// undefined otherwise; an inexact product is handled on a best-effort basis).
inline double fmaSingle(double a, double c, double b) {
    const double p = a * c;
    const double s1 = p + b;
    if (!std::isfinite(p) || !std::isfinite(s1)) return fround(std::fma(a, c, b));   // inf, NaN, overflow: no residual to track
    const unsigned csr = hostFpGet();
    if (csr & 0x6000u) hostFpSet(csr & ~0x6000u);      // the error-free transformations need round-to-nearest
    const double pe = std::fma(a, c, -p);              // exact error of the product
    const double bb = s1 - p;
    const double e1 = (p - (s1 - bb)) + (b - bb);      // two-sum: p + b == s1 + e1 exactly
    double s = s1, e = e1;
    if (pe != 0.0) { s = std::fma(a, c, b); e = ((s1 - s) + e1) + pe; }   // product not exact: operands were not singles
    double odd = s;
    if (e != 0.0 && std::isfinite(s) && (doubleToBits(s) & 1u) == 0) odd = std::nextafter(s, e > 0.0 ? INFINITY : -INFINITY);
    if (csr & 0x6000u) hostFpSet(csr);
    return fround(odd);
}

inline void lfs(Ctx& c, uint32_t d, uint32_t ea) {
    const double v = static_cast<double>(bitsToFloat(ld32(c, ea)));
    c.f[d].ps0 = v;
    c.f[d].ps1 = v;
}
inline void lfd(Ctx& c, uint32_t d, uint32_t ea) { c.f[d].ps0 = bitsToDouble(ld64(c, ea)); }
inline void stfs(Ctx& c, uint32_t s, uint32_t ea) { st32(c, ea, floatToBits(static_cast<float>(c.f[s].ps0))); }
inline void stfd(Ctx& c, uint32_t s, uint32_t ea) { st64(c, ea, doubleToBits(c.f[s].ps0)); }
inline void stfiwx(Ctx& c, uint32_t s, uint32_t ea) { st32(c, ea, static_cast<uint32_t>(doubleToBits(c.f[s].ps0))); }

inline void fcmp(Ctx& c, uint32_t crf, double a, double b) {
    uint32_t v;
    if (a != a || b != b) v = 1;          // unordered
    else if (a < b) v = 8;
    else if (a > b) v = 4;
    else v = 2;
    setCr(c, crf, v);
    c.fpscr = (c.fpscr & ~0x0001F000u) | (v << 12);   // FPCC occupies FPSCR bits 12-15 (value layout)
}

inline uint32_t fctiw(double x, bool trunc) {
    if (x != x) return 0x80000000u;
    double r = trunc ? std::trunc(x) : std::nearbyint(x);
    if (r >= 2147483648.0) return 0x7FFFFFFFu;
    if (r < -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(r));
}
// fres / frsqrte: estimate instructions. PLACEHOLDER arithmetic (exact reciprocal rounded to single); the
// hardware estimate is table driven and not bit-exact with this. See the semantics-risk section of the note.
inline double fresPlaceholder(double x) { return fround(1.0 / x); }
inline double frsqrtePlaceholder(double x) { return 1.0 / std::sqrt(x); }

// Paired-single quantised load/store (GQR layout: ST_TYPE bits 0-2, ST_SCALE 8-13, LD_TYPE 16-18, LD_SCALE 24-29).
inline int32_t gqrScale(uint32_t v) { return (static_cast<int32_t>(v & 0x3F) << 26) >> 26; }
inline double dequant(uint32_t raw, uint32_t type, int32_t scale) {
    float v;
    switch (type) {
        case 4: v = static_cast<float>(raw & 0xFF); break;
        case 5: v = static_cast<float>(raw & 0xFFFF); break;
        case 6: v = static_cast<float>(static_cast<int8_t>(raw)); break;
        case 7: v = static_cast<float>(static_cast<int16_t>(raw)); break;
        default: return static_cast<double>(bitsToFloat(raw));
    }
    return static_cast<double>(std::ldexp(v, -scale));
}
inline uint32_t elemSize(uint32_t type) { return (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4; }
inline uint32_t ldElem(Ctx& c, uint32_t ea, uint32_t sz) { return sz == 1 ? ld8(c, ea) : sz == 2 ? ld16(c, ea) : ld32(c, ea); }
inline void psqL(Ctx& c, uint32_t d, uint32_t ea, uint32_t w, uint32_t i) {
    const uint32_t g = c.gqr[i];
    const uint32_t type = (g >> 16) & 7;
    const int32_t scale = gqrScale(g >> 24);
    const uint32_t sz = elemSize(type);
    const double v0 = dequant(ldElem(c, ea, sz), type, scale);
    const double v1 = w ? 1.0 : dequant(ldElem(c, ea + sz, sz), type, scale);
    c.f[d].ps0 = v0;
    c.f[d].ps1 = v1;
}
inline uint32_t quant(double x, uint32_t type, int32_t scale) {
    if (type < 4 || type == 1 || type == 2 || type == 3) return floatToBits(static_cast<float>(x));
    double t = std::ldexp(x, scale);
    double lo, hi;
    switch (type) {
        case 4: lo = 0; hi = 255; break;
        case 5: lo = 0; hi = 65535; break;
        case 6: lo = -128; hi = 127; break;
        default: lo = -32768; hi = 32767; break;
    }
    if (t != t) t = hi;              // NaN: unverified, see risks
    t = std::trunc(t);               // conversion rounds toward zero
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    return static_cast<uint32_t>(static_cast<int32_t>(t));
}
inline void stElem(Ctx& c, uint32_t ea, uint32_t v, uint32_t sz) { if (sz == 1) st8(c, ea, v); else if (sz == 2) st16(c, ea, v); else st32(c, ea, v); }
inline void psqSt(Ctx& c, uint32_t s, uint32_t ea, uint32_t w, uint32_t i) {
    const uint32_t g = c.gqr[i];
    const uint32_t type = g & 7;
    const int32_t scale = gqrScale(g >> 8);
    const uint32_t sz = elemSize(type);
    stElem(c, ea, quant(c.f[s].ps0, type, scale), sz);
    if (!w) stElem(c, ea + sz, quant(c.f[s].ps1, type, scale), sz);
}

// ---- Scalar and paired-single arithmetic -------------------------------------------------------------
// Single-precision scalar forms (fadds, frsp, ...) write the rounded result to both halves; this follows
// common emulator behaviour and is unverified on hardware (risk list in the feasibility note).
inline void setS(Ctx& c, uint32_t d, double v) { c.f[d].ps0 = v; c.f[d].ps1 = v; }
inline void setD(Ctx& c, uint32_t d, double v) { c.f[d].ps0 = v; }
inline void setFctiw(Ctx& c, uint32_t d, uint32_t v) { c.f[d].ps0 = bitsToDouble(0xFFF8000000000000ull | v); }

inline void mffs(Ctx& c, uint32_t d) { c.f[d].ps0 = bitsToDouble(0xFFF8000000000000ull | c.fpscr); }
inline void mtfsf(Ctx& c, uint32_t fm, uint32_t b) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < 8; ++i) if (fm & (0x80u >> i)) m |= 0xFu << (28 - 4 * i);
    m &= ~0x60000000u;   // FEX and VX are derived bits and cannot be written
    setFpscr(c, (c.fpscr & ~m) | (static_cast<uint32_t>(doubleToBits(c.f[b].ps0)) & m));
}
inline void mtfsfi(Ctx& c, uint32_t field, uint32_t imm) {
    const uint32_t sh = (7 - field) * 4;
    uint32_t m = 0xFu << sh;
    m &= ~0x60000000u;
    setFpscr(c, (c.fpscr & ~m) | ((imm << sh) & m));
}
inline void mtfsb(Ctx& c, uint32_t bit, uint32_t v) {
    const uint32_t m = 1u << (31 - bit);
    setFpscr(c, v ? (c.fpscr | m) : (c.fpscr & ~m));
}
inline void mcrfs(Ctx& c, uint32_t crfD, uint32_t crfS) {
    setCr(c, crfD, (c.fpscr >> ((7 - crfS) * 4)) & 0xF);
}

// Paired operations compute both lanes from the old register values before writing.
#define AWL_PS2(name, e0, e1) \
    inline void name(Ctx& c, uint32_t d, uint32_t a, uint32_t b, uint32_t cc) { \
        (void)b; (void)cc; const Fpr A = c.f[a], B = c.f[b], C = c.f[cc]; (void)A; (void)B; (void)C; \
        const double r0 = (e0), r1 = (e1); c.f[d].ps0 = r0; c.f[d].ps1 = r1; }
AWL_PS2(ps_add, fround(A.ps0 + B.ps0), fround(A.ps1 + B.ps1))
AWL_PS2(ps_sub, fround(A.ps0 - B.ps0), fround(A.ps1 - B.ps1))
AWL_PS2(ps_mul, fround(A.ps0 * C.ps0), fround(A.ps1 * C.ps1))
AWL_PS2(ps_div, fround(A.ps0 / B.ps0), fround(A.ps1 / B.ps1))
AWL_PS2(ps_madd, fmaSingle(A.ps0, C.ps0, B.ps0), fmaSingle(A.ps1, C.ps1, B.ps1))
AWL_PS2(ps_msub, fmaSingle(A.ps0, C.ps0, -B.ps0), fmaSingle(A.ps1, C.ps1, -B.ps1))
AWL_PS2(ps_nmadd, -fmaSingle(A.ps0, C.ps0, B.ps0), -fmaSingle(A.ps1, C.ps1, B.ps1))
AWL_PS2(ps_nmsub, -fmaSingle(A.ps0, C.ps0, -B.ps0), -fmaSingle(A.ps1, C.ps1, -B.ps1))
AWL_PS2(ps_muls0, fround(A.ps0 * C.ps0), fround(A.ps1 * C.ps0))
AWL_PS2(ps_muls1, fround(A.ps0 * C.ps1), fround(A.ps1 * C.ps1))
AWL_PS2(ps_madds0, fmaSingle(A.ps0, C.ps0, B.ps0), fmaSingle(A.ps1, C.ps0, B.ps1))
AWL_PS2(ps_madds1, fmaSingle(A.ps0, C.ps1, B.ps0), fmaSingle(A.ps1, C.ps1, B.ps1))
AWL_PS2(ps_sum0, fround(A.ps0 + B.ps1), C.ps1)
AWL_PS2(ps_sum1, C.ps0, fround(A.ps0 + B.ps1))
AWL_PS2(ps_sel, A.ps0 >= 0.0 ? C.ps0 : B.ps0, A.ps1 >= 0.0 ? C.ps1 : B.ps1)
AWL_PS2(ps_merge00, A.ps0, B.ps0)
AWL_PS2(ps_merge01, A.ps0, B.ps1)
AWL_PS2(ps_merge10, A.ps1, B.ps0)
AWL_PS2(ps_merge11, A.ps1, B.ps1)
AWL_PS2(ps_mr, B.ps0, B.ps1)
AWL_PS2(ps_neg, -B.ps0, -B.ps1)
AWL_PS2(ps_abs, std::fabs(B.ps0), std::fabs(B.ps1))
AWL_PS2(ps_nabs, -std::fabs(B.ps0), -std::fabs(B.ps1))
AWL_PS2(ps_res, fresPlaceholder(B.ps0), fresPlaceholder(B.ps1))
AWL_PS2(ps_rsqrte, fround(frsqrtePlaceholder(B.ps0)), fround(frsqrtePlaceholder(B.ps1)))
#undef AWL_PS2

}  // namespace awl::rt
