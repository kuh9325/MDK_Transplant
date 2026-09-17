# Data Access Layer — Phase 3B/3C/3D/3E/3F

Status: implemented and validated. This layer provides
read-only, confined, case-insensitive access to a user-supplied MDK
data root, a bounded binary reader, the evidence-backed
container-envelope parser, an explicit file-family dispatch, and four
proven interior directory maps (`.SNI` Phase 3C, `.MTI` Phase 3D,
`.MTO` Phase 3E, `.CMI` Phase 3F — metadata only). No gameplay, no
level parsing, no graphics/audio decode.

Evidence levels follow `reverse-engineering/EVIDENCE_POLICY.md`.
Throughout: "BUILD_A" = `original/installed/` (the NoCD-repack tree;
data integrity vs a retail dump remains UNVERIFIED).

## Data-root semantics

- `--data-path DIR` selects the data root (optional; the app runs its
  synthetic diagnostic scene without it).
- ORIGINAL ENGINE OBSERVATION: the original resolves `cddata`/`hddata`
  roots from `MDK.CFG` and copies CD files to the HD/scratch area when
  needed (mdkfopen.c: `FUN_0041ae50` is a 64 KiB-chunk file copy;
  `FUN_0041ad14`/`FUN_0041adb8`/`FUN_0041ac58` join a configured root
  onto DOS-style relative paths; `FUN_0041b3c4` tries the HD copy,
  compares size/date via `FUN_0041afb8`, then reads the header and
  compares its tag to the request stem before falling back to a CD
  copy).
- NATIVE PORT DECISION: one user-supplied root, strictly read-only.
  No copy-up, no writes, no sidecars, no normalization in place.
  Everything the original would write (scratch copies, `MDK.CFG`,
  saves) is outside this layer's scope.

## Path normalization (`DataRoot::resolve`)

Request → on-disk path:

- `/` and `\` are equivalent separators; consecutive separators
  collapse; `.` components are dropped.
- `..` is rejected outright (any occurrence — simplest safe rule;
  original resource names never contain it).
- Absolute requests (`/x`, `\x`) and any `:` (drive letters, ADS) or
  NUL are rejected.
- Components are looked up case-insensitively in a per-directory index
  built lazily from `directory_iterator` and cached for the life of the
  `DataRoot`. Folding is ASCII-only (`A-Z`→`a-z`), matching DOS-era
  case semantics — e.g. `misc\load_7.lbb` resolves `MISC/LOAD_7.LBB`
  on a case-sensitive filesystem too.
- A folded name claimed by two distinct on-disk entries is reported
  **ambiguous** and rejected (cannot happen on case-insensitive
  volumes; possible on case-sensitive ones).
- The resolved candidate is `weakly_canonical`ized and must remain
  inside the canonical root — symlinks escaping the root are rejected.
  (BUILD_A contains no symlinks, so this path is synthetic-tested only.)
- Errors are structured strings: empty path / invalid character /
  missing directory / resource not found / ambiguous / escapes root.
- The directory index assumes a static tree for the process lifetime —
  acceptable for a read-only game-data root.

Read primitives: `resolve` (path only), `fileSize`, `readFile`
(whole file, caller-supplied cap), `readPrefix` (first N bytes).
All open strictly read-only.

## Binary reader (`BinaryReader`)

Bounds-checked cursor over `std::span<const std::byte>`:
`u8`, `u16le`, `u32le`, `peekU16le`, `peekU32le`, `bytes(n)` (zero-copy span),
`subReader(n)` (checked slice), `seek`/`skip`/`position`/`remaining`.
Every read returns `std::optional`; truncation yields `nullopt`, never
UB. All bounds math is `n <= remaining()` form — no overflow. No
unaligned casts; endianness-independent.

## Container envelope — established facts

From a byte-level survey of **all 57 files** in the proprietary
families in BUILD_A, cross-checked against loader code:

| Fact | Level | Detail |
|---|---|---|
| u32le @0 = fileSize − 4 | OBSERVED (57/57 across .MTO .SNI .MTI .CMI .DTI .FTI .BNI) | declared length covers everything after its own field; **excludes itself, includes the tag/name** |
| tag/name field @4 | OBSERVED (46/46 tag family) | 12-byte field `"<stem>.<ext>"`: `LEVEL7O.MAT`, `LEVEL7.CMD`, `LEVEL7O.SND`, `MDKSOUND.SND`, `STATS.MAT`, `STREAM.MAT`, `FALL3D_1.MAT`, `TRAVERSE.SND`. NUL-padded; unterminated when full |
| internal ext ≠ disk ext | OBSERVED | `.MTO/.MTI`→`.MAT`, `.SNI`→`.SND`, `.CMI`→`.CMD`, `.DTI`→`.DAT` inside the file |
| tag = name[0..4) = stem[0..4) | OBSERVED + code-corroborated | original fast-path check compares file bytes [4, 4+stemLen) vs request stem, ASCII-folded (`FUN_0042fae8`, `&0xdf` uppercase) |
| u32 @16 = fileSize − 12 | OBSERVED (46/46 tag family) | semantics UNKNOWN — possibly an inner section/chunk length covering `[12, size)`; not interpreted |
| end of envelope | OBSERVED | declared length ends exactly at EOF (single envelope per file) |
| name-field trailer | OBSERVED (46/46 tag family) | last 12 bytes of every tagged file repeat the name field; u32@16 = size−12 == that trailer's file offset |
| interior structure | OBSERVED for `.SNI`, `.MTI`, `.MTO`, `.CMI` | `.SNI` = count@0x14 + 24-byte `{name[12], u32, blobOff, size}` directory; `.MTI` = count@0x14 + 24-byte `{name[8], flags, u32, u32, blobOff}` directory with index/payload record classes; `.MTO` = count@0x14 + 12-byte `{name[8], fileOff}` overlay directory; `.CMI` = four counted variable-length tables `{u8 len, name[len], u32 imgOff}` + bounded data region — see `DATA_FORMATS.md`; other families' interiors remain UNKNOWN |
| padding/alignment | OBSERVED | name field NUL-padded to 12; `.SNI` payloads 4-byte aligned (gaps 0 or 2) |
| malformed handling | OBSERVED (code) | tag≠stem → treated as stale HD copy → re-copied from CD root (`FUN_0041ae50`) |

### chunks.c correction (Phase 2C targeted analysis)

`EXECUTABLE_MAP.md` labeled `FUN_00404084` "Chunk/container …
(matches u32-len+tag file family)". **Downgraded:** the `chunks.c`
compilation unit (0x403f6c–0x406554) is an in-memory **object-pool
allocator** — 60 nodes × 0x1aa bytes, free-list `DAT_004a1ec4`,
`FUN_00404084` recycles/releases nodes (flags @+0x186 select cleanup),
`FUN_00403f6c` allocates. It does not parse file envelopes. The real
file-boundary checks live in the mdkfopen path (`FUN_0041b3c4` et al.).
Interior file structure remains UNKNOWN — the envelope parser below is
therefore limited to the top level, per the phase brief.

## File-family dispatch (Phase 3C)

`fileFamilyForPath` (`src/core/file_family.cpp`) — case-insensitive,
extension-driven classification, the single source of truth. One
enumerator per observed BUILD_A extension family (conservative names —
extensions, not guessed semantics); `fileFamilySupport` exposes the
current parser level: `unsupported` / `envelope-only` /
`directory-metadata` / `standard-external-format`. The full matrix and
per-family interior evidence live in `DATA_FORMATS.md`.
`parserFamilyForPath` (below) now delegates to the family table so the
envelope grouping stays consistent.

## Parser applicability

`parserFamilyForPath` (extension-driven, case-insensitive):

| Family | Extensions | Envelope | Status |
|---|---|---|---|
| tag envelope | `.MTO .SNI .MTI .CMI .DTI` | u32 len + 12-byte name | parsed (`kTaggedName`) |
| length envelope only | `.FTI .BNI` | u32 len only; field@4 non-ASCII | classified, not interpreted |
| other / non-container | `.LBB` (raw), `.SAV`, standard `.FLC .MVE .GIF .FRC`, text/executables | none claimed | rejected by probe |
| unknown | everything else | — | reported as unknown |

`inspectContainer(head, totalSize)` → `ContainerInfo`:
`declaredLength`, `lengthValid` (== size−4), `shape`
(`kNone`/`kLengthEnvelope`/`kTaggedName`), raw `tag[4]`, raw
`nameField[12]`, decoded `logicalName` (tagged only), and
`nameStemMatches(info, stem)` for the original-style stem check.
Unknown tags are preserved losslessly (`tagToString` escapes
non-printables) — unfamiliar tags never fail the parse; only the
length invariant and name-field plausibility gate `kTaggedName`.

## mdk-inspect

`mdk-inspect --data-path DIR [--container] <rel-path>` prints:
request, resolved path, size, file family, envelope class, parser
support level, declared u32 vs size−4, envelope shape, tag (escaped),
logical name, stem-match, and the raw second u32 @16 (uninterpreted).

`mdk-inspect --data-path DIR --entries <rel-path>` additionally reads
the whole file (bounded) and enumerates the interior directory where a
proven parser exists — `.SNI` (Phase 3C), `.MTI` (Phase 3D), `.MTO`
(Phase 3E) and `.CMI` (Phase 3F):
entry count, directory end, trailer status, and per-record metadata.
For `.SNI`: name/raw field/blob offset/resolved file offset/size;
sentinel records (`field==size==0xffffffff`) are reported as position
markers. For `.MTI`: name[8]/raw fields `+0x08 +0x0c +0x10 +0x14`/
resolved payload file offset; index records (`+0x08==0xffffffff`) are
marked and their ignored fields reported raw; payload records show
the proven payload-header u16s (`hdr{a,b}` or `hdr{n,a,b}` for the
extended variant) and the data start offset. For `.MTO`: per-entry
name[8], block file offset/length, the embedded `.MAT` name and
record metadata, and region A/B/C counts and spans (region A's
array-B and array-C are the original's overlay-alien and
overlay-sound records). For `.CMI`: the four counted variable-length
tables (count field offset, records range, per-record offset/name/
raw value/resolved file offset), and the bounded data-region span —
record values are reported as image-relative offsets (image = file+4)
without interpreting their targets. Families without a proven
interior parser are rejected with the support level — never guessed.
`--selftest` runs synthetic envelope + SNI- + MTI- + MTO- +
CMI-directory checks. Links `mdk_core` — the same code the app uses.
Never prints payload bytes; never writes into the data root.

## Validation performed (BUILD_A, read-only)

- `traverse\level7\level7s.mti` → resolved `TRAVERSE/LEVEL7/LEVEL7S.MTI`;
  tagged-name `LEVEL7S.MAT`; stem match; u32@0 = size−4; u32@16 = size−12.
- `MISC/MDKSOUND.SNI` → `MDKSOUND.SND` (12-char unterminated name); stem match.
- `TRAVERSE/LEVEL3/LEVEL3O.MTO`, `LEVEL7.CMI`, `TRAVERSE.SNI`,
  `FALL3D/FALL3D_1.MTI` — all tagged-name, stem match.
- `misc\options.bni`, `misc\fontf.fti`, `FALL3D/FALL3D.BNI`,
  `TRAVERSE/TRAVSPRT.BNI` — length-valid envelope, non-tag field
  reported, no tag semantics claimed.
- `misc\load_7.lbb` — u32@0 mismatch → `kNone` (correctly not a
  container).
- `../outside`, `misc\no_such.x` — rejected with clear errors.
- Manifest re-verified after all inspection: 141/141 files unchanged.

Phase 3C additions (all via `mdk-inspect --entries`, read-only):

- All 15 `.SNI` files in BUILD_A enumerate `ok` — entry counts 3–53;
  `LEVEL4S.SNI`/`LEVEL6S.SNI` carry 2 and 4 `K_*` sentinel records
  respectively (offset bounds-checked, size field not a byte count).
- `LEVEL7O.MTO`, `FONTF.FTI`, `LOAD_7.LBB`, `MDK12.FLC`, `MDK95.EXE`
  correctly report `envelope-only`/`unsupported`/
  `standard-external-format` and reject `--entries`.
- Manifest re-verified after Phase 3C inspection: 141/141 unchanged.

Phase 3D additions (all via `mdk-inspect --entries`, read-only):

- All 13 `.MTI` files in BUILD_A enumerate `ok` — record counts
  40–198; every file mixes index records (`+0x08==0xffffffff`;
  `PEN_*`/`GREY*`/`NONE`/`BLACK`-style names) with payload records
  whose blob offsets tile `[dirEnd, size-12)` exactly.
- `.MTO/.CMI/.DTI/.FTI/.BNI/.LBB` correctly remain
  `envelope-only`/`unsupported` and reject `--entries`.
- Manifest re-verified after Phase 3D inspection: 141/141 unchanged.

Phase 3E additions (all via `mdk-inspect --entries`, read-only):

- All 6 `.MTO` files in BUILD_A enumerate `ok` — 10 overlay entries
  each (60 blocks): embedded `.MAT` tables (0–23 MTI records each,
  483 total), region-A records (overlay-alien refs, overlay-sound
  records ≤ the original's 0x10 cap, and the third record array),
  the fixed 0x150-byte region B, and the region-C counted arrays.
- `.CMI/.DTI/.FTI/.BNI/.LBB` correctly remain
  `envelope-only`/`unsupported` and reject `--entries`.
- Manifest re-verified after Phase 3E inspection: 141/141 unchanged.

Phase 3F additions (all via `mdk-inspect --entries`, read-only):

- All 6 `.CMI` files in BUILD_A enumerate `ok` — four counted
  variable-length tables each (record = `u8 len + name[len incl NUL] +
  u32 image-relative value`), then a bounded data region to the name
  trailer. 884 records total; every stored name ends in NUL inside the
  counted length; every nonzero value (762) lands inside the data
  region; 122 zero values, all in table[1] (the table the original's
  "Overflowed enemy table" diagnostic covers). table[0] count is 0 in
  LEVEL4/5/7 — a valid variant, not an empty file.
- `.DTI/.FTI/.BNI/.LBB` correctly remain `envelope-only`/`unsupported`
  and reject `--entries`; `.SNI/.MTI/.MTO` keep their own parsers.
- Manifest re-verified after Phase 3F inspection: 141/141 unchanged.

## Explicit unknowns (not implemented)

- Interior structure of `.DTI` (shape observations
  recorded in `DATA_FORMATS.md`; not decoded).
- `.CMI` data-region interior (≈99.9% of each file): the record
  values' targets and the region's own organization — bounded only.
  CMI table[0]'s consumer and the semantic role of each table past
  table[1]'s "enemy table" diagnostic remain UNKNOWN.
- `.MTO` region semantics: what region A's array-A records name, the
  region-C arrays' roles, and all payload contents past the proven
  boundaries (UNKNOWN — see DATA_FORMATS.md).
- `.MTI` payload data past the proven u16 header, the `+0x08` low-16
  flag semantics, the `+0x0c`/`+0x10` param semantics, and what index
  records' `+0x0c` values index (all UNKNOWN — see DATA_FORMATS.md).
- `.SNI` payload contents (mostly RIFF/WAVE by byte inspection — not
  decoded), the record `+0x0c` field, and the `K_*` sentinel records'
  marked regions.
- Semantics of the u32@16 field (equals size−12 == trailer offset in
  all tagged files; never read by the SNI loader) and the 12-byte
  trailer itself.
- `.FTI`/`.BNI` directory layout (count-like u32 + 8-byte names is a
  shape observation only).
- `.LBB`, `.SAV`, `.386` formats.
- Compression/encryption (none observed anywhere).
- Whether a retail/GOG dump matches BUILD_A bytes.
