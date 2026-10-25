// Video interface: register file, the four display interrupts, beam position and framebuffer latch events.
// Timing: a field lasts 675,675 time-base ticks (59.94 Hz) and a line 2,574 ticks; field n starts at tick
// n * 675,675. Register roles follow the SDK's VIInit/VIFlush/retrace ISR (docs/research/hle-platform.md).
#include "devices.h"

namespace awl::hle {
namespace {

constexpr uint32_t kRegs = 0x40;                 // halfword registers 0x2000-0x207F
constexpr uint32_t kDi0 = 0x30, kDiBytes = 4;    // display interrupt registers at 0x30, 0x34, 0x38, 0x3C
constexpr uint32_t kPosV = 0x2C, kPosH = 0x2E;   // current beam position

class Vi : public Device {
public:
    explicit Vi(Platform& p) : p_(p) {
        reg_[0x6C / 2] = bootValues().viClock;     // clock select: left by the IPL, read by SISetSamplingRate
        p.onEvent(Ev::ViDisplayInt, [this](Ctx&, const Event& e) { fire(static_cast<uint32_t>(e.a), e.b); });
    }

    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0xFFF;
        if (off >= 2 * kRegs) stopHw(c, addr, "VI register not modelled (read)");
        if (size == 2 && (off & 1) == 0) return half(off);
        if (size == 4 && (off & 3) == 0) return (static_cast<uint32_t>(half(off)) << 16) | half(off + 2);
        stopHw(c, addr, "VI register accessed with an unsupported width");
    }

    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (off >= 2 * kRegs) stopHw(c, addr, "VI register not modelled (write)");
        if (size == 2 && (off & 1) == 0) { setHalf(c, addr, off, static_cast<uint16_t>(v)); return; }
        if (size == 4 && (off & 3) == 0) {
            setHalf(c, addr, off, static_cast<uint16_t>(v >> 16));
            setHalf(c, addr, off + 2, static_cast<uint16_t>(v));
            return;
        }
        stopHw(c, addr, "VI register accessed with an unsupported width");
    }

private:
    uint16_t half(uint32_t off) const {
        if (off == kPosV || off == kPosH) {
            const uint64_t phase = p_.now() % kFieldTicks;
            return static_cast<uint16_t>(off == kPosV ? phase / kLineTicks + 1 : (phase % kLineTicks) / 3 + 1);
        }
        return reg_[off / 2];
    }

    static bool isDi(uint32_t off) { return off >= kDi0 && off < kDi0 + 4 * kDiBytes; }
    static bool isFramebuffer(uint32_t off) { return off >= 0x1C && off < 0x2C; }

    void setHalf(Ctx& c, uint32_t addr, uint32_t off, uint16_t v) {
        if (off == kPosV || off == kPosH) stopHw(c, addr, "VI beam position registers are read-only");
        if (isDi(off) && ((off - kDi0) & 2) == 0) {
            // High half: bit 15 is the interrupt status (writing 0 clears it, writing 1 leaves it), bit 12 the
            // enable and bits 9-0 the vertical position. Taken from the retrace ISR's read-modify-write.
            const uint32_t idx = (off - kDi0) / kDiBytes;
            const uint16_t old = reg_[off / 2];
            reg_[off / 2] = static_cast<uint16_t>((v & 0x7FFF) | (old & v & 0x8000));
            schedule(idx);
            updateLine();
            return;
        }
        if (isDi(off)) { reg_[off / 2] = v; schedule((off - kDi0) / kDiBytes); return; }
        if (isFramebuffer(off)) {
            if (reg_[off / 2] != v) {
                reg_[off / 2] = v;
                // The four framebuffer words of one VIFlush are written within a few hundred ticks: one latch event.
                const uint64_t now = p_.now();
                if (!p_.counters.haveLatch || now - p_.counters.lastLatchTick > 20000) {
                    ++p_.counters.fbLatches;
                    p_.counters.lastLatchHadCopy = p_.counters.copySinceLatch;     // an EFB copy produced the frame this latch shows
                    p_.counters.copySinceLatch = false;
                    p_.counters.sinceLatch = 0;
                    p_.counters.haveLatch = true;
                }
                p_.counters.lastLatchTick = now;
                trace().line("vi fb reg %04X=%04X t=%llu", 0x2000 + off, v, static_cast<unsigned long long>(p_.now()));
            }
            return;
        }
        reg_[off / 2] = v;
    }

    void schedule(uint32_t idx) {
        const uint16_t hi = reg_[(kDi0 + idx * kDiBytes) / 2], lo = reg_[(kDi0 + idx * kDiBytes + 2) / 2];
        if (!(hi & 0x1000)) { ++gen_[idx]; return; }
        const uint32_t vct = hi & 0x3FF, hct = lo & 0x7FF;
        const uint64_t offset = (vct ? vct - 1 : 0) * kLineTicks + (hct ? hct - 1 : 0) * 3ull;
        const uint64_t now = p_.now();
        uint64_t t = (now / kFieldTicks) * kFieldTicks + offset;
        if (t <= now) t += kFieldTicks;
        ++gen_[idx];
        p_.post(t, Ev::ViDisplayInt, idx, gen_[idx]);
    }

    void fire(uint32_t idx, uint64_t gen) {
        if (gen != gen_[idx]) return;
        uint16_t& hi = reg_[(kDi0 + idx * kDiBytes) / 2];
        if (!(hi & 0x1000)) return;
        hi |= 0x8000;
        updateLine();
        const uint64_t field = p_.now() / kFieldTicks;
        if (field != lastField_ || !haveField_) { haveField_ = true; lastField_ = field; p_.onRetrace(); }
        schedule(idx);
    }

    void updateLine() {
        bool level = false;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint16_t hi = reg_[(kDi0 + i * kDiBytes) / 2];
            level |= (hi & 0x8000) && (hi & 0x1000);
        }
        p_.setLine(pibit::Vi, level);
    }

    Platform& p_;
    uint16_t reg_[kRegs] = {};
    uint64_t gen_[4] = {};
    uint64_t lastField_ = 0;
    bool haveField_ = false;
};

}  // namespace

std::unique_ptr<Device> makeVi(Platform& p) { return std::make_unique<Vi>(p); }

}  // namespace awl::hle
