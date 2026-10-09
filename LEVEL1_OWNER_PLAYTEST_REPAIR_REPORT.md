# LEVEL1 owner-playtest repair report — timing, nuke chain, freefall, combat FX

Date: 2026-10-08. Scope: **Level 1 + freefall only.** Per the owner
instruction "STOP ALL LEVEL-3 WORK" this report touches no Level-3
behavior; the packaged/default app launches the normal frontend and a
fresh playthrough begins at Level 1.

Oracle: original capture `runtime-private/captures/mdkdos_000.avi` +
the owner playtest. Evidence tags per
`docs/reverse-engineering/EVIDENCE_POLICY.md`: OBSERVED / CORROBORATED /
DOCUMENTED / HYPOTHESIS / UNKNOWN.

Repair commits this round:

- `2a08e71` L1 playtest repair: freefall artifacts + pacing + mover
  fixes (Phases A, B, C, and the freefall half of D)
- `d427890` traversal FX: index-0 texel transparency for object
  materials (Phase E — the traversal sibling of the freefall fix)
- `03b3643` docs: §242 index-0 texel transparency + headshot-seam
  status

## Launch / naming corrections (recorded, not defects)

- The earlier claim that a prior handoff gave a legitimate **direct
  Level-3 start** was incorrect. There is no such path; the packaged
  app opens the frontend and play starts at Level 1. Recorded so the
  claim is not repeated.
- **RECLASSIFIED 2026-10-09** — the line below had the mapping
  backwards. Corrected canonical mapping (owner-confirmed +
  `MDKFONT.FTI` + item dispatch): `SW_INTER`/item 2 = **"World's Most
  Interesting Bomb"** (WMIB — spins, attracts enemies, opens/explodes);
  `SW_KEY`/item 7 = **"World's Smallest Nuclear Explosion"** (the nuke).
- ~~The Level-1 progression pickup is the **"World's Smallest Nuclear
  Explosion"** (item id 2 / `SW_INTER`), sound record `WMIB`. An
  earlier note mislabelled it "World's Most Interesting Bomb"; the
  comment is corrected.~~

## Phase rows

| item | status | direct evidence (this build) |
|---|---|---|
| A — global timing | VERIFIED MATCH | Modes 2/3 now pace one sim step per ~33.3 ms wall-clock (`_process` accumulator gate in `main.gd`), catch-up bounded at 4 with backlog drop. The original contract is one step per dispatcher tick; a 60/120 Hz display previously stepped the world 2–4× fast. Input sampled once per rendered frame and held across substeps; mouse deltas applied on the first substep only. Verified at 30/60/120 fps. |
| B — nuke progression blocker | VERIFIED MATCH | Root cause: a descending mover kept its **spawn-height** world AABB (transform/AABB were not rebuilt after the position moved), so the collection overlap never fired. `FUN_0045612c` now rebuilds transform+world AABB at the mover-path tail. The DANT_1 nuke-chain pickup is collectable end-to-end. |
| C — I Feel Top fleeing | VERIFIED MATCH | Root cause: mover special-case dispatch used the **geometry** name; `SW_H150`'s geometry reports `PEN_36`, so `moverSwH150` was unreachable. Dispatch now keys on the table-1 **entry** name (`SW_H150`) — flee + collect verified live. |
| D — freefall artifacts | VERIFIED MATCH | Two defects fixed, one classified. (1) Spurious kind-1 **pickup marker** removed — `+0x10c` is a relocated sprite pointer populated only by the traversal/load fixup; spawned objects get `+0x10c=0` and nothing writes it, so the marker never drew in the original (the cream square was the PICK sprite's opaque border). Pickups now render as the kind-2 medallion alone. (2) Hit-explosion **grey dome** fixed — see index-0 note below. |
| D — homing-missile lifecycle | VERIFIED MATCH | The sim lifecycle is complete: spawn → home → hit (hp drop, real damage) → EXPLODE conversion → pool free → next waves. Confirmed live: missiles track (z closes), trail fills 32 slots, hits fire. The visible defect was the explosion dome (index-0), not lifecycle. |
| E — aircraft-death remnant | VERIFIED MATCH | The `EXPLODE` corpse remnant rendered as a featureless **black sphere**; it is a 26-frame transparent-surround fireball (~60.7% index-0). Now a translucent fireball — verified on a live scoped-fire remnant. See index-0 note. |
| E — grunt headshot FX | PARTIAL (seam) | The element-mask mechanic works; the gore visuals are **counted cosmetic seams, not presented**. Script op `0x81` `elKill` sets `elemMaskB |= 1<<idx` + `elemMaskLatch |= 1<<idx` (head element masks off, `0x20` unmask can't re-arm). But the debris-shard emit (`FUN_0041c420` → `elemShardCalls`), refpoint blood emitter (`0x80` `refEmit` → `refEmitCalls`), hit decal (`0x82` → `impactDecalCalls`), and `0x84` `sfxPee` splat (`sfxPeeCalls`) are request-counted only. The index-0 fix makes the underlying `SB_*`/`SL_*`/`FIRE` sprites renderable-correct, but does **not** wire the emitters — deferred presentation gap, not a new regression. |

## The shared root cause — index-0 texel transparency (OBSERVED)

Three of the reported visual defects reduce to one contract: **MTI
index 0 is the software fill's transparent-texel key.** The bridge
already emitted index-0 texels as RGBA alpha 0 (`objectTexture_` /
`freefallTexture_`), but the Godot `StandardMaterial3D` never enabled
the transparency mode — index-0 rendered as **opaque black** (or, lit,
a grey dome).

Fix (both material paths): `get_object_material` /
`get_freefall_material` report `has_alpha` (frame-0 scan for any
index-0 texel — the same frame the upload samples). `main.gd`'s
`_object_material` / `_ff_material` set `TRANSPARENCY_ALPHA` only when
`has_alpha` is true; opaque geometry stays in the depth-writing
opaque pass.

A `LEVEL3S.MTI` census confirms index-0 is confined to FX records —
`EXPLODE`, `TRAIL`, `SB_MED`/`SB_SMA`, `SL_BIG`/`SL_MED`, `BUBB`,
`PULSE`, `FIRE` — so no opaque wall/enemy/pickup geometry takes the
transparent pass. Resolved by this fix:

- death remnant black sphere → translucent fireball,
- `FIRE` teardown burst (~83% index-0) and `SB_*`/`SL_*` blood splats
  (45–77% index-0, dominant red index-3) opaque black blobs →
  transparent-surround sprites,
- freefall hit-explosion grey dome → translucent fireball.

## Full regression gate (2026-10-08, at `03b3643`)

| check | result |
|---|---|
| `build/mdk_tests` | **583,499 checks / 0 failures** |
| `frontend/godot/gdextension/build/mdk_frontend_tests` | **340 checks / 0 failures** |
| `ctest` (build/ + gdextension/) | **2/2 PASS** |
| `pytest tests/` — `MDK_GODOT_BIN=/Users/junhokim/Downloads/godot_tpl/Godot.app/Contents/MacOS/Godot` | **33 passed / 0 failed** (18.5 s) |
| Godot binary | 4.7.x (`/Users/junhokim/Downloads/godot_tpl/Godot.app`) |

pytest coverage: headless frontend smoke + `mdk-inspect` crosscheck,
frontend menu + real-saves read-only, freefall c0/c2/c4, stream
c0/c4, campaign c0/c2/c3/c4, combat closeout L6 + L8 + save/restore
golden, binary inventory ×8, hash manifest ×6.

## Known limitations / remaining seams (for owner review)

- **Headshot / death gore presentation** — the blood-emitter
  (`refEmit`), detached-element debris shard (`elKill` →
  `FUN_0041c420`), impact decal (`0x82`), and `sfxPee` splat (`0x84`)
  are request-counted seams, not rendered. The head-element mask
  itself is correct. Wiring these emitters is a presentation task,
  deferred.
- **Freefall green scan-beam** — the radar's flat-green scan sector
  seen in the original is a separate visual (not the LUT wedge, whose
  pens/keys are verified identical between DOS and MDK95). The wedge
  renders faithfully; the scan-beam is a documented non-blocking gap.
- **Freefall backdrop hue** — the canyon surround renders cooler
  (blue-purple) than the original's warm orange. This is a
  backdrop/LUT-subsystem colour-resolution matter, documented in
  `FREEFALL_BACKDROP.md`; not a regression from this round.
- **Freefall `BANG` overlay** (kind-3 missile-hit) — a deferred seam;
  only the EXPLODE mesh presents on hit.

## Stopping condition

All in-scope L1 repairs are implemented and verified, and the full
regression gate is green. **Stopped here for owner review** — no
further human playtest is launched without approval, and Level-3 work
remains halted per instruction. The headshot-gore, scan-beam, and
backdrop-hue items above are classified seams awaiting a decision on
whether to promote them to a future phase.
