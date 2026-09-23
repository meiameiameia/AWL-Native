# Input-to-player trace (in progress)

Target: NTSC-U `GYWE41` `main.dol`, SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`. Addresses below were checked against that DOL with the repository's read-only Ghidra scripts. Raw decompilation and instruction exports are local, ignored `build/player-input-trace/` files. Function labels describe observed behavior only; they are not claims of completed translation.

## Confirmed PAD pipeline

1. `FUN_8000AD40` calls `FUN_80213DA4` at `0x8000AE1C` during startup. The latter initializes PAD-related state at `0x80341430`, configures sample storage and per-channel state, and calls the lower-level `FUN_801C06D4`. The read-only `hmawl` name for `0x80213DA4` is `HSD_PadInit`; behavior here is based on this project's DOL, not copied code.
2. `FUN_8000AA54` calls `FUN_80213CE4` at the end of each main-loop iteration. `FUN_80213CE4` calls `FUN_80212720`, `FUN_802136C8`, and `FUN_80213B44` in order.
3. `FUN_80212720` calls `FUN_801C0824` to collect four low-level controller records, then queues them in 52-byte slots. `FUN_801C0824` reads channel status and distinguishes disconnected/error cases; its complete platform behavior has not been translated.
4. `FUN_802136C8` consumes a queued record, applies stick processing through `FUN_802129CC` and direction/button processing through `FUN_80213180`, and writes per-channel state beginning at `0x80341464`. `FUN_80213B44` copies/filters that state into the consumer-visible array beginning at `0x80341574` (68 bytes per channel).
5. In the consumer-visible first-channel record, `0x80341574` is current button bits, `0x80341578` previous bits, `0x8034157C` newly pressed bits (`current & (previous ^ current)`), `0x80341580` repeat bits, and `0x80341584` released bits (`previous & (previous ^ current)`). The disassembly/decompilation of `FUN_80213B44` supports these exact operations. The remaining bytes include axes/status but require a separate layout audit before PC mapping.

## World-map character ownership and dispatch

- `FUN_80013F60` (which references `SceneWorldMap.cpp` in the verified DOL) allocates `0x15CC` bytes, constructs the character object through `FUN_8002FDF8`, and stores it at scene offset `+0x24`. The constructor installs tables beginning at `0x8029FF18` and initializes its state through `FUN_80031D7C`. The scene destructor `FUN_800146BC` destroys that same `+0x24` object. Together these provide high-confidence ownership of the world-map player object, rather than just an address-range guess.
- `FUN_80031D7C` writes the state ID at character offset `+0x1364`, then indexes the runtime callback table at `0x802A3AA8` with a `0x3C`-byte stride and jumps via `FUN_80235D7C`. For example, state `0xE0` indexes `0x802A6F28`; its read-only DOL template entry at `0x802A3670` points first to `FUN_80047D3C`. This establishes state dispatch, **not** the gameplay name or full effects of state `0xE0`.
- `FUN_8003083C`, called from several character-state callbacks, reads signed stick bytes at `0x8034158C`/`0x8034158D`, derives a direction using world/camera-related state, updates character fields `+0x136C` through `+0x137C`, and passes proposed/resulting positions through `FUN_8001DE44`, `FUN_80152724`, and `FUN_80030E18`. The meanings and PC equivalents of these surrounding world-space operations remain to be established before faithful movement can be implemented.

## Downstream leads, not yet translated

- `FUN_800472BC` reads pressed bit `0x100` at `0x8034157C` before branching through a large context-dependent interaction/state path. `FUN_800471F8` checks pressed bit `0x1000` after a separate eligibility check, then calls `FUN_800310A4` and `FUN_80031D7C` with state `0xE0`. The gameplay meanings of these masks and state numbers have **not** been established from this trace.
- `FUN_800372D8` reads held bit `0x100` at `0x80341574`. The state callbacks and `FUN_8003083C` are now linked to the world-map character object, but action semantics, movement/collision effects, and state guards are not fully translated.
- The prior hypothesis that `FUN_8012F3E0` initializes input is disproven: its body and `FUN_8012F480` operate a registry of world subsystems. It must not be translated as PAD setup.

## Next translation boundary

Resolve the specific world-space and character-state operations around `FUN_8003083C` before implementing movement, and the state guards/interaction targets around `FUN_800472BC` before implementing an action. Verify individual PAD masks, axes, dead zones, focus/disconnect behavior, and frame order against the DOL before connecting `NativeInputFrame` to an original-game command. The existing keyboard/XInput capture is only native device acquisition; it does not yet drive gameplay. No current native smoke demonstrates original player control.
