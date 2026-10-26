# GX core (P2 track): survey, IR and command processor

Current state of the GX core track. Nothing here is derived from game bytes beyond register names, counts, format names and texture/viewport sizes; streams, IR dumps and decoded textures stay under ignored `build/`.

## 1. What exists

| Part | Where | Role |
| --- | --- | --- |
| IR | `src/gx/gx_ir.h` (v2, frozen, architect-owned) | per-draw / per-copy commands, pipeline state, textures, `Backend` interface |
| Command processor | `src/gx/gx_core.{h,cpp}` (`Core`) | consumes the write-gather byte stream (display lists included), CP/XF/BP state, completion services through `CoreHost` |
| Vertices, XF, primitives | `src/gx/gx_vertex.{h,cpp}` (`VertexEngine`; draw framing in `gx_draw.cpp`, shared types in `gx_common.h`) | vertex loading (direct/indexed), position/normal matrices, projection, lighting channels, texgen, quad/strip/fan/line/point conversion, face culling |
| Pipeline and copies | `src/gx/gx_pipeline.cpp` | BP/XF registers -> `PipelineState` (viewport, scissor, depth, blend, alpha compare, fog, TEV, indirect, texture maps), EFB copies, aliasing of copy textures |
| Textures | `src/gx/gx_texture.cpp` | decode of I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8 C14X2 CMPR (+ TLUT, mips) to RGBA8, content-hash key, LRU decode cache (192 MB) |
| Recording / hub | `src/gx/gx_record.{h,cpp}` | `RecordingBackend` (text dump per retrace, texture PNGs), `BackendHub` (`awl::gx::backendHub()`) |
| Platform glue | `src/hle/dev_gx.cpp` | write-gather port, CP/PE registers, finish/token/copy completion (unchanged P1 semantics), guest memory for the core, stream capture, IR dump options |
| Capture / survey | `src/gx/gx_dump.{h,cpp}`, `src/gx/gx_survey.cpp` | raw stream dump and the `awl-gx-survey` console tool |
| Tests | `tests/gx_ir_tests.cpp`, `tests/gx_core_tests.cpp` | hand-assembled streams; expectations from YAGCD/libogc arithmetic (see the comments in the tests) |

**Renderer hookup.** The platform delivers every command to `awl::gx::backendHub()`; a renderer calls `backendHub().add(&backend)` before the run starts (backends are called synchronously on the guest thread, in stream order; pointers in the commands are valid only during the call, `shared_ptr<const Texture>` may be kept). Errors in the stream throw `GxError`, which `dev_gx.cpp` turns into a hardware stop with the stream offset and register/command.

**Options.** `awl-host ... --gx-dump N [--gx-dump-from F] [--gx-dump-dir D]` writes `frame_<retrace>.txt` (IR text, state printed when it changes, up to 64 vertices per draw) and `tex_<key>.png` (level 0 of each bound texture) for retraces F..F+N-1 (default dir `build/gxdump`; `AWL_GX_DUMP_DIR` also works). `AWL_GX_STATS=1` prints the core's counters and cost at exit (it turns the profiling clocks on). `AWL_GX_SCALAR=1` runs the scalar reference path. `AWL_GX_FRAMELOG=<file>` writes one summary line per retrace (draws, vertices, share inside the clip volume, mean lit color). `AWL_GX_MEM_AT=<retrace> AWL_GX_MEM_FILE=<file>` writes MEM1 when that retrace first sends commands (the memory image for `awl-gx-bench`). `awl-gx-bench --mem <MEM1 image> --dump <gx dump> [--reps N] [--profile]` replays a captured stream against a memory image (produce them with `awl-host --dump-mem` and `AWL_GX_CAPTURE`/`AWL_GX_FRAMES`) and reports ns per vertex, ms per frame and the output checksum. `AWL_GX_CAPTURE=<file>` / `AWL_GX_FRAMES=<first>-<last>` write the raw stream (`gx_dump.h`) for `awl-gx-survey <dump> [--out report] [--frames]`.

## 2. How far the run gets, and what the game uses (survey)

Deterministic run, `awl-host --retraces 3000 --limit 0` (DX11 track's EFB window and logged Z-peek provider merged): it reaches **retrace 4118** (main-loop criterion, 11.1 G instructions, about 3.5 minutes of wall time). The core processed the whole run without a stop: 2,518 frames with commands, 13,062,763 draws, 255,914,851 vertices, 4,157,738 display-list calls (1.58 GB of list data), 272 MB of FIFO bytes (capture file 1.85 GB).

| Retraces | Content |
| --- | --- |
| 0-18 | no draws; clears to XFB |
| 19-1029 | the three start-up logos (see below) |
| 1030-1116 | no draws (one clear per frame); the disc trace shows `Message.arc` and the NPC text tables (1042-1084), `title.agc` / `title.tpl` (1087), the title map objects (1096) and `sound/TITLE.arc` (1102) loading |
| 1117-4116 | the **title screen**: a 3D scene (town background) plus 2D text, one frame every 2 retraces (30 fps), about 6,700-7,600 draws and 148,000-170,000 vertices per frame, the same content for the whole 3,000-retrace window (neutral pad: no menu input yet); 1,501 copies to a 64x32 texture and 2,514 copies to XFB over the run |

**Logos (19-1029).** Textured quads, orthographic projection, one TEV stage, RGB5A3 images 359x127, 431x147 and 330x136 (linear, clamp). Decoded they read "LICENSED BY NINTENDO" and the Natsume logo (eyeballed on the dumped PNGs).

**Title scene (1117-4116), observed (counts from the retrace 1378 survey; the 4118 survey adds only: a third 2-stage TEV form `ONE` / `RASA*TEXA` (25,500 draws), blend src-alpha/one and one/one, 64 distinct textures, tex1 from tex0):**
- Commands: display lists (called 334,434 times), CP/XF/BP loads, indexed XF loads: none. Primitives: triangle fans 398k, strips 350k, quads 139k, points 28.6k, triangles 24k; no lines. Vertex formats (38 distinct, all VAT 0): positions indexed 8/16-bit s16 xyz with shifts 4-14 (or direct f32), normals indexed 8-bit f32 (or s16), color0 indexed RGB565 or RGBA4444 (8/16-bit indices), texcoords indexed s16 >> 14 / s16 >> 6..9 / f32 / s8 >> 6 / u8, a few direct formats (pos xy f32 + st f32; pos xyz f32 only for the points). No matrix-index bytes and no color1. Arrays: pos 107 distinct bases (max index 5015), tex0 108, col0 66, nrm 3; strides 2, 4, 6, 8, 12.
- XF: perspective projections (4 parameter sets) and the 2D orthographic one; position matrices 0-24 (every 3 rows) reloaded per object, a normal matrix per draw (105k loads), 2x4 texture matrices 30/33/36; **one light** (light 0 and light 2 loaded 131 times) with the channel's attenuation bit set in spotlight mode (diffuse clamp), color0 material from vertex or register, alpha unlit; 1 texgen (regular ST from tex0 through the post matrix) or 3 texgens (position source, ABC1 form) for the indirect effect.
- Pixel pipeline: 1 TEV stage (TEXC*RASC / TEXA*RASA, or C0*TEXC for the 28.6k point sprites, C0/C1 konst-register blends for 11.8k draws) and a 2-stage indirect effect (17,161 draws: 2 indirect stages with matrix 1, bias 3, wrap 0/6; map 0 IA8 128x128 repeat and map 1 I8 128x128 clamp); swap tables with the rswap/tswap = 1 selection; alpha compare variants (a > 127 for 739k draws, always, 1..255 gequal/lequal); z lequal with write (879k draws), no-write (29k) or off; blend off (863k), src-alpha/one (46k), src-alpha/inv-src-alpha (31k); **linear fog** (type 2, perspective) on 860k draws; cull back (517k), none (373k) and front (2.6k); EFB RGB8_Z24 throughout.
- Textures (28 formats/sizes, 43 distinct images): CMPR 32x32 to 1024x1024 with mips (lin_mip_lin, max LOD 3, some with LOD bias, repeat), RGB565 4x4 and 8x8, I4 1008x648, I8 and IA8 128x128, IA4 32x32 and 64x64, RGBA8 64x32 (the render-to-texture target, 2,620 draws sample it via the copy alias), RGB5A3 (logos, 292x15, 512x325, 64x32). No TLUT loads, no color-index textures.
- Points: size 96/48 (1/6 px) with texture offset; the 28.6k points are drawn with a 4x4 RGB565 texture.
- EFB copies: XFB copies as in the logos; the render-to-texture copy is 64x32 RGBA8 with clear, scissor 64x32, vertical filter {21,22,21} defaults.
- The I4 1008x648 texture is the Japanese kana font atlas (its lower part holds unwritten memory); the text is drawn as 2D quads with it (about 67 draws per frame).
- Still unobserved: XF indexed loads, matrix-index bytes, color1, STQ/emboss/color texgens, lines, TLUTs, compare-mode TEV ops, logic ops, specular/spotlight coefficients other than trivial (not checked), more than one light per channel.

## 3. Conventions settled by the implementation

- **Clip space and depth** (IR convention 1): confirmed on the game's own projection (ortho z' = z/1024 - 1 maps z=0 to -1, i.e. -w at w = 1) and in synthetic tests (libogc perspective: near -> z = -w, far -> z = 0). The title scene has depth-tested draws (z lequal) that a renderer can now check.
- **Face culling** (IR convention 6): the core culls with the sign of the determinant of the (x, y, w) rows. Evidence from the game: its logo quads are submitted clockwise as seen on screen (top-left, top-right, bottom-right, bottom-left, y up after the projection) and survive GEN_MODE cull hardware value 1 (libogc `GX_CULL_BACK`), so **a front face is clockwise on the screen**. `gx_ir.h` convention 6 states this since version 2. The sign is one constant, `kFrontFaceDeterminantSign` in `gx_draw.cpp`; `gx_core_tests` fails if it is flipped.
- **Texcoords** (convention 4): the logo quads give (s,t) = (0,0)..(1,1) with SU_SSIZE/TSIZE equal to the texture size, as the IR states.
- **Scissor**: BP scissor corners minus twice the box-offset register (libogc stores (offset+342)>>1); observed (0,0)-(639,447).
- **Clear** (EFB copy): libogc forces z update on and blending off around a clearing copy, so the core clears color/alpha according to the CMODE0 update bits and z according to the ZMODE update bit at the copy.
- **Texture filters**: the min filter register holds libogc's hardware encoding (near 0, near_mip_near 1, near_mip_lin 2, linear 4, lin_mip_near 5, lin_mip_lin 6), mapped to the IR's GX enumeration; mip levels decoded = ceil(max LOD) + 1 capped by the size, only for mip filters.
- **Copy textures** (convention 8): a copy to a texture records destination, size and a hash of the destination memory; a texture bound at that start address with unchanged memory references the copy. When the memory changed, the bind reads RAM and a note is written once per copy to the boot trace (`gx: texture bound at EFB-copy destination ...`). Z-texture copies are recognised by the EFB format being forced to Z24 during the copy.
- **Texture IA formats**: IA8 and the TLUT IA8 entry have alpha in the first byte; IA4 has alpha in the high nibble (public decoders; no observed texture uses them). CMPR uses 5/8 and 3/8 interpolation like the earlier TPL decoder.


## 4. Cost (Release)

**Vertex path, measured with `awl-gx-bench`** (replays 4 title frames, 152,000 vertices and 7,200 draws each, from a captured stream and a MEM1 image through the real core with a checksumming backend; one core for all passes so caches are warm; median of 7 passes, 3 runs; the machine is a shared desktop, so run-to-run spread is about 5%):

| | before (afc2c28) | now |
| --- | --- | --- |
| whole core per frame, excluding the backend | 17.7 ms | 5.5 ms |
| per vertex, whole core (parse + pipeline + vertex stage) | 96 ns | 37 ns |
| vertex stage only (`--profile`, includes about 3 ns of timer reads) | 98 ns | 28 ns, i.e. about 25 ns |
| texture/pipeline part per frame | 1.9 ms | 0.6 ms |

The checksum over every vertex, index and state id of the replay is identical before and after (`C09284AE74FA7894`), so the optimised path is bit-identical to the original scalar one on 608,000 real vertices. The full-run triangle and cull counts match too (113,731,551 triangles, 21,969,798 culled over 4,118 retraces). `AWL_GX_SCALAR=1` switches the specialised loaders and the 4-wide path off (the reference), and `gx_core_tests` compares both paths bit for bit.

What changed: (1) the vertex stage is its own engine (`gx_vertex.cpp`) with a per-format plan (attribute loaders chosen once per vertex format as template instances for the shapes the game uses: indexed s16/f32 positions, 8-bit-indexed f32 normals, indexed RGB565/RGBA4444/RGBA8 color, indexed s16/f32/s8 texcoord; anything else uses the generic loader); (2) per-draw decoding is hoisted and cached (plans compare the register values they were built from, XF register/light/post-matrix changes mark state dirty, matrices are fetched once per matrix index); (3) the XF stage of a draw with uniform matrices runs four vertices at a time in SSE registers (positions, normals, one-light lighting with constant attenuation, texgens with the dual post matrix), performing the same IEEE operations in the same order per lane; draws that need anything else (per-vertex matrix indices, spot/specular attenuation, color texgens, normalised post transform) use the scalar path; (4) 16-bit and f32 big-endian attributes are loaded with SIMD shuffles; (5) texture bindings that were verified since the last texture-cache invalidate are not read again (see below); (6) buffers are reused and the profiling clocks are off unless asked for.

Texture reads. The SDK invalidates the texture cache (BP 0x66) whenever it changes texture memory, as the hardware requires. The core therefore keeps, per texture binding, the raw bytes of its last decode and the invalidate epoch in which they were verified: inside an epoch the bytes are not read again, in a new epoch they are compared byte for byte (exact) and hashed only if they differ. A change of texture memory without an invalidate is not observed, as on hardware. Over the full title run: 470 MB hashed, 3.5 GB compared, 199,000 bindings trusted.

Whole title run (`AWL_GX_STATS=1`, to retrace 4118): 13.06 M draws (12.28 M on the 4-wide path), 255.9 M vertices, 19 s in the core with the profiling clocks on (47.9 s before). The logos cost about 34 microseconds per retrace.

Not done: the worker pool (not needed at this cost); further per-vertex gains would come from SIMD color decoding and a float-only culling test (the culling determinant runs in double to stay identical).

## 5. Open points

- The title scene is reached; the menus need pad input (the host runs with neutral input). A scripted pad (press Start/A at chosen retraces) in the platform layer would extend the survey into the menus.
- Spot/specular attenuation beyond constant coefficients, emboss and color texgens, compare-mode TEV, logic ops, TLUT paths and the IA4/IA8 bit order have no real stream (the title scene exercises spot-mode with constant coefficients, fog, indirect texturing, CMPR mips, a copy texture, points, IA4/IA8/I4/I8 textures, but they are only decoded; nothing is compared with the original yet).
- Not verified against Dolphin: all of the above. The owner's Dolphin captures of the logo frames and of the title scene are the next evidence.

## 6. Title scene brightness and geometry check (renderer report: "almost black")

Checked with `AWL_GX_FRAMELOG=<file>` (one line per retrace: draws, vertices, share of vertices inside the clip volume, mean lit color 0) and `awl-gx-bench` on captures of retraces 1117 and 3000:

| retrace | draws | vertices | vertices inside the clip volume | mean lit vertex color (R,G,B of 255) |
| --- | --- | --- | --- | --- |
| 1117 | 5,617 | 140,889 | 62.6% | 71, 77, 90 |
| 1300 | 6,072 | 144,625 | 64.0% | 70, 74, 76 |
| 1500 | 6,560 | 155,023 | 61.9% | 101, 105, 103 |
| 1800 | 7,420 | 158,217 | 67.3% | 158, 161, 153 |
| 2000 | 7,811 | 161,073 | 66.5% | 183, 185, 181 |
| 3000 | 7,000-8,500 | 160,000-178,000 | 60-66% | 187, 188, 186 |

- The geometry is in view: about 62-67% of all vertices are inside the clip volume in every title frame, 3-4% have w <= 0, and a coarse coverage model of the IR (viewport transform, scissor, lequal depth test, no TEV/alpha; triangles with w <= 0 skipped) writes all 286,720 pixels of the 640x448 EFB in every title frame. The draws the renderer saw at y/w between -1.8 and -11 are the minority outside the view (about 38% of vertices) plus the projected shadow polygons drawn through the 64x32 render-to-texture pass.
- **The scene starts dark by design and brightens over about 20 seconds**: the lit vertex color rises from about (71, 77, 90) at the first title frame to about (185, 186, 182) by retrace 2000. The cause is in the game's data: channel ambient register (0x100A) around (76, 82, 87) and a blue light (110, 125, 255) at the start; the title's terrain faces are lit only by ambient light at that time (the light sits behind the camera and below the horizon, N.L < 0 for upward normals). Multiplied by the (0.5) textures and the dark fog color the early frames are close to black on screen. **Frames from retrace 2000 on (framebuffer latch index about 1450 and later) are the bright ones to check the renderer against.** The mapping retrace -> latch: 1011 latches at retrace 1117, then one latch per two retraces.
- One terrain vertex end to end (retrace 1117 capture): stream bytes `2F 7B 04 26` (position index 47, normal index 123, color index 4, texcoord index 38), position element `D5 05 13 B6 E3 BB` = s16 (-11003, 5046, -7237), shift 14 = (-0.67157, 0.30798, -0.44171); model-view rows (-0.912267, 0, -0.158888, -20.85231), (-0.030317, 0.908987, 0.174069, -11.199858), (0.155969, 0.176689, -0.895506, -93.030807) give view-space (-20.16947, -10.97643, -92.68558); projection A..F = 2.779624, 0, 3.706165, 0, -0.000293055, -0.300088 gives clip (-56.0636, -40.6811, -0.272926, 92.6856) (hand calculation in double, matches the IR to six digits); x/w = -0.605, y/w = -0.439, z/w = -0.0029: inside the view. The test `testPerspectiveObjectAndDarkLight` repeats this shape with synthetic numbers (indexed s16 positions, rotation + translation, the game's projection form, the title's light and ambient) against double-precision expectations.
- Not a cause: the projection decode and use (order A, B, C, D, E, F; perspective x' = A x + B z, y' = C y + D z, z' = E z + F, w' = -z; confirmed against libogc `GX_LoadProjectionMtx`, which writes m00, m02, m11, m12, m22, m23 and the type), the position-matrix selection (XF 0x1018 index 0 with no per-vertex indices in the title formats; address = index x 4, 3x4 row-major, as `GX_LoadPosMtxImm`), indexed XF loads (none in the title), CMPR decoding and mip levels (checked per level), the normal matrix. The `fres`/`frsqrte` placeholders play no role here: all matrices in the title frames are well-formed rotations.
- Lit vertices of many title draws carry color alpha 0 (the RGBA4444 vertex colors have alpha nibble 0) in draws whose alpha compare is "always" and blend is off (their alpha does not matter); the alpha-tested draws were not examined further.