# Data Formats — Phase 3C

Status: first interior directory mapped (2026-09-17). This document
records evidence-backed file-format structure for the proprietary
families found under the data root. Phase 3B established the top-level
envelope; Phase 3C adds explicit file-family dispatch and the first
**proven interior directory** (`.SNI`, metadata only).

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
| MTI | `.MTI` | tagged-name | envelope-only | count @0x14 + 24-byte records `{name[8], u32, u32, u32, u32}`; non-monotonic offsets, zero/0xffffffff placeholders (OBSERVED, not decoded) |
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

## Unknown fields

- `u32 @0x10` (envelope): equals `fileSize - 12` in all tagged files and
  coincides with the trailer's file offset; never read by the SNI
  loader. Semantics UNKNOWN — reported raw.
- SNI record `+0x0c`: small flag-like value set; a u16 read exists in
  one path. UNKNOWN.
- SNI sentinel `+0x10` region contents: structured binary, UNKNOWN.
- All payload contents: UNKNOWN (later phases).

## Families that do NOT share the SNI layout

Checked for "same count+stride+field order" against the SNI mechanism:

- **MTI**: `name[8]` (not 12) + four u32s; `+0x10`-class fields hold
  small indices and float-looking values (0x40600000 = 3.5f), offsets
  non-monotonic with zero/0xffffffff placeholders → different table.
- **MTO**: `count @0x14` then `{name[8], u32}` 12-byte pairs followed by
  a second table with name references (`DANT_1.MAT`) → different.
- **CMI**: length-prefixed strings (`len byte + chars + u32`), count at
  0x14 can be 0 with a second count following → different.
- **DTI**: binary records, no names in the directory region → different.

So no second family shares the exact SNI directory mechanism as of
BUILD_A evidence; each family needs its own statics+bytes pass.

## Unsupported payload semantics

Per the phase brief, no semantics are assigned to SNI payloads (the
RIFF/WAVE observation is byte-level only), and nothing in MTO/MTI/CMI/
DTI/FTI/BNI interiors is decoded. The parsers enumerate boundaries —
they never interpret or dump payload bytes.

## Next targets (Phase 3D candidates, not started)

1. `.MTI` directory formalization (name[8] + 4 fields; correlate the
   index/offset fields with its consumer functions).
2. `.MTO` two-table interior (count + `{name8, off}` pairs + second
   table) — object-bundle map.
3. `.CMI` length-prefixed record stream (script/opcode stream?).
4. `.FTI`/`.BNI` length-envelope interior (count + 8-byte names).
5. Sentinel-record semantics in `.SNI` (`K_*` markers and the pointed-to
   data blocks).
6. SNI payload semantics — RIFF/WAVE vs. others; the `+0x0c` field.
