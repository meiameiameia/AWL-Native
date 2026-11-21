# P3 increment 1: New Game into the opening (no memory card)

State of the note: 2026-11-21, headless runs only; the owner's window test is pending. Replays the path title -> New Game -> "play without saving" -> opening (beach, Takakura's farm tour, naming screens) -> the hero standing in his house with the HUD (first controllable moment).

## What had to change

1. **Locked-cache DMA takes physical addresses** (`tools/recomp/runtime/guest.cpp`): see `docs/research/hle-platform.md` (locked-cache DMA). Before, "Yes" at the save question stopped the run (`STOP: MEMORY FAULT`, pc `0x801B264C`).
2. **Orthographic fog** (`src/gfx/gfx_tev.cpp`): see `docs/research/gfx-dx11.md`, "Orthographic fog". Before, every draw of the characters' colour pass was skipped (8.7 M draws, 3 shaders rejected) and Takakura and the hero were black silhouettes.

Nothing else stopped the path: no EXI, DVD, PAD, GX-decode or unimplemented-instruction stop between the title and the first controllable moment.

## Headless recipe

Run from the repository root with `AWL_PAD_SCRIPT` set (debug variable, neutral pad mode only; `from-to=HEXBUTTONS[,stickX,stickY];...`, numbers are `PADRead` call numbers, one per game loop pass, which equals the presented frame number to within a few frames in this path). Buttons: A `0100`, Start `1000`. Stick Y is +80 for up, -80 for down; X +80 for right.

```powershell
$env:AWL_PAD_SCRIPT = "2000-2010=0100;2300-2310=0100;2500-2520=0000,0,80;2600-2610=0100"   # + the A mash below
build\Release\awl-host.exe --dol rom\main.dol --disc disc --retraces 100000 --limit 0 --capture-frames 160 $out --capture-from 15500 --capture-every 60
Remove-Item Env:AWL_PAD_SCRIPT
```

Script parts (the whole string is one line; `mash` is `;T-(T+5)=0100` for T = 2800, 2840, ... every 40 calls):

| Calls | Input | Effect |
| --- | --- | --- |
| 2000-2010 | A | title: "New Game" -> "No Memory Card inserted." |
| 2300-2310 | A | card message -> "You will not be able to save your game. Is that OK?" (cursor starts on **No**: A there returns to the title, faithful) |
| 2500-2520 | stick up | cursor to **Yes** |
| 2600-2610 | A | Yes: black, then the beach (frame ~2700) |
| mash, 2800-16900 | A for 6 calls every 40 | advances every text box; at the naming screens it types the letter under the cursor (an "A" each press, up to the name length), and at "Is this name OK?" the cursor is on Yes so the next A confirms: dog, cow, farm and farmer all end up named "AAAAAAAA" or shorter without any special input |
| 17400-17700 | stick down | the hero walks towards the camera in his house (checked) |
| 17900-18200 | stick right | and to the right (the camera follows) |

To type real names instead: at each naming screen move to a letter with the stick and press A, press Start to jump to OK, then A (from the brief; the headless recipe above does not exercise Start, so confirm it in the window). Not needed for the scripted path.

Timeline (approximate presented frame numbers read from captures every 60-100 frames; 30 fps game, so about two retraces each): beach narration 2700-3700; farm tour with Takakura 3700-16000 (dog house about 7700, cow and the cow name screen about 9000-9800, farm name about 10500-11200, farmer name about 12000-12500, the village introductions 12500-14500, the harvest sprites Flak, Nak and Nic about 15500-16300); "A few days later..." and "The Beginning" (about 16700); the hero in his house with the controller-icon HUD and the clock (AM 6:xx) from about 16900. From there stick input moves him. Light through the window moves as the in-game clock runs.

Caveats:
- `--retraces N` stops after N *consecutive qualifying retraces* ("main loop reached"); scene changes (black frames, fades) reset that counter, so on this path N=100000 never triggers and the run ends at the `--limit` or the end of the capture window. Use `--limit` (instructions) to bound a run: about 5 M instructions per presented frame here; the whole path to the controllable moment is about 90 G instructions (about 4 min of wall time in Release headless).
- Files and frames go under `%LOCALAPPDATA%\dev-scratch\awl-native\<yyyymmdd-topic>`, never the repository.
- Clear `AWL_PAD_SCRIPT` afterwards.

## Verification (2026-11-21, this PC, Release)

- Whole path: no STOP, **0 skipped draws, 0 rejected shaders**; "not exact" notes only the known ones (EFB copy to non-RGBA8 / with mipmaps in the naming screens, indirect offsets scale, point sprite offsets, texture coordinate range bias).
- Title unchanged: frames 0..2400 step 20 (121 BMPs), the capture-mode trace, the `--calls 16` boot trace and `EXIT: main loop reached after 11135124000 instructions` are byte-identical between the binary built from `60a4d28` and the new one.
- Farm tour bench (`--limit 70000000000 --bench 7000` with the script, retraces 7000-25898, 315 s of game time, beach to village introductions with naming screens): **158% of real time**. The same script to the end of the capture window (retraces 8000-45434, includes the idle in-house part): 192%. Bench never opens a window.
- The hero in his house is coloured and moves with the stick (frames captured, not in git).

## Expected to be wrong or missing for now

No sound (P4). No saving: "play without saving" is the only path, and Continue does nothing useful (the memory card is a later P3 increment). Only headless behaviour is verified; presentation (vsync, window size) was checked on the title only.

## Owner window test (pending, at a time agreed beforehand)

From the project folder: `build\Release\awl-host.exe --dol rom\main.dol --window`. Keys: W/A/S/D = stick, Space = A, Enter = Start, Backspace = B, Esc quits, F1 hides the corner box.

1. Title: Enter on New Game (or Space). A message says no memory card is inserted: Space.
2. "You will not be able to save your game. Is that OK?": the cursor starts on **No** (as in the original). Press **W** (stick up) to move to **Yes**, then Space.
3. The opening plays by itself: beach narration, Takakura's farm tour. Space advances the text. Takakura and the hero should now be coloured (before they were black silhouettes). Compare with what you remember from Dolphin; tell me if colours or lighting look off.
4. Naming screens (dog, cow, farm, farmer): WASD moves the cursor, Space types the letter, Enter should jump to OK (not yet checked headless), Space confirms, then Yes at "Is this name OK?" (Space). If Enter does nothing, move the cursor onto OK with WASD.
5. After "The Beginning" the hero stands in his house; walk with WASD. Report the speed in the corner box (expected around 100%; the headless bench says 158% without a window) and anything that stops with a message box.
