// awl-host: console game host. Loads the DOL, builds the boot environment, registers the platform handlers and models,
// runs the recompiled entry point and reports where it stops. Deterministic: time is derived from retired instructions.
//   awl-host --dol rom/main.dol [--disc disc] [--trace build/boot-trace.log] [--retraces N] [--limit N] [--pad neutral|xinput]
//            [--calls DEPTH] [--dump-boot FILE] [--dump-mem FILE] [--realtime]
//            [--window] [--realtime-pace] [--capture-frames N DIR [--capture-from F [--capture-every K]]] [--gfx-warp]
// --calls records the calls of the functions in capture_parse.h (to DEPTH) as "call" trace lines, for awl-capture-compare;
// --dump-boot writes MEM1 as the game program starts, --dump-mem writes MEM1 when the run ends (use --limit to stop at an instruction).
// P2 video (src/gfx): --window opens the game window and runs until it is closed (Esc, close button, Alt+F4) unless --retraces or --limit
// is given; --realtime-pace sleeps so guest retraces happen at 59.94 Hz of wall time (implied by --window; it never changes what the guest
// computes); --capture-frames N DIR writes BMPs of the first N presented frames (headless or windowed; DIR belongs under build/);
// --gfx-warp forces the software Direct3D device. In window mode the keyboard and XInput controller 0 feed PADRead while the window has focus. Without these options the host is the deterministic console host.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "boot.h"
#include "capture_parse.h"
#include "dev_efb.h"
#include "dev_vi.h"
#include "devices.h"
#include "image.h"
#include "pad_source.h"
#ifdef AWL_HAVE_GFX
#include "gfx/gfx_output.h"
#include "gfx/gfx_renderer.h"
#include "gx/gx_record.h"
#endif

using namespace awl;
using namespace awl::hle;
using namespace awl::rt;

namespace {

#ifdef AWL_HAVE_GFX
// EFB peeks and pokes of the CPU, answered from the renderer's framebuffer. Depth peeks read the GPU depth buffer (24 bits);
// colour peeks and pokes are not modelled yet and stop loudly like the default provider.
class RendererEfb : public EfbAccess {
public:
    explicit RendererEfb(gfx::Renderer& r) : r_(r) {}
    uint32_t peek(Ctx& c, EfbKind kind, uint32_t x, uint32_t y) override {
        if (kind != EfbKind::Depth) stopHw(c, kEfbWindowBase, "EFB colour peek is not modelled yet");
        uint32_t z = 0;
        std::string err;
        if (!r_.peekDepth(x, y, z, err)) { msg_ = "EFB depth peek: " + err; stopHw(c, kEfbWindowBase, msg_.c_str()); }
        return z;
    }
    void poke(Ctx& c, EfbKind, uint32_t, uint32_t, uint32_t) override { stopHw(c, kEfbWindowBase, "EFB poke is not modelled yet"); }
private:
    gfx::Renderer& r_;
    std::string msg_;
};
#endif

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
            trace().line("call %08X site=%08X depth=%u icount=%llu r3=%08X r4=%08X r5=%08X r6=%08X", h.callee, h.site, h.depth,
                         static_cast<unsigned long long>(h.icount), h.args[0], h.args[1], h.args[2], h.args[3]);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dolPath = std::getenv("AWL_RECOMP_DOL") ? std::getenv("AWL_RECOMP_DOL") : "";
    std::string tracePath = "build/boot-trace.log";
    Config cfg;
    uint64_t limit = 4000000000ull;     // instruction budget; --limit 0 removes it
    uint32_t callDepth = 16;            // --calls: deepest call level recorded for the capture comparison (0 = off)
    std::string dumpBoot, dumpMem;
    bool optWindow = false, optPace = false, optWarp = false, retracesGiven = false, limitGiven = false;
    uint32_t captureFrames = 0, captureFrom = 0, captureEvery = 1;
    std::string captureDir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--disc" && i + 1 < argc) cfg.discDir = argv[++i];
        else if (a == "--trace" && i + 1 < argc) tracePath = argv[++i];
        else if (a == "--pad" && i + 1 < argc) { const std::string v = argv[++i]; cfg.padXInput = v == "xinput"; if (v != "xinput" && v != "neutral") { std::fprintf(stderr, "--pad takes neutral or xinput\n"); return 2; } }
        else if (a == "--retraces" && i + 1 < argc) { cfg.mainLoopRetraces = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)); retracesGiven = true; }
        else if (a == "--limit" && i + 1 < argc) { limit = std::strtoull(argv[++i], nullptr, 0); limitGiven = true; }
        else if (a == "--no-idle-skip") cfg.idleSkip = false;      // execute the scheduler idle spin instead of fast-forwarding it (same result, slower)
        else if (a == "--window") optWindow = true;
        else if (a == "--realtime-pace") optPace = true;
        else if (a == "--gfx-warp") optWarp = true;
        else if (a == "--capture-from" && i + 1 < argc) captureFrom = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--capture-every" && i + 1 < argc) captureEvery = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--capture-frames" && i + 2 < argc) { captureFrames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)); captureDir = argv[++i]; }
        else if (a == "--realtime") cfg.rtcSeconds = hostRtcSeconds();     // clock from the host: not deterministic
        else if (a == "--calls" && i + 1 < argc) callDepth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--dump-boot" && i + 1 < argc) dumpBoot = argv[++i];
        else if (a == "--dump-mem" && i + 1 < argc) dumpMem = argv[++i];
    }
    if (limit == 0) limit = ~0ull;
    const bool useVideo = optWindow || optPace || captureFrames != 0;
#ifndef AWL_HAVE_GFX
    if (useVideo) { std::fprintf(stderr, "this build has no video support (awl_gfx)\n"); return 2; }
#endif
    if (optWindow) {   // a window runs until it is closed, not until the main-loop criterion
        if (!retracesGiven) cfg.mainLoopRetraces = ~0u;
        if (!limitGiven) limit = ~0ull;
    }
    if (dolPath.empty()) {
        std::fprintf(stderr, "usage: awl-host --dol <main.dol> [--disc disc] [--trace file] [--retraces N] [--limit N] [--pad neutral|xinput]\n"
                             "                [--calls DEPTH] [--dump-boot FILE] [--dump-mem FILE] [--realtime]\n");
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

#ifdef AWL_HAVE_GFX
    std::unique_ptr<gfx::VideoOutput> video;
    std::unique_ptr<gfx::Renderer> renderer;
    std::unique_ptr<gfx::RendererSource> rendererSource;
    std::unique_ptr<RendererEfb> rendererEfb;
    if (useVideo) {
        gfx::OutputOptions go;
        go.window = optWindow;
        go.pace = optPace;
        go.captureFrames = captureFrames;
        go.captureFrom = captureFrom;
        go.captureEvery = captureEvery;
        go.captureDir = captureDir;
        go.forceWarp = optWarp;
        video = std::make_unique<gfx::VideoOutput>();
        if (!video->init(go, err)) {
            std::fprintf(stderr, "video: %s\n", err.c_str());
            return 1;
        }
        std::printf("awl-host: video %s%s%s, Direct3D device %s%s\n", optWindow ? "window" : "headless", optPace || optWindow ? " paced at 59.94 Hz" : "",
                    captureFrames ? " with frame capture" : "", video->adapterName().empty() ? "(none needed)" : video->adapterName().c_str(),
                    video->usingWarp() ? " (WARP)" : "");
        gfx::VideoOutput* v = video.get();
        if (v->haveDevice()) {   // window or capture: the DX11 renderer draws the GX core's commands
            renderer = std::make_unique<gfx::Renderer>();
            if (!renderer->init(v->device(), err)) { std::fprintf(stderr, "renderer: %s\n", err.c_str()); return 1; }
            if (const char* occ = std::getenv("AWL_GFX_OCCLUSION")) { unsigned a = 0, b = 0; if (std::sscanf(occ, "%u,%u", &a, &b) == 2) renderer->setOcclusionDebug(a, b); }
            rendererSource = std::make_unique<gfx::RendererSource>(*renderer);
            rendererEfb = std::make_unique<RendererEfb>(*renderer);
            v->setSource(rendererSource.get());
            gx::backendHub().add(renderer.get());
            setEfbAccess(rendererEfb.get());
        }
        if (v->input()) {   // window mode: keyboard and the controller, only while the window has focus
            setPadProvider(
                [](void* user) {
                    const PadSample& s = static_cast<gfx::VideoOutput*>(user)->input()->sample();
                    PadReading r;
                    r.connected = s.connected; r.buttons = s.buttons;
                    r.stickX = s.stick_x; r.stickY = s.stick_y; r.substickX = s.substick_x; r.substickY = s.substick_y;
                    r.triggerL = s.trigger_l; r.triggerR = s.trigger_r;
                    return r;
                },
                v);
        }
        setViRetraceHook([v, &plat](const ViFrameInfo* f) {
            gfx::FrameDesc d;
            if (f) {
                d.xfbAddr = f->xfbAddr; d.width = f->width; d.height = f->height; d.strideBytes = f->strideBytes;
                d.interlaced = f->interlaced; d.latchIndex = f->latchIndex;
            }
            if (v->onRetrace(f ? &d : nullptr) == gfx::RetraceAction::Quit) plat.requestExit(v->error().empty() ? "window closed" : "video output failed");
        });
    }
#endif

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
        if (r.ctx) std::printf("  MSR                  0x%08X\n", r.ctx->msr);
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
        rc = plat.exitReason() == "main loop reached" || plat.exitReason() == "window closed" ? 0 : 4;
    } else if (ok) {
        std::printf("\nentry function returned after %llu instructions\n", static_cast<unsigned long long>(machine.icount));
    }
#ifdef AWL_HAVE_GFX
    if (renderer) renderer->printSummary();
    if (video) std::printf("video: %llu frames presented, %llu captured, %llu pacing re-anchors\n", static_cast<unsigned long long>(video->presented()),
                           static_cast<unsigned long long>(video->captured()), static_cast<unsigned long long>(video->reanchors()));
#endif
    if (std::getenv("AWL_IDLE_STATS"))
        std::printf("idle skip: %llu probes, %llu skips, %llu instructions fast-forwarded of %llu retired\n", (unsigned long long)plat.idle.probes,
                    (unsigned long long)plat.idle.skips, (unsigned long long)plat.idle.instructionsSkipped, (unsigned long long)machine.icount);
    if (!plat.pcProfile.empty()) {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (const auto& e : plat.pcProfile) top.push_back({e.second, e.first});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 25; ++i)
            std::printf("pc profile %08X %5.1f%%\n", top[i].second, 100.0 * double(top[i].first) / double(plat.idle.probes));
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
