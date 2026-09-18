# Engine Reconstruction — Phase 4 status

Phase 3 (file-format structure) is CLOSED: all seven families have
evidence-backed structural parsers (SNI, MTI, MTO, CMI, DTI, FTI, BNI)
— see `DATA_FORMATS.md`. Phase 4 begins engine reconstruction:
original data flowing through the native runtime into the indexed
framebuffer and out the Metal presenter.

Phase 4A delivered the first vertical slice: one original visual
resource decoded end-to-end from user-supplied data into the window.
Phase 4B added the second: one indexed-only BNI bitmap whose palette
is stored separately and bound by the original consumer context.

## Selected first visual resource

**`MDKOPT` in `MISC/OPTIONS.BNI`** — the options-screen backdrop
(600x360 indexed bitmap with an embedded 256-entry RGB palette).

### Why this resource (candidate comparison)

| Candidate | Verdict |
|---|---|
| DTI backdrop | Consumer is a scrolling-cylinder sampler (`FUN_0046ec60`, `FUN_0047a770`), not a linear blit; palette coverage of sampled indices was not fully proven → deferred. |
| FTI `FONTSML`/`FONTBIG` | Per-glyph offset table + glyph headers observed, but the glyph draw path (`FUN_00423a24`/`FUN_00414f64`) needs more work and a faithful preview would require glyph-level semantics → deferred. |
| FTI `ARROW` | RLE sprite (0xfe escape stream) — more decode machinery than needed → deferred. |
| FTI `F8` | 1bpp font; foreground color provenance unproven → deferred. |
| BNI `MDKOPT` | **Selected** — smallest evidence-complete decode: dims, layout, stride, orientation, palette and transparency are all CODE-CORROBORATED and the payload tiles exactly. |

Per the selection rule, investigation of the other candidates stopped
once `MDKOPT` satisfied every required property.

## Original functions traced (statics, MDK95.EXE)

| Address | Role |
|---|---|
| `FUN_0041d81c` | OPTIONS.BNI load site: `FUN_004038f0` loads the whole image into the BNI slot `DAT_004a1e38` |
| `FUN_004038f0` → `FUN_00425a80` | whole-blob loader wrappers (`MOV EDX,0x4a1e38`) |
| `FUN_00403958` | the BNI lookup (count `@img+0`, 16-byte records `@img+4`, `FUN_0042fa50` name compare, returns `img + rec[+0x0c]`) — proven in Phase 3H |
| `FUN_004039c8` | lookup storing the resolved pointer into a caller global |
| `FUN_0041d7b4` | options path: resolves `MDKOPT` → `DAT_0049aa90`; sets `_DAT_0054bca0 = payload + 0x304` (pixel pointer) |
| `FUN_0041ebf4` | options state machine: copies `payload+0x304`, 54000 dwords = 216000 bytes into `DAT_00541650` |
| `FUN_0041dc90` | orchestrator: blits `0x34bc0` = 216000 bytes to the work surface; selects the payload HEAD as palette source (`local_1c = DAT_0049aa90`, fallback `DAT_00540820`); calls `FUN_00416700` to upload it |
| `FUN_0041d978` | second resolve path (`MDKOPT` → `DAT_0049aa8c`); `MOV ECX,0x258` (600) corroborates width |
| `FUN_00416700` | palette uploader: walks `0x300` = 768 source bytes (fade-in loop) |
| `FUN_0046d208` | expands `count` RGB triplets into 4-byte runtime entries at `DAT_0054d7b8` and uploads via `IDirectDrawPalette::SetEntries` (vtable +0x18; `PALETTEENTRY` order proves file bytes are **R,G,B**) |
| `FUN_0046c86c` | present: copies the 600x360 work surface into the 640x480 display surface at column +20, row +60 — i.e. centered |
| `FUN_00403a00` | generic `{u16 w, u16 h, px}` BNI bitmap reader (used by the indexed-only family) |

## Proven payload format (CODE-CORROBORATED + byte-exact)

```
MDKOPT payload [0x38, 0x34efc) — 216772 bytes, tiles exactly:

payload+0x000  u8[768]   palette: 256 x {R,G,B} (all entries defined;
                         bytes [0,192) are identical to the SYS_PAL
                         record in MISC/MDKFONT.FTI)
payload+0x300  u16le     width  = 600  (0x0258)
payload+0x302  u16le     height = 360  (0x0168)
payload+0x304  u8[216000] indexed pixels, row-major, top-down,
                         stride 600 (contiguous — the original's
                         rep-movsd copy has no row padding)
```

- **Orientation**: top-down — `FUN_0046c86c` copies work-surface row `i`
  to display row `60+i`; the payload→surface blit is contiguous so
  payload row order == display row order.
- **Transparency**: none — the blit overwrites all 216000 surface
  bytes; no color-key path exists on this route.
- **Palette depth**: full 8-bit (0xff occurs; no 6-bit VGA scaling in
  the upload path).
- **Every pixel index is covered**: the embedded table defines all 256
  entries — the "no guessed colors" requirement is met by construction.

Sibling records proven to share the layout (byte-verified, same
decode): `L1_INTRM`, `L1_MAP`, `L2_MAP`, `L3_MAP`, `L4_MAP`, `L5_MAP`
in `MISC/STATS.BNI` — each with its own embedded palette.

## Native decode architecture

```
MISC/OPTIONS.BNI (read-only, DataRoot)
  -> inspectBniDirectory       (Phase 3H, unchanged)
  -> findBniRecord(dir, name)  (new: directory-level lookup mirroring
                                FUN_00403958; bounded ASCII
                                case-insensitive compare)
  -> bounded payload span      (record's proven [offset, next) span)
  -> decodeBniPalettedImage    (new: validates size == 772 + w*h
                                exactly; rejects truncation, slack,
                                zero dims and w*h escape — no silent
                                truncation)
  -> IndexedImage              (new core type: w/h/stride, indexed
                                pixels, embedded 256-entry RGB palette)
  -> blitIndexedImage          (new: center 1:1 if it fits, uniform
                                nearest-neighbor fit otherwise;
                                MDKOPT is exactly 600x360 = full surface)
  -> IndexedFramebuffer + Palette  (Phase 3A, unchanged)
  -> MetalPresenter.present    (Phase 3A, unchanged)
```

The decoder is `src/core/bni_image.{h,cpp}`; the image type and blit
are `src/core/indexed_image.{h,cpp}`; the record lookup is in
`src/core/bni_directory.{h,cpp}`. A second proven shape —
`{u16 w, u16 h, px[w*h]}` with no embedded palette — is classified by
`probeBniImage` but has no decoder yet (its palette is resolved by the
consumer context, e.g. the stream `PAL` record; palette provenance for
that class is not yet wired).

## Preview command

```sh
build/native/mdk-native.app/Contents/MacOS/mdk-native \
  --data-path original/installed \
  --preview-resource MISC/OPTIONS.BNI MDKOPT
```

`--preview-resource FILE RECORD` requires `--data-path`. The decoded
image is blitted once and presented statically until quit (Esc or
`--frames N`). `--dump-ppm FILE` still dumps the exact presented
frame. `mdk-inspect --data-path DIR --visual-info FILE RECORD` prints
the record's span, probe classification, dims and a deterministic
FNV-1a64 digest (`MDKOPT`: `6017f4c4bd57c479`).

## Explicit non-goals (Phase 4A)

- No level loading, menus, gameplay, renderer reconstruction,
  animation, audio, `.LBB`, `.SAV/.FLC/.MVE` expansion.
- No options-screen text or cursor: `MDKOPT` is the backdrop only —
  the original then draws font glyphs (`FUN_00423b38`) and the FTI
  `ARROW` cursor (`FUN_004236c0`/`FUN_00409760`) on top.
- No palette fade: `FUN_00416700` fades the uploaded table in over
  ~140 iterations; the preview uses the final stored values.
- No brightness offset: `FUN_0046d208`'s `DAT_0054147e` +16/level
  clamp is a user-setting quirk on the hardware upload path; the
  preview uploads stored values (equivalent to the zero-setting path).
- No generic asset framework: one decoder family, one preview flag.
- Phase 3 parser semantics unchanged (additive `findBniRecord` only).

## Remaining visual unknowns

- Palette provenance for `indexed-only` BNI bitmaps (BG/SPACE/
  PLANET/MOON/EARTH/SKULL): which palette resource each consumer
  binds — likely the `PAL` records — needs per-context tracing.
- FTI font payload organization (per-glyph records under `FONTSML`/
  `FONTBIG` offset tables) and the `F8` 1bpp font's color source.
- `ARROW` RLE sprite semantics (0xfe escape stream, header u32s).
- `INTRO1A` (OPTIONS.BNI): zero w/h at +768 — different record class,
  not an image under the proven layouts.
- DTI backdrop color coverage when `s3` count < 256 (which indices a
  given plane actually uses, and the s3-vs-full-palette interaction).
- The second MDKOPT resolve path (`DAT_0049aa8c`, palette from
  `0x54b980`): when it runs and why it uses a different palette
  source — edge case in options-state routing.

## Phase 4B candidate directions

Grounded in this slice:

1. **Indexed-only BNI bitmaps + context palette** — the `{w,h,px}`
   class (BG/SPACE/PLANET/MOON/SKULL) needs each consumer's palette
   binding proven (likely the sibling `PAL` records), then a second
   trivially small decoder unlocks the stream/fall3d/stats artwork.
2. **FTI font sheet decode** — `FONTSML`/`FONTBIG` payload layout via
   `FUN_00423a24`/`FUN_00414f64`; renders the font sheet as a visual
   resource without implementing text layout.
3. **First reconstructed front-end element** — options screen =
   MDKOPT backdrop + text overlay + `ARROW` cursor; needs (2) plus the
   cursor RLE decode.
4. **DTI backdrop presentation** — the scrolling-cylinder sampler is
   the only blocker; palette coverage for a chosen plane must be
   proven first.

# Phase 4B — indexed-only BNI + proven context palette

Phase 4B added exactly one capability on top of 4A: decode and present
one indexed-only BNI bitmap whose palette lives in separate records,
using the palette binding the ORIGINAL consumer establishes — not a
sibling-name guess.

## Selected resource

**`BG` in `STREAM/STREAM.BNI`** — the stream/cinematic backdrop
(600x360 indexed, no embedded palette).

### Why this resource (candidate comparison)

| Candidate | Verdict |
|---|---|
| `FALL3D/FALL3D.BNI` `SPACE`/`EARTH`/`MOON`/`SKULL` | `SPACEPAL`/`FALLP1..5` palette-like records exist and the shapes tile, but the freefall consumer (`FUN_0040ef28`) binds palettes through a different, multi-record path that was not yet traced → deferred. |
| `STREAM/STREAM.BNI` `PLANET` | Same BNI, same indexed-only shape — but the consumer resolves it for the rotating-planet overlay path, not the backdrop; `BG` is the simpler static target. |
| `STREAM/STREAM.BNI` `BG` + `PAL` | **Selected** — complete proof chain: file load, both record resolutions, palette composition, upload, and the blit are all instruction-level traced, and real bytes corroborate every span. |

Per the selection rule, comparison stopped once `BG`+`PAL` had a
complete proof chain.

## The proven binding (statics, MDK95.EXE)

The whole chain lives in the stream-mode init `FUN_0042b270`:

1. `MOV EAX,"STREAM\STREAM.BNI"; CALL FUN_00403928` — loads the whole
   BNI image into the per-context slot `DAT_004a1e38` (the same slot
   the Phase 3H lookup functions read). The wrapper preserves the
   caller's EDX, so the subsequent lookup's out-pointer survives the
   load.
2. `MOV EAX,"PAL"; CALL FUN_004039c8` → `FUN_004039a4` →
   `FUN_00403958`: the BNI directory scan resolves the record named
   **exactly `PAL`** and returns `image + rec[+0x0c]` — a name lookup,
   not adjacency.
3. `MOVSD.REP` copies `0x240` bytes from `PAL+0xc0` to
   `DAT_004ed818`, then `0xc0` bytes from `DAT_00540820` to
   `DAT_004ed758`. The destinations are adjacent
   (`0x4ed818 == 0x4ed758 + 0xc0`), so the composed 768-byte stream
   palette is:
   ```
   DAT_004ed758  entries 0-63    <- DAT_00540820[0:192]  (system head)
   DAT_004ed818  entries 64-255  <- PAL record [0xc0,0x300)
   ```
4. `DAT_00540820` is the engine's current-palette array; its 192-byte
   head is written ONLY by `FUN_0040163c` at startup, which resolves
   the `SYS_PAL` record (`MISC/MDKFONT.FTI`, via the FTI lookup
   `FUN_00414890`), forces its entry 0 to black, and copies 192 bytes.
   Xref-verified: no other writer of `DAT_00540820[0:192]` exists.
5. `MOV EAX,"BG"; CALL FUN_00403a00` — the indexed-only reader:
   `u16 w @+0`, `u16 h @+2`, pixel base `payload+4`, `w*h` count
   (stored to `DAT_004eda88/90/94/98`). `PLANET` resolves through the
   same reader next (out of Phase 4B scope).
6. `FUN_0042c8b0` (the stream frame function) uploads the composed
   table through `FUN_00413b40`/`FUN_0046d208` — the same
   RGB-triplet → `PALETTEENTRY` → `IDirectDrawPalette::SetEntries`
   path Phase 4A proved (file order **R,G,B**).
7. `FUN_0042e684` blits `BG` into the 600x360 work surface
   `DAT_00541650` with per-axis wrap scroll (offsets mod 600/360 —
   stream camera state); stride 600 both sides, rows top-down, and
   **no transparency** — every destination byte is overwritten. At
   scroll = 0 it is a verbatim 600x360 copy.

Byte-level corroboration (BUILD_A): `PAL` span = 768; `PAL[0:192]`
equals `SYS_PAL` except entry 0 (the record carries `ff00ff` where
the loaded system palette is forced black — and the consumer never
reads `PAL[0:192]` anyway); `BG` = 4 + 600*360 exactly, pixel indices
1-255 (index 0 is never used; ~42k pixels use the system-range
entries 1-63, so the SYS_PAL head is load-bearing).

## Palette transformations

- **Stable image: none.** At fade factor `DAT_004eda9c == 1.0`,
  `FUN_0042c8b0` calls `FUN_00413b40(DAT_004ed758)`, which uploads
  entries 0-63 from `DAT_00540820` and 64-255 from the argument's
  +0xc0 — the stored bytes unmodified.
- **Fade transitions:** `DAT_004eda9c != 1.0` scales or saturates a
  stack copy of the composed table (fade-from/to-black and brighten),
  uploaded via `FUN_0046d208`. Animation-only; not needed for the
  stable display.
- **Brightness:** `FUN_0046d208` adds `DAT_0054147e * 16` per channel
  with clamp — the user brightness setting (default 0 = straight
  copy). A user-setting path, same as Phase 4A documented.
- **Runtime byte order:** `FUN_0046d208` stores a BGRX runtime table
  at `DAT_0054d7b8` and an RGB `PALETTEENTRY` array for `SetEntries`;
  source bytes are R,G,B triplets.

## Native decode architecture

```
STREAM/STREAM.BNI + MISC/MDKFONT.FTI (read-only, DataRoot)
  -> isStreamBackdropRequest    (new: the ONE proven context —
                                 STREAM/STREAM.BNI + record BG)
  -> inspectBniDirectory + findBniRecord("BG"/"PAL")   (Phase 3H/4A)
  -> inspectFtiDirectory + findFtiRecord("SYS_PAL")    (Phase 3H +
                                 new bounded lookup helper)
  -> composeStreamPalette       (new: effective 768-byte table =
                                 SYS_PAL[0:192] + PAL[0xc0:0x300] —
                                 mirrors the FUN_0042b270 copies)
  -> decodeBniIndexedImage      (new: validates 4 + w*h exactly and
                                 palette == 768 bytes; pixels and all
                                 256 RGB entries copied verbatim)
  -> IndexedImage (hasPalette)  -> blitIndexedImage -> framebuffer
                                 -> Metal presenter   (unchanged)
```

`decodeBniIndexedImage` (`src/core/bni_image.*`) is pure spans — it
never invents a palette. `src/core/stream_context.*` carries the
proven binding (record names, `PAL+0xc0`, the SYS_PAL head) and the
whole-file resolution `decodeStreamBackdrop(bniFile, ftiFile)`;
DataRoot/filesystem access stays in the app/inspect layer. There is
no generic "find PAL" behavior — requesting any other indexed-only
record (e.g. `STREAM/STREAM.BNI PLANET`) fails with an explicit
no-proven-binding error.

## Preview command

```sh
build/native/mdk-native.app/Contents/MacOS/mdk-native \
  --data-path original/installed \
  --preview-resource STREAM/STREAM.BNI BG
```

The palette is resolved automatically because the context is the
proven one — no palette flag is needed or offered. Result digest:
`662bf1e20bdd351c` (`mdk-inspect --visual-info` reports the same).
`--dump-ppm` dumps the presented frame as before.

Agent-side visual verification of `/tmp/mdk-phase4b.ppm`: a coherent
deep-space backdrop — purple/blue nebula clouds with bright star
glints — correct orientation, no stride corruption, plausible
palette. (Not a human gate.)

## Explicit non-goals (Phase 4B)

- No animation: `SWH150`/`KURT`/`BONES`/`PLANET`/`WIND` etc. are
  resolved by the same init but are NOT decoded or drawn.
- No stream scroll emulation: `FUN_0042e684`'s wrap offsets are
  presented at 0 (the verbatim-blit state).
- No fade/brightness emulation — transition/user-setting paths only.
- No second palette context: FALL3D, STATS etc. remain unproven; the
  API does not pretend otherwise.
- No generic palette database, asset registry, or PAL-by-name
  heuristic — the binding is context-explicit.
- Phase 4A semantics untouched: `MDKOPT` still decodes embedded-
  palette-first with digest `6017f4c4bd57c479`.

## Remaining indexed-BNI unknowns

- FALL3D palette binding: `SPACEPAL` + `FALLP1..5` records — which
  image uses which, and whether the binding is static or animated
  (palette cycling is plausible — `FALLP*` look like ramp variants).
- Whether other indexed-only records (TRAVSPRT `SKULL`, FINISH.BNI
  contents) share the stream-style "system head + record tail"
  composition or use full external palettes.
- `DAT_004edad0`/fade-start semantics: the stream fades in from
  black (`DAT_004eda9c` starts at 0); exact ramp timing unmeasured.
- PLANET's draw path (`FUN_0042e55c`) and the overlay records'
  transparency keys — not needed for the backdrop.

## Phase 4C candidate directions

Grounded in 4A+4B:

1. **FTI font/glyph decode** — `FONTSML`/`FONTBIG` per-glyph offset
   tables + glyph headers; renders the font sheet without text
   layout. The SYS_PAL/FTI resolution seam from 4B is reusable.
2. **`ARROW` RLE cursor decode** — the 0xfe escape-stream sprite;
   completes the options-screen ingredient list.
3. **First reconstructed front-end composition** — options screen =
   MDKOPT backdrop + text overlay + cursor, once (1)/(2) land.
4. **FALL3D context palette** — only worth doing if it proves a
   genuinely different binding mechanism (multi-record palette
   animation) rather than repeating the stream pattern.

# Phase 4C — FTI font/glyph decode

Phase 4C added the third vertical slice: one original FTI font record
decoded to individual glyphs and presented through the indexed
framebuffer — the first engine capability that is a *service* (a
glyph table consumed by byte strings) rather than a whole-screen
image.

## Selected resource

**`FONTSML` in `MISC/MDKFONT.FTI`** — the general-purpose UI text font
(span `[0x18d90, 0x2555b)`, 51,147 bytes, 204 mapped glyphs).

### Why this resource (candidate comparison)

| Candidate | Verdict |
|---|---|
| `FONTSML` | **Selected** — every required property is CODE-CORROBORATED through its own consumer chain (`FUN_00414d88` measure / `FUN_00414dd4` draw, ~40 static call sites incl. options screens), and all 204 glyph extents tile the payload exactly to EOF. |
| `FONTBIG` | Same glyph format, independently proven through `FUN_00414be8`/`FUN_00414c34`/`FUN_00414f64` — 152 mapped glyphs, 2 trailing pad bytes. The shared decoder handles it, but one font satisfied the phase rule; kept as a validated second decode (`--preview-font`/`--font-info` accept it). |
| `F8` | A different format entirely — `FUN_00414a08` reads `font + ch*8` (8×8 1bpp rows, MSB-first) and writes a *caller-supplied* color index for set bits. Not needed; deferred. |

## Original functions traced (statics, MDK95.EXE)

| Address | Role |
|---|---|
| `FUN_004149c4` | Font resolver: FTI lookups `F8` → `DAT_0054164c`, `FONTSML` → `DAT_00541644`, `FONTBIG` → `DAT_00541648` (name bytes confirmed in the EXE at `0x4950dc`–`0x4950ee`); sets the `DAT_0049a76c` initialized flag consumed by the `"Font table not initialized!"` diagnostic path |
| `FUN_00414d88` | FONTSML width measure: per byte `x += glyph[2]`, missing entry `x += 4` |
| `FUN_00414dd4` | FONTSML 1:1 draw (below) — plus a single-char trailing marker via `FUN_00414b28` (uses `glyph[1]` as a position offset; selection-marker, not decoded here) |
| `FUN_00414f1c` | FONTSML centered draw — `x = 300 - measure/2` |
| `FUN_00414be8` | FONTBIG width measure: per byte `x += glyph[2]`, missing `x += 6` |
| `FUN_00414c34` | FONTBIG 1:1 draw — identical glyph format/blit semantics |
| `FUN_00414f64` | FONTBIG scaled draw — same `{s8,s8,u8,px}` header fields read the same way under 16.16 subpixel stepping |
| `FUN_00414a08`/`FUN_00414ac0` | F8 mask draw — the 1bpp format, deferred |
| `FUN_00423a24` | Timing/fade factor feeding the scaled path (`FUN_00423b38`) — not part of glyph decode |
| `FUN_00423b38` | Options-scale text: `FUN_00423a24` → `FUN_00414be8` → `FUN_00414f64` |
| `FUN_0040163c` | `SYS_PAL` consumer: copies the 192-byte record into the resident palette head `DAT_00540820` and forces entry 0 black |

## Proven record format (CODE-CORROBORATED + byte-exact)

```
record+0x000  u32 glyphOffset[256]   indexed DIRECTLY by the input
              byte (MOVZX char -> *4 — no ASCII subtraction, no case
              fold, no code-page map). 0 = no glyph for that byte.
record+off    glyph:
  +0 s8   top     bitmap rows whose last is the pen row: bitmap row 0
                  draws at fb row (penY - top)
  +1 s8   bottom  bitmap rows below the pen row (negative allowed:
                  '!' keeps its whole body on/above the pen)
  +2 u8   width   row width AND horizontal advance
  +3 u8   pixels[width * (top + bottom + 1)]
                  row-major, top row first; byte 0 = skip
                  (transparent), nonzero = final palette index
                  written verbatim — no mask, no caller color
```

Byte-exact corroboration (FONTSML): glyph `'!'` at `+0x4e79` reads
`0c ff 04` → top 12, bottom -1, width 4, 12×4 bitmap; `offset[ch]` for
ch=1 is `0x400` so the table is exactly 256 entries; all 204 glyphs
tile to EOF with zero slack. FONTBIG: `'!'` at `+0x400` reads
`19 ff 09` → 25×9; all 152 tile except 2 pad bytes at EOF.

## Draw semantics (proven)

`FUN_00414dd4`/`FUN_00414c34` inner loop:

```
dst = fb + penX + (penY - top)*600
for r in 0..rows-1, c in 0..width-1:
    b = px[r*width + c];  if (b != 0) dst[c] = b
    dst += 600 per row
penX += width                       // glyph advance
unmapped byte: penX += 4 (FONTSML) // 6 (FONTBIG)   // constants in
                                                   // the draw code
```

The original performs NO bounds clipping — callers keep text on-screen.
The native helpers clip per-pixel (hardening only; identical output for
in-bounds pens).

## Color / transparency rule

- Byte `0` in the bitmap = transparent (the original literally skips
  the store). All nonzero bytes are final 8-bit palette indices.
- No foreground-color argument exists on this path (unlike F8, which
  colors a mask). The glyphs shade themselves — FONTSML uses indices
  `{5,10,16,32..59}` (gray ramp 16–47 + red→yellow ramp 48–59 of
  SYS_PAL); max index used: FONTSML 59, FONTBIG 62 — both inside the
  resident 64-entry SYS_PAL head (`FUN_0040163c`). **CORROBORATED**
  binding; the preview binds `SYS_PAL[0:64]` from the same FTI file
  and leaves 64–255 zeroed (never referenced).
- The glyph encoding is an MDK-specific map, NOT ASCII/CP437: codes
  1–31 are keycap legend glyphs (`Esc`, `Tab`, `F1`–`F12`, `Num Lock`
  …), 32 is unmapped (space = advance-only), 33+ carry
  printable/digit/letter glyphs, high codes carry accented and
  symbol glyphs.

## Native decode architecture

```
--preview-font MISC/MDKFONT.FTI FONTSML
  -> DataRoot.readFile            (read-only)
  -> inspectFtiDirectory          (Phase 3H, unchanged)
  -> findFtiRecord("FONTSML")     -> record span [0x18d90, 0x2555b)
  -> decodeFtiFont(payload)       (new src/core/fti_font.*)
       256-entry table walk, per-glyph extent check
       {top,bottom,width,rows*w px} — bounds-checked, verbatim copy
  -> findFtiRecord("SYS_PAL")     -> palette entries 0–63
  -> atlas or text draw           (drawFtiGlyph/drawFtiText —
       mirrors FUN_00414dd4: row 0 at penY-top, 0-skip, adv=width,
       missing +4 for FONTSML / +6 for FONTBIG)
  -> IndexedFramebuffer (600x360) + Palette -> Metal presenter
```

`decodeFtiFont` accepts only the record span — no filesystem or FTI
directory knowledge. Malformed inputs (short table, offset inside the
table, offset past the payload, truncated header, bitmap overrun,
zero mapped glyphs) return a structured failure. Trailing slack after
the last glyph is tolerated and reported (`trailingBytes`).

## Preview command

```sh
build/native/mdk-native.app/Contents/MacOS/mdk-native \
  --data-path original/installed \
  --preview-font MISC/MDKFONT.FTI FONTSML            # atlas
build/native/mdk-native.app/Contents/MacOS/mdk-native \
  --data-path original/installed \
  --preview-font MISC/MDKFONT.FTI FONTSML "MDK 1997" # text
```

Atlas mode draws every mapped glyph in table order on a diagnostic
grid (presentation-only layout; the per-glyph rule is the proven one).
Text mode draws one byte string using the proven advance rule —
advance = glyph width, unmapped bytes advance 4 (FONTSML path) or 6
(FONTBIG path). Font digest: `c7956b0fea14f2ac` (FONTSML),
`99681a15ee8479f5` (FONTBIG) — `mdk-inspect --font-info` reports the
same.

`mdk-inspect --data-path DIR --font-info FILE RECORD [CODE]` prints
metadata only (layout, mapping rule, mapped count, offset order,
encoding, per-code glyph metrics, digest) — no payload bytes.

Agent-side visual verification of `/tmp/mdk-phase4c.ppm` (atlas):
204 recognizable glyphs — keycap legends, full alphanumerics, accented
set — upright, correctly oriented, zero-transparent over black, gold
SYS_PAL ramp shading; `/tmp/mdk-phase4c-text.ppm` (text): "MDK
Transplant" rendered with correct spacing and descender placement.
(Not a human gate.)

## Explicit non-goals (Phase 4C)

- No text-layout subsystem: no menus, labels, selection state,
  wrapping, alignment, or the `FUN_00414b28` selection marker.
- `F8` (1bpp mask font) and `ARROW` are not decoded.
- The scaled FONTBIG path (`FUN_00414f64`) is evidence for the shared
  glyph format only — no scaled rendering is implemented.
- No localization: `FONTF/I/P/S.FTI` variants share the format
  (verified structurally) but are not wired to a language switch.
- STREAM's palette composition is NOT generalized — fonts bind the
  resident SYS_PAL head by the font path's own evidence.
- Phase 4A/4B semantics untouched: `MDKOPT` digest
  `6017f4c4bd57c479`, STREAM `BG` digest `662bf1e20bdd351c`.

## Remaining font unknowns

- Whether the three trailing-marker calls in `FUN_00414dd4`'s tail
  (selection bracket via `FUN_00414b28`) draw additional decoration in
  some call contexts — glyph decode is unaffected.
- `FONTBIG`'s 2-byte EOF pad: consistent with align4 payload packing;
  semantically dead.
- Whether any consumer reads a glyph's `pixels` for non-draw purposes
  (e.g. hit-testing) — none observed.
- The `F8` mask font's callers' color values — format proven, color
  provenance per call site not traced (deferred).

# Phase 4D — ARROW decode + first static front-end composition

Phase 4D decoded the `ARROW` sprite resource and composed the first
evidence-backed front-end frame from original resources using the
original draw rules, coordinates, palette, and composition order.

## The composition target (CORRECTED assumption)

Two related UI paths were distinguished:

- **Front-end root menu — `FUN_0041dc90`** (dispatched when
  `DAT_00541493==0 && DAT_00541492!=0`): blits the resolved `MDKOPT`
  pixel payload (`_DAT_0054bca0`, set by `FUN_0041d7b4`), draws the
  `OPT0..OPT4` item labels, then draws `ARROW` at the raw mouse
  position. **This is the only `MDKOPT`-backed screen — it is the
  Phase 4D target.**
- **Options sub-menu — `FUN_00420eac`** (state `0x0b`): memsets the
  framebuffer to 0 and draws the `OM_*` labels + ARROW on black. It
  does NOT blit `MDKOPT` in its redraw path.

## ARROW consumer chain (OBSERVED, instruction-level)

```
FUN_004236c0(x, y)         front-end cursor-arrow draw
  EAX = FUN_00414890("ARROW")   FTI lookup -> file+4+dirOffset
  DAT_0049ac78 = content + 4    cached table base P
  sprite = P + u32@(P+4)        frameOffsets[0]
  FUN_00409760(x, y, sprite)
FUN_00409760               generic sprite header (several call sites)
  w=u16@+0  h=u16@+2  hx=s16@+4  hy=s16@+6
  FUN_00415ff0(x-hx, y-hy, &{w,h}, sprite+8)
FUN_00415ff0               command-stream blit into DAT_00541650
```

## ARROW record format (CODE-CORROBORATED + byte-exact)

`MISC/MDKFONT.FTI` record `ARROW`, 96-byte content at file `0xecc`:

```
+0x00 u32 blockBytes   91 (4 count + 4 offset + 8 hdr + 75 stream);
                        not read by the draw path — metadata
+0x04 u32 frameCount   1
+0x08 u32 offset[]     each relative to +0x04; offsets[0] = 8
frame:  u16 w, u16 h, s16 hotX, s16 hotY, stream   (ARROW: 8x17, 0,0)
```

## Stream commands (FUN_00415ff0, OBSERVED)

| cmd | meaning |
|-----|---------|
| `0x00-0x7f` | literal packet: `cmd+1` pixel bytes follow; each byte is a final palette index — nonzero overwrites the destination, byte 0 is skipped (transparent). Dest advances 1 per byte. |
| `0x80-0xfd` | run packet: `count = cmd - 0x7c` (4..129), one value byte follows; value 0 advances the dest by `count` without writing (transparent run), nonzero writes `count` copies. |
| `0xfe` | row break: next row down; the column resets to the sprite x. If the row reaches 360 the draw returns. |
| `0xff` | end of stream. |

Original clipping (reproduced): `x>=600 || y>=360 || x+w<=0 || y+h<=0`
draws nothing; `x>=0 && x+w>600` draws nothing (right edge is
all-or-nothing — no per-pixel right clip); `y<0` consumes commands
without writes until the row counter reaches 0 (top clip); `x<0`
skips the left-of-zero part of each packet (left clip). Packets may
spill past the declared row width into the next row — the stream is
trusted (the destination pointer simply walks). Native hardening:
decode requires a `0xff` inside the record span; the blitter bounds
every write to the framebuffer. Behavior-identical on well-formed
data.

Color/transparency (CORROBORATED): stream bytes are final palette
indices — no caller color, no mask; byte 0 is the only transparency
mechanism (skips the destination write). ARROW uses only index 1 —
white under both the MDKOPT palette head and SYS_PAL.

## Decoded ARROW (real record)

8x17 left-pointing arrow, hotspot (0,0), 75-byte stream: 8 literal +
12 run packets (3 transparent runs), 16 row breaks, 91 pixel
advances, 72 opaque writes, max index 1, digest `672fff8c63fa8f4a`.
One pad byte follows the stream (reported as trailing slack).

## Selected static state (entry-state register values, OBSERVED)

`FUN_0041d85c` ("enter front-end") + `FUN_00418798` (one-time mouse
reset, called at startup before the front-end) establish:

- `DAT_0054bc98 = 1` — `FUN_00428290` finds `SAVES/*.SAV`; BUILD_A
  has `1.SAV` + `2.SAV` -> the five-item menu.
- `DAT_0049aa78 = 0` — selection = item 0 ("Continue").
- `DAT_0049aaa0 = 0`, `DAT_0049aa8c = 0` — no override/blend
  backdrop; `DAT_0049aa98 = 0` — item list = OPT0..OPT4.
- `DAT_0054b634/38 = (300,180)` — mouse reset position; the
  mouse-moved flag `DAT_0054b644` gates the `(mouseY-5)/36`
  hit-test, so without input the selection stays 0. The arrow is
  drawn at the raw mouse position (not beside the selection).

## Composition (all OBSERVED)

1. `memcpy(fb, MDKOPT+0x304, 0x34bc0)` — the 600x360 pixels verbatim.
2. Items via `FUN_00423b38(selected, x_arg, y_i, text)`:
   `y_i = 31 + 36*i` (0x1f + 0x24i); `x_arg = maxW/2` INTEGER
   signed division (SAR pattern) where `maxW` = largest UNSCALED
   `FUN_00414be8` measure over the drawn items; `scale = 1.0`
   selected / `0.65f` unselected (`FUN_00423a24` ramp endpoints);
   `finalX = trunc(x_arg - measure*scale*0.5)` — x87 truncation
   toward zero (`FUN_0047d59a` sets RC=11); draw via
   `FUN_00414f64` (FONTBIG scaled; marker flag 0).
3. `FUN_004236c0(mouseX, mouseY)` — ARROW (hotspot 0,0).
4. Palette: MDKOPT embedded 256-entry palette (`FUN_00413b40`
   uploads head 64 + tail 192; `FUN_00416700` fades toward the same
   endpoint — frozen).

Strings (OBSERVED — the OPTi payloads are the NUL-terminated labels):
OPT0 "Continue", OPT1 "New Game", OPT2 "Saved Game", OPT3 "Options",
OPT4 "Quit". (No-saves branch: OPT1..OPT4 at y 31,67,103,139,
selection 1.)

`FUN_00414f64` scaled draw (OBSERVED, implemented as
`drawFtiTextScaled`): `scale<=0.05` draws nothing; `scale==1.0` is
the 1:1 path; otherwise 16.16 fixed-point source sampling —
`srcStep = trunc(65536/scale)`, `glyphTopY = trunc(y - top*scale)`,
`srcRow0 = (top<<16) - (y-glyphTopY)*srcStep`, missing-glyph pen
advance `trunc(pen + 6*scale)`, mapped advance `trunc(pen + w*scale)`.

## Digests and verification

- `MDKOPT` `6017f4c4bd57c479` — Phase 4A regression intact.
- `STREAM BG` `662bf1e20bdd351c` — Phase 4B regression intact.
- `FONTSML` `c7956b0fea14f2ac`, `FONTBIG` `99681a15ee8479f5` —
  Phase 4C intact.
- `ARROW` `672fff8c63fa8f4a` (decoded sprite: block size + frame
  header + stream bytes).
- Composed frame `debd84b7f6e158dc` + palette `6a3cbda3822c5525`
  (fnv1a64 over the 600x360 indexed pixels / expanded palette).
- Agent-side PPM check `/tmp/mdk-phase4d-options.ppm`: the real MDK
  front-end frame — MDKOPT art, five staggered left-side labels with
  "Continue" at full scale and the rest at 0.65, the white 8x17
  arrow at (300,180) pixel-exact. Not a human gate.

## Native architecture

```
--preview-options
  -> OPTIONS.BNI/MDKOPT  decodeBniPalettedImage  (Phase 4A)
  -> MDKFONT.FTI/FONTBIG decodeFtiFont           (Phase 4C)
  -> MDKFONT.FTI/ARROW   decodeFtiSprite         (Phase 4D)
  -> MDKFONT.FTI/OPT0..4 verbatim C strings
  -> SAVES/*.SAV presence (FUN_00428290 equivalent)
  -> renderFrontendMenuFrame (src/core/frontend_menu.*):
       backdrop memcpy -> items (centered scaled FONTBIG) -> ARROW
  -> IndexedFramebuffer + Palette -> Metal presenter
```

`--preview-sprite FILE RECORD` draws one decoded FTI sprite over a
diagnostic checkerboard (QA only — not part of the frame).
`mdk-inspect --sprite-info FILE RECORD` reports metadata only
(layout, header, per-frame dims/hotspot/stream stats, digest) — no
payload bytes.

## Explicit non-goals (Phase 4D)

- No input-driven selection, cursor movement, option mutation,
  submenus, saving, audio, transitions, fades, blinking, or
  animation. The frame is a frozen stable state.
- No `FUN_00414b28` selection-marker context (the front-end path
  passes marker flag 0 — the marker is not the ARROW; both are
  traced, only the arrow is used here).
- The options sub-menu (`FUN_00420eac`, OM_* on cleared buffer) is
  traced but not composed — it shares the same primitives.
- `ARROW` sibling sprite records (other FUN_00409760 callers) share
  the proven format but are not wired to consumers yet.

## Phase 4E candidate directions

1. **First interactive front-end state** — keyboard/mouse selection
   between the five OPT items: selection index transitions
   (`DAT_0049aa78`), the `(mouseY-5)/36` hit-test
   (`DAT_0054b644`-gated), the 0.65->1.0 scale ramp
   (`FUN_00423a24` accumulator), arrow following the mouse, and the
   item-action dispatch (0x41de77..). Sounds still deferred.
2. **The options sub-menu frame** — `FUN_00420eac` static variant
   (cleared buffer + OM_* labels + arrow) as a second proven
   composition target.
3. **`F8` color provenance** — the 1bpp mask font's caller colors.

# Phase 4E — interactive front-end root menu

Phase 4E reconstructs the interactive state of `FUN_0041dc90` — the
front-end root menu's selection, keyboard/mouse input, hit-test,
scale ramp, and activation dispatch — and drives the Phase 4D
composition with it. Everything below is OBSERVED at instruction
level in `MDK95.EXE` (BUILD_A); private disassembly/decompile notes
live in `analysis-private/logs/phase4e-evidence.md`.

## Frame protocol (main loop `FUN_0040103c`, OBSERVED)

Each frame runs, in order:

1. `FUN_004187e0` — input poll: keyboard poll `FUN_00419370` builds
   the logical key bitmap, then `FUN_0046bc18` returns per-frame
   mouse deltas and the packed 4-button nibble.
2. `DAT_00541518 += DAT_0049b6e8` — tick advances by the frame step.
3. `FUN_0041dc90` — the mode handler: input queries, selection
   updates, idle timer, then the draw pass (labels + arrow) which
   lazily drives the scale ramp.
4. `FUN_0042fb68`/`FUN_0042fcd0` — frame timing update.

`FrontendMenuController::update` covers steps 2+3's input half,
`itemScale` is the draw-pass ramp query, `endFrame` is step 4.

## State (OBSERVED globals → controller fields)

| Original | Meaning |
|---|---|
| `DAT_0049aa78` | selection index 0..4 (entry: `!savesExist` → 0 with saves, 1 without) |
| `DAT_0054bc98` | saves-exist flag (FUN_00428290 SAVES/*.SAV probe) |
| `DAT_0054b634`/`b638` | logical mouse x/y — reset (300,180) by FUN_00418798 |
| `DAT_0054b644`/`b648` | per-frame mouse dx/dy (zero → accumulate skipped) |
| `DAT_0054b64c` | dz (wheel-like) — read by the gate, unused otherwise |
| `DAT_0054b640` | packed 4-button nibble (bit i = button i+1 held) |
| `DAT_0054b568`/`b56c` | UP / DOWN held level (keymap bits 103/108) |
| `DAT_0054b574` | Enter press edge (keymap bit 28 new-press) |
| `DAT_0054b554` | RIGHT press edge — attract trigger |
| `DAT_0054b570` | Esc edge — handled by the main loop, not the menu |
| `DAT_00541518` | tick counter (key-repeat clock) |
| `DAT_0049b6e8` | frame step 1..4 added to the tick each frame |
| `DAT_0049aaa4` | idle/attract timer (seconds; reset on selection change) |
| `DAT_0049ac84`/`ac88` | prev/next repeat deadlines |
| `DAT_0049ac80` | mouse-button edge latch (re-arms when nibble==0) |
| `DAT_0054bdc8`..`bdd8` | scale-ramp machine: cur key, prev key, acc |
| `DAT_0049aa98` | item-list state (0 = stable root list) |

## Keyboard navigation (OBSERVED)

UP (`prev`) and DOWN (`next`) are queried through `FUN_004237b4` /
`FUN_00423838` — identical bodies with per-key deadline state:

```
fired = held && tick > deadline
if (!held)                    deadline = 0
else if (deadline == 0)       deadline = tick + 30   // first delay
else if (tick > deadline)     deadline = tick + 3    // repeat period
else if (tick + 100 < deadline) deadline = 0         // anomalous reset
if (fired) FUN_00423734()  // SND_PUSH blip — audio deferred
```

So a press fires immediately, repeats first at tick+31, then every
~4 ticks (≈133 ms at the paced ~30 fps regime). On fire:

- UP: `sel--; if (sel<0 || (sel==0 && !saves)) sel = 4`
- DOWN: `sel++; if (sel>=5) sel = saves ? 0 : 1`

Both reset `DAT_0049aaa4` to 0 (999.0 when list state is 1 — a
transition state unreachable in the stable list). prev runs before
next within one frame — simultaneous UP+DOWN resolves prev-then-next.

## Mouse update + hit-test (OBSERVED)

`FUN_004187e0` accumulates raw deltas with no sensitivity scaling and
clamps to the work surface `x∈[0,599]`, `y∈[0,359]` — zero deltas
skip the accumulate entirely.

Inside `FUN_0041dc90` the hit-test block is gated on
`dx | dy | buttons` — any mouse input this frame. Inside the gate a
**second, tighter clamp** applies to the persistent position:
`x≤590` (`0x24e`), `y≤350` (`0x15e`). Then:

```
band = trunc((mouseY - 5) / 36)     // x86 IDIV, toward zero
if (!saves) band += 1               // hidden OPT0 keeps index 0
if (band in [saves?0:1, 4] && band != sel) { sel = band; idle = 0 }
```

x is never consulted. Valid bands (saves): y∈[-30,184] → 0..4 since
negative offsets > -36 truncate to 0; y≥185 → band≥5 invalid.
No-saves shifts computed bands to indices 1..4. No mouse input →
gate closed → resting position never selects (OBSERVED: mouse rests
at (300,180), band 4, yet entry selection is 0).

## Activation (OBSERVED)

`FUN_00423764`: fires on Enter edge (`DAT_0054b574`) OR on
`latch && buttons != 0` — a button **down-edge** for any of the four
buttons. The latch (`DAT_0049ac80`) re-arms only when the nibble is
0. Held buttons do not refire. Enter ignores the latch and fires
regardless of button state. Hit-test runs before the activation
query, so a click both selects and activates in the same frame.

Dispatch (`0x41de77` branch block) — emitted as semantic
`FrontendAction` events only:

| sel | saves | action | original target |
|---|---|---|---|
| 0 | yes | `ContinueGame` | save-load path (FUN_00415658) |
| 0 | no  | `Quit` | unreachable guard — shares the quit branch |
| 1 | — | `NewGame` | FUN_0041dbd4 + FUN_0041b630 |
| 2 | — | `SavedGame` | FUN_0041dbd4 + FUN_004202cc |
| 3 | — | `OpenOptions` | FUN_00420cf0 — sub-menu is Phase 4F |
| 4 | — | `Quit` | `DAT_0054148e = 1` + FUN_0041dbd4 |

RIGHT edge (`DAT_0054b554`) with list state ≥ 0 forces the attract
trigger (`EnterAttract`) — the slideshow path (FUN_0041ef74) itself
is deferred. An activation dispatched earlier in the same frame
already leaves the menu, so activation wins.

## Scale ramp (FUN_00423a24, OBSERVED)

One machine (`DAT_0054bdc8`..`bdd8`) serves all items, keyed by the
item's `(centerX, itemY)` pair — the draw call's identity:

```
if (selFlag):
  if key != cur: prev = cur; acc = 0; cur = key   // selection moved
  else:          acc += DAT_0049b6f0              // once per frame
return cur  : acc>=5 ? 1.0  : 0.65 + acc*0.07
       prev : acc>=5 ? 0.65 : 1.0  - acc*0.07
       other: 0.65
```

`0.07 = 0.35 × 0.2` (d[0x49601c]×d[0x496024]); limit 5.0
(d[0x496018]). `DAT_0049b6f0` is the smoothed frame-unit value
(EMA ≈1.0 at the paced regime), so acc advances ≈1/frame → ~5
frames ≈165 ms per transition, framerate-independent. Quirk
(reproduced): a mid-ramp reversal makes the interrupted item the
`prev` key, whose formula assumes a completed 1.0 — it snaps UP to
1.0 before decaying, instead of freezing mid-ramp.

## Timing (FUN_0042fcd0/FUN_0042fdc8, OBSERVED)

The raw delta is measured against a virtual clock `DAT_0049b700`
that chases real time at `rawDelta × 25/3 ms` per frame (integer-ms
domain; `25/3 = 8.3333`, d[0x4971e0]):

```
rawDelta   = (nowMs - virtualMs) * 120 / 1000   // integer
frameUnits = rawDelta * 0.25
smoothed   = smoothed*0.75 + frameUnits*0.25    // DAT_0049b6f0
deltaSec   = smoothed / 30                       // DAT_0049b6f4
stepAccum += rawDelta; step = stepAccum>>2; stepAccum &= 3
step = clamp(step, 1, 4); smoothed capped at 4.0 on clamp
tick += step                                     // DAT_0049b6e8
virtualMs += rawDelta * 25/3
```

At dtMs=100/3 (≈30 fps) rawDelta settles to 4 → step 1 → one tick
per frame, matching the observed paced regime.

## Item layout (OBSERVED — indices preserved)

- saves: items 0..4 at y = 31 + 36·i (31,67,103,139,175)
- no saves: items 1..4 at y = 31 + 36·(i−1) — OPT0 omitted visually
  but indices stay 1..4 (never compacted)

## Interactive CLI + deterministic validation

`--interactive-frontend` (requires `--data-path`) loads the shared
front-end resources (MDKOPT, FONTBIG, ARROW, OPT0..4, SAVES probe)
and runs the controller each frame: SDL input → `FrontendMenuInput`
(platform layer translates UP/DOWN/RETURN/RIGHT, integer mouse
deltas, 4-button nibble) → `update` → `renderFrontendMenuDynamic`
(draw order: backdrop → scaled labels → arrow at the logical mouse
position) → `endFrame`. Semantic actions are logged
(`frontend action: NAME (sel=N mouse=X,Y)`); `Quit` also closes the
native preview — matching the original quit-flag write.
`--preview-options` remains the static Phase 4D frame.

`--selftest` runs a deterministic script (one injected SDL event
step per frame: DOWN tap → motion (0,−41) → button down → release)
which must end at selection 3 with `OpenOptions` emitted and the
arrow at (300,139). Real device input is isolated during the script
— injected events carry a sentinel device ID; an SDL event filter
drops all real key/button/motion events and the pre-filter queue is
flushed, so physical input cannot perturb the script (observed
nondeterminism without it: real motion deltas coalesce against or
replace the injected motion). Since Phase 4F the script also feeds
the original's paced regime (`dt = 100/3 ms` per frame) to the
timing machine, so injected runs and their framebuffer digests are
deterministic across machines; the live path keeps real wall-clock
deltas, where `FUN_0042fcd0` makes the ramp pacing-dependent by
design (very fast frames truncate `rawDelta` to 0 and the ramp
crawls — faithful, not a bug).

## Digests and verification

- Dynamic snapshot `/tmp/mdk-phase4e-menu.ppm` at the end of the
  scripted sequence under the paced regime (10 frames):
  fb `cf09ecdad5b0808f`, palette `6a3cbda3822c5525`. (The digest is
  identical at 40+ frames once the ramp completes under the paced
  feed; runs driven by real wall-clock deltas are pacing-dependent
  by original design and need not reproduce it bit-for-bit.)
- Static `--preview-options` unchanged: `debd84b7f6e158dc` /
  `6a3cbda3822c5525`; all Phase 4A–4D digests intact.
- Agent-side PPM check: Options enlarged to 1.0, other four items
  at 0.65, arrow at the logical mouse position over the Options
  row, backdrop/palette unchanged.
- Controller unit tests: entry state, keyboard walk/wrap both
  branches, repeat schedule, accumulate + both clamps, hit-test
  boundaries both branches, activation latch/edge semantics,
  per-item dispatch, attract priority, idle timer, tick advance,
  ramp growth/decay/reversal, dynamic render contract.
- Interactive selftest: 15/15 deterministic PASS.

## Explicit non-goals (Phase 4E)

- No gameplay, level launch, save load/parse, or FALL3D boot.
- No options sub-menu (`FUN_00420eac` — Phase 4F target), no
  settings mutation.
- No audio (SND_PUSH push-sound and confirm sounds documented,
  deferred).
- No attract slideshow (`FUN_0041ef74`) — the RIGHT-edge trigger is
  emitted as `EnterAttract` only.
- No Esc/cancel binding inside the controller (the original menu
  has none; Esc is a main-loop concern).

# Phase 4F — interactive options sub-menu

Phase 4F connects the root menu's `OpenOptions` dispatch to the real
options sub-menu `FUN_00420eac` (front-end mode `0x0b`) and
reconstructs its visual and interactive state: the black-background
`OM_*` list, navigation, mouse hit-test, scale ramp, and semantic
action dispatch — plus the proven return to the root menu
(`FUN_00420d68`). The skill-row mutation (`DAT_0054147a` ±1 with wrap
+ the `DAT_00541486` dirty latch) is implemented here; the MDK.CFG
persistence it gates landed in Phase 4G. Everything below is OBSERVED
at
instruction level in `MDK95.EXE` (BUILD_A) unless marked otherwise;
private notes live in `analysis-private/logs/` (`decomp_20eac.txt`,
`disasm_20eac_dispatch.txt` — full instruction trace + the three
dispatch tables, `disasm_optitems.txt`, the `FUN_00420cf0`/
`FUN_00420d68` disassemblies, `decomp_25de4.txt`, `decomp_4260ac.txt`).

## Root → options transition (`FUN_00420cf0`, OBSERVED)

The root selection-3 dispatch (`CALL 0x00420cf0` at `0x41de77`,
followed by RET — see the dispatch-frame note below) runs:

1. `DAT_00541493 = 0x0b` — front-end mode becomes the options
   sub-menu handler `FUN_00420eac`.
2. `_DAT_0054bd34 = 8` — options selection starts at row 8 (OM_QUIT).
3. `FUN_0046d614(svlut)` — flattens the active (MDKOPT) palette into
   the 768-byte scratch buffer `DAT_0049ac60`.
4. `FUN_0046d208(0, 0x100, DAT_00540820)` — uploads the resident
   system-palette array.

No mouse, tick, repeat-deadline, button-latch, ramp, or timing state
is reset — the shared globals continue on the new screen. The two
menu handlers literally run over the same global block, so the
native `FrontendMachineState` is serialized verbatim between the
root and options controllers (`frontend_machines.h`).

## Options → root return (`FUN_00420d68`, OBSERVED)

Activating row 8 or pressing Esc runs `FUN_00420d68`
(`disasm_20d68.txt`):

1. `DAT_00541493 = 0` — mode back to the root handler (written first,
   `0x420d77`).
2. `TEST DAT_00541486` — the settings-dirty gate (`0x420d7d`): when
   set, `FUN_004260ac` rewrites MDK.CFG (`0x420db7`) and the flag is
   cleared (`0x420dbc`); when clear, no write happens at all.
   `FUN_004260ac` (`decomp_4260ac.txt`) opens the command-line config
   path else `C:\MDK.CFG` else `MDK.CFG`, emits the `; MDK
   Configuration file automatically generated by MDK` header, then
   walks the 92-entry `{name,type,value_ptr}` table at `0x49aca8`
   writing `name = value` lines **only for values that differ from
   the factory-defaults mirror** at `0x49afe4`. The skill entry (88:
   `"Skill"`, int `%d`, `&DAT_0054147a`, default 1) is therefore
   persisted iff `skill != 1` — the Phase 4G native seam implements
   exactly this delta contract.
3. `DAT_00541492` mode checks (`!=0 → FUN_00402590`, `==3 →
   FUN_004348d4`) — re-enter the front-end list path.
4. `FUN_0046ca84` + `FUN_0046d208(0, 0x100, svlut)` — restores the
   palette saved at entry, then releases `svlut`.

Again no input-state reset: root selection, logical mouse, ramp and
timing all carry back.

## Item table (OBSERVED — record names at `0x495ca8`..`0x495d04`)

Nine rows, drawn by `FUN_00420df8` → FTI lookup `FUN_00414890` →
`FUN_00423b88` → centered scaled text `FUN_0041518c`:

| sel | FTI record | resolved string (BUILD_A) | activation target |
|---|---|---|---|
| 0 | `OM_HELP`  | "Help"         | `FUN_0041d540` (mode 0x0a help screen) |
| 1 | `OM_SOUND` | "Sound"        | `FUN_0042322c` (mode 2 sound screen) |
| 2 | `OM_JOY`   | "Joystick"     | `FUN_0041fa24` (mode 3) — `DAT_005414f4`-gated |
| 3 | `OM_MOUSE` | "Mouse"        | `FUN_00421664` (mode 4) — `DAT_005414f4`-gated |
| 4 | `OM_KEY`   | "Keyboard"     | `FUN_0041f030` (mode 5) — `DAT_005414f4`-gated |
| 5 | `OM_PERF`  | "Performance"  | `FUN_00421e70` (mode 6 perf screen) |
| 6 | `OM_SK_0/1/2` | "Skill - Easy/Normal/Hard" | LEFT/RIGHT/Enter cycle `DAT_0054147a` ±1 (wraps) + `DAT_00541486=1` |
| 7 | `OM_DISPL` | "Display"      | `FUN_0041d020` (mode 7 display screen) |
| 8 | `OM_QUIT`  | "Quit"         | `FUN_00420d68` (leave options → root) — **activate query only** |

Row 6's record is dynamic: `==0 → OM_SK_0`, `==1 → OM_SK_1`, else →
`OM_SK_2` (the `0x421254`/`0x4212ae` 3-way branch). Canonical value is
**1** ("Skill - Normal") — corrected from the Phase 4F "canonical 0"
claim. The startup config load `FUN_00425de4` (single caller: WinMain
`FUN_00401abc` at `0x401c72`) first copies the 670-byte factory-
defaults block `DAT_0049afe4` → live block `DAT_005411f0` — whose
skill byte (`0x49b26e`, offset `0x28a`) is **1** — then applies
`name = value` lines through the same 92-entry settings table, so an
MDK.CFG `Skill = n` line would override it. BUILD_A's `MDK.CFG`
carries no `Skill` line, hence 1. `DAT_00541486` lives inside the
same copied block and starts clear.

`DAT_005414f4` (the `-mapok` command-line dev flag, canonical 0)
hides rows 2,3,4 **without compacting the index space**: keyboard
wraps 1↔5 around them, and the mouse hit-test clause
`1 < band < 5` reverts to the current selection. With the flag set,
activating a hidden index (still reachable only via carried state)
is a no-op in the original.

## Background + palette (OBSERVED)

`FUN_00420eac`'s draw block starts with `FUN_00415658` →
`FUN_0047d20a(…, 0)` — a zero-fill of the whole framebuffer to index
0. No backdrop image is drawn on this screen; MDKOPT does not leak
in. Draw order: clear → nine OM labels → ARROW → present.

The active palette is the resident system array `DAT_00540820`
uploaded at entry: entries 0–63 are the proven `SYS_PAL` head; the
tail 64–255 is filled at front-end entry by `FUN_004346e8` from the
palette record `DAT_0054c678` (resolved by `FUN_00433d40` from a
`TLEVEL`/`LEVEL%d`-family file — the exact tail bytes are
**unresolved** in BUILD_A). No drawn options pixel references
entries ≥64, so the native renderer binds `SYS_PAL[0:64]` and
zero-fills the tail — documented, not guessed. On return the saved
MDKOPT palette is restored (`FUN_0046d208` of `svlut`).

## Geometry + font (OBSERVED)

Row `i` draws at `y = 49 + 36·i` (0x31 + 0x24·i) — rows 49..337.
`FUN_0041518c` centers on the 600-wide framebuffer:
`x = trunc((600 − measure·scale) × 0.5)` — x87 truncation toward
zero; the measure is `FUN_00414be8` with missing-glyph advance 6
(FONTBIG path, marker flag 0 — same draw helper as the root menu).
Unlike the root's `maxW/2` anchor, options centering is per-row on
the framebuffer width.

## Selection + input (OBSERVED)

The selection global is `DAT_0054bd34` (entry value 8). Per frame
the handler runs, in order: prev query (`FUN_004237b4`, UP) → next
query (`FUN_00423838`, DOWN) → mouse hit-test → Esc edge check
(`DAT_0054b570`) → LEFT query (`FUN_004238bc`) → RIGHT query
(`FUN_00423940`) → activate query (`FUN_00423764`) → draw. All four
direction queries share the root's repeat machine (press fire,
deadline tick+30, repeat tick+3, staleness window +100); the
activate query shares the Enter-edge / button-latch semantics.

- UP: `sel−−`; `<0 → 8`; hidden mode `==4 → 1`.
- DOWN: `sel++`; `>=9 → 0`; hidden mode `==2 → 5`.
- Mouse (gated on `dx|dy|buttons`, in-gate clamp `x≤590,y≤350`):
  `band = trunc((mouseY − 23) / 36)` — row `i` covers
  `[23+36i, 58+36i]`; `band∈[0,8]` selects, `y≥347 → band 9`
  invalid; the `1<band<5` guard applies only in hidden mode.
  x is never consulted. (The `y−23` numerator differs from the
  root's `y−5` — proven, not assumed.)
- Esc edge: jumps to the row-8 activate case — `Back`
  (`FUN_00420d68` at `0x42101b`, `0x420fa6`).
- LEFT dispatch (table `0x420e48`, bound `cmp eax,7; ja` at
  `0x420fbe`): on row 6, `dec DAT_0054147a; <0 → 2; DAT_00541486=1`
  (`0x421085`), then falls through to the RIGHT query; on row 8 a
  silent fall-through to the RIGHT query (no dispatch — OBSERVED
  bound quirk); on rows 0–5,7 dispatch + RET.
- RIGHT dispatch (table `0x420e68`, bound `cmp eax,7; ja` at
  `0x4210b2`): on row 6, `inc DAT_0054147a; >2 → 0; dirty=1`
  (`0x421131`), then falls through to the activate query; on row 8 a
  silent fall-through to the activate query; on rows 0–5,7 dispatch +
  RET.
- Activate dispatch (table `0x420e88`, bound `cmp eax,8; ja` at
  `0x421167` — the only table that reaches row 8): on row 6, the same
  +1 cycle (`0x4211cb`) then falls through to the draw block; on rows
  0–5,7,8 dispatch + RET. A click selects (same-frame hit-test) then
  activates.
- Hidden rows 2–4 with `DAT_005414f4` set, under any of the three
  queries: `jne 0x420fd3` — a bare epilogue RET (frame ends: no draw,
  no timing update).

`DAT_00541538 != 0` would delegate the whole frame to the sound
screen — canonical 0, child screens deferred.

## Dispatch-frame semantics (OBSERVED — refined in 4F)

Every activation-dispatch branch of `FUN_00420eac` — and of the
root `FUN_0041dc90`, verified at `0x41de77`: `CALL target` then
`LEA ESP,[EBP-0x14]; POP…; RET` — returns **before** the draw
block and the `FUN_0042fe78`/`FUN_0042fb68` timing update. A
dispatched frame therefore draws nothing and does not advance the
timing machine; the previous frame persists until the next mode's
first draw. Skill cycles (fall-through to draw) and the root's
attract trigger (`FUN_0041ef74` runs, then the draw still executes)
are the exceptions. Both controllers expose this as
`frameEndedEarly()`; the application loop skips render + `endFrame`
on those frames.

## Semantic actions (`OptionsAction`, emitted only)

`None, Help, Sound, Joystick, Mouse, Keyboard, Performance,
SkillCyclePrev, SkillCycleNext, Display, Back` — the original
targets are the mode-changing child-screen entries and the skill
mutation listed in the item table. The skill mutation itself is
applied in the controller (±1 wrap + `DAT_00541486` latch, kept by
the flow across the `FUN_00420d68` return); the `FUN_004260ac`
persistence it gates is implemented in Phase 4G. The child screens
stay deferred.

## Front-end flow controller

`FrontendFlowController` (`frontend_flow.h`) owns exactly the two
reconstructed screens — `Root` (`FUN_0041dc90`) and `Options`
(`FUN_00420eac`) — routes the neutral input to the active
controller, and consumes the transition-driving actions internally:
root `OpenOptions` → `enterOptions` (`FUN_00420cf0`), options
`Back` → `returnToRoot` (`FUN_00420d68`). The shared
`FrontendMachineState` is serialized across both directions. It is
deliberately not a generic UI router.

## Interactive CLI + deterministic validation

- `--preview-options-submenu` (needs `--data-path`): static entry
  frame — clear, nine OM labels (sel 8 at 1.0, others 0.65), ARROW
  at the carried mouse, `SYS_PAL` head bound.
- `--interactive-frontend`: root `Options` now transitions into the
  live options sub-menu; Esc/Quit returns to root. Other root and
  options actions log semantic events only.
- `--frontend-root-only` (test-only): keeps `OpenOptions` deferred
  so the Phase 4E single-screen snapshot stays reproducible.
- `--selftest` injects the two-screen script (root DOWN/motion/
  click → options entry at sel 8 → DOWN×2 → motion to Display band
  → click → release → Esc → root) and now feeds the original's
  paced regime — `dt = 100/3 ms` per frame — to the timing machine,
  making injected-input runs and their digests deterministic across
  machines. The live path keeps real wall-clock deltas like the
  original (`FUN_0042fcd0` ties the ramp to real pacing: on very
  fast frames `rawDelta` truncates to 0 and the ramp crawls —
  faithful, not a bug).

## Digests and verification

- Static options preview: fb `0183fdb78c53a700`, palette
  `08e372297e745a06` (SYS_PAL head + zero tail). Changed from
  `3150a8a305ad9de8` when the canonical skill correction (0 → 1)
  redrew row 6 as "Skill - Normal".
- Dynamic options snapshot `/tmp/mdk-phase4f-options.ppm` (script
  frame 8 — sel 7 mid-ramp, Quit decaying, ARROW at 300,301):
  fb `ead555ffad0ca609`, palette `08e372297e745a06` (was
  `f00c540a40d8543d` — same reason).
- Two-screen selftest end frame (script end, back at root, sel 3
  mid-regrowth — the shared ramp's item key changed domain across
  the transition, so the root selection visibly regrows on return):
  fb `8d4eda7a8488bf02`, palette `6a3cbda3822c5525`; settled at
  40+ frames: fb `b6ffd4b319dedf51` — the saved MDKOPT palette
  restored in both.
- Phase 4E root-only regression at the paced regime:
  `cf09ecdad5b0808f` / `6a3cbda3822c5525` — reproduces exactly (the
  earlier real-time-feed variant `12226ebe0e32b479` was the same
  machine state under uncapped pacing; baseline `a1b231a` produces
  identical digests to this build at every frame count tested).
- Unit tests: options entry/walk/wrap, hidden-row skip + hidden
  hit-test guard, exact band boundaries, Esc, LEFT/RIGHT/Enter skill
  mutation + wrap + dirty latch, row-8 LEFT/RIGHT silent
  fall-through, latch semantics, ramp keyed `(−1, y)`, flow state
  carry-over both directions incl. the persistent skill global,
  static/dynamic renderer contracts.
- Interactive selftests: root-only PASS, two-screen PASS
  (entered=1 returned=1, `Display` emitted, root sel 3 restored).

## Explicit non-goals (Phase 4F)

- No other settings mutation — sound, controls, performance,
  display values never change (the Skill row's in-place mutation
  is reconstructed here; its config persistence is Phase 4G).
- No child screens — Help/Sound/Joystick/Mouse/Keyboard/
  Performance/Display sub-screens are semantic dispatches only.
- No attract slideshow, no audio (the `SND_PUSH` hook on repeat
  fires is still deferred).
- The options palette tail (`DAT_0054c678` ← level palette record)
  is unresolved in BUILD_A; the renderer binds the proven head and
  documents the gap rather than guessing.

# Phase 4G — Skill mutation + native settings persistence

Phase 4G closes the loop Phase 4F opened: the options Skill row's
mutation now flows through the **proven persistence contract** —
the dirty-gated `FUN_004260ac` rewrite — reimplemented as a small
native-owned seam that never touches the read-only data root.
Everything original below is OBSERVED at instruction level in
`MDK95.EXE` (BUILD_A); native additions are marked NATIVE PORT
DECISION / NATIVE POLICY.

## Proven persistence contract (OBSERVED)

- **Reader** `FUN_00425de4` — one caller, WinMain `FUN_00401abc`
  at `0x401c72`, so it runs once at process startup. It copies the
  670-byte factory-defaults block `DAT_0049afe4` → live block
  `DAT_005411f0` (the copy is what makes `DAT_00541486` start
  clear), then opens the resolved config and applies `name =
  value` lines through the 92-entry `{name,type,value_ptr}` table
  at `0x49aca8`. Key match is `FUN_0042fab4` — ASCII `and 0xdf`
  fold, case-insensitive.
- **Writer** `FUN_004260ac` — called from `FUN_00420d68` iff
  `DAT_00541486 != 0` (`TEST` at `0x420d7d`, call at `0x420db7`).
  It emits `; MDK Configuration file automatically generated by
  MDK` + a blank line (`0x496834`, `"\n\n"` in text mode → CRLF
  pairs on disk), then writes `name = value` per table entry
  **only when the live value differs from the defaults mirror**
  (`ptr − 0xa620c`, i.e. the same offset in `0x49afe4`). Format
  per type: int `%s = %d`, string `%s = %s`, hex `%s = 0x%X`,
  float `%s = %g`, bool `%s = TRUE`/`%s = FALSE`.
- **Settings-table entry 88** — `{name:"Skill", type:int,
  value:&DAT_0054147a}`; its defaults-mirror byte at `0x49b26e`
  is **1**. `Skill = %d` is therefore emitted iff `skill != 1`:
  Easy → `Skill = 0`, Normal → no line, Hard → `Skill = 2`.
- **Dirty flag** `DAT_00541486` — lives inside the defaults-copied
  block (starts clear), latched 1 by every row-6 mutation, read by
  `FUN_00420d68`, and cleared **unconditionally** right after the
  `FUN_004260ac` call (`MOV dword ptr [0x541486],ECX` at
  `0x420dbc` — ECX was zeroed at `0x420db5`). Write failure is
  silent: `FUN_004260ac`'s fopen-failure path RETs without
  touching the table walk.
- BUILD_A's `MDK.CFG` carries **no** `Skill` line — consistent
  with canonical skill 1: the last run left the factory value.

## Native seam (NATIVE PORT DECISION)

`src/core/frontend_settings.{h,cpp}` — deliberately narrow, Skill
only, no general settings framework:

- `FrontendSettings { int skill = 1 }` — the persisted subset
  (table entry 88). `kFrontendSkillDefault/Min/Max` = 1/0/2.
- `serializeFrontendSettings` — reproduces the proven emission
  contract: the original header line + blank line, then
  `Skill = %d` iff `!= 1`; CRLF terminators like BUILD_A's
  on-disk `MDK.CFG`. The header is retained as the **proven
  format marker** (it is a `;` comment — readers skip it) and
  keeps the file a well-formed `MDK.CFG` for the future
  full-table writer.
- `parseFrontendSettings` — the `FUN_00425de4` apply-loop shape:
  defaults first, `name = value` lines applied in order (last
  valid Skill wins), `;` comments and non-`name = value` lines
  skipped, keys matched with the original's `and 0xdf` fold.
- `load/saveFrontendSettingsFile` — the only settings I/O in the
  port, always against a **caller-supplied path outside the
  read-only DataRoot**. The original's `C:\MDK.CFG`/relative
  `MDK.CFG` resolution is *not* reproduced — the port never
  writes into original data (`original/installed/MDK.CFG`
  included); the app wires `--settings-file FILE` explicitly
  rather than inventing a platform config directory.
- `SettingsPersistSink` (`frontend_flow.h`) — the `FUN_004260ac`
  analogue installed on `FrontendFlowController`: invoked from
  `returnToRoot()` **only when** the exit consumed a latched
  `DAT_00541486`, which then clears unconditionally — mirroring
  `0x420dbc`, failure or not.

NATIVE POLICY — hardening, **not** an original-behavior claim: a
`Skill` line that fails integer parse or lands outside the proven
`[0,2]` domain is ignored (running value kept) and counted in
`ignoredSkillLines`. What the original stores on malformed input
is UNKNOWN — its strtol-style parse would store whatever it
yields; the port deliberately does not reproduce that edge.

## Dirty latch vs default-delta — the distinction that matters

Two independent mechanisms, both proven: the **dirty latch**
(`DAT_00541486`) decides *whether* a write attempt happens, and
the **default-delta** comparison decides *what* lands in the
file. Cycling Normal → Hard → Normal leaves the latch set, so
the exit still fires the persist sink — but the serialized
settings carry no `Skill` line because the value equals the
factory default. Re-entering options afterwards shows the
process-global skill unchanged — `DAT_0054147a` lives across
entries, never re-read on screen transition.

## Scale/ramp consequence of the label change (OBSERVED structure)

`Skill - Easy` / `Skill - Normal` / `Skill - Hard` measure
different widths, so row 6's centered `x` shifts when the record
swaps — but the `FUN_00423a24` ramp key is positional
(`-1, y = 49+36·6`), not textual. A skill mutation therefore
produces **no** cur/prev key swap, **no** accumulator reset, and
**no** snap: the selected scale keeps ramping from the running
`acc` while the glyph span recenters. Pinned by the
`test_options_controller` label-width block (acc continues
0.65 → 0.72 → 0.79 across mutations; `curX/curY` verified
unchanged).

## Injected selftest — deterministic lifecycle coverage

`--selftest --interactive-frontend` (two-screen) now drives the
full Phase 4G sequence — 14 steps at the paced 100/3 ms regime:

```
f0  DOWN (root sel 1)      f7  LEFT  → Hard    (Easy−1 wraps)
f1  motion → root band 3   f8  idle — LEFT release sampled so the
f2  click → OpenOptions        repeat deadline resets (tick+30)
f3  release (sel 8)        f9  LEFT  → Normal  — latch still set
f4  motion → skill band    f10 RIGHT → Hard    — persisted value
f5  RIGHT → Hard           f11 ESC   → root    — dirty persist
f6  Enter → Easy               fires; flag clears
                           f12 Enter → re-entry (skill retained)
                           f13 ESC   → root    — no mutation, no
                                                persist call
```

The verdict checks: two entries with skills `{initial, final}`
(computed through the proven wrap rules from the loaded startup
value — canonical run `{1,2}`), first exit dirty / second clean,
exactly one persist call carrying the final skill, `dirty`
cleared, process-lifetime retention on re-entry, and — when
`--settings-file` is given — the on-disk file re-read matching
the persisted skill (`Skill = 2` on the canonical run). A second
run against the same file demonstrates the restart round trip:
it loads `skill=2`, scripts back through Easy, and persists
`Skill = 0`.

## Digests and verification

- **Row-6 snapshots** (paced regime, `sel=6`, `SYS_PAL` head
  bound, palette `08e372297e745a06` for all three):
  - Easy (`OM_SK_0`, skill 0, script frame 6 — mid-ramp
    `acc≈2.02` → scale ≈0.79): fb `780cfeb3a26c7390`.
  - Normal (`OM_SK_1`, skill 1, frame 9 — `acc≈5.04` clamped →
    scale 1.0): fb `01e229facdededd1`.
  - Hard (`OM_SK_2`, skill 2, frame 10 — `acc≈6.04` → 1.0):
    fb `11ec77782b5ce37c`. `/tmp/mdk-phase4g-skill.ppm` holds
    this frame; agent-inspected.
- Two-screen selftest end frame (back at root, sel 3 mid-
  regrowth, mouse carried at 300,259): fb `733bbf271acaf5d4`,
  palette `6a3cbda3822c5525`; settled at 45 frames: fb
  `497c1e1ea335ff51`.
- Rebaselined from Phase 4F only where the frames legitimately
  changed — the canonical-skill correction and the new script
  path: static options `0183fdb78c53a700` (was
  `3150a8a305ad9de8`), frame-8 dynamic `ead555ffad0ca609` (was
  `f00c540a40d8543d`), two-screen end/settled frames (script
  changed). Unchanged: MDKOPT `6017f4c4bd57c479`, STREAM BG
  `662bf1e20bdd351c`, FONTSML `c7956b0fea14f2ac`, FONTBIG
  `99681a15ee8479f5`, ARROW `672fff8c63fa8f4a`, root static
  `debd84b7f6e158dc`, root palette `6a3cbda3822c5525`, root-only
  interactive `cf09ecdad5b0808f`, 57/57 family parses.
- Unit tests add: serializer/parser contract (incl. CRLF +
  delta omission), temp-file Easy/Normal/Hard round trips,
  malformed/out-of-range hardening counts, absent-file fresh
  boot, write-failure reporting, dirty-gated sink invocation,
  unconditional dirty clear incl. failure, process-lifetime
  re-entry, process-restart round trips (Easy + Hard), and the
  ramp key-stability block. `mdk_tests`: **1991 checks,
  0 failures**.

## Explicit non-goals (Phase 4G)

- Only `Skill` is persisted — the other 91 table entries keep
  their factory values; no general settings framework.
- No platform default config directory — `--settings-file` is
  explicit; persistence without it logs and keeps the mutation
  process-lifetime (the `FUN_004260ac` silent-failure analogue).
- The malformed-input policy is native hardening, not a
  reproduction claim (UNKNOWN original edge).
- Child screens (Sound/Display/etc.), attract mode, and audio
  remain deferred.

# Phase 4H — Display options child screen

Phase 4H reconstructs the first real child screen of the options
sub-menu: the **Display** screen entered from options row 7. The
screen's state machine, mutations, palette composition, and exit
transition are reconstructed from instruction-level evidence; the
host-side effect of the brightness setting stays a deferred
semantic hook (the bound palette lift IS the proven visible effect
and is reproduced exactly).

## Candidate comparison (OBSERVED)

Two options rows were proven to enter child screens at the
`FUN_00420eac` dispatch level — all three per-query tables
(`FUN_004238bc` LEFT @`0x420e48`-region, `FUN_00423940` RIGHT,
`FUN_00423764` activate) bind each row to one entry:

| | Display (row 7) | Sound (row 1) |
|---|---|---|
| entry | `FUN_0041d020` | `FUN_0042322c` |
| mode written (`DAT_00541493`) | 7 | 2 |
| frame handler | `FUN_0041d1e0` | `FUN_004233d8` |
| rows | 3 (`DSP_BRGT`/`DSP_DE*`/`DSP_QUIT`) | 4 (`SND_TITL`/`SND_FX`/`SND_MUSI`/`SND_DONE` + `SND_0`/`SND_100` bar) |
| settings globals | `DAT_0054147e` Brightness int [0,7], `DAT_00541482` ForcePCorrect bool | `DAT_00541308` SoundFX int [0,100] step 10, `DAT_0054130c` SoundMusic same |
| settings table idx | 89 (int), 90 (type-2 bool) | 8, 9 (int) |
| extra visuals | 4×48 swatch ramp grid (`FUN_0041cf80` → `FUN_00416aa8` rectfill) | volume bar (`FUN_00416aa8`) |
| palette | saves active → `dlut`, composes `slut` (SYS_PAL head + 4 ramps), restores on exit | none |
| side effects | none beyond palette upload | `OPTSONG`/`OPTBUTT` from `MDKSOUND.SNI` on entry/edges; `DAT_00541538` delegation (dead — written only as 0 at WinMain init) |
| backend needed | none — the only "hardware" effect is the `FUN_0046d208` palette lift, already proven math | DirectSound enumeration/playback only for the (deferred) blip — not needed to navigate, but the screen's purpose is audio state |

**Display selected**: it reuses the exact proven machinery —
`FUN_00423b88` rows (FONTBIG, centered, `(-1,y)` ramp key), the
shared query helpers in the same order, the `FUN_00416aa8`
rectfill — plus one new bounded visual (the ramp grid) and a
palette composition whose math was already proven
(`FUN_00413b40` head+tail, `FUN_0046d208` lift). Its only
persisted fields are two more proven table entries in the same
`FrontendSettings` seam. Sound is deferred, not because its UI is
unknown — its handler is equally mapped — but because Phase 4H
scopes to ONE child and Display touches no new subsystem; the
sound screen's own records (`SND_*`), volume globals, and
`OPTSONG`/`OPTBUTT` playback remain Phase 4I+ work. The
`DAT_00541538` delegation path is documented dead in BUILD_A
(only ever written 0) — no emulation needed.

## Entry transition (`FUN_0041d020`, OBSERVED — `disasm_41d020.txt`)

Reached from options row 7 under the LEFT, RIGHT, or activate
query — all three dispatch tables bind row 7 to the same
`0x4210ce` block (the earlier session note about a permuted
activate table was a mis-decode of the trap-dword layout between
tables; the full-range disassembly shows uniform binding):

- `DAT_00541493 = 7` — display mode; the mode dispatcher
  (`FUN_0040103c`, table `0x401010` indexed `mode-1`) routes it
  to `FUN_0041d1e0`.
- `DAT_0054b834 = 2` — entry selection = the `DSP_QUIT` row.
- `dlut` ← tagged `0x300` buffer (`DAT_0049aa70`);
  `FUN_0046d614` flattens the ACTIVE BGRX palette into RGB
  triplets — the saved copy the exit restores.
- `slut` ← tagged `0x300` buffer (`DAT_0049aa74`); head
  `slut[0:0xc0]` copied from `dlut` (dead on arrival — see
  palette), tail filled with four 48-entry ramps:
  gray 64–111, red 112–159, green 160–207, blue 208–255;
  intensity `i*255/47` per channel.
- `FUN_00413b40(slut)` composes SYS_PAL head (`DAT_00540820`,
  64 entries) + `slut` tail (192 entries) → `FUN_0046d208`
  upload with the brightness lift — the slut head copy is dead
  because the helper always rebinds the resident SYS_PAL head.
- **No reset** of mouse (`DAT_0054b634/38`), tick
  (`DAT_00541518`), repeat deadlines, button latch, ramp
  (`DAT_0054bdc8` block), or timing (`DAT_0049b6e4` block) —
  the shared globals continue on the child screen.
- The options screen stays alive underneath: its selection
  `_DAT_0054bd34` keeps 7.

## Screen state globals

- `DAT_0054b834` — selection, 0..2 (written only by the display
  entry and its own handler — clean single-owner global).
- `DAT_0054147e` — Brightness, int domain [0,7] (settings table
  entry 89, `Brightness`, type 0, factory 0 — mirror byte
  `@0x49b272`).
- `DAT_00541482` — ForcePCorrect, type-2 bool (entry 90,
  `ForcePCorrect`, factory FALSE — mirror `@0x49b276`).
- `DAT_00541486` — the shared settings-dirty flag (ONE global
  shared with the options screen — child mutations latch it and
  the eventual options exit persists it).
- `dlut`/`slut` — tagged palette buffers owned by the screen
  (freed on exit).

## Frame handler (`FUN_0041d1e0`, OBSERVED — `phase4h_funcs.txt`)

Same prologue and the same query helpers in the same order as
the options screen:

1. `FUN_004187e0` mouse accumulate + `DAT_00541518 +=
   DAT_0049b6e8`.
2. prev query (`FUN_004237b4`): `sel -= 1`, wraps `<0 → 2`.
3. next query (`FUN_00423838`): `sel += 1`, wraps `>=3 → 0`.
4. Mouse hit-test gate (the three-global check, clamp to
   `(590,350)` inside): `band = trunc((mouseY - 5) / 36)` — x86
   IDIV truncation, so `mouseY` 0–4 truncates to band 0
   (reproduced); valid bands 0–2 assign unconditionally.
5. `DAT_0054b570` Esc edge → `FUN_0041d144` + RET — frame ends
   before the draw block.
6. LEFT query (`FUN_004238bc`): row 0 → `brightness -= 1`
   (wraps `<0 → 7`) + dirty + `FUN_0046d208` re-upload of slut;
   row 1 → toggle `ForcePCorrect` + dirty; row 2 → `jnz` skips
   the block (no-op). Always falls through to the RIGHT query.
7. RIGHT query (`FUN_00423940`): row 0 → `brightness += 1`
   (wraps `>=8 → 0`) + dirty + re-upload; row 1 → toggle +
   dirty; row 2 → no-op. Falls through to activate.
8. Activate query (`FUN_00423764`): row 0 → same `+1` wrap +
   dirty; row 1 → toggle + dirty; row 2 → `FUN_0041d144` + RET
   (frame ends early). Rows 0/1 fall through to the draw.
9. Draw block (below), then `FUN_0042fe78` timing update.

## Draw block (OBSERVED)

1. `FUN_00415658` → zero-fill `clear(0)` — no backdrop.
2. `sprintf(buf, DSP_BRGT, brightness)` — the record text IS
   the row-0 printf format (`"Brightness %d"`,
   `FUN_0047d2e9` vsprintf) → drawn at y=31.
3. `DSP_DETH` (`"Detail is High"`) or `DSP_DETL`
   (`"Detail is Low"`) by `DAT_00541482` → y=67.
4. `DSP_QUIT` (`"Quit"`) → y=103.
   All three via `FUN_00423b88` — FONTBIG, centered on 600,
   ramp key `(-1, y)` — the identical helper the options rows
   use (`kFtiFontBigMissingAdvance` measure, x87-trunc center).
5. `FUN_0041cf80` swatch grid — four bands × 48 cells of
   `FUN_00416aa8` **inclusive** rectfill: cells
   `x = 60+10i .. 69+10i` (10 px), `y = 200+32b .. 231+32b`
   (32 px), color index `64 + 48*band + i` — the indices line
   up exactly with the entry-composed palette ramps.
6. `ARROW` at the raw logical mouse (`FUN_004236c0`).

## Palette contract (OBSERVED)

- On entry: active palette saved to `dlut`; composed palette =
  SYS_PAL head (entries 0–63, from `DAT_00540820` — NOT the
  dead slut head copy) + the four ramps (64–255), uploaded via
  `FUN_0046d208`.
- `FUN_0046d208` staging lift (OBSERVED, `disasm` head +
  `0x46d2c3` branch): per channel `min(c + level*16, 255)` at
  upload for `level = DAT_0054147e` — applied uniformly to
  every bound entry, including index 0 (nonzero brightness →
  dark-gray clear color, exactly like the original's lifted
  zeros). The runtime `DAT_0054d7b8` table stays RAW — the
  lift lives only in the staging buffer, so `FUN_0046d614`
  snapshots never compound.
- Row-0 mutations re-upload `slut` immediately (the new lift
  shows the same frame — the native renderer binds the lifted
  palette every frame, so the effect is identical).
- On exit (`FUN_0041d144`): `FUN_0046d208(0,0x100,dlut)`
  restores the saved palette; both tagged buffers freed.

## Mutations (OBSERVED)

| row | label | LEFT | RIGHT | activate |
|---|---|---|---|---|
| 0 | `DSP_BRGT` (sprintf brightness) | −1, wrap <0→7 | +1, wrap ≥8→0 | +1, wrap ≥8→0 |
| 1 | `DSP_DETH`/`DSP_DETL` | toggle | toggle | toggle |
| 2 | `DSP_QUIT` | no-op | no-op | exit |

Every mutation latches `DAT_00541486`. LEFT/RIGHT never end
the frame; Esc and row-2 activate RET before draw+timing.
Row-2 LEFT/RIGHT no-ops are reproduced, not "fixed".

## Exit transition (`FUN_0041d144`, OBSERVED)

- `DAT_00541493 = 0x0b` — back to the options handler.
- Saved `dlut` palette re-uploaded; `dlut`+`slut` freed.
- `_DAT_0054bd34` untouched → options resumes at selection 7.
- Machine state (mouse/tick/deadlines/latch/ramp/timing) and
  the dirty flag carry back; **no persist here** — the
  eventual `FUN_00420d68` options exit persists the dirty flag
  exactly like Phase 4G.

## Settings-table mapping (OBSERVED)

| idx | key | type | global | factory (mirror) |
|---|---|---|---|---|
| 89 | `Brightness` | int | `DAT_0054147e` | 0 `@0x49b272` |
| 90 | `ForcePCorrect` | 2 (bool) | `DAT_00541482` | FALSE `@0x49b276` |

`FrontendSettings` gains `brightness`/`forcePCorrect` alongside
the 4G `skill` — same serializer/parser contract: emitted in
table order (Skill, Brightness, ForcePCorrect) only when
non-default; `ForcePCorrect` emits `TRUE` only (the mirror
compare means a written bool is always non-default); type-2
parse = `toupper(first non-space value char) == 'T'` applied
unconditionally; `Brightness` gets the same native hardening
counter (`[0,7]` domain) as `Skill`.

## Native port decisions (NATIVE PORT)

- The flow owns `brightness_`/`forcePCorrect_` as process
  globals (`DAT_0054147e/82` lifetime), seeded from
  `--settings-file` config exactly like `skill_`; the child
  controller borrows them on `FUN_0041d020` and writes them
  back on `FUN_0041d144` — one shared mutable location, like
  the original's globals.
- The options controller gets `setMachineState`/
  `setSettingsDirty` — the serialization seam the
  `FUN_0041d144` return needs (the options globals were never
  re-initialized in the original either).
- `applyFrontendBrightness` (`core/frontend_palette.h`) is the
  shared `FUN_0046d208` staging lift; all three screens bind it
  over their palette composition (root/options apply it to
  SYS_PAL head + zeroed tail — the original's uniform lift over
  whatever is bound, zero entries included).
- `DAT_00541538` delegation NOT modeled — dead in BUILD_A.
- No host display-mode change, no renderer enumeration — the
  child screen never called a platform display API; its only
  visible effect is the palette lift, which is reproduced.

## CLI + deterministic validation

- `--preview-display-submenu` — composes the entry-state static
  frame (`DAT_0054b834=2`, canonical brightness 0 /
  ForcePCorrect FALSE, ARROW at the carried mouse) and logs
  fb/palette digests.
- `--interactive-frontend` — the options row-7 action now enters
  the real child; Esc or Quit-activate returns to options at
  selection 7; the display screen's frame is skipped (not
  drawn) on the dispatch frame exactly like the original's RET.
- The injected selftest extends to frames 13–24: options band-7
  motion → Enter (`FUN_0041d020`, entry sel 2) → display band-0
  motion → RIGHT + Enter (brightness 0→2) → band-1 motion →
  Enter (ForcePCorrect → TRUE) → band-2 motion → Enter
  (`FUN_0041d144`, resume sel 7) → Esc (`FUN_00420d68`,
  persist #2 writes the full triple) → Enter (entry 3 proves
  process-lifetime retention) → Esc (clean, no persist).
  Default selftest frames: 25.

## Digests and verification

- Static display preview: fb `e8c31e4839c0e1d4`, palette
  `64ce160e2384e256` (SYS_PAL head + 4 ramps, lift 0).
  `/tmp/mdk-phase4h-child.ppm` — agent-inspected: three
  centered rows ("Brightness 0" / "Detail is Low" / "Quit"
  selected), four 48-cell gray/red/green/blue ramps, ARROW.
- Dynamic child snapshot (script frame 20 — sel 2, brightness
  2, pcorrect 1): fb `5bfe84dc4fc7a023`, palette
  `514f0c9fb5abcb26`. `/tmp/mdk-phase4h-dynamic.ppm` —
  agent-inspected: "Brightness 2" / "Detail is High" / lifted
  background (index 0 lifts too, OBSERVED parity) + lifted
  ramps. The `64ce→514f` palette shift IS the brightness-2
  lift reaching the bound palette.
- Pre-entry options frame (script frame 13 — sel 7, mouse
  300,289): fb `1c5686da0ff03f41`, palette
  `08e372297e745a06`.
- Display frame at sel 0 (script frame 15): fb
  `0b3c66e7e1d108ab`, palette `64ce160e2384e256` — palette
  matches the static preview's (same composition at lift 0).
- Three-screen selftest end state: `PASS` — entries=3
  (skills 1,2,2), exit-dirty 1,1,0, persists=2 (final
  `skill=2 brightness=2 pcorrect=1`), display-entry sel 2,
  resume sel 7, 6 display frames drawn, root restored
  (sel 3, mouse 300,89).
- `--settings-file` round trip: persist #2 writes
  `Skill = 2` + `Brightness = 2` + `ForcePCorrect = TRUE`
  (CRLF); a second run loads `(2,2,1)` and the options entry
  shows skill 2 — restart persistence intact.
- Phase 4F frame-8 dynamic `ead555ffad0ca609` is **superseded
  by design**: it was the options frame after a band-7 click
  that only emitted a passthrough action; the click now enters
  Display (`FUN_0041d020`) so that frame cannot occur on any
  script path. The state it encoded remains proven by the
  unchanged static options digest `0183fdb78c53a700`, palette
  `08e372297e745a06`, all three 4G skill snapshots
  (`780cfeb3a26c7390`/`01e229facdededd1`/`11ec77782b5ce37c`),
  and the pre-entry frame above. All other 4A–4G digests
  reproduce byte-exact.
- Unit tests add: display entry state + carry-over, up/down
  wrap, exact band boundaries incl. the `(y−5)/36` truncation
  quirk and the (590,350) gate clamp, brightness wrap both
  directions + activate, ForcePCorrect toggle on all three
  queries, row-2 LEFT/RIGHT no-ops, Esc/Quit early frame end,
  prev-before-next order, latch, ramp key `(-1,y)` +
  accumulator semantics, renderer contracts + ramp/swatch
  geometry + palette composition + lift, flow transitions both
  directions + process-global mutation visibility + dirty
  carry + triple persistence + delayed-persist gate;
  `Brightness`/`ForcePCorrect` serializer/parser coverage.
  `mdk_tests`: **2133 checks, 0 failures**.

## Explicit non-goals (Phase 4H)

- No Sound screen — its records, volume globals, `OPTSONG`/
  `OPTBUTT` playback, and the (dead) `DAT_00541538` delegation
  are documented but not implemented.
- No host display-mode switching, no renderer enumeration —
  the original child applies no platform display API; the
  palette lift is the complete proven visible effect.
- No audio (`SND_PUSH` still deferred).
- No new asset formats — DSP_* are existing FTI string
  records through the proven `MDKFONT.FTI` path.
- No persistence of unproven fields — only the three mapped
  entries serialize.

## Phase 4I candidate directions

1. **Sound options screen** — `FUN_0042322c`/`FUN_004233d8`
   are already mapped (4 rows, volume globals
   `DAT_00541308`/`0c` ±10 clamp [0,100], settings idx 8/9
   `SoundFX`/`SoundMusic` defaults 70/100); its blip playback
   (`OPTSONG`/`OPTBUTT`, `MDKSOUND.SNI`) can stay deferred or
   become the first audio seam.
2. **Attract slideshow** — `FUN_0041ef74` + the timeout chain
   (30/5/4/2 s thresholds on `DAT_0049aaa4`).
3. **Frontend sound** — `SND_PUSH` (`FUN_00423734`) fires on
   every repeat/activate edge; records pending.
4. **More settings-table entries** — 89 remain unmapped; all
   `name`/type/pointer triples dumped at `0x49aca8`, each
   needs its own semantics proven before joining
   `FrontendSettings`.
