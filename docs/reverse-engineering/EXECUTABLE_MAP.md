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
| Input | `DirectInputCreateA` | `FUN_0046bd14` creates keyboard+mouse devices (`Cannot initialise mouse/keyboard from directinput`); per-frame polls: `FUN_0046b688` (kbd), `FUN_0046bc18` (mouse), `FUN_0046b9b4` (joystick via `joyGetPosEx`) |
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
| Gameplay input consumption | `FUN_004187e0` (per-frame input pump — runs the keyboard/mouse/joystick polls + `FUN_00419370`), `FUN_00419370` (configured-binding → action flags `0x54b650..0x54b6c8` from the 29-dword block `0x5413fe`: slots 0–13 + 27–28 level-queried, slots 7/9/14–23/24–26 edge-queried — 10 hidden direct-weapon hotkeys at 14–23), `FUN_004192cc`/`FUN_00419320` (level/edge queries), `FUN_0041925c` (right-modifier fold `0x36→0x2a`/`0x61→0x1d`/`0x65→0x38` inside both queries), `FUN_00419168` (lowest-set-bit scan), `FUN_00406f14` (per-frame merge → control block `0x4ce6e0..0x4ce7ac`: flag copies + joy/mouse button 16-bit action masks + W-set axis letters `0`/`A`–`H` (delta/scale, `D`/`E`/`F` negate, `|v|<0.2` deadzone, `/0x49b6f0` clamp ±4) + `G`/`H` first-wins zoom `rint(|d/s|+1)` into acc `0x4ce760` clamp ±8 decay `0x49b6e8` + SideStep reroute + STURB latch `0x540d38` + snipe synthetic edge `0x499f50` + OBSERVED rate constants `0x4944d8..0x494570`), `FUN_00464624` (sniper-mode update — dispatched only when `0x540c9c != 0 && 0x540ca0 != 0`; its raw-delta block is the sniper aim: `b54 += dy·0.12·f0·b58·(5/12)` clamp ±50, `c2c −= dx·0.12·f0·b58·(5/12)` wrap [0,360), gated `MouseOn` and only when the semantic aim channels are idle; `MouseYReversed != 0` negates the scaled dy here, not in `FUN_00406f14` — Phase 5J corrected the earlier "free-look" label: normal traversal has NO raw-mouse look, `FUN_00465228` never reads `0x54b644/48`), `FUN_004691c4` (mounted-reticle raw-delta consumer — `e70 & 0x40000` branch, reticle pixels [128,472]/[64,296], no YReversed), `FUN_0047d59a` (x87 `rint` helper, RC=11) | STRONG_ | `JOY_BA..BP`/`JOY_AA..AH`/`JOY_A0` semantic records; settings-table addresses `MouseOn 0x541472`/`MouseYReversed 0x541476`/`JoyOn 0x541310`; `MouseD*` globals have zero xrefs — dead in BUILD_A; `MouseOn` gates axis paths only (button masks still decode) — Phase 5A, see `../GAMEPLAY_RECONSTRUCTION.md` |
| Player movement (normal locomotion) | `FUN_00436100` (traversal frame — sole caller of `FUN_00463608`), `FUN_00463608` (per-frame dispatcher: zeroes event word `0x54cb00/08`, runs one player-state branch on `DAT_00540cac`, then shared tail `FUN_00464d10` debug fly-toggle → `FUN_00406f14` merge → `FUN_0047d20a(0x4ce6e0,0xd0,0)` 208-byte control-block dispatch — so the integrator consumes the PREVIOUS frame's block: OBSERVED one-frame input latency), `FUN_00465228` (normal-movement integrator, `cac<800 && e6c==0 && c9c==0`; ignores its stack args — pure state machine over `0x540bfc..0x540eb8`: channels `d48` move/`d4c` strafe/`d50` turn ← `0x4ce708/6f8/700` rate×accel-scale via `FUN_00465b54` (f0-scaled; mouse) / `FUN_00465bd8` (raw add; keyboard turn), decay `FUN_00465a84` (move/strafe `4/45` in `8/45` out bound `±2/3`×decel-scale; turn `0.55`/`1.6` bound `±4`); accel scales `e4c==0`→0.75/0.75, `e4c`+flag4→0.5/0.1, else 1.0/1.0; basis `FUN_00437f98` sin/cos(yaw·π/180); compose `x+=move·f0·cos+strafe·f0·sin`, `y+=move·f0·sin−strafe·f0·cos`, `yaw−=turn·f0` wrap ±360 → disp → `FUN_004630d4` collision seam; events 6/600 move → 5/500 strafe → 4/400 turn with suppression bits; blocked-move event cancel post-collision; bank `0x540b4c` driven `±f0·0.25`+`±2` kick clamp ±10; `0x540c94` turn-lock; `0x540cc0` move-dir latch; `0x540cc4` move-consumed), `FUN_00412ef0` (conveyor/surface-effect displacement — contact-list scan, `dir·rate·0x49b6f4`), `FUN_00466740` (jump-state machine inside `FUN_00465228`'s tail, after the horizontal `FUN_004630d4` + `FUN_0046603c`: `c88` loco-state maintain → `c84` seed `<-16`/accumulate `+f0`/grounded reset → init gate `cbc<7 && cb00<7 && c78==0 && c54&1 && c90 latch` → `c78=40.0f`, `c8c=6`, event `0x2be`/`0x2bf`(`cc4`)/`cb00=7` → held `c8c-=frameStep` / release `c78-=c8c·20·(1/6)` → `c80` rewrite (`c84!=0`: release→event `700`, else sustain event `0x2bd`/`c80=1`) → slope assist `dot/f4` → `FUN_00467180` → `FUN_00466aec` → `e28=0`; `e24` slide → jump machine skipped), `FUN_00467180` (vertical/gravity integrator — `c6c`/`c7c` gates; `c78>0 && e24==0`→`frameStep` substep loop else single `f4` step; normal `−64·f4` vs sustain `−64/3·f4` + `256·f4` rebound under `−8`, terminals `−250`/`−8` on pre-truncation doubles; `FUN_00412e94` volume → out-vec z→`c78`, force `c80`, `c84−f0·1.75` floored at bit-pattern `1.0f`; rise cap `>40`→`disp=f4·40` (`e6c`/`cac<800`); pre-land `c54&2`→`disp=c58+0.05−posZ`; `c54&=~1` → `FUN_004630d4` Z-only `r=0.5` `&e50`; landing: `impact>0`→ceiling (`c78=0`, no landing), `impact<−100 && e28==0`→hard (`806/8`, `FUN_00467a00(10)`, `d5c=0`) or silent (`e6c && e72&2`) — both clear `c84`, soft does NOT (resets next frame); anti-jitter `f4·0.35`; no-contact realized-velocity `(posZ−old)/f4`; blockers `c60/64→dc0/dc4`/`+0x14a&0x80`→`dc8`; deep-floor `posZ≤+0x44e−50`→`541554=0,c78=0,c54|=1`), `FUN_00465c4c` (semantic look integrator — see the look row below; runs on BOTH dispatch branches after `FUN_00466740`), `FUN_0046603c` (slide/dash mode — `cac==800`, gated `0x540e24`, uses `0x49b6f4` delta-seconds), `FUN_00466aec` (mantle/ledge-grab — `moveNorm>0 && c78≤−0.25`), `FUN_00469cd0` (item-action dispatch of `0x4ce778..0x4ce7ac`), `FUN_0047f4a0` (control-block record under `FUN_0047d20a`) | STRONG_ | instruction-level disasm/decomp of `0x465228` + register-level channel attribution; OBSERVED constants `0x4988e8`=`0.25`, `0x4988f0/f4`=±2.0, `0x4988f8`=−10, `0x498900`=8/45, `0x498908`=4/45, `0x498910/14`=±360, `0x498918`=1.75, `0x4986c0/c8/d0`=`0.35/2.5/0.05`, `0x497924`=π/180; accel scales `0.75/0.75`,`1.0/1.0`,`0.5/0.1`; AABB `{0.6,0.6,2.5}`/`{0.4,0.4,2.5}`; `0x540d9c` master gate; `0x540dc0`/`0x540dc8` move blockers — Phase 5B, see `../GAMEPLAY_RECONSTRUCTION.md` §14–22. Phase 5C adds `FUN_00466740`/`FUN_00467180` constants: `0x4989d0`=−16.0, `0x4989d8`=20.0, `0x4989e0`=1/6, `0x4989e8`=0.25, `0x498a38`=64.0, `0x498a40`=−250.0, `0x498a48`=64/3, `0x498a50`=−8.0, `0x498a58`=256.0, `0x498a60`=1/30(f32), `0x498a68`=40.0, `0x498a78`=1.75, `0x498a80`=0.05, `0x498a88`=−100.0, `0x498a90`=0.35, `0x498a98`=−50.0; `0x49b6f4`=1/30 has ZERO writers (true constant) — §24–31 |
| Player look / view orientation (normal traversal) | `FUN_00465c4c` (semantic look integrator — called inside `FUN_00465228`'s tail after `FUN_00466740` AND in the dispatcher's `cac >= 800` scripted branch; eligibility `(cbc<8 || cac==0x324) && (c78&0x7fffffff)==0 && (c54&1)`; `lookUp` (`0x4ce780`) `d58 -= f4·90` / `lookDown` (`0x4ce784`) `d58 += f4·90` (up tested first — wins conflicts), clamped `[-60−a462, +90−a462]` (`a462` = `[0x540c48]+0x462` arena rest-pitch scalar); else recenter `d58→0` at `f4·200` sign-snapped — momentary look, not persistent aim; posts `cb00=8/cb08=0x324` while driving/draining, self-stores at `d58==0`), `FUN_00463608` (dispatch head: pending slots `0x54cb00/08` cleared per frame; `cbc` `0x540cbc` reset for transient `cac` {300,400,500,600,601}; idle restore `cb00=1/cb08=0x65` when `cbc==0 && cb00==0`; latch `cbc<cb00 → cac=cb08,cbc=cb00`), `FUN_00461954` (animation machine — its `0x324` handler clears `cbc` once `d58` re-centres to 0: the look-state exit; the rest of the machine deferred), `FUN_004301e0` (view tail — `b54` blends `*0.85+a462*0.15` under `bec==0`; `b718` z-delta `clamp ±0.5` → EMA `·0.97/+·0.03` + sign-slew `f0·0.02` never crossing raw; `lookEff=d58` bounded so `b54+lookEff ∈ [-60−a462,+90−a462]` while mid-blend; `b50 = 90 − c2c` view yaw; `b71c = c84·2/3` cap 40 else decay `f4·40`; `0x540be0 = b54+lookEff−b718·40+b71c`; matrix/FOV rows after that are the renderer boundary), `FUN_00433c4c` (init — `b54=6.0`, `b58=2.4`), `FUN_00431100` (view-snap seam under `c9c==0 && 0x49b740!=0`), `FUN_00464d10` (debug tilt keys write `b54=a462` directly) | STRONG_ | instruction-level disasm of `0x465c4c`/`0x464624`/`0x4301e0`; constants `0x498920`=200.0, `0x498924`=90.0, `0x498928`=−60.0, `0x498858`=0.12, `0x498860`=5/12, `0x498868`=−50, `0x49886c`=−360, `0x498870`=360; `0x497218`=0.15, `0x497220`=0.85, `0x497228`=0.5, `0x497230`=−0.5, `0x497238`=0.97, `0x497240`=0.03, `0x497248`=0.02, `0x497250`=90.0, `0x497254`=−60.0, `0x497258`=40.0, `0x497260`=2/3, `0x497268`=40.0; `d58`/`c2c`/`b54`/`b50`/`b718`/`b71c`/`be0` all DEGREES (`FUN_00437f98` trig takes deg); negative proof — `FUN_00465228` never reads `0x54b644/48` — Phase 5J, see `../GAMEPLAY_RECONSTRUCTION.md` §77–89 |
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
| Display child screen (mode 7) | `FUN_0041d020` (entry from options row 7 — all three dispatch tables bind to `0x4210ce`: `DAT_00541493=7`, `DAT_0054b834=2`, active palette flattened to `dlut`/`DAT_0049aa70` via `FUN_0046d614`, `slut`/`DAT_0049aa74` composed SYS_PAL head + 4×48 ramps then `FUN_0046d208` upload — no input-state reset), `FUN_0041d1e0` (frame handler — same query order as options: prev→next→mouse `band=trunc((mouseY−5)/36)`→Esc→LEFT→RIGHT→activate→draw; row 0 `DAT_0054147e` ±1 wrap [0,7], row 1 `DAT_00541482` toggle, row 2 activate exit; LEFT/RIGHT always fall through, Esc/row-2 activate RET early), `FUN_0041d144` (exit: `DAT_00541493=0x0b`, `dlut` re-uploaded, both buffers freed — options resumes sel 7), `FUN_0041cf80` (4×48 swatch grid via `FUN_00416aa8` inclusive rectfill — cells `x=60+10i..69+10i`, `y=200+32b..231+32b`, index `64+48b+i`), `FUN_00416aa8` (solid rectfill `(x1−x0)+1`×`(y1−y0)+1`), `FUN_00415658` (framebuffer zero-fill), `FUN_00413b40` (SYS_PAL head `DAT_00540820` + src tail compose), `FUN_0046d208` (palette upload — `DAT_0054d7b8` runtime table raw; staging-buffer lift `min(c+level·16,255)` for `level=DAT_0054147e`, incl. index 0), `FUN_0046d614` (runtime BGRX→RGB-triplet flatten), `FUN_0047d2e9` (vsprintf — `DSP_BRGT` is the row-0 `"Brightness %d"` format), `FUN_0047d1a5` (toupper — settings type-2 bool parse `== 'T'`), `FUN_0040103c` (sub-mode dispatcher, table `0x401010` indexed `mode−1`: 0=root, 2=sound `FUN_004233d8`, 7=display `FUN_0041d1e0`, 0xa=help `FUN_0041d630`, 0xb=options `FUN_00420eac` — Phase 4H) | STRONG_ | `optdispl.c` source path, `DSP_BRGT/DSP_DETH/DSP_DETL/DSP_QUIT` records, `dlut`/`slut` tags, settings table `0x49aca8` entries 89 `{"Brightness",int,&DAT_0054147e}` mirror `0x49b272`=0 + 90 `{"ForcePCorrect",bool,&DAT_00541482}` mirror `0x49b276`=0 — see `../ENGINE_RECONSTRUCTION.md` Phase 4H |
| Sound child screen (mode 2) | `FUN_0042322c` (entry from options row 1 — all three dispatch tables bind to `0x420fdd`: `DAT_00541493=2`, ambient MAINSONG stopped via `FUN_0041d774`, `MISC\MDKSOUND.SNI` loaded via `FUN_00428828`, `OPTSONG→DAT_0054bdc0`/`OPTBUTT→DAT_0054bdc4` resolved via `FUN_00402fe8`, `FUN_00402388(OPTSONG,0)` starts the screen song, `DAT_0054bdb8=3`, `DAT_0054bdbc` NOT reset — process-global selection), `FUN_004233d8` (frame handler — same query order as options plus the dead `DAT_00541538` delegation clear at top: prev→next→mouse `band=trunc((mouseY−61)/46)`→Esc→LEFT→RIGHT→activate→draw; EVERY fired repeat/activate query plays `OPTBUTT` via `FUN_00402388(·,1)` first except Esc; row 0 `DAT_00541308` ±10, row 1 `DAT_0054130c` ±10, both clamp [0,100] + dirty `DAT_00541486` + `FUN_004024c4` volume push; rows 0/1 activate fall to draw, row 2/Esc → `FUN_00423280` + RET early), `FUN_00423280` (exit: `DAT_00541493=0x0b`, `FUN_0040210c` stops OPTSONG, `FUN_00428b34` releases the SNI, `FUN_0041d720` restarts MAINSONG under `DAT_00541492==0` — options resumes sel 1), `FUN_004232b0` (volume row: label `FUN_00423b10` ramp key `(4,rowY)` FONTBIG scaled x=4, bar `FUN_00416aa8` inclusive rectfill `x=210..210+trunc(vol·280/100)`, `y=rowY−12..rowY−1` color 4, `SND_100`@498/`SND_0`@175 FONTSML via `FUN_00414dd4`), `FUN_00423384` (Done row `SND_DONE` centered y=179 ramp key `(−1,179)` via `FUN_00423b88`), `FUN_00414d2c` (centered title `SND_TITL` y=31 / info `SND_INFO` y=350 — FONTBIG under 600 else `FUN_00414f1c` FONTSML), `FUN_004024c4` (volume apply → `FUN_0040202c` routes scale by voice flag bit `0x2` music channel), `FUN_0041d774`/`FUN_0041d720` (ambient song stop/restart — `DAT_0049aa94` MAINSONG handle, `DAT_00541492` game-in-progress gate) | STRONG_ | `SND_TITL/SND_INFO/SND_FX/SND_MUSI/SND_DONE/SND_100/SND_0` records (`"Sound Settings"`, `"Left/Right to Change Volumes"`, `"Effects"`, `"Music"`, `"Done"`, `"100%"`, `"0%"`; `SND_SET` "Setup Device" vestigial), `MDKSOUND.SNI` records `OPTBUTT` flags `0x0000`/`SNDTEST`/`OPTSONG` flags `0x0003` (bit `0x2` = music channel), settings table `0x49aca8` entries 8 `{"SoundFX",int,&DAT_00541308}` mirror `0x49b0fc`=70 + 9 `{"SoundMusic",int,&DAT_0054130c}` mirror `0x49b100`=100, `DAT_00541538` delegation OBSERVED dead in BUILD_A — see `../ENGINE_RECONSTRUCTION.md` Phase 4I |
| Mouse child screen (mode 4) | `FUN_00421664` (entry from options row 3 — all three dispatch tables bind to `call 0x421664` thunks: LEFT[3]→`0x421042`, RIGHT[3]→`0x4210f9`, ACTIVATE[3]→`0x421193`, each guarded by `DAT_005414f4==0`; entry body: `DAT_00541493=4`, selection `DAT_0054bd40=0`, grid column `DAT_0054bd38=0`, axis count `DAT_0054bd3c=3`, button count `DAT_0054bd44=4` — no palette/resource/input work; shared mouse/tick/repeat/latch/ramp/blink state carries over), `FUN_004217e8` (frame handler — 23-row machine: left rows 0–3 `JOY_TEST`/`M_ENA`+`M_DIS`/`M_NORM`+`M_REV`/`JOY_QUIT` hit `x<250`, `trunc((y−2)/16)`; axis rows 4–6 `trunc((y−259)/16)`; grid rows 7–22 `trunc((y−33)/16)` col `trunc((x−396)/16)` clamp [0,3] for `x≥250`; coords clamped (590,350) inside the hit gate; query order Esc-first→prev→next→mouse→LEFT/RIGHT→activate→draw; Esc/Quit/exit dispatches RET early; LEFT/RIGHT toggle rows 1/2 + clamp/wrap col; activate row 0 no-op, rows 4–6 `FUN_004216a0` cycle +1, rows 7–22 `FUN_00421774` toggle bit), `FUN_004216a0` (axis-letter cycler — domain `'0'`,`'A'..'H'`; LEFT −1, RIGHT +1, activate +1; out-of-domain repairs to `'0'` via bounded loop — original loops forever on a too-short map), `FUN_00421774` (button-bit toggle + 16-dword exclusivity table `@0x499f0c` clears conflicting bits in the other three masks), `FUN_00414b28` (selection blink bracket — double outline, colors swap on bit 3 of persistent `DAT_0049a770`), `FUN_00421834` (draw block — four left rows FONTSML x=4 y=2/34/66/98-style spacing, `JOY_B` grid headers `JOY_B%c` cols x=396+16c y=17, rows `JOY_B%d`/`JOY_A%c` labels + cells `x=396+16c y=33+16r` filled via `FUN_00416aa8` iff mask bit set, axis bars `x=250..550 y=259/275/291` + marker `floor(val·20.0+30.0)` clamp ±1 → `x=310/330/350` test indicator; all FONTSML, no FONTBIG, no ramp keys; ARROW at logical mouse) | STRONG_ | `optmouse.c` source path, `JOY_TEST`/`M_ENA`/`M_DIS`/`M_NORM`/`M_REV`/`JOY_QUIT`/`JOY_B`/`JOY_B%c`/`JOY_A%c`/`JOY_AX%d` records + formats, settings table `0x49aca8` entries 49–68 (`MouseWAxesMap`/`MouseDAxesMap` str "ABG", `MouseWButtMap`/`MouseDButtMap` str "ACB", `MouseWButtMapA..D`/`MouseDButtMapA..D` int {1,4,2,0}, `MouseW/DX/Y/ZScale` float {16,16,50}, `MouseOn` bool TRUE, `MouseYReversed` float slot toggled as raw int bits → denormal `1.4013e-45` emission) — Phase 4J, see `../ENGINE_RECONSTRUCTION.md` |
| Keyboard child screen (mode 5) | `FUN_0041f030` (entry from options row 4 — dispatch guarded by `DAT_005414f4==0`; entry body: `DAT_00541493=5`, capture `DAT_0054bca8=0`, selection `DAT_0054bcac=0x14` — **20 = KM_QUIT, not row 0**; shared mouse/tick/repeat/latch/ramp/blink state carries over), `FUN_0041f18c` (frame handler — 21-row machine: 19 binding rows two columns, `KM_RESET` row 19, `KM_QUIT` row 20; capture branch first — Esc checked BEFORE raw-key poll and cancels capture (can never be bound), else `FUN_00419168` lowest-set-bit edge pick over the 128-bit new-press bitmap → `FUN_0041925c` right-modifier fold `0x36→0x2a`/`0x61→0x1d`/`0x65→0x38` → write row's backing global iff different + `DAT_00541486=1`; normal branch — Esc exits to options pre-draw, prev=UP-or-LEFT next=DOWN-or-RIGHT wrap 0..20, mouse hit-test selects only (bindings y≥64 30px pitch, reset [2,18), quit [18,34)), activate rows 0–18 → capture, 19 → reset, 20 → exit; NO duplicate-key rejection), `FUN_00419168` (scan 128-bit edge bitmap → lowest set internal code, 0 = none), `FUN_0041925c` (right-modifier normalization AFTER winner pick), `FUN_00425db0` (reset — copies 29 dwords `0x49b1f2`→`0x5413fe`: all 19 visible bindings + 10 hidden weapon-hotkey globals; preserves ECX=1 → dirty latched even on a no-op reset), `FUN_0046b688`/`FUN_0046bc18` (DirectInput key path — DIK≤0x7f → internal=DIK, extended DIK → `0x49bbf0[DIK&0x7f]` unmapped→0x7f; latched-press bitmap + level bitmap → per-frame new-press edges), `FUN_0041f068` (row draw — label `x=310·(row/10)+10`, key glyph `x=310·(row/10)+210`, `y=30·(row%10)+64`, FONTSML; normal flags label / capture flags glyph), `FUN_00414f1c` (centered FONTSML — trunc-halving C `/2` — `KM_RESET` y=16, `KM_QUIT` y=32, `KM_DOIT` y=354 in capture), `FUN_00414b28` (blink bracket) | STRONG_ | `KM_LEFT/RIGHT/UP/DOWN/JUMP/SIDEL/SIDE/SIDER/SNIPE/FIRE/TURBO/STURB/LKUP/LKDWN/ZOOMI/ZOOMO/INEXT/IPREV/IUSE/RESET/QUIT/DOIT` + `LANG` records, three 128-byte glyph tables `0x49aaa8`/`0x49ab28`/`0x49aba8` (E/F/G by `LANG[0]`), extended-DIK table `0x49bbf0`, settings table `0x49aca8` entries 69–87 (`KeyLeft`…`KeySideR` int, factory arrows 103/105/106/108, Jump=56, Side=45, SideL=51, SideR=52, Sniper=57, Fire=29, Turbo=42, STurbo=58, Look/Zoom 30/44, ItemNext=27, ItemPrev=26, ItemUse=28) — Phase 4K, see `../ENGINE_RECONSTRUCTION.md` |
| Demo record/play | `FUN_004090fc` | STRONG_ | `demo.c`, `demo\%s`, `SAVE CORRUPT: demo file %s…` |
| Enemy/AI | `FUN_004388d8`, `FUN_00454794/4549b4`, `FUN_004574d0` | TENTATIVE_ | `Alien %s looped %d commands`, `ENEMY name %s not found`, `Unrecognised controlalien`, `tr_alcmd.c`, `allocenm.c` |
| Memory mgmt | `FUN_0041c780/41c7e8` | STRONG_ | `memblock.c`, `Total level/game memory` |
| Object-pool allocator ("chunks") | `FUN_00404084` (+cluster `0x403f6c`–`0x406554`) | STRONG_ | `chunks.c` — Phase 3B re-analysis: in-memory pool of 60 × 0x1aa-byte object nodes (free list `DAT_004a1ec4`, alloc `FUN_00403f6c`, recycle `FUN_00404084`); **not** the file-envelope parser — earlier "(matches u32-len+tag file family)" note was wrong. File-envelope evidence lives in the mdkfopen path; see `../DATA_ACCESS.md` |
| Object setup | `FUN_00428400` | STRONG_ | `setupob.c` — Phase 5E: proven as the model geometry record parser (CMI table[1] targets + MTO region-A array-B targets; see "Dynamic collision objects" row) |
| Shutdown | tail of `FUN_0040103c` + `FUN_0046bef8` (input release) | OBSERVED | WndProc close → quit flag → config write → exit |
| Camera/sniper | UNKNOWN | — | `Bones tooth not found`, sniper strings exist; path not yet isolated |
| Physics/collision | `FUN_004630d4` (player swept query/apply — `(dx,dy,dz,scale,extVec,outAux)`; pos globals `0x540bfc/c00/c04` always committed; horizontal `0.75`/`{0.6,0.6,2.5}`, vertical `0.5`/`{0.4,0.4,2.5}` + `&e50` node out; EAX = poly-record token → `e4c`), `FUN_00407fc0` (iterative sweep orchestrator, flag=slide budget 4/0 + `0x4635e0` contact callback), `FUN_00408260` (recursive BSP sweep over 0x2c nodes `{plane f4, child s16x2, polyset u32x2}`), `FUN_00408820` (leaf poly-set scan, 0x24 records `{u16 v[3], flags+0x20, surface+0x23}`), `FUN_004089c0` (box-vs-triangle SAT, axis table `{1,2,0,2,0,1}`), `FUN_00425600` (point-in-tri parity, EAX=hit/EBX=v0), `FUN_004088cc`/`FUN_0040894c` (post-slide plane pushout, `|n.z|<0.75` split), `FUN_0045ce58` (AABB overlap), `FUN_0045c838` (2.5D segment/AABB resolver, X/Y face clamp), `FUN_004138d8`/`FUN_00413730` (object local-space segment-vs-triangle), `FUN_00435eec` (per-frame floor probe — object-list only, `(z+3)->(z-3)`, writes `c58`/`c5c`/`c60`/`c64`, `c54` bit1; called at `FUN_00436100` tail, NOT inside the query), `FUN_00419ee0` (level-stream blob parse → arena `+0x24/28/2c` = verts/polys/nodes), `FUN_0040b5d0` (surface-effect dispatch — bounce/conveyor source; hook boundary), `FUN_00461878` (mount-release/debug-fly reset; hook boundary) | STRONG_ | instruction-level decomp+disasm of the full call graph; constants `{0.6,0.6,2.5}`/`{0.4,0.4,2.5}` @`0x49bab4`/`0x49baa8`, probe span ±3.0 f64 @`0x497788`/`0x497790`; slide rule `(δ·n)²>scale·|δ|²`; native `src/core/collision_query.*` + `--selftest-player-collision` + `mdk-inspect --collision-probe` — Phase 5D, see `../GAMEPLAY_RECONSTRUCTION.md` §32–40 |
| Dynamic collision objects | `FUN_00456808` (arena spawn pass over DTI s2 0x24-stride records: type-2 HotGen `field[0]=enemyIdx<<16|spawnId` dedup on (idx,spawnId,pos) + script key `arena$MODEL_spawnId` + `+0x11c=7`; type-4 HotPick `field[0]=modelIdx` + `+0x08=1` + `+0x148|=0x2008a0` + script key `arena$MODEL` + `SW_DUMMY` element masking), `FUN_0045cffc`/`FUN_0045cf90`/`FUN_0045cf18`/`FUN_004574d0` (arena `+0x68` list ops: freelist pop+push-front / unlink+free / unnamed cleanup / portal transfer via `+0x2bc`), `FUN_004566f0` (object init: health 10, scale 1.0, identity matrix, script lookup + VM run, transform rebuild), `FUN_00428400` (model geometry parser — `{u32 flag}` + name table + elements `{12B name, 12B field2, vc, verts, tc, 36B tris, 24B trailer}` or one anonymous element + `0x18` gap + ≤8 ref points; `XG1_BODY`/`XG1_HEAD` bookkeeping), `FUN_00403720`/`FUN_00403538` (model deep-copy / parametric-box fallback), `FUN_00403498` (MTO region-A array-B name→`tA+off` resolution), `FUN_004286c8` (CMI table[1] → 0x88-stride enemy table, cap 0x50), `FUN_0045612c` (transform + world-AABB rebuild: raw-matrix `+0x148&0x40` path w/ `+0x5c` zBias vs Euler `FUN_0046b2f8(+0x54,+0x13c,+0x4c,+0x58,+0x10..0x18)`; `FUN_00459e40` 8-corner element AABB → `FUN_00459d54` min/max → union `+0x198` seeded degenerately from old z bounds), `FUN_004572ac` (per-frame update: flag dispatches → `+0x2bc` transfer → script VM → physics → mover `FUN_0045897c`/`FUN_004585c4` → velocity cache `+0x18c` → ride displacement `pos+=pos−prevPos`/`yaw+=yaw−prevYaw` for `obj==dc0` → latch prev state), `FUN_00432980` (portal partner pull-in for `+0x14a&0x10`), tr_alcmd opcode `0x29` (mountable `+0x14a|=0x80` / solid `+0x149|=1` platform mode) | STRONG_ | instruction-level disasm/decomp of spawn + list ops + transform + update order; DTI record layout verified byte-exact on BUILD_A (LEVEL3 XGS spawn=9 pos=(-174,2635,-293)); angle const `0x497924`=π/180; frame order inside `FUN_00436100`: player dispatch → `FUN_00432f84` targeting → `FUN_004572ac`/`FUN_0045cf18` → scripts → `FUN_00435178` portal test → `FUN_00435eec` floor probe → render `FUN_00431300`; native `src/core/dynamic_objects.*` + `mdk-inspect --arena-objects` — Phase 5E, see `../GAMEPLAY_RECONSTRUCTION.md` §41–49 |

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
