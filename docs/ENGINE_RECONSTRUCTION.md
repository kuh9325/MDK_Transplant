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
(`FUN_00420d68`). Settings mutation stays deferred. Everything below
is OBSERVED at instruction level in `MDK95.EXE` (BUILD_A) unless
marked otherwise; private notes live in `analysis-private/logs/`
(`decomp_20eac.txt`, `disasm_optitems.txt`, the `FUN_00420cf0`/
`FUN_00420d68` disassemblies).

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

Activating row 8 or pressing Esc runs `FUN_00420d68`:

1. If the settings-dirty flag `DAT_00541486 != 0`, persist settings
   via `FUN_004260ac` — Phase 4F never mutates, so this never runs.
2. `FUN_00402590` — re-enter the front-end list path.
3. `FUN_0046d208(0, 0x100, svlut)` — restores the palette saved at
   entry, then releases `svlut`.
4. `DAT_00541493 = 0` — mode back to the root handler.

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
| 8 | `OM_QUIT`  | "Quit"         | `FUN_00420d68` (leave options → root) |

Row 6's record is dynamic: `OM_SK_0`/`OM_SK_1`/`OM_SK_2` by the skill
global `DAT_0054147a` (0/1/2). Canonical value is **0** ("Skill -
Easy") — no MDK.CFG entry or other startup writer sets it.

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
- Esc edge: jumps to the row-8 case — `Back` (`FUN_00420d68`).
- LEFT on row 6: `SkillCyclePrev` (skill −1, wraps 0→2) then falls
  through to the RIGHT query; on any other row it dispatches that
  row's action and the frame ends.
- RIGHT on row 6: `SkillCycleNext` (+1, wraps 2→0) then falls
  through to the activate query; otherwise dispatches.
- Enter/button click on row 6: forward cycle; on rows 0–5,7,8:
  dispatch. A click selects (same-frame hit-test) then activates.

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
mutation listed in the item table. Phase 4F emits the events and
performs the proven `Back` transition; settings mutation
(`DAT_0054147a`, volumes, bindings, display mode, `DAT_00541486`
dirty flag, `FUN_004260ac` persistence) is deferred.

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

- Static options preview: fb `3150a8a305ad9de8`, palette
  `08e372297e745a06` (SYS_PAL head + zero tail).
- Dynamic options snapshot `/tmp/mdk-phase4f-options.ppm` (script
  frame 8 — sel 7 mid-ramp, Quit decaying, ARROW at 300,301):
  fb `f00c540a40d8543d`, palette `08e372297e745a06`.
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
  hit-test guard, exact band boundaries, Esc, LEFT/RIGHT skill
  cycles vs row dispatch, latch semantics, ramp keyed `(−1, y)`,
  flow state carry-over both directions, static/dynamic renderer
  contracts.
- Interactive selftests: root-only PASS, two-screen PASS
  (entered=1 returned=1, `Display` emitted, root sel 3 restored).

## Explicit non-goals (Phase 4F)

- No settings mutation — skill, sound, controls, performance,
  display values never change; `DAT_00541486`/`FUN_004260ac`
  persistence path documented but unused.
- No child screens — Help/Sound/Joystick/Mouse/Keyboard/
  Performance/Display sub-screens are semantic dispatches only.
- No attract slideshow, no audio (the `SND_PUSH` hook on repeat
  fires is still deferred).
- The options palette tail (`DAT_0054c678` ← level palette record)
  is unresolved in BUILD_A; the renderer binds the proven head and
  documents the gap rather than guessing.

## Phase 4G candidate directions

1. **One real options mutation/child screen** — pick the smallest
   evidence-complete target from the action table (Display, Sound,
   Mouse, Keyboard, or the Skill cycle's `DAT_0054147a` write +
   `OM_SK_*` relabel + dirty-flag/persist path).
2. **Attract slideshow** — `FUN_0041ef74` + the timeout chain
   (30/5/4/2 s thresholds on `DAT_0049aaa4`).
3. **Frontend sound** — SND_* records behind `FUN_00423734`.
