# Recompiler generator and guest runtime: current state

Owner: P1 generator track. Describes what exists now; Git history is the changelog. Target: NTSC-U `GYWEE9`, `main.dol` SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`. Background and feasibility numbers: `recomp-feasibility.md`. The contract with the platform track is `tools/recomp/runtime/hle_abi.h` (architect-owned).

## Milestone 1: shared contract (done)

### Machine and thread state

`rt::Machine` (`tools/recomp/runtime/guest.h`) is the shared guest machine: guest memory (`mem`, `lc`), the recompiled function table (`functions`, `functionCount`, defaulting to the generated `kFunctions`), the retired-instruction counter `icount`, the instruction `limit`, and the census/trace diagnostics. `rt::Ctx` is the CPU state of one guest thread: GPRs, FPRs, CR, XER, LR, CTR, FPSCR, GQRs, storage SPRs, MSR/SR, reservation, `pc` and the shadow call stack. A `Ctx` is built from a `Machine` (`Ctx(Machine&)`, no default constructor) and reaches it through `c.m`. `createThreadContext(parent)` makes a zero-initialised `Ctx` on the parent's machine; it copies no registers, and each context has its own shadow stack (8192 frames, about 100 KB per context).

The retired-instruction counter is shared by all threads, so the default time base stays deterministic across contexts. Generated code ticks it with `rt::tick(c, n)`.

### Contract implementation (`guest.cpp`)

| Function | Behaviour |
|---|---|
| `hleRegister` | Stores `(addr -> name, handler)` in a process-wide table. A duplicate address or a null handler throws `Stop{Contract}` carrying the address and the registered name. The name must have static storage. |
| `hleCall` | Looks up the handler, retires one instruction (sets `pc` to the replaced function, so the instruction limit applies), runs it, and restores LR to its entry value (the replaced function's `blr` leaves LR unchanged). An unregistered address throws `Stop{HleUnregistered}` with the address and, when the generated name table knows it, the SDK name. |
| `mmioInstall` | Installs both models, or neither (uninstall). Passing only one throws `Stop{Contract}`. |
| Loads/stores | Plain memory (MEM1 at `0x80000000`/`0xC0000000`, locked cache at `0xE0000000`) is inline; everything else goes to `loadSlow`/`storeSlow`. In `0xCC000000-0xCC00FFFF` (access wholly inside) they call the installed model with the width (1, 2, 4, 8) and the value in guest numeric order; reads are masked to the width. Outside the window, or with no model installed, the access stops with `StopKind::Mmio` (`0xCC000000-0xCDFFFFFF`) or `Fault`. `dcbz` on hardware addresses stops instead of being routed. Census mode (`--census`) still absorbs unmodelled accesses when no model is installed. |
| `timeBaseInstall` | `mftb`, `mfspr` TBL (268) and TBU (269) call the source with the executing `Ctx`. Without one: retired instructions / 4. |
| `createThreadContext` / `destroyThreadContext` | `new Ctx(*parent.m)` / `delete`. No fibers are touched. |
| `callGuest` | Looks up a recompiled entry (anything else stops with `BadIndirect` and the address), pushes a shadow frame whose site is the current `pc`, runs it, pops it, and restores LR and `pc`. A callee's `blr` is therefore validated like any other call, including thread entries on a fresh context. |
| `declareHleNames` | Runtime-internal (`guest.h`, not in the ABI): the generated dispatch table passes `{addr, name}` pairs from the manifest so `hleCall` can name an unregistered function. |

### Manifest and generation

`awl-recomp gen` reads `tools/recomp/hle_manifest.txt` (override with `--hle-manifest FILE`; it is an error if the file cannot be opened). `tools/recomp/manifest.{h,cpp}` parses the documented line format and rejects, with `file:line:` diagnostics (all problems are reported): a module other than `dol`, missing or extra fields, an address that is not `0x` plus 1-8 hex digits, an unaligned address, a name that is not an identifier, missing `#` evidence, and duplicate addresses. Comments, blank lines and CRLF are accepted, and an empty manifest is valid. After discovery every address must be a discovered function start, otherwise `gen` fails. For each entry the generator emits `void f_XXXXXXXX(Ctx& c) { rt::hleCall(c, 0xXXXXXXXX); }` instead of the recompiled body (a manifest entry also takes precedence over a platform-evidence stub), treats the function as a leaf when computing the boot closure, and adds the name table. Output is ordered by address, so repeated runs give identical files.

### Evidence

- Tests: `recomp_contract_tests` (64 checks: MMIO routing and byte order at widths 1/2/4/8 and for float forms, window edges, no-model stops, duplicate/null/unregistered HLE, handler argument and result passing in r3, f1 and r3:r4, `callGuest` nesting and LR/pc restoration, two contexts with independent registers and shadow stacks, time base source) and `recomp_manifest_tests` (115 checks: parser accept/reject cases, validation against a hand-built program, emission in whole-DOL and boot-path modes, determinism). Expectations are derived from the contract text, big-endian numeric order and IEEE-754 encodings, not from implementation output.
- The boot-path runner (`awl-recomp-spike`, 1,736 functions) gives byte-identical output before and after the split: same stop at `0x801B3AC0` after 133,730 instructions with the same call trace, the same `--trace 4` output, and the same census output. Debug and Release agree.
- End-to-end smoke (local only): a manifest entry on `0x801B3AC0` makes the runner stop with `HLE FUNCTION WITHOUT HANDLER`, naming `OSTestPiSetup`.

### Open points and risks

- Storage-only SPRs (HID0/1/2, WPAR, DMA registers, BATs, SRR0/1, SPRGs) live in each `Ctx` as the contract lists them, so a thread does not see another thread's writes. Real hardware has one set. If the game reads back such a register on another thread, they should move to `Machine`; tell the generator track if the platform track hits this.
- The host floating-point environment (`MXCSR`, set by `syncHostFp`) is per OS thread, not per `Ctx`. The platform track's fiber switch should call `syncHostFp(c.fpscr)` for the context it resumes.
- `hleCall` does a hash lookup per call. If it shows up in profiles, the generator can emit a cached slot per function.
- Memory accesses now reach `mem` through `c.m->mem` (one extra load per access). Not measured; revisit with the first performance run.

### Portability (clang-cl)

Not compiled with clang-cl (not installed). Constructs that may need attention: `<intrin.h>` with `_byteswap_ushort/ulong/uint64` and `_BitScanReverse` (clang-cl provides them, GCC-style compilers do not); `<xmmintrin.h>` `_mm_setcsr` (portable to clang and GCC on x86); reliance on `/fp:strict` semantics (clang-cl maps it to `-ffp-model=strict`; the generated `std::fma` calls must not be contracted or reassociated); `[[noreturn]]` functions that throw (standard); `goto` into labels after declarations (the generated functions declare `ea` before any label; keep it that way). Generated code uses no MSVC-only extension.

## Contract extension: preemption, host data, locked-cache DMA (done)

- **Preemption.** `Machine::checkAt = min(instruction limit, deadline - 1)` is the single value `rt::at` compares the retired count against, so the limit and the deadline share one fast compare. The limit is changed with `Machine::setLimit`, the deadline with `preemptDeadlineSet`, and both refresh every live machine (`preemptInstall`, `preemptDeadlineSet` and the limit are process-wide). The generator emits a checkpoint (`rt::at(c, pc, 0)`) at every return (including conditional `blr`), backward `b`/`bc`, jump-table dispatch, indirect tail call, tail branch and fall-through into the next function; calls already pass through `rt::at`. When the count reaches the deadline the hook runs on the current context. It does not nest on a context already inside the hook (guest code the hook runs with `callGuest` passes checkpoints silently), but another context that reaches a still-due deadline enters the hook itself. A hook that returns with the deadline at or below the count stops with `StopKind::Contract`. The instruction limit is checked first and still stops with `InstructionLimit`. With no hook installed no deadline applies. The hook must save and restore any registers it disturbs: an interrupt delivered at a checkpoint interrupts arbitrary guest code.
- **Host data and contexts.** `setThreadHostData`/`threadHostData` store one pointer per `Ctx`. A new context has every register zero, an empty shadow stack and null host data. `callGuest` leaves the callee's r3/r4/f1 in the context and restores pc and LR.
- **Locked-cache DMA.** `mtspr` 923 (DMA_L) performs the transfer described by DMA_U (922) and DMA_L per the Gekko User's Manual layout (see `lockedCacheDma` in `guest.cpp`): length in 32-byte blocks (0 = 128), DMA_LD set loads memory into the cache, clear stores it. It completes instantly and DMA_T/DMA_F read back clear. A memory range outside MEM1 or a cache range outside the 16 KB at `0xE0000000` stops with `Fault`, naming the address and the `mtspr` pc. Evidence: `recomp_semantics_tests` (13 checks) with expectations from the manual's field layout and the SDK's flag values `0x12`/`0x02`; the loader of the field layout is the manual, not the implementation.
- **Evidence.** `recomp_contract_tests` now has 86 checks (a register-only loop is preempted at its backward branch with count in `[deadline, deadline + 2]`; limit and deadline interplay; no-progress hook; same-context and cross-context hooks) and `recomp_manifest_tests` checks the emitted checkpoints. With an empty manifest the boot-path runner output, trace and census are byte-identical to before (checkpoints retire no instructions).
