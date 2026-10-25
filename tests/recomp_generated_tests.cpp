// End-to-end generator test: awl-recomp generates C++ from the hand-assembled DOL written by
// recomp_synth_dol (see that file for the layout), the build compiles it into this executable, and the tests
// run the generated functions against the runtime contract.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "guest.h"
#include "hle_abi.h"

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

struct MmioLog {
    int writes = 0, reads = 0;
    uint32_t addr = 0, size = 0;
    uint64_t value = 0;
} g_mmio;
uint64_t modelRead(Ctx&, uint32_t addr, uint32_t size) {
    ++g_mmio.reads;
    g_mmio.addr = addr;
    g_mmio.size = size;
    return 0x1234;
}
void modelWrite(Ctx&, uint32_t addr, uint32_t size, uint64_t value) {
    ++g_mmio.writes;
    g_mmio.addr = addr;
    g_mmio.size = size;
    g_mmio.value = value;
}

void hSynth(Ctx& c) { c.r[3] = 99; }

std::vector<uint64_t> g_hits;
uint64_t g_next = 0;
void hook(Ctx& c) {
    g_hits.push_back(retiredInstructions(c));
    g_next = retiredInstructions(c) + 100;
    preemptDeadlineSet(g_next);
}

void testMmioOnlyFunctionIsRecompiled() {
    Rig r;
    Ctx& c = *r.a;
    // The function touches only the 0xCC006C00 block with constant addresses; it is recompiled, not a boundary
    // stub. Without a model its first access stops as a hardware-window access, not as a platform boundary.
    c.r[3] = 0xABCD;
    CHECK(stops([&] { callGuest(c, 0x80003200); }, StopKind::Mmio));
    // With a model the store reaches it (guest numeric order, width 4) and the load returns the model's value.
    mmioInstall(modelRead, modelWrite);
    c.r[3] = 0xABCD;
    callGuest(c, 0x80003200);
    CHECK(g_mmio.writes == 1 && g_mmio.reads == 1);
    CHECK(g_mmio.addr == 0xCC006C0C && g_mmio.size == 4);   // last access: the load
    CHECK(c.r[3] == 0x1234);
    mmioInstall(nullptr, nullptr);
    // The boot walk from the entry point also reaches the function (it is in the dispatch table).
    CHECK(lookupFunction(r.machine, 0x80003200) != nullptr);
}

void testWriteReachesModel() {
    Rig r;
    Ctx& c = *r.a;
    // Observe the store separately from the load: a model that records only writes.
    static uint64_t seenValue = 0;
    static uint32_t seenAddr = 0;
    mmioInstall([](Ctx&, uint32_t, uint32_t) -> uint64_t { return 0; },
                [](Ctx&, uint32_t addr, uint32_t size, uint64_t v) { CHECK(size == 4); seenAddr = addr; seenValue = v; });
    c.r[3] = 0xDEADBEEF;
    callGuest(c, 0x80003200);
    CHECK(seenAddr == 0xCC006C08 && seenValue == 0xDEADBEEF);
    mmioInstall(nullptr, nullptr);
}

void testPrivilegedFunctionStaysStub() {
    Rig r;
    Stop s{};
    CHECK(stops([&] { callGuest(*r.a, 0x80003500); }, StopKind::Platform, &s) && s.pc == 0x80003500 && s.what &&
          std::string(s.what).find("rfi") != std::string::npos);
}

void testHleFunction() {
    Rig r;
    Stop s{};
    CHECK(stops([&] { callGuest(*r.a, 0x80003400); }, StopKind::HleUnregistered, &s) && s.addr == 0x80003400 && s.name &&
          std::string(s.name) == "SynthHle");
    hleRegister(0x80003400, "SynthHle", hSynth);
    callGuest(*r.a, 0x80003400);
    CHECK(r.a->r[3] == 99);
}

void testLoopPreemption() {
    // Generated register-only loop: mtctr, then (addi, bdnz) per iteration, then blr = 2 * n + 2 instructions
    // (plus none for the call). A hook with a 100-instruction period sees counts in [deadline, deadline + 1],
    // because each backward-branch checkpoint follows a 2-instruction iteration.
    g_hits.clear();
    g_next = 100;
    preemptInstall(hook);
    preemptDeadlineSet(100);
    Rig r;
    r.a->r[3] = 1000;
    r.a->r[4] = 0;
    callGuest(*r.a, 0x80003300);
    preemptInstall(nullptr);
    preemptDeadlineSet(~0ull);
    CHECK(r.a->r[4] == 1000 && r.machine.icount == 2002);
    bool ok = !g_hits.empty();
    uint64_t deadline = 100;
    for (uint64_t h : g_hits) {
        ok = ok && h >= deadline && h <= deadline + 1;
        deadline = h + 100;
    }
    CHECK(ok);
    CHECK(g_hits.size() >= 19 && g_hits.size() <= 20);   // strides of 100..101 over 2002 instructions
}

// ---- REL modules ------------------------------------------------------------------------------------------
// The synthetic REL (module id 1) is linked the way the SDK would leave it: the test writes the OSModuleInfo
// block and section table that a successful OSLink leaves in guest memory (section addresses chosen freely), plus
// the data words OSLink would relocate. 0x80003660 / 0x80003740 are the synthetic DOL's OSLink / OSUnlink, found
// by the generator from the "OSLink: unknown relocation type" evidence and wrapped to register the module.
constexpr uint32_t kInfo = 0x80600000, kTable = 0x80600100, kStack = 0x80700000;
constexpr uint32_t kOsLink = 0x80003660, kOsUnlink = 0x80003740;

struct ModuleAt {
    uint32_t text, data, bss;
    uint32_t id = 1, numSections = 4, textLen = 0x120;
};

void writeLinkedModule(Ctx& c, const ModuleAt& m) {
    st32(c, kInfo + 0x00, m.id);
    st32(c, kInfo + 0x0C, m.numSections);
    st32(c, kInfo + 0x10, kTable);
    st32(c, kTable + 0, 0);            st32(c, kTable + 4, 0);                      // section 0 unused
    st32(c, kTable + 8, m.text | 1);   st32(c, kTable + 12, m.textLen);            // section 1: text (exec bit set)
    st32(c, kTable + 16, m.data);      st32(c, kTable + 20, 0x40);                  // section 2: data
    st32(c, kTable + 24, m.bss);       st32(c, kTable + 28, 0x20);                  // section 3: bss placed by OSLink
    // Data words the SDK relocates (ADDR32 against the module's own text).
    const uint32_t words[][2] = {{0x10, 0x80}, {0x18, 0xF0}, {0x1C, 0xF8}, {0x20, 0x40}, {0x24, 0x90}, {0x28, 0xA0}, {0x2C, 0xD0}};
    for (const auto& w : words) st32(c, m.data + w[0], m.text + w[1]);
}

bool linkModule(Ctx& c, const ModuleAt& m) {
    writeLinkedModule(c, m);
    c.r[3] = kInfo;
    callGuest(c, kOsLink);
    return c.r[3] == 1;
}

void call(Ctx& c, uint32_t target) { callIndirect(c, 0x80000010, target); }

void testRelLinkAndRun() {
    Rig r;
    Ctx& c = *r.a;
    c.r[1] = kStack;
    const ModuleAt a{0x80501000, 0x80502000, 0x80503000};
    // Before linking the module's code does not exist for dispatch.
    Stop s{};
    CHECK(stops([&] { call(c, a.text + 0x10); }, StopKind::BadIndirect, &s) && s.addr == a.text + 0x10);
    CHECK(linkModule(c, a) && r.machine.relLinked[1] && r.machine.relBase[1][1] == a.text && r.machine.relBase[1][2] == a.data);

    // Prolog: stores 7 at data+0 through a lis/addi pair relocated against the module's data section.
    call(c, a.text + 0x10);
    CHECK(ld32(c, a.data + 0) == 7);
    call(c, a.text + 0x30);   // epilog
    CHECK(c.r[3] == 5);
    // An address formed from the section base (ADDR16_HA/LO): r3 = data + 8 wherever the data section was placed.
    call(c, a.text + 0x90);
    CHECK(c.r[3] == a.data + 8);
    // An internal call and a REL24 call into the DOL; frames carry linked (real) addresses.
    r.machine.traceMaxDepth = 8;
    c.r[3] = 1;
    call(c, a.text + 0x40);
    CHECK(c.r[3] == 22);   // dolDouble(internal(1)) = (1 + 10) * 2
    bool sawInternal = false, sawDol = false;
    for (const TraceHit& h : r.machine.trace) {
        sawInternal = sawInternal || (h.callee == a.text + 0x80 && h.site == a.text + 0x4C);
        sawDol = sawDol || (h.callee == 0x80003800 && h.site == a.text + 0x50);
    }
    CHECK(sawInternal && sawDol);
    // A call through a function pointer stored in (relocated) module data: dispatch finds the module function.
    c.r[3] = 5;
    call(c, a.text + 0xA0);
    CHECK(c.r[3] == 15);
    // The function-pointer table the discovery used is dispatchable too.
    for (const uint32_t off : {0x40u, 0x90u, 0xA0u, 0xD0u}) CHECK(lookupFunction(r.machine, a.text + off) != nullptr);
    // A jump table whose entries are linked addresses: dispatch on the offset within the section.
    for (const auto& [in, out] : {std::pair<uint32_t, uint32_t>{0, 100}, {1, 200}, {2, 300}, {7, 300}}) {
        c.r[3] = in;
        call(c, a.text + 0xD0);
        CHECK(c.r[3] == out);
    }
    CHECK(c.sp == 0);

    // Unlink removes the module's functions and bases.
    c.r[3] = kInfo;
    callGuest(c, kOsUnlink);
    CHECK(c.r[3] == 1 && !r.machine.relLinked[1] && r.machine.relBase[1][1] == 0 && r.machine.dynFunctions.empty());
    CHECK(stops([&] { call(c, a.text + 0x10); }, StopKind::BadIndirect));

    // Relink somewhere else: the same generated code follows the new bases.
    const ModuleAt b{0x80511000, 0x80512000, 0x80513000};
    CHECK(linkModule(c, b));
    call(c, a.text + 0x10 + (b.text - a.text));
    CHECK(ld32(c, b.data) == 7);
    call(c, b.text + 0x90);
    CHECK(c.r[3] == b.data + 8);
    c.r[3] = 1;
    call(c, b.text + 0xD0);
    CHECK(c.r[3] == 200);
    CHECK(stops([&] { call(c, a.text + 0x10); }, StopKind::BadIndirect));   // the old address is gone
}

void testRelLinkErrors() {
    Stop s{};
    {   // a section count that differs from the generated module
        Rig r;
        r.a->r[1] = kStack;
        ModuleAt m{0x80501000, 0x80502000, 0x80503000};
        m.numSections = 3;
        CHECK(stops([&] { linkModule(*r.a, m); }, StopKind::Contract, &s) && s.what && std::string(s.what).find("section count") != std::string::npos);
    }
    {   // a section length that differs
        Rig r;
        r.a->r[1] = kStack;
        ModuleAt m{0x80501000, 0x80502000, 0x80503000};
        m.textLen = 0x100;
        CHECK(stops([&] { linkModule(*r.a, m); }, StopKind::Contract, &s) && s.what && std::string(s.what).find("length") != std::string::npos);
    }
    {   // a module id with no generated code
        Rig r;
        r.a->r[1] = kStack;
        ModuleAt m{0x80501000, 0x80502000, 0x80503000};
        m.id = 5;
        CHECK(stops([&] { linkModule(*r.a, m); }, StopKind::Contract, &s) && s.addr == 5);
    }
    {   // linking twice, and unlinking a module that is not linked
        Rig r;
        r.a->r[1] = kStack;
        const ModuleAt m{0x80501000, 0x80502000, 0x80503000};
        CHECK(linkModule(*r.a, m));
        CHECK(stops([&] { linkModule(*r.a, m); }, StopKind::Contract, &s) && s.what && std::string(s.what).find("already linked") != std::string::npos);
        Rig other;
        other.a->r[1] = kStack;
        writeLinkedModule(*other.a, m);
        other.a->r[3] = kInfo;
        CHECK(stops([&] { callGuest(*other.a, kOsUnlink); }, StopKind::Contract, &s) && s.addr == 1);
    }
}

}  // namespace

int main() {
    testMmioOnlyFunctionIsRecompiled();
    testWriteReachesModel();
    testPrivilegedFunctionStaysStub();
    testHleFunction();
    testLoopPreemption();
    testRelLinkAndRun();
    testRelLinkErrors();
    std::printf("recomp generated-code tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
