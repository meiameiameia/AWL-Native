# AGENTS

This root file is the canonical repository guidance. Keep task-specific plans and results in the task; update this file only for durable project facts, decisions, or risks.

## Working agreement

- Lead with the verified outcome, material tradeoffs, remaining risk, and any action the owner must take; do not expect the owner to read code.
- Trust the current code, configuration, binary evidence, and executed checks over status notes. `ARCHITECT_NOTES.md` is historical and may be stale.
- Preserve existing committed, uncommitted, and untracked work. Inspect the relevant diff before editing; never reset, clean, mass-format, or overwrite unrelated work.
- For implementation, choose the smallest coherent reversible slice, then review the diff and run targeted checks followed by the required build or smoke checks. Report checks as passed, failed, or not run.
- Own scoped technical investigation and verification. Ask the owner only when an unresolved choice materially affects intent, compatibility, data, cost, or authority.
- Require an explicit owner request before using a separate independent or external audit as a completion gate.
- Do not commit, push, publish, deploy, or change external data unless explicitly requested.

## Project and current stage

The goal is a behaviorally faithful native Windows port of *Harvest Moon: A Wonderful Life* (NTSC-U, Game ID `GYWE41`). The verified target DOL SHA1 is `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`.

The project is an early functional scaffold, not a playable port. The current executable is primarily a development harness: it creates a Win32/DX11 window, mounts the extracted disc tree, decodes selected TPL/GPL assets, and exercises a debug mesh rendering path. Placeholder subsystem calls are not verified game behavior.

The native loop maps keyboard and first-controller XInput state to a raw first-channel GameCube-style PAD sample and applies a DOL-backed subset of stick/trigger filtering, synthesized direction bits, and button transition/repeat state. A separate translated helper derives normalized world-map steering and stepped speed, but it is not connected to a gameplay scene. The controls themselves are a PC policy; camera-relative movement, collision, actions, scene-dependent repeat updates, and remaining PAD behavior remain untranslated. `FUN_8012f3e0` was checked against the verified DOL and must not be described as PAD initialization.

The ground GPL/TPL path is an evidence-backed but deliberately narrow subset.
Before changing it, read `docs/research/ground-rendering.md` and
the relevant current code and tests. Coverage claims apply only
to explicitly validated layouts and assets. Passing builds, synthetic tests,
or isolated previews does not establish broader GX fidelity or original-game
equivalence.

Unsupported structures must be rejected explicitly. Do not treat successful
parsing of one section or draw range as validation of the complete asset; the
current analysis path requires exactly one GPL section. Type `0x80`, other TEV
words, lit and non-ground layouts, general scene selection/assembly, gameplay
camera/projection, and broader game state remain untranslated. The current
bounds-fitted camera is diagnostic rather than original game behavior.

The bounded `--scene-smoke` assembles two adjacent verified ground chunks in
their serialized world coordinates with per-batch texture and material state.
It is a fixed integration fixture, not translated scene selection. Read
the scene section of `docs/research/ground-rendering.md` before extending scene coverage.

## Important paths

- `src/`, `include/awl/` — native port and platform equivalents
- `docs/research/ground-rendering.md` — compact ground evidence and supported boundary
- `docs/re-pipeline.md` — reverse-engineering workflow
- `tools/` — extraction and Ghidra helpers
- `rom/`, `disc/` — local copyrighted binary/assets; never stage, commit, copy into tests, or expose
- `build/` — generated CMake output

## Native Windows workflow

Prerequisites are Visual Studio 2022/MSVC, CMake, and the Windows SDK. Use PowerShell and native Windows tools; do not introduce WSL, Docker, Hyper-V, or another virtualization layer.

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Debug\awl-decomp.exe --target-smoke
build\Debug\awl-decomp.exe --scene-smoke
build\Debug\awl-decomp.exe
certutil -hashfile rom\main.dol SHA1
```

Run the executable from the repository root because the current development mount is relative to `disc/`. A renderer/asset change is not validated solely by compilation: use relevant parser tests plus a bounded runtime smoke, and close every process started for validation.

For non-interactive visual evidence, set `AWL_CAPTURE_FRAME` to a native BMP
path before `--target-smoke`. The renderer reads back the first completed DX11
back buffer before `Present`; keep generated captures under ignored `build/`
output and clear the environment variable afterward.

For a bounded development preview of another supported GPL, set
`AWL_PREVIEW_GPL` to its logical `/files/...` path and run
`--preview-smoke`. It renders ten frames and exits; it is not target
acceptance, and `--target-smoke` rejects this override.

Validation smoke success requires ten actually presented frames. Cancellation,
occlusion, render failure, timeout, or early window closure must fail rather
than silently approve an incomplete run.

`tools/extract_disc.ps1` extracts to an isolated ignored staging directory,
verifies Game ID and the target DOL SHA1, preserves any existing extraction,
and only then promotes the staged result. Do not bypass that staged workflow or
manually replace a valid extraction with unverified output.

## Reverse-engineering and implementation constraints

- Decompiled behavior must trace to the actual verified DOL/REL disassembly. Record original addresses and confidence where applicable; label hypotheses and unknowns.
- Never invent function bodies or present guessed responsibilities, placeholder logs, or debug scaffolding as completed decompilation.
- `hmawl` is read-only for names and addresses only. It has no license; do not copy or fork its code.
- GX, VI, OS, PAD, DVD, audio, and memory behavior require justified PC equivalents, not empty stubs disguised as completion. DirectX 11 is the renderer and standard native file I/O replaces DVD access.
- Prefer existing/native facilities. Add no dependency without a concrete material benefit and owner approval for a production dependency.
- Keep ROMs, extracted disc files, credentials, machine-private data, and copyrighted debug payloads out of commits and reports.
- Do not expand material or scene coverage on unverified decoder or GX command-layout assumptions. Resolve the relevant state and command boundaries for each evidence-backed slice.
- A required asset, parse, upload, or draw failure must make its validation mode fail; debug fallback geometry cannot satisfy the target smoke check.
- Do not weaken warnings, bounds checks, tests, validation, or error reporting to make a result pass.

## Completion standard

MSVC Debug and Release builds must compile cleanly at `/W4`. Test decompiled or translated subsystems independently when practical. For asset/rendering work, validate decoded bounds and topology and exercise the visible DX11 path with local assets when available. Synthetic tests must use independently justified expectations rather than repeating the implementation. Reuse verification evidence that remains valid for the final state; repeat checks after invalidating changes, failures, or unresolved relevant risks. Review the final diff for scope, evidence, generated artifacts, secrets, and preservation of pre-existing changes.

Canonical background: [ground rendering evidence](docs/research/ground-rendering.md) and [reverse-engineering workflow](docs/re-pipeline.md).

Input status: [the DOL-backed PAD-to-player trace](docs/research/input-player-trace.md) identifies polling, a translated first-channel HSD filtering subset, a translated but inactive steering/speed core, world-map player ownership, and state dispatch. Scene-dependent repeat updates, remaining HSD status handling, action mapping, camera-relative position updates, and collision remain untranslated.
