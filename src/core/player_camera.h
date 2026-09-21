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
//     obstruction call: 0x49b710 != 0 && 0x540d58 == +-0 →
//       FUN_00430bf8(&camPos) BETWEEN basis compute and matrix
//       commit — Phase 5M ports it (see below).
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
//   Phase 5M — FUN_00430bf8 (camera obstruction) and FUN_0042b0c0
//   (camera nudge):
//
//     FUN_00430bf8(EAX = &camPos=0x540b28) — gated on
//     0x49b710 != 0 && |0x540d58| == +-0, between basis compute and
//     the M1/M2 commit. Static pass: segment eye->camPos where
//     eye = playerPos + (0,0,5.5) (f64 const 0x4972a8), swept through
//     FUN_00407fc0 with ext {0.1,0.1,0.1} (0x49b780), flag=0
//     (contact-only), scale=0. On a primary-arena miss it re-queries
//     the carrier arena 0x540ca4 — only when ca4 != 0 && 0x540d3c
//     == 0. On a hit:
//       dist  = 2D XY distance hitPos<->camPos  (FUN_004301bc)
//       n.xy  = hit node's split-plane normal (FUN_00408254 ->
//               0x4a20c8), sign-flipped toward the eye side
//       dx = dist*nx, dy = dist*ny — the push applied to the PLAYER
//       0x540e4c == 0  -> FUN_004630d4(dx,dy,0,0.75,0,0) unconditional
//       0x540e4c != 0  -> the push is gated on a grounding probe:
//         FUN_00418c60 BSP stab of the +-4.0 vertical segment at the
//         displaced XY against the PRIMARY arena (always c48, even
//         after a carrier hit); a stab hit lands the push. On a miss
//         the displacement rotates +-90 deg by half its length —
//         retry1 dx += 0.5*dist*ny, dy -= 0.5*dist*nx; retry2 mirrors
//         (dx0 - h*ny, dy0 + h*nx). All-miss -> no displacement.
//       After every apply: camPos += (pos - snapshot) — the camera
//       follows the player's APPLIED delta, never the raw request.
//     Object pass — gated on 0x540c68 (arenaValid), runs regardless
//     of the static outcome: eye is rebuilt from the live player pos
//     (+5.5z), segTarget starts at the live camPos; each object with
//     byte+0x06 != 0, +0x08 != 0, !(flags148 & 0x810), flags14b & 0x01
//     is AABB-prefiltered (FUN_0045cd38, box+0x198 inflated by the
//     0.1 extents) then every element AABB (+0x44, 0x5c stride) runs
//     FUN_0045c838 — a rc==1 face clamp folds the crossing point into
//     the target. The tail always applies FUN_004630d4(target.xy -
//     camPos.xy, 0, 0.75) — even a zero delta — and shifts camPos by
//     the player's applied delta.
//
//     FUN_0042b0c0(EAX = arg) — the render-pass camera nudge:
//       camPos += M2[0][i] * (arg * 0x49b570)      (0x49b570 = 0.25f)
//       tick = trunc(arg * 0x49b574)               (x87 FRNDINT under
//             truncation CW — toward zero, not round-nearest)
//       r = tick * 0x496e0c                        (= 1/600 f32)
//       M1[0][i] += M1[2][i] * r                   (i = 0..2)
//       M1[row][3] = -(M1[row][0..2] . camPos)     (all three rows,
//             recomputed with the UPDATED row0 and camPos)
//     The bracketing callers (FUN_0042b060/FUN_0042b090 inside
//     FUN_00436d60 and the render passes) save/restore the whole
//     212-byte camera block 0x540b28..0x540bfc around nudge+body —
//     the perturbation never persists; only 0x49b578 (the int tick)
//     survives because it lives outside the saved range.
//     FUN_0042b20c(mode) writes 0x49b574: {0->12.0, 2->15.0,
//     else->20.0} (jump table 0x42b1fc, default >3 -> 20.0).
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
//   - The FUN_00430bf8 obstruction call is ported (Phase 5M):
//     updatePlayerCamera invokes applyCameraObstruction at the
//     original position (post-basis, pre-commit) when the gate fires
//     and env.collision is set. env.playerPos is the in/out channel
//     for the original's both-endpoints write (0x540bfc == cs.pos).
//   - FUN_0042b0c0 is a standalone helper (cameraNudge) — its callers
//     bracket it with the 212-byte camera-block save/restore, which
//     the traversal runtime mirrors on rt.camera.pose.
//   - Sniper viewport rect values are stored (they are a plain
//     write in this block) but no sniper pose/zoom is produced.
//
#ifndef MDK_CORE_PLAYER_CAMERA_H
#define MDK_CORE_PLAYER_CAMERA_H

namespace mdk {

struct CollisionState;
struct CollisionPoly;

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
  bool obstructionEnabled = true;  // 0x49b710 — OBSERVED init 1 in
                                   // the BUILD_A image (the cheat
                                   // dispatcher FUN_00423ca0 also
                                   // writes 1). Gates FUN_00430bf8.
  float nudgeShift = 0.25f;        // 0x49b570 — camPos += M2row0 *
                                   // (arg * nudgeShift); no writer
                                   // in BUILD_A (image constant).
  float nudgeScale = 20.0f;        // 0x49b574 — FUN_0042b20c writes
                                   // {0->12, 2->15, else->20}; the
                                   // image init is 20.0f.
  int nudgeTick = 0;               // 0x49b578 — the FRNDINT-truncated
                                   // tick; written by every
                                   // FUN_0042b0c0 / FUN_0042b248.
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
  // Phase 5M — the FUN_00430bf8 collision context. `collision`
  // supplies 0x540bfc (cs.pos — the same buffer env.playerPos
  // mirrors), the primary/carrier arenas (c48/ca4), the carrier gate
  // (d3c), the object-pass gate (c68) and the query machinery.
  // `contactToken` is 0x540e4c — the last gameplay apply's contact,
  // which gates the grounding-probe retry path (the obstruction's
  // own applies never write it — FUN_00430bf8 only reads it).
  CollisionState* collision = nullptr;
  const CollisionPoly* contactToken = nullptr;
};

struct PlayerCameraFrame {
  // FUN_00430bf8 ran at this point (b710 != 0 && |d58| == +-0).
  // True even when env.collision was absent (the gate fired; the
  // call needs arenas to do work).
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

// FUN_00430bf8 — the camera obstruction/displacement call. Runs the
// eye->camPos static sweep (+ carrier re-query), the hit-plane
// displacement with its grounding-probe retries, and the arena
// object pass; applies the surviving deltas to the player via
// collisionApply and shifts camPos by the applied delta. Requires
// env.collision (the caller gates it); reads env.contactToken for
// the 0x540e4c probe gate; syncs env.playerPos on exit.
void applyCameraObstruction(PlayerCameraEnvironment& env,
                            PlayerCameraState& st);

// FUN_0042b0c0 — the bracketed render-pass camera nudge. Moves
// camPos along M2 row0 by arg*nudgeShift, folds
// trunc(arg*nudgeScale)*(1/600) into M1 row0 via the M1 row2
// coefficients, and refolds all three M1 translations against the
// updated position/rows. The original callers save/restore the
// 212-byte camera block around it — the caller mirrors that on
// st.pose if the perturbation must not persist.
void cameraNudge(int arg, PlayerCameraState& st);

// FUN_0042b20c — mode -> nudgeScale: {0 -> 12.0, 2 -> 15.0,
// else -> 20.0} (jump-table default covers mode 1/3/>3).
void cameraNudgeApplyMode(PlayerCameraState& st, int mode);

} // namespace mdk

#endif // MDK_CORE_PLAYER_CAMERA_H
