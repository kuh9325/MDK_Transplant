# Gameplay Reconstruction — Phase 5A status

Phase 4 is **CLOSED at Phase 4K**. The remaining front-end children
(Help `FUN_0041d540`, Joystick, Performance) and real frontend audio
playback are deferred compatibility/polish work — they do not block
keyboard+mouse gameplay reconstruction.

Phase 5 begins gameplay reconstruction. **Phase 5A reproduces the
original per-frame gameplay-input consumption layer only**: raw
keyboard/mouse state + the configured bindings → the semantic
control block. No player movement, camera physics, weapon
simulation, sniper behavior, collision, level logic, or world
mutation — the consumers of the control block are later-phase work.

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
rate = clamp(v / frameStep, -4, +4)  // frameStep = 0x49b6f0 (~33.33)
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

## 8. Zoom accumulator tail (OBSERVED)

`0x4ce760` decays by the raw frame delta `0x49b6e8` (~33) toward 0
each frame; while `|acc| ≥ 1` the frame emits the zoom velocities
(±0.01 slow / ±0.15 fast, sign per accumulator sign). Net effect: a
single wheel detent (dz=120, scale 50 → 3 ticks) emits exactly one
frame of zoom velocity — reproduced in the tail.

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

- `0x540c80` (`moveBoostGate`) — read each frame; writers are player
  state machines. Meaning UNKNOWN — modeled as an environment
  input, default 0 (the ×4/3 lift active).
- `0x5414e4` (`debugMoveBoost`) — debug-command flag; with Tab
  level it overrides move rates to 20/3. Modeled, default 0.
- Joystick consumption — `FUN_0046b9b4` outputs and the
  `JoyOn`/`JoyType`/axis-map/button-mask tables are modeled because
  `FUN_00406f14` reads them, but nothing upstream feeds them;
  `joyOn=false` reproduces the canonical no-device path.
- `0x49b6f0`/`0x49b6e8` provenance — the frame-step divisor and raw
  frame delta are read as environment inputs (default 100/3, 33);
  the original timing writer is a later-phase concern.

## 14. Recommended Phase 5B target

The first downstream consumer of the control block — most plausibly
the player-movement path that reads `0x4ce6e0..` rate products
(`FUN_00463608` callers / the movement integrator), so the semantic
frame gains its first real consumer. Not started in Phase 5A.
