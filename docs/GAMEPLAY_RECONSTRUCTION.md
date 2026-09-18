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
