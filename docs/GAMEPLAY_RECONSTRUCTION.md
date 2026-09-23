# Gameplay Reconstruction — Phases 5A–5B status

Phase 4 is **CLOSED at Phase 4K**. The remaining front-end children
(Help `FUN_0041d540`, Joystick, Performance) and real frontend audio
playback are deferred compatibility/polish work — they do not block
keyboard+mouse gameplay reconstruction.

**Phase 5A reproduces the original per-frame gameplay-input
consumption layer only**: raw keyboard/mouse state + the configured
bindings → the semantic control block. No world mutation.

**Phase 5B reproduces the first downstream player-movement consumer
of that control block**: `FUN_00465228` — the normal-movement
integrator — from the merged rates to player-local kinematic state
(three velocity channels, yaw, bank/roll) plus the displacement the
collision seam receives. Collision, vertical/jump, camera, sniper,
slide, mantle, and the item/fire dispatch tail remain deferred
boundaries (§15 onward).

Evidence labels follow `docs/reverse-engineering/EVIDENCE_POLICY.md`.

## 1. The original input-consumer function chain

Per frame (OBSERVED, MDK95.EXE BUILD_A):

```
FUN_004187e0   per-frame input pump (called from the frame loop)
  ├─ FUN_0046b688  keyboard poll    -> level/latch/prev bitmaps
  │                                 (internal 0..127 codes, 4 dwords)
  ├─ FUN_0046bc18  mouse poll       -> 0x54b644 dx, 0x54b648 dy,
  │                                 0x54b64c dz, 0x54b650 button nibble
  ├─ FUN_0046b9b4  joystick poll    -> 0x54b534 buttons / 0x54b538 axes
  └─ FUN_00419370  action flags     -> 0x54b650..0x54b6c8 flag block
                                     from the 29-dword binding block
                                     0x5413fe (helpers below)

in traversal (FUN_00436100 -> FUN_00463608):
  FUN_00406f14   control merge      -> 0x4ce6e0..0x4ce7ac from the
                                     flags + W-set mouse map/scales +
                                     button masks + joystick tables
```

Lookup helpers under `FUN_00419370` (all OBSERVED):

| Address | Role |
|---|---|
| `FUN_004192cc` | level query `(levelWord >> code) & 1` |
| `FUN_00419320` | edge query on the `latch & ~prev` bitmap |
| `FUN_0041925c` | right-modifier fold (see §4) |
| `FUN_00419168` | lowest-set-bit scan (menu cursor use) |

Direct raw-delta consumers that do NOT go through `FUN_00406f14`:

- `FUN_00464624` — free-look/camera: reads raw dy and applies
  `MouseYReversed` (`!= 0` → negate) at ITS consumption point.
- `FUN_004691c4` — sniper/reticle: raw dx/dy + `MouseOn` + the fire
  flag; does NOT apply `MouseYReversed`.

Both are Phase 5B+ consumers; Phase 5A preserves the raw deltas and
the Y-reverse state in the frame so neither has to re-derive them.

## 2. Keyboard runtime block → semantic map

The 29-dword block at `0x5413fe` (factory mirror `0x49b1f2`) is
consumed by `FUN_00419370` in a fixed call sequence. The flag-block
targets (OBSERVED from the disassembly call order) map to the
`FUN_00406f14` mask bits as:

| Slot | Factory | Query | Semantic (mask bit) |
|---:|---:|---|---|
| 0 | 105 LEFT | level | MoveFwd… correction: turn LEFT → turn −1 |
| 1 | 106 RIGHT | level | turn +1 |
| 2 | 103 UP | level | move −1 (forward) |
| 3 | 108 DOWN | level | move +1 (back) |
| 4 | 56 LALT | level | Jump `0x0004` |
| 5 | 45 X | level | SideStep modifier `0x0400` |
| 6 | 29 LCTRL | level | Fire `0x0001` |
| 7 | 57 SPACE | edge | Sniper `0x0002` |
| 8 | 42 LSHIFT | level | Turbo `0x8000` |
| 9 | 58 CAPS | edge | STURB — toggles latch `0x540d38` |
| 10 | 30 A | level | LookUp `0x0008` |
| 11 | 44 Z | level | LookDown `0x0010` |
| 12 | 30 A | level | ZoomIn `0x0100` → zoom acc +1 |
| 13 | 44 Z | level | ZoomOut `0x0200` → zoom acc −1 |
| 14..23 | 2..11 = `1`..`0` | edge | ten direct-weapon selects |
| 24 | 27 `]` | edge | ItemNext `0x0040` |
| 25 | 26 `[` | edge | ItemPrev `0x0080` |
| 26 | 28 RETURN | edge | ItemUse `0x0020` |
| 27 | 51 `,` | level | StrafeLeft `0x0800` |
| 28 | 52 `.` | level | StrafeRight `0x1000` |

Mask-bit ↔ semantic names are OBSERVED from the original's own
`JOY_BA`..`JOY_BP` records (Fire, Sniper, Jump, LookUp, LookDown,
UseItem, NextItem, PrevItem, ZoomIn, ZoomOut, SideStep, StrafeL,
StrafeR, MoveFwd, MoveBack, Turbo).

### Held vs edge (OBSERVED per binding)

- **Level**: LEFT/RIGHT/UP/DOWN, JUMP, SIDE, FIRE, TURBO, LKUP,
  LKDWN, ZOOMI, ZOOMO, SIDEL, SIDER.
- **Edge**: SNIPE, STURB, all ten weapon hotkeys, INEXT, IPREV, IUSE.

Two edge flags (`0x54b688/8c` — zoom-edge copies) exist in the flag
block but have no BUILD_A reader; not reproduced.

## 3. The ten hidden weapon hotkeys

Global indices **14–23** hold factory internal codes **2–11** — the
`1`..`0` keys. Each is **edge-queried** into
`0x54b690 + 4·(i)` = `weaponSelect[i]`. Simultaneous edges in one
frame all land — the original writes every slot, so a later
consumer resolves priority (not flattened in 5A). They are NOT in
the settings table (entries 69–87 cover only the visible 19), so
they are never persisted — the Reset path restores them from the
mirror, and `keyboardGlobalsFromSettings` always yields factory
hotkeys (the post-Reset state).

## 4. Right-modifier folding (OBSERVED)

`FUN_0041925c` folds the right-side modifier into the left query —
inside both level and edge lookups:

| Query code | Also sees |
|---:|---:|
| `0x2a` LSHIFT | `0x36` RSHIFT |
| `0x1d` LCTRL | `0x61` RCTRL |
| `0x38` LALT | `0x65` RALT |

Results are raw masked values (e.g. `0x400400`), not normalized 1 —
consumers test `!= 0`. Reproduced in `gameplayKeyLevel/Edge`.

## 5. Mouse W-set vs D-set (BUILD_A conclusion)

- **W set is the active route** — `MouseWAxesMap` (`0x5413de`),
  `MouseWButtMapA..D` (`0x5413be..ca`), `MouseWX/Y/ZScale`
  (`0x5413e6..ee`) are all read by `FUN_00406f14`.
- **D set is dead state in BUILD_A** — the `MouseD*` globals have
  zero code xrefs. Documented dead for the Win95 build; the native
  settings round-trip preserves them for file compatibility but the
  gameplay path never reads them. No selector exists.

## 6. Mouse axis letters (OBSERVED — `JOY_AA`..`JOY_AH` + `JOY_A0`)

The map string's **position indexes the axis** (0=dx, 1=dy, 2=dz);
the letter selects the semantic:

| Letter | Action | Sign |
|---|---|---|
| `0` | off — axis ignored | — |
| `A` | turn | + |
| `D` | turn | − |
| `B` | move | + |
| `E` | move | − |
| `C` | strafe | + |
| `F` | strafe | − |
| `G` | sniper zoom | + |
| `H` | sniper zoom | − |

`A`/`D` route to **strafe** instead of turn while the SideStep
modifier is held. The `G`/`H` scan **stops at the first G/H
letter** — an OBSERVED early-out (later axes' letters never run).

## 7. Mouse scale formula (OBSERVED)

A–F letters, per axis:

```
v = delta / scale            // float division, scale is the setting
if letter in {D,E,F}: v = -v // sign flip BEFORE the deadzone
if |v| < 0.2:        v = 0   // deadzone
rate = clamp(v / smoothed, -4, +4)  // smoothed = 0x49b6f0 (~1.0)
```

The rate then multiplies the same per-axis constants the keyboard
path uses (e.g. turn `×6×0.5`, strafe `×(4/3)×0.5`) and **overwrites
the shared rate fields** — a nonzero mouse axis wins over a held
keyboard key (OBSERVED priority).

G/H letters:

```
ticks = rint(delta / scale + sign(delta) * 1)  // the ±1 bias makes
                                               // any nonzero delta
                                               // ≥1 tick
acc += (G ? +ticks : -ticks); clamp acc to [-8, 8]
```

## 8. Zoom accumulator tail (OBSERVED, corrected in 5B)

`0x4ce760` decays by the integer frame-step `0x49b6e8` toward 0 each
frame (the timing machine's `clamp(accum>>2,1,4)` count — **1** at
the nominal rate, not a ~33 ms quantity; see §13/§15 for the
corrected provenance). While `|acc| ≥ 1` the frame emits the zoom
velocities (±0.01 slow / ±0.15 fast, sign per accumulator sign).
Net effect: a single wheel detent (dz=120, scale 50 → 3 ticks)
emits exactly one frame of zoom velocity — reproduced in the tail.

## 9. MouseOn and MouseYReversed (OBSERVED)

- `MouseOn` (`0x541472`) gates **both** mouse axis scans (G/H and
  A–F). It does **NOT** gate the button-mask decode — buttons fire
  even with the mouse disabled (reproduced quirk).
- `MouseYReversed` (`0x541476`, a type-1 float slot toggled as raw
  bits) is tested `!= 0` **by `FUN_00464624`** at the free-look
  consumption point — not by `FUN_00406f14`. Phase 5A passes the
  raw dy and the nonzero-bit state through to the frame.

## 10. Mouse buttons (OBSERVED)

`DIMOUSESTATE.rgbButtons` nibble bit *i* = physical button *i*
held → selects `MouseWButtMap[A..D]` (A=left, B=right, C=middle,
D=fourth; the SDL seam maps LEFT/RIGHT/MIDDLE/X1 to bits 0–3).
Factory masks `{1, 4, 2, 0}` = Fire / Jump / Sniper / none.

- Each held button applies its 16-bit mask in button order; one
  button may set several action bits; later buttons overwrite the
  axis pre-seeds (sequential writes, as observed).
- The sniper bit is a **level → synthetic edge**: `0x499f50` latches
  the previous request so a held snipe button pulses once.
- No exclusivity is enforced at consumption — the menu's
  exclusivity table is an editing concern only.

## 11. Platform boundary and the native frame

```
SDL events
  → InputState (platform layer)
  → frontendInputFromSdl   — SDL scancode → DIK → internal 0..127,
                             level/latch/prev bitmaps (the
                             FUN_0046b688 analogue); DIMOUSESTATE-
                             domain mouse deltas (wheel ticks × 120)
  → RawGameplayInput       — keyLevel/keyEdge + dx/dy/dz + buttons
  → consumeGameplayInput   — FUN_00419370 + FUN_00406f14 analogue
  → GameplayInputFrame     — the semantic control block
```

`GameplayInputBindings` is built by
`gameplayBindingsFromSettings(FrontendSettings)` — the 29-dword
block via `keyboardGlobalsFromSettings`, the W-set mouse fields
directly. No SDL, frontend controller, renderer, or filesystem
dependency enters `src/core/gameplay_input.*`.

`GameplayInputFrame` carries (with original output addresses):

- Action flags `0x4ce768..0x4ce7ac` — jump, sniperPulse, fire,
  itemUse/Next/Prev, lookUp/Down, `weaponSelect[10]`.
- Modifier/latch state — `sideStepHeld`, `turboLatched`,
  `sniperButtonLatch`/`setTurboLatch`/`zoomAccumulator` live in
  `GameplayInputState` across frames.
- Axes — `turnAxis`, `moveAxis`, `moveDigital`, `strafeAxis`,
  `yawAxis` (the merged ±1/analog intermediates).
- Rate products `0x4ce6e0..0x4ce73c` — all OBSERVED double
  constants (0.9/4/1.3/6/0.75/0.4/0.6/45/⅓/10/(1/22.5)/⅔/(4/45)/
  (4/3)/0.5/0.05/35|15-asymmetric/(20/3)-debug), including the two
  OBSERVED quirks: the 0.75-scaled pair always uses non-turbo
  constants, and `moveSpeed` multiplies the *digital* move axis
  (0 for pure-mouse input).
- Pass-through — raw `mouseDx/Dy/Dz`, `mouseButtons`, `mouseOn`,
  `mouseYReversed` for the `FUN_00464624`/`FUN_004691c4` consumers.

## 12. Diagnostics and tests

- `--selftest-gameplay-input` — deterministic 17-frame script
  through the real SDL→DIK→internal seam (`SdlHost::
  pushGameplaySelfTestStep`); per-frame expectations are computed
  from the loaded bindings, so the same script verifies factory and
  `--settings-file` configurations. Exits RC 3 on mismatch.
  Standalone — cannot combine with `--interactive-frontend`.
- `tests/native` — `test_gameplay_input`: level/edge/fold queries,
  hidden hotkeys (simultaneous + held-no-edge + rebind/reset
  round-trip), duplicate bindings, STURB latch, turbo constants,
  item edges, all axis letters, scale/deadzone/clamp, G/H zoom +
  clamp + early-out, MouseOn axis-gate vs button pass-through,
  button masks/multi-bit/synthetic snipe edge, combined
  keyboard+mouse priority, settings-derived bindings.
- Unit tests construct `RawGameplayInput` directly; the app test
  never writes bitmaps — it injects real SDL events.

## 13. Remaining input unknowns (for later phases)

- `0x5414e4` (`debugMoveBoost`) — debug-command flag; with Tab
  level it overrides move rates to 20/3. Modeled, default 0.
- Joystick consumption — `FUN_0046b9b4` outputs and the
  `JoyOn`/`JoyType`/axis-map/button-mask tables are modeled because
  `FUN_00406f14` reads them, but nothing upstream feeds them;
  `joyOn=false` reproduces the canonical no-device path.

Resolved by Phase 5B (moved here from the former unknowns list):

- `0x540c80` (`moveBoostGate`) — **OBSERVED**: the jump-sustain
  flag. `FUN_00466740` sets it to 1 while the airborne counter
  `0x540c84 != 0` and the jump input stays held; it gates both the
  ×4/3 horizontal move lift (here) and the weaker jump-sustain
  gravity inside `FUN_00467180`. Kept as an environment input —
  the airborne counter's writers are the deferred vertical system.
- `0x49b6f0`/`0x49b6e8` provenance — **OBSERVED** (Phase 4F timing
  machine, `FUN_0042fcd0`/`FUN_0042fdc8`): `0x49b6f0` is the
  smoothed frame-units factor `f0` (EMA `x·0.75+units·0.25`, ≈1.0
  nominal — the mouse-rate divisor and the integrator's accel/
  displacement scale); `0x49b6e8` is the integer frame-step
  `clamp(accum>>2,1,4)` (the zoom-accumulator decrement). Phase 5A
  had mislabeled them as ~33 ms quantities — defaults corrected to
  `smoothedDelta = 1.0f`, `frameStep = 1`.

# Phase 5B — the normal-movement integrator (FUN_00465228)

Phase 5B reconstructs **one bounded layer**: the first downstream
player-local movement consumer of the merged control block.

## 14. Control-block reader xref map

Direct readers of the movement-related control fields
(`0x4ce6e0..0x4ce73c`), separated by consumer class (OBSERVED from
the xref table):

| Control field | Phase-5A field | Movement readers | Other readers (deferred) |
|---|---|---|---|
| `0x4ce6f8`/`0x4ce6fc` | `strafeNorm`/`strafeFast` | `FUN_00465228` | `FUN_0046603c` (slide) |
| `0x4ce700`/`0x4ce704` | `turnNorm`/`turnFast` | `FUN_00465228` | `FUN_0046603c` |
| `0x4ce708`/`0x4ce70c` | `moveVel`/`moveVelBoosted` | `FUN_00465228` | `FUN_0046603c`, `FUN_00466aec` (mantle gate) |
| `0x4ce710..` | `yawNorm`/`yawFast`/`yaw*` products | — | `FUN_00465c4c` (look/pitch), `FUN_0046603c` |
| `0x4ce764` | `mouseTurnActive` | `FUN_00465228` | — |
| `0x4ce768` | jump flag | — | `FUN_00466740` (vertical/jump) |
| `0x4ce76c`/`0x4ce770`/`0x4ce774` | item/fire paths | — | `FUN_00469cd0` (item dispatch), tail |
| `0x4ce780` | look/pitch path | — | `FUN_00465c4c` |
| `0x4ce760` | zoom accumulator | — | sniper zoom path |

Only the movement/locomotion consumers proceeded in 5B; camera
(`FUN_00464624`), sniper (`FUN_004691c4`), and the item/fire tail
remain untouched.

## 15. The dispatcher and the one-frame input order

`FUN_00436100 → FUN_00463608` is the per-frame traversal driver
(OBSERVED, single call sites). `FUN_00463608` is a **dispatcher**:
it zeroes the event word `0x54cb00/08`, runs one player-state
branch selected by `DAT_00540cac`, then runs a shared tail:

```
FUN_00463608:
  event word = 0
  switch (DAT_00540cac):           // player-state gate
    < 800:  if (e6c==0 && c9c==0)  FUN_00465228(param_1, 0)   // normal
            else mount/drive branch
    800:    FUN_0046603c(...)      // slide/dash mode — deferred
    ...                            // other state branches
  shared tail:
    FUN_00464d10                   // debug fly-mode toggle
                                   //  (DAT_005414e4 → cac=100)
    FUN_00406f14                   // merge NEXT frame's control
    FUN_0047d20a(0x4ce6e0,0xd0,0)  // dispatch/record the 208-byte
                                   //  control block
```

**OBSERVED ordering consequence**: the integrator consumes the
control block merged by the PREVIOUS frame's `FUN_00406f14` call —
the pipeline carries exactly one frame of input latency. The native
port reproduces this by integrating `prevFrame` while the current
frame's input is being merged.

`FUN_00465228(param_1, 0)` never reads either argument — the
integrator is a pure global-state machine over the player block
`0x540bfc..0x540eb8` plus the control block.

## 16. Selected target and player state

Selected target: **`FUN_00465228`, the normal-movement integrator**
— the smallest function that turns control rates into persistent
player-local kinematic state (velocity channels + yaw + bank), and
whose output (a displacement vector) is consumed by the collision
call `FUN_004630d4`. Chosen over `FUN_0046603c` (a separate gated
slide mode), `FUN_00466740`/`FUN_00467180` (the vertical/jump
consumer), and `FUN_00465c4c` (the look/pitch event consumer) —
all sibling branches, not parents of this path.

The player state the normal path proves (all OBSERVED; the
structure is the flat `0x540bfc..` block, not a passed object —
`FUN_00465228` ignores its stack args):

| Address | Native field | Proven meaning |
|---|---|---|
| `0x540c2c` | `yawDeg` | persistent yaw, **degrees**, wrapped [0,360) by ±360 constants `0x498910/0x498914` |
| `0x540d48` | `moveVel` | forward/back velocity channel (units/frame-unit) |
| `0x540d4c` | `strafeVel` | strafe channel (+ = right) |
| `0x540d50` | `turnVel` | yaw-rate channel (deg/frame-unit) |
| `0x540b4c` | `bank` | bank/roll accumulator, clamped ±10 |
| `0x540c84` | `airCharge` | airborne counter (drained here; written by the vertical system) |
| `0x540cc0` | `moveDirLatch` | ±1 move-direction latch |
| `0x540c94` | `turnLock` | turn-direction lockout (sibling-mode writers) |

Environment gates read from sibling systems (not owned here):

| Address | Env field | Proven effect |
|---|---|---|
| `0x540d9c` | `masterGate` | nonzero → immediate RET (no accel/decay/disp/events) |
| `0x540e4c` | `groundContact` | selects accel/decel scales; gates conveyor |
| `(0x540e4c+0x20)&4` | `lowFriction` | flag-4 ground: scales 0.5/0.1 |
| `0x540dc0`/`0x540dc8` | `moveBlocked` | both set → move input skipped (channel still decays) |
| `0x49b6f0` | `smoothed` | the `f0` frame-units factor |
| `0x540e4c`+contact list | `conveyorX/Y/Z` | `FUN_00412ef0` surface-effect contribution, pre-multiplied by `0x49b6f4` |

The player OBJECT (`DAT_00540c48`) is only touched at the collision
seam (`+0x462` pitch limit, `+0x2c/0x28/0x24` collision cells,
`+0x68` contact list) — deferred with collision.

## 17. Channel semantics (all OBSERVED at instruction level)

The integrator runs three independent velocity channels through two
helper shapes, then decays whichever channels received no input
(`FUN_00465a84`):

| Channel | Input product | Cap | Helper | Decay |
|---|---|---|---|---|
| `d48` moveVel | `moveVel` rate ×accelScale | `moveVelBoosted` | `FUN_00465b54` (f0-scaled) | `4/45` in / `8/45` out ×decelScale, bound ±2/3 |
| `d4c` strafeVel | `strafeNorm` ×accelScale | `strafeFast` | `FUN_00465b54` | same as move |
| `d50` turnVel | `turnNorm` | `turnFast` | `FUN_00465b54` (mouse) / `FUN_00465bd8` (kbd — raw add, NOT f0-scaled) | `0.55` in / `1.6` out, bound ±4 |

Helper semantics (OBSERVED): accelerate adds `rate·f0` toward the
signed cap, but a **sign reversal replaces** the velocity with the
increment (no brake-through-zero); the keyboard turn path uses the
unscaled variant — an OBSERVED asymmetry. Decay steps toward 0 by
`rIn` inside ±bound, `rOut` outside, snapping on crossing.

Accel/decel **scales** come from the ground state: `e4c==0` →
(0.75, 0.75); `e4c` without flag 4 → (1.0, 1.0); flag 4 →
(0.5, 0.1). Only move/strafe are scaled — turn decay is constant.

The `turnLock` (`0x540c94`) gates turn **input** (decay still
runs): 1 persists while `turnNorm<0`, 2 while `turnNorm>0`, else
clears — the lock itself is written by sibling modes.

## 18. Displacement, yaw, and the coordinate convention

After the channels settle (OBSERVED compose order):

```
basis = FUN_00437f98(yaw):  sin = sin(yaw·π/180), cos = cos(yaw·π/180)
disp += conveyor (groundContact only — FUN_00412ef0's output)
disp.x += moveVel·f0·cos + strafeVel·f0·sin
disp.y += moveVel·f0·sin − strafeVel·f0·cos
yaw   -= turnVel·f0          wrapped [0,360) via ±360 constants
```

So the local frame is **+X forward at yaw 0, +Y left** (a +strafe
"right" channel subtracts from Y), yaw in degrees, positive
`turnVel` decreasing yaw (turn-left key → negative `turnNorm` →
yaw increases). Displacement components **add** — no diagonal
normalization anywhere in the original.

`dispX/Y/Z` (Z = conveyor only here) is exactly what
`FUN_004630d4` receives — the layer stops at that call. The
collision/query/apply system, floor snapping, and gravity are
deferred; the native port exposes the seam as an output the caller
applies or discards.

## 19. Events, bank, and the post-step rules

Movement event word `0x54cb00/0x54cb08` (OBSERVED emit rules):

- `moveVel ≠ 0` → type 6 mag 600 (forward) — emits first and sets
  the consumed-latch bit that suppresses the strafe event;
  `moveDirLatch` is rewritten to ±1 by the channel sign.
- `strafeVel ≠ 0` → type 5 mag 500, only when no move event fired.
- `turnVel ≠ 0` → type 4 mag 400, only when no move event fired
  AND no live strafe input was consumed this frame.
- Post-collision (0x4655f3..0x4658ea): a move event whose apply
  produced **no position change** is cancelled back to 0.

Bank/roll `0x540b4c` (OBSERVED): while the move input is consumed,
`sign(turnNorm·moveVel)` drives the accumulator at `±f0·0.25`/frame
with a `±2` snap-through kick when crossing zero, clamped ±10, and
sets `0x54cb04`. In the dispatcher tail, when the bank event did
not fire, `bank` decays toward 0 by `clamp(|bank|·0.35, 0.05, 2.5)
·f0` — proportional decay with a floor and a cap (constants
`0x4986c0/c8/d0`).

Air-charge `0x540c84` (OBSERVED): while the forward move input is
consumed, the counter drains toward 20 at `f0·1.75` (soft ceiling
60 applied first). The counter itself is written by the deferred
vertical system (`FUN_00466740`/`FUN_00467180`).

## 20. Native boundary (`src/core/player_motion.*`)

```
GameplayInputFrame ──► integratePlayerMotion ──► PlayerMotionOutput
      (prev frame)     (FUN_00465228 mirror)       dispX/Y/Z + events
                                                   │
caller resolves displacement (collision seam — open)
                                                   ▼
                            playerMotionPostStep   event cancel +
                                                   air-charge drain +
                                                   bank tail decay
```

- `PlayerMotionState` — the eight persistent fields above only.
- `PlayerMotionEnvironment` — `smoothed`, `masterGate`,
  `groundContact`, `lowFriction`, `moveBlocked`, `conveyorX/Y/Z`.
- The output displacement is player-local and pre-collision; the
  app feeds `positionChanged` from the (deferred) apply result.

## 21. Phase 5B diagnostics and tests

- `--selftest-player-motion` — a 24-step deterministic SDL script
  (`SdlHost::pushMotionSelfTestStep`) through the real
  SDL→DIK→internal seam → `consumeGameplayInput` →
  `integratePlayerMotion(prevFrame)`. The verifier checks the
  channel/yaw/bank/event values per frame, binding-adaptively —
  the same script proves the **one-frame input latency** (f0
  integrates a zero block while a key is already held), accel/
  decay ramps, SideStep strafe, turbo rates, a custom-bound move
  key (`KeyUp=17` → 'W'), and the mouse axis-0 impulse (turn under
  `'A'`, strafe under `'C'`, inert under `'0'`). Mutually exclusive
  with `--selftest-gameplay-input` and `--interactive-frontend`
  (RC 2); RC 3 on mismatch.
- `tests/native` — `test_player_motion`: idle, turn ramp/decay/
  wrap, move/strafe/diagonal compose, turbo, mouse override,
  multi-frame state evolution, turn-lock, move-block, master gate,
  conveyor, event precedence + post-collision cancel, air-charge
  drain, bank drive/decay.

## 22. Deferred boundaries and remaining movement unknowns

Documented boundaries — siblings of this layer, NOT reconstructed:

- `FUN_004630d4` — collision query/apply (the disp consumer).
- `FUN_00466740`/`FUN_00467180` — jump sustain + vertical/gravity
  integration (`0x540c78` vertical velocity, `0x540c80`
  jump-sustain, `0x540c84` airborne counter writer).
- `FUN_00465c4c` — look/pitch event consumer (`yaw*` products).
- `FUN_0046603c` — slide/dash mode (`cac==800`, `0x540e24`-gated;
  uses `0x49b6f4` delta-seconds, not `f0`).
- `FUN_00466aec` — mantle/ledge-grab (gated on `moveNorm>0`,
  `c78≤−0.25`).
- `FUN_00469cd0` + the `FUN_0047d20a(0x4ce6e0,0xd0,0)` tail —
  item-action dispatch and the 208-byte control-block record.
- `FUN_00464624` (camera), `FUN_004691c4` (sniper) — unchanged
  Phase 5A boundaries.

Remaining unknowns inside the movement layer:

- The full `0x540cac` player-state enumeration — only `<800`
  (locomotion), `800` (slide), and the debug `100` fly-mode are
  observed; sibling-mode writers of `turnLock`/`c54`-style gates
  are only partially enumerated.
- `FUN_00412ef0`'s surface-ID scan details (contact-list record
  layout) — modeled as an env output, not re-derived.
- Whether `0x540b4c` bank feeds a visual roll or gameplay
  response downstream — consumer UNKNOWN.
- The mount/drive branch (`e6c!=0`/`c9c!=0`) — observed to slave
  the player's basis fields, not reconstructed.

## 23. Recommended Phase 5C target

The vertical/locomotion sibling: `FUN_00466740` (jump sustain) +
`FUN_00467180` (gravity/vertical integrate) — they consume the same
control block's jump flag and write `0x540c80`/`0x540c84`, which
the horizontal layer already reads. They are the smallest
remaining step toward a standing player before collision
(`FUN_004630d4`) becomes unavoidable. Camera (`FUN_00464624`) is a
viable alternative if input-only verification matters more.
Not started in Phase 5B.

# Phase 5C — jump sustain and vertical gravity

Phase 5C reconstructs **one bounded layer**: `FUN_00466740` (the
jump-state machine) and `FUN_00467180` (vertical gravity +
collision-result handling), stopping at a semantic `FUN_004630d4`
seam. No world, collision geometry, mantle internals, or level
logic is reconstructed.

## 24. Exact per-frame call order (OBSERVED)

Inside `FUN_00463608`'s `cac<800` branch, `FUN_00465228` runs the
horizontal integrator and then, in its own tail, the vertical
chain — all still consuming the PREVIOUS frame's merged control
block (the 5B one-frame latency applies to the jump flag too):

```
FUN_00465228 tail:
  FUN_004630d4(ctx, mode, dx, dy, 0, r=0.75, ...)   horizontal move
  FUN_0046603c()                                   slide helper (deferred)
  FUN_00466740(in_EAX)                             jump-state machine
    FUN_00467180()                                 vertical gravity +
      FUN_004630d4(ctx, mode, 0, 0, dispZ, r=0.5, 0, &e50)
    FUN_00466aec()                                 mantle (deferred)
    DAT_00540e28 = 0                               bounce flag clear
dispatcher tail:
  FUN_00464d10 / FUN_00406f14 / FUN_0047d20a       (next-frame merge)
```

Two `FUN_004630d4` calls per frame — horizontal (XY, radius 0.75)
then vertical (Z only, radius 0.5, with the `0x540e50` normal
out-param). `DAT_00540e24 != 0` (slide mode) makes `FUN_00466740`
early-out straight to `FUN_00467180`: no jump machine, no mantle
call, no `e28` clear.

## 25. Vertical-state block

| Addr | Native field | Writers | Meaning (evidence) |
|---|---|---|---|
| `0x540c78` | `vertVel` | jump impulse, release cut, slope assist, gravity, collision paths | vertical velocity, +Z up (OBSERVED) |
| `0x540c7c` | `vertSkip` | `FUN_00466aec` (=2 on mantle), `FUN_00461954` | skip gate: `FUN_00467180` RETs while nonzero (OBSERVED) |
| `0x540c80` | `jumpSustain` | `FUN_00466740` rewrite, `FUN_00467180` in-volume force | sustain flag: feeds 5A `moveBoostGate` + the sustain-gravity branch (OBSERVED) |
| `0x540c84` | `PlayerMotionState::airCharge` | `FUN_00466740` seed/accumulate/reset, `FUN_00467180` volume drain, hard-land/silent landing clear, 5B forward-move drain | airborne charge/airtime scalar (OBSERVED readers/writers; the single shared global lives in 5B's state) |
| `0x540c88` | `jumpActive` | `FUN_00466740` only | jump-in-progress flag; cleared once `cac` leaves 0x2be/0x2bf (OBSERVED) |
| `0x540c8c` | `jumpHoldCharge` | `FUN_00466740` only | hold charge: init 6, drains by `frameStep`/held frame, remainder scales the release cut (OBSERVED) |
| `0x540c90` | `jumpLatch` | `FUN_00466740`, `FUN_00467ed0`, `FUN_00461954` | jump edge latch: set on start + airborne release; re-arms while grounded+stopped+jump-low (OBSERVED) |
| `0x540c98` | `jumpAux` | `FUN_00466740` only | aux flag, no observed readers (OBSERVED write-only) |
| `0x540c54` | `contactFlags` | collision seam + landing paths | bit0 grounded, bit1 floor-probe valid (OBSERVED) |
| `0x540c58` | `floorZ` | `FUN_00435eec` inside the collision call | probed floor height (seam result) |
| `0x540c60/64` | `blocker0/1` | `FUN_00435eec` | probe blocker objects (opaque tokens) |
| `0x540dc0/dc4/dc8` | `moveBlocker0/1/Flag` | `FUN_00467180` pre-land-no-contact path, `FUN_00461878` | movement blockers consumed by the horizontal layer |
| `0x540e28` | `bounceFlag` | collision internals/siblings; cleared in `FUN_00466740` tail | bounce/trampoline state (OBSERVED readers) |
| `0x540e4c` | `contactObj` | `FUN_004630d4` EAX | last contact token; feeds 5B `groundContact` |
| `0x540e50..58` | `contactNormal[3]` | `FUN_004630d4` out-param | contact normal (4th dword also copied — only xyz consumed) |
| `0x540cbc` | `eventIdle` | `FUN_00466740`, `FUN_00467180`, landing | event-channel counter; jump gate `<7`, reset on 7 when sustain/land events fire (OBSERVED) |
| `0x540c6c` | `env.vertEnable` | environment | vertical master enable (0 → `FUN_00467180` RETs) |
| `0x540cac` | `env.locoState` | dispatcher | dispatched player-state code (0x2be/0x2bf jump, 0x2bd sustain, 800 slide) |
| `0x54cb00/08` | `env.eventWordType` / `frame.eventType/eventMag` | event writers | event word type/detail (jump 7/0x2be-0x2bf, sustain 7/0x2bd, fall 7/700, hard land 8/806) |
| `0x540cc4` | `env.moveConsumed` | `FUN_00465228` | move-consumed flag → selects 0x2be vs 0x2bf |
| `0x541554` | `fallCounter` | `FUN_00467a00` readers; failsafe zeroes | fall-out counter |
| `0x540d5c` | `landingAccum` | `FUN_00467180` (hard land), `FUN_00467a00` | deferred fall-damage accumulator |
| `0x540e6c`, `0x540e72`, `0x540ca4`, `0x540d3c` | env flags | shared world state | rise-cap/land-event suppression, carrier volume gates (OBSERVED readers) |
| `0x540e24` | `env.slideMode` | slide system | slide mode gate |
| `0x4ce768` | `env.jumpHeld` | `FUN_00406f14` | merged jump flag (previous frame) |
| `0x49b6e8/f0/f4` | `env.frameStep/smoothed/deltaSeconds` | timing system | frameStep 1..4, f0≈1.0, f4=1/30 constant (zero writers — true constant) |

## 26. `FUN_00466740` — the jump-state machine

Per-frame order inside the function (all OBSERVED at instruction
level):

1. `e24 != 0` → RET via `FUN_00467180` (slide mode).
2. `c88` maintain: cleared when `cac` leaves 0x2be/0x2bf.
3. `c84` update (reads the PRE-gravity velocity):
   - `c84 == 0` (bit test, ±0 counts as zero): seed `c84 += 1.0` when
     `c78 < -16.0` (f64 compare).
   - `c84 != 0`: `cac != 0x2bd && c78 > 0` → reset to 0 (rising
     outside the sustain state); else `c78 != 0` → `c84 += f0`;
     `c78 == 0 && grounded` → 0; `c78 == 0 && !grounded` → `+= f0`.
4. `c88 == 0` → jump-init gate: `cbc < 7 && cb00 < 7 && c78 == 0 &&
   c54&1` then the `c90` latch (`c90 != 0`: cleared only when the
   jump flag is low → re-arm; `c90 == 0 && flag set` → jump).
   Initiation writes `cb08 = 0x2be` (or `0x2bf` when `cc4 != 0`),
   `cb00 = 7`, `c88 = 1`, `c8c = 6`, `c78 = 40.0f` (literal
   `0x42200000`), `c98 = 1`, `c90 = 1`.
5. `c88 != 0` → held: `c8c -= frameStep` when `c8c > 0` (floored 0,
   untouched at ≤0). Released: `c8c > 0` → `c78 -= c8c * 20 * (1/6)`
   then `c8c = 0`; `c98 = 0` either way.
6. `c80` rewrite every frame: `c80 = 0`, then `c84 != 0` →
   `!held` → event `cb08=700/cb00=7` + `c98=0` + `c90=1`;
   `held && e28 != 0` → same event, no latch writes;
   else `cbc==7 → cbc=0`, event `0x2bd/7`, `c80 = 1`.
7. Slope assist (`e4c != 0 && c88 == 0 && in_EAX != 0 && c84 == 0`):
   if the contact normal (flipped when z<0) has `z > 0.25` and
   `dot(vec, normal.xy) > 0` → `c78 = min(c78, -dot/f4)`. The
   `in_EAX` vector's provenance in the normal path is UNKNOWN
   (`FUN_0046603c` is `void`; the dispatcher's `cac>=800` path
   passes 0 explicitly) — modeled as a nullable env pointer.
8. `FUN_00467180()` → `FUN_00466aec()` (deferred) → `e28 = 0`.

## 27. `FUN_00467180` — vertical gravity + collision handling

1. Gates: `c6c == 0` or `c7c != 0` → RET.
2. `c78 > 0 && e24 == 0` → rise loop: `frameStep` gravity substeps,
   `c80` re-read per substep. Otherwise → exactly ONE `f4` step —
   OBSERVED asymmetry: falls and slide-mode rises always integrate
   a single step regardless of `frameStep`.
3. Gravity (double-precision intermediates; terminal compares run
   on the pre-truncation double via `FCOMP double`):
   - normal: `c78 += -2.1333333` (precomputed f64 `64*f4`) per
     substep, or `c78 -= f4*64` single-step; terminal `-250.0`
     (clamped to f32 `-250.0f`).
   - sustain: `c78 += -0.7111111` (`64/3*f4`) or `c78 -= f4*(64/3)`;
     when below `-8.0`, rebound `+8.5333333` (`256*f4`); a rebound
     landing above `-8` pins `c78 = -8.0f`. The `-8` is a bounce
     floor, not a straight clamp — OBSERVED.
4. Ribbon-volume check: `FUN_00412e94(player, &pos, 1, &vec)` then
   the same query on the carrier (`ca4 != 0 && d3c == 0`). A hit:
   the out-vector z is copied back into `c78` (the volume can
   rewrite velocity — modeled as `ribbonVelZ`), `cbc==7 → 0`,
   event `0x2bd/7`, `c80 = 1`, `c84 -= f0*1.75` floored by a SIGNED
   BIT-PATTERN compare (`bits < 0x3f800000` → `1.0f` — every float
   below 1.0 including negatives), and `disp` is REDONE single-step
   (`c78*f4`, discarding the loop accumulation).
5. Rise cap (non-volume path only): `cac < 800 && e6c == 0 &&
   c78 > 40` → `c78 = 40.0f` and `disp = f4*40.0f`.
6. Pre-land clamp: `c78 <= 0 && c54&2 && posZ+disp <= c58` →
   `disp = c58 + 0.05 - posZ`, pre-land flag set.
7. `c54 &= ~1` → `FUN_004630d4(ctx, mode, 0, 0, dispZ, 0.5f, 0,
   &e50)` → `e4c = EAX`. The seam is semantic: position applied,
   normal, floor probe (`c54&2`, `c58`, `c60/64`), blocker `+0x14a`
   bit-7, and a bounce indication are the only consumed facts.
8. Post-collision:
   - `e4c == 0 && pre-land` → `c60/64` copied to `dc0/dc4`;
     blocker `+0x14a&0x80` → `dc8 = 1`, else `dc8 != 0` →
     `FUN_00461878(0)` (deferred release) + `dc8 = 0`; then the
     landing path runs.
   - `e4c == 0 && !pre-land` → if applied Z dropped,
     `c78 = (posZ - oldZ)/f4` (realized velocity).
   - `e24 != 0` (slide) → realized-velocity recompute on either
     route.
   - landing (impact `c78` at contact): `impact > 0` → CEILING:
     `c78 = 0`, `e4c = 0`, grounded cleared — no landing state
     (OBSERVED asymmetry). `impact < -100 && e28 == 0` → hard
     landing: `e6c == 0` or `e72&2 == 0` → event `806/8`, `cbc = 0`,
     deferred `FUN_00467a00(10)`, `d5c = 0`; `e6c && e72&2` →
     silent (no event, no anti-jitter). Hard/silent paths clear
     `c84`; the soft/bounce path does the anti-jitter check
     (`dist² < (f4*0.35)²` → restore pre-call position) and does
     **NOT** clear `c84` — it resets next frame via the grounded
     branch (OBSERVED quirk). Common tail: `c78 = 0`, `c54 |= 1`;
     `e4c == 0` → `posZ = c58` floor snap.
   - deep-floor failsafe (all exits): `posZ <= c48+0x44e - 50` →
     `0x541554 = 0`, `c78 = 0`, `c54 |= 1`.

## 28. Coordinate sign and displacement (OBSERVED)

+Z is up (jump impulse `+40`, gravity subtracts, floor clamp adds
`+0.05`). `disp = c78 * f4` (or the substep accumulation on a
multi-step rise); the displacement uses the POST-gravity velocity
of the last step.

## 29. Native boundary (`src/core/player_vertical.*`)

- `integratePlayerVertical(env, ms, vs)` — `FUN_00466740` plus the
  pre-collision half of `FUN_00467180`; returns the collision
  request (`dispZ`, `preLand`, events, jump state).
- `applyPlayerVerticalCollision(env, ms, vs, res, frame)` — the
  post-collision half: seam apply, blocker refresh/release,
  slide realized-velocity, landing/ceiling, anti-jitter, deep-floor
  failsafe.
- `playerVerticalPostStep(env, vs)` — the `FUN_00466aec` boundary
  and the `e28` clear (skipped in slide mode).
- `VerticalCollisionResult` — the semantic `FUN_004630d4` seam:
  contact token, applied position, normal, floor probe, blockers,
  `+0x14a` bit-7, bounce. Collision internals are NOT
  reconstructed.
- `PlayerMotionState::airCharge` IS `0x540c84` (single-sourced);
  `PlayerVerticalState` carries the remaining vertical globals.

## 30. Phase 5C diagnostics and tests

- `--selftest-player-vertical`: deterministic 56-frame LALT
  (KeyJump) hold/release script through the real SDL seam →
  previous-frame control block → 5B horizontal → 5C vertical →
  synthetic flat-floor collision. Verifies latency, impulse 40,
  charge drain, apex, the `-16` seed, sustain `0x2bd`, the `-8`
  bound, sustain-end `700`, soft landing, and the `c84`
  post-landing quirk. RC 3 on mismatch; RC 2 for invalid combos
  (mutual exclusion with the other selftests and
  `--interactive-frontend` preserved).
- Native tests: 3090 checks — idle/jump gates/hold/release-cut/
  sustain/normal gravity/rise-loop vs single-step asymmetry/rise
  cap/terminals/pre-land/no-contact realized velocity/landing
  variants (soft/hard/bounce/silent)/ceiling/blockers/deep-floor/
  ribbon drain+floor/slope assist/gates/the full golden arc/c80
  coupling into 5A→5B.

## 31. Deferred boundaries and remaining vertical unknowns

- `FUN_004630d4` collision internals (triangle tests, arena
  traversal, `FUN_00435eec` floor probe, `FUN_00461878` release).
- `FUN_00466aec` mantle — its full gate is retained
  (`moveVel>0`, `c78<=-0.25`, `c7c==0`, `cac!=800`, `cbc<9`) and
  `c7c=2` models its skip.
- `FUN_0046603c` slide — `e24`/`in_EAX` provenance UNKNOWN (the
  slope-assist vector is modeled nullable).
- `FUN_00412e94` volume system (ribbon/updraft internals, the
  out-vector semantics).
- `FUN_00467a00` fall damage, `FUN_00461878` blocker release,
  `FUN_00402388`/`FUN_0040210c` event side calls — deferred
  side-effect seams.
- The `0x540cac` player-state enumeration is still only partially
  mapped (0x2bc fall / 0x2bd sustain / 0x2be-0x2bf jump / 0x326
  hard-land / 800 slide / <800 locomotion observed).

# Phase 5D — collision query and floor probe

Phase 5D reconstructs the bounded collision layer around
`FUN_004630d4` (the swept player/object collision query/apply) and
`FUN_00435eec` (the per-frame floor/contact probe), including the
proven lower level: BSP traversal, polygon records, the
box-vs-triangle test, and the iterative slide.

## 32. `FUN_004630d4` recovered signature (OBSERVED)

Instruction-level recovery: six stack arguments, no register
inputs (ECX/EDX are clobbered at the prologue — the earlier
conceptual `context`/`mode` arguments were dead registers):

```text
u32 /*EAX = poly-record token*/ FUN_004630d4(
    float dx, float dy, float dz,   /* +0x08,+0x0c,+0x10 */
    float scale,                    /* +0x14 -> FUN_00407fc0 */
    float *extVec,                  /* +0x18 NULL -> defaults */
    void **outAux)                  /* +0x1c -> BSP node ptr */
```

Position lives in globals `DAT_00540bfc/c00/c04` and is ALWAYS
committed as `pos += resolved delta` — never restored on failure.

Call-site contracts:

| call | args | defaults |
|------|------|----------|
| horizontal | `(dx, dy, 0, 0.75, NULL, NULL)` | `{0.6, 0.6, 2.5}` |
| vertical | `(0, 0, dz, 0.5, NULL, &e50)` | `{0.4, 0.4, 2.5}` |

`scale` is NOT a radius: it is the slide-continuation budget —
the sweep keeps sliding while `(totalDelta · n)² > scale ·
|totalDelta|²` (grazing-incidence threshold). `outAux` receives
the hit BSP **node pointer** (default `&DAT_0049b3d0`); the
contact normal the vertical path reads is `node->plane[0..2]`.

Return value = hit **polygon-record pointer** (stored in
`DAT_00540e4c`); 0 = no contact.

## 33. Outer orchestration (OBSERVED)

```text
pos (globals) -> lifted start (posZ + ext.z + margin;
                 margin 0.5 horizontal / 0.01 vertical)
  -> FUN_00407fc0 iterative sweep vs arena c48 {nodes,polys,verts}
     (flag=4: up to 4 slide steps + 1 contact pass; resolved
      target initialized to the requested target -> full motion
      when no geometry)
  -> carrier retry once vs arena ca4 (flag=0) when the primary
     reports no contact AND portal gates allow
     (ca8 && !d3c && !e6c); replays the FULL requested segment
     in the partner arena
  -> swept-AABB object pass over arena +0x68 list:
     FUN_0045ce58 (6-float AABB overlap) + FUN_0045c838
     (2.5D segment/AABB resolver — X/Y face clamp only, Z
      early-outs; element AABBs expanded by the player extents)
  -> final static re-sweep (flag=0) when an object resolved
  -> pos += applied delta
```

The swept player AABB (`0x540c30..0x540c44`) is rebuilt per
query: `min += negative delta`, `max += positive delta` per axis
around the pre-move lifted box.

## 34. `FUN_00435eec` floor probe (OBSERVED)

NOT inside the collision call — it runs once per frame at the
tail of `FUN_00436100`'s traversal update (after the movement
dispatcher, object updates, enemy AI). The `c54&2`/`c58` state
the vertical layer reads is the PREVIOUS frame's probe.

- Probes ONLY the arena `+0x68` object list — never the static
  BSP (static-floor landings arrive through the sweep contact
  token `e4c` instead).
- Segment: `(x, y, z+3) -> (x, y, z-3)` top-to-bottom, via
  `FUN_004138d8` (node-local transform: 3x3 @+0xac, origin
  @+0xb8/c8/d8, scale @+0x58) and `FUN_00413730` (local
  segment-vs-triangle). Hit written into the bottom endpoint.
- On hit: `c58 = hitZ`, `c5c = hitZ - objectBaseZ`,
  `c64 = 1<<elemIdx`, `c60 = object`, `c54 |= 2`. Bit1 is
  cleared before each scan.
- Riding branch (`dc0 && dc8`): `c58 = dc0->baseZ + c5c` — the
  prior relative offset rides the moving object. `dc0/dc4/dc8`
  are written by the vertical LANDING path (`0x46739b`,
  gated on blocker `+0x14a & 0x80`), not by the probe.
- Stale mount (`!dc0 || !dc8`): calls `FUN_00461878` (debug-fly/
  reset — `cac=100`, zeroes velocity; gated on `0x540c9c`),
  clears `dc8`, rescans.

## 35. Runtime geometry provenance (OBSERVED)

`FUN_00419ee0` lazily parses the level-stream collision blob
(fetched via `FUN_0041ab44` stream cursor `DAT_0054b744`, inside
`FUN_004321dc`/`FUN_00432404` arena attach) into the arena
record:

```text
blob:  [u32 countA][A x 10B][pad 2B if countA odd]
       [u32 countB][B x 44B = BSP nodes][u32 countC]
       [C x 36B = poly records][u32 countD][D x 12B = f32 verts]
       [u32 tail]
arena: +0x0c=vertCount +0x10=polyCount (via EBX) +0x14=nodeCount
       +0x18=countA +0x1c=A-base
       +0x24=vertBase +0x28=polyBase +0x2c=nodeBase +0x30=end
node +0x1c/+0x20: relocated as (end + offset) pointers
```

BUILD_A confirmation: self-consistent blobs exist inside
`TRAVERSE/LEVEL3/LEVEL3O.MTO` (the `.MTO` overlay stream) —
e.g. file offset `0xdaa64`: 248 nodes / 399 polys / 234 verts,
all planes ~unit, all children in range, all poly indices
saturating the vert table. `.DTI` supplies the arena TABLE +
connect records; the collision geometry stream is `.MTO`.
`.CMI` is not involved.

## 36. Deep-layer primitives (OBSERVED)

- `FUN_00408260`: recursive BSP sweep over 0x2c-byte nodes —
  `+0x00..0x0f` plane `{nx,ny,nz,d}`, `+0x10/+0x12` s16 child
  pair, `+0x14/+0x18` poly-set dwords `{lo16 count, hi16
  firstIdx}` selected by approach side (count 0 = unused,
  firstIdx 0xffff is filler). Box projected as `|n . ext|`
  margin; computes hit `t`, slide target via plane projection.
- `FUN_00408820`: per-leaf polygon-set scan; poly records are
  0x24 bytes `{u16 v0,v1,v2 @+0; ...; u16 flags @+0x20 (bit5
  skip, bit2 low-friction); u8 surface+1 @+0x23}`.
- `FUN_004089c0`: box-vs-triangle SAT at the contact point —
  dominant-axis projection, axis table `{1,2, 0,2, 0,1}`,
  `|n_i| >= 0.1` gate; `FUN_00425600` is the point-in-triangle
  parity test (EAX = candidate hit, EBX = v0 — vertex-relative
  coordinates).
- `FUN_004088cc` / `FUN_0040894c`: post-slide plane pushout —
  full 3D vs XY-only split on `|n.z| < 0.75` (wall vs floor),
  `+0.01` margin.
- `FUN_004089c0` contact -> `0x4635e0` callback ->
  `FUN_0040b5d0` surface-effect dispatcher: poly `+0x23` gate,
  `+0x20>>24`-adjacent surface type -> player `+0x6c[type]` flag
  table + `+0x8c[type]` handler -> `FUN_004546ac`. Bounce
  (`0x540e28`) and conveyor effects originate THERE, not in
  this layer — kept as an optional contact hook.
- `FUN_0045c838` return split: first-hit/inside takes the
  slide-past alternate output; later hits take the clamp point.
- `FUN_004138d8` element records: `+0x10` tri count, `+0x14`
  vert base, `+0x18` tri records (0x24 stride, u16 x3 indices);
  element AABB @+0x44.

## 37. Position application and grounded ownership

- Sweep applies the resolved slide endpoint; `pos += applied`.
- `c54` bit0 (grounded) is written by the vertical LANDING path
  (`0x46763b` `c54 |= 1`), NOT by collision. `e4c == 0` on the
  pre-land path snaps `pos.z = c58`.
- `c54` bit1 (floor-probe valid) is written only by
  `FUN_00435eec`.
- `0x540e28` bounce = `FUN_00466aec` return + external write —
  external input to landing, kept as an external flag.

## 38. Native implementation

- `src/core/collision_query.{h,cpp}` — faithful transcription:
  `collisionApply` (FUN_004630d4), `collisionSweep`
  (FUN_00407fc0 + FUN_00408260 + leaf/SAT/pushout),
  `collisionFloorProbe` (FUN_00435eec), `collisionBlobParse`
  (FUN_00419ee0). Record layouts mirror the proven runtime
  structs (CollisionNode 0x2c, CollisionPoly 0x24).
- `CollisionState` carries the original's globals (pos, c54,
  c58/c5c, c60/c64, c68/c70, ca4/ca8/d3c, dc0/dc4/dc8, e6c,
  e68, c30..c44 box). The `0x4635e0` surface dispatch and
  `FUN_00461878` reset are optional hooks.
- `--selftest-player-collision`: the 56-frame LALT script
  through the REAL seam on a synthetic flat-floor arena —
  horizontal query (0.75) then vertical query (0.5, outAux)
  each frame, floor probe at frame end. Verifies the whole 5C
  observable arc AND the `9.99` landing quirk (lifted box stops
  0.01 below the plane — not the synthetic stub's 10.05).
- `mdk-inspect --collision-probe <file> [x y z]`: BUILD_A
  smoke — locates the first self-consistent blob, runs one real
  sweep + floor probe. On `LEVEL3O.MTO`: blob `0xdaa64`, probe
  at vert-centre contacts poly#328 via node#212 `n=(0,0,1)`,
  resolving z to `103.99` — the same `-0.01` margin quirk on
  real data.

## 39. Phase 5D diagnostics and tests

- Native tests: 3166 checks, 0 failures (3090 baseline + 76
  Phase 5D): empty/full-motion, flat floor (`9.99` quirk),
  free-fall miss, ceiling (`14.99`, `n=(0,0,-1)`), wall face
  stop + re-push zero-motion, tangent/grazing, contact token +
  node normal, player-AABB rebuild, object element sweep,
  AABB-resolver side flags, carrier retry, floor probe hit /
  bit1 clear / ride carry / stale-mount dismount, horizontal→
  vertical ordering, standing-player sequence, wall sequence
  through the real 5A→5B seam, blob parse accept/reject.
- Selftests: gameplay-input, player-motion, player-vertical,
  player-collision all PASS; mutual exclusion + frontend
  combos RC 2.
- Phase 4 regressions: root-only digest `cf09ecdad5b0808f`,
  six-screen flow PASS (200 frames).

## 40. Remaining collision unknowns / PARTIAL boundary

- `FUN_0040b5d0` surface-effect internals (bounce/conveyor
  handlers behind `+0x8c[type]`) — the dispatcher is reached
  but its per-type effects are a hook, not reconstructed.
- `FUN_00461878` full reset semantics (debug-fly path) — the
  mount-release call site is preserved via hook.
- Arena `+0x68` object-list construction and the `+0xac..0xd8`
  transform writers (which loader fills moving-object models).
- `countA`/10-byte leading blob records — parsed for layout
  but their consumer is UNKNOWN (skipped by the sweep).
- Portal `ca4` partner-arena selection details
  (`FUN_00435178` connect-record side codes) — the retry
  contract is proven; partner assignment is runtime state.
- `FUN_004089c0` is transcribed as proven; degenerate-triangle
  edge cases beyond the observed SAT path are untested.

# Phase 5E — dynamic collision objects and runtime arena attachment

Phase 5E reconstructs the runtime link behind arena `+0x68`: how
dynamic collision objects are created from original data, attached
to arenas, transformed, updated, and consumed by the `collisionApply`
object pass and `collisionFloorProbe` — the system that makes moving
floors, doors, platforms and blocker tokens work.

## 41. Arena `+0x68` object list (OBSERVED)

Singly-linked list through object `+0x00` (next); head at
`arena+0x68`. The object pool is 399 static records x 0x32e bytes at
`DAT_004f0740`, freelist-linked via `DAT_00540ed0`.

- `FUN_0045cffc` — spawn: pop the freelist head, push-front onto
  `arena+0x68`, set `+0x06=1` (named), `+0x60`=arena.
- `FUN_0045cf90` — despawn: unlink from the arena list, push onto
  the freelist.
- `FUN_0045cf18` — arena cleanup: recycles every `+0x06==0`
  (unnamed) object.
- `FUN_004574d0` — portal transfer: unlink from the object's
  current arena `+0x60` list, push-front onto the pending arena
  `+0x2bc` list, update `+0x60`. Triggered inside the per-frame
  update (`FUN_004572ac`) when `+0x2bc != 0`.
- `FUN_00432980` — portal side: for each DTI type-6 connect record,
  walks the partner arena's `+0x68` list for `+0x14a&0x10` objects
  and calls `FUN_004574d0` to pull them through.
- `FUN_00459618` — object-vs-object resolution: objects with
  `+0x14a&8` adopt the collided object's arena as `+0x2bc`
  (projectile arena-follow).

## 42. Dynamic object spawn — `FUN_00456808` (OBSERVED)

Iterates the arena's DTI sub-record table (`arena+0x38` count,
`+0x3c` records, 0x24-stride). Dispatch on `rec.type`:

- **type 2 "HotGen"** — `fields[0]` = `enemyIdx<<16 | spawnId` (the
  index half is OR-ed in at load by `FUN_00433d40` matching the
  record's name against the CMI enemy table). Dedup on
  (`enemyIdx`, `spawnId`, exact `pos`) within the arena list.
  Alloc (`FUN_0045cffc`) → model deep-copy (`FUN_00403720` over the
  enemy-table record) → `+0x04=enemyIdx`, `+0x146=spawnId`,
  `+0x10..0x18`=pos, `+0x180..0x188`=pos (prevPos), `+0x60`=arena →
  init (`FUN_004566f0`) → `+0x11c=7`. Script key
  `"%s$%s_%d"` = `arena$model_spawnId`.
- **type 4 "HotPick"** — `fields[0]` = model index (overwrites the
  field at load). Dedup on (`modelIdx`, `pos`). Same alloc/copy/pos/
  init; then `+0x08=1`, `+0x148 dword |= 0x2008a0` (bytes:
  `+0x148=0xa0`, `+0x149=0x08`, `+0x14a=0x20` — the mover bit that
  routes `FUN_004585c4` inside the update pass; the `0x08a0` low
  word also makes the sweep's `&0x810` test skip the object — movers
  don't push the player horizontally, they only carry floors).
  Script key `"%s$%s"` = `arena$model`. If the model name is
  `"SW_DUMMY"`, elements named `"SW_DUMMY"` get their bit set in
  `+0x2c8` (element-disable mask).
- **type 1/3** — trigger bounds (`FUN_00434b44`); **type 6** —
  portal partner arena index.

`FUN_004566f0` init (collision subset): `+0x08`=10, `+0x38`=50,
`+0x3c`=10, `+0x40`=15, `+0x44`=64, `+0x48`=32, `+0x58`=1.0f scale,
`+0xe0`=30 anim rate, identity matrix; then the table-2 `"%s$%s"`
init script runs on the object (Phase 5I — `traversalObjectInitScript`,
formerly "script VM out"), then `FUN_0045612c` transform/AABB rebuild.

## 43. Geometry record format — `FUN_00428400` (OBSERVED)

ONE layout shared by both source paths. The record base carries a
flag u32 the callers pass separately
(`FUN_00428400(data=base+4, flag=*(base), out)`) — verified at both
call sites (`FUN_004286c8` CMI, `FUN_00403498` MTO):

```text
record base: u32 flag                    (register arg, not parsed)
stream +4:   u32 nameCount               (unconditional)
             nameCount x {char[12] name, u32 tag}   (16B records)
             if flag != 0:
               u32 elemCount
               elemCount x element:
                 char[12] name     -> runtime elem +0x00
                 byte[12] field2   -> runtime elem +0x20
                 u32 vertCount     -> runtime elem +0x0c
                 f32 verts[vc*3]   -> runtime elem +0x14;
                                      FUN_00459d54 min/max -> +0x2c
                 u32 triCount      -> runtime elem +0x10
                 byte tris[tc*0x24]-> runtime elem +0x18
                 byte[0x18] trailer — skipped ONLY on this path
             else (flag == 0):
               ONE anonymous element (elemCount forced 1; no
               name/field2 copies, no trailer):
                 u32 vertCount, verts, u32 triCount, tris
             byte[0x18] gap               (always)
             u32 refPointCount            (<= 8; else error)
             f32 refPoints[rc][3] -> record +0x24..0x84
```

Tail (always): `record+0xb` = 0xff body-element index, then each
element named `"XG1_BODY"` stores its index; each `"XG1_HEAD"` sets
`record+0xc |= 1<<index`.

Boundary-validated against BUILD_A: XGS ends exactly at the next
record (`0x4680c`); same for XGEN, SW_GATT, BULLET. SW_GATT proves
`flag==0` still carries a name table (nameCount=1 `"SW_GATT"`).

## 44. Model resolution paths (OBSERVED)

`FUN_004286c8` builds the enemy table (0x88-stride records,
`DAT_004edcc0`, cap 0x50 = `"Overflowed enemy table"`) from CMI
table[1] `{name, u32 value}`:

- `value != 0` → geometry record at `image + value` (file offset
  `4 + value`), parsed immediately.
- `value == 0` → `record+0xa = 1` unresolved; later
  `FUN_00403498` resolves from the level `.MTO`: each overlay
  block's region-A array-B records `{name[8], u32 off}` are matched
  by name; the record base is `tA + off` (verified: LEVEL6 `XT` at
  `tA+0xa8` = `0xc2eec`, flag=1, nameCount=5, 19 elements).

`FUN_00403720` deep-copies the model per spawned object: new 0xb0
record, fresh element array, fresh per-element vertex storage —
each spawn owns an independent copy (later damage/deform affects
one instance only). `FUN_00403538` builds a parametric box model
for enemy index `0xffff` (1 element, 8 verts, 12 tris) — runtime
collision objects need not come from file data.

## 45. Transform + world-AABB rebuild — `FUN_0045612c` (OBSERVED)

Called from init/spawn, from the mover path inside the update loop
(`FUN_004585c4`/`FUN_0045897c`), and from the render traversal
(`FUN_00431300`). Collision-relevant core:

1. **AABB seed quirk**: `obj+0x198` is seeded degenerately — min =
   {old minZ x3}, max = {old maxZ x3} (the previous z bounds
   broadcast into every component). On a zeroed object the first
   build seeds {0,0,0}/{0,0,0}, so positive extents clamp at 0 on
   the first frame — OBSERVED, reproduced.
2. **Matrix path select**: `+0x148 & 0x40` → `xform[3x3] =
   +0x302..0x322 x +0x58`, origin `{x, y, z + zBias(+0x5c)}`;
   else Euler via `FUN_0046b2f8(+0x54 pitch, +0x13c bank,
   +0x4c yaw, +0x58 scale, +0x10/14/18 pos)` — translation is the
   raw position (NO zBias on this path).
3. **Element world AABBs**: for each element not masked by
   `+0x2c8`, `FUN_00459e40` transforms the 8 local-AABB corners by
   the matrix and `FUN_00459d54` writes min/max into `elem+0x44`;
   each is unioned into `obj+0x198`. Masked elements keep stale
   world bounds (original behavior).
4. Render-only tail (skipped natively): parent-matrix compose into
   `+0x7c`, screen-space bounds `+0x64..0x78`, `FUN_0046afe4`
   reference-point transform.

`+0x148 & 1` / `& 0x80` gate a steering/easing pre-pass (yaw-rate
smoothing into `+0x54`, velocity-heading bank into `+0x13c` via
`FUN_004301bc`) that only derives the angle fields — behavior
state, not collision state; objects with `+0x148` bit7 set (e.g.
type-4 `0x2008a0`) skip it entirely. Documented seam, not
implemented (the mover objects Phase 5E targets never take it).

`FUN_0046b2f8` verified at instruction level: angles are DEGREES
(`FUN_00437f98` = `angle x pi/180 -> {sin,cos}`, constant
`0x497924`), row-major 3x3 with the uniform scale baked in:

```text
row0: c2c3   -s1s2c3-c1s3   -c1s2c3+s1s3
row1: c2s3   -s1s2s3+c1c3   -c1s2s3-s1c3
row2: s2      s1c2           c1c2
translation: {x, y, z}   ((s1,c1)=+0x54, (s2,c2)=+0x13c, (s3,c3)=+0x4c)
```

— exactly the `world = M.local + origin` / `local = M^T.d / scale^2`
contract the Phase 5D query consumes.

## 46. Per-frame update + ride displacement — `FUN_004572ac` (OBSERVED)

Per named object inside the per-arena update (before the floor
probe — same-frame following):

1. Flag dispatches (`0x10` portal-follow, `0x40` update,
   `0x14b&0x40`).
2. `+0x2bc` pending arena → `FUN_004574d0` transfer.
3. Script VM → pose interpolation (`FUN_00456d28`) → physics
   (`FUN_004533d4`/`FUN_0045b9fc`/`FUN_0045bac0`) → mover
   (`FUN_0045897c` or `FUN_004585c4` per `+0x149&0x10` /
   `+0x14a&0x20`) → `FUN_004555bc`.
4. Velocity cache `+0x18c..0x194` = `(pos - prevPos) / dt`.
5. **Ride displacement**: if `obj == DAT_00540dc0` (the player's
   carrier): `pos += pos - prevPos` component-wise on the player
   position, and player yaw `+= +0x4c - +0x50` (yaw delta).
6. `prevPos = pos`, `prevYaw = yaw` (end of update).

`FUN_004585c4` (the `+0x14a&0x20` mover path — SW_H150/SW_SEAL/
SW_SBONE switch-objects) writes `+0x18` position and `+0x5c` z-bias
directly and calls `FUN_0045612c` inside the update — movers get
same-frame collision refresh; ordinary objects refresh in the
render traversal (`FUN_00431300`) → their collision AABB lags one
frame.

`FUN_00467180` (vertical landing) establishes the ride:
`dc0 = contactObj`, `dc4 = elemMask`, `dc8 = 1` only when
`contactObj+0x14a & 0x80` — the mountable gate. `+0x14a&0x80` has
NO direct writer: it is script-assigned — tr_alcmd opcode `0x29`
(handler `0x443de0`, dispatch entry `0x438afc`): arg 2 →
`+0x14a|=0x80` mountable; arg 1 → `+0x149|=1` solid non-mountable;
arg 0 → non-solid; leaving solid while ridden fires
`FUN_00461878` dismount. A second opcode (`0x4f647` site) sets
`+0x14a|0x40` (update flag).

Frame order (`FUN_00436100`): player dispatch (`FUN_00463608`
including `FUN_004630d4`) → targeting (`FUN_00432f84`) → object
update (`FUN_004572ac` + `FUN_0045cf18` per arena) → scripts
(`FUN_004388d8`) → portal test (`FUN_00435178`) → **floor probe**
(`FUN_00435eec`) → render (`FUN_00431300` transform refresh).

## 47. Native implementation (`src/core/dynamic_objects.*`)

- `RuntimeModel` — owned model record: name table, per-element
  name/field2/vert/tri storage, `CollisionElement` views (world
  `aabb` = +0x44, `localAabb` = +0x2c — field added to the Phase 5D
  struct for the rebuild), ref points, body/head bookkeeping.
  `rebind()` re-points views after parse/copy.
- `parseGeometryRecord` — `FUN_00428400`, bounds-checked.
- `deepCopyModel` — `FUN_00403720` (copy + rebind = fresh storage).
- `EnemyTable` / `buildEnemyTable` / `enemyModelData` —
  `FUN_004286c8` product + the CMI-direct / MTO-deferred
  (`FUN_00403498`) geometry lookup.
- `DynamicObject` — runtime record: `CollisionObject col` (the
  +0x68 node) + owned `RuntimeModel` + update-side fields
  (`pos/prevPos/yawDeg/prevYawDeg/pitchDeg/bankDeg/zBias/
  rawMatrix/enemyIndex/spawnId/health/behaviorByte/arena/
  pendingArena`). `pos[2]` mirrors `col.baseZ` (+0x18).
- `DynamicArena` — owns `CollisionArena` + object storage
  (`std::list`, stable addresses); `allocFront`/`detach`/`transfer`
  = `FUN_0045cffc`/`FUN_0045cf90`/`FUN_004574d0`.
- `buildObjectMatrix` — `FUN_0046b2f8`.
- `rebuildObjectTransform` — `FUN_0045612c` collision core (matrix
  select + element world AABBs + degenerate-seeded union).
- `initObjectDefaults` — the `FUN_004566f0` default block (pre-script).
  `initObjectCollision` = defaults + view + rebuild for the
  non-scripted spawn path; the scripted path splits them so the
  table-2 init runs between the defaults and the `FUN_0045612c`
  rebuild (Phase 5I).
- `resolveArenaRecordNames` — the `FUN_00433d40` load-time rewrite.
- `spawnArenaObjects` — `FUN_00456808` type-2/type-4 (dedup, model
  deep-copy, flags, `SW_DUMMY` mask, push-front attach).
- `applyRideDisplacement` / `latchObjectPrevState` /
  `updateMoverCollision` — the `FUN_004572ac` tail (carrier delta →
  player pos + yaw, then latch), in the proven order.

## 48. Phase 5E diagnostics and tests

- Native tests: 3257 checks, 0 failures (3166 baseline + 91 Phase
  5E): geometry parse (named + anonymous + bounds rejection), deep
  copy independence, matrix identity/yaw90/scale, transform rebuild
  (Euler, z-seed quirk, elemMaskB skip, raw-matrix + zBias), enemy
  table + DTI name rewrite, type-2/type-4 spawn fields + dedup +
  push-front, attach/detach/transfer, floor-probe consumption,
  ride displacement + moving-floor carry, sweep element-AABB
  blocking.
- `mdk-inspect --arena-objects <path/LEVELn.DTI>`: BUILD_A smoke —
  loads the `.DTI` + sibling `.CMI` + `<stem>O.MTO`, resolves the
  enemy table, spawns every arena's type-2/4 records and reports
  the `+0x68` list. Results: LEVEL3 — the HMO_9 `XGS` (idx 30,
  spawn 9, 25 elements) spawns and a floor probe hits its real
  geometry (`floorZ -290.794`); LEVEL6 — MTO-resolved `XT`
  (idx 40, 19 elems) floor-probes at `-2917.57`; LEVEL8 — 21
  objects (mixed HotGen/HotPick incl. SW_* movers), 17 models
  resolved, 0 failures.

## 49. Remaining Phase 5E unknowns / boundary

- Steering/easing pre-pass inside `FUN_0045612c` (`+0x148&1` path):
  `+0x54`/`+0x13c` derivation from yaw-rate and velocity heading —
  behavior-layer state; not needed by type-4 movers (bit7 skips it).
- `+0x302` dual role (raw-matrix element 0 AND transition flag) —
  the layout is reproduced; the countdown semantics are a behavior
  detail.
- Script VM (`FUN_004388d8`) — opcode `0x29`'s mountable/solid
  writes are proven; executing init scripts is a later system.
- `+0x2bc` arena assignment beyond portal-follow (`FUN_0045bac0`
  portal test) — trigger-side, not collision-side.
- Surface effects (`FUN_0040b5d0`) and `FUN_00461878` dismount
  internals — unchanged hook boundaries from Phase 5D.

# Phase 5F — surface contact effects

Phase 5F reconstructs **one bounded layer**: the surface-effect block
hanging off the collision object — the per-contact dispatcher
`FUN_0040b5d0` invoked through the sweep's registered callback
(`0x4635e0`), the poly-flag operation helper `FUN_0040a704` and the
pending-flag pass `FUN_0040b4dc`, the `+0x45e` "fan" record list with
its conveyor (`FUN_00412ef0`) and volume-updraft (`FUN_00412e94` /
`FUN_00412f84`) consumers, the record update `FUN_004134a0`, and the
type-9 slide-zone trigger behind tr_alcmd opcode `0xe0`. The script
handler (`FUN_004546ac` → the tr_alcmd VM) stays a seam — the native
port does not implement the VM.

The chain is:

```
collision contact -> callback 0x4635e0 -> FUN_0040b5d0
                  -> surface/contact dispatch -> locomotion effect
```

## 50. The contact callback — `0x4635e0` (OBSERVED)

`0x4635e0` is **pushed as a function-pointer argument** to the BSP
sweep `FUN_00407fc0` — twice in the player query (`FUN_004630d4`):
once for the player's own collision set, and again for `0x540ca4`
(the ridden/attached object from 5E) when the first sweep misses.
The sweep calls it once per contact-producing iteration with
`EDX` = the hit poly. Its register signature (recovered from exact
disassembly):

```
FUN_0040b5d0(EAX = ctx (player/arena collision object),
             EDX = poly,
             EBX = 0,
             ECX = 8,
             stack{-0xb, vecA=0x4a20c0, posB=0x540bfc, c08=0x540c08})
```

`0x4a20c0` is the sweep's working/contact-position buffer;
`0x540bfc` is the live player position; `0x540c08` is the
entry/snapshot position written by the load/transition path (not per
contact). The other six `FUN_0040b5d0` call sites use masks `0x1`,
`0x2`, `0x3`, `0x10` — the player sweep callback is channel `0x8`.

## 51. The dispatcher — `FUN_0040b5d0` (OBSERVED)

Per contact:

1. Rejects polys whose byte `+0x23` is zero (the surface enable gate).
2. Derives the surface id as `(u32@+0x20 >> 24)` — i.e. the same
   `+0x23` byte — and indexes `id - 1`; rejects ids outside the
   16-entry tables.
3. Reads `ctx+0x6c[id-1]` and tests it against the contact-context
   mask (channel `0x8` for the player sweep).
4. Config bit `0x80`: sets `marks |= 1<<id` (`ctx+0x114`) and clears
   the poly's `0x10` flag via `FUN_0040a704` op 3.
5. Config bit `0x40`: produces the secondary result condition.
6. Config bit `0x20`: adds result bit `0x02`.
7. If `ctx+0x8c[id-1]` holds a handler offset and
   `ctx+0x7c[id-1]` matches the mask, invokes the script seam
   (`FUN_004546ac`); handler invocation sets result bit `0x01`.
8. Increments `ctx+0xcc[id-1]` by the secondary arg.

The poly's `+0x20` u32 packs `{surfaceId << 24 | flags}`; the flag
bits `0x10` (per-frame armed), `0x20` (the sweep's skip bit),
`0x30` (combined op), `0x04` (low friction) live in the low byte.

## 52. Flag ops — `FUN_0040a704` / `FUN_0040b4dc` (OBSERVED)

`FUN_0040a704(surfId, polyTable, op, count)` iterates the collision
object's poly table (`ctx+0x28`, count `ctx+0x10`), matching
`poly+0x20 >> 24 == surfId`, and applies the op to the low-byte flag
bits:

```
op0  |= 0x30     op2  |= 0x10     op4  |= 0x20
op1  &= ~0x30    op3  &= ~0x10    op5  &= ~0x20
```

`FUN_0040b4dc(mode)` is the pending/persistent pass:

- **mode 0** — for each bit set in `marks` (`+0x114`), re-arm the
  `0x10` flag on that surface's polys, then clear `marks`. This is the
  per-frame re-arm that pairs with the dispatcher's `0x80` clear.
- **mode 1** — set `0x30` on polys flagged `&2`, then apply the
  persistent op masks `+0x10c` / `+0x110` (op2 for bits in `+0x110`,
  else op4); clears consumed bits.

## 53. The `+0x45e` "fan" record list (OBSERVED)

`ctx+0x45e` heads an intrusive list of 0x48-byte records (a static
pool in the original — `"P_Fan_ %s not found"`). `record+0x14` selects
the kind:

- **`-1` — surface-bound.** `+0x0c` = the surface id, `+0x20..0x28` =
  a normalized 3D direction, `+0x18` = rate, `+0x40` = target rate,
  `+0x44` = ramp delta. `FUN_004134a0` ramps `rate` toward `target`
  per frame and scrolls matching-surface poly UVs (render-side — not
  modelled); `FUN_00412ef0` adds `dir * rate * frameStep` to an out
  vector for each record whose surface matches a queried poly — the
  conveyor displacement.
- **`1..6` — volume/ribbon.** `+0x20..0x37` = an AABB. `FUN_00412e94`
  scans them and `FUN_00412f84` runs a box test plus a per-shape
  falloff `t`, easing an out `vec.z` toward `rate * t` — the updraft
  volumes 5C already models as environment inputs.

`FUN_00413380` creates a surface record (normalize dir, push front);
`FUN_00412e10`/`FUN_00412d04` create volume records from **type-7**
DTI "fan hotspot" sub-records; `FUN_00413210`/`FUN_004132e0`/
`FUN_00413354` delete / set-rate / enable by name (script-facing).

## 54. The type-9 slide-zone — opcode `0xe0` (OBSERVED)

tr_alcmd opcode `0xe0` (dispatch-table slot `0xdf` at `0x438a5c`)
takes `{flag, yaw, speed}`. With `flag == 0` it clears slide mode;
otherwise it scans the arena's DTI sub-record table (`+0x38/+0x3c`)
for **type-9** records and tests the player position against each
record's closed bounds box `fields[3..8]` (`minx..maxz`). On a
containing box it writes `DAT_00540e28 = 1` (the bounce flag) and:

- if sliding or a contact normal exists → `FUN_00465de8` enters
  slide mode (`e24 = 1`, state `0x327`) + `FUN_00465e64` applies a
  yaw/speed impulse;
- else → `vertVel -= f4 * 128.0` (the constant `double 128.0` at
  `0x497ccc`) — a downward slam, since `vertVel` is `+Z`-up.

So the type-9 record is a **slide/deflect zone** (MDK's slalom
mechanic), not a generic trampoline: `e28` marks "in zone" so the
landing path suppresses the normal landing transition while the
redirect applies. In the native seam this feeds
`VerticalCollisionResult::bounce` → `vs.bounceFlag`, which suppresses
the hard-landing event (`FUN_00467180`) and clears on the post-step.

## 55. Surface-table population — script opcodes (OBSERVED)

The collision object's surface block is **script-configured**, not
loaded from static data. The arena object is memset at creation; the
tr_alcmd opcodes then write the tables through the context pointer
(`alien+0x60` → the arena object):

| opcode | args | writes |
| ------ | ---- | ------ |
| `0x62` | `{surfId, op}` | `FUN_0040a704` + `+0x10c`/`+0x110` persistent masks |
| `0x63` | `{mask, surfId, handlerOff}` | `+0x7c` handler mask + `+0x8c` CMI-relative handler offset |
| `0xa8` | `{surfId, mask}` | `+0x6c` config (+ `FUN_0040a704` set-`0x10` when `mask&0x80`) |
| (counter setter) | `{surfId, u16}` | `+0xcc` counter |
| `0xe0` | `{flag, yaw, speed}` | type-9 slide-zone scan (above) |

`+0x8c` handler offsets are **CMI-image-relative** — `FUN_004546ac`
builds a synthetic 0x32e alien object whose script pointer is
`CMI base + handlerOff` (player as the `+0x60` context) and runs the
`FUN_004388d8` VM on it. The per-arena CMI block resolves via
`FUN_00458550` → `arena+0x220`.

## 56. Native implementation (`src/core/player_surface.*`)

- `SurfaceObjectState` — the collision object's `+0x6c..+0x45e`
  block: `config`/`handlerMask`/`handlerOff`/`counters` (16 slots),
  `opMaskA`/`opMaskB`/`marks`, the mutable poly table + count, the
  record list, and a native-only `SurfaceScriptFn` hook for the
  `FUN_004546ac` seam.
- `surfacePolyOp` — `FUN_0040a704`; `surfaceDispatch` —
  `FUN_0040b5d0`; `surfaceApplyPending` — `FUN_0040b4dc`.
- `surfaceRecordCreate` / `surfaceVolumeCreate` /
  `surfaceRecordUpdate` / `surfaceRecordsDestroy` — the `+0x45e`
  record lifecycle + rate ramp (`FUN_004134a0`'s locomotion-relevant
  half; UV-scroll poly writes are render-side and skipped).
- `surfaceConveyorDelta` — `FUN_00412ef0` (`out += dir*rate*dt`);
  `surfaceVolumeQuery` — `FUN_00412e94` + `FUN_00412f84`.
- `slideZoneTrigger` — opcode `0xe0`'s mechanics (box scan, bounce
  flag, redirect / down-slam) without the slide locomotion channel.
- `surfaceContactHook` — the `0x4635e0` callback equivalent, installed
  as `CollisionState::contactHook`; the sweep stages `cs.sweepContact`
  (the `0x4a20c0` vecA) before each invoke.

`CollisionState` gained the dispatch inputs (`entryPos`,
`sweepContact`, `surfDelta`/`surfVec` fx outputs, `surface`,
`surfaceContextMask`, `surfaceResult`) — the callback's argument set.

## 57. Phase 5F diagnostics and tests

- Native tests: **3323 checks, 0 failures** (3257 baseline + 66 Phase
  5F): surface-metadata decode + gating, the dispatch mask effects
  (`0x80`/`0x40`/`0x20`), poly-flag ops, pending-flag apply mode 0/1,
  handler mask gating + invoke + result mutation, conveyor
  normalize/accumulate/nonmatch/multi-record, record lifetime + rate
  ramp, volume query/falloff, type-9 inside/outside/redirect/down-slam,
  and the real seam paths (`collisionApply` → `surfaceContactHook`,
  conveyor → `integratePlayerMotion`, bounce →
  `applyPlayerVerticalCollision`).
- `mdk-inspect --selftest-player-surface`: synthetic end-to-end
  through the real seams — contact ordinary floor → no effect;
  contact a conveyor surface → 5B displacement; contact a slide-zone →
  5C bounce flag; pending re-arm. PASS.
- `mdk-inspect --surface-census <path/LEVELn.DTI>`: BUILD_A census —
  reads the `.DTI` + sibling `<stem>O.MTO`, counts the type-7
  (fan/volume) / type-9 (slide-zone) sub-records per arena and the
  surface byte (`+0x23`) + flag bits (`+0x20`) across each arena's
  region-C collision blob. Results (all OBSERVED on real data):

  | level | surface polys | surface ids seen | type7 | type9 |
  | ----- | ------------- | ---------------- | ----- | ----- |
  | 3 | 1044 / 7205 | 1–14 | 2 | 0 |
  | 4 | 10700 / 14509 | 1–16 (+2372 out-of-domain >16) | 5 | 0 |
  | 5 | 1023 / 6044 | 1–11 | 4 | 0 |
  | 6 | 836 / 7043 | 1–10 | 6 | 8 |
  | 7 | 1903 / 10208 | 1–16 | 2 | 0 |
  | 8 | 617 / 6593 | 1–12 | 4 | 0 |

  Type-9 slide-zones appear only in LEVEL6, all inside the `COLYM_*`
  corridor arenas — consistent with the slide/deflect mechanic living
  in connector corridors. Type-7 volumes are spread across all levels.
  **The `+0x20` flag bits are all zero in static data** — confirming
  `0x10`/`0x20`/`0x30`/`0x04` are runtime state written by opcodes
  `0x62`/`0xa8` and the dispatcher, not serialized. The `+0x23` byte
  can exceed 16 (LEVEL4) — the dispatcher's slot bound correctly
  rejects those; whether they carry a non-surface meaning is UNKNOWN.

## 58. Remaining Phase 5F unknowns / boundary

- `FUN_004546ac` — the CMI-relative handler scripts. The dispatch,
  mask, counter, and fx plumbing are proven; the script bodies that
  produce game-specific effects are the VM's domain — intentionally a
  seam, not reimplemented.
- `+0x23` values >16 — out of the dispatch domain; possibly a
  different packed field. OBSERVED distribution, semantics UNKNOWN.
- Slide-mode locomotion (`FUN_00465de8` / `FUN_00465e64` internals,
  state `0x327`) — the redirect impulse args are captured, but the
  slide channel itself is part of the deferred movement modes.
- `FUN_00461878` dismount internals — unchanged from Phase 5D.

# Phase 5G — traversal runtime assembly

Phase 5G assembles the proven Phase 5A–5F systems into a bounded,
headless traversal runtime (`src/core/traversal_runtime.*`) driven by
`mdk-inspect --traversal-runtime` on real BUILD_A data
(DTI + CMI + MTO). It is not a renderer, camera, or script VM —
unresolved spawn/script/portal behavior stays an explicit seam.

## 59. Arena streaming model (OBSERVED)

- DTI s2 expands each arena into a 0x466-stride record; the name's
  leading `c`/`C` gates `+0x44 |= 3` — NON-`c` names get it, `C*`
  corridor arenas do not. `FUN_00432404` only streams arenas with
  `+0x44` bit0, so corridors never request an `.MTO` block (the
  no-match path is the original `"No overlay data for %s"` assert).
- Each `.MTO` holds exactly the 10 main-arena blocks (LEVEL3:
  `HMO_1..HMO_10`); there are no `CHMO_*` blocks — OBSERVED on all
  6 level MTOs. Corridors therefore have NO static collision blob,
  and no main-arena blob covers corridor space (verified by direct
  spawn probes into `HMO_1`/`HMO_2` at corridor coordinates — free
  fall through the failsafe).
- `FUN_00433d40` calls `FUN_00458550` (CMI table-3 name lookup) once
  per arena, storing the result at `arena+0x220`: a `{u8 nameLen,
  name, u8 dataLen, data, u32 imageOff}` record whose trailing u32
  points at the arena's bytecode record. Table[3] = `"C"` + the 10
  main arenas + a per-level SUBSET of corridors (LEVEL5: none;
  LEVEL3 lacks `CHMO_3`/`CHMO_7`).

## 60. Portal test — `FUN_00435178` (OBSERVED, instruction-level)

- Scans the CURRENT arena's type-6 records (`+0x38` count / `+0x3c`
  table). Record: `fields[0]` = partner arena index (rewritten by the
  `FUN_00434e54` connect pairing from the file-form connect ID),
  `fields[1]` = side code, `fields[2..7]` = box `{x0,y0,z0,x1,y1,z1}`.
- Tests current pos `0x540bfc` vs previous committed pos `0x540c08`
  per axis: closed segment-interval overlap on the two slab axes
  (with `z0-5.0` margin at `0x497778`), directional crossing on the
  portal axis.
- Sides: 0/1 = x-plane crossings (−x/+x), 2/3 = y-plane, 4 = z-plane
  downward at `z0-0.5` (`0x497780`), 7 = z-plane upward, 5 = diagonal
  `cross > 0`, 6 = diagonal `cross < 0` where
  `cross = (p.y-y0)(x1-x0) - (y1-y0)(p.x-x0)`; unmatched codes take
  the side-6 path.
- On pass: `slideChannel` (`0x540e24`) → `-15` only if `>0`;
  `ca4=c48`, `ca8=1`, `c48=dest`, `FUN_00432d9c(dest)` leaves
  `ca4=dest` — the old arena remains reachable only via a NEW type-1
  attach.

## 61. Trigger scan — `FUN_00434b44` (OBSERVED, instruction-level)

- Per frame over the current arena's sub-records; x/y closed
  segment-overlap box `fields[2],[3]` vs `fields[5],[6]` on
  current-vs-previous position.
- Type 1: `fields[0]` = partner arena index → `FUN_00432d9c`
  (attach; idempotent when equal — per-frame re-fire in-zone is the
  OBSERVED behavior), or `fields[0] == -1` → `FUN_00432bf8` (detach).
- Type 3: `fields[0]` = partner index → `ca4` set + `FUN_00432980(0)`
  stream request, `ca8=0` — cold prefetch, no hot attach/spawn.
- Attach side effects (`FUN_00432d9c` tail): partner geometry stream,
  type-6-peer object migration (`+0x14a&0x10` records whose `+0x60`
  home is the partner), spawn-once guarded by `+0x44` bit2.

## 62. Arena script VM — `FUN_004388d8` (OBSERVED)

- Called once for `c48` and once for `ca4` per frame whenever the
  arena's `+0x220` is non-null — the `tr_alcmd` bytecode interpreter
  driven per-ARENA, not only per-alien.
- Per-arena persistent state: `+0x108` program counter, `+0x22c`
  wait/delay counter (execution suspends while >0), `+0x230`
  alternate PC, `0xff` end-of-stream, and a 1000-instruction-per-call
  cap guarded by the original `"Alien %s looped %d commands, off %lx"`
  diagnostic.
- The VM snapshots player pos/yaw (`0x540bfc`→`0x54c6c4`,
  `0x540c2c`→`0x54c6c0`) for opcodes. Corridor scripts spawn scripted
  objects (e.g. the `XCORDOOR` connector door) — corridor traversal is
  SCRIPT/PORTAL-DRIVEN, not static-collision-driven. The arena-script
  calls run the real VM (`scriptObj` was the Phase 5G counted seam);
  the spawned connector's open/close/class behavior is the native
  `FUN_00457738`/`FUN_004555bc` path reconstructed in Phase 5I.

## 63. Deep-floor failsafe — arena `+0x44e` (OBSERVED)

- `0x4673ee` computes `c48->+0x44e + (-50.0) >= posZ` → forced
  grounded + vertVel=0 (`kDeepFloorDelta` = `0x498a98` = -50.0).
- Zero-init is proven, not inferred: the 0x466-stride arena array is
  allocated (`0x434130` call `0x41c884`) then memset to 0
  (`0x434147` call `0x47d20a`, fill byte edx=0, size `count*0x466`),
  and the per-record init loop (`0x434191..0x434229`) writes only
  `+0x00` name, `+0x34`, `+0x38`/`+0x3c`, `+0x40`, `+0x44` flag-or
  (`c`/`C` corridor gate), `+0x5c..+0x64` (embedded self-ref), and
  `+0x462` (DTI scalar). A full code-section sweep for displacements
  `0x448..0x453` finds no store to `+0x44e`; the only overlapping
  `+0x44c`/`+0x450` dword writes are in `FUN_00413c20`/`FUN_00413dd8`
  on a *different* record type (name at `+4`, 5-pointer dispatch
  block at `+0x444` copied from table `0x49a750`).
- All five `+0x44e` readers consume it as the arena's abyss
  reference: the player failsafe (`0x4673f3`, base `c48`, `-50`),
  and object checks through `obj+0x60` (the object's arena pointer):
  `0x4583ab`/`0x4583ce` respawn an object that fell below
  `+0x44e + (-200)` by writing `obj+0x18 = +0x44e + (-150)`;
  `0x45bdd6` compares `+0x44e + (-200)` vs `obj+0x18` to skip a kill
  path; `0x45fd55` compares through `rec+0x18`.
- Net effect: a flat `posZ <= -50` catch for the player and `-200`
  (respawn to `-150`) for objects, worldwide — not per-arena
  geometry. LEVEL5 (MUSE) spawns at z=-293, so the failsafe fires
  every frame there; it only re-asserts grounded/vertVel=0 atop real
  contact — OBSERVED, harmless.
- Native note: an earlier implementation derived it from the arena's
  own vertex min — corrected to the observed flat -50.

## 64. Phase 5G validation + boundary

- `mdk-inspect --traversal-runtime` on all six BUILD_A levels: every
  spawn arena (HMO_1, MEAT_1, MUSE_1, OLYM_1, DANT_1, GUNT_1) passes
  with real contact+grounded — LEVEL3 digest `f568d6aa986f4b70`
  (60 frames); MUSE_1 spawns below the -50 abyss line so the
  failsafe fires all frames alongside real contact (OBSERVED
  no-op); GUNT_1 spawns 2 objects + 2 models cleanly.
- Corridor attach fired on every level: CHMO_1, CMEAT_3, CMUSE_1,
  COLYM_1, CDANT_3, CGUNT_1 all show `partner=1 car-vld=1
  car-geom=1` (partner = adjacent main arena, blob loaded).
- Type-1 detach proven live on CHMO_1: attach strip x[26,33] →
  `p=0`; walking into the adjacent detach strip x[34,41] → `p=-1`
  the same frame the position enters the box.
- Type-3 cold prefetch proven live on CHMO_1: `p=0` + `car-vld=0` +
  `car-geom=1` — partner selected and geometry loaded with `ca8=0`.
- Type-6 portal crossings proven live both directions:
  HMO_2 --(side 6, z-plane 68, x[-21,19] y[1092,1131])--> CHMO_2
  on a downward fall; CHMO_1 --(side 2, y-plane 654, x[-19,17]
  z[99,126] with margin)--> HMO_1 on a walk+fall segment. The
  post-swap state shows `ca4 = dest` — the observed slot semantics,
  not a scene replacement.
- Check classification: a run that crosses a portal is asserted by
  gates + post-swap partner state instead of the start arena's
  contact requirement; a start in uncovered airspace (e.g. inside a
  boundary attach strip with no floor) still fails honestly.
- Boundary (explicit seams): the `FUN_004388d8` arena scripts are
  counted but not executed; corridor crossing mechanics (script-
  driven doors/forced movement/teleports) are UNKNOWN until the VM
  is mapped; type-5/type-8 records remain UNKNOWN; `+0x44e`'s
  zero-init is proven via the creation memset for MDK95 — DOS build
  parity unchecked.

# Phase 5H — the tr_alcmd arena script VM

Phase 5H reconstructs the bounded `tr_alcmd` interpreter
(`FUN_004388d8`) that the traversal frame runs once for the current
arena and once for the active partner arena. Corridor arenas carry no
static collision geometry (Phase 5G), so their doors, triggers and
forced motion are script-driven — this VM is what advances real
corridor traversal.

## 65. Interpreter context + frame entry (OBSERVED)

The VM context is the arena record `+0x118`. The frame driver calls
`FUN_004388d8(c48+0x118)` for the current arena and
`FUN_004388d8(ca4+0x118)` for the partner whenever that arena's
`+0x220` field is nonzero. The proven ctx fields (all offsets are
`arena_base + 0x118 + off`, i.e. ctx-relative shown):

| ctx off | arena off | role |
|---------|-----------|------|
| `+0x108` | `+0x220` | script gate + persisted entry/resume PC (image-relative code offset; 0 = no script / stopped). Written by checkpoint `0x01`, by call/goto targets, restored by `0xfd`, cleared by `0x09`. |
| `+0x22c` | `+0x344` | wait timer in seconds; decremented by `1/30` (`0x49b6f4`) once per frame entry |
| `+0x230` | `+0x348` | wait-resume PC (image offset) |
| `+0x60`  | `+0x178` | bound object — the arena record itself (self) |
| `+0x0c`  | `+0x12c` | arena name ptr (diagnostics) |
| `+0x21d` | — | event byte (the `FUN_004546ac` synthetic ctx) |
| `+0x21e` | — | running flag; opcode `0xff` clears it |
| `+0x244` | — | ctx-local flag dword (flag group 2) |
| `+0x248/24c/25c/26c` | — | call-stack depth (cap 4), return-PC slots, saved-`+0x108` slots, per-depth u16 markers |
| `+0x2b8` | — | caller/parent ctx (operand mode 4, flag-group else) |
| `+0x312` | — | child flag dword (flag group 5) |
| `+0x30e` | — | f32 field written by opcode `0x99` |
| `+0x11a` | — | u8 written by `0x0b`, read by `0x0a` |

Frame-entry semantics: if `+0x22c > 0` the interpreter decrements it
by `1/30` and, while still `>0`, exits (the script is asleep); on
lapse it resumes at `+0x230`. Otherwise it reads `+0x108` as the
entry PC — so a script that ends at `0xff` without a `0x09` stop or a
wait **re-enters at its checkpointed PC next frame**. This is the
spawn-once-then-poll design the corridor setup scripts use.

## 66. Bytecode encoding + linkage (OBSERVED)

All PCs and jump/call operands are **image-relative code offsets**
(`file offset = imageBase + off`, image base `= file+4`). The dispatch
is `jmp [opcode*4 + 0x438a5c]`; legal range is opcode `1..0xfd`
(253 slots), `0xff` is the loop-head end-of-stream byte, `0xfe` is
not a standalone opcode but a linkage *mode* byte. Unknown opcodes
take the default branch `0x451e03` → `"Unrecognised controlalien"`.

Typed scalar operands carry a `mode:u8` prefix resolved by
`FUN_00438654`: `0` = global f32 array `0x540d88`, `1` = bound-object
f32 `+0x48`, `2` = ctx f32 locals `+0x234`, `3` = inline `f32`,
`>=4` = caller-ctx `+0x234`. Flag operands select a dword via
`FUN_00438744`: group `2` = ctx `+0x244`, `1` = bound-object `+0x58`,
`0` = global `0x540d98`, `5` = ctx `+0x312`, else = caller ctx
`+0x244`.

Several opcodes take a **linkage tail** — a mode byte followed by
offset operand(s) — instead of branching inline. The shared tail
reader implements: `0xfc`/`0xfe` = call (push `{retPC, saved+0x108}`
onto the 4-deep stack), `0xfd` = return, `0x0c` = goto. `0xfc` and
`0x0c` as *standalone* opcodes are indexed/random-pick forms —
`{u8 n; n×u32 offs}` with `FUN_00401ed4` = `rand()*n>>15` selecting
the target (`n=1` is deterministic).

## 67. Call/return stack + diagnostics (OBSERVED)

The call stack is 4 deep (`+0x248` depth). A call pushes
`{returnPC, saved +0x108}` and clears a u16 marker at
`+0x26c + 2*depth`. `0xfd` pops: depth 0 → `FUN_00438010`
`"Gosub underflow/overflow on %s ID %d"` + kills the script
(`+0x108 = 0`); else `--depth`, `pc = retPC`, `+0x108 = savedPC`.
Overflow (call at depth 4) is the same diagnostic + kill.

The dispatch loop counts instructions; exceeding **1000** in one
invocation reports `"Alien %s looped %d commands, off %lx"` and
halts. The native port enforces the same cap.

## 68. Implemented opcode set (OBSERVED → CORROBORATED)

A CFG-aware census over all six BUILD_A levels' arena scripts (entry
records plus reachable call/goto subroutine targets) yields ~25 live
opcodes. The native interpreter implements the proven traversal subset:

| op | form | effect |
|----|------|--------|
| `0x01` | — | checkpoint: `+0x108 = pc` (persist resume) |
| `0x09` | — | stop: clear `+0x108` gate + call stack |
| `0x40` | operand | wait: `+0x22c = seconds`, `+0x230 = resume`, suspend |
| `0x44/45` | grp,bit | set / clear a flag bit |
| `0x46/47/48` | grp,bit,link | branch if flag bit set/clear |
| `0x60` | f32×4,link | 2D player-in-box conditional |
| `0x67` | f32×6,link | 3D player-in-box conditional |
| `0x62` | surfId,op | `FUN_0040a704` poly op + `+0x10c/+0x110` masks |
| `0x63` | mask,surfId,off | surface handler bind `+0x7c/+0x8c` |
| `0xa8` | surfId,mask | surface config `+0x6c` (+ set-`0x10` if `0x80`) |
| `0x8e` | u8,str,u8,u8,f32 | type-7 volume activation `FUN_00412d04` |
| `0x95/56/a1/e6` | spawn forms | create dormant object (Phase 5E seam) |
| `0xca` | u8 | write global byte `0x541534` |
| `0x99` | f32 | write ctx `+0x30e` |
| `0x05` | f32 | write global `0x540b58` |
| `0x0b` | u8 | write ctx `+0x11a` |
| `0x0a` | obj | test `+0x11a` against a named object |
| `0x61` | u8 | bound-object flag `+0x148` |
| `0x0d` | link | linkage gated on `0x54b5e0 & 0x5414e8` |
| `0x7b` | link | linkage only when ctx-obj ≠ current arena (partner-only) |
| `0xe0` | flag[,f32,f32] | type-9 slide/deflect-zone; flag 0 clears `0x540e24/cbc` |
| `0xfc/0x0c` | n,offs | indexed/random call / goto |
| `0xfd` | — | return |
| `0xff` | — | end-of-frame (clears running flag) |

Spawning routes through the existing `DynamicArena`/`DynamicObject`
abstractions (`allocFront` + `spawnRecord`); the spawned object's
`FUN_004566f0` table-2 init script then configures it (Phase 5I —
anim refs, radius, collision toggle). The connector door's open/close
class behavior is the native `FUN_00457738`/`FUN_004555bc` path; other
classes' native behaviors remain Phase 5E seams. Rare
subroutine opcodes (`0x04`, `0x64`, `0x87`, `0xad`, `0xdf`) are
mechanically identified as object/spawn-family but their bounded
native effect is less complete — they remain explicit seams that
diagnose rather than guess.

## 69. `FUN_004546ac` surface-handler seam (OBSERVED)

Surface contact handlers are invoked through `FUN_004546ac`, which
builds a **synthetic ctx** at `0x54c6d0` (memset `0x32e` each call),
sets `+0x60` = bound object, `+0x21d` = event byte, `+0x108` =
`scriptBase + handlerOff`, runs `FUN_004388d8` **synchronously once**,
then `FUN_00458204` cleanup. No per-event VM state persists. The
native port wires this via `SurfaceObjectState::scriptFn` →
`traversalScriptSurfaceHandler`, which builds a transient
`TraversalScriptState` (env `stateOverride`) so the gate is never
written back to the arena.

## 70. Native integration + validation (Phase 5H)

`stepTraversalRuntime` replaces the Phase 5G counted `scriptObj` seam
with real VM invocation at the proven frame position — current arena
before partner arena, partner only when active with a script. The
runtime tracks `scriptRuns`/`scriptInsnTotal`/`scriptSpawned` and a
bounded `scriptDiag` log. The deterministic traversal digest now
folds in the current arena's persisted PC/wait/call-depth plus a
surface-state summary — no script bytes.

Verified on real BUILD_A data:

- `--script-disasm` decodes every LEVEL3–LEVEL8 corridor entry script
  (CHMO/CMEAT/COLYM/CDANT/CGUNT) with zero undecoded bytes; LEVEL5
  (MUSE) has no corridor records, consistent with `+0x220 = 0`.
- LEVEL3 HMO_1: `runs=8 insn=68 spawned=0` — the checkpointed box2d
  trigger re-enters each frame, faithful to the spawn-once-then-poll
  design. Spawn arenas emit `spawned=11` objects once on frame 0.
- Portal side-6 (HMO_2→CHMO_2) and side-2 (CHMO_1→HMO_1) regressions
  hold with the VM live; type-1 attach/detach and type-3 cold
  prefetch unchanged.
- Digest is deterministic across runs (`947098fb82f45826`, 8-frame
  CHMO_1).

## 71. Phase 5H boundary

- Object/spawn-family opcodes `0x04/0x64/0x87/0xad/0xdf` and a few
  object-script opcodes (`0x74`, `0x8a`, `0x8b`, `0x98`, `0x49/4a`)
  are partially decoded — retained as bounded seams.
- The spawned door object's open/close behavior (XCORDOOR etc.) is
  native object code, not VM — the VM creates the record, the
  `FUN_004566f0` table-2 init configures it, and `FUN_00457738` +
  `FUN_004555bc` drive it (Phase 5I). The XCORDOOR route CHMO_2→HMO_3
  is proven end-to-end (portal crossing, no teleport).
- AI, camera, audio, combat and rendering opcodes are mapped in the
  dispatch table but intentionally not implemented — out of scope.
- DOS-build parity for opcode numbering is unchecked.
- Malformed-stream policy is NATIVE SAFETY POLICY: the port
  bounds-checks every fetch and halts the script rather than reading
  out of range; valid BUILD_A streams never reach the bound.

# Phase 5I — scripted corridor-door init + the XCORDOOR route

Phase 5H left the connector door inert: `traversalScriptSpawn` created
the dormant record but never ran the per-object configuration the
original applies, so a spawned `XCORDOOR` kept the spawn's dead fields
( no anim records, default radius, no collision toggle) and the
corridor could not be traversed. The missing piece is the **table-2
object-init script** the original runs inside `FUN_004566f0`.

## 72. The object-init script — `FUN_004566f0` (OBSERVED)

`FUN_004566f0` is generic object init, called for every spawned object
(the connector create `FUN_0045cffc` calls it; so does the record
spawn `FUN_00456808` tail). Its order is OBSERVED:

1. Write the default block: `+0x08`=10, `+0x38`=50, `+0x3c`=10,
   `+0x40`=15, `+0x44`=64, `+0x48`=32, `+0x58`=1.0 (scale),
   `+0xd4`=1, `+0xe0`=30.0 (anim rate), `+0xe8`=1, `+0x2c0`=1,
   `+0x2c4`=1000, `+0xc0`/`+0xac`=1, identity matrix.
2. Format the key `"%s$%s"` = `*(obj+0x60)` arena name `$`
   `*(obj+0x0c)` model name (e.g. `CHMO_2$XCORDOOR`), scan CMI
   **table[2]** for a record with that name, and — on a hit — run its
   script **immediately** via a fresh `FUN_004388d8` ctx whose bound
   object is the new object (field ops write the object's `+0xNN`, not
   an arena ctx). Table-2 `record.value` IS the image-relative code
   offset directly — unlike table-3 there is no `{str}{str}{u32}`
   indirection. A miss leaves the defaults.
3. `FUN_0045612c` transform/AABB rebuild — AFTER the script, so the
   script's `+0x58` scale / `+0x5c` zBias take effect.

Dispatch correction (OBSERVED, resolves an earlier mislabel): the
opcode jump table is indexed `table[opcode - 1]`, not `table[opcode]`.
Under the corrected map the door's init stream decodes cleanly.

## 73. `CHMO_2$XCORDOOR` decode (OBSERVED, LEVEL3.CMI image 0x204c0)

```
10 e8fd          +0x8 = +0x2a2 = 0xfde8 (MOVZX u16); +0x21f=1 (>=0xfde8)
53 03 <1.0102>   +0x58 scale = 1.0102   (mode 3 inline f32)
96 <64a68><654fc>+0x306/+0x30a = anim records (open 16f, close 21f)
97 D..           4 counted strings -> +0x31a/+0x322/+0x316/+0x31e
                 ("NONE" sentinel 0x497c54 -> slot cleared)
98 <10>          +0x312 = (+0x312&0xf)|(op&0xf0)  -> 0x18 (toggle en)
99 <20.0>        +0x30e radius = 20.0
ff               end
```

So `XCORDOOR` **does** get animation records (the earlier "no anim
records" claim was wrong — it described the spawn opcode, which does
not bind them; the init script does). `0x96` resolves two image-rel
refs through `FUN_00438898` (lazy `*ptr==0 -> symbol`, a seam — the
door's records carry a nonzero rate so no resolve fires). `0x98`
merges the operand's high nibble into `+0x312`, enabling the
collision-toggle bit `0x10`.

## 74. Connector state machine — `FUN_00457738` (OBSERVED)

Gated by `col.flags14a & 0x10` (the `0x95` connector spawn writes
`+0x148` dword `0x01108000` → `flags148=0x8000`, `flags149=0x80`,
`flags14a=0x10`). Runs first in the per-object update:

- `+0x312` is ONE byte: low nibble = phase (`8` closed, `2` opening,
  `1` open, `4` closing), high nibble = sub-flags (`0x10` = collision
  toggle, `0x40` = mask variant gated by `+0x313` bit0).
- An "active" anim (`+0x114 != 0` and `+0x118 != 0xff00`) blocks the
  open/close latch; `+0x114 == 0` latches immediately.
- Proximity on `+0x30e` (squared distance): inside → opening +
  far-side partner attach (`FUN_00432d9c`); outside → closing;
  closing latch → closed + detach.
- Collision toggle: when `+0x312` bit `0x10` is set, `+0x148` bit
  `0x10` (sweep-skip) follows the open bit — open door is passable.
- `+0x2c8` element mask rebuilt from `+0x326` ("LOCK")/`+0x32a` ("HC*").

## 75. Connector anim player — `FUN_004555bc` (OBSERVED)

Runs after the connector update for every named object. `+0x114` is a
record `{f32 rate, u32, i32 frameCount}`; per frame `+0xdc +=
rate * +0xe0 * (1/30)`; the applied frame `+0xe4 = FRNDINT(+0xdc)`.
Reaching `frameCount-1` on a non-looping anim (`+0x148` bit3 clear)
clamps `+0xdc` and latches `+0x118 = 0xff00` — the connector's "done"
gate. Transitions write `+0xdc=-1`, `+0xe4=0xffff`, `+0x118=0xffff`.
`FUN_00455890` (apply frame to element transforms) is a render seam —
collision only needs the latch.

## 76. Phase 5I validation (OBSERVED on real BUILD_A LEVEL3)

- `traversalScriptSpawn` order matches `FUN_004566f0`: connector
  defaults → `initObjectDefaults` → table-2 init →
  `rebuildObjectTransform`. The temporary forced sweep-skip `0x810`
  is removed; the door is born solid and opens via the toggle.
- `mdk-inspect --traversal-runtime ... --arena CHMO_2 --start 3 1230
  -929`: the scripted `0x95` spawns XCORDOOR, its `CHMO_2$XCORDOOR`
  init runs (`diag=0`), the door animates `st 0x18 -> 0x12 -> 0x11`
  over 16 frames as `+0x118` goes `0xffff -> 0xff00`, `flags148` goes
  `0x8000 -> 0x8010`, partner HMO_3 attaches (`ca4`), and the player
  crosses the `y=1237` portal into HMO_3 (arena 11 -> 2) through the
  normal type-6 path — `portals=1`, `teleport=0`, no scripted warp.
- Synthetic tests cover `cmiObjectScriptOffset` lookup, the door
  init-stream field writes, and the open/close/collision-toggle cycle.
- The interpreter covers the field/flag/mask/connector family plus
  `0x5a`/`0xc6`/`0x75` (`+0xe8`/`+0x104`/`+0x118` writes) — 58 of the
  63 LEVEL3 table-2 scripts init fully. The remainder use
  conditional/string opcodes not yet decoded (`0x07` yaw-normalize,
  `0x3c`, `0x55`, `0xd4`, `0xe8`); they halt with a diagnostic per the
  safety policy — the spawn completes regardless.

# Phase 5J — Player Look and Camera Orientation

Phase 5J pins down the normal traversal look/orientation path. The
headline result is a **negative proof**: normal traversal has NO raw
mouse look. The raw-delta consumer `FUN_00464624` is the sniper-mode
update, and `FUN_00465228` (the normal movement path) never reads the
raw deltas. Normal look is semantic-key driven through
`FUN_00465c4c`.

## 77. Call order (OBSERVED, FUN_00436100 / FUN_00463608)

Per traversal frame, in original order:

1. `FUN_00402388` — `consumeGameplayInput` produces the frame-N+1
   merged control block (raw deltas land at `0x54b644`/`0x54b648`;
   `0x4ce780`/`0x4ce784` carry the level-triggered look flags).
2. `FUN_00463608` dispatch head — the pending-event slots
   `0x54cb00`/`0x54cb08` are cleared; `0x540cbc` (cbc) is reset while
   `cac` sits in the transient set `{300, 400, 500, 600, 601}`.
3. Normal branch (`cac < 800`): `FUN_00465228` → horizontal
   integration → `FUN_004630d4` collision → `FUN_0046603c` slide
   helper → `FUN_00466740` jump machine → `FUN_00465c4c` look →
   `FUN_00469cd0` deferred ops.
4. Scripted branch (`cac >= 800`): semantic channels `d48`/`d4c`/
   `d50`/`d54` cleared (no horizontal motion), then the same
   `FUN_0046603c`/`FUN_00466740`/`FUN_00465c4c` tail. The look state
   `0x324` is the only >=800 state whose exit path is proven.
5. `FUN_00406f14` merges the frame-N+1 block into `0x4ce6xx` —
   the one-frame latency hand-off (movement AND look share it).
6. Idle restore: when `cbc == 0` and the pending priority is also 0,
   the dispatcher posts `cb00=1 / cb08=0x65` (unmounted idle; the
   `0x64` mounted variant is deferred). Then the latch:
   `cbc < cb00 -> cac = cb08, cbc = cb00`.
7. `FUN_00461954` (render pass): the `0x324` handler clears `cbc`
   once `d58` has re-centred to exactly 0 — the look-state exit.
8. `FUN_004301e0` view tail: arena-scalar blend → z-delta follower →
   lookEff clamp → view yaw → pitch lift → effective pitch.

## 78. `FUN_00464624` — sniper mode (OBSERVED, boundary)

Dispatched only when `c9c != 0 && ca0 != 0` — the sniper-mode latch
pair (`c9c` set by the sniper-enter event `0x323` path; `ca0`
stepped by `FUN_00436100`'s head through scope phases 1→2→3). The
function body is the sniper update: strafe slide, semantic aim
channels `d48`/`d4c`, sniper-exit on the `0x4ce76c` pulse, fire and
zoom handling, plus the raw-mouse aim fallback:

```
pitch (0x540b54) += dy * 0.12 * f0 * b58 * (5/12)   clamp [-50, +50]
yaw   (0x540c2c) -= dx * 0.12 * f0 * b58 * (5/12)   wrap  [0, 360)
```

gated on `MouseOn` (0x541472) AND only when neither semantic aim
channel is active; `MouseYReversed` (0x541476) negates the scaled
dy. All sniper-side — NOT ported (Phase 5J boundary). The mounted
reticle path is `FUN_004691c4` (raw dx/dy -> reticle pixels
[128,472]/[64,296], no YReversed) — also deferred.

## 79. `FUN_00465c4c` — semantic look integrator (OBSERVED,
instruction-level)

```
eligible = (cbc < 8 || cac == 0x324)      // priority/state gate
           && (c78 & 0x7fffffff) == 0     // vertVel == ±0
           && (c54 & 1)                   // grounded
```

- `eligible && lookUp`   → `d58 -= f4 * 90`, clamp `>= -60 - a462`
- `eligible && lookDown` → `d58 += f4 * 90`, clamp `<= +90 - a462`
- otherwise              → `d58 -> 0` at `f4 * 200`, sign-snapped

`lookUp` is tested first — it wins a press-conflict. `a462` is
`[0x540c48]+0x462`, the current arena's rest-pitch scalar, so the
ABSOLUTE pitch `a462 + d58` stays in `[-60, +90]`. Every driving or
draining frame posts `cb00=8 / cb08=0x324` into the shared slots;
when `d58` sits at exactly 0 the function rewrites the slots with
their entry values — a self-store, no observable post. The look is
momentary: release (or any gate failure) drains it at 200 deg/s.

Note the recenter branch is reached on ANY eligibility failure —
there is no early-out; a blocked look still drains the offset.

## 80. `playerObj + 0x462` resolved (OBSERVED)

The earlier tentative "pitch limit" label is resolved: `+0x462` is a
field of the ARENA record (`[0x540c48]` — the current arena object),
not the player object. It is the arena rest-pitch scalar that both
`FUN_00465c4c`'s clamp bounds and `FUN_004301e0`'s `b54` blend
target are relative to. Debug keys write `b54 = a462` directly; the
port models it as `TraversalArena::scalar`.

## 81. Angle units + pitch limits (OBSERVED)

All orientation state is DEGREES — `FUN_00437f98` (the shared
trig helper) takes degrees (`out1=sin(deg), out2=cos(deg)`), and the
clamp constants decode as `+90`/`-60`/`40`. Limits: `d58` is bounded
to `[-60 - a462, +90 - a462]`; the effective sum in `FUN_004301e0`
re-clamps `b54 + lookEff` to the same range while `b54` is
mid-blend. No ±89 — the original asymmetry (-60/+90) is preserved.

## 82. Persistent orientation state (OBSERVED)

| address      | native field            | proven semantic                    |
|--------------|-------------------------|------------------------------------|
| 0x540d58     | `PlayerLookState::lookPitchOffset` | semantic look offset (deg) |
| 0x540b54     | `TraversalRuntime::viewScalar`     | blended arena scalar (init 4.0, `FUN_00433c4c` — corrected Phase 5K) |
| 0x540b50     | `PlayerViewTail::viewYawDeg`       | view yaw = 90 - yaw (deg)  |
| 0x540be0     | `PlayerViewTail::viewPitchDeg`     | effective pitch (deg)      |
| 0x49b718     | `PlayerViewTail::viewZDelta`       | smoothed z-delta follower  |
| 0x49b71c     | `PlayerViewTail::viewPitchLift`    | air-charge pitch lift      |
| 0x540c2c     | `PlayerMotionState::yawDeg`        | locomotion yaw (deg)       |
| 0x540cac     | `TraversalRuntime::locoState`      | dispatched state (cac)     |
| 0x540cbc     | `TraversalRuntime::eventPriority`  | current event pri (cbc)    |
| 0x54cb00/08  | `TraversalRuntime::eventType/eventMag` | pending event slots    |
| 0x540bec     | `TraversalRuntime::flagBec`        | scalar blend gate          |
| 0x540c9c/a0  | — (sniper latch pair)              | sniper-mode gate, deferred |
| 0x540c84     | `PlayerMotionState::airCharge`     | air-charge counter         |

## 83. View yaw + coupling (OBSERVED)

`0x540b50 = 90 - yaw` — the view yaw is a pure function of the
locomotion yaw. There is NO independent free-look yaw in normal
traversal (the independent aim yaw exists only inside the sniper
branch, writing `c2c` directly). Movement yaw semantics are
untouched — no double yaw integration.

## 84. `FUN_004301e0` view tail (OBSERVED, ported slice)

Runs in the render pass after the traversal dispatch. Ported:

- `c9c == 0 && 0x49b740 != 0` → `FUN_00431100` view-snap seam +
  prev-pos commit only (seam counted, not emulated).
- `c9c != 0` → `bec = 0`. Else `b54 != a462 && bec == 0` →
  `b54 = b54*0.85 + a462*0.15` (the blend); else `bec = 0`.
- `dz = clamp(posZ - prevPosZ, ±0.5)`; `b718 = b718*0.97 + dz*0.03`;
  then a sign-disagreement slew at `f0*0.02` that never crosses the
  raw value (the EMA alone decays too slowly to flip sign).
- `lookEff = d58`; while `d58 != 0 && b54 != a462`, the sum
  `b54 + lookEff` is bounded to `[-60 - a462, +90 - a462]` —
  `lookEff` takes the excess.
- `b71c = c84 * 2/3` capped at 40 while `c84 != 0` (negative c84 is
  NOT capped — OBSERVED asymmetry); else decays to 0 at `f4 * 40`,
  clamped at 0.
- effective pitch `0x540be0 = b54 + lookEff - b718*40 + b71c`.
- prev-pos commit (`0x540c08..0x540c10 = 0x540bfc..0x540c04`) runs
  every frame on all paths.

The matrix rows / camera position / FOV writes that follow
(`0x540b28..`) are the renderer boundary — NOT ported.

## 85. Timing (OBSERVED)

- `FUN_00465c4c` integrates with `f4` (`0x49b6f4`, delta-seconds
  ~1/30) — NOT `f0`. 90 deg/s look rate, 200 deg/s recenter.
- The `b718` sign-slew is `f0`-scaled (`0x49b6f0`, frame-units);
  the `b71c` lift decay is `f4`-scaled. The asymmetry is preserved.

## 86. Raw vs semantic, MouseYReversed, MouseOn (OBSERVED)

- Raw `dx`/`dy` (`0x54b644`/`0x54b648`): consumed ONLY by
  `FUN_00464624` (sniper) and `FUN_004691c4` (mounted reticle).
  `FUN_00465228` never reads them — normal traversal has no raw
  mouse look. Phase 5A carries the fields as input state only.
- Semantic `0x4ce780`/`0x4ce784`: keyboard level | button-mask bits
  (LookUp bit 0x8, LookDown bit 0x10) — merged with the same
  one-frame latency as movement.
- `MouseYReversed`/`MouseOn` gate the SNIPER raw path only; the
  semantic path ignores both (a semantic "mouse turn" axes-map
  product would arrive as key-level look flags — unchanged by
  either flag). Not reproduced in normal look.

## 87. Native implementation

`src/core/player_look.{h,cpp}`:

- `integratePlayerLook(ctrl, env, state)` — `FUN_00465c4c`. Returns
  the post flag; the runtime owns the shared pending slots.
- `updatePlayerViewTail(env, tail)` — the `FUN_004301e0` orientation
  tail (z-delta follower, lookEff clamp, view yaw, lift, effective
  pitch). The `b54` blend + `bec` gate stay in the runtime.

`stepTraversalRuntime` dispatch restructure (OBSERVED ordering):
pending-slot clear + transient `cbc` reset at the head; the
`cac >= 800` scripted-branch model (channels cleared, no horizontal
motion) scoped to `0x324`; motion/vertical/look posts all write the
pending slots in producer order; the `FUN_00406f14` merge; then
idle restore + the `cbc < cb00` latch; the `FUN_00461954` fold runs
in the render pass before `FUN_004301e0`'s tail.

## 88. Phase 5J validation

- `tests/native/test_main.cpp` `test_player_look`: 61 checks —
  idle/up/down/both-pressed integration, arena-relative clamps,
  200 deg/s recenter with the exact-zero snap, all four eligibility
  gates (cbc>=8, vertVel, grounded, the `cac==0x324` override), f4
  timing, the view-tail clamp/EMA/slew/lift math, a golden
  press-hold-release-settle sequence, and a `stepTraversalRuntime`
  end-to-end test (real floor: raw-key latency, `0x324` entry,
  scripted-branch motion suppression, recenter, fold, idle exit).
- `--selftest-player-look`: 'A'/'Z' SDL script through the real
  input seam; verifies the offset, posts, and the latched state
  sequence per frame. PASS.
- `--traversal-runtime` on LEVEL3-8: all PASS; `CHMO_2 -> HMO_3`
  still completes `portals=1`, `teleport=0` — the look path does
  not perturb the corridor route. The digest folds the deterministic
  look/view fields; `59db780d09689c7b` (120f) is stable across runs.
- 3481 native checks / 0 failures; CTest 1/1; Python 17/17.

## 89. Phase 5J boundary / remaining unknowns

- `FUN_00464624` (sniper mode: strafe slide, semantic+raw aim, exit,
  fire, zoom) — decoded but NOT ported; the `c9c`/`ca0` latch pair
  and scope-phase stepping live in `FUN_00436100`'s head (seam).
- `FUN_004691c4` mounted reticle (`e70 & 0x40000` branch) — decoded
  boundary, not ported.
- The generic animation machine `FUN_00461954` — only its `0x324`
  `cbc`-clear write is folded; the full machine is deferred, so
  other `cac >= 800` states (806 hard-land, `0x323`/`0x384` sniper,
  `0x385` slide) keep the normal dispatch path for now.
- `FUN_004301e0`'s camera matrix/position/FOV block (`0x540b28..`)
  and `FUN_00431100` view-snap — renderer boundary, counted seams.
- `0x540c84` air-charge semantics belong to the vertical model
  (Phase 5C); its `b71c` lift consumption is ported, its production
  is unchanged.

# Phase 5K — Normal Camera Pose and View Matrix

Phase 5K reconstructs the remainder of `FUN_004301e0` — everything
after the Phase 5J effective-pitch write at `0x540be0` — plus the
`FUN_00431100` overhead block. The result: the original's full
normal-traversal camera pose (world position, orientation basis, the
two 3x4 view matrices, projection scalars, viewport rect) is now
produced by native code every frame, with an explicit seam where the
unported camera-collision call sits.

## 90. Ordered instruction map (OBSERVED, 0x43042b..0x4309dd)

After the `0x540be0` store the block runs, in original order:

1. `FUN_00437f98(viewYaw)` / `FUN_00437f98(effPitch)` — sin/cos pairs
   (locals, plus the `0x540be4`/`0x540be8` cache written earlier in
   the prefix).
2. Position branch on `effPitch` sign (`FCOMP` at `0x430437`).
3. Shake add: `|0x540ce4| != 0` -> `camX += 0x540cf8*0.2`,
   `camY += 0x540cfc*0.2` (f64 `0x497280`).
4. Banked up-vector: `bank = 0x540b4c + 0x540b60`, trig call.
5. `right = up x back`; `down = -(back x right)` (f32 locals).
6. Projection scalars `0x540bf0/bf4/bf8` (`bf8 = -1.0` normal).
7. `0x49b710 && 0x540d58 == 0` -> `FUN_00430bf8` obstruction call
   (seam — may move BOTH `0x540b28` and `0x540bfc`).
8. `0x540b80` M1 commit (rows `bf0*right | bf4*down | bf8*back`,
   `t = scale*(-(row.cam))`); the unscaled `t` stays live on the FPU
   stack.
9. `0x540bb0` M2 commit (rows `basis*|basis|`, `t = t_unscaled*|row|`;
   `FSQRT` lengths never stored f32).
10. View-config write (`0x540b64..0x540b7c`): normal vs sniper rect.
11. Tail: `0x49b714 = 0`; `eye = player + (0,0,3.0)`;
    `FUN_00435178(cur, eye -> camPos)`; `hit == 0x540ca4` ->
    `0x49b714 = 1`. `FUN_00435178` is a pure segment scan (verified
    read-only on both endpoints) — the same routine used for the
    player prev->pos portal test.

`prevPos <- pos` commits at `0x430272` (normal path, inside the tail
before the camera block) and `0x4309ed` (overhead path, after
`FUN_00431100`).

## 91. Camera globals (OBSERVED — writers/readers xref-verified)

| Address | Field | Notes |
| --- | --- | --- |
| `0x540b28..30` | camera world pos | written here; read by `FUN_00430bf8`, the portal tail, render |
| `0x540b34..3c` | view row0 "back" | `(-sinY*cosP, -cosY*cosP, sinP)` — prefix write |
| `0x540b40..48` | view row1 "up" | banked up (below) |
| `0x540b4c` | player bank deg | writer: dispatch/state machine (`rt.motion.bank`) |
| `0x540b50` | viewYaw | `90 - yaw` (Phase 5J) |
| `0x540b54` | view scalar / rest-pitch blend | init 4.0 (`0x433c9d`), blend 0.85/0.15 gated by `bec` |
| `0x540b58` | zoom | init 2.4; level `ZOOM_%4.4d` + debug keys scale it |
| `0x540b5c` | height offset | init 0; debug +-25 (`FUN_00464d10`) |
| `0x540b60` | aux bank term | adds into bank; feeds `bankIdle` test |
| `0x540b64..7c` | view rect config | zoom-mode copy / W/H / centre / origin |
| `0x540b80..ac` | M1 view matrix | projection-folded world->camera |
| `0x540bb0..dc` | M2 basis matrix | unscaled snapshot (`*|row|` lengths) |
| `0x540be4/e8` | sin/cos pitch cache | prefix writes |
| `0x540bec` | blend gate | `FUN_0045e590` focus-aim sets it one frame |
| `0x540bf0/f4/f8` | scaleX/scaleY/scaleZ | `1/(zoom*.5)`, `1/(zoom*(H/W)*.5)`, `-1`/`+1` |
| `0x540ce4` | shake magnitude | decays in `FUN_00436100` head |
| `0x540cf8/cfc` | shake XY | written by sniper branch; consumed here |
| `0x540db4` | pullback | init 8.0 (`0x433c93`); `FUN_00461878` resets |
| `0x540db8` | eye height | init 4.5; `FUN_00461954`/`FUN_00464624` adjust |
| `0x49b710` | obstruction enable | cheat toggle (`FUN_00423ca0`, gated `0x5414e4`) |
| `0x49b714` | view-on-partner | portal tail output; render select at `0x436405` |
| `0x49b740` | overhead gate | `0x40031` script event + cheat; latch |
| `0x49b74c` | overhead height | default 50.0; clamp 20..200 debug |
| `0x5414bc` | alt aspect | swaps the 360/600 pair for 280/384 |
| `0x540c9c/0x540ca0` | mode gates | both set -> sniper rect write only |
| `0x540d58` | semantic look offset | `|d58|==0` gates the obstruction call |

`0x540d00` is resolved: read+written ONLY inside `FUN_00463608` — a
state-machine field, not camera state. `FUN_00401ed4` is a generic
helper called from ~30 sites, none in the camera block — removed
from the concern list.

## 92. Camera position (OBSERVED)

Anchor: the player position `0x540bfc..0x540c04` itself (no bone/
tooth lookup in the normal path — `"Bones tooth not found"` lives in
`FUN_0045897c`, the mover/bone resolver, unrelated to the camera).

```
height = eyeHeight - heightOffset            // shared
if effPitch > 0:
    rise = (1 - cosP) * 5.0                  // 0x497278
    camX = px + sinYaw*(rise - pullback*cosP)
    camY = py + cosYaw*(rise - pullback*cosP)
    camZ = pz + height + pullback*sinP
else:
    D = pullback                             // pitch >= -20
    D = (pitch + 100)*pullback*0.0125        // pitch < -20 (cont. at -20)
    camX = px - sinYaw*D*cosP
    camY = py - cosYaw*D*cosP
    camZ = pz + height + D*sinP
if |shakeMag| != 0: camX += shakeX*0.2; camY += shakeY*0.2
```

Two quirks preserved: the `pitch>0` branch adds a `(1-cosP)*5` rise
term that pulls the camera INWARD as pitch grows, and the `pitch<=-20`
branch shrinks the arm linearly to zero at `-100`. `viewZDelta` /
`viewPitchLift` do NOT feed position — they only shape `effPitch`
(Phase 5J); raw `pz` anchors Z directly.

## 93. Basis and matrix (OBSERVED)

Coordinate convention (Phase 5B, CONFIRMED): +X forward at yaw 0,
+Y left, +Z up; `viewYaw = 90 - yaw`.

```
back  = (-sinYaw*cosP, -cosYaw*cosP, sinP)   // 0x540b34 row
up    = (sinY*cosB*sinP + cosY*sinB,
         cosY*cosB*sinP - sinY*sinB,
         cosB*cosP)                          // 0x540b40 row, bank b4c+b60
right = up x back                            // locals
down  = -(back x right)                      // == -up when orthonormal
```

Both matrices are row-major 3x4, `[x y z t]` per row,
`out_i = row_i.xyz . p + t_i` (consumer `FUN_0046b4f8`: row0 ->
screenX numerator, row1 -> screenY numerator, row2 -> depth):

- `0x540b80` M1 (world->camera, projection folded):
  rows `scaleX*right`, `scaleY*down`, `scaleZ*back`,
  `t_i = scale_i * (-(row_i . cam))`. `scaleZ=-1` flips the back row
  to forward-facing, so depth `= fwd.(p - cam) > 0` in front.
- `0x540bb0` M2 (unscaled snapshot): rows `basis_i*|basis_i|`,
  `t_i = (-(row_i . cam))*|basis_i|` — the sqrt lengths multiply BOTH
  the coefficients and the translation (the `t` values stay on the
  FPU stack across the sqrt block). Readers: `FUN_0042b0c0` (camera
  nudge API — shifts camPos along M2's row0 and re-folds M1),
  `FUN_0042e684` (framebuffer rotate/blit — renderer), `FUN_004691c4`
  (sniper reticle — Phase 5L boundary).

## 94. Projection / FOV (OBSERVED)

There is NO stored FOV angle. Projection is the folded scale pair:
`scaleX = 1/(zoom*0.5)` (= 0.8333 at zoom 2.4) and
`scaleY = 1/(zoom*(H/W)*0.5)` (= 1.3889 normal, 1.1429 alt aspect).
`scaleZ = -1.0` normal / `+1.0` overhead (row-sign flip, not a
depth scale). The viewport rect write is part of this block: normal
`600x360@(0,0)` centre `(300,180)` mode-zoom `2.4`; the sniper rect
`384x280@(107,79)` centre `(299,219)` mode-zoom `1.0` is written when
`c9c && ca0` — the rect write is ported (it's in this block) but the
sniper POSE is Phase 5L scope.

`zoom` (`0x540b58`) is loaded from the level `ZOOM_%4.4d` MTI record
(`FUN_0040ef28` writes both `b58` and `b64`) and scaled by debug
keys in `FUN_00464d10`.

## 95. FUN_00431100 — overhead view (OBSERVED, ported)

Gate: `0x540c9c == 0 && 0x49b740 != 0` (early path at `0x4301f4`).
`0x49b740` is a LATCH: set to 1 (with `0x49b74c = 50.0`) by the
scripted `0x40031` spawn event inside `FUN_00463608`, cleared on
completion; also toggled by the cheat dispatcher `FUN_00423ca0`
(gated by cheat flag `0x5414e0`). Persistent across saves
(`FUN_00427218`).

The block writes: `camPos = (px, py, pz + overheadHeight)`; the same
scale triple except `scaleZ = +1.0`; trig on the RAW locomotion yaw
`0x540c2c` (not `viewYaw`); M1 rows `[scaleX*(sinY,-cosY,0) |
scaleY*(-cosY,-sinY,0) | (0,0,-1)]` with `t = scale*(-(row.cam))`;
M2 the same rows unscaled. It does NOT touch the basis rows
(`b34..b48` go stale), the trig cache, the view config, `b714`, or
the portal tail — and `prevPos` commits after it (`0x4309ed`).

## 96. Camera collision boundary (OBSERVED, seam)

`FUN_00430bf8` is a genuine obstruction subsystem, called between
basis compute and the matrix commit when `0x49b710 != 0 &&
0x540d58 == +-0`: arena collision query `FUN_00407fc0` on camPos, up
to three slide-sample retries through `FUN_00418c60`, then
`FUN_004630d4` applies the resulting delta to BOTH the player and
the camera; an object-list pass gated by `0x540c68` follows. The
native port exposes the call site as `PlayerCameraFrame::
obstructionSeam` (counted in `seams.cameraObstructionCalls`) with
`env.playerPos` in/out — the call contract is real, the internals
are deferred (needs the arena collision query against the camera
point, not a spring-arm guess).

## 97. Native implementation

`src/core/player_camera.{h,cpp}`:

- `PlayerCameraState` — the persistent camera globals (zoom 2.4,
  pullback 8.0, eyeHeight 4.5, heightOffset 0, overheadHeight,
  shake triple, obstruction gate) with `FUN_00433c4c`-proven
  initializers. NOTE: `viewScalar` init corrected 6.0 -> 4.0 to
  match `0x433c9d` (`0x40800000`) — no writer ever stores 6.0.
- `PlayerCameraPose` — the per-frame `0x540b28..` block: pos, back,
  up, pitch trig cache, M1 `view[3][4]`, M2 `basis[3][4]`, scalars,
  view rect. Stale fields persist across frames like the original.
- `updatePlayerCamera` — the `FUN_004301e0` tail in original order.
- `updatePlayerCameraOverhead` — `FUN_00431100`.
- `cameraTrigDeg` reproduces `FUN_00437f98`: `deg * 0x497924` (the
  stored f64 `0x3f91df46a2529d35`, 4 ULP below correctly-rounded
  pi/180), `sin`/`cos` on the same f64 product, f32 stores.
- x87: the port accumulates each FLD/FMUL/FADD chain in double and
  rounds once per FSTP — matching the original's store points.

Runtime (`stepTraversalRuntime`): after `updatePlayerViewTail`, the
`entryPos <- pos` commit runs at the original `0x430272` point; the
normal path calls `updatePlayerCamera`, counts the obstruction seam,
then runs the portal tail via the new `traversalPortalScanSegment`
(eye `pos+3z` -> `pose.pos`; `hit == partner` -> `viewOnPartner`).
The `flag49b740` early path calls `updatePlayerCameraOverhead` and
commits `entryPos` after it (`0x4309ed` ordering). `TraversalFrame`
now carries `camera` + `overheadViewActive` + `viewOnPartner`.

## 98. Validation

- `test_player_camera` in `test_main.cpp`: 109 checks — exact-value
  oracle cases (pitch-0 pose, +30 rise branch, -50 shrunk pullback,
  -20 boundary continuity, bank rotation, alt aspect, zoom, shake
  gate, overhead block), basis orthogonality/unit-length, M1/M2
  layout + translation signs, stale-field persistence.
- `mdk-inspect --selftest-camera-pose`: 14-step diagnostic — PASS.
- The traversal digest now folds the pose (pos, basis rows, both
  matrices, scalars, rect, overhead/viewOnPartner selects) as f32
  bit patterns — deterministic across runs.
- LEVEL3-8 `--traversal-runtime`: all PASS; `CHMO_2 -> HMO_3`
  regression unchanged (`0x18->0x12->0x11`, `flags148 0x8000->0x8010`,
  `portals=1`, `teleport=0`), camera stays finite through the portal.
- 3590 native checks / 0 failures; CTest 1/1; Python 17/17; all
  app selftests PASS.

## 99. Phase 5K boundary / remaining unknowns

- `FUN_00430bf8` internals (arena query + slide retries + object
  pass) — call site + contract proven, internals deferred.
- `FUN_0042b0c0` camera nudge (world-tick + event writers) — the M2
  consumer that mutates camPos post-pose; decode noted, not ported.
- Sniper pose/zoom (`FUN_00464624`, `FUN_004691c4` reticle) — the
  rect write inside this block is ported; the pose path is Phase 5L.
- Renderer consumption of M1 (`FUN_0046b4f8` clip codes and onward)
  — transform convention proven at the boundary; drawing itself is
  out of scope.
- Whether the original expresses an equivalent FOV angle anywhere —
  UNKNOWN; only the scale pair is evidenced.

# Phase 5L — Sniper Scope and Mounted Reticle

Phase 5L reconstructs the original's two aim modes: the sniper scope
(`FUN_00464624` core + `FUN_00464b50` zoom + `FUN_00461878` reset) and
the mounted reticle / bomb-sight (`FUN_00463608` mount-scan + class
entries + `FUN_004691c4` update). They are SEPARATE state paths that
share only the semantic-channel globals and the `FUN_00467a00` drain —
the executable gives no evidence of a unified weapon system, and none
was introduced. Oracle: `MDK95.EXE` BUILD_A only.

## 100. Dispatch order (OBSERVED, FUN_00463608)

Each frame the dispatcher picks exactly one branch:

1. `e6c != 0 && +0x14b & 2` — the mounted-class dispatch (byte2 of the
   `e70` dword selects: `1` XD/XD2 -> `FUN_00467ac4`, `2` XSNOWB ->
   `FUN_00467ed0`, `4` X_STRIKE/XE -> `FUN_004691c4`). Recognised
   classes then run the `FUN_00469cd0` weapon-slot seam; an
   unrecognised class logs "Unrecognised controlalien" and unmounts.
2. else `c9c != 0` — sniper. `ca0 == 0` clears the four semantic
   channels; `ca0 != 0` runs `FUN_00464624` then the `FUN_00469b98`
   weapon-select seam.
3. else `c9c == 0` — `cac >= 800` scripted (channels cleared, no
   horizontal motion) or the `FUN_00465228` normal path (whose tail
   runs the mount-scan + class entry + sniper entry + normal-fire
   latch).

The mount outranks the sniper and the normal path. This is the order
now wired into `stepTraversalRuntime`.

## 101. Sniper entry (OBSERVED, FUN_00465228 tail)

`itemUse` is checked first (seam), then the sniper pulse:

- Gate: `ce76c (sniperPulse) && cbc < 8 && cb00 < 8`.
- Eligibility: `c6c == 0` (vertical disabled) -> free; else requires
  `vertVel == +-0 && grounded && no dying-surface record` under the
  contact poly (`FUN_0041342c` — kind -1, rate > 0, surfType match).
- Writes: `c74 = 0`, `c9c = 1`, `ca0 = 0`, all four channels = 0,
  `cb08 = 0x323` / `cb00 = 8` (scope-in state at priority 8).

The entry reads the N-1 merged frame — the same one-frame latency as
movement.

## 102. Scope phase (OBSERVED, FUN_00436100 head)

`sniperScopePhaseAdvance` runs after the input consume, before the
dispatch — one phase per frame, committed then requested:

- `ca0 == 2` -> overlay commit (`FUN_00416700`), `e74 = 0`, `ca0 = 3`.
- `ca0 == 1` -> overlay request (`FUN_0041664c` when unlatched),
  `5414bc = 1`, `b54 = 0`, `d58 = 0`, `ccc..cd4 = 0`, `e94 = e98 = 1.0`,
  `ca0 = 2`.

The `ca0 = 1` trigger is the `0x323` anim first-frame write below, so
the full scope-in spans several frames: entry -> `0x323` anim sets
`ca0 = 1` -> head walks `1 -> 2 -> 3`.

## 103. `FUN_00464624` — sniper core (OBSERVED, ported)

Original order inside the scoped frame:

1. `c74 = 0`; `FUN_00467180` gravity + vertical collision DIRECTLY —
   no jump machine (the normal path reaches gravity through
   `FUN_00466740`; the sniper skips it). `integratePlayerGravity` is
   the extracted callable.
2. Last contact (`e4c`) picks the lateral surface multiplier; the
   `d50` channel (semantic strafe, `accelChannel`/`decelChannel`,
   `kLateralCapK = 0.25`, inner `0.0888..`, outer `0.1777..`, bound
   `0.6667`) feeds a swept strafe `FUN_004630d4(d50*f0*sin,
   -d50*f0*cos, 0, 0.75)`.
3. Abort: `vertEnable && !grounded && (vertVel < -30 || vertVel > 0)`
   -> `FUN_00461878` (the `-30..0` band does NOT abort).
4. Semantic aim from the N-1 frame through `directChannel` (NOT
   frame-scaled): `yawNorm -> d4c` (suppressed while strafing),
   `moveNorm -> d48`.
5. Raw mouse from the CURRENT frame (zero latency) — only when no
   semantic channel fired, `mouseOn != 0`, not strafing. `kMouseK =
   0.12`; `MouseYReversed` flips dy.
6. Non-fired channels decay via `decelChannel` (16/15 inside +-4.0,
   0.8 outside).
7. Apply: `b54 += d48*f0*b58*0.4167` clamped `+-50`; `c2c -=
   d4c*f0*b58*0.4167` wrapped to `[0,360)`.
8. Post-abort guard `ca0 == 0 -> RET` (the aim above already ran).
9. Manual unscope `ce76c != 0` OR dying-surface contact -> the
   `0x464986` block (`c9c = 0`, `cb08 = 0x384`/`cb00 = 9`, camera
   restored, `ca0` LEFT — only the abort reset clears it).
10. Fire gate `ce770 != 0 && d0c >= 5 && 54161b == 0` -> `FUN_0045f138`
    (d0c = 0 only on the NON-sniper branch).
11. Zoom tail `FUN_00464b50`.

## 104. `FUN_00464b50` zoom + `FUN_00461878` reset (OBSERVED, ported)

- Zoom: scoped `b58` stays in `[floor, 1.0]`; `floor =
  min(focus-derived or 1000, 0.25)`. `d54 < 0` zooms in
  (`b58 /= 1 - d54`), `d54 > 0` zooms out while `b58 > 1.0`; the
  channel decays `0.0147/frame` when no zoom input. Entry snaps
  `2.4 -> 1.0`.
- Reset `FUN_00461878` (runs only while `c9c != 0`): `c9c = 0`, scope
  latch released, `b54 =` arena scalar, `5414bc = 0`, all channels = 0,
  `b58 = 2.4`, `db8 = 4.5`, `db4 = 8.0`, `ca0 = 0`, `d34 = -101`,
  `d58 = 0`, `cbc = 0`, `cac = 0x64`.

## 105. `FUN_00467a00` — shared drain (OBSERVED, ported)

Difficulty-scaled energy/health drain shared by the sniper zoom drain
and the mounted reticle: `easy -> max(1, 2a/3)`, `normal -> a`,
`hard -> 2a`; `dac += 25*scaled` clamped `[75,180]`; `541554 -=
scaled`; `d5c += scaled`. Runs only while `health` or the health gate
is nonzero.

## 106. Anim subset (OBSERVED gate, bounded port)

`FUN_00436ea8 -> FUN_00431300 -> FUN_00461954` runs the player anim
job only when `(!c9c || ca0 == 0) && (!e6c || !(e70 & 0x20))` and is
suppressed entirely while `0x4999d0 && 0x541548`. The ported subset
`playerAnimAdvance` handles the sniper-lifecycle states only; the full
frame-table machine stays deferred:

- `0x323` (scope-in): first-frame resets `cb4`, steady advances it by
  `frameStep`; `d34 = rint(scopeScale*eyeHeight + 29)` (FUN_0047d59a
  round), `pullback = 0`, `eyeHeight = 4.0`, `ca0 = 1` on the pending
  frame.
- `0x384` (unscope): steady releases the overlay + `cbc`, advances
  `cb4`; both write `pullback = 8.0`, `eyeHeight = 4.5`, `d34 = -101`.
- Tail: `cb0 = cac` (first-frame latch) every dispatched state.

## 107. Mounted reticle (OBSERVED, ported — separate from sniper)

`FUN_00463608` mount-scan (normal-path tail): `e68 = rideObj` when
`rideObj && rideActive`, then mounts `e68` when `named && +0x14b&2 &&
!e6c && !c74`. Class dword `e70` byte2 selects the entry:

- XD/XD2 -> `e70 = 0x10039`, `+0x14a |= 8`, yaw/pos pinned, channels
  cleared (requires `c74 == 0 && airCharge == 0`).
- XSNOWB -> `e70 = 0x20002`, `+0x148 |= 0x80800` then `&= ~0x80100`,
  `FUN_00461878(0)` if riding.
- X_STRIKE/XE -> `e70 = 0x40031`, overhead cam `b740 = 1`, aux fields
  cleared, `b74c = 50`, `d48 = 300`/`d4c = 180`, `ea0 = 10`,
  `ea4 = 1.0`, channels cleared.

`FUN_004691c4` per frame: yaw/pos pinned to the mount; overhead settle
`-= f4*25` (obj `+0x148 |= 0x10` on expiry); semantic `accelChannel`
or raw mouse `dx/3`, `dy/3` (no MouseYReversed, no zoom gain);
channels decay `0.6667`; integrate then clamp `x [128,472]`,
`y [64,296]`; semi-auto latch `fire == 0 -> d0c = 999`, held fire
spawns only on the armed frame then `d0c -= frameStep` (negative
allowed); recharge one bomb/second to 10; energy deficit off the
`10000` sentinel drains through `FUN_00467a00` and empty health kills
the mount.

## 108. World-tick internals (OBSERVED, FUN_00436d60)

Wired at the `FUN_00436d60(1)` seam (after the extra `flag541548`
tick, before `FUN_0040b4dc`/`FUN_00435eec`):

- `FUN_00436f08 -> FUN_00436088`: `d0c++` while `5414d4 (hudActive)` —
  resolved in Phase 6B as the frame-skip draw gate written by the
  `FUN_0042fb68` limiter (1 = frame is drawn) — saturated at 999 —
  the SAME `0x540d0c` the reticle uses as its fire
  latch (OBSERVED shared; the refill re-arms the latch each frame).
- `FUN_00437660`: `54161b` blends up `f4*8.0` to 3.0 during a weapon
  switch (then `wpnSel0` adopts + `54161a` resets), else decays
  `f4*4.0` floored at 0 (the fire cadence the sniper gate reads).
  Skipped while `0x4999d0 && 0x541548`.
- `FUN_00436ea8 -> FUN_00431300`: the anim subset (sec. 106).

## 109. Native implementation

- `src/core/motion_channels.h` — `accelChannel`/`decelChannel`/
  `directChannel`/`linearDecay` single-sourced for motion, sniper and
  reticle.
- `src/core/player_sniper.{h,cpp}` — `sniperScopePhaseAdvance`,
  `sniperCoreUpdate` (FUN_00464624), `sniperZoomUpdate` (FUN_00464b50),
  `sniperReset` (FUN_00461878), `sniperDamageDrain` (FUN_00467a00),
  `sniperFireSeam` (FUN_0045f138), `sniperDyingSurface` (FUN_0041342c),
  `playerAnimAdvance` (FUN_00461954 subset).
- `src/core/player_reticle.{h,cpp}` — `playerReticleMountScan`
  (FUN_00463608 + class entries), `playerReticleDispatchMounted`
  (class dispatch), `playerReticleUpdate` (FUN_004691c4).
- `integratePlayerGravity`/`playerVerticalApplyCollision` extracted in
  `player_vertical` so the sniper runs gravity without the jump
  machine.
- `stepTraversalRuntime` wires mounted > sniper > normal, the scope
  phase at the head, the normal-path tail (entry + mount-scan), and
  the world-tick internals.

## 110. Validation

`tests/native/test_main.cpp::test_player_sniper` — floor-arena golden
checks: entry latency (`c9c`/`cac 0x323`/`cbc 8`/channels cleared),
scope-in camera pin (`pullback 0`, `eyeHeight 4.0`), `ca0` 1->2->3
(latch request/commit, `zoom -> 1.0`), raw-mouse aim (`dx*0.12*f0*zoom
*0.41667`), manual unscope (camera restore, `cac 0x384 -> 0x65`), and
the X_STRIKE mount (`e70 0x40031`, reticle `300/180`, `ea0 10`,
overhead settle `-25/s`, pos/yaw pinned, semi-auto latch, recharge).

## 111. Phase 5L boundary / remaining unknowns

- Full `FUN_00461954` frame-table machine + the non-sniper anim
  handlers — only the `0x323`/`0x384`/`cb0` latch subset is ported.
- `FUN_00467ac4`/`FUN_00467ed0` (XD/XSNOWB per-frame updates),
  `FUN_0046603c` slide helper, `FUN_00469cd0`/`FUN_00469b98` weapon
  seams, `FUN_0045f138` projectile spawn, the reticle spawn/unproject
  internals — counted seams, not ported.
- `0x540ccc..0x540cd4`, `0x540e94/e98` scope channels — written on the
  request, consumers UNKNOWN.
- Whether `0x5414d4` (hudActive) is cleared during the mounted
  reticle in the original — UNKNOWN; the shared `d0c` refill means the
  reticle auto-fires while it stays up and is semi-auto while down.
- `0x540eb0`/`0x540eb4` event-timer gate (`c9c != 0 || !liveTimerObj`)
  — the `eb0 = 0` clear is gated in the original; the native countdown
  model predates Phase 5L and is left unchanged.

# Phase 5M — Camera Obstruction and Camera Nudge

Phase 5M closes the two normal-camera seams Phase 5K left open:
`FUN_00430bf8` (the obstruction/displacement call inside the pose
tail) and `FUN_0042b0c0` (the bracketed render-pass nudge). Both are
fully reconstructed from OBSERVED disassembly; no spring-arm or
generic camera-collision abstraction is introduced.

## 112. `FUN_00430bf8` — signature, gate, call site (OBSERVED)

- Signature: `FUN_00430bf8(EAX)` — `EAX = &0x540b28` (the camera
  position inside the 212-byte camera block). All other inputs are
  globals: player pos `0x540bfc`, arenas `0x540c48`/`0x540ca4`,
  carrier gate `0x540d3c`, object gate `0x540c68`, contact token
  `0x540e4c`.
- Call gate (proven in Phase 5K): `0x49b710 != 0 && |0x540d58| == 0`,
  fired after the basis rows exist and before the M1/M2 commit.
- `0x49b710` defaults to **1** in the BUILD_A image — obstruction is
  ON by default. The only writer found is the cheat-string table
  handler `FUN_00423ca0` (also sets it to 1); the earlier
  "default-off" note was wrong.
- `0x540d58` gate: semantic-look offset — while look is active the
  whole call is skipped (no alternate obstruction path).

## 113. Static query — `FUN_00407fc0` (OBSERVED)

- `eye = playerPos + (0, 0, 5.5)` (f64 const `0x4972a8`), segment
  `eye -> camPos`, box extents `0x49b780 = {0.1, 0.1, 0.1}`,
  `flag = 0` (single contact pass — no slide iterations),
  `scale = 0`, callback 0. This is the same swept-box-vs-BSP query
  as the player move — a swept AABB, not a raycast.
- On a primary-arena miss the query is retried against
  `0x540ca4` (the carrier/partner arena) when `0x540ca4 != 0 &&
  0x540d3c == 0`.
- `FUN_00408254` returns the sweep's hit BSP node (`0x4a20c8`) —
  the displacement derives from that node's split plane, not the
  poly.

## 114. Displacement + grounding probe (OBSERVED)

On a hit:

1. `dist = |hitPos.xy - camPos.xy|` (FUN_004301bc — 2D XY Euclidean
   distance, f64 sqrt, f32 store).
2. `nx, ny` = the hit node's plane XY. The eye is re-derived from the
   live `0x540bfc` (+5.5) and its plane distance `pd` is folded as
   `(x*nx + z*nz) + (y*ny + d)`; `pd < 0` flips `nx, ny` (the
   original XORs the float sign bytes).
3. `dx = dist * nx`, `dy = dist * ny` (f64 products, f32 stores).
4. If `0x540e4c != 0` (the last locomotion contact token — written
   only by `FUN_00467180`/`FUN_00467ed0`, never by the camera's own
   applies), the displacement is gated by `FUN_00418c60`: a vertical
   stab from `pos + (dx, dy, +4.0)` to `pos + (dx, dy, -4.0)`
   (`0x4972b0`/`0x4972b4`) against the PRIMARY arena `0x540c48` —
   even after a carrier hit.
5. Retry 1 (`h = dist * 0.5`, f64 const `0x4972b8`):
   `dx = dx0 + h*ny`, `dy = dy0 - h*nx`. Retry 2:
   `dx = dx0 - h*ny`, `dy = dy0 + h*nx`. Three stabs total.
6. All three missing -> `JMP 0x430e8e`: the function returns with NO
   apply AND no object pass — the object pass is skipped too.
7. Surviving displacement: `FUN_004630d4(dx, dy, 0, 0.75, 0, 0)` —
   the PLAYER is moved through the full collision apply (scale
   `0x3f400000`); the camera then follows the APPLIED player delta:
   `camPos += (0x540bfc - snapshot)`. The snapshot is taken
   immediately before the call — the moved player position becomes
   the frame's committed position (the `entryPos` commit already ran
   earlier in the tail, so obstruction does not leak into prevPos).

## 115. `FUN_00418c60` — the grounding stab (OBSERVED, ported)

Bounded helper chain: `FUN_00418c60` (verts-null bail, `mode = 0`,
copies the crossing point to the caller on hit) -> `FUN_00418a50`
recursive BSP walk -> `FUN_004189c8` (crossing-point interpolator:
`t = -dStart / ((cand2-cand1).plane)`, `t = 1` when the denominator
is 0) -> `FUN_00418930` (poly-set scan; skips `flags & 0x20`;
`FUN_00425600` point-in-triangle — the dominant-axis ray-cast already
ported as `pointInTri`).

Walk semantics (byte-verified): at each node compute
`dStart/dEnd = (ny*p.y + nx*p.x) + nz*p.z + d`; descend the side
containing cand1 first (`dStart < 0 -> childFar`, else `childNear`);
on a strict crossing (`dStart*dEnd < 0`) interpolate the crossing and
scan `polysPos` then `polysNeg`; on a double miss iterate the
OPPOSITE side (`dStart >= 0 -> childFar`, `dStart < 0 -> childNear`)
and loop.

## 116. Dynamic-object pass (OBSERVED, `0x430db2..0x430e8b`)

- Gate: `0x540c68 != 0` — the arena object-data validity flag
  (`CollisionState::arenaValid`; the same gate `collisionApply` uses
  for its object pass).
- `eye` rebuilt from live `0x540bfc + 5.5`; `target` starts at the
  live `0x540b28` camPos (post-static-displacement when one applied).
- Object list: `arena + 0x68`. Per object:
  `named(+0x6) != 0 && model(+0x8) != 0 && !(flags148 & 0x810) &&
   (flags14b & 0x01) != 0`.
- Prefilter `FUN_0045cd38(eye, target, obj+0x198, ext=0.1)` — the
  non-strict per-axis segment/AABB overlap.
- Element scan: `obj+0xc` -> `{count @ +0x1c, elems @ +0x20}`,
  stride `0x5c`, element AABB at `+0x44`. Per element
  `FUN_0045c838(eye, target, elemAABB, clamp, outAlt=0)` — on a
  face-clamp (`rc == 1`) `target` adopts the clamp point; `rc == 2`
  is ignored here (unlike the player apply, which prefers `altPt`).
- Tail — runs UNCONDITIONALLY when the gate passed:
  `FUN_004630d4(target.xy - camPos.xy, 0, 0.75, 0, 0)`; the camera
  follows the applied player delta (same snapshot pattern). A zero
  delta still runs the apply.

## 117. Position ownership + ordering (OBSERVED)

- `0x540bfc` (player pos) and `0x540b28` (camPos) are the only
  written endpoints; `entryPos` committed BEFORE the camera block
  (Phase 5K order), so the obstruction push does not enter prevPos.
- The M1/M2 commit runs AFTER the call, so both matrices fold the
  displaced camPos; the eye->camPos portal tail
  (`FUN_00435178`) then consumes the displaced camPos — obstruction
  feeds the `0x49b714` view-on-partner select.

## 118. `FUN_0042b0c0` — camera nudge (OBSERVED, byte-verified)

`FUN_0042b0c0(EAX arg)`:

1. `camPos += M2row0 * (arg * 0x49b570)` — `0x49b570 = 0.25f`, M2
   row0 = the unscaled `right*|right|` basis row. NO writer for
   `0x49b570` was found (image const).
2. `0x49b578 = FRNDINT(arg * 0x49b574)` under the trunc control word
   (`FUN_0047d59a`) — toward zero, not round-nearest.
   `0x49b574` is written by `FUN_0042b20c` (init 20.0).
3. `r = tick * 0x496e0c` (`= 1/600` f32); `M1row0 += M1row2 * r`
   — including `M1[0][2] += M1[2][2]*r` (the `dc cb` = `FMUL ST3,ST0`
   byte pair at `0x42b163` + `FXCH ST3` confirmed symmetric).
4. All three M1 translations refolded against the UPDATED row0 and
   camPos: `t_r = -(row[1]*cy + row[0]*cx + row[2]*cz)`, f32 stores.
   M2 and `camPos` itself are NOT re-refolded; row1/row2 rotations
   are untouched.

## 119. Nudge callers + `FUN_0042b20c` (OBSERVED)

- `FUN_0042b20c(mode)` writes `0x49b574`: `{0 -> 12.0, 2 -> 15.0,
  else -> 20.0}` (jump table `0x42b1fc`); called from level init
  `FUN_0040ef28` and the `FUN_00436100` world-tick head.
- `FUN_0042b060`/`FUN_0042b090` save/restore the whole 212-byte
  camera block `0x540b28..0x540bdb` — the nudge is a bracketed
  per-pass perturbation; `0x49b578` sits OUTSIDE the saved range, so
  `nudgeTick` is the bracket's only persistent footprint.
- World-tick site (`0x436491`, inside `FUN_00436100`):
  `flag541548 == 0` -> `FUN_00436d60(0)` (body only, NO nudge);
  `flag541548 != 0` -> `FUN_0042b20c(c9c && ca0>1 ? 1 : 0)` then
  `FUN_00436d60(-1)` AND `FUN_00436d60(+1)` — the shared body runs
  TWICE per frame, once inside each call's save/nudge/restore
  bracket. Each `36d60` tails with `FUN_0042b248(arg)` when
  `flag541548` (re-writes the same `nudgeTick` — no delta).
- `0x541548` is BSS and has **zero writers** in BUILD_A (all 27
  xrefs are reads) — a dead second-viewport/stereo path in this
  build; the nudge is unreachable in normal play.
- `FUN_0047c504` contains the same `nudge(-1) -> render ->
  nudge(+1)` bracket on its second-pass path — same pattern, same
  inert-in-BUILD_A flag family.
- Nudge vs shake: `0x540ce4/cf8/cfc` is the Phase 5K pose shake
  (added pre-basis); `FUN_0042b0c0` is a post-pose M2-row0
  translation + M1 row0 tilt driven by `0x49b578`'s int accumulator —
  a separate mechanism (viewport wobble for the dual-viewport path),
  kept separate.

## 120. Native implementation

- `applyCameraObstruction(env, st)` — the `FUN_00430bf8` core; reads
  `env.collision` (`CollisionState` = `0x540bfc`/`c48`/`ca4`/`d3c`/
  `c68`) and `env.contactToken` (`0x540e4c`); syncs `env.playerPos`
  on exit. Called from `updatePlayerCamera` at the original seam
  point (post-basis, pre-M1-commit).
- `collisionStab(arena, from, to, outPos)` — the `FUN_00418c60`
  mode-0 chain (exported from `collision_query.cpp` alongside
  `collisionSegAabbOverlap`/`collisionSegAabbResolve`).
- `cameraNudge(arg, st)` — `FUN_0042b0c0`;
  `cameraNudgeApplyMode(st, mode)` — `FUN_0042b20c`.
- `PlayerCameraState`: `obstructionEnabled` (`0x49b710`, default
  true per the image), `nudgeShift`/`nudgeScale`/`nudgeTick`
  (`0x49b570/574/578`).
- `stepTraversalRuntime`: `ce.collision = &rt.cs`,
  `ce.contactToken = rt.lastContactPoly` (the locomotion apply's
  `0x540e4c`); the `0x436491` site mirrors the original — the shared
  body runs once via `36d60(0)` when `flag541548 == 0`, twice inside
  the `-1`/`+1` brackets when set.

## 121. Validation

- `test_camera_obstruction` + `test_camera_nudge`: 40 checks —
  miss, static hit (+4.1 player push, camera follows to the box
  margin, M1 refolds the displaced pos), gates (b710 off, look
  active), carrier retry + `d3c` suppression, grounding-probe retry2
  (+2.05 perpendicular), all-probes-miss early return (object pass
  skipped too), object AABB clamp (+3 push, camera to x=-5), object
  flag filters (`0x810`, `flags14b bit0`), nudge pos/tick/row0/
  refold/truncation/mode-map.
- `mdk-inspect --selftest-camera-obstruction`: 21 checks — PASS.
- `mdk_tests`: 3671 checks / 0 failures; CTest 1/1; Python 17/17.
- LEVEL3-8 `--traversal-runtime`: all PASS; `camcol` now counts the
  seam firing each frame (was a bare count before).
- `CHMO_2 -> HMO_3`: `portals=1`, `teleport=0`, door
  `0x18->0x12->0x11`, `flags148 0x8000->0x8010` — unchanged; the
  deterministic digest `8ecf2d101a3b5c70` (400f) is stable across
  runs AND bit-identical to the Phase 5L baseline — the seam fires
  every frame (`camcol=400`) but the corridor camera never clips
  geometry, so no displacement is produced on this route.
- BUILD_A manifest: 141/141, missing 0, added 0, changed 0.

## 122. Phase 5M boundary / remaining unknowns

- `0x541548` producer — UNKNOWN (no writer in BUILD_A; likely a
  stereo/second-viewport config written via a computed pointer or an
  absent subsystem). The dual `36d60` bracket is dead code here.
- `FUN_0047c504`'s second-pass nudge bracket — decoded shape only;
  its caller/gating is a render-path seam.
- `0x540c9c`/`0x540ca0` mode fields — consumed here as the
  `b20c(1)`/`b20c(0)` selector; their producers stay with the sniper
  subsystem.
- Whether retail DOS builds wire `0x541548` — UNKNOWN (BUILD_A only).
- Projectile/weapon systems remain deferred per scope.

# Phase 5N — Player Weapon Fire

Phase 5N reconstructs the bounded player weapon-fire transaction —
the whole path from fire input to the shot/projectile creation
boundary:

    fire input → eligibility → selected weapon → cadence/burst/ammo
    → muzzle/aim → original weapon dispatch → shot/projectile
    creation boundary

It replaces the Phase 5L `sniperFireSeam` placeholder at the
`FUN_0045f138` callsite with the real dispatch, and wires the
scoped weapon selector, the normal-path fire latch, and the
normal-mode punch hitscan into the world tick. Hitscan weapons keep
their original hitscan boundary — nothing is forced into a
projectile object.

Everything below is OBSERVED from instruction-level BUILD_A
disassembly + decompile of `MDK95.EXE` unless tagged otherwise.

## 123. Functions reconstructed (OBSERVED)

- `FUN_0045f138` — the player weapon-fire dispatch. Weapon-5 branch
  (charge-gated thrown object) plus the weapons-0..4 three-slot
  shot-pool spawn. `playerFireDispatch`.
- `FUN_00432f84` — the normal-mode punch hitscan. `playerPunch`.
- `FUN_004337ac` — the punch cone/target-selection test.
  `punchConeTest`.
- `FUN_0045c230` — the AABB segment clipper (Liang–Barsky entry
  clip). `segClipAabb`.
- `FUN_0045f634` — the element-name predicate (name starts with
  `prefix` AND `name[digitOfs]` is a digit). `elemNamePredicate`.
- `FUN_0047d5dc` — element-name `"HEAD"` substring test.
- `FUN_00469b98` — the hotkey/itemNext scoped weapon selector.
  `playerWeaponSelect`.
- `FUN_00437660` — the scoped cadence/burst/ammo machine.
  `playerWeaponCadence`.
- `FUN_00465228` tail — the normal-path fire latch (event posts).
  `playerFireLatch`.
- `FUN_00437f30` — bearing `norm360(deg(atan2(dy,dx)))`.
- `FUN_00437f98` — sincos-deg helper (`arg2 = sin`, `arg3 = cos`).
- `FUN_00430160` — dist3. `FUN_0047f357` — the FPATAN wrapper.
- `FUN_0047d20a` — shot-slot init (memset `0xfc`); `FUN_00454794`
  pool reference (three `0xfc`-stride slots at `0x540ed4`).

## 124. Weapon dispatch — `FUN_0045f138` (OBSERVED, ported)

- Weapon select `0x541618` (`rt.wpnSel0`) is read once at the head.
- **Weapon 5** (`wpnSel0 == 5`): denied when `0x540e14 == 0` (charge
  probe) or `0x54163b != 0` (fire latch) → no-fire seam. Else
  `0x54163b = 1` when `0x541498 > 3`, `burstIndex -= 1`,
  `fireCadence += 1.0`, `0x541633 -= 1`, `fireCadence = 3.0` if
  `burstIndex == 0`, then the weapon-5 spawn seam `FUN_0045a4dc`.
  `0x540e80` (`shotSerial`) is **not** incremented on this path.
- **Weapons 0..4**: scan the 3-slot pool for `state == 0`; return
  silently if all busy. `0x540d0c = 0`, fire-sound seam, slot memset,
  `state = 1`, `classIdx = -1`, `pos ← 0x540b28` (camera pos),
  `yawDeg ← 0x540c2c`, `pitchDeg ← 0x540b54 + 0x540d58`,
  `arena ← 0x540c48`, `fieldCc = 2.0`. Then the per-weapon
  lifetime/type/flyKind/speed/ammo table, the shared cadence/burst
  tail (`fireCadence += 1.0`, `burstIndex -= 1`, `= 3.0` at 0), and
  the homing tail.
- Per-weapon fields (OBSERVED): w0 `life=0x4b type=0 tracer
  speedH=1100`; w1 `life=0xf0 type=1 grenade ammo-1 speedH=400`;
  w2 `life=0x4b type=2 tracer ammo-1 speedH=1100`; w3 `life=0xf0
  type=3 grenade ammo-1 speedH=400`; w4 `life=0x1c2 type=4 lobbed
  ammo-1`, `speedH = cos(pitch)·150` / `speedV = sin(pitch)·-150`
  (`0x4984e8`/`0x4984ec`). The class index for w1..w4 resolves
  through the enemy table (`SW_HOME`/`SW_SGREN`/`SW_HGREN`/`SW_LGREN`).
- `shotSerial` (`0x540e80 += 1`) fires only on the weapons-0..4
  exits (no-target, non-homing-weapon, null-target, and the shared
  tail) — never on weapon 5.

## 125. Homing tail (OBSERVED, ported — weapons 1/3)

- `0x540cc8 != 0` gates a lock target; `homeObj ← 0x540cd8`
  (`focusObj`) for **all** weapons. For non-1/3 weapons the shot
  keeps the target but picks no element.
- Weapons 1/3 walk the target's element set: the first `"HEAD"`
  element wins immediately; other candidates need the target's
  standable bit (`flags149 & 0x20`) and the prefix/digit predicate
  (`0x302` prefix, `0x306` digit offset).
- The ray runs `shot.pos → shot.pos − camera.basis[2]·10000`
  (`basis[2]` = `0x540bd0`, M2 row-2). `FUN_0045c230` clips it to
  each candidate element AABB; the score is `|clippedHit|` (distance
  from world origin), and the nearest valid hit wins. The shot
  stores the element pointer and index.

## 126. `FUN_0045c230` — the AABB segment clipper (OBSERVED, ported)

- `__fastcall`: `ECX = param_1` = primary out point, `EDX =
  param_2` = segment end, `EAX` = segment start, `EBX` = AABB, one
  stack arg `param_3` = optional secondary out (`0` at the homing
  callsite → its `MOV [EDX],…` writes are skipped).
- The entry point is computed into callee locals then copied to
  `*param_1` via `MOVSD ×3`. `param_3` is a separate optional out —
  not the flag bitmask.
- `flags` (`local_10`) is the outside-sides bitmask: X `1`/`2`,
  Y `4`/`8`, Z `0x10`/`0x20`.
- OBSERVED asymmetry: the X faces compare `t < 0x4982b0` (constant
  `1.1`) while the Y/Z faces compare `t < tEnter` (the running best,
  init `1.1`). Equivalent because X is always tested first while
  `tEnter` is still `1.1` — reproduced verbatim (`kClipInit` for X,
  `tEnter` for Y/Z).
- Return: `flags != 0` → `tEnter > 1 ? 0 : 1`; `flags == 0` (start
  inside) → `out = start`, return `2`.

## 127. Punch — `FUN_00432f84` (OBSERVED, ported)

- Gates: `0x540c74 != 0`, `excludeObj`/`mountClass & 2` exclusion.
- `aimPt = {pos.x, pos.y, pos.z + 5}` (`0x4973b0`).
- Charge drain: `ammo[0] <= 0` → `punchStep = frameStep`, state `-1`;
  else `ammo[0] -= frameStep`, `punchStep = frameStep·6`, state `-2`.
- Reload path (`ammo[0]` reaches 0): `ammo[0] = 0` and the
  `FUN_00469668(1)` notify are **unconditional**; `FUN_0046a3d8`
  (item reload) runs only when a type-6 inventory entry exists —
  the inventory table is not modelled, so it stays a seam and does
  **not** bump `itemUseCalls` (that counter is the separate
  `FUN_00459d28` item-use input path).
- Object scan iterates the current arena `0x540c48`, then the
  partner/carrier `0x540ca4` when `partnerActive` and
  `carrierBusy == 0`. Filters: named, has model, `flags148 & 0x10`
  and `& 0x20` clear.
- Element scan on `flags149 & 0x20` objects, `elemMaskB` exclusion,
  element-name predicate, whole-object AABB fallback when no element
  wins. `CollisionObject` offsets verified: `aabb +0x198`,
  `elemMaskB +0x2c8`, `elements +0x0c` (stride `0x5c`, element
  `aabb +0x44`), `objects +0x68`.
- Cone test (`FUN_004337ac`): `d = center − aimPt`,
  `dist1 = diag < 10 ? 10 : diag` (`0x4973cc`), reach
  `dist2 <= dist1 + 140` (`0x4973d0`), `score = dx² + dy² + 4·dz²`
  (`0x4973e0`), `cone = (dist1−2)·90 / (dist1−2+dist2)`
  (`0x4973d4`/`0x4973d8`, OBSERVED `fdivrp` order), accept
  `rel <= cone || rel >= 360 − cone`.
  `bestScore = −1.0` sentinel accepts the first candidate
  (nearest-tracking, `score <= best || best < 0`). Occlusion stab
  on `cur`, partner gated on `carrier != 0 && carrierBusy == 0`.
  (Phase 10A re-verified these constants instruction-level; the
  values listed in the Phase 5N draft of this section were wrong.)
- Hit: `field21e = 0xff`, `punchHitTime += frameStep`; the full
  damage/knockback/kill tail is ported in Phase 10A (§133+).
  Miss stab: `missPt = {pos.x + cos(yaw)·150, pos.y + sin(yaw)·150,
  aimPt.z}` (`0x4973c0`), current arena then partner (gated
  `carrier != 0 && carrierBusy == 0`).

## 128. Selector + cadence + fire latch (OBSERVED, ported)

- `FUN_00469b98` (`playerWeaponSelect`): scoped hotkey scan —
  hotkey `i` selects weapon `i` when `ammo[i] != 0` (JNZ);
  `itemNext`/`itemPrev` wrap-scan with `ammo > 0` (JG). Wired at the
  `weaponScanCalls` site.
- `FUN_00437660` (`playerWeaponCadence`): the scoped cadence/burst
  machine — burst-recharge gated on `wpnSel1 == 0 ||
  burstIndex < ammo[wpnSel1]`, `burstIndex == 0 → wpn0` reload,
  skipped when `flag4999d0 && flag541548`.
- `FUN_00465228` tail (`playerFireLatch`): normal-path fire latch —
  gate `fire && eventPriority <= 7 && eventType <= 7`, busy set
  `{0x2bd, 0x2bc, 0x190, 0x1f4, 0x2bf, 0x2be}`, anim posts
  `0x258 → 0x259/0x256` else `0x12c/0x123`.

## 129. World-tick scope correction (OBSERVED)

- `FUN_00436d60` — both scope gates are `0x540c9c && 0x540ca0 > 1`
  (not `ca0 != 0`). `d0c++` (`FUN_00436f08`) and the cadence machine
  sit inside gate B; `FUN_0045f030(0)` + the charge probe
  `FUN_00437aa8` sit inside gate A. `FUN_00469f7c` (inventory-icon
  HUD) is unconditional and stays outside fire scope.

## 130. Native implementation

- `src/core/player_fire.cpp` / `.h` — `playerFireDispatch`,
  `playerPunch`, `playerWeaponSelect`, `playerWeaponCadence`,
  `playerFireLatch`, `segClipAabb`, `punchConeTest`,
  `elemNamePredicate`. `src/core/player_sniper.cpp` calls
  `playerFireDispatch` at the `FUN_0045f138` callsite;
  `traversal_runtime.cpp` wires the selector, the normal fire latch,
  the punch callsite, and the corrected `36d60` scope gates.
- `TraversalRuntime` gains the `PlayerShot` pool (3 × `0xfc`-stride
  slots), the cadence/ammo/weapon/target fields, and the seam
  counters.

## 131. Validation

- `tests/native/test_main.cpp::test_player_fire`: 40 checks —
  weapon-5 deny/latch/cadence, weapons-0..4 pool scan + spawn fields,
  per-weapon lifetime/type/speed/ammo, `shotSerial` on 0..4 vs not
  on 5, homing HEAD/predicate/nearest-element selection, clipper
  entry/inside/miss returns, punch gates/cone/hit/miss arenas.
- `mdk_tests`: **3711 checks / 0 failures**; CTest **1/1**; Python
  **17/17**.
- `mdk-inspect --selftest*` — all PASS.
- LEVEL3–8 `--traversal-runtime` (400f each): all PASS — real
  contact+grounded, `teleport=0`, no crash. `CHMO_2 → HMO_3` digest
  `8ecf2d101a3b5c70` (400f) **bit-identical** to the Phase 5L/5M
  baseline — the fire path does not perturb the corridor route.
- BUILD_A manifest: **141/141, missing 0, added 0, changed 0** —
  `original/installed/` untouched; no proprietary/analysis files
  tracked.

## 132. Phase 5N boundary / remaining unknowns

- Deliberately out of scope: projectile world flight beyond the
  creation boundary, enemy damage/death/AI, explosions, HUD weapon
  UI, and exotic weapons not required by the dispatch. These stay
  seams (`shotSpawnCalls`, `weapon5SpawnCalls`, `fireSoundCalls`,
  `fireDenyCalls`, `fireNotifyCalls`, `classLookupCalls`,
  `punchHitCalls`, `itemUseCalls`) or unimplemented code.
- `FUN_0046a3d8` type-6 inventory reload — the inventory table is
  not modelled; only the unconditional `ammo[0] = 0` +
  `FUN_00469668(1)` notify are ported.
- `FUN_0045f030` shot render, `FUN_0045a4dc` weapon-5 spawn,
  `FUN_004022b8`/`FUN_00402388` fire/deny sounds, `FUN_00469668`
  notify — counted seams, not ported.
- The fire transaction is a **state/mutation reconstruction**, not
  a rendered projectile sim — shots enter the pool with correct
  fields but nothing integrates their motion or impact yet.
  (Projectile world flight, impact, splash damage, and the
  damage/death boundary are ported in Phase 10A — §133+.)

## 133. Shot pool + per-slot update — `FUN_0045f9b8` (OBSERVED, ported)

- Pool: three `0xfc`-stride `PlayerShot` records at `0x540ed4`.
  Layout: `+0x00 state` (0 free / 1 flight / 2 kill / 3
  hit-survived / 4 wall|expire|killz / 5 detonated),
  `+0x04 yawDeg +0x08 pitchDeg +0x0c spinDeg`, `+0x10 lifetime`
  and `+0x14 dyingTimer` in `frameStep` units, `+0x18 arena`,
  `+0x1c classIdx` (`0x4edd48` default record, native `−1`),
  `+0x20 pos`, `+0xbc tailLen`, `+0xc0..c8 tail`,
  `+0xcc fieldCc`, `+0xd0 type`, `+0xd4 flyKind`,
  `+0xd8 homeObj +0xdc homeElem +0xe0 homeElemIdx`,
  `+0xe4 speedH`, `+0xe8 yawAccum`, `+0xf0 speedV` (type 4),
  `+0xf4 remnantIdx`, `+0xf8 flags` (bit0 = ribbon-bound).
- Head: `state > 1 && +0x14 > 0` → dying branch —
  `+0x10 > 0`: `+0x10 -= frameStep`; type ∉ {4,2,3}:
  `tailLen -= 5·dt` + tail rebuild (NO clamp on shrink).
  `+0x14 -= frameStep`; `<= 0` → `state = 0` release. Return.
  A stale `state > 1` shot with `+0x14 <= 0` **falls through to
  flight** — the stale-state quirk is preserved.
- The pool tick sits at the **tail of every `FUN_004572ac`
  arena update** — once per arena invocation, so the same pool
  ticks twice per frame while a partner arena is active.
- Timing split: `+0x10`/`+0x14`/tail-shrink use integer
  `frameStep`; all flight integration uses the fixed `1/30 s`
  tick constant (`0x49b6f4`, never written — OBSERVED);
  the ribbon parameter uses smoothed frame units (`0x49b6f0`).

## 134. Flight callbacks (OBSERVED, ported)

- Tracer `FUN_004601d4` (types 0/1): `pos += sH·dt·dir`,
  `dir = {cosY·cosP, sinY·cosP, −sinP}`; `tailLen += 10·dt`
  cap 10; `fieldCc −= 0.5·dt` floor 1.0; tail rebuild.
- Homing `FUN_004602d8`/`FUN_00460424` (types 2/3): while
  `homeObj && +0x10 <= 233`: `named == 0 → homeObj = 0`;
  element masked in `+0x2c8` bit `homeElemIdx → homeElem = 0`;
  target = element or object AABB centre (`(min+max)·0.5`).
  Steering: `yawErr = norm180(bearing − yaw)`; accumulator
  `+0xe8` resets on reversal/zero, `±540·dt` rate, `±270` cap;
  `yaw += clamp(e8·dt, err)`; `pitchErr = norm180(360 −
  bearing(dz, hDist) − pitch)`; `pitch += clamp(err, ±120·dt)`;
  `hDist == 0 → 1` substitution. Speed target `err <= 35 → 250`
  else 100, eased `−500·dt`/`+200·dt`; then the tracer step.
- Lobbed `FUN_004608bc` (type 4): `t = |sV| + sH`; `t == 0 → 1`;
  `sH -= t·60·dt` floor 0; `sV > 0 → sV -= (1−t)·60·dt` floor 0;
  `sV -= 32·dt` floor −220. Volume query
  (`surfaceVolumeQuery`, mask 4, vel `{0,0,sV}`) on cur then
  partner (`ca4 && !d3c`): hit → `sH *= 0.25`, `sV = outVec.z`.
  `pos += {sH·dt·cosY, sH·dt·sinY, sV·dt}`; tail fields as tracer.
- Ribbon `FUN_0046075c` (`+0xf8 & 1`): `+0xe4 += smoothed`;
  cubic Hermite over `0x28`-stride keys `{frame, pos, tanIn,
  tanOut.xy}` (`FUN_00456bc8`, standard
  `p0 + m0·u + A·u² + B·u³`); `pos = eval(u)`; `+0x10 = 99`
  while `u < lastKey − 1` else 0; tail extends away from the
  player, `z += tailLen·0.25`; `hDist == 0 → 1`. Ribbon-bound
  shots **skip collision and the object scan entirely**.
- Binder `FUN_00460860`: `flyKind = ribbon`, `+0xe0 = pathRec`,
  `+0xe4 = 0`, `+0xf8 |= 1` (`+0xe0`/`+0xe4` alias
  `homeElemIdx`/`speedH` while bound).

## 135. Impact + collision dispatch (OBSERVED, ported)

- Object scan (non-ribbon): cur arena objects, then partner
  gated `ca8 && ca4 && !d3c`; gates `named && model &&
  !(flags148 & (0x10|0x20))` — **no `excludeObj` gate**
  (OBSERVED). `segAabbOverlap(prev, pos, aabb, ext=0)` then
  `objectProbe` with `end = shot.pos` writeback (nearest wins
  by shortening); `hitElem >= 0` records `hitObj`/`hitElem`/
  `hitTri`.
- Type 4: `collisionSweep` (ext `{0.5}³`, flag 0, cb 0) cur
  then partner (`ca4 && !d3c`). On poly: `0x49b8e4 = shot`;
  `surfaceDispatch(ch1, sec 0, {ev=type, vecA=hitPt, posB=pos,
  contactPos=prevPos})`; `ret & 2 → restH/restV = 1.05/1.25`
  else 1.75. Bounce: `vel −= n·(vel·n)·rest` (z uses `restV`),
  `pos = hitPt`, `speedV = vel.z` (`0 < v < 4 → 0`),
  `speedH = |vel.xy|` (`sH > 0 && != 0 → yaw = bearing(vel)`),
  settle `+0x10 > 15 && sH < 0.5 && sV < 1 → +0x10 = 15`.
- Types 0–3: `collisionStab` (cur then partner). On node:
  `hitPt += n·sign(prevPos·n + d)·1.0`; `FUN_00460164` →
  `surfaceDispatch(ch1, sec = type 0/1 ? 8 : 0)`; types 0/1 →
  state 4 (`+0x10 = 0, +0x14 = 30, +0xf4 = 0`); types 2/3 →
  `pos = hitPt`; `detonate(150, r 25, direct 0)`.
- Shared tail: `+0x10 -= frameStep`; `type4 && +0x10 <= 0 →
  detonate(150, r 50, hitObj)`; `pos.z < arena+0x44e
  (deepFloorZ) || +0x10 <= 0` → state 4. Object-hit dispatch:
  type ≥ 2 → `detonate(150, r = type4 ? 50 : 25, hitObj)` then
  marks `+0x21e <= 0 → +0x21e = +0x21c = elem+1, +0x21d = type;
  +0x210.. = pos; +0x220 = tri; +0x224 = yaw; +0x228 = pitch`.
  Types 0/1: `+0x21e = 0xfe; +0x21d = type; +0x21c = elem+1;
  +0x210.. = pos; +0x220 = tri; +0x224 = yaw; +0x228 = pitch;
  health -= 8 (gate < 0xfde8); 0x540e84++`; survived → fx seam
  + state 3; killed → `killTally` + `death` + state 2;
  `+0x10 = 30; +0x14 = 45; +0xf4 = 0; +0xbc = 15` + tail
  rebuild. No-hit tail: `type != 4 → +0x0c += 720·dt`.

## 136. Detonation + splash — `FUN_00460b7c`/`00460d44`/`00460c08` (OBSERVED, ported)

- `detonate(shot, dmg, radius, directObj)`: `dmg44(flags 6,
  dmg, directObj)` then `dmg44(flags 1, round(dmg·0.5))` →
  remnant + sound seams → `state 5, +0x10 = +0x14 = 30,
  +0xf4 = 0`.
- Falloff `FUN_00460c08`: `dist = dist3(centre, blast)`;
  `effR = 0.5·diag`; occlusion stab cur + partner (`!d3c`)
  forces out-of-range on the same-surface substitution;
  `dmg = round(scale·(range−aux)/range)` clamp ≥ 0 —
  `fdivrp` order and `frndint` (round-half-even) conversion,
  NOT truncation (corrected during Phase 10A verification).
- `dmg44(blast, dmgScale, range, tallyGate, directObj, flags,
  exclMask)`: `flags & 2` objects (cur then partner
  `ca8 && ca4`, NO `d3c`): standable (`f149 & 0x20`) element
  pass (`elemMaskB` skip, name predicate, best-dmg tracking)
  applies **16-bit** `elemHp[e] -= dmg` (wraps before the
  sign test — OBSERVED `sub word`), `<= 0 → hp 0,
  deadElem = e+1 (+0x21c), +0x220 = 0`; `directObj == o` →
  full `dmgScale` at the AABB centre, `aux = 0`; whole-object
  falloff `aux <= +0x2c4` (default 1000.0) gates application;
  marks `+0x21e = bestElem+1 (0xfe init), +0x21d = exclMask,
  +0x210.. = hitPt, +0x228 = 0, +0x224 = bearing`; killed →
  `tallyGate ? killTally : death(obj, hitPt, bearing+180)`.
- `flags & 1` player: `d2 = dist²(playerPos+1z, blast)`; cur
  stab hit → occluded out; `dist = √d2·2`; `dmg =
  round(scale·(range−dist)/range)` cap 15 → `playerDamage`;
  `landingAccum *= 2.0`. Partner stab gated on cur miss.
- `flags & 4` polys (cur then partner `ca8 && ca4`, NO `d3c`):
  surface mask from arena `+0x6c` byte set | `+0x8c` dword set;
  per-poly centroid `≤ range²`; same-surface occlusion →
  partner retry; `dmg = round(falloff)`; `surfaceDispatch`
  `(ch = dmg != 0 ? 3 : 4, sec = dmg, {ev −7, vecA = contact,
  posB = contact, contactPos = blast})`; surface bit cleared
  once fired.

## 137. Damage + death boundary (OBSERVED, ported)

- `playerDamageApply` (`FUN_0046771c`): `health == 0 && gate`
  return; suppress `fieldE10 > 0 || locoState ∈ {0x326, 0x385,
  0x3ea} || fieldEb8 == 1` → `landingAccum = 0`, return.
  `diff 0: dmg = 2d/3 min 1; diff 2: 2d`; `fieldDac += dmg·25`
  clamp `[75, 180]`; mount path (`excludeObj && mountClass & 1`):
  `m->hp -= dmg` (gate `< 0xfde8`), `<= 0` → mount death; else
  `playerHp -= dmg` floor 0, `landingAccum += dmg`.
- `objectKillTally` (`FUN_0042ac90`): `gate != 0 → 0x540e90++`
  (the 34-name class table stays a counted seam).
- `objectDeathBoundary` (`FUN_00458140`): `+0x110 != 0` → script
  handoff `field11e = 0; +0x08 = 0; +0x22c = 0;
  flags148 |= 0x20; +0x108 = +0x230 = +0x110; +0x110 = 0`;
  else `FUN_00457cf4` teardown seam — remnant spawn
  (`FUN_004575fc`), wipe (`FUN_0045828c` memset keeping
  next/arena), global-ref clears (`excludeObj` → clear +
  50 `playerDamage`; `lastObjContact` → clear).
- `objectDieFacingPlayer` (`FUN_004581a4`): `facing =
  bearing(player − obj) + 180` — the `+180` lives in the
  callers/callee, applied once (`hitPt = obj.pos + {0,0,3}`).

## 138. Punch damage/death tail (OBSERVED, ported)

- `FUN_00432f84` now shares the damage/death boundary:
  charged `dmg = frameStep·6`, state `−2`; uncharged
  `frameStep`, `−1`.
- Whole-object hit: `punchHitTime += frameStep`; `hp -= dmg`
  (gate `< 0xfde8`); `+0x21d = state, +0x228 = 0,
  +0x224 = bearing` gated `+0x21e == 0xff`. `hp <= 0` →
  `killTally(obj, charged)` → charged displacement
  `pos += {20·cos(yaw), 20·sin(yaw)}` on `+0x28`/`+0x2c` →
  `death(obj, hitPos, bearing + 180)`.
- Survived: element hits knock back on the **world** element
  AABB (stride 92), whole-object on `col.aabb` —
  `hitPt −= dir·0.5·extent`; `+0x2a2 <= 900` (unsigned word)
  → `eventTimerObj = obj, eventTimer = 1.0`; fx seam.
- Element hit: `+0x21e = 0xff` set at hit-select; **16-bit**
  `elemHp[e] -= dmg` (wrap before sign test, same quirk as
  splash); `<= 0 → hp 0, +0x21d = 0, +0x220 = 0,
  +0x21e = +0x21c = e+1, +0x224 = bearing, +0x228 = 0`, then
  **falls through to whole-object damage** (OBSERVED quirk).
- Element latch `+0x31e <= 900` → pseudo-object `eventLatch`
  (`+0x08` gets post-decrement `elemHp[e]` via `+0x30c>>16`).
- Miss: 150-unit stab (`0x4973c0`), wall → `surfaceDispatch`
  `(ch2, sec = dmg, {ev = state, vecA = missPt, posB = aimPt,
  contactPos = hitPt})`; handler ran → fx seam ch1 count 2.

## 139. Runtime integration + validation (OBSERVED, ported)

- `src/core/player_projectiles.cpp`/`.h` — `playerShotPoolTick`,
  `playerDamageApply`, `objectKillTally`, `objectDeathBoundary`,
  `objectDieFacingPlayer`, `splashDamage`; `player_fire.cpp`
  includes the shared header so the punch calls the same
  boundary.
- `traversal_runtime.cpp` — `playerShotPoolTick` at each
  `FUN_004572ac` arena tail (double-tick with partner);
  `fieldB85c` latch inside the named-object loop on
  `field07 == 1`; per-frame `eventTimer` decay (cleared when
  the latched object loses `named`), `fieldD2c -= frameStep`,
  `fieldE10 -= 1/30`.
- `collision_query` gains `collisionStabFull` (hit polygon out)
  and `collisionObjectProbe` (object-local segment probe with
  `end` writeback); `DynamicObject` gains `field28`/`field2c`
  (corpse displacement), `field150`, `elemThresh` (+0x31e);
  `TraversalArena` gains the `eventLatch` pseudo-object;
  `TraversalRuntime` gains `fieldD2c`/`fieldB85c`.
- `mdk_tests`: **3882 checks / 0 failures** — the
  `test_player_projectiles` suite covers the pool head/dying
  branch, stale-state re-flight, tracer flight fields, lifetime/
  kill-floor state 4, type-4 detonate, wall dispatch, object-hit
  survived/killed marks, player-damage gates/scaling, tally and
  death boundary, the int16 element wrap, splash falloff/occlusion,
  the punch element→whole fallthrough, and the partner double tick.
- Remaining seams: `FUN_0045f030` shot render, `FUN_004575fc`
  remnant spawn, `FUN_00437444`/fx callsites, sound calls,
  the 34-name tally table — counted, not ported.

# Phase 10B — Combat Presentation Semantics

Bounded tail over the Phase 10A seam list. Nothing below mutates
gameplay state on the original; the native side records a
presentation-neutral contract (`PlayerShotVisual` snapshot +
`CombatFxEvent` log) instead of porting the software renderer,
particle pool or sound engine.

## 140. Shot render path — `FUN_0045f030`/`FUN_0045ee7c`/`FUN_0045e9a0` (OBSERVED)

- `FUN_0045f030` snapshots the `0x540b28` camera block (0xd4 bytes)
  to a local, sets render guard `0x540d44 = 1`, calls
  `FUN_0045ee7c` once per pool slot with that slot's HUD-window
  constants, then restores the block and clears the guard.
  Sole caller is the world tick at `0x436dd3` (mode 0) /
  `0x436e33` (mode 1), both gated `flagC9c != 0 &&
  transitionPhase > 1` — the fully scoped sniper state. There is
  NO unscoped shot-render path (OBSERVED; no other xrefs).
- `FUN_0045ee7c(slot, x, y, x2, y2, w, h, mode)` computes a frame
  select: `state == 0` → `0`; `lifetime < 1` → state 4 → `3`,
  state 3 → `0x3c`, other → `0xf4`; active (`lifetime >= 1`) → `-1`.
- Mode 0 (world): active records only — `FUN_0045e9a0` builds the
  billboard, `FUN_0042b0c0` may run when `0x541548` is set (the
  dead dual-viewport path), `FUN_0046ec60` under the `0x5414d4`
  HUD gate, then `FUN_0045ee08` drives animation.
- Mode 1 (HUD): `hudActive && frame != -1` — non-type-4 slots
  `FUN_00416aa8` rect-fill over the per-slot window; type 4 uses
  `FUN_00409760` and advances `+0xf4` by `min(frameStep, 2)` capped
  at `*0x54c66c·2 − 1`.
- `FUN_0045e9a0` billboard (OBSERVED): anchored at the tail
  endpoint `+0xc0..+0xc8` written into `0x540b34..0x540b48`;
  render yaw `0x540b50 = 90 − yawDeg`; render pitch `= pitchDeg`;
  `+0xcc` feeds scalars `0x540b58`/`0x540b64`. Type 4 instead
  derives yaw from the horizontal tail→pos delta
  (`90 − atan2deg(dx, dy)`) and pitch `= −speedV·0.5` clamped
  `[−60, +60]` (doubles `0x4984a0`/`0x4984a8`/`0x4984ac`).
- Per-slot image window written to `0x540b78..` — slot 0
  `{0x48,10,0x8d,0x2c}`, slot 1 `{0xe4,0,0x129,0x22}`, slot 2
  `{0x180,10,0x1c5,0x2c}`; `0x540b68/0x540b6c = {140,70}`.
- HUD tables (OBSERVED data): positions `0x49b900` =
  `{72,10},{228,0},{384,10}`, sizes `0x49b8e8` = `{140,70}` ×3.
- Native: `playerShotRenderGate(rt)` reproduces the
  `flagC9c && transitionPhase > 1` gate; `playerShotVisuals(rt)`
  fills `PlayerShotVisual` per slot (state, type, arena, pos,
  tail endpoint/length, yaw/pitch/spin, `fieldCc`, ribbon flag,
  `worldRenderable`, `hudFrame`); `kShotHudRect` carries the HUD
  rects. No renderer code is ported.

## 141. `FUN_00437444` — impact presentation dispatch (OBSERVED callsites)

Signature `(EAX ctx, EDX &pos, EBX sndName, ECX paletteMode,
stack count)`. Internally: `paletteMode` selects
`{life|color, scale}` — `0 → {0xd or 3 (0x54150c gate), 3, 1.0}`,
`1 → {0x25, 0xf0, 0.5}`, `else → {10, 3, 1.0}`; `sndName` null or
empty-string → random RICO1/2/3 handle (`FUN_00401ed4` bounded
pick → `0x54c5e8/0x54c5ec/0x54c5f0`), else the name is resolved
via `FUN_00402fe8` and played (`FUN_00402288`); `count` particles
spawn through the `FUN_00403f6c`/`FUN_00404108` pool
(presentation-only, depth-evicted).

- Shot wall (`FUN_00460164` tail, `0x4601a8`/`0x4601c3`):
  `surfaceDispatch` bit0 set → `{mode 1, count 2}` else
  `{mode 3, count 1}`; EBX `0`.
- Shot object survived (`FUN_0045ff9d`, `0x460059`):
  `{mode 3, count flag21f}`, EBX `= obj+0x150`, pos `=&obj+0x210`.
- Punch object survived (`0x4334b0`): `{mode 1, count flag21f}`,
  EBX `= obj+0x150`, pos `=&hitPt` (element or whole-object).
- Punch wall (`0x43371f`/`0x43377e` vs `0x43379b`): `{mode 1,
  count 2}` when the surface handler ran else `{mode 1, count 1}`;
  EBX `0`.
- `FUN_0045bec8` water splash (`0x45c068`/`0x45c087`):
  `{mode 1, count 1|0}` — sits on the generic `FUN_004533d4`
  sweep chain, not the shot path; classified, not emitted.
- Native: one `CombatFxEvent` per callsite carrying
  `kind/mode(palette)/variant(count)/aux(sndName)/pos/obj` on
  `rt.combatFx`. `field150` is kept as an int32 marker — the
  pointed-at name data is not ported.

## 142. `FUN_004575fc` — detonation remnant (OBSERVED, gameplay-inert)

- Allocates a real DynamicObject through `FUN_0045cffc` (LRU
  eviction over the arena `+0x68` list; class names `XG`, `XF`,
  `BOLT`, `BIGBOLT` are eviction-protected), sets class record
  `0x4edcc0`, `+0x08 = 0`, `flags148 |= 0x20` (the dead flag that
  excludes it from combat scans), `+0xe4 = 0xffff` (no model
  frame), `+0x60 = arena`, pos + prevPos = the impact point,
  `+0x4c/+0x50` = camera-relative yaw, `+0x13c` = pitch,
  `+0xdc = −1.0`, `+0x58 = param_3` (scale `2.0` at the
  `FUN_00460b7c` callsite `0x460bd8`), then plays `EXPLODE`
  (`0x54c61c`, `FUN_00402160` vol `0x7fff` rate `200.0`).
- Two other callers exist inside VM opcode handlers `0xac`/`0xb2`
  (script-driven effect spawns) — same record family.
- The remnant is a render/update-side corpse marker: `health 0`
  and the `0x20` flag make it invisible to every combat scan —
  presentation-only. Native keeps it as the `kDetonation` event
  plus `seams.remnantSpawnCalls`; no object is spawned.

## 143. Element HP/threshold init — opcode `0xc6` (OBSERVED, ported)

- The tr_alcmd jump table (`jmp [0x438a5c + (op−1)·4]`) maps
  opcode `0xc6` → handler `0x4394d0`. Grammar
  `{str8 prefix → +0x302, u8 digitOfs → +0x306, u32 hpThresh,
  u32 extra → +0x30a}`; side effects `+0x149 |= 0x20` (enables the
  element/homing scan) then fills all eight `int16` slots of
  `+0x31e` AND `+0x30e` with `low16(hpThresh)` — a uniform
  hp = threshold init.
- `0xc7` → `0x45188f` is the actual `+0x104` variable write
  (same operand grammar as `0x53`/`0x54`, inline-f32 mode). The
  previous native mapping `0xc6 → +0x104` was a mislabel — fixed;
  the decoder grammar/name tables follow the same correction.
- The `+0x30e..+0x32d` region is a class-dependent union: `0xc6`
  element pools vs connector ops `0x96`–`0x99` (anim pointers
  `+0x306/+0x30a`, sound-name pointers `+0x316..+0x322`, radius
  `+0x30e`). The two never coexist on one class (OBSERVED).
- Census: 344 table-2 init scripts across LEVEL3–8 contain exactly
  one `0xc6` — `LEVEL3 HMO_1$XH1_DOOR`:
  `XH1_KEY / digitOfs 7 / hp 120 / extra 0` (the object also gets
  the `0xfde8` invulnerable sentinel). Its model carries elements
  `XH1_KEY1`/`XH1_KEY2`; the scan predicate `FUN_0045f634` accepts
  `name == prefix` (NUL-terminated) with `name[digitOfs] ∈ 0..9`,
  so `XH1_KEY1/2` hit and `XH1_DOOR1`/`XH1_KEYX`/`XH1_KEY` do not.
- Consumers confirmed: element damage writes `+0x30e+2e` (int16
  wrap quirk, Phase 10A); the `+0x31e <= 900` latch arms the
  pseudo-object event timer; `+0x302/+0x306` drive both the
  hit-element scan and the homing/meat targeting string compare
  (`FUN_0042fa50` vs `MEAT_3/4/8`).

## 144. Death-script handoff — `+0x108`/`+0x110`/`+0x230` (OBSERVED)

- `+0x108` is the object script PC — the VM fetch cursor
  (dispatch head `0x4389dd`, jump table `0x438a5c`); `+0x22c`
  the wait timer, `+0x230` the resume PC after wait. VM entry
  `FUN_004388d8` selects between them on resume.
- `+0x110` is written by opcode `0x4c` (`image-ref`: stores
  `cmiBase + off`) and populated at spawn by `FUN_00427218` from
  the class template — same script-stream pointer domain as
  `+0x108/+0x230`.
- `FUN_00458140` handoff (already ported): `+0x110 != 0` →
  `+0x108 = +0x230 = +0x110`, `+0x110 = 0`, `field11e = 0`,
  `health = 0`, `+0x22c = 0`, `flags148 |= 0x20`. The `0x20` flag
  excludes the corpse from every combat scan; the record stays
  in its arena list — the deferred script drives the death
  animation/removal itself. Where the handed-off script
  terminates the object is the G5 boundary (UNKNOWN beyond this
  point by scope).
- Native: `kObjectDeathScript` event on the handoff branch,
  `kObjectTeardown` on the `FUN_00457cf4` branch — both carry the
  object pointer and position.

## 145. Combat sound callsites — classification only (OBSERVED names)

`FUN_0043394c` (level sound-table loader inside `FUN_00433d40`)
registers 40 `FUN_00402fe8` name→handle lookups into
`0x54c5d0..0x54c664`, plus two level-specific names from the
`LEVEL-d .SNI` record into `0x54c628/0x54c62c`:

```
0x54c5d0 SNIPERSHOT   0x54c5d4 SNIPERON   0x54c5d8 SNIPEROFF
0x54c5dc BREATH       0x54c5e0 MULTIFIRE  0x54c5e4 GATTFIRE
0x54c5e8 RICO1        0x54c5ec RICO2      0x54c5f0 RICO3
0x54c5f4 ALERT        0x54c5f8 ALDIE      0x54c5fc CHUTEOUT
0x54c600 CHUTEIN      0x54c604 CHUTEON    0x54c608 LAND
0x54c60c FOOT1        0x54c610 FOOT2      0x54c614 FOOT3
0x54c618 FOOT4        0x54c61c EXPLODE    0x54c630 GRUNTFIRE
0x54c634 APPLE        0x54c638 DUMMY      0x54c63c COW
0x54c640 SNIPRELD     0x54c644 RUNNER     0x54c648 FAN
0x54c64c TORNADO      0x54c650 RASPBER    0x54c654 ZOOMBEG
0x54c658 ZOOM         0x54c65c WMIB       0x54c660 BONES
0x54c664 COLLECT
```

Combat usage (OBSERVED callsites): fire per weapon handle via the
`FUN_0045f138` table; fire-deny `RASPBER`; `FUN_00437444` wall
impacts pick RICO1/2/3 (or the `+0x150` per-object override on
object hits); detonation remnant plays `EXPLODE`; `ALDIE` sits on
the teardown path. No audio playback is ported — the `CombatFxEvent`
metadata preserves the handles' call context.

## 146. Native implementation + validation (OBSERVED, ported)

- `traversal_script.cpp`: `0xc6` = element-set declaration,
  `0xc7` = `+0x104` write; init-decoder grammar/name tables
  updated.
- `dynamic_objects.h`: `field30a` added; `elemHp`/`elemThresh`/
  `field104`/`field150`/`+0x108`/`+0x230` comments carry the
  proven provenance/union notes.
- `player_projectiles.h/.cpp`: `PlayerShotVisual`,
  `kShotHudRect`, `playerShotRenderGate`, `playerShotVisuals`,
  `CombatFxKind`/`CombatFxEvent`, event pushes at the
  `FUN_00460164`/`FUN_0045ff9d`/`FUN_00460b7c`/`FUN_00458140`
  seams; `player_fire.cpp` pushes the two punch events.
- `TraversalRuntime::combatFx` — drainable presentation event
  log alongside the existing `seams` counters.
- `mdk_tests`: **3957 checks / 0 failures** — new coverage for
  the `0xc6` declaration (incl. the real `XH1_DOOR` operand
  triple), the `0xc7` write, an end-to-end `0xc6` → punch
  element-damage case, the render gate + snapshot fields +
  type-4 billboard math + HUD frame mapping, and one event
  assertion per `CombatFxKind`.
- Remaining seams for Phase 10C: the actual particle/sound/
  software-render implementations, the `FUN_0045ee08` anim
  driver, type-4 `FUN_00409760` widget, and the handed-off death
  script's execution (G5).

## 147. Per-object update order — `FUN_004572ac` (OBSERVED, disasm)

Instruction-level decomp of the object loop (`/tmp/g5_loop.txt` —
private artifact, not committed). Per list node:

1. Advance the +0x68 list; nodes with `+0x06 == 0` are skipped at
   fetch time (the `named`/live gate is a loop condition).
2. `+0x07 == 1` → `0x49b85c` view-anchor latch.
3. `+0x14a & 0x10` → connector `FUN_00457738`;
   `+0x14a & 0x40` → orbit `FUN_00457ab8`.
4. `+0x14b & 0x40` → command runner `FUN_0045ab44` — the
   `do…while` wraps back to step 1 when it returns `[2]==0`
   (the runner can swallow the rest of an object's update).
5. `+0x2bc != 0` → pending-arena transfer `FUN_004574d0`.
6. `+0x108 != 0` → **`FUN_004388d8` object script VM** — NOT
   gated on `+0x06` at this point (the live check already ran at
   step 1).
7. `+0x06 != 0` re-checked (the script may have killed the
   object) → `+0xec != 0` → path follower `FUN_00456d28`;
   `FUN_004533d4` subtype dispatch.
8. `+0x06 != 0` → gravity `FUN_0045b9fc` + collide
   `FUN_0045bac0`.
9. `+0x06 != 0` → `+0x149 & 0x10` → enemy dispatch
   `FUN_0045897c` (post-check `+0x06` again — the dispatch can
   kill), else `+0x14a & 0x20` → mover `FUN_004585c4`.
10. Anim driver `FUN_004555bc`.
11. `+0x06 != 0` → velocity write `+0x18c/190/194 =
    (pos − prevPos)·(1.0/DAT_0049b6f0)` (the constant is 1.0),
    `+0x148 & 0x40` raw-matrix path, ride displacement for the
    carrier, `prevPos`/`prevYaw` latch.

Native: `traversal_runtime.cpp` object loop follows this order
for the ported stages (connector pre-pass + script tick + anim +
mover/ride tail, each behind the same flag/`+0x06` gates; the
unported stages — orbit, command runner, path, subtype, gravity,
enemy dispatch — remain seams). The post-anim `+0x06` re-check
and the `+0x18c` velocity write are included.

## 148. Persistent object-script VM — `FUN_004388d8` object ctx (OBSERVED, ported)

`FUN_004388d8` is a single interpreter parameterized by a context
block; the arena ctx (`TraversalScriptState`, Phase 5I) and the
object ctx (the `DynamicObject` itself) share the opcode set but
map the ctx fields differently (OBSERVED dispatch head
`0x4389dd`, jump table `0x438a5c`).

Object-ctx field map (all OBSERVED from handler disasm):
- `+0x108` — persisted PC (image pointer); entry point and the
  checkpoint target. Gate: the caller only invokes the VM when
  it is nonzero.
- `+0x22c` — wait seconds, decremented by `1/30`
  (`DAT_0049b6f4`) at the head of every invocation; while the
  remainder stays positive the fetch is skipped entirely; on
  crossing zero the pass resumes at `+0x230`.
- `+0x230` — wait-resume PC (image pointer).
- `+0x234` — 4×f32 script locals (varop mode 2).
- `+0x244` — local flag dword (flag group 2).
- `+0x248` — event-call depth, cap 4 ("Gosub overflow").
- `+0x24c`/`+0x25c` — per-depth return PC / saved `+0x108`.
- `+0x26c` — per-depth marker (`0x09` clears slot 0).
- `+0x312` — child flag dword (flag group 5).
- `+0x21d` — event byte; `+0x21e` — running/suspend byte
  (`0xff` clears it).
- `+0xec` — bound path record; `+0xf0` path frame;
  `+0xf4..+0xfc` lateral offset; `+0xe6` path cursor
  (`0xffff` on bind).
- `+0x11e` — subtype; `+0x120..+0x128` subtype dwords;
  `+0x2a0/+0x2a1/+0x2a8/+0x2ac` subtype/mover state.
- `+0x11a`/`+0x11b` — byte fields (ops `0x0b`/`0x49`).

Execution model (OBSERVED): one pass per invocation, max 1000
fetches ("Alien %s looped %d commands, off %lx" diagnostic +
`+0x108 = 0`). `+0x108` is NOT rewritten on exit — progression
is by explicit checkpoint: op `0x01` writes `+0x108 = pc` and
continues; calls push `{retPc, savedPc}` and set `+0x108` to the
target; `0xfd` pops and restores `+0x108`; `0x0c` rgoto
checkpoints the jump target; `0x09` clears `+0x108`/depth/
`mark[0]`; `0xff` suspends the pass (`+0x21e = 0`). A script
that suspends without a fresh checkpoint re-enters at the last
one — matching the observed real scripts (`ckpt; …; ff`).

Native: `traversalObjectScriptTick` (declaration in
`traversal_script.h`) — pointer-faithful PCs
(`field108`/`field230`/stack slots are `const void*` image
pointers; converted to offsets only at the Reader boundary, all
reads bounds-checked). The same `objScriptInsn` dispatcher
serves `traversalObjectInitScript` (synchronous, clears
`+0x108` after — `FUN_004566f0` semantics) and the per-frame
tick. `ScriptCtxSlots` binds the shared var/flag resolvers to
the object block.

## 149. Object animation — `FUN_004555bc` / `FUN_00455890` / `FUN_00455c48` (OBSERVED, ported)

`FUN_004555bc` is the per-object animation driver (called at
update step 10 above). Timing: `+0xdc` accumulator advances by
`rate·dt` (`+0xe0` = 30); `steps = FRNDINT(+0xdc) − +0xe4`
(`FSUBR` at `0x45572c`); `FUN_00455890(obj, steps)` advances
`+0xe4` per step and wraps at `frameCount` when `+0x148 & 8`
(loop, op `0x3b`) or latches `+0x118 = 0xff00` at the last frame
(one-shot, op `0x03`). The `+0x140`/`+0x144` sound marker fires
once when the accumulator crosses the mark.

Record format (OBSERVED — verified against ~1274 real records
across BUILD_A LEVEL3–8):
`{f32 rate; u32 chanCount; u32 frameCount; u32 chanOff[chanCount];
rootKeys[frameCount]×12B; u32 refCount; refKeys[refCount][frameCount]×12B;
channels…}`. `FUN_00455890` per step: root key is a displacement
delta rotated by the object matrix (`FUN_0046b048` — pure 3×3,
no translation) into `+0x294/298/29c` (animation root motion);
ref points copy `refKeys[r][frame]`; each channel is
`{name[12]; u32 vertCount; f32 scale; …}` name-matched
(`FUN_0042fa50` = plain strcmp) to model elements.

Two channel forms (both OBSERVED in real records):
- scale ≠ 0 — vertex-delta: base pose `vertCount`×12B then
  per-frame `{i16 tag; i8 delta[3·vc]}` applied as
  `v[j] += delta[j]·scale` to the element's cloned verts.
- scale == 0 — rigid (`FUN_00455c48`): `{u8 rotShift, u8
  trnShift; base pose; i16 xform[12]/frame}` — per-frame
  fixed-point 3×3+T applied to the base pose.

Native: `src/core/object_animation.{h,cpp}` —
`objectAnimTick` (driver + frame advance + wrap/latch + sound
mark), delta and rigid channel appliers, root-motion write.
`traversalObjectAnimUpdate` calls it at the observed position;
the former connector-only `connAnim*` fields are generalized to
`animRec`/`animAcc`/`animFrame`/`animLatch`/`animRate` +
`animSoundName`/`animSoundMark` on `DynamicObject`.

## 150. Object-script opcode map — corrections + coverage (OBSERVED)

Handler-disasm corrections to the Phase 5I tables:
- `0x75` is `+0x148 &= ~u32` (handler `0x44d025`) — NOT the
  anim-target writer; that is `0x76` (handler `0x43976b`:
  `{u32, low16 used}` → `+0x118 = (i16)low16 − 1`). The earlier
  `0x75`→`+0x118` label was a mislabel.
- `0x03`/`0x3b` `{u32 imgref}` — bind `+0x114` via the lazy
  image-ref resolver; on rebind or when the done latch is set:
  `+0xe4 = 0xffff`, `+0xdc = −1.0`, `+0x118 = 0xffff`; `0x03`
  clears `+0x148 & 8` (one-shot), `0x3b` sets it (loop).
- `0x02` — path bind `{u32 ref, u8 f1, u8 f2, u16 frame,
  u8 mode, [f32×3 if mode==0]}` → `+0xec`/`+0x149` bits/
  `+0x14b & 8`/`+0xe8`/`+0xf0`/`+0xf4..fc`/`+0xe6 = −1`.
  `frame != 0` → `+0xf0 = frame`; else `f2 & 2` → `+0xf0 =
  *(u32*)(path + 4 + (count−1)·0x28) − 1` (record
  `{u32 count; entry[count]×0x28}` — 0x438f96); else 0.
  `mode != 0` → `+0xf4 = pos − FUN_00456bc8(path, +0xf0)` (the
  sampler stays a seam natively; offset zeroed).
- `0x4e` — subtype set `{u32×3}` → `+0x120..0x128`,
  `+0x11e = 0x4e`, unbinds `+0xec`, clears
  `+0x2a0/2a1/2a8/2ac`; `FUN_00451ee8` tail seam.
- `0x66` — conditional linkage gated on `+0xec == 0`: unbound →
  `0xfe`/`0xfc` call target A, `0xfd` return, `0x0c` goto A;
  bound → only `0xfe` acts, calling target B.
- `0x5f` — weighted event call `{u8 n; n×{u8 weight, u32 tgt}}`
  — `FUN_00401ed4(sum)` picks, first cumulative weight above the
  pick wins (native seam: pick = 0 → first positive weight;
  all-zero → no call).
- `0xfc` — random call `{u8 n; n×u32}` (seam: index 0);
  `0x0c` — same operand form, goto.
- `0x40` — wait `{varop}` → `+0x22c` (±0 → `0x3727c5ac`
  ≈ 1e-5 nudge), `+0x230` = post-operand pc.
- `0x41` — var write `{u8 mode, u8 idx, u32 bits}` → the
  FUN_00438654-selected slot.
- `0x44/45/46` — flag-group `|= / &=~ / ^=` `1<<(bit&31)`;
  `0x47` — test bit + linkage.
- `0xcd` `{u8}` → `+0x21f`.
- `0x18` `{u8 mark, str name}` → `+0x144 = mark−1`,
  `+0x140` = name (the `FUN_004555bc` one-shot sound seam).
- `0x4c` `{u32 imgref}` → `+0x110` (0 → null) — the
  death-script reference consumed by `FUN_00458140`.
- `0x10` `{u16}` → `+0x08`/`+0x2a2` health (+ mirror); ≥ `0xfde8`
  arms `+0x21f`; `0` → the die-facing removal path.
- `0x96`/`0x97`/`0x98`/`0x99` — connector data block
  (`+0x306/+0x30a` anim records, four sound slots, `+0x312` hi
  nibble, `+0x30e` radius).
- `0xc6` — element-set decl (§143); `0x1f` — element-name mask
  → `+0x2c8` ("ALL" wildcard).
- `0x08` `{i16}` → `+0x4c` yaw (+360 if negative); `0x0b`/`0x49`
  → `+0x11a`/`+0x11b`; `0x6f` `{u32}` → `+0x146` low16;
  `0x32/33/34` → `+0x38/3c/40`; `0x53` → `+0x58` scale (`0xff`
  ramp form consumed, runtime ease is a seam); `0x54` → `+0x5c`;
  `0x5a` → `+0xe8`; `0xc7` → `+0x104`; `0x23/24/3f/61/29/74` —
  `+0x148/149/14a` bit writes (§143 family).

Spawn binding (OBSERVED `FUN_00456808`): the CMI table-0 record
named `arena$model_spawnId` (e.g. `HMO_1$XGS_9`) stores the
object script's code offset — bound to `+0x108` after the
synchronous init script. The table-2 `arena$model` init runs
through the same interpreter and `+0x108` is cleared afterward
(transient). Verified on LEVEL3: `HMO_1$XGS_9` decodes as
`3b <anim>` / `02 <path>` / `4c <death>` / `10 fde8` / `01` /
`66 …` / `ff` — a path-bound looping walker.

## 151. Native implementation + validation (OBSERVED, ported)

- `dynamic_objects.h`: `scriptLocals[16]`/`scriptFlagsLocal`/
  `scriptCallDepth`/`scriptRetPc[4]`/`scriptSavedPc[4]`/
  `scriptMark[4]`/`scriptFlagsChild` (+0x234/244/248/24c/25c/
  26c/312 ctx block), `fieldEC`/`fieldF0`/`fieldF4`/`fieldE6`
  (+0xec/f0/f4..fc/e6 path state), `field18c[3]` (+0x18c
  velocity), `animRec`/`animAcc`/`animFrame`/`animLatch`/
  `animRate`/`animSound*` (generalized from `connAnim*`).
- `dynamic_objects.cpp`: `spawnArenaObjects`/`spawnRecord` take
  a `DynamicObjectScriptSource` — binds `+0x108` from the
  table-0 `arena$model_spawnId` record at DTI spawn;
  `latchObjectPrevState` writes `+0x18c` before the latch.
- `traversal_script.{h,cpp}`: `ScriptCtxSlots` resolver adapter,
  `ObjScriptPass`/`objScriptInsn` shared dispatcher,
  `traversalObjectScriptTick` (the `FUN_004388d8` object tick),
  `traversalObjectInitScript` rebuilt on the same dispatcher
  (still clears `+0x108`), opcode corrections per §150.
- `traversal_runtime.cpp`: shared `objEnv` per frame; the tick
  runs at the observed position inside the `+0x06`-gated object
  loop; `traversalObjectScriptFor` resolves the table-0 key;
  post-anim `+0x06` re-check added.
- `object_animation.{h,cpp}`: new subsystem (§149).
- `mdk_bridge.cpp`: `conn_anim_active` reads `animRec` (field
  rename only — no presentation change; Godot enemy work stays
  deferred per scope).
- `mdk_tests`: **4072 checks / 0 failures** — persistent-VM
  coverage: ckpt/suspend persistence, wait decrement + resume,
  `0x09` stop, `0xfc`/`0xfd` call stack, `0x5f` weighted pick,
  object locals/flags, `0x02` path bind, `0x4e` subtype,
  death-handoff execution, foreign-PC bounds, 1000-insn cap,
  init transience, DTI spawn binding; plus the animation record
  suite (header/delta/rigid/loop/latch/bounds).
- `mdk_frontend_tests`: 60 checks / 0 failures; Metal startup
  `--frames 30` clean; BUILD_A manifest 141/141 unchanged.
- Remaining seams (Phase 11B+): `FUN_00456d28` path follower,
  `FUN_004533d4` subtype behaviors, `FUN_0045897c` enemy command
  dispatch, `FUN_0045b9fc`/`FUN_0045bac0` gravity+collide,
  `FUN_0045ab44` command runner, `FUN_00457ab8` orbit, the
  `+0x14b` byte, the path sampler `FUN_00456bc8` in script
  context, RNG-real `FUN_00401ed4` picks.

# Phase 11B — Native Enemy Motion, Command Dispatch, Attack Boundaries

Phase 11A left the FUN_004572ac loop's native stages as seams.
Phase 11B ports them all, in the observed order, and fills the
object-script opcode census the live LEVEL3–8 scripts actually
reach.

## 152. Object paths — `FUN_00456bc8` / `FUN_00456d28` / `FUN_00457264` (OBSERVED, ported)

Path record (OBSERVED): `{i32 count; entry[count]×0x28}` with each
entry `{i32 frame; f32 pos[3]; f32 tanIn[3]; f32 tanOut[3]}`.

- `FUN_00456bc8(path, frame, out)` — the cubic Hermite sampler.
  Segment select walks `count−2` down to `0`, first entry with
  `entry.frame <= frame`. No clamping: out-of-range frames
  extrapolate through the first/last segment basis (OBSERVED
  quirk — a frame 25 request on a 0–20 record yields the Hermite
  tail value, not the endpoint).
- `FUN_00456d28` — the per-frame follower. Advances `+0xf0` by
  `dt·rate`, samples the record, applies the `+0xf4..0xfc`
  lateral offset, integrates the position delta, and fires the
  forward-release boundary at `endFrame − 2` (OBSERVED — the
  +0xec binding drops two frames early, not at the terminator).
- `FUN_00457264` — bind-time snap: positions the object at
  `FUN_00456bc8(path, +0xf0)` + the lateral offset.

Native: `src/core/object_path.{h,cpp}` — `pathSample`,
`objectPathSnap`, `objectPathFollow`, `pathFirstFrame`/
`pathLastFrame`. The path lane fields (`+0xf4..0xfc`) are raw
dword storage with `std::bit_cast<float>` views — the original
writes both floats and dword patterns into them.

## 153. Enemy runtime core (OBSERVED, ported)

- `FUN_0045b9fc` — gravity: gated on `+0x148 & 2`; `+0x30 −=
  +0x48·dt`; medium damp (surface-volume query, mask 2) only
  when `(+0x148 dword & 0x45000) == 0x1000` and command ∉
  {0x80, 4} — then `+0x28/+0x2c ×= 0.1`; terminal `−220`.
- `FUN_0045bac0` — collision/integration: clears `+0x14c` bits
  0/1/4 at head; drag (3D for non-gravity objects, XY-only
  under `+0x148 & 2`); `vel += +0x294 impulse·dt`; `pos +=
  vel·dt` through axis-separated `FUN_0045d174` sweeps over
  `FUN_00407fc0` with the `+0x27c` clamp box and partner-arena
  retry; `+0x14c` contact bits (wall/floor/touch); floor-death
  `FUN_00458354` below the arena kill plane.
- `FUN_004533d4` — `+0x11e` subtype dispatcher: leader-follow,
  fly-to-camera, heartbeat (30-frame period, current-arena
  only), the spawnId-indexed chain drive (subtype 0x1e —
  pitch+yaw+scale-only `FUN_0046b3e4` matrices, member index is
  `+0x146` NOT a separate field), timed shot, attach,
  speed-ramp, ground-timer, and the 0x2b/0x4e/0xc5 shared
  steering tail (air `FUN_00452b80`, ground `FUN_004524e0`,
  re-seek `FUN_00452140` — target-hold check, turn-diff speed
  scaling, z-share damp, stuck accumulators, magnet pull).
- `FUN_00457ab8` — pendulum orbit about `+0x1c..0x24`.
- `FUN_0045ab44` — command runner (`+0x14b & 0x40` gate):
  airborne seek of the `+0x302` XY target at 50 u/s with
  per-axis clamp, `FUN_00454c6c` proximity arm (`+0x148 |=
  0x20`), landed fuse countdown at `dt` → die facing player.
- `turnToward`/`FUN_0045b56c` — quantized wrap steering:
  `diff ∓ trunc((diff ± 180)/360)·360`, cap `dt·180`, yaw
  wrap at exactly 360.0. FRNDINT under `FUN_0047d59a` is
  truncation toward zero (RC=11), not round-half-even —
  verified from the x87 control-word write.
- RNG — MSVC CRT LCG `FUN_0047d2b5` (`state·0x41c64e6d +
  0x3039`, `(>>16)&0x7fff`) + `FUN_00401ed4` pick
  (`(rand·n)>>15`); deterministic per `TraversalRuntime::rngState`.

Native: `src/core/enemy_runtime.{h,cpp}`.

## 154. Enemy command dispatch + attack boundaries (OBSERVED, ported)

`FUN_0045897c` (`+0x149 & 0x10` gate) dispatches on `+0x30a` with
the `+0x30e` tick countdown. Command bodies decoded and ported:

- cmd1 `FUN_00459330` (lunge), cmd2 `FUN_00459c5c` (spin
  `dt·235`), cmd3 `FUN_00459968` (spin `dt·360`), cmd4
  `FUN_00459160`, cmd5/0x81 detonate block (`SW_NUKE` morph —
  `FUN_00454794` lookup by literal name, NOT `+0x15c`; child
  init `+0x30e=900`, `+0x30=−5`, `+0x148 |= 0x818a6`,
  `FUN_0045612c` rebuild on the CHILD, `+0x15c =
  PTR_DAT_0049b854`), cmd7 `FUN_00459a9c` (morph burst —
  connector unlock at dist²<2500 via `FUN_00430190`, `+0x312 =
  (b & 0x1f)|0x80`, radial damage 200/60/−6-side −8),
  cmd8 `FUN_00459450` (pitch drift `dt·30`, shrink `×0.9`,
  die < 0.1), cmd9 `FUN_00459554` (timer freezes at 600 —
  OBSERVED clamp), cmd0x80 path-dropper, `+0x14a&4` carry
  `FUN_004599e8`.
- Bank falloff `dt·45` (not the 270 first read); expiry and
  contact-death share the contact tail; touch-scan masks are
  per-command (cmd5 → excl `0x810`, cmd0x81 → excl `0x830`,
  others → req `0x1000000`).
- `FUN_00459618` — the AABB-delta touch scan (`+0x14c` bit4);
  `FUN_00454c6c` — the melee proximity/damage scan (scale the
  `+0x198` AABB about its center by `+0x2c0`, test the player
  box `0x540c30` and arena objects).
- `FUN_00458354` — floor/kill-plane death boundary.

## 155. Update loop — full `FUN_004572ac` order (OBSERVED, ported)

`traversal_runtime.cpp` now runs the complete per-object order
verified against raw disasm (`0x4572c7–0x4574d0`):

`+0x06` head-scan → `+0x07==1` view latch → connector
(`+0x14a&0x10`) → orbit (`+0x14a&0x40`) → runner (`+0x14b&0x40`,
then `+0x08` health check — dead skips the rest) → pending
transfer (`+0x2bc`, nonzero skips) → script VM (`+0x108`) →
`+0x06` → path (`+0xec`) → subtype → `+0x06` → gravity →
collide → `+0x06` → enemy dispatch (`+0x149&0x10`, REPLACES the
mover) / mover (`+0x14a&0x20`) → anim → `+0x06` → `+0x18c`
vel-cache → roll ride (`+0x148&0x40`, `FUN_0045d578` —
pre-multiplies the `+0x302` rawMatrix by the frame-delta roll)
→ ridden-carrier displacement (`edx == 0x540dc0` check) →
prev-state latch.

Iteration reads the next link BEFORE the body (`0x4572c1`), so
mid-frame transfers/teardowns cannot corrupt the walk — the
port mirrors this with `std::next` capture. The `FUN_0045f9b8`
3-slot shot pool ticks at the tail of EACH arena update
(`0x4572cd`) — with a partner it ticks twice (OBSERVED quirk,
preserved).

`FUN_004574d0` transfer: `+0x302` dest arena, `+180°` yaw flip
on migrate, `+0x148`/`+0x14b` activate/deactivate byte split.
The mover's `+0x312`-as-child-pointer tail (`child+0x278 = ctx`,
`child+0x11e = 0x4a`, `FUN_0045612c` rebuild) is documented;
mover-child spawning stays a counted seam.

## 156. Broadcast dispatch — `FUN_00438094` / `FUN_004382e0` (OBSERVED, ported)

Object/arena op `0x04` is the shared broadcast:
`FUN_00438094(ctx, outerMode, point, innerMode, name, arg)`
filters arena objects then `FUN_004382e0` acts per object.

- Outer modes: `7` remote script call, `0xfc` remote GOSUB
  retag, `0x2b` camera-relative seek order, `1` formation-slot
  command.
- Inner filters: `3` all objects; `2`/`4`/`7` model-name match;
  `5` spawnId; `6` range + line-of-sight; `0xa` position-Y
  threshold; `7`/`8` subordinate filter; `9` bound object only
  (`+0x2b8`); `4` first match only.
- Rank gates `+0x11a`/`+0x11b`; remote-call dedup `+0x10c`;
  leader/context `+0x138`; target/formation offsets
  `+0x120`/`+0x12c`; seek orders run `FUN_00451ee8`
  (`objectWaypointReseek`) + the seek tail. Formation mode 1
  alternates ±side across matches in iteration order (the
  arena storage front-inserts — newest first).

Native: `broadcastDispatchOp` in `traversal_script.cpp`, shared
by both VM forms (arena ctx = `eventLatch`, object ctx = the
`DynamicObject`).

## 157. Phase 11B object/arena opcode additions (OBSERVED, ported)

The live LEVEL3–8 census drove implementation until zero
unknown-opcode diagnostics remained. Operand grammars are the
original's, including quirks:

- Linkage family: `0x2a` (name-match vs `+0x21e` bound record
  — the `-0x258` gate clears the mark only on fire, s1-empty
  scripts still dispatch), `0xa6` (LOS to `0x540e60`, exits
  entirely when the cmd object is null), `0xa7` (flee with
  `±operand·(rand−0x4000)·2⁻¹⁴` perpendicular jitter, operand
  clamped to 6.0), `0x2b` (camera-relative seek point —
  `bearingDeg` + `FUN_0045acf0` convention verified),
  `0x0e`/`0x39` (FUN_0045d880 cone+LOS — `0x39` inverts the
  target polarity: operand 1 = else, operand 2 = true),
  `0x36` (FUN_0045ad40 compare vs seek-target distance, 7
  compare kinds, ±0.05 epsilon), `0x16` (`+0x21e != 0 &&
  != −3`), `0x11` (anim-done), `0x12` (timed mark ≥
  wait·30), `0x2c` (no subtype), `0x2f` (percent chance —
  `rand(10000) < prob·100`), `0x48` (flag-bit-CLEAR, the
  `0x47` inverse), `0x6c` (player-box overlap —
  `FUN_0045c1b8` first-unmasked element vs `0x540c30`),
  `0x2d` (camera distance compare), `0xb0` (`+0x14a & 4`),
  `0xcf` (yaw morph via `FUN_0045dc18` wrap-aware approach —
  `flag==0xff` escapes to a `while(yaw≥360)−=360` wrap; the
  negative-side loop is dead code, OBSERVED).
- Writes: `0x35` → `+0x34` steer/projectile rate; `0x3a` →
  `+0xe0` animRate; `0x52` → `+0x44` drag; `0x6a` → `+0x302`
  f32; `0x86` → `+0x54` pitch drift `dt·(1/30)`; `0xd8` → var
  accumulate `*slot += operand·const`; `0x04` broadcast.
- Actions: `0x3c` face camera (yaw only); `0x65` face camera +
  pitch aim (`+0x4c` only when XY dist > 2.0, `+0x13c =
  bearing(dz+3.0, xyDist)`, same dead wrap loop); `0x3d`
  model spawn at refpoint/named-element centroid
  (`FUN_0045db60` vertex centroid, `FUN_0045cffc` freelist
  alloc with cross-arena XG reclaim edge case, `+0x11e=0x3d`,
  `+0x148|=0x80820`, `+0x4c/+0x13c` inherit, `+0x108=pc`);
  `0x59` sfx bind (`+0x15c` string when mode&4; the 0x4a1220
  record resolve + state calls are a presentation seam — the
  position locals are DEAD in the original); `0x6e` immediate
  teardown (`FUN_0045828c`); `0xc8` move-toward-point +
  arrive link (`+0x294` impulse triple); `0x17`/`0x52` flag
  writes; `0x43` element spawn.
- Arena-VM additions: `0x04` (shared broadcast), `0x77`
  (model-name count → `FUN_0045ad40` compare), `0xaf`
  (0x54155c inventory count — the table is unmodelled; scan
  yields 0, same seam convention as the player reload path),
  `0xd8` (var accumulate).

doCall/doGoto mark convention (OBSERVED): the shared tails
clear `mark[depth+1]` post-increment — `scriptMark` widened to
5 slots (`+0x26c..+0x274`).

## 158. Native implementation + validation (OBSERVED, ported)

- `object_path.{h,cpp}`: §152 sampler/snap/follower.
- `enemy_runtime.{h,cpp}`: §153–154 runtime, RNG, steering
  family (`FUN_00452b80`/`FUN_004524e0`/`FUN_00452140`),
  `objectWaypointReseek` (`FUN_00451ee8`), `objectRollRide`
  (`FUN_0045d578`), `degAsin` (`FUN_00437ff4`), transfer
  (`FUN_004574d0`), teardown (`FUN_0045828c`), touch/melee
  scans, command bodies.
- `traversal_runtime.cpp`: the loop now runs the full §155
  order; `traversalModelFor` moved to mdk scope for the
  spawn path.
- `traversal_script.cpp`: §156 shared broadcast + §157 opcodes
  on both VMs; `coneLosTest` extracted (`FUN_0045d880`).
- `collision_query.h`: `+0x14c` per-frame contact byte.
- `dynamic_objects.h`: `+0x11a/11b/11e/120/12c/138` subtype +
  broadcast fields, `+0x22c/230` script state, `+0x2b8` bound
  object, `+0x10c` dedup, `+0x108` script PC, `field2a4`,
  orbit anchor `+0x1c`, `animRate +0xe0`, `field34` steer
  rate, `scriptMark[5]`.
- `mdk_tests`: **4152 checks / 0 failures** — path sampler
  (Hermite incl. no-clamp extrapolation), snap/follow,
  release boundary `endFrame−2`, ghost-path impulse, RNG LCG,
  bearing/sincos, steering turn semantics, subtype behaviors,
  orbit, runner fuse, dispatch bodies + detonate morph,
  kill-plane, transfer yaw-flip, broadcast filters
  (name/spawnId/range-LOS/bound/first), formation ±side
  ordering, timed/probability/cone/no-subtype/anim-done/
  flag-clear/box-overlap/camera-distance/`+0x14a&4`/yaw-morph
  links, field-write ops, spawn, sfx, teardown, and the
  XCORDOOR negative control (connector objects inert under
  every gated Phase-11B piece).
- LEVEL3–8 census at 3000 frames: `diag=0` on every level,
  all runtime checks PASS, live migrations (LEVEL3 `624`,
  LEVEL7 `2999`), no unknown-opcode diagnostics. Digests:
  L3 `efde02a3b732fe6d`, L4 `bc18768bc028eab9`, L5
  `45cd15c6721169cf`, L6 `24bd007d063dcac0`, L7
  `bc02606139da10c6`, L8 `1079f68afa71a14d`.
- Remaining seams (documented, counted): mover-child spawn
  (`+0x312` pointer tail), FX/SFX record table (`0x4a1220`),
  inventory table (`0x54155c`), the `FUN_004585c4` name
  branches, and `FUN_00407fc0` clamp-box edge cases.
