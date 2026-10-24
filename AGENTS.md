# AWL Native agent guide

## Goal and strategy

- Goal: Harvest Moon: A Wonderful Life, NTSC-U `GYWEE9` (verified DOL SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`), running natively on Windows without emulation, as a project other people can install and play: first-run setup from their own disc, settings, mod support and opt-in enhancements. The default configuration is faithful to the original.
- Strategy: statically recompile the original PowerPC code (`main.dol` plus `wlp0/1/2.rel`) into C++ compiled by MSVC, and run it on a native platform runtime: GX to DirectX 11, DSP/AX audio to a native audio API, PAD to XInput/keyboard, DVD to native files with a mod overlay, memory card to save files, and OS/VI timing. Hand-written native replacements of individual game functions are allowed for platform needs, enhancements, mods or performance.
- `docs/roadmap.md` holds the phases, current status and open owner decisions. The strategy is in its P0 feasibility gate until the owner records a go/no-go there.
- The existing hand-translated `src/world_map_*` code is reference material. Do not extend it unless the roadmap calls for a native replacement of that behavior.

## Roles and authority

- The owner decides intent, acceptance, dependencies, licensing, distribution and publication. Explain progress, tradeoffs and gaps in plain language, without requiring code literacy, and reply in the owner's language (English or Portuguese).
- The main Claude thread is the architect and engineer: it plans, traces, writes implementation briefs, reviews results and owns the roadmap. Implementation passes run in separate Sonnet sessions with a self-contained brief: DOL SHA1, baseline commit, scope, files, acceptance and required checks. The architect reviews the diff and reruns checks before reporting work as done or committing it.
- Implementation sessions work only on their briefed scope and their own branch or worktree, never push, and stop to report when they reach an owner decision, a new dependency, or a scope change.
- Reviews, diagnosis and planning are read-only unless changes are requested. Verify workspace, branch, relevant diff and actual paths before editing; preserve committed, uncommitted and untracked work.
- Complete scoped implementation through correction, targeted verification and self-review. Stop at an agreed boundary or a material intent, compatibility, data, cost or authority decision, not merely at a first draft.
- Commit, push, publication and external-data actions require explicit authority. Reuse a standing approval only for its stated branch, remote and conditions; it never covers deployment, spending, unrelated changes or publishing game data.
- Commits are authored only as `meiameiameia`, the project's sole contributor, with no `Co-Authored-By` or other attribution trailers. Do not rewrite published history.
- Do not use Computer Use; give precise manual steps when owner input is needed. Use native Windows/PowerShell; no WSL, Docker, Hyper-V or other virtualization.
- Every new dependency (production, build or tooling) needs owner approval. Never weaken bounds checks, warnings, tests or error reporting to pass a check. Never expose credentials or private data.

## Fidelity

- Game-logic fidelity comes from running the recompiled original code. The recompiler must preserve every PowerPC/Gekko semantic that can affect results: big-endian memory, CR/XER/carry/overflow, paired singles and GQR quantization, FPSCR rounding, fused multiply-add, `fres`/`frsqrte` estimates, reservations, and resolved indirect calls and jump tables. Any approximation is a documented and tested native difference.
- Instruction matching or a byte-identical DOL is not a goal. A hand-written replacement of a game function must reproduce the original's observable behavior where it affects results and must cite the traced addresses in a research note.
- The platform runtime reproduces the observable behavior of the GX, audio, PAD, DVD, OS and VI services that the game actually uses. Justify each service from the game's use of it plus independent references. Dolphin is GPL: read it for understanding only and never copy its code unless the owner adopts a compatible license.
- Unsupported instructions, calls, GX state or formats fail loudly with address and context. A temporary stub is allowed only if it logs every use and is listed in the roadmap's stub register; it never counts toward acceptance.
- Enhancements (resolution, widescreen, frame rate, texture packs) are opt-in settings and must not change game-logic outcomes. Code mods are an explicit owner-facing feature, not a default.
- At user-visible boundaries, compare against the original game (Dolphin captures or traces supplied by the owner). Record the scenario, the agreement or difference, and the evidence gaps. Synthetic tests, builds and smokes alone do not establish fidelity.
- `hmawl` is an unlicensed, read-only source of names and addresses only. Never copy its code or Git history, and do not invent function bodies from labels.

## Legal and data

- Keep `rom/`, `disc/`, extracted assets, Ghidra data, decompiler output and all code or data generated from the DOL/RELs (including recompiler output) local and ignored. Never put them in commits, fixtures, public reports or release artifacts until the owner records a distribution decision.
- The recompiler, runtime, launcher and evidence tools are this project's own code and are tracked under `tools/` or `src/`. They read the user's game files at runtime and embed no game bytes. Evidence scripts never live only in `build/`.
- The repository has no license yet. Do not add one, and do not import code from licensed projects, without an owner decision.

## Staying on course

- Progress means the executable or a declared tool can now do something it could not do before, and the owner or a check can observe it. Groundwork that the executable cannot reach does not advance a phase.
- Before starting an increment, state its phase, its user-visible outcome and its acceptance check. If three consecutive increments only uncover further dependencies without a visible outcome, stop and re-plan with the owner.
- Prefer breadth that reaches the running game over depth in a single subsystem.
- Research notes describe the current state: what is supported, its evidence and the open gaps. They are not journals; Git history is the changelog. Keep each roadmap status entry to a few lines.
- Read bounded exports and relevant sections, not whole disassemblies, logs or every research document.

## Context

- `docs/roadmap.md`: phases, status, stub register and owner decisions.
- `docs/re-pipeline.md`: disc extraction and the Ghidra tracing workflow.
- `docs/research/`: format and subsystem notes. `ground-rendering.md` covers GPL/TPL/GX ground rendering. `input-player-trace.md` and `first-playable-slice.md` are historical evidence from the hand-translation phase. `ARCHITECT_NOTES.md` is historical.

## Build and verification

Prerequisites: Visual Studio 2022/MSVC, CMake, Windows SDK. Command entry points from the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Debug\awl-decomp.exe --target-smoke
build\Debug\awl-decomp.exe --scene-smoke
certutil -hashfile rom\main.dol SHA1
```

- Run focused checks while iterating. A completed code increment requires clean MSVC Debug and Release builds at `/W4` and its relevant tests. Guidance-only edits need guidance/diff checks, not product builds or launches.
- Coordinate a safe time with the owner before any validation that opens a game/DX11 window; it can steal focus and minimize a fullscreen game. Builds and console tests can run normally.
- Synthetic test expectations must be independently justified (hardware manuals, format evidence, the original's observed behavior), not copied from the implementation.
- The executable mounts `disc/` relative to the working directory; run it from the root. A smoke succeeds only after ten actually presented frames. Occlusion, cancellation, timeout, early closure, or a parse/upload/draw failure must fail; fallback geometry cannot satisfy acceptance. `--target-smoke` and `--scene-smoke` are development fixtures, not game behavior.
- `AWL_CAPTURE_FRAME` selects a BMP path under ignored `build/` for the first completed DX11 back buffer. `AWL_PREVIEW_GPL` selects a logical `/files/...` path for `--preview-smoke`, which `--target-smoke` must reject. Clear both afterward.
- `tools/extract_disc.ps1` stages the extraction, verifies the Game ID and DOL hash, preserves any existing extraction, then promotes the new one. Never replace a valid extraction manually with unverified output.
- Reuse passing checks until changes, failures or unresolved risk invalidate them. Review the final diff and new files, and report exact evidence and gaps. Close only processes the task created, and keep technical verification separate from the owner's gameplay acceptance.
