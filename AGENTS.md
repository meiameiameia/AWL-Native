# AWL Native

Harvest Moon: A Wonderful Life (NTSC-U `GYWEE9`, DOL SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`) running natively on Windows without emulation, by statically recompiling the original PowerPC code (`main.dol` + `wlp0/1/2.rel`) to C++ built with MSVC, on a native runtime (GX to DirectX 11, DSP/AX audio, PAD to XInput/keyboard, DVD to files with mod overlay, memory card to saves). Hobby project the owner works on when there is spare budget: keep increments small and visible.

## Authority, legal and data

- Commits are authored only as `meiameiameia`, with no `Co-Authored-By` or other trailers. A standing approval covers committing and pushing verified scoped increments to `origin` (`meiameiameia/AWL-Native`); never deployment, publishing game data or rewriting published history.
- Every new dependency (production, build or tooling) and any license needs owner approval. The repo has no license yet.
- `rom/`, `disc/`, extracted assets, Ghidra data, decompiler output and anything generated from the DOL/RELs (including recompiler output) stay local and ignored. Never commit, publish or put them in fixtures. `rom/` and `disc/` are the owner's extracted game data: never delete them.
- `hmawl` is a read-only source of names and addresses only; never copy its code. Dolphin is GPL: read for understanding, never copy.
- Do not use Computer Use. Before anything that opens a game/DX11 window, agree a time with the owner (it steals focus from fullscreen games).

## Rules that must not break

- Game logic comes from the recompiled original code. The recompiler preserves every PowerPC/Gekko semantic that can change results (big-endian memory, CR/XER, paired singles and GQR, FPSCR rounding, FMA, `fres`/`frsqrte`, reservations, indirect calls and jump tables). Any approximation is a documented, tested native difference.
- A hand-written replacement of a game function reproduces the original's observable behavior and cites traced addresses in a research note.
- Unsupported instructions, calls, GX state or formats fail loudly with address and context. Temporary stubs log every use and are listed in the roadmap's stub register; they never count toward acceptance.
- Enhancements (resolution, widescreen, frame rate, texture packs) are opt-in and never change game logic. Default behavior is faithful to the original.
- Test expectations come from hardware manuals, format evidence or the original's observed behavior (owner-supplied Dolphin captures), never from the implementation.
- Progress means the executable can do something new that can be observed. If three increments in a row only uncover dependencies, stop and re-plan with the owner. Prefer breadth that reaches the running game.

## Stack and commands

Visual Studio 2022/MSVC, CMake, Windows SDK. From the root:

```powershell
cmake -S . -B build
cmake --build build --config Debug ; ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release ; ctest --test-dir build -C Release --output-on-failure
build\Debug\awl-decomp.exe --target-smoke
build\Debug\awl-decomp.exe --scene-smoke
```

- A finished code increment needs clean Debug and Release builds at `/W4` plus its relevant tests. Iterate with focused checks.
- Run the executable from the root (it mounts `disc/` relative to it). A smoke passes only after ten presented frames; fallback geometry never satisfies acceptance.
- `tools/extract_disc.ps1` is the only way to (re)extract the disc; it verifies Game ID and DOL hash.
- `AWL_CAPTURE_FRAME` / `AWL_PREVIEW_GPL` / `AWL_PAD_SCRIPT` are debug env vars; clear them afterwards.
- `build/` is regenerable and may be deleted by the owner's cleanup. Use git worktrees only for parallel work and remove them after merge.

## Read on demand

- `docs/roadmap.md`: phases, stub register, owner decisions; read before planning an increment.
- `docs/re-pipeline.md`: disc extraction and Ghidra tracing.
- `docs/research/`: subsystem notes (`ground-rendering.md` for GX ground). `src/world_map_*` and `ARCHITECT_NOTES.md` are historical reference.

## Continuity

Current state and next step: `docs/STATUS.md`. Update it at the end of each work block. Git history is the changelog; research notes describe current state, not journals.
