// Shared helpers for the platform runtime: guest memory access without a CPU context, stop helpers and
// the boot-trace log. See docs/research/hle-platform.md.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "guest.h"

namespace awl::hle {

using rt::Ctx;
using rt::Machine;
using rt::StopKind;

// Guest time base: 40.5 MHz (bus clock 162 MHz / 4). One time-base tick is 4 retired instructions.
constexpr uint64_t kTickRate = 40500000;
constexpr uint64_t kInstructionsPerTick = 4;
// NTSC field: 40.5e6 * 1001 / 60000 ticks; one scan line is 858 pixel clocks of 13.5 MHz = 2574 ticks.
constexpr uint64_t kFieldTicks = 675675;
constexpr uint64_t kLineTicks = 2574;

constexpr uint32_t kMsrEE = 0x8000;

// Guest memory seen from the platform (no CPU context needed). Addresses may use either the cached
// (0x80000000) or the uncached (0xC0000000) view.
class GuestMem {
public:
    explicit GuestMem(uint8_t* base = nullptr) : base_(base) {}
    bool valid(uint32_t addr, uint32_t n) const {
        const uint32_t seg = addr >> 28;
        const uint32_t off = addr & 0x0FFFFFFFu;
        return base_ && (seg == 0x8 || seg == 0xC) && off <= rt::kMem1Size && n <= rt::kMem1Size - off;
    }
    uint8_t* ptr(uint32_t addr, uint32_t n) const { return valid(addr, n) ? base_ + (addr & 0x0FFFFFFFu) : nullptr; }
    uint32_t r32(uint32_t addr) const { const uint8_t* p = ptr(addr, 4); return p ? be32(p) : 0; }
    uint16_t r16(uint32_t addr) const { const uint8_t* p = ptr(addr, 2); return p ? static_cast<uint16_t>(p[0] << 8 | p[1]) : 0; }
    uint8_t r8(uint32_t addr) const { const uint8_t* p = ptr(addr, 1); return p ? *p : 0; }
    void w32(uint32_t addr, uint32_t v) const { if (uint8_t* p = ptr(addr, 4)) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); } }
    void w16(uint32_t addr, uint16_t v) const { if (uint8_t* p = ptr(addr, 2)) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); } }
    void w8(uint32_t addr, uint8_t v) const { if (uint8_t* p = ptr(addr, 1)) *p = v; }
    uint8_t* base() const { return base_; }
    static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

private:
    uint8_t* base_;
};

// Boot-trace log: every line is deterministic (no host time). Opened by the host; harmless when closed.
class TraceLog {
public:
    void open(const std::string& path);
    void close();
    void line(const char* fmt, ...);
    bool isOpen() const { return f_ != nullptr; }
    ~TraceLog() { close(); }
private:
    FILE* f_ = nullptr;
};
TraceLog& trace();

// Stops for unsupported hardware behaviour: always name the guest pc and the register or call.
[[noreturn]] inline void stopHw(Ctx& c, uint32_t addr, const char* what) { rt::stop(c, StopKind::Mmio, c.pc, addr, what); }
[[noreturn]] inline void stopPlatform(Ctx& c, uint32_t addr, const char* what) { rt::stop(c, StopKind::Platform, c.pc, addr, what); }

}  // namespace awl::hle
