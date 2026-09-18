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
| Config | `FUN_00425de4` (reader — one caller, WinMain `FUN_00401abc`@`0x401c72`: copies 670-byte factory defaults `0x49afe4`→live `0x5411f0`, then applies `name = value` lines through the 92-entry `{name,type,value_ptr}` table `0x49aca8`, key match `FUN_0042fab4` ASCII `and 0xdf` fold), `FUN_004260ac` (writer — from `FUN_00420d68` iff dirty `DAT_00541486`; `; MDK Configuration file automatically generated by MDK` header + delta `name = value` vs defaults mirror; fmt `int %d`/`str %s`/`hex 0x%X`/`float %g`/`bool TRUE/FALSE`; dirty cleared unconditionally @`0x420dbc`; fopen-fail silent) | STRONG_ | `C:\MDK.CFG`, `MDK.CFG`, format strings `0x49681c..0x4968ac`; table entry 88 = `{"Skill",int,&DAT_0054147a}` default mirror byte `0x49b26e`=1 — Phase 4G, see `../ENGINE_RECONSTRUCTION.md` |
| Input | `FUN_0046bd14`, polls `46bc18/46b688/46b9b4` | CONFIRMED_ | DirectInputCreate + device error strings |
| File I/O wrapper | `FUN_0041ae50` (`mdkfopen.c`) | STRONG_ | source-path string; used by all loaders |
| Level bundle load | `FUN_0041b7b4` | STRONG_ | `LEVEL%dO.MTO/SNI`, `LEVEL%dS.MTI`, `.CMI/.DTI`, `FALL3D.*`, `TRAVERSE.SNI`, `TRAVSPRT.BNI`, `STREAM.*`, `TLEVEL.*`, `LOAD_CPY` |
| .SNI directory | `FUN_00428a0c` (stream loader), `FUN_004259a8` (whole-blob loader), `FUN_00428c90`/`FUN_00429014` (entry lookup + payload read), `FUN_00428be8`/`FUN_00428828` (iterate) | STRONG_ | count u32@0x14, N×24-byte records `{name[12], u32@+0x0c, blobOff@+0x10, size@+0x14}`; seek `stored+4` SEEK_SET; name compare ≤12 (`MOV EBX,0xc`→`FUN_0042fa80`); blob image = file+4 — Phase 3C, see `../DATA_FORMATS.md` |
| .MTI directory (material table, `loadmats.c`) | `FUN_0041a1e0` (table parser — shared), `FUN_0041a4d0` (level `LEVEL%dS.MTI` path via `FUN_00425c8c`), `FUN_0041a480` (`STATS/STREAM/FALL3D_%d.MTI` via `FUN_00425bfc`), `FUN_0041a820` (second table `DAT_0054b730`), `FUN_0041a590`/`FUN_0041a5ec`/`FUN_0041a694` (name lookups, 0x34-stride in-memory records, name at +0x28) | STRONG_ | count u32@0x14, N×24-byte records `{name[8], flags@+0x08, u32@+0x0c, u32@+0x10, blobOff@+0x14}`; `+0x08==0xffffffff` = index record (only `+0x0c` read); else `+0x14` dereferenced blob-relative (file pos = stored+4); flags `&0x30000` select 4- vs 8-byte payload u16 header; strings `matdef`/`matlkup`/`Texture %s not in material list` — Phase 3D, see `../DATA_FORMATS.md` |
| .MTO overlay directory | `FUN_0041a84c` (dir load), `FUN_0041a910` (max-block scan → scratch alloc), `FUN_0041a9d8` (name lookup → fseek + len read), `FUN_0041aad0`/`FUN_0041ab44` (≤0x8000-chunk stream + accessor), `FUN_00432534` (block consumer → `FUN_0041a820`→`FUN_0041a1e0` on buf+0x10), `FUN_00419ee0` (region-C walk), `FUN_00403720` (overlay-alien resolve), `FUN_004287cc`/`FUN_00402e2c` (overlay sounds), `FUN_004387ec`/`FUN_00403498` (region-A lookups), `FUN_0041b7b4` (`LEVEL%dO.MTO` path) | STRONG_ | count u32@0x14, N×12-byte records `{name[8], fileOff@+0x08}` (fseek SEEK_SET, name compare bound 8); block u32 = self-inclusive byte len, streamed from off+4; each block embeds a tagged `.MAT` image at +0x10 (parsed by the shared MTI parser) + region A `{ca,cb,cc}` (overlay-alien/overlay-sound records, cc≤0x10 → `"Too many overlay sounds"`) + fixed 0x150-byte region B + nested region C ({10,44,36,12}-stride counted arrays, 2-byte pad iff c1 odd); strings `overlay`, `No overlay data for %s`, `Failed to resolve overlay alien %s` — Phase 3E, see `../DATA_FORMATS.md` |
| .CMI table directory | `FUN_00425d18` (whole-blob load into `DAT_0054c6bc`, length → `DAT_0054c680`), `FUN_0045840c`/`FUN_0045843c`/`FUN_0045846c` (table-end walkers), `FUN_0045849c`/`FUN_00458550` (table-3 name lookups + indirection), `FUN_004286c8` (table-1 → `DAT_004edcc0` 0x88-stride array, cap 0x50), `FUN_004566f0` (table-2 lookup in object init), `FUN_00426f34`/`FUN_00426738` (load/save pointer relocation vs the image base) | STRONG_ | four counted variable-length tables `{u8 len, name[len incl NUL], u32 imgOff}` @0x14 then a data region to the trailer; values are image-relative offsets (image = file+4); `"Overflowed enemy table"` caps table 1; table-3 targets begin `{lenStr, lenStr, u32}` — Phase 3F, see `../DATA_FORMATS.md`. **Note:** the pre-3F "collision/map" guess below was wrong — no arena/BSP structure is proven for `.CMI` |
| .DTI sectioned bundle | `FUN_00425c8c` (whole-blob load → `_DAT_0054c67c`), `FUN_00433d40` (traversal loader — consumes s0 params, expands s2 arena records into 0x466-stride runtime records, resolves HotGen/HotPick names, calls CMI table-3 lookup `FUN_00458550` per arena → `+0x220`), `FUN_00423bf0`/`FUN_0043490c` (s1 keyed-record lookup → view-state globals; called only from the debug-command dispatcher `FUN_00423ca0`), `FUN_00434e54` (connect pairing: matches type-6 records by connect-ID + endpoint floats + side codes 0↔1/2↔3/4↔5/6↔7, rewrites field[1] to partner arena index), `FUN_00432ec4`/`FUN_00432e2c` (arena-name lookups), `FUN_004346e8`/`FUN_0046d490` (s3 palette: `count×3`-byte RGB → 4-byte entries `DAT_0054d7b8`, entry 0 forced black), `FUN_0046ec60`/`FUN_0047a770` (s4 grid sampling into the framebuffer, horizontal wrap, optional second plane) | STRONG_ | five image-relative TOC offsets @0x14 (image = file+4) tiling s0..s4: 0x74-byte params / `{count, count×24B}` keyed records / `{count, count×16B {name[8], imgOff, f32}}` arena table + `{count, count×36B}` typed payloads (2=HotGen, 4=HotPick, 6=connect per original diagnostics; types 1/3/5/7/8/9 UNKNOWN) / `{count, 768B RGB}` palette / `(cols+4)×rows` grid, 1–2 planes — Phase 3G, see `../DATA_FORMATS.md` |
| .FTI resource directory | `FUN_00425b34` (whole-blob load into `DAT_0049ff50` — `MISC\MDKFONT.FTI` default / `MISC\FONT%c.FTI` language variant via `FUN_0047d2e9`; `MISC\FONTG.FTI` via `FUN_0041b004`), `FUN_00414890` (name lookup — 41 static callers), `FUN_0047da70` (`"Font table not initialized!"`), `FUN_00408fac` (`"Error finding %s"`) | STRONG_ | length-only envelope (no tag/trailer); count u32 @img+0, N×12-byte records `{name[8], u32 imgOff}` @img+4; name compared as exact two-u32 equality (query zero-padded); lookup returns `img + rec[+0x08]`; payloads tile to EOF — Phase 3H, see `../DATA_FORMATS.md` |
| .FTI font payloads (FONTSML/FONTBIG) | `FUN_004149c4` (resolver: `F8`→`DAT_0054164c`, `FONTSML`→`DAT_00541644`, `FONTBIG`→`DAT_00541648`; sets `DAT_0049a76c` init flag), `FUN_00414d88`/`FUN_00414dd4`/`FUN_00414f1c` (FONTSML measure/draw/centered — ~40 call sites), `FUN_00414be8`/`FUN_00414c34`/`FUN_00414f64` (FONTBIG measure/1:1 draw/scaled draw), `FUN_00423b38` (options-scale text via `FUN_00423a24`→`FUN_00414f64`), `FUN_00414a08`/`FUN_00414ac0` (F8 mask path — different format) | STRONG_ | font record = `u32 offsetTable[256]` indexed by byte + `{s8 top, s8 bottom, u8 width, u8 px[w*(top+bottom+1)]}` glyphs; byte 0 transparent, nonzero = verbatim palette index; advance = width, missing entry +4 (FONTSML)/+6 (FONTBIG); glyph indices <= 62 (resident SYS_PAL head) — Phase 4C, see `../DATA_FORMATS.md` |
| .FTI sprite payloads (ARROW) | `FUN_004236c0` (front-end cursor-arrow draw: resolves `ARROW` via `FUN_00414890`, caches content+4 in `DAT_0049ac78`, draws frame 0), `FUN_00409760` (shared sprite-header reader: `{u16 w,u16 h,s16 hotX,s16 hotY}` → dest = pos−hotspot), `FUN_00415ff0` (command-stream blit into `DAT_00541650`) | STRONG_ | record = `u32 blockBytes, u32 frameCount, u32 frameOffset[]` (+4-relative) then per frame `{u16 w,u16 h,s16 hx,s16 hy,stream}`; stream ops: `<0x80` literal (cmd+1 idx bytes, 0=transparent), `0x80-0xfd` run (cmd−0x7c copies of one value, 0=transparent run), `0xfe` row break, `0xff` end; pixels = final palette indices — Phase 4D, see `../DATA_FORMATS.md` |
| .BNI resource directory | `FUN_004038f0`/`FUN_0040390c`/`FUN_00403928` (load wrappers — `MOV EDX,0x4a1e38` → `FUN_00425a80`/`FUN_00425b34`/`FUN_00425bfc`), `FUN_00403944` (slot clear), `FUN_00403958` (name lookup), `FUN_004039a4`→`FUN_00408fac` (`"Error finding %s"`), `FUN_004039c8`/`FUN_004039d8`/`FUN_004039ec`/`FUN_00403a00` (lookup variants incl. payload-head u16 readers), `FUN_0047b0fc` (FINISH.BNI stream load → 5 name resolutions) | STRONG_ | length-only envelope (no tag/trailer); count u32 @img+0, N×16-byte records `{name[12], u32 imgOff}` @img+4; name via unbounded `FUN_0042fa50` strcmp; lookup returns `img + rec[+0x0c]`; single global slot `DAT_004a1e38` (one live BNI at a time — per-context bundles); payloads tile to EOF — Phase 3H, see `../DATA_FORMATS.md` |
| Traversal load | `FUN_00433d40` | STRONG_ | `MISC\LOAD_%d.LBB`, `TLEVEL.*`, level families |
| FALL3D/freefall | `FUN_0040ef28` (+init `FUN_0041e070`-equiv) | STRONG_ | `fall_3d.c` string, `FALL3D_%d.MTI`, `FALLP_%d/LEVEL_%d/POD_%d` |
| Renderer (3D) | `FUN_00431300` draw_arena, `FUN_00432e2c` BSPShow, `FUN_0040bd40/…` poly sort | STRONG_ | `Overflowed MaxObjects in draw_arena`, `BSPShow %s not found`, `arena %s not found`, `Too many polygons for current sort list`, `3 Cooridors not allowed for arena` |
| Frame present | `FUN_0046c86c`, `FUN_0046ca84` | STRONG_ | surface vtable calls + 360-row copy from `DAT_00541650` |
| Audio | `FUN_0046c198` init, `FUN_00402160/…` SndPlay, `FUN_00402e2c` samples | STRONG_ | DirectSoundCreate, `SndPlay failed for %s`, `Out of sound samples`, `soundset.c` |
| Video | `FUN_0041d7b4` MDK12, `FUN_0041ebf4` PIE, `FUN_0047b0fc` MDKEND+FINISH.BNI, `FUN_0047b674` MDKBZK.MVE | STRONG_ | FLIC/MVE path strings, `Cannot open movie file` |
| Attract slideshow | `FUN_0041ef74`, enum `FUN_00417d20` | STRONG_ | `MISC\MDKS_%3.3d.GIF`, `%s*.gif`, `gifread.c`, `GIF87a` |
| Save | `FUN_004206d0` write, `FUN_00427218` validate, `FUN_00427f94` load | STRONG_ | `%.SAV`, `Mismatch Version on Save File`, `SAVE CORRUPT: …` packet diagnostics, `savegame.c` |
| Options/menus | `FUN_00420eac` (sub-mode 11 — options sub-menu: framebuffer clear via `FUN_00415658`/`FUN_0047d20a` + OM_* labels + ARROW, no backdrop; input order prev→next→mouse→Esc→LEFT→RIGHT→activate→draw; every dispatch branch RETs before draw/timing — Phase 4F), `FUN_00420cf0` (options entry: `DAT_00541493=0x0b`, `_DAT_0054bd34=8`, palette flattened to `svlut`/`DAT_0049ac60` via `FUN_0046d614`, `DAT_00540820` uploaded via `FUN_0046d208` — no input-state reset), `FUN_00420d68` (options exit: persists iff dirty `DAT_00541486` via `FUN_004260ac`, `FUN_00402590` re-enter, `svlut` palette restored + released, `DAT_00541493=0`), `FUN_004202cc` optload, `FUN_0041dc90` (front-end root draw: `MDKOPT` memcpy + OPT0..OPT4 items + ARROW; Phase 4D), `FUN_0041d85c` (front-end enter: save probe `FUN_00428290` → `DAT_0054bc98`, initial selection `DAT_0049aa78`, resolves `MDKOPT` → `_DAT_0054bca0`, palette via `FUN_00413b40`), `FUN_00420df8` (OM item draw: `y = i*36+49` → `FUN_00423b88` → `FUN_0041518c` centered `x = trunc((600 − w·scale)·0.5)`), `FUN_00423b38` (item draw: `x = trunc(x_arg − w·scale·0.5)`), `FUN_00423a24` (selection scale ramp 0.65→1.0 — `(centerX,itemY)`-keyed acc machine `DAT_0054bdc8..bdd8`, `+=DAT_0049b6f0`/frame, `0.65±acc·0.07`, limit 5.0), `FUN_00418798` (mouse reset → `DAT_0054b634/38 = 300,180`), `FUN_004187e0` (per-frame input accumulate: `b634/38 += b644/48` clamp `[0,599]×[0,359]`, zero-delta skipped), `FUN_004237b4`/`FUN_00423838` (prev/next key queries — press fire, deadline `tick+30`, repeat `tick+3`, stale window `+100`; fire also calls `FUN_00423734` SND_PUSH), `FUN_004238bc`/`FUN_00423940` (LEFT/RIGHT queries — same bodies; options only), `FUN_00423764` (activate query — Enter edge `b574` or button down-edge on latch `DAT_0049ac80`), `FUN_0042fcd0`/`FUN_0042fdc8` (frame timing: raw delta vs virtual clock `DAT_0049b700` chasing at `rawDelta·25/3` ms; step `clamp(accum>>2,1,4)` → `DAT_0049b6e8`; EMA `b6f0 = b6f0·0.75+units·0.25`), `FUN_0047d59a` (x87 truncation RC=11) | STRONG_ | `optmenu.c/optload.c/options.c` strings, `OM_*` records (`OM_HELP/OM_SOUND/OM_JOY/OM_MOUSE/OM_KEY/OM_PERF/OM_SK_0..2/OM_DISPL/OM_QUIT`); root item Ys `0x1f+0x24i` hit-test `trunc((mouseY−5)/36)`, options hit-test `trunc((mouseY−23)/36)` + hidden-rows guard `1<band<5` under `DAT_005414f4` (`-mapok`); options sel `DAT_0054bd34`, skill `DAT_0054147a` cycles ±1 wraps, `DAT_00541538` delegation flag — gated by `b644|b648|b640`, tighter gate clamp `x≤590,y≤350` — Phases 4E/4F, see `../ENGINE_RECONSTRUCTION.md` |
| Demo record/play | `FUN_004090fc` | STRONG_ | `demo.c`, `demo\%s`, `SAVE CORRUPT: demo file %s…` |
| Enemy/AI | `FUN_004388d8`, `FUN_00454794/4549b4`, `FUN_004574d0` | TENTATIVE_ | `Alien %s looped %d commands`, `ENEMY name %s not found`, `Unrecognised controlalien`, `tr_alcmd.c`, `allocenm.c` |
| Memory mgmt | `FUN_0041c780/41c7e8` | STRONG_ | `memblock.c`, `Total level/game memory` |
| Object-pool allocator ("chunks") | `FUN_00404084` (+cluster `0x403f6c`–`0x406554`) | STRONG_ | `chunks.c` — Phase 3B re-analysis: in-memory pool of 60 × 0x1aa-byte object nodes (free list `DAT_004a1ec4`, alloc `FUN_00403f6c`, recycle `FUN_00404084`); **not** the file-envelope parser — earlier "(matches u32-len+tag file family)" note was wrong. File-envelope evidence lives in the mdkfopen path; see `../DATA_ACCESS.md` |
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
per-level families `%s\LEVEL%d\…`: `.MTO` overlay data, `O.SNI`/`S.SNI`
sound, `S.MTI` imagery, `.CMI` counted name→offset tables + data
region (Phase 3F — interior proven; the earlier "collision/map" label
was a guess and is withdrawn: nothing in `.CMI` is proven to be
collision, map, arena or BSP data), `.DTI` five-section bundle
(Phase 3G — interior proven: params, keyed records, the original's
"arena" table with typed payloads, palette, backdrop grid).
`FALL3D\FALL3D_%d.MTI`
+ `.SNI` + `.BNI` for freefall; `STREAM\` for mid-level streams;
`MISC\` for fonts/config/movies/slideshow/sound set; `demo\` for input
recordings; `SAVES\%.SAV` + `LASTGAME` for saves. (Phase 3C: the `.SNI`
interior directory — count + 24-byte name/offset/size records — was the
first proven interior format; Phase 3D adds the `.MTI` material-table
directory — count + 24-byte name[8]/flags/params/blob-offset records;
Phase 3E adds the `.MTO` overlay directory — count + 12-byte
name[8]/file-offset records indexing overlay blocks that each embed a
`.MAT` image plus three bounded data regions; Phase 3F adds the `.CMI`
interior — four counted `{u8 len, name, u32 imgOff}` tables plus a
bounded data region, consumed at traversal load for the original's
"enemy table" and per-arena name lookups; Phase 3G adds the `.DTI`
interior — a five-section image-relative TOC (params / keyed records /
the original's "arena" table with typed 36-byte payloads / palette /
backdrop grid), and the proven link that the CMI table-3 lookup runs
once per DTI arena record; Phase 3H adds the two length-envelope
directories — `.FTI` = `{name[8], u32 imgOff}` ×12-byte records into
the engine-wide `DAT_0049ff50` resource table ("font table"
diagnostic), `.BNI` = `{name[12], u32 imgOff}` ×16-byte records into
the per-context `DAT_004a1e38` slot — distinct record layouts proven
by their separate original lookups;
see `../DATA_FORMATS.md`.)

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
  topology rules, exact CRT↔game seam, `.BNI` payload semantics,
  freefall control model.
- UNKNOWN: per-packet save fields, AI command opcode set, camera/sniper
  internals, physics constants, renderer inner loop (rasterizer texturing),
  hardware-variant renderer internals, `.MVE` decoder details.

## Remaining unknowns / next targets (Phase 2C candidates)

1. Semantic decode of the mode values 0–8 and sub-modes 1–11 (runtime
   correlation in DOS oracle).
2. `.SAV` packet field map (correlate `SAVE CORRUPT` tags with live saves).
3. `.CMI` data-region organization and per-table target semantics
   (directory structure proven in Phase 3F — it is NOT an arena/BSP
   structure as once guessed); `.MTI` payload data interpretation
   past the u16 header remains open.
4. Alien command interpreter (`tr_alcmd.c` region) — opcode inventory.
5. FLIC/MVE decoder boundaries for future video playback.
6. Win95 runtime lane (blocked on licensed Windows 95) to validate the
   DDraw/DInput/DSound paths end-to-end.
7. Verify a pristine retail/GOG build to lift the BUILD_A/NoCD caveat.
