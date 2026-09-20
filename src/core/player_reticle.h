// Phase 5L — the mounted reticle / bomb-sight (X_STRIKE / XE mounts):
// the FUN_00463608 mount-scan + class entries and the FUN_004691c4
// per-frame update. This is a SEPARATE path from the sniper mode —
// the executable never shows them sharing a state machine; they share
// only the channel globals (d48..d54) and the FUN_00467a00 drain.
//
// EVIDENCE (instruction-level, MDK95.EXE BUILD_A):
//
//   FUN_00463608 mounted branch — dispatched when e6c != 0 (it outranks
//     the sniper/normal paths). The e70 class dword's byte2 selects:
//       bit0 (0x01) -> FUN_00467ac4   (XD / XD2 — 0x10039)   SEAM
//       bit1 (0x02) -> FUN_00467ed0   (XSNOWB — 0x20002)     SEAM
//       bit2 (0x04) -> FUN_004691c4   (X_STRIKE / XE — 0x40031)
//       no match    -> "Unrecognised controlalien" log, then the
//                      object's +0x14b&~2 and e6c=0 (unmount).
//     Every path calls the FUN_00469cd0 weapon-slot seam afterward.
//
//   FUN_00463608 normal-path mount-scan — runs in the unscoped,
//     unmounted dispatch tail. When standing on (or last touching) an
//     object that is named && +0x14b&2 && !mounted && !c74, it latches
//     e6c = candidate, then runs the class-name entry:
//       XD / XD2  (0x10039): gate c74|c84 — on success e70=0x10039,
//         obj+0x14a |= 0x08, e64 = obj yaw, player pos = obj pos,
//         cb08/cb00 = 0, d48..d54 = 0.
//       XSNOWB    (0x20002): obj+0x148 dword |= 0x80800 then
//         &= ~0x80100, e70=0x20002, ride-dismount (FUN_00461878(0) +
//         dc8=0), e64 = obj yaw, cb08/cb00 = 0, d48..d54 = 0.
//       X_STRIKE / XE (0x40031): e70=0x40031, b740=1 (overhead cam),
//         b744/b748/b76c = 0, c2c = obj yaw, player pos = obj pos,
//         b74c = 50.0 (overheadHeight), d48 = 300, d4c = 180,
//         d50/d54 = 0, d08 = 0, ea0 = 10, ea4 = 1.0, cb08/cb00 = 0.
//       no match: "Unrecognised controlalien" log only (e6c left set
//         with e70 = 0 — the mounted dispatch unmounts it next frame).
//
//   FUN_004691c4 — the reticle per-frame core, dispatched while mounted
//     class 4. In original order it:
//       1. c2c = obj->+0x4c (player yaw pinned to the mount every frame),
//          player pos = obj->+0x10, and the overhead settle
//          b74c -= f4*25 — on crossing below 0 it marks obj+0x148|=0x10
//          and clamps b74c=0.
//       2. obj->+0x14b & 4 (disabled) -> d58=0, d54=0, straight to the
//          energy check (channels, raw mouse, fire and recharge skipped).
//       3. semantic channels through accelChannel: ce730/ce734 -> d50
//          (DL |= 4), ce738/ce73c -> d54 (DL |= 8). With DL == 0 the raw
//          mouse path runs: (mouseDx || mouseDy) && mouseOn ->
//          d48 += dx*(1/3), d4c += dy*(1/3), d50 = d54 = 0 — the "/3"
//          mouse rate and NO mouseYReversed flip (the sniper's 0x12*f0*
//          zoom*5/12 aim gain does NOT apply here).
//       4. inactive channels decay: !DL&4 -> linearDecay(d50, 0.6667),
//          !DL&8 -> linearDecay(d54, 0.6667).
//       5. integrate + pixel clamp: d48 += d50*f0 clamped [128,472],
//          d4c += d54*f0 clamped [64,296] — the reticle lives in the
//          600x360 third-person frame, not the sniper viewport.
//       6. fire latch: fire==0 -> d0c = 999 (re-arm); fire!=0 -> when
//          d0c == 999 && ea0 > 0 the ballistic spawn runs (ea0 -= 1, the
//          +0x40 z = -5.0 drop, the aim unproject, projectile spawn — a
//          counted seam), then d0c -= frameStep (drains while held, can
//          go negative). This is the same d0c counter the sniper's
//          fire gate reads — cross-mode interaction is OBSERVED.
//       7. recharge: ea0 >= 10 -> ea4 = 1.0; else ea4 -= f4 and on
//          ea4 <= 0, ea0 += 1, ea4 = 1.0 (one bomb per second to 10).
//       8. energy: obj->+0x8 != 10000 -> FUN_00467a00(10000 - +0x8),
//          +0x8 = 10000; when the drain empties the health sentinel
//          (541554 <= 0) +0x8 = 0 and the death seam runs.
//
// OBSERVED quirks preserved: the +0x14b&2 mountable bit is the scan
// gate while +0x14a&0x80 marks mountable geometry — two different flag
// bytes; the "/3" raw mouse rate; the negative-going d0c drain; the
// shared channel globals take a second role as pixel coordinates.
//
#ifndef MDK_CORE_PLAYER_RETICLE_H
#define MDK_CORE_PLAYER_RETICLE_H

#include "core/gameplay_input.h"

namespace mdk {

struct TraversalRuntime;
struct RawGameplayInput;

// OBSERVED reticle constants (BUILD_A).
inline constexpr double kReticleSettleRate = 25.0;    // 0x498bf8 (f64)
inline constexpr double kReticleMouseK = 0.3333333333333333; // 0x498c00
inline constexpr float kReticleXMin = 128.0f;         // 0x43000000
inline constexpr float kReticleXMax = 472.0f;         // 0x43ec0000
inline constexpr float kReticleYMin = 64.0f;          // 0x42800000
inline constexpr float kReticleYMax = 296.0f;         // 0x43940000
inline constexpr float kReticleDecay = 0.6666667f;    // 0x3f2aaaab
inline constexpr int kReticleLatchArmed = 999;        // 0x3e7 semi-auto
inline constexpr int kReticleBombMax = 10;            // ea0 ceiling
inline constexpr float kReticleRecharge = 1.0f;       // ea4 = 1.0 / bomb
inline constexpr int kReticleEnergySentinel = 10000;  // 0x2710 (+0x8)

// The FUN_00463608 normal-path tail mount-scan. Latches the ride
// contact into e68, picks up a mountable candidate into e6c and runs
// the class-name entry writes (e70 / flags / pos / channels / reticle
// init). Called at the end of the unscoped, unmounted dispatch.
void playerReticleMountScan(TraversalRuntime& rt);

// The mounted class dispatch (FUN_00463608 e6c != 0 branch): reads the
// e70 class dword's byte2 and runs the per-class update (the class-1/2
// updates are counted seams; class 4 is FUN_004691c4). No match ->
// unmount. Always calls the FUN_00469cd0 weapon-slot seam afterward.
void playerReticleDispatchMounted(TraversalRuntime& rt,
                                  const RawGameplayInput& raw,
                                  const GameplayInputBindings& bindings,
                                  const GameplayInputFrame& ctrl,
                                  float f0, float f4, int frameStep);

// FUN_004691c4 — the mounted reticle/bomb-sight per-frame update
// (class 4). `ctrl` is the merged N-1 semantic frame (prevFrame);
// `raw`/`bindings` carry the CURRENT-frame mouse deltas + mouseOn
// (zero latency); `f0`/`f4` are the channel/delta time factors and
// `frameStep` the integer frame step.
void playerReticleUpdate(TraversalRuntime& rt,
                         const RawGameplayInput& raw,
                         const GameplayInputBindings& bindings,
                         const GameplayInputFrame& ctrl,
                         float f0, float f4, int frameStep);

} // namespace mdk

#endif // MDK_CORE_PLAYER_RETICLE_H
