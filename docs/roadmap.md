# Roadmap

Target: NTSC-U `GYWEE9`, `main.dol` SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`, plus `wlp0.rel`, `wlp1.rel` and `wlp2.rel`.

Strategy (adopted 2026-10-24): statically recompile the original PowerPC code into C++ and run it on a native Windows platform runtime. The earlier hand-translation phase (Sep–Oct 2026) is recorded in `docs/research/first-playable-slice.md` and `docs/research/input-player-trace.md`; its parsers, renderer core, input and disc tooling carry forward, and its hand-translated game logic is reference material.

## Phases

| Phase | Outcome | Acceptance | Status |
| --- | --- | --- | --- |
| P0 Feasibility | Recompiler prototype: load the DOL/RELs, decode all code, discover functions, resolve indirect control flow, emit and natively run the boot path up to the first platform call | Measured coverage report and an owner go/no-go | Spike complete, recommendation go (`docs/research/recomp-feasibility.md`): all code decodes, 99.72% assigned to functions, 248/248 `bctr` resolved, boot path runs natively to the first hardware access after 133,730 instructions. Awaiting owner go/no-go |
| P1 Boot | Full DOL + REL recompilation; the runtime reaches the game's main loop and logs every unsupported platform call | The main loop runs repeatably, with the stub register listing every gap | Planned |
| P2 Graphics | GX to DirectX 11 for the logo, title and menus | Visual comparison against Dolphin captures | Planned |
| P3 Playable farm | Input, saves and the in-game loop | Owner plays a session | Planned |
| P4 Audio | DSP/AX audio to a native API | Music and sound effects play correctly | Planned |
| P5 Product | First-run setup from the user's disc, settings overlay, asset mod overlay, packaging | A fresh user installs and plays | Planned |
| P6 Enhancements | Resolution, widescreen, texture packs, code mods; frame rate later | Opt-in; the default stays faithful | Planned |

## Owner decisions

| Decision | Needed by | Status |
| --- | --- | --- |
| P0 go/no-go for the recompilation strategy | End of P0 | Open |
| High-level replacement at the OS/GX/audio/DVD API boundary instead of hardware-level emulation | Start of P1 | Open |
| Use an independent emulator only as a black-box development oracle (FP reference values, boot-trace comparison); no code or tables copied | Start of P1 | Open |
| Run P1 as two parallel tracks (generator, platform runtime) | Start of P1 | Open |
| Distribution of recompiled code (generated on the user's machine vs. prebuilt binaries) | Before P5 | Open; until then all generated code stays local |
| Project license | Before accepting outside contributions or adapting any licensed code | Open; no license yet |
| Standing commit/push approval for the Claude workflow | Next commit | Granted 2026-10-24: the architect may commit and push each verified, reviewed increment to `origin/main` as `meiameiameia` |

## Stub register

Every temporary runtime stub is listed here with its address or service, its logging, and the phase that removes it. None exist yet.
