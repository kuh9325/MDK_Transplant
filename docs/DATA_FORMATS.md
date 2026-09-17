# Data Formats — Phase 3F

Status: fourth interior directory mapped. This document records
evidence-backed file-format structure for the proprietary families
found under the data root. Phase 3B established the top-level
envelope; Phase 3C added explicit file-family dispatch and the first
proven interior directory (`.SNI`); Phase 3D added the second proven
interior directory (`.MTI`, metadata only); Phase 3E added the third
proven interior directory (`.MTO`, metadata only — the original's
"overlay" subsystem); Phase 3F adds the fourth proven interior
directory (`.CMI`, metadata only — counted variable-length tables).

Evidence levels follow `reverse-engineering/EVIDENCE_POLICY.md`.
"BUILD_A" = `original/installed/` — the NoCD repack; data integrity vs
a retail/GOG dump remains **UNVERIFIED**.

## Evidence policy for this document

- OBSERVED = verified on real file bytes across BUILD_A.
- CODE-CORROBORATED = also confirmed in original loader disassembly
  (Ghidra, MDK95.EXE BUILD_A).
- UNKNOWN / HYPOTHESIS = not yet evidence; never load-bearing in code.

## Dispatch matrix

`fileFamilyForPath(relPath)` (`src/core/file_family.cpp`) is the single
source of truth: case-insensitive extension dispatch on the trailing
extension of a relative path (both `/` and `\` separators). It never
inspects file contents. `parserFamilyForPath` (envelope-level grouping)
delegates to it — no duplicated extension tables.

| Family | Ext | Envelope | Support | Interior status |
|---|---|---|---|---|
| MTO | `.MTO` | tagged-name | **directory-metadata** | count + 12-byte `{name[8], u32 fileOff}` records → overlay blocks containing an embedded `.MAT`/MTI image + regions A/B/C (below) |
| SNI | `.SNI` | tagged-name | **directory-metadata** | count + 24-byte directory records proven (below) |
| MTI | `.MTI` | tagged-name | **directory-metadata** | count + 24-byte records `{name[8], u32 flags, u32, u32, u32 off}` proven (below) |
| CMI | `.CMI` | tagged-name | **directory-metadata** | four counted variable-length tables `{u8 len, name[len], u32 imgOff}` + bounded data region (below) |
| DTI | `.DTI` | tagged-name | **directory-metadata** | five-section image-relative TOC: params / keyed records / arena table (+typed 36-byte payloads) / RGB table / byte grid (below) |
| FTI | `.FTI` | length only | **directory-metadata** | count + 12-byte `{name[8], u32 imgOff}` records → payloads tiling to EOF (below) |
| BNI | `.BNI` | length only | **directory-metadata** | count + 16-byte `{name[12], u32 imgOff}` records → payloads tiling to EOF (below) |
| LBB | `.LBB` | none observed | unsupported | raw structure; fails u32@0 envelope |
| SAV | `.SAV` | none observed | unsupported | save-game packet skeleton (per loader strings); no envelope |
| FLIC | `.FLC` | — | standard-external-format | Autodesk FLIC (magic 0xAF12 observed) |
| MVE | `.MVE` | — | standard-external-format | Interplay MVE |
| GIF | `.GIF` | — | standard-external-format | GIF87a/89a |
| FRC | `.FRC` | — | standard-external-format | RIFF "FORC" force-effect data |
| other-known | `.EXE .DLL .COM .VXD .386 .SYS .OVL .BAT .CFG .INI .TXT .INF .CONF .ICO .PDF .WMV .DB` | — | unsupported | observed in BUILD_A, not MDK data families |
| unknown | anything else | — | unsupported | no parser claim |

## Common top-level envelope (recap, Phase 3B)

Tag family (.MTO/.SNI/.MTI/.CMI/.DTI — 46/46 files):

```
u32le @0x00 = fileSize - 4     declared length of everything after it
bytes [4,16)  12-byte logical-name field "<stem>.<ext>" (NUL-padded;
              unterminated when full — e.g. "MDKSOUND.SND")
u32le @0x10 = fileSize - 12    == absolute file offset of the trailing
                               12-byte name-field repeat (OBSERVED 46/46).
                               The SNI streaming loader SKIPS this field
                               entirely (fseek +0x10 SEEK_CUR); it is not
                               load-bearing for SNI parsing. Whether the
                               field is *meant* as a trailer offset or a
                               derived length is UNKNOWN (writer-side
                               code not analyzed).
interior at 0x14 …
bytes [size-12, size)          trailer: verbatim repeat of the name
                               field (OBSERVED 46/46 tag family)
```

The "content blob" model (CODE-CORROBORATED): the original's whole-file
loader (`FUN_004259a8`) reads u32@0 then `fread`s exactly `size-4`
bytes — the in-memory image starts at file offset 4 and **all interior
stored offsets index that blob** (image + off == file + 4 + off).

## Proven interior directory: SNI

Evidence class: OBSERVED (15/15 `.SNI` files in BUILD_A) +
CODE-CORROBORATED (MDK95.EXE `FUN_00428a0c` directory loader +
`FUN_004259a8` blob loader + `FUN_00428c90`/`FUN_00429014` consumers).

### Layout

```
file offset
0x00  u32le        blob length = fileSize - 4          (envelope)
0x04  char[12]     logical name "<stem>.SND"           (envelope)
0x10  u32le        = fileSize - 12 (see envelope note; skipped by loader)
0x14  u32le        entry count N
0x18  record[N]    24-byte directory records:
  +0x00  char[12]  entry name, NUL-padded; original compares at most
                   12 bytes (MOV EBX,0xc → bounded compare FUN_0042fa80)
  +0x0c  u32le     field — semantics UNKNOWN. OBSERVED values form a
                   small set: 0x00000000/0x00000001/0x00000003/
                   0x50000001/0x7fff0000/0x7fff0001/0x7fff0003 and
                   0xffffffff on sentinel records. The original reads
                   it as a u16 in one path (FUN_00428be8) — plausibly
                   {u16 lo, u16 hi} flags/params; do not interpret.
  +0x10  u32le     stored payload offset, relative to the content blob
                   (file position = stored + 4). CODE-CORROBORATED:
                   the stream path does fseek(stored + 4, SEEK_SET).
  +0x14  u32le     payload byte count (CODE-CORROBORATED: read as the
                   fread length by FUN_00429014)
…     payloads     [4+off, 4+off+size) each
size-12  char[12]  trailer = name field repeat
```

### OBSERVED invariants (validated by the parser)

- Payloads are stored in directory order and tile `[dirEnd, size-12)`
  exactly; first payload begins at `dirEnd`; every payload start is
  4-byte aligned (gaps 0 or 2 bytes).
- Every payload `[4+off, +size)` lies inside `[dirEnd, size-12)`.
- Entries with both `+0x0c == +0x14 == 0xffffffff` are sentinel
  position markers (see below).

### Sentinel records (OBSERVED)

Six records across `LEVEL4S.SNI` (K_SURF, K_SURFJ) and `LEVEL6S.SNI`
(K_SLIP, K_SLIDE, K_BSLIDE, K_FSLIDE) — always `K_`-prefixed, always
the trailing entries — carry `+0x0c = +0x14 = 0xffffffff`. Their `+0x10`
still holds a real in-bounds blob offset pointing at data past the last
regular payload; the data region runs to the next record's position or
the trailer. `0xffffffff` is never a byte count — a payload read of
that size is impossible, so these records never take the fread path.
The parser bounds-checks the marker position only. The `K_` name
correlation and the pointed-to data's structure are UNKNOWN.

### BUILD_A samples (all parse `ok`)

| File | Entries | Sentinels | Notes |
|---|---|---|---|
| `MISC/MDKSOUND.SNI` | 3 | 0 | OPTBUTT, SNDTEST, OPTSONG |
| `TRAVERSE/TRAVERSE.SNI` | 53 | 0 | incl. 10–11-char names (SNIPERSHOT) |
| `TRAVERSE/LEVEL*/LEVEL*O.SNI` | 14–18 | 0 | per-level sets |
| `TRAVERSE/LEVEL*/LEVEL*S.SNI` | 8–19 | 2–4 | sentinel records present in LEVEL4S/LEVEL6S |
| `FALL3D/FALL3D.SNI` | 23 | 0 | freefall set |

Payload contents: most begin with RIFF/WAVE headers (OBSERVED by byte
inspection — **not decoded**; payload interpretation is out of scope
for this phase).

### Original loader functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_00428a0c` | SNI directory loader: fread u32@0; fseek(+0x10, SEEK_CUR) → 0x14; fread count; alloc `count*0x18`; fread `count` × 24-byte records; then allocates the payload pool |
| `FUN_004259a8` | Whole-blob loader (used for `MISC\MDKSOUND.SNI`): reads u32@0, allocs, freads exactly `size-4` bytes → image = content blob |
| `FUN_00428c90` | Entry lookup/stream dispatch: name compare bounded to 12, seeks `entry+0x10 + 4` SEEK_SET |
| `FUN_00429014` | Payload consumer: `entry+0x14` is the byte count, chunked freads (0xf768) |
| `FUN_00428be8` | Iterates the loaded directory; reads `+0x0c` as u16 |
| `FUN_00428828` | In-memory iterator: `img+0x10` = count, `img+0x14` = records where img = blob (file+4) — confirms blob-relative offsets |
| `FUN_0041b1f8` | Open + tag validation (mdkfopen path) used by the loader |
| `FUN_0042fa80` | Bounded (≤12) name compare |
| `FUN_0041c884` | 8-byte-aligned pool allocator |
| `FUN_0047e086`/`FUN_0047e1ef` | fseek/fread wrappers (Watcom calling convention) |

## Proven interior directory: MTI

Evidence class: OBSERVED (13/13 `.MTI` files in BUILD_A) +
CODE-CORROBORATED (MDK95.EXE `FUN_0041a1e0` table parser +
`FUN_00425c8c`/`FUN_00425bfc` blob loaders + `FUN_0041a590`/
`FUN_0041a5ec`/`FUN_0041a694` lookup consumers).

### Layout

```
file offset
0x00  u32le        blob length = fileSize - 4          (envelope)
0x04  char[12]     logical name "<stem>.MTI"/".MAT"    (envelope)
0x10  u32le        = fileSize - 12 (envelope; not read by the parser)
0x14  u32le        entry count N      (img+0x10 in the original —
                   img = content blob loaded from file offset 4)
0x18  record[N]    24-byte records (img+0x14; the original advances
                   the source pointer by 6 u32s per record):
  +0x00  char[8]   entry name, NUL-padded (copied as 2 u32s to
                   in-memory +0x28; looked up by C-string compare via
                   FUN_0042fa50; may fill the field — no terminator
                   inside it in that case)
  +0x08  u32le     class/flag word. == 0xffffffff marks an INDEX
                   record (CODE-CORROBORATED compare). Otherwise the
                   original tests bits 0x00030000 to select the
                   extended payload header and preserves the low 16
                   bits. OBSERVED values: 0x00000000 (556 recs),
                   0xffffffff (579), 0x00010001 (59), 0x00010000 (11),
                   0x00000002 (1).
  +0x0c  u32le     INDEX records: the index value (CODE-CORROBORATED —
                   copied to in-memory +0x08; the original reads no
                   other field of index records). PAYLOAD records:
                   copied verbatim to in-memory +0x1c; semantics
                   UNKNOWN (observed 0; four records carry
                   0x469c4000 — a float bit pattern).
  +0x10  u32le     INDEX records: ignored by the original (OBSERVED 0
                   in all 579). PAYLOAD records: copied verbatim to
                   in-memory +0x20; semantics UNKNOWN (only
                   0x40600000 = 3.5f and 0x40c00000 = 6.0f bit
                   patterns observed — float-typed usage is plausible
                   but NOT proven).
  +0x14  u32le     INDEX records: ignored by the original (OBSERVED 0
                   in all 579). PAYLOAD records: stored payload offset
                   RELATIVE TO THE CONTENT BLOB — file position =
                   stored + 4 (CODE-CORROBORATED: the original
                   dereferences img + stored directly).
…     payloads     [4+off, next payload's 4+off) each
size-12  char[12]  trailer = name field repeat
```

### Record classes (CODE-CORROBORATED)

- **Index records** (`+0x08 == 0xffffffff`): name + `+0x0c` index only.
  The original stores `index` and a `-1` marker in the in-memory
  descriptor. `+0x10`/`+0x14` are ignored — our parser preserves them
  raw and never bounds-checks them. OBSERVED examples: `PEN_1`…
  `PEN_216` (index follows the name's digits), `NONE` (index 256),
  `BLACK` (index 0), `GREY*` — the name→index correlation is OBSERVED
  byte-level fact; its semantics are UNKNOWN.
- **Payload records** (any other `+0x08`): name + flags + two raw
  params + blob offset to a payload whose first bytes form a header
  (below).

### Payload header (CODE-CORROBORATED — read by FUN_0041a1e0)

The record stores no byte count; the original reads the payload's own
header to bound it:

```
flags & 0x00030000 == 0  ("plain"):
  u16 @payload+0   header field A
  u16 @payload+2   header field B
  data             starts at payload+4
flags & 0x00030000 != 0  ("extended"):
  u16 @payload+0   header count (merged into the in-memory flags
                   high word by the original)
  u16 @payload+4   header field A
  u16 @payload+6   header field B
  data             starts at payload+8
```

The original derives a shift count from field A (smallest k with
`2^k >= A`, capped at 12 iterations) and a family of masks from A and
B — consistent with A/B being dimensions — but no semantic name is
proven; they are reported raw as `headerFieldA`/`headerFieldB`.

### OBSERVED invariants (validated by the parser)

- Payload records' stored offsets are ascending in record order and
  their `[4+off, …)` payloads tile `[dirEnd, size-12)` exactly —
  first payload at `dirEnd`, last ends at the trailer.
- Index records interleave freely among payload records.
- Every payload header lies inside `[dirEnd, size-12)`.
- Record `count` may be 0 (the original branches on count==0 and
  produces an empty table).

### BUILD_A MTI inventory (all 13 parse `ok`)

| File | Records | Payload | Index |
|---|---:|---:|---:|
| `FALL3D/FALL3D_1..5.MTI` | 56 | 34 | 22 |
| `MISC/STATS.MTI` | 40 | 2 | 38 |
| `STREAM/STREAM.MTI` | 55 | 12 | 43 |
| `TRAVERSE/LEVEL3/LEVEL3S.MTI` | 198 | 71 | 127 |
| `TRAVERSE/LEVEL4/LEVEL4S.MTI` | 138 | 95 | 43 |
| `TRAVERSE/LEVEL5/LEVEL5S.MTI` | 111 | 67 | 44 |
| `TRAVERSE/LEVEL6/LEVEL6S.MTI` | 123 | 58 | 65 |
| `TRAVERSE/LEVEL7/LEVEL7S.MTI` | 132 | 82 | 50 |
| `TRAVERSE/LEVEL8/LEVEL8S.MTI` | 129 | 70 | 59 |

One structural class across the corpus; no level-specific variants,
no malformed/outlier files. Record counts 40–198.

### Original loader functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_0041a1e0` | MTI table parser: `count = *(img+0x10)`; records at `img+0x14`, source stride 24; per record copies name[8] to in-memory +0x28; `+0x08 == 0xffffffff` → index path (dest+0x08 = `+0x0c`, dest+0x0c = -1); else dest+0x1c = `+0x0c`, dest+0x20 = `+0x10`, payload ptr = `img + +0x14`; flags `& 0x30000` select the 4- vs 8-byte payload header; dest stride 0x34 |
| `FUN_0041a4d0` | Level-MTI load path: `LEVEL%dS.MTI`/`TLEVEL.MTI` → `FUN_00425c8c` (whole-blob) → `FUN_0041a1e0` into `DAT_0054b72c` |
| `FUN_0041a480` | Shared load path (`STATS.MTI` via `FUN_00429200`, `STREAM.MTI` via `FUN_0042b270`, `FALL3D_%d.MTI` via `FUN_0040ef28`): `FUN_00425bfc` (whole-blob) → `FUN_0041a1e0` into `DAT_0054b72c` |
| `FUN_0041a820` | Parses a second MTI image into the second table `DAT_0054b730`/`DAT_0054b734` (default "matdef" allocator); called from `FUN_00432534` |
| `FUN_00425c8c`/`FUN_00425bfc` | Whole-blob loaders: open via `FUN_0041b300`/`FUN_0041b148` (tag-checked path), fread u32@0, alloc, fread `size-4` → img = content blob at file+4 |
| `FUN_0041a590`/`FUN_0041a5ec`/`FUN_0041a694` | Name lookups: iterate `DAT_0054b728` (+ second table) with 0x34 stride, compare name at +0x28 via `FUN_0042fa50`; failure message `Texture %s not in material list` |
| `FUN_0041a548` | Second-table teardown; asserts against `...\mdksrc\share\loadmats.c` — the original's own name for this subsystem |
| `FUN_0042fa50` | Unbounded C-string compare used for name lookups |
| `FUN_00433d40` | Level-bundle loader: sequences `.MTI` (FUN_0041a4d0), `.BNI`, `.CMI`, `.DTI`, `.MTO` loads |

### Naming note (CODE-CORROBORATED)

The original's own strings call this the **material/texture table**:
allocation tag `"matdef"`, lookup tag `"matlkup"`, failure message
`"Texture %s not in material list"`, teardown assert path
`...\mdksrc\share\loadmats.c`. Record/field names in the parser still
use offsets (`fieldAt0x08` etc.) because per-field semantics beyond
the proven behavior remain UNKNOWN.

## Proven interior directory: MTO

Evidence class: OBSERVED (6/6 `.MTO` files = 60/60 overlay blocks in
BUILD_A) + CODE-CORROBORATED (MDK95.EXE overlay cluster
`FUN_0041a84c`…`FUN_0041ab44`, stream consumer `FUN_00432534`, the
shared MTI parser `FUN_0041a1e0` running on each block's embedded
image, and the region-C walker `FUN_00419ee0`).

### Subsystem naming (CODE-CORROBORATED)

The original's own strings call this the **overlay** subsystem:
allocation tag `"overlay"`, lookup failure `"No overlay data for %s"`,
the record-cap diagnostic `"Too many overlay sounds"`, and the
resolver diagnostic `"Failed to resolve overlay alien %s"`. Paths are
built as `"%s\LEVEL%d\LEVEL%dO.MTO"` (`FUN_0041b7b4`) and
`"TLEVEL.mto"` (`FUN_00433d40`); nearby assert/source strings name
`setupob.c`, `loadmats.c`, `soundset.c`, `traverse.c`, `tr_alcmd.c`.
The parser therefore says "overlay block"/"overlay data" and
"overlay-alien"/"overlay-sound" records — it does NOT call a block an
object/model/actor/mesh; that semantic step remains UNPROVEN.

### Outer layout

```
file offset
0x00  u32le        blob length = fileSize - 4          (envelope)
0x04  char[12]     logical name "<stem>.MAT"           (envelope)
0x10  u32le        = fileSize - 12 (envelope)
0x14  u32le        overlay count N   (CODE-CORROBORATED: fread 4 at
                   fseek(0x14, SEEK_SET) in FUN_0041a84c)
0x18  record[N]    12-byte directory records (fread N x 12):
  +0x00  char[8]   entry name, NUL-padded; the original compares at
                   most 8 bytes (MOV EBX,0x8 → bounded compare)
  +0x08  u32le     FILE-ABSOLUTE offset of the overlay block
                   (CODE-CORROBORATED: fseek(stored, SEEK_SET) in
                   FUN_0041a9d8)
…     blocks       OBSERVED: align4-chained inside [dirEnd, size-12)
size-12  char[12]  trailer = name field repeat
```

Block streaming (CODE-CORROBORATED): the u32 at the block offset is
the block's byte length, read by `FUN_0041a9d8`; `FUN_0041aad0` then
streams `len` bytes starting at `off+4` in ≤0x8000 chunks. The stored
length is therefore SELF-INCLUSIVE — the last four streamed bytes run
into the align4 slack after the block. All interior offsets resolve
inside `[off, off+len)`, so this is harmless and is preserved as
OBSERVED behavior, never "corrected".

### Overlay block interior

```
block-relative
+0x00  u32le   block length (self-inclusive, see above)
+0x04  u32le   region-A target: struct base = off+8+ofsA
+0x08  u32le   region-B target: base = off+4+ofsB
+0x0c  u32le   region-C target: base = off+4+ofsC
+0x10          embedded tagged ".MAT" file (full envelope below)
```

Embedded ".MAT" file (OBSERVED 60/60; CODE-CORROBORATED — the
original runs `FUN_0041a1e0`, the MTI table parser, on `buf+0x10`):

```
inner+0x00  u32le    innerSize - 4   (same envelope convention)
inner+0x04  char[12] inner name "<ENTRY>.MAT"
inner+0x10  u32le    = innerSize - 12
inner+0x14  u32le    MTI record count
inner+0x18  rec[N]   MTI 24-byte records {name[8], flags, u32, u32,
                     u32 off}; +0x14 payload offsets are relative to
                     the embedded NAME FIELD (inner+4) — file pos =
                     off+0x14+stored (CODE-CORROBORATED: the parser's
                     blob base is the image pointer it is handed)
…           payloads inside [innerDirEnd, innerEnd-12)
innerEnd-12 char[12] trailer = inner name repeat (OBSERVED 60/60)
```

Record classes are the shared MTI ones (index flag 0xffffffff exists
in the mechanism but is OBSERVED-absent in all 483 MTO records; flags
∈ {0, 2, 0x20000}).

Region A at `off+8+ofsA`, preceded by a self-exclusive `u32 sizeA`
at `off+4+ofsA` (OBSERVED: `tA == innerEnd+4`, i.e.
`ofsA == innerLen+0x10`, in 60/60):

```
tA+0x00  u32le countA → rec12[countA] {name[8], u32 tA-relative off}
tA+0x04  u32le countB → rec12[countB] {name[8], u32 tA-relative off}
         CODE-CORROBORATED: resolved by FUN_00403720 — "Failed to
         resolve overlay alien %s" → overlay-alien references
tA+0x08  u32le countC → rec24[countC]
         {u32,u32,u32,u16,u16,u32 tA-rel off,u32}
         CODE-CORROBORATED: countC > 0x10 aborts with "Too many
         overlay sounds"; FUN_004287cc/FUN_00402e2c resolve them →
         overlay-sound records
…        trailing blobs that the record offsets index
align4(tA + sizeA) == region-B base (OBSERVED 60/60)
```

Region B at `off+4+ofsB`: exactly 0x150 bytes in 60/60 blocks; the
original copies it into a fixed structure (`DAT_00540dcc`-based).
Palette-like; semantics UNKNOWN — reported as a fixed-size span.

Region C at `off+4+ofsC` (CODE-CORROBORATED walk — `FUN_00419ee0`):

```
u32le c1;  rec10[c1] name fields; 2-byte pad iff c1 odd
u32le c2;  rec44[c2]
u32le c3;  rec36[c3]
u32le c4;  rec12[c4]
u32le;     trailing data to block end
```

### Field-by-field evidence matrix

| Element | BUILD_A bytes | Original code | Status |
|---|---|---|---|
| count @0x14 | 10 in all 6 files | fread 4 after fseek(0x14, SEEK_SET) | CODE-CORROBORATED |
| table-1 stride 12 | records tile [0x18, 0x90) | alloc `count*0xc`; fread(...,12,count); lookup walk stride 0xc | CODE-CORROBORATED |
| name[8] | NUL-padded ≤8, no dups | compare bound 8 (MOV EBX,0x8) | CODE-CORROBORATED |
| +0x08 = file offset | align4-chained targets | fseek(stored, SEEK_SET) in FUN_0041a9d8 | CODE-CORROBORATED |
| block u32 = byte len | next = align4(off+len) | fread 4 → DAT_0054b758 = stream byte count | CODE-CORROBORATED |
| stream base off+4 | interiors resolve | FUN_0041aad0 streams `len` bytes after the len field | CODE-CORROBORATED |
| embedded .MAT @+0x10 | full envelope 60/60 | FUN_0041a1e0 (MTI parser) on buf+0x10 | CODE-CORROBORATED |
| f04/f08/f0c targets | chain 60/60 | buf+buf[0]+4 / buf+buf[1] / buf+buf[2] consumers | CODE-CORROBORATED |
| region A 3-array layout | 60/60 sane, 595 recs | FUN_00432534 reads {ca,cb,cc}; cc ≤ 0x10 | CODE-CORROBORATED |
| region B size 0x150 | 60/60 | fixed-size copy in FUN_00432534 | OBSERVED + fixed consumer |
| region C nested walk | 60/60 sane | FUN_00419ee0 stride chain {10,44,36,12} | CODE-CORROBORATED |
| f20 = innerSize-12 | 60/60 | — | OBSERVED |
| f04 == innerLen+0x10 (tA = innerEnd+4) | 60/60 | implied by layout | STRONG invariant |
| inner flags vocabulary | {0, 2, 0x20000} | MTI `flags & 0x30000` test | OBSERVED |
| index records in MTO | 0 of 483 | mechanism exists | OBSERVED-absent |
| overlay sounds cap | cc ≤ 11 | cc > 0x10 → "Too many overlay sounds" | CODE-CORROBORATED |
| name-field ASCII shape | all printable | no validation in original | OBSERVED (parser hardening) |

### OBSERVED corpus statistics

- 6 files `TRAVERSE/LEVEL{3..8}/LEVEL{3..8}O.MTO`, count=10 each →
  60 overlay blocks, one structural class, zero anomalies.
- blockLen range 0x259–0x1ad7bc.
- Embedded MTI records: 0–23 per block, 483 total; no index records.
- Region A: countA 0–19 (319 records), countB 0–13 (166), countC
  0–11 (110); 13 blocks carry the empty form (sizeA=0xc).
- Region B: 0x150 in 60/60.
- Region C: c1 1–32, c2 1–3050, c3 2–5314, c4 4–2878.

### Original loader functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_0041a84c` | MTO open + directory load: fseek(0x14), fread count, alloc `count*0xc` (tag "overlay"), fread 12-stride records |
| `FUN_0041a910` | Iterates all records, fseeks each `+0x08`, reads the block len, allocs max-block scratch |
| `FUN_0041a9d8` | Name lookup (bound 8) → fseek(rec+8, SEEK_SET) → fread 4 → block len; failure "No overlay data for %s" |
| `FUN_0041aad0` | Chunked streamer: ≤0x8000 bytes per call into the scratch buffer |
| `FUN_0041ab44` | Stream-buffer accessor |
| `FUN_00432534` | Block consumer: runs `FUN_0041a820`→`FUN_0041a1e0` (MTI parser) on buf+0x10; reads regions A/B/C; enforces cc ≤ 0x10 |
| `FUN_00419ee0` | Region-C counted-array walk ({10,44,36,12} strides + odd pad) |
| `FUN_004287cc`/`FUN_00402e2c` | Overlay-sound processing/resolution |
| `FUN_00403720` | Overlay-alien resolution ("Failed to resolve overlay alien %s") |
| `FUN_004387ec`/`FUN_00403498` | Region-A name lookups |
| `FUN_0041b7b4` | `"%s\LEVEL%d\LEVEL%dO.MTO"` path builder |
| `FUN_00433d40` | Level-bundle loader; `"TLEVEL.mto"` path |

## Proven interior directory: CMI

Evidence class: OBSERVED (6/6 `.CMI` files in BUILD_A, 884 records)
+ CODE-CORROBORATED (MDK95.EXE whole-blob loader `FUN_00425d18`,
the walker chain `FUN_0045840c`/`FUN_0045843c`/`FUN_0045846c`, the
name lookups `FUN_0045849c`/`FUN_00458550`, the table-1 consumer
`FUN_004286c8`, and the load/save relocators `FUN_00426f34`/
`FUN_00426738`).

### Naming note (what is and is not proven)

`.CMI` files carry the internal logical name `"LEVELn.CMD"` — the
disk extension and the internal extension disagree, as in every
tagged family. **No original string, source path, or consumer traced
so far expands "CMI"/"CMD" or names the format.** Earlier working
notes (and `EXECUTABLE_MAP.md` before Phase 3F) floated
collision/map/arena/BSP guesses — those labels are NOT supported by
evidence and are withdrawn. The only original-diagnostic vocabulary
established is that table 1's output feeds the destination array the
original calls the **"enemy table"** (`"Overflowed enemy table"`,
see below). Table indices and `record`/`value` stay deliberately
neutral.

### Layout

```
file offset
0x00  u32le        blob length = fileSize - 4          (envelope)
0x04  char[12]     logical name "<stem>.CMD"           (envelope)
0x10  u32le        = fileSize - 12 (envelope; equals the trailer's
                   file offset, OBSERVED 6/6)
0x14  table[0]     u32le count, then count x variable records
      table[1]     same form, starting at table[0]'s end
      table[2]     same form
      table[3]     same form
      data region  [table[3] end, size-12) — bounded only (below)
size-12  char[12]  trailer = name field repeat
```

Record form (CODE-CORROBORATED — identical stride math in all five
table functions):

```
+0x00     u8       nameLength
+0x01     byte[nameLength]  name bytes. OBSERVED 884/884: the counted
                          bytes end with exactly one trailing NUL and
                          the length INCLUDES it (e.g. len 11 =
                          "HMO_1$XD_0\0"); the original compares the
                          name as a C string at +0x01 (FUN_0042fa50),
                          so the length is the record's stride input,
                          not a bounded-compare bound
+0x01+len u32le    value — image-relative offset: the loaded image is
                          file bytes [4, size), so the target file
                          offset is 4 + value (CODE-CORROBORATED:
                          FUN_0045849c / FUN_00458550 / FUN_004286c8
                          all form blob+value and dereference it).
                          value == 0 is the null form for the table-1
                          consumer (TEST EDX,EDX in FUN_004286c8).
record stride = nameLength + 5
```

### Table chain (CODE-CORROBORATED)

The original locates the four table headers with three nested
walkers; each walker repeats "read count, advance `len+5` per
record":

- `FUN_0045840c`: count at `img+0x10` (= file 0x14), records at
  `img+0x14` → returns table[1]'s count field.
- `FUN_0045843c`: calls `FUN_0045840c`, repeats the walk → returns
  table[2]'s count field.
- `FUN_0045846c`: calls `FUN_0045843c`, repeats → returns table[3]'s
  count field.
- `FUN_004583fc`: returns `img+0x10` (table[0] header) — no static
  caller in this build; table[0]'s consumer path is UNKNOWN.

The data region follows table[3] and runs to the name trailer; the
next u32 after table[3] is data content, not a fifth count (OBSERVED
6/6 — interpreting it as a count fails immediately).

### Consumers (what each table feeds — observed call sites)

| Table | Consumer (static evidence) | Role evidence |
|---|---|---|
| table[0] | none traced (`FUN_004583fc` uncalled) | names look like `OBJ$ANIM` composites; UNKNOWN |
| table[1] | `FUN_00433d40` → `FUN_004286c8`: walks it into a 0x88-stride array at `DAT_004edcc0`, cap 0x50 | the cap diagnostic is the original string `"Overflowed enemy table"` — table 1 is therefore CODE-CORROBORATED as the enemy-name table's source. value==0 → dest flag + null pointer |
| table[2] | `FUN_004566f0` (object init): formats a name via sprintf, searches table[2], stores `blob+value` at `obj+0x108`, calls `FUN_004388d8` (tr_alcmd.c region), then clears it | per-object init-time lookup; semantics UNKNOWN |
| table[3] | `FUN_0045849c` (callers `FUN_00431fbc`, `FUN_0043394c`) and `FUN_00458550` (called once per arena record in `FUN_00433d40`, result stored at `arena+0x220`) | name → target structure in the data region |

### Data-region target head (CODE-CORROBORATED for table[3])

`FUN_0045849c` follows a table-3 record's `value` to `blob+value` and
copies TWO consecutive NUL-terminated strings there, each preceded by
its own u8 length (the second at `target+1+len1`). `FUN_00458550`
skips the first the same way, then reads the u32 after the second —
if nonzero it returns `blob+that`, else 0. So the table[3] target
head is `{u8 len, chars incl NUL} {u8 len, chars incl NUL} {u32
image-relative off}` — verified against real bytes (e.g. table[3]
`"HMO_1"` → `{"NONE"} {"H1"} {->…}`). Everything past that head —
record iteration, payload fields, region organization — is UNKNOWN
and is not enumerated.

### Load/save relocation (CODE-CORROBORATED)

`FUN_00426f34`/`FUN_00426738` add/subtract the CMI image base
(`DAT_0054c6bc`) across a large fixed field list of an in-memory
object record (`+0xec`, `+0x108`, `+0x10c`, … `+0x322`), preserving
`0xffffffff` as the in-memory null (`FUN_004262b0`/`FUN_004262c8`).
This corroborates that CMI content is offset-serialized into the
image and re-pointed at load — and that `0xffffffff` is a *memory*
convention, not a file-format value (none observed in the corpus).

### Field-by-field evidence matrix

| Element | BUILD_A bytes | Original code | Status |
|---|---|---|---|
| count chain @0x14… | 6/6 files, exactly 4 tables | walker chain `FUN_0045840c`→`43c`→`46c`; lookups read `img+0x10` count | CODE-CORROBORATED |
| record = len+name+u32 | 884 records, all walk sane | `len+5` stride in 5 functions; u32 read at `+1+len` | CODE-CORROBORATED |
| name length includes NUL | 884/884 trailing NUL, 0 inner NULs, all printable | names compared as C strings | OBSERVED + consumer-consistent |
| value = img-relative offset | 762/762 nonzero values land in the data region (6/6 files); 122 zeros, all in table[1] | `blob+value` dereferenced in `FUN_004286c8`/`49c`/`58550`; 0=null in table-1 path | CODE-CORROBORATED |
| data region = [t3 end, trailer) | 6/6 | T3 lookups land inside it | OBSERVED |
| image base = file+4 | implied by every target | `FUN_00425d18` freads u32@0 then reads `size-4` bytes into the image | CODE-CORROBORATED |

### Structural variants / anomalies

- table[0] count is 0 in LEVEL4/5/7 (records simply absent; the next
  count follows at 0x18) — a valid structural variant, not an empty
  file. 3/6 files exercise it.
- No other variants: one structural class across the corpus; every
  file has all four tables plus a large data region
  (0.94–1.67 MB, ≈99.9% of file size).
- Counts: table[0] 0–24, table[1] 51–76, table[2] 15–73,
  table[3] 11–19. Record name lengths 2–17.

### Original loader/consumer functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_00433d40` | Level-bundle loader; builds `"%s\LEVEL%d\LEVEL%d.CMI"` (sprintf fmt `0x4975c0`), calls `FUN_00425d18` → `*DAT_0054c6bc` = image, `*DAT_0054c680` = declared length; then walks table[1] into the "enemy table" array and resolves table[3] per arena record |
| `FUN_00425d18` | Whole-blob loader: open (`FUN_0041b300`), fread u32@0, alloc via callback (`FUN_0041c884`), fread `size-4` bytes → image = file+4 |
| `FUN_0045840c`/`43c`/`46c` | Table-end walkers: read count, advance `len+5` per record |
| `FUN_004583fc` | Returns `img+0x10` (table[0] header); no static caller found |
| `FUN_004286c8` | Table-1 → "enemy table" copy: name→dest+0, flag at +0xa when value==0, `blob+value`-derived pointer at +0x20; cap 0x50 → `"Overflowed enemy table"` |
| `FUN_0045849c` | Table-3 lookup → copies the target's two length-prefixed strings |
| `FUN_00458550` | Table-3 lookup → follows the target's second-level u32; returns `blob+off` or 0 |
| `FUN_004566f0` | Object init: sprintf-name → table-2 lookup → `blob+value` to `obj+0x108`, consumed by `FUN_004388d8` |
| `FUN_00426f34`/`FUN_00426738` | Load/save pointer relocation over object fields vs `DAT_0054c6bc`; `0xffffffff` null convention via `FUN_004262b0`/`FUN_004262c8` |
| `FUN_0042fa50` | Unbounded C-string compare (name lookups) |
| `FUN_0041c884` | 8-byte-aligned pool allocator (image allocation callback) |

## Proven interior structure: DTI

Evidence class: OBSERVED (6/6 `.DTI` files in BUILD_A —
`TRAVERSE/LEVEL{3..8}/LEVEL{3..8}.DTI`, 3,931,716 bytes, 114 arena
records, 60 keyed records, 633 payload sub-records) +
CODE-CORROBORATED (MDK95.EXE whole-blob loader `FUN_00425c8c`,
traversal loader `FUN_00433d40`, keyed-record lookup
`FUN_00423bf0`/`FUN_0043490c`, connect pairing `FUN_00434e54`, arena
name lookups `FUN_00432ec4`/`FUN_00432e2c`, palette upload
`FUN_0046d490`/`FUN_004346e8`, grid samplers `FUN_0046ec60`/
`FUN_0047a770`).

### Naming note (what is and is not proven)

`.DTI` files carry the internal logical name `"LEVELn.DAT"` — disk
and internal extensions disagree as in every tagged family. **No
original string expands "DTI"/"DAT".** The file is a five-section
bundle, and only these original names are proven, scoped exactly to
where the diagnostics fire:

- **"arena"** — the s2 records (`"arena %s not found"`,
  `FUN_00432ec4`; `"BSPShow %s not found"` is the lookup key in
  `FUN_00432e2c`; `"Alien has NOT changed arenas"`).
- **"connect"** — the type-6 payload sub-records (`"connect_%d"`,
  `"Mismatched connect coords/type %s,%s : connect_%d"`,
  `"Unmatched connect %s : connect_%d"`, all in `FUN_00434e54`).
- **"HotGen" / "HotPick"** — the type-2 / type-4 payload
  sub-records (`"HotGen %s not found"` / `"HotPick %s not found"`,
  `FUN_00433d40`).
- **"Cooridor"** (original spelling) — `"3 Cooridors not allowed
  for arena"`, `"Used/Avail corridor"` pool stats; the C-prefixed
  arena names (`CHMO_*`, `CMEAT_*`, …) are the corridor arenas —
  CORROBORATED by the `name[0] == 'c'/'C'` flag quirk in the load
  loop (non-C names get `+0x44 |= 3`, C names skip it).
- **palette** — s3 feeds the runtime palette array
  (`"init_palette failed"` exists for `FUN_0046d010`; the
  3-byte→4-byte upload in `FUN_0046d490` and the entry-0 black
  force at load are CODE-CORROBORATED).

DTI as a whole is NOT proven to be "the arena format" — it is the
per-level bundle that CONTAINS the arena table among five sections.

### Layout

```
file offset
0x00  u32le        blob length = fileSize - 4          (envelope)
0x04  char[12]     logical name "<stem>.DAT"           (envelope)
0x10  u32le        = fileSize - 12 (envelope; equals the trailer's
                   file offset, OBSERVED 6/6)
0x14  u32le x5     TOC: image-relative section offsets s0..s4
                   (loaded image = file bytes [4, size), so target
                   file offset = 4 + value; CODE-CORROBORATED —
                   FUN_00433d40 dereferences img + img[0x10],
                   img + img[0x18], img + img[0x1c], img + img[0x20]
                   directly; FUN_00423bf0 uses img + img[0x14].
                   OBSERVED strictly increasing, sections tiled
                   back-to-back, toc[0] = 0x24 in all 6 files)
      s0  [toc0, toc1)   parameter block — OBSERVED 0x74 = 29 u32s
      s1  [toc1, toc2)   {u32 count, count x 24-byte records}
      s2  [toc2, toc3)   {u32 count, count x 16-byte arena records}
                         then tiled payloads {u32 count,
                         count x 36-byte sub-records}
      s3  [toc3, toc4)   {u32 count, u8 rgb[768]} — OBSERVED span 772
      s4  [toc4, trailer) byte grid plane(s)
size-12  char[12]  trailer = name field repeat
```

### s0 — parameter block (CODE-CORROBORATED per-field)

All 29 words are read by `FUN_00433d40` at load:

| Word | Destination / behavior |
|---|---|
| [0] | × 0x466 into `DAT_00540c48` — initial arena index |
| [1..4] | → `DAT_00540bfc`/`c00`/`c04`/`c2c` (with saved copies `c08`/`c0c`/`c10`) — the view state `FUN_0043490c` also writes from s1 records; position-like floats + an angle (90/180/270-ish) in the corpus |
| [5],[6] | fill bytes, replicated ×4 into `_DAT_0054ec9c`/`ecac` |
| [7],[8] | → `_DAT_0054ecb8`/`ecbc` — backdrop sample base offsets (`FUN_0046ec60`) |
| [9] | → `_DAT_0054ec8c` — grid columns−4 (row stride is +4; the value itself is the wrap modulus) |
| [10] | → `_DAT_0054ec90` — grid rows |
| [0x0b] | → `_DAT_0054eca0` — secondary fill byte A; when positive (signed) a second s4 plane exists (`_DAT_0054ec98`) and fills switch |
| [0x0c] | → `_DAT_0054ecb0` — secondary fill byte B |
| [13..28] | four u32 groups transposed into a 16-byte matrix `DAT_0054c5c0`, consumed by `FUN_00406d84` ×4 — role UNKNOWN |

### s1 — keyed records (CODE-CORROBORATED stride; semantics UNKNOWN)

`{u32 count, count × 24B}`; record = `{u32 word0, u32 key, f32 f[4]}`.
`FUN_00423bf0` walks with `piVar2 += 6` (ints), matches `word[1]`
against an argument, then calls `FUN_0043490c(word[3], word[4],
word[5], word[6])` — **word[6] is the first u32 of the next record**
(for the last record it reads s2's count). OBSERVED boundary-crossing
read, reproduced in the docs rather than sanitized. Corpus: count=10
in all 6 files; `word0` runs 1..9,0; keys run 0..9; floats look like
position+heading presets. The only caller is the debug/cheat command
handler `FUN_00423ca0`, whose command strings are obfuscated — key
semantics UNKNOWN.

### s2 — arena table + payloads (CODE-CORROBORATED)

`{u32 count, count × 16B {char name[8], u32 payloadImageOff, f32
scalar}}` — count=19 in all 6 files (10 main + 9 `C*` corridor arenas).
The loader expands each record into a 0x466-stride runtime record
(`_DAT_0054c670`), copies the name, stores the scalar at +0x462,
stores payload count/array at +0x38/+0x3c, and calls the CMI table-3
lookup `FUN_00458550` once per record → `+0x220` (**the proven
CMI↔DTI link**: arena names like `HMO_*`/`MUSE_*`/`GUNT_*` are the
same names CMI table[3] keys). Non-`c`/`C` names get `+0x44 |= 3` —
the corridor-name quirk.

Each `payloadImageOff` (image-relative; file = 4+v) points at
`{u32 count, count × 36-byte sub-records}` inside s2's payload region;
payloads tile contiguously and end exactly at toc[3] in all 6 files.

Sub-record = `{u32 type, u32 field[8]}` (36 bytes). Type dispatch is
CODE-CORROBORATED:

| Type | Proven role | Evidence |
|---|---|---|
| 2 | "HotGen" | name @+0x18 (12 bytes) strcmp'd against the CMI enemy table; match index OR-ed into high half of field[1] (`"HotGen %s not found"`). Corpus: 23 records, all +0x18 names match enemy names |
| 4 | "HotPick" | same +0x18 name field; match index overwrites field[1] (`"HotPick %s not found"`). Corpus: 9 |
| 6 | "connect" | field[1] = connect-ID (>999 in file form), field[2] = side code (pairs 0↔1, 2↔3, 4↔5, 6↔7), fields[3..8] = six floats; `FUN_00434e54` pairs endpoints across arenas (same ID, identical floats, complementary sides) and rewrites field[1] to the partner arena index. Corpus: 168, all IDs 1000–1017 |
| 1,3,5,7,8,9 | UNKNOWN | OBSERVED only; histogram {1:85, 3:58, 5:171, 7:23, 8:88, 9:8} — type 9 exists only in LEVEL6 |

### s3 — RGB table (CODE-CORROBORATED as palette)

`{u32 count, u8 rgb[768]}` — span exactly 772 bytes in all 6 files.
`count` → `DAT_00540dcc` (corpus 112/112/112/64/112/112); the first
three payload bytes are zeroed at load (entry 0 forced black);
`FUN_004346e8` copies 0x90 u32s from +0xc0 (entries 64..255) to
`DAT_005408e0`; `FUN_0046d490(0, count)` expands the `count×3`-byte
RGB triplets into 4-byte entries of the runtime palette array
`DAT_0054d7b8`. "init_palette" exists in the binary.

### s4 — byte grid (CODE-CORROBORATED as backdrop image)

`(s0[9]+4) × s0[10]` bytes per plane, one plane plus a second when
`s0[0xb] > 0` (signed). The plane count × plane size equals the
section span exactly in all 6 files (1804×360 single for LEVEL3/4/7/8;
904×360 ×2 for LEVEL5/6). `FUN_0047a770` samples `s4 + plane*row +
col` into the framebuffer rows (`DAT_00541650 + y*600`) with
horizontal wrap at the s0[9] modulus; `FUN_0046ec60` blits columns.
CORROBORATED usage: scrolling indexed-color backdrop (byte values are
palette indices). Nothing semantic beyond "image bytes" is claimed.

### CMI↔DTI relationship (answers the Phase 3F open question)

`FUN_00433d40` loads `.CMI` then `.DTI`; the DTI s2 loop calls
`FUN_00458550` — the CMI table-3 name lookup — once per arena record
and stores the result at runtime `+0x220`. So the array iterated by
the CMI table-3 consumer **is** the DTI-derived arena array, and the
lookup keys are the arena names. PROVEN end to end.

### Structural variants / anomalies

- One structural class across all 6 files; only counts/sizes vary.
- s4 plane count: 1 (LEVEL3/4/7/8) vs 2 (LEVEL5/6), driven by
  `s0[0xb]` sign — OBSERVED exact-size match 6/6.
- s2 sub-record type 9 exists only in LEVEL6 (`COLYM_*` arenas).
- Zero-count payloads are normal (empty arenas store just `count=0`).
- No other anomalies: all payload offsets land inside s2's payload
  region; no trailing bytes in any section.

### Original loader/consumer functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_00433d40` | Traversal loader: loads `.DTI` after `.CMI`, walks s0/s2/s3/s4, expands s2 records into 0x466-stride runtime records, resolves HotGen/HotPick names against the CMI enemy table, calls `FUN_00458550` per arena |
| `FUN_00425c8c` | Whole-blob loader (same family as `FUN_00425d18`): fread u32@0, alloc, fread remaining bytes → image = file+4, stored at `_DAT_0054c67c` |
| `FUN_00423bf0` | s1 lookup: `piVar2 += 6` walk, `word[1]` key match, `FUN_0043490c(word[3..6])` — word[6] reads the next record |
| `FUN_0043490c` | Writes `DAT_00540bfc`/`c00`/`c04`/`c2c` — the same view-state globals s0[1..4] initialize |
| `FUN_00423ca0` | Debug-command dispatcher; the only s1 consumer's caller (obfuscated command strings) |
| `FUN_00434e54` | Connect pairing pass: matches type-6 records by connect-ID + endpoint floats + side pairing; rewrites field[1] to partner arena index; emits the three connect diagnostics |
| `FUN_00432ec4` | Arena name lookup → `"arena %s not found"` |
| `FUN_00432e2c` | Arena name lookup for the `"BSPShow %s"` key |
| `FUN_004346e8` | Post-load: copies s3+0xc0 region (0x90 u32s) to `DAT_005408e0`; builds the 4 ramp tables from `DAT_0054c5c0` via `FUN_00406d84` |
| `FUN_0046d490` | Expands count×3-byte RGB triplets into 4-byte palette entries at `DAT_0054d7b8` |
| `FUN_0046ec60` / `FUN_0047a770` | s4 samplers: column/span copies into framebuffer rows with horizontal wrap; second plane at `s4 + planeSize` when `_DAT_0054ec98` |
| `FUN_00435178` | Arena payload walker (portal/crossing tests on type-6 endpoint floats) |

## Proven interior directory: FTI

Phase 3H. `.FTI` was the first of the two length-only envelope
families — same `u32 @0 == size-4` outer rule as the tagged family but
NO logical-name field at +4 and NO name trailer (bytes [4,8) are a
small u32 count; `inspectContainer` reports `kLengthEnvelope`).

### Naming note (what is and is not proven)

The disk extension `.FTI` has no proven expansion. The resident image
global `DAT_0049ff50` belongs to the subsystem that emits
**"Font table not initialized!"** (`FUN_0047da70` path when the image
is absent) and two record names are `FONTSML`/`FONTBIG` — the original
treats this bundle as its font resource table among other things. But
the 315 payloads per file are heterogeneous: `SND_PUSH` is a RIFF/WAVE
file, `SYS_PAL` is a 192-byte palette-like table, others carry
image-like or animation-like data. So "FTI" is documented as a named
resource directory only — no semantic name is assigned to the format
or to individual payloads.

### Layout (CODE-CORROBORATED)

```
file offset
0x00  u32le    fileSize - 4                 (envelope length)
0x04  u32le    record count N               (img+0; N=315 in all 5)
0x08  N x 12   directory records            (img+4):
  +0x00 name[8]   compared as an EXACT two-u32 equality
                  (local_24 == rec[0] && local_20 == rec[1] in
                  FUN_00414890; the query is zero-padded into a
                  12-byte stack buffer). NOT NUL-terminated when the
                  name fills the field — 81/315 records per file use
                  all 8 bytes. Bytes after a NUL still participate in
                  the original compare.
  +0x08 u32       image-relative payload offset — the lookup returns
                  image + value (DAT_0049ff50 + piVar4[2])
dirEnd  payloads [dirEnd, size): OBSERVED 5/5 — stored offsets unique
        and sorted ascending in record order; smallest resolves to the
        directory end exactly; each payload spans to the next stored
        offset, the last to EOF. No stored sizes.
```

The not-found path is the shared `"Error finding %s"` fatal
(`FUN_00408fac`).

### Original loader/lookup functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_00425b34` | Whole-blob loader: fread u32@0, alloc, fread remaining → image = file+4; called with `EDX = &DAT_0049ff50` and the path in EAX (disasm of `FUN_00401abc`) |
| `FUN_00401abc` | Startup: loads `MISC\MDKFONT.FTI` into `DAT_0049ff50`; if `DAT_00541530 != 0`, builds `MISC\FONT%c.FTI` via `FUN_0047d2e9` and reloads into the same slot; `MISC\FONTG.FTI` loads via `FUN_0041b004` |
| `FUN_00414890` | THE resource lookup — 41 static callers engine-wide: walks `img+4` records with `piVar4 += 3` (12-byte stride), two-u32 name compare, returns `img + rec[+0x08]` |
| `FUN_0047da70` | Emits `"Font table not initialized!"` when `DAT_0049ff50 == 0` |
| `FUN_004149c4` | Font-record consumer (resolves `FONTSML`/`FONTBIG`) |
| `FUN_0040163c` | `SYS_PAL` consumer |
| `FUN_004236fc` / `FUN_00423734` | `SND_PUSH` consumers |

### BUILD_A FTI inventory (all 5 parse `ok`)

| File | Size | Records | Notes |
|---|---|---|---|
| `MISC/MDKFONT.FTI` | 159,954 | 315 | default bundle loaded at startup |
| `MISC/FONTF.FTI` | 161,319 | 315 | `FONT%c` language variant |
| `MISC/FONTI.FTI` | 161,055 | 315 | language variant |
| `MISC/FONTP.FTI` | 160,395 | 315 | language variant |
| `MISC/FONTS.FTI` | 161,442 | 315 | language variant |

One structural class across the corpus: same count, same record names
(the five files differ only in localized payload content), offsets
sorted+unique, first payload at dirEnd, payloads tile to EOF.

## Proven interior directory: BNI

Phase 3H. `.BNI` shares the length-only envelope with FTI but has a
DIFFERENT record layout — the two families are parallel named-resource
directories, not one format (evidence: different stride, different
name width, different compare, different image global).

### Naming note (what is and is not proven)

`.BNI` has no proven expansion. The six files are per-context bundles
(STATS/OPTIONS/FINISH screens, FALL3D, TRAVSPRT, STREAM) loaded into a
single global slot `DAT_004a1e38` — one BNI image is live at a time.
Payloads are heterogeneous (RIFF/WAVE in FINISH/STATS, a 768-byte
palette-shaped `PAL` record in STREAM, image/animation structures in
FALL3D/TRAVSPRT). No semantic name is assigned.

### Layout (CODE-CORROBORATED — instruction level)

```
file offset
0x00  u32le    fileSize - 4                 (envelope length)
0x04  u32le    record count N               (img+0; MOV EBX,[img])
0x08  N x 16   directory records            (img+4; ADD ECX,0x10):
  +0x00 name[12]  compared by the shared unbounded C-string
                  comparator FUN_0042fa50 — a well-formed name must
                  contain a NUL within the field (OBSERVED 178/178:
                  all NUL-terminated within 12 bytes; longest 9 chars,
                  "BONESANIM"). A non-terminated field would read the
                  compare into the offset word — anomaly, reported.
  +0x0c u32       image-relative payload offset — the lookup returns
                  image + value (MOV EAX,[img]; ADD EAX,[rec+0xc])
dirEnd  payloads [dirEnd, size): OBSERVED 6/6 — stored offsets unique
        and sorted ascending in record order; smallest resolves to the
        directory end exactly; each payload spans to the next stored
        offset, the last to EOF. No stored sizes.
```

The not-found path is the shared `"Error finding %s"` fatal
(`FUN_004039a4` → `FUN_00408fac`).

### Original loader/lookup functions (static evidence)

| Address | Role (observed behavior) |
|---|---|
| `FUN_004038f0` | BNI load wrapper: `MOV EDX,0x4a1e38; CALL FUN_00425a80` |
| `FUN_0040390c` | BNI load wrapper: `MOV EDX,0x4a1e38; CALL FUN_00425b34` |
| `FUN_00403928` | BNI load wrapper: `MOV EDX,0x4a1e38; CALL FUN_00425bfc` |
| `FUN_00403944` | Clears `DAT_004a1e38` (unload) |
| `FUN_00403958` | THE BNI lookup: count `[img]`, records `img+4`, stride `0x10`, `FUN_0042fa50` name compare, return `img + rec[+0x0c]` |
| `FUN_004039a4` | Lookup + `"Error finding %s"` fatal on miss |
| `FUN_004039c8` | Lookup storing the resolved pointer into a caller global — 14 call sites |
| `FUN_004039d8` / `FUN_004039ec` / `FUN_00403a00` | Lookup variants returning payload+4 / reading u16 fields at the payload head (consumers treat payload heads as structures) |
| `FUN_0040ef28` | FALL3D.BNI load site (`KURTANIM` resolver) |
| `FUN_0041b7b4` | Level-bundle loader: builds `FALL3D\FALL3D.BNI`, `%s\TRAVSPRT.BNI`, `STREAM\STREAM.BNI` paths |
| `FUN_0041d81c` | OPTIONS.BNI load site |
| `FUN_00429200` | STATS.BNI load site |
| `FUN_0042b270` | STREAM.BNI load site (`SWH150` resolver) |
| `FUN_00433d40` | TRAVSPRT.BNI load site (`H150_I` resolver) |
| `FUN_0047b0fc` | FINISH.BNI load site: streams the image chunked, resolves 5 names into 5 globals |

### BUILD_A BNI inventory (all 6 parse `ok`)

| File | Size | Records | Context |
|---|---|---|---|
| `FALL3D/FALL3D.BNI` | 2,481,256 | 67 | fall intro sequence |
| `MISC/FINISH.BNI` | 273,852 | 5 | finish screen (all payloads RIFF/WAVE-observed) |
| `MISC/OPTIONS.BNI` | 1,161,192 | 3 | options screen |
| `MISC/STATS.BNI` | 1,386,068 | 17 | stats screen |
| `STREAM/STREAM.BNI` | 1,507,653 | 29 | stream sequence (`BONESANIM` 9-char name) |
| `TRAVERSE/TRAVSPRT.BNI` | 2,333,436 | 57 | traversal sprites/resources |

One structural class across the corpus (same record layout, varying
counts); no subtypes — the heterogeneity is in payload content only.

### Shared shape vs. FTI — evaluated (Phase 3H requirement)

| Property | FTI | BNI | Same? |
|---|---|---|---|
| Envelope | u32@0 = size-4 | u32@0 = size-4 | yes (length-only) |
| Count location | img+0 | img+0 | yes |
| Directory start | img+4 | img+4 | yes |
| Record stride | 12 | 16 | **no** |
| Name field | 8 bytes, exact two-u32 compare | 12 bytes, unbounded strcmp | **no** |
| Offset position | +0x08 | +0x0c | no (different field) |
| Offset base | image-relative | image-relative | yes |
| Payload framing | sorted tiling to EOF | sorted tiling to EOF | yes |
| Image global | `DAT_0049ff50` | `DAT_004a1e38` | **no** |
| Lookup | `FUN_00414890` | `FUN_00403958` | **no** |

Verdict: **shared outer envelope + shared addressing convention only**;
the interior layouts are distinct and implemented as two separate
parsers (`src/core/fti_directory.*`, `src/core/bni_directory.*`).

### Proven BNI image payloads (Phase 4A)

Two payload layouts are CODE-CORROBORATED inside BNI records:

- **Palette-embedded bitmap** `{u8 rgb[768], u16le w, u16le h,
  u8 px[w*h]}` — the `MDKOPT`/`L1_INTRM`/`L1..L5_MAP` class. The
  consumer derives the pixel pointer as payload+0x304
  (`FUN_0041d7b4`, `FUN_0041ebf4`), blits `w*h` bytes verbatim into
  the 600x360 work surface, and uploads the 768-byte head as the
  256-entry display palette (`FUN_00416700` → `FUN_0046d208` →
  `IDirectDrawPalette::SetEntries`; `PALETTEENTRY` order proves the
  file triplets are R,G,B). Rows are top-down, stride = width, no
  transparency. All four observed payloads are 600x360 and tile
  exactly. Decoded by `src/core/bni_image.*`.
- **Indexed-only bitmap** `{u16le w, u16le h, u8 px[w*h]}` —
  `FUN_00403a00` reads the two head u16s and returns payload+4 plus
  the `w*h` count. OBSERVED byte-exact in `BG`, `SPACE`, `PLANET`,
  `MOON`, `EARTH`, `SKULL` (and `TRAVSPRT` `SKULL`). The palette is
  resolved by the consumer context (separate `PAL` records) — that
  binding is not yet proven, so this shape is classified by
  `probeBniImage` but has no decoder yet.

## Unknown fields

## Unknown fields

- `u32 @0x10` (envelope): equals `fileSize - 12` in all tagged files and
  coincides with the trailer's file offset; never read by the SNI
  loader. Semantics UNKNOWN — reported raw.
- SNI record `+0x0c`: small flag-like value set; a u16 read exists in
  one path. UNKNOWN.
- SNI sentinel `+0x10` region contents: structured binary, UNKNOWN.
- MTI record `+0x0c` (payload records): observed 0 or 0x469c4000;
  copied verbatim to in-memory +0x1c. UNKNOWN.
- MTI record `+0x10` (payload records): only 0x40600000 (3.5f) and
  0x40c00000 (6.0f) bit patterns observed; copied verbatim to
  in-memory +0x20. UNKNOWN (float-typed consumption not proven).
- MTI `+0x08` low-16 semantics (0/1/2 observed) and the payload
  header u16 fields' semantics: UNKNOWN.
- MTI index-record `+0x0c` values: name-correlated small integers;
  what they index is UNKNOWN.
- All payload contents past the proven header u16s: UNKNOWN (later
  phases).
- MTO block `+0x04`/`+0x08`/`+0x0c`: the three region target bases are
  proven (off+8+ofsA, off+4+ofsB, off+4+ofsC); why the original stores
  them as offsets rather than chaining sizes is UNKNOWN.
- MTO region-A array-A records: name+offset pairs whose semantic role
  is UNKNOWN (the overlay-alien resolution uses array B; array A's
  lookup consumers exist but their target semantics are unproven).
- MTO region-A array-C record fields other than the `+0x10` offset:
  semantics UNKNOWN (sound-processing reads them; no field names
  proven).
- MTO region B contents: fixed 0x150 bytes, palette-like; UNKNOWN.
- MTO region C record internals (44/36/12-byte arrays, the c1 name
  table's role, the trailing data): UNKNOWN.
- MTO embedded ".MAT" payloads: same UNKNOWN level as MTI payloads.
- CMI table[0] consumer path: UNKNOWN (`FUN_004583fc` has no static
  caller; the `OBJ$ANIM`-shaped names suggest a per-object animation
  role — HYPOTHESIS only).
- CMI record `value` semantics beyond "image-relative offset": what
  each table's targets ARE is UNKNOWN (table[3] targets provably begin
  with two length-prefixed strings + a second-level offset; deeper
  structure undecoded).
- CMI data region interior (≈99.9% of each file): UNKNOWN — bounded
  only. Contains the offset targets plus further structure.
- CMI "enemy table" table-1 destination record fields (0x88-stride
  array): only +0x00 name / +0x0a flag / +0x20 pointer are proven.
- The expansion of "CMI"/"CMD": UNKNOWN — no original string names it.
- DTI s0 semantics per word: proven *destinations* (above), but what
  the view params/fill bytes/ramp matrix MEAN is UNKNOWN.
- DTI s1 record semantics: key space 0..9, four floats feeding the
  view-state setter via a debug-command path — what the keys denote
  is UNKNOWN (command strings are obfuscated).
- DTI s2 `+0x0c` scalar: stored at runtime `+0x462`; role UNKNOWN.
- DTI sub-record types 1, 3, 5, 7, 8, 9: OBSERVED, semantics UNKNOWN
  (render-side consumers walk them but no per-type diagnostic names
  them; only 2/4/6 are proven).
- DTI s3 `count` when < 256: only `count` entries are uploaded — the
  remaining palette entries' provenance is UNKNOWN.
- The expansion of "DTI"/"DAT": UNKNOWN — no original string names it.
- FTI payload interiors (all 315 per file): UNKNOWN — bounded only;
  a few are self-identifying (RIFF/WAVE headers) but no per-record
  type field exists.
- FTI post-NUL name bytes: OBSERVED nonzero in some records (the
  original's two-u32 compare treats them as significant); what they
  encode beyond name matching is UNKNOWN.
- The expansion of "FTI": UNKNOWN — the subsystem diagnostic says
  "Font table", which describes the bundle's role, not the acronym.
- BNI payload interiors (records outside the image classes): UNKNOWN —
  bounded only. Two image layouts are proven (Phase 4A, above);
  payload-head u16 fields read by FUN_004039d8/9ec/a00 belong to the
  `{u16 w, u16 h, px}` bitmap family — `FUN_00403a00`'s semantics are
  now proven; the other lookup variants' consumers remain UNKNOWN.
- The expansion of "BNI": UNKNOWN — no original string names it.
- Whether FTI/BNI payloads carry trailing slack inside their spans:
  UNKNOWN — spans are inferred from the next stored offset; no stored
  per-payload sizes exist.

## Families that do NOT share the SNI layout

Checked for "same count+stride+field order" against the SNI mechanism:

- **MTI**: shares count@0x14 + 24-byte records + blob-relative offsets,
  but the record is `name[8]` + four u32s and `+0x14` is the offset
  (not `+0x10`); proven separately in Phase 3D (above).
- **MTO**: `count @0x14` + `{name[8], u32 fileOff}` 12-byte records →
  self-contained overlay blocks each embedding a full tagged `.MAT`
  file plus regions A/B/C; proven separately in Phase 3E (above).
- **CMI**: variable-length records (`u8 len + name[len] + u32`), four
  chained counted tables then a data region → different; proven
  separately in Phase 3F (above).
- **DTI**: five image-relative TOC offsets @0x14 → heterogeneous
  sections (params / keyed records / arena table / palette / grid) →
  different; proven separately in Phase 3G (above).
- **FTI**: different envelope entirely (length-only, no tag); count
  @img+0 + 12-byte `{name[8], imgOff}` records; proven separately in
  Phase 3H (above).
- **BNI**: same length-only envelope as FTI but 16-byte
  `{name[12], imgOff}` records and an unbounded name compare →
  different; proven separately in Phase 3H (above).

So each family needed its own statics+bytes pass; SNI, MTI, MTO, CMI,
DTI, FTI and BNI interiors are all proven now.

## Unsupported payload semantics

Per the phase briefs, no semantics are assigned to SNI payloads (the
RIFF/WAVE observation is byte-level only), to MTI payload data past
its proven u16 header, to MTO block contents past the proven region
boundaries, to the CMI data region past its proven target-head shape,
or to DTI sub-record payload fields past the proven type dispatch
(the renderer-side field meanings for types 1/3/5/7/8/9 are not
decoded); nothing in FTI payload interiors and nothing in BNI payload
interiors outside the two proven image layouts (Phase 4A, above) is
decoded. The parsers enumerate boundaries — they never interpret or
dump payload bytes.

## Next targets (Phase 4 candidates, not started)

1. CMI data-region organization — how records inside the region are
   delimited/iterated, and what table[0]/table[2] targets are
   (consumer-side tracing beyond the proven directory structure).
2. DTI payload type semantics — renderer-side decode of sub-record
   types 1/3/5/7/8/9 (geometry/portals/props roles are UNKNOWN; the
   `draw_arena` walker `FUN_00431300` and `FUN_00435178` are the entry
   points), plus the s1 key space.
3. Sentinel-record semantics in `.SNI` (`K_*` markers and the pointed-to
   data blocks).
4. SNI payload semantics — RIFF/WAVE vs. others; the `+0x0c` field.
5. MTI payload data interpretation past the proven header (the
   descriptor's derived shift/mask fields hint at bit-packed data;
   consumer functions needed).
6. MTO region semantics — what the embedded `.MAT` images, region-A
   arrays, and region-C arrays drive at load time (consumer-side
   tracing beyond the proven directory structure).
7. FTI/BNI payload interiors — partially resolved in Phase 4A: the two
   BNI image layouts are proven and decoded. Still UNKNOWN: all FTI
   payload organization (candidate entry points: the `FONTSML`/
   `FONTBIG` font consumers) and non-image BNI records.
8. `.LBB` structure — the only BUILD_A data family with no proven
   envelope (separate later analysis).
