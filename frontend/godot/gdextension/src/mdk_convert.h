// Phase 7 (G1) — godot-cpp adapters over mdk_math.h. The ONLY place
// MDK->Godot axis/sign work touches engine types; GDScript never
// sees raw MDK-space values except inside explicitly-labelled
// diagnostic keys ("*_mdk").
#pragma once

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "core/player_camera.h"

#include "mdk_math.h"

namespace godot {

inline Vector3 mdkToGodotVec(const float v[3]) {
  const mdkfront::Vec3 g = mdkfront::mdkVecToGodot(v);
  return Vector3(g.x, g.y, g.z);
}

// M2 basis rows (right/down/back) -> Godot Basis columns
// (X=right, Y=up=-down, Z=back).
inline Basis mdkToGodotCameraBasis(const mdk::PlayerCameraPose& p) {
  const mdkfront::BasisCols c = mdkfront::mdkCameraBasisToGodot(p.basis);
  Basis b;
  b.set_column(0, Vector3(c.cols[0][0], c.cols[0][1], c.cols[0][2]));
  b.set_column(1, Vector3(c.cols[1][0], c.cols[1][1], c.cols[1][2]));
  b.set_column(2, Vector3(c.cols[2][0], c.cols[2][1], c.cols[2][2]));
  return b.orthonormalized();
}

inline Transform3D mdkToGodotCameraTransform(
    const mdk::PlayerCameraPose& p) {
  return Transform3D(mdkToGodotCameraBasis(p), mdkToGodotVec(p.pos));
}

}  // namespace godot
