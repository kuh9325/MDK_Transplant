# Stream scene engine — mode 5 (and mode-8 boundary)

Phase 19A reverse-engineering contract for the post-traversal
intermission. Evidence levels per `EVIDENCE_POLICY.md`. Primary
evidence: Ghidra disasm/decomp of `MDK95.EXE` (logs under
`analysis-private/logs/p19a_*.txt|asm`, `p18c_mode5_*.txt|asm`) plus
a direct record census of `original/installed/STREAM/STREAM.BNI`,
`STREAM/STREAM.MTI`, `MISC/STATS.*`, `FINISH.BNI`, `MISC/FLIC/*`.

## 0. Headline correction (OBSERVED)

- Mode 5 (`FUN_0042b270`/`FUN_0042c8b0`) opens **`STREAM\STREAM.BNI`
  / `STREAM\STREAM.MTI`** (string VAs `0x496e24`/`0x496e38`,
  `MOV EAX` args @`0x42b3xx`). The Phase-18C claim that mode 5 uses
  `MISC\STATS.BNI`/`STATS.MTI` was wrong.
- `MISC\STATS.BNI`/`STATS.MTI` are opened by the **mode-6** init
  `FUN_00429200` (`0x496c78`/`0x496c88`) — the statistics/briefing
  screen (`CGUN`/`SNIPER`/`RICO1..3`/`ALDIE`/`XGHEAD1`/`XGHEAD2`/
  `TELETYPE`/`XGHEAD`). STATS.BNI has no other code reference.
- Mode 8 (`FUN_0047b06c`) is a **video pipeline** —
  `MISC\FLIC\MDKEND.FLC` (FLIC, `FUN_00414158` decoder) then
  `MISC\FLIC\MDKBZK.MVE` (Interplay MVE, `FUN_0047b674` +
  `0x489xxx` callback player, 640×480). It shares **no scene
  engine** with mode 5 — only generic plumbing (frame limiter
  `FUN_0042fb68`, framebuffer `0x541650`, palette upload
  `FUN_0046d208`, present `FUN_0046c86c`).

## 1. Subsystem map (mode 5)

| subsystem | function(s) | evidence |
|---|---|---|
| init/load | `FUN_0042b270` | OBSERVED disasm |
| frame/update | `FUN_0042c8b0` (re-entry `0x42cb0b`) | OBSERVED |
| tunnel emit | `FUN_0042be4c` | OBSERVED |
| object alloc/free | `FUN_0045cffc`/`FUN_0045cf90` (shared pool) | OBSERVED |
| object spawn helpers | `FUN_0042bdc4`, `FUN_0042c6f0`, `FUN_0042c578` | OBSERVED |
| object reap | `FUN_0042c7b4` | OBSERVED |
| bucket migration | `FUN_0042da40` (`rint(obj+0x5c)` key) | OBSERVED |
| hero update | `FUN_0042d24c` | OBSERVED |
| companion update | `FUN_0042db0c` | OBSERVED |
| flourish update | `FUN_0042d034` | OBSERVED |
| pickup update | `FUN_0042d118` (`541554=150` on reach) | OBSERVED |
| shadow/rescue sync | `FUN_0042dabc` | OBSERVED |
| generic updater | `FUN_0042cf6c` | OBSERVED |
| wall probe | `FUN_0042d97c` (plane crossing) | OBSERVED |
| path lookup | `FUN_0042dc68` (ring lerp, `round&0x1f`+frac) | OBSERVED |
| camera build | `FUN_0042dcf4`/`FUN_0042de28` | OBSERVED |
| ribbon render | `FUN_0042e100` + `FUN_0042e620` (clip) | OBSERVED |
| backdrop | `FUN_0042e684` (parallax scroll blit) | OBSERVED |
| sprite emit | `FUN_0042e55c`, `FUN_0042e49c` | OBSERVED |
| model submit | `FUN_00455e24` (shared) | OBSERVED |
| draw-list flush | `FUN_00409a00` (shared) | OBSERVED |
| project | `FUN_0046b4f8` (shared, ported) | OBSERVED |
| anim tick | `FUN_004555bc` (shared, ported) | OBSERVED |
| teletype svc | `FUN_0041cb44` (queue cleared at init) | OBSERVED |
| HUD digits | `FUN_00417e20` (`541554>0x14` gate) | OBSERVED |
| camera save/offset/restore | `FUN_0042b060`/`0x2b0c0`/`0x2b090` (stereo) | OBSERVED |
| palette upload | `FUN_0046d208`, `FUN_00413b40` | OBSERVED |
| teardown | `FUN_0042c824` | OBSERVED |
| limiter | `FUN_0042fb30`/`FUN_0042fb68` | OBSERVED |
| RNG | `FUN_0047d2b5`/`FUN_00401ed4` (ported as `enemyRandNext/Below`) | OBSERVED |

## 2. Data census — `STREAM\STREAM.BNI` (29 records, OBSERVED)

| record | size | class |
|---|---|---|
| `BG` | 216004 | backdrop, 600×360 indexed (+4B) |
| `PAL` | 768 | palette tail (entries 64–255 from `[0xc0,0x300)`) |
| `PLANET` | 16388 | sprite 128×128 (+4B), rescue backdrop decal |
| `LIGHT` | 4100 | sprite 64×64 (+4B), debris image |
| `WIND`,`HITSIDE`,`RESCUE`,`HURT1..7`,`APPLE` | various | RIFF WAVE — SFX (audio deferred) |
| `SC_BSTAT`,`SC_STAT`,`SNIP_TXT` | small | text/layout records (exact semantics TENTATIVE) |
| `KURT`,`BONES`,`PROFSHIP`,`GUNTA`,`SWH150` | various | model protos (`FUN_00428400` parser — existing format) |
| `KURTANIM`,`BONESANIM`,`GUNTANIM`,`SWHANM`,`FL_HVR`,`FL_WAVE` | various | `ObjectAnimView` records (existing format) |

`STREAM.MTI` = standard material table (existing `MTI` parser).

## 3. Object pool (OBSERVED)

- Shared global pool: 399 records at `0x4f0740`, stride `0x32e`,
  freelist head `0x540ed0`. Identical link loop in `FUN_0042b270`
  (mode 5) and `FUN_00429200` (mode 6) → the traversal
  `DynamicObject` arena — port: `src/core/dynamic_objects.*`.
- Alloc `FUN_0045cffc` (pop head), free `FUN_0045cf90` (push),
  spawn `FUN_0042bdc4` (pop + bucket link + pathT seed),
  reap `FUN_0042c7b4` (bucket unlink + pool push).
- Key record fields (byte offsets): `+0x10/14/18` pos,
  `+0x1c..0x24` vel, `+0x28..0x30` angVel, `+0x34` rate(?),
  `+0x5c` pathT (bucket key), `+0xac` local 3×4 matrix,
  `+0xc`/`+0x108` sprite/model record refs, `+0x114` animRec,
  `+0xe4` animFrame (u16), `+0xe0` anim rate, `+0x118` anim ctl,
  `+0x11a>>0x10` last-tick stamp vs `0x49b5a4` (once-per-tick),
  `+0x148` flags bit3 (hero hide?), `+0x13c`/`+0x4c`/`+0x54`/`+0x58`
  state scalars.

## 4. Bucket / render order (OBSERVED)

- `0x4ed6b8[32]` = array of object-list heads per tunnel slot.
- Window `[0x4e74b0, 0x4e74b4)`; producer = `0x4e74b4` (incremented
  per `FUN_0042be4c` emit), consumer = `0x4e74b0`.
- `FUN_0042da40` migrates objects by `rint(pathT)`; landing outside
  the window → reap. Render walks slots back-to-front
  (`FUN_0042e100`), ribbons first, then the slot's object list via
  the shared `FUN_00409a00` draw-list.

## 5. Script/actor model (OBSERVED)

There is **no bytecode VM** in mode 5 — the "scripts" are the six
hardcoded per-object updaters dispatched on object identity inside
`FUN_0042c8b0`, plus generic `FUN_0042cf6c` scripted-prop behavior
(param fields on the record). Teletype `FUN_0041cb44` is a queue
service (ring `0x54b7fc`/`0x54b800`×4 of `{rate,flags,str}` —
implemented §15); mode-5 init clears it (`FUN_0041cf5c`) and the
scene posts no queued text (the `SC_*`/`SNIP_TXT` records feed
`FUN_00417e20`-style HUD/layout drawing — exact usage TENTATIVE).
The `TELETYPE` *record* (in mode-6 `STATS.BNI`) is a RIFF WAVE
typing-sfx sample, not a script (OBSERVED bytes).

## 6. Tunnel generator — `FUN_0042be4c` (OBSERVED)

Per emit tick:
- advances drift accumulators `0x4ed738/73c/740` by ±1.0 toward
  targets (sign-flip on zero cross; clamp `|x| > edad4` → `·0.8`);
- radius `0x4ed744 += rand·6.103515625e-05 − 0x4000·…` clamped to
  `[edad8, edadc]`;
- writes path node `0x4e74b8[i&0x1f]` (0xc0 stride): position +
  accumulated Euler frame; node transform `0x4eccb8[i]`;
- 16-point ring @ `22.5°` steps (`0x496fa8`), jittered radius, into
  the slot's cross-section; pen bytes `0x4ed2b8` (triangle fold);
- plane records `0x4e8cb8[i]` (0x200 stride): `n·x + d` per ring
  segment (16 planes — the collision/ribbon surface);
- drains objects in the overwritten bucket; ~75% ticks spawn a
  debris sprite (`FUN_0042c578`, `+0x108=LIGHT`).

Constants (f64 OBSERVED): `0x496f70=186`, `0x496f98=−1`,
`0x496f90=6.103515625e-05`, `0x496fa0=0.8`, `0x496fa8=22.5`,
`0x497000=177`, `0x497008=186`, `0x496e0c`=f32 `1/600`,
`0x49b6f4`=f32 `1/30` (frame dt).

## 7. Camera (OBSERVED)

- `FUN_0042dc68(t)` = lerp over the path-node ring translations —
  `slot=round(t)&0x1f`, frac blend → path point.
- Per frame: `A = dc68(heroPathT −0.75)`, `B = dc68(heroPathT +2.0)`;
  smoothed eye `= prev·0.4 + (A·1.7+B·0.6-overshoot→hero·1.375)·0.6`;
  `FUN_0042de28` rebuilds the full projection block `0x540b28..`
  (600×360, c=(300,180), `0x540b58=2.4`, M1 `0x540b80`,
  M2 `0x540bb0`).
- Stereo: `0x541544` → save `FUN_0042b060`, eye offset
  `FUN_0042b0c0` (`0x49b578`), render, restore `FUN_0042b090`,
  second eye.

## 8. Hero / collision / health (OBSERVED)

`FUN_0042d24c`: input axes `0x4ce758/0x4ce75c` (`FUN_00407f2c`),
pos/vel integration, emit-tick feed into `FUN_0042be4c`, wall
crossing via `FUN_0042d97c` over plane records (bank-roll mixed
probe). On crossing: ricochet (speed `·0.9`, floor 4.5), crash SFX
(`HITSIDE`/`HURTn`), health drain per beat — skill0 `−2`, skill1
`−(rand15()+2)`, skill2 `−(rand15()·2+4)`. Non-final clamps
`541554≥1`; final (`edad0`, course>3) lets it hit 0 → latches
`0x4ed748` @`0x42d95b` and sets `0x4eda9c=2.0` (fast fade).

## 9. Completion chain (OBSERVED)

Latch `0x4ed748` by any of: (a) rescue twin `edac0` `+0xe4 > 0x50`
(BONESANIM dock played out — spawn gated `541554>0 && (e74b0>177 ||
health==1)`, non-final only); (b) `edad0 && e74b4 ≥ 186`;
(c) hero-death write @`0x42d95b`. Then `0x4eda9c` decrements by
`1/30`; at `≤0` → DAC memset `0xff` if `edad0==0 && health>0`
(white flash) else `0` (black) → `FUN_0046d208` → return 1 →
dispatcher runs `FUN_0042c824` teardown → existing Phase-14
routing (`541554≤0`→frontend, `id<4`→mode 6, `id≥4`→`541498=5`→
mode 7).

### Counter / drain / completion state inventory (OBSERVED)

- `0x541554` — the displayed bonus/health pool. `FUN_00417e20`
  draws it directly (decimal digits gated `541554 > 0x14`; at
  ≤20 the digits blink — drawn only while the `0x49a8dc`
  accumulator `< 0x10`). There is NO separate display copy in
  mode 5: the rendered value IS the field. Writers inside the
  mode-5 call graph: `FUN_0042d24c` drain/latch (below) and
  `FUN_0042d118` pickup catch (`541554 = 150`, `0x42d221`).
- Pool drain (`FUN_0042d24c` @`0x42d89d`, wall-beat only — reached
  after the `FUN_0042d97c` probe hit + deflect + HITSIDE/HURTn
  emits): `skill==0` `−2`; `skill==1` `−(rand15(2)+2)`;
  `skill==2` `−(rand15(2)·2+4)`; skill outside {0,1,2} skips the
  drain entirely. Then `541554 ≤ 0` → `edad0` arm: final →
  `541554=0`, `ed748=1`, `eda9c=2.0` (writes in that order,
  @`0x42d955/d95b/d961`), non-final → `541554=1`. Both paths
  continue to the `0x42d8c8` speed decay the same frame.
- `0x4eda9c` — the frame drain accumulator. `+1/30` (`0x49b6f4`
  f32) per frame while `!complete`, clamps at `1.0` and installs
  the base palette; skipped entirely at `fade==1.0 && !complete`
  (`0x42ca90` JZ + `0x42cb19` re-check). While `complete`:
  `−1/30` per frame; `fade ≥ 0` continues the frame (walk/camera/
  draw all still run), `fade < 0` clamps to 0 and exits. The
  death write seeds 2.0 — a 60-step red-ramp drain before exit.
- `0x4ed748` — scene completion latch. Exactly three write sites:
  `0x42ca55` (twin `+0xe4 > 0x50`, s16), `0x42ca79`
  (`edad0 != 0 && FILD(e74b4) ≥ 186.0`, f64 `0x497008`),
  `0x42d95b` (heroUpdate death). The two frame-fn sites sit
  INSIDE the twin gate — `541554 ≤ 0` or `winLo ≤ 177 &&
  health != 1` skips them too. The twin write precedes the
  window write (`ca42` before `ca5f`).
- `0x4edad0` — final flag (course ≥ 4), seeded once by init.
- `0x4e74b0`/`0x4e74b4` — window bounds. Twin-gate entry uses
  `e74b0 > 177` (f64 `0x497000`); the terminal latch uses
  `e74b4 ≥ 186`; `FUN_0042be4c`'s final-mode freeze is a
  DISTINCT gate — `e74b4 > 186` (f64 `0x496f70`), marker spawn
  once, `winHi` pinned at 187. Latch and freeze never
  double-write.
- HUD-internal accumulators (inside `FUN_00417e20`, deferred
  with the `present` seam — numeric state only): `0x49a8dc`
  blink accumulator `+= 0x49b6e8 & 0x1f` per call (mode-5
  active; gates the ≤20 digit draw); `0x49a8e0` score-chase
  accumulator — sits behind `541492==3` (`0x417f54`), DEAD in
  mode 5.
- Exit handoff: `fade < 0` → `eda9c = 0`, DAC memset fill
  `0xff` iff `!edad0 && 541554 > 0` else `0x00`,
  `FUN_0046d208` install, return 1 → dispatcher teardown. The
  frame fn is never re-entered after that (the port latches
  `finished()`; a second `step()` is a no-op).

## 10. Palette / backdrop (OBSERVED)

- Scene palette `0x4ed758` = SYS_PAL head (entries 0–63) +
  `PAL[0xc0,0x300)` (64–255); upload `FUN_00413b40`/`FUN_0046d208`.
- Fade in `0x4eda9c`: non-final `rint(pal[i]·t + 255·(1−t))`
  (white-in), final `rint(pal[i]·t)` (black-in), dead
  (`541554≤0`) gray-desaturate (`·256` flat mix). Fade out at latch
  → white-flash/black exit per §9.
- Backdrop `FUN_0042e684`: parallax scroll of `BG` —
  `Δy = rint-accum((curR22·prevR22 − curT2·prevR12)·180)`,
  `Δx = rint-accum((curR10·prevR00 − curR00·prevR10)·300)`
  (exact cross-terms per `p19a_e684.asm`), stereo subtracts
  `0x49b578` from x, wrapped blit mod (600,360) into `0x541650`.

## 11. Mode-8 pipeline (OBSERVED)

`FUN_0047b06c`: teardown → `FUN_0047b0fc` (open
`MISC\FLIC\MDKEND.FLC` `0x4990a4`, stream-load w/ 350px progress
bar, FINISH.BNI WAV handles `EXPLODE1`/`DROP`/`FLYBY`/`ENDEXP`/
`DOGSHIP` → `0x54f398..a8`, decoder init `FUN_00413c20` → ctx
`0x54ef40`, 600-wide buffer) → `FUN_0047b3f4` frame pump
(`FUN_00414158` decode → `0x541650` → `FUN_0046c86c`; marks per
EXECUTABLE_MAP §8) → `FUN_0047b674` (`MISC\FLIC\MDKBZK.MVE`
`0x499130`; `0x489xxx` callback player @640×480, `PeekMessage`
pump) → `FUN_0041d85c` → frontend.

### FLIC decoder `FUN_00414158`/`FUN_00413c20` (OBSERVED)

- Header 0x80B; magic `0xAF11`(speed jiffies →·1000/70 ms) /
  `0xAF12`(speed ms); ctx fields `+0x108` stream pos,
  `+0x118` total frames, `+0x128` decoded, `+0x11c` w, `+0x120` h,
  `+0x13c` line ptr, `+0x140` pitch, `+0x144` 768B palette,
  handlers `+0x444..0x454` (variant×5 table `0x49a750`).
- Frame: `u32 size, u16 magic 0xF1FA, u16 chunks`; chunk dispatch
  on `u16` type: `0x4`→`h[0]` (`0x4144b8`, in-line label of the
  color decoder), `0xb`→`h[1]` `FUN_00414550` (COLOR packets —
  u16 count; per packet skip·3 then count·3 bytes → `ctx+0x144`),
  `7`→`h[2]` `FUN_0041465c` (DELTA_FLC word-pair delta —
  `u16&0xc000` line-opcodes, `0x4000` low-byte skip, signed count
  run/lit), `0xf`→`h[3]` `0x4145d8` (BRUN), `0x10`→`h[4]`
  `FUN_0041474c` (LCOPY — literal w×h rows), `0xc`/`0xd`/`0x12`
  skipped (PSTAMP thumbnail ignored). Bounded reads; in-memory
  stream mode flag `ctx+0x4e` bit1.
- The MVE stage (`0x489xxx` Interplay player) is a separate codec —
  out of Build-A core scope per the phase §22 stop rule; the core
  emits a stage-boundary event instead.

## 12. Port contract

- `CinematicRuntime` (new `src/core/stream_scene.*`) owns: path
  ring, node frames, planes, pen bytes, bucket ring, scene objects
  (reusing `DynamicObject`), updaters, camera, backdrop scroll,
  palette/fade, counters, completion latch. No Godot types.
- Reuse: `RuntimeModel`+`deepCopyModel` (`FUN_00403720`),
  `ObjectAnimView`/`objectAnimTick` (`FUN_004555bc`),
  `FUN_0046b2f8` Euler→3×4, `FUN_0046b4f8` project,
  `enemyRandNext/Below` (`FUN_0047d2b5`/`FUN_00401ed4`),
  `stream_context` palette/backdrop decode.
- Snapshot contract (copy-safe): objects (id, model/sprite ref,
  pos, matrix, animFrame), ribbons (per-slot 16 pts + pen),
  camera (M1/M2, eye), backdrop (scroll x/y), palette (fade t,
  effective table), counters, flags, completion.
- Mode 8 core: bounded FLIC decoder + frame-mark events +
  palette ramps + completion; MVE stage = boundary event.

## 13. Phase 19A.2B — actor updater family (implemented, OBSERVED)

The six mode-5 actor updaters are implemented in
`src/core/stream_scene.cpp` from the captured asm
(`analysis-private/logs/p19a_asm1/2/4.txt`, `p19a_batch1/5/7.txt`,
`p19a_dispatch.asm`). Dispatch order confirmed from the frame asm:
**hero → stray → escort → pickup → (twin skipped — `twinSync` owns
it) → generic**.

- `heroUpdate` (`FUN_0042d24c`): input axes staged via `input_`/
  `stepDt_`; speed grow `field34 → 6.0`; `zBias` advance; window feed
  `zBias-0.75 > winLo+1` → `migrate(hero/twin, winLo+1)` →
  `winLo_++` → `tunnelExtend()` in that order; animStep (`§14`); path
  frame `(zBias, zBias+1)`; gate `(twin_==null && health>0)` selects
  swim-ease+steer vs offset decay; yaw `[45,135]`/bank `[-45,45]`
  clamps and ±180°·dt recentre; d47f offset kicks
  (`cos yaw`/`sin bank`·25·dt); wall probe + ricochet (deflect,
  offset damp `1-wp`, HITSIDE + `rand(7)` HURT, skill-scaled drain,
  `·0.9`/floor-4.5 speed decay); death latch (final: `health=0,
  complete=1, fade=2.0`; non-final: `health=1` — arms the rescue gate
  via the `health==1` re-entry).
- `genericUpdate` (`FUN_0042cf6c`): `field34`-gated `zBias` advance;
  trunc→migrate→reap-on-fail; fractional path offset cleared after
  transform; no anim call.
- `escortUpdate` (`FUN_0042d034`): unconditional advance; migrate,
  reap+clear-pointer on fail; animStep (`§14`); escort frac constant.
- `pickupUpdate` (`FUN_0042d118`): advance, migrate, animStep,
  transform, hero-distance gate → APPLE + `health=0x96` + reap +
  pointer clear; fail path reaps without anim.
- `strayUpdate` (`FUN_0042db0c`): lead clamp to `hero zBias + 5`
  (copies hero `field34` when clamped); migrate; reap on fail keeps
  the stray pointer (OBSERVED asymmetry); trailing frame
  `(zBias-4, zBias-3)`.
- `twinSync` (`FUN_0042dabc`): copies hero pos[3] + the 48-byte
  `+0xac` transform block, ticks anim — **does NOT copy `zBias`**
  (OBSERVED desync; the rescue twin keeps its spawn-time path slot
  while mirroring position/orientation).

Regression: `mdk_tests` 9662/0, CTest 1/1, traversal L3–8 and
freefall c0–c4 canonical digests unchanged. Bounded real-data
diagnostic: `mdk-inspect --data-path <installed> --stream-init
--course N --skill 1 --stream-frames F` — per-frame updater reach,
window feed, animStep dispatch, state digest. Non-final course: hero +
pickup + generic every frame; course 4: hero + escort + generic;
death latch on c0 arms the `health==1` twin gate → `twinSync`.
`strayUpdate` unreachable on real data — nothing spawns the stray
slot (`strayIdx=-1` at init; OBSERVED, not a port gap).

Deferred seams preserved (counted, not implemented):
backdrop/draw/present, limiter, fillSelect internals.
`animStep` is implemented — see §14; the TELETYPE queue service is
implemented — see §15.

**MODE-5 CINEMATIC ACTOR UPDATERS: CLOSED FOR BUILD_A.**

## 14. Phase 19A.2C — the `FUN_004555bc` animator family (implemented)

`StreamScene::animStep` is the mode-5 port of `FUN_004555bc`, the
shared per-object animator dispatch called from `heroUpdate`,
`strayUpdate`, `escortUpdate`, `pickupUpdate` and `twinSync` (never
`genericUpdate` — OBSERVED). The dispatch order is the native one
(`p19a_helpers5.txt` 0x4555bc..0x4557ac, OBSERVED):

1. `+0x04 == -1` → **classless body** (`FUN_00455500` tail): rate-free
   timing — `+0xdc += +0xe0 * DT`, `+0xe4 = FRNDINT(+0xdc)`, repeat-wrap
   by the bound record's frame count (or the 0-frame hold when no
   record resolves). Never runs `FUN_00455890`. In
   `object_animation.cpp` (`objectAnimTickDt`), letter `C`.
2. `+0x0c == DAT_004edcc0` (shared class-table record 0) → **fuse
   body**: `+0xdc += DAT_0049b6f0` frame units (the port: `dt*30`);
   bound read through rec0's `+0x10` chain, `SAR >>0x10` — carried as
   `StreamAssets::animFuseBound` since no mode-5 spawn binds a
   class-table record (OBSERVED: spawns bind deep copies or null).
   `+0x149 & 0x40` selects loop-hold (`+0xe4 = bound-1`, letter `F`)
   vs `FUN_0045828c` in-place teardown keeping `+0x00`/`+0x60`
   (letter `T`). Unordered `FCOMP` lands on the end branch — NaN
   accumulators teardown/hold like the native JBE/JA.
3. **Hold body**: `+0x118 >= 0 && +0xe4 == +0x118`, or the `0xff00`
   done latch — resyncs `+0xdc = +0xe4`, no record read. Letter `H`.
4. **Null body**: `+0x114 == 0` or a record that fails the bounded
   `ObjectAnimView` walk — plain return. Letter `N`.
5. **Advance body**: `+0xdc += rate * +0xe0 * DT`; `+0x118` target
   clamp (caps AT the target); `+0x148 & 8` loop vs `frameCount-1`
   clamp; `FUN_00455890` applies `FRNDINT(+0xdc) - +0xe4` single-frame
   steps (root impulse `+0x294..+0x29c`, ref keys, named-channel
   vertex deltas/rigid frames, local bounds rebuild); non-loop end
   latches `0xff00`. Letter `A`.
6. **Advance + sound-marker body**: same path after consuming the
   `+0x140`/`+0x144` marker — the accumulator crossing the mark emits
   the name once (`FUN_00402fe8` resolve → `FUN_00402160` mode
   `0x1000e` positional at `+0x10`) and clears the slot. Ported as a
   name-bound `kPlaySound` event (`tag=-1`, `name` = the `+0x140`
   text, `aux=0x1000e`, `f[0..2]` = pos). Letter `S`.

Diagnostics: `StreamSeams` carries per-body counters
(`animCalls`/`animClassless`/`animFuse`/`animFuseEnd`/`animHold`/
`animNull`/`animAdvance`/`animSound` — body counters sum to calls);
`StreamScene::animLog()` is the per-step trace
(`slot : body [recSlot] frame/acc/latch`); `mdk-inspect
--stream-frames` prints it per frame. Real anim records are bound
with `animLimit` = the BNI image end so the `ObjectAnimView` walk is
bounded.

Reachability on real data (course 0/4 `--stream-frames` runs, bounded):
| body | non-final | final | notes |
|---|---|---|---|
| C classless | — | — | no `+0x04==-1` stream spawn observed |
| F/T fuse | — | — | no `+0x0c==classRec0` spawn observed (synthetic-only) |
| H hold | — | — | reachable state, not hit in bounded runs |
| N null | — | — | every spawned actor binds a valid record |
| A advance | **yes** | **yes** | hero KURTANIM + pickup SWHANM / escort GUNTANIM |
| S advance+sound | — | — | no `+0x140` writer on stream spawns (synthetic-only) |

Regression: `mdk_tests` 9723/0 (new `test_stream_animator` covers all
six bodies + malformed record + NaN acc), CTest 1/1; traversal L3–8
and freefall c0–c4 canonical digests unchanged; the real-data
`--stream-frames` state digests are bit-identical to the pre-19A.2C
baseline (animator state is not hash-covered).

**MODE-5 CINEMATIC ANIMATOR FAMILY: CLOSED FOR BUILD_A** — explicitly
not the full mode-5 core (backdrop/draw/teletype/limiter seams
remain).

## 15. Phase 19A.2D — the TELETYPE queue service (implemented)

Format finding (OBSERVED, `p19a_teletype.asm` + STATS.BNI bytes):
**there is no TELETYPE bytecode in BUILD_A.** The boundary is a
4-deep message queue plus a timed two-line text renderer — no opcode
dispatch, no PC, no object/camera operand anywhere in it. The STATS.BNI
record named `TELETYPE` is a `00 2c 00 00` + `RIFF WAVE` (8-bit mono
16 kHz) typing-sfx sample — unrelated data. Engine-global service:
traversal (`0x410920`), mode 5 (`0x42cc90`/`0x42ccb8`) and mode-6 paths
all call it.

- `FUN_0041cf5c` clear (init): writes `0x54b800`/`0x54b7fc`/`0x54b7f4`/
  `0x54b7f8` = 0 only — line buffers, entry flags and queue payloads
  stay stale.
- `FUN_0041cad0` post `(EAX=name, EDX=flags, [stk]=rate; RET 4)`:
  resolves the name through FTI (`FUN_00414890`) and stores the char*;
  `flags&2` front-pushes by decrementing `qRead` **before** the
  resolve — a failed front-push keeps the decrement (quirk). The ring
  has no full check — a post onto a full ring overwrites the tail.
- `FUN_0041cb44` service `(EAX = draw enable = 0x5414d4)`:
  `localRate = dt` when the queue is empty, `f32(dt * 2.0)` when an
  entry is pending — queued work halves the current message's hold.
  `charTimer==0` (x87: equal-or-unordered) with `holdTimer` integer
  ±0 and a pending entry → load `charTimer=rate`, `entryFlags`,
  `holdTimer=0`, `curLine=0`, then consume the str cursor: bytes into
  `lineBuf[curLine][col++]`, `\n` splits at line0/line1 (a second `\n`
  terminates mid-string), `\0` terminates → `curLine++`, `qRead++`.
  `charTimer!=0`: steady (`flags&1==0` or hold x87==0.5) draws the
  line(s) plain (`FUN_00414d2c` y=0x78 | 0x69+0x87 — the 0x541548!=0
  variant uses scaled `FUN_0041518c` at 1.0) then `charTimer -=
  localRate`, clamped at 0; `flags&1` and hold<0.5 → slide-in —
  `holdTimer += dt` to 0.5, scaled draws at `hold*2` (y `120∓t`,
  `t = hold*2*15` FRNDINT-trunc); `charTimer==0` with hold≠±0 →
  page-out — `holdTimer -= dt` to 0, same scaled geometry. Draws are
  `kTeletypeDraw` events; `0x4999d0 && 0x541548` suppresses only the
  timer update (never drawn out — no 0x541548 writer in BUILD_A).
- The service never touches pool objects, the camera, palette, or the
  trail config — object/camera command families do not exist here.

The port keeps the whole `0x54b7a4..0x54b834` block as one flat
byte arena (`ttMem_`) so the consume loop's **unbounded** line write
aliases the trailing scalars/queue exactly like the original (a line
past 36 bytes runs into `entryFlags`/`curLine`/timers/`qRead`/`qWrite`/
the queue); writes past the arena end are clipped and counted
(`seams().teletypeOverflow` — the one place the native would chase a
wild pointer is a corrupt str cursor, which the port bounds at the
entry's text).

Mode-5 reachability (course 0/4 bounded `--stream-frames`): the scene
posts nothing — the service runs the idle arm every frame; all live
arms are exercised synthetically. Regression: `mdk_tests` 9994/0,
CTest 1/1; traversal/freefall digests unchanged; teletype state is not
mixed into `stateHash` (`ttHash` covers the arena).

**MODE-5 TELETYPE SCRIPT SERVICE: CLOSED FOR BUILD_A** — not the full
mode-5 core (backdrop/draw/palette/fillSelect/limiter/trail seams
remain).

## 16. Phase 19A.2G — the Mode-5 trail/ribbon engine (implemented)

The trail the tunnel leaves behind the fly-through is a per-slot
triangle-band skin over the §6 ring geometry — decoded from
`p19a_asm2.txt` (e620), `p19a_asm3.txt`/`g1_walk.txt` (e100),
`g1_spanemit.txt` (0ca00), `p19a_batch7.txt` (be4c tail). All asm-
verified OBSERVED.

### Call chain (OBSERVED)

`FUN_0042c8b0` frame → `emitDrawList` (`FUN_0042e100`) → per bucket:
16× `project6b4f8` (FUN_0046b4f8) ring projection → 32×
`FUN_0042e620` ribbon calls → object chain → `FUN_00409a00` flush.
Each `e620` plane-tests then tail-calls the shared triangle gate
`FUN_0040ca00`, which trivial-rejects or hands the clipper
(`FUN_0040c860` raster dispatch — negative-pen family, host seam).

### Ribbon geometry — `FUN_0042e620` (OBSERVED)

Per bucket `cur` the native makes **exactly 32 calls**: a
15-iteration loop over `i=0..14`, then the `i=15` wrap segment as an
unrolled tail (loop bound = the `b[15]` record address):

| call | verts | plane | pen |
|---|---|---|---|
| `2i`   | `(b[i], a[i+1], b[i+1])` | `plane[2i]`   | `penBase − pen[2i]` |
| `2i+1` | `(b[i], a[i], a[i+1])` | `plane[2i+1]` | `penBase − pen[2i+1]` |

`a` = the higher slot's projected ring (native buf `[-0x44]`),
`b` = this bucket's (`[-0x38]`); the buffers swap at the bucket tail
so each ring projects exactly once. The two triangle families share
no pen byte — `pen[2i]`/`pen[2i+1]` are distinct pairs per ring edge
(OBSERVED: be4c writes two pen bytes per edge).

`e620` body (asm-exact):

```
dist = n.y*eye.y + n.x*eye.x + n.z*eye.z + d   (f80 order y,x,z,+d)
dist < 0  → return 1                          (backface skip)
else      → FUN_0040ca00(v0,v1,v2,aux,pen)    (≥0 or NaN/unordered)
```

The port accumulates in `double` to stand in for the x87 extended-
precision chain, same operand order.

`0ca00` order (OBSERVED): draw gate `0x5414d4==0` → return; then the
**AND** of the three verts' `+0x14` clip-flag bytes — nonzero is the
trivial reject; then the OR — nonzero enters the clipper, zero
dispatches direct. The port emits a pre-clip `kRibbonTri` event:
`tag` = the raw negative pen scalar, `aux` = `f0 | f1<<8 | f2<<16`
(the three packed flag bytes), `f[0..8]` = the three view-space
verts — copy-safe values, never pointers into the ring buffers.

### Window / bucket traversal quirks (OBSERVED)

`cur = (winHi−1)&0x1f`, `stop = winLo&0x1f`; the exit test runs on
the MASKED slots BEFORE the decrement, so:

- buckets `winHi−2 .. winLo` are processed — `winHi−1`'s ring feeds
  the first ribbon band but **its objects are never drawn**;
- a window with `(winHi−winLo) ≡ 1 (mod 32)` draws zero buckets —
  `span 33` silently no-ops like `span 1`;
- `span 32` draws the full 31 buckets `winHi−2..winLo`.

Pen base is distance-banded on `dist = cur − winLo` (decremented
per bucket, OBSERVED ladder):
`>25 → −0x545 / 21..25 → −0x505 / 16..20 → −0x4c5 /
11..15 → −0x485 / 6..10 → −0x445 / ≤5 → −0x405`. All values are the
negative material indices the `0c860` dispatch remaps through the
`0x412970` filler LUT (`(−1029−pen)·256` row select).

### Ordering (OBSERVED)

Per bucket: 32 e620 calls **first**, then the object-chain record
appends (models flag-0, sprites flag-1), then one 09a00 flush iff ≥1
record appended — flag-0 records fire in chain order, flag-1 drain
descending by key (far-first). So ribbon triangles precede that
bucket's models and sprites; the closed model/sprite ordering is
unchanged (19A.2F tests still pass untouched).

### Trail update — `FUN_0042be4c` tail (OBSERVED, pre-existing)

`seams_.trailUpdate` now counts be4c calls: `cur = winHi&0x1f`,
`prev = (winHi−1)&0x1f`; final-mode freeze gate `winHi > 186`; the
current bucket drains, 16 ring points regenerate at 22.5° with
jittered radius, and — when a `prev` segment exists — 2 pen bytes +
2 plane records per ring edge write into the PREVIOUS slot
(truncated `penBase·(1−penT) + penTarget·penT`, `penT += 0.1`,
target re-roll at completion), plane normals normalized and `d`
computed; `winHi++`; drift/radius update; `fillSelect(0)` seam.

### Diagnostics (course 0 / course 4, `--stream-frames` to natural exit)

| metric | course 0 | course 4 |
|---|---|---|
| frames / exitFrame | 466 / 465 (hero latch) | 354 / 353 (death) |
| e620 calls | 446400 (13950 buckets ×32) | 338880 (10590 ×32) |
| `kRibbonTri` emitted | 139162 | 97740 |
| plane culls | 252576 | 193316 |
| gate skips | 0 | 0 |
| trivial rejects | 54662 | 47824 |
| clip takes | 10738 | 8333 |
| trail updates (be4c) | 114 | 94 |
| window high water | 31 | 31 |
| record arena high water / overflow | 28 / 0 | 28 / 0 |
| pen digest | `2191c615a6734cee` | `9c2bfe130e0a4b90` |
| geometry digest | `3e4b35eba5ec4c8f` | `ae71dbe78491c257` |

`emit + cull + reject + gate = calls` on both courses. The ~57%
plane cull is the backfacing half of the tube; gate never fires on
the real path (`emitDrawList` is itself `drawDue_`-gated).

### Reachability

| branch | course 0 | course 4 | synthetic |
|---|---|---|---|
| 32-call band emit | yes | yes | yes |
| plane backface cull | yes | yes | yes |
| 0ca00 draw gate | no (`due=1` always) | no | yes |
| trivial reject (AND) | yes | yes | yes |
| clip take (OR) | yes | yes | yes |
| `span≡1 (mod 32)` zero-draw | no (span≤31) | no | yes |
| 64-record arena overflow | no | no | yes |
| distance-band ladder bands | yes (all six) | yes | yes |
| negative-pen `0c860` remap | — host seam — | — host seam — | tag passthrough |

Regression: `mdk_tests` 145846/0 (new `test_stream_ribbon` covers
minimum/zero/wrapped windows, multi-segment order, tri count/order/
winding, pen pairs + the six-band ladder, plane cull, gate, trivial
reject, per-vert flag packing, near/side clip, arena capacity, copy
safety across ring mutation, reap, window slide, teardown/re-init,
and deterministic replay), CTest 1/1; traversal L3–8 and freefall
c0–c4 canonical digests unchanged.

**MODE-5 TRAIL / RIBBON ENGINE: CLOSED FOR BUILD_A** — explicitly
not the full mode-5 core (backdrop internals, palette host upload,
fillSelect internals, limiter, present remain seams).
