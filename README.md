# AWL Native

An experimental native Windows port of *Harvest Moon: A Wonderful Life* (NTSC-U, Game ID `GYWE41`). This project is in an early development stage and is not yet a playable game.

The current executable is a Win32/DirectX 11 development harness. It can load selected assets from a locally extracted game disc and exercise bounded rendering checks. These checks do not establish full game behavior or rendering fidelity.

## Build and test

With Visual Studio 2022/MSVC, CMake, and the Windows SDK installed, run from the repository root in PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The asset smokes require your own verified extraction under the ignored `disc/` directory. See [the extraction workflow](tools/extract_disc.ps1) and [the project guidance](AGENTS.md) for current constraints. Game binaries and assets are not included.

This is a separate native port project. The [hmawl decompilation](https://github.com/ChrisNonyminus/hmawl) was consulted as a read-only reference for names and addresses; this repository does not contain its Git history.
