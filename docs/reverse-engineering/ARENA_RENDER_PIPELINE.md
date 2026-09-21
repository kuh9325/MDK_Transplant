# Arena Render Pipeline (G1-RE)

Phase 6A — instruction-level reconstruction of the BUILD_A
(`MDK95.EXE`) arena visual-geometry/material path, and the
platform-neutral boundary it produced in `mdk_core`. Evidence levels
follow `EVIDENCE_POLICY.md`. `FUN_*` names are Ghidra addresses, not
original symbols.

Private artifacts (decompiles, disasms, scripts) live in
`analysis-private/` and are not committed.

## Call graph (OBSERVED, static)

```
FUN_00436d60  frame driver (sole caller of the arena pass)
└─ FUN_00436ea8  per-arena wrapper — called for the CURRENT arena
   (c48) and again for the partner arena (ca4) when attached
   └─ FUN_00431300  draw_arena("Overflowed MaxObjects in draw_arena")
      ├─ vertex transform: arena+0x24 f32 triples -> scratch
      │   {x',y',z', sx,sy, clipFlags} records via FUN_0046b4f8,
      │   using camera matrix M1 (0x540b80) — the projection-folded
      │   world->camera matrix proven in Phase 5K
      ├─ draw-entry list build: 48-byte {fn, depth, flags, aabb[6],
      │   param, mask} records for the player/object/shot handlers
      │   inserted into per-BSP-node lists (node +0x24/+0x28)
      └─ FUN_0040a688  binder: stores the render globals
          0x4a2458 = xform verts, 0x4a2460 = nodes,
          0x4a2464 = polys, 0x4a2468 = camPos
          ├─ FUN_0040e838  per-frame prep (transformed-vertex
          │   visibility preprocess; maintains DAT_00499f78 for a
          │   special clip path)
          ├─ FUN_00409e4c  (same family)
          └─ FUN_00409a6c  recursive BSP submission walk
              ├─ FUN_00409860  per-node poly span submitter
              │   └─ FUN_0040ca00  Sutherland-Hodgman clipper —
              │       clips the projected triangle, interpolating
              │       the embedded UV triples
              │       └─ FUN_0040c860  material dispatch +
              │           scanline rasterizer
              └─ FUN_00409a00  dynamic-entry list flush (draws the
                  inserted 48-byte records through their fn ptrs)
```

`FUN_0045ee08` also calls `draw_arena` twice (a second scene context
— most likely the sniper/overlay view; PARTIAL).

## BSP / visibility (OBSERVED, disasm + real-data verified)

Render BSP == collision BSP. The renderer consumes the identical
region-C tables `FUN_00419ee0` produces for the sweep: 0x2c nodes,
0x24 polys, f32 verts. Child polarity was verified against real
LEVEL3 HMO_1 geometry: `+0x10` (childFar) holds the negative-halfspace
subtree, `+0x12` (childNear) the positive-halfspace one.

`FUN_00409a6c` per node (`dist = n . camPos + d`, normal mode
`0x499f8c == 0`):

- `dist > 0`  : recurse `+0x12` (camera side) → flush `+0x24` entry
  list → submit `+0x14` span → flush `+0x28` list → tail-descend
  `+0x10`
- `dist <= 0` : the symmetric order — `+0x10` first → flush `+0x28`
  → submit `+0x18` span → flush `+0x24` → `+0x12` last

So the camera-side subtree is always submitted first, the pre-span
flush is the camera-side entry list (`+0x24` when `dist>0`, `+0x28`
when `dist<=0`), the node polys next, the far-side list after them —
front-to-back in BSP terms, dynamic entries interleaved by depth.

`DAT_00499f8c` selects a child-swapped variant (recurse the far
subtree first, span and list order mirrored) — i.e. the classic
back-to-front painter's order — but has zero writers in BUILD_A:
one read, always 0, dead code (VERIFIED via xref). The dead path
being the painter-correct one sharpens the occlusion question below.

Spans are `{lo16 count | hi16 firstIdx}` into the poly table — the
same encoding the collision walker documents. `FUN_00409860` skips
polys with `+0x20 bit4 (0x10)` (the render-skip bit; the collision
skip is `0x20` — different bit, same record) and gates a two-sided
submission on `+0x20 bit0 && DAT_005414b4`.

Painter order caveat (UNKNOWN, documented for oracle work): the
camera-side-first order is instruction-proven, yet every inspected
span drawer (all 12 variants in both dispatch tables) performs
unconditional indexed-byte stores and no depth test or coverage
buffer was found anywhere. Overlap analysis on real HMO_1 data shows
later-submitted far polys do cover earlier near ones in some views —
so either an unfound resolve mechanism exists or the artifact is real
and masked by content. Do NOT "fix" the order — reproduce it and
flag for a runtime oracle comparison.

## Visual geometry (OBSERVED)

The renderable polygon is the shared 0x24-byte region-C record —
the collision `pad[26]` region IS the render data:

| off | field | evidence |
|---|---|---|
| +0x00 | u16 v[3] — indices into the f32 vert triples | OBSERVED (submitter + collision) |
| +0x06 | s16 material — >=0: region-C name-table index; <0: pen/effect | OBSERVED |
| +0x08 | f32 uv0 {u,v} | OBSERVED (clipper interpolates) |
| +0x10 | f32 uv1 | OBSERVED |
| +0x18 | f32 uv2 | OBSERVED |
| +0x20 | u8 flags — bit4 render-skip, bit0 two-sided gate | OBSERVED |
| +0x21 | u8 aux — UNKNOWN, preserved raw | UNKNOWN |
| +0x22 | u8 aux — bits 0x10/0x20/0x40 per-vertex debug-normal mask; bit7 gates an extra path under a global mode | OBSERVED bits / UNKNOWN names |
| +0x23 | u8 surface index+1 (collision) | OBSERVED |

Primitives are triangles only. There are no stored normals, no
per-vertex colors and no light values in the arena poly record —
shading is flat-per-triangle through the material/pen system.

## Material / texture pipeline (OBSERVED)

- Region-C array-1 = `char[10]` material names; the poly's
  nonnegative material index selects a NAME SLOT, not an MTI record.
- `FUN_0041a694` ("matlkup") builds the arena's pointer table
  (arena+0x20): for each name, search bank A (`0x54b728/2c`, the
  shared `LEVELnS.MTI`) FIRST, then bank B (`0x54b730/34`, the
  arena's embedded `.MAT`). Name compare `FUN_0042fa50` = exact
  case-sensitive byte compare to NUL.
- Miss → `FUN_0047d2e9` logs `"Texture %s not in material list"` once
  at lookup time and stores `DAT_0054b73c` — which the bank loaders
  (`FUN_0041a480`/`FUN_0041a4d0`) set to NULL. At draw time a NULL
  table entry (or a record whose `+0x24` pixel pointer is 0) takes
  the flat-fill path with pen `0xff` (OBSERVED `0x40c9da`). Real-data
  note: OLYM_9 (LEVEL6) genuinely references `O3_FLR1/O3_CORR/
  O3_BLAST` — names that exist only in OLYM_3's `.MAT` — so the
  fallback path is exercised on real content. The unused XF1_* names
  in DANT_1 (LEVEL7) and `A` in GUNT_4 (LEVEL8) are declared-but-
  unreferenced table entries.
- MTI -> runtime record (`FUN_0041a1e0`, 0x34-stride, table buffer
  memset(0) by `FUN_0047d20a` before the record loop):
  `+0x00 shift = smallest k<12 with (1<<k) >= width`;
  `+0x04 width`, `+0x08 height`, `+0x0c class/flags` (extended records
  merge `frameCount<<16`);
  `+0x10 uMask = (1<<shift)-1`; `+0x14 vMask = (w==h ? uMask :
  bucketMask(h)) << shift` with buckets {0x0f,0x1f,0x3f,0x7f,0xff,
  0x1ff,0x3ff} at heights {<0x11,<0x21,<0x41,<0x81,<0x101,<0x201,
  else}; `+0x18 ~uMask`; `+0x1c/+0x20` raw record params;
  `+0x24` pixel pointer; `+0x28` name[8].
- Index records (`classWord == 0xffffffff`) write ONLY `+0x08` =
  the index value (record `+0x0c`) and `+0x0c` = -1; the payload
  offset is never dereferenced and `+0x24` stays zero -> any poly
  resolving to one draws flat 0xff, same as a miss.
- Pixels are single-byte palette indices tiling
  `frameCount*width*height` after the 4- or 8-byte header (extended
  header = `flags & 0x30000`; OBSERVED animated payload EXPLODE =
  26 frames x 128x128).
- Palette: region B = 0x150 bytes = 112 RGB triplets (OBSERVED all
  60 blocks), uploaded by `FUN_0046d490` into 4-byte entries at
  `0x54d7b8` with dirty-range tracking.
- `FUN_0040b4dc` is a texture-animation toggle: polys with flags bit1
  get bits 0x30 set/cleared per anim frame (the render-skip bit
  doubles as the animation frame gate).

## Rasterizer dispatch (FUN_0040c860 — OBSERVED)

After Y-sorting the (clipped) triangle verts, the s16 material index
selects:

| index | action |
|---|---|
| >= 0, `table[idx] != NULL`, `rec+0x24 != 0` | `FUN_0046daac` — perspective texture mapper (per-vertex `1/z, u/z, v/z` gradients; `rec+0x0c & 3` picks one of 12 span-drawer variants) |
| >= 0, `table[idx] == NULL` | flat fill `FUN_00415260`, pen `0xff` (`0x40c9da`) |
| >= 0, `rec+0x24 == 0` | error lookup + flat fill `0xff` |
| -1023..-1 except -1010..-990 | flat fill, pen = `(-idx) & 0xff` (the `PEN_n` names) |
| -1010..-990 | `FUN_0047a770(idx+1000)` — s4-grid family effect (UNKNOWN semantics) |
| -1027..-1024 | `FUN_00412970(table[0x540b20] + ((-1024-idx)<<8))` — 4 variants (UNKNOWN semantics) |
| -1028 | `FUN_0046e940` (UNKNOWN semantics) |
| <= -1029 | `FUN_00412970(table + ((-1029-idx)<<8))` (UNKNOWN semantics) |

All span stores are unconditional (no z-buffer, no coverage mask —
searched both span-drawer tables and the flat fill: 600-byte
framebuffer stride, `MOV [EDI],AL`-style stores only).

## Rasterizer replacement boundary (the G1 contract)

MUST reproduce (semantics): BSP submission order, per-poly skip
(+0x20 bit4), the material dispatch above (including NULL->0xff and
the pen mapping), the UV/material association, palette-index pixels.

MAY replace (implementation): the scanline/DDA span drawers, the
DirectDraw plumbing, the 600x360 back buffer itself. A modern
frontend can draw the submitted triangles with hardware z-ordering or
a faithful painter — the original's submission order is the
visibility contract, so preserve `arenaRenderOrder`'s output even if
a depth buffer makes it redundant.

## Camera consumption (cross-check, CONSISTENT)

`draw_arena` consumes camPos (0x540b34-adjacent globals through the
binder `0x4a2468`) and the M1 matrix `0x540b80` — the Phase 5K
projection-folded world->camera matrix (`scaleX*right |
scaleY*down | scaleZ*back`, `t = scale*-(row.cam)`). Projector
variants: five precision modes; variant 0 = the 600x360 viewport
(299.95/180.4 divisors + 0.05 bias). The existing Godot conversion
`(-MDK.y, MDK.z, -MDK.x)` is unaffected — nothing in the render path
contradicts the camera contract.

## Platform-neutral boundary (implemented)

`src/core/arena_render.{h,cpp}`:

- `ArenaRenderPoly` — the decoded render view of the shared poly
  record (`arenaRenderPolyDecode`).
- `ArenaRenderMaterial` — the decoded 0x34-record view
  (`arenaRenderMaterialDecode`, including the extended/animated
  header and index records).
- `ArenaMatClass` + `arenaMatClassFor`/`arenaPenIndex` — the
  `FUN_0040c860` dispatch classification.
- `ArenaRenderData` + `arenaRenderDataBuild` — the per-arena bundle:
  aliased collision tables (verts/nodes), decoded polys, the
  material-name table, both banks decoded, `materialOfName` (the
  matlkup result: bank A index, `bankA.size()+` bank B index, or -1
  for the NULL fallback), the palette triplet span, and
  `materialFor`/`polyMaterialClass`.
- `arenaRenderOrder` — the `FUN_00409a6c` submission order
  (iterative equivalent of the recursion + tail-descend; `mirror`
  exposes the dead 0x499f8c variant).

Ownership: `ArenaRenderData` aliases the source file bytes for
verts/nodes/pixels/palette (caller keeps the buffers alive); decoded
records are by-value.

## Diagnostic

`mdk-inspect --data-path DIR --arena-render <LEVELn.DTI>` decodes the
sibling `<stem>O.MTO` + `<stem>S.MTI` for every block: counts,
vertex AABB, bank sizes, resolved/missing material names, dispatch
class histogram, palette size, FNV-1a digests of the decoded
geometry and of the BSP submission order at a probe camera
(`--arena NAME`, `--start X Y Z`).

## Cross-level results (BUILD_A, all six levels)

60/60 MTO blocks decode cleanly; zero crashes/asserts. Material-name
resolution is complete except where the original itself falls back
(see above). Dispatch classes observed in real data: textured,
unresolved(->0xff), pen, fx770 (HMO_9 LEVEL3), fx12970 — no fxe94
instances.

## Remaining render RE (census)

- P0 (before 2026-10-10): the painter-order/occlusion question above
  — needs a runtime oracle frame-diff or a missed-mechanism search
  (span-emitter coverage, write-masking). Also P0: the two-sided
  bit0 gate `DAT_005414b4`'s live value, and which real paths use
  `DAT_005414d4 != 0` in the clipper (deferred-sort suspicion).
- P1 (useful, can wait): semantics of `FUN_0047a770`, `FUN_0046e940`,
  `FUN_00412970` (the negative-index effect drawers — all write the
  same framebuffer; classes exist in real data: fx770 40 polys in
  LEVEL3 HMO_9, fx12970 in several arenas); the `+0x21` byte;
  `+0x22` bit7's gated path; the `FUN_0040e838`/`FUN_00409e4c`
  preprocessing details; per-frame `FUN_0040b4dc` texture-animation
  frame selection (which frame index when animating).
- P2 (ordinary implementation, no executable archaeology): wiring
  `ArenaRenderData` into a consumer, palette->RGBA expansion for a
  frontend, the camera-pose binding.

## Open unknowns carried forward

- The occlusion/resolve mechanism (or proof none exists) — P0.
- Effect-drawer semantics (fx770/fx12970/fxe94) — P1.
- `DAT_005414d4` clipped-path behavior — P1.
- Whether bank A is exclusively `LEVELnS.MTI` in all streaming
  states (a per-arena reload could change the shared bank between
  matlkup calls) — P1, matters only for cross-arena name shadows.
