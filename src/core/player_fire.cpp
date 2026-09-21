// Phase 5N — player weapon fire. See player_fire.h for the evidence
// map and the per-function OBSERVED notes. Everything here is a
// direct port of the BUILD_A disassembly/decompile; the deferred
// work (projectile flight, enemy damage, thrown-object spawn,
// HUD/notify calls) is counted in TraversalSeams, not implemented.

#include "core/player_fire.h"

#include <cmath>
#include <cstring>

#include "core/collision_query.h"
#include "core/dynamic_objects.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

constexpr double kDegToRad = 0.017453292519943276;  // 0x497924
constexpr double kRadToDeg = 57.295779513082323;    // 0x497914
constexpr float kConeFloor = 140.0f;                // 0x4973cc
constexpr float kConeReach = -2.0f;                 // 0x4973d0
constexpr float kConeBase = 90.0f;                  // 0x4973d4
constexpr float kConeWrap = 360.0f;                 // 0x4973d8 / 0x4973dc
constexpr float kConeZWeight = 20.0f;               // 0x4973e0
constexpr float kConeScoreFloor = 10.0f;            // (dist1 floor)
constexpr float kHomeRayLen = 10000.0f;             // 0x4984f0
constexpr float kMissRayLen = 40.0f;                // 0x4973c0
constexpr float kAimZBias = 5.0f;                   // 0x4973b0
constexpr float kClipInit = 1.1f;                   // 0x4982b0
constexpr int kShotPoolSize = 3;                    // 0x540ed4 slots
constexpr float kCadenceAdopt = 3.0f;               // 0x497900
constexpr float kCadenceBlend = 8.0f;               // 0x4978f8
constexpr float kCadenceDecay = 4.0f;               // 0x4978f0

// FUN_00430160 — 3D Euclidean distance.
float dist3(const float* a, const float* b) {
  const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// FUN_00437f30 — norm360(deg(atan2(dy,dx))) with the (dx|dy)==0
// passthrough. The original loads [EBP+0xc]=dx then [EBP+0x8]=dy and
// FXCH;FPATANs -> atan2(dy,dx), then wraps <0 by +360.
float bearingDeg(float dy, float dx) {
  if (dx == 0.0f && dy == 0.0f) return 0.0f;
  float deg = static_cast<float>(
      std::atan2(static_cast<double>(dy), static_cast<double>(dx)) *
      kRadToDeg);
  if (deg < 0.0f) deg += kConeWrap;
  return deg;
}

// FUN_00437f98 — deg -> {sin,cos} (arg2 = sin, arg3 = cos).
void sincosDeg(float deg, float* sinOut, float* cosOut) {
  const double rad = static_cast<double>(deg) * kDegToRad;
  *sinOut = static_cast<float>(std::sin(rad));
  *cosOut = static_cast<float>(std::cos(rad));
}

// FUN_0045f634 — the homing/punch element-name predicate: name is
// in_EAX, prefix is param_2 (the +0x302 string), the digit offset is
// unaff_EBX (+0x306). Accept iff name[digitOfs] is '0'..'9' AND name
// prefix-matches `prefix` (the loop returns 1 when the prefix is
// exhausted — i.e. a full prefix match).
bool elemNamePredicate(const char* name, const char* prefix, int digitOfs) {
  const unsigned char d = static_cast<unsigned char>(name[digitOfs]);
  if (d <= 0x2f || d >= 0x3a) return false;
  for (;;) {
    const char p = *prefix++;
    if (p == '\0') return true;
    if (p != *name++) return false;
  }
}

// FUN_0045c230 — segment-vs-AABB Liang-Barsky ENTRY clipper.
// start=in_EAX, end=param_2, out=param_1, aabb=unaff_EBX. Returns 2 if
// the start is inside (out=start), 1 if the segment enters the box
// (out=entry point), 0 on a miss / entry past the end. OBSERVED: the
// two X faces threshold against the 1.1 sentinel while the Y and Z
// faces compete against the running best tEnter.
int segClipAabb(const float* start, const float* end, const float* aabb,
                float* out) {
  const float dx = end[0] - start[0];
  const float dy = end[1] - start[1];
  const float dz = end[2] - start[2];
  unsigned flags = 0;       // local_10 — which sides the start is outside
  float tEnter = kClipInit; // local_14 — running best entry t (>1 = miss)
  // X
  if (start[0] <= aabb[0]) {
    flags = 1;
    if (aabb[0] < end[0]) {
      const float t = (aabb[0] - start[0]) / dx;
      if (t < kClipInit) {
        const float y = t * dy + start[1], z = t * dz + start[2];
        if (y <= aabb[4] && aabb[1] <= y && z <= aabb[5] && aabb[2] <= z) {
          out[0] = aabb[0]; out[1] = y; out[2] = z; tEnter = t;
        }
      }
    }
  } else if (aabb[3] <= start[0]) {
    flags = 2;
    if (end[0] < aabb[3]) {
      const float t = (aabb[3] - start[0]) / dx;
      if (t < kClipInit) {
        const float y = t * dy + start[1], z = t * dz + start[2];
        if (y <= aabb[4] && aabb[1] <= y && z <= aabb[5] && aabb[2] <= z) {
          out[0] = aabb[3]; out[1] = y; out[2] = z; tEnter = t;
        }
      }
    }
  }
  // Y
  if (aabb[1] < start[1]) {
    if (aabb[4] <= start[1]) {
      flags |= 8;
      if (end[1] < aabb[4]) {
        const float t = (aabb[4] - start[1]) / dy;
        if (t < tEnter) {
          const float x = t * dx + start[0], z = t * dz + start[2];
          if (x <= aabb[3] && aabb[0] <= x && z <= aabb[5] && aabb[2] <= z) {
            out[0] = x; out[1] = aabb[4]; out[2] = z; tEnter = t;
          }
        }
      }
    }
  } else {
    flags |= 4;
    if (aabb[1] < end[1]) {
      const float t = (aabb[1] - start[1]) / dy;
      if (t < tEnter) {
        const float x = t * dx + start[0], z = t * dz + start[2];
        if (x <= aabb[3] && aabb[0] <= x && z <= aabb[5] && aabb[2] <= z) {
          out[0] = x; out[1] = aabb[1]; out[2] = z; tEnter = t;
        }
      }
    }
  }
  // Z
  if (aabb[2] < start[2]) {
    if (aabb[5] <= start[2]) {
      flags |= 0x20;
      if (end[2] < aabb[5]) {
        const float t = (aabb[5] - start[2]) / dz;
        if (t < tEnter) {
          const float x = t * dx + start[0], y = t * dy + start[1];
          if (x <= aabb[3] && aabb[0] <= x && y <= aabb[4] && aabb[1] <= y) {
            out[0] = x; out[1] = y; out[2] = aabb[5]; tEnter = t;
          }
        }
      }
    }
  } else {
    flags |= 0x10;
    if (aabb[2] < end[2]) {
      const float t = (aabb[2] - start[2]) / dz;
      if (t < tEnter) {
        const float x = t * dx + start[0], y = t * dy + start[1];
        if (x <= aabb[3] && aabb[0] <= x && y <= aabb[4] && aabb[1] <= y) {
          out[0] = x; out[1] = y; out[2] = aabb[2]; tEnter = t;
        }
      }
    }
  }
  if (flags != 0) return (tEnter > 1.0f) ? 0 : 1;
  out[0] = start[0]; out[1] = start[1]; out[2] = start[2];
  return 2;
}

// FUN_004337ac — the punch cone test. aabb is EAX, center EDX,
// bestScore [0x8], stabOut [0xc], aimPt [0x10], outBearing [0x14].
// Returns the score (>=0) on accept, -1.0 on reject. bestScore < 0 is
// the "unset" sentinel — the first valid candidate always accepts
// (nearest tracking).
float punchConeTest(const float* aabb, const float* center, float bestScore,
                    float* stabOut, const float* aimPt, float* outBearing,
                    const TraversalRuntime& rt) {
  const float diag = dist3(aabb, aabb + 3);   // min->max diagonal
  const float dist1 = (diag < kConeFloor) ? kConeScoreFloor : diag;
  const float dist2 = dist3(aimPt, center);   // aim->center
  if (!(dist2 <= dist1 + kConeReach)) return -1.0f;
  const float dx = center[0] - aimPt[0];
  const float dy = center[1] - aimPt[1];
  const float bearing = bearingDeg(dy, dx);
  *outBearing = bearing;
  float rel = bearing - rt.motion.yawDeg;      // vs 0x540c2c
  while (rel < 0.0f) rel += kConeWrap;
  while (rel >= kConeWrap) rel -= kConeWrap;
  const float cone = (dist1 + kConeBase) * kConeWrap / (dist1 + kConeBase + dist2);
  if (!(rel <= cone || kConeWrap - cone <= rel)) return -1.0f;
  const float dz = center[2] - rt.cs.pos[2];   // center.z - 0x540c04
  const float score = dy * dy + dx * dx + dz * kConeZWeight * dz;
  if (!(score <= bestScore || bestScore < 0.0f)) return -1.0f;
  // occlusion — cur arena then the partner (carrier && !carrierBusy).
  if (rt.cs.arena != nullptr &&
      collisionStab(*rt.cs.arena, aimPt, center, stabOut) != nullptr)
    return -1.0f;
  if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
      collisionStab(*rt.cs.carrier, aimPt, center, stabOut) != nullptr)
    return -1.0f;
  return score;
}

// Recover the DynamicObject that owns a +0x68 CollisionObject list
// node (col is the first DynamicObject member).
DynamicObject* objectOf(const CollisionObject* o) {
  return reinterpret_cast<DynamicObject*>(
      const_cast<CollisionObject*>(o));
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_0045f138 — the player weapon-fire dispatch (sniper callsite).
// ---------------------------------------------------------------------------

void playerFireDispatch(TraversalRuntime& rt) {
  ++rt.seams.sniperFireCalls;   // FUN_0045f138 invoked
  if (rt.wpnSel0 == 5) {
    // Weapon 5 — the thrown-object path. Gated on the charge-probe
    // flag 0x540e14 and the fire latch 0x54163b.
    if (rt.fieldE14 == 0 || rt.field54163b != 0) {
      ++rt.seams.fireDenyCalls;   // FUN_00402388(1, 0x54c650)
      return;
    }
    if (rt.field541498 > 3) rt.field54163b = 1;   // charge >= 4
    rt.burstIndex -= 1;
    rt.fireCadence += 1.0f;
    rt.ammo[5] -= 1;               // 0x541633
    if (rt.burstIndex == 0) rt.fireCadence = 3.0f;
    ++rt.seams.weapon5SpawnCalls; // FUN_0045a4dc
    return;                        // 0x540e80 untouched on this path
  }

  // Weapons 0..4 — scan the 3-slot pool for state == 0.
  int slot = 0;
  while (slot < kShotPoolSize && rt.shots[slot].state != 0) ++slot;
  if (slot == kShotPoolSize) return;   // all busy -> silent return

  rt.fieldD0c = 0;
  ++rt.seams.fireSoundCalls;      // FUN_004022b8(0x54c5d0)
  PlayerShot& s = rt.shots[slot];
  s = PlayerShot{};               // FUN_0047d20a — memset 0xfc
  s.state = 1;
  s.classIdx = -1;                // +0x1c = the 0x4edd48 default record
  s.pos[0] = rt.camera.pose.pos[0];
  s.pos[1] = rt.camera.pose.pos[1];
  s.pos[2] = rt.camera.pose.pos[2];
  s.yawDeg = rt.motion.yawDeg;                       // 0x540c2c
  s.pitchDeg = rt.viewScalar + rt.look.lookPitchOffset;  // b54 + d58
  s.arena = rt.cur;                                  // 0x540c48
  s.fieldCc = 2.0f;                                  // +0xcc
  switch (rt.wpnSel0) {
    case 0:
      s.type = 0; s.flyKind = kShotFlyTracer;
      s.lifetime = 0x4b; s.speedH = 1100.0f;
      break;
    case 1:
      s.lifetime = 0xf0; s.type = 1; s.flyKind = kShotFlyGrenade;
      rt.ammo[1] -= 1;
      s.speedH = 400.0f;
      ++rt.seams.classLookupCalls;
      s.classIdx = rt.level.enemies.indexOf("SW_HOME");
      break;
    case 2:
      s.type = 2; s.flyKind = kShotFlyTracer;
      s.lifetime = 0x4b; s.speedH = 1100.0f;
      rt.ammo[2] -= 1;
      ++rt.seams.classLookupCalls;
      s.classIdx = rt.level.enemies.indexOf("SW_SGREN");
      break;
    case 3:
      s.lifetime = 0xf0; s.type = 3; s.flyKind = kShotFlyGrenade;
      rt.ammo[3] -= 1;
      s.speedH = 400.0f;
      ++rt.seams.classLookupCalls;
      s.classIdx = rt.level.enemies.indexOf("SW_HGREN");
      break;
    case 4: {
      s.lifetime = 0x1c2; s.type = 4; s.flyKind = kShotFlyLobbed;
      rt.ammo[4] -= 1;
      float sinP, cosP;
      sincosDeg(s.pitchDeg, &sinP, &cosP);
      s.speedH = cosP * 150.0f;    // 0x4984e8
      s.speedV = sinP * -150.0f;   // 0x4984ec
      ++rt.seams.classLookupCalls;
      s.classIdx = rt.level.enemies.indexOf("SW_LGREN");
      break;
    }
    default:
      break;                       // >4 — the cleared defaults stand
  }
  // Shared fire tail.
  rt.fireCadence += 1.0f;
  rt.burstIndex -= 1;
  if (rt.burstIndex == 0) rt.fireCadence = 3.0f;

  ++rt.seams.shotSpawnCalls;

  // Homing tail — 0x540cc8 != 0 means a lock target exists.
  if (rt.fieldCc8 == 0) {
    s.homeObj = nullptr;
    s.homeElem = nullptr;
    ++rt.shotSerial;
    return;
  }
  s.homeObj = rt.focusObj;         // 0x540cd8
  if (rt.wpnSel0 != 1 && rt.wpnSel0 != 3) {
    ++rt.shotSerial;
    return;
  }
  // Weapons 1/3 — walk the lock target's element set.
  s.homeElem = nullptr;
  const DynamicObject* target = rt.focusObj;
  if (target == nullptr || target->col.elements == nullptr) {
    ++rt.shotSerial;
    return;
  }
  const CollisionElementSet& set = *target->col.elements;
  float bestDist = 0.0f;
  for (int e = 0; e < set.count; ++e) {
    const std::string name = target->model.elemName(e);
    if (name.find("HEAD") != std::string::npos) {
      s.homeElem = &set.elems[e];   // first HEAD element wins
      s.homeElemIdx = e;
      break;
    }
    if ((target->col.flags149 & 0x20) == 0) continue;   // needs standable
    if (!elemNamePredicate(name.c_str(), target->homingPrefix.c_str(),
                           target->homingDigitOfs))
      continue;
    // Segment slotPos -> slotPos - basis[2]*10000 clipped to the
    // element AABB; the score is |out| (dist from world origin).
    float rayEnd[3], hit[3];
    for (int i = 0; i < 3; ++i)
      rayEnd[i] = s.pos[i] - rt.camera.pose.basis[2][i] * kHomeRayLen;
    if (segClipAabb(s.pos, rayEnd, set.elems[e].aabb, hit) == 0) continue;
    const float d = std::sqrt(hit[0] * hit[0] + hit[1] * hit[1] +
                              hit[2] * hit[2]);
    if (s.homeElem == nullptr || d < bestDist) {
      s.homeElem = &set.elems[e];
      s.homeElemIdx = e;
      bestDist = d;
    }
  }
  ++rt.shotSerial;
}

// ---------------------------------------------------------------------------
// FUN_00437660 — the scoped cadence/burst/ammo machine. `dt` is
// timing.deltaSec; the caller scopes it to (c9c && ca0 > 1).
// ---------------------------------------------------------------------------

void playerWeaponCadence(TraversalRuntime& rt, float dt) {
  if (rt.flag4999d0 && rt.flag541548) {
    // (0x4999d0 && 0x541548) — the machine is skipped; only the HUD
    // tail below it would run. Deferred.
    return;
  }
  if (rt.wpnSel0 != rt.wpnSel1) {
    // Pending weapon switch — blend the cadence up; adopt at 3.0.
    rt.fireCadence += dt * kCadenceBlend;
    if (rt.fireCadence >= kCadenceAdopt) {
      rt.burstIndex = 0;
      rt.wpnSel0 = rt.wpnSel1;
    }
    return;
  }
  if (rt.fireCadence <= 0.0f) return;
  rt.fireCadence -= dt * kCadenceDecay;
  if (rt.fireCadence < 0.0f) rt.fireCadence = 0.0f;
  if (rt.burstIndex == 3) return;         // full — no recharge
  // Recharge a burst pip: weapon 0 always, else gated by ammo left.
  if (rt.wpnSel1 == 0 ||
      rt.burstIndex < rt.ammo[rt.wpnSel1]) {
    ++rt.burstIndex;
    ++rt.seams.hudEventCalls;             // FUN_00402388 reload
  }
  if (rt.burstIndex == 0) {
    // The gate left an empty weapon at 0 — reload to weapon 0.
    rt.wpnSel1 = 0;
    rt.burstIndex = 1;
    ++rt.seams.hudEventCalls;             // FUN_00402388 reload
  }
}

// ---------------------------------------------------------------------------
// FUN_00469b98 — the scoped weapon selector (writes wpnSel1).
// ---------------------------------------------------------------------------

void playerWeaponSelect(TraversalRuntime& rt,
                        const GameplayInputFrame& ctrl) {
  ++rt.seams.weaponScanCalls;
  const auto& ws = ctrl.weaponSelect;
  // Hotkeys 0..5 — 0 unconditional; 1..5 write wpnSel1 only when
  // ammo[i] != 0 (OBSERVED JNZ, so a negative count still selects).
  if (ws[0] != 0) { rt.wpnSel1 = 0; return; }
  if (ws[1] != 0) { if (rt.ammo[1] != 0) rt.wpnSel1 = 1; return; }
  if (ws[2] != 0) { if (rt.ammo[2] != 0) rt.wpnSel1 = 2; return; }
  if (ws[3] != 0) { if (rt.ammo[3] != 0) rt.wpnSel1 = 3; return; }
  if (ws[4] != 0) { if (rt.ammo[4] != 0) rt.wpnSel1 = 4; return; }
  if (ws[5] != 0) { if (rt.ammo[5] != 0) rt.wpnSel1 = 5; return; }
  int dir;
  if (ctrl.itemNext != 0) dir = 1;
  else if (ctrl.itemPrev != 0) dir = -1;
  else return;
  // Wrap-scan the pending weapon, skipping slots with ammo <= 0
  // (OBSERVED JG); weapon 0 always lands (unconditional).
  for (;;) {
    const int w = rt.wpnSel1 + dir;
    rt.wpnSel1 = w;
    if (w < 0) rt.wpnSel1 = 5;
    else {
      if (w > 5) { rt.wpnSel1 = 0; return; }
      if (w == 0) return;
    }
    if (rt.ammo[rt.wpnSel1] > 0) return;
  }
}

// ---------------------------------------------------------------------------
// FUN_00432f84 — the normal-mode punch hitscan. `frameStep` is
// timing.frameStep (0x49b6e8). Bounded to the scan + the +0x21e mark;
// the damage tail and the impact effects are counted seams.
// ---------------------------------------------------------------------------

void playerPunch(TraversalRuntime& rt, int frameStep) {
  DynamicObject* bestObj = nullptr;
  int bestElem = -1;
  float bestBearing = 0.0f;
  float bestCenter[3] = {0, 0, 0};
  float bestScore = -1.0f;

  // Gate: 0x540c74 != 0 AND NOT (excludeObj && !(mountClass & 2)).
  if (rt.fieldC74 == 0) return;
  if (rt.cs.excludeObj != nullptr && (rt.mountClass & 2) == 0) return;

  const float aimPt[3] = {rt.cs.pos[0], rt.cs.pos[1],
                          rt.cs.pos[2] + kAimZBias};

  // Charge drain — ammo[0] doubles as the punch-charge resource.
  int punchStep;
  int punchState;
  if (rt.ammo[0] < 1) {
    punchStep = frameStep;
    punchState = -1;
  } else {
    rt.ammo[0] -= frameStep;
    if (rt.ammo[0] < 1) {
      // Reload — the original scans the 0x54155c inventory table for a
      // type-6 entry and calls FUN_0046a3d8 on it; the inventory isn't
      // modelled, so the scan finds nothing and that call never fires.
      // ammo[0] = 0 and the FUN_00469668(1) notify are unconditional.
      rt.ammo[0] = 0;
      ++rt.seams.fireNotifyCalls;
    }
    punchStep = frameStep * 6;
    punchState = -2;
  }
  rt.punchTime += punchStep;
  (void)punchState;   // carried for the deferred damage tail

  float stabOut[3], bearing = 0.0f;
  // Two-arena scan — cur always, then partner iff (ca8 && ca4).
  const CollisionArena* arenas[2];
  int nArenas = 0;
  arenas[nArenas++] = rt.cs.arena;
  if (rt.partnerActive && rt.cs.carrier != nullptr)
    arenas[nArenas++] = rt.cs.carrier;
  for (int a = 0; a < nArenas; ++a) {
    if (arenas[a] == nullptr) continue;
    for (const CollisionObject* o = arenas[a]->objects; o != nullptr;
         o = o->next) {
      if (o == rt.cs.excludeObj || !o->named || o->model == nullptr)
        continue;
      if ((o->flags148 & 0x10) != 0 || (o->flags148 & 0x20) != 0)
        continue;
      DynamicObject* obj = objectOf(o);
      float center[3];
      if ((o->flags149 & 0x20) == 0) {
        // Whole-object — centre = the object AABB midpoint.
        center[0] = (o->aabb[0] + o->aabb[3]) * 0.5f;
        center[1] = (o->aabb[1] + o->aabb[4]) * 0.5f;
        center[2] = (o->aabb[2] + o->aabb[5]) * 0.5f;
        const float ret =
            punchConeTest(o->aabb, center, bestScore, stabOut, aimPt,
                          &bearing, rt);
        if (ret >= 0.0f) {
          bestObj = obj; bestElem = -1;
          bestCenter[0] = center[0]; bestCenter[1] = center[1];
          bestCenter[2] = center[2];
          bestBearing = bearing; bestScore = ret;
        }
      } else {
        // Standable — scan the element set.
        const CollisionElementSet& set = *o->elements;
        for (std::int32_t e = 0; e < set.count; ++e) {
          if ((o->elemMaskB & (1u << (e & 0x1f))) != 0) continue;
          const std::string name = obj->model.elemName(e);
          if (!elemNamePredicate(name.c_str(),
                                 obj->homingPrefix.c_str(),
                                 obj->homingDigitOfs))
            continue;
          const CollisionElement& el = set.elems[e];
          center[0] = (el.aabb[0] + el.aabb[3]) * 0.5f;
          center[1] = (el.aabb[1] + el.aabb[4]) * 0.5f;
          center[2] = (el.aabb[2] + el.aabb[5]) * 0.5f;
          const float ret =
              punchConeTest(el.aabb, center, bestScore, stabOut, aimPt,
                            &bearing, rt);
          if (ret >= 0.0f) {
            bestObj = obj; bestElem = e;
            bestCenter[0] = center[0]; bestCenter[1] = center[1];
            bestCenter[2] = center[2];
            bestBearing = bearing; bestScore = ret;
          }
        }
        if (bestObj != obj) {
          // No element won — the whole object gets a test too.
          center[0] = (o->aabb[0] + o->aabb[3]) * 0.5f;
          center[1] = (o->aabb[1] + o->aabb[4]) * 0.5f;
          center[2] = (o->aabb[2] + o->aabb[5]) * 0.5f;
          const float ret =
              punchConeTest(o->aabb, center, bestScore, stabOut, aimPt,
                            &bearing, rt);
          if (ret >= 0.0f) {
            bestObj = obj; bestElem = -1;
            bestCenter[0] = center[0]; bestCenter[1] = center[1];
            bestCenter[2] = center[2];
            bestBearing = bearing; bestScore = ret;
          }
        }
      }
    }
  }
  (void)bestElem; (void)bestBearing; (void)bestCenter;

  if (bestObj != nullptr) {
    // Hit — the +0x21e mark and the hit-time accumulator are the
    // observable writes; the damage/knockback/death tail is deferred.
    bestObj->field21e = 0xff;
    rt.punchHitTime += frameStep;
    ++rt.seams.punchHitCalls;
    return;
  }

  // Miss — the 40-unit forward stab (pos + yawdir*40, z at the aim
  // point). A hit on either arena is the wall-impact path; a clean
  // whiff returns without an impact.
  float sinY, cosY;
  sincosDeg(rt.motion.yawDeg, &sinY, &cosY);
  const float missPt[3] = {rt.cs.pos[0] + cosY * kMissRayLen,
                           rt.cs.pos[1] + sinY * kMissRayLen,
                           aimPt[2]};
  bool occluded = false;
  if (rt.cs.arena != nullptr &&
      collisionStab(*rt.cs.arena, aimPt, missPt, stabOut) != nullptr)
    occluded = true;
  if (!occluded && rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
      collisionStab(*rt.cs.carrier, aimPt, missPt, stabOut) != nullptr)
    occluded = true;
  if (occluded) ++rt.seams.punchWallCalls;   // FUN_00437444 wall impact
}

// ---------------------------------------------------------------------------
// FUN_00465228 tail — the normal-mode fire latch (the punch gate).
// ---------------------------------------------------------------------------

void playerFireLatch(TraversalRuntime& rt,
                     const GameplayInputFrame& ctrl) {
  // Fire held requires the event priority + pending type <= 7; a
  // higher-priority event or a released button clears the latch.
  if (ctrl.fire == 0 || rt.eventPriority > 7 || rt.eventType > 7) {
    if (rt.fieldC74 != 0) {
      rt.fieldC74 = 0;
      ++rt.seams.fireNotifyCalls;   // FUN_00469668 off
    }
    return;
  }
  if (rt.fieldC74 == 0) {
    rt.fieldC74 = 1;
    ++rt.seams.fireNotifyCalls;     // FUN_00469668 on
  }
  // Post the fire anim event unless a busy event is already pending
  // (the move/strafe/turn + scripted magnitude set 0x2bd/0x2bc/0x190/
  // 0x1f4/0x2bf/0x2be). 0x258 -> 0x259/6; otherwise the 0x12c/3 post.
  if (rt.eventMag != 0x2bd && rt.eventMag != 0x2bc && rt.eventMag != 0x190 &&
      rt.eventMag != 0x1f4 && rt.eventMag != 0x2bf && rt.eventMag != 0x2be) {
    if (rt.eventMag != 0x258) {
      rt.eventMag = 0x12c;
      rt.eventType = 3;
    } else {
      rt.eventMag = 0x259;
      rt.eventType = 6;
    }
  }
}

// ---------------------------------------------------------------------------
// FUN_00437aa8 — the scoped charge-probe flag update. 0x540e14 =
// (the FUN_0046145c charge probe is live). The probe spawn is a
// deferred seam; `weapon5Probe` carries the live flag for tests.
// ---------------------------------------------------------------------------

void playerChargeProbe(TraversalRuntime& rt) {
  ++rt.seams.chargeProbeCalls;
  rt.fieldE14 = rt.weapon5Probe;   // FUN_0046145c(0x540e18) live flag
}

} // namespace mdk
