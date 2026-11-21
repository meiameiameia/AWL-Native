# DirectX 11 renderer: current state and design

Owner: the P2 DX11 renderer track (`src/gfx/**`, target `awl_gfx`; host hooks in `src/hle/host_main.cpp` and `src/hle/dev_vi.cpp`).
Scope of this note: what exists and is tested, the rules the renderer will follow, and the evidence gaps. Rules are derived from public sources only: libogc `gx.h` / `gx.c` (names, parameter meanings, the fog parameter derivation), YAGCD and the public GX register descriptions, and the D3D11 documentation. Dolphin was read for understanding of behaviour where public documents are silent and nothing was copied; every such point is marked **verify** and is settled by a comparison against the owner's Dolphin captures, not by the source of an emulator.

Status legend: **done** = built and covered by an automated headless test; **window** = written, never run by this track (verified by the owner at an agreed time); **design** = rules only, implemented in milestone 2.

## 1. VI framebuffer: what the renderer is told (done)

The VI model (`src/hle/dev_vi.cpp`) calls an optional hook at every retrace (field start), before the retrace is counted, with the picture the VI shows from that field on whenever the framebuffer registers changed since the previous retrace. A latch becomes visible at the next retrace, as on hardware: the SDK writes the registers from its retrace callback, and the VI picks them up at the next field. With no hook installed the model is unchanged, the boot trace is byte-identical, and nothing is recorded.

Decoded fields (register offsets inside the VI block at `0xCC002000`):

| Field | Register | Rule | Value seen at the game's main loop |
| --- | --- | --- | --- |
| XFB address | TFBL `0x1C/0x1E` | bits 0-23, shifted left by 5 when bit 28 (page offset) is set | `0x100B7CC9` -> `0x016F9920`; alternates with `0x0166D920` (double buffering); BFBL = TFBL + 0x500 (one 1280-byte line) |
| Height | VTR `0x00` bits 4-13 (active lines per field), DCR `0x02` bit 2 (1 = non-interlaced) | interlaced: 2 x lines, otherwise lines | `0x0E06`, DCR `0x0001` -> 224 x 2 = **448** lines, interlaced |
| Width | `0x70` | pixels per line | `0x0280` = **640** |
| Stride | `0x48` low byte | units of 16 bytes (the high byte is the same value in units of 32 bytes) | `0x2850` -> 80 x 16 = 1280 bytes = 640 pixels x 2 bytes (YCbCr 4:2:2) |
| Enabled | DCR bit 0 | frames are reported only while set | 1 |

So the game shows a 640 x 448 interlaced frame stored as one contiguous 640 x 448 buffer (both fields woven in memory; the VI fetches the bottom field one line below the top). The renderer presents the frame as a whole and does not simulate field-by-field output. The horizontal stepping register (`0x4A`, 256 = 1:1) is 256 here. Evidence gaps: other video modes (progressive, 480p, PAL) are decoded by the same rules but unobserved; the first latch in the boot trace (VI initialisation, before the game mode is set) has different stride bits and is not a game picture.

Display aspect is 4:3 (letterboxed in the window). Widescreen is an opt-in enhancement (section 9).

### Presentation source

`gfx::FrameSource::produce(FrameDesc, Image&)` returns the picture for a latched XFB. Until the IR exists, `DiagnosticSource` draws a magenta frame with the latch count in white digits. It is a development indicator and is never acceptance evidence. In milestone 2 the source is the GPU image that the EFB-to-XFB copy wrote for the XFB address; the presenter blits it. The hardware XFB is YCbCr 4:2:2 (chroma shared by pairs of pixels); the renderer keeps the copy in RGB and presents it directly (Dolphin's default does the same). An optional 4:2:2 round trip in the display-copy shader can reproduce the chroma resolution; it is not on by default (**verify** against the captures whether the difference is visible).

## 2. Device layer, window, pacing, input

### Device (done)

`gfx::Device` creates a D3D11 device from the hardware adapter (feature level 11.1, falling back to 11.0) and falls back to WARP when the hardware device cannot be created or `forceWarp` is set. `OffscreenTarget` is an RGBA8 render target with CPU readback; `ImageTexture` uploads an `Image`; `Blitter` draws a texture into any render target with a letterboxed viewport. Tests render offscreen on both the hardware adapter and WARP (`gfx_device_tests`): a 1:1 point-sampled blit reproduces the source bit for bit on both.

### Frame capture (done)

`--capture-frames N DIR` writes `frame_0000.bmp`... for the first N presented frames, headless or windowed. BMPs are 24-bit uncompressed bottom-up with no timestamps, so equal pixels give equal bytes. With the diagnostic source, two runs produce identical files on the hardware device and on WARP (checked by `gfx_device_tests` and by running the host twice). DIR belongs under the ignored `build/`.

### Window (window)

`gfx::GameWindow`: Win32 window, flip-model swap chain (R8G8B8A8, two buffers, vsync: `Present(1, 0)` with a one-frame queue; see "Window small fixes"), resizable with the back buffer resized on `WM_SIZE`, borderless fullscreen on F11 or Alt+Enter, close on Esc, the close button and Alt+F4 (all become a close request that makes the host stop the guest with the exit reason `window closed`; the host exits with code 0). A tap on Alt does not enter the menu loop (`SC_KEYMENU` is swallowed) because that loop would stall the host. The window is created on the thread that runs the guest; guest fibers share that thread, so messages are pumped from the retrace hook. A guest that makes no progress for a long time would make Windows mark the window as not responding; a separate presentation thread is the fix if that proves to matter.

The title bar shows the name, the measured speed (every 30 retraces) and the main keys (`SpeedMeter`: 100% = 59.94 retraces per wall-clock second; a pause resets it). The same text is drawn in a dark box in the top-left corner of the picture (`overlayLines`, `drawTextPanel`; F1 hides it) onto a copy of the frame, so it appears in fullscreen too and never in `--capture-frames` images. When the run stops for any reason except the window being closed (a stop report, a failed present, the entry function returning), `awl-host` shows a message box with the reason before it exits; start-up failures (no device, renderer) get a message box too. The console keeps the full report.

`--window` also lifts the main-loop and instruction limits so the game runs until the window is closed, unless `--retraces` or `--limit` is given. `--gfx-warp` forces the software device.

### Pacing (done for the maths; window verified by hand)

`gfx::RetracePacer` makes retrace k due at `anchor + k x 1001/60000 s` (59.94 Hz; the same 675,675 time-base ticks per field as the VI model). Integer arithmetic only; 60,000 fields are exactly 1001 s at any counter frequency. If the host falls more than four fields behind it re-anchors instead of running fast to catch up. The host sleeps coarsely and spins for the last stretch; the 1 ms timer period is requested while pacing is active. **Pacing only decides when the guest is allowed to continue; the guest time base stays instruction-based and the boot trace is byte-identical with and without `--realtime-pace`** (checked). Measured: 141 retraces to the main-loop criterion take 2.48 s of wall time (ideal 2.35 s plus start-up).

Focus loss pauses the game: at the next retrace the host stops running the guest, clears input, and waits (pumping messages) until the window is focused again, then restarts the pacing schedule so there is no catch-up burst. Minimising counts as focus loss. The pause is announced on screen and in the title ("Window small fixes").

### Input (done for the mapping; the PAD hook needs a change in a file this track does not own)

`gfx::WindowInput` feeds keys (from window messages) and XInput controller 0 into the project's `NativeInputAccumulator` and `PadAdapter` (`src/input.cpp`). Defaults: W/A/S/D main stick, arrows D-pad, Space = A, Backspace = B, X = X, Tab = Y, Z = Z, Q = L, E = R, Enter = Start; Esc, F11 and Alt+Enter belong to the window. Input is active only while the window has focus: losing focus releases every key and clears the pad, and keys pressed while unfocused are dropped. The tests check the mapping against the SDK button constants.

`awl-host --window` installs `WindowInput::sample()` as the PAD provider (`setPadProvider`, `src/hle/pad_source.*`), so `PADRead` returns the keyboard and controller state while the window has focus and reports the controller absent otherwise (the host also pauses the guest on focus loss). Without `--window` the PAD replacement is unchanged (neutral by default, `--pad xinput` for the controller without focus rules). `hle_sched_tests` checks the provider path (PADStatus fields, absent controller, back to neutral).

### Build structure

`src/gfx/gfx.cmake` defines `awl_gfx` and `awl_pad_input` (the one copy of `src/input.cpp`, used by both the PAD replacement and the window input; `tools/recomp/platform.cmake` links it instead of compiling the file twice). `awl-host` links `awl_gfx` and defines `AWL_HAVE_GFX` when the target exists. The tests (`gfx_device_tests`, `gfx_pacing_tests`, `gfx_input_tests`, `gfx_tev_tests`) never open a window.

### Window small fixes (2026-11-21; implemented, window check pending: at a time agreed with the owner, never started by an assistant)

Findings of the owner's re-check after the speed block (speed passed at 100%): (a) the arrow keys do not move the title cursor, (b) nothing says "paused" on focus loss, (c) the picture looks blurry and trembling. Presentation and input only: the 121 captured frames (presented 0..2400 step 20), the capture-mode trace, the `--calls 16` boot trace and `EXIT: main loop reached after 11135124000 instructions` are byte-identical to the baseline made from main (c9e219c) first; the bench path does not touch any of this code.

**(c) Steady picture.** Causes found in the code: `Present(0, 0)` (no vsync) from a software timer (flip model: DWM takes whichever frame is newest at its own refresh, so frames are dropped or shown twice unevenly and can tear), the frame produced and drawn *after* the pacer wait (its variable cost moved the present time), and a plain bilinear stretch of the 640 x 448 frame to 4:3 (480 lines at the old 640 x 480 window: 1.07x vertically, a blend in every row). Changes:
- `GameWindow::present` now uses `Present(1, 0)` and `IDXGIDevice1::SetMaximumFrameLatency(1)`: a frame reaches the screen at the next refresh after the call, never torn, at most one frame queued. The game stays paced at 59.94 Hz by the unchanged `RetracePacer`; the pacer decides when `Present` is called and the display decides which refresh shows it.
- `VideoOutput::onRetrace` makes the picture (XFB readback, help-text copy) *before* the pacer wait, so the wait absorbs that work and the present happens right at the due time.
- Cadence by monitor: 60 Hz shows each game frame for one refresh (59.94 vs 60.00 drifts one refresh every ~17 s: one repeated frame, invisible); 120 Hz exactly two; 144 Hz 2 or 3 refreshes (2.40 on average, pattern 2,2,3,2,3: whole refreshes only, at the refresh nearest the ideal time; without a variable-refresh display that unevenness is inherent); below 59.94 Hz (e.g. 50 Hz) the game cannot reach full speed and the speed reading says so. The monitor's rate is read with `EnumDisplaySettingsEx` for the monitor the window is on (re-read after fullscreen toggles, moves and display changes), printed at start (`awl-host: display refresh rate N Hz`) and shown in the corner box (`displayLine`). **This PC's primary monitor reports 144 Hz** (AMD RX 6600; second monitor 1366 x 768), so expect the 2/3 pattern there; the box reads `DISPLAY 144 HZ: GAME FRAMES SHOWN FOR 2 OR 3 REFRESHES`.
- Scaling: aspect stays 4:3 (letterboxed). 1:1 stays point sampled; otherwise the new `BlitFilter::Sharp` ("sharp bilinear": each source pixel is held flat over most of its footprint and blended over about one destination pixel at the edge, plain bilinear below 1x). At an exact integer scale it equals pixel repetition (tested with hand-derived values), at other sizes it removes the all-rows blur of plain bilinear. The first window opens at 2x (1280 x 960 client) when the work area has room (this PC: yes), otherwise at 1x, so the default is an exact multiple of 640 x 480. Captures keep the point-sampled blit (`BlitFilter::Point`).
- The copy deflicker filter stays on (decision in force: softer image, as on hardware). `awl-host --no-copy-filter` exists for comparison only; a sharper title screen with it is the filter, not a bug.

**(b) Pause indicator.** The title-bar text was never seen because the old title put the controls first (about 110 characters) and the state at the end: Windows cuts a long title at the window width. The title is now short and state-first: `PAUSED - click this window to continue - <name> - Esc quit, F11 fullscreen, F1 help text` while paused, `<name> - speed N% of real time - Esc quit, F11 fullscreen, F1 help text` otherwise (a test checks that the state comes first). On focus loss the host also presents the last picture dimmed to half brightness with a big "PAUSED" and "CLICK THIS WINDOW TO CONTINUE" in the middle (`makePausedImage`; shown even when F1 hid the help text, drawn on the picture so it works in F11 fullscreen where there is no title bar), once before waiting, and again if the window is resized or restored while paused (`GameWindow::needsRedraw`). The paused picture is a copy: captures are untouched, and the renderer is told to drop its cached D3D state afterwards (`afterPresent`).

**(a) D-pad: the title menu ignores it; the game receives the bits correctly.** Checked in our own run with the new debug variable `AWL_PAD_SCRIPT` (headless, neutral mode only; `from-to=HEXBUTTONS[,stickX,stickY];...` sets channel 0 for the PADRead calls numbered from..to):
- The game's consumer-visible pad record (`0x80341574`, current buttons) at the end of a run holding D-pad Down (`0004`) reads `0x00000004`, holding D-pad Up (`0008`) reads `0x00000008`; holding stick down (`0,-80`) reads `0x00020000` (the stick-synthesized "down" bit of `FUN_80213180`, filtered stick byte at `0x8034158D` = -70). So `PADStatus.button` has the SDK bits (Left 0x0001, Right 0x0002, Down 0x0004, Up 0x0008) and reaches the game unchanged: the mapping is correct and nothing was changed.
- At the title screen (cursor on "New Game": red arrow centred at y = 342 in the captured frame) a short stick-down press (`2000-2010=0000,0,-80`, frame 2400 of `--retraces 3000`) moves the cursor to "Continue" (y = 372); a short D-pad Down press (`0004`) or D-pad Up press (`0008`) leaves it on "New Game" (a short stick-up press moves it to "Continue" by wrapping; D-pad Down together with stick down moves it). The title menu navigates on the stick-synthesized bits (`0x10000` up, `0x20000` down, the same ones the world-map selection code uses, see `docs/research/input-player-trace.md`), so it **ignores the D-pad as far as our traces show**. Recorded as the original behaviour, not a port bug; the owner can confirm in Dolphin when convenient (D-pad on the title screen). Not tested: other menus (any code that tests the D-pad bits receives them).

Owner window check (a few minutes; `build\Release\awl-host.exe --dol rom\main.dol --window` from the repository root, at a time agreed beforehand):
1. The picture on the title screen: steady, without trembling or tearing, sharper than before (the deflicker still softens it a little). The window opens larger than before (about 1280 x 960 client) on a 1080p screen. Resize it by dragging: still no shimmer, proportions kept.
2. The corner box: line 1 says speed near 100%, line 2 the display rate (expected `DISPLAY 144 HZ: GAME FRAMES SHOWN FOR 2 OR 3 REFRESHES` on this PC). Write down the lowest speed reading and whether anything stutters at 144 Hz (a faint 2/3-refresh unevenness may be visible in slow camera motion; tell me if it bothers you).
3. Click another window: the game picture dims and says PAUSED / CLICK THIS WINDOW TO CONTINUE, and the title bar starts with "PAUSED - click this window to continue". Click the game window: it resumes at 100% without a fast-forward burst. Repeat in F11 fullscreen (click the other monitor): the dimmed PAUSED picture must be visible without a title bar.
4. Arrow keys on the title screen do nothing (expected, original behaviour); W / S move the cursor. Optional: the same check in Dolphin.
5. Esc closes it; the console ends with `EXIT: window closed`.

### Owner window test (done 2026-11-21, kept for the controls and report list; the speed remark below is from before the speed block)

Goal: see the game's logos and title screen in a real window for the first time, and report how it behaves. **The game runs slower than real time for now** (roughly a fifth of the original speed, measured before this window existed); that is expected, the window says so, and fixing it is the next work block. Expect the logos and the title screen to take a while to appear and to move in slow motion.

Before you start: close fullscreen games (the window takes focus). From the project folder (`C:\Users\mperez\Projects\awl-native`) in PowerShell, build once if `build\Release\awl-host.exe` does not exist (`cmake --build build --config Release`, several minutes), then run:

```powershell
build\Release\awl-host.exe --dol rom\main.dol --disc disc --window
```

Leave the black console window open behind the game: it is where the program writes what it is doing.

What should appear:
1. A window titled "Harvest Moon: A Wonderful Life (native)" followed by the keys and, after a couple of seconds, the speed ("speed NN% of real time"). A small dark box in the top-left corner of the picture shows the same: the speed in percent and the controls. Below about 95% it adds "SLOWER THAN THE ORIGINAL (EXPECTED FOR NOW)". The first seconds, possibly half a minute at this speed, may be a black picture or the magenta start-up marker while the game boots.
2. The company logos, then the title screen: the sign, the town behind it, "New Game / Continue" and the copyright line, as in your Dolphin screenshots. Colors, layout and the menu text are what to look at. The picture is a little softer than Dolphin's (a faithful console filter, a known and chosen difference).

Controls (also written in the window): arrows = D-pad, W A S D = stick, Space = A, Backspace = B, Enter = Start, X / Tab / Z / Q / E = X / Y / Z / L / R. A connected Xbox-type controller works too. Esc, F11 and Alt+Enter belong to the window.

What to try, in this order, a few seconds each:
1. Wait for the title screen and press the Down and Up arrows: does the menu cursor move between "New Game" and "Continue"?
2. Press Space or Enter on "New Game" only if you want to; what happens after that is not part of this test (the game may stop with a message; that is useful to report).
3. F1: the help text in the corner hides and shows. The title bar keeps the speed.
4. Drag the window edges to resize it: the picture stays 4:3 with black bars and does not stretch.
5. F11 (or Alt+Enter): borderless fullscreen, the help text is still visible; press it again to come back.
6. Click on another window or minimize this one: the game pauses (the title says "paused: click this window to continue") and the speed does not drop; click the game window to continue.
7. Esc, Alt+F4 or the close button: the window closes by itself and the console says "EXIT: window closed" (exit code 0).

If the game stops by itself, a message box shows the reason (for example a "MEMORY FAULT" or "UNTRANSLATED CODE" with a guest address) and the console window has the full report. That is a finding, not a failure of the test.

What to report back (plain words are enough):
- Did the window open and come to the front by itself, or did you need to click it?
- Which screens appeared, and how long after starting (roughly)?
- The speed percentage you saw on the title screen.
- Did anything look wrong: colors, missing or garbled text, flicker, a stretched picture?
- Did the arrows move the menu cursor? Did resize, F11, F1, pause and Esc behave as described?
- If a message box appeared: take a photo or screenshot, or copy the text. Also copy the last 30 lines of the console window.
- Anything surprising, including a crash with no message.

How it was checked without a window (by the assistant): the text, overlay drawing, speed meter and key table are unit-tested (`gfx_pacing_tests`, `gfx_input_tests`); the host without video options is byte-identical across runs; headless captures of the title frame are unchanged. The window itself has never been opened by an assistant, so everything above that needs eyes is what this test is for.

## 3. Internal-resolution scale factor (design only; 1x default)

A single integer `S` (1 by default) scales the EFB render target: 640 x 528 becomes 640S x 528S. Everything defined in EFB pixels is multiplied by `S` on the way into D3D: viewport rectangle and scissor, EFB-copy source rectangles, clear rectangles. Depth range, texture coordinates, fog and TEV maths do not change. Rules that keep game logic and the default look unaffected:

- At `S = 1` every code path is the faithful one; nothing in the shaders depends on `S`.
- EFB copies that become textures keep the scaled image (the later texture lookup uses normalised coordinates, so it just samples a sharper image); copies to the XFB are presented at the scaled size and filtered down or up to the window.
- EFB peeks and pokes (`GX_PeekARGB`, `GX_PokeARGB`, depth peeks) read and write the sample at the centre of the corresponding `S x S` block, so a game that feeds an EFB read back into its logic sees the same value at any `S`.
- Effects that address the EFB in pixel units (for example blur offsets of one pixel) are expressed in the game's normalised texture coordinates, so they keep their meaning; if the survey finds an effect that uses absolute pixel offsets, it gets a per-effect compensation, not a global one.
- Line and point sizes, which GX specifies in 1/6-pixel units of the EFB, are multiplied by `S`.
- Anti-aliasing (the multisampled pixel formats) is independent of `S` and stays out of scope here.

The setting lives in a `RenderConfig` that the renderer reads at start-up; changing it at run time recreates the EFB targets. It is an enhancement: it is exposed only through an opt-in setting, never enabled by default.

## 4. TEV pixel shader generation (skeleton done; full generation in milestone 2)

### Key and cache

`tev::PixelShaderKey` holds everything that changes the generated code, as plain bytes (no padding), so equality is `memcmp` and the hash is FNV-1a over the bytes: the stage count (1-16), per stage the colour and alpha combiner descriptions, texture map and coordinate set, raster channel, konst selectors, raster/texture swap table indices and indirect stage; the four swap tables; the projective mask of the coordinate sets; the alpha-compare codes and logic; fog type; EFB pixel format. Values are the libogc `gx.h` values because they are what the BP registers carry. Values that change only constants (the TEV registers, konst colours, alpha references, fog colour and parameters) are not part of the key: they live in the constant buffer, so one shader serves every colour. `PixelShaderCache` maps key bytes to a compiled pixel shader (or to the recorded failure) and counts lookups, hits, compilations and rejections. A key that cannot be generated is rejected with a named reason and the rejection is cached; the renderer reports each distinct reason once on the console and counts further uses, so no unsupported state is silently dropped. A persistent on-disk cache of compiled bytecode is a later optimisation (compile times matter in real time; the number of distinct keys the game reaches is known from the survey).

The vertex shader is one fixed shader: it takes the GX core's post-transform vertices (clip-space position, two colours, eight 3-component texture coordinate sets) and applies the depth mapping of section 5. The skeleton fixes this layout; it will be matched to `src/gx/gx_ir.h` when that is frozen (open question to the GX core: are texture coordinates normalised by the texture size, and is the third component the projective q?).

### Combiner equation

For each of the colour (three channels) and alpha (one channel) halves of a stage, per `GX_SetTevColorOp` / `GX_SetTevAlphaOp`:

    out = (d (+|-) ((1 - c) * a + c * b) + bias) * scale          bias in {0, +0.5, -0.5}, scale in {1, 2, 4, 0.5}

followed by an optional clamp and a write to the output register (PREV, REG0, REG1 or REG2). Inputs (`GX_CC_*`): CPREV, APREV, C0, A0, C1, A1, C2, A2, TEXC, TEXA, RASC, RASA, ONE, HALF, KONST, ZERO; alpha inputs (`GX_CA_*`): APREV, A0, A1, A2, TEXA, RASA, KONST, ZERO. A colour input that names an alpha (APREV, A0, TEXA, RASA...) replicates it to three channels. The colour half of stage n reads the registers as they were before stage n and the alpha half likewise; the generated code computes the colour half first and writes only the colour channels, then computes the alpha half and writes only the alpha channel, which preserves that rule. After the last stage, PREV is the pixel's colour.

Compare mode (bias field = 3 in the register): `out = d + (cmp(a, b) ? c : 0)` where the compared quantity is selected by the mode: R8 (red channel), GR16 (green:red as 16 bits), BGR24 (blue:green:red as 24 bits) with `>` or `==`, and RGB8 (each channel compared on its own; for alpha: A8) with `>` or `==`. These are listed in the key and rejected by the generator today (**design**: integer compares on 8-bit quantised inputs; the result is added to d and then scaled and clamped like the arithmetic form).

Precision (**verify**): the hardware keeps register values as signed 10-11 bit integers and evaluates the interpolation in fixed point (the weight c is extended from 8 to 9 bits, and the sum is rounded). The skeleton evaluates in float with a, b, c clamped to [0, 1] and d unclamped, and clamps the result to [0, 1] or, without the clamp bit, to the signed range [-1024/255, 1023/255]. The expected difference is at most one least-significant step. If captures show systematic differences, the generator switches to an integer evaluation (SM5 supports `uint`/`int`) behind the same key.

### Konst, swap tables, raster channels

- Konst selectors (`GX_TEV_KCSEL_*`): 0-7 are the fractions 1, 7/8, 3/4, 5/8, 1/2, 3/8, 1/4, 1/8; 12-15 are K0-K3 (RGB); 16-31 are single channels (R, G, B, A of K0-K3, replicated for colour). Alpha selectors (`KASEL_*`): fractions 0-7, 16-31 single channels. Selector values outside these sets are rejected.
- Swap tables (`GX_SetTevSwapModeTable`): four tables, each assigning a source channel (R, G, B, A) to each output channel; per stage, one table applies to the raster colour and one to the texture colour (`GX_SetTevSwapMode`). The generator emits the channel selection as a constant swizzle.
- Raster channels: COLOR0A0 and COLOR1A1 (the lit vertex colours from the XF stage), zero; the two alpha-bump sources need indirect texturing and are rejected.
- A stage with a texture input but no bound map/coordinate is rejected (the value hardware produces for a `GX_TEXMAP_NULL` stage is not asserted without a source; the game's actual use decides).
- Texture lookups sample with the per-map sampler (section 6) at the coordinate set's first two components, divided by the third for projective sets (key mask).

### Indirect texturing (design)

Per `GX_SetTevIndirect` / `GX_SetIndTexMatrix` / `GX_SetIndTexCoordScale`: a stage may first sample an indirect texture (its own coordinate set, scaled by the indirect coordinate scale), take three 8-bit components (formats 8, 5, 4 or 3 bits wide), apply a bias, multiply by one of three 2x3 matrices with a power-of-two exponent, wrap the resulting offsets (or not), optionally add the previous stage's offsets, and add them to the stage's normal coordinates before the main lookup; the bump-alpha variants feed a component into the raster alpha. The exact bit-level formulas will be derived from the register layout when the GX core's survey tells which indirect modes the game uses; the first implementation covers exactly those and keeps rejecting the rest. The key already reserves a per-stage indirect index.

### Alpha compare

`GX_SetAlphaCompare(comp0, ref0, op, comp1, ref1)`: the pixel passes when `(a comp0 ref0) op (a comp1 ref1)` with comp in NEVER, LESS, EQUAL, LEQUAL, GREATER, NEQUAL, GEQUAL, ALWAYS, and op in AND, OR, XOR, XNOR. `a` is the 8-bit alpha of PREV after the last stage (generated as `(uint)(saturate(a) * 255 + 0.5)`), the references are 8-bit integers from the constant buffer, and a failing pixel is discarded (`discard`). Interaction with depth: `GX_SetZCompLoc(TRUE)` performs the depth test and update before texturing/alpha test (early Z), so a pixel that later fails the alpha test still updates depth. D3D's `discard` prevents the depth write. The game's use of early Z together with a real alpha test is detected in milestone 2 and reported loudly if it occurs (emulating it needs depth written from the shader or a depth pre-pass).

### Fog (design)

`GX_SetFog(type, startz, endz, nearz, farz, color)` with types linear, exp, exp2, reverse-exp, reverse-exp2. libogc derives, for perspective projections, `a = (far*near) / ((far-near)*(end-start))`, `b = far/(far-near)`, `c = start/(end-start)`; the hardware turns the 24-bit screen depth z into `ze = a / (b - z/2^24)`, takes `f = clamp(ze - c, 0, 1)` (orthographic projections use a linear function of z instead), applies the type function (linear: f; exp: 1 - 2^(-8f); exp2: 1 - 2^(-8f^2); reverse exp: 2^(-8(1-f)); reverse exp2: 2^(-8(1-f)^2)), and blends the pixel colour towards the fog colour by that amount. The parameters and the type go in the constant buffer and the key; `GX_InitFogAdjTable` (range adjustment for wide fields of view) is a gap until the game is seen to use it. **Verify** the exponential forms against captures before relying on them.

## 5. Depth range and viewport

GX perspective and orthographic projections map the view volume to clip z in `[-w, 0]` (near -> -w, far -> 0); for example libogc's `guPerspective` gives z/w = -1 at the near plane and 0 at the far plane. `GX_SetViewport(x, y, w, h, nearz, farz)` maps that range to the depth range `[nearz, farz]` (0..1) of the 24-bit depth buffer. The vertex shader outputs `z_d3d = w * far + z * (far - near)` (and `w`, `x`, `y` unchanged), so D3D's divide gives `far + (z/w) * (far - near)`, which is `near` at the near plane and `far` at the far plane. The D3D viewport is always depth 0..1; `near`/`far` of the GX viewport travel in the vertex constant buffer. Depth clip stays on; guard-band clipping is done by the GX core's XF stage (**verify** with the IR: it states whether clipped primitives arrive already clipped).

Pixel coordinates: GX window space has its origin at the top left of the EFB and positive y down, and the XF viewport scale for y is negative, so clip-space +y is up on screen, which matches D3D's NDC; no flip is needed. The EFB is 640 x 528 pixels at most; the D3D viewport rectangle is `(xOrig, yOrig, wd, ht)` in EFB pixels (the 342-pixel bias in the BP/XF registers is a hardware guard-band offset that the GX core removes). Scissor is `(xOrig, yOrig, wd, ht)` in EFB pixels (again without the register bias) set with `RSSetScissorRects` and `ScissorEnable = TRUE`. **Verify** pixel-centre alignment (half-pixel conventions) with a hand-built scene against the capture of a known screen.

Cull mode: `GX_CULL_NONE/FRONT/BACK/ALL`. GX defines the front face as counter-clockwise; whether that equals D3D's default front after the y-flip of window space must be settled with a test triangle of known winding in milestone 2, not by assumption (**verify**).

Depth buffer: 24-bit integer in the EFB. Use `D24_UNORM_S8_UINT` (the closest quantisation); equality-based depth tests (`GX_EQUAL`, used for decals) cannot be bit exact across different rasterisers and are reported when the game uses them. `GX_SetZMode(enable, func, update)` maps to the D3D depth-stencil state with the eight GX comparison functions mapped to the eight D3D comparison functions one to one. `GX_SetColorUpdate` / `GX_SetAlphaUpdate` map to the render-target write mask.

## 6. Textures and samplers (design)

Textures arrive decoded to RGBA8 with a content key (GX core); the renderer keeps a cache keyed by content key plus format, and uploads the full mip chain the game provides. Sampler state per map from the BP texture-mode registers: wrap (clamp, repeat, mirror) -> `D3D11_TEXTURE_ADDRESS_CLAMP/WRAP/MIRROR`; the six GX filters (near, linear, near_mip_near, lin_mip_near, near_mip_lin, lin_mip_lin) -> the D3D11 min/mag/mip filter combinations; LOD bias, min/max LOD -> `MipLODBias`, `MinLOD`, `MaxLOD` (**verify** the GX LOD calculation with the edge-LOD and bias bits against captures: D3D computes LOD from screen-space derivatives, GX from its own gradient calculation, so the selected mip may differ by a fraction). Anisotropy is off by default (an enhancement). Textures produced by EFB copies are GPU textures (section 7), bound by the address they were copied to.

## 7. Pixel formats and EFB

`GX_SetPixelFmt(RGB8_Z24 | RGBA6_Z24 | RGB565_Z16 | Z24 | Y8 | U8 | V8 | YUV420)`. The EFB is stored as an 8-bit RGBA target plus the depth buffer; the format changes what is stored and read back:

- **RGB8_Z24**: exact. No stored alpha: the target's alpha channel is written and read as the TEV alpha (it does not matter for presentation); `GX_SetDstAlpha` has no effect in this format on hardware (**verify**: what a DSTALPHA blend factor reads here).
- **RGBA6_Z24**: 6 bits per channel. Planned handling: the shader quantises its output to 6 bits and expands it again to 8 bits by bit replication (`(x << 2) | (x >> 4)`; **verify**), so that stored values are already in the 6-bit set and later blends read what hardware would read. The quantisation is applied to the shader output before the fixed-function blend, so a blended result can differ from hardware by one 6-bit step (documented approximation; a post-blend quantisation pass is the exact alternative). Dithering (`GX_SetDither`) is an option that is not applied by default (**verify** whether the game enables it).
- **RGB565_Z16**: 5/6/5 bits with a 16-bit depth buffer whose near/far allocation is chosen by `GX_SetPixelFmt`'s Z format (linear, near, mid, far); same quantise-and-expand approach; the depth quantisation is applied through the depth range or a 16-bit depth target.
- **Z24, Y8, U8, V8, YUV420**: copy-only formats used when the EFB is the source of depth or planar copies; handled in the copy shaders.
- Generation today rejects every format except RGB8_Z24 with the reason "EFB pixel format ... not emulated yet".

**Destination alpha** (`GX_SetDstAlpha(enable, alpha)`): when enabled, the alpha channel written to the EFB is the constant `alpha`, while blending still uses the pixel's own TEV alpha as the source alpha. D3D11 expresses this with dual-source blending: the shader outputs the colour (and source alpha) on target 0 and the constant on target 1; the colour blend uses source alpha as usual and the alpha blend uses `SRC1_ALPHA` with destination factor zero. This is not combinable with logic ops (section 8).

## 8. Blending, logic ops, depth and alpha masks

`GX_SetBlendMode(type, src, dst, op)`:

- `GX_BM_NONE`: blending off (write colour).
- `GX_BM_BLEND`: `src * sf + dst * df` with the factors ZERO, ONE, SRCCLR/DSTCLR (source colour is usable as a factor for the destination, destination colour for the source), INV versions, SRCALPHA, INVSRCALPHA, DSTALPHA, INVDSTALPHA, mapped one to one onto `D3D11_BLEND_*`. The alpha channel is assumed to use the same equation with colour factors replaced by their alpha counterparts (**verify**).
- `GX_BM_SUBTRACT`: destination minus source, clamped at 0: `D3D11_BLEND_OP_REV_SUBTRACT` with factors ONE and ONE.
- `GX_BM_LOGIC` with the 16 logic ops (CLEAR, AND, REVAND, COPY, INVAND, NOOP, XOR, OR, NOR, EQUIV, INV, REVOR, INVCOPY, INVOR, NAND, SET): D3D11.1 logic ops apply only to integer render-target formats and need hardware support (`OutputMergerLogicOp`). Planned implementation: draws with a logic op other than COPY render to a typeless target viewed as `R8G8B8A8_UINT` with the shader producing packed integer colours, when the device supports it; otherwise the draw runs as a shader read-modify-write against a copy of the target. Any logic op other than COPY or NOOP reached by the game is reported by the first draw that uses it (it is a rare feature).

Fixed-function blend, depth-stencil, rasteriser and sampler state objects are created through small caches keyed by the decoded state, like the shader cache.

## 9. EFB copies, XFB and widescreen (design)

- **EFB to XFB** (`GX_CopyDisp`): source rectangle from `GX_SetDispCopySrc`, destination size from `GX_SetDispCopyDst`/the Y scale (`GX_SetDispCopyYScale` maps the 528-line EFB window to the XFB height), the 7-tap vertical deflicker filter (`GX_SetCopyFilter`), gamma (`GX_SetDispCopyGamma`), and the optional clear of colour and depth afterwards (`GX_SetCopyClear`, with the colour and Z update masks applying to the clear). The result is a GPU image keyed by the XFB address and picked up by the presenter at the VI latch.
- **EFB to texture** (`GX_CopyTex`): destination formats I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, A8, R/G/B/GB/RG 8, Z formats, optional mip generation; the shader reproduces the format's bit depth (for example 4-bit intensity) and the RGB-to-intensity weights (**verify**); the result is bound later as the texture at that address.
- Copies may overlap draws: the renderer is the only writer of the EFB and executes the IR commands in order, so no explicit synchronisation with the guest is needed beyond the finish/token events that the GX FIFO model already completes.
- **Widescreen** (opt-in): the 3D projection is widened by the GX core's enhancement path, not here; the presenter only changes the display aspect (`WindowOptions::aspectNum/Den`).

## 9a. EFB peeks and pokes (platform model done; renderer provider in milestone 2)

CPU access to the EFB goes through the window `0xC8000000-0xC8FFFFFF`: the address is `base | x << 2 | y << 12 | type << 22` (x, y 10 bits; type 0 = colour as 0xAARRGGBB, 1 = 24-bit depth), as `GXPeekARGB`/`GXPeekZ` build it. The platform decodes it (`src/hle/dev_efb.*`) and forwards to an `EfbAccess` provider that the renderer implements. The default provider, used until the renderer is attached, answers depth peeks with `0x00FFFFFF` and logs the first use at each call site; colour peeks and all pokes stop loudly. The game peeks Z first at retrace 1378 (`0x801CE03C`, pixel 410, 139). The renderer's provider reads one pixel back from the GPU (colour or depth), converts it to the EFB pixel format and Z format the IR reports, and for `S > 1` takes the sample at the centre of the pixel's block (section 3).

**Known native difference:** game logic that depends on a depth peek sees GPU-precision results (the depth buffer is a D24 target rasterised by the host GPU), which can differ slightly across GPUs and from the hardware's own interpolation. It is unaffected by the resolution scale. A pick that compares a peeked Z against a threshold is the sensitive case.

## 9b. Renderer status (milestone 2)

`gfx::Renderer` (`src/gfx/gfx_renderer.*`) is a `gx::Backend` registered on `gx::backendHub()` by `awl-host` whenever a window or `--capture-frames` creates a device. It renders on a 640 x 528 RGBA8 EFB target with a D24S8 depth buffer (scale 1x; the resolution factor is not wired beyond that).

| Part | State |
| --- | --- |
| Vertices, viewport, scissor, depth range | rendered: IR clip space with `z + w`, D3D viewport depth range = the GX viewport range, scissor inclusive, no backend culling (IR convention 6) |
| TEV | generated per key (section 4): arithmetic stages with bias/scale/clamp/register chaining, all compare modes on colour and the A8 form on alpha, konst, swap tables, raster channels, texture stages with `TexCoordScale / map size` and the divide by q; cached |
| Alpha compare, fog | alpha compare with all logics; fog for perspective projections, all five curves (formulas of section 4, **verify**) |
| Textures, samplers | RGBA8 textures by content key with their mip chains; wrap, the six filters, LOD bias and range; texture maps backed by an EFB copy |
| Depth, blend, masks | depth func/write; blend (all factors, subtract), colour/alpha write masks; logic op copy and no-op |
| EFB copies | to XFB (vertical filter taps as supplied, clamp rows, y-scale by destination size, clear of colour/alpha/z over the copied rectangle) and to texture RGBA8 (half scale by 2x2 box); presented by `RendererSource` at the VI latch |
| EFB depth peeks | `EfbAccess` provider reading the depth buffer (the host installs it) |

Points and lines are expanded to quads on the CPU (sizes in 1/6 pixel; point sprite texture offsets applied in normalized coordinate units). Indirect texturing is generated for 8-bit offsets with the static matrices: the lookup samples the indirect map at its coordinate set scaled by `TexCoordScale / 2^scale`, takes (S, T, U) = (A, B, G) minus the bias, applies the matrix and divides by 256 to get texel offsets, wraps the base coordinate per the wrap codes, adds the previous stage's coordinate when asked, and samples with the sum.

Reported loudly (first use on stderr, counted, listed by `Renderer::unsupported()` and in the host summary) and the draw skipped: logic ops other than copy/no-op, indirect texturing with narrow formats, bump alpha or the S/T-only matrices, orthographic fog, fog range adjustment, cylindrical wrap, EFB formats other than RGB8_Z24, early depth test with an alpha test, texture maps that are unbound. Rendered but reported as approximate and counted: indirect offsets (the /256 scale and the (A, B, G) order are not verified), point sprite texture offsets, range bias (ignored), copies to textures in formats other than RGBA8 (bit depth and intensity conversion not applied), mipmap generation, gamma other than 1.0.

Caches: textures by content key with a 512 MB least-recently-used budget (trimmed between draws), copy textures by id (at most 512 kept), vertex/index data in ring buffers (NO_OVERWRITE appends). Debug aids: `AWL_GFX_OCCLUSION=<first draw seq>,<count>` prints the samples each draw passes; `--capture-from F --capture-every K` select the captured frames. Colour peeks and pokes stop loudly. Dither is ignored (RGB8 EFB only).

Evidence: the logos of the start-up sequence (three crossfaded textured quads over a cleared EFB) render as the game's own images with the XFB copy filter applied; `awl-host --capture-frames` gives byte-identical BMPs across runs and across devices for a given run (checked on the hardware adapter: two runs of frames 340-341 are identical, and the boot trace is unchanged by capturing). **Fixed: the scene after the logos was invisible because D3D ignored the viewport.** The GX core decodes the viewport depth range from the XF registers as near = 1/1024, far = 16777216 / 16777215, which is just above 1.0. D3D11 silently ignores `RSSetViewports` when MaxDepth exceeds 1 and keeps the previous viewport, here the 64x32 viewport of the render-to-texture copy pass, so every scene draw was clipped to a 64x32 corner. The renderer now clamps the depth range to [0, 1]. **Native difference:** GX's far value 16777216 / 16777215 maps a hair beyond 1.0; clamping it to 1.0 changes depth values by at most one step of the 24-bit depth buffer, which is negligible. Found by comparing the viewport that was set with `RSGetViewports` for the first failing draw (`AWL_GFX_VPCHECK=1`); a replay of the same vertices in a fresh renderer drew 23,624 pixels where the live draw drew 0. Regression test: `gfx_renderer_tests` section 5b3 (render-to-texture pass, then a full-screen composite with the game's exact depth range). The title screen now renders (sign, town, copyright line); the menu text is garbled by the font-atlas guest-memory bug that belongs to another track. Diagnostic switches kept: `AWL_GFX_FRAMESTATS`, `AWL_GFX_OCCLUSION`, `AWL_GFX_VPCHECK`, `AWL_GFX_LITPROBE=<seq>` (bound D3D state and lit-pixel counts around one draw), `AWL_GFX_DUMPDRAW=<seq>` with `AWL_GFX_DUMPDRAW_FILE`, `AWL_GFX_DEBUG_NOALPHA`, `AWL_GFX_DEBUG_NODEPTH`, `AWL_GFX_DEBUG_NOSCISSOR`, `AWL_GFX_DEBUG_SOLID`. **Sharpness against Dolphin.** The title picture with the XFB copy's vertical filter applied (the taps the game sets, {8,8,10,12,10,8,8}/64, as the hardware does) is visibly softer than the owner's Dolphin screenshots and the CPU reference render, which apply no filter; with `--no-copy-filter` the DX11 picture has the same sharpness as both. Dolphin's default setting leaves its copy filter off. Which one is the default here is a decision for the owner (faithful to the console, or matching the reference screenshots): the renderer applies the filter unless `--no-copy-filter` is given. Not yet compared with the owner's Dolphin captures: the vertical filter (applied exactly as the copy command supplies it, so the logos look softer than a raw EFB), gamma, the depth-test-off semantics, and the culling convention beyond the logo quads.

## 9c. Performance (speed block, 2026-11-21)

Goal: the title screen at 100% of real time in the window. The slowness was not the recompiled code (it runs the title at about 212% of real time alone); it was the per-draw cost of the renderer (about 4,500 draws per frame on the title screen, 11.5 million in the run) and the capture call trace that was on by default.

**How it is measured.** `awl-host --bench <retrace>`: the DX11 device and the renderer run offscreen (no window, no pacing, no capture files; each presented frame still pays the renderer's XFB readback, as in the window) and at the end one line says `speed: retraces A-B, game X s, wall Y s, Z% of real time`, from retrace A (59.94 Hz fields) to the last. The acceptance command, on the title screen (23.66 s of game time), AMD RX 6600:

```powershell
build\Release\awl-host.exe --dol rom\main.dol --retraces 3000 --limit 0 --bench 2700
```

| Step (each one on top of the previous) | Speed | Wall time |
| --- | --- | --- |
| Before: main as it was (`--calls 16` call trace on by default) | 24% | 100.2 s |
| 1. `--calls` defaults to 0 (the capture comparison passes `--calls 16`) | 30% | 79.7 s |
| 3a+3b. The four `AWL_GFX_*` variables are read once at start (`getenv` per draw), `report()` counts string literals by address (no `std::string` per draw) | 83% | 28.4 s |
| 3c. What a draw derives from its pipeline state alone (pixel shader, constants, sampler / blend / depth objects, viewport, scissor, the maps to bind) is prepared once per `stateId` and reused by the draws that follow with the same id (most consecutive draws share the id); the shader cache key is the fixed-size `PixelShaderKey` with a word-wise hash instead of a `std::string` | 104% | 22.7 s |
| 3d. D3D state shadowing: a setter is called only when the value differs from the last one this renderer set; the vertex / index ring buffers stay bound at offset 0 and each draw addresses its data with the start index and base vertex; the constant buffer is rewritten only when the state id changes; the per-draw SRV unbind is gone | 128% | 18.5 s |
| 3e. Triangle draws (the common case) convert the vertices straight into the mapped vertex buffer and copy the indices straight into the mapped index buffer (no intermediate vector) | 136% (reproduced: 136%, 137%) | 17.4 s |

Not done because the target was met: batching several draws into one Map (the brief's last item) and step 4 (flat `lookupFunction` table, write-gather store fast path). What is left on the profile at 128% (sampling profiler, RelWithDebInfo): the GX core's vertex transform about 9%, the XFB readback wait about 7% (a synchronous GPU to CPU copy per presented frame), memory moves about 6%, the rest spread thin. If more speed is needed: present the XFB texture on the GPU instead of reading it back, then batching.

**Rules that keep this exact.**
- Output is byte-identical to before: the 121 BMPs of presented frames 0, 20, ..., 2400 (logos and title) of `--retraces 3000 --capture-frames`, the `--calls 16` `boot-trace.log` of the headless run, and `EXIT: main loop reached after 11135124000 instructions` were compared against baselines taken from the previous `main` (frames after step 3c, frames and traces after 3d and 3e); the renderer's counters (draws, triangles, shader hits, the "not exact" counts) are unchanged too.
- `stateId == 0` is "no id": such draws (the hand-built scenes of the tests) are prepared on every draw. A texture that went away between two draws of one state sends the draw down the full path, which reports it exactly as before.
- The shadowed D3D state is valid only while this renderer is the one using the device context. `efbCopy` drops it itself; anything else that draws or binds on the shared context between draws (presenting, blitting) must be followed by `Renderer::invalidateBoundState()`. `VideoOutput` does this through `FrameSource::afterPresent()` after every present or capture.
- The debugging aids that read the expanded vertices (`AWL_GFX_DUMPDRAW`, `AWL_GFX_OCCLUSION`) switch the triangle path back to the vector-based upload.

## 10. Tests (all headless; no window is ever opened)

| Test | Covers |
| --- | --- |
| `gfx_device_tests` | hardware and forced-WARP device creation; 1:1 blit identity through the real pipeline; letterbox maths; BMP encode/decode round trip and file bytes; diagnostic frame; `VideoOutput` capture (first N of a mixed latch sequence, same bytes on hardware and WARP, loud failure on a bad folder) |
| `gfx_pacing_tests` | the 1001/60000 s schedule at 10 MHz and 1 GHz counters (60,000 fields = 1001 s exactly; 10^7 fields by hand), early/late/re-anchor/pause behaviour, the speed meter (100%, 50%, 250%, reset), window title and overlay text, text drawing (clipping, case, darkened box only inside its area), host clock, four paced retraces take at least four field periods |
| `gfx_input_tests` | key table against the SDK button constants, sticks, triggers, XInput values, focus rules |
| `gfx_renderer_tests` | hand-built IR scenes through `Renderer` and the XFB copy: pipeline state ids (reuse, a new id, drawing after an EFB copy), quad placement and orientation, blending (src alpha), depth test/write with the viewport depth range and depth peeks, a texture in four quadrants with nearest sampling and `TexCoordScale`, the copy filter on an edge (taps worked out by hand), the five fog curves, and loud reporting of unsupported states |
| `gfx_tev_tests` | key identity and hashing; every unsupported feature is rejected with its reason; offscreen draws of hand-computed combiner results (pass-through, konst modulate, bias/scale/clamp, subtract with alpha scale, register chaining, alpha compare with the AND, OR, XOR and XNOR logics, texture with swap table); cache hits and cached rejection |

The draw tests' expected pixels are worked out from the combiner equation in the test source, not produced by the generator.

## 11. Open items

- Milestone 2 (IR rendering) waits for `src/gx/gx_ir.h`.
- Window behaviours (resize, fullscreen toggle, Esc/Alt+F4/close, focus pause, input) are written but not run; they need the owner at an agreed time. The vsync cadence on 144 Hz (2/3 refresh pattern), the paused screen and the 2x first window are in the pending window check above.
- Debug variable `AWL_PAD_SCRIPT` (see (a) above; clear it after use, like `AWL_CAPTURE_FRAME`).
- All **verify** points above are settled by comparison with the owner's Dolphin captures once the first screens render.
