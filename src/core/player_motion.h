// Phase 5B — first player movement consumer.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md §"Phase 5B" for the full
// evidence chain):
//
//   Per frame the traversal FUN_00436100 -> FUN_00463608 runs the
//   normal-movement branch FUN_00465228 (gated on DAT_00540cac < 800,
//   DAT_00540e6c == 0, DAT_00540c9c == 0) BEFORE the control merge
//   FUN_00406f14. The integrator therefore consumes the PREVIOUS
//   frame's merged control block — one frame of input latency at the
//   original boundary. FUN_00465228's stack args are never read; it
//   is a pure global-state machine over the 0x540bfc..0x540eb8
//   player block plus the 0x4ce6e0 control block.
//
//   Reconstructed here (OBSERVED unless marked otherwise):
//     control rates -> persistent velocity channels ->
//     yaw-basis displacement + yaw update -> collision seam.
//
//     channel           input rate          cap               helper
//     ----------------  ------------------  ----------------  ------------
//     d48 moveVel       0x4ce708 moveVel    0x4ce70c boosted  f0-scaled
//     d4c strafeVel     0x4ce6f8 strafeNorm 0x4ce6fc fast     f0-scaled
//     d50 turnVel       0x4ce700 turnNorm   0x4ce704 fast     f0-scaled
//                                                          (mouse) /
//                                                          raw add (kbd)
//
//   Displacement (before the FUN_004630d4 collision/query call):
//     X += moveVel*f0*cos(yaw) + strafeVel*f0*sin(yaw)
//     Y += moveVel*f0*sin(yaw) - strafeVel*f0*cos(yaw)
//     yaw -= turnVel*f0, wrapped to [0,360) via +-360 constants.
//
//   The layer STOPS at the collision seam: dispX/Y/Z is what
//   FUN_004630d4 receives. The slide mode (FUN_0046603c), jump/
//   vertical state (FUN_00466740/FUN_00467180), look/pitch
//   (FUN_00465c4c), mantle (FUN_00466aec) and the fire/sniper/item
//   dispatch tail all consume the same control block but are
//   documented boundaries, not part of this layer.
//
// NATIVE PORT DECISIONS:
//   - The per-frame event word (DAT_0054cb00/0x54cb08) is OUTPUT
//     state — the dispatcher zeroes it before the movement branch.
//   - The bank-roll decay runs in the FUN_00463608 shared tail (not
//     inside FUN_00465228); it is reproduced in playerMotionPostStep
//     so the state object stays self-contained for the normal path.
//   - Gates the original reads from sibling systems (ground contact,
//     blocked flags, master gate, conveyor) are environment inputs.

#ifndef MDK_CORE_PLAYER_MOTION_H
#define MDK_CORE_PLAYER_MOTION_H

#include "core/gameplay_input.h"

namespace mdk {

// Persistent movement state — the globals FUN_00465228 carries
// across frames. Only fields the normal path proves are modeled.
struct PlayerMotionState {
  // DAT_00540d48/d4c/d50 — the three velocity channels, units of
  // displacement per frame-unit (d48/d4c capped at +-2/3 normal,
  // +-4/3 turbo; d50 is deg/frame-unit, capped at +-4 normal,
  // +-6 turbo, +-12+ under mouse impulse).
  float moveVel = 0.0f;    // DAT_00540d48 — forward/back channel.
                          // REMAPPED in sniper mode -> pitch rate, in
                          // the mounted reticle -> X pixel coord.
  float strafeVel = 0.0f;  // DAT_00540d4c — strafe channel (+ = right).
                          // REMAPPED in sniper mode -> yaw rate, in the
                          // mounted reticle -> Y pixel coord.
  float turnVel = 0.0f;    // DAT_00540d50 — yaw-rate channel. REMAPPED
                          // in sniper mode -> lateral strafe channel,
                          // in the mounted reticle -> X velocity.
  float zoomChannel = 0.0f;// DAT_00540d54 — the fourth shared channel.
                          // Unused by normal movement; the sniper zoom
                          // input (FUN_00464b50) and the reticle's Y
                          // velocity channel (FUN_004691c4) drive it.
  // DAT_00540c2c — persistent yaw, DEGREES, wrapped to [0,360) by
  // the +-360 float constants at 0x498910/0x498914. The basis is
  // sin/cos(yaw*pi/180); positive turnVel DECREASES yaw.
  float yawDeg = 0.0f;
  // DAT_00540b4c — bank/roll accumulator, clamped [-10,10]. Driven
  // by sign(turnNorm * moveVel) while moving at +-f0*0.25/frame with
  // a +-2 snap-through-kick; decays toward 0 in the dispatcher tail.
  float bank = 0.0f;
  // DAT_00540c84 — airborne counter owned by the deferred vertical
  // consumer FUN_00466740; the horizontal layer only drains it
  // (toward 20, cap 60, rate f0*1.75) while moving forward.
  float airCharge = 0.0f;
  // DAT_00540cc0 — +-1 move-direction latch, rewritten every frame
  // the move channel is nonzero (negative channel -> -1).
  int moveDirLatch = 0;
  // DAT_00540c94 — turn-direction lockout written by sibling movement
  // modes: 1 persists while turnNorm < 0, 2 persists while > 0,
  // anything else resets to 0. While nonzero the turn input is
  // ignored (the channel decays instead).
  int turnLock = 0;
};

// Runtime gates the integrator reads from systems outside this
// layer. All are per-frame inputs; nothing here persists.
struct PlayerMotionEnvironment {
  // DAT_0049b6f0 — smoothed frame-units factor (f0). ~1.0 at the
  // nominal 30 Hz. Scales accel increments and displacement alike.
  float smoothed = 1.0f;
  // DAT_00540d9c — nonzero returns immediately: no accel, no decay,
  // no displacement, no events (the bank decay in the dispatcher
  // tail still runs — see playerMotionPostStep).
  bool masterGate = false;
  // DAT_00540e4c != 0 — ground/contact object present. Selects the
  // accel/decel scales: contact 1.0/1.0, none 0.75/0.75, and gates
  // the conveyor contribution.
  bool groundContact = false;
  // (DAT_00540e4c + 0x20 surface flags) & 4 — low-friction ground:
  // accel/decel scales become 0.5/0.1 (weak drive, weak braking).
  bool lowFriction = false;
  // DAT_00540dc0 && DAT_00540dc8 — collision blockers; when both set
  // the move input is skipped (the channel still decays).
  bool moveBlocked = false;
  // FUN_00412ef0's net conveyor/surface displacement contribution —
  // already multiplied by DAT_0049b6f4 inside the original helper
  // and only applied when groundContact is set. Modeled as the
  // helper's output, not its inputs (the contact-list scan is a
  // deferred surface system).
  float conveyorX = 0.0f, conveyorY = 0.0f, conveyorZ = 0.0f;
};

// Per-frame products of the integrator.
struct PlayerMotionOutput {
  // The displacement vec handed to FUN_004630d4 (player-local, pre-
  // collision). The collision/query/apply layer is deferred — the
  // caller owns applying or discarding this.
  float dispX = 0.0f, dispY = 0.0f, dispZ = 0.0f;
  // DAT_0054cb00 / DAT_0054cb08 — the movement event word as this
  // layer left it: 6/600 forward-or-back move, 5/500 strafe, 4/400
  // turn. Emit order is move -> strafe -> turn (later overwrites);
  // turn is suppressed by a move event or live strafe input. The
  // deferred jump/look/fire consumers may overwrite afterwards.
  int eventType = 0;
  int eventMag = 0;
  // DAT_0054cb04 — bank-roll event flag written by the bank drive;
  // while set the dispatcher-tail bank decay is suppressed.
  bool bankEvent = false;
  // DAT_00540cc4 — this frame's write of the "move input consumed"
  // flag (the deferred jump consumer reads it to pick the jump
  // event magnitude).
  bool moveConsumed = false;
  // The integrator's forward-motion flag (moveVel input > 0 while
  // not blocked) — gates the air-charge drain in postStep and the
  // deferred mantle scan.
  bool forwardIntent = false;
  // False when masterGate early-returned — the in-function post
  // rules (event cancel, air-charge drain) are skipped, matching
  // the original's immediate RET.
  bool ran = false;
};

// FUN_00465228's movement section, in original order: channel
// accel/decay -> conveyor -> displacement compose + event emit ->
// yaw update. Stops where the original calls FUN_004630d4 — the
// returned dispX/Y/Z is exactly what the collision seam received.
PlayerMotionOutput integratePlayerMotion(const GameplayInputFrame& f,
                                         const PlayerMotionEnvironment& env,
                                         PlayerMotionState& s);

// The post-collision rules, run after the caller resolves the
// displacement (the original performs them inside FUN_00465228
// right after FUN_004630d4, interleaved with the deferred slide/
// jump/look calls):
//   - if the move event (600) was emitted but the position did not
//     change, the event word is cleared (0x4655f3..0x4658ea).
//   - while forwardIntent, airCharge drains toward 20 at f0*1.75
//     (soft ceiling 60) — skipped when the integrator was gated.
// `positionChanged` is the collision result (the original compares
// the applied X/Y against the pre-call snapshot).
// Plus the FUN_00463608 shared tail: when no bank event fired this
// frame, bank decays toward 0 by clamp(|bank|*0.35, 0.05, 2.5)*f0 —
// that runs even when the integrator was master-gated.
void playerMotionPostStep(const PlayerMotionEnvironment& env,
                          bool positionChanged, PlayerMotionState& s,
                          PlayerMotionOutput& out);

} // namespace mdk

#endif // MDK_CORE_PLAYER_MOTION_H
