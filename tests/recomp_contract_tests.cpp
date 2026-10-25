// Tests for the runtime contract in tools/recomp/runtime/hle_abi.h: HLE registration and calls, the MMIO
// window, thread contexts, callGuest and the time base. "Guest" functions are written by hand in the shape
// the generator emits (rt::at / AWL_CALL / rt::ret). Expected values are derived by hand from the contract's
// wording and from the architecture: guest numeric order is big-endian order (0x11223344 stored by stw is
// the value 0x11223344), IEEE-754 single 1.0f is 0x3F800000, double 2.0 is 0x4000000000000000, and so on.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "guest.h"
#include "hle_abi.h"

namespace awl::rt {
namespace {

uint32_t g_depthSeen = 0;
Ctx* g_other = nullptr;

void guestAdd(Ctx& c) {   // 0x80001000: r3 = r3 + r4
    at(c, 0x80001000, 2);
    c.r[3] = c.r[3] + c.r[4];
    g_depthSeen = c.sp;
    ret(c, 0x80001004);
}
void stub3000(Ctx& c) { hleCall(c, 0x80003000); }
void stub3100(Ctx& c) { hleCall(c, 0x80003100); }
void stub3200(Ctx& c) { hleCall(c, 0x80003200); }
void stub3300(Ctx& c) { hleCall(c, 0x80003300); }
void stub3400(Ctx& c) { hleCall(c, 0x80003400); }
void stub3500(Ctx& c) { hleCall(c, 0x80003500); }
void guestCallsHle(Ctx& c) {   // 0x80002000: bl 0x80003000 (HLE)
    at(c, 0x80002000, 1);
    const uint32_t savedLr = c.lr;   // mflr r0 ... mtlr r0 around the call, as a real prologue/epilogue does
    AWL_CALL(c, 0x80002004, 0x80003000, stub3000);
    c.lr = savedLr;
    ret(c, 0x80002008);
}
void guestCallsCallback(Ctx& c) {   // 0x80002100: bl 0x80003400 (HLE handler that calls back into guest code)
    at(c, 0x80002100, 1);
    const uint32_t savedLr = c.lr;   // mflr r0 ... mtlr r0 around the call, as a real prologue/epilogue does
    AWL_CALL(c, 0x80002104, 0x80003400, stub3400);
    c.lr = savedLr;
    ret(c, 0x80002108);
}
void guestCallsSwitch(Ctx& c) {   // 0x80002200: sets r14, then bl 0x80003200 (HLE handler running code on another context)
    at(c, 0x80002200, 2);
    c.r[14] = 111;
    const uint32_t savedLr = c.lr;   // mflr r0 ... mtlr r0 around the call, as a real prologue/epilogue does
    AWL_CALL(c, 0x80002208, 0x80003200, stub3200);
    c.lr = savedLr;
    ret(c, 0x8000220C);
}
void guestScribble(Ctx& c) {   // 0x80006000: leaves distinctive values in registers that threads must not share
    at(c, 0x80006000, 6);
    c.r[14] = c.r[3];
    c.f[14].ps0 = c.f[1].ps0;
    c.cr = c.r[3] ^ 0xF;
    c.ctr = c.r[3] + 1;
    c.gqr[2] = c.r[3];
    c.spr[1008] = c.r[3];
    ret(c, 0x80006018);
}
void guestNested(Ctx& c) {   // 0x80005000: bl 0x80001000, leaving a frame pending while the callee runs
    at(c, 0x80005000, 1);
    const uint32_t savedLr = c.lr;   // mflr r0 ... mtlr r0 around the call, as a real prologue/epilogue does
    AWL_CALL(c, 0x80005004, 0x80001000, guestAdd);
    c.lr = savedLr;
    ret(c, 0x80005008);
}
void guestReadsTb(Ctx& c) {   // 0x80007000: mftb / mfspr of the time base into r5..r8
    at(c, 0x80007000, 4);
    c.r[5] = mftb(c, 268);
    c.r[6] = mftb(c, 269);
    c.r[7] = mfspr(c, 0x80007008, 268);
    c.r[8] = mfspr(c, 0x8000700C, 269);
    ret(c, 0x80007010);
}

void guestLoop(Ctx& c) {   // 0x80008000: r4 += 1, r3 -= 1 until r3 == 0; no calls or memory accesses, as generated
    at(c, 0x80008000, 1);
  L_80008004:;
    c.r[4] += 1;
    c.r[3] -= 1;
    tick(c, 3);   // addi, addic., bne
    if (c.r[3] != 0) { at(c, 0x8000800C, 0); goto L_80008004; }   // the backward-branch checkpoint the generator emits
    at(c, 0x80008010, 1);
    ret(c, 0x80008010);
}
void guestRetF1(Ctx& c) {   // 0x80008100: results in r3, r4 and f1
    at(c, 0x80008100, 4);
    c.r[3] = 77;
    c.r[4] = 88;
    c.f[1].ps0 = 9.5;
    ret(c, 0x8000810C);
}
// Handlers (the platform track's side of the contract).
void hDouble(Ctx& c) { c.r[3] = c.r[3] * 2; }                       // 0x80003000: reads r3, returns in r3
void hFloat(Ctx& c) { c.f[1].ps0 = c.f[1].ps0 + 0.5; }              // 0x80003100: f1 argument and result
void hSwitch(Ctx& c) { (void)c; callGuest(*g_other, 0x80006000); }  // 0x80003200: runs code on the other context
void h64(Ctx& c) {                                                   // 0x80003300: 64-bit argument and result in r3:r4
    const uint64_t v = ((static_cast<uint64_t>(c.r[3]) << 32) | c.r[4]) + 1;
    c.r[3] = static_cast<uint32_t>(v >> 32);
    c.r[4] = static_cast<uint32_t>(v);
}
void hCallback(Ctx& c) { callGuest(c, 0x80001000); }                // 0x80003400: like an alarm/DVD callback dispatch
void hClobberLr(Ctx& c) { c.lr = 0xDEADBEEF; }                      // 0x80003500: a handler that leaves LR changed

}  // namespace

const FnEntry kFunctions[] = {
    {0x80001000, guestAdd},    {0x80002000, guestCallsHle}, {0x80002100, guestCallsCallback}, {0x80002200, guestCallsSwitch},
    {0x80003000, stub3000},    {0x80003100, stub3100},      {0x80003200, stub3200},           {0x80003300, stub3300},
    {0x80003400, stub3400},    {0x80003500, stub3500},      {0x80005000, guestNested},        {0x80006000, guestScribble},
    {0x80007000, guestReadsTb}, {0x80008000, guestLoop},     {0x80008100, guestRetF1},
};
const uint32_t kFunctionCount = sizeof kFunctions / sizeof kFunctions[0];

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

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kMem1Size, 0);
    std::vector<uint8_t> lc = std::vector<uint8_t>(kLockedCacheSize, 0);
    Machine machine;
    std::unique_ptr<Ctx> a;
    Rig() {
        machine.mem = mem.data();
        machine.lc = lc.data();
        a = std::make_unique<Ctx>(machine);
    }
};

template <class F>
bool stops(F&& f, StopKind kind, Stop* out = nullptr) {
    try {
        f();
    } catch (const Stop& s) {
        if (out) *out = s;
        return s.kind == kind;
    }
    return false;
}

// ---- MMIO window ---------------------------------------------------------------------------------------
struct MmioLog {
    uint32_t addr = 0, size = 0;
    uint64_t value = 0;
    int reads = 0, writes = 0;
    uint64_t readResult = 0;
} g_mmio;

uint64_t modelRead(Ctx&, uint32_t addr, uint32_t size) {
    ++g_mmio.reads;
    g_mmio.addr = addr;
    g_mmio.size = size;
    return g_mmio.readResult;
}
void modelWrite(Ctx&, uint32_t addr, uint32_t size, uint64_t value) {
    ++g_mmio.writes;
    g_mmio.addr = addr;
    g_mmio.size = size;
    g_mmio.value = value;
}

void testMmioWithoutModel() {
    Rig r;
    Ctx& c = *r.a;
    // No model installed: every access in the window still stops with StopKind::Mmio.
    CHECK(stops([&] { ld32(c, 0xCC006C00); }, StopKind::Mmio));
    CHECK(stops([&] { st32(c, 0xCC008000, 1); }, StopKind::Mmio));
    CHECK(stops([&] { ld8(c, 0xCC00FFFF); }, StopKind::Mmio));
    // mmioInstall takes both halves or neither.
    CHECK(stops([&] { mmioInstall(modelRead, nullptr); }, StopKind::Contract));
    CHECK(stops([&] { ld32(c, 0xCC006C00); }, StopKind::Mmio));
}

void testMmioRouting() {
    Rig r;
    Ctx& c = *r.a;
    mmioInstall(modelRead, modelWrite);

    // Stores arrive in guest numeric order for every width: stw 0x11223344 is the value 0x11223344
    // (not 0x44332211), sth keeps the low halfword, stb the low byte, and an 8-byte store passes both words.
    st32(c, 0xCC008000, 0x11223344);
    CHECK(g_mmio.writes == 1 && g_mmio.addr == 0xCC008000 && g_mmio.size == 4 && g_mmio.value == 0x11223344);
    st16(c, 0xCC002002, 0xAABB1122);
    CHECK(g_mmio.addr == 0xCC002002 && g_mmio.size == 2 && g_mmio.value == 0x1122);
    st8(c, 0xCC003003, 0xFFFFFF7E);
    CHECK(g_mmio.addr == 0xCC003003 && g_mmio.size == 1 && g_mmio.value == 0x7E);
    st64(c, 0xCC008000, 0x1122334455667788ull);
    CHECK(g_mmio.size == 8 && g_mmio.value == 0x1122334455667788ull);
    // Floating-point stores: stfs of 1.0 writes the IEEE single 0x3F800000; stfd of 2.0 writes 0x4000...0.
    c.f[3].ps0 = 1.0;
    stfs(c, 3, 0xCC008000);
    CHECK(g_mmio.size == 4 && g_mmio.value == 0x3F800000);
    c.f[4].ps0 = 2.0;
    stfd(c, 4, 0xCC008000);
    CHECK(g_mmio.size == 8 && g_mmio.value == 0x4000000000000000ull);

    // Loads return the model's value, masked to the access width; byte-reversed forms swap afterwards.
    g_mmio.readResult = 0xAABBCCDD;
    CHECK(ld32(c, 0xCC006C00) == 0xAABBCCDD && g_mmio.addr == 0xCC006C00 && g_mmio.size == 4);
    CHECK(ld32r(c, 0xCC006C00) == 0xDDCCBBAA);
    g_mmio.readResult = 0x1BEEF;   // a model returning more bits than the width: only the low halfword counts
    CHECK(ld16(c, 0xCC004010) == 0xBEEF && g_mmio.size == 2);
    CHECK(ld16s(c, 0xCC004010) == 0xFFFFBEEF);
    g_mmio.readResult = 0x1FF;
    CHECK(ld8(c, 0xCC004011) == 0xFF && g_mmio.size == 1);
    g_mmio.readResult = 0x4000000000000000ull;
    lfd(c, 5, 0xCC006800);
    CHECK(c.f[5].ps0 == 2.0 && g_mmio.size == 8);
    g_mmio.readResult = 0x3F800000;
    lfs(c, 6, 0xCC006800);
    CHECK(c.f[6].ps0 == 1.0 && c.f[6].ps1 == 1.0);

    // Window edges: the last word of 0xCC00xxxx is inside; 0xCC010000 and a word straddling the end are not.
    const int writes = g_mmio.writes;
    st32(c, 0xCC00FFFC, 7);
    CHECK(g_mmio.writes == writes + 1 && g_mmio.addr == 0xCC00FFFC);
    CHECK(stops([&] { st32(c, 0xCC010000, 7); }, StopKind::Mmio));
    CHECK(stops([&] { ld32(c, 0xCC00FFFE); }, StopKind::Mmio));
    CHECK(stops([&] { ld32(c, 0xCC010000); }, StopKind::Mmio));
    CHECK(g_mmio.writes == writes + 1);
    // dcbz cannot be routed to a model and stops rather than being ignored.
    CHECK(stops([&] { dcbz(c, 0xCC008000); }, StopKind::Mmio));

    // Plain memory is untouched by the model and stays big-endian in host memory.
    const int before = g_mmio.reads + g_mmio.writes;
    st32(c, 0x80000100, 0x11223344);
    CHECK(r.mem[0x100] == 0x11 && r.mem[0x101] == 0x22 && r.mem[0x102] == 0x33 && r.mem[0x103] == 0x44);
    CHECK(ld32(c, 0x80000100) == 0x11223344);
    CHECK(g_mmio.reads + g_mmio.writes == before);
    // Addresses outside every region still fault, and the EFB-style 0xC8000000 is not the hardware window.
    CHECK(stops([&] { ld32(c, 0xC8000000); }, StopKind::Fault));

    mmioInstall(nullptr, nullptr);
    CHECK(stops([&] { ld32(c, 0xCC006C00); }, StopKind::Mmio));
}

// ---- HLE -----------------------------------------------------------------------------------------------
void testHle() {
    Rig r;
    Ctx& c = *r.a;
    hleRegister(0x80003000, "TestDouble", hDouble);
    hleRegister(0x80003100, "TestFloat", hFloat);
    hleRegister(0x80003300, "Test64", h64);
    hleRegister(0x80003500, "TestClobberLr", hClobberLr);

    // A handler reads its argument from r3 and returns in r3, through the guest call path.
    c.r[3] = 21;
    callGuest(c, 0x80002000);
    CHECK(c.r[3] == 42 && c.sp == 0);
    // Float argument and result in f1.
    c.f[1].ps0 = 1.25;
    callIndirect(c, 0x80000010, 0x80003100);
    CHECK(c.f[1].ps0 == 1.75);
    // 64-bit values travel in r3:r4 (high:low). 0x00000000FFFFFFFF + 1 carries into r3.
    c.r[3] = 0;
    c.r[4] = 0xFFFFFFFF;
    callIndirect(c, 0x80000010, 0x80003300);
    CHECK(c.r[3] == 1 && c.r[4] == 0);
    // The replaced function returns with LR as it entered, whatever the handler did to it.
    c.lr = 0x80001234;
    hleCall(c, 0x80003500);
    CHECK(c.lr == 0x80001234);
    // Each hleCall retires one instruction and records the function as the current pc.
    const uint64_t before = r.machine.icount;
    hleCall(c, 0x80003000);
    CHECK(r.machine.icount == before + 1 && c.pc == 0x80003000);

    // A second registration of the same address stops, naming the address and the existing registration.
    Stop s{};
    CHECK(stops([&] { hleRegister(0x80003000, "Other", hDouble); }, StopKind::Contract, &s) && s.addr == 0x80003000 &&
          s.name && std::string(s.name) == "TestDouble");
    CHECK(stops([&] { hleRegister(0x80003600, "NullHandler", nullptr); }, StopKind::Contract, &s) && s.name &&
          std::string(s.name) == "NullHandler");

    // hleCall on an unregistered address stops with the address, and the generated name table supplies the name.
    static const HleName names[] = {{0x80003700, "OSNeverRegistered"}};
    declareHleNames(names, 1);
    CHECK(stops([&] { hleCall(c, 0x80003700); }, StopKind::HleUnregistered, &s) && s.addr == 0x80003700 && s.pc == 0x80003700 &&
          s.name && std::string(s.name) == "OSNeverRegistered");
    CHECK(stops([&] { hleCall(c, 0x80003704); }, StopKind::HleUnregistered, &s) && s.addr == 0x80003704 && s.name == nullptr);
    declareHleNames(nullptr, 0);
}

// ---- callGuest and thread contexts ---------------------------------------------------------------------
void testCallGuest() {
    Rig r;
    Ctx& c = *r.a;
    // Run a recompiled function to completion on a fresh context: the callee's blr validates against the frame
    // callGuest pushed, and the shadow stack is empty again afterwards.
    c.r[3] = 30;
    c.r[4] = 12;
    callGuest(c, 0x80001000);
    CHECK(c.r[3] == 42 && g_depthSeen == 1 && c.sp == 0);
    // A callback from inside a handler, three frames deep: callGuest -> guest -> handler -> callGuest -> callee.
    hleRegister(0x80003400, "TestCallback", hCallback);
    c.r[3] = 5;
    c.r[4] = 6;
    callGuest(c, 0x80002100);
    CHECK(c.r[3] == 11 && g_depthSeen == 3 && c.sp == 0);
    // callGuest restores LR and pc, so the replaced function can return normally afterwards.
    c.lr = 0x80000AAA;
    c.pc = 0x80000BBB;
    c.r[3] = 1;
    c.r[4] = 2;
    callGuest(c, 0x80001000);
    CHECK(c.lr == 0x80000AAA && c.pc == 0x80000BBB && c.r[3] == 3);
    // A target that is not a recompiled function entry stops loudly with its address (also a mid-function address).
    Stop s{};
    CHECK(stops([&] { callGuest(c, 0x80001004); }, StopKind::BadIndirect, &s) && s.addr == 0x80001004);
    CHECK(stops([&] { callGuest(c, 0); }, StopKind::BadIndirect, &s) && s.addr == 0);
}

void testThreadContexts() {
    Rig r;
    Ctx& a = *r.a;
    Ctx* b = createThreadContext(a);
    CHECK(b != nullptr && b != &a && b->m == a.m);
    // No registers are copied from the parent.
    a.r[1] = 0x80100000;
    a.r[14] = 7;
    a.lr = 0x1234;
    Ctx* b2 = createThreadContext(a);
    CHECK(b2->r[1] == 0 && b2->r[14] == 0 && b2->lr == 0 && b2->sp == 0 && b2->cr == 0 && b2->f[14].ps0 == 0.0);
    destroyThreadContext(b2);

    // Each context keeps its own registers: run the scribbler on both with different arguments.
    a.r[3] = 0x100;
    a.f[1].ps0 = 1.5;
    b->r[3] = 0x200;
    b->f[1].ps0 = -2.5;
    callGuest(a, 0x80006000);
    callGuest(*b, 0x80006000);
    CHECK(a.r[14] == 0x100 && b->r[14] == 0x200);
    CHECK(a.f[14].ps0 == 1.5 && b->f[14].ps0 == -2.5);
    CHECK(a.cr == (0x100u ^ 0xF) && b->cr == (0x200u ^ 0xF));
    CHECK(a.ctr == 0x101 && b->ctr == 0x201);
    CHECK(a.gqr[2] == 0x100 && b->gqr[2] == 0x200);
    CHECK(a.spr[1008] == 0x100 && b->spr[1008] == 0x200);
    CHECK(a.r[1] == 0x80100000 && b->r[1] == 0);   // untouched registers stay separate too

    // Independent shadow stacks: leave frames pending on A while B runs a full call, and the reverse.
    pushFrame(a, 0x80000100, 0x80005000);
    pushFrame(a, 0x80005004, 0x80001000);
    const CallFrame f0 = a.cs[0], f1 = a.cs[1];
    CHECK(a.sp == 2 && b->sp == 0);
    b->r[3] = 1;
    b->r[4] = 2;
    callGuest(*b, 0x80005000);   // B's callee blr validates against B's own frames, not A's
    CHECK(b->r[3] == 3 && b->sp == 0 && g_depthSeen == 2);   // callGuest frame + guestNested's call
    CHECK(a.sp == 2 && a.cs[0].site == f0.site && a.cs[0].callee == f0.callee && a.cs[0].ret == f0.ret);
    CHECK(a.cs[1].site == f1.site && a.cs[1].callee == f1.callee && a.cs[1].ret == f1.ret);
    popFrame(a);
    popFrame(a);
    CHECK(a.sp == 0);

    // A handler on one context can run guest code on another (what a fiber switch does) without disturbing it.
    hleRegister(0x80003200, "TestSwitch", hSwitch);
    g_other = b;
    a.r[3] = 0x300;
    b->r[3] = 0x400;
    b->r[14] = 0;
    callGuest(a, 0x80002200);
    CHECK(a.r[14] == 111 && b->r[14] == 0x400 && a.sp == 0 && b->sp == 0);
    g_other = nullptr;

    // The machine counter is shared: both contexts advanced it.
    CHECK(r.machine.icount > 0);
    destroyThreadContext(b);
    CHECK(a.r[14] == 111);   // destroying one context leaves the other intact
}

// ---- Host data and context creation ----------------------------------------------------------------------
void testHostDataAndFreshContext() {
    Rig r;
    Ctx& a = *r.a;
    // A context made from a fully dirty parent starts with every register zero, an empty shadow stack and no host data.
    for (int i = 0; i < 32; ++i) { a.r[i] = 0xA0A0A0A0 + i; a.f[i].ps0 = i + 0.5; a.f[i].ps1 = -i - 0.5; }
    a.cr = a.lr = a.ctr = a.fpscr = a.msr = a.pc = 0x55555555;
    a.so = a.ov = a.ca = 1;
    a.xerbc = 0x7F;
    for (int i = 0; i < 8; ++i) a.gqr[i] = 0x1111 * (i + 1);
    a.spr[1008] = 0xFFFF0000;
    a.sr[3] = 7;
    int parentData = 0;
    setThreadHostData(a, &parentData);
    pushFrame(a, 0x80000100, 0x80001000);
    Ctx* b = createThreadContext(a);
    bool zero = b->m == a.m && b->sp == 0 && b->cr == 0 && b->lr == 0 && b->ctr == 0 && b->fpscr == 0 && b->msr == 0 && b->pc == 0 &&
                b->so == 0 && b->ov == 0 && b->ca == 0 && b->xerbc == 0 && b->spr[1008] == 0 && b->sr[3] == 0;
    for (int i = 0; i < 32; ++i) zero = zero && b->r[i] == 0 && b->f[i].ps0 == 0.0 && b->f[i].ps1 == 0.0;
    for (int i = 0; i < 8; ++i) zero = zero && b->gqr[i] == 0;
    CHECK(zero);
    CHECK(threadHostData(*b) == nullptr && threadHostData(a) == &parentData);
    // Host data is per context.
    int childData = 0;
    setThreadHostData(*b, &childData);
    CHECK(threadHostData(*b) == &childData && threadHostData(a) == &parentData);
    setThreadHostData(*b, nullptr);
    CHECK(threadHostData(*b) == nullptr);
    destroyThreadContext(b);

    // callGuest leaves the callee's r3/r4/f1 results in the context and restores pc and LR.
    popFrame(a);
    a.lr = 0x80000AAA;
    a.pc = 0x80000BBB;
    callGuest(a, 0x80008100);
    CHECK(a.r[3] == 77 && a.r[4] == 88 && a.f[1].ps0 == 9.5 && a.lr == 0x80000AAA && a.pc == 0x80000BBB && a.sp == 0);
}

// ---- Preemption ----------------------------------------------------------------------------------------
struct PreLog {
    std::vector<uint64_t> counts, deadlines;   // retired count at each hook entry, and the deadline it satisfied
    uint64_t next = 0;
    Ctx* a = nullptr;
    Ctx* b = nullptr;
    int hooksA = 0, hooksB = 0;
    uint32_t bRuns = 0;
    bool nestedOk = false;
} g_pre;

void resetPreemption() {
    preemptInstall(nullptr);
    preemptDeadlineSet(~0ull);
    g_pre = PreLog{};
}

void hookStride(Ctx& c) {   // a periodic timer: next deadline 100 instructions after this one
    g_pre.counts.push_back(retiredInstructions(c));
    g_pre.deadlines.push_back(g_pre.next);
    g_pre.next = retiredInstructions(c) + 100;
    preemptDeadlineSet(g_pre.next);
}
void hookLazy(Ctx& c) { (void)c; }   // returns without moving the deadline
void hookRunsGuestOnSameCtx(Ctx& c) {   // delivers an "interrupt": runs guest code on the interrupted context
    ++g_pre.hooksA;
    const uint32_t r3 = c.r[3], r4 = c.r[4];   // a real delivery saves and restores the interrupted registers
    c.r[3] = 10;
    c.r[4] = 0;
    callGuest(c, 0x80008000);   // reaches checkpoints with the deadline still due: must not re-enter the hook
    g_pre.nestedOk = c.r[4] == 10;
    c.r[3] = r3;
    c.r[4] = r4;
    preemptDeadlineSet(~0ull);
}
// A hook that switches to the other context (here by running its code to completion). With g_lateDeadline
// false it sets a far deadline first; with true it leaves the stale one, so the other context's first
// checkpoint enters the hook itself.
bool g_lateDeadline = false;
void hookSwitch(Ctx& c) {
    if (&c != g_pre.a) {
        ++g_pre.hooksB;
        preemptDeadlineSet(~0ull);
        return;
    }
    ++g_pre.hooksA;
    const uint32_t r3 = c.r[3], r4 = c.r[4];
    if (!g_lateDeadline) preemptDeadlineSet(~0ull);
    g_pre.b->r[3] = 20;
    g_pre.b->r[4] = 0;
    callGuest(*g_pre.b, 0x80008000);
    g_pre.bRuns += g_pre.b->r[4];
    c.r[3] = r3;
    c.r[4] = r4;
    preemptDeadlineSet(retiredInstructions(c) + 100);
}

void testPreemption() {
    resetPreemption();
    // Without a hook no deadline applies.
    {
        preemptDeadlineSet(5);
        Rig r;
        r.a->r[3] = 1000;
        callGuest(*r.a, 0x80008000);
        CHECK(r.a->r[4] == 1000 && retiredInstructions(*r.a) == r.machine.icount && r.machine.icount == 3002);
    }
    resetPreemption();

    // A register-only loop (no call, return or memory access inside) is preempted at its backward branch. The
    // deadline and hook are set before the machine exists. The loop retires 1 + 3 per iteration + 1 instructions;
    // each checkpoint follows a 3-instruction iteration, so a hook sees a count in [deadline, deadline + 2].
    {
        g_pre.next = 100;
        preemptInstall(hookStride);
        preemptDeadlineSet(100);
        Rig r;
        r.a->r[3] = 1000;
        callGuest(*r.a, 0x80008000);
        CHECK(r.a->r[4] == 1000 && r.machine.icount == 3002);
        bool spacing = !g_pre.counts.empty();
        for (size_t i = 0; i < g_pre.counts.size(); ++i)
            spacing = spacing && g_pre.counts[i] >= g_pre.deadlines[i] && g_pre.counts[i] <= g_pre.deadlines[i] + 2;
        CHECK(spacing);
        CHECK(g_pre.counts.size() >= 29 && g_pre.counts.size() <= 30);   // strides of 100..102 over 3002 instructions
    }
    resetPreemption();

    // A hook that does not move the deadline stops loudly.
    {
        preemptInstall(hookLazy);
        preemptDeadlineSet(50);
        Rig r;
        r.a->r[3] = 1000;
        Stop s{};
        CHECK(stops([&] { callGuest(*r.a, 0x80008000); }, StopKind::Contract, &s) && s.what && std::string(s.what).find("deadline") != std::string::npos);
    }
    resetPreemption();

    // The instruction limit is separate: the deadline hook runs below it, and the limit still stops the run.
    {
        g_pre.next = 100;
        preemptInstall(hookStride);
        preemptDeadlineSet(100);
        Rig r;
        r.machine.setLimit(500);
        r.a->r[3] = 1000;
        CHECK(stops([&] { callGuest(*r.a, 0x80008000); }, StopKind::InstructionLimit));
        CHECK(g_pre.counts.size() >= 4 && g_pre.counts.size() <= 5 && r.machine.icount > 500 && r.machine.icount <= 503);
    }
    resetPreemption();
    {   // a far deadline never delays the limit
        preemptInstall(hookStride);
        preemptDeadlineSet(100000);
        Rig r;
        r.machine.setLimit(500);
        r.a->r[3] = 1000;
        CHECK(stops([&] { callGuest(*r.a, 0x80008000); }, StopKind::InstructionLimit) && g_pre.counts.empty());
    }
    resetPreemption();

    // Guest code run by the hook on the interrupted context does not re-enter the hook.
    {
        preemptInstall(hookRunsGuestOnSameCtx);
        preemptDeadlineSet(100);
        Rig r;
        r.a->r[3] = 1000;
        callGuest(*r.a, 0x80008000);
        CHECK(g_pre.hooksA == 1 && g_pre.nestedOk && r.a->r[4] == 1000 && r.a->r[3] == 0 && !r.a->inPreempt);
    }
    resetPreemption();

    // A hook that switches to another context and back; A's registers survive the switch.
    for (const bool late : {false, true}) {
        g_lateDeadline = late;
        Rig r;
        Ctx* b = createThreadContext(*r.a);
        g_pre.a = r.a.get();
        g_pre.b = b;
        preemptInstall(hookSwitch);
        preemptDeadlineSet(100);
        r.a->r[3] = 1000;
        callGuest(*r.a, 0x80008000);
        CHECK(r.a->r[4] == 1000 && r.a->r[3] == 0);
        CHECK(g_pre.hooksA >= 10);                    // each switch costs about 62 instructions of B plus A's 100
        CHECK(g_pre.bRuns == 20u * g_pre.hooksA);     // each switch ran B's loop in full
        // Stale deadline left in place (late): B's first checkpoint enters the hook on B once per switch;
        // far deadline set first: B is never preempted.
        CHECK(g_pre.hooksB == (late ? g_pre.hooksA : 0));
        destroyThreadContext(b);
        resetPreemption();
    }
    g_lateDeadline = false;
}

// ---- Time base -----------------------------------------------------------------------------------------
uint64_t fixedSource(Ctx&) { return 0x0000000500000007ull; }
uint64_t perCtxSource(Ctx& c) { return c.r[31]; }

void testTimeBase() {
    Rig r;
    Ctx& a = *r.a;
    // Default: retired instructions / 4 (4 instructions per tick). guestReadsTb retires 4 + hle bookkeeping:
    // the count when mftb executes is 4 (counted through the first trap point), so TBL = 1.
    r.machine.icount = 4000;
    CHECK(mftb(a, 268) == 1000 && mftb(a, 269) == 0);
    // Installed source: mftb and the TBL/TBU SPRs both read it (TBL = low word, TBU = high word).
    timeBaseInstall(fixedSource);
    callGuest(a, 0x80007000);
    CHECK(a.r[5] == 7 && a.r[6] == 5 && a.r[7] == 7 && a.r[8] == 5);
    // The source receives the context that executed the instruction.
    timeBaseInstall(perCtxSource);
    Ctx* b = createThreadContext(a);
    a.r[31] = 100;
    b->r[31] = 200;
    CHECK(mftb(a, 268) == 100 && mftb(*b, 268) == 200 && mfspr(*b, 0, 268) == 200 && mfspr(a, 0, 269) == 0);
    destroyThreadContext(b);
    timeBaseInstall(nullptr);
    r.machine.icount = 8;
    CHECK(mftb(a, 268) == 2);   // back to the deterministic default
}

}  // namespace

int main() try {
    testMmioWithoutModel();   // must run before any model is installed
    testMmioRouting();
    testHle();
    testCallGuest();
    testThreadContexts();
    testHostDataAndFreshContext();
    testPreemption();
    testTimeBase();
    std::printf("recomp contract tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
catch (const Stop& s) {
    std::fprintf(stderr, "uncaught Stop: kind %u pc 0x%08X addr 0x%08X: %s\n", static_cast<unsigned>(s.kind), s.pc, s.addr, s.what);
    return 2;
}
