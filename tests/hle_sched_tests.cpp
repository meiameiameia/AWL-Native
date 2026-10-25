// Tests for guest threads on fibers and interrupt delivery. "Guest" functions are hand-written in the shape the generator
// emits and registered at the real SDK addresses of OSSaveContext and OSLoadContext (the platform's handlers are bound to
// those addresses). Expectations come from the SDK code read in the DOL: OSSaveContext stores r1, r2, r13-r31, CR, LR (also as
// SRR0), MSR (as SRR1), CTR, XER and a 1 in the r3 slot and returns 0; OSLoadContext resumes the saved context; the
// external-interrupt vector flags the context (+0x1A2 bit 1), runs the table handler with r3 = exception number and
// r4 = context; the callback envelope is OSDisableScheduler, callback, OSEnableScheduler, __OSReschedule
// (docs/research/hle-platform.md, sections 2.1 and 5.1-5.2).
#include <cstdio>
#include <memory>
#include <vector>

#include "devices.h"
#include "guest.h"
#include "platform.h"

constexpr uint32_t kCtxMain = 0x80100000, kCtxThread = 0x80101000, kMarkBase = 0x80150000;
constexpr uint32_t kMarkA = kMarkBase + 0, kMarkB = kMarkBase + 4, kMarkC = kMarkBase + 8, kMarkD = kMarkBase + 12, kMarkE = kMarkBase + 16,
                   kMarkF = kMarkBase + 20, kIrqCount = kMarkBase + 24, kIrqR3 = kMarkBase + 28, kIrqR4 = kMarkBase + 32,
                   kIrqMsr = kMarkBase + 36, kSeq = kMarkBase + 64, kSeqLen = kMarkBase + 60;
namespace awl::rt {
namespace {


void mark(Ctx& c, uint32_t addr, uint32_t v) { st32(c, addr, v); }
void seq(Ctx& c, uint32_t id) { const uint32_t n = ld32(c, kSeqLen); st32(c, kSeq + 4 * n, id); st32(c, kSeqLen, n + 1); }

void stubSave(Ctx& c) { hleCall(c, 0x801B2D8C); }
void stubLoad(Ctx& c) { hleCall(c, 0x801B2E0C); }
void stubTrigger(Ctx& c) { hleCall(c, 0x80003900); }
void stubClear(Ctx& c) { hleCall(c, 0x80003A00); }
void stubCallbackTrigger(Ctx& c) { hleCall(c, 0x80003B00); }
void stubDecTrigger(Ctx& c) { hleCall(c, 0x80003C00); }

void fMain(Ctx& c) {   // 0x80002000
    at(c, 0x80002000, 2);
    const uint32_t savedLr = c.lr;
    c.r[3] = kCtxMain;
    AWL_CALL(c, 0x80002004, 0x801B2D8C, stubSave);
    if (c.r[3] == 0) {
        mark(c, kMarkA, 1);
        c.r[3] = kCtxThread;
        AWL_CALL(c, 0x80002010, 0x801B2E0C, stubLoad);       // starts the thread; returns when the thread loads this context
        mark(c, kMarkC, 3);
    }
    c.lr = savedLr;
    ret(c, 0x80002020);
}
void fThread(Ctx& c) {   // 0x80002100: thread entry with the parameter in r3
    at(c, 0x80002100, 2);
    const uint32_t savedLr = c.lr;
    mark(c, kMarkB, c.r[3]);
    mark(c, kMarkE, c.r[1]);
    mark(c, kMarkF, c.msr);
    c.r[3] = kCtxMain;
    AWL_CALL(c, 0x80002110, 0x801B2E0C, stubLoad);           // back to the main context; never returns here
    mark(c, kMarkD, 4);
    c.lr = savedLr;
    ret(c, 0x80002120);
}
void fIrq(Ctx& c) {      // 0x80002200: exception handler from the table
    at(c, 0x80002200, 2);
    const uint32_t savedLr = c.lr;
    mark(c, kIrqCount, ld32(c, kIrqCount) + 1);
    mark(c, kIrqR3, c.r[3]);
    mark(c, kIrqR4, c.r[4]);
    mark(c, kIrqMsr, c.msr);
    AWL_CALL(c, 0x80002210, 0x80003A00, stubClear);          // the device stops asserting its line
    c.lr = savedLr;
    ret(c, 0x80002220);
}
void fIntrMain(Ctx& c) {  // 0x80002300: runs the interrupt trigger
    at(c, 0x80002300, 1);
    const uint32_t savedLr = c.lr;
    AWL_CALL(c, 0x80002304, 0x80003900, stubTrigger);
    c.lr = savedLr;
    ret(c, 0x80002308);
}
void fCbMain(Ctx& c) {    // 0x80002400
    at(c, 0x80002400, 1);
    const uint32_t savedLr = c.lr;
    AWL_CALL(c, 0x80002404, 0x80003B00, stubCallbackTrigger);
    c.lr = savedLr;
    ret(c, 0x80002408);
}
void fDecMain(Ctx& c) {   // 0x80002600
    at(c, 0x80002600, 1);
    const uint32_t savedLr = c.lr;
    AWL_CALL(c, 0x80002604, 0x80003C00, stubDecTrigger);
    c.lr = savedLr;
    ret(c, 0x80002608);
}
void fCallback(Ctx& c) { at(c, 0x80002500, 1); seq(c, 100 + c.r[3] * 10 + c.r[4]); ret(c, 0x80002504); }
void fDisableSched(Ctx& c) { at(c, 0x801B6B98, 1); seq(c, 1); ret(c, 0x801B6B9C); }
void fEnableSched(Ctx& c) { at(c, 0x801B6BD8, 1); seq(c, 2); ret(c, 0x801B6BDC); }
void fReschedule(Ctx& c) { at(c, 0x801B70A4, 1); seq(c, 3); ret(c, 0x801B70A8); }

}  // namespace

const FnEntry kFunctions[] = {
    {0x80002000, fMain},      {0x80002100, fThread},     {0x80002200, fIrq},       {0x80002300, fIntrMain}, {0x80002400, fCbMain},
    {0x80002500, fCallback},  {0x80002600, fDecMain},  {0x80003C00, stubDecTrigger},  {0x80003900, stubTrigger}, {0x80003A00, stubClear},  {0x80003B00, stubCallbackTrigger},
    {0x801B2D8C, stubSave},   {0x801B2E0C, stubLoad},    {0x801B6B98, fDisableSched}, {0x801B6BD8, fEnableSched}, {0x801B70A4, fReschedule},
};
const uint32_t kFunctionCount = sizeof kFunctions / sizeof kFunctions[0];
}  // namespace awl::rt

using namespace awl;
using namespace awl::hle;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// Test-only handlers registered next to the platform's.
void hTrigger(Ctx& c) {
    Platform& p = Platform::get();
    p.piMask() = pibit::Vi;
    p.setLine(pibit::Vi, true);
    c.msr |= kMsrEE;
    p.deliver(c);
}
void hDecTrigger(Ctx& c) {
    Platform& p = Platform::get();
    c.msr |= kMsrEE;
    p.setDecrementer(10);                      // fires when the counter goes from 0 to -1: 10 + 1 ticks from now
    c.m->icount += 10 * kInstructionsPerTick;
    p.processEvents(c);
    p.deliver(c);
    GuestMem m(c.m->mem);
    m.w32(kMarkBase + 40, m.r32(kIrqCount));   // deliveries so far, after 10 ticks
    c.m->icount += 1 * kInstructionsPerTick;
    p.processEvents(c);
    p.deliver(c);
}
void hClear(Ctx&) { Platform::get().setLine(pibit::Vi, false); }
void hCallbackTrigger(Ctx& c) {
    Platform& p = Platform::get();
    c.msr |= kMsrEE;
    p.queueCallback(0x80002500, 5, 6);
    p.deliver(c);
}

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(rt::kMem1Size, 0);
    std::vector<uint8_t> lc = std::vector<uint8_t>(rt::kLockedCacheSize, 0);
    rt::Machine machine;
    std::unique_ptr<Ctx> boot;
    Rig() {
        machine.mem = mem.data();
        machine.lc = lc.data();
        Platform::get().attach(machine, Config());
        boot = std::make_unique<Ctx>(machine);
        boot->msr = 0x00002032;
        rt::setFpscr(*boot, 0);
    }
    GuestMem g() { return GuestMem(mem.data()); }
};

void testThreadSwitch(Rig& r) {
    GuestMem m = r.g();
    // OSContext image of a thread that has never run, as OSInitContext and OSCreateThread leave it: SRR0 = entry,
    // SRR1 = 0x9032 (EE, ME, IR, DR, RI), r1 = stack, r3 = parameter, LR = exit function.
    m.w32(kCtxThread + 4, 0x80180000);
    m.w32(kCtxThread + 0xC, 77);
    m.w32(kCtxThread + 0x84, 0x80002900);
    m.w32(kCtxThread + 0x198, 0x80002100);
    m.w32(kCtxThread + 0x19C, 0x9032);
    r.boot->r[1] = 0x80190000;
    r.boot->r[2] = 0x80200000;
    r.boot->r[13] = 0x80300000;
    r.boot->r[14] = 0x14141414;
    r.boot->cr = 0x24000000;
    const bool ok = Platform::get().run(*r.boot, 0x80002000);
    CHECK(ok);
    CHECK(!Platform::get().report().stopped);
    CHECK(m.r32(kMarkA) == 1);               // OSSaveContext returned 0 the first time
    CHECK(m.r32(kMarkB) == 77);              // the thread started with its parameter in r3
    CHECK(m.r32(kMarkE) == 0x80180000);      // ... on the stack from the image
    CHECK(m.r32(kMarkF) == 0x9032);          // ... with the MSR from SRR1
    CHECK(m.r32(kMarkC) == 3);               // the main context resumed after its own OSLoadContext call
    CHECK(m.r32(kMarkD) == 0);               // the thread never continued past its OSLoadContext(main)
    // The image OSSaveContext wrote: r1, r2, r13, r14 (via stmw r13), CR, LR and SRR0 = return address, SRR1 = MSR, r3 slot = 1.
    CHECK(m.r32(kCtxMain + 4) == 0x80190000 && m.r32(kCtxMain + 8) == 0x80200000);
    CHECK(m.r32(kCtxMain + 0x34) == 0x80300000 && m.r32(kCtxMain + 0x38) == 0x14141414);
    CHECK(m.r32(kCtxMain + 0x80) == 0x24000000);
    CHECK(m.r32(kCtxMain + 0x84) == 0x80002008 && m.r32(kCtxMain + 0x198) == 0x80002008);   // call site 0x80002004 + 4
    CHECK(m.r32(kCtxMain + 0x19C) == 0x00002032);
    CHECK(m.r32(kCtxMain + 0xC) == 1);
}

void testInterrupt(Rig& r) {
    GuestMem m = r.g();
    // The vector stores the interrupted state in the context named by the OS's current-context word.
    m.w32(0x800000D4, kCtxMain);
    const uint32_t table = 0x80003000;
    r.boot->r[13] = 0x80300000;
    m.w32(r.boot->r[13] - 21468, table);       // the exception-table pointer __OSSetExceptionHandler reads
    m.w32(table + 4 * 4, 0x80002200);          // external interrupt handler
    r.boot->r[1] = 0x80190000;
    r.boot->r[5] = 0x55555555;
    r.boot->cr = 0x11000000;
    r.boot->msr = 0x00002032;
    m.w32(kIrqCount, 0);
    const bool ok = Platform::get().run(*r.boot, 0x80002300);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kIrqCount) == 1);              // delivered once: the handler cleared the line
    CHECK(m.r32(kIrqR3) == 4 && m.r32(kIrqR4) == kCtxMain);
    CHECK((m.r32(kIrqMsr) & kMsrEE) == 0);     // handlers run with external interrupts disabled
    CHECK((m.r16(kCtxMain + 0x1A2) & 2) == 2); // the context is flagged as holding exception state
    CHECK(m.r32(kCtxMain + 0x19C) == (0x00002032u | kMsrEE));   // SRR1 holds the interrupted MSR (the trigger had enabled EE)
    CHECK(m.r32(kCtxMain + 4) == 0x80190000 && m.r32(kCtxMain + 0x14) == 0x55555555 && m.r32(kCtxMain + 0x80) == 0x11000000);
    CHECK((r.boot->msr & kMsrEE) != 0);        // the interrupted context keeps its own MSR
}

void testDecrementer(Rig& r) {
    GuestMem m = r.g();
    m.w32(0x800000D4, kCtxMain);
    r.boot->r[13] = 0x80300000;
    m.w32(r.boot->r[13] - 21468, 0x80003000);
    m.w32(0x80003000 + 4 * 8, 0x80002200);     // decrementer handler (exception 8)
    m.w32(kIrqCount, 0);
    r.boot->msr = 0x00002032;
    const bool ok = Platform::get().run(*r.boot, 0x80002600);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kMarkBase + 40) == 0);         // not yet after 10 ticks
    CHECK(m.r32(kIrqCount) == 1 && m.r32(kIrqR3) == 8);   // delivered after 11 ticks with exception number 8
}

void testCallbackEnvelope(Rig& r) {
    GuestMem m = r.g();
    m.w32(0x800000D4, kCtxMain);
    m.w32(kSeqLen, 0);
    r.boot->msr = 0x00002032;
    const bool ok = Platform::get().run(*r.boot, 0x80002400);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kSeqLen) == 4);
    CHECK(m.r32(kSeq) == 1);                   // OSDisableScheduler
    CHECK(m.r32(kSeq + 4) == 100 + 5 * 10 + 6);  // the callback with r3 = 5, r4 = 6
    CHECK(m.r32(kSeq + 8) == 2);               // OSEnableScheduler
    CHECK(m.r32(kSeq + 12) == 3);              // __OSReschedule
}

}  // namespace

int main() try {
    {
        Rig probe;
        Platform::get().registerHandlers();
        rt::hleRegister(0x80003900, "TestTrigger", hTrigger);
        rt::hleRegister(0x80003A00, "TestClear", hClear);
        rt::hleRegister(0x80003B00, "TestCallbackTrigger", hCallbackTrigger);
        rt::hleRegister(0x80003C00, "TestDecTrigger", hDecTrigger);
    }
    { Rig r; testThreadSwitch(r); }
    { Rig r; testInterrupt(r); }
    { Rig r; testDecrementer(r); }
    { Rig r; testCallbackEnvelope(r); }
    std::printf("hle scheduler tests: %d checks, %d failures\n", g_checks, g_failures);
    std::fflush(nullptr);
    // Threads left suspended inside OSLoadContext keep MSVC debug-iterator proxies (rt::hleCall holds a table iterator
    // across the handler) on abandoned fiber stacks; static destruction would walk them. Leave without running it.
    std::_Exit(g_failures == 0 ? 0 : 1);
}
catch (const rt::Stop& s) {
    std::fprintf(stderr, "uncaught Stop: kind %u pc 0x%08X addr 0x%08X: %s\n", static_cast<unsigned>(s.kind), s.pc, s.addr, s.what);
    return 2;
}
