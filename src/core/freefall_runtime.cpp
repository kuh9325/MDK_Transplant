// freefall_runtime.cpp — Phase 13A: native FALL3D/freefall runtime.
// See freefall_runtime.h for the original-ownership map and the
// OBSERVED mechanics this ports. Frame-unit model: frameStep =
// 0x49b6e8 (int timers), frameUnits = 0x49b6f0 (smoothed units),
// dtSec = 0x49b6f4 (seconds).

#include "core/freefall_runtime.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "core/enemy_runtime.h"  // enemyRandNext / enemyRandBelow (FUN_0047d2b5/0x401ed4)
#include "core/motion_channels.h"
#include "core/player_fire.h"    // segClipAabb (FUN_0045c230)

namespace mdk {
namespace {

// -------------------------------------------------------------------------
// Constants — OBSERVED doubles/floats decoded at the noted addresses.
// -------------------------------------------------------------------------
constexpr float kFallSpeed = -66.6666667f;      // 0x494f00
constexpr float kBonesSpeed = -74.0740741f;     // 0x494f60
constexpr float kControlSecs = 30.0f;           // 0x494f08
constexpr float kCompleteSecs = 33.0f;          // 0x494d10
constexpr float kExitFadeStart = 31.0f;         // 0x494cc8
constexpr float kAccelRate = 11.7647059f;       // 0x494578
constexpr float kAccelCap = 117.647059f;        // 0x494580
constexpr float kClampX = 58.8235294f;          // 0x494f20/28
constexpr float kClampY = 35.2941176f;          // 0x494f30/38
constexpr float kAabbX = 4.0f;                  // 0x494f40/48
constexpr float kAabbY = 5.0f;                  // 0x494f50
constexpr float kAabbZ = 5.0f;                  // 0x494f58
constexpr float kSpringK = 2.0f;                // 0x494f10
constexpr float kSpringDamp = 0.5f;             // 0x494f18
constexpr float kCamFactor = 0.85f;             // 0x494ce0
constexpr float kCamOffZ = 10.0f;               // 0x494cf0
constexpr float kZoomAccel = 33.3333333f;       // 0x494cf8 = -33.3333;
                                                // FSUBR negates it:
                                                // edbfc += dt*33.3333
constexpr float kZoomIdxK = -0.015f;            // 0x494d00
constexpr float kZoomIdxScale = 3072.0f;        // 0x494d08
constexpr float kMissileSpeed = 250.0f;         // 0x494d90 / 0x494dc8
constexpr float kMissileThrottle = 0.75f;       // 0x494d58
constexpr float kMissileBoost = 3.0f;           // 0x494d60 (flt)
constexpr float kLeadPerUnit = 1.0f / 225.0f;   // 0x494d78
constexpr float kLeadClamp = 10.0f;             // 0x494d80
constexpr float kLeadClamped = -666.6667f;      // 0xc426aaab imm
constexpr float kHomeK1 = 0.8f;                 // 0x494d98
constexpr float kHomeK2 = 0.2f;                 // 0x494da0
constexpr float kPassDz = -5.0f;                // 0x494d70
constexpr float kCollideSecs = 30.0f;           // 0x494d68
constexpr int16_t kMissileLaunch = 60;          // imm 0x3c
constexpr int16_t kMissileLinger = -60;         // imm -0x3c
constexpr float kPickupFall = -133.333333f;     // 0xc3055555 imm
constexpr float kPickupGlide = -50.0f;          // 0x494de8
constexpr float kPickupBrake = 66.6666667f;     // 0x494df0
constexpr float kPickupSpin = 30.0f;            // 0x494de0
constexpr float kPickupSpawnZ = 15.0f;          // 0x494e40
constexpr float kPickupRangeX = 58.8235294f * 0.95f;  // 0x494e20*0x494e28
constexpr float kPickupRangeY = 35.2941176f * 0.95f;  // 0x494e38*0x494e28
constexpr float kRadarPlaneOff = -3.0f;         // 0x494e60
constexpr float kRadarRise = 3000.0f;           // 0x494e90
constexpr float kRadarSink = 1500.0f;           // 0x494e98
constexpr float kRadarBlend1 = 0.75f;           // 0x494e68
constexpr float kRadarBlend2 = 0.25f;           // 0x494e70
constexpr float kWanderBox = 2.9411765f;        // 0x494e78
constexpr float kLockDistSq = 225.0f;           // 0x494e80
constexpr float kLockFlash = -0.5f;             // 0x494e88
constexpr float kLockFlashMin = 0.75f;
constexpr int16_t kWanderRetarget = 30;         // imm 0x1e
constexpr float kWanderX = 58.8235294f;         // 0x494ea0/0x494e48
constexpr float kWanderY = 35.2941176f;         // 0x494eb0/0x494e58
constexpr float kIntroScale = 1.0f / 150.0f;    // 0x494c98
constexpr float kFadeScale = 1.0f / 60.0f;      // 0x494c9c
constexpr float kSpawnArcX0 = -30.0f;           // 0x494cb8
constexpr float kSpawnArcX = 30.0f;             // 0x494ca0
constexpr float kSpawnArcY0 = -10.0f;           // 0x494cb0
constexpr float kSpawnArcY = 10.0f;             // 0x494ca8
constexpr float kSpawnArcZ = -10.0f;
constexpr float kPlayerSpawnZ = 5206.0f;        // 0x45a4b000 imm
constexpr float kBonesSpawnZ = 5290.0f;         // 0x45a55000 imm
constexpr float kExplodeScale = 2.0f;           // 0x494dd8
constexpr float kFlashHit = 3.0f;               // 0x40400000 imm
constexpr float kFlashMissile = -0.2f;          // 0x494cd8
constexpr float kFlashMissileMin = 0.5f;        // 0x494cc0 (0x3f000000)
constexpr float kRandUnit = 6.103515625e-5f;    // 1/16384
constexpr int16_t kKurtHitFrames = 18;          // KURT_HIT anim frame
                                                // count (BNI census)

// FUN_0047d59a — the x87 rint used for the explosion frame index.
inline float ffRound(float v) { return std::nearbyint(v); }

inline void emit(FreefallRuntime& rt, int kind, int a, int b) {
  rt.events.push_back({kind, a, b});
}
inline void emitSound(FreefallRuntime& rt, int id, int ch = 0) {
  emit(rt, kFfEvSound, id, ch);
}

int objIndex(const FreefallRuntime& rt, const FreefallObject& o) {
  return static_cast<int>(&o - rt.pool.data());
}

// FUN_0040f96c — LIFO freelist pop + insert after list head. Returns
// the pool index or -1 on exhaustion (the original sets the quit flag;
// headless: the caller just gets -1).
int allocObj(FreefallRuntime& rt) {
  if (rt.freeHead < 0) return -1;
  const int idx = rt.freeHead;
  FreefallObject& o = rt.pool[idx];
  rt.freeHead = o.next;
  const int head = rt.listHead;
  if (head >= 0) {
    o.next = rt.pool[head].next;
    rt.pool[head].next = idx;
  } else {
    o.next = -1;
  }
  return idx;
}

// FUN_0040f9d0 — unlink from the active list + LIFO freelist push.
// Only the link field is rewritten; every other field stays stale
// (OBSERVED: reused records inherit prior field values — e.g. the
// missile spawner never writes position, so a fresh record launches
// from {0,0,0} and a reused one from wherever it died).
void freeObj(FreefallRuntime& rt, int idx) {
  if (idx < 0) return;
  int* link = &rt.listHead;
  while (*link >= 0) {
    if (*link == idx) {
      *link = rt.pool[idx].next;
      break;
    }
    link = &rt.pool[*link].next;
  }
  rt.pool[idx].next = rt.freeHead;
  rt.freeHead = idx;
  if (rt.bonesIdx == idx) rt.bonesIdx = -1;
}

// The list-head anchor: the original stores the player record pointer
// in 0x4edaec and treats it as both list head and player handle.
inline FreefallObject* player(FreefallRuntime& rt) {
  return rt.listHead >= 0 ? &rt.pool[rt.listHead] : nullptr;
}

void radarRetarget(FreefallRuntime& rt, FreefallObject& o);
void explodeTick(FreefallRuntime& rt, FreefallObject& o, float frameUnits);
void applyPickup(FreefallRuntime& rt, int recIdx);

// -------------------------------------------------------------------------
// Spawns — FUN_0040ff78 (player/bones), FUN_0041151c (missile),
// FUN_004118b0 (pickup), FUN_00412060 (radar). Each writes only the
// fields the original writes; everything else inherits the record's
// stale values (fresh records are zero — OBSERVED pool wipe at init).
// -------------------------------------------------------------------------

void spawnPlayer(FreefallRuntime& rt) {
  const int idx = allocObj(rt);
  if (idx < 0) return;
  rt.listHead = idx;  // 0x4edaec — the head IS the player
  FreefallObject& o = rt.pool[idx];
  o.alive = 1;
  o.yaw = 90.0f;         // 0x42b40000
  o.roll = -90.0f;       // 0xc2b40000
  o.scale = 1.0f;
  o.model = kFfModelKurt;
  o.type = 0;
  o.animFrame = -1;
  o.animAcc = -1.0f;
  o.animHandle = kFfAnimKurt;
  o.animSentinel = -1;
  o.animRate = 30.0f;    // 0x41f00000
  o.flags |= 0x8;
}

void spawnBones(FreefallRuntime& rt) {
  const int idx = allocObj(rt);
  if (idx < 0) return;
  rt.bonesIdx = idx;
  FreefallObject& o = rt.pool[idx];
  o.type = 5;
  o.alive = 1;
  o.yaw = 90.0f;
  o.roll = -90.0f;
  o.scale = 1.0f;
  o.model = kFfModelBones;
  o.animFrame = -1;
  o.animAcc = 0.0f;
  o.animHandle = kFfAnimBones;
  o.animSentinel = -2;   // 0xfffe
  o.animRate = 30.0f;
  o.pz = kBonesSpawnZ;
  o.flags |= 0x8;
}

void spawnMissile(FreefallRuntime& rt) {
  const int idx = allocObj(rt);
  if (idx < 0) return;
  FreefallObject& o = rt.pool[idx];
  o.model = kFfModelMissile;
  o.type = 1;
  o.alive = 1;
  o.scale = 1.0f;
  const float dir = static_cast<float>(enemyRandNext(rt.rng)) *
                    (360.0f / 32768.0f);              // rand*360/32768
  const float rad = dir * (3.14159265358979323846f / 180.0f);
  o.vx = std::sin(rad) * kMissileSpeed;
  o.vy = std::cos(rad) * kMissileSpeed;
  o.vz = kMissileSpeed;                               // 0x437a0000
  o.timer = kMissileLaunch;
  o.tx = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
         rt.wanderScale * kRandUnit;                  // +0x120 lead x
  o.ty = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
         rt.wanderScale * kRandUnit;                  // +0x124 lead y
  o.tz = 0.0f;                                        // +0x128
  o.fx = 1;                                           // +0x60 trail seam
  o.subTimer = 0;
  emitSound(rt, kFfSndMLnch);
}

void spawnPickup(FreefallRuntime& rt, int recIdx) {
  const int idx = allocObj(rt);
  if (idx < 0) return;
  FreefallObject& o = rt.pool[idx];
  // FUN_00454794 name->model index; the tag keeps the record index so
  // a later resolver can map it back onto the roster.
  o.model = kFfModelPickup + recIdx;
  o.type = 4;
  o.alive = 1;
  o.scale = 1.0f;
  o.vx = 0.0f;
  o.vy = 0.0f;
  o.vz = kPickupFall;
  o.px = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
         kPickupRangeX * kRandUnit;
  o.py = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
         kPickupRangeY * kRandUnit;
  const FreefallObject* pl = player(rt);
  o.pz = (pl ? pl->pz : 0.0f) + kPickupSpawnZ;
  o.timer = static_cast<int16_t>((enemyRandNext(rt.rng) & 0x3f) + 30);
  o.pickupRec = recIdx;                               // +0x302
  emitSound(rt, kFfSndPFall);
}

void spawnRadar(FreefallRuntime& rt) {
  const int idx = allocObj(rt);
  if (idx < 0) return;
  FreefallObject& o = rt.pool[idx];
  o.model = kFfModelRadar;
  o.type = 3;
  o.alive = 1;
  o.scale = 1.0f;
  o.px = o.py = o.pz = 0.0f;
  o.tx = o.ty = o.tz = 0.0f;
  o.aux2 = 0.0f;
  emitSound(rt, kFfSndRStart);
  radarRetarget(rt, o);  // FUN_004119ec — initial wander target
}

// FUN_004119ec — collect up to 12 candidate {x,y} points from live
// type-0/type-4 objects, pad with up to 3 random full-bounds points,
// then randBelow() picks the wander target.
void radarRetarget(FreefallRuntime& rt, FreefallObject& o) {
  float cand[12][2];
  int count = 0;
  for (int i = rt.listHead; i >= 0 && count < 12; i = rt.pool[i].next) {
    const FreefallObject& c = rt.pool[i];
    if (c.type == 0 || c.type == 4) {
      cand[count][0] = c.px;
      cand[count][1] = c.py;
      ++count;
    }
  }
  int added = 0;
  while (count < 12 && added < 3) {
    cand[count][0] = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
                     kWanderX * kRandUnit;
    cand[count][1] = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
                     kWanderY * kRandUnit;
    ++count;
    ++added;
  }
  const int pick = enemyRandBelow(rt.rng, count);
  o.aux0 = cand[pick][0];  // +0x1c
  o.aux1 = cand[pick][1];  // +0x20
}

// -------------------------------------------------------------------------
// Type handlers — dispatch order of the 0x410e20 table.
// -------------------------------------------------------------------------

// Type 0 — FUN_0041210c.
void playerTick(FreefallRuntime& rt, FreefallObject& o,
                const float chan[4], float frameUnits, float dtSec) {
  // Hit-anim restore gate: +0x118 == -256 (clip finished) or no handle.
  if (o.animSentinel == -256 || o.animHandle == 0) {
    o.animSentinel = -1;
    o.animAcc = -1.0f;
    o.animHandle = kFfAnimKurt;
    o.animFrame = -1;
    o.flags |= 0x8;
  }
  // FUN_004555bc — anim sequencer seam. Modeled subset: the clip
  // accumulator advances in frame units; a latched KURT_HIT expires
  // into the -256 sentinel after its frame count (18 — BNI census).
  o.animAcc += frameUnits;
  if (o.animHandle == kFfAnimKurtHit && o.animAcc >= kKurtHitFrames) {
    o.animSentinel = -256;
  }

  o.pz += dtSec * kFallSpeed;
  // FUN_00469cd0 — weapon-slot scan seam (no freefall effect).

  if (rt.timeline > kControlSecs) {
    // Landing phase — spring-damp to center (OBSERVED coefficients);
    // the K_FINISH edge reads the pre-update 0x4ce6a8.
    if (rt.camPrevTimeline < kControlSecs) {
      emitSound(rt, kFfSndKFinish, 1);
    }
    o.vx = (o.vx - o.px * kSpringK) * kSpringDamp;
    o.vy = (o.vy - o.py * kSpringK) * kSpringDamp;
  } else {
    // The original tests (bits & 0x7fffffff) — a raw-magnitude check.
    const uint32_t bx = std::bit_cast<uint32_t>(chan[0]) & 0x7fffffffu;
    const uint32_t by = std::bit_cast<uint32_t>(chan[2]) & 0x7fffffffu;
    if (bx != 0) {
      accelChannel(o.vx, chan[0], chan[1], frameUnits);
    } else {
      linearDecay(o.vx, kAccelRate, frameUnits);
    }
    if (by != 0) {
      accelChannel(o.vy, chan[2], chan[3], frameUnits);
    } else {
      linearDecay(o.vy, kAccelRate, frameUnits);
    }
  }

  o.px += o.vx * dtSec;
  if (o.px < -kClampX) {
    o.px = -kClampX;
    o.vx = 0.0f;
  } else if (o.px > kClampX) {
    o.px = kClampX;
    o.vx = 0.0f;
  }
  o.py += o.vy * dtSec;
  if (o.py < -kClampY) {
    o.py = -kClampY;
    o.vy = 0.0f;
  } else if (o.py > kClampY) {
    o.py = kClampY;
    o.vy = 0.0f;
  }
  // +0x198 AABB — half-extents {4, 5, 5}.
  o.aabb[0] = o.px - kAabbX;
  o.aabb[3] = o.px + kAabbX;
  o.aabb[1] = o.py - kAabbY;
  o.aabb[4] = o.py + kAabbY;
  o.aabb[2] = o.pz - kAabbZ;
  o.aabb[5] = o.pz + kAabbZ;
  rt.camPrevTimeline = rt.timeline;  // 0x4ce6a8
}

// Type 1 — FUN_00410e9c.
void missileTick(FreefallRuntime& rt, FreefallObject& o,
                 float frameUnits, float dtSec, int frameStep) {
  (void)frameUnits;
  o.prevx = o.px;
  o.prevy = o.py;
  o.prevz = o.pz;
  o.px += o.vx * dtSec;
  o.py += o.vy * dtSec;
  o.pz += o.vz * dtSec;
  FreefallObject* pl = player(rt);
  if (pl && o.pz < pl->pz * kMissileThrottle) {
    o.pz += o.vz * kMissileBoost * dtSec;  // z-throttle catch-up
  }

  if (pl && rt.timeline < kCollideSecs) {
    // FUN_0045c230 — segment prevPos->pos vs the player AABB.
    const float seg[3] = {o.prevx, o.prevy, o.prevz};
    const float tgt[3] = {o.px, o.py, o.pz};
    float hit[3];
    if (segClipAabb(seg, tgt, pl->aabb, hit) != 0) {
      o.px = hit[0];
      o.py = hit[1];
      o.pz = hit[2];
      pl->animSentinel = -1;
      pl->animAcc = -1.0f;
      pl->animFrame = -1;
      pl->animHandle = kFfAnimKurtHit;
      pl->flags &= ~0x8;
      if (rt.health > 0) {
        if (rt.skill == 0) {
          rt.health -= 4;
        } else {
          rt.health -= 4 + enemyRandBelow(rt.rng, 8);
          if (rt.skill == 2) rt.health -= 4 + enemyRandBelow(rt.rng, 8);
        }
        rt.fade = kFlashHit;  // damage flash
        if (rt.health < 0) {
          rt.health = 0;
          pl->flags |= 0x8;
        }
      }
      emitSound(rt, static_cast<int>(enemyRandNext(rt.rng) & 1)
                        ? kFfSndKHit1
                        : kFfSndKHit0,
                1);
      emitSound(rt, kFfSndExplode, enemyRandBelow(rt.rng, 7));
      // convert to the type-2 explosion at the player position.
      o.type = 2;
      o.alive = 1;
      o.model = kFfModelBang;
      o.px = pl->px;
      o.py = pl->py;
      o.pz = pl->pz;
      o.roll = 90.0f;
      o.scale = 1.0f;
      o.yaw = static_cast<float>(enemyRandNext(rt.rng)) * (360.0f / 32768.0f);
      o.animHandle = 0;
      o.animFrame = -1;
      o.animAcc = -1.0f;
      o.explodeFlag = 1;
      explodeTick(rt, o, frameUnits);
      return;
    }
  }

  if (o.timer < 0) {
    // post-pass linger: counts down by frameStep to -60 then frees.
    o.timer = static_cast<int16_t>(o.timer - frameStep);
    if (o.timer < kMissileLinger) freeObj(rt, objIndex(rt, o));
    return;
  }
  if (o.timer > 0) {
    // launch phase: +0x108 ramps to 8, timer 60 -> 0.
    if (o.subTimer != 8) ++o.subTimer;
    o.timer = static_cast<int16_t>(o.timer - frameStep);
    if (o.timer > 0) return;
    o.timer = 0;
    return;
  }
  // homing — +0x108 counts back down (OBSERVED write, no readers).
  if (o.subTimer != 0) --o.subTimer;
  if (!pl) return;
  const float dz = pl->pz - o.pz;
  if (dz <= kPassDz) {
    emitSound(rt, kFfSndMPass);
    o.timer = -1;
    return;
  }
  const float lead = dz * kLeadPerUnit;
  o.tz = (lead < kLeadClamp) ? lead * kFallSpeed : kLeadClamped;
  const float tx = pl->px + o.tx - o.px;
  const float ty = pl->py + o.ty - o.py;
  const float tz = pl->pz + o.tz - o.pz;
  const float distSq = tx * tx + ty * ty + tz * tz;
  if (distSq == 0.0f) return;
  const float s = kMissileSpeed / std::sqrt(static_cast<double>(distSq));
  o.vx = o.vx * kHomeK1 + tx * s * kHomeK2;
  o.vy = o.vy * kHomeK1 + ty * s * kHomeK2;
  o.vz = o.vz * kHomeK1 + tz * s * kHomeK2;
}

// Type 2 — FUN_00411660.
void explodeTick(FreefallRuntime& rt, FreefallObject& o, float frameUnits) {
  const FreefallObject* pl = player(rt);
  if (pl) o.pz = pl->pz;
  o.animAcc += frameUnits;
  if (o.animAcc >= rt.explodeAnimFrames) {
    freeObj(rt, objIndex(rt, o));
    return;
  }
  o.animFrame = static_cast<int16_t>(ffRound(o.animAcc));
  o.scale = static_cast<float>(o.animFrame) / kExplodeScale;
}

// Type 3 — FUN_00411aac.
void radarTick(FreefallRuntime& rt, FreefallObject& o,
               float frameUnits, float dtSec, int frameStep) {
  (void)frameUnits;
  const FreefallObject* pl = player(rt);
  o.aux2 = (pl ? pl->pz : 0.0f) + kRadarPlaneOff;  // +0x24 plane
  if (o.timer < 0) {
    // sink phase — despawn below z 0, re-arm the radar timer.
    o.tz -= kRadarSink * dtSec;
    if (o.tz <= 0.0f) {
      rt.radarTimer = rt.radarDelay +
                      static_cast<int>(enemyRandNext(rt.rng) & 0x3f);
      freeObj(rt, objIndex(rt, o));
    }
    return;
  }
  if (o.tz < o.aux2) {
    o.tz += kRadarRise * dtSec;
    if (o.tz <= o.aux2) {
      // still rising: projected marker pos follows the wander target.
      if (o.aux2 != 0.0f) {
        o.tx = o.aux0 * o.tz / o.aux2;
        o.ty = o.aux1 * o.tz / o.aux2;
      }
      return;
    }
    // reached the plane — snap + fresh random wander target.
    o.tx = o.aux0;
    o.ty = o.aux1;
    o.tz = o.aux2;
    o.aux0 = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
             kWanderX * kRandUnit;
    o.aux1 = (static_cast<int>(enemyRandNext(rt.rng)) - 0x4000) *
             kWanderY * kRandUnit;
    return;
  }
  o.tz = o.aux2;
  const float dx = o.aux0 - o.tx;
  const float dy = o.aux1 - o.ty;
  const float distSq = dx * dx + dy * dy;
  float ddx = 0.0f, ddy = 0.0f;
  if (distSq != 0.0f) {
    const float s = rt.radarSpeed / std::sqrt(distSq);
    ddx = dx * s;
    ddy = dy * s;
  }
  o.vx = o.vx * kRadarBlend1 + ddx * kRadarBlend2;
  o.vy = o.vy * kRadarBlend1 + ddy * kRadarBlend2;
  o.prevx = o.aux0;
  o.prevy = o.aux1;
  o.prevz = o.aux2;
  o.tx += o.vx * dtSec;
  o.ty += o.vy * dtSec;
  o.timer = static_cast<int16_t>(o.timer + frameStep);  // counts UP
  const bool nearWander =
      std::fabs(o.tx - o.aux0) < kWanderBox &&
      std::fabs(o.ty - o.aux1) < kWanderBox;
  if (nearWander || o.timer > kWanderRetarget) {
    emitSound(rt, kFfSndRMove, 1);
    o.timer = 0;
    o.tx = o.aux0;
    o.ty = o.aux1;
    o.tz = o.aux2;
    radarRetarget(rt, o);
  }
  // player proximity — the lock.
  if (pl) {
    const float pdx = pl->px - o.tx;
    const float pdy = pl->py - o.ty;
    if (pdx * pdx + pdy * pdy < kLockDistSq) {
      o.aux0 = o.tx;
      o.aux1 = o.ty;
      o.aux2 = o.tz;
      o.timer = -1;
      rt.fadeRate = kFlashHit;           // 0x40400000 — flash rate 3.0
      rt.fadeTarget += kLockFlash;
      if (rt.fadeTarget < kLockFlashMin) rt.fadeTarget = kLockFlashMin;
      rt.missileBudget += rt.waveSize +
                          static_cast<int>(enemyRandNext(rt.rng) & 1);
      rt.missileTimer = 1;
      emitSound(rt, kFfSndKSeen, 0);
    }
  }
}

// Type 4 — FUN_00411710.
void pickupTick(FreefallRuntime& rt, FreefallObject& o,
                float frameUnits, float dtSec, int frameStep) {
  (void)frameUnits;
  o.prevx = o.px;
  o.prevy = o.py;
  o.prevz = o.pz;
  o.px += o.vx * dtSec;
  o.py += o.vy * dtSec;
  o.pz += o.vz * dtSec;
  if (o.chute != 0) o.yaw += dtSec * kPickupSpin;
  o.timer = static_cast<int16_t>(o.timer - frameStep);
  if (o.timer > 0) return;
  o.timer = 0;
  if (o.chute == 0) {
    o.chute = kFfModelChute;   // +0x306 = CHUTE model attach
    emitSound(rt, kFfSndChute);
    // falls through to the glide block the same frame (OBSERVED).
  }
  if (o.vz < kPickupGlide) {
    o.vz += kPickupBrake * dtSec;
    if (o.vz > kPickupGlide) o.vz = kPickupGlide;
  }
  if (o.pz > rt.cameraPos[2]) {
    freeObj(rt, objIndex(rt, o));
    return;
  }
  const FreefallObject* pl = player(rt);
  if (!pl) return;
  const float seg[3] = {o.prevx, o.prevy, o.prevz};
  const float tgt[3] = {o.px, o.py, o.pz};
  float hit[3];
  if (segClipAabb(seg, tgt, pl->aabb, hit) != 0) {
    emitSound(rt, kFfSndPColl);
    emitSound(rt, kFfSndKColl, enemyRandBelow(rt.rng, 2));
    applyPickup(rt, o.pickupRec);          // FUN_0046a9c8
    freeObj(rt, objIndex(rt, o));
  }
}

// Type 5 — FUN_0041236c.
void bonesTick(FreefallRuntime& rt, FreefallObject& o, float frameUnits,
               float dtSec) {
  o.animAcc += frameUnits;  // FUN_004555bc seam — clip accumulator
  o.pz += dtSec * kBonesSpeed;
  const FreefallObject* pl = player(rt);
  if (pl && o.pz < pl->pz && !rt.bonesPassLatch) {
    emitSound(rt, kFfSndBones, 1);
    rt.bonesPassLatch = true;
  }
}

// FUN_0046a9c8 — pickup name -> grant. Table 1 (0x49bad4) is the
// health/ammo/inventory list, table 2 (0x49bba0) the key/seal list.
// Headless port emits the grant events; the observed health cases
// (rows 5..9) also apply to the runtime health.
void applyPickup(FreefallRuntime& rt, int recIdx) {
  static const char* const kGrantTable[] = {
      "SW_HOME",  "SW_SGREN", "SW_HGREN", "SW_LGREN", "SW_BONES",
      "SW_H25",   "SW_H50",   "SW_H100",  "SW_H150",  "SW_H01",
      "SW_EWJ",   "BONEFLC",
  };
  static const char* const kKeyTable[] = {
      "SW_DUMMY", "SW_INTER", "SW_TWIST", "SW_THUMP", "SW_HBOMB",
      "SW_GATT",  "SW_KEY",   "SW_SEAL",  "SW_SBONE",
  };
  if (recIdx < 0 || recIdx >= static_cast<int>(rt.pickups.size())) return;
  const FreefallPickupRec& rec = rt.pickups[recIdx];
  for (int i = 0; i < 12; ++i) {
    if (std::strncmp(rec.name, kGrantTable[i], 8) == 0) {
      switch (i) {
        case 5: {  // SW_H25 +10 cap 100
          rt.health += 10;
          if (rt.health > 100) rt.health = 100;
          emit(rt, kFfEvGrantHealth, i, 10);
          return;
        }
        case 6: {  // SW_H50 +50 cap 100
          rt.health += 50;
          if (rt.health > 100) rt.health = 100;
          emit(rt, kFfEvGrantHealth, i, 50);
          return;
        }
        case 7:
        case 8: {  // SW_H100 / SW_H150 — set 100
          rt.health = 100;
          emit(rt, kFfEvGrantHealth, i, 100);
          return;
        }
        case 9: {  // SW_H01 +1 cap 100
          rt.health += 1;
          if (rt.health > 100) rt.health = 100;
          emit(rt, kFfEvGrantHealth, i, 1);
          return;
        }
        default: {
          // ammo/inventory rows — the grant amounts live in the shared
          // ammo table; emit the semantic event only.
          emit(rt, kFfEvGrantAmmo, i, 0);
          return;
        }
      }
    }
  }
  for (int i = 0; i < 9; ++i) {
    if (std::strncmp(rec.name, kKeyTable[i], 8) == 0) {
      emit(rt, kFfEvGrantKey, i, 0);
      return;
    }
  }
}

// -------------------------------------------------------------------------
// FUN_0040ff78 — the intro tick.
// -------------------------------------------------------------------------

void introTick(FreefallRuntime& rt, int frameStep, float frameUnits) {
  rt.introProgress =
      1.0f - static_cast<float>(rt.introCountdown) * kIntroScale;
  rt.introCountdown -= frameStep;
  const int t = rt.introCountdown;
  if (t > 90) {
    rt.fade = 1.0f - static_cast<float>(t - 90) * kFadeScale;
  } else if (t >= 60) {
    if (rt.fade != 1.0f) rt.fade = 1.0f;  // palette-apply seam
  } else {
    rt.fade = 1.0f - static_cast<float>(60 - t) * kFadeScale;
  }
  // zoom sprite block (render seam; index state kept for snapshots).
  if (t < 120) {
    rt.zoomSub = (t <= 60) ? 0xc00 : (((120 - t) * 12) / 60) << 8;
    if (++rt.zoomFrame >= 16) rt.zoomFrame = 0;
  }
  if (t < 90) {
    FreefallObject* pl = player(rt);
    if (!pl) {
      spawnPlayer(rt);
      pl = player(rt);
    }
    if (pl) {
      // FUN_004555bc seam — the clip accumulator advances per frame.
      float tt = static_cast<float>(90 - t) * kFadeScale;
      if (tt > 1.0f) tt = 1.0f;
      const float s = 1.0f - (1.0f - tt) * (1.0f - tt);
      pl->px = s * kSpawnArcX + kSpawnArcX0;
      pl->py = s * kSpawnArcY + kSpawnArcY0;
      pl->pz = s * kSpawnArcZ;
      pl->animAcc += frameUnits;
    }
  }
  if (t <= 0) {
    rt.introCountdown = 0;
    rt.phase = FreefallRuntime::Phase::kPlay;
    rt.radarTimer = static_cast<int>(enemyRandNext(rt.rng) & 0xf) + 7;
    if (rt.pickupsRemaining > 0) {
      rt.pickupTimer =
          static_cast<int>(enemyRandNext(rt.rng) & 0x1f) + 0x1f;
    }
    FreefallObject* pl = player(rt);
    if (pl) pl->pz = kPlayerSpawnZ;
    rt.zoomSub = 0xc00;
    if (rt.bonesCourse) spawnBones(rt);
  }
}

} // namespace

// ---------------------------------------------------------------------------

void freefallInputFold(const FreefallInput& in, float out[4]) {
  // FUN_00407e50 — digital wins; analog only when neither digital held.
  if (in.left) {
    out[0] = -kAccelRate;
    out[1] = -kAccelCap;
  } else if (in.right) {
    out[0] = kAccelRate;
    out[1] = kAccelCap;
  } else {
    out[0] = in.axisX * kAccelRate;
    out[1] = in.axisX * kAccelCap;
  }
  if (in.up) {
    out[2] = kAccelRate;
    out[3] = kAccelCap;
  } else if (in.down) {
    out[2] = -kAccelRate;
    out[3] = -kAccelCap;
  } else {
    out[2] = -in.axisY * kAccelRate;   // OBSERVED: Y analog negated
    out[3] = -in.axisY * kAccelCap;
  }
}

void freefallInit(FreefallRuntime& rt, const FreefallCourseData& data,
                  std::uint32_t seed) {
  const int c = data.course;
  const int s = data.skill;
  rt = FreefallRuntime{};  // the original zeroes the whole 0x4f7e0 pool
  rt.rng = seed;
  rt.course = c;
  rt.skill = s;
  rt.pickups = data.pickups;
  rt.explodeAnimFrames = data.explodeAnimFrames;
  rt.health = 100;             // 0x541554 — both entry paths write 100
  rt.pickupsRemaining = static_cast<int32_t>(data.pickups.size());
  rt.bonesCourse = (c >= 4);   // 0x4edaf4

  // 0x4edc10..0x4edc20 — the difficulty block (OBSERVED formulas;
  // course/skill are ints, the divisions are integer).
  const float kSpeed = 117.6470588f;  // 0x494bf0
  switch (s) {
    case 0:
      rt.waveSize = c / 5 + 2;
      rt.radarSpeed = kSpeed * (1.0f + c * 0.1f);
      rt.wanderScale = 7.5f - c;
      rt.missileDelay = 32 - c;
      rt.radarDelay = 63 - 3 * c;
      break;
    case 1:
      rt.waveSize = c / 3 + 2;
      rt.radarSpeed = kSpeed * (1.0f + c * 0.2f);
      rt.wanderScale = 6.5f - c;
      rt.missileDelay = 32 - 7 * c;
      rt.radarDelay = 63 - 7 * c;
      break;
    default:
      rt.waveSize = c / 2 + 2;
      rt.radarSpeed = kSpeed * (1.0f + c / 3.0f);
      rt.wanderScale = 5.5f - c;
      rt.missileDelay = 32 - 5 * c;
      rt.radarDelay = 63 - 9 * c;
      break;
  }

  // freelist chain — FUN_0040ef28 links the pool forward and stores
  // pool[0] as the freelist head (fresh allocs run pool[0], [1], ...).
  for (int i = 0; i < 399; ++i) {
    rt.pool[i].next = (i + 1 < 399) ? i + 1 : -1;
  }
  rt.freeHead = 0;
  rt.listHead = -1;
}

bool freefallStep(FreefallRuntime& rt, const FreefallInput& input,
                  int frameStep, float frameUnits, float dtSec) {
  rt.events.clear();
  if (rt.phase == FreefallRuntime::Phase::kDone) return true;
  if (rt.introCountdown > 0) {
    introTick(rt, frameStep, frameUnits);
    return false;
  }
  rt.phase = FreefallRuntime::Phase::kPlay;

  // audio seam (FUN_0040eeac) + palette cycle accumulator.
  rt.palette += frameUnits * 0.5f;
  while (rt.palette >= 8.0f) rt.palette -= 8.0f;

  rt.prevTimeline = rt.timeline;  // pre-increment snapshot
  // --- fade state machine (0x4edc04/0x4edc0c/0x4edc08) ---
  if (rt.timeline < 1.0f) {
    rt.fade = rt.timeline;
    rt.fadeTarget = 1.0f;
    rt.fadeRate = 100000.0f;
  } else if (rt.health <= 0) {
    rt.phase = FreefallRuntime::Phase::kDead;
    rt.fade -= dtSec;
    if (rt.fade <= 0.0f) {
      rt.fade = 0.0f;
      rt.died = true;
      rt.phase = FreefallRuntime::Phase::kDone;
      return true;
    }
  } else if (rt.prevTimeline > kExitFadeStart) {
    rt.fade = 1.0f - (rt.prevTimeline - kExitFadeStart) * 0.5f;
  } else {
    if (rt.fade != rt.fadeTarget) {
      const float step = rt.fadeRate * dtSec;
      if (rt.fade < rt.fadeTarget) {
        rt.fade += step;
        if (rt.fade > rt.fadeTarget) rt.fade = rt.fadeTarget;
      } else {
        rt.fade -= step;
        if (rt.fade < rt.fadeTarget) rt.fade = rt.fadeTarget;
      }
    } else if (rt.fadeTarget == 1.0f) {
      // ambient dip: ~1/32 chance per frame (OBSERVED rand & 0x1f).
      if ((enemyRandNext(rt.rng) & 0x1f) == 0) {
        rt.fadeTarget = 0.9f;
        rt.fadeRate = 0.5f;
      }
    } else {
      rt.fadeTarget = 1.0f;
      rt.fadeRate = 0.5f;
    }
  }

  // --- three frame-step timers (0x4edcac/0x4edca8/0x4edcb4) ---
  if (rt.radarTimer != 0) {
    rt.radarTimer -= frameStep;
    if (rt.radarTimer <= 0) {
      spawnRadar(rt);
      rt.radarTimer = 0;
    }
  }
  if (rt.pickupTimer != 0) {
    rt.pickupTimer -= frameStep;
    if (rt.pickupTimer <= 0) {
      rt.pickupsRemaining -= 1;
      if (rt.pickupsRemaining >= 0) {
        spawnPickup(rt, rt.pickupsRemaining);
      }
      rt.pickupTimer = (rt.pickupsRemaining == 0)
                           ? 0
                           : static_cast<int>(enemyRandNext(rt.rng) & 0x7f) +
                                 0x1f;
    }
  }
  if (rt.missileTimer != 0) {
    rt.missileTimer -= frameStep;
    if (rt.missileTimer <= 0) {
      spawnMissile(rt);
      rt.fadeTarget += kFlashMissile;
      if (rt.fadeTarget < kFlashMissileMin) rt.fadeTarget = kFlashMissileMin;
      rt.missileBudget -= 1;
      rt.missileTimer = (rt.missileBudget == 0)
                            ? 0
                            : static_cast<int>(enemyRandNext(rt.rng) & 0x1f) +
                                  rt.missileDelay;
    }
  }

  rt.timeline += dtSec;

  // --- FUN_00410e38 object walk ---
  float chan[4];
  freefallInputFold(input, chan);
  for (int i = rt.listHead; i >= 0;) {
    FreefallObject& o = rt.pool[i];
    const int next = o.next;
    switch (o.type) {
      case 0: playerTick(rt, o, chan, frameUnits, dtSec); break;
      case 1: missileTick(rt, o, frameUnits, dtSec, frameStep); break;
      case 2: explodeTick(rt, o, frameUnits); break;
      case 3: radarTick(rt, o, frameUnits, dtSec, frameStep); break;
      case 4: pickupTick(rt, o, frameUnits, dtSec, frameStep); break;
      case 5: bonesTick(rt, o, frameUnits, dtSec); break;
      default: break;
    }
    // handlers free their record themselves; the original returns the
    // saved next link before free() — same walk semantics here.
    i = next;
  }

  // --- camera + zoom-out ---
  const FreefallObject* pl = player(rt);
  rt.camX = (pl ? pl->px : 0.0f) * kCamFactor;
  rt.camY = (pl ? pl->py : 0.0f) * kCamFactor;
  if (rt.timeline > kControlSecs) {
    rt.camZ += rt.zoomRate * dtSec;
    rt.zoomRate += dtSec * kZoomAccel;
    if (rt.zoomRate > 0.0f) rt.zoomRate = 0.0f;
    rt.zoomSub = static_cast<int>(ffRound(rt.zoomRate * kZoomIdxK *
                                          kZoomIdxScale));
  } else {
    rt.camZ = (pl ? pl->pz : 0.0f) + kCamOffZ;
  }
  rt.cameraPos[0] = rt.camX;
  rt.cameraPos[1] = rt.camY;
  rt.cameraPos[2] = rt.camZ;

  if (rt.timeline > kCompleteSecs) {
    rt.finished = true;
    rt.phase = FreefallRuntime::Phase::kDone;
    return true;
  }
  return false;
}

} // namespace mdk
