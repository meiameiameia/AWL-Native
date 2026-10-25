// awl-host: console game host. Loads the DOL, builds the boot environment, registers the platform handlers and models,
// runs the recompiled entry point and reports where it stops. Deterministic: time is derived from retired instructions.
//   awl-host --dol rom/main.dol [--disc disc] [--trace build/boot-trace.log] [--retraces N] [--limit N] [--pad neutral|xinput]
//            [--calls DEPTH] [--dump-boot FILE] [--dump-mem FILE]
// --calls records the calls of the functions in capture_parse.h (to DEPTH) as "call" trace lines, for awl-capture-compare;
// --dump-boot writes MEM1 as the game program starts, --dump-mem writes MEM1 when the run ends (use --limit to stop at an instruction).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "boot.h"
#include "capture_parse.h"
#include "devices.h"
#include "image.h"

using namespace awl;
using namespace awl::hle;
using namespace awl::rt;

namespace {

const char* kindName(StopKind k) {
    switch (k) {
        case StopKind::Platform: return "PLATFORM BOUNDARY";
        case StopKind::Mmio: return "HARDWARE REGISTER ACCESS";
        case StopKind::Privileged: return "PRIVILEGED INSTRUCTION";
        case StopKind::UnresolvedJump: return "UNRESOLVED bctr";
        case StopKind::BadIndirect: return "INDIRECT TARGET NOT RECOMPILED";
        case StopKind::BadReturn: return "NON-LOCAL RETURN";
        case StopKind::Fault: return "MEMORY FAULT";
        case StopKind::Unimplemented: return "UNTRANSLATED CODE";
        case StopKind::DepthExceeded: return "CALL DEPTH";
        case StopKind::InstructionLimit: return "INSTRUCTION LIMIT";
        case StopKind::HleUnregistered: return "HLE FUNCTION WITHOUT HANDLER";
        case StopKind::Contract: return "RUNTIME CONTRACT VIOLATION";
    }
    return "?";
}

bool writeMemory(const std::string& path, const std::vector<uint8_t>& mem) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s (does the folder exist?)\n", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(mem.data(), 1, mem.size(), f) == mem.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) std::fprintf(stderr, "short write to %s\n", path.c_str());
    return ok && closed;
}

// Call lines for awl-capture-compare: the listed functions in execution order, plus the instruction count at which OSInit
// has returned (the next call of the same caller), which is where capture A's second dump is taken.
void writeCallTrace(const Machine& m) {
    const std::vector<TraceHit>& t = m.trace;
    uint32_t osInitDepth = ~0u;
    size_t osInitAt = t.size();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].callee == 0x801B0DE0 && t[i].depth < osInitDepth) { osInitDepth = t[i].depth; osInitAt = i; }
    for (size_t i = osInitAt + 1; i < t.size(); ++i)
        if (t[i].depth <= osInitDepth) { trace().line("boot.after-osinit icount=%llu", static_cast<unsigned long long>(t[i].icount)); break; }
    for (const TraceHit& h : t)
        if (capture::watchedName(h.callee))
            trace().line("call %08X site=%08X depth=%u icount=%llu", h.callee, h.site, h.depth, static_cast<unsigned long long>(h.icount));
}

}  // namespace

int main(int argc, char** argv) {
    std::string dolPath = std::getenv("AWL_RECOMP_DOL") ? std::getenv("AWL_RECOMP_DOL") : "";
    std::string tracePath = "build/boot-trace.log";
    Config cfg;
    uint64_t limit = 4000000000ull;     // instruction budget; --limit 0 removes it
    uint32_t callDepth = 16;            // --calls: deepest call level recorded for the capture comparison (0 = off)
    std::string dumpBoot, dumpMem;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--disc" && i + 1 < argc) cfg.discDir = argv[++i];
        else if (a == "--trace" && i + 1 < argc) tracePath = argv[++i];
        else if (a == "--pad" && i + 1 < argc) { const std::string v = argv[++i]; cfg.padXInput = v == "xinput"; if (v != "xinput" && v != "neutral") { std::fprintf(stderr, "--pad takes neutral or xinput\n"); return 2; } }
        else if (a == "--retraces" && i + 1 < argc) cfg.mainLoopRetraces = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--limit" && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--calls" && i + 1 < argc) callDepth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--dump-boot" && i + 1 < argc) dumpBoot = argv[++i];
        else if (a == "--dump-mem" && i + 1 < argc) dumpMem = argv[++i];
    }
    if (limit == 0) limit = ~0ull;
    if (dolPath.empty()) {
        std::fprintf(stderr, "usage: awl-host --dol <main.dol> [--disc disc] [--trace file] [--retraces N] [--limit N] [--pad neutral|xinput]\n"
                             "                [--calls DEPTH] [--dump-boot FILE] [--dump-mem FILE]\n");
        return 2;
    }
    recomp::DolImage dol;
    std::string err;
    if (!recomp::loadDol(dolPath, dol, err)) {
        std::fprintf(stderr, "DOL: %s\n", err.c_str());
        return 1;
    }

    std::vector<uint8_t> mem(kMem1Size), lc(kLockedCacheSize);
    Machine machine;
    machine.mem = mem.data();
    machine.lc = lc.data();
    machine.setLimit(limit);
    Platform& plat = Platform::get();
    trace().open(tracePath);
    plat.attach(machine, cfg);
    BootLayout layout;
    if (!buildBootEnvironment(machine, dol, cfg.discDir, layout, err)) {
        std::fprintf(stderr, "boot environment: %s\n", err.c_str());
        return 1;
    }
    plat.installHandlers();
    if (!dumpBoot.empty() && !writeMemory(dumpBoot, mem)) return 1;
    machine.traceMaxDepth = callDepth;

    auto ctx = std::make_unique<Ctx>(machine);
    ctx->msr = bootValues().entryMsr;
    setFpscr(*ctx, 0);
    if (!lookupFunction(machine, dol.entry)) {
        std::fprintf(stderr, "entry 0x%08X is not a recompiled function\n", dol.entry);
        return 1;
    }
    std::printf("awl-host: %u recompiled functions, entry 0x%08X, trace %s\n", machine.functionCount, dol.entry, tracePath.c_str());

    const bool ok = plat.run(*ctx, dol.entry);
    const StopReport& r = plat.report();
    int rc = 0;
    if (r.stopped) {
        const Stop& s = r.stop;
        std::printf("\nSTOP: %s\n  guest pc             0x%08X\n", kindName(s.kind), s.pc);
        if (s.addr) std::printf("  address involved     0x%08X\n", s.addr);
        if (s.name) std::printf("  function name        %s\n", s.name);
        std::printf("  detail               %s\n", s.what);
        std::printf("  instructions retired %llu\n  thread               %u\n", static_cast<unsigned long long>(machine.icount), r.threadId);
        if (r.ctx) {
            std::printf("  call trace (innermost first; function <- call site):\n");
            for (uint32_t k = r.ctx->sp; k-- > 0;) {
                const uint32_t caller = k == 0 ? dol.entry : r.ctx->cs[k - 1].callee;
                std::printf("    #%-3u 0x%08X <- 0x%08X (in 0x%08X)\n", r.ctx->sp - 1 - k, r.ctx->cs[k].callee, r.ctx->cs[k].site, caller);
            }
        }
        trace().line("STOP %s pc=%08X addr=%08X %s", kindName(s.kind), s.pc, s.addr, s.what);
        rc = 3;
    } else if (!plat.exitReason().empty()) {
        std::printf("\nEXIT: %s after %llu instructions\n", plat.exitReason().c_str(), static_cast<unsigned long long>(machine.icount));
        trace().line("EXIT %s icount=%llu", plat.exitReason().c_str(), static_cast<unsigned long long>(machine.icount));
        rc = plat.exitReason() == "main loop reached" ? 0 : 4;
    } else if (ok) {
        std::printf("\nentry function returned after %llu instructions\n", static_cast<unsigned long long>(machine.icount));
    }
    if (machine.traceMaxDepth) writeCallTrace(machine);
    if (!dumpMem.empty() && !writeMemory(dumpMem, mem)) rc = 1;
    const auto& k = plat.counters;
    std::printf("retraces %llu, framebuffer latches %llu, EFB copies %llu, tokens %llu, finishes %llu\n", static_cast<unsigned long long>(k.retraces),
                static_cast<unsigned long long>(k.fbLatches), static_cast<unsigned long long>(k.efbCopies), static_cast<unsigned long long>(k.tokens),
                static_cast<unsigned long long>(k.finishes));
    trace().line("END icount=%llu tick=%llu retraces=%llu", static_cast<unsigned long long>(machine.icount), static_cast<unsigned long long>(plat.now()),
                 static_cast<unsigned long long>(k.retraces));
    trace().close();
    return rc;
}
