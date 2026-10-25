// Audio hardware: DSP control and mailboxes, ARAM and its DMA, the AI DMA engine and the AI control block. Nothing is
// played; every handshake the SDK waits on completes. The DSP is a protocol responder, not an emulation: it follows the
// boot ROM mail sequence and the task mails the SDK's DSP interrupt handler expects (docs/research/hle-platform.md,
// section 2.5). Register roles come from the SDK functions that touch them (AIInit, ARStartDMA, DSPInit, __DSPHandler, ...).
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

#include "devices.h"

namespace awl::hle {
namespace {

namespace dspctl {
constexpr uint16_t Reset = 1u << 0, Halt = 1u << 2, AiInt = 1u << 3, AiMask = 1u << 4, ArInt = 1u << 5, ArMask = 1u << 6,
                   DspInt = 1u << 7, DspMask = 1u << 8, DmaBusy = 1u << 9;
constexpr uint16_t W1C = AiInt | ArInt | DspInt;
}
namespace aictl {
constexpr uint32_t Play = 1u << 0, Rate48 = 1u << 1, IntMask = 1u << 2, IntStatus = 1u << 3, IntValid = 1u << 4, ScReset = 1u << 5, DspRate48 = 1u << 6;
}

constexpr uint32_t kAramSize = 16u * 1024 * 1024;
constexpr uint32_t kMailPending = 0x8000;

// DSP firmware stand-in. States follow the SDK's __DSP_boot_task and __DSPHandler (see the note).
class DspResponder {
public:
    enum class State { Reset, BootRom, Running };
    void reset() { state_ = State::Reset; bootWords_.clear(); haveBabe_ = false; }
    State state() const { return state_; }
    // Called when the DSP is released from reset/halt: the ROM announces itself.
    void start(std::deque<uint32_t>& toCpu) {
        state_ = State::BootRom;
        bootWords_.clear();
        toCpu.push_back(0x8071FEED);
    }
    // Returns nullptr when the mail is part of a modelled exchange, otherwise a static description of the gap.
    const char* onCpuMail(uint32_t mail, std::deque<uint32_t>& toCpu) {
        if (state_ == State::BootRom) {
            bootWords_.push_back(mail);
            // Five command/data pairs: iram source, iram destination, iram length, dram length, entry vector.
            if (bootWords_.size() == 10) {
                static const uint32_t cmds[5] = {0x80F3A001, 0x80F3C002, 0x80F3A002, 0x80F3B002, 0x80F3D001};
                for (int i = 0; i < 5; ++i) {
                    if (bootWords_[2 * i] != cmds[i]) return "DSP boot ROM: unexpected command mail in the upload sequence";
                }
                state_ = State::Running;
                toCpu.push_back(0xDCD10000);       // task INIT mail from the freshly started microcode
            }
            return nullptr;
        }
        // Running AX microcode: a frame is announced by a 0xBABExxxx mail followed by the command-list address; the
        // microcode processes it (nothing is mixed here) and answers with the RESUME mail (0xDCD10001), which the SDK's
        // DSP handler routes to the task's resume callback to signal "ready for the next frame".
        if (haveBabe_) {
            haveBabe_ = false;
            ++frames;
            toCpu.push_back(0xDCD10001);
            return nullptr;
        }
        if ((mail >> 16) == 0xBABE) { haveBabe_ = true; return nullptr; }
        return "DSP microcode protocol: CPU mail with no modelled response";
    }
private:
    State state_ = State::Reset;
    bool haveBabe_ = false;
    std::vector<uint32_t> bootWords_;
public:
    uint64_t frames = 0;
};

struct Audio {
    explicit Audio(Platform& p) : p(p), aram(kAramSize, 0) {}
    Platform& p;
    std::vector<uint8_t> aram;
    // DSP block
    uint16_t control = dspctl::Halt;
    uint32_t mailToDsp = 0;
    bool mailToDspHiSet = false;
    std::deque<uint32_t> toCpu;           // mails waiting for the CPU
    bool cpuMailValid = false;
    uint32_t cpuMail = 0;
    DspResponder dsp;
    uint16_t arSize = 0, arRefresh = 0;
    uint16_t arMm[2] = {}, arAddr[2] = {}, arCnt[2] = {};
    uint16_t aiDmaStart[2] = {}, aiDmaCtl = 0;
    uint64_t aiDmaGen = 0, aramGen = 0;
    // AI block
    uint32_t aiControl = 0, aiVolume = 0, aiIt = 0;
    uint64_t aiScOrigin = 0, aiItGen = 0;
    uint64_t mailCount = 0;

    void updateLines() {
        using namespace dspctl;
        const bool dspLine = ((control & AiInt) && (control & AiMask)) || ((control & ArInt) && (control & ArMask)) || ((control & DspInt) && (control & DspMask));
        p.setLine(pibit::Dsp, dspLine);
        p.setLine(pibit::Ai, (aiControl & aictl::IntStatus) && (aiControl & aictl::IntMask));
    }

    void deliverNextMail() {
        if (cpuMailValid || toCpu.empty()) return;
        cpuMail = toCpu.front();
        toCpu.pop_front();
        cpuMailValid = true;
        control |= dspctl::DspInt;
        ++mailCount;
        trace().line("dsp mail ->cpu %08X t=%llu", cpuMail, static_cast<unsigned long long>(p.now()));
        updateLines();
    }

    uint32_t aiRate() const { return (aiControl & aictl::Rate48) ? 48000 : 32000; }
    uint32_t dmaRate() const { return (aiControl & aictl::DspRate48) ? 48000 : 32000; }

    void scheduleAiDma() {
        ++aiDmaGen;
        const uint32_t blocks = aiDmaCtl & 0x7FFF;
        if (!(aiDmaCtl & 0x8000) || blocks == 0) return;
        // One 32-byte block holds 8 stereo 16-bit samples.
        const uint64_t ticks = static_cast<uint64_t>(blocks) * 8 * kTickRate / dmaRate();
        p.postAfter(ticks, Ev::AiDmaDone, aiDmaGen);
    }

    void scheduleAiSc() {
        ++aiItGen;
        if (!(aiControl & aictl::IntValid)) return;
        const uint64_t fire = aiScOrigin + static_cast<uint64_t>(aiIt) * kTickRate / aiRate();
        if (fire > p.now()) p.post(fire, Ev::AiSampleCounter, aiItGen);
    }

    void aramDma(Ctx& c) {
        const uint32_t mm = (static_cast<uint32_t>(arMm[0]) << 16 | arMm[1]) & 0x03FFFFFF;
        const uint32_t ar = static_cast<uint32_t>(arAddr[0]) << 16 | arAddr[1];
        const uint32_t cnt = static_cast<uint32_t>(arCnt[0] & 0x7FFF) << 16 | arCnt[1];
        const bool toMem = (arCnt[0] & 0x8000) != 0;
        uint8_t* ram = p.mem().ptr(0x80000000u | mm, cnt);
        if (!ram) stopHw(c, mm, "ARAM DMA addresses memory outside MEM1");
        if (cnt) {
            // ARAM is 16 MB; accesses beyond it read zero and are dropped (no expansion memory attached).
            for (uint32_t i = 0; i < cnt;) {
                const uint32_t a = ar + i;
                const uint32_t chunk = a < kAramSize ? std::min<uint32_t>(cnt - i, kAramSize - a) : cnt - i;
                if (a < kAramSize) {
                    if (toMem) std::memcpy(ram + i, &aram[a], chunk); else std::memcpy(&aram[a], ram + i, chunk);
                } else if (toMem) {
                    std::memset(ram + i, 0, chunk);
                }
                i += chunk;
            }
        }
        control |= dspctl::DmaBusy;
        ++aramGen;
        trace().line("aram dma %s mem=%08X aram=%08X len=%u t=%llu", toMem ? "aram->mem" : "mem->aram", mm, ar, cnt, static_cast<unsigned long long>(p.now()));
        p.postAfter(500 + cnt / 4, Ev::AramDmaDone, aramGen);
    }
};

class Dsp : public Device {
public:
    explicit Dsp(std::shared_ptr<Audio> a) : a_(a), s_(*a) {
        Platform& p = s_.p;
        p.onEvent(Ev::AiDmaDone, [this](Ctx&, const Event& e) {
            if (e.a != s_.aiDmaGen) return;
            s_.control |= dspctl::AiInt;
            s_.updateLines();
            s_.scheduleAiDma();           // the engine replays the same buffer until software reprograms it
        });
        p.onEvent(Ev::AramDmaDone, [this](Ctx&, const Event& e) {
            if (e.a != s_.aramGen) return;
            s_.control = static_cast<uint16_t>((s_.control & ~dspctl::DmaBusy) | dspctl::ArInt);
            s_.updateLines();
        });
        p.onEvent(Ev::DspMail, [this](Ctx&, const Event&) { s_.deliverNextMail(); });
    }
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2 || (off & 1)) stopHw(c, addr, "DSP register accessed with a width other than 16 bits");
        switch (off) {
            case 0x00: return static_cast<uint16_t>((s_.mailToDsp >> 16) & 0x7FFF);       // never left pending: the DSP takes mails at once
            case 0x02: return static_cast<uint16_t>(s_.mailToDsp);
            case 0x04: return static_cast<uint16_t>(((s_.cpuMail >> 16) & 0x7FFF) | (s_.cpuMailValid ? kMailPending : 0));
            case 0x06: {
                const uint16_t lo = static_cast<uint16_t>(s_.cpuMail);
                if (s_.cpuMailValid) {            // reading the low half consumes the mail
                    s_.cpuMailValid = false;
                    s_.p.postAfter(100, Ev::DspMail);
                }
                return lo;
            }
            case 0x0A: return s_.control;
            case 0x12: return s_.arSize;
            case 0x16: return 1;                  // ARAM controller ready
            case 0x1A: return s_.arRefresh;
            case 0x20: return s_.arMm[0]; case 0x22: return s_.arMm[1];
            case 0x24: return s_.arAddr[0]; case 0x26: return s_.arAddr[1];
            case 0x28: return s_.arCnt[0]; case 0x2A: return s_.arCnt[1];
            case 0x30: return s_.aiDmaStart[0]; case 0x32: return s_.aiDmaStart[1];
            case 0x36: return s_.aiDmaCtl;
            default: stopHw(c, addr, "DSP register not modelled (read)");
        }
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2 || (off & 1)) stopHw(c, addr, "DSP register accessed with a width other than 16 bits");
        const uint16_t h = static_cast<uint16_t>(v);
        switch (off) {
            case 0x00: s_.mailToDsp = (s_.mailToDsp & 0xFFFF) | (uint32_t(h) << 16); break;
            case 0x02: {
                s_.mailToDsp = (s_.mailToDsp & 0xFFFF0000u) | h;
                trace().line("dsp mail ->dsp %08X t=%llu", s_.mailToDsp, static_cast<unsigned long long>(s_.p.now()));
                if (const char* gap = s_.dsp.onCpuMail(s_.mailToDsp, s_.toCpu)) stopHw(c, s_.mailToDsp, gap);
                s_.p.postAfter(s_.dsp.state() == DspResponder::State::Running ? 20000 : 100, Ev::DspMail);
                break;
            }
            case 0x0A: control(c, addr, h); break;
            case 0x12: s_.arSize = h; break;
            case 0x1A: s_.arRefresh = h; break;
            case 0x20: s_.arMm[0] = h; break; case 0x22: s_.arMm[1] = h; break;
            case 0x24: s_.arAddr[0] = h; break; case 0x26: s_.arAddr[1] = h; break;
            case 0x28: s_.arCnt[0] = h; break;
            case 0x2A: s_.arCnt[1] = h; s_.aramDma(c); break;
            case 0x30: s_.aiDmaStart[0] = h; break; case 0x32: s_.aiDmaStart[1] = h; break;
            case 0x36: s_.aiDmaCtl = h; s_.scheduleAiDma(); break;
            default: stopHw(c, addr, "DSP register not modelled (write)");
        }
    }
private:
    void control(Ctx& c, uint32_t addr, uint16_t h) {
        using namespace dspctl;
        const uint16_t old = s_.control;
        (void)c; (void)addr;
        uint16_t n = static_cast<uint16_t>(h & ~(W1C | Reset));       // control bits come from the written value
        n = static_cast<uint16_t>(n | (old & W1C & ~h));            // status bits stay until a 1 is written to them
        n = static_cast<uint16_t>((n & ~DmaBusy) | (old & DmaBusy)); // busy is read-only
        s_.control = n;
        if ((h & Reset) && (old & Halt) == 0) { s_.dsp.reset(); s_.toCpu.clear(); s_.cpuMailValid = false; }
        const bool released = !(n & Halt) && ((old & Halt) || (h & Reset));
        if (released && s_.dsp.state() != DspResponder::State::BootRom) {
            s_.dsp.start(s_.toCpu);
            s_.p.postAfter(500, Ev::DspMail);
        }
        s_.updateLines();
    }

    std::shared_ptr<Audio> a_;
    Audio& s_;
};

class Ai : public Device {
public:
    explicit Ai(std::shared_ptr<Audio> a) : a_(a), s_(*a) {
        s_.p.onEvent(Ev::AiSampleCounter, [this](Ctx&, const Event& e) {
            if (e.a != s_.aiItGen) return;
            s_.aiControl |= aictl::IntStatus;
            s_.updateLines();
        });
    }
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0x3FF;
        if (size != 4 || (off & 3)) stopHw(c, addr, "AI register accessed with a width other than 32 bits");
        switch (off) {
            case 0x00: return s_.aiControl;
            case 0x04: return s_.aiVolume;
            case 0x08: return static_cast<uint32_t>((s_.p.now() - s_.aiScOrigin) * s_.aiRate() / kTickRate);
            case 0x0C: return s_.aiIt;
            default: stopHw(c, addr, "AI register not modelled (read)");
        }
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0x3FF;
        if (size != 4 || (off & 3)) stopHw(c, addr, "AI register accessed with a width other than 32 bits");
        const uint32_t w = static_cast<uint32_t>(v);
        switch (off) {
            case 0x00: {
                const uint32_t old = s_.aiControl;
                uint32_t n = (w & ~(aictl::IntStatus | aictl::ScReset)) | (old & aictl::IntStatus & ~w);
                s_.aiControl = n;
                if (w & aictl::ScReset) { s_.aiScOrigin = s_.p.now(); }
                s_.scheduleAiSc();
                s_.updateLines();
                break;
            }
            case 0x04: s_.aiVolume = w; break;
            case 0x0C: s_.aiIt = w; s_.scheduleAiSc(); break;
            default: stopHw(c, addr, "AI register not modelled (write)");
        }
    }
private:
    std::shared_ptr<Audio> a_;
    Audio& s_;
};

}  // namespace

void makeAudio(Platform& p, std::unique_ptr<Device>& dsp, std::unique_ptr<Device>& ai) {
    auto audio = std::make_shared<Audio>(p);
    dsp = std::make_unique<Dsp>(audio);
    ai = std::make_unique<Ai>(audio);
}

}  // namespace awl::hle
