// Hardware models behind the MMIO window 0xCC000000-0xCC00FFFF. Register layouts come from the public
// GameCube register map and from what the recompiled SDK code does with them (docs/research/hle-platform.md).
#pragma once

#include <memory>

#include "platform.h"

namespace awl::hle {

// MMIO blocks in the order of their address ranges.
enum Block : uint32_t { BlkCp, BlkPe, BlkVi, BlkPi, BlkMi, BlkDsp, BlkDi, BlkSi, BlkExi, BlkAi, BlkGx, BlkCount };

constexpr uint32_t kMmioBase = 0xCC000000u;
int blockOf(uint32_t addr);   // -1 when the address is outside every modelled block

void createDevices(Platform& p, std::unique_ptr<Device> blocks[16]);

// Big-endian sub-register access helpers.
inline uint32_t subExtract(uint32_t reg, uint32_t byteOff, uint32_t size) {
    const uint32_t shift = (4 - byteOff - size) * 8;
    const uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
    return (reg >> shift) & mask;
}
inline uint32_t subInsert(uint32_t reg, uint32_t byteOff, uint32_t size, uint32_t v) {
    const uint32_t shift = (4 - byteOff - size) * 8;
    const uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
    return (reg & ~(mask << shift)) | ((v & mask) << shift);
}

// Individual device factories (one per source file).
std::unique_ptr<Device> makePi(Platform& p);
std::unique_ptr<Device> makeMi(Platform& p);
std::unique_ptr<Device> makeDi(Platform& p);
std::unique_ptr<Device> makeSi(Platform& p);
std::unique_ptr<Device> makeVi(Platform& p);
std::unique_ptr<Device> makeExi(Platform& p);
void makeAudio(Platform& p, std::unique_ptr<Device>& dsp, std::unique_ptr<Device>& ai);
void makeGx(Platform& p, std::unique_ptr<Device>& cp, std::unique_ptr<Device>& pe, std::unique_ptr<Device>& fifo);

}  // namespace awl::hle
