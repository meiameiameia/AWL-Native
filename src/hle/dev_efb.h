// EFB access window 0xC8000000-0xC8FFFFFF: CPU peeks and pokes of the embedded framebuffer (GXPeekARGB, GXPeekZ,
// GXPokeARGB, GXPokeZ). The address carries the pixel: bits 2-11 = x, bits 12-21 = y, bits 22-23 = access type (0 colour,
// 1 depth), layout as libogc's GX_PeekARGB / GX_PeekZ build it and as the recompiled GXPeekZ at 0x801CE024 does.
// The renderer supplies the real framebuffer through EfbAccess; until then a logged default answers depth peeks.
#pragma once

#include <cstdint>

#include "hle_common.h"

namespace awl::hle {

enum class EfbKind : uint32_t { Color = 0, Depth = 1 };

struct EfbAddress {
    EfbKind kind;
    uint32_t type;    // raw access type bits (0 colour, 1 depth; 2 and 3 are not modelled)
    uint32_t x, y;    // EFB pixel
};
constexpr uint32_t kEfbWindowBase = 0xC8000000u, kEfbWindowEnd = 0xC9000000u;
inline bool inEfbWindow(uint32_t addr) { return addr >= kEfbWindowBase && addr < kEfbWindowEnd; }
EfbAddress decodeEfbAddress(uint32_t addr);

// Implemented by the renderer. Values are guest numeric order: colour = 0xAARRGGBB as GXPeekARGB returns it (alpha
// first), depth = the 24-bit Z value (0x00FFFFFF is the far plane). Called on the guest thread.
class EfbAccess {
public:
    virtual ~EfbAccess() = default;
    virtual uint32_t peek(rt::Ctx& c, EfbKind kind, uint32_t x, uint32_t y) = 0;
    virtual void poke(rt::Ctx& c, EfbKind kind, uint32_t x, uint32_t y, uint32_t value) = 0;
};

// Installs the provider (not owned); null restores the default. The default (stub register: EFB access) returns 0x00FFFFFF for
// depth peeks and logs the first use at each call site with the coordinates; colour peeks and every poke stop loudly.
void setEfbAccess(EfbAccess* provider);

uint64_t efbRead(rt::Ctx& c, uint32_t addr, uint32_t size);
void efbWrite(rt::Ctx& c, uint32_t addr, uint32_t size, uint64_t value);

}  // namespace awl::hle
