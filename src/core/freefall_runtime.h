#pragma once
// freefall_runtime.h — Phase 13A: native FALL3D/freefall runtime.
//
// Deterministic, presentation-neutral host for the original mode-2
// freefall course (the opening sequence of each level: Kurt drops from
// the pod toward the arena while dodging homing missiles, popping
// pickups and tracking a radar arrow). Headless: no SDL, renderer or
// audio — presentation side-effects are emitted as typed events.
//
// Original ownership (MDK95.EXE, OBSERVED via disassembly):
//
//   FUN_0040ef28  course init: clears the 399-record 0x32e dynamic
//                 object pool (FUN_0047d20a 0x4f7e0 bytes), chains the
//                 LIFO freelist forward through 0x540ed0, resolves the
//                 FALL3D bundle (MTI render records + BNI gameplay
//                 records: KURTANIM/KURT_HIT/BONESANM anims, FALLPU_%d
//                 pickup lists, models), writes the difficulty block
//                 0x4edc10..0x4edc24 from course 0x541498 and skill
//                 0x54147a, sets 0x4edcb8 = 150 (intro countdown),
//                 0x4edaf4 = (course >= 4), 0x4edbfc = -66.6667,
//                 viewport 600x360 zoom 2.4, then mode 0x541492 = 2.
//   FUN_004103d8  the mode-2 frame. Order (OBSERVED):
//                 zoom-sprite pitch -> 0x4edcb8 > 0: FUN_0040ff78
//                 intro and return -> audio seam -> palette cycle ->
//                 fade state machine (fade-in < 1s, damage-flash ramp,
//                 death fade, exit fade) -> three frame-step timers:
//                   0x4edcac radar spawn (FUN_00412060 once, then the
//                     radar object re-arms it on despawn),
//                   0x4edca8 FALLPU pop backwards (FUN_004118b0, re-arm
//                     (rand & 0x7f) + 0x1f, stop when index hits 0),
//                   0x4edcb4 missile spawn (FUN_0041151c, flash -0.2,
//                     budget 0x4edcb0 decremented, re-arm
//                     (rand & 0x1f) + 0x4edc1c)
//                 -> 0x4edc00 += dt -> FUN_00410e38 object walk ->
//                 camera {0.85*px, 0.85*py} + z = pz+10 below 30s,
//                 zoom-out rate ramp above 30s -> render seams ->
//                 return 1 when 0x4edc00 > 33.0 (completion) or when
//                 the death fade reaches 0 (health <= 0 path). The
//                 orchestrator routes return!=0 through FUN_0040fa68
//                 teardown, then 0x541554 > 0 -> traversal
//                 (FUN_004346e8) else frontend (FUN_0041d85c).
//   FUN_0040ff78  the scripted intro: 0x4edcb8 counts down 150 -> 0
//                 by frameStep. Fade phases (150..90 ramp-in, 90..60
//                 hold, <60 ramp handled by the main fade). Zoom
//                 sprite index 0x4edbf0 cycles 0..15; sub-frame index
//                 0x4edbf4 = ((120 - t) * 12 / 60) << 8 for t in
//                 61..119, else 0xc00. Player spawns (type 0, KURT,
//                 KURTANIM) the first frame t < 90; arc drives
//                 {30s-30, 10s-10, -10s} with s = 1-(1-t)^2,
//                 t = (90-t)/60 clamped to 1. At t <= 0: seed
//                 0x4edcac = (rand&0xf)+7, 0x4edca8 = (rand&0x1f)+0x1f
//                 (when pickups exist), player.z = 5206.0, and when
//                 0x4edaf4 spawn Bones (type 5, z = 5290).
//   FUN_00410e38  object walk: dispatch +0x04 type 0..5 via the
//                 0x410e20 table -> player FUN_0041210c, missile
//                 FUN_00410e9c, explosion FUN_00411660, radar
//                 FUN_00411aac, pickup FUN_00411710, bones
//                 FUN_0041236c. Handlers return the next record.
//   FUN_0040f96c  alloc: LIFO freelist pop, insert after list head.
//   FUN_0040f9d0  free: unlink, release seams, LIFO freelist push.
//
// Object types (OBSERVED):
//   0 player   — FUN_0041210c: z -= 66.6667*dt; while edc00 <= 30 the
//                shared FUN_00465b54 accel channel steers x/y
//                (FUN_00407e50 input: digital keys -> rate/cap pairs
//                +-11.7647/+-117.647, analog axis scaled, Y negated);
//                after 30s spring-damp (v - 2p) * 0.5 + K_FINISH edge.
//                Pos clamps x +-58.8235, y +-35.2941. AABB half-extents
//                {4, 5, 5}. Hit-anim restore when +0x118 == -256.
//   1 missile  — FUN_0041151c spawn: type 1, scale 1, dir rand*360/32768
//                deg, vel {sin*250, cos*250, +250}, timer 60, x/y lead
//                offsets (rand-0x4000)*edc14*(1/16384), trail FX seam.
//                FUN_00410e9c: integrate, z-throttle (z < pz*0.75 ->
//                extra vz*3*dt), collision window edc00 < 30 via
//                FUN_0045c230(prevPos->pos, player AABB). Launch: timer
//                counts 60 -> 0 (+0x108 ramps to 8). Homing: lead =
//                dz/225 capped < 10 (else -666.667), dir = (player +
//                {leadX, leadY, leadZ} - pos)/dist * 250, vel =
//                vel*0.8 + dir*0.2. Pass: dz <= -5 -> M_PASS, timer -1,
//                sink 60 frames then free. Hit: clamp pos, KURT_HIT
//                latch, damage by skill (4 / 4+rand8 / twice), flash
//                edc04 = 3.0, convert to type 2 explosion at player pos.
//   2 explosion— FUN_00411660: z pinned to player.z, animAcc += f0,
//                frame = rint(animAcc), scale = frame/2, free when
//                animAcc >= model anim0 frame count (bounded seam:
//                FreefallCourseData::explodeAnimFrames).
//   3 radar    — FUN_00412060 spawn: type 3, all pos fields 0, wander
//                target via FUN_004119ec. FUN_00411aac: plane =
//                player.z - 3; marker +0x128 rises 3000*dt to plane,
//                then steers vel = vel*0.75 + dir*0.25 (dir toward
//                wander target at edc10 speed), projected pos follows.
//                Within 2.9412 of the wander point or timer > 30:
//                R_MOVE + retarget. Player proximity <= 15: lock —
//                budget += waveSize + (rand&1), missileTimer = 1,
//                K_SEEN, flash -0.5 clamp 0.75, timer -1 -> sink
//                1500*dt; at z <= 0 re-arm radar timer edc20 +
//                (rand&0x3f) and free.
//   4 pickup   — FUN_004118b0 spawn: model index via FUN_00454794
//                name lookup, type 4, vel {0,0,-133.333}, pos random
//                in +-56.38/+-33.53 at player.z + 15, timer
//                (rand&0x3f)+30, P_FALL. FUN_00411710: integrate; timer
//                expiry -> deploy CHUTE (+0x306) + sound; then yaw
//                spin 30*dt, vz brakes toward -50 at 66.6667*dt;
//                z > camZ -> free; FUN_0045c230 hit -> P_COLL + K_COLL
//                + FUN_0046a9c8 grant + free.
//   5 bones    — FUN_0041236c: anim tick, z -= 74.074*dt, one-shot
//                pass sound the frame it overtakes the player.
//
// Difficulty block (OBSERVED, c = course, s = skill; integer div):
//   s=0: wave = c/5+2, speed = K*(1+c*0.1), wander = 7.5-c,
//        delay = 32-c,   radarDelay = 63-3c
//   s=1: wave = c/3+2, speed = K*(1+c*0.2), wander = 6.5-c,
//        delay = 32-7c,  radarDelay = 63-7c   (K = 117.64705882352942)
//   s=2: wave = c/2+2, speed = K*(1+c/3.0), wander = 5.5-c,
//        delay = 32-5c,  radarDelay = 63-9c
//
// Deferred presentation seams (emitted as FreefallEvent, never
// emulated): all sounds (R_START/R_MOVE/K_SEEN/K_FINISH/M_LNCH/M_PASS/
// BONES/P_FALL/P_COLL/K_COLL/K_HIT/K_GRUNT/WINDLOOP), fades/palette,
// zoom sprite indices (kept as state), the +0x60 trail FX, model
// basis matrices, and the FUN_0046a9c8 grant (reported as
// grantHealth/grantAmmo/grantKey events).

#ifndef MDK_CORE_FREEFALL_RUNTIME_H
#define MDK_CORE_FREEFALL_RUNTIME_H

#include <array>
#include <cstdint>
#include <vector>

namespace mdk {

// ---------------------------------------------------------------------------
// Course data — everything the runtime needs that the original reads
// from FALL3D.BNI / the globals 0x541498/0x54147a. Pickup records are
// the FALLPU_%d 12-byte entries ({name[8], u32}) minus the terminator.
// ---------------------------------------------------------------------------

struct FreefallPickupRec {
  char name[9];  // 8-char name + NUL (source record is name[8] + u32)
};

struct FreefallCourseData {
  int course = 0;               // 0x541498 (0..4)
  int skill = 0;                // 0x54147a (0..2)
  std::vector<FreefallPickupRec> pickups;  // FALLPU_%d, forward order
  // EXPLODE model anims[0] +0xc >> 16 — the type-2 lifetime. Bounded
  // seam: the model anims-table entry layout is not yet decoded, so
  // the caller supplies the frame count (tests use fixtures).
  float explodeAnimFrames = 30.0f;
};

// ---------------------------------------------------------------------------
// Input — the FUN_00407e50 fold. Digital keys win per axis; the analog
// axis applies only when neither digital on that axis is held, and the
// Y analog is negated (OBSERVED quirk).
// ---------------------------------------------------------------------------

struct FreefallInput {
  bool left = false;   // 0x54b650 — negative X
  bool right = false;  // 0x54b654 — positive X
  bool up = false;     // 0x54b658 — positive Y
  bool down = false;   // 0x54b65c — negative Y
  float axisX = 0.0f;  // 0x54b538 analog (-1..1)
  float axisY = 0.0f;  // 0x54b53c analog (negated in the reader)
};

// ---------------------------------------------------------------------------
// Events — the presentation/inventory seams. `kind` plus two ints; for
// sounds `a` is the original SNI slot id tag, for grants the table
// index / amount.
// ---------------------------------------------------------------------------

enum FreefallEventKind : int {
  kFfEvSound = 0,     // a = FreefallSound id, b = channel param
  kFfEvGrantAmmo,     // a = ammo table index (0x49bad4 row), b = amount
  kFfEvGrantKey,      // a = key table index (0x49bba0 row), b = value
  kFfEvGrantHealth,   // a = health-table row, b = amount applied
};

// Sound tags — the 0x4edc3c..0x4edc98 slot block populated by init.
enum FreefallSound : int {
  kFfSndRStart = 0,   // 0x4edc44
  kFfSndRMove,        // 0x4edc48
  kFfSndMPass,        // 0x4edc4c
  kFfSndMLnch,        // 0x4edc50
  kFfSndChute,        // 0x4edc54
  kFfSndPColl,        // 0x4edc58
  kFfSndPFall,        // 0x4edc5c
  kFfSndKHit0,        // 0x4edc3c[0]
  kFfSndKHit1,        // 0x4edc3c[1]
  kFfSndKSeen,        // 0x4edc88
  kFfSndKFinish,      // 0x4edc7c
  kFfSndBones,        // 0x4edc98
  kFfSndExplode,      // 0x4edc60[randBelow(7)] — K_GRUNT/bang group
  kFfSndKColl,        // 0x4edc80[randBelow(2)] — pickup collect
};

struct FreefallEvent {
  int kind;
  int a;
  int b;
};

// ---------------------------------------------------------------------------
// Object record — the gameplay fields of the 0x32e pool record.
// Field comments carry the original offsets.
// ---------------------------------------------------------------------------

enum FreefallModelTag : int {
  kFfModelNone = 0,
  kFfModelKurt,      // 0x4edd48
  kFfModelMissile,   // 0x4ede58
  kFfModelChute,     // 0x4edee0 (pickup +0x306 attachment)
  kFfModelRadar,     // 0x4eddd0
  kFfModelBones,     // 0x4edf68
  kFfModelBang,      // 0x4ee980 (type-2 conversion)
  kFfModelPickup = 100,  // modelKind = kFfModelPickup + model table idx
};

enum FreefallAnimTag : int {
  kFfAnimNone = 0,
  kFfAnimKurt,       // 0x4edae0 KURTANIM
  kFfAnimKurtHit,    // 0x4edae4 KURT_HIT
  kFfAnimBones,      // 0x4edae8 BONESANM
};

struct FreefallObject {
  int32_t next = -1;         // +0x00 link (pool index or -1)
  int16_t type = 0;          // +0x04 — 0..5 dispatch
  int8_t alive = 0;          // +0x06
  int32_t model = kFfModelNone;  // +0x0c model tag
  float px = 0, py = 0, pz = 0;  // +0x10/+0x14/+0x18 position
  float aux0 = 0, aux1 = 0, aux2 = 0;  // +0x1c/+0x20/+0x24 radar
                                       // wander target + plane
  float vx = 0, vy = 0, vz = 0;        // +0x28/+0x2c/+0x30 velocity
  float yaw = 0;             // +0x4c
  float scale = 0;           // +0x58
  int32_t fx = 0;            // +0x60 trail FX presence (seam)
  float animAcc = 0;         // +0xdc anim accumulator
  float animRate = 0;        // +0xe0
  int16_t animFrame = 0;     // +0xe4
  int32_t subTimer = 0;      // +0x108
  int32_t explodeFlag = 0;   // +0x110 (set 1 on hit->explode)
  int32_t animHandle = 0;    // +0x114 FreefallAnimTag
  int16_t animSentinel = 0;  // +0x118 (-256 = clip finished)
  int16_t timer = 0;         // +0x11c
  float tx = 0, ty = 0, tz = 0;  // +0x120/+0x124/+0x128 target
  float roll = 0;            // +0x13c
  uint8_t flags = 0;         // +0x148 — bit3: anim dead/auto-restore
  float prevx = 0, prevy = 0, prevz = 0;  // +0x180.. previous pos
  float aabb[6] = {};        // +0x198..1ac {minx,miny,minz,maxx,maxy,maxz}
  int32_t pickupRec = -1;    // +0x302 FALLPU record index
  int32_t chute = 0;         // +0x306 CHUTE model attached
};

// ---------------------------------------------------------------------------
// Runtime — mirrors the original globals. Step semantics are the
// original frame-unit model: `frameStep` = 0x49b6e8 integer timer
// decrement, `frameUnits` = 0x49b6f0 smoothed frame units, `dtSec` =
// 0x49b6f4 seconds delta.
// ---------------------------------------------------------------------------

struct FreefallRuntime {
  enum class Phase : int {
    kIntro = 0,     // 0x4edcb8 > 0
    kPlay,          // main tick, health > 0
    kDead,          // death fade running
    kDone,          // returned 1 (finished or death-fade end)
  };

  Phase phase = Phase::kIntro;
  bool finished = false;    // returned 1 via edc00 > 33
  bool died = false;        // returned 1 via death fade

  int32_t introCountdown = 150;  // 0x4edcb8
  float introProgress = 0;       // 1 - edcb8/150 (fade param)
  int32_t zoomFrame = 0;         // 0x4edbf0 (0..15 sprite index)
  int32_t zoomSub = 0xc00;       // 0x4edbf4 fixed-point sub-frame
  float timeline = 0;            // 0x4edc00 seconds
  float prevTimeline = 0;        // pre-increment snapshot (0x54b584-local)
  float camPrevTimeline = 0;     // 0x4ce6a8 (K_FINISH edge)
  int32_t health = 100;          // 0x541554
  float fade = 0;                // 0x4edc04
  float fadeTarget = 1.0f;       // 0x4edc0c
  float fadeRate = 100000.0f;    // 0x4edc08
  float palette = 0;             // 0x4edb5c (palette-cycle accumulator)

  int32_t radarTimer = 0;        // 0x4edcac
  int32_t pickupTimer = 0;       // 0x4edca8
  int32_t missileTimer = 0;      // 0x4edcb4
  int32_t missileBudget = 0;     // 0x4edcb0
  int32_t pickupsRemaining = 0;  // 0x4edca4

  float radarSpeed = 0;          // 0x4edc10
  float wanderScale = 0;         // 0x4edc14
  int32_t waveSize = 0;          // 0x4edc18
  int32_t missileDelay = 0;      // 0x4edc1c
  int32_t radarDelay = 0;        // 0x4edc20

  float zoomRate = -66.6667f;    // 0x4edbfc
  float camZ = 0;                // 0x4ce6a4
  float camX = 0, camY = 0;      // 0x4ce69c/0x4ce6a0
  float cameraPos[3] = {};       // 0x540b28/2c/30 (projection writer)

  uint32_t rng = 0;              // FUN_0047d2b5 MSVC LCG state
  bool bonesCourse = false;      // 0x4edaf4 (course >= 4)
  bool bonesPassLatch = false;   // 0x4edaf8
  bool finishLatch = false;      // K_FINISH fired

  int32_t course = 0;
  int32_t skill = 0;
  float explodeAnimFrames = 30.0f;
  std::vector<FreefallPickupRec> pickups;

  std::array<FreefallObject, 399> pool{};  // 0x4f0740 pool
  int32_t freeHead = -1;                   // 0x540ed0 freelist
  int32_t listHead = -1;                   // 0x4edaec (player anchor)
  int32_t bonesIdx = -1;                   // 0x4edaf0

  std::vector<FreefallEvent> events;
};

// Course init — FUN_0040ef28 gameplay subset: clears the pool, chains
// the freelist, computes the difficulty block, arms the intro
// countdown and (deferred) timers. `seed` loads the LCG state.
void freefallInit(FreefallRuntime& rt, const FreefallCourseData& data,
                  std::uint32_t seed);

// One mode-2 frame — FUN_004103d8. Returns true when the original
// returns 1 (completion or death-fade end); inspect `rt.finished` /
// `rt.died` to distinguish. `input` folds through FUN_00407e50.
bool freefallStep(FreefallRuntime& rt, const FreefallInput& input,
                  int frameStep, float frameUnits, float dtSec);

// The FUN_00407e50 input fold, exposed for tests: fills the four
// channel values {rateX, capX, rateY, capY}.
void freefallInputFold(const FreefallInput& in, float out[4]);

} // namespace mdk

#endif // MDK_CORE_FREEFALL_RUNTIME_H
