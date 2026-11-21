# Status

Updated: 2026-11-21 by Claude Code (architecture conversation). Phases, decisions and the stub register: `docs/roadmap.md`.

## Goal now
P2 Graphics: the game's logos, title screen and menus drawn natively with DirectX 11 in a game window, then the owner's first window test. After P2: recompiled-code speed (needed for real time), then P3 playable farm.

## Decisions in force
- Strategy: static recompilation of the DOL + RELs to C++ on a native runtime (`tools/recomp`, `src/hle`, `src/gx`, `src/gfx`). The Sep–Oct hand-translation (`src/world_map_*`) is historical reference only.
- High-level replacement at the SDK API boundary; Dolphin only as a black-box reference the owner runs on request.
- Faithful by default: the XFB copy deflicker filter stays on (softer image, as on hardware); `--no-copy-filter` becomes an opt-in setting later.
- GX IR (`src/gx/gx_ir.h`) frozen at v2, architect-owned; front faces are clockwise on screen.
- Commits as `meiameiameia`, no trailers; standing approval to push verified increments to `origin`. Push only with zero `/W4` warnings in Debug and Release and all CTests passing.

## State (main `d95d72f`)
- P0 and P1 complete. `awl-host` boots the recompiled game (15,635 functions) into its main loop deterministically; the boot matches the owner's Dolphin captures (boot memory, call order, DSP/AX exchange).
- P2: GX core decodes everything the game draws up to the title screen (vertices, CPU transform/lighting, TEV, fog, indirect, textures incl. TMEM preloads, EFB copies); the DX11 renderer draws the logos and the full title screen (sign, 3D town, "New Game / Continue", copyright), matching the owner's Dolphin screenshots in layout. A CPU reference renderer (`awl-gx-render`, `AWL_GX_SOFT`) exists for comparisons.
- Window mode (`awl-host --window`) exists but has never been opened.
- Speed: recompiled code runs at about 30 M guest instructions/s; real time needs about 160 M. The deterministic idle skip is in. GX core costs about 5.5 ms per title frame.
- 2026-11-02 cleanup: `build/` deleted (regenerate: build `awl-recomp`, run `gen --full`, rebuild). Unfinished work of the two paused P2 sessions is saved as patches in `C:\Users\mperez\Archive\projects-2026-11-02\awl-native-worktree-patches\` (names are misleading: `p1-platform-0f4dfd.patch` is the renderer's window-test prep on `d95d72f`; `p1-generator-16d2d7.patch` is a small GX pipeline change on `40161cc`).

## Next step: brief for one implementation session (Sonnet)
**Goal:** the owner can run the game window and see the logos and title screen, with controls and speed shown, and a clear message if the run stops.

**Scope:**
1. Rebuild from scratch: configure, build `awl-recomp`, `gen --full` from `rom\main.dol` / `disc\files`, build everything.
2. Review and apply `p1-platform-0f4dfd.patch`, which touches `src/gfx/gfx_output.*`, `gfx_pacing.*`, `gfx_window.*`, `src/hle/host_main.cpp` and `tests/gfx_pacing_tests.cpp`. Finish it:
   - a corner overlay or window title showing the controls and the actual speed as a percentage of real time;
   - a stop reason shown in the window or a message box instead of vanishing;
   - keyboard mapping: arrows/WASD to the stick/D-pad, Enter = Start, Space = A, Backspace = B.
3. Review `p1-generator-16d2d7.patch` (`src/gx/gx_pipeline.cpp`). Apply it only if it is complete and tested; otherwise record what it was.
4. Write "Owner window test" steps for a non-programmer in `docs/research/gfx-dx11.md`: the command, what should appear, and what to try (move the menu arrow, resize, F11, Esc, Alt+F4, clicking away pauses).

**Acceptance:**
- Zero warnings in Debug and Release; all CTests pass.
- `awl-host` without video options still reaches the main loop with an identical trace across two runs.
- Headless `--capture-frames` of the title is unchanged.
- No window is opened by the session; the owner runs the test at an agreed time.

**Then:** the owner's window test. Next work block after that: recompiled-code speed (fewer per-instruction checks, a cheaper memory fast path; output must stay identical).
