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
// While a handler runs, the context's pc is guestAddr, so one handler may serve several entries
// and report the address. LR is restored to its entry value when the handler returns (blr).
void hleCall(Ctx& c, uint32_t guestAddr);

// ---- Hardware window ----------------------------------------------------------------------------
// Guest loads and stores in 0xCC000000-0xCC00FFFF (including the GX write-gather pipe at
// 0xCC008000, which game code writes directly) and in the EFB access window 0xC8000000-0xC8FFFFFF
// (CPU peeks and pokes of the embedded framebuffer, physical 0x08000000) are routed here, to the
// same model, which dispatches by address. size is 1, 2, 4 or 8 bytes, and
// values are in guest (big-endian) numeric order. Without installed models every access stops
// with StopKind::Mmio. Models must stop on registers they do not support, never ignore them.
using MmioRead = uint64_t (*)(Ctx& c, uint32_t addr, uint32_t size);
using MmioWrite = void (*)(Ctx& c, uint32_t addr, uint32_t size, uint64_t value);
void mmioInstall(MmioRead read, MmioWrite write);

// ---- Guest threads and callbacks ----------------------------------------------------------------
// Each guest thread runs on its own host fiber with its own Ctx (registers and shadow call stack);
// all contexts share one guest machine: memory, the locked cache, the retired-instruction count,
// the instruction limit and diagnostics. The platform track owns scheduling and fibers.
// createThreadContext returns a context on parent's machine with all registers zero, an empty
// shadow call stack (so its first return is unchecked) and no host data; the caller sets
// pc/r1/arguments. destroyThreadContext frees it; never destroy the context that is running.
Ctx* createThreadContext(Ctx& parent);
void destroyThreadContext(Ctx* c);
// Run the recompiled function at guestFn on c until it returns (thread entry, alarm, interrupt,
// DVD and other callbacks). An address that is not a recompiled function entry stops loudly.
// The call gets a return address its blr accepts; on return the callee's r3/r4/f1 results are
// left in c, and c's pc and LR are restored to their values before the call.
void callGuest(Ctx& c, uint32_t guestFn);
// One pointer per context reserved for the platform's per-thread data (for example its fiber).
void setThreadHostData(Ctx& c, void* data);
void* threadHostData(Ctx& c);

// ---- Preemption ---------------------------------------------------------------------------------
// Asynchronous events (decrementer, interrupts, DVD completion) are delivered at instruction-count
// checkpoints. Recompiled code reaches a checkpoint at least at every call, every return and every
// backward branch, so no loop runs unbounded without one. When the machine's retired-instruction
// count reaches the deadline, the installed hook runs on the current context. The hook may deliver
// events and switch fibers, and it must set a new deadline before it returns; a deadline left at or
// below the current count stops loudly. The separate instruction limit (a diagnostic stop) is
// unaffected. Without a hook, no deadline applies.
using PreemptHook = void (*)(Ctx& c);
void preemptInstall(PreemptHook hook);
void preemptDeadlineSet(uint64_t retiredInstructions);
uint64_t retiredInstructions(const Ctx& c);

// ---- Module linking -----------------------------------------------------------------------------
// The SDK's OSLink/OSUnlink stay recompiled. After a successful recompiled OSLink, the generator's
// runtime registers the linked REL's recompiled functions for indirect dispatch at their linked
// addresses, and removes them after OSUnlink. The platform track needs no hook for this.

// ---- Time base ------------------------------------------------------------------------------------
// Source for mftb/mfspr TBL/TBU, in guest ticks (the platform track defines the tick rate it
// models). Until installed, the deterministic retired-instruction count is used.
using TimeBaseSource = uint64_t (*)(Ctx& c);
void timeBaseInstall(TimeBaseSource source);

}  // namespace awl::rt
