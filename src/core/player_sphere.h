// Level-3 scripted sphere ride — the "sphere segment" the canonical
// playthrough enters at the HMO_1 pad after the canyon ambush.
//
// CORRECTED EVIDENCE MAP (BUILD_A + canonical AVI):
//
//   The ride is a scripted positional drive PLUS the sniper scope —
//   NOT a mounted object. The original's own save lists contain no pod
//   object; the mount dword 0x540e70's only writers are the standing-
//   contact mount paths (0x463c57 / unmount clears) — never the
//   sequence. What the ride actually runs:
//
//   1. Sequence drives 0x540bfc (player pos) along a path at a
//      constant speed over the observed ~35.6 s window.
//   2. The scope engages at ride start: 0x540c9c = 1, 0x540ca0 walks
//      1 -> 3 via FUN_00436100 (mask wipe-in = the scope overlay
//      request + commit).
//   3. Per-frame the SNIPER dispatch branch runs: FUN_00464624 —
//      gravity (pod footing pins 0x540c54 bit0 so the abort never
//      trips), the semantic/raw aim channels (the recorded reticle
//      wandering IS the scoped yaw/pitch aim), then the tail:
//      manual unscope check, the fire gate, FUN_00464b50 zoom.
//   4. Fire: ce770 != 0 && 0x540d0c >= 5 && 0x54161b == 0 ->
//      FUN_0045f138 weapon dispatch. With 0x541618 == 5 (sniper
//      selected): gates on 0x540e14 + 0x54163b, decrements 0x541633
//      (ammo[5]), calls FUN_0045a4dc which spawns "X_STRIKE"
//      (0x498150) via FUN_00454794 — the same bolt as on-foot sniper
//      fire. X_STRIKE IS present in LEVEL3.CMI's class table; the
//      earlier XBN_BOMB hypothesis is DISPROVEN for L3 (absent from
//      the CMI; only present on L4/L7 where the standing-mounted
//      turret sequence uses it).
//   5. The "x N" ladder is NOT ammunition: 0x417f47 draws it when
//      0x540ca0 > 1 && mode == 3 as (1 - 0x540b58)^2 * 1.0519395 * 100
//      clamped to 100 — the scope zoom-charge percentage. The observed
//      x0 (zoom = 1.0 wide) -> x98 (zoom ~0.04, deep zoom-in) -> drop
//      on zoom-out -> x0 at dismount matches this contract.
//   6. Dismount: path end -> FUN_00461878 unscope write list +
//      on-foot fields; Kurt stands on the alcove pad.
//
//   Prior model (DISPROVEN): class-4 mount dword 0x40031 + 0x540ea0
//   bomb ladder + FUN_004691c4's XBN_BOMB lob. The class-4 counter is
//   capped at 10 (0x46952a jge) but the observed ladder reaches x98;
//   XBN_BOMB is absent from LEVEL3.CMI so its lookup would fail even
//   under a mount. The mount-specific XBN_BOMB fire path
//   (playerReticleFireBomb) is retained for real class-4 mounts
//   (other levels) but is never invoked by this ride.
//
// Integration: the sequence tick owns cs.pos; playerSphereStep is the
// PRE-dispatch call (path advance + pod-footing pin + return false so
// the sniper branch runs), playerSpherePostStep is the POST-dispatch
// call (pos re-assert + scripted dismount).

#ifndef MDK_CORE_PLAYER_SPHERE_H
#define MDK_CORE_PLAYER_SPHERE_H

#include <cstdint>

namespace mdk {

struct TraversalRuntime;
struct FrontendTimingState;
struct RawGameplayInput;
struct GameplayInputBindings;
struct GameplayInputFrame;

// Arm the sphere sequence — called when the Level-3 spawn script for
// HMO_1 fires. Gates: level index == 3 and the spawn arena is named
// "HMO_1" (the script's own identity; OBSERVED gating preserved).
void playerSphereArm(TraversalRuntime& rt, int spawnArena);

// Pre-dispatch ride step — the ~0.7 s entry window idles on foot,
// then sphereSeqEnter engages the scope. During the ride: advances
// the scripted path, writes the pod position, pins pod footing; the
// dispatcher's sniper branch then runs the frame (aim, fire gate ->
// FUN_0045f138 -> X_STRIKE, zoom). `timing` carries the frame-step
// family used by the ride cadence.
void playerSphereStep(TraversalRuntime& rt,
                      const RawGameplayInput& raw,
                      const GameplayInputBindings& bindings,
                      const GameplayInputFrame& ctrl,
                      const FrontendTimingState& timing);

// Post-dispatch ride step — re-asserts the scripted path position
// over the sniper update's writes and runs the scripted dismount
// (FUN_00461878 unscope + on-foot fields) when the path ends.
void playerSpherePostStep(TraversalRuntime& rt);

} // namespace mdk

#endif // MDK_CORE_PLAYER_SPHERE_H
