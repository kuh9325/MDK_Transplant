// Phase 10A — projectile lifecycle, impact, and the damage/death
// boundary for player weapons 0..4.
//
// EVIDENCE (instruction-level, MDK95.EXE BUILD_A; offsets in the
// original shot record/globals are noted per field):
//
//   FUN_0045f9b8(shot) — the per-slot update, invoked at the TAIL of
//   each FUN_004572ac(arena) call. Because FUN_004572ac runs once for
//   the current arena and again for the partner while one is active,
//   the shot pool ticks once per arena invocation (the partner-active
//   double-tick is ORIGINAL behavior — preserved, not "fixed").
//
//     Head gate (OBSERVED): state(+0x00) > 1 AND +0x14 > 0 selects the
//     dying branch — +0x10 decrements (only while positive), types
//     other than 4/2/3 shrink the tail by 5*dt and rebuild it, +0x14
//     decrements, and +0x14 <= 0 releases the slot (state = 0). Any
//     other slot state falls into the flight section — including a
//     stale state>1 with +0x14 <= 0 (the original flies it again).
//
//     Flight (OBSERVED): snapshot prevPos = pos (+0x20), invoke the
//     +0xd4 fly callback, and — unless +0xf8 bit0 is set (the ribbon-
//     bound path, which skips collision AND the object scan) — scan
//     the current arena's +0x68 object list, then the partner's when
//     (0x540ca8 && 0x540ca4 && !0x540d3c). Object gates: +0x06 named,
//     +0x08 model present, !+0x148&0x10, !+0x148&0x20, then the
//     zero-extent segment-vs-AABB prefilter FUN_0045cc2c and the
//     local-tri probe FUN_004138d8 (whose end-point writeback shorts
//     the segment so the last hit recorded is the nearest). After the
//     scan, type 4 sweeps (FUN_00407fc0, flag=0, ext={0.5}^3,
//     callback=0) the current arena then the partner (0x540ca4 &&
//     !0x540d3c — no 0x540ca8 gate); other types stab (FUN_00418c60)
//     with the same partner retry. A non-type-4 wall hit pushes the
//     contact one unit toward prevPos along the node plane, runs the
//     FUN_00460164 dispatch seam (surfaceDispatch channel 1,
//     secondary 8 for types 0/1 else 0, then the FUN_00437444 effect
//     seam), and preempts the object-hit tail: types 0/1 enter state
//     4 (+0x10=0, +0x14=30, +0xf4=0); types 2/3 detonate at the wall
//     (FUN_00460b7c(150, r=25, direct=0)). Type 4 instead bounces:
//     the 0x49b8e4 lobbed-shot latch stores the slot, surfaceDispatch
//     channel 1 secondary 0 runs (result bit1 swaps restitution
//     1.75/1.75 for 1.05/1.25), the velocity {sH*cosY, sH*sinY, sV}
//     reflects off the node normal with per-axis restitution, pos
//     becomes the sweep contact, speedV clears into (0,4), speedH is
//     the horizontal magnitude (nonzero rewrites yaw), and
//     +0x10 > 15 && sH < 0.5 && sV < 1 forces +0x10 = 15 (settle).
//
//     Tail (OBSERVED): +0x10 -= frameStep. Type 4 with +0x10 <= 0
//     detonates (150, r=50, direct=hitObj). pos.z below
//     shot->+0x18->+0x44e (the kill floor) or +0x10 <= 0 enters state
//     4 (+0x10=0, +0x14=30, +0xf4=0). A hit object then dispatches by
//     type: >= 2 detonates (150, r= type4 ? 50 : 25, direct=hitObj)
//     BEFORE the mark writes — the splash itself may set +0x21e;
//     when +0x21e stays <= 0 the marks +0x21e/+0x21c = hitElem+1 and
//     +0x21d = type fill in. Types 0/1 write +0x21e=0xfe, +0x21d=type,
//     +0x21c=hitElem+1, +0x210..218=pos, +0x220=tri, +0x224=yaw,
//     +0x228=pitch, subtract 8 from object health (only when
//     < 0xfde8), bump 0x540e84, and pick state 3 (survived — the
//     FUN_00437444 hit-effect seam fires) or state 2 (killed —
//     FUN_0042ac90 tally + FUN_004581a4 death). Both set +0x10=30,
//     +0x14=45, +0xf4=0, +0xbc=15.0 and rebuild the tail; the state-3
//     path also rewrites +0x21e = hitElem+1. No hit: type != 4 spins
//     +0x0c by 720*dt.
//
//   Fly callbacks (+0xd4) — OBSERVED:
//     FUN_004601d4 tracer (w0/w2): pos += dir*speedH*dt, tail +=10*dt
//       (cap 10), +0xcc -= 0.5*dt (floor 1.0), tail rebuilt.
//     FUN_004602d8 homing (w1/w3): while +0x10 <= 233 (the 240-tick
//       lifetime less a 7-tick grace) validate the lock — unnamed
//       homeObj clears +0xd8, an elemMaskB-masked homeElemIdx clears
//       +0xdc; steer toward the (elem ? element : object) AABB centre
//       via FUN_00460424 — yaw accumulator +0xe8 slews +-540 deg/s^2
//       capped +-270 and resets on reversal/zero, pitch slews
//       +-120 deg/s, the returned |yawErr|+|pitchErr| picks the speed
//       target (err <= 35 -> 250 else 100, eased -500/+200*dt), then
//       the tracer body runs. No target -> straight tracer flight.
//     FUN_004608bc lobbed (w4): t = |sV|+sH -> sH/t (1 when 0); sH
//       decays 60*t*dt (floor 0); rising sV sheds (1-t)*60*dt (floor
//       0); sV -= 32*dt (floor -220); surfaceVolumeQuery(mask=4,
//       vec={0,0,sV}) on the current arena — a hit scales sH by 0.25
//       and adopts outVec.z; the partner (ca4 && !d3c) only adopts
//       outVec.z. pos += dir*sH*dt + {0,0,sV*dt}; tail/cc as tracer.
//     FUN_0046075c ribbon (bound via FUN_00460860 when the 0x49b8e4
//       latch names a type-4 slot): +0xe4(param) += smoothed frame
//       units; the Hermite key record (+0xe0) is evaluated at the
//       param; +0x10 stays 99 while the param is below the last key's
//       frame-1, else 0 (release). The tail points away from the
//       player with z lifted by +0xbc*0.25. The key-record producer
//       (the thrown path/arc builder) is a seam — see the shot pool
//       tick seam note.
//
//   FUN_00460b7c detonate(shot, dmg, radius, directObj) — OBSERVED:
//     FUN_00460d44 object+poly splash (flags=6), then the player pass
//     (flags=1) at round(dmg*0.5), then the FUN_004575fc remnant +
//     effect seams; the shot enters state 5 with +0x10 = +0x14 = 30
//     and +0xf4 = 0.
//
//   FUN_00460d44(blast, dmgScale, range, tallyGate, directObj, flags,
//   exclMask) — OBSERVED. flags bit1 objects / bit0 player / bit2
//   arena polys; exclMask = -7 from the detonation.
//     objects (cur then partner on ca8 && ca4 — NO d3c gate): skip
//       unnamed/unmodelled/+0x148&0x10|0x20/excludeObj. Standable
//       objects (+0x149&0x20) falloff-test each unmasked, predicate-
//       named element (FUN_00460c08: centre distance vs range with a
//       stab-occlusion rule and an aux readback): the per-element
//       int16 pool at +0x30e+2e decrements and clamps at 0, latching
//       +0x21c = elem+1 / +0x220 = 0 on death. The direct-hit object
//       takes the full dmgScale at its AABB centre (aux=0) instead of
//       a falloff — the override is explicit, not radial. When the
//       best damage beats +0x2c4's gate (aux <= field2c4), whole
//       health drops (only when < 0xfde8), +0x21e=bestElem+1,
//       +0x21d=exclMask, +0x210=hitPt, +0x224=bearing, +0x228=0;
//       health <= 0 runs FUN_0042ac90 (tallyGate != 0) then
//       FUN_00458140(hitPt, bearing+180).
//     player (flags&1): dist2 to (playerPos + 1z); a current-arena
//       stab occlusion forces out-of-range; dist = sqrt(d2)*2;
//       dmg = round(scale*(range-dist)/range) capped at 15 ->
//       FUN_0046771c; 0x540d5c doubles.
//     polys (flags&4, cur then partner on ca8 && ca4): the arena's
//       surface slots (config byte + handlerOff nonzero) form the
//       channel mask; each non-skip poly on a live surface within
//       range^2 of the blast centroid stab-tests for occlusion — a
//       same-surface blocker in the current arena retries the
//       partner. Survivors dispatch channel 3 (tallyGate!=0) or 4
//       (tallyGate==0) with secondary=dmg and eventCode -7; a fired
//       channel clears.
//
//   FUN_0046771c(dmg, pt) — OBSERVED player damage: no-op while
//     health==0 && the +0x541510 gate is 0; suppressed to a
//     landingAccum clear when 0x540e10 > 0 or the loco state is
//     0x326/0x385/0x3ea or 0x540eb8 == 1. Difficulty: easy takes
//     max(2d/3,1), normal d, hard 2d. The 0x540dac accumulator takes
//     dmg*25 clamped [75,180]. A mounted class&1 mount redirects the
//     damage to its health (death via FUN_004581a4) — else player
//     health floors at 0 and 0x540d5c gains dmg.
//
//   FUN_0042ac90(obj, countGate) — OBSERVED kill tally: countGate!=0
//     and a model-name match in the 34-entry tally table bumps
//     0x540e90. The table contents are a seam (counted only).
//
//   FUN_00458140(obj, hitPt, facingDeg) — OBSERVED death boundary:
//     +0x110 set -> field11e=0, +0x08=0, +0x22c=0, +0x148 |= 0x20
//     (dead — the update scans skip it), +0x108 = +0x230 = +0x110,
//     +0x110 = 0 — the deferred-script handoff. +0x110 clear ->
//     FUN_00457cf4: remnant/effect spawn then the FUN_0045828c record
//     wipe (memset keeping +0x00/+0x60) and FUN_00458204 global-ref
//     clear (a cleared mount deals 50 to the player). Downstream AI,
//     death animation, sound, and particles remain seams.
//   FUN_004581a4(obj) — OBSERVED: hitPt = objPos + {0,0,3} and the
//     raw bearing(player - obj) -> FUN_00458140 — NO +180 here; the
//     +180 lives in the kill-path callers (earlier map note wrong).
//
//   FUN_00432f84 punch tail — OBSERVED: dmg = frameStep*6 while the
//   charge ammo[0] lasts else frameStep; the state byte is -2/-1.
//   A whole-object hit accumulates punchHitTime, subtracts health
//   (when < 0xfde8), writes +0x21d=state/+0x228=0/+0x224=bearing; a
//   kill runs FUN_0042ac90(charged), then — only when charged —
//   displaces the corpse pos by {20*cos,20*sin}*yaw before
//   FUN_00458140(hitPos, bearing+180). An element hit damages the
//   int16 elemHp TOO (death latches +0x21e/+0x21c=elem+1) and then
//   FALLS THROUGH to the whole-object damage — the quirk is original.
//   A miss stabs pos+yawdir*150 on both arenas; a wall runs
//   surfaceDispatch channel 2 secondary=dmg {ev=state, vecA=aimPt,
//   posB=missPt, contact=hitPt} and the FUN_00437444 seam when a
//   handler ran.
//
// See docs/reverse-engineering/GAMEPLAY_RECONSTRUCTION.md for the
// evidence-level breakdown.

#ifndef MDK_CORE_PLAYER_PROJECTILES_H
#define MDK_CORE_PLAYER_PROJECTILES_H

#include <array>
#include <cstdint>

namespace mdk {

struct TraversalRuntime;
struct TraversalArena;
struct DynamicObject;

// ---------------------------------------------------------------------------
// The FUN_004572ac tail — the 3-slot shot-pool update. Runs once per
// FUN_004572ac(arena) invocation in the original: the world tick calls
// it for the current arena and again for the partner while one is
// active, so the pool can tick twice per frame (OBSERVED — preserved).
// `frameStep` is timing.frameStep (int domain, drives +0x10/+0x14);
// `dt` is timing.deltaSec (float domain, drives flight); `smoothed`
// is timing.smoothed (the ribbon path's frame units).
void playerShotPoolTick(TraversalRuntime& rt, int frameStep, float dt,
                        float smoothed);

// ---------------------------------------------------------------------------
// The proven damage/death boundary — shared by the projectile path and
// the punch tail (FUN_00432f84). Not a generic combat framework: these
// are the exact original entry points.
// ---------------------------------------------------------------------------

// FUN_0046771c — player damage entry (difficulty-scaled, mount
// redirect, suppress gates). `pt` is the damage source position.
void playerDamageApply(TraversalRuntime& rt, int dmg, const float pt[3]);

// FUN_0042ac90 — the kill-tally boundary: countGate != 0 and the
// object's model name in the tally table bumps rt.killTally.
void objectKillTally(TraversalRuntime& rt, const DynamicObject& obj,
                     int countGate);

// FUN_00458140 — the death boundary (script handoff OR the
// FUN_00457cf4 teardown seam). `facingDeg` is the corpse's facing.
void objectDeathBoundary(TraversalRuntime& rt, DynamicObject& obj,
                         const float hitPt[3], float facingDeg);

// FUN_004581a4 — die-facing-the-player wrapper used by the kill paths.
void objectDieFacingPlayer(TraversalRuntime& rt, DynamicObject& obj);

// FUN_00460d44 — the splash/damage dispatcher (objects | player |
// arena polys by `flags`). `tallyGate` feeds FUN_0042ac90's count
// arg; `directObj` gets the full-scale direct hit; `excludeObj` is the
// mounted/exclusion object; `exclMask` is the +0x21d byte written on
// damage (-7 from the detonation).
void splashDamage(TraversalRuntime& rt, const float blast[3],
                  float dmgScale, float range, int tallyGate,
                  DynamicObject* directObj, std::uint32_t flags,
                  std::int8_t exclMask);

// ---------------------------------------------------------------------------
// Phase 10B — combat presentation contract (core-neutral).
//
// Everything below is derived from the original shot-render chain
// FUN_0045f030 -> FUN_0045ee7c -> FUN_0045e9a0 and the FUN_00437444 /
// FUN_004575fc / FUN_00458140 effect seams (all OBSERVED; see
// GAMEPLAY_RECONSTRUCTION.md). No Godot types — the frontend consumes
// these records verbatim.
// ---------------------------------------------------------------------------

// Per-slot shot presentation record — FUN_0045ee7c's two modes merged.
struct PlayerShotVisual {
  int slot = 0;              // pool index 0..2
  int state = 0;             // +0x00 verbatim
  int type = 0;              // +0xd0 weapon index
  const TraversalArena* arena = nullptr; // +0x18
  float pos[3] = {0, 0, 0};  // +0x20 — head position
  float tail[3] = {0, 0, 0}; // +0xc0 — tail endpoint; FUN_0045e9a0
                             // anchors the billboard here
  float tailLen = 0.0f;      // +0xbc
  float yawDeg = 0.0f;       // +0x04 stored yaw
  float pitchDeg = 0.0f;     // +0x08 stored pitch
  // FUN_0045e9a0's billboard basis inputs (0x540b50/local_20):
  //   type != 4: billboardYaw = 90 - yawDeg,  billboardPitch = pitchDeg
  //   type == 4: billboardYaw = 90 - atan2deg(dx, dy) over the
  //              pos-tail xy delta, billboardPitch = -speedV*0.5
  //              clamped to [-60, +60].
  float billboardYawDeg = 0.0f;
  float billboardPitchDeg = 0.0f;
  float spinDeg = 0.0f;      // +0x0c streak spin
  float fieldCc = 0.0f;      // +0xcc render scalar (0x540b58/0x540b64)
  bool ribbonBound = false;  // +0xf8 bit0
  // Mode-0 gate: the original submits geometry iff the record is
  // active (state != 0 && lifetime > 0).
  bool worldRenderable = false;
  // Mode-1 HUD indicator frame select (FUN_0045ee7c iVar7): -1 for an
  // active record (no indicator drawn), else 0 (free) / 3 (state 4
  // expired) / 0x3c (state 3 expired) / 0xf4 (any other expired
  // state). The indicator is gated on hudActive (0x5414d4).
  int hudFrame = -1;
};

// The original's per-slot HUD indicator rects — pos at 0x49b900
// {72,10} {228,0} {384,10} and size {140,70} at 0x49b8e8 (OBSERVED).
struct PlayerShotHudRect { int x, y, w, h; };
inline constexpr PlayerShotHudRect kShotHudRect[3] = {
    {72, 10, 140, 70}, {228, 0, 140, 70}, {384, 10, 140, 70}};

// FUN_0045f030's caller gate (0x436dd3, OBSERVED): the shot render
// pass runs only when flagC9c != 0 && transitionPhase > 1 — the fully
// scoped sniper state. There is no unscoped shot-render path.
bool playerShotRenderGate(const TraversalRuntime& rt);

// Per-frame snapshot of the 3-slot pool, mirroring FUN_0045f030's
// per-slot dispatch. Pure read — the pool is untouched.
std::array<PlayerShotVisual, 3>
playerShotVisuals(const TraversalRuntime& rt);

// ---------------------------------------------------------------------------
// Presentation-facing combat events — one record per original
// callsite, carrying the exact original effect args. Nothing here is
// gameplay state; the frontend drains TraversalRuntime::combatFx.
// ---------------------------------------------------------------------------

enum class CombatFxKind : int {
  kShotWallImpact = 0,   // FUN_00460164 tail -> FUN_00437444
  kShotObjectImpact,     // FUN_0045ff9d survived path -> FUN_00437444
  kPunchWallImpact,      // FUN_00432f84 miss stab -> FUN_00437444
  kPunchObjectImpact,    // FUN_00432f84 survived -> FUN_00437444
  kDetonation,           // FUN_00460b7c -> state 5 + FUN_004575fc
                         // (remnant DynamicObject, scale 2.0f)
  kObjectDeathScript,    // FUN_00458140 -> +0x110 script handoff
  kObjectTeardown,       // FUN_00458140 -> FUN_00457cf4 teardown
  // FUN_0045bec8's two water-splash FUN_00437444 callsites are NOT
  // emitted here: they sit on the generic sweep helper chain
  // (FUN_004572ac -> FUN_004533d4 -> FUN_0045b6f8), classified-only.
};

struct CombatFxEvent {
  CombatFxKind kind = CombatFxKind::kShotWallImpact;
  // FUN_00437444 ECX arg — particle palette/scale select: 0 -> {0xd|3,
  // 3, 1.0}, 1 -> {0x25, 0xf0, 0.5}, else(3) -> {10, 3, 1.0}. 0 on
  // non-00437444 events.
  int mode = 0;
  // FUN_00437444 stack arg — particle COUNT (the spawn loop runs
  // count times). flag21f on object hits (0/1), handler-ran 2 else 1
  // on wall hits.
  int variant = 0;
  // FUN_00437444 EBX arg — per-object impact-sound name override
  // (obj +0x150). Null/empty selects a random RICO1/2/3 handle.
  std::int32_t aux = 0;
  float pos[3] = {0, 0, 0};  // the EDX vector (hit/contact point)
  const DynamicObject* obj = nullptr;  // subject object, if any
};

} // namespace mdk

#endif // MDK_CORE_PLAYER_PROJECTILES_H
