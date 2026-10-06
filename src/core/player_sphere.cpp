// Level-3 scripted sphere ride — see player_sphere.h for the full
// evidence map.

#include "core/player_sphere.h"

#include <cmath>

#include "core/frontend_machines.h"
#include "core/player_sniper.h"
#include "core/traversal_runtime.h"

namespace mdk {
namespace {

// OBSERVED (SPHERE_ENTRY.md): ~0.7 s of on-foot idle precedes the
// masked view; the mask wipe itself is a ~0.1-0.2 s presentation
// detail folded into the entry frame.
constexpr float kSphereEntryDelaySec = 0.7f;

// OBSERVED: the masked window spans ~344.6 -> ~380.3 s of the
// canonical AVI (~35.7 s); the path below is traversed at uniform
// speed over this window.
constexpr float kSphereRideSeconds = 35.6f;

// The ride path. Endpoints are evidence-bound; interior waypoints are
// HYPOTHESIS (straight-line segments — the original's spline/shape is
// UNKNOWN).
constexpr float kSpherePath[][3] = {
    // dock: the spawn pad — the masked view keeps the same tower
    // dead ahead, so the pod engages at Kurt's own position.
    {-4.0f, 0.0f, 190.0f},
    // canyon descent alongside the tower face (terrain scrolls).
    {0.0f, 320.0f, 140.0f},
    // door 117-1 approach — the door record sits at (0, 632, 104).
    {0.0f, 600.0f, 110.0f},
    // dismount pad inside the walled alcove (HMO_1 floor z ~104
    // verified by port probe at y = 650).
    {0.0f, 650.0f, 104.0f},
};
constexpr int kSpherePathCount =
    sizeof(kSpherePath) / sizeof(kSpherePath[0]);

float spherePathLength() {
  float total = 0.0f;
  for (int i = 0; i + 1 < kSpherePathCount; ++i) {
    const float dx = kSpherePath[i + 1][0] - kSpherePath[i][0];
    const float dy = kSpherePath[i + 1][1] - kSpherePath[i][1];
    const float dz = kSpherePath[i + 1][2] - kSpherePath[i][2];
    total += std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  return total;
}

void spherePathPoint(float dist, float out[3]) {
  for (int i = 0; i + 1 < kSpherePathCount; ++i) {
    const float dx = kSpherePath[i + 1][0] - kSpherePath[i][0];
    const float dy = kSpherePath[i + 1][1] - kSpherePath[i][1];
    const float dz = kSpherePath[i + 1][2] - kSpherePath[i][2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= len) {
      const float t = len > 0.0f ? dist / len : 0.0f;
      out[0] = kSpherePath[i][0] + dx * t;
      out[1] = kSpherePath[i][1] + dy * t;
      out[2] = kSpherePath[i][2] + dz * t;
      return;
    }
    dist -= len;
  }
  out[0] = kSpherePath[kSpherePathCount - 1][0];
  out[1] = kSpherePath[kSpherePathCount - 1][1];
  out[2] = kSpherePath[kSpherePathCount - 1][2];
}

// Scripted-ride entry — the masked ride is the sniper scope running on
// the scripted path. OBSERVED (BUILD_A + canonical AVI): the sequence
// does NOT set the mount dword (0x540e70's only writers are the
// standing-mount paths) — the masked view + HUD are the scope overlay.
// Evidence: the cockpit mask + crosshair + "x N" readout; "x N" is the
// 0x417f47 zoom-charge percentage ((1-0x540b58)^2 * 1.0519395 * 100),
// drawn when 0x540ca0 > 1 && traversal mode — NOT ammunition. The ride
// engages the scope (0x540c9c = 1); FUN_00436100's per-frame advance
// walks 0x540ca0 1 -> 3 over the entry window, matching the observed
// wipe-in while Kurt still stands on the pad.
void sphereSeqEnter(TraversalRuntime& rt) {
  rt.spherePhase = 2;
  rt.sphereTimer = 0.0f;
  rt.sphereDist = 0.0f;
  rt.mountClass = 0;            // 0x540e70 stays 0 — no mount object
  rt.eventMag = 0;
  rt.eventType = 0;
  rt.flagC9c = 1;               // 0x540c9c — scope engaged by sequence
  rt.transitionPhase = 1;       // 0x540ca0 — advances 1 -> 3
  // 0x540b58 is not written here — the zoom tail's >= 1.0 ceiling
  // snaps the unscoped 2.4 default down to 1.0 (the observed "x 0").
}

} // namespace

void playerSphereArm(TraversalRuntime& rt, int spawnArena) {
  if (rt.field541498 != 3 || spawnArena < 0 ||
      static_cast<std::size_t>(spawnArena) >= rt.arenas.size() ||
      rt.arenas[static_cast<std::size_t>(spawnArena)]->name != "HMO_1")
    return;
  rt.spherePhase = 1;
  rt.sphereTimer = 0.0f;
}

void playerSphereStep(TraversalRuntime& rt,
                      const RawGameplayInput& /*raw*/,
                      const GameplayInputBindings& /*bindings*/,
                      const GameplayInputFrame& /*ctrl*/,
                      const FrontendTimingState& timing) {
  if (rt.spherePhase == 1) {
    rt.sphereTimer += timing.deltaSec;
    if (rt.sphereTimer < kSphereEntryDelaySec)
      return;
    sphereSeqEnter(rt);
  }
  if (rt.spherePhase != 2) return;

  // Scripted path advance — uniform speed over the observed window.
  // The pod pos is written BEFORE the dispatch so the sniper branch's
  // gravity/aim/fire see this frame's pod position; the post-step
  // re-asserts it after the update (the sequence owns the position).
  const float total = spherePathLength();
  const float speed = total / kSphereRideSeconds;
  rt.sphereDist += speed * timing.deltaSec;
  if (rt.sphereDist > total) rt.sphereDist = total;
  float p[3];
  spherePathPoint(rt.sphereDist, p);
  rt.cs.pos[0] = p[0];
  rt.cs.pos[1] = p[1];
  rt.cs.pos[2] = p[2];

  // Pod footing — the scripted pod is solid footing: pin the contact
  // flag + vertical velocity so sniperCoreUpdate's abort check
  // (!grounded && vel outside -30..0) never trips mid-ride.
  rt.vert.contactFlags |= 1;
  rt.vert.vertVel = 0.0f;
}

void playerSpherePostStep(TraversalRuntime& rt) {
  if (rt.spherePhase != 2) return;

  // The sequence owns the player position — re-assert the path point
  // over whatever the sniper update wrote (gravity/lateral writes are
  // superseded), and keep the pod-footing pins for the next frame's
  // abort check.
  float p[3];
  spherePathPoint(rt.sphereDist, p);
  rt.cs.pos[0] = p[0];
  rt.cs.pos[1] = p[1];
  rt.cs.pos[2] = p[2];
  rt.vert.posX = p[0];
  rt.vert.posY = p[1];
  rt.vert.posZ = p[2];
  rt.vert.vertVel = 0.0f;
  rt.vert.contactFlags |= 1;

  if (rt.sphereDist >= spherePathLength()) {
    // Scripted dismount — OBSERVED: Kurt stands on the alcove pad in
    // normal third-person traversal; the scope releases. FUN_00461878
    // is the evidenced unscope write list (c9c/ca0 clear, b58 = 2.4,
    // view scalar restored, cac = 0x64).
    rt.spherePhase = 0;
    sniperReset(rt);
    rt.vert.vertVel = 0.0f;
    for (int i = 0; i < 3; ++i) rt.cs.entryPos[i] = rt.cs.pos[i];
  }
}

} // namespace mdk
