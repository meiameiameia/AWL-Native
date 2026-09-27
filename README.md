# AWL Native

**A work-in-progress native Windows port of *Harvest Moon: A Wonderful Life*.**

The aim is to bring the original game's behavior to a modern Windows executable, one verified system at a time. The target is the North American GameCube release (`GYWEE9`). This is an unofficial project and requires a local copy of the target game data.

> **Current status: pre-playable.** The application opens a native window, renders selected ground assets with DirectX 11, and reads keyboard and controller input. A fixed development scene now moves a temporary marker over terrain, but input does not yet move a playable character in the native game.

## What works today

- A Win32 and DirectX 11 development application with clean startup and exit.
- Local loading of selected files from an extracted disc; game files stay outside Git.
- Partial GPL/TPL ground rendering, with bounded ten-frame rendering checks.
- Native keyboard and XInput translated to GameCube-style pad input.
- Isolated, tested movement and collision pieces, including terrain height, object contact, room collision lookup, and scene-position bookkeeping.
- A fixed development scene where keyboard or controller input moves a temporary marker across a tested ground seam; focus loss pauses it. This is a rehearsal with a fixed camera and incomplete collision objects.

These pieces are development evidence. They do not yet add up to original-game movement, a complete scene, or a playable port.

## Road to the first playable build

| Gate | Outcome | Status |
| --- | --- | --- |
| 1. Collision foundation | Verify the terrain radius response in isolation. | Complete |
| 2. Movement acceptance | Connect the original movement decision to justified terrain, object, state, and scene data. | In progress |
| 3. Development scene | Move a temporary marker through a fixed scene with a controlled camera. | Complete as a rehearsal |
| 4. Player presentation | Render the verified player resources and idle/walk states. | Planned |
| 5. First interaction | Add one verified inspect-and-return action, then check startup, control, and exit together. | Planned |

The [first playable plan](docs/research/first-playable-slice.md) records the current gate, evidence, and remaining work. A development scene or a passing automated test is a milestone on this road; actual gameplay acceptance requires an in-game check.

## Build and test

Use Windows with Visual Studio 2022/MSVC, CMake, and the Windows SDK. From the repository root in PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

With a verified local extraction, run `build\Debug\awl-decomp.exe --movement-rehearsal` to try the development marker. **D** moves it right across the visible seam; **Esc** exits. This mode does not represent the original player or scene.

The executable is a development harness, not a game release. Asset-backed checks require your own verified local extraction in `disc/`. The [extraction script](tools/extract_disc.ps1) checks the game ID and executable hash before preserving or promoting an extraction. `rom/`, `disc/`, and build output are ignored; no disc image, game binary, or extracted asset is supplied here.

## Approach

We trace behavior against the verified game executable, translate only bounded parts with clear evidence, and test them on Windows before connecting them to gameplay. DirectX 11 handles rendering, native file I/O replaces disc access, and unsupported asset structures fail explicitly. The [research notes](docs/research/input-player-trace.md) separate traced behavior from translated helpers and connected runtime behavior.

The external [hmawl repository](https://github.com/ChrisNonyminus/hmawl) has been consulted read-only for names and addresses. No code or Git history was copied from it.
