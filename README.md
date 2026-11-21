# AWL Native

**A native Windows port of *Harvest Moon: A Wonderful Life*, built by statically recompiling the original GameCube code.**

The goal is to play the game on a modern PC without an emulator in the loop: native performance, a clean installer and settings, opt-in enhancements such as higher resolution and widescreen, and mod support. By default it should look and behave exactly like the original. The target is the North American GameCube release (`GYWEE9`). This is an unofficial fan project. You need your own copy of the game: no game code or data is included here, now or later.

> **Status: early development, not yet playable.** The whole game program is recompiled to native code and runs in a native window: the logos and the title screen draw at full speed. Starting a new game now plays the opening scenes up to the moment you first control the hero. There is no saving and no sound yet.

## How it works

1. **Static recompilation.** A recompiler in [`tools/recomp`](tools/recomp) reads the game's PowerPC executable and its three code modules from your disc. It turns every function (about 15,600 of them) into C++, which MSVC compiles into a native Windows program. Game logic runs as the original logic, so its behavior stays faithful without hand-rewriting the game.
2. **A native platform layer.** The parts of the GameCube that the game talks to (the operating system, threads and interrupts, disc reads, controllers, video timing, audio, memory card and the GX graphics chip) are replaced by native Windows code in [`src/hle`](src/hle). Rendering goes through DirectX 11.
3. **Checked against the original.** Behavior is compared with the real game running in an emulator, used purely as an outside reference. Its code is never copied. Every deliberate difference and every temporary placeholder is written down.

The recompiled code is generated on your machine from your own disc. It is never committed or distributed.

## Progress

| Phase | Goal | Status |
| --- | --- | --- |
| P0 | Prove that recompilation works for this game | ✅ Done |
| P1 | The recompiled game boots into its main loop | ✅ Done, checked against the original |
| P2 | Graphics: logos, title screen and menus in a game window | ✅ Done |
| P3 | A playable farm: input, saves, gameplay | 🔧 In progress: New Game reaches the farm; no saving yet |
| P4 | Music and sound | Planned |
| P5 | Installer, first-run setup from your disc, settings, mod loading | Planned |
| P6 | Enhancements: resolution, widescreen, texture packs, code mods | Planned |

This table is updated when a phase is finished.

## Building (developers)

There is no playable release yet. These steps are for following development.

Requirements: Windows, Visual Studio 2022 (MSVC), CMake, the Windows SDK, and your own disc image of the NTSC-U game.

```powershell
# 1. Put your disc image (ISO or RVZ) in rom\, then extract it into disc\.
#    The script verifies the game ID and the executable's hash. It uses DolphinTool for the extraction
#    (pass -DolphinToolPath if yours is not at C:\tools\Dolphin-x64\DolphinTool.exe).
powershell -File tools\extract_disc.ps1
Copy-Item disc\sys\main.dol rom\main.dol

# 2. Build the tools and run the tests
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# 3. Recompile the game from your files (output stays in the ignored build\ folder)
build\Release\awl-recomp.exe gen --full --dol rom\main.dol --rel-dir disc\files --out build\recomp-gen
cmake -S . -B build
cmake --build build --config Release --target awl-host

# 4. Run it in a window (Enter = Start, Space = A, Backspace = B, WASD = stick, F11 fullscreen, Esc quits)
build\Release\awl-host.exe --dol rom\main.dol --window
```

`rom/`, `disc/`, `build/` and everything generated from the game are ignored by Git.

## Earlier work

The project began as a hand translation of individual game systems into C++. That code (`src/world_map_*`) is kept as reference; the ground-rendering and asset-format work from that phase fed into the graphics phase.

## Credits and legal

*Harvest Moon: A Wonderful Life* belongs to its rights holders; this project is not affiliated with them. The [hmawl](https://github.com/ChrisNonyminus/hmawl) project was consulted read-only for function names and addresses; no code was copied from it. No license has been chosen for this repository yet.
