// Phase 5C — jump sustain and vertical gravity.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md §"Phase 5C" for the full
// evidence chain):
//
//   Per frame, inside FUN_00465228's tail (after the horizontal
//   FUN_004630d4 call and the deferred slide helper FUN_0046603c):
//
//     FUN_00466740(slideVec)          jump-state machine
//       c88 clear when cac leaves 0x2be/0x2bf
//       c84 air-charge update (seed/accumulate/reset)
//       c88 == 0 -> jump-init gate   |   c88 != 0 -> hold/release
//       c80 sustain rewrite
//       slope assist (in_EAX vector — UNKNOWN provenance)
//       FUN_00467180()               vertical gravity + collision
//       FUN_00466aec()               mantle (deferred boundary)
//       e28 = 0
//
//   DAT_00540e24 != 0 (slide mode) skips everything except
//   FUN_00467180 — and skips the e28 clear + mantle call too.
//
//   FUN_00467180:
//     c6c == 0 -> RET            (vertical master gate)
//     c7c != 0 -> RET            (mantle/vertical-skip state)
//     c78 > 0 && e24 == 0 -> frameStep-substep gravity loop
//     otherwise             -> single f4 gravity step
//     ribbon-volume check (FUN_00412e94) -> forced sustain +
//         c84 drain (bit-pattern floor 1.0f) + disp recompute
//     rise cap: cac < 800 && e6c == 0 && c78 > 40 -> c78 = 40
//     pre-land clamp (c78 <= 0 && c54&2 && pos+disp <= c58)
//     c54 &= ~1 -> FUN_004630d4(ctx, mode, 0, 0, dispZ, 0.5, 0, &e50)
//     -> contact (landing/ceiling) / no-contact (realized velocity)
//     -> deep-floor failsafe (posZ <= player+0x44e - 50)
//
//   The layer STOPS at the semantic FUN_004630d4 seam: the native
//   code produces the requested displacement and consumes only the
//   facts the original reads back (contact, applied position,
//   normal, floor probe, blocker objects). Triangle tests, arena
//   traversal, penetration resolution and the apply itself are
//   deferred collision internals.
//
// NATIVE PORT DECISIONS:
//   - PlayerMotionState::airCharge IS the original's 0x540c84 (the
//     5B drain lives in playerMotionPostStep); the vertical API takes
//     it as an in/out param so the global stays single-sourced.
//   - The event word (0x54cb00/0x54cb08) is shared state: this layer
//     reads the current type for the jump gate and overwrites it.
//   - Collision feedback is the smallest semantic seam the original
//     caller logic consumes — nothing else is modeled.
//
#ifndef MDK_CORE_PLAYER_VERTICAL_H
#define MDK_CORE_PLAYER_VERTICAL_H

#include "core/collision_query.h"
#include "core/player_motion.h"

namespace mdk {

// Persistent vertical state — the globals FUN_00466740/FUN_00467180
// carry across frames. airCharge (0x540c84) lives in
// PlayerMotionState and is passed in separately.
struct PlayerVerticalState {
  // DAT_00540c78 — vertical velocity, units of displacement per
  // delta-second (positive = up). Writers: jump impulse (40.0f),
  // release cut, slope assist, gravity, collision-result paths.
  float vertVel = 0.0f;
  // DAT_00540c7c — vertical skip gate; FUN_00466aec sets 2 on a
  // successful mantle (deferred writer), FUN_00461954 also writes.
  // While nonzero FUN_00467180 returns before any work.
  int vertSkip = 0;
  // DAT_00540c80 — jump-sustain flag. Cleared every frame in
  // FUN_00466740 then conditionally rewritten; also forced inside
  // ribbon volumes by FUN_00467180. Feeds 5A's moveBoostGate and the
  // sustain-gravity branch.
  std::uint32_t jumpSustain = 0;
  // DAT_00540c88 — jump-in-progress flag. Set on jump initiation;
  // cleared when the dispatched state (cac) leaves 0x2be/0x2bf.
  int jumpActive = 0;
  // DAT_00540c8c — jump hold charge: initialized to 6, drains by
  // frameStep per held frame; on release the remainder scales the
  // upward-velocity cut (c78 -= c8c * 20/6).
  int jumpHoldCharge = 0;
  // DAT_00540c90 — the original's jump edge latch. Set on jump start
  // and on airborne release; re-arms to 0 while grounded+stopped
  // with the jump flag low. Also read/written by FUN_00467ed0 and
  // FUN_00461954 (sibling systems).
  int jumpLatch = 0;
  // DAT_00540c98 — jump aux flag. Written by FUN_00466740 only (set
  // on start, cleared on release/sustain-end); no observed readers.
  int jumpAux = 0;
  // DAT_00540bfc/0x540c00/0x540c04 — player position. Written by the
  // collision apply (outside this layer); read here for the pre-land
  // clamp, anti-jitter restore, realized-velocity and failsafe.
  float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
  // DAT_00540c54 — contact flag byte. bit0 = grounded (cleared
  // before the call, set on landing), bit1 = floor-probe valid
  // (refreshed by the collision seam).
  std::uint8_t contactFlags = 0;
  // DAT_00540c58 — current floor height from the collision-internal
  // probe (FUN_00435eec). Refreshed through the seam.
  float floorZ = 0.0f;
  // DAT_00540c60/0x540c64 — probe blocker objects (opaque tokens).
  std::uint32_t blocker0 = 0, blocker1 = 0;
  // DAT_00540dc0/0x540dc4/0x540dc8 — movement blockers consumed by
  // the horizontal layer (5B's env.moveBlocked). Written only on the
  // pre-land-no-contact landing path.
  std::uint32_t moveBlocker0 = 0, moveBlocker1 = 0;
  int moveBlockerFlag = 0;
  // DAT_00540e4c — last collision EAX (contact object, opaque token;
  // 0 = no contact). Feeds 5B's env.groundContact and the surface
  // helper FUN_00412ef0.
  std::uint32_t contactObj = 0;
  // DAT_00540e50..0x540e58 — last contact normal out-param.
  float contactNormal[3] = {0.0f, 0.0f, 0.0f};
  // DAT_00540cbc — event-channel idle counter. Jump requires < 7;
  // reset to 0 when it was 7 and a sustain/landing event fires.
  int eventIdle = 0;
  // DAT_00540e28 — bounce flag, written by collision internals /
  // sibling systems. Suppresses the sustain-end c90 write and the
  // hard-landing event; cleared at the end of FUN_00466740.
  int bounceFlag = 0;
  // DAT_00541554 — fall-out counter; zeroed by the deep-floor
  // failsafe (its readers are the deferred FUN_00467a00/FUN_0046771c
  // systems).
  int fallCounter = 0;
  // DAT_00540d5c — landing accumulator: zeroed on the hard-landing
  // event path; the deferred FUN_00467a00 then adds scaled damage.
  float landingAccum = 0.0f;
};

// Per-frame inputs the vertical layer reads from outside itself.
struct PlayerVerticalEnvironment {
  // DAT_0049b6f0 — smoothed frame-units factor (~1.0 at 30 Hz). The
  // c84 accumulator and in-volume drain are f0-scaled.
  float smoothed = 1.0f;
  // DAT_0049b6f4 — delta-seconds constant (f32 0.033333335). Used by
  // the single-step gravity path, displacement, anti-jitter and the
  // realized-velocity recompute. NOT interchangeable with `smoothed`.
  float deltaSeconds = 1.0f / 30.0f;
  // DAT_0049b6e8 — integer frame step (1..4). Counts the rise-loop
  // substeps and drains jumpHoldCharge. The fall path always runs
  // exactly one step — OBSERVED asymmetry.
  int frameStep = 1;
  // DAT_004ce768 — the merged jump control flag. Consumed through
  // the same previous-frame control block as horizontal movement
  // (FUN_00466740 runs before FUN_00406f14 — one-frame latency).
  bool jumpHeld = false;
  // DAT_00540cc4 — this frame's "move input consumed" flag written
  // by the horizontal layer; selects the moving-jump event 0x2bf.
  bool moveConsumed = false;
  // DAT_00540cac — dispatched player state code. Gates the c88
  // clear, the c84 accumulate-when-rising exception (0x2bd) and the
  // rise cap (< 800).
  int locoState = 0;
  // DAT_0054cb00 — the current event word type as earlier writers
  // left it this frame. Jump initiation requires < 7.
  int eventWordType = 0;
  // DAT_00540c6c — vertical master enable (0 -> FUN_00467180 RETs).
  bool vertEnable = true;
  // DAT_00540e24 — slide mode. FUN_00466740 early-outs to the
  // integrator (no jump machine, no mantle, no e28 clear).
  bool slideMode = false;
  // DAT_00540e6c — shared world-state flag: suppresses the rise cap
  // and (with flagE72bit1) the hard-landing event.
  bool sharedGateE6C = false;
  // DAT_00540e72 & 2 — silences the hard-landing event (landing
  // still happens, without anti-jitter).
  bool flagE72bit1 = false;
  // DAT_00540ca4 != 0 — a carrier/attached object exists; its volume
  // list is also queried when carrierCheckGate is clear.
  bool carrierObj = false;
  // DAT_00540d3c != 0 — suppresses the carrier volume check.
  bool carrierCheckGate = false;
  // FUN_00412e94(player, 1, &pos, &vec) — position inside a
  // registered ribbon/updraft volume (deferred volume system
  // result). OBSERVED: the query takes a {0,0,c78} out-vector and
  // its z is copied back into c78 (0x467276-0x46727f), so a hit can
  // also rewrite the vertical velocity — `ribbonVelZ` models that
  // out-param (nullptr = the query left z untouched). The same
  // buffer serves the carrier query.
  bool insideRibbonVolume = false;
  // FUN_00412e94(carrier, 1, &pos, &vec) — same query on the
  // carrier object (checked only when carrierObj && !carrierCheckGate).
  bool carrierInsideRibbonVolume = false;
  const float* ribbonVelZ = nullptr;
  // FUN_00466740's in_EAX — the slope-assist XY vector. In the
  // normal path this is FUN_0046603c's implicit EAX leftover
  // (UNKNOWN provenance — the dispatcher's cac>=800 path passes 0
  // explicitly). Modeled nullable; nullptr skips the assist.
  const float* slideVec = nullptr;
  // current arena (c48) +0x44e — the deep-floor reference height.
  // The failsafe fires when posZ <= deepFloorZ - 50. OBSERVED: the
  // 0x466-stride arena array is memset to 0 at creation
  // (FUN_00433d40: call 0x41c884 alloc -> call 0x47d20a memset) and
  // no code path ever writes +0x44e, so it is 0 for every arena —
  // a flat -50 catch worldwide. The same field is read for objects
  // through obj+0x60 (arena) at 0x4583ab/0x45bdd6/0x45fd55 with
  // -200/-150 respawn constants.
  float deepFloorZ = -1.0e30f;
};

// What FUN_00467180 hands to FUN_004630d4 plus the event/jump state
// the layer produced before the call, then the post-collision
// outcome. The remaining call args are fixed: dx=0, dy=0,
// radius=0.5f, aux=0, normalOut=&e50 (the ctx/mode register pair is
// opaque collision context not consumed by this layer).
struct PlayerVerticalFrame {
  // --- request (arg5 displacement, post pre-land clamp) ---
  float dispZ = 0.0f;
  bool collisionIssued = false;   // false when c6c/c7c gated the call
  bool preLand = false;           // the floor-epsilon clamp engaged
  bool inRibbonVolume = false;    // FUN_00412e94 returned nonzero
  // --- event word as this layer left it (0x54cb00/0x54cb08) ---
  int eventType = 0, eventMag = 0;
  bool jumped = false;            // jump initiated this frame
  bool sustain = false;           // c80 after this frame's rewrite
  // --- outcome (filled by applyPlayerVerticalCollision) ---
  bool landed = false;            // the landing transition ran
  bool ceilingHit = false;        // upward contact -> velocity zeroed
  bool hardLanding = false;       // event 806 + deferred damage call
  bool blockerRefresh = false;    // moveBlocker0/1 copied from probe
  bool blockerReleased = false;   // FUN_00461878(0) would run (deferred)
  bool realizedVelocity = false;  // vertVel = applied delta / f4
  bool deepFloorReset = false;    // failsafe fired
};

// Semantic FUN_004630d4 result — only the facts the vertical state
// machine reads back. Position is the full applied vector because
// the anti-jitter check compares all three axes.
struct VerticalCollisionResult {
  // EAX — contact object token (0 = no contact).
  std::uint32_t contactObj = 0;
  // Applied position after the move (0x540bfc..0x540c04).
  float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
  // Contact normal -> 0x540e50..0x540e58 (only meaningful on contact).
  float normalX = 0.0f, normalY = 0.0f, normalZ = 0.0f;
  // Floor-probe refresh (FUN_00435eec inside the call): bit1 of
  // 0x540c54, the floor height 0x540c58, and the blocker objects
  // 0x540c60/0x540c64 with blocker0's +0x14a bit-7 flag.
  bool hasFloor = false;
  float floorZ = 0.0f;
  std::uint32_t blocker0 = 0, blocker1 = 0;
  bool blocker0Flag80 = false;
  // Collision internals may set DAT_00540e28 (trampoline/bounce) —
  // OR'd into state before the landing evaluation.
  bool bounce = false;
};

// FUN_00466740's jump-state machine followed by FUN_00467180's
// pre-collision half (gravity integration -> volume check -> rise
// cap -> pre-land clamp). `ms` is used only for the shared 0x540c84
// airCharge. Returns the collision request + events; the caller
// then supplies the semantic collision result.
PlayerVerticalFrame integratePlayerVertical(
    const PlayerVerticalEnvironment& env, PlayerMotionState& ms,
    PlayerVerticalState& vs);

// FUN_00467180 alone — the vertical gravity + collision request with
// NO jump machine. The normal path reaches it through
// integratePlayerVertical (jump machine first); the sniper path
// (FUN_00464624) calls it directly (OBSERVED 0x464637). `frame`
// accumulates any events the caller seeded (empty for the sniper
// path); returns the FUN_004630d4 request.
PlayerVerticalFrame integratePlayerGravity(
    const PlayerVerticalEnvironment& env, PlayerMotionState& ms,
    PlayerVerticalState& vs, PlayerVerticalFrame frame = {});

// FUN_00467180's post-collision half: applies the seam result and
// runs the landing/ceiling/no-contact/blocker/deep-floor rules in
// original order. Mutates `frame` (outcome fields + landing events).
void applyPlayerVerticalCollision(
    const PlayerVerticalEnvironment& env, PlayerMotionState& ms,
    PlayerVerticalState& vs, const VerticalCollisionResult& res,
    PlayerVerticalFrame& frame);

// Shared vertical-collision plumbing — collisionApply for the frame's
// dispZ (scale 0.5, outAux -> normal) then applyPlayerVerticalCollision
// on the semantic result. Mirrors what the original does inside
// FUN_004630d4's caller; used by both the normal path and the sniper
// path (FUN_00464624's embedded FUN_00467180 call). Updates cs.pos,
// vs.contactObj/posX..Z/contactNormal/contactFlags/floorZ/blockers and
// the frame outcome; returns the contact poly (nullptr = none) and
// writes *appliedDispZ when non-null.
const CollisionPoly* playerVerticalApplyCollision(
    const PlayerVerticalEnvironment& env, CollisionState& cs,
    PlayerMotionState& ms, PlayerVerticalState& vs,
    PlayerVerticalFrame& frame, float* appliedDispZ);

// FUN_00466740's tail: the deferred FUN_00466aec mantle boundary and
// the unconditional e28 clear — skipped entirely in slide mode.
void playerVerticalPostStep(const PlayerVerticalEnvironment& env,
                            PlayerVerticalState& vs);

} // namespace mdk

#endif // MDK_CORE_PLAYER_VERTICAL_H
