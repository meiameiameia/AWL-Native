// External interface: three channels with the SDK's CSR/MAR/LENGTH/CR/DATA registers. Channel 0 chip select 1 is
// the RTC and SRAM device (read by __OSInitSram and written by UnlockSram); every other device is absent, so the
// EXT (card inserted) bit stays clear and the SDK reports "no card". Immediate transfers complete instantly.
#include <algorithm>

#include "devices.h"

namespace awl::hle {
namespace {

constexpr uint32_t kCsrExiMask = 1u << 0, kCsrExiInt = 1u << 1, kCsrTcMask = 1u << 2, kCsrTcInt = 1u << 3,
                   kCsrExtMask = 1u << 10, kCsrExtInt = 1u << 11, kCsrRomDis = 1u << 13;
constexpr uint32_t kCsrStored = 0x3FFF & ~(kCsrExiInt | kCsrTcInt | kCsrExtInt) & ~(1u << 12);

class RtcSram {
public:
    explicit RtcSram(uint32_t seconds) : seconds_(seconds) {
        // 64-byte SRAM block: checksums, counter bias, display offset, NTD, language, flags, flash IDs ...
        std::memset(sram_, 0, sizeof sram_);
        sram_[19] = 0x04;      // flags: stereo output, no progressive scan, video mode bits 0
        uint16_t sum1 = 0, sum2 = 0;
        for (int i = 12; i < 20; i += 2) {
            const uint16_t h = static_cast<uint16_t>(sram_[i] << 8 | sram_[i + 1]);
            sum1 = static_cast<uint16_t>(sum1 + h);
            sum2 = static_cast<uint16_t>(sum2 + static_cast<uint16_t>(~h));
        }
        sram_[0] = uint8_t(sum1 >> 8); sram_[1] = uint8_t(sum1); sram_[2] = uint8_t(sum2 >> 8); sram_[3] = uint8_t(sum2);
    }
    void select() { phase_ = 0; }
    void deselect() { phase_ = 0; }
    // len bytes, data left-aligned in the 32-bit word as the DATA register holds it. Returns read data.
    uint32_t imm(Ctx& c, uint32_t addr, bool write, uint32_t data, uint32_t len) {
        if (phase_ == 0) {
            if (!write || len != 4) stopHw(c, addr, "RTC/SRAM: first transfer after select must be a 4-byte command write");
            cmd_ = data;
            phase_ = 1;
            const uint32_t base = cmd_ & 0x7FFFFFFFu;
            isWrite_ = (cmd_ & 0x80000000u) != 0;
            if (base == 0x20000000u) { region_ = 1; pos_ = 0; }
            else if (base >= 0x20000100u && base < 0x20000100u + (64u << 6)) { region_ = 2; pos_ = (base - 0x20000100u) >> 6; }
            else stopHw(c, addr, "RTC/SRAM: unsupported command");
            return 0;
        }
        if (region_ == 1) {   // RTC counter
            if (isWrite_) stopHw(c, addr, "RTC counter write is not modelled");
            uint32_t v = seconds_;
            return len >= 4 ? v : (v & (0xFFFFFFFFu << (32 - 8 * len)));
        }
        uint32_t out = 0;
        for (uint32_t i = 0; i < len; ++i) {
            if (pos_ >= 64) stopHw(c, addr, "SRAM access beyond 64 bytes");
            if (isWrite_) sram_[pos_] = uint8_t(data >> (24 - 8 * i));
            else out |= uint32_t(sram_[pos_]) << (24 - 8 * i);
            ++pos_;
        }
        return out;
    }
    // DMA transfer of len bytes after the command phase: same byte stream as immediate transfers.
    void dma(Ctx& c, uint32_t addr, bool write, uint8_t* mem, uint32_t len) {
        uint32_t done = 0;
        while (done < len) {
            const uint32_t n = std::min<uint32_t>(4, len - done);
            uint32_t word = 0;
            if (write) for (uint32_t i = 0; i < n; ++i) word |= uint32_t(mem[done + i]) << (24 - 8 * i);
            const uint32_t out = imm(c, addr, write, word, n);
            if (!write) for (uint32_t i = 0; i < n; ++i) mem[done + i] = uint8_t(out >> (24 - 8 * i));
            done += n;
        }
    }
private:
    uint8_t sram_[64];
    uint32_t seconds_;
    uint32_t phase_ = 0, cmd_ = 0, pos_ = 0;
    int region_ = 0;
    bool isWrite_ = false;
};

class Exi : public Device {
public:
    explicit Exi(Platform& p) : p_(p), rtc_(p.config().rtcSeconds) {}

    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        uint32_t ch, reg;
        decode(c, addr, size, ch, reg);
        return ch_[ch].r[reg];
    }

    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        uint32_t ch, reg;
        decode(c, addr, size, ch, reg);
        Channel& k = ch_[ch];
        const uint32_t val = static_cast<uint32_t>(v);
        switch (reg) {
            case 0: {
                const uint32_t old = k.r[0];
                uint32_t n = (val & kCsrStored) | (old & (kCsrExiInt | kCsrTcInt | kCsrExtInt));
                n &= ~(val & (kCsrExiInt | kCsrTcInt | kCsrExtInt));
                k.r[0] = n;
                if (ch == 0) handleSelect(old, n);
                else if ((n >> 7 & 7) != 0 && (n >> 7 & 7) != (old >> 7 & 7)) stopHw(c, addr, "EXI channel select on a channel with no device");
                break;
            }
            case 1: case 2: k.r[reg] = val; break;
            case 3:
                k.r[3] = val & ~1u;
                if (val & 1) transfer(c, addr, ch, val);
                break;
            case 4: k.r[4] = val; break;
        }
        updateLine();
    }

private:
    struct Channel { uint32_t r[5] = {}; };

    void decode(Ctx& c, uint32_t addr, uint32_t size, uint32_t& ch, uint32_t& reg) {
        const uint32_t off = addr & 0x3FF;
        if (size != 4 || (off & 3) || off >= 3 * 0x14) stopHw(c, addr, "EXI register not modelled or accessed with a width other than 32 bits");
        ch = off / 0x14;
        reg = (off % 0x14) / 4;
    }

    void handleSelect(uint32_t oldCsr, uint32_t newCsr) {
        const uint32_t was = (oldCsr >> 7) & 7, now = (newCsr >> 7) & 7;
        if (was != now) {
            if (was == 2) rtc_.deselect();
            if (now == 2) rtc_.select();
        }
    }

    void transfer(Ctx& c, uint32_t addr, uint32_t ch, uint32_t cr) {
        Channel& k = ch_[ch];
        const uint32_t type = (cr >> 2) & 3;
        const uint32_t len = ((cr >> 4) & 3) + 1;
        const uint32_t cs = (k.r[0] >> 7) & 7;
        if (cr & 2) {
            const uint32_t dmaLen = k.r[2];
            uint8_t* mem = p_.mem().ptr(0x80000000u | (k.r[1] & 0x03FFFFFFu), dmaLen);
            if (!mem) stopHw(c, k.r[1], "EXI DMA addresses memory outside MEM1");
            if (type == 2) stopHw(c, addr, "EXI DMA read-write transfer is not modelled");
            if (ch == 0 && cs == 2) rtc_.dma(c, addr, type == 1, mem, dmaLen);
            else if (type == 0) std::memset(mem, 0, dmaLen);
        } else if (ch == 0 && cs == 2) {
            if (type == 2) stopHw(c, addr, "EXI read-write transfer to the RTC/SRAM device");
            const uint32_t out = rtc_.imm(c, addr, type == 1, k.r[4], len);
            if (type == 0) k.r[4] = out;
        } else if (type != 1) {
            k.r[4] = 0;     // absent device reads zero
        }
        k.r[0] |= kCsrTcInt;
    }

    void updateLine() {
        bool level = false;
        for (const Channel& k : ch_) {
            const uint32_t s = k.r[0];
            level |= ((s & kCsrExiInt) && (s & kCsrExiMask)) || ((s & kCsrTcInt) && (s & kCsrTcMask)) || ((s & kCsrExtInt) && (s & kCsrExtMask));
        }
        p_.setLine(pibit::Exi, level);
    }

    Platform& p_;
    RtcSram rtc_;
    Channel ch_[3];
};

}  // namespace

std::unique_ptr<Device> makeExi(Platform& p) { return std::make_unique<Exi>(p); }

}  // namespace awl::hle
