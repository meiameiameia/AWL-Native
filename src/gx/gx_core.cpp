// GX core: command stream parsing and CP/XF/BP register state. Command and register formats: YAGCD chapter 5, libogc gx.c.
#include "gx/gx_core.h"

#include <cstdio>
#include <cstring>

namespace awl::gx {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string hex(const char* what, uint32_t v) {
    char b[96];
    std::snprintf(b, sizeof b, "%s 0x%X", what, v);
    return b;
}

// BP registers the core accepts; anything else is an undocumented register and stops the run (docs/research/gx-core.md).
bool knownBp(uint32_t r) {
    return r <= 0x04 || (r >= 0x06 && r <= 0x4B) || (r >= 0x4D && r <= 0x59) || (r >= 0x60 && r <= 0x69) || (r >= 0x80 && r <= 0xBB && (r & 0x1F) < 0x1C) ||
           (r >= 0xC0 && r <= 0xFE);
}
// BP writes that cannot change the draw pipeline state.
bool stateNeutral(uint32_t r) {
    return (r >= 0x01 && r <= 0x04) || r == 0x23 || r == 0x24 || (r >= 0x44 && r <= 0x58) || r == 0x67 || r == 0x68 || r == 0x69;
}

}  // namespace

Core::Core(const GuestMemory& mem, Backend& backend, CoreHost& host) : mem_(mem), backend_(backend), host_(host) {
    tlutRam_.assign(1024 * 256, 0);
}

void Core::fail(const std::string& what, uint32_t code) const { throw GxError(what, offset_, code); }

void Core::feed(const uint8_t* data, size_t n) {
    const auto t0 = profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    if (retrace_ != frameSeen_) {
        frameSeen_ = retrace_;
        backend_.frameBoundary(retrace_);
    }
    pending_.insert(pending_.end(), data, data + n);
    const size_t used = parse(pending_.data(), pending_.size(), 0);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    if (profile_) stats_.busySeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

size_t Core::parse(const uint8_t* d, size_t n, int depth) {
    size_t i = 0;
    while (i < n) {
        const uint8_t op = d[i];
        size_t need = 1;
        if (depth == 0) offset_ = streamBase_ + i;
        if (op == 0x00 || op == 0x48) {
            // NOP (pads the stream to 32 bytes) / invalidate vertex cache: no effect on the model
        } else if (op == 0x08) {
            need = 6;
            if (n - i < need) break;
            cpLoad(d[i + 1], be32(d + i + 2));
        } else if (op == 0x10) {
            if (n - i < 5) break;
            const uint32_t hdr = be32(d + i + 1), count = (hdr >> 16) + 1;
            need = 5 + size_t(count) * 4;
            if (n - i < need) break;
            xfLoad(hdr & 0xFFFF, count, d + i + 5);
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            need = 5;
            if (n - i < need) break;
            xfIndexed(op, be32(d + i + 1));
        } else if (op == 0x40) {
            need = 9;
            if (n - i < need) break;
            callList(be32(d + i + 1), be32(d + i + 5), depth);
        } else if (op == 0x61) {
            need = 5;
            if (n - i < need) break;
            bpLoad(be32(d + i + 1));
        } else if ((op & 0xC0) == 0x80 && (op & 0x38) != 0x08) {
            size_t used = 0;
            if (!draw(op, d + i, n - i, used)) break;
            need = used;
        } else {
            fail(hex("GX FIFO: unknown command opcode", op), op);
        }
        if (obs_ && (op == 0x08 || op == 0x10 || op == 0x61)) obs_->command(op, d + i, need);
        i += need;
        ++stats_.commands;
    }
    if (depth == 0) streamBase_ += i;
    return i;
}

void Core::callList(uint32_t addr, uint32_t size, int depth) {
    if (depth >= 4) fail("GX display list nested deeper than 4", 0x40);
    const uint32_t phys = addr & 0x01FFFFFFu;
    const uint8_t* dl = mem_.read(phys, size);
    if (!dl) fail(hex("GX display list outside MEM1 at", addr), 0x40);
    ++stats_.displayLists;
    if (obs_) obs_->displayList(addr, size, dl);
    // The list is read while parsing; a draw inside it that reads vertex arrays sees the memory as it is now.
    std::vector<uint8_t> copy(dl, dl + size);   // the callee may not alias the memory it reads (arrays), keep a stable copy
    const size_t used = parse(copy.data(), copy.size(), depth + 1);
    if (used != size) fail(hex("GX display list ends inside a command at", addr), 0x40);
}

void Core::cpLoad(uint32_t reg, uint32_t v) {
    if (reg == 0x30) { cp_.matIdxA = v; }
    else if (reg == 0x40) { cp_.matIdxB = v; }
    else if (reg == 0x50) { cp_.vcdLo = v; }
    else if (reg == 0x60) { cp_.vcdHi = v; }
    else if (reg >= 0x70 && reg < 0x78) { cp_.vat[0][reg - 0x70] = v; }
    else if (reg >= 0x80 && reg < 0x88) { cp_.vat[1][reg - 0x80] = v; }
    else if (reg >= 0x90 && reg < 0x98) { cp_.vat[2][reg - 0x90] = v; }
    else if (reg >= 0xA0 && reg < 0xB0) { cp_.arrayBase[reg - 0xA0] = v & 0x03FFFFFFu; }
    else if (reg >= 0xB0 && reg < 0xC0) { cp_.arrayStride[reg - 0xB0] = v & 0xFFu; }
    else if (reg == 0x20) { /* written once by GXInit; no documented effect (YAGCD lists it as unknown) */ }
    else fail(hex("CP register not modelled", reg), reg);
}

void Core::xfLoad(uint32_t addr, uint32_t count, const uint8_t* words) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t a = addr + i, v = be32(words + 4 * i);
        if (a < 0x100 || (a >= 0x400 && a < 0x460) || (a >= 0x500 && a < 0x680)) {
            xf_.mem[a] = v;
        } else if (a >= 0x1000 && a < 0x1060) {
            xf_.reg[a - 0x1000] = v;
            dirty_ = true;
        } else {
            fail(hex("XF load outside the modelled memory map at", a), 0x10);
        }
    }
    engine_.xfWritten(addr, count);
}

void Core::xfIndexed(uint32_t op, uint32_t w) {
    const uint32_t which = (op - 0x20) >> 3;            // A..D -> CP arrays 12..15
    const uint32_t index = w >> 16, len = ((w >> 12) & 0xF) + 1, addr = w & 0xFFF;
    const uint32_t phys = cp_.arrayBase[12 + which] + cp_.arrayStride[12 + which] * index;
    const uint8_t* src = mem_.read(phys, len * 4);
    if (!src) fail(hex("indexed XF load reads outside memory at", phys), op);
    xfLoad(addr, len, src);
}

void Core::bpLoad(uint32_t word) {
    const uint32_t reg = word >> 24, data = word & 0xFFFFFF;
    if (reg == 0xFE) {          // BP mask: applies to the next BP write only
        bpMask_ = data;
        return;
    }
    const uint32_t mask = bpMask_;
    bpMask_ = 0xFFFFFF;
    if (!knownBp(reg)) fail(hex("BP register not modelled", reg), reg);
    const uint32_t value = (bp_[reg] & ~mask & 0xFFFFFF) | (data & mask);
    bp_[reg] = value;
    if (!stateNeutral(reg)) dirty_ = true;
    bpWrite(reg, value);
}

void Core::bpWrite(uint32_t reg, uint32_t v) {
    switch (reg) {
        case 0x45:
            backend_.sync(SyncKind::DrawDone, 0);
            host_.drawDone();
            break;
        case 0x47:
            backend_.sync(SyncKind::Token, static_cast<uint16_t>(v));
            host_.token(static_cast<uint16_t>(v), false);
            break;
        case 0x48:
            backend_.sync(SyncKind::TokenInterrupt, static_cast<uint16_t>(v));
            host_.token(static_cast<uint16_t>(v), true);
            break;
        case 0x52: efbCopy(); break;
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x66: ++texGeneration_; break;   // preload / cache invalidate
        case 0x65: tlutLoad(); break;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
            tevColor_[(v >> 23) & 1][(reg - 0xE0) >> 1][reg & 1] = v & 0x7FFFFF;
            break;
        case 0x08: case 0x4C: break;
        default: break;
    }
}

}  // namespace awl::gx
