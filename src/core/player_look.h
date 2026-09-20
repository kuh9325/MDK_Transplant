// Phase 5J — player look and view orientation.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md §"Phase 5J" for the full
// evidence chain):
//
//   FUN_00465c4c — the normal-mode semantic look integrator. It runs
//   inside FUN_00465228's tail (after the jump machine FUN_00466740)
//   when the dispatched state 0x540cac < 800, and in the dispatcher's
//   cac >= 800 scripted branch — i.e., on BOTH traversal branches.
//   It integrates the persistent look offset 0x540d58 (DEGREES) from
//   the merged look controls 0x4ce780 (lookUp) / 0x4ce784 (lookDown)
//   — the PREVIOUS frame's control block, since FUN_00406f14 merges
//   at dispatch end (the same one-frame latency as movement):
//
//     eligible = (cbc < 8 || cac == 0x324)         // priority/state
//                && (c78 bits & 0x7fffffff) == 0   // vertVel == ±0
//                && (c54 & 1)                      // grounded
//     eligible && lookUp   → d58 -= f4 * 90;  clamp ≥ -60 - a462
//     eligible && lookDown → d58 += f4 * 90;  clamp ≤ +90 - a462
//     otherwise            → d58 → 0 at f4*200, sign-snapped
//
//   where a462 = [0x540c48]+0x462, the current arena's rest-pitch
//   scalar — so the absolute pitch a462 + d58 stays in [-60, +90].
//   Both controls pressed → lookUp wins (it is tested first). Every
//   driving or draining frame posts the pending event
//   cb00 = 8 / cb08 = 0x324 into the shared slots; when d58 is
//   exactly 0 the function rewrites the slots with the values it
//   loaded at entry — a self-store, no observable post. The post
//   keeps the look animation state 0x324 latched while the offset
//   is nonzero; once it reaches 0, FUN_00461954's 0x324 handler
//   clears the event priority cbc and the next frame's dispatcher
//   idle restore returns cac.
//
//   FUN_004301e0 tail — the view-orientation consumer (render pass,
//   after the traversal dispatch). The ported slice:
//     viewScalar (0x540b54) eases toward a462 at 0.85/0.15 while the
//       blend gate 0x540bec is clear;
//     0x49b718 smoothed z-delta: dz = clamp(posZ - prevPosZ, ±0.5),
//       EMA old*0.97 + dz*0.03, then a sign-disagreement slew at
//       f0*0.02 that never crosses the raw value;
//     lookEff = d58, but while d58 != 0 AND viewScalar != a462 the
//       sum viewScalar + lookEff is bounded to the same
//       arena-relative range [-60-a462, +90-a462];
//     0x540b50 viewYaw = 90 - locomotion yaw (0x540c2c);
//     0x49b71c pitch lift = airCharge(0x540c84) * 2/3 capped at 40
//       while the counter is nonzero, else decaying to 0 at f4*40;
//     effective pitch (0x540be0) = viewScalar + lookEff
//       - viewZDelta*40 + viewPitchLift.
//   The matrix rows / camera position / FOV writes that follow
//   (0x540b28..0x540bd8, 0x540bf0..) are the renderer boundary and
//   are NOT ported.
//
//   Raw DirectInput mouse deltas (0x54b644/0x54b648) are NOT consumed
//   by either function in normal traversal — FUN_00465228 never
//   reads them. The only raw-mouse look path is inside the sniper
//   branch FUN_00464624 (dispatcher gate c9c != 0 && ca0 != 0):
//   pitch += dy*0.12*f0*b58*(5/12) clamped to [-50,+50],
//   yaw   -= dx*0.12*f0*b58*(5/12) wrapped to [0,360),
//   gated on MouseOn (0x541472) and only when neither semantic aim
//   channel is active, with MouseYReversed (0x541476) negating the
//   scaled dy. All sniper-side — deferred, NOT ported here.
//
// NATIVE PORT DECISIONS:
//   - lookPitchOffset (d58) is the only persistent look state.
//   - The pending-event post is returned, not written — the runtime
//     owns the shared cb00/cb08 slots and the priority latch.
//   - The view tail is a pure function of the proven inputs; the
//     viewScalar blend and the blend gate stay in the runtime.
//   - Sniper (FUN_00464624) is a documented boundary, not ported.
//
#ifndef MDK_CORE_PLAYER_LOOK_H
#define MDK_CORE_PLAYER_LOOK_H

#include "core/gameplay_input.h"

namespace mdk {

// The pending-event pair FUN_00465c4c posts (0x54cb00 = 8 priority,
// 0x54cb08 = 0x324 look-state code).
inline constexpr int kLookEventPri = 8;
inline constexpr int kLookEventCode = 0x324;

// Persistent look state — the globals FUN_00465c4c carries across
// frames.
struct PlayerLookState {
  // DAT_00540d58 — semantic look pitch offset, DEGREES. Integrated
  // from the lookUp/lookDown controls; consumed by FUN_004301e0's
  // effective-pitch sum. Auto-recentres to 0 at 200 deg/s whenever
  // neither control is held or the mode predicate fails — the look
  // is momentary, not a persistent aim (OBSERVED).
  float lookPitchOffset = 0.0f;
};

// Per-frame inputs the look integrator reads from outside itself.
struct PlayerLookEnvironment {
  // DAT_0049b6f4 — delta-seconds constant (f32 1/30). The integrator
  // is f4-scaled (NOT f0 — OBSERVED asymmetry with movement).
  float deltaSeconds = 1.0f / 30.0f;
  // [0x540c48]+0x462 — current arena's rest-pitch scalar; the clamp
  // bounds are this field minus the fixed constants.
  float arenaScalar = 0.0f;
  // DAT_00540cbc — current event priority (the dispatcher's cbc).
  int eventPriority = 0;
  // DAT_00540cac — dispatched state code (the dispatcher's cac).
  int locoState = 0;
  // (DAT_00540c78 bits & 0x7fffffff) == 0 — vertical velocity is ±0.
  bool vertVelZero = false;
  // DAT_00540c54 & 1 — grounded contact bit.
  bool grounded = false;
};

// What FUN_00465c4c pushes into the shared pending slots this frame.
// The posted values are the constants 8 / 0x324; `eventPosted` is
// false exactly when d58 sat at 0 in the recenter branch (the
// original self-stores the entry values — no observable write).
struct PlayerLookFrame {
  bool eventPosted = false;
};

// FUN_00465c4c — the semantic look integrator. `ctrl` is the
// PREVIOUS frame's merged control block (the N-1 latency seam).
PlayerLookFrame integratePlayerLook(const GameplayInputFrame& ctrl,
                                    const PlayerLookEnvironment& env,
                                    PlayerLookState& s);

// Persistent view-orientation state and the derived per-frame pose —
// the FUN_004301e0 tail globals the look path depends on.
struct PlayerViewTail {
  // DAT_0049b718 — smoothed vertical-delta (pitch-dip) follower:
  // EMA old*0.97 + dz*0.03, then the sign-disagreement slew.
  float viewZDelta = 0.0f;
  // DAT_0049b71c — air-charge pitch lift (deg).
  float viewPitchLift = 0.0f;
  // DAT_00540b50 — view yaw = 90 - locomotion yaw (deg).
  float viewYawDeg = 0.0f;
  // DAT_00540be0 — effective pitch the renderer consumes (deg).
  float viewPitchDeg = 0.0f;
  // The clamped effective look offset actually summed (local_28 in
  // FUN_004301e0) — d58 or the arena-relative bound minus b54.
  float lookEffDeg = 0.0f;
};

// Per-frame inputs to the view tail.
struct PlayerViewTailEnvironment {
  // DAT_0049b6f0 — smoothed frame-units factor (~1.0 at 30 Hz); the
  // z-delta slew is f0-scaled (OBSERVED — unlike the f4 lift decay).
  float smoothed = 1.0f;
  // DAT_0049b6f4 — delta-seconds constant; the lift decay is f4-scaled.
  float deltaSeconds = 1.0f / 30.0f;
  // [0x540c48]+0x462 — current arena rest-pitch scalar.
  float arenaScalar = 0.0f;
  // DAT_00540c2c — locomotion yaw (deg).
  float yawDeg = 0.0f;
  // DAT_00540d58 — semantic look offset (deg).
  float lookOffset = 0.0f;
  // DAT_00540b54 — current view scalar, post-blend (deg).
  float viewScalar = 0.0f;
  // Raw vertical displacement this frame (posZ - prevPosZ), BEFORE
  // the ±0.5 clamp — the function applies the clamp itself.
  float dz = 0.0f;
  // DAT_00540c84 — the air-charge counter (PlayerMotionState).
  float airCharge = 0.0f;
};

// FUN_004301e0's orientation tail (the shared non-snap branch):
// z-delta clamp+EMA+slew → lookEff clamp → viewYaw → pitch lift →
// effective pitch. The viewScalar blend (0x540b54 ← *0.85 +
// a462*0.15) and the blend gate 0x540bec stay with the runtime —
// they precede this tail in the original's order.
void updatePlayerViewTail(const PlayerViewTailEnvironment& env,
                          PlayerViewTail& t);

} // namespace mdk

#endif // MDK_CORE_PLAYER_LOOK_H
