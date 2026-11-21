# Status

Updated: 2026-11-21 by Claude Code (window-prep implementation session). Phases, decisions and the stub register: `docs/roadmap.md`.

## Goal now
P2 Graphics: the game's logos, title screen and menus drawn natively with DirectX 11 in a game window, then the owner's first window test. After P2: recompiled-code speed (needed for real time), then P3 playable farm.

## Decisions in force
- Strategy: static recompilation of the DOL + RELs to C++ on a native runtime (`tools/recomp`, `src/hle`, `src/gx`, `src/gfx`). The Sep–Oct hand-translation (`src/world_map_*`) is historical reference only.
- High-level replacement at the SDK API boundary; Dolphin only as a black-box reference the owner runs on request.
- Faithful by default: the XFB copy deflicker filter stays on (softer image, as on hardware); `--no-copy-filter` becomes an opt-in setting later.
- GX IR (`src/gx/gx_ir.h`) frozen at v2, architect-owned; front faces are clockwise on screen.
- Commits as `meiameiameia`, no trailers; standing approval to push verified increments to `origin`. Push only with zero `/W4` warnings in Debug and Release and all CTests passing.

## State (main after the window-prep commit)
- P0 and P1 complete. `awl-host` boots the recompiled game (15,349 functions: DOL 12,114 + modules 3,235) into its main loop deterministically; the boot matches the owner's Dolphin captures (boot memory, call order, DSP/AX exchange).
- P2: GX core decodes everything the game draws up to the title screen; the DX11 renderer draws the logos and the full title screen (sign, 3D town, "New Game / Continue", copyright), matching the owner's Dolphin screenshots in layout. A CPU reference renderer (`awl-gx-render`, `AWL_GX_SOFT`) exists for comparisons.
- Window mode (`awl-host --window`) is ready for the owner's first test and has **never been opened**: title bar and a corner overlay (F1 hides it) show the controls and the measured speed as a percentage of real time (below 95% it says "slower than the original, expected for now"); a message box shows why the run stopped (stop report, failed present, start-up failure); focus loss pauses; keys: arrows = D-pad, WASD = stick, Space = A, Backspace = B, Enter = Start (X/Tab/Z/Q/E = X/Y/Z/L/R). Owner steps: `docs/research/gfx-dx11.md`, "Owner window test".
- GX: a preloaded (TMEM) texture that asks for a mip chain now fails loudly (the bank layout of lower levels is not modelled); no game texture hits it up to the title screen (`gx_core_tests` covers both cases).
- Speed: recompiled code runs at about 30 M guest instructions/s on the idle-skip path; real time needs about 160 M. GX core costs about 5.5 ms per title frame. The window will therefore run in slow motion (roughly a fifth of real time) until the speed work below.
- 2026-11-02 cleanup: `build/` deleted (regenerate: build `awl-recomp`, run `gen --full --dol rom\main.dol --rel-dir disc\files --out build\recomp-gen`, rebuild). Both paused P2 sessions' patches are now applied (renderer window-prep, finished; GX mip-chain guard, with a test) and the archive copies are obsolete.

## Next step
1. **Owner window test** (pending, needs an agreed time; no assistant opens the window): follow "Owner window test" in `docs/research/gfx-dx11.md` and report what it asks for.
2. **Recompiled-code speed** (the next work block, strong model to design, Sonnet to implement): fewer per-instruction checks, a cheaper memory fast path, a faster GX core path; output must stay byte-identical (boot trace and title captures). Goal: 100% of real time on the title screen. Acceptance includes the speed shown by the window overlay/headless pacing.
3. Then P3 playable farm (see `docs/roadmap.md`).
