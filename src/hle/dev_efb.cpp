#include "dev_efb.h"

#include <set>

namespace awl::hle {
namespace {

class DefaultEfb : public EfbAccess {
public:
    uint32_t peek(rt::Ctx& c, EfbKind kind, uint32_t x, uint32_t y) override {
        if (kind != EfbKind::Depth) stopHw(c, kEfbWindowBase, "EFB colour peek: no framebuffer behind the default EFB access (needs the renderer)");
        if (seen_.insert(c.pc).second)
            trace().line("stub efb peek z pc=%08X x=%u y=%u -> 00FFFFFF (far plane, no framebuffer yet)", c.pc, x, y);
        return 0x00FFFFFFu;
    }
    void poke(rt::Ctx& c, EfbKind, uint32_t, uint32_t, uint32_t) override {
        stopHw(c, kEfbWindowBase, "EFB poke: no framebuffer behind the default EFB access (needs the renderer)");
    }
private:
    std::set<uint32_t> seen_;
};

DefaultEfb g_default;
EfbAccess* g_provider = &g_default;

}  // namespace

EfbAddress decodeEfbAddress(uint32_t addr) {
    EfbAddress a;
    a.x = (addr >> 2) & 0x3FF;
    a.y = (addr >> 12) & 0x3FF;
    a.type = (addr >> 22) & 3;
    a.kind = a.type == 1 ? EfbKind::Depth : EfbKind::Color;
    return a;
}

void setEfbAccess(EfbAccess* provider) { g_provider = provider ? provider : &g_default; }

uint64_t efbRead(rt::Ctx& c, uint32_t addr, uint32_t size) {
    const EfbAddress a = decodeEfbAddress(addr);
    if (size != 4 || (addr & 3)) stopHw(c, addr, "EFB peek with a width other than an aligned 32 bits");
    if (a.type > 1) stopHw(c, addr, "EFB access type is not colour or depth (not modelled)");
    return g_provider->peek(c, a.kind, a.x, a.y);
}

void efbWrite(rt::Ctx& c, uint32_t addr, uint32_t size, uint64_t value) {
    const EfbAddress a = decodeEfbAddress(addr);
    if (size != 4 || (addr & 3)) stopHw(c, addr, "EFB poke with a width other than an aligned 32 bits");
    if (a.type > 1) stopHw(c, addr, "EFB access type is not colour or depth (not modelled)");
    g_provider->poke(c, a.kind, a.x, a.y, static_cast<uint32_t>(value));
}

}  // namespace awl::hle
