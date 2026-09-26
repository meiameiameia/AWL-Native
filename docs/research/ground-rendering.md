# Ground rendering: verified native subset

Target: NTSC-U `GYWE41` `main.dol`, SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`. The findings below come from bounded assets, local Ghidra 12.1.2 traces of that DOL, independent GX references, parser tests, and native DX11 smokes. Raw DOL, disc assets, decompiler output, and captures remain local under ignored paths.

## What the harness implements

The GPL path accepts exactly one complete section and the observed unlit ground layouts. It parses an ordered material command stream, binds selected TPL images, converts each verified display-list range to a triangle list, and submits per-batch texture and material state. Unknown structures, unsupported state, truncated ranges, and partial assets fail validation. `--target-smoke` is the fixed `jimen-L-0-0-3.gpl` check; `AWL_PREVIEW_GPL` with `--preview-smoke` is an isolated development preview. Each successful smoke presents ten frames.

This is not general GPL/GX support. Type `0x80`, other TEV words, lit layouts, non-ground meshes, original scene selection, gameplay camera/projection, and broader game state are untranslated.

## Reproducing the binary evidence

Verify `rom/main.dol` against the SHA1 above before using Ghidra. Import it with a GameCube DOL loader into a local Ghidra project. The scripts in `tools/` can export one function's decompiler output, instructions, references, or scalar matches. Run headless traces with `-noanalysis -readOnly`, and write output under ignored `build/` or `docs/ghidra/`. Record the function address, actual instruction behavior, and uncertainty before extending the parser or renderer. The relevant addresses are listed below so the findings can be rechecked from the owner's binary.

## Asset and command structure

- `FUN_801A3DB8` resolves GPL pointers relative to the section base. The material header's `+8` big-endian `u16` is the command count; serialized commands are 16 bytes. `FUN_801A4078` expands them to 20-byte runtime records.
- `FUN_801A3000` executes commands in order. Type 1 draws its attached range with the active texture, then installs its replacement texture. Type 2 installs VCD/VAT and then draws. Type 3 installs TEV state and then draws. Every nonempty draw needs a known texture, supported VCD, verified TEV state, and an exact in-bounds range. The first draw pointer agrees with the material header.
- `FUN_801A50E8` uses the low 13 bits of a type-1 word as the zero-based TPL image index and bits 13–15 as texture unit. `FUN_801A5F6C` resolves the eight-byte descriptor. For `jimen-L-0-0-3.gpl`, command word `0x11110002` selects image 2 in `image.jimen-L_s1.tpl`.
- `FUN_8001BAE4`, `FUN_8001BF3C`, and `FUN_8001C174` establish the 20 ground TPL names and the 4×10 table at `0x802994F0` that selects the `@ground.tpl` alias. The native harness supplies `(period=0, step=0)` for development; the gameplay meaning and producers of those axes are unproven.

### Supported vertex descriptors

`FUN_801A5950` decodes the type-2 word; `FUN_801A4C94` binds arrays and emits VAT state. Serialized references use the GX attribute order below. `P`, `N`, `C`, and `T` mean position, normal, COLOR0, and Tex0; `8`/`16` are big-endian index widths in bits.

| VCD | Reference layout | Bytes | Evidence asset |
| --- | --- | ---: | --- |
| `0x828` | P8 N8 T8 | 3 | `jimen-L-0-0-3.gpl` |
| `0x8A8` | P8 N8 C8 T8 | 4 | `jimen-L-0-0-1.gpl` |
| `0x8AC` | P16 N8 C8 T8 | 5 | `jimen-L-2-0-0.gpl` |
| `0x8EC` | P16 N8 C16 T8 | 6 | `jimen-L-3-0-1.gpl` |
| `0xCEC` | P16 N8 C16 T16 | 7 | `jimen-L-2-0-1.gpl` |

`0x828` has one constant Sub[1] color, so no per-vertex color index is serialized. The earlier interpretation of its middle byte as a color index was disproven by the DOL VCD and array bounds. The normal index is retained but the verified unlit path does not consume normal vectors in DX11. Multi-element Sub[1] selects indexed COLOR0. `FUN_801A59C0` decodes supported RGB565 and RGBA4 by bit replication; `FUN_801A5B5C` gives each a two-byte stride. Some RGBA4 assets serialize zero alpha throughout, so an empty-looking isolated preview is expected. Unsupported formats and out-of-range indices fail.

For `jimen-L-0-0-3.gpl`, VCD `0x828` points to one exact display-list range at file offsets `0x200..0x360`: five VAT-0 primitives, 103 references, 153 DX11 indices, and 28 GX NOP bytes. Quad, triangle, strip, and fan records are supported only at their validated boundaries. `jimen-L-1-0-0.gpl` demonstrated why a type-2-only parser is insufficient: its type-2 range is 32 bytes, followed by a type-1 attached range of `0x2E00` bytes.

## Material, texture, and presentation state

- The target's type-3 word `1` selects one texture-times-raster TEV stage: `FUN_801A714C` and `FUN_801A6E8C`. `FUN_801A4C94`, `FUN_801A32CC`, and `FUN_801A7BA4` select either registered material color or per-vertex COLOR0 with lighting disabled. The target's RGB565 `0xB659` becomes RGBA `(181,203,206,255)`. Other TEV words and lit layouts are rejected.
- `FUN_801A57D4` expands the selected TPL image's wrap, filter, and LOD fields. Target image 2 is a 1024×1024 CMPR texture with four complete mip levels (total 696,320 encoded bytes). The verified state is repeat S/T, linear min/mag and mip filtering, LOD `0..3`, bias `-1.0`. The native loader validates the full chain and rejects unsupported sampler values.
- `FUN_8017A770` sets LEQUAL depth testing with writes; `FUN_8017A6F0` sets source-alpha/inverse-source-alpha blending. `FUN_801D1F1C` and `FUN_801D1E70` write the corresponding GX PE state. The DX11 target uses a resize-matched depth buffer and that demonstrated blend/depth combination. Other PE modes are outside the slice.
- The `ObjectGround` constructor `FUN_8014C5D8` creates ground objects without per-chunk placement. `FUN_801A2A4C` initializes each model matrix as identity; the draw path `FUN_8014C2B4` → `FUN_801A2F78` → `FUN_801A2CFC` multiplies it by the active view matrix. Decoded GPL positions therefore already carry world coordinates. The current bounds-fit camera is diagnostic. `FUN_8017B6D4` and `FUN_8017B834` are part of the still-untranslated gameplay camera flow.

## Current integration fixture and checks

All 15 nonempty observed `jimen-L-*` ground GPLs passed bounded isolated previews with one, two, or five ordered draw batches. `jimen-L-0-0-0.gpl` is material-only. This is an observed asset set, not a claim about all GPLs.

`--scene-smoke` assembles `/files/jimen-L-1-0-2.gpl` and `/files/jimen-L-2-0-2.gpl` in their serialized world coordinates. Their X bounds meet at approximately 123–125 units. It uploads seven unique texture slots and submits ten batches, 26,125 vertices, and 41,079 indices. Both selected materials use indexed COLOR0; the fixed target separately exercises constant color. Scene selection and camera remain fixed diagnostics.

For the [first playable slice candidate](first-playable-slice.md), a local Debug scene smoke again presented ten frames. The X `120..128`, Z `168` line crosses the fixture seam and has primary height samples in both local movement COL variants. This is only a candidate development route: scene selection, spawn, accepted movement, and a gameplay camera remain untranslated.

The final recorded MSVC Debug and Release `/W4` builds and all four CTest targets passed. Target and scene smokes each presented ten frames in both configurations; Debug and Release captures matched within each mode. The target BMP SHA256 was `030F75AC34646E2C22BF333ED2B9114AEB07523EDD8F59C8232B6CE25A729FF2`; the two-chunk scene BMP SHA256 was `50607F8A48ADA286574E79DDBC7B948D47DE21BD80126F4C7DDBFD60BB0E36BD`. These hashes identify the recorded captures, not universal gameplay correctness. Synthetic tests exercise exact index widths, command order, array bounds, partial ranges, and failure cleanup.

Independent GX cross-checks: [Dolphin CP and opcode decoding](https://github.com/dolphin-emu/dolphin/tree/master/Source/Core/VideoCommon), [Dolphin texture decoding](https://github.com/dolphin-emu/dolphin/tree/master/Source/Core/VideoCommon), and [libogc GX declarations](https://github.com/devkitPro/libogc/blob/master/gc/ogc/gx.h). Do not infer unsupported behavior from those references alone.
