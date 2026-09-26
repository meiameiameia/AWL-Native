# AWL Native agent guide

## Authority and evidence

- The owner decides intent and acceptance; own technical investigation and explain progress, tradeoffs, and gaps without requiring code literacy.
- Reviews, diagnosis, and planning are read-only unless changes are requested. Verify workspace, branch, relevant diff, and actual paths before editing; preserve committed, uncommitted, and untracked work.
- Complete scoped implementation through correction, targeted verification, and self-review. Stop at an agreed boundary or material intent, compatibility, data, cost, or authority decision, not merely a first draft.
- Commit/push/publication/external-data actions require explicit authority. Reuse an existing scoped standing commit/push approval only for its verified branch/remote and conditions; it grants no deployment, spending, unrelated changes, or publication of game assets.
- Separate independent/external audits or delegated tasks require an explicit owner request. Do not use Computer Use; give precise manual gameplay steps when owner input is needed.
- Use native Windows/PowerShell. No WSL, Docker, Hyper-V, or other virtualization. Prefer existing/native facilities; production dependencies need approval.
- Never weaken bounds checks, warnings, tests, or error reporting to pass a check. Never expose credentials or private data.

## Product and fidelity boundaries

- Goal: a behaviorally faithful native Windows port of Harvest Moon: A Wonderful Life, NTSC-U `GYWE41`. Verified target DOL SHA1: `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`.
- The harness and partial translations do not establish a playable port. Distinguish traced behavior, translated isolated helpers, connected runtime behavior, and owner-accepted gameplay; placeholders and diagnostic camera/scene fixtures are not original-game behavior.
- Trace each translation to the verified DOL/REL disassembly; retain addresses, branch/layout evidence, confidence, and unresolved assumptions in the relevant research note.
- `hmawl` is an unlicensed, read-only source of names/addresses only. Never copy its code or Git history. Do not invent bodies or responsibilities from labels.
- Use justified native equivalents for GX/VI/OS/PAD/DVD/audio/memory behavior, not empty stubs. DirectX 11 is the renderer; native file I/O replaces DVD access.
- Reject unsupported structures explicitly. One decoded section, draw range, asset, screenshot, or successful build does not validate the whole format or broader GX/game fidelity.
- Do not connect an isolated movement helper as accepted gameplay movement before its required collision/state dependencies are justified.
- Keep `rom/`, `disc/`, extracted assets, Ghidra data, raw decompiler output, and copyrighted debug payloads local and ignored. Never embed them in commits, synthetic fixtures, or public reports.

## Context and work sizing

- Use `docs/re-pipeline.md` for extraction/tracing; `docs/research/ground-rendering.md` for supported GPL/TPL/GX and scene boundaries; `docs/research/input-player-trace.md` for PAD, steering, collision, and remaining dependencies. Inspect relevant source/tests; `ARCHITECT_NOTES.md` may be historical.
- Keep small dependent reverse-engineering/implementation slices with the same executor to retain useful local context. Propose a separate task only for a substantial bounded subsystem investigation or implementation; create it only when requested, with verified DOL identity, addresses, file baseline, scope, and acceptance.
- At a completed slice, update the existing research note with new evidence, supported boundary, and remaining dependency; do not restate the trace in AGENTS.md. A task handoff needs only checkout/SHA plus dirty delta, authority, relevant note sections, checks/gaps, and next bounded step.
- Read bounded exports and relevant sections, not whole disassembly/logs or every research document. Choose supported model/effort controls for the task; do not switch or rotate sessions just for cache. Reuse valid final-state evidence.

## Native build and verification

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

- Run focused checks during iteration; completed code increments require clean MSVC Debug/Release builds at `/W4` and relevant tests. Guidance-only edits require guidance/diff checks, not product builds or launches.
- For asset/rendering changes, check bounds/topology and the relevant bounded DX11 smoke with local assets when available. Synthetic expectations must be independently justified, not copies of the implementation.
- The executable currently mounts `disc/` relative to the working directory; run it from the root. Smoke success requires ten actually presented frames. Occlusion, cancellation, timeout, early closure, or parse/upload/draw failure must fail; fallback geometry cannot satisfy target acceptance.
- `--scene-smoke` is a fixed adjacent-ground integration fixture, not translated scene selection. Read the ground research note before expanding material or scene coverage.
- For generated evidence, `AWL_CAPTURE_FRAME` selects a native BMP under ignored `build/`; it captures the first completed DX11 back buffer before Present. Clear the variable afterward.
- `AWL_PREVIEW_GPL` selects a supported logical `/files/...` path for the ten-frame `--preview-smoke`. It is a development preview, not target acceptance; `--target-smoke` must reject the override. Clear it after use.
- `tools/extract_disc.ps1` stages extraction, verifies Game ID/DOL hash, preserves existing extraction, then promotes. Never replace a valid extraction manually with unverified output.
- Reuse passing checks until changes, failures, or unresolved risk invalidate them. Review the final diff/new files and report exact evidence/gaps; builds and synthetic tests do not establish original-game equivalence.
- Close only task-created validation processes/helpers. Preserve user sessions. Keep the final handoff short and separate technical verification from actual gameplay acceptance.
