# GODOT_FRONTEND.md — Phase 7/8 (G1+G2) Godot 4 frontend

Status: **implemented and validated** — static LEVEL3 arena rendering
plus a snapshot-driven player presentation through a retained
GDExtension bridge. Dynamic objects, animation, HUD, audio, and the
fx drawers remain out of scope (see "Limitations").

This document covers the in-repo frontend. The architecture
rationale and the disposable-spike evidence live in
`docs/GODOT_INTEGRATION_AUDIT.md`; this file is the retained,
authoritative description of what is actually checked in.

## Ownership boundary

`mdk_core` is authoritative for all game semantics: file parsing
(DTI/CMI/MTO/MTI/FTI), the traversal runtime, player/camera state,
`ArenaRenderData` decoding, material classification, BSP painter
ordering (`arenaRenderOrder`), and palette semantics.

Godot (scene + GDScript + GDExtension glue) is presentation only:
it converts core-provided snapshots into `ArrayMesh`/`Image`/
`Camera3D` objects and forwards input bits back into the core. No
MDK file is parsed and no game rule is evaluated outside the core.

## Layout

```
frontend/godot/
  project.godot                     engine config only (no data)
  main.tscn                         Node3D + ArenaMesh + PlayerRoot +
                                    PlayerBoxWire + CollisionDebug +
                                    Camera3D + DebugUI
  src/main.gd                       driver + --smoke / --screenshot
  shaders/arena_unshaded.gdshader   unshaded masked-fetch shader
  gdextension/
    mdk_bridge.gdextension          descriptor (entry: mdk_godot_library_init)
    CMakeLists.txt                  standalone build; pins godot-cpp
    src/mdk_math.h                  pure-scalar MDK->Godot math
    src/mdk_convert.h               godot-cpp adapters over mdk_math
    src/arena_presenter.{h,cpp}     bundle -> Image/ArrayMesh
    src/mdk_bridge.{h,cpp}          RefCounted bridge class
    src/register_types.cpp          GDExtension init/terminate
  build.sh                          configure+build wrapper (+ ext list)
  run.sh                            canonical launcher (Godot discovery)
  bin/<platform>/libmdkbridge.*     generated output (ignored)
  .godot/                           editor cache + extension_list (ignored)
```

## Toolchain

| Component | Version / commit |
|---|---|
| Godot | 4.7.2.stable.official.ed1daf0bf (native arm64) |
| godot-cpp | `507ed9d840c01a3c5b2a39af8bb4000bfac30bf5` |
| godot-cpp config | API `4.7`, `precision=single`, `target=editor`, Release |
| Compiler | Apple clang 21, C++20 |

godot-cpp is pinned in `gdextension/CMakeLists.txt`
(`MDK_GODOT_CPP_COMMIT` + SHA256 of the archive tarball). The build
reuses an existing checkout via `-DGODOT_CPP_DIR=...` or fetches the
pinned tarball via `FetchContent` otherwise.

### Prerequisite: a Godot 4.7.x binary

The frontend needs a *Godot binary* (the official macOS build is an
editor+runtime in one). Resolution order used by `run.sh` and the
pytest orchestration:

1. `MDK_GODOT_BIN=/path/to/Godot` (explicit override);
2. `godot` on `PATH`;
3. `/Applications/Godot.app/Contents/MacOS/Godot`.

Godot.app itself is never committed and no game data is downloaded.

## Build

```sh
# canonical clean build (downloads the pinned godot-cpp tarball):
frontend/godot/build.sh

# reuse an existing godot-cpp checkout (much faster):
GODOT_CPP_DIR=/path/to/godot-cpp frontend/godot/build.sh
```

Outputs (all ignored by git):

- `frontend/godot/bin/Darwin-arm64/libmdkbridge.dylib` — arm64
  extension (mdk_core + godot-cpp statically linked);
- `frontend/godot/gdextension/build/` — CMake tree incl. the
  fetched godot-cpp;
- `frontend/godot/.godot/extension_list.cfg` — see below.

`mdk_core` is recompiled from `src/core/*.cpp` as PIC — the
extension does not depend on the SDL root build.

## Run

`run.sh` is the single entry point. It resolves Godot, checks the
dylib exists, seeds `.godot/extension_list.cfg` if missing, and
defaults `--data-path` to `<repo>/original/installed` (or
`$MDK_DATA_ROOT`).

```sh
# interactive HMO_1 view (WASD move, Q/E strafe, A/D turn,
# R/F look, Space jump, Shift turbo, mouse captured, F1 collision
# wire, F3 debug text, Esc release-then-quit):
frontend/godot/run.sh

# deterministic headless smoke (dummy renderer):
frontend/godot/run.sh --smoke

# startup proof — N real frames, then exit 0:
frontend/godot/run.sh --frames 30

# framebuffer capture (real renderer, brief window):
frontend/godot/run.sh --screenshot /tmp/shot.png

# overrides — forwarded verbatim:
frontend/godot/run.sh --data-path /path/to/installed \
    --level TRAVERSE/LEVEL4/LEVEL4.DTI --arena SOME_ARENA
```

Direct Godot invocation works identically; `run.sh` only adds
discovery + defaults. `--data-path DIR` (default `$MDK_DATA_ROOT`,
else `<repo>/original/installed` relative to the project),
`--level RELDTI`, `--arena NAME` (default `HMO_1`; `""` = spawn
arena), `--smoke`, `--screenshot PATH`, `--frames N`. Relative
paths resolve against the launch directory (`$PWD`) because Godot
chdirs into the project directory.

### GDExtension discovery (important)

Godot **game mode** loads GDExtensions listed in
`.godot/extension_list.cfg` — a file normally produced by an
*editor* filesystem scan. `build.sh` and `run.sh` both seed it
(`res://gdextension/mdk_bridge.gdextension`) so no editor pass is
ever required. If it is missing, the scene reports

```
MdkBridge class missing — the GDExtension is not loaded. Run
frontend/godot/build.sh ..., then relaunch.
```

and exits 1 (the script is deliberately untyped on the bridge so a
missing extension never becomes a GDScript parse error).

Headless editor caveat (known upstream): `--headless --editor`
crashes inside MoltenVK shader conversion on Apple Silicon. Use
`--rendering-driver dummy` for editor-side scans; game mode is
unaffected.

## Screenshot semantics

`--screenshot PATH` waits 8 frames then reads back the viewport
texture and saves a PNG, exiting 0 on success. `--headless`
**always** selects the dummy rendering server — there is no
viewport texture to read, so the command fails intentionally with
exit 2 and an explanatory message (no null dereference). Run it in
game mode (no `--headless`); the window opens briefly.

## Bridge API (`MdkBridge`, RefCounted)

```gdscript
var b := MdkBridge.new()
b.initialize(data_root)        # bool — DataRoot open (no writes)
b.load_level(dti_rel_path)     # bool — DTI+CMI+MTO via the runtime
b.load_arena(arena_name)       # bool — "" selects the spawn arena
b.step_frame(dt_ms, mask)      # one traversal frame; QA input bits
b.step_frame_input(dt_ms, {    # same, with raw device state:
    "actions": mask,           #   QA action bits -> bound key codes
    "keys": [codes...],        #   held internal key codes (0..127)
    "mouse_dx": n, "mouse_dy": n, "mouse_dz": n,  # DIMOUSESTATE
    "mouse_buttons": nibble})  #   4 device buttons -> core masks
b.get_player_snapshot()        # pos/transform/box, channels, state
b.get_camera_snapshot()        # Transform3D, fov_deg, aspect, rect
b.get_collision_snapshot()     # poly/vert counts + debug line soup
b.get_input_config()           # mouse axes map, scales, btn masks
b.get_arena_render_snapshot()  # mesh, material, atlas, arrays, stats
b.get_arena_order_digest()     # cheap painter-order digest (poll)
b.get_arena_names()            # level arena list
b.is_level_loaded() / b.is_arena_loaded() / b.get_last_error()
b.shutdown()
```

Only copy-safe values cross the boundary — packed arrays,
dictionaries, Godot resources. No core pointers, spans, or mutable
ownership escape into GDScript.

`step_frame_input` returns the frame result plus an `input` echo of
the just-consumed `GameplayInputFrame` (the 0x4ce control block —
fire/jump/sniper-pulse/look flags, turn/move/strafe axes, turbo,
zoom accumulator) so tests can verify which semantic action a raw
device state produced without guessing at the internals. The frame
itself also feeds `last_` for all snapshot getters.

## Player presentation (G2)

`PlayerRoot : Node3D` is a retained presentation node. Every frame
the script applies `get_player_snapshot()["transform"]` verbatim —
a `Transform3D` built in C++ from the core's position and yaw-only
basis. The scene tree:

```
Main (Node3D, src/main.gd)
  ArenaMesh        — ordered static geometry (G1)
  PlayerRoot       — snapshot transform, nothing writes back
    DebugBody      — capsule, green grounded / amber airborne
    ForwardMarker  — blue nose box on local -Z (facing)
  PlayerBoxWire    — 12-edge AABB of the standing collision box
  CollisionDebug   — arena collision-poly line soup (F1)
  Camera3D         — core PlayerCameraPose, sibling not child
  DebugUI          — tiny QA label (F3): pos, yaw, channels
```

There is no `CharacterBody3D`, no `RigidBody3D`, and no Godot
physics or raycast anywhere on the gameplay path — the smoke test
proves `PlayerRoot` writes cannot reach core state (position is
sampled before/after deliberately corrupting the node transform).

### Player snapshot fields

`get_player_snapshot()` — every value a verbatim copy of a proven
core output (`TraversalFrameResult` / collision state); `*_mdk`
keys carry the raw MDK-space numbers for diagnostics:

| Key | Source |
|---|---|
| `pos`, `pos_mdk` | `cs.pos` (0x540c40-ish) |
| `transform` | `pos` + yaw-only basis (presentation) |
| `yaw_deg`, `pitch_deg` | `PlayerMotionState.yawDeg`, view pitch |
| `grounded` | `contactFlags & 1` |
| `arena`, `arena_display`, `frame` | core arena, shown arena, tick |
| `box`, `box_mdk` | `playerBodyBox` — standing extents |
| `query_box`, `query_box_mdk` | `playerBox` — last query AABB (volatile) |
| `loco_state` | `rt.locoState` (0x540cac dispatched code) |
| `move_vel`, `strafe_vel` | 0x540d48 / 0x540d4c channels |
| `turn_vel`, `vert_vel` | 0x540d50 / 0x540c78 channels |
| `contact`, `contact_normal` | 0x540e4c token + swept normal |
| `position_changed` | horizontal apply moved the player |
| `look_offset_deg` | 0x540d58 look offset |
| `event_type`, `event_mag` | 0x54cb00/08 pending event slots |

Two box fields because the original reuses 0x540c30..44: each
traversal-active frame's tail rebuilds the *standing* box
(`pos +- 1.25 x/y`, `pos.z .. pos.z+4.25`), then the in-frame
collision queries overwrite it with per-probe AABBs. The core now
snapshots the standing value into `TraversalFrameResult.
playerBodyBox` at the rebuild point; `box` presents that for the
wire, `query_box` keeps the last query AABB for diagnostics.

### Player orientation

`mdkYawToGodotBasisDeg(yawDeg)` treats the MDK yaw as a same-sign
Godot Y rotation: converted right = `(cos t, 0, -sin t)`, up =
`(0,1,0)`, back = `(sin t, 0, cos t)`. The smoke asserts the
`PlayerRoot` basis columns against the converted MDK forward/right
and orthonormality, and that `-Z` agrees with movement direction
and camera orientation.

### Player collision debug

- `PlayerBoxWire` — the standing body box (`box` above) drawn as a
  12-edge line box in world space every frame.
- `CollisionDebug` — `get_collision_snapshot()` returns the loaded
  arena's collision polys as a PRIMITIVE_LINES vertex soup (6
  vertices per poly, one per edge) plus poly/vert counts; F1
  toggles visibility. Visualization only — no Godot collision
  bodies are created.
- `contact` / `contact_normal` / `grounded` surface the per-frame
  contact state for the QA overlay.

## Raw mouse input route (G2)

Godot `InputEventMouseMotion.relative` accumulates into per-frame
deltas; the 4 device buttons form a nibble (`MOUSE_BUTTON_XBUTTON1`
is the 4th). Per frame GDScript forwards `{actions, keys, mouse_dx,
mouse_dy, mouse_dz, mouse_buttons}` into `step_frame_input`, which
fills `RawGameplayInput` — nothing is pre-interpreted:

```
Godot mouse event -> raw deltas/buttons -> RawGameplayInput
  -> consumeGameplayInput (FUN_00406f14 merge)
     W-set axes "ABG", scales 16/16/50, button masks 1/4/2/0
  -> existing player/camera semantics
```

The configured W-set mapping stays entirely in core: axis A turns,
axis B moves, G feeds the zoom path; button masks map LMB=fire,
RMB=jump, MMB=sniper, 4th=unmapped — all OBSERVED factory values,
verified in the smoke via the consumed-frame `input` echo (no
button semantics are hardcoded in GDScript). Raw one-frame input
latency is preserved exactly as the original consumes the previous
frame's merged block.

Interactive runs capture the mouse at startup (`MOUSE_MODE_
CAPTURED`); a click re-captures after release; Esc releases once,
then quits. F1 toggles the collision wire, F3 the debug text.

### Keyboard path

`"keys"` accepts held internal key codes (the FUN_0046b688 domain)
so the frontend can drive bindings directly; the legacy `actions`
mask still maps QA bits onto the configured bound codes. Both feed
the same `RawGameplayInput.keyLevel`, and the bridge computes
`keyEdge = level & ~prev` per frame like the original's poll diff.
WASD/QE/AD/RF/Space/Shift behavior is unchanged.

### Optional real Kurt model — status

Not used: the `K_*` records in `TRAVSPRT.BNI` use a different
container layout (length/count/offset-table) than the proven
`FUN_00428400` geometry-record layout the model parser handles, so
no validated static pose exists today. Missing evidence for a real
mesh: the K-record container decode, the per-part transform/bone
mapping, and the animation semantics (all UNKNOWN — G5 territory).
The capsule proxy is therefore the honest G2 visual.

## Arena transitions (G2 partial)

`stepCore_` compares `last_.curArenaIndex` against the displayed
arena each frame and calls `load_arena` on the new arena when a
render block exists, so normal traversal through arena boundaries
keeps the view synchronized. Corridor arenas (`CHMO_*`, no MTO
block) keep the previous display and mark the desync instead of
failing. Full portal/object streaming remains G3 scope.

## Coordinate conversion (`mdk_math.h`)

`v_godot = (-v_mdk.y, v_mdk.z, -v_mdk.x)` — a proper rotation
(det +1). MDK: +X forward, +Y left, +Z up, degrees. Godot: -Z
forward, +X right, +Y up, radians. The camera basis converts the
core's M2 rows (right/down/back, MDK world) into Godot columns
(X=right, Y=up=-down, Z=back). The only place axis math exists is
`mdk_math.h`/`mdk_convert.h`; GDScript sees Godot-space values (raw
MDK copies ride inside `*_mdk` diagnostic keys).

Projection: `scaleY = 1/(zoom*(H/W)*0.5)` folds the original's
pixel divisors (299.95/180.4 for the 600x360 normal viewport —
OBSERVED). The frontend sets `Camera3D.fov` from
`atan(viewHalfH/(scaleY*yDiv))` ~= 71.36 deg and drives the
transform verbatim from `PlayerCameraPose` — pullback, eye height,
pitch positioning, shake, and the obstruction seam all live in the
core.

## Arena geometry and painter order

`src/core/arena_mesh.{h,cpp}` flattens `ArenaRenderData` into a
platform-neutral bundle:

- `arenaPaletteCompose` — effective 256-entry RGB palette
  (SYS_PAL[0,64) + region-B[64,64+count) + DTI s3 tail; entry 0
  black — the OBSERVED three-source composition).
- `arenaMeshTexturesBuild` — expands each referenced material's
  indexed payload into RGBA over its *masked address space*
  (`pitch=uMask+1`, `bucketH=(vMask>>shift)+1`, both powers of two),
  then shelf-packs a 2048-wide atlas plus a one-row LUT strip.
- `arenaMeshTrisEmit` — emits one `ArenaMeshTri` per `order` entry,
  in verbatim sequence. The order comes from
  `arenaRenderOrder(arena, camPos, /*frontToBack=*/false)` — the
  original painter's back-to-front submission, render-skip bit
  already applied.

The presenter uploads the soup as a single non-indexed
`PRIMITIVE_TRIANGLES` surface whose vertex order IS the submission
order — no re-sorting, no material batching, no reliance on the
depth buffer to fix ordering mistakes (depth testing stays on as an
opaque-mode aid). The mesh is rebuilt only when
`get_arena_order_digest()` changes.

Per-vertex `ARRAY_CUSTOM0` (RGBA float) carries the material
descriptor `{atlasOriginX, atlasOriginY, pitch, bucketH}` in atlas
texels; the shader reproduces the original masked fetch with
`mod(floor(uv), pitch/bucketH)` — exact two's-complement wrap for
negative and oversized UVs, per pixel.

## Material classes

| Class | Frontend treatment |
|---|---|
| Textured | palette-expanded atlas region + masked fetch |
| Flat pen (`-1023..-1` except fx ranges) | LUT strip texel `(-m)&0xff` |
| Unresolved / NULL name slot | LUT strip texel `0xff` (original fallback) |
| fx770 (-1010..-990) | tagged placeholder color (blue) |
| fxE94 (-1028) | tagged placeholder color (amber) |
| fx12970 (other) | tagged placeholder color (green) |

The fx colors are deterministic front-end stand-ins — explicitly
tagged via `ArenaMeshTri::cls`/`flatSlot` and trivially replaceable
once the original effect drawers are reconstructed (P1 work). They
are *not* claims about original appearance. Edge-overlay bits
(`+0x22` bit7 + edge selects) are preserved on the tri but not yet
drawn.

## Deterministic smoke

`--smoke` asserts (61 checks). G1: extension load, LEVEL3 load,
arena count, HMO_1 counts (verts 234 / polys 399 / names 20 /
resolved 20 / textured 193 / pen 203 / unresolved 3 / submitted 296
/ textures 11), array alignment, atlas sizing, palette size,
coordinate goldens, camera basis/FOV, and both FNV-1a digests
matching `mdk-inspect --arena-render` verbatim:

```
geom  f1cc72cbe4056174   (camera-independent)
order 9ff16337ea1582ec   (frame-0 spawn camera)
```

G2 additions: player pos/yaw goldens, snapshot transform ==
`PlayerRoot`, basis columns == converted MDK forward/right,
standing-box extents/base, grounded state, arena display sync,
collision snapshot counts + line soup, input-config echo (axes map
"ABG", button masks 1/4/2/0), `PlayerRoot`-write isolation, W move
channel + forward-dominant displacement, Q/E strafe channels +
lateral displacement (accumulated over grounded frames — the spawn
platform's perpendicular edges are bounded by climbable steps, so
the checks turn ~90 deg first to run the corridor axis), Space jump
rise, A turn sign, mouse dx -> yaw sign, mouse dy -> moveVel sign,
and configured button masks (LMB fire echo, RMB visible jump, MMB
sniper pulse edge + hold-no-repeat, 4th button unmapped).

Jump-check ordering note: the original's jump gate requires
`vertVel == 0` exactly plus grounded, and after a SOFT landing no
event posts — `locoState` stays latched on a jump code and
`jumpActive` holds until a priority-8+ event (look/hard-land/
sniper) moves the dispatched state. The smoke re-arms with a held
look key before the second (RMB) jump check — all within proven
semantics.

`tests/test_godot_frontend.py` orchestrates the headless run and
the inspect cross-check; it skips cleanly when Godot, the dylib, or
original data are absent.

## Validation commands

```sh
# frontend native tests (pure math, no engine):
cd frontend/godot/gdextension && cmake --build build --target mdk_frontend_tests
./build/mdk_frontend_tests            # 26 checks

# headless in-engine smoke (canonical launcher path):
MDK_GODOT_BIN=/path/to/Godot python3 -m pytest tests/test_godot_frontend.py

# native regression (unchanged):
./build/mdk_tests && ctest --test-dir build
python3 -m pytest tests/
./build/mdk_inspect --data-path original/installed --arena-render \
    TRAVERSE/LEVEL3/LEVEL3.DTI   # .. LEVEL8 for the sweep
```

## Limitations / remaining fidelity items

- Static arena geometry plus a *debug-proxy* player — no real Kurt
  mesh (K-record layout UNKNOWN, see above), no models, dynamic
  objects, enemies, or animation (by design).
- The player capsule is a presentation stand-in; pitch/bank are
  snapshot-only (the proxy applies yaw — matching the traversal
  channels the frontend can show honestly).
- Arena display follows the core's current arena per frame, but
  portal/streaming presentation and objects remain G3.
- fx770/fxE94/fx12970 are tagged placeholder colors, not the
  original effect drawers (UNKNOWN semantics — P1 reverse
  engineering).
- The `+0x22` bit7 edge overlay is decoded but not drawn.
- The `+0x20` bit0 alternate span-drawer selection is preserved on
  the tri but has no frontend expression (renderer-internal).
- Atlas rebuild per `load_arena` only; painter-order rebuilds reuse
  the atlas. Draw calls: one ordered surface (correctness first —
  batching is a later optimization).
- Framebuffer capture requires a real driver run — under `--headless`
  `--screenshot` exits 2 by design (dummy renderer, no viewport
  texture). Game-mode capture works.
- Human QA items (texture orientation, palette plausibility,
  occlusion, camera feel, player proxy legibility, mouse feel vs.
  the original, RMB jump, landing/contact feedback) need eyes on a
  real run.

## Proprietary boundary

Nothing under `original/installed` is copied, transformed, or
embedded — the project reads it read-only at runtime via
`--data-path`/`MDK_DATA_ROOT`. No game data ships in the project or
extension binary.
