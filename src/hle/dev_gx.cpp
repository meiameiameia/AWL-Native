// GX: the write-gather port, the command-processor / pixel-engine registers the SDK uses, and the connection of the byte
// stream to the GX core (src/gx): the core parses the command stream, runs the XF stage and delivers IR commands to the
// backend hub; this file completes the services the SDK waits on (draw-done/finish, draw-sync tokens, EFB copies) and
// provides guest memory to the core. P1 behaviour (the completion semantics) is unchanged.
// Register roles and command formats: public GameCube documentation and what GXInit/GXSetDrawSync/GXPEInit do in the DOL
// (docs/research/hle-platform.md, sections 2.7 and 5.5); the GX core note is docs/research/gx-core.md.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <string>

#include "devices.h"
#include "gx/gx_core.h"
#include "gx/gx_dump.h"
#include "gx/gx_record.h"

namespace awl::hle {
namespace {

constexpr uint64_t kFinishLatency = 2000;   // ticks between the BP finish command reaching the GP and the interrupt
constexpr uint64_t kTokenLatency = 2000;

// ---- Stream capture ---------------------------------------------------------------------------------------------
// AWL_GX_CAPTURE=<file> writes the command stream (format: src/gx/gx_dump.h) of the retraces AWL_GX_FRAMES=<first>-<last>
// (default: all). Frames before <first> contribute only their state-setting commands, so a later start still has the
// register state. The file belongs under build/.
class Capture {
public:
    Capture() {
        const char* path = std::getenv("AWL_GX_CAPTURE");
        if (!path || !*path) return;
        const char* frames = std::getenv("AWL_GX_FRAMES");
        if (frames && *frames) {
            char* end = nullptr;
            first_ = static_cast<uint32_t>(std::strtoul(frames, &end, 0));
            if (*end == '-') last_ = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0));
            else last_ = first_;
        }
        if (!w_.open(path)) return;
        std::fprintf(stderr, "gx capture: %s, retraces %u-%u\n", path, first_, last_);
    }

    // Called with every 32-byte chunk before it is parsed and after the parse, so that the display lists called by the
    // chunk are written before the chunk (the reader pops them in call order).
    void begin(uint32_t retrace, bool pendingEmpty) {
        mode_ = Mode::Off;
        if (!w_.isOpen()) return;
        if (retrace > last_) { w_.close(); return; }
        if (retrace >= first_ && (active_ || pendingEmpty)) active_ = true;
        mode_ = active_ ? Mode::Active : Mode::Prelude;
        if (mode_ == Mode::Active && retrace != frame_) {
            frame_ = retrace;
            w_.frame(retrace);
        }
    }
    void end(const uint8_t* data, size_t n) {
        if (mode_ == Mode::Active) w_.fifo(data, n);
    }
    void displayList(uint32_t addr, uint32_t size, const uint8_t* data) {
        if (mode_ == Mode::Active) w_.displayList(addr, size, data);
    }
    void command(uint8_t op, const uint8_t* d, size_t n) {
        if (mode_ == Mode::Prelude && (op == 0x08 || op == 0x10 || op == 0x61)) w_.prelude(d, n);
    }

private:
    enum class Mode { Off, Prelude, Active };
    gx::dump::Writer w_;
    Mode mode_ = Mode::Off;
    bool active_ = false;
    uint32_t first_ = 0, last_ = 0xFFFFFFFFu, frame_ = 0xFFFFFFFFu;
};

// ---- Core adapter ---------------------------------------------------------------------------------------------
// The command processor lives in src/gx (gx_core.h): it parses the stream, models CP/XF/BP state, does the XF stage and
// delivers IR commands to the backend hub (recording backend, DX11 renderer). This file connects it to the platform:
// guest memory, the finish/token/copy services the SDK waits on, the stream capture above, and the optional IR dump
// (--gx-dump N [--gx-dump-from F] [--gx-dump-dir D] on the awl-host command line; environment AWL_GX_STATS=1 prints the
// core's per-frame cost at exit).
class PlatformMemory : public gx::GuestMemory {
public:
    explicit PlatformMemory(Platform& p) : p_(p) {}
    const uint8_t* read(uint32_t phys, uint32_t size) const override { return p_.mem().ptr(0x80000000u | phys, size); }
private:
    Platform& p_;
};

class PlatformHost : public gx::CoreHost {
public:
    explicit PlatformHost(Platform& p) : p_(p) {}
    void drawDone() override { p_.post(p_.now() + kFinishLatency, Ev::PeFinish); }
    void token(uint16_t value, bool interrupt) override {
        if (interrupt) p_.post(p_.now() + kTokenLatency, Ev::PeToken, value, 1);
        else p_.post(p_.now(), Ev::PeToken, value, 0);
    }
    void efbCopyIssued(gx::CopyTarget) override { ++p_.counters.efbCopies; p_.counters.copySinceLatch = true; }
    void log(const char* message) override { trace().line("%s", message); }
private:
    Platform& p_;
};

class GxFeed : public gx::StreamObserver {
public:
    explicit GxFeed(Platform& p) : p_(p), mem_(p), host_(p), core_(mem_, gx::backendHub(), host_) {
        core_.setObserver(this);
        core_.setProfiling(std::getenv("AWL_GX_STATS") != nullptr);
        parseOptions();
    }
    ~GxFeed() override {
        if (rec_) gx::backendHub().remove(rec_.get());
        if (frameLog_) gx::backendHub().remove(frameLog_.get());
        if (std::getenv("AWL_GX_STATS")) {
            const gx::CoreStats& s = core_.stats();
            const double frames = p_.counters.retraces ? double(p_.counters.retraces) : 1.0;
            std::fprintf(stderr, "gx core: %llu commands, %llu draws, %llu vertices, %llu triangles (%llu culled), %llu copies, %llu pipeline rebuilds, textures %llu decoded / %llu cached, "
                         "%.1f MB hashed + %.1f MB compared (%llu bindings trusted within an invalidate epoch), %llu draws on the 4-wide path; %.3f s in the core = %.1f us per retrace\n",
                         (unsigned long long)s.commands, (unsigned long long)s.draws, (unsigned long long)s.vertices, (unsigned long long)s.triangles, (unsigned long long)s.culledTriangles,
                         (unsigned long long)s.copies, (unsigned long long)s.pipelineRebuilds, (unsigned long long)s.textureDecodes, (unsigned long long)s.textureCacheHits,
                         s.textureBytesHashed / 1048576.0, s.textureBytesCompared / 1048576.0, (unsigned long long)s.textureTrusted, (unsigned long long)s.batchedDraws, s.busySeconds, 1e6 * s.busySeconds / frames);
            std::fprintf(stderr, "gx core split: pipeline %.3f s, vertices+XF+primitives %.3f s, backend %.3f s\n", s.pipelineSeconds, s.vertexSeconds, s.backendSeconds);
        }
    }
    void displayList(uint32_t addr, uint32_t size, const uint8_t* data) override { cap_.displayList(addr, size, data); }
    void command(uint8_t op, const uint8_t* d, size_t n) override { cap_.command(op, d, n); }

    void feed(Ctx& c, const uint8_t* data, size_t n) {
        const uint32_t retrace = static_cast<uint32_t>(p_.counters.retraces);
        if (memAt_ && retrace >= memAt_ && !core_.midCommand()) dumpMemory();
        cap_.begin(retrace, !core_.midCommand());
        core_.setRetrace(retrace);
        try {
            core_.feed(data, n);
        } catch (const gx::GxError& e) {
            static std::string message;   // stopHw keeps the pointer
            char where[96];
            std::snprintf(where, sizeof where, " (command stream offset %llu, code 0x%X)", (unsigned long long)e.streamOffset, e.code);
            message = std::string("GX core: ") + e.what() + where;
            stopHw(c, e.code, message.c_str());
        }
        cap_.end(data, n);
    }
    void reset() {}

private:
    // AWL_GX_MEM_AT=<retrace> AWL_GX_MEM_FILE=<file>: write MEM1 as it is when that retrace first sends commands (for awl-gx-bench).
    void dumpMemory() {
        const char* path = std::getenv("AWL_GX_MEM_FILE");
        const uint8_t* base = p_.mem().ptr(0x80000000u, 0x01800000u);
        memAt_ = 0;
        if (!path || !base) return;
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(base, 1, 0x01800000u, f);
            std::fclose(f);
        }
    }
    void parseOptions() {
        if (const char* m = std::getenv("AWL_GX_MEM_AT")) memAt_ = static_cast<uint32_t>(std::strtoul(m, nullptr, 0));
        uint32_t count = 0, from = 0;
        std::string dir = std::getenv("AWL_GX_DUMP_DIR") ? std::getenv("AWL_GX_DUMP_DIR") : "build/gxdump";
        for (int i = 1; i + 1 < __argc; ++i) {
            const std::string a = __argv[i];
            if (a == "--gx-dump") count = static_cast<uint32_t>(std::strtoul(__argv[i + 1], nullptr, 0));
            else if (a == "--gx-dump-from") from = static_cast<uint32_t>(std::strtoul(__argv[i + 1], nullptr, 0));
            else if (a == "--gx-dump-dir") dir = __argv[i + 1];
        }
        if (const char* fl = std::getenv("AWL_GX_FRAMELOG")) {
            frameLog_ = std::make_unique<gx::FrameLogBackend>(fl);
            gx::backendHub().add(frameLog_.get());
        }
        if (count) {
            rec_ = std::make_unique<gx::RecordingBackend>(dir, from, count);
            gx::backendHub().add(rec_.get());
        }
    }
    Platform& p_;
    PlatformMemory mem_;
    PlatformHost host_;
    gx::Core core_;
    Capture cap_;
    uint32_t memAt_ = 0;
    std::unique_ptr<gx::RecordingBackend> rec_;
    std::unique_ptr<gx::FrameLogBackend> frameLog_;
};

// ---- Shared GX state -------------------------------------------------------------------------------------------
struct GxShared {
    explicit GxShared(Platform& p) : feeder(p), p(p) {}
    GxFeed feeder;
    Platform& p;
    uint16_t peTokenReg = 0;
    uint16_t peControl = 0;        // bits 0-1 enables, bit 2 token status, bit 3 finish status
    uint8_t staging[32] = {};
    uint32_t staged = 0;

    void updateLines() {
        p.setLine(pibit::PeToken, (peControl & 4) != 0);
        p.setLine(pibit::PeFinish, (peControl & 8) != 0);
    }

    // One 32-byte chunk leaves the write-gather pipe: mirror it into the CPU FIFO ring in memory (when configured)
    // and hand it to the command processor.
    void flushChunk(Ctx& c) {
        uint32_t& wp = p.piFifoWp;
        const uint32_t base = p.piFifoBase & 0x03FFFFFFu, end = p.piFifoEnd & 0x03FFFFFFu;
        if (end > base) {
            uint32_t addr = wp & 0x03FFFFFFu;
            if (addr < base || addr > end) addr = base;
            if (uint8_t* dst = p.mem().ptr(0x80000000u | addr, 32)) std::memcpy(dst, staging, 32);
            addr += 32;
            uint32_t wrap = wp & 0x04000000u;
            if (addr > end) { addr = base; wrap ^= 0x04000000u; }
            wp = wrap | addr;
        }
        feeder.feed(c, staging, 32);
    }
};

// ---- CP -------------------------------------------------------------------------------------------------------
class Cp : public Device {
public:
    Cp(Platform& p, std::shared_ptr<GxShared>) : p_(p) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2 || (off & 1) || off >= 0x40) stopHw(c, addr, "CP register not modelled (read)");
        switch (off) {
            case 0x00: return 0x000C;                         // read idle, command idle; no overflow, underflow or breakpoint
            case 0x30: case 0x32: return 0;                   // FIFO read/write distance: the GP always keeps up
            case 0x34: return p_.piFifoWp & 0xFFFF;
            case 0x36: return (p_.piFifoWp >> 16) & 0x07FF;
            case 0x38: return p_.piFifoWp & 0xFFFF;           // read pointer == write pointer
            case 0x3A: return (p_.piFifoWp >> 16) & 0x07FF;
            default: return reg_[off / 2];
        }
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2 || (off & 1) || off >= 0x40) stopHw(c, addr, "CP register not modelled (write)");
        reg_[off / 2] = static_cast<uint16_t>(v);
    }
private:
    Platform& p_;
    uint16_t reg_[0x20] = {};
};

// ---- PE -------------------------------------------------------------------------------------------------------
class Pe : public Device {
public:
    Pe(Platform& p, std::shared_ptr<GxShared> sp) : p_(p), hold_(sp), s_(*sp) {
        p.onEvent(Ev::PeFinish, [this](Ctx&, const Event&) {
            s_.peControl |= 8;
            ++p_.counters.finishes;
            s_.updateLines();
        });
        p.onEvent(Ev::PeToken, [this](Ctx&, const Event& e) {
            s_.peTokenReg = static_cast<uint16_t>(e.a);
            if (e.b) { s_.peControl |= 4; ++p_.counters.tokens; s_.updateLines(); }
        });
    }
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2 || (off & 1) || off >= 0x20) stopHw(c, addr, "PE register not modelled (read)");
        if (off == 0x0A) return s_.peControl;
        if (off == 0x0E) return s_.peTokenReg;
        return reg_[off / 2];
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2 || (off & 1) || off >= 0x20 || off == 0x0E) stopHw(c, addr, "PE register not modelled (write)");
        if (off == 0x0A) {
            // bits 0-1 enables; bits 2-3 are status bits cleared by writing 1 (the token and finish handlers do exactly this)
            const uint16_t status = s_.peControl & 0xC;
            s_.peControl = static_cast<uint16_t>((v & 3) | (status & ~static_cast<uint16_t>(v & 0xC)));
            s_.updateLines();
            return;
        }
        reg_[off / 2] = static_cast<uint16_t>(v);
    }
private:
    Platform& p_;
    std::shared_ptr<GxShared> hold_;
    GxShared& s_;
    uint16_t reg_[0x10] = {};
};

// ---- Write-gather port -----------------------------------------------------------------------------------------
class GxFifo : public Device {
public:
    GxFifo(Platform&, std::shared_ptr<GxShared> sp) : hold_(sp), s_(*sp) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t) override { stopHw(c, addr, "GX write-gather port is write-only"); }
    void write(Ctx& c, uint32_t, uint32_t size, uint64_t v) override {
        for (uint32_t i = 0; i < size; ++i) {
            s_.staging[s_.staged++] = static_cast<uint8_t>(v >> (8 * (size - 1 - i)));
            if (s_.staged == 32) { s_.staged = 0; s_.flushChunk(c); }
        }
    }
private:
    std::shared_ptr<GxShared> hold_;
    GxShared& s_;
};

}  // namespace

void makeGx(Platform& p, std::unique_ptr<Device>& cp, std::unique_ptr<Device>& pe, std::unique_ptr<Device>& fifo) {
    auto shared = std::make_shared<GxShared>(p);
    cp = std::make_unique<Cp>(p, shared);
    pe = std::make_unique<Pe>(p, shared);
    fifo = std::make_unique<GxFifo>(p, shared);
}

}  // namespace awl::hle