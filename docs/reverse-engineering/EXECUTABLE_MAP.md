# MDK Executable Map

Phase 2B — first evidence-backed static map of the original executables.
All findings below are derived from **BUILD_A** (`original/installed/`), a
third-party NoCD-modified repack classified `PROVISIONAL_RUNTIME_SOURCE`.
Nothing here asserts pristine-factory behavior. Evidence levels follow
`EVIDENCE_POLICY.md`. Function names such as `FUN_0040103c` are
auto-generated addresses, **not** semantic names; the *label* column is our
own confidence-tagged assignment (`CONFIRMED_`/`STRONG_`/`TENTATIVE_`).

Private artifacts (Ghidra project, decompiler output, call graphs) live in
`analysis-private/` and are not committed.

## Analyzed binaries

| Binary | Format | SHA-256 | Notes |
|---|---|---|---|
| `MDK95.EXE` | PE32 GUI i386, 684,544 B | `e57bd63b…ba788` | primary target; full Ghidra analysis |
| `MDKDOS.EXE` | LE (DOS/4GW) i386, 904,198 B | `7471fa6a…f591b` | secondary; analyzed via fixup-applied flat image |

Renderer-variant PEs were compared at import/string level only (no full
projects): `MDK95FF`, `MDKD3D`, `MDKD3DFF`, `MDK3DFX`, `MDKPVR`, `MDKRED`.

## Toolchain

- Ghidra **12.1.3** PUBLIC (`ghidra_12.1.3_PUBLIC_20260817.zip`, official
  SHA-256 `93a5d11a…81fd54`), headless `analyzeHeadless`.
- OpenJDK **21.0.12.1** (Homebrew `openjdk@21`, `/opt/homebrew/opt/openjdk@21`).
- Ghidra macOS arm64 natives built locally via `support/gradle/gradlew
  buildNatives` (`mac_arm_64`: decompiler, sleigh, lzfse, demanglers).
- `tools/exe_headers.py` — independent PE/LE header parser (tracked).
- Private scripts: `analysis-private/ghidra/scripts/{ExportMap,
  ExportCallGraph,DecompileFunc,ShowFunc,XrefTo,DisasmRange}.java`.
- LE loader: Ghidra 12.1.3 ships **no** LE importer (only an
  `lx.LinearExecutable` format stub). `MDKDOS.EXE` was mapped by a private
  Python tool that parsed the LE header, rebuilt the 173-page flat image,
  and applied all 22,117 internal 32-bit fixups
  (`analysis-private/ghidra/mdkdos_flat_fixed.bin`, loaded at 0x10000).
  Entry/fixup results were verified against `tools/exe_headers.py`.

## Binary layout — MDK95.EXE

| Region | VA | Notes |
|---|---|---|
| `AUTO` | `0x401000`–`0x492fff` | r-x code + r/o data (Watcom fused section) |
| `.idata` | `0x493000` | imports: 7 DLLs, 109 functions |
| `DGROUP` | `0x494000`–`0x9efff` | writable data |
| `.bss` | `0x9f000`–`0x150fff` | zero-init |
| `.reloc` | `0x151000` | relocations |

PE entry `0x47d524` = `JMP 0x480042` (Watcom trampoline). Image base
`0x400000`, timestamp 1997-06-05.

## Compiler / runtime boundary (OBSERVED)

- `WATCOM C/C++32 Run-Time system … 1988-1995` banner embedded in MDK95;
  C/C++16 banner in MDKDOS. Build-date string `Jun 04 1997`.
- PE entry → `FUN_00480042` performs Watcom CRT init (stack-growth setup,
  `GetModuleHandleA` via IAT, then `CALL 0x00401abc` = WinMain-equivalent,
  then `CALL 0x0047d4d2` = exit/cleanup). DOS entry `0x91318` begins
  `EB 76` jump over the embedded Watcom banner — same `_cstart` idiom.
- Runtime/library code concentrates at `0x47d000`–`0x492fff` (post-entry
  region: CRT init/exit, Win32 wrappers, FP/string/file helpers).
  Some game-library code (movie/resource helpers around `0x489xxx`) is
  interleaved — the exact CRT↔game seam inside that range is UNKNOWN.
- Game code occupies roughly `0x401000`–`0x47cfff`.

## Platform API boundary (OBSERVED, import-xref anchored)

| Subsystem | Anchor(s) | Evidence |
|---|---|---|
| Windowing/messages | `PeekMessageA`, `TranslateMessage`, `DispatchMessageA`, `CreateWindowExA` | `FUN_0046ceac` = message pump (≤17 msgs/frame); `FUN_0046d010` = window+DD init (`init_application/init_dd failed` strings) |
| Input | `DirectInputCreateA` | `FUN_0046bd14` creates keyboard+mouse devices (`Cannot initialise mouse/keyboard from directinput`); per-frame polls: `FUN_0046bc18` (kbd), `FUN_0046b688` (mouse), `FUN_0046b9b4` (joystick via `joyGetPosEx`) |
| Rendering | `DirectDrawCreate`, `DirectDrawEnumerateA` | `FUN_0047aa74` sets cooperative level + `SetDisplayMode 0x280×0x1e0×8` (640×480 8bpp), creates primary+back surfaces (`CreateSurface failed: %X`); `FUN_0042277c` enumerates drivers (perf/options screen) |
| Audio | `DirectSoundCreate` | `FUN_0046c198` (`DirectSound Init Failed`, `SoundInit Failed: Resources in Use/Invalid Parameter`) |
| Timing | `timeGetTime` | `FUN_0046c650` + `FUN_00489f40/48a610/48a760/48aa40` (frame/video pacing) |
| File I/O | `CreateFileA`, `ReadFile`, `FindFirstFileA`, `SetFilePointer` | wrappers throughout; `..\mdksrc\main\mdkfopen.c` path-resolution function at `FUN_0041ae50` |

Framebuffer pointer `DAT_00541650` (≈600×360 software back buffer) has ~50
xrefs — written by draw routines, consumed by the DirectDraw present path
`FUN_0046c86c`/`FUN_0046ca84` (region copy 0x168=360 rows, stride math on
surfaces).

## Candidate main loop — STRONG (OBSERVED structure)

`FUN_0040103c` (called once from WinMain `FUN_00401abc`):

- Outer `do { … } while (DAT_0054148e == 0)` — quit flag also written by
  WndProc-close path and mode handlers.
- Per iteration: `FUN_0046ceac` message pump → `FUN_004187e0` tick/input
  (which calls the kbd/joy polls) → frame delta `DAT_0049b6e8` accumulated
  into `DAT_00541518`/`_DAT_005414fc`.
- Demo-recording inner loop: `while (DAT_005414d0) { FUN_004090fc();
  FUN_004187e0(); }` (`FUN_004090fc` = `demo.c` handler, `demo\%s` files).
- Overlay-flag chain on `_DAT_0054b5d4/d8/dc/f8/fc/600` selects modal
  dialogs (save dialog, options menu, abort console, …).
- Sub-mode `switch (DAT_00541493)` cases 1–11 → per-dialog functions, each
  followed by `FUN_004026f8` (post-dialog frame op — exact role TENTATIVE).
- Primary mode `DAT_00541492` dispatch:
  - `0` frontend (`FUN_0041dc90` orchestrator),
  - `2` transition → `FUN_004103d8`/`FUN_0040fa68` then intro
    `FUN_0041d85c` or `FUN_004346e8` (writes three `0x447a0000`=1000.0f
    state floats — fall/reset init),
  - `3` traversal frame `FUN_00436100`, else `FUN_004371bc`+`FUN_0042b270`
    (stream),
  - `5` `FUN_0042c824/0x42c8b0` (stats/high-score),
  - `6` level-load `FUN_004296f0`→`FUN_004295c4`,
  - `7` traverse-load continuation,
  - `8` cinematic `FUN_0047b06c`.
- Loop tail: `FUN_0041ab50` (shutdown-time file op on a path buffer —
  exact role TENTATIVE), then `thunk_FUN_0047f47f` exits.

## Mode orchestrator — `FUN_0041dc90` (OBSERVED)

Called only from the main loop; branches to: `FUN_0041d85c` intro/frontend,
`FUN_0041b630` menu, `FUN_004202cc` save-game list, `FUN_0040ef28` FALL3D
init, `FUN_0042b270` stream, `FUN_00429200` stats, `FUN_0041b7b4` level
bundle loader, `FUN_00433d40` traversal loader, `FUN_004346e8` transition,
`FUN_0047b06c` finish/video, `FUN_0041ef74` GIF slideshow,
`FUN_004206d0` save writer.

## Subsystem map (BUILD_A)

| Subsystem | Address(es) | Confidence | Key evidence |
|---|---|---|---|
| Process/startup | `FUN_00480042`→`FUN_00401abc` | CONFIRMED_ | entry chain, CRT init, shutdown calls |
| Config | `FUN_00425de4`/`FUN_004260ac` | STRONG_ | `C:\MDK.CFG`, `MDK.CFG` xrefs |
| Input | `FUN_0046bd14`, polls `46bc18/46b688/46b9b4` | CONFIRMED_ | DirectInputCreate + device error strings |
| File I/O wrapper | `FUN_0041ae50` (`mdkfopen.c`) | STRONG_ | source-path string; used by all loaders |
| Level bundle load | `FUN_0041b7b4` | STRONG_ | `LEVEL%dO.MTO/SNI`, `LEVEL%dS.MTI`, `.CMI/.DTI`, `FALL3D.*`, `TRAVERSE.SNI`, `TRAVSPRT.BNI`, `STREAM.*`, `TLEVEL.*`, `LOAD_CPY` |
| Traversal load | `FUN_00433d40` | STRONG_ | `MISC\LOAD_%d.LBB`, `TLEVEL.*`, level families |
| FALL3D/freefall | `FUN_0040ef28` (+init `FUN_0041e070`-equiv) | STRONG_ | `fall_3d.c` string, `FALL3D_%d.MTI`, `FALLP_%d/LEVEL_%d/POD_%d` |
| Renderer (3D) | `FUN_00431300` draw_arena, `FUN_00432e2c` BSPShow, `FUN_0040bd40/…` poly sort | STRONG_ | `Overflowed MaxObjects in draw_arena`, `BSPShow %s not found`, `arena %s not found`, `Too many polygons for current sort list`, `3 Cooridors not allowed for arena` |
| Frame present | `FUN_0046c86c`, `FUN_0046ca84` | STRONG_ | surface vtable calls + 360-row copy from `DAT_00541650` |
| Audio | `FUN_0046c198` init, `FUN_00402160/…` SndPlay, `FUN_00402e2c` samples | STRONG_ | DirectSoundCreate, `SndPlay failed for %s`, `Out of sound samples`, `soundset.c` |
| Video | `FUN_0041d7b4` MDK12, `FUN_0041ebf4` PIE, `FUN_0047b0fc` MDKEND+FINISH.BNI, `FUN_0047b674` MDKBZK.MVE | STRONG_ | FLIC/MVE path strings, `Cannot open movie file` |
| Attract slideshow | `FUN_0041ef74`, enum `FUN_00417d20` | STRONG_ | `MISC\MDKS_%3.3d.GIF`, `%s*.gif`, `gifread.c`, `GIF87a` |
| Save | `FUN_004206d0` write, `FUN_00427218` validate, `FUN_00427f94` load | STRONG_ | `%.SAV`, `Mismatch Version on Save File`, `SAVE CORRUPT: …` packet diagnostics, `savegame.c` |
| Options/menus | `FUN_00420eac` (sub-mode 11), `FUN_004202cc` optload, `FUN_00420cf0` optmenu | STRONG_ | `optmenu.c/optload.c/options.c` strings, `OM_SOUND/OM_MOUSE` |
| Demo record/play | `FUN_004090fc` | STRONG_ | `demo.c`, `demo\%s`, `SAVE CORRUPT: demo file %s…` |
| Enemy/AI | `FUN_004388d8`, `FUN_00454794/4549b4`, `FUN_004574d0` | TENTATIVE_ | `Alien %s looped %d commands`, `ENEMY name %s not found`, `Unrecognised controlalien`, `tr_alcmd.c`, `allocenm.c` |
| Memory mgmt | `FUN_0041c780/41c7e8` | STRONG_ | `memblock.c`, `Total level/game memory` |
| Chunk/container | `FUN_00404084` | STRONG_ | `chunks.c` (matches u32-len+tag file family) |
| Object setup | `FUN_00428400` | TENTATIVE_ | `setupob.c` |
| Shutdown | tail of `FUN_0040103c` + `FUN_0046bef8` (input release) | OBSERVED | WndProc close → quit flag → config write → exit |
| Camera/sniper | UNKNOWN | — | `Bones tooth not found`, sniper strings exist; path not yet isolated |
| Physics/collision | UNKNOWN | — | `fan hotspot`/`controlalien` strings only |

`savegame.c` evidence: `.SAV` = header (`SAVE` tag per runtime oracle) +
version check + sequence of `(4-char tag, u32 len)` packets
(`SAVE CORRUPT: want %c%c%c%c:%d, found %c%c%c%c:%d`, `packet %d not
found`) — OBSERVED format skeleton, semantics UNKNOWN.

## File/data boundary

All path strings are `.\`-relative or `C:\MDK.CFG`-absolute; `mdkfopen`
resolves `cddata`/`hddata` roots (BUILD_A ships both as `.\`). Coordinated
per-level families `%s\LEVEL%d\…`: `.MTO` objects, `O.SNI`/`S.SNI` sound,
`S.MTI` imagery, `.CMI` collision/map, `.DTI` data. `FALL3D\FALL3D_%d.MTI`
+ `.SNI` + `.BNI` for freefall; `STREAM\` for mid-level streams;
`MISC\` for fonts/config/movies/slideshow/sound set; `demo\` for input
recordings; `SAVES\%.SAV` + `LASTGAME` for saves.

## DOS ↔ Win95 shared code — CORROBORATED

- **1,068 of 1,626** MDKDOS strings are byte-identical to MDK95 strings.
- **25 identical `..\mdksrc\` source-path strings** in both (`main\*` +
  `share\*`); DOS adds only `..\mdksrc\dos\opthmi.c`. ⇒ One shared engine
  source tree; platform layer is per-OS (`dos\` vs the Win95 layer).
- Homologous structures (same call-shape, same string clusters):

| Role | MDK95 | MDKDOS |
|---|---|---|
| Main loop body | `FUN_0040103c` | code at `0x10380–0x1089e` (mode switch via jump table `0x10320`) |
| Mode orchestrator | `FUN_0041dc90` | `FUN_0002ddf0` |
| Primary mode var | `DAT_00541492` | `DAT_0016e0a0` (same values 0/2/3/5/6/8) |
| Sub-mode var | `DAT_00541493` | `DAT_0016e0a1` |
| Demo handler | `FUN_004090fc` | `FUN_00018454` |
| Level bundle loader | `FUN_0041b7b4` | `FUN_0002babc` |
| Traversal loader | `FUN_00433d40` | `FUN_00044928` |
| FALL3D | `FUN_0040ef28` | `FUN_0001e070`→`FUN_00020f14` |
| Save validate | `FUN_00427218` | `FUN_0003780c` |
| Save list | `FUN_004202cc` | `FUN_0003083c` |
| Finish/video dispatch | `FUN_0047b06c` | `FUN_0008ee38` |
| MVE player | `FUN_0047b674` | `FUN_0008f4ac` |

- DOS-only platform code: `FUN_0007ffa4` VESA init (`VESA driver 1.2 or
  better required`), `FUN_0007eca0` HMI setup (`opthmi.c`,
  `MISC\HMISETUP.INI`, `hmidet.386`/`hmidrv.386`), INT9 scancode input,
  `JOY_*` calibration strings, `DOS4GPATH`. ⇒ video/audio/input replaced
  per platform; engine above the platform seam is shared.

## Renderer variant notes (import-level differential, OBSERVED)

| Variant | Size | Renderer imports | Delta vs MDK95 |
|---|---|---|---|
| MDK95 | 684,544 | DDRAW only | software rasterizer → DDraw present |
| MDKD3D | 671,232 | DDRAW only | identical import set — Direct3D reached via DirectDraw COM (no D3D DLL import) |
| MDK3DFX | 646,144 | `glide2x.dll` ×34 + DDRAW ×1 | 3dfx Glide path; fewer USER32/GDI32 |
| MDKPVR | 648,704 | `sgl.dll` ×12 + DDRAW | PowerVR SGL path |
| MDKRED | 651,264 | `verite.dll` ×20 + `redline.dll` ×31, **no DDRAW** | Rendition Vérité path |
| MDK95FF / MDKD3DFF | 774,656 / 678,400 | as base; −3 WINMM, +1 USER32/KERNEL32 | force-feedback builds (runtime `MDK_FF.DLL` + `.FRC` RIFF effects); drop joystick imports |

All variants keep DSOUND+DINPUT — audio/input layers are constant; only the
render module (and joystick vs FF peripheral) is swapped at link time.
`PERF_W95.EXE` (268 KB) is a standalone perf checker (DINPUT+DDRAW, no
DSOUND).

## Runtime correlations (with `RUNTIME_ORACLE.md`, all BUILD_A)

- New Game sequence `FALL3D_* → LOAD_7.LBB → LEVEL7.*` matches the static
  loader strings and mode-2→6/7 transitions. (CORROBORATED)
- `SMKT.SAV` (`SAVE` magic, 33 KB) ↔ `savegame.c` packet-writer cluster.
- Attract slideshow `MISC\MDKS_%3.3d.GIF` ↔ `FUN_0041ef74`/`FUN_00417d20`.
- DOS INT9 input ↔ no DINPUT in DOS; Win95 DInput device objects.

## Confidence summary

- CONFIRMED/STRONG: startup chain, main loop, mode dispatch, platform API
  boundaries, file-family loaders, save packet skeleton, video players,
  DOS↔Win95 shared-core architecture.
- TENTATIVE: enemy/AI module boundaries, object setup, corridor/arena
  topology rules, exact CRT↔game seam, `.BNI` semantics, freefall control
  model.
- UNKNOWN: per-packet save fields, AI command opcode set, camera/sniper
  internals, physics constants, renderer inner loop (rasterizer texturing),
  hardware-variant renderer internals, `.MVE` decoder details.

## Remaining unknowns / next targets (Phase 2C candidates)

1. Semantic decode of the mode values 0–8 and sub-modes 1–11 (runtime
   correlation in DOS oracle).
2. `.SAV` packet field map (correlate `SAVE CORRUPT` tags with live saves).
3. Arena/BSP structure of `.CMI`/`.MTI` (loader entry points known).
4. Alien command interpreter (`tr_alcmd.c` region) — opcode inventory.
5. FLIC/MVE decoder boundaries for future video playback.
6. Win95 runtime lane (blocked on licensed Windows 95) to validate the
   DDraw/DInput/DSound paths end-to-end.
7. Verify a pristine retail/GOG build to lift the BUILD_A/NoCD caveat.
