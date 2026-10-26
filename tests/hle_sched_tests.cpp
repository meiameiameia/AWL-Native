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
#include "pad_source.h"
#include "guest.h"
#include "platform.h"

constexpr uint32_t kCtxMain = 0x80100000, kCtxThread = 0x80101000, kMarkBase = 0x80150000;
constexpr uint32_t kThreadMem = 0x80120000;   // an OSThread: context at +0, state halfword at +0x2C8
constexpr uint32_t kMarkA = kMarkBase + 0, kMarkB = kMarkBase + 4, kMarkC = kMarkBase + 8, kMarkD = kMarkBase + 12, kMarkE = kMarkBase + 16,
                   kMarkF = kMarkBase + 20, kIrqCount = kMarkBase + 24, kIrqR3 = kMarkBase + 28, kIrqR4 = kMarkBase + 32,
                   kIrqMsr = kMarkBase + 36, kMarkMsr = kMarkBase + 40, kHandlerR1 = kMarkBase + 52, kHandlerChain = kMarkBase + 56, kSeq = kMarkBase + 64, kSeqLen = kMarkBase + 60;
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
void stubPadRead(Ctx& c) { hleCall(c, 0x801C0824); }
void stubPadInit(Ctx& c) { hleCall(c, 0x801C06D4); }
void stubIdlePost(Ctx& c) { hleCall(c, 0x80003D00); }

void fMain(Ctx& c) {   // 0x80002000
    at(c, 0x80002000, 2);
    const uint32_t savedLr = c.lr;
    c.msr &= ~awl::hle::kMsrFP;                                        // the OS cleared floating-point-available (OSSetCurrentContext does)
    c.r[3] = kCtxMain;
    AWL_CALL(c, 0x80002004, 0x801B2D8C, stubSave);
    if (c.r[3] == 0) {
        mark(c, kMarkA, 1);
        c.r[3] = kCtxThread;
        AWL_CALL(c, 0x80002010, 0x801B2E0C, stubLoad);       // starts the thread; returns when the thread loads this context
        mark(c, kMarkC, 3);
        mark(c, kMarkMsr, c.msr);
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
    mark(c, kHandlerR1, c.r[1]);
    mark(c, kHandlerChain, ld32(c, c.r[1]));
    st32(c, c.r[1] + 4, 0xBADBAD00);                         // what a callee's prologue does: its LR goes to 4(r1)
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
void fPadMain(Ctx& c) {   // 0x80002700: PADInit(), then PADRead(status) as the HSD pad reader does
    at(c, 0x80002700, 2);
    const uint32_t savedLr = c.lr;
    AWL_CALL(c, 0x80002704, 0x801C06D4, stubPadInit);
    mark(c, kMarkBase + 44, c.r[3]);
    c.r[3] = kMarkBase + 128;
    AWL_CALL(c, 0x80002710, 0x801C0824, stubPadRead);
    mark(c, kMarkBase + 48, c.r[3]);
    c.lr = savedLr;
    ret(c, 0x80002720);
}
void fExitMain(Ctx& c) {   // 0x80002800: starts a thread that exits, then reuses its OSThread memory for a new thread
    at(c, 0x80002800, 2);
    const uint32_t savedLr = c.lr;
    c.r[3] = kCtxMain;
    AWL_CALL(c, 0x80002804, 0x801B2D8C, stubSave);
    if (c.r[3] == 0) {
        c.r[3] = kThreadMem;
        AWL_CALL(c, 0x80002810, 0x801B2E0C, stubLoad);        // thread 1 runs, "exits" and switches back here
        mark(c, kMarkBase + 60 + 4, 1);                      // main resumed after the first thread exited
        // OSCreateThread / OSInitContext again in the same memory: new entry, parameter and state.
        st32(c, kThreadMem + 0x198, 0x80002A00);
        st32(c, kThreadMem + 0xC, 88);
        st32(c, kThreadMem + 4, 0x80181000);
        st32(c, kThreadMem + 0x19C, 0x9032);
        st16(c, kThreadMem + 0x2C8, 1);
        c.r[3] = kThreadMem;
        AWL_CALL(c, 0x80002820, 0x801B2E0C, stubLoad);        // must start a fresh thread, not resume the dead one
        mark(c, kMarkBase + 60 + 8, 1);
    }
    c.lr = savedLr;
    ret(c, 0x80002830);
}
void fExitThread1(Ctx& c) {   // 0x80002900
    at(c, 0x80002900, 2);
    const uint32_t savedLr = c.lr;
    mark(c, kMarkBase + 52, c.r[3]);
    st32(c, 0x800000E4, kThreadMem);                         // the scheduler's current thread is this one ...
    st16(c, kThreadMem + 0x2C8, 0);                          // ... and OSExitThread has marked it exited (a detached thread; 8 for a joinable one)
    c.r[3] = kThreadMem;
    AWL_CALL(c, 0x80002910, 0x801B2D8C, stubSave);           // SelectThread still saves the dying context
    c.r[3] = kCtxMain;
    AWL_CALL(c, 0x80002920, 0x801B2E0C, stubLoad);           // switches away for good
    mark(c, kMarkBase + 60 + 12, 1);                         // must never run
    c.lr = savedLr;
    ret(c, 0x80002930);
}
void fExitThread2(Ctx& c) {   // 0x80002A00
    at(c, 0x80002A00, 2);
    const uint32_t savedLr = c.lr;
    mark(c, kMarkBase + 56, c.r[3]);
    c.r[3] = kCtxMain;
    AWL_CALL(c, 0x80002A10, 0x801B2E0C, stubLoad);
    c.lr = savedLr;
    ret(c, 0x80002A20);
}
// The shape of SelectThread's idle spin (0x801B6FC8-0x801B6FD0 in the generated code): lwz of the run-queue bit set at r13 - 0x5348,
// cmplwi, beq back; checkpoints after the first instruction and at the branch. It leaves when the word becomes non-zero.
uint64_t g_idleSpins = 0, g_idleExitIcount = 0;
void fIdleMain(Ctx& c) {   // 0x80002B00
    at(c, 0x80002B00, 2);
    const uint32_t savedLr = c.lr;
    AWL_CALL(c, 0x80002B04, 0x80003D00, stubIdlePost);
    for (;;) {
        at(c, 0x801B6FC8, 1);
        c.r[0] = ld32(c, c.r[13] - 21320);
        tick(c, 2);
        ++g_idleSpins;
        if (c.r[0] != 0) break;
        at(c, 0x801B6FD0, 0);
    }
    g_idleExitIcount = c.m->icount;
    c.lr = savedLr;
    ret(c, 0x80002B20);
}
void fCallback(Ctx& c) {
    at(c, 0x80002500, 1);
    seq(c, 100 + c.r[3] * 10 + c.r[4]);
    mark(c, kHandlerR1, c.r[1]);
    mark(c, kHandlerChain, ld32(c, c.r[1]));
    st32(c, c.r[1] + 4, 0xBADBAD00);
    ret(c, 0x80002504);
}
void fDisableSched(Ctx& c) { at(c, 0x801B6B98, 1); seq(c, 1); ret(c, 0x801B6B9C); }
void fEnableSched(Ctx& c) { at(c, 0x801B6BD8, 1); seq(c, 2); ret(c, 0x801B6BDC); }
void fReschedule(Ctx& c) { at(c, 0x801B70A4, 1); seq(c, 3); ret(c, 0x801B70A8); }

}  // namespace

// Sorted by address: the runtime looks entries up by binary search.
const FnEntry kFunctions[] = {
    {0x80002000, fMain},         {0x80002100, fThread},      {0x80002200, fIrq},          {0x80002300, fIntrMain},
    {0x80002400, fCbMain},       {0x80002500, fCallback},    {0x80002600, fDecMain},      {0x80002700, fPadMain},
    {0x80002800, fExitMain},     {0x80002900, fExitThread1}, {0x80002A00, fExitThread2}, {0x80002B00, fIdleMain},
    {0x80003D00, stubIdlePost},
    {0x80003900, stubTrigger},   {0x80003A00, stubClear},    {0x80003B00, stubCallbackTrigger}, {0x80003C00, stubDecTrigger},
    {0x801B2D8C, stubSave},      {0x801B2E0C, stubLoad},     {0x801B6B98, fDisableSched}, {0x801B6BD8, fEnableSched},
    {0x801B70A4, fReschedule},   {0x801C06D4, stubPadInit},  {0x801C0824, stubPadRead},
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

// Idle skip: the platform fast-forwards the idle spin to the next event. A timer event 40,000 ticks away wakes the loop by setting the
// run-queue word. Skipping must be invisible to the guest: same wake-up tick, same retired-instruction count when the loop exits.
uint64_t g_wakeTick = 0;
void hIdlePost(Ctx& c) {
    Platform& p = Platform::get();
    p.onEvent(Ev::Custom, [](Ctx& hc, const Event&) {
        Platform& q = Platform::get();
        g_wakeTick = q.now();
        rt::st32(hc, hc.r[13] - 21320, 1);
    });
    p.post(p.now() + 40000, Ev::Custom, 0);
    g_wakeTick = 0;
    (void)c;
}

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(rt::kMem1Size, 0);
    std::vector<uint8_t> lc = std::vector<uint8_t>(rt::kLockedCacheSize, 0);
    rt::Machine machine;
    std::unique_ptr<Ctx> boot;
    explicit Rig(Config cfg = Config()) {
        machine.mem = mem.data();
        machine.lc = lc.data();
        Platform::get().attach(machine, cfg);
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
    CHECK(m.r32(kMarkF) == (0x9032u | kMsrFP));   // ... with the MSR from SRR1 plus floating-point available (always-available-FPU policy)
    CHECK((m.r32(kMarkMsr) & kMsrFP) != 0);  // the resumed main context has floating point available although it saved a clear bit
    CHECK(m.r32(kMarkC) == 3);               // the main context resumed after its own OSLoadContext call
    CHECK(m.r32(kMarkD) == 0);               // the thread never continued past its OSLoadContext(main)
    // The image OSSaveContext wrote: r1, r2, r13, r14 (via stmw r13), CR, LR and SRR0 = return address, SRR1 = MSR, r3 slot = 1.
    CHECK(m.r32(kCtxMain + 4) == 0x80190000 && m.r32(kCtxMain + 8) == 0x80200000);
    CHECK(m.r32(kCtxMain + 0x34) == 0x80300000 && m.r32(kCtxMain + 0x38) == 0x14141414);
    CHECK(m.r32(kCtxMain + 0x80) == 0x24000000);
    CHECK(m.r32(kCtxMain + 0x84) == 0x80002008 && m.r32(kCtxMain + 0x198) == 0x80002008);   // call site 0x80002004 + 4
    CHECK(m.r32(kCtxMain + 0x19C) == 0x00000032);   // the MSR at the save: the test cleared floating-point-available first
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
    r.boot->msr = 0x00000032;                  // floating point not available in the interrupted context
    m.w32(kIrqCount, 0);
    m.w32(0x80190004, 0xCAFEF00D);             // the interrupted function's LR slot: a prologue stored it and has not run its stwu yet
    const bool ok = Platform::get().run(*r.boot, 0x80002300);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kIrqCount) == 1);              // delivered once: the handler cleared the line
    CHECK(m.r32(kIrqR3) == 4 && m.r32(kIrqR4) == kCtxMain);
    CHECK((m.r32(kIrqMsr) & kMsrEE) == 0);     // handlers run with external interrupts disabled
    CHECK((m.r32(kIrqMsr) & kMsrFP) != 0);     // ... and with floating point available
    CHECK((m.r16(kCtxMain + 0x1A2) & 2) == 2); // the context is flagged as holding exception state
    CHECK(m.r32(kCtxMain + 0x19C) == (0x00000032u | kMsrEE));   // SRR1 holds the interrupted MSR (the trigger had enabled EE)
    CHECK(m.r32(kCtxMain + 4) == 0x80190000 && m.r32(kCtxMain + 0x14) == 0x55555555 && m.r32(kCtxMain + 0x80) == 0x11000000);
    CHECK((r.boot->msr & kMsrEE) != 0);        // the interrupted context keeps its own MSR
    // The vector tail (0x801B1CC8-0x801B1CD0: stwu r1,-8(r1)) gives the handler its own 8-byte frame below the interrupted r1,
    // so the handler's prologue (stw r0,4(r1)) cannot overwrite the interrupted function's LR slot at 4(r1).
    CHECK(m.r32(kHandlerR1) == 0x80190000 - 8 && m.r32(kHandlerChain) == 0x80190000);
    CHECK(m.r32(0x80190004) == 0xCAFEF00D);
}

void testDecrementer(Rig& r) {
    GuestMem m = r.g();
    m.w32(0x800000D4, kCtxMain);
    r.boot->r[13] = 0x80300000;
    m.w32(r.boot->r[13] - 21468, 0x80003000);
    m.w32(0x80003000 + 4 * 8, 0x80002200);     // decrementer handler (exception 8)
    m.w32(kIrqCount, 0);
    r.boot->msr = 0x00002032;
    r.boot->r[1] = 0x80190000;
    const bool ok = Platform::get().run(*r.boot, 0x80002600);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kMarkBase + 40) == 0);         // not yet after 10 ticks
    CHECK(m.r32(kIrqCount) == 1 && m.r32(kIrqR3) == 8);   // delivered after 11 ticks with exception number 8
}

void testThreadExitAndReuse(Rig& r) {
    GuestMem m = r.g();
    m.w32(kThreadMem + 4, 0x80180000);
    m.w32(kThreadMem + 0xC, 77);
    m.w32(kThreadMem + 0x198, 0x80002900);
    m.w32(kThreadMem + 0x19C, 0x9032);
    r.boot->r[1] = 0x80190000;
    const bool ok = Platform::get().run(*r.boot, 0x80002800);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kMarkBase + 52) == 77);          // the first thread ran with its parameter
    CHECK(m.r32(kMarkBase + 64) == 1);           // main resumed after it exited
    CHECK(m.r32(kMarkBase + 56) == 88);          // the new thread in the same memory started fresh with its own parameter
    CHECK(m.r32(kMarkBase + 68) == 1);           // main resumed again afterwards
    CHECK(m.r32(kMarkBase + 72) == 0);           // the exited thread never continued
    CHECK(Platform::get().reaped >= 1);          // its fiber was freed
}

void testPad(Rig& r) {
    GuestMem m = r.g();
    for (uint32_t i = 0; i < 48; ++i) m.w8(kMarkBase + 128 + i, 0xEE);      // garbage the handler must overwrite
    const bool ok = Platform::get().run(*r.boot, 0x80002700);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kMarkBase + 44) == 1);                                       // PADInit reports success
    CHECK(m.r32(kMarkBase + 48) == 0);                                       // PADRead returns an empty reset mask
    // PADStatus is 12 bytes per channel: button u16, stick x/y, substick x/y, triggers l/r, analog a/b, err s8, pad.
    for (uint32_t i = 0; i < 12; ++i) CHECK(m.r8(kMarkBase + 128 + i) == 0);   // channel 0 connected, neutral, err 0
    for (uint32_t ch = 1; ch < 4; ++ch) {
        CHECK(m.r8(kMarkBase + 128 + 12 * ch + 10) == 0xFF);                   // PAD_ERR_NO_CONTROLLER (-1)
        CHECK(m.r8(kMarkBase + 128 + 12 * ch + 0) == 0 && m.r8(kMarkBase + 128 + 12 * ch + 11) == 0);
    }
}

PadReading testProvider(void* user) {
    ++*static_cast<int*>(user);
    PadReading p;
    p.connected = true;
    p.buttons = 0x1100;                       // A + Start (SDK bits)
    p.stickX = 127; p.stickY = -128; p.substickX = -5; p.substickY = 6;
    p.triggerL = 200; p.triggerR = 9;
    return p;
}
PadReading testAbsent(void*) { return PadReading(); }   // connected = false: the window has no focus

// Each platform run needs a fresh rig.
void padRun(Rig& r, uint32_t e) {
    GuestMem m = r.g();
    for (uint32_t i = 0; i < 48; ++i) m.w8(e + i, 0xEE);
    CHECK(Platform::get().run(*r.boot, 0x80002700) && !Platform::get().report().stopped);
}

void testPadProvider(Rig& r) {
    GuestMem m = r.g();
    int calls = 0;
    setPadProvider(testProvider, &calls);
    padRun(r, kMarkBase + 128);
    CHECK(calls == 1);                                       // one PADRead, one poll of the provider
    const uint32_t e = kMarkBase + 128;
    CHECK(m.r16(e) == 0x1100);                                // PADStatus: button u16, stick x/y, substick x/y, triggers l/r, analog a/b, err
    CHECK(m.r8(e + 2) == 0x7F && m.r8(e + 3) == 0x80 && m.r8(e + 4) == 0xFB && m.r8(e + 5) == 6);
    CHECK(m.r8(e + 6) == 200 && m.r8(e + 7) == 9 && m.r8(e + 8) == 0 && m.r8(e + 9) == 0 && m.r8(e + 10) == 0 && m.r8(e + 11) == 0);
    for (uint32_t ch = 1; ch < 4; ++ch) CHECK(m.r8(e + 12 * ch + 10) == 0xFF);
    setPadProvider(nullptr, nullptr);
}

void testPadProviderAbsent(Rig& r) {
    GuestMem m = r.g();
    const uint32_t e = kMarkBase + 128;
    // a provider that reports the controller absent (unfocused window) gives PAD_ERR_NO_CONTROLLER on channel 0 too
    setPadProvider(testAbsent, nullptr);
    padRun(r, e);
    CHECK(m.r8(e + 10) == 0xFF && m.r16(e) == 0);
    setPadProvider(nullptr, nullptr);                         // back to the deterministic neutral default
}

struct IdleRun { uint64_t spins, exitIcount, wakeTick, wakeIcountDue, skips, skipped; };
IdleRun runIdle(bool skip) {
    Config cfg;
    cfg.idleSkip = skip;
    Rig r(cfg);
    GuestMem m = r.g();
    r.boot->r[13] = 0x80300000;
    r.boot->r[1] = 0x80190000;
    r.boot->msr = 0x0000A032;                  // external interrupts enabled, as in the idle loop
    m.w32(r.boot->r[13] - 21320, 0);
    rt::g_idleSpins = 0;
    Platform::get().installPreemption();
    const uint64_t startTick = Platform::get().now();
    const bool ok = Platform::get().run(*r.boot, 0x80002B00);
    CHECK(ok && !Platform::get().report().stopped);
    IdleRun out;
    out.spins = rt::g_idleSpins;
    out.exitIcount = rt::g_idleExitIcount;
    out.wakeTick = g_wakeTick;
    out.wakeIcountDue = Platform::get().retiredAt(startTick + 40000);
    out.skips = Platform::get().idle.skips;
    out.skipped = Platform::get().idle.instructionsSkipped;
    return out;
}

void testIdleSkip() {
    const IdleRun slow = runIdle(false), fast = runIdle(true);
    // The wake-up is due 40,000 ticks = 160,000 instructions after the post (4 instructions per tick).
    CHECK(slow.skips == 0 && slow.skipped == 0);
    CHECK(slow.spins > 40000);                          // 160,000 instructions at 3 per iteration, executed one by one
    CHECK(slow.wakeTick >= 40000 && slow.wakeTick <= 40001 + 4);
    // With the skip the guest sees exactly the same: wake-up tick and the retired-instruction count at the exit.
    CHECK(fast.wakeTick == slow.wakeTick);
    CHECK(fast.exitIcount == slow.exitIcount);
    CHECK(fast.skips > 0 && fast.skipped > 100000);     // nearly all of the wait was fast-forwarded (probes every 2048 instructions)
    CHECK(fast.spins < slow.spins / 10);                // ... and not executed
    CHECK(fast.skipped + fast.spins * 3 >= slow.spins * 3 - 3 * 8);   // what was skipped plus what ran covers the same spin
    // The skip never passes the deadline: the exit is within a few instructions of the due count.
    CHECK(fast.exitIcount >= fast.wakeIcountDue && fast.exitIcount < fast.wakeIcountDue + 64);
}

void testCallbackEnvelope(Rig& r) {
    GuestMem m = r.g();
    m.w32(0x800000D4, kCtxMain);
    m.w32(kSeqLen, 0);
    r.boot->msr = 0x00002032;
    r.boot->r[1] = 0x80190000;
    m.w32(0x80190004, 0xCAFEF00D);
    const bool ok = Platform::get().run(*r.boot, 0x80002400);
    CHECK(ok && !Platform::get().report().stopped);
    CHECK(m.r32(kSeqLen) == 4);
    CHECK(m.r32(kSeq) == 1);                   // OSDisableScheduler
    CHECK(m.r32(kSeq + 4) == 100 + 5 * 10 + 6);  // the callback with r3 = 5, r4 = 6
    CHECK(m.r32(kSeq + 8) == 2);               // OSEnableScheduler
    CHECK(m.r32(kSeq + 12) == 3);              // __OSReschedule
    CHECK(m.r32(kHandlerR1) == 0x80190000 - 8 && m.r32(kHandlerChain) == 0x80190000);   // same frame rule as the interrupt vector
    CHECK(m.r32(0x80190004) == 0xCAFEF00D);
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
        rt::hleRegister(0x80003D00, "TestIdlePost", hIdlePost);
    }
    { Rig r; testThreadSwitch(r); }
    { Rig r; testInterrupt(r); }
    { Rig r; testDecrementer(r); }
    { Rig r; testThreadExitAndReuse(r); }
    { Rig r; testPad(r); }
    { Rig r; testPadProvider(r); }
    { Rig r; testPadProviderAbsent(r); }
    { Rig r; testCallbackEnvelope(r); }
    testIdleSkip();
    std::printf("hle scheduler tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
catch (const rt::Stop& s) {
    std::fprintf(stderr, "uncaught Stop: kind %u pc 0x%08X addr 0x%08X: %s\n", static_cast<unsigned>(s.kind), s.pc, s.addr, s.what);
    return 2;
}
