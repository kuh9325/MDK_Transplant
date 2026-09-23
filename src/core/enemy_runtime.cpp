// enemy_runtime.cpp — Phase 11B/G5-RE: native object-side motion,
// command dispatch and attack boundaries. Ported from MDK95.EXE
// BUILD_A decompiles + raw disasm (see enemy_runtime.h provenance).
#include "core/enemy_runtime.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "core/collision_query.h"
#include "core/dynamic_objects.h"
#include "core/object_path.h"
#include "core/player_camera.h"
#include "core/player_projectiles.h"
#include "core/traversal_runtime.h"

namespace mdk {
// DAT_0049b6f4 / DAT_0049b6f0 — tick dt (1/30) + frame-step (1.0).
constexpr float kDt = 1.0f / 30.0f;
constexpr float kFrameStep = 1.0f;

// FUN_00437f98 — deg -> {sin,cos} (same formulation as the player path).
void sincosDeg(float deg, float* sinOut, float* cosOut) {
  const double rad = static_cast<double>(deg) * 0.01745329251994328;
  *sinOut = static_cast<float>(std::sin(rad));
  *cosOut = static_cast<float>(std::cos(rad));
}

namespace {



// FUN_0047d59a — FRNDINT under RC=11 (truncate toward zero), OBSERVED
// from raw disasm @0x47d59a (fldcw byte = 0x1f).
int frndint(float v) { return static_cast<int>(std::trunc(v)); }

// FUN_00437ff4 — deg(asin(v)) through the CRT asin core (0x49793c =
// 180/pi as f64).
float degAsin(float v) {
  return static_cast<float>(std::asin(static_cast<double>(v)) *
                            57.29577951308232);
}

// +0x148..+0x14b dword view (the original tests OR-masks on the whole
// flag dword, e.g. the 0x810/0x830 touch-scan masks and gravity's
// 0x45000).
std::uint32_t flagsDword(const DynamicObject& o) {
  return static_cast<std::uint32_t>(o.col.flags148) |
         (static_cast<std::uint32_t>(o.col.flags14a) << 16) |
         (static_cast<std::uint32_t>(o.col.flags14b) << 24);
}

// +0x302/+0x306/+0x30a/+0x30e union views (dword-bit storage).
float f302(const DynamicObject& o) { return std::bit_cast<float>(o.field302); }
float f306(const DynamicObject& o) { return std::bit_cast<float>(o.field306); }
float f30a(const DynamicObject& o) { return std::bit_cast<float>(o.field30a); }
float f30e(const DynamicObject& o) { return std::bit_cast<float>(o.field30e); }
void setF302(DynamicObject& o, float f) { o.field302 = std::bit_cast<std::uint32_t>(f); }
void setF306(DynamicObject& o, float f) { o.field306 = std::bit_cast<std::uint32_t>(f); }
void setF30e(DynamicObject& o, float f) { o.field30e = std::bit_cast<std::int32_t>(f); }

// FUN_0045ce58 — 6-float AABB overlap test (non-strict).
bool aabbOverlap(const float* a, const float* b) {
  return !(a[3] < b[0] || b[3] < a[0]) && !(a[4] < b[1] || b[4] < a[1]) &&
         !(a[5] < b[2] || b[5] < a[2]);
}

// The +0x14a&8 partner-arena retry gate: partner attached AND the
// 0x540d3c carrier-busy flag clear.
TraversalArena* migrationPartner(TraversalRuntime& rt,
                                 const DynamicArena& home) {
  if (!(rt.partner && rt.partnerActive)) return nullptr;
  if (rt.cs.carrierBusy) return nullptr;
  TraversalArena* other =
      (rt.cur && home.owner == rt.cur) ? rt.partner : rt.cur;
  return other;
}

// +0x14a&8 portal scan — FUN_00435178's DTI sub-record portal test.
// The original walks the home arena's +0x38/+0x3c type-6 portal table
// and returns the destination arena record when prevPos->pos crosses a
// portal plane. The port models the record table via the connector
// graph: crossing is detected by the sweep retry instead, so this is a
// counted seam (the +0x2bc write itself is ported in objectSweptMove).
void portalScanSeam(TraversalRuntime& rt, DynamicObject& o) {
  rt.seams.objectMigrations++;
  (void)o;
}

// The sweep's surface dispatch (FUN_0040b5d0 invoked per contact from
// FUN_0045d174, secondary=0x10, contextMask=0x10, eventCode=-0xa).
void objectSurfaceDispatch(DynamicObject& o, const CollisionPoly* poly,
                           const float outPos[3], const float start[3]) {
  SurfaceFxState fx;
  surfaceDispatch(o.surface, /*secondary*/ 0x10, /*contextMask*/ 0x10,
                  const_cast<CollisionPoly*>(poly), /*eventCode*/ -0xa,
                  outPos, o.pos, start, fx, nullptr, nullptr);
}

} // namespace

// FUN_00437f30 — norm360(deg(atan2(dy,dx))) with (dx|dy)==0 passthrough.
float bearingDeg(float dy, float dx) {
  if (dx == 0.0f && dy == 0.0f) return 0.0f;
  float d = static_cast<float>(std::atan2(static_cast<double>(dy),
                                          static_cast<double>(dx)) *
                               57.29577951308232);
  if (d < 0.0f) d += 360.0f;
  return d;
}

// ---------------------------------------------------------------------------
// RNG — FUN_0047d2b5 (MSVC CRT rand) + FUN_00401ed4
// ---------------------------------------------------------------------------

std::uint32_t enemyRandNext(std::uint32_t& state) {
  state = state * 0x41c64e6du + 0x3039u;
  return (state >> 16) & 0x7fffu;
}

int enemyRandBelow(std::uint32_t& state, int n) {
  if (n <= 0) return 0;
  return static_cast<int>(
      (static_cast<std::uint64_t>(enemyRandNext(state)) *
       static_cast<std::uint32_t>(n)) >> 15);
}

// ---------------------------------------------------------------------------
// FUN_00457ab8 — pendulum orbit (+0x14a&0x40)
// ---------------------------------------------------------------------------

void objectOrbit(DynamicObject& o, const float playerPos[3], float dt) {
  float sinB, cosB;
  sincosDeg(o.bankDeg, &sinB, &cosB);            // FUN_00437f98(+0x13c)
  const float rate = f30a(o);                  // +0x30a accel coeff
  const float radius = f306(o);                // +0x306 swing radius
  const float prevPhase = f302(o);             // +0x302 phase
  // Pendulum ODE (frame-step scaled — OBSERVED DAT_0049b6f0 = 1.0,
  // NOT the dt constant).
  const float phase = prevPhase - sinB * rate * kFrameStep;
  setF302(o, phase);
  o.bankDeg += phase * rate * kFrameStep;      // +0x13c integrates
  float sY, cY;
  sincosDeg(o.yawDeg, &sY, &cY);               // FUN_00437f98(+0x4c)
  // pos = center + R * (facing-rotated (sin(bank)), -cos(bank)) — the
  // object swings below +0x1c..0x24.
  o.pos[0] = cY * sinB * radius + o.field1c[0];
  o.pos[1] = sY * sinB * radius + o.field1c[1];
  o.pos[2] = o.field1c[2] - cosB * radius;
  if (phase * prevPhase <= 0.0f) {
    // Zero-crossing — chase the player within +/-15 deg, folded into
    // the +/-90 facing band (OBSERVED wrap/fold/clamp chain).
    if (o.yawDeg > 359.99997f) o.yawDeg -= 360.0f;   // >0x43b3ffff
    if (o.yawDeg < 0.0f) o.yawDeg += 360.0f;
    float d = bearingDeg(playerPos[1] - o.field1c[1],
                         playerPos[0] - o.field1c[0]) - o.yawDeg;
    if (d < -180.0f) d += 360.0f;
    if (d > 180.0f) d -= 360.0f;
    if (d < -90.0f) d += 180.0f;
    if (d > 90.0f) d -= 180.0f;
    if (d <= 15.0f) {
      if (d < -15.0f) d = -15.0f;
    } else {
      d = 15.0f;
    }
    setF30e(o, o.yawDeg + d);                  // +0x30e target yaw
  }
  // Ease yaw toward +0x30e at 10 deg/s (OBSERVED C(0x497fc0)).
  const float target = f30e(o);
  if (target <= o.yawDeg) {
    if (target < o.yawDeg) {
      o.yawDeg -= dt * 10.0f;
      if (o.yawDeg < target) o.yawDeg = target;
    }
  } else {
    o.yawDeg += dt * 10.0f;
    if (target < o.yawDeg) o.yawDeg = target;
  }
  // Tail (OBSERVED): pos + center snapshot, both flags set.
  o.field2d2[0] = o.pos[0];
  o.field2d2[1] = o.pos[1];
  o.field2d2[2] = o.pos[2];
  o.field2d2[3] = o.field1c[0];
  o.field2d2[4] = o.field1c[1];
  o.field2d2[5] = o.field1c[2];
  o.field2d1 = 1;
  o.field2d0 = 1;
}

// ---------------------------------------------------------------------------
// FUN_0045d174 — the swept-move primitive
// ---------------------------------------------------------------------------

const CollisionPoly* objectSweptMove(TraversalRuntime& rt,
                                     DynamicObject& o, DynamicArena& home,
                                     TraversalArena* otherArena,
                                     float dx, float dy, float dz,
                                     const float extOfs[6], float scale,
                                     const CollisionNode** outNode) {
  const CollisionPoly* hit = nullptr;
  const CollisionNode* node = nullptr;
  if (dx * dx + dy * dy + dz * dz == 0.0f) return nullptr;
  const bool clampValid = (o.clampBox[3] - o.clampBox[0]) > 0.0f;
  if ((o.col.flags148 & 4) == 0) {
    // Ghost/uncollidable — direct move + the +0x27c clamp box
    // (validity = X span only, OBSERVED).
    o.pos[0] += dx;
    o.pos[1] += dy;
    o.pos[2] += dz;
    if (clampValid) {
      for (int i = 0; i < 3; ++i) {
        if (o.pos[i] < o.clampBox[i]) o.pos[i] = o.clampBox[i];
        if (o.clampBox[3 + i] < o.pos[i]) o.pos[i] = o.clampBox[3 + i];
      }
    }
    return nullptr;
  }
  // Default extVec when the caller passes none (OBSERVED):
  //   {quarter W, quarter H, halfTop, midOfs.x, midOfs.y,
  //    halfTop + (dz!=0 ? 0.05 : 0.5)} — C(0x49832c)=0.25,
  //   C(0x498334)=0.5, C(0x49833c)=0.05.
  float defExt[6];
  if (extOfs == nullptr) {
    defExt[0] = (o.col.aabb[3] - o.col.aabb[0]) * 0.25f;
    defExt[1] = (o.col.aabb[4] - o.col.aabb[1]) * 0.25f;
    const float halfTop = std::fabs(o.col.aabb[5] - o.pos[2]) * 0.5f;
    defExt[2] = halfTop;
    defExt[3] = (o.col.aabb[3] + o.col.aabb[0]) * 0.5f - o.pos[0];
    defExt[4] = (o.col.aabb[4] + o.col.aabb[1]) * 0.5f - o.pos[1];
    defExt[5] = halfTop + ((dz != 0.0f) ? 0.05f : 0.5f);
    extOfs = defExt;
  }
  float start[3] = {o.pos[0] + extOfs[3], o.pos[1] + extOfs[4],
                    o.pos[2] + extOfs[5]};
  float target[3] = {start[0] + dx, start[1] + dy, start[2] + dz};
  if (clampValid) {
    for (int i = 0; i < 3; ++i) {
      if (target[i] < o.clampBox[i]) target[i] = o.clampBox[i];
      if (o.clampBox[3 + i] < target[i]) target[i] = o.clampBox[3 + i];
    }
  }
  float outPos[3] = {start[0], start[1], start[2]};
  {
    CollisionState scratch{};
    scratch.pos[0] = start[0];
    scratch.pos[1] = start[1];
    scratch.pos[2] = start[2];
    // FUN_00407fc0(flag=2) — the object's slide-iteration budget.
    hit = collisionSweep(scratch, start, target, /*flag*/ 2, home.col,
                         extOfs, scale, outPos, &node);
  }
  if ((o.col.flags14a & 8) != 0 && hit == nullptr) {
    // Partner-arena retry (OBSERVED): restart from the failed sweep's
    // end point against the other arena; on contact queue +0x2bc.
    TraversalArena* other = otherArena ? otherArena : migrationPartner(rt, home);
    if (other != nullptr) {
      float retryStart[3] = {outPos[0], outPos[1], outPos[2]};
      CollisionState scratch{};
      hit = collisionSweep(scratch, retryStart, target, /*flag*/ 2,
                           other->dyn.col, extOfs, scale, outPos, &node);
      if (hit != nullptr) {
        o.pendingArena = &other->dyn;         // +0x2bc = other arena
      }
    }
  }
  o.pos[0] += outPos[0] - start[0];
  o.pos[1] += outPos[1] - start[1];
  o.pos[2] += outPos[2] - start[2];
  if (hit != nullptr) {
    // FUN_0040b5d0 per contact (secondary/context = 0x10).
    objectSurfaceDispatch(o, hit, outPos, start);
  }
  if (outNode != nullptr) *outNode = node;
  return hit;
}

// ---------------------------------------------------------------------------
// FUN_0045b9fc — gravity (+0x148&2 gate inside)
// ---------------------------------------------------------------------------

void objectGravity(TraversalRuntime& rt, DynamicObject& o,
                   DynamicArena& home, float dt) {
  if ((o.col.flags148 & 2) == 0) return;
  o.field30 -= o.field48 * dt;                 // +0x30 -= +0x48*dt
  // Water/medium damp — skipped for raw-matrix objects while the
  // weapon-5 charge level sits at 1 (OBSERVED 0x541498 gate).
  if (!(rt.field541498 == 1 && (o.col.flags148 & 0x40) != 0)) {
    float vec[3] = {0, 0, 0};
    if (home.owner != nullptr &&
        surfaceVolumeQuery(home.owner->surface, /*mask*/ 2, o.pos, dt,
                           vec) != 0) {
      // (+0x148 dword & 0x45000) == 0x1000 AND command != 0x80/4.
      if ((flagsDword(o) & 0x45000) == 0x1000 && o.field30a != 0x80u &&
          o.field30a != 4u) {
        o.field28 *= 0.1f;                     // C(0x49825c) = 0.1
        o.field2c *= 0.1f;
      }
    }
  }
  if (o.field30 < -220.0f) o.field30 = -220.0f;   // terminal 0xc35c0000
}

// ---------------------------------------------------------------------------
// FUN_0045bac0 — object collision/integration
// ---------------------------------------------------------------------------

void objectCollide(TraversalRuntime& rt, DynamicObject& o,
                   DynamicArena& home, TraversalArena* otherArena,
                   float dt) {
  o.col.flags14c &= 0xec;                      // clear contact bits 0/1/4
  // Drag — 3D for non-gravity objects, XY-only for +0x148&2 (OBSERVED).
  if ((o.col.flags148 & 2) == 0) {
    const float v2 = o.field2c * o.field2c + o.field28 * o.field28 +
                     o.field30 * o.field30;
    if (v2 != 0.0f) {
      const float sp = std::sqrt(v2);
      float s = sp - o.field44 * dt;
      if (s < 0.0f) s = 0.0f;
      s /= sp;
      o.field28 *= s;
      o.field2c *= s;
      o.field30 *= s;
    }
  } else {
    const float v2 = o.field2c * o.field2c + o.field28 * o.field28;
    if (v2 != 0.0f) {
      const float sp = std::sqrt(v2);
      float s = sp - o.field44 * dt;
      if (s < 0.0f) s = 0.0f;
      s /= sp;
      o.field28 *= s;
      o.field2c *= s;
    }
  }
  // Swept-box extents + center offsets (OBSERVED FUN_0045bac0):
  //   ext[0..1] = quarter aabb span, ext[2] = 0.5 + |hiZ-pos.z|*0.5,
  //   ext[3..4] = aabb mid - pos, ext[5] = 0.
  float ext[6];
  ext[0] = (o.col.aabb[3] - o.col.aabb[0]) * 0.25f;   // C(0x49826c)
  ext[1] = (o.col.aabb[4] - o.col.aabb[1]) * 0.25f;
  ext[2] = std::fabs(o.col.aabb[5] - o.pos[2]) * 0.5f; // C(0x498274)
  ext[3] = (o.col.aabb[3] + o.col.aabb[0]) * 0.5f - o.pos[0];
  ext[4] = (o.col.aabb[4] + o.col.aabb[1]) * 0.5f - o.pos[1];
  ext[5] = 0.0f;
  ext[2] += 0.5f;
  // Conveyor/floor-handle displacement (FUN_00412ef0 against the
  // ARENA surface records).
  float conv[3] = {0.0f, 0.0f, 0.0f};
  if (o.field2b0 != nullptr && home.owner != nullptr) {
    surfaceConveyorDelta(home.owner->surface, o.field2b0, dt, conv);
  }
  // Requested displacement: (vel + +0x294..0x29c impulse) * dt + conv.
  float disp[3];
  disp[0] = (o.field28 + o.animImpulse[0]) * dt + conv[0];
  disp[1] = (o.field2c + o.animImpulse[1]) * dt + conv[1];
  disp[2] = (o.field30 + o.animImpulse[2]) * dt + conv[2];
  // Pass 1 — XY sweep (dz=0, scale 0.75).
  const CollisionNode* node = nullptr;
  const CollisionPoly* hit = objectSweptMove(
      rt, o, home, otherArena, disp[0], disp[1], 0.0f, ext, 0.75f, &node);
  if (hit != nullptr) {
    o.col.flags14c |= 1;                       // wall contact
    if ((o.col.flags14b & 0x20) != 0) {
      // Bounce — reflect velocity about the node normal * -1.8
      // (OBSERVED C(0x49827c)).
      const float nx = node ? node->nx : 0.0f;
      const float ny = node ? node->ny : 0.0f;
      const float nz = node ? node->nz : 0.0f;
      const float k = (nz * o.field30 + ny * o.field2c + nx * o.field28) *
                      -1.8f;
      o.field28 += nx * k;
      o.field2c += ny * k;
      o.field30 += nz * k;
      disp[2] += nz * k * dt;
    }
  }
  // Pass 2 — Z sweep (ext[2] += zBias*0.5, ext[5] = extZ - zBias*0.5,
  // scale 0.5). The +0x2b4 normal node is written into the object.
  const float extZ = ext[2] - 0.5f;
  ext[2] = extZ + o.zBias * 0.5f;
  ext[5] = extZ - o.zBias * 0.5f - 0.5f + 0.5f;   // = extZ - zBias*0.5
  ext[5] = extZ - o.zBias * 0.5f;
  const CollisionNode* node2 = nullptr;
  const CollisionPoly* hit2 =
      objectSweptMove(rt, o, home, otherArena, 0.0f, 0.0f, disp[2], ext,
                      0.5f, &node2);
  o.field2b0 = hit2;
  o.field2b4 = node2;
  if (hit2 != nullptr) {
    if (disp[2] > 0.0f) {
      o.field2b0 = nullptr;                    // ceiling hit — no floor
    } else {
      o.col.flags14c |= 2;                     // floor contact
    }
    if ((o.col.flags14b & 0x20) == 0) {
      o.field30 = 0.0f;                        // stop vertical motion
    } else {
      const float nx = node2 ? node2->nx : 0.0f;
      const float ny = node2 ? node2->ny : 0.0f;
      const float nz = node2 ? node2->nz : 0.0f;
      const float k = (nz * o.field30 + ny * o.field2c + nx * o.field28) *
                      -1.8f;
      o.field28 += nx * k;
      o.field2c += ny * k;
      o.field30 += nz * k;
    }
  }
  // Impulse consumed every frame (OBSERVED clear order).
  o.animImpulse[2] = 0.0f;
  o.animImpulse[1] = 0.0f;
  o.animImpulse[0] = 0.0f;
  // Kill plane — pos.z below home arena deepFloorZ - 200 (OBSERVED
  // C(0x49828c) f32 -200).
  if (home.owner != nullptr &&
      o.pos[2] < home.col.deepFloorZ + (-200.0f)) {
    objectFloorDeath(rt, o);
  }
  // +0x14a&8 portal migration queue (FUN_00435178 seam — the sweep's
  // partner retry is the primary writer).
  if ((o.col.flags14a & 8) != 0 && o.pendingArena == nullptr) {
    portalScanSeam(rt, o);
  }
}

// ---------------------------------------------------------------------------
// FUN_0045d578 — rolling-contact ride update (+0x148 & 0x40): the frame
// XY delta rolls the +0x302 raw matrix (log/roller objects). OBSERVED
// from raw disasm: rate = 360*dist/(cnt*2pi) with cnt = +0x326 read as
// f32 (clamped to 1.0 when <= 0); angles = degAsin(d{xy}/dist)*rate/90;
// rawMatrix = Ry(b')*Rx(-a') * rawMatrix (row-major premultiply).
// ---------------------------------------------------------------------------

void objectRollRide(DynamicObject& o) {
  const float dx = o.pos[0] - o.prevPos[0];
  const float dy = o.pos[1] - o.prevPos[1];
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (dist <= 0.0f) return;
  const float inv = 1.0f / dist;
  const float a = degAsin(dy * inv);
  const float b = degAsin(dx * inv);
  float cnt = std::bit_cast<float>(o.connMaskLock);  // +0x326 as f32
  if (cnt <= 0.0f) cnt = 1.0f;
  const float rate = dist / (cnt * 6.28318531f) * 360.0f;
  const float a2 = a * (1.0f / 90.0f) * rate;
  const float b2 = b * (1.0f / 90.0f) * rate;
  // FUN_0046b480 — R = Ry(b2)*Rx(-a2); FUN_00437f98 writes cos,sin.
  float s1, c1, s2, c2;
  sincosDeg(-a2, &s1, &c1);
  sincosDeg(b2, &s2, &c2);
  const float r[9] = {c2, 0.0f, s2,
                      s1 * s2, c1, -s1 * c2,
                      -c1 * s2, s1, c1 * c2};
  const float* m = o.rawMatrix;
  float out[9];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      out[i * 3 + j] = r[i * 3 + 0] * m[0 * 3 + j] +
                       r[i * 3 + 1] * m[1 * 3 + j] +
                       r[i * 3 + 2] * m[2 * 3 + j];
  std::memcpy(o.rawMatrix, out, sizeof(out));
}

// ---------------------------------------------------------------------------
// FUN_0045ab44 — command runner (+0x14b&0x40)
// ---------------------------------------------------------------------------

void objectCommandRunner(TraversalRuntime& rt, DynamicObject& o,
                         DynamicArena& home, float dt) {
  if ((o.col.flags148 & 0x20) == 0) {
    // Spawn-slot check (FUN_00454c6c mask 2 = arena objects, mask 1 =
    // player box) — arms +0x148|0x20 early when a contact exists.
    const int hit2 = objectMeleeScan(rt, o, home, /*dmg*/ 0, /*mask*/ 2,
                                     /*killOnHit*/ false);
    const int hit1 = objectMeleeScan(rt, o, home, /*dmg*/ 0, /*mask*/ 1,
                                     /*killOnHit*/ false);
    if (hit1 != 0 || hit2 != 0) o.col.flags148 |= 0x20;
    if ((o.col.flags14c & 2) == 0) {
      // Airborne — seek the +0x302 target XY at 50 u/s (OBSERVED
      // C(0x4981c0)), each axis independently clamped to the target.
      const float step = dt * 50.0f;
      if (o.field302ptr != nullptr) {
        const float tx = o.field302ptr[0];
        const float ty = o.field302ptr[1];
        if (o.pos[0] < tx && (o.pos[0] += step, tx < o.pos[0]))
          o.pos[0] = tx;
        if (tx < o.pos[0] && (o.pos[0] -= step, o.pos[0] < tx))
          o.pos[0] = tx;
        if (o.pos[1] < ty && (o.pos[1] += step, ty < o.pos[1]))
          o.pos[1] = ty;
        if (ty < o.pos[1] && (o.pos[1] -= step, o.pos[1] < ty))
          o.pos[1] = ty;
      }
    }
  }
  if ((o.col.flags14c & 2) != 0) {
    // Landed — fuse counts down at dt; die when it expires.
    setF306(o, f306(o) - dt);
    o.col.flags148 |= 0x20;
    if (f306(o) < 0.0f) {
      objectDieFacingPlayer(rt, o);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// FUN_00454c6c — melee proximity/damage scan
// ---------------------------------------------------------------------------

int objectMeleeScan(TraversalRuntime& rt, DynamicObject& o,
                    DynamicArena& home, int dmg, std::uint32_t mask,
                    bool killOnHit) {
  int any = 0;
  // FUN_0045c138 — scale a copy of +0x198 about its center by +0x2c0.
  float box[6];
  std::memcpy(box, o.col.aabb, sizeof(box));
  for (int i = 0; i < 3; ++i) {
    const float half = (box[3 + i] - box[i]) * 0.5f * o.field2c0;
    const float mid = (box[3 + i] + box[i]) * 0.5f;
    box[i] = mid - half;
    box[3 + i] = mid + half;
  }
  if ((mask & 1) != 0) {
    // Player AABB (0x540c30 = cs.playerBox) -> FUN_0046771c.
    if (aabbOverlap(box, rt.cs.playerBox)) {
      playerDamageApply(rt, dmg, o.pos);
      any = 1;
    }
    // Per-element test against the player box too (OBSERVED: element
    // loop runs under mask1, damage on each overlap).
    for (std::size_t e = 0; e < o.model.elems.size(); ++e) {
      if (((1u << (e & 31)) & o.col.elemMaskB) != 0) continue;
      if (aabbOverlap(box, o.model.elems[e].aabb)) {
        playerDamageApply(rt, dmg, o.pos);
        any = 1;
      }
    }
  }
  if ((mask & 2) != 0) {
    for (auto& up : home.storage) {
      DynamicObject& t = *up;
      if (&t == &o || !t.col.named || t.col.model == nullptr) continue;
      if ((flagsDword(t) & 0x820) != 0) continue;      // +0x148 skip
      if (!aabbOverlap(box, t.col.aabb)) continue;
      any = 1;
      // Punch/damage marks (OBSERVED +0x21e=0xfd, +0x21d=0xfc, yaw
      // + pitch from the scanner, health -= dmg).
      t.field21e = 0xfd;
      t.field21d = 0xfc;
      t.field224 = o.yawDeg;
      t.field228 = o.bankDeg;
      if (t.health < 65000) t.health -= dmg;
      if (t.health < 1) objectDieFacingPlayer(rt, t);
    }
  }
  if (any != 0 && killOnHit) {
    o.health = 0;
    objectDieFacingPlayer(rt, o);
  }
  return any;
}

// ---------------------------------------------------------------------------
// FUN_00459618 — AABB-delta touch scan
// ---------------------------------------------------------------------------

int objectTouchScan(TraversalRuntime& rt, DynamicObject& o,
                    DynamicArena& home, TraversalArena* otherArena,
                    std::uint32_t exclMask, std::uint32_t reqMask,
                    DynamicObject** hitObj) {
  const float delta[3] = {o.pos[0] - o.prevPos[0], o.pos[1] - o.prevPos[1],
                          o.pos[2] - o.prevPos[2]};
  // Own AABB expanded along the frame delta (OBSERVED sign per axis).
  float box[6];
  std::memcpy(box, o.col.aabb, sizeof(box));
  for (int i = 0; i < 3; ++i) {
    if (delta[i] > 0.0f) box[3 + i] += delta[i];
    else box[i] += delta[i];
  }
  int hit = 0;
  for (int pass = 0; pass < 2; ++pass) {
    DynamicArena* arena = (pass == 0) ? &home
                                      : (otherArena ? &otherArena->dyn
                                                    : nullptr);
    if (arena == nullptr) break;
    for (auto& up : arena->storage) {
      DynamicObject& t = *up;
      if (&t == &o) continue;
      if (!t.col.named || t.col.model == nullptr) continue;
      const std::uint32_t tflags = flagsDword(t);
      if ((tflags & exclMask) != 0) continue;
      if (reqMask != 0 && (tflags & reqMask) == 0) continue;
      if (!aabbOverlap(box, t.col.aabb)) continue;
      for (std::size_t e = 0; e < t.model.elems.size(); ++e) {
        const CollisionElement& el = t.model.elems[e];
        if (!aabbOverlap(box, el.aabb)) continue;
        // Minkowski-grow the element box by the scanner's own AABB
        // span (OBSERVED: span = obj+0x1a4..-0x198.. per axis).
        float mink[6];
        for (int i = 0; i < 3; ++i) {
          const float span = o.col.aabb[3 + i] - o.col.aabb[i];
          mink[i] = el.aabb[i] - span;
          mink[3 + i] = el.aabb[3 + i] + span;
        }
        float clampPt[3], altPt[3];
        const int rc = collisionSegAabbResolve(o.prevPos, o.pos, mink,
                                               clampPt, altPt);
        if (rc == 0) continue;
        o.col.flags14c |= 0x10;
        hit = 1;
        if (hitObj != nullptr) *hitObj = &t;
        // rc==2 (inside) -> alt point; rc==1 (face) -> clamp point.
        const float* pt = (rc == 2) ? altPt : clampPt;
        o.pos[0] = pt[0];
        o.pos[1] = pt[1];
        o.pos[2] = pt[2];
        if ((o.col.flags14a & 8) != 0) {
          // Arena migration queue: cross-arena contact queues +0x2bc,
          // same-arena contact clears it.
          if (o.arena != t.arena) o.pendingArena = t.arena;
          else if (o.pendingArena != nullptr) o.pendingArena = nullptr;
        }
      }
    }
  }
  (void)rt;
  return hit;
}

// ---------------------------------------------------------------------------
// FUN_0045828c — immediate record teardown
// ---------------------------------------------------------------------------

void objectTeardownNow(TraversalRuntime& rt, DynamicObject& o) {
  // OBSERVED: memset except +0x00 (list link) and +0x60 (arena) —
  // +0x06 named clears, so every live gate (named/model/health) drops.
  // The port preserves the collision-node identity + arena membership
  // and wipes the gameplay record.
  rt.seams.objectTeardownCalls++;
  if (rt.fieldB85c == &o) rt.fieldB85c = nullptr;   // view-anchor clear
  if (rt.eventTimerObj == &o) rt.eventTimerObj = nullptr;
  if (rt.cmdObj5c == &o) rt.cmdObj5c = nullptr;
  if (rt.cmdObj60 == &o) rt.cmdObj60 = nullptr;
  surfaceRecordsDestroy(o.surface);
  o.surface = SurfaceObjectState{};
  const bool keepNamedList = true;
  (void)keepNamedList;
  o.col.named = false;
  o.col.model = nullptr;
  o.health = 0;
  o.field108 = nullptr;
  o.field230 = nullptr;
  o.field110 = nullptr;
  o.fieldEC = nullptr;
  o.field138 = nullptr;
  o.field278 = nullptr;
  o.field158 = nullptr;
  o.moverChild = nullptr;                  // +0x312 (mover union)
  o.field15c.clear();
  o.field11e = 0;
  o.field22c = 0.0f;
  o.pendingArena = nullptr;
  o.field2b0 = nullptr;
  o.field2b4 = nullptr;
  o.animRec = nullptr;
  o.animRecNear = nullptr;
  o.animRecFar = nullptr;
}

// ---------------------------------------------------------------------------
// FUN_00458354 — kill-plane / floor death
// ---------------------------------------------------------------------------

void objectFloorDeath(TraversalRuntime& rt, DynamicObject& o) {
  if (o.field110 == nullptr) {
    objectTeardownNow(rt, o);
    return;
  }
  // Deferred script handoff (the +0x110 pending-script form).
  o.field11e = 0;
  o.health = 0;
  o.field22c = 0.0f;
  const void* pc = o.field110;
  o.field110 = nullptr;
  o.field108 = pc;
  o.field230 = pc;
  o.col.flags148 |= 0x20;
  // Deep-floor snap: below arena deepFloorZ - 150 -> clamp + clear
  // gravity (OBSERVED C(0x497ff4)/C(0x497ff8)).
  if (o.arena != nullptr &&
      o.pos[2] < o.arena->col.deepFloorZ + (-150.0f)) {
    o.field30 = 0.0f;
    o.pos[2] = o.arena->col.deepFloorZ + (-150.0f);
    o.col.flags148 &= ~0x2u;
  }
}

// ---------------------------------------------------------------------------
// FUN_004574d0 — pending-arena transfer (+0x2bc) + the FUN_0045a2d0 /
// FUN_0045a3b0 leave/enter helpers
// ---------------------------------------------------------------------------

namespace {

// Dword view of the +0x148..+0x14b flag word — keeps the flags149
// byte-mirror coherent (the port models +0x149 both inside the
// flags148 u16 and as a standalone byte).
void setFlagDword148(CollisionObject& c, std::uint32_t v) {
  c.flags148 = static_cast<std::uint16_t>(v & 0xffffu);
  c.flags149 = static_cast<std::uint8_t>((v >> 8) & 0xffu);
  c.flags14a = static_cast<std::uint8_t>((v >> 16) & 0xffu);
  c.flags14b = static_cast<std::uint8_t>((v >> 24) & 0xffu);
}

// FUN_0045a2d0 — deactivate on migration OUT of the active arenas
// (OBSERVED): clears the view-anchor latch, clears the cmd-1/cmd-2
// registry slots for command-exec objects then tears the object down
// (FUN_0045828c), or drops the +0x158 voice handle for others.
void objectArenaDeactivate(TraversalRuntime& rt, DynamicObject& o) {
  if (rt.fieldB85c == &o) rt.fieldB85c = nullptr;
  if ((o.col.flags149 & 0x50) != 0) {
    if ((o.col.flags149 & 0x10) != 0 && o.field30a == 1)
      rt.cmdObj5c = nullptr;
    if ((o.col.flags149 & 0x10) != 0 && o.field30a == 2)
      rt.cmdObj60 = nullptr;
    objectTeardownNow(rt, o);
    return;
  }
  if (o.field158 != nullptr) {
    // FUN_004020b4(+0x158) — voice/sound stop, a counted seam.
    rt.seams.fireSoundCalls++;
    o.field158 = nullptr;
  }
}

// FUN_0045a3b0 — activate on migration INTO an active arena
// (OBSERVED): when the model record (+0xc) is unbound, reload it via
// the +0x04 enemy index (FUN_00403720) or the +0x316 named lookup
// (FUN_00403538 when the index is 0xffff); an armed anim (+0x114)
// re-syncs (+0xe4 = 0xffff) and the +0x148 flag dword becomes
// FRNDINT(+0xdc + 1.0) | 0x80000000 — the anim frame marker replaces
// the flag word (OBSERVED quirk, written verbatim).
void objectArenaActivate(TraversalRuntime& rt, DynamicObject& o) {
  if (o.col.model == nullptr) {
    if (o.enemyIndex == 0xffff) {
      // FUN_00403538(+0x316, &+0x322, +0x31a, +0x31e) — named model
      // lookup, a counted seam (the port binds via enemyIndex).
      rt.seams.classLookupCalls++;
    } else {
      const RuntimeModel* src =
          traversalModelFor(o.enemyIndex, &rt.level);
      if (src != nullptr) {
        o.model = deepCopyModel(*src);
        o.syncCollisionView();
      }
    }
    if (o.animRec != nullptr) {
      o.animFrame = -1;
      setFlagDword148(
          o.col,
          static_cast<std::uint32_t>(
              static_cast<std::int32_t>(
                  static_cast<float>(o.animAcc + 1.0f))) |
              0x80000000u);
    }
  }
  // Voice restart: +0x158 == 0 with a nonempty +0x15c name restarts
  // the object voice (FUN_004020b4-ish path at 0x45a49d) — counted.
  if (o.field158 == nullptr && !o.field15c.empty())
    rt.seams.fireSoundCalls++;
}

} // namespace

// Returns 1 when the object was transferred (skip the rest of its
// update this frame), 0 to continue — the FUN_004574d0 contract.
int objectArenaTransfer(TraversalRuntime& rt, DynamicObject& o,
                        DynamicArena& home) {
  DynamicArena* dst = o.pendingArena;
  if (o.arena == dst) {
    // Same-arena pend — FUN_00408eb0(0x497f64) warn + clear; the
    // object continues its update (OBSERVED return-0 path).
    o.pendingArena = nullptr;
    ++rt.seams.objectMigrations;
    return 0;
  }
  // Unlink + relink. The original's not-found path warns and returns
  // 0 — unreachable in the port (list membership is authoritative).
  home.transfer(o, *dst);
  // Connector yaw flip (OBSERVED 0x45754b..0x457577): +0x4c += 180,
  // wrap by -360 when >= 360.
  if ((o.col.flags14a & 0x10) != 0) {
    o.yawDeg += 180.0f;
    if (o.yawDeg >= 360.0f) o.yawDeg += -360.0f;
  }
  if (dst->owner == rt.cur || dst->owner == rt.partner) {
    objectArenaActivate(rt, o);
  } else {
    objectArenaDeactivate(rt, o);
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Command bodies — FUN_0045897c callees
// ---------------------------------------------------------------------------

namespace {

// Shared "anim done" test used by the bodies: +0x114==0 OR
// +0x116>>16 == -0x100 (the animLatch 0xff00 form).
bool cmdAnimDone(const DynamicObject& o) { return o.animDone(); }

// Arm a command animation: +0x114=rec, +0xe0=30.0, +0x118/+0xe4=-1,
// +0xdc=0 (OBSERVED arming block repeated across the bodies). `rec` is
// the table-loaded anim record — an asset pointer the port resolves
// through the seams (nullptr keeps animDone() true, matching the
// original's unloaded-asset behavior).
void cmdArmAnim(DynamicObject& o, const void* rec, float acc) {
  o.animRate = 30.0f;              // +0xe0
  o.animLatch = -1;                // +0x118 = 0xffff
  o.animFrame = -1;                // +0xe4 = 0xffff
  o.animAcc = acc;                 // +0xdc
  o.animRec = rec;                 // +0x114
}

// FX/spawn seams — counted, presentation-only (FUN_00402160 child FX
// spawn, FUN_00405ffc afterimage, FUN_00403f6c/0x4108 spawn FX,
// FUN_004575fc remnant, FUN_004387ec anim-ctx spawn).
DynamicObject* fxChildSpawn(TraversalRuntime& rt, DynamicObject& o) {
  rt.seams.reticleSpawnCalls++;    // same FUN_00402160 seam family
  (void)o;
  return nullptr;                  // the FX child is presentation-only
}

void fxAfterimage(TraversalRuntime& rt, DynamicObject& o) {
  rt.seams.remnantSpawnCalls++;
  (void)o;
}

void fxShockwave(TraversalRuntime& rt, DynamicObject& o, float scale) {
  // FUN_004575fc — the detonation remnant seam (CombatFxEvent kind
  // kDetonation carries scale).
  rt.seams.remnantSpawnCalls++;
  CombatFxEvent ev;
  ev.kind = CombatFxKind::kDetonation;
  ev.obj = &o;
  ev.variant = static_cast<int>(scale * 1000.0f);
  ev.pos[0] = o.pos[0];
  ev.pos[1] = o.pos[1];
  ev.pos[2] = o.pos[2];
  rt.combatFx.push_back(ev);
}

// cmd1 — FUN_00459330 (lunge): dummy-anim re-arm + touch scan + timer.
void cmdBody1(TraversalRuntime& rt, DynamicObject& o, DynamicArena& home,
              TraversalArena* other) {
  rt.cmdObj5c = &o;                            // DAT_00540e5c
  o.field30e -= 1;                             // tick countdown
  if (cmdAnimDone(o)) {
    if (o.field158 != nullptr) {
      // FUN_004020b4 — release the previous FX child (seam).
      o.field158 = nullptr;
      o.field15c.clear();
    }
    o.field15c = "DUMMY";                      // s_DUMMY_004980fc
    o.field158 = fxChildSpawn(rt, o);          // FUN_00402160
    cmdArmAnim(o, /*rec*/ nullptr, 0.0f);      // _DAT_0054c6ac seam
    o.col.flags148 |= 8;
  }
  objectTouchScan(rt, o, home, other, /*excl*/ 0x810, /*req*/ 0, nullptr);
  if (o.field30e < 1) {
    rt.cmdObj5c = nullptr;
    objectDieFacingPlayer(rt, o);
  }
}

// cmd2 — FUN_00459c5c (spin/detonate): yaw spin while +0x118==0, then
// detonate on anim completion.
void cmdBody2(TraversalRuntime& rt, DynamicObject& o, DynamicArena& home,
              TraversalArena* other) {
  (void)home;
  (void)other;
  rt.cmdObj60 = &o;                            // DAT_00540e60
  if (o.animLatch == 0) {
    o.yawDeg += kDt * 235.0f;                  // C(0x49814c) spin rate
    o.field30e -= 1;
    if (o.field30e < 1) o.animLatch = -1;      // +0x118 = 0xffff
    return;
  }
  if (cmdAnimDone(o)) {
    splashDamage(rt, o.pos, 450.0f, 80.0f, 1, nullptr, 6, -9);   // 0x1c2
    splashDamage(rt, o.pos, 67.0f, 80.0f, 1, nullptr, 1, -9);    // 0x43
    fxShockwave(rt, o, 3.0f);                  // FUN_004575fc 0x40400000
    objectTeardownNow(rt, o);
  }
}

// cmd3 — FUN_00459968 (teleport): yaw spin + afterimage burst every 15
// ticks below the +0x302 threshold, die at 0.
void cmdBody3(TraversalRuntime& rt, DynamicObject& o) {
  o.yawDeg += kDt * 360.0f;                    // C(0x498120) spin rate
  o.field30e -= 1;
  if (o.field30e < static_cast<std::int32_t>(o.field302)) {
    o.field302 -= 0xf;                         // next burst threshold
    fxAfterimage(rt, o);                       // FUN_00405ffc
  }
  if (o.field30e <= 0) objectDieFacingPlayer(rt, o);
}

// cmd4 — FUN_00459160 (roar): anim-frame-gated arena damage scan.
void cmdBody4(TraversalRuntime& rt, DynamicObject& o, DynamicArena& home) {
  if (o.animRec == nullptr || o.animLatch == -256 /*0xff00*/) {
    objectDieFacingPlayer(rt, o);
    return;
  }
  // The +0x302 stage gates on the anim cursor (+0xe2>>16). The port's
  // cursor is animFrame (+0xe4); the original's keyframe thresholds
  // (DAT_0049b884 table) are asset-driven — modeled by the stage
  // counter advancing when the cursor passes each stage.
  if (static_cast<std::int32_t>(o.field302) <= o.animFrame) {
    o.field302 += 1;
    if (static_cast<std::int32_t>(o.field302) > 3 &&
        rt.lastContactPoly != nullptr) {
      rt.vert.landingAccum = 5.0f;             // DAT_00540d5c shake
    }
    if (rt.camera.shakeMag < 5.0f) rt.camera.shakeMag = 5.0f;  // FUN_00465200
    // Arena scan — every named object in the home arena takes the
    // proximity damage marks (OBSERVED FUN_00459160 loop).
    for (auto& up : home.storage) {
      DynamicObject& t = *up;
      if (&t == &o || !t.col.named) continue;
      if ((t.col.flags148 & 0x1030) != 0) continue;
      const float cx = (t.col.aabb[0] + t.col.aabb[3]) * 0.5f;
      const float cy = (t.col.aabb[1] + t.col.aabb[4]) * 0.5f;
      const float b = bearingDeg(cy - rt.cs.pos[1], cx - rt.cs.pos[0]);
      if (t.health < 65000) t.health -= 4;
      t.field21e = 0xff;
      t.field21d = 0xfd;
      t.field228 = 0.0f;
      t.field224 = b;
      if (t.health > 0) continue;
      objectKillTally(rt, t, 1);
      objectDeathBoundary(rt, t, t.pos, b + 180.0f);
    }
  }
}

// cmd7 — FUN_00459a9c (morph burst): connector-unlock + radial damage
// on anim completion; shake while it plays.
void cmdBody7(TraversalRuntime& rt, DynamicObject& o, DynamicArena& home,
              TraversalArena* other) {
  int i3 = rt.fieldDa4;                        // DAT_00540da4
  if (cmdAnimDone(o)) {
    // Unlock nearby connectors (+0x14a&0x10 within dist^2 < 2500 =
    // 50 u, OBSERVED FUN_00430190 + C(0x498148)): +0x312 rewritten as
    // (b & 0x1f) | 0x80 — the original CLEARS bits 5/6 while setting
    // the unlock bit.
    for (int pass = 0; pass < 2; ++pass) {
      DynamicArena* arena = (pass == 0) ? &home
                                        : (other ? &other->dyn : nullptr);
      if (arena == nullptr) break;
      for (auto& up : arena->storage) {
        DynamicObject& t = *up;
        if ((t.col.flags14a & 0x10) == 0) continue;
        const float dx = t.pos[0] - o.pos[0];
        const float dy = t.pos[1] - o.pos[1];
        const float dz = t.pos[2] - o.pos[2];
        if (dx * dx + dy * dy + dz * dz < 2500.0f)
          t.connState = (t.connState & 0x1f) | 0x80;
      }
    }
    splashDamage(rt, o.pos, 200.0f, 60.0f, 1, nullptr, 6, -8);
    splashDamage(rt, o.pos, 19.0f, 60.0f, 1, nullptr, 1, -8);
    // 16x FUN_00403f6c/0x4108 debris spawns — counted seam.
    rt.seams.remnantSpawnCalls += 16;
    fxChildSpawn(rt, o);                       // FUN_00402160 burst
    objectTeardownNow(rt, o);
    return;
  }
  if (rt.camera.shakeMag < 3.0f) rt.camera.shakeMag = 3.0f;   // 0x540ce4
  // OBSERVED: the +0xdc test is a raw dword/int compare against the
  // 70.0f bit pattern (0x428c0000). The FRNDINT operand is lost in the
  // decompile — HYPOTHESIS: it is +0xdc itself, making fieldDa4 the
  // max-anim-progress tracker (a rand-based read would explode past
  // the 0xff bound the morph transition checks).
  if (o.animAcc > 70.0f /*0x428c0000*/) {
    const int r = frndint(o.animAcc);
    if (i3 < r) rt.fieldDa4 = r;
  }
}

// cmd8 — FUN_00459450 (timed scale-shrink + anim arm): while the
// script ctx flag +0x244&1 is clear, count +0x30e down; under 601 the
// mover bit arms; at 0 the scale shrinks and dies under 0.4.
void cmdBody8(TraversalRuntime& rt, DynamicObject& o) {
  if ((o.scriptFlagsLocal & 1) == 0) {
    if (o.field30e < 0x259) o.col.flags14a |= 0x20;   // mover bit
    o.field30e -= 1;
    if (o.field30e < 1) {
      o.col.scale *= 0.9f;                     // C(0x498110) double
      if (o.col.scale < 0.1f /*C(0x498118)*/) {
        objectDieFacingPlayer(rt, o);
        return;
      }
    }
  } else {
    o.col.scale = 1.0f;
    o.col.flags14a &= ~0x20u;
  }
  if (o.animRec == nullptr && (o.scriptFlagsLocal & 2) == 0) {
    o.pitchDeg = 0.0f;                         // +0x54
    o.col.flags148 |= 6;                       // +0x148 byte |= 6
    o.scriptFlagsLocal |= 2;                   // +0x244 one-shot latch
    cmdArmAnim(o, /*rec*/ nullptr, 0.0f);      // FUN_004387ec ctx anim
    o.col.flags148 &= ~8u;
  }
}

// cmd9 — FUN_00459554 (timed + anim arm): same latch structure without
// the shrink/die tail.
void cmdBody9(TraversalRuntime& rt, DynamicObject& o) {
  (void)rt;
  if ((o.scriptFlagsLocal & 1) == 0) {
    if (o.field30e < 0x259) {
      o.col.flags14a |= 0x20;
    } else {
      o.field30e -= 1;
    }
  } else {
    o.col.scale = 1.0f;
    o.col.flags14a &= ~0x20u;
  }
  if (o.animRec == nullptr && (o.scriptFlagsLocal & 2) == 0) {
    o.scriptFlagsLocal |= 2;
    cmdArmAnim(o, /*rec*/ nullptr, 0.0f);
    o.col.flags148 &= ~8u;
  }
}

// +0x14a&4 — FUN_004599e8 (converge-on-player lerp): pos = home +
// (1-scale)*(player-home), scale shrink in the last 15 ticks, yaw
// spin, teardown at 0.
void cmdBodyCarry(TraversalRuntime& rt, DynamicObject& o) {
  o.yawDeg -= kDt * 360.0f;                    // C(0x498128) spin
  if (o.field30e < 0xf) {
    o.col.scale = static_cast<float>(o.field30e) * (1.0f / 15.0f);
  } else {
    o.col.scale = 1.0f;
  }
  const float s = 1.0f - o.col.scale;
  o.pos[0] = s * (rt.cs.pos[0] - o.field1c[0]) + o.field1c[0];
  o.pos[1] = s * (rt.cs.pos[1] - o.field1c[1]) + o.field1c[1];
  const float pz = rt.cs.pos[2] + 3.0f;        // C(0x498130)
  o.field30e -= 1;
  o.pos[2] = s * (pz - o.field1c[2]) + o.field1c[2];
  if (o.field30e <= 0) objectTeardownNow(rt, o);
}

// cmd0x80 — the path-dropper spawn attempt (FUN_0045aaa4 BOMB slot ->
// FUN_00454794 enemy -> FUN_00454af8 spawn). The child gets the
// kamikaze block (+0x30a=5, vel.z=-5, flags|0x818a6).
void cmdDropperSpawn(TraversalRuntime& rt, DynamicObject& o,
                     DynamicArena& home, DynamicModelSource modelFor,
                     void* modelCtx) {
  // FUN_0045aaa4 — find a "BOMB_%d" element (random digit), mark the
  // element mask and take its center as the spawn point.
  float spawnPos[3] = {o.pos[0], o.pos[1], o.pos[2]};
  bool slot = false;
  const int digit = enemyRandBelow(rt.rngState, 10);
  char name[16];
  std::snprintf(name, sizeof(name), "BOMB_%d", digit);
  for (std::size_t e = 0; e < o.model.elems.size(); ++e) {
    if (o.model.elemName(e) == name) {
      o.col.elemMaskB |= (1u << (e & 31));
      const CollisionElement& el = o.model.elems[e];
      for (int i = 0; i < 3; ++i)
        spawnPos[i] = (el.aabb[i] + el.aabb[3 + i]) * 0.5f;
      slot = true;
      break;
    }
  }
  if (!slot) {
    rt.seams.mountUnmountCalls++;              // FUN_00408eb0 fallback
    return;
  }
  // FUN_00454794 — the spawn uses the object's own model name (the
  // dropper re-spawns its class as a falling bomb).
  rt.seams.classLookupCalls++;
  const int idx = rt.level.enemies.indexOf(o.model.modelName());
  if (idx < 0) return;
  const RuntimeModel* src = modelFor ? modelFor(idx, modelCtx) : nullptr;
  if (src == nullptr) return;
  DynamicObject& child = home.allocFront();
  child.model = deepCopyModel(*src);
  child.syncCollisionView();
  child.enemyIndex = static_cast<std::uint16_t>(idx);
  child.arena = &home;
  child.setPosition(spawnPos[0], spawnPos[1], spawnPos[2]);
  child.prevPos[0] = spawnPos[0];
  child.prevPos[1] = spawnPos[1];
  child.prevPos[2] = spawnPos[2];
  initObjectCollision(child);
  child.field30e = 900;                        // +0x30e fuse
  child.field44 = 0.0f;                        // +0x44 drag
  child.col.scale = 1.0f;                      // +0x58
  child.field30a = 5;                          // +0x30a cmd 5
  child.field30 = -5.0f;                       // +0x30 vel.z 0xc0a00000
  // +0x148 dword |= 0x818a6 — bytes {a6,18,08,00} -> flags148|=0x18a6
  // covers +0x148/+0x149, flags14a |= 0x08.
  child.col.flags148 |= 0x18a6;
  child.col.flags14a |= 0x08;
  rebuildObjectTransform(child);               // FUN_0045612c
  child.field15c = o.model.modelName();        // PTR_DAT_0049b854 token
  child.field158 = fxChildSpawn(rt, child);    // FUN_00402160
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_0045897c — enemy command dispatch (+0x149&0x10 gate)
// ---------------------------------------------------------------------------

void enemyCommandDispatch(TraversalRuntime& rt, DynamicObject& o,
                          DynamicArena& home, float dt) {
  TraversalArena* other = migrationPartner(rt, home);
  // +0x14a&4 — the converge/lerp command always runs first.
  if ((o.col.flags14a & 4) != 0) {
    cmdBodyCarry(rt, o);
    return;
  }
  if ((o.col.flags149 & 0x40) != 0) {
    // EXEC phase — per +0x30a command id.
    const std::uint32_t cmd = o.field30a;
    switch (cmd) {
      case 1: cmdBody1(rt, o, home, other); return;
      case 2: cmdBody2(rt, o, home, other); rt.cmdFlag54 = 1; return;
      case 3: cmdBody3(rt, o); return;
      case 4: cmdBody4(rt, o, home); return;
      case 7: cmdBody7(rt, o, home, other); return;
      case 8: cmdBody8(rt, o); return;
      case 9: cmdBody9(rt, o); return;
      case 0x80: {
        if (rt.field54163b == 0) {
          // Idle dropper — random spawn gate ((edx<rnd<10) => the
          // original's per-tick spawn chance window).
          if (o.fieldEC == nullptr) {
            objectTeardownNow(rt, o);
            return;
          }
          const int r = enemyRandBelow(rt.rngState, 10);
          if (r > 0 && r < 10) {
            cmdDropperSpawn(rt, o, home, traversalModelFor,
                            &rt.level);
            o.field306 += 1;                   // drop count
          }
          rt.cmdFlag54 = 1;
          return;
        }
        // Weapon-5 latch — kamikaze run: past path mid -> release path
        // to velocity, then die on wall/floor contact.
        if (o.fieldEC != nullptr) {
          const double mid =
              static_cast<double>(pathLastFrame(o.fieldEC) +
                                  pathFirstFrame(o.fieldEC)) * 0.5;
          if (static_cast<double>(o.fieldF0) < mid ||
              static_cast<double>(o.fieldF0) == mid) {
            rt.cmdFlag54 = 1;
            return;
          }
          const float inv = 1.0f / kDt;
          o.field44 = 0.0f;
          o.field28 = (o.pos[0] - o.prevPos[0]) * inv;
          o.fieldEC = nullptr;
          o.field2c = (o.pos[1] - o.prevPos[1]) * inv;
          o.col.flags148 |= 6;
          o.field30 = (o.pos[2] - o.prevPos[2]) * inv;
        }
        if ((o.col.flags14c & 3) != 0) {
          splashDamage(rt, o.pos, 450.0f, 80.0f, 1, nullptr, 6, -6);
          splashDamage(rt, o.pos, 67.0f, 80.0f, 1, nullptr, 1, -6);
          objectDieFacingPlayer(rt, o);
          return;
        }
        rt.cmdFlag54 = 1;
        return;
      }
      default:
        return;
    }
  }
  // IDLE phase (+0x149&0x40 clear).
  if (o.field30a == 1) rt.cmdObj5c = &o;
  if (o.field30a != 5 || ++rt.cmdDetonate58 > 2) rt.cmdFlag54 = 1;
  // Scale grow-in at +0x58<1 (OBSERVED C(0x4980a8) rate).
  if (o.col.scale < 1.0f) {
    o.col.scale += kDt * 3.0f;
    if (o.col.scale > 1.0f) o.col.scale = 1.0f;
  }
  // cmd 0x81 bank falloff toward -90 (C(0x4980b0) bound, C(0x4980b8)
  // rate = 45 deg/s — OBSERVED).
  if (o.field30a == 0x81u && o.bankDeg > -90.0f) {
    o.bankDeg -= kDt * 45.0f;
    if (o.bankDeg < -90.0f) o.bankDeg = -90.0f;
  }
  if (o.field30a == 8u) {
    o.pitchDeg += kDt * 30.0f;                 // C(0x4980bc) drift
  }
  DynamicObject* touched = nullptr;
  if (o.field30a != 4u) {
    // OBSERVED masks: cmd5 -> excl=0x810/req=0; cmd0x81 -> excl=0x830
    // /req=0; all other cmds -> excl=0x810/req=0x1000000.
    std::uint32_t excl = 0x810u, req = 0x1000000u;
    if (o.field30a == 5u) {
      req = 0;
    } else if (o.field30a == 0x81u) {
      excl = 0x830u;
      req = 0;
    }
    objectTouchScan(rt, o, home, other, excl, req, &touched);
    if (o.field30a == 7u && (o.col.flags14c & 0x10) != 0) {
      o.field2c = 0.0f;
      o.field28 = 0.0f;
      o.col.flags14c &= ~0x10u;
    }
  }
  o.field30e -= 1;
  if ((o.col.flags14c & 0x13) == 0 && o.field30e > 0) return;
  if (o.field30a == 4u && (o.col.flags14c & 2) == 0) return;
  // TRANSITION to exec (+0x149 |= 0x40) — vel cleared, contacts
  // cleared, gravity/sweep bits off.
  o.field30 = 0.0f;
  o.col.flags148 &= 0xf9u;
  o.col.flags149 |= 0x40;
  o.col.flags14c &= 0xec;
  o.field2c = o.field30;
  o.field28 = o.field2c;
  const std::uint32_t cmd = o.field30a;
  if (cmd < 4) {
    if (cmd == 1) {
      // Lunge — velocity along the current yaw * 5 (C(0x4980c0)
      // double, OBSERVED from the 0x458ea2 block).
      o.field30e = 0x1c2;
      float s, c;
      sincosDeg(o.yawDeg, &s, &c);
      o.field28 = c * 5.0f;
      o.field44 = 0.0f;
      o.animRate = 30.0f;
      o.col.flags148 |= 6;
      o.field2c = s * 5.0f;
      return;
    }
    if (cmd == 2) {
      cmdArmAnim(o, /*rec*/ nullptr, 0.0f);    // _DAT_0054c698 seam
      o.animLatch = 0;                         // spin phase first
      o.field30e = 600;
      return;
    }
    if (cmd == 3) {
      fxAfterimage(rt, o);
      o.field30e = 0x3c;
      o.field302 = 0x2d;
      rt.seams.fireDenyCalls++;                // FUN_00402388(0) seam
      return;
    }
    return;
  }
  if (cmd == 4) {
    cmdArmAnim(o, /*rec*/ nullptr, 0.0f);      // _DAT_0054c6b4 seam
    o.field302 = 0;
    o.field30e = 0x1e;
    cmdBody4(rt, o, home);                     // body runs immediately
    return;
  }
  if (cmd == 7) {
    // Morph — OBSERVED: the target class is the FIXED string
    // "SW_NUKE" (FUN_00454794 @ 0x4980a0), NOT the object's +0x15c.
    // -1 lookup -> teardown. Then model release (seam) + the
    // 136-byte table record's model swap + re-arm.
    rt.seams.classLookupCalls++;
    const int idx = rt.level.enemies.indexOf("SW_NUKE");
    if (idx < 0) {
      objectTeardownNow(rt, o);
      return;
    }
    const RuntimeModel* src = traversalModelFor(idx, &rt.level);
    if (src == nullptr) {
      objectTeardownNow(rt, o);
      return;
    }
    o.model = deepCopyModel(*src);
    o.syncCollisionView();
    cmdArmAnim(o, /*rec*/ nullptr, 1.0f);      // +0xdc=1.0 (OBSERVED)
    o.field30e = 900;
    o.col.flags148 &= ~6u;
    if (rt.fieldDa4 < 0xff) rt.fieldDa4 = 0xff;
    o.field15c = o.model.modelName();
    o.field158 = fxChildSpawn(rt, o);
    return;
  }
  if (cmd == 5 || cmd == 0x81) {
    // Detonate — splash + remnant + teardown at the transition.
    splashDamage(rt, o.pos, 150.0f, 40.0f, 1, touched, 6, -7);
    splashDamage(rt, o.pos, 75.0f, 40.0f, 1, touched, 1, -7);
    fxShockwave(rt, o, 2.0f);                  // FUN_004575fc 0x40000000
    objectTeardownNow(rt, o);
    return;
  }
  // cmd 8/9/others — the transition itself does nothing (the exec
  // bodies run next frame).
  (void)dt;
}

// ---------------------------------------------------------------------------
// FUN_004533d4 — the +0x11e subtype dispatcher
// ---------------------------------------------------------------------------

namespace {
// Steering family forward decls (definitions below).
void forwardGroundProbe(TraversalRuntime& rt, DynamicObject& o,
                        DynamicArena& home, const float fwd[3]);
void turnToward(DynamicObject& o, const float target[3], float dt,
                float* outDiff = nullptr);
void subtypeSeekPoint(TraversalRuntime& rt, DynamicObject& o,
                      DynamicArena& home, const float target[3],
                      float dt);
void subtypeSteeringTail(DynamicObject& o,
                         DynamicArena& home, TraversalArena* other,
                         float dt);
} // namespace

void objectSubtypeUpdate(TraversalRuntime& rt, DynamicObject& o,
                         DynamicArena& home, float dt) {
  // +0x11c tick — decremented only while +0x138 == 0 (OBSERVED).
  if (o.field138 == nullptr && o.behaviorByte > 0) o.behaviorByte -= 1;
  const std::uint8_t sub = o.field11e;
  TraversalArena* other = migrationPartner(rt, home);
  switch (sub) {
    case 0:
      return;
    case 1: {
      // Leader-follow: compute the +0x120 target from the leader's
      // yaw + the +0x12c..0x134 offsets, then move at +0x38 speed.
      if (o.field138 == nullptr) {
        o.field11e = 0;
        return;
      }
      DynamicObject* lead = o.field138;
      float sL, cL;
      sincosDeg(lead->yawDeg, &sL, &cL);
      // OBSERVED (FUN_004533d4 case 1): field120 = the leader-relative
      // anchor — side offset +0x12c rides the leader's sin term,
      // forward +0x130 the cos term, +0x134 a flat z lift.
      o.field120[0] = o.field12c[0] * sL +
                      (lead->pos[0] - o.field12c[1] * cL);
      o.field120[1] = (lead->pos[1] - o.field12c[1] * sL) -
                      o.field12c[0] * cL;
      o.field120[2] = lead->pos[2] + o.field12c[2];
      const float dx = o.field120[0] - o.pos[0];
      const float dy = o.field120[1] - o.pos[1];
      const float dz = o.field120[2] - o.pos[2];
      const float step = o.field38 * dt;
      if (step * step <= dx * dx + dy * dy + dz * dz) {
        // Steer toward the target (FUN_0045b6e0 -> FUN_0045b56c),
        // then apply the +0x38 speed impulse along the NEW yaw.
        turnToward(o, o.field120, dt);
        float s, c;
        sincosDeg(o.yawDeg, &s, &c);
        o.animImpulse[0] += o.field38 * c;
        o.animImpulse[1] += o.field38 * s;
        if (o.pos[2] <= o.field120[2] - 1.0f) {
          o.animImpulse[2] += 1.0f;
          return;
        }
        if (o.field120[2] + 1.0f <= o.pos[2]) {
          o.animImpulse[2] -= 1.0f;
          return;
        }
        o.animImpulse[2] = o.field120[2] - o.pos[2];
        return;
      }
      // Arrived — snap to the target and ease yaw to the leader's.
      o.pos[0] = o.field120[0];
      o.pos[1] = o.field120[1];
      o.pos[2] = o.field120[2];
      if (o.yawDeg < lead->yawDeg) {
        o.yawDeg += dt * 180.0f;               // C(0x497dfc)
        if (o.yawDeg > lead->yawDeg) o.yawDeg = lead->yawDeg;
      } else if (o.yawDeg > lead->yawDeg) {
        o.yawDeg -= dt * 180.0f;               // C(0x497e00)
        if (o.yawDeg < lead->yawDeg) o.yawDeg = lead->yawDeg;
      }
      return;
    }
    case 6: {
      // Fly-to-camera interest point (DAT_0054c6c4..cc + C(0x497e04)
      // z-offset = 8.0) through FUN_0045b6f8.
      float target[3] = {rt.camera.pose.pos[0], rt.camera.pose.pos[1],
                         rt.camera.pose.pos[2] + 8.0f};
      subtypeSeekPoint(rt, o, home, target, dt);
      return;
    }
    case 0x0f: {
      // Heartbeat — only while home == the player's arena; emits the
      // sound seam and re-arms the 0x540d28 countdown.
      if (home.owner != rt.cur) return;
      if ((rt.frameCounter & 0x1f) == 0) {
        rt.seams.fireSoundCalls++;             // FUN_00402160 seam
      }
      rt.fieldD2c = 10;                        // DAT_00540d28
      return;
    }
    case 0x1e: {
      // Chain drive — OBSERVED. The member index is +0x146 (spawnId,
      // the hi16 of the +0x144 dword — NOT +0x144 itself). The
      // spawnId==0 object drives: member i gets yaw = leader.yaw +
      // i*90 and anchors its model refpoint[0] to the previous
      // link's model refpoint[i], each transformed through a
      // pitch+yaw+scale-only matrix (FUN_0046b3e4 — no bank term,
      // zero translation; pos added separately). Members
      // (spawnId!=0) only validate that links 0..spawnId-1 exist in
      // this arena; a dead leader or missing link clears
      // +0x138/+0x11e. The +0x146!=0 early-out means only the
      // spawnId-0 driver ever runs the chain pass.
      DynamicObject* lead = o.field138;
      bool bad = (lead == nullptr) || !lead->col.named;
      for (int i = 0; i < static_cast<int>(o.spawnId) && !bad; ++i) {
        bool found = false;
        for (auto& up : home.storage) {
          DynamicObject& c = *up;
          if (!c.col.named || c.field138 != lead ||
              static_cast<int>(c.spawnId) != i)
            continue;
          found = true;
          break;
        }
        if (!found) bad = true;
      }
      if (bad) {
        o.field138 = nullptr;
        o.field11e = 0;
        return;
      }
      if (o.spawnId != 0) return;
      DynamicObject* prev = lead;
      for (int i = 0;; ++i) {
        DynamicObject* m = nullptr;
        for (auto& up : home.storage) {
          DynamicObject& c = *up;
          if (!c.col.named || c.field138 != lead ||
              static_cast<int>(c.spawnId) != i)
            continue;
          m = &c;
          break;
        }
        if (m == nullptr) return;
        m->yawDeg = lead->yawDeg + static_cast<float>(i * 90);
        m->prevYawDeg = m->yawDeg;
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        float pm[9], po[3], pw[3] = {0.0f, 0.0f, 0.0f};
        buildObjectMatrix(prev->pitchDeg, 0.0f, prev->yawDeg,
                          prev->col.scale, zero, pm, po);
        // OBSERVED: the previous link's refpoint index advances per
        // link (record +0x24 + 0xc*i); member's is always slot 0.
        // HYPOTHESIS: real chains stay within the 8-refpoint record —
        // the port bounds the read (original would walk past +0x84).
        if (i < static_cast<int>(prev->model.refPointCount) && i < 8)
          transformPoint(pm, po, prev->model.refPoints[i], pw);
        float mm[9], mo[3], mw[3] = {0.0f, 0.0f, 0.0f};
        buildObjectMatrix(m->pitchDeg, 0.0f, m->yawDeg, m->col.scale,
                          zero, mm, mo);
        if (m->model.refPointCount > 0)
          transformPoint(mm, mo, m->model.refPoints[0], mw);
        m->pos[0] = prev->pos[0] + pw[0] - mw[0];
        m->pos[1] = prev->pos[1] + pw[1] - mw[1];
        m->pos[2] = prev->pos[2] + pw[2] - mw[2];
        prev = m;
      }
    }
    case 0x2b:
    case 0x4e:
    case 0xc5: {
      // Steering tail — FUN_00452b80 (air) / FUN_004524e0 (ground)
      // then the shared stall/step logic (OBSERVED).
      subtypeSteeringTail(o, home, other, dt);
      return;
    }
    case 0x3d: {
      // Timed projectile-move: pos += +0x34*dt*dir, +0x302 countdown
      // -> die; stab the path into arena geometry + the player box.
      float s, c, s2, c2;
      sincosDeg(o.yawDeg, &s, &c);
      sincosDeg(o.bankDeg, &s2, &c2);
      const float from[3] = {o.pos[0], o.pos[1], o.pos[2]};
      o.pos[0] += o.field34 * dt * c * c2;
      o.pos[1] += o.field34 * dt * s * c2;
      o.pos[2] += o.field34 * dt * s2;
      setF302(o, f302(o) - dt);
      if (f302(o) <= 0.0f) {
        objectDieFacingPlayer(rt, o);
        return;
      }
      if (home.owner != nullptr) {
        float hitPos[3];
        if (collisionStab(home.col, from, o.pos, hitPos) != nullptr) {
          o.pos[0] = hitPos[0];
          o.pos[1] = hitPos[1];
          o.pos[2] = hitPos[2];
          objectDieFacingPlayer(rt, o);
          return;
        }
      }
      // Player box test — the +0x540c30 AABB grown by 1 in X and Y
      // ONLY (OBSERVED: min -= C(0x497e14)=-1 on [0]/[1], max += 1.0
      // on [3]/[4]; the z span is untouched). FUN_0045c230 is the
      // slab resolver — mapped to collisionSegAabbResolve (entry
      // point -> pos is the observable; HYPOTHESIS on tie-breaks).
      float pbox[6];
      std::memcpy(pbox, rt.cs.playerBox, sizeof(pbox));
      pbox[0] -= 1.0f;
      pbox[3] += 1.0f;
      pbox[1] -= 1.0f;
      pbox[4] += 1.0f;
      float clampPt[3], altPt[3];
      if (collisionSegAabbResolve(from, o.pos, pbox, clampPt, altPt) !=
          0) {
        o.col.flags14c |= 4;
        o.pos[0] = clampPt[0];
        o.pos[1] = clampPt[1];
        o.pos[2] = clampPt[2];
        return;
      }
      o.col.flags14c &= ~4u;
      return;
    }
    case 0x4a: {
      // Refpoint attach — glue to the bound object's world refpoints.
      DynamicObject* t = o.field278;
      if (t != nullptr && t->col.named && t->col.model != nullptr) {
        const float* mine = o.worldRef[o.field276 & 7];
        const float* theirs = t->worldRef[o.field277 & 7];
        o.pos[0] = (theirs[0] + o.pos[0]) - mine[0];
        o.pos[1] = (theirs[1] + o.pos[1]) - mine[1];
        o.pos[2] = (theirs[2] + o.pos[2]) - mine[2];
        o.yawDeg = t->yawDeg;
        o.bankDeg = t->bankDeg;
        return;
      }
      o.field11e = 0;
      return;
    }
    case 0x58: {
      // Speed ramp — accelerate +0x34 toward the target (+0x38 when
      // +0x11f set else 0) by +0x3c up / +0x40 down, then emit the
      // speed impulse along yaw (+bank in 3D when not gravity-flagged).
      const float target = (o.field11f != 0) ? o.field38 : 0.0f;
      if (target <= o.field34) {
        if (target < o.field34) {
          o.field34 -= o.field40 * dt;
          if (o.field34 < target) o.field34 = target;
        }
      } else {
        o.field34 += o.field3c * dt;
        if (o.field34 > target) o.field34 = target;
      }
      if ((o.col.flags148 & 2) == 0) {
        float s, c, s2, c2;
        sincosDeg(o.yawDeg, &s, &c);
        sincosDeg(o.bankDeg, &s2, &c2);
        o.animImpulse[0] += o.field34 * c * c2;
        o.animImpulse[1] += o.field34 * s * c2;
        o.animImpulse[2] += o.field34 * s2;
      } else {
        float s, c;
        sincosDeg(o.yawDeg, &s, &c);
        o.animImpulse[0] += o.field34 * c;
        o.animImpulse[1] += o.field34 * s;
      }
      if (o.field11f == 0 && o.field34 == target) {
        o.field11e = 0;
        return;
      }
      // Stall detect — impulse queued but the frame displacement was
      // under 0.2 (OBSERVED C(0x497e1c) double).
      const float imp = std::fabs(o.animImpulse[0]) +
                        std::fabs(o.animImpulse[1]) +
                        std::fabs(o.animImpulse[2]);
      const float moved = std::fabs(o.pos[0] - o.prevPos[0]) +
                          std::fabs(o.pos[1] - o.prevPos[1]) +
                          std::fabs(o.pos[2] - o.prevPos[2]);
      if (imp > 1.0f && moved < 0.2f) {
        o.field2a0 += 1;
      } else {
        o.field2a0 = 0;
      }
      return;
    }
    case 0xe5: {
      // Grounded timer — +0x306 accumulates dt; once past
      // +0x302*0.5 AND a floor handle exists, clear the subtype and
      // zero velocity.
      setF306(o, f306(o) + dt);
      if (f306(o) < f302(o) * 0.5f) return;
      if (o.field2b0 == nullptr) return;
      o.field11e = 0;
      o.field30 = 0.0f;
      o.field2c = 0.0f;
      o.field28 = 0.0f;
      return;
    }
    default:
      return;
  }
}

// ---------------------------------------------------------------------------
// Subtype helpers — the FUN_0045b6f8 / FUN_00452b80 / FUN_004524e0 /
// FUN_00452140 steering family
// ---------------------------------------------------------------------------

namespace {

// FUN_0045b56c — turn-toward helper: desired bearing toward target,
// QUANTIZED ±180 wrap (OBSERVED: diff -= FRNDINT((diff+180)/360)*360
// / diff += FRNDINT((180-diff)/360)*360 — a round-to-360 fold, not a
// plain ±360), then advance yaw by the per-frame cap (dt*180). The
// wrapped diff is also handed back through `outDiff` (the original
// writes it through EBX).
void turnToward(DynamicObject& o, const float target[3], float dt,
                float* outDiff) {
  const float dx = target[0] - o.pos[0];
  const float dy = target[1] - o.pos[1];
  float desired;
  if (std::fabs(dx) == 0.0f && std::fabs(dy) == 0.0f) {
    desired = o.yawDeg;
  } else {
    desired = bearingDeg(dy, dx);
  }
  float diff = desired - o.yawDeg;
  if (diff <= 180.0f) {
    if (diff < -180.0f)
      diff += static_cast<float>(
                  frndint((180.0f - diff) * (1.0f / 360.0f))) *
              360.0f;
  } else {
    diff -= static_cast<float>(
                frndint((diff + 180.0f) * (1.0f / 360.0f))) *
            360.0f;
  }
  const float step = dt * 180.0f;
  if (outDiff != nullptr) *outDiff = diff;
  if (diff >= 0.0f) {
    if (diff > 0.0f) {
      float s = step;
      if (diff < s) s = diff;
      o.yawDeg += s;
      if (o.yawDeg > 360.0f) o.yawDeg -= 360.0f;   // OBSERVED: bound is
    }                                            // exactly 360.0f
  } else {
    float s = step;
    if (-diff < s) s = -diff;
    o.yawDeg -= s;
    if (o.yawDeg < 0.0f) o.yawDeg += 360.0f;
  }
}

// FUN_0045b6f8 — move-toward-point (OBSERVED): folded bearing diff,
// close-facing forward probe (<30 deg), near-quadrant turn+ramp skip
// (>=90 deg AND |dx|,|dy| <= 30), signed-cap turn at dt*180, speed
// ramp on DAT_0049b6f0=1.0 TICK units (accel (22.5-diff)/135 to
// 7/3, decel 0.05 to 1/3), then the unconditional move step*dir and
// the z-follow at step*0.25.
void subtypeSeekPoint(TraversalRuntime& rt, DynamicObject& o,
                      DynamicArena& home, const float target[3],
                      float dt) {
  const float dx = target[0] - o.pos[0];
  const float dy = target[1] - o.pos[1];
  float desired;
  if (std::fabs(dx) == 0.0f && std::fabs(dy) == 0.0f) {
    desired = o.yawDeg;
  } else {
    desired = bearingDeg(dy, dx);
  }
  float diff = std::fabs(o.yawDeg - desired);
  if (diff >= 360.0f) {
    do {
      diff -= 360.0f;
    } while (diff > 359.99997f);
  }
  if (diff > 180.0f) diff = 360.0f - diff;
  if (diff < 30.0f) {
    // Close-facing — FUN_0045bec8 forward probe (splash/landing seam).
    float s, c;
    sincosDeg(o.yawDeg, &s, &c);
    const float fwd[3] = {c * 0.866f, s * 0.866f, -0.5f};
    forwardGroundProbe(rt, o, home, fwd);
  }
  float speed = o.field34;
  if (!(diff >= 90.0f && std::fabs(dx) <= 30.0f &&
        std::fabs(dy) <= 30.0f)) {
    // Turn toward desired — OBSERVED signed-delta branches at
    // dt*180 deg/s with the asymmetric post-wrap clamps.
    const float delta = desired - o.yawDeg;
    const float cap = dt * 180.0f;
    if (delta >= -180.0f) {
      if (delta >= 0.0f) {
        if (delta < 180.0f) {
          const float f = o.yawDeg + cap;
          o.yawDeg = f;
          if (desired < f) o.yawDeg = desired;
        } else {
          o.yawDeg -= cap;
          if (o.yawDeg < 0.0f) {
            const float f = o.yawDeg + 360.0f;
            o.yawDeg = f;
            if (f < desired) o.yawDeg = desired;
          }
          // OBSERVED: when yaw stays >= 0 after the back-turn the
          // desired clamp is skipped entirely.
        }
      } else {
        const float f = o.yawDeg - cap;
        o.yawDeg = f;
        if (f < desired) o.yawDeg = desired;
      }
    } else {
      o.yawDeg += cap;
      if (o.yawDeg > 359.99997f) {
        const float f = o.yawDeg - 360.0f;
        o.yawDeg = f;
        if (desired < f) o.yawDeg = desired;
      }
    }
    // Speed ramp — OBSERVED tick-unit constants (DAT_0049b6f0=1.0).
    if (diff <= 22.5f) {
      speed += (22.5f - diff) * (1.0f / 6.0f) * (1.0f / 22.5f);
      if (speed > 2.3333333f) speed = 2.3333333f;
    } else {
      speed -= 0.05f;
      if (speed < 0.33333334f) speed = 0.33333334f;
    }
    o.field34 = speed;
  }
  // Move — unconditional; step = speed * DAT_0049b6f0 (=1.0 tick).
  float s, c;
  sincosDeg(o.yawDeg, &s, &c);
  const float step = speed;
  o.pos[0] += step * c;
  o.pos[1] += step * s;
  if (target[2] <= o.pos[2]) {
    o.pos[2] -= step * 0.25f;
    if (o.pos[2] < target[2]) o.pos[2] = target[2];
  } else {
    o.pos[2] += step * 0.25f;
    if (target[2] < o.pos[2]) o.pos[2] = target[2];
  }
}

// FUN_0045bec8 — the forward ground/wall probe: builds the probe
// point, stabs home then partner; on a hit emits the FUN_00437444
// splash seam (+ the FUN_0046771c landing path when the player is in
// the probe cone — counted seam).
void forwardGroundProbe(TraversalRuntime& rt, DynamicObject& o,
                        DynamicArena& home, const float fwd[3]) {
  // Distance along fwd to the player's plane (OBSERVED projection).
  float d = -((fwd[2] * o.pos[2] + fwd[0] * o.pos[0] +
               fwd[1] * o.pos[1]) -
              (fwd[2] * rt.cs.pos[2] + fwd[0] * rt.cs.pos[0] +
               fwd[1] * rt.cs.pos[1]));
  int nearPlayer = 0;
  float pt[3];
  if (d > 0.0f && d < 150.0f) {
    pt[0] = fwd[0] * d + o.pos[0];
    pt[1] = fwd[1] * d + o.pos[1];
    pt[2] = fwd[2] * d + o.pos[2];
    const float ddx = pt[0] - rt.cs.pos[0];
    const float ddy = pt[1] - rt.cs.pos[1];
    if (ddx * ddx + ddy * ddy < 3.0f) {
      if (rt.cs.pos[2] + 0.5f < pt[2] && pt[2] < rt.cs.pos[2] + 5.0f)
        nearPlayer = 1;
    }
  }
  if (nearPlayer == 0) {
    pt[0] = fwd[0] * 150.0f + o.pos[0];
    pt[1] = fwd[1] * 150.0f + o.pos[1];
    pt[2] = fwd[2] * 150.0f + o.pos[2];
  }
  // Stab home then the partner arena.
  float hitPos[3];
  const CollisionNode* hit =
      collisionStab(home.col, o.pos, pt, hitPos);
  if (hit == nullptr) {
    TraversalArena* other = migrationPartner(rt, home);
    if (other == nullptr ||
        collisionStab(other->dyn.col, o.pos, pt, hitPos) == nullptr) {
      if (nearPlayer == 0) return;
      rt.seams.mountDamageCalls++;             // FUN_0046771c path
      return;
    }
  }
  // FUN_00437444(1, hitPos, 1) — impact fx seam.
  rt.seams.shotImpactFxCalls++;
}

// The +0x2a0..+0x2ac mover block (OBSERVED): stall-detection
// accumulators shared by the steering family — +0x2a0 stall counter,
// +0x2a1 sample counter, +0x2a4/+0x2a8/+0x2ac per-axis moved sum.
void clearMoverBlock(DynamicObject& o) {
  o.field2a0 = 0;
  o.field2a1 = 0;
  o.field2ac = 0.0f;
  o.field2a8 = o.field2ac;
  o.field2a4 = o.field2a8;
}

// Speed ramp toward `target` — +0x3c*dt acceleration, +0x40*dt
// deceleration, clamped at the target (OBSERVED, shared by
// FUN_00452b80/FUN_004524e0/FUN_004533d4 case 0x58).
void rampSpeed34(DynamicObject& o, float target, float dt) {
  if (target <= o.field34) {
    if (target < o.field34) {
      o.field34 -= o.field40 * dt;
      if (o.field34 < target) o.field34 = target;
    }
  } else {
    o.field34 += o.field3c * dt;
    if (target < o.field34) o.field34 = target;
  }
}

// Magnet pull (OBSERVED): nudge the +0x294 impulse per axis so the
// projected next position approaches the target within ±margin*dt.
void magnetPull(float& imp, float pos, float tgt, float margin,
                float dt) {
  if (imp * dt + pos <= margin * dt + tgt) {
    if (imp * dt + pos < tgt - margin * dt) imp += margin;
  } else {
    imp -= margin;
  }
}

// FUN_00451e90 — OBSERVED thin wrapper over the FUN_00418c60 BSP
// stab; returns 1 when the pos+8 -> cand+8 segment is blocked. The
// extent block the callers compute is passed in a register the callee
// never reads (dead in the original — the probes are pure stabs).
bool waypointBlocked(DynamicArena& home, const float a[3],
                     const float b[3]) {
  float hitPos[3];
  return collisionStab(home.col, a, b, hitPos) != nullptr;
}

// FUN_00452140 — OBSERVED obstacle re-seek. When the target already
// left the anchor (a detour is active), reset to the anchor and clear
// the stuck flag. Otherwise search a lateral-offset waypoint: mix
// planes {0.25 pos/0.75 anchor, then pure pos} — the 0.75/0.25 middle
// mix is dead code (loop steps by 2, OBSERVED quirk) — offset
// 0.1..2.0 by 0.3 on both sides of the pos->anchor bearing, accept
// the first candidate whose pos->cand AND cand->anchor segments are
// both clear.
void steerReseek52140(DynamicObject& o, DynamicArena& home) {
  const bool held = o.field12c[0] == o.field120[0] &&
                    o.field12c[1] == o.field120[1] &&
                    o.field12c[2] == o.field120[2];
  if (!held) {
    o.field12c[0] = o.field120[0];
    o.field12c[1] = o.field120[1];
    o.field12c[2] = o.field120[2];
    o.field2a1 = 0;
    o.field2ac = 0.0f;
    o.field2a8 = o.field2ac;
    o.field2a4 = o.field2a8;
    o.col.flags14c &= 0xf7;
    return;
  }
  // Lifted endpoints (z + 8.0, C(0x497d5c)).
  const float pz[3] = {o.pos[0], o.pos[1], o.pos[2] + 8.0f};
  const float az[3] = {o.field120[0], o.field120[1],
                       o.field120[2] + 8.0f};
  const float bearing =
      bearingDeg(o.field120[1] - o.pos[1],
                 o.field120[0] - o.pos[0]);
  const float dist = std::sqrt((o.field120[0] - o.pos[0]) *
                                   (o.field120[0] - o.pos[0]) +
                               (o.field120[1] - o.pos[1]) *
                                   (o.field120[1] - o.pos[1]));
  float sb, cb;
  sincosDeg(bearing, &sb, &cb);
  for (int plane = 0; plane < 3; plane += 2) {
    for (float off = 0.1f; off <= 2.0f; off += 0.3f) {
      for (int side = -1; side < 2; side += 2) {
        float cand[3];
        if (plane == 0) {
          cand[0] = o.field120[0] * 0.75f + o.pos[0] * 0.25f +
                    static_cast<float>(side) * off * sb * dist;
          cand[1] = o.field120[1] * 0.75f + o.pos[1] * 0.25f +
                    static_cast<float>(side) * off * cb * dist;
          cand[2] = az[2] * 0.75f + pz[2] * 0.25f;
        } else {
          // plane 2 — the local_24==1 middle-mix branch is dead
          // (OBSERVED: loop steps 0,2,4 — never hits 1).
          cand[0] = static_cast<float>(side) * off * sb * dist +
                    o.pos[0];
          cand[1] = static_cast<float>(side) * off * cb * dist +
                    o.pos[1];
          cand[2] = pz[2];
        }
        if (!waypointBlocked(home, pz, cand) &&
            !waypointBlocked(home, az, cand)) {
          o.field12c[0] = cand[0];
          o.field12c[1] = cand[1];
          o.field12c[2] = cand[2];
          return;
        }
      }
    }
  }
}

// FUN_00452b80 — OBSERVED air steering (flags148&2 == 0). Turn toward
// +0x12c (skipped within XZ 3.0 or when +0x14a&1), speed ramp toward
// +0x38 halved when |diff| > 80, then either the free-flight impulse
// branch (z-share direction damp, stall accumulators, arrive
// retarget, magnet pull) or the +0x14a&1 direct slide.
void steerAir52b80(DynamicObject& o, DynamicArena& home, float dt) {
  const bool held = o.field12c[0] == o.field120[0] &&
                    o.field12c[1] == o.field120[1] &&
                    o.field12c[2] == o.field120[2];
  float diff = 0.0f;
  if ((o.col.flags14a & 1) == 0 &&
      std::fabs(o.field12c[1] - o.pos[1]) +
              std::fabs(o.field12c[0] - o.pos[0]) >
          3.0f) {
    turnToward(o, o.field12c, dt, &diff);
  }
  float target = o.field38;
  if (std::fabs(diff) > 80.0f) target *= 0.5f;  // C(0x497ddc/de4)
  rampSpeed34(o, target, dt);
  if ((o.col.flags14a & 1) == 0) {
    // Free-flight branch.
    float s, c;
    sincosDeg(o.yawDeg, &s, &c);
    const float xz =
        std::sqrt((o.field12c[0] - o.pos[0]) *
                      (o.field12c[0] - o.pos[0]) +
                  (o.field12c[1] - o.pos[1]) *
                      (o.field12c[1] - o.pos[1]));
    float zshare = std::fabs(o.field12c[2] - o.pos[2]);
    zshare = (xz + zshare <= 0.0f) ? 0.0f : zshare / (xz + zshare);
    s *= 1.0f - zshare;
    c *= 1.0f - zshare;
    float zdir = zshare;
    if (o.field12c[2] < o.pos[2]) zdir = -zdir;
    // Stall detection — accumulate pos deltas while airborne-
    // touching (+0x14c&1) at speed; no movement over 16 samples sets
    // the stuck bit (+0x14c|8).
    if ((o.col.flags14c & 1) == 0 ||
        std::bit_cast<std::int32_t>(o.field34) < 0x40000001) {
      clearMoverBlock(o);
    } else if (o.field2a1 < 0x10) {
      o.field2a4 += o.pos[0] - o.prevPos[0];
      o.field2a8 += o.pos[1] - o.prevPos[1];
      o.field2ac += o.pos[2] - o.prevPos[2];
      o.field2a1 += 1;
    } else if (o.field34 * 0.5f <=
               std::fabs(o.field2ac) + std::fabs(o.field2a8) +
                   std::fabs(o.field2a4)) {
      o.field2a1 = 0;
      o.field2ac = 0.0f;
      o.field2a8 = o.field2ac;
      o.field2a4 = o.field2a8;
    } else {
      o.col.flags14c |= 8;
    }
    o.animImpulse[0] += o.field34 * c;
    o.animImpulse[1] += o.field34 * s;
    o.animImpulse[2] += o.field34 * zdir;
    const float xz2 =
        std::sqrt((o.field12c[0] - o.pos[0]) *
                      (o.field12c[0] - o.pos[0]) +
                  (o.field12c[1] - o.pos[1]) *
                      (o.field12c[1] - o.pos[1]));
    if (xz2 < 4.0f && std::fabs(o.field12c[2] - o.pos[2]) < 3.0f) {
      if (!held) {
        // Detour waypoint reached — retarget the anchor.
        o.field12c[0] = o.field120[0];
        o.field12c[1] = o.field120[1];
        o.field12c[2] = o.field120[2];
      } else {
        o.field11e = 0;
        o.field34 = 0.0f;
        clearMoverBlock(o);
        o.col.flags14c &= 0xf7;
      }
    }
    const float m = o.field34 * 0.01f;       // C(0x497df4)
    magnetPull(o.animImpulse[0], o.pos[0], o.field12c[0], m, dt);
    magnetPull(o.animImpulse[1], o.pos[1], o.field12c[1], m, dt);
    magnetPull(o.animImpulse[2], o.pos[2], o.field12c[2], m, dt);
    return;
  }
  // +0x14a&1 — direct slide toward +0x12c..134.
  const float d[3] = {o.field12c[0] - o.pos[0],
                      o.field12c[1] - o.pos[1],
                      o.field12c[2] - o.pos[2]};
  const float sum =
      std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]);
  if (sum < 1.0f) {
    if (held) {
      o.pos[0] = o.field12c[0];
      o.pos[1] = o.field12c[1];
      o.pos[2] = o.field12c[2];
      o.field11e = 0;
      o.field34 = 0.0f;
      clearMoverBlock(o);
      o.col.flags14c &= 0xf7;
      return;
    }
    o.field12c[0] = o.field120[0];
    o.field12c[1] = o.field120[1];
    o.field12c[2] = o.field120[2];
  }
  float step[3];
  for (int i = 0; i < 3; ++i) {
    step[i] = (o.field34 * dt * d[i]) / sum;
    // Per-axis clamp into [-|d|,|d|] preserving sign (OBSERVED
    // branch pair).
    if (d[i] > 0.0f && d[i] < step[i]) step[i] = d[i];
    if (d[i] < 0.0f && step[i] < d[i]) step[i] = d[i];
  }
  o.animImpulse[0] += step[0] / dt;
  o.animImpulse[1] += step[1] / dt;
  o.animImpulse[2] += step[2] / dt;
  (void)home;
}

// FUN_004524e0 — OBSERVED grounded steering (flags148&2 != 0). XZ
// arrive radius 5.0 (retargets the anchor or finishes), always turns
// toward +0x12c, same speed ramp, subtype-specific stall logic, then
// XY-only impulse + magnet pull.
void steerGround524e0(DynamicObject& o, float dt) {
  const bool held = o.field12c[0] == o.field120[0] &&
                    o.field12c[1] == o.field120[1] &&
                    o.field12c[2] == o.field120[2];
  if (std::fabs(o.field12c[1] - o.pos[1]) +
          std::fabs(o.field12c[0] - o.pos[0]) <
      5.0f) {                                          // C(0x497da4)
    if (held) {
      o.field11e = 0;
      o.field34 = 0.0f;
      clearMoverBlock(o);
      o.col.flags14c &= 0xf7;
      return;
    }
    o.field12c[0] = o.field120[0];
    o.field12c[1] = o.field120[1];
    o.field12c[2] = o.field120[2];
  }
  float diff = 0.0f;
  turnToward(o, o.field12c, dt, &diff);
  float target = o.field38;
  if (std::fabs(diff) > 80.0f) target *= 0.5f;        // C(0x497dac/db4)
  rampSpeed34(o, target, dt);
  float s, c;
  sincosDeg(o.yawDeg, &s, &c);
  if (o.field11e == 0x2b) {
    if ((o.col.flags14c & 1) == 0) {
      clearMoverBlock(o);
    } else {
      o.col.flags14c |= 8;
      o.field2a1 = 0x1e;
    }
  } else if (o.field11e == 0xc5) {
    if ((o.col.flags14c & 1) == 0 ||
        std::bit_cast<std::int32_t>(o.field34) < 0x40000001) {
      clearMoverBlock(o);
    } else if (o.field2a1 < 9) {
      o.field2a4 += o.pos[0] - o.prevPos[0];
      o.field2a8 += o.pos[1] - o.prevPos[1];
      o.field2ac += o.pos[2] - o.prevPos[2];
      o.field2a1 += 1;
    } else if (o.field34 * 0.5f <=
               std::fabs(o.field2ac) + std::fabs(o.field2a8) +
                   std::fabs(o.field2a4)) {
      o.field2a1 = 0;
      o.field2ac = 0.0f;
      o.field2a8 = o.field2ac;
      o.field2a4 = o.field2a8;
    } else {
      o.col.flags14c |= 8;
    }
  } else if ((o.col.flags14c & 1) == 0 ||
             std::bit_cast<std::int32_t>(o.field34) < 0x40000001) {
    clearMoverBlock(o);
  } else if (o.field2a1 < 0x10) {
    o.field2a4 += o.pos[0] - o.prevPos[0];
    o.field2a8 += o.pos[1] - o.prevPos[1];
    o.field2ac += o.pos[2] - o.prevPos[2];
    o.field2a1 += 1;
  } else if (o.field34 * 0.5f <=
             std::fabs(o.field2ac) + std::fabs(o.field2a8) +
                 std::fabs(o.field2a4)) {
    o.field2a1 = 0;
    o.field2ac = 0.0f;
    o.field2a8 = o.field2ac;
    o.field2a4 = o.field2a8;
  } else {
    o.col.flags14c |= 8;
  }
  o.animImpulse[0] += o.field34 * c;
  o.animImpulse[1] += o.field34 * s;
  const float m = o.field34 * 0.01f;                  // C(0x497dcc)
  magnetPull(o.animImpulse[0], o.pos[0], o.field12c[0], m, dt);
  magnetPull(o.animImpulse[1], o.pos[1], o.field12c[1], m, dt);
}

// FUN_004533d4 shared tail — the 0x2b/0x4e/0xc5 steering dispatch and
// the post-steer stall handling (OBSERVED): while the subtype lives
// and the stuck bit (+0x14c&8) is set and yaw moved < 3.0 this frame
// — count stalls; 0xc5 quits immediately, under 3 stalls re-seeks
// (FUN_00452140, counted as a second stall), 0x4e steps toward the
// target, anything else quits.
void subtypeSteeringTail(DynamicObject& o,
                         DynamicArena& home, TraversalArena* other,
                         float dt) {
  const float yaw0 = o.yawDeg;
  if ((o.col.flags148 & 2) == 0) {
    steerAir52b80(o, home, dt);
  } else {
    steerGround524e0(o, dt);
  }
  (void)other;
  if (o.field11e != 0 && (o.col.flags14c & 8) != 0 &&
      std::fabs(o.yawDeg - yaw0) < 3.0f) {          // C(0x497e0c)
    o.field2a0 += 1;
    if (o.field11e == 0xc5) {
      o.field11e = 0;
      o.field34 = 0.0f;
    } else if (o.field2a0 < 3) {
      steerReseek52140(o, home);
      o.field2a0 += 1;
    } else if (o.field11e == 0x4e) {
      const float step = o.field34 * dt;
      for (int i = 0; i < 3; ++i) {
        float d = o.field12c[i] - o.pos[i];
        if (d <= step) {
          if (d < -step) d = -step;
        } else {
          d = step;
        }
        o.pos[i] += d;
      }
    } else {
      o.field11e = 0;
      o.field34 = 0.0f;
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_004585c4 — the +0x14a&0x20 mover (enemy-dispatch else branch)
// ---------------------------------------------------------------------------
//
// OBSERVED (MDK95.EXE 0x4585c4 — full instruction-level decode):
//
//   Entry gate (dword +0x148 & 0x20002) == 2 — gravity-active
//   (+0x148 bit1) AND not yet hop-landed (+0x14a bit1):
//     * airborne (+0x14c&2 == 0): when +0x30 falls below C(0x498070)
//       = -15.0 the mover clamps +0x30 = -15.0 (0xc1700000) and —
//       once, while +0x312 == 0 — spawns the "SW_CHUTE" child:
//         FUN_00454794("SW_CHUTE") — name scan of the level model
//         table (a miss is fatal in the original; a port without the
//         model simply cannot spawn the child);
//         FUN_00454af8(+0x60 arena, +0x10/+0x14/+0x18 pos, spawnId=1,
//         modelIdx, scriptPC=0, connInit=0) -> +0x312;
//         child +0x148 dword = 0x820, +0x276 = 0, +0x277 = 0,
//         +0x278 = parent, +0x11e = 0x4a (the refpoint-attach
//         subtype — the chute rides the parent's refpoint 0), then
//         FUN_0045612c(child) rebuilds the transform.
//     * floor contact (+0x14c&2): +0x5c = 1.5 (0x3fc00000),
//       +0x14a |= 2 (the hop-landed latch — the 0x20000 gate bit),
//       +0x18 += C(0x498078) = 1.5; return. The hop never re-arms —
//       once landed, the name dispatch below runs instead.
//
//   Non-hop path (dword gate != 2):
//     * child fade — while +0x312 != 0 and child +0x58 >
//       C(0x498028) = 0.2: +0x58 -= dt; at <= 0.2 the child is torn
//       down (FUN_0045828c) and +0x312 = 0. The chute shrinks out
//       once the parent leaves the hop branch.
//     * name dispatch on the model record name (+0x0c):
//         SW_H150 — the fleeing health pickup (moverSwH150);
//         SW_SEAL / SW_SBONE — explicit no-ops (return);
//         default — +0x4c += dt * C(0x498030) = 180.0 (the pickup
//         spin every BUILD_A HotPick takes: SW_HOME/SW_GREN/
//         SW_GATT/SW_THUMP/SW_INTER/BONEHEAD/SW_DUMMY on LEVEL8
//         GUNT_9, plus cmd-8/9-armed enemies elsewhere).

namespace {

// FUN_0045dc18 — approach `target` from `cur` by `step`, clamping at
// the target and wrapping ±360 when the short way crosses 0/360.
// OBSERVED constants: -180 (0x49839c), 180 (0x4983a0), 360
// (0x4983a4), -360 (0x4983a8).
float moverSteerTo(float target, float cur, float step) {
  const float diff = target - cur;
  const float up = cur + step;
  if (diff < -180.0f) {
    float out = up;
    if (out >= 360.0f) {
      out -= 360.0f;
      if (out > target) out = target;
    }
    return out;
  }
  const float down = cur - step;
  if (diff < 0.0f) return down < target ? target : down;
  if (diff < 180.0f) return up <= target ? up : target;
  float out = down;
  if (out < 0.0f) {
    out += 360.0f;
    if (out < target) out = target;
  }
  return out;
}

// The hop branch's one-shot child — FUN_00454794 + FUN_00454af8 +
// the post-spawn field block (0x45863c..0x4586af).
void moverChuteSpawn(TraversalRuntime& rt, DynamicObject& o,
                     DynamicArena& home) {
  const int idx = rt.level.enemies.indexOf("SW_CHUTE");
  if (idx <= 0) return;                     // original gates idx > 0
  const RuntimeModel* src = traversalModelFor(idx, &rt.level);
  if (src == nullptr) return;
  DynamicArena& target = o.arena ? *o.arena : home;   // +0x60
  DynamicObject& c = target.allocFront();             // freelist alloc
  c.model = deepCopyModel(*src);                      // FUN_00403720
  c.enemyIndex = static_cast<std::uint16_t>(idx);     // +0x04
  c.spawnId = 1;                                      // +0x146 (arg 1)
  c.field1c[0] = o.pos[0];                            // +0x1c anchor
  c.field1c[1] = o.pos[1];
  c.field1c[2] = o.pos[2];
  c.setPosition(o.pos[0], o.pos[1], o.pos[2]);        // +0x10..0x18
  c.prevPos[0] = o.pos[0];                            // +0x180..
  c.prevPos[1] = o.pos[1];
  c.prevPos[2] = o.pos[2];
  initObjectCollision(c);                             // FUN_004566f0
  c.behaviorByte = 7;                                 // +0x11c (FUN_00454af8 tail)
  // +0x108 = 0 (scriptPC arg 0).
  o.moverChild = &c;                                  // +0x312
  // Post-spawn writes (OBSERVED 0x45866c..0x4586a2).
  c.col.flags148 = 0x820;                             // dword +0x148
  c.col.flags149 = 0x08;                              // byte1 mirror
  c.col.flags14a = 0;
  c.field276 = 0;                                     // refpoint idx
  c.field277 = 0;
  c.field278 = &o;                                    // +0x278 = parent
  c.field11e = 0x4a;                                  // attach subtype
  rebuildObjectTransform(c);                          // FUN_0045612c
}

// SW_H150 — the health pickup that runs away from the player.
// OBSERVED (0x45871b..0x458930): idle (H150_I, one-shot) until the
// player closes inside sqrt(C(0x498038)) = 20 u (3D dist²); then the
// looping H150_R reaction + the 0x54c644 "RUNNER" SFX call
// (FUN_00402388 — audio seam). While reacting it steers to the
// compass bearing away from the player (+180) at dt*270 deg/s
// (C(0x498060)) and accumulates a 40·(cos,sin)(yaw) impulse into
// +0x294/+0x298 (C(0x498040)); once the player's XY distance²
// exceeds C(0x498068) = 1000 it drops back to idle. The 0x45885b
// lottery re-arms a completed idle anim with probability
// frameStep*72 / 32768 per frame (DAT_0049b6e8 = 1 in the port,
// matching the command-timer normalization).
void moverSwH150(TraversalRuntime& rt, DynamicObject& o, float dt,
                 int frameStep) {
  o.zBias = 0.0f;                                     // +0x5c = 0
  if (o.moverChild != nullptr) return;                // chute linked
  // +0x114 == 0 or +0x118 == 0xff00 — the rebind lottery: with
  // probability frameStep*72/32768 the idle record is (re)bound;
  // otherwise control falls to the state dispatch at 0x458751.
  if (o.animDone() &&
      static_cast<int>(enemyRandNext(rt.rngState)) < frameStep * 72) {
    o.animRate = 30.0f;                               // +0xe0
    o.animLatch = -1;                                 // +0x118 = 0xffff
    o.animAcc = 0.0f;                                 // +0xdc = 0
    o.animFrame = -1;                                 // +0xe4 = 0xffff
    o.animRec = rt.animH150I;                         // +0x114 = H150_I
    o.col.flags148 &= ~0x8u;                          // one-shot
    return;
  }
  // 0x458751 — state dispatch on the bound record.
  if (o.animRec == rt.animH150R) {                    // reacting
    float sn, cs;
    sincosDeg(o.yawDeg, &sn, &cs);                    // FUN_00437f98
    o.animImpulse[0] += cs * 40.0f;                   // +0x294
    o.animImpulse[1] += sn * 40.0f;                   // +0x298
    const float dx = rt.cs.pos[0] - o.pos[0];
    const float dy = rt.cs.pos[1] - o.pos[1];
    // FUN_00437f30(dx,dy) — the original's arg order yields
    // atan2(dx,dy); +180 (0x498048) = away from the player.
    float ang = bearingDeg(dx, dy) + 180.0f;
    if (ang > 360.0f) ang -= 360.0f;                  // C(0x498050/58)
    o.yawDeg = moverSteerTo(ang, o.yawDeg, dt * 270.0f);
    o.col.flags148 |= 2;                              // gravity while
                                                    // fleeing
    if (dx * dx + dy * dy <= 1000.0f) return;         // C(0x498068)
    // Player out of range — back to the idle record.
    o.animAcc = 0.0f;
    o.animFrame = -1;
    o.animRec = rt.animH150I;
    o.col.flags148 &= ~0x8u;
    return;
  }
  // Idle (or any other record) — 3D proximity gate (FUN_00430190).
  const float dx = rt.cs.pos[0] - o.pos[0];
  const float dy = rt.cs.pos[1] - o.pos[1];
  const float dz = rt.cs.pos[2] - o.pos[2];
  if (dx * dx + dy * dy + dz * dz >= 400.0f) return;  // C(0x498038)
  // Player close — the reaction starts looping + RUNNER call.
  o.animRate = 30.0f;
  o.animLatch = -1;
  o.animAcc = 0.0f;
  o.col.flags148 |= 8;                                // looping
  o.animRec = rt.animH150R;
  o.animFrame = -1;
  ++rt.seams.moverSfxCalls;                           // FUN_00402388
                                                    // (0x54c644, 0)
}

} // namespace

void objectMover(TraversalRuntime& rt, DynamicObject& o,
                 DynamicArena& home, float dt, int frameStep) {
  if ((flagsDword(o) & 0x20002) == 2) {               // hop branch
    if ((o.col.flags14c & 2) == 0) {                  // airborne
      if (o.field30 < -15.0f) {                       // C(0x498070)
        o.field30 = -15.0f;                           // 0xc1700000
        if (o.moverChild == nullptr)
          moverChuteSpawn(rt, o, home);
      }
      return;
    }
    o.zBias = 1.5f;                                   // +0x5c = 0x3fc00000
    o.col.flags14a |= 2;                              // hop-landed
    o.pos[2] += 1.5f;                                 // C(0x498078)
    return;
  }
  // Child fade (0x4586bd..0x4586fc).
  DynamicObject* c = o.moverChild;
  if (c != nullptr && c->col.scale > 0.2f) {          // C(0x498028)
    c->col.scale -= dt;                               // DAT_0049b6f4
    if (c->col.scale <= 0.2f) {
      objectTeardownNow(rt, *c);                      // FUN_0045828c
      o.moverChild = nullptr;                         // +0x312 = 0
    }
  }
  const std::string& nm = o.model.modelName();
  if (nm == "SW_H150") { moverSwH150(rt, o, dt, frameStep); return; }
  if (nm == "SW_SEAL" || nm == "SW_SBONE") return;    // no-op names
  o.yawDeg += dt * 180.0f;                            // C(0x498030)
}

// ---------------------------------------------------------------------------
// FUN_004599e8 is implemented as cmdBodyCarry above (+0x14a&4).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FUN_00451ee8 — bind-time waypoint re-seek. +0x12c..0x134 =
// +0x120..+0x128; when the z+8-lifted pos->target segment is blocked,
// probe mid + t*k*dist*(cos,sin)(brg) for t in (0,1.2] step 0.1 and
// k in {-1,+1}; the first candidate whose pos->cand AND cand->target
// half-segments both stab clear becomes the waypoint. OBSERVED from
// raw disasm. The caller-computed extent block is dead in the
// original (FUN_00451e90 never reads it).
// ---------------------------------------------------------------------------

void objectWaypointReseek(DynamicObject& o, DynamicArena& home) {
  for (int i = 0; i < 3; ++i) o.field12c[i] = o.field120[i];
  const float s0[3] = {o.pos[0], o.pos[1], o.pos[2] + 8.0f};
  const float s1[3] = {o.field120[0], o.field120[1],
                       o.field120[2] + 8.0f};
  if (!waypointBlocked(home, s0, s1)) return;
  const float brg = bearingDeg(s1[1] - s0[1], s1[0] - s0[0]);
  const float ddx = s1[0] - s0[0], ddy = s1[1] - s0[1],
              ddz = s1[2] - s0[2];
  const float dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
  float sn, cs;
  sincosDeg(brg, &sn, &cs);
  const float mid[3] = {(s0[0] + s1[0]) * 0.5f,
                        (s0[1] + s1[1]) * 0.5f,
                        (s0[2] + s1[2]) * 0.5f};
  for (float t = 0.1f; t <= 1.2f; t += 0.1f) {
    for (float k = -1.0f; k <= 1.0f; k += 2.0f) {
      const float cand[3] = {mid[0] + t * k * cs * dist,
                             mid[1] + t * k * sn * dist, mid[2]};
      if (!waypointBlocked(home, s0, cand) &&
          !waypointBlocked(home, cand, s1)) {
        for (int i = 0; i < 3; ++i) o.field12c[i] = cand[i];
        return;
      }
    }
  }
}

// Shared opcode tail (0x2a/0xa6/0x4e handlers, OBSERVED): clear the
// steering stall byte, the mover-block accumulators and the stuck
// bit (+0x14c&~8).
void seekOpcodeTail(DynamicObject& o) {
  o.field2a0 = 0;
  o.field2a1 = 0;
  o.field2ac = 0.0f;
  o.field2a8 = o.field2ac;
  o.field2a4 = o.field2a8;
  o.col.flags14c &= 0xf7;
}

// tr_alcmd 0x2a (handler 0x439aef) — camera-relative seek target.
// {f32 fwd, f32 lat}: +0x120..+0x128 = pos + d*0.01*R(-b)(fwd,lat)
// toward the camera interest point (0x54c6c4..cc); subtype 0x2b, path
// unbound, re-seek. Runs only while the object lives in the CURRENT
// arena (obj+0x60 == 0x540c48). OBSERVED from raw disasm.
void objectOpSeekCamera(TraversalRuntime& rt, DynamicObject& o,
                        DynamicArena& cur, float fwd, float lat) {
  if (o.arena != &cur) return;
  const float* cam = rt.camera.pose.pos;             // 0x54c6c4..cc
  const float dx = cam[0] - o.pos[0];
  const float dy = cam[1] - o.pos[1];
  const float dz = cam[2] - o.pos[2];
  const float d = std::sqrt(dx * dx + dy * dy + dz * dz);  // FUN_00430160
  const float b = bearingDeg(dy, dx);                      // FUN_0045acf0
  float sn, cs;
  sincosDeg(b, &sn, &cs);                                // FUN_00437f98
  const float s = d * 0.00999999978f;                      // C(0x4979b4)
  // OBSERVED: +0x120 = x + fwd*s*cos + lat*s*sin;
  //           +0x124 = y + fwd*s*sin - lat*s*cos.
  o.field120[0] = o.pos[0] + fwd * s * cs + lat * s * sn;
  o.field120[1] = o.pos[1] + fwd * s * sn - lat * s * cs;
  o.field120[2] = cam[2] + o.zBias;
  o.field11e = 0x2b;
  o.fieldEC = nullptr;
  objectWaypointReseek(o, cur);                          // FUN_00451ee8
  seekOpcodeTail(o);
}

// tr_alcmd 0xa6 (handler 0x442de1) — flee-the-cmd2-object seek
// target. {f32 dist}: +0x120..+0x124 = cmdObj60.pos + awayDir*dist
// plus a rand()-jittered perpendicular step (dist capped at 6.0,
// jitter = (rand-0x4000)*6.103515625e-5*dist); +0x128 = cmdObj60.z +
// 2.5 + zBias; subtype 0x4e, path unbound, re-seek. No-op while no
// cmd-2 object is registered (0x540e60 == 0). OBSERVED.
void objectOpSeekAway(TraversalRuntime& rt, DynamicObject& o,
                      float dist) {
  DynamicObject* anchor = rt.cmdObj60;
  if (anchor == nullptr) return;
  float dx = o.pos[0] - anchor->pos[0];
  float dy = o.pos[1] - anchor->pos[1];
  const float len2 = dx * dx + dy * dy;
  if (len2 > 0.0f) {
    const float inv = 1.0f / std::sqrt(len2);
    dx *= inv;
    dy *= inv;
  }
  o.field120[0] = anchor->pos[0] + dx * dist;
  o.field120[1] = anchor->pos[1] + dy * dist;
  if (dist > 6.0f) dist = 6.0f;                          // C(0x497b2c)
  const float jit =
      static_cast<float>(static_cast<int>(enemyRandNext(rt.rngState)) -
                         0x4000) *
      6.103515625e-5f * dist;                            // C(0x497b34)
  o.field120[0] -= dy * jit;
  o.field120[1] += dx * jit;
  o.field120[2] = anchor->pos[2] + 2.5f + o.zBias;       // C(0x497b3c)
  o.field11e = 0x4e;
  o.fieldEC = nullptr;
  if (o.arena != nullptr)
    objectWaypointReseek(o, *o.arena);                   // FUN_00451ee8
  seekOpcodeTail(o);
}

} // namespace mdk
