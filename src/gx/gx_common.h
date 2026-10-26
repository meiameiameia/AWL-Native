// Types shared by the GX core and its vertex engine: errors, guest memory, CP/XF register state.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace awl::gx {

class GxError : public std::runtime_error {
public:
    GxError(const std::string& what, uint64_t offset, uint32_t code) : std::runtime_error(what), streamOffset(offset), code(code) {}
    uint64_t streamOffset;   // bytes of command stream consumed before the failing command (display-list bytes not counted)
    uint32_t code;           // opcode, register number or address that failed
};

// Guest memory by physical address (0 .. 0x01FFFFFF is MEM1). Returns null when [addr, addr+size) is not readable.
class GuestMemory {
public:
    virtual ~GuestMemory() = default;
    virtual const uint8_t* read(uint32_t physAddr, uint32_t size) const = 0;
};

// ---- Register state (inspectable for tests and tools) -------------------------------------------------------------
struct CpState {
    uint32_t vcdLo = 0, vcdHi = 0;
    uint32_t vat[3][8] = {};              // VAT A, B, C of the 8 vertex formats
    uint32_t arrayBase[16] = {};          // 0xA0-0xAF: pos, nrm, col0, col1, tex0-7, XF index arrays A-D (physical)
    uint32_t arrayStride[16] = {};
    uint32_t matIdxA = 0, matIdxB = 0;    // CP 0x30 / 0x40 (mirrors of XF 0x1018 / 0x1019)
};
struct XfState {
    uint32_t mem[0x800] = {};             // 0x000-0x0FF position/texture matrices, 0x400 normal matrices, 0x500 post matrices, 0x600 lights
    uint32_t reg[0x60] = {};              // XF registers 0x1000-0x105F
};

}  // namespace awl::gx
