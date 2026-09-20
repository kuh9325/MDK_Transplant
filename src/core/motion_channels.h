// Phase 5B/5L — the shared per-channel integrate/decay helpers.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A):
// these are the channel primitives the movement, sniper and
// mounted-reticle paths all share (the originals read the smoothed
// frame factor 0x49b6f0 as a global — the native port passes it as
// `f0`). Single-sourced here so player_motion.cpp (normal movement),
// player_sniper.cpp (FUN_00464624) and player_reticle.cpp
// (FUN_004691c4) all call the same reconstruction.
//
//   FUN_00465b54  accelChannel  — frame-scaled accelerate-toward-cap.
//                 A sign reversal REPLACES the velocity with the
//                 increment (no brake-through-zero). `cap` carries
//                 the sign (positive cap for positive rates).
//   FUN_00465bd8  directChannel — identical shape but the increment
//                 is the raw rate (NOT frame-scaled — OBSERVED quirk
//                 on the keyboard-turn and sniper-aim paths).
//   FUN_00465a84  decelChannel  — decay toward 0; rIn inside +-bound,
//                 rOut outside, both frame-scaled; snaps to 0 on
//                 crossing. The sniper aim channels invert the rates
//                 (rIn 16/15 fast inside, rOut 0.1 slow outside).
//   FUN_00465a28  linearDecay   — symmetric decay toward 0 at
//                 rate*f0, snapping through the crossing. Used by the
//                 reticle velocity channels and the slide vector.

#ifndef MDK_CORE_MOTION_CHANNELS_H
#define MDK_CORE_MOTION_CHANNELS_H

namespace mdk {

// FUN_00465b54 — frame-scaled accelerate-toward-cap.
inline void accelChannel(float& vel, float rate, float cap, float f0) {
  const float inc = rate * f0;
  if (rate >= 0.0f) {
    vel = vel >= 0.0f ? vel + inc : inc;
    if (cap < vel) vel = cap;
  } else {
    vel = vel <= 0.0f ? vel + inc : inc;
    if (vel < cap) vel = cap;
  }
}

// FUN_00465bd8 — raw-rate add-toward-cap (the keyboard-turn path and
// the sniper semantic-aim channels are NOT frame-scaled).
inline void directChannel(float& vel, float rate, float cap) {
  if (rate >= 0.0f) {
    vel = vel >= 0.0f ? vel + rate : rate;
    if (cap < vel) vel = cap;
  } else {
    vel = vel <= 0.0f ? vel + rate : rate;
    if (vel < cap) vel = cap;
  }
}

// FUN_00465a84 — decay toward 0; rIn inside +-bound, rOut outside,
// both frame-scaled; snaps to 0 on crossing.
inline void decelChannel(float& vel, float rIn, float rOut, float bound,
                         float f0) {
  if (vel <= bound) {
    if (vel <= 0.0f) {
      if (-bound <= vel) {
        if (vel < 0.0f) {
          vel += rIn * f0;
          if (vel > 0.0f) vel = 0.0f;
        }
      } else {
        vel += rOut * f0;
        if (vel > 0.0f) vel = 0.0f;
      }
    } else {
      vel -= rIn * f0;
      if (vel < 0.0f) vel = 0.0f;
    }
  } else {
    vel -= rOut * f0;
    if (vel < 0.0f) vel = 0.0f;
  }
}

// FUN_00465a28 — symmetric linear decay toward 0 at rate*f0, snapping
// through the crossing.
inline void linearDecay(float& vel, float rate, float f0) {
  const float step = rate * f0;
  if (vel > 0.0f) {
    vel -= step;
    if (vel < 0.0f) vel = 0.0f;
  } else if (vel < 0.0f) {
    vel += step;
    if (vel > 0.0f) vel = 0.0f;
  }
}

} // namespace mdk

#endif // MDK_CORE_MOTION_CHANNELS_H
