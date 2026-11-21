# Status

Updated: 2026-11-21 by Claude Code (architecture conversation, after the owner's window test). Phases, decisions and the stub register: `docs/roadmap.md`.

## Goal now
P2 is accepted. Next work block: recompiled-code speed, so the game runs at 100% of real time; then P3 playable farm.

## Decisions in force
- Strategy: static recompilation of the DOL + RELs to C++ on a native runtime (`tools/recomp`, `src/hle`, `src/gx`, `src/gfx`). The Sep–Oct hand-translation (`src/world_map_*`) is historical reference only.
- High-level replacement at the SDK API boundary; Dolphin only as a black-box reference the owner runs on request.
- Faithful by default: the XFB copy deflicker filter stays on (softer image, as on hardware); `--no-copy-filter` becomes an opt-in setting later.
- GX IR (`src/gx/gx_ir.h`) frozen at v2, architect-owned; front faces are clockwise on screen.
- Commits as `meiameiameia`, no trailers; standing approval to push verified increments to `origin`. Push only with zero `/W4` warnings in Debug and Release and all CTests passing.

## State (main after the window-prep commit)
- P0 and P1 complete. `awl-host` boots the recompiled game (15,349 functions: DOL 12,114 + modules 3,235) into its main loop deterministically; the boot matches the owner's Dolphin captures (boot memory, call order, DSP/AX exchange).
- P2: GX core decodes everything the game draws up to the title screen; the DX11 renderer draws the logos and the full title screen (sign, 3D town, "New Game / Continue", copyright), matching the owner's Dolphin screenshots in layout. A CPU reference renderer (`awl-gx-render`, `AWL_GX_SOFT`) exists for comparisons.
- **Owner window test, 2026-11-21: passed, P2 accepted.** The window came to the front by itself, the logos and title screen looked right compared with the Dolphin screenshots, the title ran at 40% of real time, W/S moved the menu cursor, F1 and F11 worked, the picture kept its proportions when resized, and Esc closed cleanly. Open findings: the arrow keys (D-pad) did not move the title cursor (only the stick did); clicking another window minimized the game window, so the focus pause could not be seen.
- Window mode (`awl-host --window`): title bar and a corner overlay (F1 hides it) show the controls and the measured speed as a percentage of real time (below 95% it says "slower than the original, expected for now"); a message box shows why the run stopped (stop report, failed present, start-up failure); focus loss pauses; keys: arrows = D-pad, WASD = stick, Space = A, Backspace = B, Enter = Start (X/Tab/Z/Q/E = X/Y/Z/L/R). Owner steps: `docs/research/gfx-dx11.md`, "Owner window test".
- GX: a preloaded (TMEM) texture that asks for a mip chain now fails loudly (the bank layout of lower levels is not modelled); no game texture hits it up to the title screen (`gx_core_tests` covers both cases).
- Speed: measured 40% of real time on the title screen in the window. Headless profiling earlier: recompiled code about 30 M guest instructions/s on the idle-skip path, against about 160 M needed; GX core about 5.5 ms per title frame.
- 2026-11-02 cleanup: `build/` deleted (regenerate: build `awl-recomp`, run `gen --full --dol rom\main.dol --rel-dir disc\files --out build\recomp-gen`, rebuild). Both paused P2 sessions' patches are now applied (renderer window-prep, finished; GX mip-chain guard, with a test) and the archive copies are obsolete.

## Next step
1. **Recompiled-code speed** (next work block; start it in a new architecture conversation on the strong model, which profiles, designs and writes the implementation brief here). Candidates: fewer per-instruction counter/checkpoint updates (count per basic block), a cheaper memory fast path, less `/fp:strict` cost where provably safe, a faster GX core path. Output must stay byte-identical (boot trace, title captures). Goal: 100% of real time on the title screen, shown by the window overlay (owner check) and by a headless pacing measurement.
2. Small fixes that can ride along with the speed block or follow it: (a) find out whether the title menu reads the D-pad on real hardware (owner can check in Dolphin) and fix the arrow-key mapping if ours is wrong; (b) the game window should not minimize when another window is clicked in windowed mode; check that the focus pause works.
3. Then P3 playable farm (see `docs/roadmap.md`).
