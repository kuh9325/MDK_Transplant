# Data Access Layer — Phase 3B

Status: implemented and validated (2026-09-17). This layer provides
read-only, confined, case-insensitive access to a user-supplied MDK
data root, a bounded binary reader, and the first evidence-backed
container-envelope parser. No gameplay, no level parsing, no graphics
decode.

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
`u8`, `u16le`, `u32le`, `peekU32le`, `bytes(n)` (zero-copy span),
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
| interior structure | UNKNOWN | named 8-byte entries visible after offset 20+ (e.g. `DANT_1`, `AMB02`, `EXPLODE`) — a per-family directory/index is HYPOTHESIS only |
| padding/alignment | OBSERVED | name field NUL-padded to 12; no other alignment rule observed |
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
request, resolved path, size, extension family, declared u32 vs
size−4, envelope shape, tag (escaped), logical name, stem-match, and
the raw second u32 @16 (uninterpreted). `--selftest` runs a synthetic
envelope check. Links `mdk_core` — the same code the app uses. Never
prints payloads; never writes into the data root.

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

## Explicit unknowns (not implemented)

- Semantics of every interior structure: entry tables, per-family
  payloads, the size−12 field, compression/encryption (none observed).
- `.FTI`/`.BNI` directory layout (count-like u32 + 8-byte names is a
  shape observation only).
- `.LBB`, `.SAV`, `.386` formats.
- Whether a retail/GOG dump matches BUILD_A bytes.
