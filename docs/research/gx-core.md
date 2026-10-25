# GX core (P2 track): survey, IR and command processor

Current state of the GX core track. Nothing here is derived from game bytes beyond register names, counts, format names and texture/viewport sizes; streams, IR dumps and decoded textures stay under ignored `build/`.

## 1. What exists

| Part | Where | Role |
| --- | --- | --- |
| IR | `src/gx/gx_ir.h` (v1, frozen, architect-owned) | per-draw / per-copy commands, pipeline state, textures, `Backend` interface |
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

`awl-host` is deterministic. With `--retraces 3000` it reaches **retrace 1033 and stops** with `NON-LOCAL RETURN` in `0x801B5A68` (hmawl `OSGetResetButtonState`; the function branches into `func_801B5AD4` inside itself), pc `0x801B5CFC`, instruction 2,798,279,835. That is a P1 platform/generator issue and the blocker for seeing anything after the logos. Up to the stop (1,015 retraces captured, 1,110 draws, 805,600 FIFO bytes, no display lists):

- Retraces 0-18: no draws; clears to XFB. Retraces 19-1029: the **three start-up logos** (300+ retraces each, crossfaded: 1 textured quad per frame, 2 during a fade). The images are RGB5A3, 359x127, 431x147 and 330x136, linear min/mag, clamp. Decoded by the core they read "LICENSED BY NINTENDO" (431x147) and the Natsume logo (359x127), checked by eye on the dumped PNGs. Retraces 1030+: nothing. **The title screen and menus are not reached**; everything the title will use is unobserved.
- Commands: NOP, CP/XF/BP loads, vertex-cache invalidate, draws (quads only). Vertex formats: pos direct xy s16 + color0 direct RGBA8888 + tex0 direct st f32 (16 bytes), and pos direct xyz s16 only (6 bytes); VAT 0, no indexed attributes, no normals.
- XF: orthographic projection for every draw (viewport 640x448, once 640x480), one color channel taking the vertex color (no lights), one regular ST texgen from tex0 with post matrix row 61 (dual transform on, identity).
- Pixel pipeline: 1 TEV stage (`TEXC*RASC` / `TEXA*RASA`, or `RASC` / `RASA` for the plain quad), default swap tables, alpha compare always, z lequal+write (textured) / off (plain), blend src-alpha / inv-src-alpha, dither on, EFB RGB8_Z24, no fog, scissor 640x448, no indirect.
- EFB copies: 1,013, all to XFB with clear (clear white or black), 640x448, y-scale 1, gamma 1.0, clamp top+bottom, vertical filter {8,8,10,12,10,8,8}, sample pattern 0x666666; no copies to texture. Tokens and token interrupts once per frame; no PE_DONE in this range.
- Unobserved (implemented from the documented formats, validated only by synthetic streams): indexed attributes/arrays, matrix-index bytes, normals, lights/specular, STQ and multiple texgens, more TEV stages, konst/swap tables, compare ops, indirect texturing, fog, logic ops, alpha test, 3D projections, other primitives, display lists, copies to texture, all texture formats except RGB5A3, TLUTs, mips.

## 3. Conventions settled by the implementation

- **Clip space and depth** (IR convention 1): confirmed on the game's own projection (ortho z' = z/1024 - 1 maps z=0 to -1, i.e. -w at w = 1) and in synthetic tests (libogc perspective: near -> z = -w, far -> z = 0). Verification with a depth-tested 3D draw waits for the title screen.
- **Face culling** (IR convention 6): the core culls with the sign of the determinant of the (x, y, w) rows. Evidence from the game: its logo quads are submitted clockwise as seen on screen (top-left, top-right, bottom-right, bottom-left, y up after the projection) and survive GEN_MODE cull hardware value 1 (libogc `GX_CULL_BACK`), so **a front face is clockwise on the screen**. `gx_ir.h` convention 6 says counter-clockwise; this is an open wording correction (change request to the architect). The sign is one constant, `kFrontFaceDeterminantSign` in `gx_draw.cpp`; `gx_core_tests` fails if it is flipped.
- **Texcoords** (convention 4): the logo quads give (s,t) = (0,0)..(1,1) with SU_SSIZE/TSIZE equal to the texture size, as the IR states.
- **Scissor**: BP scissor corners minus twice the box-offset register (libogc stores (offset+342)>>1); observed (0,0)-(639,447).
- **Clear** (EFB copy): libogc forces z update on and blending off around a clearing copy, so the core clears color/alpha according to the CMODE0 update bits and z according to the ZMODE update bit at the copy.
- **Texture filters**: the min filter register holds libogc's hardware encoding (near 0, near_mip_near 1, near_mip_lin 2, linear 4, lin_mip_near 5, lin_mip_lin 6), mapped to the IR's GX enumeration; mip levels decoded = ceil(max LOD) + 1 capped by the size, only for mip filters.
- **Copy textures** (convention 8): a copy to a texture records destination, size and a hash of the destination memory; a texture bound at that start address with unchanged memory references the copy. When the memory changed, the bind reads RAM and a note is written once per copy to the boot trace (`gx: texture bound at EFB-copy destination ...`). Z-texture copies are recognised by the EFB format being forced to Z24 during the copy.
- **Texture IA formats**: IA8 and the TLUT IA8 entry have alpha in the first byte; IA4 has alpha in the high nibble (public decoders; no observed texture uses them). CMPR uses 5/8 and 3/8 interpolation like the earlier TPL decoder.

## 4. Cost

On the logo run (Release, deterministic 1,033 retraces): 116,286 commands, 1,110 draws, 1,110 pipeline rebuilds (every draw changes some BP register), 3 textures decoded and 1,005 cache hits, 99 MB of texture memory hashed, **0.035 s in the core = about 34 microseconds per retrace** (0.2% of a 16.7 ms frame). The cost is dominated by hashing the bound texture's memory at every pipeline rebuild (the SDK invalidates the texture cache on every load); heavier scenes will need measuring once the title is reachable. With `--gx-dump` the recorder's file output adds to it.

## 5. Open points

- The title screen is unreachable until the `OSGetResetButtonState` stop is fixed (P1 platform/generator, assigned to the DX11 track). When it is: merge main, re-run the survey through the title and menus, add what the new frames need.
- IR wording change request: convention 6, front faces are clockwise on screen (see section 3).
- Lighting with spotlights and specular attenuation, emboss texgen (rejected, loud), color-to-texcoord texgen, indirect texturing and fog are written from the public formulas and have no real stream to check against.
- Not verified against Dolphin: all of the above, and the face-culling sign beyond the logo quads. The owner's Dolphin captures of the logo frames are the next evidence.
