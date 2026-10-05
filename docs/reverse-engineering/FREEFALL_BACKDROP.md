# Freefall mode-2 backdrop / materials / launch visuals (BUILD_A)

Scope: `FUN_00412530` backdrop pass, the generated 384-row palette LUT,
the mode-2 span renderers, the missile trail pens, the kind-5 launch
FLARE, and the kind-1 entry. All addresses MDK95.EXE BUILD_A vaddr.
Evidence level per item: OBSERVED = instruction-level decode;
CORROBORATED = decode + resource cross-check; HYPOTHESIS = open.

## 1. Backdrop frame (FUN_00412530, OBSERVED)

Called once per freefall frame by the mode-2 frame renderer
`FUN_00410920`, before the object draw walk. Output is the shared
600x360 indexed framebuffer `0x541650` (stride 600).

### Inputs

| Global | Meaning |
|---|---|
| `0x540b28/2c/30` | camera world pos (copied from the camera object by `FUN_004123f4`; runtime `camX=px*0.85, camY=py*0.85, camZ=pz+10` before the 30 s zoom-out) |
| `0x4edc00` | `scrollPos` float, `+= 1/30` per frame (`0x41043d` region, `0x49b6f4` = 1/30) |
| `0x4edbf0` | ZOOM table counter, `++` per call, wraps `&15` |
| `0x4edb5c` | chunk scroll float, `+= 0x49b6f0` (0.5) per frame, wraps at bound 8 |
| `0x4edc24` | `LEVEL%d` record ptr (1024x1024 indexed, 1024 stride) |
| `0x4edc2c` | `POD%d` record ptr (64x1024 indexed, 64 stride) |
| `0x4edc30` | previous scroll row (i32) |
| `0x4edbb0[16]` | ZOOM0000-0015 record ptrs (`ZOOM%4.4d` bind loop `0x40f100+`) |
| `0x4edb3c[8]` | `L%d_C000%d` chunk record ptrs (bind loop `0x40f104+`, `0x403e00`) |
| `0x4edb30/34/38` | last-bound chunk aux `{rec+4, rec+8, rec+0xc>>16}` = `{64, 108, ...}` = chunk src W,H |
| `0x4edc34` | generated-LUT base + 0x400 (row 4 alias) |
| `0x4edbf4` | `0xc00` — backdrop LUT row offset 12 (relative to `0x4edc34`, i.e. LUT row 16) |
| `0x541548` | windowed-render gate (0 = full 600x360 path) |
| `0x49b578` | windowed display-height int (runtime-set; 0 in image) |

### Formula (full-screen path, `0x541548 == 0`)

```
p        = camZ * 0.0001893939            (0x494fb0; = camZ/5280)
uCenter  = 0.36*camX + 512                (0x494f88, 0x494f90)
v3c      = 824 - 18.90909*scrollPos - 0.36*camY
                                          (0x494f98=1/33 * 0x494fa0=-624
                                           -> -18.90909; 0x494fa8=824)
uStart   = uCenter - 300*p                (0x412955: v28*0x494fb8 -> fsubr v38)
vStart   = v3c - 180*p                    (0x412740: v34*0x494fbc=180)
scrollRow = trunc(824 - 18.90909*scrollPos - 33)
            where 33 = (0x4edb34 * 80) >> 8  (0x4edb34 = chunk H 108;
            int math: 108*80>>8 = 33)
```

Windowed path (`0x541548 != 0`): `v40 = int(0x49b578*(1 - scrollPos*
0.0151515))` (0x494fc8); `uStart = uCenter - (v40+300)*p`; the chunk
x offset also adds v40. Our port renders the 600x360 full frame, so
v40 = 0.

### Per-pixel sampler (FUN_0046d780, OBSERVED)

Arg struct `{+0x00 uFracInit, +0x04 vFracInit, +0x08 uFracStep,
+0x0c vFracStep, +0x10 uIntStep, +0x14 vIntStepRows, +0x18 texelPtr,
+0x1c dstPtr, +0x20 spanRec, +0x24 lutBase}`:

```
uFrac = frac(uStart); vFrac = frac(vStart)
uInt  = floor(p);      vInt  = floor(p)        (same p both axes)
texel = LEVEL + floor(uStart) + floor(vStart)*1024
360 output rows; each table row drawn twice (line doubling)
per pixel: uFrac += frac(p); texel += uInt + carry
per row:   vFrac += frac(p); texel += vInt*1024 + (carry ? 1024 : 0)
per pixel: out = shaded ? lut[shadeByte][texel] : texel
lutBase = 0x4edc34 + 0xc00 -> LUT bank0 rows 16+shade (shade bytes 1..8)
v wraps modulo 1024 rows (LEVEL is the circular scroll buffer).
```

### ZOOM span records (OBSERVED, resource-verified)

`ZOOM%4.4d` in FALL3D.BNI, 16 records ~95-97 KB. Header u32 = size-4.
Then exactly 180 rows; each row:

```
u32 cntA; u8 shadesA[cntA*4]; u32 cntB; u32 cntC; u8 shadesC[cntC*4]
with (cntA+cntB+cntC)*4 == 600 pixels.
Phase A/C pixels are LUT-remapped per-pixel shade byte; phase B raw.
```

Verified parses: all 16 ZOOM0000-0015 — each is exactly 180 rows,
every row sums cntA+cntB+cntC == 150 quads, and the walk consumes the
record to the last byte (the u32 after `size-4` is row 0's cntA == 150
for the all-shaded top row — NOT a second header field). Shade bytes
range 1..8. 16 tables cycled one per frame = temporal dither of the
shade pattern.

### POD -> LEVEL seam wedge (OBSERVED)

```
delta = max(24, prevScrollRow - scrollRow)   (0x4125f7: <24 -> 24)
for row in 0..delta-1:
    count = row >= 24 ? 32 : round(row*2/3 + 16)   (0x494fd0=2/3, 0x494fe0=16)
    LEVEL[scrollRow+row][512-count .. 511] = POD[scrollRow+row][32-count .. 31]
dst row steps +1024, src row steps +64. prevScrollRow stored to
0x4edc30 each frame.
```

A wedge of POD columns under screen column 512 repairs the
vertical-scroll seam where the pod column crosses the terrain.

### Chunk sprite draw (OBSERVED)

After the floor pass, `FUN_00403a40` dispatches `0x46d680`
(transparent scaled blit, skip pen 0) with:

```
x  = int(-0.36*camX/p + 300 [+v40])          ([ebp-0x74])
y  = int(+0.36*camY/p + 180)                 ([ebp-0x70])
sx = sy = int(192/p)                         (0x494fd8 / v28 -> [ebp-0x6c/0x68])
src = L%d_C000{int(chunkScroll)}             (record table 0x4edb3c)
aux = {0x4edb30, 0x4edb34} = {64, 108} src dims
```

Scale grows as `192/p` — the pod sprite grows during approach; the
camera lands on it (p -> ~1 near 5280 -> smaller when close... p
shrinks as camZ drops -> sprite grows).

## 2. Generated LUT (OBSERVED)

`0x540b20` = allocated LUT base: 6 banks x 64 rows x 256 bytes.
`0x4edc34` = base+0x400 (row 4 alias used by draws).

Builder `FUN_00406d84(pal=0x499adc, color{3B}, level ebx, dst)`:
`dst[c] = nearestPal((pal[c]*(256-level) + color*level) >> 8)`.

Init (`0x42b847` region): keyframe table at `0x49b57c`, entries
`{R,G,B,steps}` (file data, OBSERVED):

```
entry0 {0x5a,0xce,0xde} s=0    (cyan-white anchor)
entry1 {0x21,0x7b,0x8c} s=8    (steel blue)
entry2 {0x08,0x31,0x7b} s=8    (deep blue)
entry3 {0x84,0xa5,0xc6} s=8    (light steel)
entry4 {0x8c,0x7b,0x84} s=8    (mauve)
entry5 {0xe7,0xc6,0xd6} s=8    (pale rose)
entry6 {0x84,0xa5,0xc6} s=8    (light steel)
entry7 {0x08,0x31,0x7b} s=8    (deep blue)
entry8 {0x5a,0xce,0xde} s=8    (cyan-white)
```

Per keyframe i (1..8): `steps` rows lerping prevColor->thisColor;
each row is built into all six banks with levels {90,85,80,60,40,15}
(bank offsets 0x0000,0x4000,0x8000,0xc000,0x10000,0x14000). 64 rows
per bank total.

Effective color ramp (bank 0, mix 90/256 = 35.2% toward row color):

```
rows  0- 7  cyan-white -> steel blue
rows  8-15  steel blue  -> deep blue
rows 16-23  deep blue   -> light steel
rows 24-31  light steel -> mauve
rows 32-39  mauve       -> pale rose
rows 40-47  pale rose   -> light steel
rows 48-55  light steel -> deep blue
rows 56-63  deep blue   -> cyan-white
```

Consumers seen in mode 2 (all bank 0):

```
backdrop:      row 12 + shade(1..8)  -> rows 13-20
kind-5 FLARE:  row 6 + count(0..8) + srcPx(0..8) -> rows 6-22
trail body -1054:    row 25
trail head -1058..-1065: rows 29-36
```

## 3. Negative pen dispatch (FUN_0040c860, OBSERVED)

Signed material pen < 0 goes through the fill dispatcher
(`FUN_00412970` = fixed-point triangle fill into the 600-stride
buffer, `dst = lutRow[dstPx]` destination remap):

```
-255..-1        -> FUN_00415260 flat fill, color = -pen (u8 low)
-1010..-990     -> FUN_0047a770(pen+1000)
-1023..-1011    -> FUN_00415260 flat fill, color = -pen & 0xff
                   (=> flat palette colors 253/254/255 for -1021..-1023)
-1027..-1024    -> FUN_00412970 LUT rows (-1024-pen) = rows 0..3
-1028           -> FUN_0046e940 (textured/shaded tri fill)
<-1028          -> FUN_00412970 LUT row (-1029-pen)
                   (pen -1054 -> row 25)
```

## 4. Missile trail (kind 4, OBSERVED)

- Spawn (`FUN_0042eaa8` alloc + `FUN_0042eadc` init cap 32) when the
  object carries +0x60.
- `FUN_0042eb3c` anchor scan (call once at missile spawn `0x411657`):
  over `model+0x20` element table -> element[0]'s vertex list;
  stores min-X and max-X vertices as anchors +0x24/+0x28.
  MISSILE (FALL3D.BNI): one element, 13 verts / 18 tris; anchors =
  vert 11 (-3.3899,-12.3584,~0) and vert 12 (+1.9458,-12.3351,-2.9439)
  — the tail's widest points, X span ~5.336.
- `FUN_0042ecc4` feed per tick: basis-transformed anchor points into
  the ring slot; slot pt[anchors] = pt[0] (closes the section).
- Draw `FUN_0042ee74`: consecutive ring slots OLDEST->NEWEST from
  the +0x20 read cursor -> section quads, composited into the
  indexed framebuffer via `FUN_00412970`'s `dst = lut[row][dst]`.
  Section s pairs walked slots s-1,s. Pens (OBSERVED 0x42f096,
  `pen = 0xfffffbfb - ((0x26 + s - 1) - count)`):
  the NEWEST eight sections (s >= count-8) get `count-s-1066`
  = -1058..-1065 -> LUT rows 29-36; every older section gets -1054
  -> LUT row 25. The earlier "-1021..-1028 oldest-section" note was
  wrong — disproven by the 0x42f096 accumulator decode.
  Young taper `0x49b634`: edge anchors spread toward the slot
  centroid by t = {1.0,1.25,1.2,1.1,1.05,1.0} for the six NEWEST
  slots (walked-age 0..5), then `1-(age-6)/(cap-6)` decay.
- Port: `freefallSceneTrailComposite` bakes the veil into
  `backdropFrame` after the backdrop pass (same relative order as
  the original's object walk). Boundary caveat: the composite is
  behind ALL 3D bodies — the original sorts trail sections at
  their own depth, so a veil passing in FRONT of a farther body
  would still hide under it (rare; trails sit below the camera).

## 5. Kind-5 launch FLARE (OBSERVED)

- Draw-entry builder `FUN_004109d8`: `obj+0x108 != 0` emits kind 5
  keyed `obj.z + 10.0` (sorts just in front of the missile body).
- Handler `0x410d88`: sprite = FLARE4 (64x64 ring/disc, pixel values
  0..8) through `0x46d6d1` — scaled transparent blit, per src pixel
  `dst = lut[6 + obj+0x108 + srcPx][dstPx]` (base `0x4edc34+0x600` +
  `+0x108 << 8` at `0x410dfb-0x410e10`, i.e. LUT row 6+count);
  screen pos = projected object pos; scale field 0x80.
- Lifecycle (object tick `0x410f7f`+): spawn writes `obj+0x11c = 60`
  (frames, `0x411590` inside the sin/cos launch-velocity block).
  While `+0x11c > 0`: `+0x108` ramps 0 -> 8 cap and `+0x11c` counts
  down by the frame delta `0x49b6e8`; after `+0x11c` reaches 0:
  `+0x108` decays to 0. So the ring brightens ~60 frames then fades.
  `obj+0x11c = -1` = disabled state.
- FLARE1/2/3/4 are FALL3D.BNI image records {u16 w,h,px}: 32x32,
  48x48, 16x16, 64x64; kind 5 always binds the FLARE4 aux triple
  (0x4edb94/98/ac, name string `FLARE4` at 0x494ad4).

## 6. Kind-1 entry (OBSERVED)

`obj+0x10c != 0` emits kind 1 keyed `obj.z - 1e-5` -> handler
`0x410bf2`: PICK sprite (records bound via name `PICK` at 0x494ab4 ->
0x4edb60/64/68/6c) through `0x403a40 -> 0x46d680` (center-pos,
pen-0 transparent scaled blit) at the projected pos, gated z' > 0.

Scale (OBSERVED `0x410c37-0x410c4c`): the handler computes
`scale = trunc(viewW * 32.0 / (z' * zoom))` = `trunc(8000/z')`
(viewW 600, zoom 2.4), then `0x403a40` emits
`outPx = srcPx * scale >> 8` per axis. The `0x494d30` constant is
the exe's .rodata double **32.0** (`fmul qword` — bytes
`00 00 00 00 00 00 40 40`; a dword-only read sees 0, and 3.0
would encode `00..00 08 40`). The next qword `0x494d38` = -1e-5
is the kind-1 sort bias.
Effective: PICK 64x64 -> outW = 64*scale>>8 — first visible pixel
at z' <= 2000, ~16px at z' = 500, full 64px at z' ~= 31.
It is the pickup marker visual — not a missile radar. Missile
approach warning in mode 2 = the kind-5 FLARE + the trail; there is
no separate radar ring on missiles.

## 7. FALL_T1 opening teletype (OBSERVED, implemented)

`FUN_0040ef28` (freefall init) at `0x40f60a` gates on `levelId == 0`
(course 0 only — skill does not gate) and posts
`FUN_0041cad0("FALL_T1", EDX=flags 1, [stk]=rate 3.0f)`. The name at
`0x494be4` resolves through `MISC/MDKFONT.FTI` to the single-line
record `Avoid the RADAR!\0` — no `\n` split, so the queue's consume
arm produces `lines=1`, drawn at `y=0x78`.

The mode-2 draw block services the engine-global teletype queue
(`FUN_0041cb44`, documented in STREAM_SCENE §15) once per frame, so
the entry runs its full lifecycle during the descent:

- **consume** — `charTimer = 3.0`, `holdTimer = 0`, flags 1.
- **slide-in** (flags&1, hold<0.5) — `holdTimer += dt30` to 0.5,
  drawn scaled at `hold*2` through renderer 1 (`FUN_0041518c`,
  centered FONTBIG): ~15 frames.
- **steady hold** — `charTimer -= dt30` from 3.0, drawn through
  renderer 0 (`FUN_00414d2c` — centered FONTBIG, FONTSML on
  600px-width overflow): ~90 frames.
- **page-out** — `charTimer == 0`, `holdTimer -= dt30` to 0, same
  scaled geometry in reverse: ~15 frames.

Port: `FreefallRuntime::teletypePost` records the post (name/flags/
rate — the runtime does not own the FTI resolver). The Godot bridge
arms a single-entry subset of `FUN_0041cb44` at freefall entry
(`MdkBridge::ffTtEnter_`, called from both `load_freefall` and the
campaign handoff), services it once per `stepFreefall_` frame
(`ffTtService_`), and presents the transparent indexed strip as an
RGBA overlay (`ff_teletype_frame`) — the same FONTBIG/FONTSML
renderers the mode-5 stream presenter uses, with `SYS_PAL`'s head
as the text palette. Verified live on the campaign route: the
scaled slide-in is visible on freefall entry, the full-size hold
runs ~3 s centered at y=0x78, and the strip pages out ~4 s in —
course>0 runs post nothing (native test asserts the gate and the
exact name/flags/rate).

Note the resolve-miss path (`0x41cb25`): a missing record posts
nothing silently — kept as a no-op rather than an error.

## 8. Depth-flag semantics probe (§4A, OBSERVED on the shipping build)

The `e5a4487` commit message claimed `depth_draw_never` had disabled
the depth test entirely. A minimal overlap scene on the exact shipped
backend (Godot `4.7.2.stable.official.ed1daf0bf`, Metal 4.0 Forward+)
disproves that — probe scene preserved at
`analysis-private/depth_probe/`:

| quad | render_mode | result |
|------|-------------|--------|
| A (behind occluder) | `depth_draw_never` + `ALPHA=1.0` | 0 px — occluded |
| B (behind occluder) | `depth_test_disabled` + `depth_draw_never` | 676 px — draws over |
| C (behind occluder) | ALPHA transparent, no depth flags | 0 px — occluded |

`depth_draw_never` disables depth **writes** only; the test stays on
(matches the Godot docs). `depth_test_disabled` is the separate flag
that drops the test. For an `ALPHA`-writing shader both e8cc920 and
e5a4487 land in the transparent pass — depth-tested, never writing —
so the e5a4487 render_mode edit was functionally neutral on this
backend. The currently-presented overlap (trail/flare sections
occluding behind nearer bodies, `depth_overlap_f000460.png`) is the
correct contract either way; the causal attribution in the old
commit message was wrong, and this note supersedes it without
rewriting the pushed history. Current flags are kept — they produce
the observed-correct result: trail/flare = transparent pass
(depth-tested, no write); wedge = `depth_test_disabled` +
`depth_draw_never`, priority 1.

**Correction (§4C):** the earlier "painter-last, matching its z'≈0
object key" parenthetical was wrong. The radar object sits at
`pz = 0` (the scan plane the camera descends toward), so its
draw-entry key is the SMALLEST mdk z — the wedge is the FARTHEST
draw in the sorted walk, not the last. Its veil tris composite over
the already-drawn backdrop and get overwritten by every nearer
surface. The priority-1/depth-test-disabled presentation is still
correct: the §4C chain mask + depth gate reproduces the painter
semantics exactly (buried wedge elements are skipped), and the
untested fragment is required so the remap reaches backdrop px.

## 9. rgb565 inverse-lookup audit (§4B, OBSERVED on real palettes)

The veil shaders recover the source index via a 65536-entry rgb565
map built from the 256-entry FALLP_<c+1> palette — later entries
overwrite earlier same-key entries, uninitialized keys read 0.
Audit over all five course palettes and all 64 bank-0 LUT rows
(only bank 0 is uploaded to `lut_tex`):

| palette | keys init | collision groups | identical-RGB | different-RGB | LUT-divergent losers |
|---------|-----------|------------------|---------------|---------------|----------------------|
| FALLP1  | 252       | 3                | 3             | 0             | 0                    |
| FALLP2  | 247       | 3                | 3             | 0             | 0                    |
| FALLP3  | 249       | 3                | 3             | 0             | 0                    |
| FALLP4  | 252       | 3                | 3             | 0             | 0                    |
| FALLP5  | 246       | 3                | 3             | 0             | 0                    |

Every collision is a duplicate-color palette entry — no two
different-RGB entries share a key, and no collision changes any
used row's output. So the recovery is **exact for palette-exact
screen colors** on all five courses (measured, not assumed — the
earlier "lossless" wording needed this scope).

Non-palette-pixel reachability (what could hit the ~65284
uninitialized keys → index 0): all scene materials are unshaded
with nearest filtering; the project runs no MSAA (default); the
sRGB→linear→sRGB round-trip on this backend preserves the 8-bit
palette values byte-exact; fade is a `FadeLayer` CanvasLayer the
`screen_texture` never contains (veils read the unfaded 3D pixels —
correct); the FALL_T1/HUD overlays are CanvasLayers too. Veil
output itself is never sampled (screen_texture is the pre-
transparent-pass snapshot — see §10). Remaining theoretical source:
a non-palette color produced by edge resolve/tonemap — none
configured.

**Cache invalidation fix (real bug found).** `ff_palette` was
loaded once and never invalidated; `ff_pal_tex`/`ff_idx_tex`/
`ff_lut_tex`, the `lut:`/`pen:`/`m:` material caches, the
trail/flare veil materials, and the PICK sprite texture all held
the first course's palette forever. The five palettes differ by
123-128 entries each — through FALLP1's map, ~119 of FALLP2's
colors hit uninitialized keys (→ index 0 → `lut[row][0]`) plus 2-4
wrong-index hits. Every campaign continuation (course≥1 freefall)
would have presented visibly wrong veil colors. `_apply_freefall`
now compares the snapshot's palette each frame and drops every
palette-derived cache on change; per-frame-rebuilt paths (backdrop
RGBA, `bdf["lut"]`) already tracked the live course.

## 10. Serial veil ordering (§4C, OBSERVED mechanism + correction)

`screen_texture` snapshots the opaque pass only — a veil shader
reading it can never see an earlier veil's output, so a px under K
veils got K-1 lost remaps (`lut[r2][p]` instead of
`lut[r2][lut[r1][p]]`). Exhaustive LUT-level measurement: ~86% of
all (r1, r2, p) triples differ, max RGB dist² ~50k — not subtle.

**Draw-order correction (supersedes the §8 note's claim).** The
sorted draw walk keys entries by mdk `obj.z` ascending = far->near
(CORROBORATED: the kind-5 flare's `obj.z + 10.0` key lands just
nearer than its body, the kind-1 marker's `obj.z - 1e-5` just
farther). The radar wedge's key is `pz = 0` — the SMALLEST z, the
FARTHEST draw — its tris composite over the backdrop and are
overwritten by every nearer surface. Not painter-last.

**Mechanism.** `freefallSceneVeilMask` (core) rasterizes each
frame's ordered veil ops — trail sections (per-section mdk-z key),
kind-5 flare quads (`obj.z + 10.0`, per-srcPx rows, texel 0
transparent), wedge tris (`obj.z`) — through the same folded-view
projection into a per-px record buffer: 8 ordered `{row, z'}`
slots, packed RGBAH and uploaded each frame (`mask_tex`). Every
veil shader walks the chain in order and applies each element iff
`z'_elem < z'_winner` (`hint_depth_texture`, linearized) — painter
semantics: an element behind the winning opaque surface was
overwritten, not remapped. `screen_texture` supplies the chain's
BASE index — the winning surface below every veil — via the §4B
inverse map, exact on palette-exact pixels.

Correctness properties — three cases per fragment:
- (a) Records exist, ≥1 passes the gate: `L_k[...L_1[p]]` replays
  in painter order, including trail-over-trail, flare-over-trail,
  and the wedge's own ring-on-ring overlaps. Every covering veil
  quad recomputes the full chain from the opaque base, so
  overlapping quad coverage writes the identical result —
  idempotent, no double-application.
- (b) Records exist but every element fails the gate (a nearer
  body won): the fragment writes the winner's own color back —
  an identity write — so a buried veil cannot tint the body.
  This is the dominant real-world case: the wedge cone spans a
  third of the screen and bodies fly through it constantly.
- (c) No records (CPU-vs-GPU rasterization edge px): the quad's
  own single remap as the coverage fallback.

Live diagnostics (`ff-veil` trace, OBSERVED, full campaign):
`ops` = veil ops (46 wedge tris + up to ~62 trail sections +
flares), `elems`/`covered`/`multi` = per-px records. The wedge's
rings project inside each other, so `multi ≈ covered` on nearly
every cone px — serial composition is exercised ~100% of the
time, not a corner case. `overflow` held 0 for ~95% of ticks
then spiked to 328-692 during a dense near-camera trail cluster
(cov ≤556 px, multi ≤532) — the 8-slot cap dropped the farthest
trail sections on those px only.

Residual seams:
- Record cap 8/px (nearest records win on overflow; `overflow`
  counter exposed in `ff_veil_mask`; first nonzero live readings
  logged at dense trail clusters, magnitude above).
- The CPU rasterizer's coverage can differ from the GPU's by an
  edge px — the own-row fallback absorbs it.
- The mask stores the painter *key* order, not a hypothetical
  per-px z' order (they coincide for the veil classes here).
- Half-float z' quantizes to ~4 units at the far end — two veil
  elements within one quantization step can swap gate order
  (degenerate-depth only).

Native test (`test_freefall_scene` §4C block): missile trail +
radar wedge + kind-5 flare on a real FALL3D scene — records carry
frustum z', wedge records sort FIRST at every shared px, multi-
element px exist (`multi > 0`).

## 11. Remaining seams / open items

- Trail depth ordering — the veil composites into `backdropFrame`,
  so it always sits behind the 3D bodies; the original depth-sorts
  trail sections inside the object walk (§4 boundary caveat).
  The Godot presentation path solves this for the display frame
  (depth-tested veils + the §4C chain mask); the caveat applies
  only to the native/headless compositor.
- `0x46e940` (pen -1028) — textured/shaded triangle fill path in
  the negative-pen dispatch; no current trail section reaches it.
- Windowed-path letterbox (`0x541548 != 0`, `0x49b578`) — not
  exercised by the 600x360 presentation; documented, not ported.
- The keyframe color ramp is exe data; reproduced as constants.
- The LUT quantizes mixes to nearest palette index; the port keeps
  the exact table so the remap is bit-faithful in index space.
