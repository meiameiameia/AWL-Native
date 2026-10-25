// MMIO routing and the small register blocks: PI (interrupt controller), MI, DI.
#include "devices.h"

namespace awl::hle {

int blockOf(uint32_t addr) {
    if (addr < kMmioBase) return -1;
    const uint32_t off = addr - kMmioBase;
    if (off < 0x1000) return BlkCp;
    if (off < 0x2000) return BlkPe;
    if (off < 0x3000) return BlkVi;
    if (off < 0x4000) return BlkPi;
    if (off < 0x5000) return BlkMi;
    if (off < 0x6000) return BlkDsp;
    if (off < 0x6400) return BlkDi;
    if (off < 0x6800) return BlkSi;
    if (off < 0x6C00) return BlkExi;
    if (off < 0x7000) return BlkAi;
    if (off >= 0x8000 && off < 0x8008) return BlkGx;
    return -1;
}

namespace {

// ---- PI ------------------------------------------------------------------------------------------------
class Pi : public Device {
public:
    explicit Pi(Platform& p) : p_(p) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 4) stopHw(c, addr, "PI register accessed with a width other than 32 bits");
        switch (off) {
            case 0x00: return (p_.piStatus() & 0x3FFF) | pibit::ResetSwitchState;
            case 0x04: return p_.piMask();
            case 0x0C: return p_.piFifoBase;
            case 0x10: return p_.piFifoEnd;
            case 0x14: return p_.piFifoWp;
            case 0x24: return 0;   // reset register: cold boot, reset code 0
            default: stopHw(c, addr, "PI register not modelled (read)");
        }
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 4) stopHw(c, addr, "PI register accessed with a width other than 32 bits");
        switch (off) {
            case 0x00: p_.piLatched() &= ~static_cast<uint32_t>(v); break;   // write 1 to clear a latched cause
            case 0x04: p_.piMask() = static_cast<uint32_t>(v) & 0x3FFF; p_.lowerDeadline(p_.machine().icount + 1); break;
            case 0x0C: p_.piFifoBase = static_cast<uint32_t>(v); break;
            case 0x10: p_.piFifoEnd = static_cast<uint32_t>(v); break;
            case 0x14: p_.piFifoWp = static_cast<uint32_t>(v); break;
            case 0x18: break;      // CPU FIFO reset: no model state beyond the pointers
            case 0x24: stopHw(c, addr, "PI reset register written: the guest requested a reset");
            default: stopHw(c, addr, "PI register not modelled (write)");
        }
    }
private:
    Platform& p_;
};

// ---- MI ------------------------------------------------------------------------------------------------
// The memory interface only appears in __OSInitMemoryProtection and the interrupt mask/cause words; the
// protection hardware is not modelled and never raises an interrupt.
class Mi : public Device {
public:
    explicit Mi(Platform&) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2) stopHw(c, addr, "MI register accessed with a width other than 16 bits");
        switch (off) {
            case 0x1C: return mask_;
            case 0x1E: case 0x22: case 0x24: return 0;      // interrupt cause and address-error registers: nothing pending
            default: stopHw(c, addr, "MI register not modelled (read)");
        }
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (size != 2) stopHw(c, addr, "MI register accessed with a width other than 16 bits");
        switch (off) {
            case 0x10: case 0x20: case 0x28: break;          // protection setup, accepted
            case 0x1C: mask_ = static_cast<uint16_t>(v); break;
            default: stopHw(c, addr, "MI register not modelled (write)");
        }
    }
private:
    uint16_t mask_ = 0;
};

// ---- DI ------------------------------------------------------------------------------------------------
// The DVD command layer is replaced natively, so the drive interface only sees the two writes DVDInit makes.
class Di : public Device {
public:
    explicit Di(Platform&) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        // DI_CONFIG: read by EXISync through __OSGetDIConfig to detect a development-kit quirk (value 0xFF); any other value
        // selects the normal path. The retail value is unknown (Dolphin question 1 area), so report 0.
        if ((addr & 0xFFF) == 0x24 && size == 4) return 0;
        stopHw(c, addr, "DI register read: the DVD command layer is replaced natively");
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        const uint32_t off = addr & 0xFFF;
        if (size == 4 && ((off == 0x00 && v == 0x2A) || (off == 0x04 && v == 0))) return;
        stopHw(c, addr, "DI register write other than DVDInit's status/cover clear");
    }
};

// ---- SI ------------------------------------------------------------------------------------------------
// The SI library is not used (PAD and GBA are replaced natively), but the VI retrace handler still calls
// SIRefreshSamplingRate, which programs the polling register to match the video mode. Only that register is modelled.
class Si : public Device {
public:
    explicit Si(Platform&) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t size) override {
        if ((addr & 0x3FF) == 0x30 && size == 4) return poll_;
        stopHw(c, addr, "SI register: the SI library is not used (PAD and GBA are replaced natively)");
    }
    void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t v) override {
        if ((addr & 0x3FF) == 0x30 && size == 4) { poll_ = static_cast<uint32_t>(v); return; }
        stopHw(c, addr, "SI register: the SI library is not used (PAD and GBA are replaced natively)");
    }
private:
    uint32_t poll_ = 0;
};

}  // namespace

std::unique_ptr<Device> makePi(Platform& p) { return std::make_unique<Pi>(p); }
std::unique_ptr<Device> makeMi(Platform& p) { return std::make_unique<Mi>(p); }
std::unique_ptr<Device> makeDi(Platform& p) { return std::make_unique<Di>(p); }
std::unique_ptr<Device> makeSi(Platform& p) { return std::make_unique<Si>(p); }

}  // namespace awl::hle

namespace awl::hle {
namespace {

// Block whose model does not exist yet or is not used: every access stops and names the register.
class Unmodelled : public Device {
public:
    explicit Unmodelled(const char* what) : what_(what) {}
    uint64_t read(Ctx& c, uint32_t addr, uint32_t) override { stopHw(c, addr, what_); }
    void write(Ctx& c, uint32_t addr, uint32_t, uint64_t) override { stopHw(c, addr, what_); }
private:
    const char* what_;
};

}  // namespace

void createDevices(Platform& p, std::unique_ptr<Device> blocks[16]) {
    makeGx(p, blocks[BlkCp], blocks[BlkPe], blocks[BlkGx]);
    blocks[BlkVi] = makeVi(p);
    blocks[BlkPi] = makePi(p);
    blocks[BlkMi] = makeMi(p);
    makeAudio(p, blocks[BlkDsp], blocks[BlkAi]);
    blocks[BlkDi] = makeDi(p);
    blocks[BlkSi] = makeSi(p);
    blocks[BlkExi] = makeExi(p);
}

}  // namespace awl::hle
