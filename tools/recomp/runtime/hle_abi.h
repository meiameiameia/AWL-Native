#pragma once
// Contract between recompiled code (P1 generator track) and the platform runtime (P1 platform
// track). Owned by the architect: both tracks code against it and request changes instead of
// editing it. The generator track implements every function declared here, in
// tools/recomp/runtime/; the platform track only registers handlers and installs models.
//
// Calling convention: handlers and guest calls use the PowerPC EABI held in rt::Ctx. Arguments
// arrive in r3-r10 / f1-f8, integer results return in r3 (r3:r4 for 64-bit), float results in f1,
// and r1 is the guest stack pointer. A handler that returns has completed the original
// function's observable effect; one that cannot must call rt::stop with the guest address.
#include <cstdint>

namespace awl::rt {

struct Ctx;  // the CPU state of the current guest thread; reaches the shared guest memory

// ---- Function replacement (HLE) ------------------------------------------------------------------
// Every function listed in tools/recomp/hle_manifest.txt is emitted as a call to hleCall instead
// of its recompiled body. An address with no registered handler stops loudly, naming the function.
using HleHandler = void (*)(Ctx& c);
// Register at runtime startup, before guest code runs. Registering an address twice is an error.
void hleRegister(uint32_t guestAddr, const char* name, HleHandler handler);
void hleCall(Ctx& c, uint32_t guestAddr);

// ---- Hardware window ----------------------------------------------------------------------------
// Guest loads and stores in 0xCC000000-0xCC00FFFF (including the GX write-gather pipe at
// 0xCC008000, which game code writes directly) are routed here. size is 1, 2, 4 or 8 bytes, and
// values are in guest (big-endian) numeric order. Without installed models every access stops
// with StopKind::Mmio. Models must stop on registers they do not support, never ignore them.
using MmioRead = uint64_t (*)(Ctx& c, uint32_t addr, uint32_t size);
using MmioWrite = void (*)(Ctx& c, uint32_t addr, uint32_t size, uint64_t value);
void mmioInstall(MmioRead read, MmioWrite write);

// ---- Guest threads and callbacks ----------------------------------------------------------------
// Each guest thread runs on its own host fiber with its own Ctx (registers and shadow call stack);
// all contexts share one guest memory. The platform track owns scheduling and fibers.
Ctx* createThreadContext(Ctx& parent);   // copies no registers; the caller sets pc/r1/arguments
void destroyThreadContext(Ctx* c);
// Run the recompiled function at guestFn on c until it returns (thread entry, alarm, interrupt,
// DVD and other callbacks). An address that is not a recompiled function entry stops loudly.
void callGuest(Ctx& c, uint32_t guestFn);

// ---- Time base ------------------------------------------------------------------------------------
// Source for mftb/mfspr TBL/TBU, in guest ticks (the platform track defines the tick rate it
// models). Until installed, the deterministic retired-instruction count is used.
using TimeBaseSource = uint64_t (*)(Ctx& c);
void timeBaseInstall(TimeBaseSource source);

}  // namespace awl::rt
