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
#include "core/player_projectiles.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

constexpr double kDegToRad = 0.017453292519943276;  // 0x497924
constexpr double kRadToDeg = 57.295779513082323;    // 0x497914
constexpr float kDiagFloor = 10.0f;                 // 0x4973cc — dist1 floor
constexpr float kConeReach = 140.0f;                // 0x4973d0 — reach add
constexpr float kConeBase = -2.0f;                  // 0x4973d4 — dist1 bias
constexpr float kConeScale = 90.0f;                 // 0x4973d8 — cone factor
constexpr float kConeWrap = 360.0f;                 // 0x4973dc / 0x4973e4
constexpr float kConeZWeight = 4.0f;                // 0x4973e0
constexpr float kHomeRayLen = 10000.0f;             // 0x4984f0
constexpr float kMissRayLen = 150.0f;               // 0x4973c0
constexpr float kAimZBias = 5.0f;                   // 0x4973b0
constexpr float kKillPush = 20.0f;                  // 0x4973c4 — corpse push
constexpr float kKillFacing = 180.0f;               // 0x4973c8 — death face
constexpr int kLatchThresh = 0x384;                 // 900 — event-latch arm
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

} // namespace

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

namespace {

// FUN_004337ac — the punch cone test. aabb is EAX, center EDX,
// bestScore [0x8], stabOut [0xc], aimPt [0x10], outBearing [0x14].
// Returns the score (>=0) on accept, -1.0 on reject. bestScore < 0 is
// the "unset" sentinel — the first valid candidate always accepts
// (nearest tracking).
float punchConeTest(const float* aabb, const float* center, float bestScore,
                    float* stabOut, const float* aimPt, float* outBearing,
                    const TraversalRuntime& rt) {
  const float diag = dist3(aabb, aabb + 3);   // min->max diagonal
  const float dist1 = (diag < kDiagFloor) ? kDiagFloor : diag;
  const float dist2 = dist3(aimPt, center);   // aim->center
  if (!(dist2 <= dist1 + kConeReach)) return -1.0f;
  const float dx = center[0] - aimPt[0];
  const float dy = center[1] - aimPt[1];
  const float bearing = bearingDeg(dy, dx);
  *outBearing = bearing;
  float rel = bearing - rt.motion.yawDeg;      // vs 0x540c2c
  while (rel < 0.0f) rel += kConeWrap;
  while (rel >= kConeWrap) rel -= kConeWrap;
  // cone = (dist1-2)*90 / (dist1-2+dist2) — OBSERVED fdivrp order.
  const float coneBase = dist1 + kConeBase;
  const float cone = coneBase * kConeScale / (coneBase + dist2);
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

// ---------------------------------------------------------------------------
// FUN_0046153c — bounded ray-vs-world probe as invoked by the weapon-5
// charge probe (flags=3: object scan + BSP stab, +0x148 mask 0x30).
// `end` arrives as the ray target and is clipped to the nearest object
// hit; a BSP hit overwrites it with the stab crossing (the original's
// outPos writes through to the caller's vec). Returns true on any hit.
// ---------------------------------------------------------------------------
bool weapon5RayProbe(TraversalRuntime& rt, const float start[3],
                     float end[3]) {
  bool hit = false;
  const DynamicArena* lists[2] = {
      rt.cur ? &rt.cur->dyn : nullptr,
      (rt.partnerActive && rt.partner) ? &rt.partner->dyn : nullptr};
  for (const DynamicArena* la : lists) {
    if (!la) continue;
    for (const auto& up : la->storage) {
      const DynamicObject& o = *up;
      if (!o.col.named || o.col.model == nullptr) continue;  // +6/+8
      if ((o.col.flags148 & 0x30) != 0) continue;            // mask arg
      int elem = -1, tri = -1;
      collisionObjectProbe(&o.col, start, end, &elem, &tri);
      if (elem >= 0) hit = true;
    }
  }
  float stabPt[3] = {0, 0, 0};
  if (rt.cs.arena != nullptr &&
      collisionStab(*rt.cs.arena, start, end, stabPt) != nullptr) {
    for (int i = 0; i < 3; ++i) end[i] = stabPt[i];
    return true;
  }
  if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
      collisionStab(*rt.cs.carrier, start, end, stabPt) != nullptr) {
    for (int i = 0; i < 3; ++i) end[i] = stabPt[i];
    return true;
  }
  return hit;
}

// ---------------------------------------------------------------------------
// FUN_0045a4dc — weapon-5 thrown X_STRIKE spawn + charged trajectory.
// The object is an ordinary arena DynamicObject bound to cmd 0x80 and
// the shared five-key path record at 0x54ca00.
// ---------------------------------------------------------------------------
constexpr float kW5Behind = 50.0f;   // 0x498160 — spawn dist behind player
constexpr float kW5ZBase = 30.0f;    // 0x498168 — z base + frame scale/150
constexpr float kW5ApexZ = 32.0f;    // 0x498170 — aim z raise
constexpr float kW5Step1 = 0.09f;    // 0x498190 — charged/first march step
constexpr float kW5Step2 = 0.1f;     // 0x4981a0 — uncharged second march
constexpr float kW5ZBlend = 1.0f / 7.0f;  // 0x498198 — >7-step z blend
constexpr float kW5Frame = 0.2f;     // 30 * (1/150) — dist -> frame scale
const float kW5Ext[3] = {3.0f, 3.0f, 1.5f};  // 0x49b8c0 — sweep extents

// FUN_00407fc0 march test — sweep A->pt on the current arena; a clear
// result re-tests the partner arena when attached and !carrierBusy
// (OBSERVED at 0x45a919). True while the segment stays obstructed.
bool weapon5Blocked(TraversalRuntime& rt, const float A[3],
                    const float pt[3]) {
  float out[3] = {0, 0, 0};
  if (rt.cs.arena != nullptr &&
      collisionSweep(rt.cs, A, pt, 0, *rt.cs.arena, kW5Ext, 0.0f, out,
                     nullptr))
    return true;
  if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
      collisionSweep(rt.cs, A, pt, 0, *rt.cs.carrier, kW5Ext, 0.0f, out,
                     nullptr))
    return true;
  return false;
}

void weapon5Spawn(TraversalRuntime& rt) {
  ++rt.seams.weapon5SpawnCalls;
  // FUN_0042f310(0) — the X_STRIKB/X_STRIKD held-bomb presentation
  // routine. Presentation-only; counted, not ported.
  ++rt.seams.bombCinematicCalls;
  if (rt.cur == nullptr) return;    // port bound — original derefs 0x540c48
  TraversalArena& home = *rt.cur;

  // Spawn point — 50 units behind the player on the yaw circle;
  // z = arena+0x45a (a zero-init field, no writer) + 30.
  float sinY = 0.0f, cosY = 0.0f;
  sincosDeg(rt.motion.yawDeg, &sinY, &cosY);
  const float S[3] = {rt.cs.pos[0] - kW5Behind * cosY,
                      rt.cs.pos[1] - kW5Behind * sinY, kW5ZBase};

  ++rt.seams.classLookupCalls;      // FUN_00454794
  const int idx = rt.level.enemies.indexOf("X_STRIKE");
  if (idx < 0) return;
  const RuntimeModel* src = traversalModelFor(idx, &rt.level);
  if (src == nullptr) return;

  // FUN_00454af8 — generic object spawn (model + pos + spawnId 1 +
  // initDefaults + +0x11c=7), then FUN_0045a4dc's own field writes.
  DynamicObject& o = home.dyn.allocFront();
  o.scriptClass = "X_STRIKE";
  o.scriptOff = 0;                  // +0x108 = arg 7 (0)
  o.enemyIndex = static_cast<std::uint16_t>(idx);
  o.arena = &home.dyn;
  o.spawnId = 1;                    // +0x146 = the literal arg 1
  o.setPosition(S[0], S[1], S[2]);
  o.prevPos[0] = S[0]; o.prevPos[1] = S[1]; o.prevPos[2] = S[2];
  o.yawDeg = rt.motion.yawDeg;
  o.prevYawDeg = rt.motion.yawDeg;
  o.behaviorByte = 7;               // +0x11c
  o.model = deepCopyModel(*src);
  initObjectCollision(o);
  o.col.flags148 = 0x5e20;          // +0x148 dword = 0x85e20
  o.col.flags149 = 0x5e;
  o.col.flags14a = 0x08;
  o.col.flags14b = 0x00;
  o.health = 0xfde8;                // +0x08 = 65000

  // +0x120 anchor = the charge-probe aim + 32 z.
  float A[3] = {rt.weapon5Aim[0], rt.weapon5Aim[1],
                rt.weapon5Aim[2] + kW5ApexZ};
  o.field120[0] = A[0]; o.field120[1] = A[1]; o.field120[2] = A[2];

  // Path keys. V = 2*A - S (far apex); M = mid(A,V) charged / V
  // uncharged (whose z stays at the spawn plane). The key0 scratch
  // record in the original is callee-residue stack below esp — the
  // port bounds that as an all-zero first key (see doc note).
  float V[3] = {A[0] * 2.0f - S[0], A[1] * 2.0f - S[1],
                (rt.field54163b != 0) ? A[2] * 2.0f - S[2] : S[2]};
  float M[3] = {(rt.field54163b != 0) ? (A[0] + V[0]) * 0.5f : V[0],
                (rt.field54163b != 0) ? (A[1] + V[1]) * 0.5f : V[1],
                (rt.field54163b != 0) ? (A[2] + V[2]) * 0.5f : V[2]};

  // March 1 — anchor steps from S toward A (xy, 0.09 step) while the
  // A->anchor segment stays obstructed; <= 10 iterations.
  float anchor[3] = {S[0], S[1], S[2]};
  const float st1[2] = {(A[0] - S[0]) * kW5Step1,
                        (A[1] - S[1]) * kW5Step1};
  int steps = 0;
  while (steps < 10) {
    anchor[0] += st1[0]; anchor[1] += st1[1];
    if (!weapon5Blocked(rt, A, anchor)) break;
    ++steps;
  }

  if (rt.field54163b == 0) {
    // Uncharged only — a >7-step march 1 blends A.z back toward the
    // spawn plane, then march 2 walks M (starts at V) toward A at the
    // coarser 0.1 step with the same post-blend.
    if (steps > 7)
      A[2] += (S[2] - A[2]) * static_cast<float>(steps - 4) * kW5ZBlend;
    const float st2[2] = {(A[0] - V[0]) * kW5Step2,
                          (A[1] - V[1]) * kW5Step2};
    int steps2 = 0;
    while (steps2 < 10) {
      M[0] += st2[0]; M[1] += st2[1];
      if (!weapon5Blocked(rt, A, M)) break;
      ++steps2;
    }
    if (steps2 > 7)
      A[2] += (S[2] - A[2]) * static_cast<float>(steps2 - 4) * kW5ZBlend;
  }

  // Frames — cumulative round-half dist * (30/150) along the chain
  // S -> anchor -> A -> M -> V (OBSERVED frndint at 0x45a600).
  static const float kZero[3] = {0.0f, 0.0f, 0.0f};
  const float* P[5] = {kZero, S, anchor, A, M};
  const float* D[5] = {S, anchor, A, M, V};   // dist chain positions
  std::int32_t frames[5] = {0, 0, 0, 0, 0};
  for (int k = 1; k < 5; ++k)
    frames[k] = frames[k - 1] + static_cast<std::int32_t>(std::lround(
                                  dist3(D[k - 1], D[k]) * kW5Frame));

  // Serialize the five keys — FUN_00409558's tangent pass resolves to
  // tanIn = 0, tanOut = next - cur for the stored {0.5, 1.0, 0.5}
  // weights (the key4 "next" is V, which has no key of its own).
  std::int32_t* w = rt.weapon5Path;
  w[0] = 5;
  for (int k = 0; k < 5; ++k) {
    std::int32_t* e = w + 1 + k * 10;
    e[0] = frames[k];
    for (int c = 0; c < 3; ++c) {
      float v = P[k][c];
      std::memcpy(e + 1 + c, &v, 4);
      float ti = 0.0f;
      std::memcpy(e + 4 + c, &ti, 4);
      float to = (k > 0) ? (D[k][c] - P[k][c]) : 0.0f;
      std::memcpy(e + 7 + c, &to, 4);
    }
  }

  o.fieldEC = rt.weapon5Path;       // +0xec = 0x54ca00
  o.fieldE6 = -1;
  o.fieldF0 = 1.0f;
  o.fieldE8 = 1.0f;
  o.field30a = 0x80;                // +0x30a — path-dropper command
  o.fieldF4[0] = 0.0f;              // +0xf4..+0xfc = 0 (zero block copy)
  o.fieldF4[1] = 0.0f;
  o.fieldF4[2] = 0.0f;
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
    weapon5Spawn(rt);              // FUN_0045a4dc
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
// timing.frameStep (0x49b6e8). Phase 10A: the full damage/death tail
// is ported (shared boundary with player_projectiles).
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

  // Charge drain — ammo[0] doubles as the punch-charge resource;
  // `charged` latches the pre-drain state (the kill tally's gate).
  int punchStep;
  int punchState;
  int charged;
  if (rt.ammo[0] < 1) {
    charged = 0;
    punchStep = frameStep;
    punchState = -1;
  } else {
    charged = 1;
    rt.ammo[0] -= frameStep;
    if (rt.ammo[0] <= 0) {
      // Reload — the original scans the 0x54155c inventory table for a
      // type-6 entry and calls FUN_0046a3d8 on it; the inventory isn't
      // modelled, so the scan finds nothing and that call never fires.
      // ammo[0] = 0 and the FUN_00469668(1) notify are unconditional;
      // the charged punch still runs this frame (OBSERVED).
      rt.ammo[0] = 0;
      ++rt.seams.fireNotifyCalls;
    }
    punchStep = frameStep * 6;
    punchState = -2;
  }
  rt.punchTime += punchStep;   // 0x540e78 += dmg (both branches)

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
  if (bestObj != nullptr) {
    // Hit — FUN_00432f84's damage/death tail (OBSERVED ordering:
    // +0x21e = 0xff latches first; element hits damage the int16 pool
    // AND fall through to the whole-object damage; the +0x21e == -1
    // (signed) gate suppresses the state marks when an element death
    // already wrote them).
    DynamicObject& obj = *bestObj;
    ++rt.seams.punchHitCalls;
    obj.field21e = 0xff;
    if (bestElem >= 0) {
      // Element damage — the +0x30e+2e int16 pool; `sub word` wraps
      // on underflow before the sign test (OBSERVED), then clamps.
      if (bestElem < static_cast<int>(obj.elemHp.size())) {
        std::int16_t v = static_cast<std::int16_t>(
            obj.elemHp[bestElem] -
            static_cast<std::int16_t>(punchStep));
        if (v <= 0) {
          // Element death — the death marks, then the whole-object
          // damage still runs (the +0x21e = e+1 write closes the
          // -1 gate below).
          v = 0;
          obj.field21d = 0;
          obj.field220 = 0;
          obj.field21e = obj.field21c =
              static_cast<std::uint8_t>(bestElem + 1);
          obj.field224 = bestBearing;
          obj.field228 = 0.0f;
        }
        obj.elemHp[bestElem] = static_cast<std::int16_t>(v);
      }
      // The event latch — elemThresh[e] <= 900 plants the arena's
      // +0x118 pseudo-object (named=1, health=elemHp[e] post-write,
      // +0x2a2=elemThresh[e]) into 0x540eb4 and arms 0x540eb0 = 1.0.
      if (bestElem < static_cast<int>(obj.elemThresh.size()) &&
          obj.elemThresh[bestElem] <= kLatchThresh &&
          obj.arena != nullptr && obj.arena->owner != nullptr) {
        TraversalArena& a = *obj.arena->owner;
        rt.eventTimerObj = &a.eventLatch;
        rt.eventTimer = 1.0f;
        a.eventLatch.col.named = true;
        a.eventLatch.health =
            (bestElem < static_cast<int>(obj.elemHp.size()))
                ? obj.elemHp[bestElem]
                : 0;
        a.eventLatch.healthMirror2a2 = static_cast<std::uint32_t>(
            static_cast<std::uint16_t>(obj.elemThresh[bestElem]));
      }
    }
    // Whole-object damage — punchHitTime(0x540e7c) += frameStep,
    // health -= dmg (the < 0xfde8 gate), then the state marks when
    // +0x21e still reads -1 (signed — an element death wrote e+1).
    rt.punchHitTime += frameStep;
    if (obj.health < 0xfde8) obj.health -= punchStep;
    if (static_cast<std::int8_t>(obj.field21e) == -1) {
      obj.field21d = static_cast<std::uint8_t>(punchState);
      obj.field228 = 0.0f;
      obj.field224 = bestBearing;
    }
    if (obj.health <= 0) {
      // Kill — tally(charged), the charged-only corpse displacement
      // {20*cos,20*sin}(obj.field224), then the death boundary at
      // bearing+180 (the +180 lives HERE, not in FUN_004581a4).
      objectKillTally(rt, obj, charged);
      ++rt.seams.punchDeathCalls;
      if (charged != 0) {
        float s, c;
        sincosDeg(obj.field224, &s, &c);
        obj.field28 += kKillPush * c;
        obj.field2c += kKillPush * s;
      }
      objectDeathBoundary(rt, obj, bestCenter,
                          bestBearing + kKillFacing);
      return;
    }
    // Survived — knockback: hitPt -= {cos,sin}(bearing) * half the
    // winning AABB's X/Y extents (element bounds for element hits,
    // object bounds otherwise — OBSERVED stride-0x5c element aabb).
    float s, c;
    sincosDeg(bestBearing, &s, &c);
    const float* kb = (bestElem >= 0)
        ? bestObj->col.elements->elems[bestElem].aabb
        : bestObj->col.aabb;
    float hitPt[3] = {bestCenter[0], bestCenter[1], bestCenter[2]};
    hitPt[0] -= static_cast<float>(
        static_cast<double>(c) * 0.5 * (kb[3] - kb[0]));
    hitPt[1] -= static_cast<float>(
        static_cast<double>(s) * 0.5 * (kb[4] - kb[1]));
    // Whole-object latch — +0x2a2 (unsigned word compare, ja) <= 900
    // arms the timer with the REAL object (element path: pseudo).
    if (bestElem == -1 &&
        static_cast<std::uint16_t>(obj.healthMirror2a2) <=
            static_cast<std::uint16_t>(kLatchThresh)) {
      rt.eventTimerObj = bestObj;
      rt.eventTimer = 1.0f;
    }
    // FUN_00437444(obj.arena, &hitPt, obj.field150, 1, flag21f).
    ++rt.seams.punchEffectCalls;
    CombatFxEvent fx;
    fx.kind = CombatFxKind::kPunchObjectImpact;
    fx.mode = 1;
    fx.variant = obj.flag21f;
    fx.aux = obj.field150;
    fx.pos[0] = hitPt[0]; fx.pos[1] = hitPt[1]; fx.pos[2] = hitPt[2];
    fx.obj = bestObj;
    rt.combatFx.push_back(fx);
    return;
  }

  // Miss — the 150-unit forward stab (pos + yawdir*150, z at the aim
  // point). A wall hit pulls the contact one unit back along the yaw
  // dir, dispatches channel 2 secondary=dmg {ev=punchState, vecA=
  // aimPt, posB=missPt, contactPos=hitPt} and runs the FUN_00437444
  // seam (handler-ran -> count 1 variant 2, else count 1 variant 1).
  float sinY, cosY;
  sincosDeg(rt.motion.yawDeg, &sinY, &cosY);
  const float missPt[3] = {rt.cs.pos[0] + cosY * kMissRayLen,
                           rt.cs.pos[1] + sinY * kMissRayLen,
                           aimPt[2]};
  for (int a = 0; a < 2; ++a) {
    const CollisionArena* arena;
    TraversalArena* ctx;
    if (a == 0) {
      arena = rt.cs.arena;
      ctx = rt.cur;
    } else if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0) {
      arena = rt.cs.carrier;
      ctx = rt.partner;
    } else {
      continue;
    }
    if (arena == nullptr || ctx == nullptr) continue;
    const CollisionPoly* poly = nullptr;
    if (collisionStabFull(*arena, aimPt, missPt, stabOut, &poly) ==
        nullptr)
      continue;
    float hitPt[3] = {stabOut[0] - cosY, stabOut[1] - sinY,
                      stabOut[2]};
    SurfaceFxState fx{};
    const std::uint8_t res = surfaceDispatch(
        ctx->surface, punchStep, 2, const_cast<CollisionPoly*>(poly),
        punchState, aimPt, missPt, hitPt, fx, ctx->surface.scriptFn,
        ctx->surface.scriptUser);
    ++rt.seams.punchWallCalls;
    // FUN_00437444(arena, &vec, 0, 1, variant) — handler-ran (res&1)
    // -> variant 2, else variant 1 (0x43371f/0x43377e vs 0x43379b).
    ++rt.seams.punchEffectCalls;
    CombatFxEvent ev;
    ev.kind = CombatFxKind::kPunchWallImpact;
    ev.mode = 1;
    ev.variant = (res & 1) ? 2 : 1;
    ev.pos[0] = hitPt[0]; ev.pos[1] = hitPt[1]; ev.pos[2] = hitPt[2];
    rt.combatFx.push_back(ev);
    return;
  }
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
// FUN_00437aa8 — the scoped charge-probe flag update. The probe section
// (0x437c9b) runs when !(0x4999d0 && 0x541548) && wpnSel==5 &&
// 0x54161b==0 and stores FUN_0046145c's result in 0x540e14. The
// 0x540e94/e98 overlay blends + palette cycling it feeds are
// presentation and stay deferred; `weapon5Probe` forces the live flag
// for tests instead of running the real probe.
// ---------------------------------------------------------------------------

void playerChargeProbe(TraversalRuntime& rt) {
  ++rt.seams.chargeProbeCalls;
  if (rt.flag4999d0 && rt.flag541548) return;    // machine skipped
  if (rt.wpnSel0 != 5 || rt.fireCadence != 0.0f) return;
  if (rt.weapon5Probe != 0) {                    // PORT test hook
    rt.fieldE14 = 1;
    return;
  }
  rt.fieldE14 = 0;
  if (rt.cmdFlag54 != 0) return;   // 0x540e54 — dead while a bomb lives

  // FUN_0046145c — ray from the camera along -basis[2] for 5000,
  // clipped by the object scan + BSP (FUN_0046153c flags=3, mask 0x30).
  const float* cam = rt.camera.pose.pos;
  const float* fwd = rt.camera.pose.basis[2];
  float end[3] = {cam[0] - 5000.0f * fwd[0], cam[1] - 5000.0f * fwd[1],
                  cam[2] - 5000.0f * fwd[2]};
  if (!weapon5RayProbe(rt, cam, end)) return;

  // Aim = hit + one basis step; then the overhead stab (aim+1000z ->
  // aim) on cur, then the partner (attached && !carrierBusy) — any
  // crossing kills the probe (OBSERVED 0x4614f4..0x461523).
  float aim[3] = {end[0] + fwd[0], end[1] + fwd[1], end[2] + fwd[2]};
  const float top[3] = {aim[0], aim[1], aim[2] + 1000.0f};
  float pt[3] = {0, 0, 0};
  if (rt.cs.arena != nullptr &&
      collisionStab(*rt.cs.arena, top, aim, pt) != nullptr)
    return;
  if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
      collisionStab(*rt.cs.carrier, top, aim, pt) != nullptr)
    return;
  rt.weapon5Aim[0] = aim[0];
  rt.weapon5Aim[1] = aim[1];
  rt.weapon5Aim[2] = aim[2];
  rt.fieldE14 = 1;
}

} // namespace mdk
