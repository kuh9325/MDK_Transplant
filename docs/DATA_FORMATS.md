# Data Formats — Phase 3E

Status: third interior directory mapped. This document records
evidence-backed file-format structure for the proprietary families
found under the data root. Phase 3B established the top-level
envelope; Phase 3C added explicit file-family dispatch and the first
proven interior directory (`.SNI`); Phase 3D added the second proven
interior directory (`.MTI`, metadata only); Phase 3E adds the third
proven interior directory (`.MTO`, metadata only — the original's
"overlay" subsystem).

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
| CMI | `.CMI` | tagged-name | envelope-only | length-prefixed name records + u32 (OBSERVED, not decoded) |
| DTI | `.DTI` | tagged-name | envelope-only | binary records, no name table (OBSERVED, not decoded) |
| FTI | `.FTI` | length only | envelope-only | count-like u32 + 8-byte names (shape observation only) |
| BNI | `.BNI` | length only | envelope-only | same shape as FTI |
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

## Families that do NOT share the SNI layout

Checked for "same count+stride+field order" against the SNI mechanism:

- **MTI**: shares count@0x14 + 24-byte records + blob-relative offsets,
  but the record is `name[8]` + four u32s and `+0x14` is the offset
  (not `+0x10`); proven separately in Phase 3D (above).
- **MTO**: `count @0x14` + `{name[8], u32 fileOff}` 12-byte records →
  self-contained overlay blocks each embedding a full tagged `.MAT`
  file plus regions A/B/C; proven separately in Phase 3E (above).
- **CMI**: length-prefixed strings (`len byte + chars + u32`), count at
  0x14 can be 0 with a second count following → different.
- **DTI**: binary records, no names in the directory region → different.

So each tagged family needs its own statics+bytes pass; SNI, MTI and
MTO are proven, CMI/DTI interiors remain undecoded.

## Unsupported payload semantics

Per the phase brief, no semantics are assigned to SNI payloads (the
RIFF/WAVE observation is byte-level only), to MTI payload data past
its proven u16 header, or to MTO block contents past the proven
region boundaries; nothing in CMI/DTI/FTI/BNI interiors is decoded.
The parsers enumerate boundaries — they never interpret or dump
payload bytes.

## Next targets (Phase 3F candidates, not started)

1. `.CMI` length-prefixed record stream (script/opcode stream?).
2. `.FTI`/`.BNI` length-envelope interior (count + 8-byte names).
3. `.DTI` binary-record interior (no name table).
4. Sentinel-record semantics in `.SNI` (`K_*` markers and the pointed-to
   data blocks).
5. SNI payload semantics — RIFF/WAVE vs. others; the `+0x0c` field.
6. MTI payload data interpretation past the proven header (the
   descriptor's derived shift/mask fields hint at bit-packed data;
   consumer functions needed).
7. MTO region semantics — what the embedded `.MAT` images, region-A
   arrays, and region-C arrays drive at load time (consumer-side
   tracing beyond the proven directory structure).
