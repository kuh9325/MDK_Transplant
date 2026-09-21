# GODOT_FRONTEND.md — Phase 7 (G1) Godot 4 frontend

Status: **implemented and validated** — static LEVEL3 arena rendering
through a retained GDExtension bridge. Dynamic objects, animation,
HUD, audio, and the fx drawers remain out of scope (see
"Limitations").

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
  main.tscn                         Node3D + MeshInstance3D + Camera3D
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
  build.sh                          configure+build wrapper
  bin/<platform>/libmdkbridge.*     generated output (ignored)
  .godot/                           editor cache (ignored)
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

## Build

```sh
# one-shot (downloads the pinned godot-cpp tarball):
frontend/godot/build.sh

# reuse an existing godot-cpp checkout (much faster):
GODOT_CPP_DIR=/path/to/godot-cpp frontend/godot/build.sh
```

Output: `frontend/godot/bin/Darwin-arm64/libmdkbridge.dylib`.
`mdk_core` is recompiled from `src/core/*.cpp` as PIC and linked
statically — the extension does not depend on the SDL root build.

## Run

```sh
# interactive acceptance scene (WASD move, Q/E strafe, R/F look,
# Space jump, Shift turbo, Esc quit):
Godot --path frontend/godot -- --data-path /path/to/installed

# deterministic headless smoke (dummy renderer):
Godot --headless --rendering-driver dummy --audio-driver Dummy \
    --path frontend/godot -- --smoke --data-path /path/to/installed

# framebuffer capture (real renderer, brief window):
Godot --rendering-driver metal --audio-driver Dummy \
    --path frontend/godot -- --screenshot /tmp/shot.png \
    --data-path /path/to/installed
```

User args after `--`: `--data-path DIR` (default `$MDK_DATA_ROOT`,
else `<repo>/original/installed` relative to the project),
`--level RELDTI`, `--arena NAME` (default `HMO_1`; `""` = spawn
arena), `--smoke`, `--screenshot PATH`. Relative paths resolve
against the launch directory (`$PWD`) because Godot chdirs into the
project directory.

Headless editor caveat (known upstream): `--headless --editor`
crashes inside MoltenVK shader conversion on Apple Silicon. Use
`--rendering-driver dummy` for editor-side scans; game mode is
unaffected.

## Bridge API (`MdkBridge`, RefCounted)

```gdscript
var b := MdkBridge.new()
b.initialize(data_root)        # bool — DataRoot open (no writes)
b.load_level(dti_rel_path)     # bool — DTI+CMI+MTO via the runtime
b.load_arena(arena_name)       # bool — "" selects the spawn arena
b.step_frame(dt_ms, mask)      # one traversal frame; QA input bits
b.get_player_snapshot()        # pos (Godot), yaw/pitch, grounded...
b.get_camera_snapshot()        # Transform3D, fov_deg, aspect, rect
b.get_arena_render_snapshot()  # mesh, material, atlas, arrays, stats
b.get_arena_order_digest()     # cheap painter-order digest (poll)
b.get_arena_names()            # level arena list
b.is_level_loaded() / b.is_arena_loaded() / b.get_last_error()
b.shutdown()
```

Only copy-safe values cross the boundary — packed arrays,
dictionaries, Godot resources. No core pointers, spans, or mutable
ownership escape into GDScript.

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

`--smoke` asserts (26 checks): extension load, LEVEL3 load, arena
count, HMO_1 counts (verts 234 / polys 399 / names 20 / resolved
20 / textured 193 / pen 203 / unresolved 3 / submitted 296 /
textures 11), array alignment, atlas sizing, palette size,
coordinate goldens, camera basis/FOV, and both FNV-1a digests
matching `mdk-inspect --arena-render` verbatim:

```
geom  f1cc72cbe4056174   (camera-independent)
order 9ff16337ea1582ec   (frame-0 spawn camera)
```

`tests/test_godot_frontend.py` orchestrates the headless run and
the inspect cross-check; it skips cleanly when Godot, the dylib, or
original data are absent.

## Validation commands

```sh
# frontend native tests (pure math, no engine):
cd frontend/godot/gdextension && cmake --build build --target mdk_frontend_tests
./build/mdk_frontend_tests            # 16 checks

# headless in-engine smoke:
MDK_GODOT_BIN=/path/to/Godot python3 -m pytest tests/test_godot_frontend.py

# native regression (unchanged):
./build/mdk_tests && ctest --test-dir build
python3 -m pytest tests/
./build/mdk_inspect --data-path original/installed --arena-render \
    TRAVERSE/LEVEL3/LEVEL3.DTI   # .. LEVEL8 for the sweep
```

## Limitations / remaining G1 fidelity items

- Static arena geometry only — no models, dynamic objects, or
  animation (by design).
- fx770/fxE94/fx12970 are tagged placeholder colors, not the
  original effect drawers (UNKNOWN semantics — P1 reverse
  engineering).
- The `+0x22` bit7 edge overlay is decoded but not drawn.
- The `+0x20` bit0 alternate span-drawer selection is preserved on
  the tri but has no frontend expression (renderer-internal).
- Atlas rebuild per `load_arena` only; painter-order rebuilds reuse
  the atlas. Draw calls: one ordered surface (correctness first —
  batching is a later optimization).
- Framebuffer capture requires a real driver run (headless = dummy
  renderer). `--screenshot` works in game mode.
- Human QA items (texture orientation, palette plausibility,
  occlusion, camera feel) need eyes on a real run.

## Proprietary boundary

Nothing under `original/installed` is copied, transformed, or
embedded — the project reads it read-only at runtime via
`--data-path`/`MDK_DATA_ROOT`. No game data ships in the project or
extension binary.
