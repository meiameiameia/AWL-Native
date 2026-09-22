# Reverse engineering workflow

The target is the NTSC-U `GYWE41` game. The verified `main.dol` SHA1 is `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`. Work from the owner's local game image and keep binaries, extracted files, Ghidra projects, decompiler output, and screenshots in ignored directories.

1. Extract the disc with [`tools/extract_disc.ps1`](../tools/extract_disc.ps1). It stages the extraction, checks the Game ID and DOL hash, and preserves any existing extraction until validation succeeds. Do not manually replace `disc/` with unverified output.
2. Verify `rom/main.dol` separately with `certutil -hashfile rom\main.dol SHA1` before tracing DOL addresses. The Ghidra project is local under ignored `docs/ghidra/`; import the verified DOL with a GameCube loader and run analysis there.
3. Use the Ghidra scripts in `tools/` to export a function's decompiler output, instructions, references, or scalar matches to ignored `build/` output. For follow-up headless traces of the existing project, use `-noanalysis -readOnly`. Record target address, exact instruction behavior, tool version, and confidence in a compact research note. [`ground-rendering.md`](research/ground-rendering.md) is the current ground path evidence.
4. Translate only a bounded behavior that has a justified native equivalent. Test parser/state assumptions independently, verify the relevant local asset, then run the matching bounded DX11 smoke. A passing build or image alone does not prove original-game equivalence.

The [hmawl project](https://github.com/ChrisNonyminus/hmawl) may be consulted for names and addresses. Do not copy its source or Git history into this repository. The native port uses its own implementation and build system.
