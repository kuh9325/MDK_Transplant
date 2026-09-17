# Engine Reconstruction — Phase 4 status

Phase 3 (file-format structure) is CLOSED: all seven families have
evidence-backed structural parsers (SNI, MTI, MTO, CMI, DTI, FTI, BNI)
— see `DATA_FORMATS.md`. Phase 4 begins engine reconstruction:
original data flowing through the native runtime into the indexed
framebuffer and out the Metal presenter.

Phase 4A delivered the first vertical slice: one original visual
resource decoded end-to-end from user-supplied data into the window.

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
