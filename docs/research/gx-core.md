# GX core (P2 track): survey, IR and command processor

Current state of the GX core track. Nothing here is derived from game bytes beyond register names, counts, format names and texture/viewport sizes; streams, IR dumps and decoded textures stay under ignored `build/`.

## 1. What exists

| Part | Where | Role |
| --- | --- | --- |
| IR | `src/gx/gx_ir.h` (v2, frozen, architect-owned) | per-draw / per-copy commands, pipeline state, textures, `Backend` interface |
| Command processor | `src/gx/gx_core.{h,cpp}` (`Core`) | consumes the write-gather byte stream (display lists included), CP/XF/BP state, completion services through `CoreHost` |
| Vertices, XF, primitives | `src/gx/gx_draw.cpp` | vertex loading (direct/indexed), position/normal matrices, projection, lighting channels, texgen, quad/strip/fan/line/point conversion, face culling |
| Pipeline and copies | `src/gx/gx_pipeline.cpp` | BP/XF registers -> `PipelineState` (viewport, scissor, depth, blend, alpha compare, fog, TEV, indirect, texture maps), EFB copies, aliasing of copy textures |
| Textures | `src/gx/gx_texture.cpp` | decode of I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8 C14X2 CMPR (+ TLUT, mips) to RGBA8, content-hash key, LRU decode cache (192 MB) |
| Recording / hub | `src/gx/gx_record.{h,cpp}` | `RecordingBackend` (text dump per retrace, texture PNGs), `BackendHub` (`awl::gx::backendHub()`) |
| Platform glue | `src/hle/dev_gx.cpp` | write-gather port, CP/PE registers, finish/token/copy completion (unchanged P1 semantics), guest memory for the core, stream capture, IR dump options |
| Capture / survey | `src/gx/gx_dump.{h,cpp}`, `src/gx/gx_survey.cpp` | raw stream dump and the `awl-gx-survey` console tool |
| Tests | `tests/gx_ir_tests.cpp`, `tests/gx_core_tests.cpp` | hand-assembled streams; expectations from YAGCD/libogc arithmetic (see the comments in the tests) |

**Renderer hookup.** The platform delivers every command to `awl::gx::backendHub()`; a renderer calls `backendHub().add(&backend)` before the run starts (backends are called synchronously on the guest thread, in stream order; pointers in the commands are valid only during the call, `shared_ptr<const Texture>` may be kept). Errors in the stream throw `GxError`, which `dev_gx.cpp` turns into a hardware stop with the stream offset and register/command.

**Options.** `awl-host ... --gx-dump N [--gx-dump-from F] [--gx-dump-dir D]` writes `frame_<retrace>.txt` (IR text, state printed when it changes, up to 64 vertices per draw) and `tex_<key>.png` (level 0 of each bound texture) for retraces F..F+N-1 (default dir `build/gxdump`; `AWL_GX_DUMP_DIR` also works). `AWL_GX_STATS=1` prints the core's counters and cost at exit. `AWL_GX_CAPTURE=<file>` / `AWL_GX_FRAMES=<first>-<last>` write the raw stream (`gx_dump.h`) for `awl-gx-survey <dump> [--out report] [--frames]`.

## 2. How far the run gets, and what the game uses (survey)

`awl-host` is deterministic. After the DX11 track's interrupt-frame fix the run passes the old `OSGetResetButtonState` stop and now ends at **retrace 1378** with a memory fault on an EFB Z peek (`0xC848B668`, `0x801CE03C` from `0x800DA1D4`, `0x8019B8F8`, `0x8017A770`; platform work in progress). Captured: 1,149 frames with commands, 939,781 draws, 19,964,059 vertices, 334,434 display-list calls (125 MB of list data), 21 MB of FIFO bytes. The core processed everything without a stop.

| Retraces | Content |
| --- | --- |
| 0-18 | no draws; clears to XFB |
| 19-1029 | the three start-up logos (see below) |
| 1030-1116 | no draws (one clear per frame) |
| 1117-1376 | a **3D scene**, one frame every 2 retraces (30 fps), about 6,700-7,600 draws and 148,000-162,000 vertices per frame; 131 copies to a 64x32 texture and 1,144 copies to XFB over the whole run |

**Logos (19-1029).** Textured quads, orthographic projection, one TEV stage, RGB5A3 images 359x127, 431x147 and 330x136 (linear, clamp). Decoded they read "LICENSED BY NINTENDO" and the Natsume logo (eyeballed on the dumped PNGs).

**3D scene (1117-1376), observed:**
- Commands: display lists (called 334,434 times), CP/XF/BP loads, indexed XF loads: none. Primitives: triangle fans 398k, strips 350k, quads 139k, points 28.6k, triangles 24k; no lines. Vertex formats (38 distinct, all VAT 0): positions indexed 8/16-bit s16 xyz with shifts 4-14 (or direct f32), normals indexed 8-bit f32 (or s16), color0 indexed RGB565 or RGBA4444 (8/16-bit indices), texcoords indexed s16 >> 14 / s16 >> 6..9 / f32 / s8 >> 6 / u8, a few direct formats (pos xy f32 + st f32; pos xyz f32 only for the points). No matrix-index bytes and no color1. Arrays: pos 107 distinct bases (max index 5015), tex0 108, col0 66, nrm 3; strides 2, 4, 6, 8, 12.
- XF: perspective projections (4 parameter sets) and the 2D orthographic one; position matrices 0-24 (every 3 rows) reloaded per object, a normal matrix per draw (105k loads), 2x4 texture matrices 30/33/36; **one light** (light 0 and light 2 loaded 131 times) with the channel's attenuation bit set in spotlight mode (diffuse clamp), color0 material from vertex or register, alpha unlit; 1 texgen (regular ST from tex0 through the post matrix) or 3 texgens (position source, ABC1 form) for the indirect effect.
- Pixel pipeline: 1 TEV stage (TEXC*RASC / TEXA*RASA, or C0*TEXC for the 28.6k point sprites, C0/C1 konst-register blends for 11.8k draws) and a 2-stage indirect effect (17,161 draws: 2 indirect stages with matrix 1, bias 3, wrap 0/6; map 0 IA8 128x128 repeat and map 1 I8 128x128 clamp); swap tables with the rswap/tswap = 1 selection; alpha compare variants (a > 127 for 739k draws, always, 1..255 gequal/lequal); z lequal with write (879k draws), no-write (29k) or off; blend off (863k), src-alpha/one (46k), src-alpha/inv-src-alpha (31k); **linear fog** (type 2, perspective) on 860k draws; cull back (517k), none (373k) and front (2.6k); EFB RGB8_Z24 throughout.
- Textures (28 formats/sizes, 43 distinct images): CMPR 32x32 to 1024x1024 with mips (lin_mip_lin, max LOD 3, some with LOD bias, repeat), RGB565 4x4 and 8x8, I4 1008x648, I8 and IA8 128x128, IA4 32x32 and 64x64, RGBA8 64x32 (the render-to-texture target, 2,620 draws sample it via the copy alias), RGB5A3 (logos, 292x15, 512x325, 64x32). No TLUT loads, no color-index textures.
- Points: size 96/48 (1/6 px) with texture offset; the 28.6k points are drawn with a 4x4 RGB565 texture.
- EFB copies: XFB copies as in the logos; the render-to-texture copy is 64x32 RGBA8 with clear, scissor 64x32, vertical filter {21,22,21} defaults.
- Still unobserved: display-list-free XF indexed loads, matrix-index bytes, color1, STQ/emboss/color texgens, lines, TLUTs, compare-mode TEV ops, logic ops, specular/spotlight coefficients other than trivial (not checked), more than one light per channel.

## 3. Conventions settled by the implementation

- **Clip space and depth** (IR convention 1): confirmed on the game's own projection (ortho z' = z/1024 - 1 maps z=0 to -1, i.e. -w at w = 1) and in synthetic tests (libogc perspective: near -> z = -w, far -> z = 0). The 3D scene of retraces 1117-1376 has depth-tested draws (z lequal) that a renderer can now check.
- **Face culling** (IR convention 6): the core culls with the sign of the determinant of the (x, y, w) rows. Evidence from the game: its logo quads are submitted clockwise as seen on screen (top-left, top-right, bottom-right, bottom-left, y up after the projection) and survive GEN_MODE cull hardware value 1 (libogc `GX_CULL_BACK`), so **a front face is clockwise on the screen**. `gx_ir.h` convention 6 states this since version 2. The sign is one constant, `kFrontFaceDeterminantSign` in `gx_draw.cpp`; `gx_core_tests` fails if it is flipped.
- **Texcoords** (convention 4): the logo quads give (s,t) = (0,0)..(1,1) with SU_SSIZE/TSIZE equal to the texture size, as the IR states.
- **Scissor**: BP scissor corners minus twice the box-offset register (libogc stores (offset+342)>>1); observed (0,0)-(639,447).
- **Clear** (EFB copy): libogc forces z update on and blending off around a clearing copy, so the core clears color/alpha according to the CMODE0 update bits and z according to the ZMODE update bit at the copy.
- **Texture filters**: the min filter register holds libogc's hardware encoding (near 0, near_mip_near 1, near_mip_lin 2, linear 4, lin_mip_near 5, lin_mip_lin 6), mapped to the IR's GX enumeration; mip levels decoded = ceil(max LOD) + 1 capped by the size, only for mip filters.
- **Copy textures** (convention 8): a copy to a texture records destination, size and a hash of the destination memory; a texture bound at that start address with unchanged memory references the copy. When the memory changed, the bind reads RAM and a note is written once per copy to the boot trace (`gx: texture bound at EFB-copy destination ...`). Z-texture copies are recognised by the EFB format being forced to Z24 during the copy.
- **Texture IA formats**: IA8 and the TLUT IA8 entry have alpha in the first byte; IA4 has alpha in the high nibble (public decoders; no observed texture uses them). CMPR uses 5/8 and 3/8 interpolation like the earlier TPL decoder.


## 4. Cost (Release, Core::feed wall time, `AWL_GX_STATS=1`)

Whole run to retrace 1378 (heavy 3D frames dominate): 8.2 M commands, 939,781 draws, 19.96 M vertices, 119,261 pipeline rebuilds. Typical result 3.9-5.1 s in the core (machine load adds noise; one run with another game running took 80 s), that is about **15 ms per 3D frame** at 30 fps, split: vertices + XF + primitives about 2.7 s (about 130 ns per vertex, of which lighting about 55 ns), pipeline rebuilds about 0.6 s, backend calls 0.03 s. The logos cost about 34 microseconds per retrace.

Texture rehashing after the SDK's per-load invalidation was 1.5 s of 9 s (5.7 GB hashed); the core now keeps the raw bytes (and palette) of each texture binding's last decode and compares byte for byte (memcmp, exact), hashing only a new or changed binding: 43 MB hashed and 5.65 GB compared in the same run. The content-key semantics are unchanged. Remaining cost is per-vertex work; candidates if it must shrink: skip lighting for vertices whose normal/position are equal, a per-draw vertex cache for repeated index tuples, or moving the core to its own thread.

## 5. Open points

- The run ends at retrace 1378 on an EFB Z peek (platform track). When it is on main: merge, re-run the survey through the title and menus.
- Spot/specular attenuation, emboss and color texgens, compare-mode TEV, logic ops, TLUT paths and the IA4/IA8 bit order have no real stream (the 3D scene exercises spot-mode with trivial coefficients, fog, indirect texturing, CMPR mips and a copy texture, but only decoded; nothing is compared with the original yet).
- Not verified against Dolphin: all of the above. The owner's Dolphin captures of the logo frames and of the 3D scene are the next evidence.