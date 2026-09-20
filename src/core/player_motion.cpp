// Phase 5B — first player movement consumer (see player_motion.h
// for the evidence map and boundary decisions).

#include "core/player_motion.h"

#include "core/motion_channels.h"

#include <cmath>

namespace mdk {
namespace {

// OBSERVED constants (raw dumps, MDK95.EXE BUILD_A):
constexpr double kDegToRad = 3.141592653589793 / 180.0;  // 0x497924
constexpr double kBankRate = 0.25;      // 0x4988e8 (FMUL double)
constexpr float kBankKickPos = 2.0f;    // 0x4988f0
constexpr float kBankKickNeg = -2.0f;   // 0x4988f4
constexpr float kBankLimit = 10.0f;     // +-10 (0x41200000 / 0x4988f8)
constexpr double kBankDecayRate = 0.35; // 0x4986c0
constexpr double kBankDecayCeil = 2.5;  // 0x4986c8
constexpr double kBankDecayFloor = 0.05;// 0x4986d0
constexpr double kDecelOut = 8.0 / 45.0;   // 0x498900 (rOut)
constexpr double kDecelIn = 4.0 / 45.0;    // 0x498908 (rIn)
constexpr float kMoveDecayBound = 0.6666667f;  // 0x3f2aaaab literal
constexpr float kTurnDecayIn = 0.55f;   // 0x3f0ccccd literal
constexpr float kTurnDecayOut = 1.6f;   // 0x3fcccccd literal
constexpr float kTurnDecayBound = 4.0f; // 0x40800000 literal
constexpr float kYawWrap = 360.0f;      // 0x498910/-, 0x498914/+
constexpr double kAirDrainRate = 1.75;  // 0x498918 (FMUL double)
constexpr float kAirDrainFloor = 20.0f; // 0x41a00000 literal
constexpr float kAirDrainCeil = 60.0f;  // 0x42700000 literal
constexpr int kEventTurn = 4, kEventTurnMag = 400;   // 4 / 0x190
constexpr int kEventStrafe = 5, kEventStrafeMag = 500;  // 5 / 0x1f4
constexpr int kEventMove = 6, kEventMoveMag = 600;      // 6 / 0x258

} // namespace

PlayerMotionOutput integratePlayerMotion(const GameplayInputFrame& f,
                                         const PlayerMotionEnvironment& env,
                                         PlayerMotionState& s) {
  PlayerMotionOutput out{};
  // DAT_00540d9c — the master gate: an immediate RET before any
  // work (no accel, no decay, no events).
  if (env.masterGate) return out;
  out.ran = true;
  const float f0 = env.smoothed;

  // accel/decel scales from the ground-contact surface state
  // (OBSERVED: e4c==0 -> 0.75/0.75; e4c without flag 4 -> 1.0/1.0;
  // e4c + flag 4 -> 0.5/0.1).
  float accelScale, decelScale;
  if (!env.groundContact) {
    accelScale = decelScale = 0.75f;
  } else if (!env.lowFriction) {
    accelScale = decelScale = 1.0f;
  } else {
    accelScale = 0.5f;
    decelScale = 0.1f;
  }

  // FUN_00437f98 — double-precision trig of yaw*deg-to-rad stored to
  // float: [EBP-0x34] = sin, [EBP-0x38] = cos.
  const double yawRad = (double)s.yawDeg * kDegToRad;
  const float sy = (float)std::sin(yawRad);
  const float cy = (float)std::cos(yawRad);

  // The original's local_18 consumed-input bitmask: 1 move, 2
  // strafe, 4 move-event latch, 8 turn.
  unsigned bits = 0;

  // ---- strafe channel (0x4ce6f8/0x4ce6fc -> DAT_00540d4c) --------
  if (f.strafeNorm != 0.0f) {
    accelChannel(s.strafeVel, f.strafeNorm * (float)accelScale,
                 f.strafeFast, f0);
    bits |= 0x2;
  }

  // ---- turn channel (0x4ce700/0x4ce704 -> DAT_00540d50) ----------
  // Gated by the turnLock; mouse-derived input uses the f0-scaled
  // helper, keyboard input the raw-add helper (0x4652ce/0x4657d7).
  if (f.turnNorm != 0.0f && s.turnLock == 0) {
    if (f.mouseTurnActive != 0) {
      accelChannel(s.turnVel, f.turnNorm, f.turnFast, f0);
    } else {
      directChannel(s.turnVel, f.turnNorm, f.turnFast);
    }
    bits |= 0x8;
  }
  // DAT_00540c94 maintain — runs whether or not input was consumed.
  if (s.turnLock == 1) {
    if (!(f.turnNorm < 0.0f)) s.turnLock = 0;
  } else if (s.turnLock == 2) {
    if (!(f.turnNorm > 0.0f)) s.turnLock = 0;
  } else if (s.turnLock > 2) {
    s.turnLock = 0;
  }

  // ---- move channel (0x4ce708/0x4ce70c -> DAT_00540d48) ----------
  if (f.moveVel != 0.0f && !env.moveBlocked) {
    accelChannel(s.moveVel, f.moveVel, f.moveVelBoosted, f0);
    out.moveConsumed = true;
    bits |= 0x1;
    out.forwardIntent = f.moveVel > 0.0f;
    // Bank drive: sign(turnNorm * moveVel) rolls the bank at
    // f0*0.25/frame with a +-2 snap-through kick, clamped +-10.
    const float bankDrive = f.turnNorm * f.moveVel;
    if (bankDrive < 0.0f) {
      s.bank -= f0 * (float)kBankRate;
      out.bankEvent = true;
      if (s.bank > 0.0f) s.bank += kBankKickNeg;
      if (s.bank < -kBankLimit) s.bank = -kBankLimit;
    } else if (bankDrive > 0.0f) {
      s.bank += f0 * (float)kBankRate;
      out.bankEvent = true;
      if (s.bank < 0.0f) s.bank += kBankKickPos;
      if (s.bank > kBankLimit) s.bank = kBankLimit;
    }
  }

  // ---- per-channel decay when the input was absent --------------
  if (!(bits & 0x1)) {
    decelChannel(s.moveVel, decelScale * (float)kDecelIn,
                 decelScale * (float)kDecelOut, kMoveDecayBound, f0);
  }
  if (!(bits & 0x2)) {
    decelChannel(s.strafeVel, decelScale * (float)kDecelIn,
                 decelScale * (float)kDecelOut, kMoveDecayBound, f0);
  }
  if (!(bits & 0x8)) {
    decelChannel(s.turnVel, kTurnDecayIn, kTurnDecayOut,
                 kTurnDecayBound, f0);
  }

  // ---- conveyor / surface displacement (FUN_00412ef0 seam) ------
  float dx = 0.0f, dy = 0.0f, dz = 0.0f;
  if (env.groundContact) {
    dx += env.conveyorX;
    dy += env.conveyorY;
    dz += env.conveyorZ;
  }

  // ---- displacement compose + event emit + yaw update -----------
  if (s.moveVel != 0.0f) {
    if (!(bits & 0x4)) {
      out.eventMag = kEventMoveMag;
      out.eventType = kEventMove;
      s.moveDirLatch = s.moveVel < 0.0f ? -1 : 1;
      bits |= 0x4;
    }
    const float p = s.moveVel * f0;
    dx += p * cy;
    dy += p * sy;
  }
  if (s.strafeVel != 0.0f) {
    if (!(bits & 0x4)) {
      out.eventMag = kEventStrafeMag;
      out.eventType = kEventStrafe;
    }
    const float p = s.strafeVel * f0;
    dx += p * sy;
    dy -= p * cy;
  }
  if (s.turnVel != 0.0f) {
    if (!(bits & 0x6)) {
      out.eventMag = kEventTurnMag;
      out.eventType = kEventTurn;
    }
    s.yawDeg -= s.turnVel * f0;
    if (s.yawDeg >= kYawWrap) s.yawDeg -= kYawWrap;
    if (s.yawDeg < 0.0f) s.yawDeg += kYawWrap;
  }

  out.dispX = dx;
  out.dispY = dy;
  out.dispZ = dz;
  return out;
}

void playerMotionPostStep(const PlayerMotionEnvironment& env,
                          bool positionChanged, PlayerMotionState& s,
                          PlayerMotionOutput& out) {
  if (out.ran) {
    // 0x4655f3..0x4658ea — a move event with no resulting position
    // change (the collision apply left X/Y untouched) is cancelled.
    if (out.eventMag == kEventMoveMag && !positionChanged) {
      out.eventMag = 0;
      out.eventType = 0;
    }
    // 0x465623..0x465675 — forward motion drains the airborne
    // counter toward 20 (soft ceiling 60 first).
    if (out.forwardIntent && s.airCharge > kAirDrainFloor) {
      if (s.airCharge > kAirDrainCeil) s.airCharge = kAirDrainCeil;
      s.airCharge -= env.smoothed * (float)kAirDrainRate;
      if (s.airCharge < kAirDrainFloor) s.airCharge = kAirDrainFloor;
    }
  }
  // FUN_00463608 shared tail — bank-roll decay while no bank event
  // fired; rate = clamp(|bank|*0.35, 0.05, 2.5) frame-scaled. Runs
  // even when the integrator was master-gated.
  if (!out.bankEvent) {
    if (s.bank > 0.0f) {
      float r = s.bank * (float)kBankDecayRate;
      if (r > (float)kBankDecayCeil) r = (float)kBankDecayCeil;
      if (r < (float)kBankDecayFloor) r = (float)kBankDecayFloor;
      s.bank -= r * env.smoothed;
      if (s.bank < 0.0f) s.bank = 0.0f;
    } else if (s.bank < 0.0f) {
      float r = -s.bank * (float)kBankDecayRate;
      if (r > (float)kBankDecayCeil) r = (float)kBankDecayCeil;
      if (r < (float)kBankDecayFloor) r = (float)kBankDecayFloor;
      s.bank += r * env.smoothed;
      if (s.bank > 0.0f) s.bank = 0.0f;
    }
  }
}

} // namespace mdk
