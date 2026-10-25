# GX core (P2 track): survey of the game's GX usage and the IR

Current state of the GX core track. Milestone 1 (survey and IR draft) is done; milestone 2 (command processor, XF on the CPU, BP/TEV decode, texture decoding, recording backend) is not started. Nothing here is derived from game bytes beyond register names, counts, format names and texture/viewport sizes; the streams themselves stay under ignored `build/`.

## 1. Tools

| What | Where | Use |
| --- | --- | --- |
| Stream capture | `src/hle/dev_gx.cpp` (`Capture`), format in `src/gx/gx_dump.h` | `AWL_GX_CAPTURE=<file>` writes the write-gather stream of the run, display lists inlined; `AWL_GX_FRAMES=<first>-<last>` limits it by retrace number (default all). Frames before `<first>` contribute only their CP/direct-XF/BP loads (prelude records), so a later start still has register state; matrices loaded through indexed XF loads before `<first>` are not reproduced. |
| Survey tool | `src/gx/gx_survey.cpp`, target `awl-gx-survey` | `awl-gx-survey <dump> [--out report.txt] [--frames]` decodes the stream (CP, XF, BP, vertex data) and reports commands, formats, state, textures, EFB copies and per-frame counts. It stops with the stream offset and register on anything it cannot decode. It reads no guest memory (indexed data is counted by index). |

Reproduce (from a worktree with the generated game library built, see `docs/re-pipeline.md`):

```powershell
$env:AWL_GX_CAPTURE='build\gxcap\all3000.gxd'
build\Release\awl-host.exe --dol <abs main.dol> --disc <abs disc> --retraces 3000
build\Release\awl-gx-survey.exe build\gxcap\all3000.gxd --out build\gxcap\survey-all3000.txt --frames
```

Register numbers and field layouts used by the decoder: YAGCD chapter 5 (CP, XF and BP register maps) and libogc `gx.h`/`gx.c` (what the SDK-compatible API writes). One correction to the P1 parser found while checking: the VAT C fields of texture coordinates 5-7 start at bit 4 (count), 5 (format), 8 (shift), 13/14/17 and 22/23/26; the P1 vertex-size code in `dev_gx.cpp` uses bit 5/6 for tex5, which is wrong. The game does not use texcoords 5-7 (vertex formats below), so P1 was unaffected; it is replaced in milestone 2.

## 2. How far the run gets

`awl-host` is deterministic (same 1,954,052,100 instructions for `--retraces 700` with and without capture). With `--retraces 3000` it runs to **retrace 1033 and then stops** with `NON-LOCAL RETURN` (instruction 2,798,279,835): `blr` with LR different from the shadow-stack return address, in `0x801B5A68` (hmawl label `OSGetResetButtonState`, called from the HSD main loop through `0x8000CCA4`; the function branches into `func_801B5AD4` inside itself) at pc `0x801B5CFC`. This is a P1 platform/generator issue, not GX; it is the blocker for seeing anything after the logos. The last disc reads before it are `sound/StreamHeader.arc`, `sound/JYOUCHUU.arc` and `.samp`.

What is drawn before the stop (1,015 retraces captured, 1,110 draws, 805,600 FIFO bytes, 0 display-list calls):

- Retraces 0-18: no draws; one EFB copy with clear (black/white), then idle clears.
- Retraces 19-1029: the **three start-up logos**, 300+ retraces each, as textured quads with fades between them (1 quad per frame while a logo is shown, 2 quads during the cross-fade). The three logo images are 359x127, 431x147 and 330x136 texels, RGB5A3, clamp/clamp, linear magnification.
- Retraces 1030+: nothing (the stop follows at 1033). **The title screen and menus are not reached**, so the survey describes the logo screens only. Everything the title uses (3D scenes, lighting, indexed arrays, display lists, more TEV, texture formats other than RGB5A3) is unobserved.

## 3. What the game uses (observed)

Commands: NOP, `LOAD_CP_REG`, `LOAD_XF_REG`, `LOAD_BP_REG`, vertex-cache invalidate, draw (quads only). Not seen: indexed XF loads, call display list, all other primitives.

Vertex formats (VAT 0 only, two descriptors): (a) position direct 2-component s16 (shift 0), color0 direct RGBA8888, texcoord 0 direct 2-component f32 (16 bytes per vertex); (b) position direct 3-component s16 (6 bytes) with no color or texcoord. No indexed attributes, no normals, no matrix-index bytes; array registers (0xA0-0xBF) written once at init with zero strides.

CP/XF: matrix-index defaults (CP 0x30/0x40, XF 0x1018/0x1019) written per draw; direct XF matrix loads of 12 words at matrix 0 and 8 words at matrix 30 (a 2x4 texture matrix) per frame; one normal-matrix load and one post-transform load at init. Projection (XF 0x1020-0x1026) written per draw: orthographic (the game's 2D projection) for every draw; a perspective projection is loaded each frame but not used by a draw. Viewport 640x448 at (0,0) (once 640x480), depth range written in two variants. One lighting channel: color0 and alpha0 take the vertex color, lighting off, no lights loaded; channel 1 never used. One texgen: regular 2x4 ST from tex0, post-transform matrix row 61 with dual-transform enabled, no normalisation. No emboss or color texgen.

BP / pixel pipeline: GEN_MODE 1 TEV stage, 0 or 1 texgen, 1 color channel, 0 indirect stages, cull = hardware value 1 (cull back). TEV: 1 stage; textured draws `color = TEXC*RASC`, `alpha = TEXA*RASA` (a=ZERO, b=TEXx, c=RASx, d=ZERO, add, bias 0, scale x1, clamp, dest prev); untextured draws `color = RASC`, `alpha = RASA`. Default swap tables (identity, rrra, ggga, bbba); konst/reg colors not used. Alpha compare always true; z test on, LEQUAL, write on for textured quads and off/ALWAYS for the plain ones; blend enabled src-alpha / inv-src-alpha, dither on, color and alpha update on, no logic op, no constant destination alpha; EFB format RGB8_Z24, z compare after texture; fog none; scissor (0,0)-(639,447) with no box offset; line/point size 6 (1/6 pixel). Texturing: texmap 0 only, wrap clamp, mag linear, min near_mip_lin with max LOD 0 (one level), TMEM cache configuration 1x3, no TLUT loads, no indirect state beyond the init writes, texcoord scale equal to the texture size. 160 distinct BP registers are written, all inside the documented map (init writes the whole TEV/texture/indirect register file once).

EFB copies: all 1,013 are **copy to XFB with clear**, source (0,0) 640x448, y-scale 1.0, gamma 1.0, clamp top and bottom, EFB format RGB8_Z24; clear color is white (most frames) or black/dark during fades, z clear 0xFFFFFF with z write on; vertical filter coefficients {8,8,10,12,10,8,8} (the deflicker filter is on) and the default sample pattern 0x666666. No copies to texture. BP PE_TOKEN and PE_TOKEN_INT are written once per frame; PE_DONE (finish) is never written in this range (the host reports 0 finishes).

Per-frame cost of the stream: at most 2 draws and 8 vertices per frame; the GX core's cost in this range is negligible.

## 4. Not observed (the IR defines them, the core will implement them in milestone 2 where the title screen will need them, but no real stream validates them)

Indexed vertex attributes and arrays, matrix-index bytes, normals, per-vertex colors other than RGBA8888, lights, specular, multi-texgen and projective (STQ) texgen, bump/emboss and color texgen, more than one TEV stage, konst colors and registers, swap tables other than the defaults, TEV compare ops, indirect texturing, fog, logic ops, alpha test, 3D projections in draws, triangles/strips/fans/lines/points, display lists, EFB copies to texture (and every copy format), texture formats other than RGB5A3 (I4, I8, IA4, IA8, RGB565, RGBA8, C4, C8, C14X2, CMPR), TLUTs, mip levels, scissor changes.

## 5. IR

`src/gx/gx_ir.h` (version 1, draft) defines the per-draw and per-copy commands, the pipeline state, the textures and the backend interface, with the numbered conventions (clip space and depth range, viewport mapping, colors, texcoords, winding and culling, texture format and EFB-copy references, ordering and lifetime) and a coverage table (Observed / Defined / Rejected). `tests/gx_ir_tests.cpp` checks that the header compiles at `/W4` and that the documented encodings and the callback order hold.

## 6. Open points

- The title screen is unreachable until the `OSGetResetButtonState` stop is fixed (P1 platform/generator); the survey must be repeated afterwards.
- Face-culling sign (IR convention 6) and the exact scissor/viewport offsets (convention 2) follow libogc and YAGCD; the logo quads (cull = back, observed) are the first check in milestone 2, against Dolphin screenshots.
