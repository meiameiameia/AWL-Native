// End-to-end generator test: awl-recomp generates C++ from the hand-assembled DOL written by
// recomp_synth_dol (see that file for the layout), the build compiles it into this executable, and the tests
// run the generated functions against the runtime contract.
#include <cstdio>
#include <memory>
#include <string>
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

}  // namespace

int main() {
    testMmioOnlyFunctionIsRecompiled();
    testWriteReachesModel();
    testPrivilegedFunctionStaysStub();
    testHleFunction();
    testLoopPreemption();
    std::printf("recomp generated-code tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
