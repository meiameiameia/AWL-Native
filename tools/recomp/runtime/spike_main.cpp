// awl-recomp-spike: console runner for the recompiled boot path (no window, no DX11).
//   awl-recomp-spike [--dol main.dol]      (or AWL_RECOMP_DOL)
// Loads the DOL into a flat 24 MB guest memory, installs the documented OS low-memory values the IPL and
// apploader would have left, and runs the recompiled code from the DOL entry point until it reaches a
// platform boundary or any other stop condition. Stops are reported with the call trace.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "guest.h"
#include "image.h"

using namespace awl;
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
    }
    return "?";
}

void store32(uint8_t* mem, uint32_t guestAddr, uint32_t v) {
    uint8_t* p = mem + (guestAddr & 0x0FFFFFFFu);
    p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);  p[3] = static_cast<uint8_t>(v);
}

// Values documented for the OS low-memory area that the IPL/apploader leave behind (names only; no game data).
void installLowMemory(uint8_t* mem) {
    std::memcpy(mem, "GYWEE9", 6);                  // game id (verified target)
    store32(mem, 0x80000020, 0x0D15EA5E);           // boot magic: retail disc boot
    store32(mem, 0x80000024, 1);                    // boot version
    store32(mem, 0x80000028, kMem1Size);            // physical memory size
    store32(mem, 0x800000CC, 0);                    // video mode: NTSC
    store32(mem, 0x800000D0, 0x01000000);           // ARAM size
    store32(mem, 0x800000F0, kMem1Size);            // simulated memory size
    store32(mem, 0x800000F8, 162000000);            // bus clock
    store32(mem, 0x800000FC, 486000000);            // CPU clock
}

}  // namespace

int main(int argc, char** argv) {
    std::string dolPath = std::getenv("AWL_RECOMP_DOL") ? std::getenv("AWL_RECOMP_DOL") : "";
    bool census = false;
    uint32_t traceDepth = 0;
    uint64_t limit = ~0ull;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--census") census = true;
        else if (a == "--trace" && i + 1 < argc) traceDepth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--limit" && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 0);
    }
    if (dolPath.empty()) {
        std::fprintf(stderr, "usage: awl-recomp-spike --dol <main.dol>   (or set AWL_RECOMP_DOL)\n");
        return 2;
    }
    recomp::DolImage dol;
    std::string err;
    if (!recomp::loadDol(dolPath, dol, err)) {
        std::fprintf(stderr, "DOL: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> mem(kMem1Size, 0);
    std::vector<uint8_t> lc(kLockedCacheSize, 0);
    for (const auto& s : dol.sections) {
        if ((s.addr >> 28) != 8 || (s.addr & 0x0FFFFFFFu) + s.size > kMem1Size) {
            std::fprintf(stderr, "DOL section %d (0x%08x, %u bytes) does not fit MEM1\n", s.index, s.addr, s.size);
            return 1;
        }
        std::memcpy(mem.data() + (s.addr & 0x0FFFFFFFu), dol.file.data() + s.fileOffset, s.size);
    }
    installLowMemory(mem.data());

    auto ctx = std::make_unique<Ctx>();
    ctx->mem = mem.data();
    ctx->lc = lc.data();
    ctx->msr = 0x00002032;   // FP and ME set; interrupts off (only visible through mfmsr)
    setFpscr(*ctx, 0);
    ctx->skipPlatform = census;
    ctx->limit = limit;
    ctx->traceMaxDepth = traceDepth;

    GuestFn entry = lookupFunction(dol.entry);
    std::printf("awl-recomp-spike: %u recompiled functions linked, entry 0x%08X %s\n", kFunctionCount, dol.entry, entry ? "found" : "MISSING");
    if (!entry) return 1;

    int rc = 0;
    try {
        entry(*ctx);
        std::printf("entry function returned after %llu instructions\n", static_cast<unsigned long long>(ctx->icount));
    } catch (const Stop& s) {
        std::printf("\nSTOP: %s\n", kindName(s.kind));
        std::printf("  guest pc            0x%08X\n", s.pc);
        if (s.addr) std::printf("  address involved    0x%08X\n", s.addr);
        std::printf("  detail              %s\n", s.what);
        std::printf("  instructions retired %llu (counted through the stopping instruction)\n", static_cast<unsigned long long>(ctx->icount));
        std::printf("  call depth           %u\n", ctx->sp);
        std::printf("  call trace (innermost first; function <- call site):\n");
        for (uint32_t k = ctx->sp; k-- > 0;) {
            const uint32_t caller = k == 0 ? dol.entry : ctx->cs[k - 1].callee;
            std::printf("    #%-3u 0x%08X <- 0x%08X (in 0x%08X)\n", ctx->sp - 1 - k, ctx->cs[k].callee, ctx->cs[k].site, caller);
        }
        std::printf("    entry 0x%08X\n", dol.entry);
        rc = 3;
    }
    if (traceDepth) {
        std::printf("\ncall trace to depth %u (execution order; indentation = nesting, @ = instructions retired at the call):\n", traceDepth);
        for (const auto& t : ctx->trace)
            std::printf("  %*s0x%08X <- 0x%08X  @%llu\n", static_cast<int>(t.depth * 2), "", t.callee, t.site, static_cast<unsigned long long>(t.icount));
    }
    if (census) {
        std::printf("\nplatform boundaries skipped in execution order (census mode; results after the first are NOT valid emulation):\n");
        std::vector<uint32_t> seen;
        for (const auto& b : ctx->boundaries) {
            bool dup = false;
            for (uint32_t a : seen) dup |= a == b.addr;
            if (dup) continue;
            seen.push_back(b.addr);
            std::printf("  #%-3zu 0x%08X  first hit at instruction %-10llu from site 0x%08X  [%s]\n", seen.size(), b.addr,
                        static_cast<unsigned long long>(b.icount), b.callSite, b.evidence);
        }
        std::printf("  %zu distinct boundaries, %zu total hits\n", seen.size(), ctx->boundaries.size());
        std::printf("absorbed memory faults (hardware registers read as 0, writes dropped): %zu accesses\n", ctx->faults.size());
        std::vector<uint32_t> faultAddrs;
        for (const auto& f : ctx->faults) {
            bool dup = false;
            for (uint32_t a : faultAddrs) dup |= a == f.addr;
            if (dup) continue;
            faultAddrs.push_back(f.addr);
            if (faultAddrs.size() <= 40)
                std::printf("  0x%08X  %-16s first at instruction %-10llu pc 0x%08X\n", f.addr, f.evidence,
                            static_cast<unsigned long long>(f.icount), f.callSite);
        }
        std::printf("  %zu distinct addresses\n", faultAddrs.size());
    }
    return rc;
}
