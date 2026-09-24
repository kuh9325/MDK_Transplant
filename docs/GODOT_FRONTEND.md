# GODOT_FRONTEND.md — Phase 7-9 (G1-G3) Godot 4 frontend

Status: **implemented and validated** — static arena rendering,
a snapshot-driven player presentation, and dynamic-object
presentation (real RuntimeModel geometry, movers/doors, portal
transitions) through a retained GDExtension bridge. Animation,
combat, enemies/AI, HUD, audio, and the fx drawers remain out of
scope (see "Limitations").

This document covers the in-repo frontend. The architecture
rationale and the disposable-spike evidence live in
`docs/GODOT_INTEGRATION_AUDIT.md`; this file is the retained,
authoritative description of what is actually checked in.

## Ownership boundary

`mdk_core` is authoritative for all game semantics: file parsing
(DTI/CMI/MTO/MTI/FTI), the traversal runtime, player/camera state,
`ArenaRenderData` decoding, material classification, BSP painter
ordering (`arenaRenderOrder`), palette semantics, dynamic-object
spawn/update state (transforms, AABBs, mover/connector state, arena
transfers), and RuntimeModel geometry parsing.

Godot (scene + GDScript + GDExtension glue) is presentation only:
it converts core-provided snapshots into `ArrayMesh`/`Image`/
`Camera3D` objects and forwards input bits back into the core. No
MDK file is parsed and no game rule is evaluated outside the core.

## Layout

```
frontend/godot/
  project.godot                     engine config only (no data)
  main.tscn                         Node3D + ArenaRoot +
                                    DynamicObjectRoot +
                                    ObjectDebugRoot + PlayerRoot +
                                    PlayerBoxWire + CollisionDebug +
                                    Camera3D + DebugUI
  src/main.gd                       driver + --smoke / --screenshot
  shaders/arena_unshaded.gdshader   unshaded masked-fetch shader
  gdextension/
    mdk_bridge.gdextension          descriptor (entry: mdk_godot_library_init)
    CMakeLists.txt                  standalone build; pins godot-cpp
    src/mdk_math.h                  pure-scalar MDK->Godot math
    src/mdk_convert.h               godot-cpp adapters over mdk_math
    src/mdk_objid.h                 opaque stable object IDs
    src/arena_presenter.{h,cpp}     bundle -> Image/ArrayMesh
    src/object_presenter.{h,cpp}    RuntimeModel -> ArrayMesh
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
# wire, F2 object AABB/name debug, F3 debug text,
# Esc release-then-quit):
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
                               # for the PRIMARY displayed arena
b.get_arena_order_digest()     # cheap painter-order digest (poll)
b.get_arena_render_snapshots() # one snapshot per displayed arena
b.get_display_snapshot()       # cur/partner/portals/object counts
b.get_display_digest()         # display-set + order digest (poll)
b.get_object_snapshots()       # DynamicObjects in the view set
b.get_object_geometry(id)      # per-object local model mesh
b.diagnostic_start(idx,pos,yaw)# test-only player re-anchor
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
  ArenaRoot          — one MeshInstance3D per displayed arena (G1/G3)
  DynamicObjectRoot  — one Node3D per live object id (G3)
    Object_<id>      — snapshot transform, per-element meshes
  ObjectDebugRoot    — F2 object AABB wires + Label3D tags (G3)
  PlayerRoot         — snapshot transform, nothing writes back
    DebugBody        — capsule, green grounded / amber airborne
    ForwardMarker    — blue nose box on local -Z (facing)
  PlayerBoxWire      — 12-edge AABB of the standing collision box
  CollisionDebug     — display-set collision-poly line soup (F1)
  Camera3D           — core PlayerCameraPose, sibling not child
  DebugUI            — tiny QA label (F3): pos, yaw, channels,
                       cur/partner/portal/migration counters
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

## Dynamic objects (G3)

`DynamicObject`s spawn in core from DTI arena sub-records (type-2
HotGen, type-4 HotPick/mover — see `docs/GAMEPLAY_RECONSTRUCTION.md`
§42-49). The frontend presents the objects living in the **object
view set** — the same `{cur} + active partner` arenas that drive the
display set, so a geometry-less corridor's objects (e.g. the
CHMO_2 connector door) still enumerate while its partner's geometry
shows. Objects are NOT enumerated level-wide; resident but
out-of-view arenas contribute nothing. Only named (`+0x06 != 0`)
records enumerate: a teardown corpse pending the arena's post-pass
`FUN_0045cf18` sweep (`DynamicArena::reapUnnamed`) is already inert
and never reaches the snapshot, matching the original where the
sweep unlinks+frees it to the `0x540ed0` freelist the same frame.

### Opaque object IDs (`mdk_objid.h`)

GDScript never sees a `DynamicObject*`. `MdkObjectIds` mints dense
`uint64` ids keyed on the object's (stable, list-owned) address,
gated by an FNV fingerprint over spawn-stable fields only —
`enemyIndex`, `spawnId`, `scriptVariant`, `scriptOff`, model and
script names. Mutable state (position, flags, AABB) is excluded,
so mover updates and arena transfers keep the same id. A live-set
pass over every arena's storage precedes each snapshot call:
despawned objects are pruned, a stale id resolves to an empty
`get_object_geometry` result forever after, and an address reused
by a different object re-mints rather than aliasing. Same-address
same-fingerprint reuse is indistinguishable from a re-spawn of the
same record — documented edge.

### Object snapshot fields

`get_object_snapshots()` returns one Dictionary per object in the
view set — all copy-out values:

| Key | Source |
|---|---|
| `id` | opaque `MdkObjectIds` id (NOT an address) |
| `arena`, `arena_name` | owning arena list (index + name) |
| `model` | `RuntimeModel::modelName()` — internal geometry name (e.g. `XG_BOD`) |
| `enemy_name` | DTI enemy-table record name (e.g. `XGS`) — a different field from `model` |
| `enemy_index`, `spawn_id` | spawn record coordinates |
| `pos`, `pos_mdk` | object position |
| `transform` | full core transform — see below |
| `aabb`, `aabb_mdk` | `CollisionObject::aabb` (world) |
| `yaw_deg`, `pitch_deg`, `bank_deg`, `scale` | diagnostic Euler/scale fields |
| `health` | core health field |
| `flags148/149/14a` | raw collision flags |
| `mover`, `connector`, `mountable`, `ride_capable` | decoded flag bits (14a&0x20/0x10/0x80, 149&0x01) |
| `conn_state`, `conn_state_hi`, `conn_anim_active` | connector/door state |
| `pending_arena` | pending transfer target index (-1 none) |
| `elem_count`, `elem_mask` | element count + disable mask (`elemMaskB`) |
| `vert_count`, `tri_count`, `geom_key` | geometry census + content digest |
| `script_class`, `script_name` | script metadata |

### RuntimeModel geometry (`object_presenter.cpp`)

Dynamic-object models parse through the proven `FUN_00428400`
geometry record: per-element local f32 vertex triples plus
0x24-byte triangle records whose only established field is the
`u16 v[3]` index triple at +0. `get_object_geometry(id)` builds
one indexed `PRIMITIVE_TRIANGLES` surface per element carrying the
verbatim local verts (P-converted); out-of-range indices are
clamped and reported, never fatal. `surface_elems` maps mesh
surface → element index, `elem_names` carries every element name
(including zero-geometry elements), and `geom_key` is an FNV-1a
digest over the immutable geometry content — the presentation
cache key. Two objects whose deep-copied models hold byte-identical
geometry share cached meshes; any future deformation that mutates
verts changes the key and forces a rebuild.

The element-disable mask applies per element each frame:
`elem_mask` bit *set* = element hidden (connector/HC door masks,
`SW_DUMMY` bits — same semantics as the core's mask consumers).
`Object_<id>` nodes keep one `MeshInstance3D` child (`E<elem>`)
per element so visibility toggles without mesh rebuilds.

### Object transform — core-authoritative

`CollisionObject::xform` (3x3, Euler or raw-matrix path, scale
baked) + `origin` (+0x78) is the COMPLETE authoritative transform:
`world_mdk = M * local + origin`. The bridge converts it by change
of basis, `M_godot = P * M_mdk * P^T`, `origin_godot = P *
origin_mdk`, inside `mdk_math.h`/`mdk_convert.h`; local verts are
P-converted at mesh build, so the composite reproduces
`P * world_mdk` exactly. GDScript applies
`snapshot["transform"]` verbatim and never recomputes
yaw/pitch/bank — this is what makes raw-matrix movers correct.
Native frontend tests prove identity, translation, yaw/pitch/bank,
non-unit scale, arbitrary raw matrices, and the point-equivalence
`P(M*v + o) == M_g*P(v) + P(o)`.

### Material / UV verdict — debug materials (documented seam)

Audit verdict: the model triangle record's bytes past `v[3]` are
NOT evidenced for RuntimeModel records — the proven interior layout
(material index, UV semantics) belongs to the arena region-C poly
format, a different consumer. Per the evidence gate, objects render
the real geometry under deterministic unshaded per-element debug
materials (element-name-hashed hue, cull disabled). This is a
named fidelity seam, not a claim about original appearance; no
material semantics were guessed.

### Movers, doors, platforms

Type-4 HotPick movers (flag14a&0x20) and connector doors
(flag14a&0x10) present through the same snapshot path: core updates
the object transform/collision per frame, the snapshot carries it,
`Object_<id>.transform` follows. There is no second integration in
GDScript — if a mover does not animate it is because its driving
script opcode is unimplemented in the core script VM (a known seam;
the LEVEL8 GUNT_9 movers currently sit static in the tested view),
and the frontend reports rather than fakes motion.

### F2 object debug

`ObjectDebugRoot` (F2) draws per-object world-space AABB wires
(`ImmediateMesh`, 12 edges from the snapshot `aabb`) plus a
billboard `Label3D` tag with model name, opaque id, arena index,
enemy index and spawn id. Wires/tags are keyed by the same opaque
id and freed with the object node. F1 keeps the arena collision
soup — now the union of every displayed arena's `collision_lines`.

## Arena transitions + display set (G3)

The old single-arena path (switch `ArenaMesh` to `curArenaIndex`
when an MTO block exists, else keep the stale display) is replaced
by a **display set** driven by the traversal view state. After
every stepped frame (and after `diagnostic_start`) the bridge
recomputes the view set — `{cur}` plus `{partner}` when
`rt.partnerActive` — builds an `ArenaSet` per member that has an
MTO render block, and re-evaluates BSP painter order for each set
at the current core camera. `get_arena_render_snapshots()` returns
one snapshot per presented arena, current-first; `ArenaRoot` keeps
one `MeshInstance3D` (`Arena_<index>`) per displayed arena and
drops nodes for arenas that leave the set.

`get_display_snapshot()` surfaces the core state that drives the
set: `cur_arena`/`cur_name`, `partner_arena`/`partner_name`,
`partner_active`, `view_on_partner`, `swapped`
(`currentArenaSwapped`), `portal_candidate`, the cumulative
`portals_crossed`/`object_migrations` counters, the `primary`
presented index, and two arena lists — `arenas` (rendered set)
and `object_arenas` (object enumeration set, which can include
geometry-less corridors). `get_display_digest()` folds the set
membership plus every member's order digest into one value; the
frame loop only rebuilds `ArenaRoot` when it changes.

Corridor arenas (`CHMO_*`, no MTO block) now behave correctly
instead of leaving stale geometry: a corridor current contributes
no render block, but its active partner stays in the display set,
so the adjacent arena's geometry remains up while the corridor's
own objects still enumerate and present. No corridor geometry is
fabricated — the visible set is exactly what the proven
current/partner relationship yields.

## Coordinate conversion (`mdk_math.h`)

`v_godot = (-v_mdk.y, v_mdk.z, -v_mdk.x)` — a proper rotation
(det +1). MDK: +X forward, +Y left, +Z up, degrees. Godot: -Z
forward, +X right, +Y up, radians. The camera basis converts the
core's M2 rows (right/down/back, MDK world) into Godot columns
(X=right, Y=up=-down, Z=back). Object transforms convert by change
of basis: `M_godot = P * M_mdk * P^T`, `origin_godot = P *
origin_mdk` (`mdkTransformToGodot`), so a complete core 3x3 —
including the raw-matrix mover path — carries over verbatim. The
only place axis math exists is `mdk_math.h`/`mdk_convert.h`;
GDScript sees Godot-space values (raw MDK copies ride inside
`*_mdk` diagnostic keys).

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

`--smoke` asserts (119 checks as of the lifecycle fix; the count
grows with per-element visibility checks). G1: extension load, LEVEL3 load,
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
semantics. The MMB sniper check is followed by an explicit unscope
edge: sniper state (loco 0x323) replaces locomotion with aim
channels, and a second MMB while `transitionPhase==0` is consumed
without toggling — the unscope needs its own press after the phase
settles, matching the original's edge semantics.

G3 additions: display snapshot (cur/partner names + indices,
partner active, portal fields), multi-arena `ArenaRoot` nodes and
digest-driven rebuild, the real HMO_9 `XGS` object (enemy index 30,
spawn id 9, `model`=`XG_BOD` vs `enemy_name`=`XGS`), object
transform/AABB conversion goldens, real geometry resolution
(`elem_count` 25, per-element meshes), `geom_key` consistency,
`Object_<id>` node creation and per-element child count,
stale/unknown id → empty geometry, F2 debug wire/tag creation and
cleanup, element-mask visibility, the below-plane reap (the XGS
sits under the -200 kill plane and drops out of the enumeration on
the first stepped frame — OBSERVED `FUN_0045bac0`/`FUN_0045cf18`),
the CHMO_2→HMO_3 corridor run (connector door enumerated on the
geometry-less corridor, door `conn_state` transition, portal
crossing counted, at most one live connector per snapshot, a dead
connector's id never re-enumerates, no duplicate ids), and
`object_migrations` reported as a counter. OBSERVED on that route:
each connector is a ~2-frame kill-plane transient — the CHMO_2
door attaches HMO_3 and dies, HMO_3's script respawns a second
door that self-migrates into CHMO_2 and dies too — so the id set
is sequential (`door_ids=2`), not a single stable id.

Cross-level: `--smoke` on LEVEL6/LEVEL8 runs the generic path —
object enumeration census, per-object geometry resolution, node
creation, mover transform mirroring against live snapshots
(reports how many watched movers actually animate; 0 on the
tested GUNT_9 view — a script-VM seam, wired but undriven), masked
element visibility, and display-set coherence.

`tests/test_godot_frontend.py` orchestrates the headless run and
the inspect cross-check; it skips cleanly when Godot, the dylib, or
original data are absent.

## Validation commands

```sh
# frontend native tests (pure math, no engine):
cd frontend/godot/gdextension && cmake --build build --target mdk_frontend_tests
./build/mdk_frontend_tests            # 60 checks

# headless in-engine smoke (canonical launcher path):
MDK_GODOT_BIN=/path/to/Godot python3 -m pytest tests/test_godot_frontend.py

# native regression (unchanged):
./build/mdk_tests && ctest --test-dir build
python3 -m pytest tests/
./build/mdk_inspect --data-path original/installed --arena-render \
    TRAVERSE/LEVEL3/LEVEL3.DTI   # .. LEVEL8 for the sweep
```

## Limitations / remaining fidelity items

- A *debug-proxy* player — no real Kurt mesh (K-record layout
  UNKNOWN, see above). The capsule is a presentation stand-in;
  pitch/bank are snapshot-only (the proxy applies yaw — matching
  the traversal channels the frontend can show honestly).
- Dynamic objects present real RuntimeModel geometry under
  per-element debug materials — the model triangle record's
  material/UV fields are NOT evidenced (named fidelity seam, see
  "Material / UV verdict").
- Movers/doors mirror core transforms exactly; whether a mover
  animates depends on the core script VM — undriven objects sit
  still (reported, never faked).
- No enemy limb animation, Kurt animation, or bone semantics —
  whole-object transforms only (animation boundary).
- fx770/fxE94/fx12970 are tagged placeholder colors, not the
  original effect drawers (UNKNOWN semantics — P1 reverse
  engineering).
- The `+0x22` bit7 edge overlay is decoded but not drawn.
- The `+0x20` bit0 alternate span-drawer selection is preserved on
  the tri but has no frontend expression (renderer-internal).
- Per-arena `ArenaSet`s build lazily on first display and cache —
  painter-order rebuilds reuse the atlas. Draw calls: one ordered
  surface per displayed arena plus one per object element
  (correctness first — batching is a later optimization).
- Framebuffer capture requires a real driver run — under `--headless`
  `--screenshot` exits 2 by design (dummy renderer, no viewport
  texture). Game-mode capture works.
- Human QA items (texture orientation, palette plausibility,
  occlusion, camera feel, player proxy legibility, mouse feel vs.
  the original, RMB jump, landing/contact feedback, object/door
  placement legibility, corridor transitions, F2 tag alignment)
  need eyes on a real run.

## Proprietary boundary

Nothing under `original/installed` is copied, transformed, or
embedded — the project reads it read-only at runtime via
`--data-path`/`MDK_DATA_ROOT`. No game data ships in the project or
extension binary.
