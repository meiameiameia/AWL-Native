# Roadmap

Target: NTSC-U `GYWEE9`, `main.dol` SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`, plus `wlp0.rel`, `wlp1.rel` and `wlp2.rel`.

Strategy (adopted 2026-10-24): statically recompile the original PowerPC code into C++ and run it on a native Windows platform runtime. The earlier hand-translation phase (Sep–Oct 2026) is recorded in `docs/research/first-playable-slice.md` and `docs/research/input-player-trace.md`; its parsers, renderer core, input and disc tooling carry forward, and its hand-translated game logic is reference material.

## Phases

| Phase | Outcome | Acceptance | Status |
| --- | --- | --- | --- |
| P0 Feasibility | Recompiler prototype: load the DOL/RELs, decode all code, discover functions, resolve indirect control flow, emit and natively run the boot path up to the first platform call | Measured coverage report and an owner go/no-go | Complete; owner go on 2026-10-24 (`docs/research/recomp-feasibility.md`): all code decodes, 99.72% assigned to functions, 248/248 `bctr` resolved, boot path runs natively to the first hardware access after 133,730 instructions |
| P1 Boot | Full DOL + REL recompilation; the runtime reaches the game's main loop and logs every unsupported platform call | The main loop runs repeatably; a deterministic boot trace matches the original's call sequence and key globals; the stub register lists every gap | Complete (2026-10-25). `awl-host` runs the recompiled DOL and RELs (15,635 functions) and passes the 120-retrace main-loop criterion deterministically (386.5 M instructions, about 0.5 s host time for 2.4 s guest time). Against the owner's Dolphin captures (`awl-capture-compare`): 42 boot-memory fields match plus 1 intentional retail difference; render mode and one-frame-per-retrace pacing agree; the first 261 system calls agree in order with no real argument differences; the DSP/AX exchange matches frame for frame. Native differences are recorded in `docs/research/hle-platform.md` |
| P2 Graphics | GX to DirectX 11 for the logo, title and menus | Visual comparison against Dolphin captures | Planned |
| P3 Playable farm | Input, saves and the in-game loop | Owner plays a session | Planned |
| P4 Audio | DSP/AX audio to a native API | Music and sound effects play correctly | Planned |
| P5 Product | First-run setup from the user's disc, settings overlay, asset mod overlay, packaging | A fresh user installs and plays | Planned |
| P6 Enhancements | Resolution, widescreen, texture packs, code mods; frame rate later | Opt-in; the default stays faithful | Planned |

## P1 tracks

The tracks meet only at `tools/recomp/runtime/hle_abi.h` (owned by the architect; changes are requested, not made by a track) and `tools/recomp/hle_manifest.txt` (written by the platform track, read by the generator).

| Track | Owns | Delivers |
| --- | --- | --- |
| Generator | `tools/recomp/**` except the manifest and `platform.cmake`; `tests/recomp_*`; `docs/research/recomp-generator.md` | Whole DOL + RELs generated and compiled; fragments/overlaps resolved; REL symbolic bases and module registration; per-thread contexts; the `hle_abi.h` implementation; portable output for clang-cl |
| Platform | `src/hle/**`; `tools/recomp/platform.cmake`; `tools/recomp/hle_manifest.txt`; `tests/hle_*`; `docs/research/hle-platform.md` | SDK function identification; OS (threads on fibers, alarms, interrupts, time base, memory), DVD to native files, PAD/SI, VI retrace, EXI/memory card, AI/DSP silent, GX FIFO capture; the console game host |

## Owner decisions

| Decision | Status |
| --- | --- |
| P0 go/no-go for the recompilation strategy | Go (2026-10-24) |
| High-level replacement at the OS/GX/audio/DVD API boundary instead of hardware-level emulation | Accepted (2026-10-24) |
| Independent emulator (Dolphin) only as a black-box development oracle: FP reference values and boot-trace comparison; no code or tables copied | Accepted (2026-10-24); the owner runs it when asked |
| Run P1 as two parallel tracks | Accepted (2026-10-24) |
| Distribution of recompiled code (generated on the user's machine vs. prebuilt binaries) | Deferred until before P5; meanwhile the generator output stays compatible with clang-cl and all generated code stays local |
| Install the Visual Studio "C++ Clang tools for Windows" component to verify clang-cl compatibility | Open |
| Project license | Open; no license yet. Needed before accepting outside contributions or adapting any licensed code |
| Standing commit/push approval for the Claude workflow | Granted 2026-10-24: the architect may commit and push each verified, reviewed increment to `origin/main` as `meiameiameia` |

## Stub register

Every temporary runtime stub is listed here with its address or service, its logging, and the phase that removes it. Details: `docs/research/hle-platform.md` section 9.

| Stub | Behaviour | Logging | Removed in |
| --- | --- | --- | --- |
| PAD (`PADInit`/`Reset`/`Recalibrate`/`Read`/`ControlMotor`) | channel 0 neutral, channels 1-3 absent | first 8 calls | P3 (input through `PadAdapter`) |
| GBA (`GBAInit`/`GetStatus`/`Read`/`Write`/`Reset`) | not ready | every call | P3 |
| DVD streaming (commands 6, 7, 9) | accepted, no audio data | every call | P4 |
| DVD inquiry | drive info set to Dolphin's reference values (Capture A), not a measured drive | once | P5 (review) |
| DVD timing | 1 ms + 4 ticks per byte | model parameter | P3 |
| DSP / AI | boot ROM and AX frame handshake only; silent; DMA replays its buffer | handshake events | P4 |
| `__OSInitAudioSystem` | net effect only (DSP left reset); `SIInit` reproduces handler registration and unmask, no SI transfers | once | P4 / P3 |
| EXI | no memory cards; RTC fixed at 2026-01-01; SRAM defaults | every access to absent devices | P3 (saves), P5 (real clock) |
| GX | command stream parsed, not drawn; finish/token/copy complete after 2,000 ticks | counters | P2 |
| Boot environment | values confirmed by Capture A; console type kept retail (`0x00000003`) by decision; system time fixed in deterministic mode | printed in the boot-trace header | P5 (real-time mode default) |
| Clock | 4 instructions per time-base tick | model parameter | P3 calibration |
| `fres` / `frsqrte` | exact reciprocal placeholders, not the hardware estimate (3 and 138 sites) | generator note | P3 (needs an FP reference capture) |
