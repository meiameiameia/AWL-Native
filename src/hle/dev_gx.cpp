// GX in P1: the write-gather port, the command-processor / pixel-engine registers the SDK uses, and a command-stream
// parser that keeps the stream in sync (vertex sizes come from the CP vertex descriptor and attribute formats) so the
// services the SDK waits on can be completed: draw-done (finish), draw-sync tokens and EFB copies. Nothing is drawn.
// Register roles and command formats: public GameCube documentation and what GXInit/GXSetDrawSync/GXPEInit do in the DOL
// (docs/research/hle-platform.md, sections 2.7 and 5.5).
#include <memory>
#include <vector>

#include "devices.h"

namespace awl::hle {
namespace {

constexpr uint64_t kFinishLatency = 2000;   // ticks between the BP finish command reaching the GP and the interrupt
constexpr uint64_t kTokenLatency = 2000;

// ---- Parser ---------------------------------------------------------------------------------------------------
class GxParser {
public:
    explicit GxParser(Platform& p) : p_(p) {}

    void feed(Ctx& c, const uint8_t* data, size_t n) {
        pending_.insert(pending_.end(), data, data + n);
        size_t used = parse(c, pending_.data(), pending_.size(), 0);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    }
    void reset() { pending_.clear(); }

    uint64_t commands = 0, draws = 0, bpWrites = 0;

private:
    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

    // Parses as many whole commands as the buffer holds; returns the bytes consumed.
    size_t parse(Ctx& c, const uint8_t* d, size_t n, int depth) {
        size_t i = 0;
        while (i < n) {
            const uint8_t op = d[i];
            size_t need = 1;
            if (op == 0x00 || op == 0x48) {
                // NOP / invalidate vertex cache
            } else if (op == 0x08) {
                need = 6;
                if (n - i < need) break;
                cpLoad(d[i + 1], be32(d + i + 2));
            } else if (op == 0x10) {
                if (n - i < 5) break;
                const uint32_t count = (be32(d + i + 1) >> 16) + 1;
                need = 5 + size_t(count) * 4;
                if (n - i < need) break;
            } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
                need = 5;
                if (n - i < need) break;
            } else if (op == 0x40) {
                need = 9;
                if (n - i < need) break;
                callList(c, be32(d + i + 1), be32(d + i + 5), depth);
            } else if (op == 0x61) {
                need = 5;
                if (n - i < need) break;
                bpLoad(be32(d + i + 1));
            } else if ((op & 0xC0) == 0x80) {
                if (n - i < 3) break;
                const uint32_t count = uint32_t(d[i + 1]) << 8 | d[i + 2];
                need = 3 + size_t(count) * vertexSize(c, op & 7);
                if (n - i < need) break;
                ++draws;
            } else {
                stopHw(c, op, "GX FIFO: unknown command opcode (the stream is out of sync or the opcode is not modelled)");
            }
            i += need;
            ++commands;
        }
        return i;
    }

    void callList(Ctx& c, uint32_t addr, uint32_t size, int depth) {
        if (depth >= 4) stopHw(c, addr, "GX display list nested deeper than 4");
        const uint8_t* dl = p_.mem().ptr(0x80000000u | (addr & 0x01FFFFFFu), size);
        if (!dl) stopHw(c, addr, "GX display list outside MEM1");
        const size_t used = parse(c, dl, size, depth + 1);
        if (used != size) stopHw(c, addr, "GX display list ends inside a command");
    }

    void cpLoad(uint8_t reg, uint32_t value) {
        if (reg == 0x50) vcdLo_ = value;
        else if (reg == 0x60) vcdHi_ = value;
        else if (reg >= 0x70 && reg < 0x78) vatA_[reg - 0x70] = value;
        else if (reg >= 0x80 && reg < 0x88) vatB_[reg - 0x80] = value;
        else if (reg >= 0x90 && reg < 0x98) vatC_[reg - 0x90] = value;
    }

    void bpLoad(uint32_t word);

    static uint32_t compSize(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
    static uint32_t colorSize(uint32_t fmt) { return fmt == 0 || fmt == 3 ? 2 : fmt == 1 || fmt == 4 ? 3 : 4; }
    static uint32_t bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }

    uint32_t vertexSize(Ctx& c, uint32_t vat) {
        const uint32_t a = vatA_[vat], b = vatB_[vat], cc = vatC_[vat];
        uint32_t size = 0;
        if (vcdLo_ & 1) size += 1;
        for (uint32_t t = 0; t < 8; ++t) if (vcdLo_ & (2u << t)) size += 1;
        auto attr = [&](uint32_t mode, uint32_t direct) -> uint32_t {
            switch (mode) { case 0: return 0; case 1: return direct; case 2: return 1; default: return 2; }
        };
        size += attr(bits(vcdLo_, 9, 2), (bits(a, 0, 1) ? 3 : 2) * compSize(bits(a, 1, 3)));
        const uint32_t nrmMode = bits(vcdLo_, 11, 2);
        const uint32_t nrmComps = bits(a, 9, 1) ? 9 : 3;
        if (nrmMode == 1) size += nrmComps * (bits(a, 10, 3) == 1 ? 1 : bits(a, 10, 3) == 3 ? 2 : 4);
        else if (nrmMode >= 2) size += (nrmMode == 2 ? 1 : 2) * ((nrmComps == 9 && bits(a, 31, 1)) ? 3 : 1);
        size += attr(bits(vcdLo_, 13, 2), colorSize(bits(a, 14, 3)));
        size += attr(bits(vcdLo_, 15, 2), colorSize(bits(a, 18, 3)));
        for (uint32_t t = 0; t < 8; ++t) {
            const uint32_t mode = bits(vcdHi_, 2 * t, 2);
            if (!mode) continue;
            uint32_t cnt, fmt;
            switch (t) {
                case 0: cnt = bits(a, 21, 1); fmt = bits(a, 22, 3); break;
                case 1: cnt = bits(b, 0, 1); fmt = bits(b, 1, 3); break;
                case 2: cnt = bits(b, 9, 1); fmt = bits(b, 10, 3); break;
                case 3: cnt = bits(b, 18, 1); fmt = bits(b, 19, 3); break;
                case 4: cnt = bits(b, 27, 1); fmt = bits(b, 28, 3); break;
                case 5: cnt = bits(cc, 5, 1); fmt = bits(cc, 6, 3); break;
                case 6: cnt = bits(cc, 14, 1); fmt = bits(cc, 15, 3); break;
                default: cnt = bits(cc, 23, 1); fmt = bits(cc, 24, 3); break;
            }
            size += attr(mode, (cnt ? 2 : 1) * compSize(fmt));
        }
        if (size == 0) stopHw(c, vat, "GX draw with an empty vertex descriptor");
        return size;
    }

    Platform& p_;
    std::vector<uint8_t> pending_;
    uint32_t vcdLo_ = 0, vcdHi_ = 0;
    uint32_t vatA_[8] = {}, vatB_[8] = {}, vatC_[8] = {};
};

// ---- Shared GX state -------------------------------------------------------------------------------------------
struct GxShared {
    explicit GxShared(Platform& p) : parser(p), p(p) {}
    GxParser parser;
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
        parser.feed(c, staging, 32);
    }
};

void GxParser::bpLoad(uint32_t word) {
    ++bpWrites;
    const uint32_t reg = word >> 24;
    switch (reg) {
        case 0x45: p_.post(p_.now() + kFinishLatency, Ev::PeFinish); break;
        case 0x47: p_.post(p_.now(), Ev::PeToken, word & 0xFFFF, 0); break;                // token value only
        case 0x48: p_.post(p_.now() + kTokenLatency, Ev::PeToken, word & 0xFFFF, 1); break; // token with interrupt
        case 0x52: ++p_.counters.efbCopies; p_.counters.copySinceLatch = true; break;
        default: break;
    }
}

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
