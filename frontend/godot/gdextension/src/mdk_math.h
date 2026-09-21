// Phase 7 (G1) — MDK -> Godot coordinate math, engine-agnostic.
//
// Canonical conversion (docs/GODOT_INTEGRATION_AUDIT.md, proven by
// the disposable spike):
//   MDK world:  +X forward (yaw 0), +Y left, +Z up, degrees, RH
//   Godot:      -Z forward, +X right, +Y up, radians, RH
//   v_godot = (-v_mdk.y, v_mdk.z, -v_mdk.x)   (proper rotation, det +1)
//
// The core M2 snapshot (PlayerCameraPose::basis) stores the camera
// basis as ROWS in MDK world coords: row0 = right, row1 = down,
// row2 = back. A Godot Basis takes columns X = right, Y = up
// (= -down), Z = back — so the converted columns are
//   colX = P(right)   colY = -P(down)   colZ = P(back).
//
// Projection (PlayerCameraPose::scaleX/scaleY/scaleZ, the folded M1
// scalars): the original projector divides screen numerators by
// depth with per-axis pixel divisors (299.95 / 180.4 for the normal
// 600x360 viewport — OBSERVED constants). Matching the vertical axis
// exactly gives
//   tan(fovY/2) = viewHalfH / (scaleY * yDiv) = 180 / (scaleY*180.4)
// and the implied horizontal aspect is scaleY*yDiv/(scaleX*xDiv)
// (≈1.6707 for the normal viewport — the 0.24% asymmetry vs 600/360
// is the original's divisor quirk, preserved here).
//
// This header is pure scalar math — no engine types — so the same
// conversion is exercised by the native frontend tests and the
// GDExtension wrapper alike.
#ifndef MDK_FRONTEND_MATH_H
#define MDK_FRONTEND_MATH_H

#include <cmath>

namespace mdkfront {

struct Vec3 {
  float x, y, z;
};

// Column-major 3x3 basis for a Godot Transform3D (cols[c][r]).
struct BasisCols {
  float cols[3][3];
};

inline Vec3 mdkVecToGodot(float x, float y, float z) {
  return {-y, z, -x};
}
inline Vec3 mdkVecToGodot(const float v[3]) {
  return mdkVecToGodot(v[0], v[1], v[2]);
}

// M2 basis rows (right/down/back, MDK world coords) -> Godot basis
// columns (X = right, Y = up = -down, Z = back).
inline BasisCols mdkCameraBasisToGodot(const float rows[3][4]) {
  const Vec3 right = mdkVecToGodot(rows[0][0], rows[0][1], rows[0][2]);
  const Vec3 down = mdkVecToGodot(rows[1][0], rows[1][1], rows[1][2]);
  const Vec3 back = mdkVecToGodot(rows[2][0], rows[2][1], rows[2][2]);
  return {{{right.x, right.y, right.z},
           {-down.x, -down.y, -down.z},
           {back.x, back.y, back.z}}};
}

// Locomotion yaw (PlayerMotionState::yawDeg, 0x540c2c) -> the upright
// Godot basis for the player presentation. MDK forward at yaw t is
// (cos t, sin t, 0), right is (sin t, -cos t, 0), up is +Z, so the
// converted columns are
//   X = P(right) = (cos t, 0, -sin t)
//   Y = P(up)    = (0, 1, 0)
//   Z = P(-fwd)  = (sin t, 0,  cos t)
// which is exactly a Godot +Y rotation by t radians — the same value
// as rotation.y = deg_to_rad(yaw_mdk). Positive yaw turns left,
// matching the documented same-sign yaw rule.
inline BasisCols mdkYawToGodotBasisDeg(float yawDeg) {
  const float t = yawDeg * (3.14159265358979323846f / 180.0f);
  const float c = std::cos(t), s = std::sin(t);
  return {{{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}}};
}

// MDK {minx,miny,minz,maxx,maxy,maxz} AABB (e.g. the player collision
// box 0x540c30..44) -> Godot {min,max}. P negates x/y, so each Godot
// axis min/max pair comes from the opposite MDK axis endpoint.
struct Aabb3 {
  Vec3 min, max;
};
inline Aabb3 mdkBoxToGodot(const float box6[6]) {
  return {{-box6[4], box6[2], -box6[3]},
          {-box6[1], box6[5], -box6[0]}};
}

// tan(fovY/2) = viewHalfH / (scaleY * yDivisor). The pixel divisors
// default to the normal 600x360 viewport (299.95 / 180.4).
inline float mdkCameraTanHalfFovY(float scaleY, float viewH,
                                float yDiv) {
  if (scaleY <= 0.0f || yDiv <= 0.0f) return 0.0f;
  return (viewH * 0.5f) / (scaleY * yDiv);
}
inline float mdkCameraFovYDeg(float scaleY, float viewH, float yDiv) {
  return 2.0f * std::atan(mdkCameraTanHalfFovY(scaleY, viewH, yDiv)) *
         (180.0f / 3.14159265358979323846f);
}
// Implied aspect (tan(hfov/2)/tan(fovY/2)) — for diagnostics.
inline float mdkCameraAspect(float scaleX, float scaleY, float viewW,
                             float viewH, float xDiv, float yDiv) {
  if (scaleX <= 0.0f || xDiv <= 0.0f || viewH <= 0.0f) return 0.0f;
  return (scaleY * yDiv * viewW) / (scaleX * xDiv * viewH);
}

}  // namespace mdkfront

#endif  // MDK_FRONTEND_MATH_H
