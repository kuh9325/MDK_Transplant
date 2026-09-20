// Phase 5L — the sniper-aim mode: FUN_00464624 core, FUN_00464b50 zoom
// tail, FUN_00461878 reset, the FUN_00436100 scope-phase head and the
// FUN_00465228 entry gate — plus the shared FUN_00467a00 drain.
//
// EVIDENCE (instruction-level, MDK95.EXE BUILD_A):
//
//   FUN_00464624 — the sniper per-frame core, dispatched when
//     c9c != 0 && ca0 != 0 (the mounted class dispatch at e6c != 0
//     outranks it). In original order it:
//       1. clears 0x540c74 and calls FUN_00467180 DIRECTLY — gravity +
//          vertical collision, NO jump machine (the jump-sustain fields
//          keep their stale values).
//       2. reads the last contact (e4c): picks a surface multiplier
//          pair (no contact -> 0.75/0.75; contact without poly+0x20&4
//          -> 1.0/1.0; contact on a low-friction poly -> 0.5/0.1) and
//          runs the lateral channel d50 (semantic strafe through
//          accelChannel, else decelChannel) feeding a swept move
//          FUN_004630d4(d50*f0*sin(yaw), -d50*f0*cos(yaw), 0, 0.75).
//       3. abort check: c6c != 0 && !(c54 & 1) && (c78 < -30 || c78 > 0)
//          -> FUN_00461878. The -30..0 falling band does NOT abort.
//       4. semantic aim (N-1 merged frame): yawNorm -> d4c (yaw),
//          moveNorm -> d48 (pitch), strafeNorm -> d50 — each through
//          directChannel (NOT frame-scaled) with the ECX strafe flag
//          suppressing the yaw channel + raw mouse when set.
//       5. raw mouse aim (CURRENT frame, zero latency): only when no
//          semantic channel fired, mouseOn != 0 and the strafe flag is
//          clear — b54 += dy*0.12*f0*b58*0.41667 (clamped +-50),
//          c2c -= dx*0.12*f0*b58*0.41667 (wrapped 0..360). mouseYReversed
//          flips the dy sign.
//       6. non-semantic channels decay through decelChannel
//          (16/15 inside +-4.0, 0.8 outside).
//       7. semantic channels apply: b54 += d48*f0*b58*0.41667 (+-50),
//          c2c -= d4c*f0*b58*0.41667 (wrapped).
//       8. ca0 == 0 -> RET (post-abort guard): the unscope/fire/zoom
//          tail is gated on ca0 != 0.
//       9. manual unscope: sniperPulse (ce76c) != 0 OR the contact poly
//          matches a dying-surface record (FUN_0041342c) -> the 0x464986
//          path: c9c=0, cb08=0x384/cb00=9, e74 overlay latch, b54 restored
//          to the arena scalar, b58=2.4, d58/d54/d50/d4c/d48=0,
//          db4=8.0, db8=4.5, d34=-101. ca0 is LEFT (not cleared).
//      10. fire gate: ce770 != 0 && d0c >= 5 && 54161b == 0 ->
//          FUN_0045f138 (projectile spawn seam).
//      11. zoom tail: FUN_00464b50.
//
//   FUN_00464b50 — scoped zoom update: d54 (zoom channel) is fed by
//     zoomVel (ce6f0) through accelChannel else linearDecay(0.015).
//     d54 < 0 -> b58 /= (1 - d54) floored at min(focusFloor, 0.25);
//     d54 > 0 -> b58 *= (1 + d54); universal ceiling b58 >= 1.0 -> 1.0.
//
//   FUN_00461878 — abort/reset: clears c9c and ca0, the reticle/render
//     latch, restores b54 to the arena scalar + b58=2.4 + db8=4.5,
//     clears the aim channels + zoom, ends at cac=0x64.
//
//   FUN_00436100 head — scope phase advances one step per frame:
//     ca0==2 -> commit (FUN_00416700) + e74=0 + ca0=3;
//     ca0==1 -> overlay request (b54=0, d58=0, 5414bc=1, ccc..cd4=0,
//     e94=e98=1.0, FUN_00402388) + ca0=2.
//
//   FUN_00465228 tail — sniper entry: itemUse (ce774) is checked first
//     (item seam), then sniperPulse (ce76c) && cbc < 8 && cb00 < 8 with
//     eligibility (c6c == 0 -> free; else vertVel == +-0 && grounded &&
//     !dyingSurface) -> c9c=1, ca0=0, d48..d54=0, cb08=0x323, cb00=8.
//     A second pulse while scoped toggles off through the same write.
//
//   FUN_00467a00 — the difficulty-scaled damage/energy drain shared by
//     the hard-land path and the mounted-reticle drain:
//       scaled = easy ? max(1, 2a/3) : normal ? a : hard ? 2a
//       dac += 25*scaled clamped to [75,180]... OBSERVED: 180 ceiling
//       541554 -= scaled floored at 0
//       d5c += scaled
//
//   FUN_0045f138 — the fire seam. OBSERVED writes: 54161b += 1.0
//     (3.0 at burst end), 54161a -= 1, 541633 -= 1; d0c = 0 only on the
//     NON-sniper branch. The projectile spawn is a counted seam.
//
// The mounted reticle (FUN_004691c4) is a SEPARATE path — see
// player_reticle.h. It shares only the channel globals and FUN_00467a00.

#ifndef MDK_CORE_PLAYER_SNIPER_H
#define MDK_CORE_PLAYER_SNIPER_H

#include "core/gameplay_input.h"
#include "core/player_vertical.h"

namespace mdk {

struct TraversalRuntime;
struct RawGameplayInput;

// OBSERVED sniper constants (0x498xxx doubles, BUILD_A).
inline constexpr float kSniperLateralCapK = 0.25f;      // 0x498848
inline constexpr double kSniperLateralDecelIn = 0.08888888889; // 0x498840
inline constexpr double kSniperLateralDecelOut = 0.1777777778; // 0x498838
inline constexpr float kSniperLateralBound = 0.6666667f;       // 0x3f2aaaab
inline constexpr float kSniperAbortFall = -30.0f;              // 0x498850
inline constexpr double kSniperMouseK = 0.12;                  // 0x498858
inline constexpr double kSniperAimK = 0.4166666667;            // 0x498860
inline constexpr float kSniperPitchClamp = 50.0f;              // +-0x42480000
inline constexpr float kSniperYawWrap = 360.0f;                // 0x43b40000
inline constexpr float kSniperDecelIn = 1.0666667f;            // 0x3f888889 (16/15)
inline constexpr float kSniperDecelOut = 0.8f;                 // 0x3fcccccd
inline constexpr float kSniperDecelBound = 4.0f;               // 0x40800000

// Surface multiplier pairs ([EBP-0x18] rate / [EBP-0x1c] decel scale).
struct SniperSurfaceMul {
  float rate = 0.75f;   // accel/decel rate multiplier
  float decay = 0.75f;  // decel rate multiplier
};

// FUN_00436100 head — advance the scope phase one step. Called at the
// TOP of the frame (before the dispatch), once per phase per frame.
void sniperScopePhaseAdvance(TraversalRuntime& rt);

// The lateral surface-multiplier pair from the last contact (the
// ESI->+0x20&4 low-friction test; 0x540e4c token + poly pointer).
SniperSurfaceMul sniperSurfaceMul(const CollisionPoly* contactPoly);

// FUN_0041342c — the dying-surface match: true when the current arena's
// +0x45e record list holds a surface record (kind -1, rate > 0) whose
// surfType equals the contact poly's +0x23 surface byte. Shared by the
// sniper unscope trigger and the entry eligibility gate.
bool sniperDyingSurface(const TraversalRuntime& rt,
                        const CollisionPoly* contactPoly);

// FUN_00464624 — the sniper per-frame core. `ctrl` is the merged N-1
// semantic frame (prevFrame); `raw`/`bindings` carry the CURRENT-frame
// mouse deltas + MouseOn/MouseYReversed gates (zero latency — the
// original reads the raw 0x54b644/0x54b648 accumulators); `vertEnv` is
// the shared vertical environment. Runs gravity, the lateral channel +
// swept move, the abort, semantic + raw aim, the channel decays, the
// yaw/pitch apply, then (ca0 != 0) the unscope / fire / zoom tail.
// `outVf`/`outAppliedZ`/`outMoved` (all optional) report the internal
// gravity frame, the applied Z and the lateral-sweep move so the
// dispatcher's out-diagnostics stay faithful on the sniper branch.
void sniperCoreUpdate(TraversalRuntime& rt, const RawGameplayInput& raw,
                      const GameplayInputBindings& bindings,
                      const GameplayInputFrame& ctrl,
                      const PlayerVerticalEnvironment& vertEnv, float f0,
                      PlayerVerticalFrame* outVf = nullptr,
                      float* outAppliedZ = nullptr,
                      bool* outMoved = nullptr);

// FUN_00464b50 — the scoped zoom tail (end of sniper processing).
void sniperZoomUpdate(TraversalRuntime& rt, float f0);

// FUN_00461878 — the abort/reset write list (clears c9c + ca0, restores
// the view scalar + camera fields, clears the aim channels, cac=0x64).
void sniperReset(TraversalRuntime& rt);

// FUN_00467a00 — the difficulty-scaled damage/energy drain. `amount`
// is the raw drain (mounted-reticle energy deficit or land damage).
void sniperDamageDrain(TraversalRuntime& rt, int amount);

// FUN_0045f138 — the sniper fire seam. Observable writes only
// (54161b/54161a/541633); the projectile spawn is a counted seam.
void sniperFireSeam(TraversalRuntime& rt);

// FUN_00461954 subset — the animation/state machine's sniper-lifecycle
// states (0x323 scope-in, 0x384 unscope). The full machine (frame-table
// pointers 0x54cb14/0x54cb18 and the movement/idle handlers) is
// deferred; the states the sniper depends on are reproduced.
//
// OBSERVED gates (in order):
//   FUN_00431300 — the player anim job runs only when
//     (!c9c || ca0==0) && (!e6c || !(e70 & 0x20)).
//   FUN_00461954 head — !(0x4999d0 && 0x541548) or it skips to the
//     HUD tail (the cb0 latch is skipped too).
// The cb0 = cac first-frame latch runs for every dispatched state,
// handled or not. `frameStep` is DAT_0049b6e8 (the cb4 increment).
void playerAnimAdvance(TraversalRuntime& rt, int frameStep);

} // namespace mdk

#endif // MDK_CORE_PLAYER_SNIPER_H
