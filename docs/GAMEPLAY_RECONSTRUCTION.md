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
