// Phase 5J — player look and view orientation.
// FUN_00465c4c (semantic look integrator) + the FUN_004301e0
// orientation tail. See player_look.h for the evidence chain.

#include "core/player_look.h"

namespace mdk {

namespace {

// OBSERVED constants (MDK95.EXE BUILD_A constant pool):
//   0x498924 = 90.0   — look rate (deg/s) AND the look-down bound
//   0x498928 = -60.0  — look-up bound
//   0x498920 = 200.0  — recenter rate (deg/s)
//   0x497228 = 0.5 / 0x497230 = -0.5   — z-delta clamp
//   0x497238 = 0.97 / 0x497240 = 0.03  — z-delta EMA weights
//   0x497248 = 0.02                    — z-delta slew rate (f0-scaled)
//   0x497258 = 40.0 / 0x497268 = 40.0  — lift decay rate / cap
//   0x497260 = 2/3                     — lift gain on airCharge
constexpr float kLookRate = 90.0f;
constexpr float kLookUpBound = -60.0f;
constexpr float kLookDownBound = 90.0f;
constexpr float kLookRecenterRate = 200.0f;
constexpr float kZDClamp = 0.5f;
constexpr float kZDEmaOld = 0.97f;
constexpr float kZDEmaNew = 0.03f;
constexpr float kZDSlew = 0.02f;
constexpr float kLiftDecay = 40.0f;
constexpr float kLiftCap = 40.0f;
constexpr float kLiftGain = 2.0f / 3.0f;

} // namespace

PlayerLookFrame integratePlayerLook(const GameplayInputFrame& ctrl,
                                    const PlayerLookEnvironment& env,
                                    PlayerLookState& s) {
  PlayerLookFrame f;
  // 0x465c63..0x465c92 — the eligibility predicate, evaluated in
  // this order: cbc >= 8 && cac != 0x324 → ineligible; then the
  // vertVel bit test; then the grounded bit. Any failure routes to
  // the recenter branch (it does NOT early-out).
  const bool eligible =
      (env.eventPriority < 8 || env.locoState == kLookEventCode) &&
      env.vertVelZero && env.grounded;
  // Arena-relative bounds: 0x498928/0x498924 minus [c48]+0x462.
  const float lo = kLookUpBound - env.arenaScalar;
  const float hi = kLookDownBound - env.arenaScalar;
  if (eligible && ctrl.lookUp != 0) {
    // 0x465ca5..0x465ce4 — d58 = max(d58 - f4*90, -60 - a462).
    // lookUp is tested FIRST (0x465c9c) — it wins a press-conflict.
    s.lookPitchOffset -= env.deltaSeconds * kLookRate;
    if (s.lookPitchOffset < lo) s.lookPitchOffset = lo;
    f.eventPosted = true;
  } else if (eligible && ctrl.lookDown != 0) {
    // 0x465d2b..0x465d66 — d58 = min(d58 + f4*90, +90 - a462).
    s.lookPitchOffset += env.deltaSeconds * kLookRate;
    if (s.lookPitchOffset > hi) s.lookPitchOffset = hi;
    f.eventPosted = true;
  } else if (s.lookPitchOffset != 0.0f) {
    // 0x465d69..0x465de0 — recenter at f4*200, sign-snapped to 0.
    // Reached on ANY eligibility failure or idle controls; the post
    // still fires while the offset is draining.
    const float t = env.deltaSeconds * kLookRecenterRate;
    if (s.lookPitchOffset < 0.0f) {
      s.lookPitchOffset += t;
      if (s.lookPitchOffset > 0.0f) s.lookPitchOffset = 0.0f;
    } else {
      s.lookPitchOffset -= t;
      if (s.lookPitchOffset < 0.0f) s.lookPitchOffset = 0.0f;
    }
    f.eventPosted = true;
  }
  // d58 == 0: the original rewrites cb00/cb08 with the entry values
  // (0x465db2 JNC 0x465cf1) — a self-store, no observable post.
  return f;
}

void updatePlayerViewTail(const PlayerViewTailEnvironment& env,
                          PlayerViewTail& t) {
  // 0x430272..0x4302ff — dz clamp ±0.5, then the 0x49b718 smoothed
  // z-delta: EMA old*0.97 + dz*0.03; on sign disagreement with the
  // raw input it additionally slews toward dz at f0*0.02, never
  // crossing it (the EMA alone decays too slowly to flip sign).
  float dz = env.dz;
  if (dz > kZDClamp) dz = kZDClamp;
  else if (dz < -kZDClamp) dz = -kZDClamp;
  t.viewZDelta = t.viewZDelta * kZDEmaOld + dz * kZDEmaNew;
  if (t.viewZDelta > 0.0f && dz <= 0.0f) {
    t.viewZDelta -= env.smoothed * kZDSlew;
    if (t.viewZDelta < dz) t.viewZDelta = dz;
  } else if (t.viewZDelta < 0.0f && dz >= 0.0f) {
    t.viewZDelta += env.smoothed * kZDSlew;
    if (t.viewZDelta > dz) t.viewZDelta = dz;
  }

  // 0x430300..0x430337 — the consumer-side look clamp: while the
  // offset is nonzero AND the scalar is mid-blend, the sum
  // viewScalar + lookEff is bounded to the same arena-relative
  // range the integrator enforces on d58 ([-60-a462, +90-a462]).
  t.lookEffDeg = env.lookOffset;
  if (env.lookOffset != 0.0f && env.viewScalar != env.arenaScalar) {
    const float sum = env.viewScalar + env.lookOffset;
    const float hi = kLookDownBound - env.arenaScalar;
    const float lo = kLookUpBound - env.arenaScalar;
    if (sum > hi) t.lookEffDeg = hi - env.viewScalar;
    else if (sum < lo) t.lookEffDeg = lo - env.viewScalar;
  }

  // 0x540b50 — view yaw mirrors locomotion yaw about +90 deg.
  t.viewYawDeg = 90.0f - env.yawDeg;

  // 0x49b71c — air-charge pitch lift: c84 * 2/3 capped at 40 while
  // the counter is nonzero, else decaying to 0 at f4 * 40.
  if (env.airCharge == 0.0f) {
    if (t.viewPitchLift > 0.0f)
      t.viewPitchLift -= env.deltaSeconds * kLiftDecay;
    if (t.viewPitchLift < 0.0f) t.viewPitchLift = 0.0f;
  } else {
    t.viewPitchLift = env.airCharge * kLiftGain;
    if (t.viewPitchLift > kLiftCap) t.viewPitchLift = kLiftCap;
  }

  // 0x540be0 — effective pitch = scalar + look - dip + lift.
  t.viewPitchDeg = env.viewScalar + t.lookEffDeg -
                   t.viewZDelta * 40.0f + t.viewPitchLift;
}

} // namespace mdk
