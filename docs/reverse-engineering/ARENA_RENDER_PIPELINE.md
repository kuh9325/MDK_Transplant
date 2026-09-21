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

## BSP / visibility (OBSERVED, disasm + real-data + image verified)

Render BSP == collision BSP. The renderer consumes the identical
region-C tables `FUN_00419ee0` produces for the sweep: 0x2c nodes,
0x24 polys, f32 verts. Child polarity was verified against real
LEVEL3–8 geometry: `+0x10` (childFar) holds the negative-halfspace
subtree (99.5–99.8% of its verts on the negative side), `+0x12`
(childNear) the positive-halfspace one (99.8–99.9%).

`FUN_00409a6c` per node (`dist = n . camPos + d`, live mode
`DAT_00499f8c != 0`):

- `dist > 0`  : recurse `+0x10` (far side) → flush `+0x28` entry
  list → submit `+0x14` span → flush `+0x24` list → tail-descend
  `+0x12`
- `dist <= 0` : the symmetric order — `+0x12` first → flush `+0x24`
  → submit `+0x18` span → flush `+0x28` → `+0x10` last

So the far-side subtree is always submitted first, the far-side
entry list flushes before the node's polys, the node's camera-facing
span next (`+0x14` when the camera is on the positive side, `+0x18`
otherwise), then the near-side list, then the near subtree —
**back-to-front painter's order**, dynamic entries interleaved in
the same far→near sequence.

Phase 6B correction: `DAT_00499f8c` was previously assumed zero
(BSS). It is in fact **initialized to 1 in DGROUP** (image offset
0x9838c = `01 00 00 00`), has exactly one code reference (the read
at `0x409a9d`), and zero writers — so the flag≠0 painter's path is
live on every frame. The flag==0 variant is the child-swapped
front-to-back order (dist>0: `+0x12` first → flush `+0x24` → span
`+0x14` → flush `+0x28` → `+0x10` last; note its list-flush order is
also mirrored, still far-side list first relative to travel) — dead
code kept only as a documented variant. RESOLVED — the apparent
"front-to-back + unconditional writes" paradox was produced by
misreading the flag's initial value; with the real order there is no
contradiction: unconditional last-write-wins stores under a
far→near submission produce correct occlusion, and no z-buffer or
coverage buffer exists anywhere in the path (all 12 entries of both
installed span-drawer tables, the flat fill, the three negative-
material effect drawers, and the clipper were audited — direct
indexed-byte stores only). Dynamic objects follow the same
convention: `FUN_00409a00` buckets entries into per-node `+0x24`/
`+0x28` lists, and under `DAT_00541500 == 1` (set once at init,
0x40108d) flag-1 entries go to a 4096-entry depth-keyed deferred
list sorted `b_key - a_key` (descending = far-first) and drained
through the same clipper — painter's again.

Spans are `{lo16 count | hi16 firstIdx}` into the poly table — the
same encoding the collision walker documents. `FUN_00409860` skips
polys with `+0x20 bit4 (0x10)` (the render-skip bit; the collision
skip is `0x20` — different bit, same record), derives
`DAT_005414b8 = (+0x20 bit0 && DAT_005414b4)` per poly, and draws
the `+0x22` bit7 edge overlay — see below.

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
| +0x20 | u8 flags — bit4 render-skip; bit0 selects the \|4 span-drawer half when the view is unbanked (see DAT_005414b4 below) | OBSERVED |
| +0x21 | u8 aux — UNKNOWN, preserved raw | UNKNOWN |
| +0x22 | u8 aux — bit7 enables the edge-line overlay; bits 0x10/0x20/0x40 select edges v1→v0 / v2→v1 / v2→v0 (see below) | OBSERVED |
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
| >= 0, `table[idx] != NULL`, `rec+0x24 != 0` | `FUN_0046daac` — perspective texture mapper (per-vertex `1/z, u/z, v/z` gradients; span-drawer index = `(rec+0x0c & 3) \| (DAT_005414b8 ? 4 : 0)` into the installed 12-entry table) |
| >= 0, `table[idx] == NULL` | flat fill `FUN_00415260`, pen `0xff` (`0x40c9da`) |
| >= 0, `rec+0x24 == 0` | error lookup + flat fill `0xff` |
| -1023..-1 except -1010..-990 | flat fill, pen = `(-idx) & 0xff` (the `PEN_n` names) |
| -1010..-990 | `FUN_0047a770(idx+1000)` — s4-grid family effect (UNKNOWN semantics) |
| -1027..-1024 | `FUN_00412970(table[0x540b20] + ((-1024-idx)<<8))` — 4 variants (UNKNOWN semantics) |
| -1028 | `FUN_0046e940` (UNKNOWN semantics) |
| <= -1029 | `FUN_00412970(table + ((-1029-idx)<<8))` (UNKNOWN semantics) |

All span stores are unconditional (no z-buffer, no coverage mask —
searched both span-drawer tables and the flat fill: 600-byte
framebuffer stride, `MOV [EDI],AL`-style stores only). Correctness
comes from the painter's submission order above.

## Frame-level render globals (Phase 6B — RESOLVED)

- `DAT_005414d4` — **frame-skip draw gate** (OBSERVED). Written only
  by the framerate limiter `FUN_0042fb68` (plus init paths that set
  it to 1): each frame sets it to 1, and clears it to 0 when the
  time accumulator decides a rendered frame should be dropped
  (`DAT_0049b6fc < 4` consecutive skips while the accumulator
  exceeds the budget). Readers: the clipper `FUN_0040ca00` returns
  immediately when it is 0 (whole-frame raster suppression), the
  `+0x22` edge overlay in `FUN_00409860` is gated on it, the
  scope/secondary-buffer overlay `FUN_0046ec60` runs only when it
  is nonzero, and several traversal/HUD paths read it
  (`GAMEPLAY_RECONSTRUCTION.md` already calls it `hudActive`).
  NOT an occlusion selector, NOT a deferred-sort switch.
- `DAT_005414b4` — **unbanked-view flag** (OBSERVED). Recomputed per
  frame in `FUN_00436100` (and the `FUN_0047c504` camera path) as
  `(DAT_00540b4c + DAT_00540b60 == 0.0f)` — the two bank/roll
  accumulators (`0x540b4c` = turn-bank, ±2 kick, clamp ±10;
  `0x540b60` = aux bank; their sum drives the banked basis). When
  the sum is nonzero the same code calls `FUN_0046ae60(1)` — the
  banked projector install at `0x49bbe8` — so `5414b4` reads as
  "view is not banked this frame".
- `DAT_005414b8` — per-poly derived flag (OBSERVED): set to 1 by
  `FUN_00409860` when `poly+0x20 bit0 && DAT_005414b4`, else 0;
  the dynamic-object pushers zero it. Consumed in `FUN_0046daac`
  as `spanDrawerIdx = (materialFlags & 3) | (b8 ? 4 : 0)` — i.e.
  bit0 polys use the upper half of whichever 12-entry span-drawer
  table is installed (`FUN_0046da6c` installs table A when
  `DAT_00541520 == 6`, else table B; `541520` is a config/mode
  value defaulting to 4). Table A's `|4` entries skip texel 0
  (`CMP DL,0; JZ` — masked transparency); table B's `|4` entries
  are structurally different but unconditional — so the variant's
  visual role is install-dependent (PARTIAL). Bit0 census (BUILD_A,
  LEVEL3–8): 825–1607 polys per level (~5–23%), on both textured
  and negative-material polys. The bank-zero gate means the variant
  applies only in the normal unbanked view.
- `DAT_00541500` — deferred-sort mode for dynamic draw entries
  (OBSERVED): initialized to 1 at `0x40108d`, re-armed by
  `FUN_00423ca0`; when nonzero, flag-1 entries are pushed into the
  4096-entry depth-keyed list and drained far-first after sorting.

## Polygon `+0x22` edge overlay (Phase 6B — RESOLVED, OBSERVED)

In `FUN_00409860`, when `poly+0x22 bit7` is set AND `DAT_005414d4`
(draw-frame) is nonzero, the submitter draws the poly's flagged
edges as screen-space lines — before the triangle itself is
clipped/submitted. Bits `0x10/0x20/0x40` select edges
v1→v0, v2→v1, v2→v0 respectively. Each edge goes through
`FUN_0040da54` — a 3D clipper (near z=0.05 plus the per-vertex clip
masks) feeding `FUN_0046b5f0` (the installable projector at
`0x49bbe8`) — then a Bresenham line:

- color arg `>= -1023` → `FUN_00415450` — flat pen line
  (`*dst = pen`): pen = 1 for textured polys, `-matIdx` for pen
  class, raw `matIdx` for `<= -256` classes (which then take the
  remap variant below anyway when `<= -1024`);
- color arg `< -1023` (i.e. `matIdx <= -1024`) → `FUN_004154f0` —
  LUT-remap line: each covered framebuffer byte is remapped through
  `DAT_00540b20 + idx*256`, the same remap-table family as the
  `FUN_00412970` poly fill.

Census (BUILD_A, LEVEL3–8): **every** bit7 poly carries a
`FUN_00412970`-class material index (-1027..-1024) — 0 to 1512
polys per arena, edge-mask bits mostly set. So this is a deliberate
effect, not a debug leftover: the remap-effect polys (screen-space
LUT-remap fills — force-field/shimmer regions) draw their edges
through the same remap LUT, producing energized borders. Bits
0x0f of `+0x22` are unused in all real data. The earlier
"debug-normal mask" naming was wrong — the drawn primitives are the
poly's edges, not normals.

## Rasterizer replacement boundary (the G1 contract)

MUST reproduce (semantics): BSP submission order (painter's
back-to-front — with a hardware depth buffer the order is redundant
but harmless; without one it IS the visibility contract), per-poly
skip (+0x20 bit4), the material dispatch above (including NULL->0xff
and the pen mapping), the UV/material association, palette-index
pixels, and the +0x22 edge overlay for remap-effect polys.

MAY replace (implementation): the scanline/DDA span drawers, the
DirectDraw plumbing, the 600x360 back buffer itself, and the
frame-skip gate (a Godot frontend draws every frame —
`DAT_005414d4` is a timing throttle, not a visibility mechanism).
Preserve `arenaRenderOrder`'s output order regardless: it is the
original's submission order and cheap to keep even with z-ordering.

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
  (iterative equivalent of the recursion + tail-descend; the live
  painter's order is the default, `frontToBack` exposes the dead
  0x499f8c==0 variant).
- Named flag bits: `kArenaPolySkip`/`kArenaPolyAltSpan` (+0x20) and
  `kArenaEdgeOverlay`/`kArenaEdgeV10`/`kArenaEdgeV21`/`kArenaEdgeV20`
  (+0x22).

Ownership: `ArenaRenderData` aliases the source file bytes for
verts/nodes/pixels/palette (caller keeps the buffers alive); decoded
records are by-value.

## Diagnostic

`mdk-inspect --data-path DIR --arena-render <LEVELn.DTI>` decodes the
sibling `<stem>O.MTO` + `<stem>S.MTI` for every block: counts,
vertex AABB, bank sizes, resolved/missing material names, dispatch
class histogram, +0x20/+0x22 flag census (alt-span bit0, edge-overlay
bit7, edge-mask 0x70), palette size, FNV-1a digests of the decoded
geometry and of the BSP submission order at a probe camera
(`--arena NAME`, `--start X Y Z`).

## Cross-level results (BUILD_A, all six levels)

60/60 MTO blocks decode cleanly; zero crashes/asserts. Material-name
resolution is complete except where the original itself falls back
(see above). Dispatch classes observed in real data: textured,
unresolved(->0xff), pen, fx770 (HMO_9 LEVEL3), fx12970 — no fxe94
instances.

## Remaining render RE (Phase 6B census — P0 CLOSED)

All three Phase 6B P0 items are resolved (see the sections above):
the occlusion order (painter's, flag init 1), `DAT_005414d4`
(frame-skip draw gate), `DAT_005414b4`/`+0x20 bit0` (unbanked-view
alternate span-drawer selection), and `+0x22 bit7` (edge-line
overlay). **No render P0 items remain** — a frontend can render
static arenas from `ArenaRenderData` + `arenaRenderOrder` without
further executable archaeology.

- P1 (useful fidelity, does not block a first renderer):
  semantics of `FUN_0047a770`, `FUN_0046e940`, `FUN_00412970` (the
  negative-index effect drawers — all write the same framebuffer;
  classes exist in real data: fx770 40 polys in LEVEL3 HMO_9,
  fx12970 widespread); the exact visual role of the `|4` span
  variants across both installed tables (PARTIAL — table A masks
  texel 0); which LUT index `FUN_004154f0` uses for each
  remap-material index; the `+0x21` byte; the
  `FUN_0040e838`/`FUN_00409e4c` preprocessing details; per-frame
  `FUN_0040b4dc` texture-animation frame selection; the banked-view
  projector modes (`FUN_0046ae60` modes 1–4); runtime-oracle frame
  comparison of the painter order (confirmatory only — the static
  case is already dispositive).
- P2 (ordinary implementation, no executable archaeology): wiring
  `ArenaRenderData` into a consumer, palette->RGBA expansion for a
  frontend, the camera-pose binding, edge-overlay drawing for
  remap-effect polys (can ship without it — purely cosmetic).

## Open unknowns carried forward

- Effect-drawer semantics (fx770/fx12970/fxe94) — P1.
- `|4` span-variant visual role across installs — P1 (PARTIAL).
- `DAT_005414d4` frameskip policy (the throttle counters) — P1,
  cosmetic only; the gate semantics are resolved.
- Whether bank A is exclusively `LEVELnS.MTI` in all streaming
  states (a per-arena reload could change the shared bank between
  matlkup calls) — P1, matters only for cross-arena name shadows.
