// Level-3 scripted sphere ride — see player_sphere.h for the full
// evidence map.

#include "core/player_sphere.h"

#include <cmath>

#include "core/frontend_machines.h"
#include "core/motion_channels.h"
#include "core/player_reticle.h"
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

// HYPOTHESIS — the ladder is observed at x0 (entry), x22 (~19 s in)
// and x50+ (~35 s in). 75 covers the observed maximum; 0.7 s/unit
// lands x22 at ~15.4 s and x50 at ~35 s, inside the observed window.
// The original cap/rate is UNKNOWN.
constexpr int kSphereAmmoCap = 75;
constexpr float kSphereRechargeSec = 0.7f;

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

// The class-4 mount dword — CORROBORATED: the XE/X_STRIKE object
// entry selects the same 0x40031 and the reticle/fire machinery is
// the shared FUN_004691c4 path.
constexpr std::uint32_t kSphereMountClass = 0x40031u;

void sphereSeqEnter(TraversalRuntime& rt) {
  rt.spherePhase = 2;
  rt.sphereTimer = 0.0f;
  rt.sphereDist = 0.0f;
  rt.mountClass = kSphereMountClass;
  rt.eventMag = 0;
  rt.eventType = 0;
  rt.reticleAux = 0;
  rt.motion.moveVel = 300.0f;     // reticle X — class-4 entry center
  rt.motion.strafeVel = 180.0f;   // reticle Y
  rt.motion.zoomChannel = 0.0f;
  rt.motion.turnVel = 0.0f;
  rt.bombs = 0;                   // OBSERVED: ladder reads "x 0" at
                                  // entry (SPHERE_ENTRY.md section 2)
  rt.bombRecharge = kSphereRechargeSec;
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

bool playerSphereStep(TraversalRuntime& rt,
                      const RawGameplayInput& raw,
                      const GameplayInputBindings& bindings,
                      const GameplayInputFrame& ctrl,
                      const FrontendTimingState& timing) {
  if (rt.spherePhase == 1) {
    rt.sphereTimer += timing.deltaSec;
    if (rt.sphereTimer < kSphereEntryDelaySec)
      return false;
    sphereSeqEnter(rt);
  }
  if (rt.spherePhase != 2) return false;

  const float dt = timing.deltaSec;
  const float f0 = timing.smoothed;
  const float total = spherePathLength();
  const float speed = total / kSphereRideSeconds;

  // Scripted path advance — uniform speed over the observed window;
  // player pos follows the pod (the class-4 pin semantic).
  rt.sphereDist += speed * dt;
  float p[3];
  spherePathPoint(rt.sphereDist, p);
  rt.cs.pos[0] = p[0];
  rt.cs.pos[1] = p[1];
  rt.cs.pos[2] = p[2];

  // Reticle channels — FUN_004691c4's channel block (semantic channels
  // through accelChannel; the "/3" raw-mouse fallback when no semantic
  // channel is live; inactive channels decay; integrate + pixel clamp).
  int dl = 0;
  if (ctrl.yawThird != 0.0f) {
    accelChannel(rt.motion.turnVel, ctrl.yawThird, ctrl.yaw10, f0);
    dl |= 4;
  }
  if (ctrl.moveThird != 0.0f) {
    accelChannel(rt.motion.zoomChannel, ctrl.moveThird, ctrl.move10,
                 f0);
    dl |= 8;
  }
  if (dl == 0 && (raw.mouseDx != 0 || raw.mouseDy != 0) &&
      bindings.mouseOn) {
    const float mk = static_cast<float>(kReticleMouseK);
    rt.motion.moveVel += static_cast<float>(raw.mouseDx) * mk;
    rt.motion.strafeVel += static_cast<float>(raw.mouseDy) * mk;
    rt.motion.turnVel = 0.0f;
    rt.motion.zoomChannel = 0.0f;
  }
  if ((dl & 4) == 0) linearDecay(rt.motion.turnVel, kReticleDecay, f0);
  if ((dl & 8) == 0)
    linearDecay(rt.motion.zoomChannel, kReticleDecay, f0);
  rt.motion.moveVel += rt.motion.turnVel * f0;
  if (rt.motion.moveVel < kReticleXMin)
    rt.motion.moveVel = kReticleXMin;
  if (rt.motion.moveVel > kReticleXMax)
    rt.motion.moveVel = kReticleXMax;
  rt.motion.strafeVel += rt.motion.zoomChannel * f0;
  if (rt.motion.strafeVel < kReticleYMin)
    rt.motion.strafeVel = kReticleYMin;
  if (rt.motion.strafeVel > kReticleYMax)
    rt.motion.strafeVel = kReticleYMax;

  // Fire latch — the shared 0x540d0c semi-auto counter (re-arms to
  // 999 on release; the armed frame + ammo > 0 spawns the projectile
  // — counted seam — and drains by frameStep while held).
  if (ctrl.fire == 0) {
    rt.fieldD0c = kReticleLatchArmed;
  } else {
    if (rt.fieldD0c == kReticleLatchArmed && rt.bombs > 0) {
      rt.bombs -= 1;
      ++rt.seams.reticleSpawnCalls;
    }
    rt.fieldD0c -= timing.frameStep;
  }

  // Ladder recharge — HYPOTHESIS rate/cap (see header); the class-4
  // object mount's 1 s/bomb cadence is the nearest OBSERVED analog.
  if (rt.bombs >= kSphereAmmoCap) {
    rt.bombRecharge = kSphereRechargeSec;
  } else {
    rt.bombRecharge -= dt;
    if (rt.bombRecharge <= 0.0f) {
      rt.bombs += 1;
      rt.bombRecharge = kSphereRechargeSec;
    }
  }

  if (rt.sphereDist >= total) {
    // Scripted dismount — OBSERVED: Kurt stands third-person on the
    // alcove pad; the pod is left behind. Single-frame placement like
    // the takeoff's handoff; the mount dword + channels clear so the
    // next frame's normal dispatch resumes on foot.
    rt.spherePhase = 0;
    rt.mountClass = 0;
    rt.motion.moveVel = 0.0f;
    rt.motion.strafeVel = 0.0f;
    rt.motion.turnVel = 0.0f;
    rt.motion.zoomChannel = 0.0f;
    rt.vert.vertVel = 0.0f;
    rt.vert.posX = rt.cs.pos[0];
    rt.vert.posY = rt.cs.pos[1];
    rt.vert.posZ = rt.cs.pos[2];
    for (int i = 0; i < 3; ++i) rt.cs.entryPos[i] = rt.cs.pos[i];
  }
  return true;
}

} // namespace mdk
