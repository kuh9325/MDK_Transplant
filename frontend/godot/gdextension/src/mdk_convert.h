// Phase 7 (G1) — godot-cpp adapters over mdk_math.h. The ONLY place
// MDK->Godot axis/sign work touches engine types; GDScript never
// sees raw MDK-space values except inside explicitly-labelled
// diagnostic keys ("*_mdk").
#pragma once

#include <godot_cpp/variant/aabb.hpp>
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

inline Basis mdkToGodotBasis(const mdkfront::BasisCols& c) {
  Basis b;
  b.set_column(0, Vector3(c.cols[0][0], c.cols[0][1], c.cols[0][2]));
  b.set_column(1, Vector3(c.cols[1][0], c.cols[1][1], c.cols[1][2]));
  b.set_column(2, Vector3(c.cols[2][0], c.cols[2][1], c.cols[2][2]));
  return b;
}

// M2 basis rows (right/down/back) -> Godot Basis columns
// (X=right, Y=up=-down, Z=back).
inline Basis mdkToGodotCameraBasis(const mdk::PlayerCameraPose& p) {
  const mdkfront::BasisCols c = mdkfront::mdkCameraBasisToGodot(p.basis);
  return mdkToGodotBasis(c).orthonormalized();
}

inline Transform3D mdkToGodotCameraTransform(
    const mdk::PlayerCameraPose& p) {
  return Transform3D(mdkToGodotCameraBasis(p), mdkToGodotVec(p.pos));
}

// Upright player presentation basis from the locomotion yaw
// (PlayerMotionState::yawDeg). Equals rotation.y = deg_to_rad(yaw).
inline Basis mdkToGodotPlayerBasis(float yawDeg) {
  return mdkToGodotBasis(mdkfront::mdkYawToGodotBasisDeg(yawDeg));
}

// The player presentation transform: snapshot position + yaw basis.
// pos sits at the collision box bottom (the feet).
inline Transform3D mdkToGodotPlayerTransform(const float pos[3],
                                             float yawDeg) {
  return Transform3D(mdkToGodotPlayerBasis(yawDeg),
                     mdkToGodotVec(pos));
}

// MDK {minx,miny,minz,maxx,maxy,maxz} -> Godot AABB.
inline AABB mdkToGodotAabb(const float box6[6]) {
  const mdkfront::Aabb3 a = mdkfront::mdkBoxToGodot(box6);
  return AABB(Vector3(a.min.x, a.min.y, a.min.z),
              Vector3(a.max.x - a.min.x, a.max.y - a.min.y,
                      a.max.z - a.min.z));
}

// Phase 9 (G3) — a full MDK object transform (row-major 3x3 +
// origin) -> the Godot Transform3D the object's Node3D should
// carry. The conversion is P*M*P^T inside mdk_math.h; local model
// verts are P-converted in object_presenter.cpp, so the composite
// reproduces P * world_mdk exactly.
inline Transform3D mdkToGodotObjectTransform(
    const mdkfront::MdkTransform& t) {
  return Transform3D(mdkToGodotBasis(t.basis),
                     Vector3(t.origin.x, t.origin.y, t.origin.z));
}

}  // namespace godot
