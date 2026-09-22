// Phase 10A — projectile lifecycle, impact, and the damage/death
// boundary. See player_projectiles.h for the evidence map; every
// behaviour below is OBSERVED at instruction level in BUILD_A
// (MDK95.EXE) unless marked otherwise. This is a direct port of
// FUN_0045f9b8 + the four +0xd4 fly callbacks + FUN_00460b7c +
// FUN_00460d44/FUN_00460c08 + FUN_0046771c + the
// FUN_00458140/FUN_004581a4/FUN_00457cf4 death chain — not a combat
// redesign.

#include "core/player_projectiles.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "core/collision_query.h"
#include "core/dynamic_objects.h"
#include "core/player_fire.h"
#include "core/player_surface.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

constexpr double kDegToRad = 0.017453292519943276;  // 0x497924
constexpr double kRadToDeg = 57.295779513082323;    // 0x497914

// 0x49b6f4 — the flight step is a literal constant in the original
// (1/30, never written). Flight physics use it; +0x10/+0x14 run on
// timing.frameStep (0x49b6e8).
constexpr float kTickDt = 0.03333333507180214f;

// Pool/tail constants.
constexpr float kTailGrow = 10.0f;            // 0x498598 — +10*dt
constexpr float kTailCap = 10.0f;             // 0x49851c — tail cap
constexpr double kCcDecay = 0.5;              // 0x4985a0 — -0.5*dt
constexpr float kCcFloor = 1.0f;              // aux floor
constexpr float kDyingShrink = 5.0f;          // 0x498508 — -5*dt
constexpr float kSpinRate = 720.0f;           // 0x498518 — spin deg/s
constexpr float kImpactTailLen = 15.0f;       // 0x41700000 — +0xbc on hit
constexpr int kImpactLife = 30;               // +0x10 on impact/state-4
constexpr int kImpactDying = 30;              // +0x14 on impact/state-4

// Homing (FUN_004602d8/FUN_00460424).
constexpr int kHomeLifeGate = 0xe9;           // +0x10 > 233 -> tracer
constexpr double kAabbMid = 0.5;              // 0x498528 — centre coeff
constexpr float kErrGate = 35.0f;             // 0x498530
constexpr float kHomeNear = 250.0f;           // 0xfa — err <= 35 target
constexpr float kHomeFar = 100.0f;            // 0x64 — err > 35 target
constexpr float kHomeDecel = 500.0f;          // 0x498538 — -500*dt
constexpr float kHomeAccel = 200.0f;          // 0x498534 — +200*dt
constexpr float kSteerAccel = 540.0f;         // 0x498550 — +-540 deg/s^2
constexpr float kSteerCap = 270.0f;           // 0x43870000
constexpr float kSteerCapNeg = -270.0f;       // 0xc3870000
constexpr float kPitchSlew = 120.0f;          // 0x498560 — +-120 deg/s
constexpr float kWrapHalf = 180.0f;           // 0x49853c
constexpr float kWrapNeg = -180.0f;           // 0x498540
constexpr float kWrapTurns = 0.0027777778450399637f; // 0x498544 (1/360)
constexpr double kWrapDeg = 360.0;            // 0x498548
constexpr float kYawFull = 360.0f;            // 0x49855c

// Lobbed (FUN_004608bc) + bounce.
constexpr float kLobDrag = 60.0f;             // 0x498570 — 60*dt
constexpr double kLobShed = 60.0;             // 0x498578 — rising shed
constexpr double kLobGravity = 32.0;          // 0x498580 — 32*dt
constexpr double kLobVFloor = -220.0;         // 0x498588 — 0xc35c0000
constexpr double kLobVolDamp = 0.25;          // 0x498590 — volume sH damp
constexpr float kLobTailZ = 0.005f;           // 0x4985a8 — sV tail z
constexpr float kSweepExt = 0.5f;             // 0x49b918 — ext {0.5}^3
constexpr float kRestitH = 1.75f;             // 0x3fe00000 — default
constexpr float kRestitV = 1.75f;
constexpr float kRestitAltH = 1.05f;          // 0x3f866666 — bit1 surface
constexpr float kRestitAltV = 1.25f;          // 0x3fa00000
constexpr int kSettleLife = 15;               // +0x10 settle target
constexpr double kSettleH = 0.5;              // 0x498510 — sH < 0.5
constexpr float kSettleV = 1.0f;              // sV < 1
constexpr float kLobBand = 4.0f;              // speedV clears into (0,4)

// Splash/damage (FUN_00460d44/FUN_00460c08/FUN_00460b7c/FUN_0046771c).
constexpr int kDetonateDmg = 150;             // 0x96
constexpr float kSplashR25 = 25.0f;           // 0x41c80000 — types 2/3
constexpr float kSplashR50 = 50.0f;           // 0x42480000 — type 4
constexpr double kPlayerScale = 0.5;          // 0x4985b0 — half-damage
constexpr double kDistDouble = 2.0;           // 0x4985d0 — effDist *2
constexpr float kAccumDouble = 2.0f;          // 0x4985d8 — 0x540d5c *= 2
constexpr double kCentroid = 0.3333333;       // 0x4985e0 — poly centroid
constexpr double kAuxQuart = 0.25;            // 0x4985c0 — (diag/2)^2
constexpr int kPlayerDmgCap = 15;             // 0xf — player pass cap
constexpr int kHealthGate = 0xfde8;           // health subtract gate
constexpr std::int8_t kDetonateExcl = -7;     // 0xf9 — +0x21d exclMask
constexpr float kFacing180 = 180.0f;          // 0x4985e8 — kill facing

// ---------------------------------------------------------------------------
// Math helpers — the same original functions the fire layer uses
// (FUN_00430160 / FUN_00430190 / FUN_00437f30 / FUN_00437f98).
// ---------------------------------------------------------------------------

float dist3Sq(const float* a, const float* b) {
  const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  return dx * dx + dy * dy + dz * dz;
}

// FUN_00437f30 — norm360(deg(atan2(dy,dx))), 0 passthrough.
float bearingDeg(float dy, float dx) {
  if (dx == 0.0f && dy == 0.0f) return 0.0f;
  float deg = static_cast<float>(
      std::atan2(static_cast<double>(dy), static_cast<double>(dx)) *
      kRadToDeg);
  if (deg < 0.0f) deg += kYawFull;
  return deg;
}

// FUN_00437f98 — deg -> {sin,cos}.
void sincosDeg(float deg, float* sinOut, float* cosOut) {
  const double rad = static_cast<double>(deg) * kDegToRad;
  *sinOut = static_cast<float>(std::sin(rad));
  *cosOut = static_cast<float>(std::cos(rad));
}

// FUN_0047d59a — the rounding helper: sets the FPU to round-nearest,
// FRNDINT, restores the control word; the callers' FISTP then stores
// the already-integral value. Round-half-even — NOT trunc.
int roundInt(double v) {
  return static_cast<int>(std::lrint(v));
}

// ---------------------------------------------------------------------------
// Record helpers
// ---------------------------------------------------------------------------

DynamicObject* objectOf(const CollisionObject* o) {
  return reinterpret_cast<DynamicObject*>(
      const_cast<CollisionObject*>(o));
}

// FUN_0045f634 — the element-name predicate (digit at +0x306 then a
// +0x302 prefix match). Same shape as the fire layer's local copy.
bool elemNamePredicate(const char* name, const char* prefix, int digitOfs) {
  const unsigned char d = static_cast<unsigned char>(name[digitOfs]);
  if (d <= 0x2f || d >= 0x3a) return false;
  for (;;) {
    const char p = *prefix++;
    if (p == '\0') return true;
    if (p != *name++) return false;
  }
}

// The surface-dispatch seam — the same call shape as
// surfaceContactHook: run the dispatcher, count the call, copy the
// fx state back for the caller to consume.
std::uint8_t shotSurfaceDispatch(SurfaceObjectState& ctx,
                                 std::int32_t secondary,
                                 std::uint8_t contextMask,
                                 CollisionPoly* poly,
                                 std::int32_t eventCode,
                                 const float vecA[3], const float posB[3],
                                 const float contactPos[3]) {
  SurfaceFxState fx{};
  return surfaceDispatch(ctx, secondary, contextMask, poly, eventCode,
                         vecA, posB, contactPos, fx, ctx.scriptFn,
                         ctx.scriptUser);
}

// ---------------------------------------------------------------------------
// Ribbon path record — read-only consumer of the thrown-path seam's
// key array. Keys sit at stride 0x28 with the ORIGINAL layout
// (OBSERVED in FUN_00456bc8):
//   +0x00 aux     — key0's aux doubles as the key count; key i's aux
//                   doubles as key (i-1)'s tanOut.z
//   +0x04 frame   — integer frame time
//   +0x08..+0x10  pos x/y/z
//   +0x14..+0x1c  tanIn x/y/z
//   +0x20..+0x24  tanOut x/y   (tanOut.z is the NEXT key's +0x00)
// ---------------------------------------------------------------------------

int ribbonKeyCount(const void* rec) {
  return *static_cast<const std::int32_t*>(rec);
}

const std::uint8_t* ribbonKey(const void* rec, int i) {
  return static_cast<const std::uint8_t*>(rec) + i * 0x28;
}

float keyF32(const std::uint8_t* k, int off) {
  float v;
  std::memcpy(&v, k + off, sizeof v);
  return v;
}

std::int32_t keyI32(const std::uint8_t* k, int off) {
  std::int32_t v;
  std::memcpy(&v, k + off, sizeof v);
  return v;
}

// FUN_00456bc8 — cubic Hermite over the key record. Find the last
// segment start with key.frame < param (scanning back from count-2),
// u = (param-f0)/(f1-f0), out = ((B*u + A)*u + m0)*u + p0 with
// A = 3dp-2m0-m1, B = m0+m1-2dp per axis (OBSERVED Horner form).
void ribbonEval(const void* rec, float param, float out[3]) {
  const int count = ribbonKeyCount(rec);
  int seg = count - 2;
  if (seg > 0) {
    while (seg > 0 &&
           static_cast<float>(keyI32(ribbonKey(rec, seg), 4)) >= param) {
      --seg;
    }
  } else {
    seg = 0;
  }
  const std::uint8_t* k0 = ribbonKey(rec, seg);
  const std::uint8_t* k1 = ribbonKey(rec, seg + 1);
  const float u = (param - static_cast<float>(keyI32(k0, 4))) /
                  static_cast<float>(keyI32(k1, 4) - keyI32(k0, 4));
  for (int a = 0; a < 3; ++a) {
    const float p0 = keyF32(k0, 8 + a * 4);
    const float dp = keyF32(k1, 8 + a * 4) - p0;
    const float m0 = keyF32(k0, 0x20 + a * 4);  // +0x28 aliases k1.aux
    const float m1 = keyF32(k1, 0x14 + a * 4);
    const float A = 3.0f * dp - 2.0f * m0 - m1;   // 0x497f0c / 0x497f08
    const float B = m0 + m1 - 2.0f * dp;
    out[a] = ((B * u + A) * u + m0) * u + p0;
  }
}

// ---------------------------------------------------------------------------
// FUN_0045f94c — rebuild the tracer tail: tail = pos - tailLen*dir.
// ---------------------------------------------------------------------------

void rebuildShotTail(PlayerShot& s) {
  float sinY, cosY, sinP, cosP;
  sincosDeg(s.yawDeg, &sinY, &cosY);
  sincosDeg(s.pitchDeg, &sinP, &cosP);
  s.tail[0] = s.pos[0] - s.tailLen * cosY * cosP;
  s.tail[1] = s.pos[1] - s.tailLen * sinY * cosP;
  s.tail[2] = s.pos[2] + s.tailLen * sinP;
}

// ---------------------------------------------------------------------------
// The fly callbacks (+0xd4)
// ---------------------------------------------------------------------------

// FUN_004601d4 — straight tracer flight (weapons 0/2).
void flyTracer(PlayerShot& s) {
  float sinY, cosY, sinP, cosP;
  sincosDeg(s.yawDeg, &sinY, &cosY);
  sincosDeg(s.pitchDeg, &sinP, &cosP);
  const float v = s.speedH * kTickDt;
  s.pos[0] += v * cosY * cosP;
  s.pos[1] += v * sinY * cosP;
  s.pos[2] -= v * sinP;
  s.tailLen += kTickDt * kTailGrow;
  if (s.tailLen > kTailCap) s.tailLen = kTailCap;
  s.fieldCc -= static_cast<float>(kTickDt * kCcDecay);
  if (s.fieldCc < kCcFloor) s.fieldCc = kCcFloor;
  rebuildShotTail(s);
}

// FUN_00460424 — steer yaw/pitch toward `target`; returns
// |yawErr| + |pitchErr|. The yaw accumulator integrates +-540 deg/s^2
// capped at +-270 and resets on sign reversal (OBSERVED quirk); the
// yaw/pitch apply is clamped to the residual error.
float steerShot(PlayerShot& s, const float target[3]) {
  const float dx = target[0] - s.pos[0];
  const float dy = target[1] - s.pos[1];
  const float dz = target[2] - s.pos[2];
  float yawErr;
  if (dx == 0.0f && dy == 0.0f) {
    yawErr = 0.0f;                 // (0x7fffffff bit test on dx|dy)
  } else {
    yawErr = bearingDeg(dy, dx) - s.yawDeg;
  }
  // Trunc-wrap to [-180,180]: n = FRNDINT((err+180)/360); err -= n*360.
  if (yawErr > kWrapHalf) {
    const int n = static_cast<int>(std::nearbyint(
        static_cast<double>((yawErr + kWrapHalf) * kWrapTurns)));
    yawErr = static_cast<float>(
        static_cast<double>(yawErr) - n * kWrapDeg);
  } else if (yawErr < kWrapNeg) {
    const int n = static_cast<int>(std::nearbyint(
        static_cast<double>((kWrapHalf - yawErr) * kWrapTurns)));
    yawErr = static_cast<float>(
        static_cast<double>(yawErr) + n * kWrapDeg);
  }
  const float rate = kTickDt * kSteerAccel;
  float errSum = std::fabs(yawErr);
  if (yawErr > 0.0f) {
    if (s.yawAccum < 0.0f) s.yawAccum = 0.0f;   // reversal reset
    s.yawAccum += rate;
    if (s.yawAccum > kSteerCap) s.yawAccum = kSteerCap;
  } else if (yawErr < 0.0f) {
    if (s.yawAccum > 0.0f) s.yawAccum = 0.0f;
    s.yawAccum -= rate;
    if (s.yawAccum < kSteerCapNeg) s.yawAccum = kSteerCapNeg;
  } else {
    s.yawAccum = 0.0f;
  }
  float step = s.yawAccum * kTickDt;
  if (yawErr > 0.0f) {
    if (step > yawErr) step = yawErr;     // don't overshoot
  } else if (yawErr < 0.0f) {
    if (step < yawErr) step = yawErr;
  }
  s.yawDeg += step;
  if (s.yawDeg >= kYawFull) s.yawDeg -= kYawFull;  // single-step wrap
  if (s.yawDeg < 0.0f) s.yawDeg += kYawFull;
  const float hDist = std::sqrt(dx * dx + dy * dy);
  float pitchErr = (kYawFull - bearingDeg(dz, hDist)) - s.pitchDeg;
  if (pitchErr > kWrapHalf) {
    const int n = static_cast<int>(std::nearbyint(
        static_cast<double>((pitchErr + kWrapHalf) * kWrapTurns)));
    pitchErr = static_cast<float>(
        static_cast<double>(pitchErr) - n * kWrapDeg);
  } else if (pitchErr < kWrapNeg) {
    const int n = static_cast<int>(std::nearbyint(
        static_cast<double>((kWrapHalf - pitchErr) * kWrapTurns)));
    pitchErr = static_cast<float>(
        static_cast<double>(pitchErr) + n * kWrapDeg);
  }
  errSum += std::fabs(pitchErr);
  const float pRate = kTickDt * kPitchSlew;
  if (pitchErr > 0.0f) {
    s.pitchDeg += (pRate > pitchErr) ? pitchErr : pRate;
  } else if (pitchErr < 0.0f) {
    s.pitchDeg -= (pRate > -pitchErr) ? -pitchErr : pRate;
  }
  return errSum;
}

// FUN_004602d8 — homing flight (weapons 1/3). While the lock
// validates and +0x10 <= 233 it steers toward the (element or
// object) AABB centre; otherwise it is plain tracer flight.
void flyHoming(TraversalRuntime& rt, PlayerShot& s) {
  (void)rt;
  if (s.homeObj != nullptr && s.lifetime <= kHomeLifeGate) {
    const DynamicObject* target = s.homeObj;
    if (!target->col.named) {
      s.homeObj = nullptr;               // unnamed target drops lock
    } else {
      if (s.homeElem != nullptr &&
          (target->col.elemMaskB & (1u << (s.homeElemIdx & 0x1f))) !=
              0) {
        s.homeElem = nullptr;            // masked element drops lock
      }
      const float* b = (s.homeElem != nullptr) ? s.homeElem->aabb
                                               : target->col.aabb;
      const float centre[3] = {
          static_cast<float>((b[0] + b[3]) * kAabbMid),
          static_cast<float>((b[1] + b[4]) * kAabbMid),
          static_cast<float>((b[2] + b[5]) * kAabbMid)};
      const float err = steerShot(s, centre);
      const float targetSpeed = (err <= kErrGate) ? kHomeNear : kHomeFar;
      if (s.speedH > targetSpeed) {
        s.speedH -= kHomeDecel * kTickDt;
        if (s.speedH < targetSpeed) s.speedH = targetSpeed;
      } else if (s.speedH < targetSpeed) {
        s.speedH += kHomeAccel * kTickDt;
        if (s.speedH > targetSpeed) s.speedH = targetSpeed;
      }
    }
  }
  flyTracer(s);
}

// FUN_004608bc — lobbed grenade flight (weapon 4). Ballistic drag,
// rising-shed, gravity and the arena volume query; the partner hit
// adopts outVec.z only (no sH damp — OBSERVED quirk).
void flyLobbed(TraversalRuntime& rt, PlayerShot& s) {
  float t = std::fabs(s.speedV) + s.speedH;
  if (t == 0.0f) {
    t = 1.0f;
  } else {
    t = s.speedH / t;
  }
  s.speedH -= t * kLobDrag * kTickDt;
  if (s.speedH < 0.0f) s.speedH = 0.0f;
  if (s.speedV > 0.0f) {
    s.speedV -= static_cast<float>((1.0 - t) * kLobShed * kTickDt);
    if (s.speedV < 0.0f) s.speedV = 0.0f;
  }
  s.speedV -= static_cast<float>(kTickDt * kLobGravity);
  if (s.speedV < kLobVFloor) s.speedV = static_cast<float>(kLobVFloor);
  // Volume/updraft query — mask 4, vec {0,0,sV}. Current arena first,
  // then the partner (ca4 && !d3c — no partnerActive gate).
  float outVec[3] = {0.0f, 0.0f, s.speedV};
  bool hit = false;
  if (rt.cur != nullptr) {
    hit = surfaceVolumeQuery(rt.cur->surface, 4, s.pos, kTickDt,
                             outVec) != 0;
  }
  if (hit) {
    s.speedH = static_cast<float>(s.speedH * kLobVolDamp);
    s.speedV = outVec[2];
  } else if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
             rt.partner != nullptr &&
             surfaceVolumeQuery(rt.partner->surface, 4, s.pos, kTickDt,
                                outVec) != 0) {
    s.speedV = outVec[2];               // partner: adopt z only
  }
  float sinY, cosY;
  sincosDeg(s.yawDeg, &sinY, &cosY);
  s.pos[0] += s.speedH * kTickDt * cosY;
  s.pos[1] += s.speedH * kTickDt * sinY;
  s.pos[2] += s.speedV * kTickDt;
  s.tailLen += kTickDt * kTailGrow;
  if (s.tailLen > kTailCap) s.tailLen = kTailCap;
  s.fieldCc -= static_cast<float>(kTickDt * kCcDecay);
  if (s.fieldCc < kCcFloor) s.fieldCc = kCcFloor;
  // The streak tail points away from the player; falling shots add the
  // sV*0.005 z term, rising shots use 0 (OBSERVED branch).
  const float dx = s.pos[0] - rt.cs.pos[0];
  const float dy = s.pos[1] - rt.cs.pos[1];
  float hDist = std::sqrt(dx * dx + dy * dy);
  if (hDist == 0.0f) hDist = 1.0f;
  const float tailZ = (s.speedV < 0.0f) ? s.speedV * kLobTailZ : 0.0f;
  s.tail[0] = s.pos[0] - s.tailLen * dx * (1.0f / hDist);
  s.tail[1] = s.pos[1] - s.tailLen * dy * (1.0f / hDist);
  s.tail[2] = s.pos[2] - s.tailLen * tailZ;
}

// FUN_0046075c — ribbon-bound flight. The path param advances by the
// smoothed frame units (0x49b6f0); +0x10 holds at 99 below the last
// key's frame-1, else the param clamps and +0x10 = 0 (release).
void flyRibbon(TraversalRuntime& rt, PlayerShot& s, float smoothed) {
  const int count = ribbonKeyCount(s.ribbonPath);
  const float lastFrame = static_cast<float>(
      keyI32(ribbonKey(s.ribbonPath, count - 1), 4) - 1);
  s.ribbonT += smoothed;
  if (s.ribbonT >= lastFrame) {
    s.ribbonT = lastFrame;
    s.lifetime = 0;
  } else {
    s.lifetime = 0x63;
  }
  ribbonEval(s.ribbonPath, s.ribbonT, s.pos);
  const float dx = s.pos[0] - rt.cs.pos[0];
  const float dy = s.pos[1] - rt.cs.pos[1];
  float hDist = std::sqrt(dx * dx + dy * dy);
  if (hDist == 0.0f) hDist = 1.0f;
  s.tail[0] = s.pos[0] - s.tailLen * dx * (1.0f / hDist);
  s.tail[1] = s.pos[1] - s.tailLen * dy * (1.0f / hDist);
  s.tail[2] = s.pos[2] +
              static_cast<float>(static_cast<double>(s.tailLen) * 0.25);
}

// ---------------------------------------------------------------------------
// FUN_00460c08 — the splash falloff helper. Returns the integer
// damage; `auxOut` is the effective distance (sqrt of the positive
// dist^2 - (diag/2)^2 residual, OBSERVED). Occlusion: stab
// blast->centre on cur then partner (ca4 && !d3c — NO partnerActive).
// ---------------------------------------------------------------------------

int splashFalloff(TraversalRuntime& rt, const float aabb[6],
                  float centreOut[3], const float blast[3],
                  int dmgScale, float range, float* auxOut) {
  centreOut[0] = static_cast<float>((aabb[0] + aabb[3]) * kAabbMid);
  centreOut[1] = static_cast<float>((aabb[1] + aabb[4]) * kAabbMid);
  centreOut[2] = static_cast<float>((aabb[2] + aabb[5]) * kAabbMid);
  const float diag2 = dist3Sq(aabb, aabb + 3);
  float aux = dist3Sq(centreOut, blast) -
              static_cast<float>(diag2 * kAuxQuart);
  if (aux < 0.0f) aux = 0.0f;
  aux = std::sqrt(aux);
  float stabPt[3];
  bool occluded = false;
  if (rt.cs.arena != nullptr &&
      collisionStab(*rt.cs.arena, blast, centreOut, stabPt) != nullptr) {
    occluded = true;
  } else if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
             collisionStab(*rt.cs.carrier, blast, centreOut, stabPt) !=
                 nullptr) {
    occluded = true;
  }
  *auxOut = aux;
  if (occluded || aux > range) return 0;   // pre-sqrt range^2 < aux
  // fdivrp: (range-aux)/range first, then FILD dmgScale + fmulp.
  return roundInt(static_cast<double>(dmgScale) *
                  ((static_cast<double>(range) - aux) /
                   static_cast<double>(range)));
}

// ---------------------------------------------------------------------------
// FUN_00460164 — the wall-impact dispatch helper: surfaceDispatch on
// the stabbed arena (channel from the caller, ev = shot.type, vecA =
// hitPt, posB = shot.pos, contactPos = prevPos) then the FUN_00437444
// effect seam (handler-ran -> {count 1, variant 2} else {3, 1} — the
// args are counted, not emulated).
// ---------------------------------------------------------------------------

void wallImpactDispatch(TraversalRuntime& rt, TraversalArena& arena,
                        PlayerShot& s, std::int32_t secondary,
                        const float hitPt[3], const float prevPos[3],
                        CollisionPoly* poly) {
  const std::uint8_t res =
      shotSurfaceDispatch(arena.surface, secondary, 1, poly, s.type,
                          hitPt, s.pos, prevPos);
  (void)res;   // the fx variant selects on result bit0 — counted only
  ++rt.seams.shotImpactFxCalls;
}

// ---------------------------------------------------------------------------
// FUN_00460b7c — detonation: object+poly splash at full scale, the
// player pass at trunc(dmg*0.5), the remnant seam, then state 5.
// ---------------------------------------------------------------------------

void detonateShot(TraversalRuntime& rt, PlayerShot& s, int dmg,
                  float radius, DynamicObject* directObj) {
  splashDamage(rt, s.pos, static_cast<float>(dmg), radius, 1, directObj,
               6, kDetonateExcl);
  const int half =
      static_cast<int>(std::nearbyint(static_cast<double>(dmg) *
                                      kPlayerScale));
  splashDamage(rt, s.pos, static_cast<float>(half), radius, 1, directObj,
               1, kDetonateExcl);
  ++rt.seams.remnantSpawnCalls;   // FUN_004575fc(arena, pos, 2.0f)
  s.state = 5;
  s.lifetime = kImpactLife;
  s.remnantIdx = 0;
  s.dyingTimer = s.lifetime;      // 30 — NOT +15 on this path
}

// ---------------------------------------------------------------------------
// The kill tally table — FUN_0042ac90's 34-entry model-name list
// (OBSERVED data at 0x49b4e8; entry 16 is duplicated at 24).
// ---------------------------------------------------------------------------

constexpr const char* kTallyTable[34] = {
    "XB",      "XB1",   "XB2",   "XB3",   "XBSHARK", "XBSHIP",
    "XBT",     "XC",    "XCARGO", "XD",   "XD6GUN",  "XE",
    "XEARTH",  "XF",    "XFORK",  "XG",   "XG_BOMB1", "XGEN",
    "XGHTARG", "XGSNOW", "XGSMOKE", "XG_MISS", "XGTARG", "XGUNTA",
    "XG_BOMB1", "XM3",   "XMART",  "XPER", "XS",      "XT",
    "XTANK",   "XTGUN", "XTUR",   "XU",
};

} // namespace

// ---------------------------------------------------------------------------
// FUN_0042ac90 — countGate != 0 AND the model name in the tally table
// bumps rt.killTally (0x540e90).
// ---------------------------------------------------------------------------

void objectKillTally(TraversalRuntime& rt, const DynamicObject& obj,
                     int countGate) {
  if (countGate == 0) return;
  const std::string name = obj.model.modelName();
  for (const char* entry : kTallyTable) {
    if (name == entry) {
      ++rt.killTally;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// FUN_00458140 — the death boundary. A live +0x110 record hands the
// object to the deferred-script path; otherwise the FUN_00457cf4
// teardown runs (the corpse/gib spawn is render-side — counted).
// ---------------------------------------------------------------------------

void objectDeathBoundary(TraversalRuntime& rt, DynamicObject& obj,
                         const float hitPt[3], float facingDeg) {
  ++rt.seams.objectDeathCalls;
  if (obj.field110 != nullptr) {
    obj.field11e = 0;
    obj.health = 0;
    obj.field22c = 0.0f;
    obj.col.flags148 |= 0x20;
    obj.field108 = obj.field110;
    obj.field230 = obj.field110;
    obj.field110 = nullptr;
    return;
  }
  // FUN_00457cf4 — the teardown. The observable runtime writes:
  //   +0x11e == 0xf  -> 0x540d2c = 10
  //   the FUN_0045828c record wipe: 0x49b85c global latch clear,
  //   the resource/model release (seam), the memset keeping
  //   +0x00/+0x60, and FUN_00458204's global-ref clears:
  //   excludeObj == obj -> clear + playerDamage(50); lastObjContact
  //   == obj -> clear. The corpse-model/gib spawn, death sound and
  //   the fade accumulator are render/effect seams — counted.
  ++rt.seams.objectTeardownCalls;
  if (obj.field11e == 0xf) rt.fieldD2c = 0xa;
  if (rt.fieldB85c == &obj) rt.fieldB85c = nullptr;
  if (rt.cs.excludeObj == &obj.col) {
    rt.cs.excludeObj = nullptr;
    // A dying mount deals 50 to the player (FUN_00458204, OBSERVED).
    playerDamageApply(rt, 50, obj.pos);
  }
  if (rt.cs.lastObjContact == &obj.col) rt.cs.lastObjContact = nullptr;
  (void)hitPt;
  (void)facingDeg;
}

// ---------------------------------------------------------------------------
// FUN_004581a4 — die facing the player: hitPt = obj.pos + {0,0,3},
// facing = bearing(player - obj). NO +180 here — the +180 lives in
// the kill-path callers (OBSERVED correction to earlier docs).
// ---------------------------------------------------------------------------

void objectDieFacingPlayer(TraversalRuntime& rt, DynamicObject& obj) {
  const float pt[3] = {obj.pos[0], obj.pos[1], obj.pos[2] + 3.0f};
  const float facing = bearingDeg(rt.cs.pos[1] - pt[1],
                                  rt.cs.pos[0] - pt[0]);
  objectDeathBoundary(rt, obj, pt, facing);
}

// ---------------------------------------------------------------------------
// FUN_0046771c — player damage. Difficulty scaling, the damage
// accumulator, the mount redirect and the suppress gates — exactly
// as observed.
// ---------------------------------------------------------------------------

void playerDamageApply(TraversalRuntime& rt, int dmg, const float pt[3]) {
  (void)pt;
  if (rt.fieldHealth == 0 && rt.fieldHealthGate == 0) return;
  // Suppress -> landingAccum cleared, no damage.
  if (rt.fieldE10 > 0.0f || rt.locoState == 0x326 ||
      rt.locoState == 0x385 || rt.locoState == 0x3ea ||
      rt.fieldEb8 == 1) {
    rt.vert.landingAccum = 0.0f;
    return;
  }
  int scaled;
  if (rt.difficulty == 0) {
    scaled = dmg * 2 / 3;                 // idiv-3 of 2*dmg
    if (scaled < 1) scaled = 1;
  } else if (rt.difficulty == 2) {
    scaled = dmg * 2;
  } else {
    scaled = dmg;
  }
  if (scaled > 0) {
    rt.fieldDac += scaled * 25;
    if (rt.fieldDac > 180) rt.fieldDac = 180;
    if (rt.fieldDac < 75) rt.fieldDac = 75;
  }
  if (rt.cs.excludeObj != nullptr && (rt.mountClass & 1) != 0) {
    // Mounted — the mount's health takes the damage (class&1).
    ++rt.seams.mountDamageCalls;
    DynamicObject* mount = objectOf(rt.cs.excludeObj);
    if (mount->health < kHealthGate) mount->health -= scaled;
    if (mount->health <= 0) objectDieFacingPlayer(rt, *mount);
    return;
  }
  rt.fieldHealth -= scaled;
  if (rt.fieldHealth < 0) rt.fieldHealth = 0;
  rt.vert.landingAccum += static_cast<float>(scaled);
}

// ---------------------------------------------------------------------------
// FUN_00460d44 — the splash/damage dispatcher.
//   blast     damage origin
//   dmgScale  integer damage scale (the original passes EDX)
//   range     falloff radius
//   tallyGate feeds FUN_0042ac90's count arg AND selects the poly
//             dispatch channel (3 when nonzero else 4 — OBSERVED)
//   directObj takes the full dmgScale at its AABB centre (aux 0)
//   flags     bit1 objects / bit0 player / bit2 arena polys
//   exclMask  the +0x21d byte written on damage (-7 from detonate)
// ---------------------------------------------------------------------------

void splashDamage(TraversalRuntime& rt, const float blast[3],
                  float dmgScaleF, float range, int tallyGate,
                  DynamicObject* directObj, std::uint32_t flags,
                  std::int8_t exclMask) {
  const int dmgScale = static_cast<int>(dmgScaleF);
  const float rangeSq = range * range;

  // --- objects pass (flags & 2): cur then partner — ca8 && ca4, NO
  // carrier-busy gate (OBSERVED difference from the other passes).
  if ((flags & 2) != 0) {
    for (int a = 0; a < 2; ++a) {
      const CollisionArena* arena = nullptr;
      if (a == 0) {
        arena = rt.cs.arena;
      } else if (rt.partnerActive && rt.cs.carrier != nullptr) {
        arena = rt.cs.carrier;
      }
      if (arena == nullptr) continue;
      for (const CollisionObject* o = arena->objects; o != nullptr;) {
        // The original saves [o]->next BEFORE the damage/death writes
        // (0x460f02) — teardown wipes the record, so iterate the
        // snapshot, not o->next.
        const CollisionObject* const next = o->next;
        if (!o->named || o->model == nullptr ||
            (o->flags148 & 0x10) != 0 || (o->flags148 & 0x20) != 0 ||
            o == rt.cs.excludeObj) {
          o = next;
          continue;
        }
        DynamicObject* obj = objectOf(o);
        int bestDmg = 0;
        float bestAux = 0.0f;
        float bestCenter[3] = {0, 0, 0};
        std::int8_t bestElemMark = -2;   // -2 -> 0xfe written to +0x21e
        // Standable objects — the per-element falloff + int16 damage.
        if ((o->flags149 & 0x20) != 0 && o->elements != nullptr) {
          const CollisionElementSet& set = *o->elements;
          for (std::int32_t e = 0; e < set.count; ++e) {
            if ((o->elemMaskB & (1u << (e & 0x1f))) != 0) continue;
            const std::string nm = obj->model.elemName(e);
            if (!elemNamePredicate(nm.c_str(),
                                   obj->homingPrefix.c_str(),
                                   obj->homingDigitOfs)) {
              continue;
            }
            float center[3], aux = 0.0f;
            const int dmg = splashFalloff(rt, set.elems[e].aabb, center,
                                          blast, dmgScale, range, &aux);
            if (dmg <= 0) continue;
            if (dmg > bestDmg) {
              bestDmg = dmg;
              bestAux = aux;
              std::memcpy(bestCenter, center, sizeof center);
            }
            if (e < static_cast<std::int32_t>(obj->elemHp.size())) {
              // 16-bit subtract + store BEFORE the sign test — the
              // low word wraps on underflow like the original's
              // merged-register `sub ebx, eax` (OBSERVED).
              const std::int16_t v = static_cast<std::int16_t>(
                  static_cast<std::int32_t>(obj->elemHp[e]) - dmg);
              obj->elemHp[e] = v;
              if (v <= 0) {
                obj->elemHp[e] = 0;
                bestElemMark = static_cast<std::int8_t>(e + 1);
                obj->field220 = 0;
                obj->field21c = static_cast<std::uint8_t>(e + 1);
              }
            }
          }
        }
        // Whole-object — direct hits take the full scale at the AABB
        // centre; others take the falloff when it beats the element
        // best.
        if (obj == directObj) {
          const float* b = o->aabb;
          bestCenter[0] = static_cast<float>((b[0] + b[3]) * kAabbMid);
          bestCenter[1] = static_cast<float>((b[1] + b[4]) * kAabbMid);
          bestCenter[2] = static_cast<float>((b[2] + b[5]) * kAabbMid);
          bestAux = 0.0f;
          bestDmg = dmgScale;
        } else {
          float center[3], aux = 0.0f;
          const int dmg = splashFalloff(rt, o->aabb, center, blast,
                                        dmgScale, range, &aux);
          if (dmg > 0 && dmg > bestDmg) {
            bestDmg = dmg;
            bestAux = aux;
            std::memcpy(bestCenter, center, sizeof center);
          }
        }
        if (bestDmg == 0) continue;
        if (bestAux > obj->field2c4) continue;    // the +0x2c4 gate
        const float bearing =
            bearingDeg(bestCenter[1] - blast[1],
                       bestCenter[0] - blast[0]);
        if (obj->health < kHealthGate) obj->health -= bestDmg;
        obj->field21e = static_cast<std::uint8_t>(bestElemMark);
        obj->field21d = static_cast<std::uint8_t>(exclMask);
        obj->field210[0] = bestCenter[0];
        obj->field210[1] = bestCenter[1];
        obj->field210[2] = bestCenter[2];
        obj->field228 = 0.0f;
        obj->field224 = bearing;
        if (obj->health <= 0) {
          if (tallyGate != 0) objectKillTally(rt, *obj, tallyGate);
          objectDeathBoundary(rt, *obj, bestCenter,
                              bearing + kFacing180);
        }
        o = next;
      }
    }
  }

  // --- player pass (flags & 1): dist2 to (playerPos + 1z); a stab
  // hit on cur OR the partner (ca4 && !d3c) forces out-of-range;
  // dist = sqrt(d2)*2; dmg = round(scale*(range-dist)/range) cap 15;
  // 0x540d5c doubles.
  if ((flags & 1) != 0) {
    float pt[3] = {rt.cs.pos[0], rt.cs.pos[1], rt.cs.pos[2] + 1.0f};
    float d2 = dist3Sq(blast, pt);
    if (d2 < 0.0f) d2 = 0.0f;               // OBSERVED dead clamp
    float stabPt[3];
    if (rt.cs.arena != nullptr &&
        collisionStab(*rt.cs.arena, blast, pt, stabPt) != nullptr) {
      d2 = 1.0f + rangeSq;                  // occluded -> out of range
    } else if (rt.cs.carrier != nullptr && rt.cs.carrierBusy == 0 &&
               collisionStab(*rt.cs.carrier, blast, pt, stabPt) !=
                   nullptr) {
      d2 = 1.0f + rangeSq;
    }
    const double dist = std::sqrt(static_cast<double>(d2)) * kDistDouble;
    if (dist < range) {
      int dmg = roundInt(static_cast<double>(dmgScale) *
                         ((static_cast<double>(range) - dist) /
                          static_cast<double>(range)));
      if (dmg > kPlayerDmgCap) dmg = kPlayerDmgCap;
      playerDamageApply(rt, dmg, blast);
      rt.vert.landingAccum *= kAccumDouble;
    }
  }

  // --- poly pass (flags & 4): cur then partner on ca8 && ca4. The
  // live-surface channel mask comes from config[i]||handlerOff[i];
  // each non-skip poly on a live surface within range^2 stab-tests
  // blast->centroid for occlusion — a same-surface blocker in the
  // current arena substitutes its hit point and retries the partner;
  // a different-surface blocker skips the poly; the poly's own record
  // never blocks. Survivors dispatch channel 3 (tallyGate != 0) or 4
  // with secondary=dmg, ev=exclMask — a fired channel clears.
  if ((flags & 4) != 0) {
    for (int a = 0; a < 2; ++a) {
      TraversalArena* arena = nullptr;
      if (a == 0) {
        arena = rt.cur;
      } else if (rt.partnerActive && rt.partner != nullptr) {
        arena = rt.partner;
      }
      if (arena == nullptr) continue;
      std::uint32_t chanMask = 0;
      for (int i = 0; i < kSurfaceSlots; ++i) {
        if (arena->surface.config[i] != 0 ||
            arena->surface.handlerOff[i] != 0) {
          chanMask |= 1u << i;
        }
      }
      if (chanMask == 0) continue;
      const CollisionArena& ca = arena->dyn.col;
      if (ca.verts == nullptr) continue;
      for (std::int32_t p = 0; p < arena->surface.polyCount; ++p) {
        CollisionPoly* poly = arena->surface.polys + p;
        if ((poly->flags & 0x20) != 0) continue;         // skip bit
        const int surfIdx = static_cast<int>(poly->surface) - 1;
        if ((chanMask & (1u << (surfIdx & 0x1f))) == 0) continue;
        float cent[3];
        for (int i = 0; i < 3; ++i) {
          cent[i] = static_cast<float>(
              (static_cast<double>(ca.verts[poly->v[0] * 3 + i]) +
               ca.verts[poly->v[1] * 3 + i] +
               ca.verts[poly->v[2] * 3 + i]) *
              kCentroid);
        }
        float d2 = dist3Sq(blast, cent);
        if (d2 > rangeSq) continue;
        if (d2 < 0.0f) d2 = 0.0f;
        // Occlusion — cur arena first (NOT the poly's arena), then
        // the partner (ca4 && !d3c).
        float stabPt[3] = {0, 0, 0};
        bool blocked = false;
        bool substituted = false;
        if (rt.cs.arena != nullptr) {
          const CollisionPoly* p2 = nullptr;
          if (collisionStabFull(*rt.cs.arena, blast, cent, stabPt,
                                &p2) != nullptr) {
            if (p2 != nullptr && p2 != poly) {
              if (static_cast<int>(p2->surface) - 1 != surfIdx) {
                blocked = true;                  // different surface
              } else {
                substituted = true;              // same surface
              }
            }
            // p2 == nullptr or p2 == poly -> partner retry
          }
        }
        if (!blocked && !substituted && rt.cs.carrier != nullptr &&
            rt.cs.carrierBusy == 0) {
          const CollisionPoly* p2 = nullptr;
          float stab2[3];
          if (collisionStabFull(*rt.cs.carrier, blast, cent, stab2,
                                &p2) != nullptr) {
            if (p2 != nullptr && p2 != poly &&
                static_cast<int>(p2->surface) - 1 != surfIdx) {
              blocked = true;
            } else if (p2 != nullptr && p2 != poly) {
              std::memcpy(stabPt, stab2, sizeof stabPt);
              substituted = true;
            }
          }
        }
        if (blocked) continue;
        if (substituted) {
          std::memcpy(cent, stabPt, sizeof cent);
          d2 = dist3Sq(blast, cent);      // recompute — no range re-check
        }
        const float dist = std::sqrt(d2);
        const int dmg = roundInt(static_cast<double>(dmgScale) *
                                 ((static_cast<double>(range) - dist) /
                                  static_cast<double>(range)));
        const std::uint8_t chan = (tallyGate != 0) ? 3 : 4;
        shotSurfaceDispatch(arena->surface, dmg, chan, poly, exclMask,
                            cent, cent, blast);
        chanMask &= ~(1u << (surfIdx & 0x1f));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// FUN_0045f9b8 — the per-slot update, and the pool tick invoked at
// the tail of each FUN_004572ac(arena) call.
// ---------------------------------------------------------------------------

namespace {

void updateShot(TraversalRuntime& rt, PlayerShot& s, int frameStep,
                float smoothed) {
  // Dying branch — state > 1 AND +0x14 > 0 (OBSERVED head gate).
  if (s.state > 1 && s.dyingTimer > 0) {
    if (s.lifetime > 0) {
      s.lifetime -= frameStep;
      if (s.type != 4 && s.type != 2 && s.type != 3) {
        s.tailLen -= kTickDt * kDyingShrink;   // no clamp (OBSERVED)
        rebuildShotTail(s);
      }
    }
    s.dyingTimer -= frameStep;
    if (s.dyingTimer <= 0) {
      s.state = 0;                            // release the slot
    }
    return;
  }
  // Flight — a stale state > 1 with +0x14 <= 0 re-enters here.
  float prevPos[3] = {s.pos[0], s.pos[1], s.pos[2]};
  switch (s.flyKind) {
    case kShotFlyTracer:
      flyTracer(s);
      break;
    case kShotFlyGrenade:
      flyHoming(rt, s);
      break;
    case kShotFlyLobbed:
      flyLobbed(rt, s);
      break;
    case kShotFlyRibbon:
      flyRibbon(rt, s, smoothed);
      break;
    default:
      break;
  }
  DynamicObject* hitObj = nullptr;
  int hitElem = -1;
  int hitTri = -1;
  if ((s.flags & 1) == 0) {
    // --- object scan (the probe shortens s.pos; nearest wins) ---
    for (int a = 0; a < 2; ++a) {
      const CollisionArena* arena = nullptr;
      if (a == 0) {
        arena = rt.cs.arena;
      } else if (rt.partnerActive && rt.cs.carrier != nullptr &&
                 rt.cs.carrierBusy == 0) {
        arena = rt.cs.carrier;
      }
      if (arena == nullptr) continue;
      for (const CollisionObject* o = arena->objects; o != nullptr;
           o = o->next) {
        if (!o->named || o->model == nullptr ||
            (o->flags148 & 0x10) != 0 || (o->flags148 & 0x20) != 0) {
          continue;
        }
        static const float kZeroExt[3] = {0, 0, 0};
        if (collisionSegAabbOverlap(prevPos, s.pos, o->aabb,
                                    kZeroExt) == 0) {
          continue;
        }
        int elem = -1, tri = -1;
        collisionObjectProbe(o, prevPos, s.pos, &elem, &tri);
        if (elem >= 0) {
          hitObj = objectOf(o);
          hitElem = elem;
          hitTri = tri;
        }
      }
    }
    // --- static arena collision ---
    if (s.type == 4) {
      // FUN_00407fc0 — flag 0, ext {0.5}^3, callback 0 (a zeroed
      // CollisionState matches the original's no-context call).
      const float ext[3] = {kSweepExt, kSweepExt, kSweepExt};
      CollisionState cs{};
      const CollisionNode* node = nullptr;
      const CollisionPoly* poly = nullptr;
      TraversalArena* hitArena = nullptr;
      float contact[3] = {0, 0, 0};
      if (rt.cs.arena != nullptr) {
        poly = collisionSweep(cs, prevPos, s.pos, 0, *rt.cs.arena, ext,
                              0.0f, contact, &node);
        if (poly != nullptr) hitArena = rt.cur;
      }
      if (poly == nullptr && rt.cs.carrier != nullptr &&
          rt.cs.carrierBusy == 0) {
        poly = collisionSweep(cs, prevPos, s.pos, 0, *rt.cs.carrier,
                              ext, 0.0f, contact, &node);
        if (poly != nullptr) hitArena = rt.partner;
      }
      if (poly != nullptr && node != nullptr && hitArena != nullptr) {
        // The lobbed-shot latch (0x49b8e4) — the ribbon binder's gate;
        // the pool never clears it (OBSERVED).
        rt.lobbedShot = &s;
        // Direct FUN_0040b5d0 callsite — contextMask 1, secondary 0,
        // ev = type, vecA = contact, posB = pos, contactPos = prevPos.
        const std::uint8_t res =
            shotSurfaceDispatch(hitArena->surface, 0, 1,
                                const_cast<CollisionPoly*>(poly),
                                s.type, contact, s.pos, prevPos);
        float rH = kRestitH, rV = kRestitV;
        if ((res & 2) != 0) {
          rH = kRestitAltH;
          rV = kRestitAltV;
        }
        float sinY, cosY;
        sincosDeg(s.yawDeg, &sinY, &cosY);
        float vel[3] = {s.speedH * cosY, s.speedH * sinY, s.speedV};
        const float dot = node->nx * vel[0] + node->ny * vel[1] +
                          node->nz * vel[2];
        vel[0] -= rH * dot * node->nx;
        vel[1] -= rH * dot * node->ny;
        vel[2] -= rV * dot * node->nz;
        s.pos[0] = contact[0];
        s.pos[1] = contact[1];
        s.pos[2] = contact[2];
        s.speedV = vel[2];
        if (s.speedV > 0.0f && s.speedV < kLobBand) s.speedV = 0.0f;
        s.speedH = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
        if (s.speedH > 0.0f) s.yawDeg = bearingDeg(vel[1], vel[0]);
        if (s.lifetime > kSettleLife &&
            static_cast<double>(s.speedH) < kSettleH &&
            s.speedV < kSettleV) {
          s.lifetime = kSettleLife;
        }
        // falls into the shared tail
      }
    } else {
      float hitPt[3] = {0, 0, 0};
      const CollisionPoly* poly = nullptr;
      const CollisionNode* node = nullptr;
      TraversalArena* hitArena = nullptr;
      if (rt.cs.arena != nullptr) {
        node = collisionStabFull(*rt.cs.arena, prevPos, s.pos, hitPt,
                                 &poly);
        if (node != nullptr) hitArena = rt.cur;
      }
      if (node == nullptr && rt.cs.carrier != nullptr &&
          rt.cs.carrierBusy == 0) {
        node = collisionStabFull(*rt.cs.carrier, prevPos, s.pos, hitPt,
                                 &poly);
        if (node != nullptr) hitArena = rt.partner;
      }
      if (node != nullptr && hitArena != nullptr) {
        // One unit toward prevPos along the node plane:
        // sign = +1 when prevPos is on the positive side, else -1.
        const float dist = prevPos[0] * node->nx +
                           prevPos[1] * node->ny +
                           prevPos[2] * node->nz + node->d;
        const float sign = (dist > 0.0f) ? 1.0f : -1.0f;
        hitPt[0] += sign * node->nx;
        hitPt[1] += sign * node->ny;
        hitPt[2] += sign * node->nz;
        if (s.type == 2 || s.type == 3) {
          wallImpactDispatch(rt, *hitArena, s, 0, hitPt, prevPos,
                             const_cast<CollisionPoly*>(poly));
          s.pos[0] = hitPt[0];
          s.pos[1] = hitPt[1];
          s.pos[2] = hitPt[2];
          detonateShot(rt, s, kDetonateDmg, kSplashR25, nullptr);
          return;
        }
        s.state = 4;
        s.lifetime = 0;
        s.dyingTimer = kImpactDying;
        s.remnantIdx = 0;
        wallImpactDispatch(rt, *hitArena, s, 8, hitPt, prevPos,
                           const_cast<CollisionPoly*>(poly));
        return;
      }
    }
  }
  // --- shared tail ---
  s.lifetime -= frameStep;
  if (s.type == 4 && s.lifetime <= 0) {
    detonateShot(rt, s, kDetonateDmg, kSplashR50, hitObj);
    return;
  }
  if (s.pos[2] < (s.arena != nullptr ? s.arena->dyn.col.deepFloorZ
                                     : 0.0f) ||
      s.lifetime <= 0) {
    // Kill floor / lifetime end -> state 4 (does NOT return — the
    // object-hit dispatch still runs, OBSERVED).
    s.state = 4;
    s.lifetime = 0;
    s.dyingTimer = kImpactDying;
    s.remnantIdx = 0;
  }
  if (hitObj != nullptr) {
    if (s.type == 2 || s.type == 3 || s.type == 4) {
      detonateShot(rt, s, kDetonateDmg,
                   (s.type == 4) ? kSplashR50 : kSplashR25, hitObj);
      // The splash may have latched +0x21e already — the fallback
      // mark writes only run while it stays <= 0 (signed byte).
      if (static_cast<std::int8_t>(hitObj->field21e) <= 0) {
        const std::uint8_t mark =
            static_cast<std::uint8_t>(hitElem + 1);
        hitObj->field21e = mark;
        hitObj->field21c = mark;
        hitObj->field21d = static_cast<std::uint8_t>(s.type);
      }
      hitObj->field210[0] = s.pos[0];
      hitObj->field210[1] = s.pos[1];
      hitObj->field210[2] = s.pos[2];
      hitObj->field220 = hitTri;
      ++rt.shotHitCount;              // 0x540e84
      hitObj->field224 = s.yawDeg;
      hitObj->field228 = s.pitchDeg;
      return;
    }
    // Types 0/1 — marks, 8 damage, survived (3) / killed (2).
    hitObj->field21e = 0xfe;
    hitObj->field21d = static_cast<std::uint8_t>(s.type);
    hitObj->field21c = static_cast<std::uint8_t>(hitElem + 1);
    hitObj->field210[0] = s.pos[0];
    hitObj->field210[1] = s.pos[1];
    hitObj->field210[2] = s.pos[2];
    hitObj->field220 = hitTri;
    hitObj->field224 = s.yawDeg;
    hitObj->field228 = s.pitchDeg;
    if (hitObj->health < kHealthGate) hitObj->health -= 8;
    ++rt.shotHitCount;
    if (hitObj->health > 0) {
      // Survived — FUN_00437444(arena, &field210, field150, 3,
      // flag21f) — counted.
      ++rt.seams.shotImpactFxCalls;
      s.state = 3;
      s.lifetime = kImpactLife;
      s.dyingTimer = s.lifetime + 15;
      s.remnantIdx = 0;
      s.tailLen = kImpactTailLen;
      rebuildShotTail(s);
      hitObj->field21e = static_cast<std::uint8_t>(hitElem + 1);
      return;
    }
    // Killed — the OBSERVED countGate quirk: the tally's count arg
    // is the shot's raw pitch bits (EDX reuse at the callsite).
    objectKillTally(rt, *hitObj, std::bit_cast<int>(s.pitchDeg));
    objectDieFacingPlayer(rt, *hitObj);
    s.state = 2;
    s.lifetime = kImpactLife;
    s.dyingTimer = s.lifetime + 15;
    s.remnantIdx = 0;
    s.tailLen = kImpactTailLen;
    rebuildShotTail(s);
    return;
  }
  if (s.type != 4) s.spinDeg += kSpinRate * kTickDt;
}

} // namespace

// FUN_004572ac tail — run the per-slot update for every non-free
// slot. `dt` is unused: flight physics use the fixed 1/30 tick
// (OBSERVED); lifetimes use frameStep; `smoothed` feeds the ribbon.
void playerShotPoolTick(TraversalRuntime& rt, int frameStep, float dt,
                        float smoothed) {
  (void)dt;
  ++rt.seams.shotPoolTickCalls;
  for (PlayerShot& s : rt.shots) {
    if (s.state != 0) updateShot(rt, s, frameStep, smoothed);
  }
}

} // namespace mdk
