# Original MDK Builds — Strategy

Status: Phase 1 inventory complete for one supplied build tree (BUILD_A,
below). Everything not covered by that inventory remains UNKNOWN until
verified from user-provided files or authoritative technical documentation.

## Historical platform releases (documented at a high level)

MDK (1997, Shiny Entertainment) is documented to have shipped on multiple
platforms, including at least:

- **DOS x86** — the original PC release
- **Windows x86** — a Windows-targeted build
- **Classic Mac OS / PowerPC** — the contemporary Mac port

Console ports (e.g., PlayStation) are also DOCUMENTED to exist but are out of
scope unless a specific need arises.

> We do NOT claim exact executable structures, linker layouts, or toolchain
> fingerprints for any of these builds. Those must be established per-build
> from actual files (see `docs/research/ORIGINAL_DATA_CHECKLIST.md`).

## Analysis strategy

Each original build serves as an **oracle** — a reference implementation whose
observable behavior constrains our reimplementation:

| Build | Role |
|---|---|
| DOS x86 | Primary **behavioral oracle**: game logic, timing, data formats |
| Windows x86 | Secondary **behavioral + platform oracle**: API usage, portability differences vs DOS |
| Classic Mac OS / PPC | **Cross-platform comparison oracle**: agreement/disagreement with x86 builds is high-value evidence for what is game logic vs platform accident |

Rationale: where DOS, Windows, and Mac builds agree on a data layout or
behavior, that behavior is very likely engine-level rather than platform-level.
Where they disagree, the disagreement itself localizes platform abstraction
boundaries.

## Independence requirement

- Reimplementation code must **not** depend on original executable code at
  runtime — no wrapping, no dynamic loading of original binaries, no embedded
  original code.
- Original executables are used **offline only**, as analysis subjects and
  behavioral oracles.
- Original executables are never modified, patched, or redistributed.

## Supplied builds — Phase 1 inventory (2026-09-16)

### BUILD_A — `original/installed/`

An installed PC game tree: 141 files, 194,445,862 bytes total, of which
122 files / 164,755,326 bytes are original-game candidates (the remainder
is repack additions and prior-user state, enumerated below).
Full SHA-256 manifest: `analysis-private/manifests/BUILD_A/manifest.json`
(ignored). Per-file format identification:
`analysis-private/inventories/BUILD_A.json` (ignored).

**Provenance caveat (OBSERVED):** the tree's own `_README.TXT` identifies
it as a third-party repack distributed with DOSBox preconfigured and with
all executables "NoCD-Fixed". Consequences:

- The executables are **modified** relative to factory state. Byte-level
  code observations from them must not be assumed pristine.
- Data-file integrity vs a retail/GOG dump is UNVERIFIED. Before
  oracle-grade behavioral work, obtaining a verifiably clean copy (original
  CD image or GOG release) is recommended.
- `MDK.EXE` in this tree is **not the game**: it is a renamed DOSBox 0.72
  Win32 binary (OBSERVED: PE32 timestamp 2007-08-27, embedded
  "DOSBox 0.72" strings, imports SDL.dll/SDL_net.dll/OPENGL32.DLL).
  `DOSBOX.CONF` autoexec mounts `.` as drive B: and runs `MDKDOS.EXE`.
- The original DOS game executable is present as `MDKDOS.EXE` (STRONG
  EVIDENCE: LE module name `mdk`; repack readme rename claim; original
  `READMEE.TXT` documents the DOS version being launched as "MDK").

**Runtime status (Phase 2A):** BUILD_A is in use as
`PROVISIONAL_RUNTIME_SOURCE`. `MDKDOS.EXE` launches to the main menu
under DOSBox-X 2026.08.31 from a disposable copy — see
`RUNTIME_ORACLE.md`. All runtime observations remain PROVISIONAL pending
a verified-clean build.

#### Executables (all x86 32-bit; OBSERVED via headers/imports)

| File | Size | Format | Role (evidence) |
|---|---|---|---|
| `MDKDOS.EXE` | 904,198 | LE, DOS/4GW-bound | DOS game exe — module name `mdk` (STRONG) |
| `MDK95.EXE` | 684,544 | PE32 GUI | Win95 software build (STRONG: filename + imports + patch readme) |
| `MDK95FF.EXE` | 774,656 | PE32 GUI | Win95 + force feedback (STRONG) |
| `MDKD3D.EXE` | 671,232 | PE32 GUI | Direct3D build (STRONG: filename + `MDKD3D.TXT`) |
| `MDKD3DFF.EXE` | 678,400 | PE32 GUI | Direct3D + force feedback (STRONG) |
| `MDK3DFX.EXE` | 646,144 | PE32 GUI | Glide/3dfx build (CONFIRMED: imports `glide2x.dll`) |
| `MDKPVR.EXE` | 648,704 | PE32 GUI | PowerVR build (CONFIRMED: imports `sgl.dll`) |
| `MDKRED.EXE` | 651,264 | PE32 GUI | Rendition build (CONFIRMED: imports `verite.dll` + `redline.dll`) |
| `MDKZAP.EXE` | 38,116 | LE, DOS/4GW | sound-config reset utility (STRONG: module `mdkzap`, embedded MDK.CFG messages) |
| `PERF_DOS.EXE` | 380,643 | LE, DOS/4GW | DOS perf utility (STRONG: module `perf_dos`) |
| `PERF_W95.EXE` | 268,800 | PE32 GUI | Win95 perf utility (STRONG) |
| `DOS4GW.EXE` | 265,420 | MZ | DOS/4GW extender program (CONFIRMED: embedded Rational Systems banners) |
| `MDK_FF.DLL` | 58,368 | PE32 DLL | force-feedback support (CONFIRMED: imports DINPUT; `MDK95FF.TXT`) |
| `HMIDET.386`, `HMIDRV.386` | 92,928 / 281,934 | custom | HMI SOS audio detect/driver for DOS build (STRONG: referenced by `MDKDOS.EXE` strings + `HMISETUP.INI`) |
| `DOSWINKY/DOSWINKY.VXD` | 5,208 | LE VxD | Win9x VxD helper (TENTATIVE role; `DOSWINKY.INF` is its install script) |

SHA-256 of primary game executables:

```
MDKDOS.EXE   7471fa6abce220d526d96b826529aba1fa280681a6c6752716c983ba27df591b
MDK95.EXE    e57bd63ba6af1ce201e3dd28a352e36ffa737267a9be0ed733fb81e6c40ba788
MDK95FF.EXE  8a97255551ab90d1c8141857f9c6767a95ea57a1b617c5708af5ca5de9046f36
MDKD3D.EXE   f7ce0a80a699faf4471bb3283865123938918b650934cb97f4a1715af8960bfc
MDKD3DFF.EXE 78affe7b5218c3a18dc39e57fa2f797bedfdd69e209982d7b04df8a3220e078d
MDK3DFX.EXE  52b425809a7a55e4baf18abf9413ec6d273ea449ca0a4b07667e739a36c907b3
MDKPVR.EXE   2d22322acd688f6a4abfecd8d6dfdd77719c47caf6dfdade577eb1e48f561c26
MDKRED.EXE   e1a729fd4fb52c67585a9b8702071a7b30dade962b6642cc00a43560df9704d5
```

PE timestamps (OBSERVED): MDK95/MDK3DFX/MDKRED 1997-06-05, MDKD3D
1997-06-30, MDKPVR 1997-09-15, MDK95FF/MDKD3DFF 1997-10-06,
MDK_FF.DLL 1997-07-11, PERF_W95 1997-02-03. The included official patch
readmes (`MDK*.TXT`, dated May–June 1997) document this install as having
the post-release patches: F2 save key, sound crash fix, SpaceOrb 360
support (DOS), Direct3D beta, Glide beta, PowerVR beta, Rendition beta,
and force feedback (DirectX 5 required).

#### Toolchain evidence

- OBSERVED: "WATCOM C/C++32 Run-Time system" banner embedded in
  `MDKDOS.EXE` and `MDK95.EXE`; C/C++16 banner also present in
  `MDKDOS.EXE`/`MDKZAP.EXE`.
- STRONG EVIDENCE (Watcom wlink): all game PEs use section names
  `AUTO`/`DGROUP`/`BEGTEXT` and have `VirtualSize == 0` in section
  headers (sizes live in `SizeOfRawData`), which breaks naive RVA
  mapping in stock `objdump`.
- OBSERVED: DOS executables are LE format bound to DOS/4GW; the DOS extender
  is DOS/4GW (Rational Systems; `DOS4GW.EXE` bundled alongside).

#### Dependencies (OBSERVED from import tables / embedded references)

- All Windows game executables statically import `DDRAW.dll`,
  `DSOUND.dll`, `DINPUT.dll`, `WINMM.dll` plus kernel/user/gdi —
  DirectX-era APIs. `MDKD3D.EXE` has no separate D3D import (DX3-era
  Direct3D is reached through DirectDraw; consistent with its readme).
- Variant-specific: `glide2x.dll` (3dfx), `sgl.dll` (PowerVR SGL),
  `verite.dll` + `redline.dll` (Rendition). None of these, nor the
  DirectX DLLs, are bundled — all are external runtime dependencies.
- Force feedback: `MDK_FF.DLL` (imports DINPUT) + 13 `*.FRC` files in
  RIFF `FORC` form (DirectInput force-effect files).
- DOS build: no import modules (statically bound LE image); loads HMI
  `.386` drivers at runtime (`hmidet.386`, `hmidrv.386`,
  `MISC\HMISETUP.INI` — "HMI Sound Operating System", (C) 1995 Human
  Machine Interfaces Inc.).
- No Smacker/Miles/Indeo/QuickTime evidence found in the supplied files;
  the `.MVE` cinematic is an Interplay MVE movie and `.FLC` files are
  Autodesk FLIC animations (magic 0xAF12).

#### Data-file families (OBSERVED header-level facts only)

Most proprietary families begin with a u32 LE length field (file size − 4)
followed by a 4-char ASCII tag equal to the file stem (verified for .MTI,
.SNI, .CMI, .DTI, .MTO — `.FTI` was on this list at the early survey but
its second field is a non-ASCII u32 count, not a tag: corrected Phase 3B).
Semantics beyond the tag were UNKNOWN at
this survey; interior directories have since been proven for `.SNI`
(Phase 3C), `.MTI` (3D), `.MTO` (3E), `.CMI` (3F), `.DTI` (3G), and the
two length-envelope families `.FTI`/`.BNI` (3H — distinct record layouts,
see `../DATA_FORMATS.md`). .LBB files
(6 × 40,772 bytes, `LOAD_*`) look like raw data — format UNKNOWN.

| Family | Count | Total bytes | Notes |
|---|---|---|---|
| .MTO | 6 | 45,561,671 | `LEVEL{3..8}O.MTO`, tag `LEVE` |
| .SNI | 15 | 27,304,832 | incl. `MDKSOUND.SNI`; tags `FALL`/`LEVE`/`MDKS` |
| .MTI | 13 | 23,043,544 | `FALL3D_{1..5}.MTI`, `LEVEL*S.MTI`, `STATS.MTI`, `STREAM.MTI` |
| .BNI | 6 | 9,143,457 | per-context resource directories `FALL3D`, `FINISH`, `OPTIONS`, `STATS`, `STREAM`, `TRAVSPRT` |
| .CMI | 6 | 8,323,957 | `LEVEL{3..8}.CMI`, tag `LEVE` |
| .DTI | 6 | 3,931,716 | `LEVEL{3..8}.DTI`, tag `LEVE` |
| .FLC | 2 | 8,459,481 | FLIC animation 600x360x8 (`MDK12`, `MDKEND`) |
| .MVE | 1 | 29,813,692 | Interplay MVE bonus FMV |
| .FTI | 5 | 804,165 | engine-wide resource directory ("font table" diagnostic); `MDKFONT` + `FONT{F,I,P,S}` variants |
| .LBB | 6 | 244,632 | `LOAD_{3..8}` loading data, format UNKNOWN |
| .FRC | 13 | 3,744 | RIFF `FORC` force-effect files |
| .GIF | 10 | 912,107 | `MDKS_001..010` screenshots 600x360 |
| .SAV | 2 | 61,415 | prior-user savegames, binary, format UNKNOWN |
| .386 | 2 | 374,862 | HMI audio drivers, custom format |
| .CFG/.INI/.CONF/.TXT/.INF/.ICO/.PDF/.WMV/.DB | — | — | configs, docs, repack extras |

Directory layout (OBSERVED): `FALL3D/` (freefall sections),
`TRAVERSE/LEVEL3`..`LEVEL8/` (six traverse levels; no LEVEL1/LEVEL2
directories exist in this tree), `MISC/` (fonts, FLIC, load screens,
options/stats, sound), `STREAM/`, plus repack dirs `DOSWINKY/`, `ZMBV/`,
`EXTRAS/`, `SAVES/`.

#### Non-original content in BUILD_A (repack additions)

`MDK.EXE` (DOSBox 0.72), `DOSBOX.TXT`, `DOSBOX.CONF`, `SDL.DLL`,
`SDL_NET.DLL`, `ZMBV/` (capture codec), `EXTRAS/` (manual PDF,
walkthroughs, cheats, WMV re-encode of the bonus FMV), `MISC/Thumbs.db`,
`SAVES/` (prior-user saves), `MDK.CFG` (generated config), `.DS_Store`.

#### Known-absent / open items

- `MISC\FLIC\SHINY.FLC` is referenced by both `MDKDOS.EXE` and
  `MDK95.EXE` but is absent from this tree — possibly CD-only content or
  dropped by the repack (UNKNOWN).
- No Macintosh build supplied; no CD-ROM image or installer supplied.
- Whether game data is byte-identical to a retail install: UNVERIFIED.

## Confirmed facts vs research questions

Confirmed (this session):
- Multiple platform builds exist at the level documented above.
- BUILD_A supplies DOS + Windows x86 executables (multiple renderer
  variants) plus full game data for one installed tree (see above).

Remaining research questions:
- Exact Watcom version and precise per-build toolchain fingerprint
- Whether DOS/Windows share a common logic core or differ substantially
- How the Mac port relates architecturally to the PC builds
- File format versions per build and whether they are interchangeable
- Whether BUILD_A data files are byte-identical to a pristine retail
  install (repack is self-described as patched/NoCD-fixed)

See `RESEARCH_QUESTIONS.md` for the full open-question list.
