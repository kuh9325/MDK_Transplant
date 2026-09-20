// Phase 5K — normal traversal camera pose and view matrix.
//
// ORIGINAL ENGINE OBSERVATION (instruction-level, MDK95.EXE BUILD_A —
// see docs/GAMEPLAY_RECONSTRUCTION.md §"Phase 5K" for the full
// evidence chain):
//
//   FUN_004301e0 tail (0x43042b..0x4309dd) — the normal-mode camera
//   block, entered every non-sniper frame immediately after the
//   Phase 5J effective-pitch write (0x540be0):
//
//     position: anchored at the player (0x540bfc..0x540c04) with
//       pullback 0x540db4 (init 8.0) and eye-height 0x540db8
//       (init 4.5) minus height offset 0x540b5c. Two OBSERVED
//       branches on effective pitch:
//         pitch > 0:
//           T = (1 - cosP) * 5.0                     (0x497278)
//           camX = px + sinYaw * (T - pullback*cosP)
//           camY = py + cosYaw * (T - pullback*cosP)
//           camZ = pz + (eyeHeight - heightOffset)
//                  + pullback*sinP
//         pitch <= 0:
//           D = pullback                            (pitch >= -20)
//           D = (pitch + 100.0) * pullback * 0.0125 (pitch < -20;
//               0x49726c/0x497270/0x497274 — continuous at -20)
//           camX = px - sinYaw*D*cosP
//           camY = py - cosYaw*D*cosP
//           camZ = pz + (eyeHeight - heightOffset) + D*sinP
//       (sin/cos of 0x540b50 viewYaw and 0x540be0 pitch via
//       FUN_00437f98 — deg * stored f64 0x497924, FSIN/FCOS, f32.)
//     shake: |0x540ce4| != 0 → camX += 0x540cf8*0.2,
//       camY += 0x540cfc*0.2 (0x497280 = f64 0.2).
//     basis: back  = (-sinYaw*cosP, -cosYaw*cosP, sinP) — the
//       0x540b34 row written by the Phase 5J prefix; up =
//       (sinYaw*cosB*sinP + cosYaw*sinB,
//        cosYaw*cosB*sinP - sinYaw*sinB,
//        cosB*cosP) with bank = 0x540b4c + 0x540b60 — the
//       0x540b40 row. right = up x back; down = -(back x right).
//     projection scalars (0x540bf0/0x540bf4/0x540bf8):
//       scaleX = 1/(zoom*0.5)             (0x540b58, init 2.4)
//       scaleY = 1/(zoom*(H/W)*0.5)       H/W = 360/600 normal,
//                                         280/384 when 0x5414bc
//       scaleZ = -1.0                     (normal path)
//     matrix pair (row-major 3x4, row = [x,y,z,t]):
//       0x540b80 M1 = rows scaleX*right | scaleY*down | scaleZ*back
//         with t = scale*(-(row . cam)) — the projection-folded
//         world->view transform consumed by FUN_0046b4f8
//         (out0 = screenX num, out8 = screenY num, out4 = depth).
//       0x540bb0 M2 = rows right*|right| | down*|down| | back*|back|
//         with t = (-(row . cam))*|row| — the unscaled snapshot
//         consumed by FUN_0042b0c0 / FUN_0042e684 / FUN_004691c4.
//     obstruction seam: 0x49b710 != 0 && 0x540d58 == +-0 →
//       FUN_00430bf8(&camPos, &backCopy) BETWEEN basis compute and
//       matrix commit — the original may move BOTH the camera and
//       the player position there (evidence-backed, not ported:
//       needs FUN_00407fc0 arena queries + FUN_00418c60 slide +
//       FUN_004630d4 apply + the 0x540c68 object-list pass).
//     view config: normal (c9c==0 || ca0==0) → 0x540b64 = 2.4,
//       600x360@(0,0) centre (300,180); sniper (c9c&&ca0) → 1.0,
//       384x280@(107,79) centre (299,219) — boundary only, the
//       sniper POSE is Phase 5L and NOT implemented here.
//     portal tail: 0x49b714 = 0; eye = player + (0,0,3.0);
//       FUN_00435178(cur, eye -> camPos); result == 0x540ca4 →
//       0x49b714 = 1.
//
//   FUN_00431100 — the overhead view block (early path when
//   0x540c9c == 0 && 0x49b740 != 0; b740 is written by the scripted
//   0x40031 event in FUN_00463608 and by the cheat dispatcher
//   FUN_00423ca0, height 0x49b74c default 50.0):
//       camPos = (px, py, pz + overheadHeight)
//       M1 rows: [scaleX*sinYaw, -scaleX*cosYaw, 0, scaleX*tA]
//                [scaleY*(-cosYaw), scaleY*(-sinYaw), 0, scaleY*tB]
//                [0, 0, scaleZ*(-1)= -1*scaleZ... = -1],  (0x4972b0:
//                 scaleZ = +1.0 OBSERVED — opposite sign to normal)
//       M2 rows: [sinYaw, -cosYaw, 0, tA]
//                [-cosYaw, -sinYaw, 0, tB]
//                [0, 0, -1, tC]  (all |row| = 1)
//       yaw = raw locomotion yaw 0x540c2c (NOT viewYaw); writes
//       camPos + both matrices + scalars only — no view-config, no
//       portal tail, no prev-pos commit (the caller does that).
//
//   World convention (Phase 5B, CONFIRMED by the basis formulas):
//   +X forward at yaw 0, +Y left, +Z up; viewYaw = 90 - yaw.
//   Both matrices are world->camera, row-major 3x4, translation in
//   element [3] of each row (FUN_0046b4f8: out = row.xyz.p + t).
//   There is no stored FOV angle — the projection is the folded
//   scale pair (scaleX/scaleY = 1/(zoom*0.5*aspect)); UNKNOWN
//   whether the original expresses an equivalent angle anywhere.
//
// NATIVE PORT DECISIONS:
//   - PlayerCameraState carries the persistent camera globals; the
//     per-frame PlayerCameraPose mirrors the 0x540b28..0x540b7c +
//     matrix block exactly (stale fields persist across frames the
//     original does not rewrite them — e.g. the overhead path).
//   - The FUN_00430bf8 obstruction call is a documented seam:
//     updatePlayerCamera reports `obstructionSeam` when the original
//     would have invoked it (gate b710 && |d58|==0). Nothing is
//     emulated; the call sits between basis compute and the matrix
//     commit in the original order, so env.playerPos is in/out.
//   - Sniper viewport rect values are stored (they are a plain
//     write in this block) but no sniper pose/zoom is produced.
//
#ifndef MDK_CORE_PLAYER_CAMERA_H
#define MDK_CORE_PLAYER_CAMERA_H

namespace mdk {

// Per-frame camera outputs — the globals FUN_004301e0 /
// FUN_00431100 leave behind for the renderer + gameplay consumers.
// Field order follows the original 0x540b28.. region.
struct PlayerCameraPose {
  float pos[3] = {0, 0, 0};    // 0x540b28/2c/30 — camera world pos
  float back[3] = {0, 0, 0};   // 0x540b34/38/3c — view row0 (-forward)
  float up[3] = {0, 0, 0};     // 0x540b40/44/48 — banked view up row
  float sinPitch = 0.0f;       // 0x540be4 — pitch trig cache
  float cosPitch = 0.0f;       // 0x540be8
  // Projection-folded world->camera matrix M1 (0x540b80..0x540bac)
  // and the unscaled basis snapshot M2 (0x540bb0..0x540bdc).
  // Row-major 3x4: [row][0..2] = basis coefficients, [row][3] = t.
  float view[3][4] = {};
  float basis[3][4] = {};
  float scaleX = 0.0f;         // 0x540bf0 — 1/(zoom*0.5)
  float scaleY = 0.0f;         // 0x540bf4 — 1/(zoom*(H/W)*0.5)
  float scaleZ = 0.0f;         // 0x540bf8 — -1 normal / +1 overhead
  float modeZoom = 0.0f;       // 0x540b64 — 2.4 normal / 1.0 alt
  int viewW = 0;               // 0x540b68
  int viewH = 0;               // 0x540b6c
  int viewCX = 0;              // 0x540b70
  int viewCY = 0;              // 0x540b74
  int viewOX = 0;              // 0x540b78
  int viewOY = 0;              // 0x540b7c
};

// Persistent camera globals — written by FUN_00433c4c (init),
// FUN_00461878 (mode-exit reset), FUN_0040ef28 (level ZOOM record),
// FUN_00464d10 (debug keys), FUN_00461954 (scripted states),
// FUN_00436100 (shake outputs), FUN_00423ca0/FUN_00427218
// (cheat/save paths).
struct PlayerCameraState {
  float zoom = 2.4f;           // 0x540b58 — OBSERVED init (FUN_00433c4c)
  float pullback = 8.0f;       // 0x540db4 — OBSERVED init
  float eyeHeight = 4.5f;      // 0x540db8 — OBSERVED init
  float heightOffset = 0.0f;   // 0x540b5c — OBSERVED init
  float overheadHeight = 0.0f; // 0x49b74c — set by 0x40031 / cheat
  float shakeMag = 0.0f;       // 0x540ce4 — |x|!=0 gates the XY add
  float shakeX = 0.0f;         // 0x540cf8 — shake pixel offset
  float shakeY = 0.0f;         // 0x540cfc
  bool obstructionEnabled = false; // 0x49b710 — cheat/debug gate for
                                   // the FUN_00430bf8 seam
  PlayerCameraPose pose;       // persistent — stale fields survive
                               // frames the original does not write
};

// Per-frame inputs the camera block reads from outside itself.
struct PlayerCameraEnvironment {
  // 0x540bfc..0x540c04 — player world position. in/out: the
  // FUN_00430bf8 seam (when ported) may move BOTH ends of the
  // eye->camera segment.
  float playerPos[3] = {0, 0, 0};
  float viewYawDeg = 0.0f;     // 0x540b50 — view tail write
  float effPitchDeg = 0.0f;    // 0x540be0 — view tail write
  float bankDeg = 0.0f;        // 0x540b4c + 0x540b60
  float yawDeg = 0.0f;         // 0x540c2c — raw locomotion yaw
                             // (the overhead path uses it, not
                             // viewYaw — OBSERVED)
  bool altAspect = false;      // 0x5414bc — 280/384 aspect pair
  bool sniperViewport = false; // c9c != 0 && ca0 != 0 — alt rect
  bool lookActive = false;     // |0x540d58| != 0 — suppresses the
                               // obstruction call (OBSERVED gate)
};

struct PlayerCameraFrame {
  // FUN_00430bf8 would have run at this point (b710 != 0 &&
  // |d58| == +-0) — counted seam, internals deferred (needs arena
  // collision queries + slide sampling + the 0x540c68 object pass).
  bool obstructionSeam = false;
};

// FUN_004301e0 tail — the normal-mode camera block. Runs the
// position branch, shake add, basis, projection scalars, the M1/M2
// matrix pair, the obstruction seam point, and the view-config
// write — in the original order. The portal tail (0x49b714 /
// FUN_00435178 eye->camPos) stays with the caller because it needs
// the arena table.
PlayerCameraFrame updatePlayerCamera(PlayerCameraEnvironment& env,
                                     PlayerCameraState& st);

// FUN_00431100 — the overhead view block (the flag49b740 early
// path). Writes camPos + scalars + both matrices only.
void updatePlayerCameraOverhead(const PlayerCameraEnvironment& env,
                                PlayerCameraState& st);

} // namespace mdk

#endif // MDK_CORE_PLAYER_CAMERA_H
