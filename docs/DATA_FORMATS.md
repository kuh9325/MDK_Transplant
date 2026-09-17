# Data Formats — Phase 3D

Status: second interior directory mapped. This document records
evidence-backed file-format structure for the proprietary families
found under the data root. Phase 3B established the top-level
envelope; Phase 3C added explicit file-family dispatch and the first
proven interior directory (`.SNI`); Phase 3D adds the second proven
interior directory (`.MTI`, metadata only).

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
| MTO | `.MTO` | tagged-name | envelope-only | interior ≠ SNI: `u32 count @0x14` then `{name[8], u32}` pairs (12-stride) + a second table (OBSERVED, not decoded) |
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

## Families that do NOT share the SNI layout

Checked for "same count+stride+field order" against the SNI mechanism:

- **MTI**: shares count@0x14 + 24-byte records + blob-relative offsets,
  but the record is `name[8]` + four u32s and `+0x14` is the offset
  (not `+0x10`); proven separately in Phase 3D (above).
- **MTO**: `count @0x14` then `{name[8], u32}` 12-byte pairs followed by
  a second table with name references (`DANT_1.MAT`) → different.
- **CMI**: length-prefixed strings (`len byte + chars + u32`), count at
  0x14 can be 0 with a second count following → different.
- **DTI**: binary records, no names in the directory region → different.

So each tagged family needs its own statics+bytes pass; SNI and MTI
are proven, MTO/CMI/DTI interiors remain undecoded.

## Unsupported payload semantics

Per the phase brief, no semantics are assigned to SNI payloads (the
RIFF/WAVE observation is byte-level only) or to MTI payload data past
its proven u16 header; nothing in MTO/CMI/DTI/FTI/BNI interiors is
decoded. The parsers enumerate boundaries — they never interpret or
dump payload bytes.

## Next targets (Phase 3E candidates, not started)

1. `.MTO` two-table interior (count + `{name8, off}` pairs + second
   table) — object-bundle map.
2. `.CMI` length-prefixed record stream (script/opcode stream?).
3. `.FTI`/`.BNI` length-envelope interior (count + 8-byte names).
4. Sentinel-record semantics in `.SNI` (`K_*` markers and the pointed-to
   data blocks).
5. SNI payload semantics — RIFF/WAVE vs. others; the `+0x0c` field.
6. MTI payload data interpretation past the proven header (the
   descriptor's derived shift/mask fields hint at bit-packed data;
   consumer functions needed).
