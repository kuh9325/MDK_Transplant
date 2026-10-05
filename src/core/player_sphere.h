// Level-3 scripted sphere ride — the ae0a0-mode vehicle segment.
//
// EVIDENCE MAP
//
//   OBSERVED (out/playthrough-reference/2026-10-05_canonical/
//   SPHERE_ENTRY.md, canonical AVI mdkdos_000.avi):
//     - ~0.7 s of on-foot idle at the HMO_1 spawn precedes the masked
//       view (343.8 -> 344.5 s); no locomotion, so the entry is
//       automatic/scripted — not input- or object-contact-driven.
//     - The mask wipes on (344.5) and is fully engaged at ~344.6 with
//       the same tower dead ahead — the ride starts from the spawn
//       pad's own view direction.
//     - The ammunition ladder reads "x 0" at entry and recharges
//       through the flight (x22 -> x50+ observed mid-ride).
//     - The masked window ends ~380.3 s (~35.7 s of flight) with a
//       scripted dismount: Kurt stands third-person on a docking pad
//       inside a walled alcove at a different world position.
//
//   CORROBORATED (MDK95.EXE BUILD_A analysis):
//     - The mechanism is the scripted-sequence state machine (ae0a0:
//       mode-2 transition -> mode-3 vehicle loop), armed by the
//       level-entry checkpoint reconcile — a fresh-entry path, not an
//       arena-object mount.
//     - No pod object exists in the original f10/f200 save object
//       lists (OBJECT ABSENT — OBSERVED), so the ride is modelled as a
//       scripted segment driving rt fields directly; it shares the
//       class-4 mount dword + reticle channel machinery rather than a
//       DynamicObject.
//     - The dismount alcove floor at z ~= 104 spans y ~615-650 in
//       HMO_1's own collision (port probe, DOCUMENTED).
//
//   HYPOTHESIS:
//     - Interior path waypoints (only the endpoints are
//       evidence-bound: dock = spawn pad; dismount pad = the walled
//       alcove inside the door-117-1 opening, ~ (0, 650, 104)).
//     - Ammo cap 75 and recharge ~1.43/s — bounded by the observed
//       x0 -> x22 -> x50+ ladder over the ~35.7 s window.
//     - The cockpit mask art is the pod's own geometry (X_STRIKB is a
//       30-entry table — likely model/anim data, UNKNOWN); the native
//       presents the ride as the scripted forward view plus the HUD
//       reticle/ladder until the pod model is decoded.
//
//   UNKNOWN:
//     - The exact original trigger field (the ae146 arm is the best
//       candidate found in MDK95.EXE); the port arms on fresh
//       level-3 entry only.
//     - Ladder cap, exact recharge rate, fire visuals, any damage
//       drain while riding.
//
#ifndef MDK_CORE_PLAYER_SPHERE_H
#define MDK_CORE_PLAYER_SPHERE_H

#include "core/gameplay_input.h"

namespace mdk {

struct TraversalRuntime;
struct RawGameplayInput;
struct FrontendTimingState;

// Arm the scripted segment at fresh level-3 entry — the original's
// analog is the level-entry checkpoint reconcile (CORROBORATED);
// diagnostic/save-suppressed loads are not the fresh-entry path and
// do not arm it. No-op unless rt.field541498 == 3 and the spawn arena
// is HMO_1.
void playerSphereArm(TraversalRuntime& rt, int spawnArena);

// Per-frame step at the traversal dispatch's scripted-branch slot
// (alongside fieldDa0's takeoff). Phase 1 is the on-foot entry window:
// the ~0.7 s delay elapses while the normal dispatch still runs and
// this returns false. At expiry the ride engages (phase 2) and this
// consumes the frame until the scripted dismount. `ctrl` is the
// merged N-1 semantic frame (prevFrame), matching the reticle
// convention.
bool playerSphereStep(TraversalRuntime& rt,
                      const RawGameplayInput& raw,
                      const GameplayInputBindings& bindings,
                      const GameplayInputFrame& ctrl,
                      const FrontendTimingState& timing);

} // namespace mdk

#endif // MDK_CORE_PLAYER_SPHERE_H
