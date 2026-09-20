// Phase 5L — the sniper-aim mode (see player_sniper.h for the full
// evidence map). FUN_00464624 core + FUN_00464b50 zoom + FUN_00461878
// reset + the FUN_00436100 scope-phase head + the shared
// FUN_00467a00 drain, in original order.

#include "core/player_sniper.h"

#include <cmath>
#include <cstring>

#include "core/collision_query.h"
#include "core/motion_channels.h"
#include "core/player_camera.h"
#include "core/player_look.h"
#include "core/player_surface.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

// OBSERVED constants (raw dumps + instruction-level operands,
// MDK95.EXE BUILD_A):
constexpr double kDegToRad =
    0.017453292519943276;   // 0x497924 — f64 4 ULP below pi/180
constexpr float kLateralCapK = 0.25f;         // 0x498848 — cap mul
constexpr double kLateralDecelIn = 0.08888888889;   // 0x498840
constexpr double kLateralDecelOut = 0.1777777778;   // 0x498838
constexpr float kLateralBound = 0.6666667f;   // 0x3f2aaaab literal
constexpr float kAbortFall = -30.0f;          // 0x498850
constexpr double kMouseK = 0.12;              // 0x498858
constexpr double kAimK = 0.4166666667;        // 0x498860
constexpr float kPitchClamp = 50.0f;          // +-0x42480000
constexpr float kPitchClampNeg = -50.0f;      // 0xc2480000
constexpr float kYawWrap = 360.0f;            // 0x49886c/-, 0x498870/+
constexpr float kDecelIn = 1.0666667f;        // 0x3f888889 (16/15)
constexpr float kDecelOut = 0.8f;             // 0x3fcccccd
constexpr float kDecelBound = 4.0f;           // 0x40800000
// FUN_00464b50 zoom:
constexpr float kZoomFloorCap = 0.25f;        // 0x498880 / 0x3e800000
constexpr double kZoomExtentFloor = 10.0;     // 0x498874
constexpr double kZoomFocusK = 1.5;           // 0x498878
constexpr float kZoomFloorOpen = 1000.0f;     // 0x4479c000
constexpr float kZoomDecayRate = 0.0147f;     // 0x3c75c28f literal
constexpr float kZoomCeil = 1.0f;             // universal b58 ceiling

std::uint32_t asToken(const void* p) {
  return static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(p));
}

// yaw basis — FUN_00437f98: deg * kDegToRad -> {sin,cos}, f32 stores.
void yawBasis(float yawDeg, float* sy, float* cy) {
  const double rad = static_cast<double>(yawDeg) * kDegToRad;
  *sy = static_cast<float>(std::sin(rad));
  *cy = static_cast<float>(std::cos(rad));
}

// Wrap the locomotion yaw into [0,360) via the +-360 float constants
// (0x49886c/-360, 0x498870/+360 — the same wrap the normal path uses).
void wrapYaw(float& yaw) {
  if (yaw >= kYawWrap) yaw -= kYawWrap;
  if (yaw < 0.0f) yaw += kYawWrap;
}

// The b54 view-scalar clamp: b54 >= -50 gates, then > +50 caps (the
// original's two-sided branch at 0x464af2/0x464b11).
void clampPitch(float& b54) {
  if (b54 < kPitchClampNeg) {
    b54 = kPitchClampNeg;
  } else if (b54 > kPitchClamp) {
    b54 = kPitchClamp;
  }
}

} // namespace

// FUN_0041342c — the dying-surface match: walk the current arena's
// +0x45e record list for a surface record (kind -1) with rate > 0
// whose surfType matches the contact poly's +0x20>>24 surface id
// (the byte at +0x23 — CollisionPoly::surface). Returns true when a
// match exists (the sniper unscope trigger / entry gate).
bool sniperDyingSurface(const TraversalRuntime& rt,
                        const CollisionPoly* contact) {
  if (contact == nullptr || rt.cur == nullptr) return false;
  const SurfaceRecord* rec = rt.cur->surface.records;
  while (rec != nullptr) {
    if (rec->kind == -1 && rec->rate > 0.0f &&
        rec->surfType == contact->surface) {
      return true;
    }
    rec = rec->next;
  }
  return false;
}

SniperSurfaceMul sniperSurfaceMul(const CollisionPoly* contactPoly) {
  SniperSurfaceMul m;
  if (contactPoly == nullptr) {
    m.rate = 0.75f;
    m.decay = 0.75f;          // no contact -> 0.75/0.75
  } else if ((contactPoly->flags & 4) == 0) {
    m.rate = 1.0f;
    m.decay = 1.0f;           // contact, not low-friction -> 1.0/1.0
  } else {
    m.rate = 0.5f;
    m.decay = 0.1f;           // low-friction -> 0.5/0.1
  }
  return m;
}

void sniperScopePhaseAdvance(TraversalRuntime& rt) {
  // FUN_00436100 head — one scope phase per frame, committed then
  // requested (the original tests ca0==2 first, then ca0==1).
  if (rt.flagC9c != 0 && rt.transitionPhase == 2) {
    // ca0==2 -> overlay commit (FUN_00416700) + e74=0 + ca0=3.
    ++rt.seams.scopeOverlayCalls;
    rt.scopeAnimLatch = 0;
    rt.transitionPhase = 3;
  }
  if (rt.flagC9c != 0 && rt.transitionPhase == 1) {
    // ca0==1 -> overlay request when unlatched, then the request
    // writes: 5414bc=1, b54=0, d58=0, ccc..cd4=0, e94=e98=1.0, ca0=2.
    if (rt.scopeAnimLatch == 0) {
      ++rt.seams.scopeOverlayCalls;   // FUN_0041664c
      rt.scopeAnimLatch = 1;
    }
    ++rt.seams.hudEventCalls;         // FUN_0046c92c + FUN_00402388
    rt.flag5414bc = true;
    rt.viewScalar = 0.0f;             // b54 = 0 (NOT the arena scalar)
    rt.look.lookPitchOffset = 0.0f;   // d58 = 0
    rt.scopeChanC = 0.0f;             // ccc/cd0/cd4 — consumers UNKNOWN
    rt.scopeChanD0 = 0.0f;
    rt.scopeChanD4 = 0.0f;
    rt.transitionPhase = 2;
    rt.scopeBlend94 = 1.0f;           // e94 = e98 = 1.0
    rt.scopeBlend98 = 1.0f;
  }
}

void sniperCoreUpdate(TraversalRuntime& rt, const RawGameplayInput& raw,
                      const GameplayInputBindings& bindings,
                      const GameplayInputFrame& ctrl,
                      const PlayerVerticalEnvironment& vertEnv, float f0,
                      PlayerVerticalFrame* outVf, float* outAppliedZ,
                      bool* outMoved) {
  // 0x46462f — the object-prepass gate is cleared every scoped frame.
  rt.fieldC74 = 0;

  // 0x464637 — FUN_00467180 DIRECTLY: gravity + vertical collision,
  // NO jump machine (the jump-sustain fields keep stale values).
  rt.vert.posX = rt.cs.pos[0];
  rt.vert.posY = rt.cs.pos[1];
  rt.vert.posZ = rt.cs.pos[2];
  PlayerVerticalFrame vf =
      integratePlayerGravity(vertEnv, rt.motion, rt.vert);
  if (vf.collisionIssued) {
    const CollisionPoly* vc = playerVerticalApplyCollision(
        vertEnv, rt.cs, rt.motion, rt.vert, vf, outAppliedZ);
    if (vc != nullptr) rt.lastContactPoly = vc;
  }
  if (outVf != nullptr) *outVf = vf;
  rt.vert.posX = rt.cs.pos[0];
  rt.vert.posY = rt.cs.pos[1];
  rt.vert.posZ = rt.cs.pos[2];

  // 0x46463c — the last contact (e4c) picks the lateral surface mul.
  const SniperSurfaceMul mul = sniperSurfaceMul(rt.lastContactPoly);

  // ECX — the strafe-semantic flag: set when the lateral channel is
  // driven this frame; suppresses the yaw channel + raw mouse below.
  int ecx = 0;
  // DL — bit0 = pitch channel (d48) fired, bit1 = yaw (d4c) fired.
  int dl = 0;

  // 0x464666 — the lateral channel d50 (semantic strafe through
  // accelChannel, else decelChannel), feeding a swept move.
  if (ctrl.strafeNorm != 0.0f) {
    accelChannel(rt.motion.turnVel, ctrl.strafeNorm * mul.rate,
                 ctrl.strafeFast * kLateralCapK, f0);
    ecx = 1;
  } else {
    decelChannel(rt.motion.turnVel,
                 static_cast<float>(mul.decay * kLateralDecelIn),
                 static_cast<float>(mul.decay * kLateralDecelOut),
                 kLateralBound, f0);
  }

  // 0x4646a6 — nonzero lateral channel -> swept strafe move
  // FUN_004630d4(d50*f0*sin, -d50*f0*cos, 0, 0.75) — the normal
  // strafe basis, radius 0.75.
  if (rt.motion.turnVel != 0.0f) {
    float sy, cy;
    yawBasis(rt.motion.yawDeg, &sy, &cy);
    const float dx = rt.motion.turnVel * f0 * sy;
    const float dy = -rt.motion.turnVel * f0 * cy;
    const float preX = rt.cs.pos[0], preY = rt.cs.pos[1];
    const CollisionPoly* hc =
        collisionApply(rt.cs, dx, dy, 0.0f, 0.75f, nullptr, nullptr);
    rt.vert.contactObj = asToken(hc);
    if (hc != nullptr) rt.lastContactPoly = hc;
    if (outMoved != nullptr)
      *outMoved = rt.cs.pos[0] != preX || rt.cs.pos[1] != preY;
  }

  // 0x464705 — the abort check: vertEnable && !grounded &&
  // (vertVel < -30 || vertVel > 0) -> FUN_00461878. The -30..0 band
  // does NOT abort.
  if (rt.vertEnable && (rt.vert.contactFlags & 1) == 0 &&
      (rt.vert.vertVel < kAbortFall || rt.vert.vertVel > 0.0f)) {
    ++rt.seams.hudEventCalls;   // FUN_0047d2e9 (the fall log seam)
    sniperReset(rt);
  }

  // 0x46474f — semantic aim (N-1 merged frame) through directChannel
  // (NOT frame-scaled). The strafe flag (ECX) suppresses the yaw
  // channel.
  if (ctrl.yawNorm != 0.0f && ecx == 0) {
    directChannel(rt.motion.strafeVel, ctrl.yawNorm, ctrl.yawFast);
    dl |= 0x2;
  }
  if (ctrl.moveNorm != 0.0f) {
    directChannel(rt.motion.moveVel, ctrl.moveNorm, ctrl.moveFast);
    dl |= 0x1;
  }

  // 0x464795 — raw mouse aim (CURRENT frame, zero latency): only when
  // no semantic channel fired, mouseOn != 0 and the strafe flag is
  // clear.
  if (dl == 0 && bindings.mouseOn && ecx == 0) {
    float sdx = static_cast<float>(raw.mouseDx) *
                static_cast<float>(kMouseK);
    float sdy = static_cast<float>(raw.mouseDy) *
                static_cast<float>(kMouseK);
    if (bindings.mouseYReversedBits != 0) sdy = -sdy;
    rt.viewScalar = static_cast<float>(
        (double)rt.viewScalar +
        (double)sdy * (double)f0 * (double)rt.camera.zoom * kAimK);
    clampPitch(rt.viewScalar);
    rt.motion.yawDeg = static_cast<float>(
        (double)rt.motion.yawDeg -
        (double)sdx * (double)f0 * (double)rt.camera.zoom * kAimK);
    wrapYaw(rt.motion.yawDeg);
  }

  // 0x464874 — non-fired semantic channels decay through decelChannel
  // (rIn 16/15 inside +-4.0, rOut 0.8 outside).
  if ((dl & 0x1) == 0) {
    decelChannel(rt.motion.moveVel, kDecelIn, kDecelOut, kDecelBound,
                 f0);
  }
  if ((dl & 0x2) == 0) {
    decelChannel(rt.motion.strafeVel, kDecelIn, kDecelOut, kDecelBound,
                 f0);
  }

  // 0x4648b0 — semantic channels apply: b54 += d48*f0*b58*0.41667
  // (clamped +-50), c2c -= d4c*f0*b58*0.41667 (wrapped).
  if (rt.motion.moveVel != 0.0f) {
    rt.viewScalar = static_cast<float>(
        (double)rt.viewScalar + (double)rt.motion.moveVel *
                                    (double)f0 *
                                    (double)rt.camera.zoom * kAimK);
    clampPitch(rt.viewScalar);
  }
  if (rt.motion.strafeVel != 0.0f) {
    rt.motion.yawDeg = static_cast<float>(
        (double)rt.motion.yawDeg - (double)rt.motion.strafeVel *
                                       (double)f0 *
                                       (double)rt.camera.zoom * kAimK);
    wrapYaw(rt.motion.yawDeg);
  }

  // 0x46496c — the post-abort guard: ca0 == 0 -> RET (abort reset ca0
  // mid-function but the aim above already ran).
  if (rt.transitionPhase == 0) return;

  // 0x464979 — manual unscope: sniperPulse (ce76c) != 0 OR the
  // contact poly matches a dying-surface record (FUN_0041342c).
  bool unscope = (ctrl.sniperPulse != 0);
  if (!unscope && sniperDyingSurface(rt, rt.lastContactPoly)) {
    unscope = true;
  }
  if (unscope) {
    // 0x464986 — the unscope block: c9c=0, cb08=0x384/cb00=9, the
    // overlay latch, b54 restored to the arena scalar, b58=2.4,
    // d58/d54/d50/d4c/d48=0, db4=8.0, db8=4.5, d34=-101. ca0 is LEFT
    // (the abort reset would clear it — the manual path does not).
    rt.flagC9c = 0;
    rt.eventMag = 0x384;
    rt.eventType = 9;
    if (rt.scopeAnimLatch == 0) {
      ++rt.seams.scopeOverlayCalls;   // FUN_0041664c
      rt.scopeAnimLatch = 1;
    }
    ++rt.seams.sniperExitCalls;       // FUN_0046ca84
    rt.viewScalar = rt.cur != nullptr ? rt.cur->scalar : 0.0f;
    rt.look.lookPitchOffset = 0.0f;
    rt.camera.zoom = 2.4f;
    rt.flag5414bc = false;
    ++rt.seams.hudEventCalls;         // FUN_0040210c + FUN_00402388
    rt.camera.eyeHeight = 4.5f;
    rt.scopeHudOffset = -101;
    rt.motion.zoomChannel = 0.0f;
    rt.motion.turnVel = 0.0f;
    rt.motion.strafeVel = 0.0f;
    rt.motion.moveVel = 0.0f;
    rt.camera.pullback = 8.0f;
    rt.seams.hudEventCalls += 2;      // FUN_00402014 + FUN_0040210c
  }

  // 0x464a56 — the fire gate: ce770 != 0 && d0c >= 5 &&
  // 54161b == 0 -> FUN_0045f138.
  if (ctrl.fire != 0 && rt.fieldD0c >= 5 && rt.fireCadence == 0.0f) {
    sniperFireSeam(rt);
  }

  // 0x464a7c — the zoom tail.
  sniperZoomUpdate(rt, f0);
}

void sniperZoomUpdate(TraversalRuntime& rt, float f0) {
  // FUN_00464b50 — scoped only (c9c && ca0 gates at the head).
  if (rt.flagC9c == 0 || rt.transitionPhase == 0) return;

  // The zoom floor: min(floor, 0.25). With a focus object (cc8) the
  // floor is focus-derived (extent*1.5/dist*0.25); else 1000 -> the
  // 0.25 cap wins.
  float floor = kZoomFloorOpen;
  if (rt.focusObj != nullptr) {
    const CollisionObject& o = rt.focusObj->col;
    double extent = (double)o.aabb[5] - (double)o.aabb[2];
    if (extent < kZoomExtentFloor) extent = kZoomExtentFloor;
    // focusDist = dist(player, obj->pos) — the +0x10 field.
    const float dx = rt.focusObj->pos[0] - rt.cs.pos[0];
    const float dy = rt.focusObj->pos[1] - rt.cs.pos[1];
    const float dz = rt.focusObj->pos[2] - rt.cs.pos[2];
    rt.focusDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    floor = static_cast<float>(extent * kZoomFocusK /
                               (double)rt.focusDist * 0.25);
  }
  if (floor > kZoomFloorCap) floor = kZoomFloorCap;

  // The zoom channel d54: fed by zoomVel (ce6f0) through accelChannel
  // else linearDecay(0.0147).
  if (rt.prevFrame.zoomVel != 0.0f) {
    accelChannel(rt.motion.zoomChannel, rt.prevFrame.zoomVel,
                 rt.prevFrame.zoomVelFast, f0);
  } else {
    linearDecay(rt.motion.zoomChannel, kZoomDecayRate, f0);
  }

  // Apply the zoom to b58 (camera.zoom).
  if (rt.motion.zoomChannel < 0.0f) {
    // d54 < 0 -> zoom in: b58 /= (1 - d54), floored.
    if (rt.camera.zoom > floor) {
      rt.camera.zoom = static_cast<float>(
          (double)rt.camera.zoom / (1.0 - (double)rt.motion.zoomChannel));
      ++rt.seams.hudEventCalls;   // FUN_00402388
      if (rt.camera.zoom <= floor) rt.camera.zoom = floor;
    } else {
      ++rt.seams.hudEventCalls;   // at floor — FUN_0040210c
    }
  } else if (rt.motion.zoomChannel > 0.0f) {
    // d54 > 0 -> zoom out: b58 *= (1 + d54) while b58 > 1.0.
    if (rt.camera.zoom > kZoomCeil) {
      ++rt.seams.hudEventCalls;   // FUN_00402388
      rt.camera.zoom = static_cast<float>(
          (double)rt.camera.zoom * (1.0 + (double)rt.motion.zoomChannel));
    } else {
      ++rt.seams.hudEventCalls;   // at ceiling — FUN_0040210c
    }
  } else {
    ++rt.seams.hudEventCalls;     // d54 == 0 — FUN_0040210c
  }
  // Universal ceiling: b58 >= 1.0 -> 1.0.
  if (rt.camera.zoom >= kZoomCeil) rt.camera.zoom = kZoomCeil;
}

void sniperReset(TraversalRuntime& rt) {
  // FUN_00461878 — runs only while scoped (c9c != 0).
  if (rt.flagC9c == 0) return;
  rt.flagC9c = 0;
  ++rt.seams.sniperExitCalls;           // FUN_0046ca84
  if (rt.scopeAnimLatch != 0) {
    ++rt.seams.scopeOverlayCalls;       // FUN_00416700 release
    rt.scopeAnimLatch = 0;
  }
  rt.viewScalar = rt.cur != nullptr ? rt.cur->scalar : 0.0f;
  rt.flag5414bc = false;
  rt.motion.zoomChannel = 0.0f;         // d54 = 0
  rt.motion.turnVel = 0.0f;             // d50 = 0
  rt.motion.strafeVel = 0.0f;           // d4c = 0
  rt.motion.moveVel = 0.0f;             // d48 = 0
  rt.camera.zoom = 2.4f;                // b58 = 2.4
  rt.seams.hudEventCalls += 3;          // FUN_00402014 + 2x FUN_0040210c
  rt.camera.eyeHeight = 4.5f;           // db8 = 4.5
  rt.transitionPhase = 0;               // ca0 = 0
  rt.scopeHudOffset = -101;             // d34 = -101
  rt.look.lookPitchOffset = 0.0f;       // d58 = 0
  rt.eventPriority = 0;                 // cbc = 0
  rt.camera.pullback = 8.0f;            // db4 = 8.0
  rt.locoState = 0x64;                  // cac = 0x64
}

void sniperDamageDrain(TraversalRuntime& rt, int amount) {
  // FUN_00467a00 — the difficulty-scaled drain. Gate: runs only when
  // health or the health gate is nonzero.
  if (rt.fieldHealth == 0 && rt.fieldHealthGate == 0) return;
  int scaled = amount;
  const int twoA = amount + amount;
  if (rt.difficulty == 0) {
    scaled = twoA / 3;
    if (scaled < 1) scaled = 1;         // easy -> max(1, 2a/3)
  } else if (rt.difficulty == 2) {
    scaled = twoA;                      // hard -> 2a
  }                                     // normal -> a
  if (scaled > 0) {
    rt.fieldDac += 25 * scaled;         // dac += 25*scaled
    if (rt.fieldDac > 180) rt.fieldDac = 180;
    else if (rt.fieldDac < 75) rt.fieldDac = 75;
  }
  rt.fieldHealth -= scaled;
  if (rt.fieldHealth < 0) rt.fieldHealth = 0;
  rt.vert.landingAccum += static_cast<float>(scaled);   // d5c += scaled
}

void sniperFireSeam(TraversalRuntime& rt) {
  // FUN_0045f138 — the observable writes only (the projectile spawn
  // is a counted seam): 54161b += 1.0 (the cadence/blend timer),
  // 54161a -= 1, 541633 -= 1. d0c = 0 only on the NON-sniper branch.
  rt.fireCadence += 1.0f;
  rt.burstIndex -= 1;
  ++rt.seams.sniperFireCalls;
}

void playerAnimAdvance(TraversalRuntime& rt, int frameStep) {
  // OBSERVED gate — FUN_00431300 runs the player anim job only when
  // (!c9c || ca0==0) && (!e6c || !(e70 & 0x20)); FUN_00461954's head
  // then skips to the HUD tail when (0x4999d0 && 0x541548) — skipping
  // the cb0 latch too.
  const bool gate =
      (rt.flagC9c == 0 || rt.transitionPhase == 0) &&
      (rt.cs.excludeObj == nullptr || (rt.mountClass & 0x20) == 0) &&
      !(rt.flag4999d0 && rt.flag541548);
  if (!gate) return;

  if (rt.locoState == 0x323) {
    // 0x323 (scope-in): the first-frame path resets cb4; the steady
    // path advances it — then the shared body runs either way.
    if (rt.animPrev == rt.locoState) rt.animFrame += frameStep;
    else rt.animFrame = 0;
    // 0x461e10 — HUD seam, then d34 = rint(scopeScale*eyeHeight + 29)
    // (FUN_0047d59a = round-to-nearest), pullback = 0, eyeHeight = 4.0,
    // and ca0 = 1 on the pending frame.
    ++rt.seams.hudEventCalls;   // FUN_00402388
    rt.scopeHudOffset = static_cast<int>(std::lrint(
        (double)rt.scopeScale * (double)rt.camera.eyeHeight + 29.0));
    rt.camera.pullback = 0.0f;
    rt.camera.eyeHeight = 4.0f;
    if (rt.transitionPhase == 0) {
      ++rt.seams.hudEventCalls;   // FUN_00401ffc
      rt.transitionPhase = 1;
    }
  } else if (rt.locoState == 0x384) {
    // 0x384 (unscope): the steady path releases the scope overlay and
    // the event priority every frame; the first frame only resets
    // cb4. Both write the camera restore (pullback/eyeHeight/d34).
    if (rt.animPrev == rt.locoState) {
      ++rt.seams.scopeOverlayCalls;   // FUN_00416700 (overlay release)
      rt.scopeAnimLatch = 0;
      rt.eventPriority = 0;
      rt.animFrame += frameStep;
    } else {
      rt.animFrame = 0;
      rt.seams.hudEventCalls += 2;    // FUN_0040210c + FUN_00402388
    }
    rt.camera.pullback = 8.0f;
    rt.camera.eyeHeight = 4.5f;
    rt.scopeHudOffset = -101;
  }
  // 0x4619c4 — cb0 = cac: the first-frame detect latch runs for every
  // dispatched state (handled or not — the bounded subset still
  // mirrors it).
  rt.animPrev = rt.locoState;
}

} // namespace mdk
