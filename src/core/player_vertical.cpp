// Phase 5C — jump sustain and vertical gravity (see
// player_vertical.h for the evidence map and boundary decisions).

#include "core/player_vertical.h"

#include <cstring>

namespace mdk {
namespace {

// OBSERVED constants (raw dumps + instruction-level operands,
// MDK95.EXE BUILD_A):
constexpr float kJumpImpulse = 40.0f;      // 0x42200000 literal (0x466a7a)
constexpr int kJumpHoldInit = 6;           // literal (0x466a75)
constexpr double kReleaseCutRate = 20.0;   // 0x4989d8
constexpr double kReleaseCutDiv = 1.0 / 6.0;  // 0x4989e0
constexpr double kAirSeedVel = -16.0;      // 0x4989d0 — c78 threshold
constexpr double kSlopeNormalMin = 0.25;   // 0x4989e8
// event words (0x54cb08 / 0x54cb00)
constexpr int kEvJumpIdle = 0x2be;         // 702 — standing jump
constexpr int kEvJumpMove = 0x2bf;         // 703 — moving jump
constexpr int kEvSustain = 0x2bd;          // 701 — sustained flight
constexpr int kEvFall = 700;               // 0x2bc — airborne release/bounce
constexpr int kEvHardLand = 806;           // 0x326 — hard landing
constexpr int kEvWordJump = 7, kEvWordLand = 8;
// FUN_00467180 gravity — the rise loop uses the precomputed f64
// products (rate * 1/30), the single-step path multiplies f4 live.
constexpr double kGravStepNormal = -2.133333333333333;   // 0x498aa0
constexpr double kGravStepSustain = -0.7111111111111111; // 0x498aa8
constexpr double kReboundStep = 8.533333333333333;       // 0x498ab0
constexpr double kGravRateNormal = 64.0;                 // 0x498a38
constexpr double kGravRateSustain = 64.0 / 3.0;          // 0x498a48
constexpr double kReboundRate = 256.0;                   // 0x498a58
constexpr double kTermNormal = -250.0;                   // 0x498a40
constexpr float kTermNormalF = -250.0f;                  // 0xc37a0000
constexpr double kTermSustain = -8.0;                    // 0x498a50
constexpr float kTermSustainF = -8.0f;                   // 0xc1000000
constexpr double kRiseCap = 40.0;                        // 0x498a68
constexpr float kRiseCapF = 40.0f;                       // 0x498a70
constexpr float kSubstepDt = 0.033333335f;               // 0x498a60
constexpr double kInVolDrain = 1.75;                     // 0x498a78
constexpr double kPreLandEps = 0.05;                     // 0x498a80
constexpr double kHardLandVel = -100.0;                  // 0x498a88
constexpr double kAntiJitter = 0.35;                     // 0x498a90
constexpr float kDeepFloorDelta = -50.0f;                // 0x498a98
constexpr std::int32_t kAirFloorBits = 0x3f800000;       // 1.0f bits

bool floatNonzero(float v) {
  std::uint32_t b;
  std::memcpy(&b, &v, 4);
  return (b & 0x7fffffffu) != 0;
}

// The in-volume air-charge floor is a SIGNED integer compare on the
// bit pattern (0x4672d5): patterns below 0x3f800000 — every float
// < 1.0f including all negatives — are rewritten to 1.0f.
void airChargeBitFloor(float& v) {
  std::int32_t b;
  std::memcpy(&b, &v, 4);
  if (b < kAirFloorBits) v = 1.0f;
}

} // namespace

PlayerVerticalFrame integratePlayerVertical(
    const PlayerVerticalEnvironment& env, PlayerMotionState& ms,
    PlayerVerticalState& vs) {
  PlayerVerticalFrame frame{};
  const float f0 = env.smoothed;
  const float f4 = env.deltaSeconds;
  float& airCharge = ms.airCharge;   // the shared 0x540c84

  // ================= FUN_00466740 =================
  if (!env.slideMode) {
    // 0x46675b — c88 maintain: cleared once the dispatched state
    // leaves the jump codes 0x2be/0x2bf.
    if (vs.jumpActive != 0 && env.locoState != kEvJumpIdle &&
        env.locoState != kEvJumpMove) {
      vs.jumpActive = 0;
    }

    // 0x466780 — air-charge update (reads the PREVIOUS frame's
    // velocity before this frame's gravity runs).
    if (!floatNonzero(airCharge)) {
      // 0x466980 — seed once falling faster than -16.
      if (vs.vertVel < (float)kAirSeedVel) airCharge += 1.0f;
    } else {
      if (env.locoState != kEvSustain && vs.vertVel > 0.0f) {
        airCharge = 0.0f;          // rising outside sustain state
      } else {
        // 0x466950 — airborne accumulate / grounded reset.
        if (floatNonzero(vs.vertVel)) {
          airCharge += f0;
        } else if ((vs.contactFlags & 0x1) != 0) {
          airCharge = 0.0f;
        } else {
          airCharge += f0;
        }
      }
    }

    if (vs.jumpActive == 0) {
      // 0x4669f4 — jump initiation gate: event channel free,
      // velocity exactly zero, grounded, then the c90 edge latch.
      if (vs.eventIdle < 7 && env.eventWordType < 7 &&
          !floatNonzero(vs.vertVel) && (vs.contactFlags & 0x1) != 0) {
        if (vs.jumpLatch != 0) {
          if (!env.jumpHeld) vs.jumpLatch = 0;   // re-arm
        } else if (env.jumpHeld) {
          frame.eventMag =
              env.moveConsumed ? kEvJumpMove : kEvJumpIdle;
          frame.eventType = kEvWordJump;
          vs.jumpActive = 1;
          vs.jumpHoldCharge = kJumpHoldInit;
          vs.vertVel = kJumpImpulse;
          vs.jumpAux = 1;
          vs.jumpLatch = 1;
          frame.jumped = true;
        }
      }
    } else {
      // 0x4667c6 — held: the hold charge drains by frameStep.
      // Released: the remaining charge cuts the upward velocity.
      if (!env.jumpHeld) {
        // 0x4669b5 — release cut: c78 -= c8c * 20 * (1/6).
        if (vs.jumpHoldCharge > 0) {
          vs.vertVel = (float)((double)vs.vertVel -
              (double)vs.jumpHoldCharge * kReleaseCutRate *
                  kReleaseCutDiv);
          vs.jumpHoldCharge = 0;
        }
        vs.jumpAux = 0;
      } else if (vs.jumpHoldCharge > 0) {
        // 0x4667d4 — charge decay while held (JLE skips the write
        // entirely once the charge is spent).
        vs.jumpHoldCharge -= env.frameStep;
        if (vs.jumpHoldCharge < 0) vs.jumpHoldCharge = 0;
      }
    }

    // 0x4667f6 — sustain flag rewrite (every frame).
    vs.jumpSustain = 0;
    if (floatNonzero(airCharge)) {
      if (!env.jumpHeld) {
        // 0x466ab1 — airborne release: fall event + latch held.
        frame.eventMag = kEvFall;
        frame.eventType = kEvWordJump;
        vs.jumpAux = 0;
        vs.jumpLatch = 1;
      } else if (vs.bounceFlag != 0) {
        // Same event word but no latch/aux writes (bounce in flight).
        frame.eventMag = kEvFall;
        frame.eventType = kEvWordJump;
      } else {
        // 0x466848 — sustained flight event.
        if (vs.eventIdle == 7) vs.eventIdle = 0;
        frame.eventMag = kEvSustain;
        frame.eventType = kEvWordJump;
        vs.jumpSustain = 1;
      }
    }

    // 0x46685f — slope assist: on steep contacts while grounded,
    // moving into the upward normal pulls vertVel downward.
    if (vs.contactObj != 0 && env.slideVec != nullptr &&
        vs.jumpActive == 0 && !floatNonzero(airCharge)) {
      float nx = vs.contactNormal[0];
      float ny = vs.contactNormal[1];
      float nz = vs.contactNormal[2];
      if (nz < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
      if (nz > (float)kSlopeNormalMin) {
        float fVar1 = env.slideVec[0] * nx + env.slideVec[1] * ny;
        if (fVar1 > 0.0f) {
          fVar1 = fVar1 / -f4;
          if (fVar1 < vs.vertVel) vs.vertVel = fVar1;
        }
      }
    }
  }
  frame.sustain = vs.jumpSustain != 0;

  // ================= FUN_00467180 =================
  return integratePlayerGravity(env, ms, vs, frame);
}

// FUN_00467180 — the vertical gravity + collision request. Extracted
// from integratePlayerVertical because the sniper path (FUN_00464624)
// calls it WITHOUT the FUN_00466740 jump machine (OBSERVED 0x464637).
// Runs the gravity integration, the ribbon-volume check, the rise cap
// and the pre-land clamp, then hands the caller a FUN_004630d4 request.
PlayerVerticalFrame integratePlayerGravity(
    const PlayerVerticalEnvironment& env, PlayerMotionState& ms,
    PlayerVerticalState& vs, PlayerVerticalFrame frame) {
  const float f0 = env.smoothed;
  const float f4 = env.deltaSeconds;
  float& airCharge = ms.airCharge;   // the shared 0x540c84

  // 0x46718b/0x467198 — the vertical master gate and the
  // mantle/vertical-skip gate return before any work.
  if (!env.vertEnable || vs.vertSkip != 0) return frame;

  float disp = 0.0f;   // local_24 — accumulated Z displacement
  bool preLand = false;

  if (vs.vertVel > 0.0f && !env.slideMode) {
    // 0x4671c9 — the rise loop: frameStep gravity substeps. The
    // terminal compares run on the pre-truncation double (FCOMP
    // double) — OBSERVED precision detail.
    for (int i = 0; i < env.frameStep; ++i) {
      if (vs.jumpSustain != 0) {
        double nv = (double)vs.vertVel + kGravStepSustain;
        vs.vertVel = (float)nv;
        if (nv < kTermSustain) {
          nv += kReboundStep;
          vs.vertVel = (float)nv;
          if (nv > kTermSustain) vs.vertVel = kTermSustainF;
        }
      } else {
        const double nv = (double)vs.vertVel + kGravStepNormal;
        vs.vertVel = (float)nv;
        if (nv < kTermNormal) vs.vertVel = kTermNormalF;
      }
      disp += vs.vertVel * kSubstepDt;
    }
  } else {
    // 0x467448 — falling (or slide-mode rise): exactly ONE f4 step.
    if (vs.jumpSustain != 0) {
      const double f4d = (double)f4;
      double nv = (double)vs.vertVel - f4d * kGravRateSustain;
      vs.vertVel = (float)nv;
      if (nv < kTermSustain) {
        nv += f4d * kReboundRate;
        vs.vertVel = (float)nv;
        if (nv > kTermSustain) vs.vertVel = kTermSustainF;
      }
    } else {
      const double nv = (double)vs.vertVel - (double)f4 * kGravRateNormal;
      vs.vertVel = (float)nv;
      if (nv < kTermNormal) vs.vertVel = kTermNormalF;
    }
    disp = vs.vertVel * f4;
  }

  // 0x467247 — ribbon-volume check (FUN_00412e94 on the player
  // object, then on the carrier when present and ungated).
  bool inVol = env.insideRibbonVolume;
  if (!inVol && env.carrierObj && !env.carrierCheckGate) {
    inVol = env.carrierInsideRibbonVolume;
  }
  frame.inRibbonVolume = inVol;
  if (inVol) {
    // 0x467276 — the query's out-vector z is copied back into c78
    // BEFORE the sustain rewrite (FUN_00412e94 out-param, modeled
    // nullable; the volume system itself is deferred).
    if (env.ribbonVelZ != nullptr) vs.vertVel = *env.ribbonVelZ;
    // Forced sustain + air-charge drain (floor 1.0f).
    if (vs.eventIdle == 7) vs.eventIdle = 0;
    frame.eventMag = kEvSustain;
    frame.eventType = kEvWordJump;
    vs.jumpSustain = 1;
    airCharge =
        (float)((double)airCharge - (double)f0 * kInVolDrain);
    airChargeBitFloor(airCharge);
    disp = vs.vertVel * f4;   // recomputed — single step even on rise
  } else {
    // 0x4674f6 — rise cap (not in cac>=800 states, gated by e6c).
    if (env.locoState < 800 && !env.sharedGateE6C &&
        vs.vertVel > (float)kRiseCap) {
      vs.vertVel = kRiseCapF;
      disp = f4 * kRiseCapF;
    }
  }

  // 0x467302 — pre-land clamp: when falling onto the probed floor,
  // the displacement is clamped to land 0.05 above it.
  if (vs.vertVel <= 0.0f && (vs.contactFlags & 0x2) != 0 &&
      vs.posZ + disp <= vs.floorZ) {
    disp = (float)((double)vs.floorZ + kPreLandEps - (double)vs.posZ);
    preLand = true;
  }
  frame.preLand = preLand;
  frame.dispZ = disp;
  frame.collisionIssued = true;
  frame.sustain = vs.jumpSustain != 0;
  return frame;
}

const CollisionPoly* playerVerticalApplyCollision(
    const PlayerVerticalEnvironment& env, CollisionState& cs,
    PlayerMotionState& ms, PlayerVerticalState& vs,
    PlayerVerticalFrame& frame, float* appliedDispZ) {
  // FUN_004630d4(ctx, mode, 0, 0, dispZ, 0.5, 0, &e50) — the vertical
  // sweep. appliedDispZ is measured against the PRE-call posZ (the
  // snapshot vs.posZ still holds — the apply writes cs.pos, not vs).
  const CollisionNode* node = nullptr;
  const CollisionPoly* vContact =
      collisionApply(cs, 0.0f, 0.0f, frame.dispZ, 0.5f, nullptr, &node);
  if (appliedDispZ) *appliedDispZ = cs.pos[2] - vs.posZ;
  // 0x540e4c — every apply's EAX is stored (0 clears). The contact
  // normal feeds the slope-assist + hard-landing consumers.
  vs.contactObj = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(vContact));
  VerticalCollisionResult vres;
  vres.contactObj = vs.contactObj;
  vres.posX = cs.pos[0];
  vres.posY = cs.pos[1];
  vres.posZ = cs.pos[2];
  if (vContact) {
    vres.normalX = node->nx;
    vres.normalY = node->ny;
    vres.normalZ = node->nz;
    vs.contactNormal[0] = node->nx;
    vs.contactNormal[1] = node->ny;
    vs.contactNormal[2] = node->nz;
  }
  vres.hasFloor = (cs.contactFlags & 2) != 0;
  vres.floorZ = cs.floorZ;
  vres.blocker0 = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(cs.floorObj));
  vres.blocker1 = cs.floorElemMask;
  vres.blocker0Flag80 =
      cs.floorObj && (cs.floorObj->flags14a & 0x80);
  applyPlayerVerticalCollision(env, ms, vs, vres, frame);
  return vContact;
}

void applyPlayerVerticalCollision(
    const PlayerVerticalEnvironment& env, PlayerMotionState& ms,
    PlayerVerticalState& vs, const VerticalCollisionResult& res,
    PlayerVerticalFrame& frame) {
  const float f4 = env.deltaSeconds;
  float& airCharge = ms.airCharge;
  // Pre-call snapshot (EBP-0x70): the position handed to the seam.
  const float oldX = vs.posX, oldY = vs.posY, oldZ = vs.posZ;

  // 0x467375 — the grounded bit is cleared before every call.
  vs.contactFlags = (std::uint8_t)(vs.contactFlags & ~0x1);

  // Seam apply + probe refresh (FUN_00435eec inside the call).
  vs.posX = res.posX;
  vs.posY = res.posY;
  vs.posZ = res.posZ;
  vs.contactObj = res.contactObj;
  vs.contactNormal[0] = res.normalX;
  vs.contactNormal[1] = res.normalY;
  vs.contactNormal[2] = res.normalZ;
  vs.contactFlags =
      (std::uint8_t)((vs.contactFlags & ~0x2) | (res.hasFloor ? 0x2 : 0));
  vs.floorZ = res.floorZ;
  vs.blocker0 = res.blocker0;
  vs.blocker1 = res.blocker1;
  if (res.bounce) vs.bounceFlag = 1;

  auto deepFloorCheck = [&] {
    // 0x4673ee — fell below the world's deep floor: forced grounded.
    if (env.deepFloorZ + kDeepFloorDelta >= vs.posZ) {
      vs.fallCounter = 0;
      vs.vertVel = 0.0f;
      vs.contactFlags |= 0x1;
      frame.deepFloorReset = true;
    }
  };

  if (vs.contactObj == 0) {
    if (!frame.preLand) {
      // 0x467580 — no contact, no clamp: if the applied Z still
      // dropped, the realized velocity is recomputed from the delta.
      if (vs.posZ < oldZ) {
        vs.vertVel = (vs.posZ - oldZ) / f4;
        frame.realizedVelocity = true;
      }
      deepFloorCheck();
      return;
    }
    // 0x46739b — landed on the epsilon clamp without contact: the
    // probe blockers become the movement blockers.
    vs.moveBlocker0 = vs.blocker0;
    vs.moveBlocker1 = vs.blocker1;
    frame.blockerRefresh = true;
    if (res.blocker0Flag80) {
      vs.moveBlockerFlag = 1;
    } else if (vs.moveBlockerFlag != 0) {
      frame.blockerReleased = true;   // FUN_00461878(0) — deferred
      vs.moveBlockerFlag = 0;
    }
  }
  // 0x4673cb — slide mode diverts to realized velocity for both the
  // contact and the pre-land routes (unconditional recompute).
  if (env.slideMode) {
    vs.vertVel = (vs.posZ - oldZ) / f4;
    frame.realizedVelocity = true;
    deepFloorCheck();
    return;
  }

  // 0x4675bf — the landing path (also reached via preLand w/o
  // contact). impact = velocity at contact time.
  const double impact = (double)vs.vertVel;
  if (impact > 0.0) {
    // 0x4676d4 — ceiling: velocity zeroed, contact discarded.
    vs.vertVel = 0.0f;
    vs.contactObj = 0;
    vs.contactFlags = (std::uint8_t)(vs.contactFlags & ~0x1);
    frame.ceilingHit = true;
    deepFloorCheck();
    return;
  }
  bool silent = false;
  if (impact < kHardLandVel && vs.bounceFlag == 0) {
    if (!env.sharedGateE6C) {
      // 0x4675fe — hard landing: event 806, idle reset, deferred
      // FUN_00467a00(10) damage accumulate.
      frame.eventMag = kEvHardLand;
      frame.eventType = kEvWordLand;
      vs.eventIdle = 0;
      vs.landingAccum = 0.0f;
      frame.hardLanding = true;
    } else if ((env.flagE72bit1 ? 1 : 0) != 0) {
      silent = true;   // suppressed event — landing without anti-jitter
    } else {
      frame.eventMag = kEvHardLand;
      frame.eventType = kEvWordLand;
      vs.eventIdle = 0;
      vs.landingAccum = 0.0f;
      frame.hardLanding = true;
    }
  }
  if (!silent && !frame.hardLanding) {
    // 0x467672 — soft landing (or a bounce): if the collision barely
    // moved the player (< f4*0.35) the pre-call position is restored.
    const float dx = oldX - vs.posX, dy = oldY - vs.posY,
                dz = oldZ - vs.posZ;
    const float thresh = f4 * (float)kAntiJitter;
    if (dx * dx + dy * dy + dz * dz < thresh * thresh) {
      vs.posX = oldX;
      vs.posY = oldY;
      vs.posZ = oldZ;
    }
  }
  // 0x46762d — the c84 clear is reached ONLY from the hard-land and
  // silent paths; the soft/bounce anti-jitter path jumps to
  // 0x467635, skipping it (c84 resets next frame via the grounded
  // branch of the 0x466950 update). OBSERVED asymmetry.
  if (frame.hardLanding || silent) airCharge = 0.0f;
  vs.vertVel = 0.0f;
  vs.contactFlags |= 0x1;
  frame.landed = true;
  if (vs.contactObj == 0) {
    // 0x467658 — the pre-land-no-contact path snaps to the floor.
    vs.posZ = vs.floorZ;
  }
  deepFloorCheck();
}

void playerVerticalPostStep(const PlayerVerticalEnvironment& env,
                            PlayerVerticalState& vs) {
  // 0x46692c — FUN_00466aec (mantle) is the deferred boundary; its
  // dependency fields (c78, c7c, cac, cbc, forward input) are all
  // retained. In slide mode the original RETs before this point.
  if (env.slideMode) return;
  vs.bounceFlag = 0;
}

} // namespace mdk
