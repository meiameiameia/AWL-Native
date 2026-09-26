# First playable slice: feasibility baseline

This is a bounded development candidate, not a claim that the game is playable or that the original game chooses this scene or spawn. Evidence was checked against `main.dol` SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374` and the owner's local extracted files. Raw exports, assets, probe code, and logs remain under ignored paths.

## Candidate scene and route

- Keep the existing `--scene-smoke` fixture: `/files/jimen-L-1-0-2.gpl` and `/files/jimen-L-2-0-2.gpl`. The current renderer submits their five batches each in serialized world coordinates. A Debug smoke on 2026-09-26 presented ten frames and exited successfully. Its combined mesh bounds were X `-1..261.906`, Y `-5.17188..48.1562`, Z `144..252.328`. The bounds-fit camera and chunk selection are diagnostic.
- Probe the route from `(X=120, Z=168)` to `(X=128, Z=168)`, crossing the visual chunk boundary near X `123..125`. Both local movement COL variants validated. In each, the existing primary height sampler succeeded at every 0.5-unit point on that line, with raw surface flags `0x0008` throughout. Endpoint heights were about `5.21` and `6.91`; the selected COL leaf changed across the line. These are data samples, not decoded walkability semantics.
- A local ignored C++ probe built with MSVC `/W4` called the current radius-pass helper for 80 successive 0.1-unit proposals along the route using radius `0.3`. Both `jimen-move.col` and `jimen1-move.col` returned 80 supported results, with zero reported contacts and zero reverts. This only exercises the bounded helper. `FUN_8002009C` terrain adjustment, `FUN_8001DE44` directional contact metadata, and the shared resolver are not connected, so the route is not yet accepted movement.

## Player and interaction leads

- `FUN_80013F60` constructs the world-map character through `FUN_8002FDF8` at scene offset `+0x24`. The constructor gets a model-related resource from slot zero through `FUN_8007DECC`/`FUN_8007E248`. `FUN_8002BBB8` reads a table beginning at `0x80249A6C` whose first name is `boy_0.arc`, and `FUN_80028104` reads an animation table beginning at `0x80248B40` with `boy_0.anm.arc` and subarchives. These are strong player-asset leads, but the selected model variant, archive contents, skeleton, and rendering path are not validated. The native project has no ARC/SKN player-model decoder.
- `FUN_800472BC` tests newly pressed bit `0x100` after querying nearby context through `FUN_80010A94`. It branches on a context code and may create an action object through `FUN_80011680` before state changes through `FUN_80031D7C`. The branch for context code `0x10` maps to internal case `1`; its world target, response, and return behavior are still unknown. Do not present this as a verified inspect action or assign it to a placed object yet.

## Identity discrepancy

Both local image formats report disc ID `GYWEE9`, and the extracted `disc/sys/boot.bin` agrees. The extracted `main.dol` and `rom/main.dol` match the project's verified SHA1. Repository guidance and `tools/extract_disc.ps1` currently expect `GYWE41`, so the extraction script would reject these local images. Preserve the current extraction; resolve which ID the project should authorize before changing the extraction rule or replacing files.

## Next small increments and observable checks

1. Complete pinned-leaf behavior in the isolated type-1 radius passes. Test a candidate whose intermediate response crosses a leaf, retain the already verified no-contact route behavior, and pass Debug/Release `/W4` builds and CTest. This remains an inactive translation helper.
2. Translate the remaining `FUN_8002009C` terrain-adjustment sequence, then the necessary `FUN_8001DE44` metadata and shared-resolver acceptance boundary. Check slopes, edge/wall contact, failure, and both COL variants before connecting player position updates.
3. Add a fixed development scene using the two supported chunks, a clearly marked temporary player marker, a fixed spawn on the probed route, and a controlled camera. A focused native smoke should show input moving the marker across the seam while collision changes its accepted Y; loss of input focus should stop movement. This is a rehearsal milestone, not the first playable MVP.
4. Decode only the verified player ARC/SKN resources and states needed for idle/walk on this patch, with asset bounds and visible native checks. Replace the marker only after that path is supported.
5. Isolate one `FUN_800472BC` context target and its guards, action result, and return state from DOL and scene data. Then implement one verified inspect-and-return interaction. The first playable MVP requires the real player, controlled movement and camera, that interaction, and reliable startup/exit in this patch. Actual gameplay acceptance remains the owner's in-game check after automated verification.

Do not infer full scene selection, all movement states, button meaning, or broader GX support from this slice.
