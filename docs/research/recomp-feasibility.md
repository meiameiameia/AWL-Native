# Static recompilation feasibility (phase P0)

Status: spike complete (built from `main` `3f2a0f6`); results independently reproduced by the architect on 2026-10-24 (35/35 CTests Debug/Release, identical generation counts and runner stop). Awaiting the owner's go/no-go in `docs/roadmap.md`. Target: NTSC-U `GYWEE9`, `main.dol` SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374` (verified with `certutil` before every run). Everything below was measured on the owner's local files; nothing derived from game bytes is committed. Host: Windows 11, MSVC 2022 x64, 12 logical cores.

## Recommendation

**Go**, on three conditions: (1) the owner accepts HLE at the OS/GX/AI/DVD API boundary rather than hardware-level emulation, (2) the packaging question below is decided before P2, (3) floating-point fidelity gets an external oracle (see "Main unknowns"). Reasons: the decoder covers all 584,304 text words with no real gaps; discovery assigns 99.72% of the DOL text to 12,366 functions and the REL text to 3,269 more; 248 of 248 `bctr` are resolved; 1,736 boot-path functions (102,639 instructions) generate, compile and run natively with zero untranslated instructions and reach a hardware boundary after 133,730 instructions with a correct call trace; the whole DOL generates in 8 s and compiles in about 5.5 minutes on this machine. The work remaining is platform runtime (the OS layer in particular) and fidelity validation, not a code-translation problem.

## What exists

| Piece | File | Notes |
| --- | --- | --- |
| Gekko decoder | `tools/recomp/ppc.{h,cpp}` | 222 operations incl. paired singles, psq, `dcbz_l`, supervisor ops; each has a hand-assembled test word |
| DOL/REL loader + REL linker | `tools/recomp/image.{h,cpp}` | validates every offset; `linkRel` lays out and relocates at fixed bases |
| Discovery, jump tables, platform evidence | `tools/recomp/analysis.{h,cpp}` | see below |
| C++ generator | `tools/recomp/codegen.{h,cpp}` | writes only under an ignored directory (`build/recomp-gen`) |
| Guest runtime | `tools/recomp/runtime/guest.{h,cpp}` | context, flat big-endian memory, semantics helpers, shadow call stack |
| Console runner | `tools/recomp/runtime/spike_main.cpp` -> `awl-recomp-spike` | no window, no DX11 |
| Tests | `tests/recomp_{decoder,analysis,semantics}_tests.cpp` | CTest; 496 + 23 + 120 checks |

Commands (game paths come from arguments or `AWL_RECOMP_DOL` / `AWL_RECOMP_REL_DIR`; the runner is only configured when generated sources exist):

```powershell
cmake -S . -B build
cmake --build build --config Release --target awl-recomp
build\Release\awl-recomp.exe info     --dol rom\main.dol --rel-dir disc\files
build\Release\awl-recomp.exe decode   --dol rom\main.dol
build\Release\awl-recomp.exe discover --dol rom\main.dol --rel-dir disc\files
build\Release\awl-recomp.exe rel      --dol rom\main.dol --rel-dir disc\files
build\Release\awl-recomp.exe gen      --dol rom\main.dol --rel-dir disc\files --out build\recomp-gen   # boot path; --full for the whole DOL
cmake -S . -B build; cmake --build build --config Release --target awl-recomp-spike
build\Release\awl-recomp-spike.exe --dol rom\main.dol [--trace 3] [--census --limit 300000000]
```

## Measured coverage

**Decoder (DOL text, 2,337,216 bytes = 584,304 words, sections `0x80003100`/9,664 B and `0x80005980`/2,327,552 B).** 582,662 words decode. 1,642 do not: 1,628 are all-zero padding; 14 are non-zero data inside text (a `Metr...` string at `0x800034AC`, and 13 words of a table at `0x80005624-0x800056BC`, the tail of the first text section). No instruction word in real code failed to decode. 161 distinct base mnemonics (182 with `.`/`o`/`l`/`a` forms). Counts that matter for the semantics plan: `ps_*` ALU/merge/compare 2,776 plus 1 `dcbz_l` (the 2,777 in the brief), `psq_*` 3,460 (`psq_l` 1,705, `psq_st` 1,471, `psq_stu` 254, `psq_lu` 30, no indexed forms), `bl` 38,884, `bctrl` 2,256, `blrl` 123, `bctr` 248, `fcmpo` 1,243, `fcmpu` 266, `frsqrte` 131, `fres` 3, `fmadds` 397, `fdivs` 655, `mtfsf` 3, `mffs` 3, `mtfsb1` 1, `lwarx`/`stwcx.` 0, `icbi` 4, `rfi` 35, `sc` 3, `twi` 4.

**Function discovery (DOL).**

| Measure | Value |
| --- | --- |
| Functions | 12,366 |
| Text bytes assigned to a function body | 2,330,656 (99.72%) |
| Padding (nop/zero) between functions | 6,512 B (0.28%) |
| Unassigned non-padding | 48 B (the `Metr...` string) |
| Seeds (a function can have several) | `bl` target 5,998; data-section pointer 5,798; REL reference 962; code constant (`lis`+`addi`/`ori`) 361; gap heuristic 186; tail `b` 28; entry 1 |
| Text pointers found in data sections | 9,196 distinct: 5,588 start functions, 3,397 point inside an existing body (case labels), 1 rejected |
| Code-constant text addresses | 367 distinct, 261 were new function starts (this cut the pure gap-heuristic seeds from 449 to 186) |
| Tail-call edges / fall-through entry chains | 857 / 34 (CodeWarrior `_savegpr`/`_restgpr` style chains) |

*Overlap and ambiguity.* 10,337 words (1.8% of text) belong to more than one body, in 386 function pairs. 6,355 of those words are in 88 fragments seeded by the gap or tail heuristics; 3,259 are in 18 functions with a strong seed that share a tail with a neighbour; 723 involve data-pointer seeds. The largest case is `0x800FDA08` inside `0x800FD9E0` (1,067 words): a cleanup/dispatch region with no branch, jump-table or data reference to it. I did not find what reaches it (dead code and unwinder/landing-pad style entry are both plausible). The generator currently emits these fragments as separate functions; P1 must resolve them (merge into the parent or prove unreachable). 13 gap-seeded "functions" overlap the data table at `0x80005624` and are suspect data-in-text. REL references into DOL text: 962 distinct addresses, all discovered function starts, 705 of them also found independently from `bl`/data/code constants, which is a useful cross-check of discovery.

**REL text** (`rel` command: each REL is laid out and relocated at a synthetic base, then decoded and run through the same discovery):

| Module | id | text bytes | functions | assigned | shared words | `bctr` | `bctrl` | `psq_*` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `wlp0.rel` | 1 | 192,280 | 999 | 100% | 92 | 10 (all bounded jump tables) | 76 | 98 |
| `wlp1.rel` | 2 | 287,060 | 1,382 | 100% | 161 | 31 (all bounded jump tables) | 1,743 | 58 |
| `wlp2.rel` | 3 | 137,344 | 888 | 100% | 87 | 11 (all bounded jump tables) | 70 | 22 |

No undecodable word, no `ps_*` ALU op in any REL. Total recompilable text: DOL 2,337,216 B + RELs 616,684 B = 2,953,900 B (738,475 instructions), 15,635 functions.

## Indirect control flow

All 248 non-linking `bcctr` words in the DOL are classified (unique by address, none outside a discovered function):

| Kind | Count | How |
| --- | --- | --- |
| jump table, length from `cmplwi`+`bgt`/`bge` | 233 | `mtctr` <- `lwzx` <- table base from `lis`/`addi`/`ori` constant propagation; table entries must be text pointers within +/-256 KB |
| jump table, length found by scanning valid pointers | 14 | no usable compare; entries scanned until the first non-text word (examples `0x8001246C`, `0x800124B0`, `0x8001251C`, `0x800298D4`) |
| indirect tail call through a loaded pointer | 1 | `0x80235DA0`, the pointer-to-member call thunk at `0x80235D7C`, entered from many `bl` sites; resolved by runtime lookup |
| constant tail jump / unresolved | 0 / 0 | |

7,296 jump-table entries resolved in total. Unit tests cover the compare-bounded case on a hand-assembled function (a trailing extra table word must not be included). Codegen turns each table into a `switch (ctr)` with `goto` labels. The 14 length-scanned tables are the weakest part: a scan can overrun into an adjacent table, which would only add unreachable cases, but I did not verify each one.

`bctrl` (2,256) and `blrl` (123) become `callIndirect(site, target)`: binary search in a generated, address-sorted table `kFunctions[]`, push a shadow-stack frame, call. A target that is not a recompiled function entry stops with `BadIndirect` and the address. REL code needs per-module registration of entries at link time (below). Static resolution of constant `bctrl` targets is possible later but not needed for correctness.

`blr` is checked against the shadow stack (`LR` must equal the recorded return address); a mismatch stops with `BadReturn`. This is the detector for non-local control flow (setjmp-style, thread switches). It never fired in the 133,730 instructions of the boot path.

## Boot path result

`awl-recomp gen` emitted the functions reachable from the entry point `0x80003154` through `bl`, tail `b`, fall-through chains and constant tail jumps, stopping at functions with platform evidence: **1,736 functions, 102,639 instructions, 69 of them platform stubs**, 7 source files, 5.7 MB. 0 untranslated instructions, 0 unresolved `bctr`, 58 jump tables. It compiles in about 9 s (Debug, boot subset) and the runner links against it.

```
awl-recomp-spike: 1736 recompiled functions linked, entry 0x80003154 found

STOP: PLATFORM BOUNDARY
  guest pc            0x801B3AC0
  detail              MMIO PI x1
  instructions retired 133730 (counted through the stopping instruction)
  call trace (innermost first; function <- call site):
    #0   0x801B3AC0 <- 0x801B0F6C (in 0x801B0DE0)
    #1   0x801B0DE0 <- 0x80003264 (in 0x80003154)
    entry 0x80003154
```

The result is identical on every run (deterministic: the time base derives from the retired-instruction count). Path (from `--trace 2`, instruction counts are those retired at each call):

1. `0x80003154` is the entry. It calls `0x800032B0` (@1, register setup: 36 instructions) and `0x80003400` (@38). The latter reads/writes MSR and HID state (runs because MSR/SR/BAT/HID/L2CR are modelled as plain storage) and calls three small helpers.
2. `0x80003340` (@388) calls `0x800054E4` three times (@514, @120,542, @121,734). The gap of about 120,000 instructions is a fill loop over the 410,016-byte BSS (`0x802E8E00`), then two more fills.
3. `0x801D3348` (@122,079), then `0x801B0DE0` (@122,090), a large initialization function that calls 16 helpers in turn before the boundary, among them a 15-iteration loop (about 715 instructions per iteration, `0x801B11B8`) over `0x801B2428`/`0x801B24AC`/`0x801D33C8`/`0x800055CC`, and a second 15-iteration loop of `0x801B1460`; all recompiled and executed natively.
4. `0x801B3AC0` (@133,730) is the first function in the path that writes a hardware register. The static evidence is a constant address in the `0xCC003000` block (the processor-interface block in the public GameCube register map); the function also clears 128 bytes at `0x800030C0` and registers a handler through `0x801B1460`. It is an interrupt/exception setup routine of the SDK's OS init, found by address evidence, not by a guessed name.

The census mode (`--census`) skips each boundary (returning 0, reading hardware registers as 0) to list the boundaries in first-hit order; **only the first entry is valid emulation**, later ones come from corrupted state, and the run derails shortly after (`INDIRECT TARGET NOT RECOMPILED` at `0x801B69EC`). The order is still informative: PI `0x801B3AC0` (@133,730), EXI `0x801F7440`, SI `0x801F52F4`, a combined AI/DSP/EXI/MI/PI routine `0x801B3B34`, DI `0x801B15D8`.

## Platform boundary surface

Static evidence flags **248 functions (2.0% of the DOL's 12,366)** as hardware-facing: 212 with constant-address MMIO accesses and 36 containing `rfi`/`sc`. By hardware block (a function can touch several): GX FIFO write-gather port 110, DSP 24, DI 23, AI 15, VI 12, PI 11, SI 11, EXI 10, MI 5. Supervisor *state* moves (`mfmsr`/`mtmsr`, segment registers, BAT/HID/L2CR SPRs, `tlbie`) are modelled as storage and do not make a function a boundary. The block names come from the public GameCube register map (address ranges only).

The GX FIFO count is the actual size of the graphics API surface the platform layer must cover; the 110 functions are the SDK's GX command writers, which can be intercepted at call level instead of emulating the FIFO bytes. Names are deliberately not asserted here; mapping addresses to SDK names (via `hmawl` as a names-only reference) is a P1 task.

## REL linking requirements

Three version-3 RELs, module ids 1/2/3, 21-22 sections, prolog/epilog/unresolved entry points all in section 1. Relocations are 8-byte records in per-import streams with `R_DOLPHIN_SECTION`/`END` (and one `NOP` in `wlp0`) control records. Only four real types occur:

| Type | wlp0 | wlp1 | wlp2 |
| --- | --- | --- | --- |
| `R_PPC_ADDR32` | 1,455 | 3,262 | 1,467 |
| `R_PPC_ADDR16_LO` | 3,125 | 3,970 | 2,295 |
| `R_PPC_ADDR16_HA` | 3,125 | 3,970 | 2,295 |
| `R_PPC_REL24` | 2,010 (2,001 to DOL, 9 to wlp2) | 3,968 (3,965 to DOL, 3 to wlp2) | 1,695 (all to DOL) |

`wlp0` and `wlp1` both import module 3, so `wlp2` is the shared module; `wlp0` and `wlp1` never import each other. `ADDR16_HA/LO` pairs patch `lis`/`addi` address constants, `ADDR32` patches data (vtables, tables), `REL24` patches `bl` into DOL functions. REL-internal calls are already PC-relative in the file. My tool laid all three out and applied every relocation (9,715 / 15,170 / 7,752) with no unresolved import and no branch-range error; every REL24 target in the DOL (351 / 572 / 420 distinct) is a discovered function start.

What a native implementation needs:

1. **Linking.** The DOL contains the SDK's own `OSLink` (error strings `OSLink: unknown relocation type` and the `wlp2.rel`/`wlp1.rel`/`wlp0.rel` names are in the DOL data). The recompiled `OSLink` can perform the relocation into guest memory unchanged as long as DVD reads are served natively; only cache-invalidate calls need no-op stubs.
2. **Fixed or symbolic bases.** The game picks the load address at run time. Generated REL code must not depend on it: address constants come from `lis`/`addi` pairs whose sites are known from the relocation table, so the generator can emit `moduleBase[section] + offset` instead of the literal; internal calls are direct C++ calls. Alternative: pin each REL at a fixed guest address by intercepting the allocation.
3. **Dispatch registration.** After `OSLink` returns, register each recompiled REL function under `base + offset` so `bctrl` and vtable calls from the DOL find it; remove entries on `OSUnlink`. Function pointers in REL data (ADDR32 into text) rely on this.
4. **Entry points.** Run the `prolog` after link and the `epilog` before unlink as ordinary recompiled calls.
5. **Lifetime hazards.** A stale function pointer to an unloaded module must fail loudly (`BadIndirect`), which the lookup already does.
6. `fixSize` in the v3 header shows `OSLinkFixed` is used (relocation data can be dropped after linking); unverified whether the game calls it.

I did not determine when each REL is loaded or whether `wlp0` and `wlp1` are ever resident together (they have no mutual imports and `wlp2` is shared, which suggests alternating overlays). That needs an execution trace from the original.

## Semantics plan and risks

Implemented means present in `guest.h` and covered by `recomp_semantics_tests` with hand-derived expectations.

| Area | Handling | Status | Risk / evidence needed |
| --- | --- | --- | --- |
| Paired singles | Both lanes held as doubles in `Fpr{ps0,ps1}`; ops compute both lanes from the old register values, round each lane to single, write both | implemented, tested (`madd` fusion, `sum0/1`, `merge`, `muls`, `sel`, in-place `d==a`) | Scalar single-precision ops (`fadds`, `frsp`, ...) also write `ps1`; this follows common emulator behaviour, not verified on hardware. `ps_res`/`ps_rsqrte` not used in the DOL (0 occurrences) |
| GQR load/store quantization | Types float/u8/u16/s8/s16, signed 6-bit scale, `W` bit (single value, `ps1`=1.0); stores truncate toward zero and saturate | implemented, tested | NaN/inf stores and reserved types 1-3 (treated as float) are guesses; HID2[LSQE/PSE] and WPAR are stored, not enforced. 3,460 psq sites make this the highest-volume paired-single feature |
| FPSCR rounding / non-IEEE | `RN` mapped to MXCSR rounding on x64 with `/fp:strict`; `NI` mapped to FTZ+DAZ; `mtfsf/mtfsfi/mtfsb*/mffs/mcrfs` implemented | implemented, tested (frsp under all four modes, `fctiw` modes, denormal flush) | Sticky exception bits (FX/VX/OX/UX/ZX/XX), FPRF after arithmetic and enabled FP exceptions are not modelled; only `fcmp*` update FPCC. The DOL touches the FPSCR in just 7 places (3 `mtfsf`, 3 `mffs`, 1 `mtfsb1`); check what they set. NI-vs-FTZ equivalence is an approximation |
| `fmadd` fusion, single rounding | `std::fma` (single rounding, matches the architecture); single forms round the fused double result to float | implemented, tested | `fmadds` as `float(fma(double))` can double-round in rare cases; needs a round-to-odd step. Gekko's multiplier rounds the `C` operand of single-precision multiplies to fewer mantissa bits; not modelled. `fmuls` 2,617 and `fmadds` 397 sites make this the main FP-fidelity item |
| `fres` / `frsqrte` | placeholder (`1/x`, `1/sqrt`), explicitly marked | **not bit-exact** | The architecture only bounds the estimate error; real results come from a table. 131 `frsqrte` and 3 `fres` sites. Evidence that would justify an implementation: input/output pairs for every exponent parity and mantissa index, obtained from hardware or from the original binary running under an independent reference emulator used as a black-box oracle (no code or tables copied), then fitted and cross-checked against game-captured values |
| Carry, overflow, record forms | `addx<SetCa,SetOv>` handles add/sub/neg and the extended forms; `cr0` copies XER[SO]; `sraw` CA rules; `mullw` OV | implemented, tested | `divw` by zero and `INT_MIN/-1` (undefined by the architecture) return `-1`/`0` by sign with OV set, a commonly observed hardware result, unverified. `divwu` 1,043 and `divw` 262 sites |
| `lwarx`/`stwcx.` | single-thread reservation, cleared by `stwcx.` and `dcbz` on the line | implemented, tested | 0 occurrences in the DOL; irrelevant until REL/other code proves otherwise |
| Cache ops | `dcbz`/`dcbz_l` clear 32 bytes; `dcbf/dcbst/dcbt/dcbtst/icbi/sync/isync/eieio/dcbi` are no-ops | implemented, tested (`dcbz`) | Self-modifying code and code copied to RAM and executed are unsupported (a recompiled program does not execute memory). `icbi` appears 4 times, `dcbi` 3: check they only serve `OSLink`/exception-vector setup |
| SPRs, MSR, SR, time base | LR/CTR/XER real; GQR0-7 real; HID0/1/2, WPAR, DMA, L2CR, BATs, SRR0/1, SPRG, DEC kept as storage; PVR = `0x00083214` (recalled from public emulator documentation, unverified; only code that reads the PVR cares); `mftb` derived from retired instructions (4 per tick) | implemented, tested | The locked-cache DMA registers (`DMA_U/L`) do not trigger transfers yet (needed for any `LC*` SDK routine); DEC does not interrupt; the time base is deterministic but not wall-clock |
| Exceptions, interrupts, `rfi`, `sc`, threads | not recompiled; 36 functions flagged; `twi` (4) stops as untranslated | n/a | Biggest design item: guest threads need separate host stacks (fibers) plus a shadow stack each; interrupt-driven callbacks need an event model. The `BadReturn` check is the safety net |
| Endianness and memory | flat 24 MB MEM1 with big-endian helpers; `0x80000000`/`0xC0000000` views alias; `0xE0000000` 16 KB locked cache; anything else faults (`0xCC000000+` reports `Mmio`) | implemented, tested | ARAM and the 0xCC window need runtime models; 8-byte-aligned `lfd` etc. rely on x86 unaligned access |

## Generated code size and build time

| | boot subset | whole DOL (`--full`) |
| --- | --- | --- |
| Functions / instructions | 1,736 / 102,639 | 12,366 / 570,564 (includes the duplicated fragment bodies; 248 platform stubs) |
| Source | 5.7 MB, 7 files | 32 MB, 31 files of about 1 MB |
| Generation time | 4 s | 8 s |
| MSVC time | about 9 s (Debug) | 5 min 8 s and 5 min 34 s (Release `/O2 /fp:strict`, two runs, 12 logical cores, `/MP`); objects total 40 MB (timed on the output one revision earlier: 12,365 functions, 570,562 instructions) |

A single 1 MB chunk takes about 15 s in Release on one core. The whole DOL is therefore a manageable build, and chunked output keeps incremental rebuilds proportional to what changed. Adding about 26% more text for the RELs scales linearly. Runtime performance of the generated code was not measured (the run executes 133,730 instructions); straight-line C++ over a flat memory array is the usual shape of static recompilers and I expect it to be far above real time, but that is an expectation, not a measurement.

In the whole-DOL output 18 instructions are untranslated by construction (13 undecodable words in the data table, 4 `twi`, 1 `bl` into a non-function) and 10 `bctr` are reported unresolved; all of these sit in the suspect fragments, not in reachable code.

## Main unknowns

1. **Packaging.** Generated C++/objects are derived from copyrighted game bytes and cannot be redistributed. An installable product therefore has to run the recompiler on the user's own disc and compile on their machine. MSVC is not something to assume; options are a bundled compiler, direct x64/LLVM emission, or an interpreter/threaded-code fallback. This is an owner-level decision, not a P0 feasibility question, but it determines the shape of the generator output.
2. **Threads, interrupts and timing.** How much of the OS to run as recompiled code versus replace. I recommend replacing the scheduler and interrupt machinery (fibers, event queue) and keeping software-only OS code. Not designed or measured.
3. **Floating-point fidelity** (table above). Requires an external oracle, which the owner must authorize.
4. **Fragments and unreachable-looking code.** 10,337 words in overlapping bodies and 186 gap-seeded functions: what reaches them (dead code, exception tables, code only entered through paths I have not found) is unknown. `extab`/`extabindex` style tables were not investigated.
5. **REL life cycle.** When each module is loaded and unloaded, and whether the game relies on exact load addresses.
6. **Audio/DSP and memory cards.** 24 DSP + 15 AI + 10 EXI functions; the DSP microcode interface was not examined.
7. **Real-time performance** of generated code and of the shadow-stack bookkeeping.

## P1 estimate

P1 = whole DOL and RELs recompiled, game reaches its main loop (video output not required). Rough, one engineer focused full time, with low confidence (+/-50%, skewed upward):

| Work item | Weeks |
| --- | --- |
| Generator hardening: resolve fragments/overlaps, shared tails, noreturn calls, full-DOL build and chunking, regression comparison of generated output | 1-2 |
| REL pipeline: per-module generation with symbolic bases, `OSLink` hook and dispatch registration | 1-2 |
| OS layer: fiber threads, alarms/timers, interrupt event model, time base, exception cases, replacing the 36 `rfi`/`sc` functions | 3-5 |
| Peripherals to reach the main loop: DVD -> native files (existing `dvd.cpp`), PAD/SI (existing `input.cpp`), VI retrace/framebuffer, EXI/memory-card stub, AI/DSP silent, GX FIFO consumer or call-level stubs (110 functions) | 3-4 |
| Bring-up debugging against an oracle, FP semantics fixes (`fres`/`frsqrte`, `fmadds`) | 2-4 |
| **Total** | **10-17 (about 3-4 months)** |

The generator and the platform runtime are separable (two tracks), which could shorten wall-clock time if the owner wants parallel sessions. A reasonable P1 exit test is a deterministic boot trace through OS init to the main loop that matches the original on the call sequence and key globals.

## Open questions for the owner

1. Is HLE at the API boundary (OS scheduler, GX, AI, DVD) acceptable, instead of hardware-level emulation?
2. May I use an independent emulator purely as a black-box oracle (inputs, outputs, traces) for FP vectors and boot-trace comparison? No code or tables would be copied.
3. What is the intended distribution model (compile on the user's machine, bundled compiler, or something else)?
4. Should P1 run as two parallel tracks (generator, platform runtime)?

## Appendix A: instruction counts (DOL text, base mnemonics, record/overflow/link forms merged)

161 distinct base mnemonics occur; 582662 decoded words.

```
addi 103639, lwz 78535, stw 73250, b 57641, bc 38593, or 34804, addis 18004, rlwinm 13349, lfs 13287, bclr 13015, mtspr 11326, cmpli 10188, cmpi 9405, stfs 8200, stwu 7666, mfspr 7543, add 6764, lbz 6382, stb 5772, lfd 4020, subf 3068, sth 2670, lhz 2649, fmuls 2617, cmpl 2522, bcctr 2504, cmp 2253, fsubs 2135, addic. 2005, mulli 1934, psq_l 1705, stfd 1693, fmul 1658, lwzx 1604, fmr 1589, psq_st 1471, mullw 1364, fcmpo 1243, fadds 1121, extsh 1076, divwu 1043, lha 1038, ori 974, xoris 929, rlwimi 898, subfe 728, ps_sub 710, ps_add 706, extsb 667, fdivs 655, frsp 607, and 573, lwzu 565, stwx 533, subfic 513, lbzx 488, addic 472, cror 443, srawi 439, cntlzw 433, fsub 397, xor 397, fmadds 397, slw 385, fctiwz 333, subfc 303, fneg 274, fcmpu 266, divw 262, crxor 260, psq_stu 254, neg 250, stbx 239, ps_mul 230, fnmsub 222, ps_msub 213, srw 192, lfsx 173, oris 138, ps_merge00 134, fmadd 134, lmw 133, ps_madd 132, stmw 132, ps_nmsub 132, frsqrte 131, fadd 125, ps_merge10 120, sthx 117, ps_merge11 116, andc 107, lhax 106, lhzx 101, mulhwu 96, adde 94, stfsx 94, subfze 82, addze 80, andi. 80, nor 80, eqv 76, stbu 73, mulhw 73, fabs 67, addc 64, lfsu 62, xori 61, fdiv 58, ps_madds0 57, mfcr 55, mfmsr 54, lbzu 51, ps_madds1 47, ps_muls0 46, ps_merge01 41, fmsubs 36, rfi 35, sync 33, rlwnm 33, sraw 32, ps_mr 31, ps_sum0 31, psq_lu 30, fnmsubs 29, mtmsr 27, stfiwx 24, lfdx 23, isync 21, lhzu 19, fmsub 18, ps_muls1 17, mtsr 16, mfsr 16, dcbz 14, mtcrf 9, sthu 8, fctiw 8, mftb 8, ps_sum1 6, andis. 6, stfsu 5, dcbst 5, stfdu 4, icbi 4, dcbf 4, twi 4, mtfsf 3, dcbi 3, fres 3, ps_neg 3, sc 3, mffs 3, ps_cmpo0 2, lfdu 2, ps_nmadd 2, stfdx 2, mcrxr 2, mtfsb1 1, dcbt 1, orc 1, dcbz_l 1
```

## Appendix B: undecodable words

1,642 words do not decode. 1,628 are all-zero, in 28 runs: 27 in the first text section (`0x800034DC-0x800035A8`, `0x800035B0-0x800036A8`, `0x800036F8-0x800037A8`, `0x800037E0-0x800038A8`, `0x800038E0-0x800039A8`, `0x800039E0-0x80003AA8`, `0x80003AE0-0x80003BA8`, `0x80003BE0-0x80003CA8`, `0x80003CE0-0x80003DA8`, `0x80003DE0-0x800040A8`, `0x800040E0-0x800041A8`, `0x800041E0-0x800042A8`, `0x800042E0-0x800043A8`, `0x800043B0-0x800043C8`, `0x80004434-0x800044A8`, `0x8000451C-0x800045A8`, `0x8000461C-0x800046A8`, `0x8000471C-0x800047A8`, `0x800047E0-0x800048A8`, `0x800048E0-0x80004AA8`, `0x80004AE0-0x80004BA8`, `0x80004BE0-0x800050A8`, `0x800050E0-0x800051A8`, `0x800051E0-0x800052A8`, `0x800052E0-0x800053A8`, `0x80005694-0x8000569C`, `0x800056B8-0x800056BC`) and one 3-word run at `0x80234C24-0x80234C2C`. The blocks in the first section repeat on a 0x100-byte stride, which fits exception-vector slots with zero fill (inference from layout only). The 14 non-zero words: `0x800034AC` (`0x4D657472`, the start of an ASCII string) and `0x80005624`, `0x80005630`, `0x8000563C`, `0x80005648`, `0x80005654`, `0x80005660`, `0x8000566C`, `0x80005678`, `0x80005684`, `0x80005690`, `0x800056A4`, `0x800056AC`, `0x800056B4` (a table of 12-byte records at the end of the first text section; their neighbours decode as instructions by accident, so the discovery step treats that region as 13 suspect functions).
