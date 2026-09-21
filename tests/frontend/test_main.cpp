// Phase 7 (G1) — frontend conversion-layer tests. These run native
// (no Godot runtime): they pin the pure math in mdk_math.h — the
// same scalar pipeline the GDExtension wrapper (mdk_convert.h)
// feeds into Godot Basis/Transform3D — against the proven spike
// goldens for the LEVEL3 HMO_1 spawn camera.
//
// Canonical mapping (docs/GODOT_INTEGRATION_AUDIT.md):
//   godot = (-mdk.y, mdk.z, -mdk.x)   — proper rotation, det +1.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "mdk_math.h"

static int gChecks = 0, gFailures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++gChecks;                                                         \
    if (!(cond)) {                                                     \
      ++gFailures;                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                   #cond);                                             \
    }                                                                  \
  } while (0)

static bool near(double a, double b, double eps = 1e-5) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace mdkfront;

  // ---- position conversion: LEVEL3 goldens ----
  {
    // Player spawn (-4, 0, 190) -> (0, 190, 4).
    const Vec3 p = mdkVecToGodot(-4.0f, 0.0f, 190.0f);
    CHECK(near(p.x, 0.0) && near(p.y, 190.0) && near(p.z, 4.0));
    // Spawn camera (-3.16708231, -7.92468166, 195.058044) ->
    // (7.92468166, 195.058044, 3.16708231).
    const Vec3 c =
        mdkVecToGodot(-3.16708231f, -7.92468166f, 195.058044f);
    CHECK(near(c.x, 7.92468166, 1e-6) && near(c.y, 195.058044, 1e-6) &&
          near(c.z, 3.16708231, 1e-6));
    // Unit axes: MDK +X (forward) -> Godot -Z; +Y (left) -> -X;
    // +Z (up) -> +Y.
    const Vec3 fx = mdkVecToGodot(1, 0, 0);
    const Vec3 fy = mdkVecToGodot(0, 1, 0);
    const Vec3 fz = mdkVecToGodot(0, 0, 1);
    CHECK(fx.x == 0 && fx.y == 0 && fx.z == -1);
    CHECK(fy.x == -1 && fy.y == 0 && fy.z == 0);
    CHECK(fz.x == 0 && fz.y == 1 && fz.z == 0);
  }

  // ---- basis conversion: yaw-0 MDK basis -> Godot identity ----
  {
    // MDK yaw 0 faces +X with +Z up: right = -Y (MDK +Y is left),
    // down = -Z, back = -X (the M2 row order is right/down/back).
    const float rows[3][4] = {{0, -1, 0, 0},   // right
                              {0, 0, -1, 0},   // down
                              {-1, 0, 0, 0}};  // back
    const BasisCols b = mdkCameraBasisToGodot(rows);
    // Godot columns: X=(1,0,0) Y=(0,1,0) Z=(0,0,1) — identity.
    CHECK(near(b.cols[0][0], 1) && near(b.cols[0][1], 0) &&
          near(b.cols[0][2], 0));
    CHECK(near(b.cols[1][0], 0) && near(b.cols[1][1], 1) &&
          near(b.cols[1][2], 0));
    CHECK(near(b.cols[2][0], 0) && near(b.cols[2][1], 0) &&
          near(b.cols[2][2], 1));
    // Determinant +1 — proper rotation, no mirroring.
    const double det =
        b.cols[0][0] * (b.cols[1][1] * b.cols[2][2] -
                        b.cols[1][2] * b.cols[2][1]) -
        b.cols[0][1] * (b.cols[1][0] * b.cols[2][2] -
                        b.cols[1][2] * b.cols[2][0]) +
        b.cols[0][2] * (b.cols[1][0] * b.cols[2][1] -
                        b.cols[1][1] * b.cols[2][0]);
    CHECK(near(det, 1.0));
  }

  // ---- basis conversion: the real spawn basis stays orthonormal --
  {
    // LEVEL3 spawn: yaw 96 deg, pitch 0. MDK basis rows for a yawed
    // level camera (pitch 0): back = (-sinYaw, -cosYaw, 0),
    // right = (-cosYaw, sinYaw, 0)... derive from the documented
    // row model (player_camera.h): back=(-sinY*cosP,-cosY*cosP,sinP),
    // right = up x back, down = -(back x right). At yaw=96 pitch=0:
    // back = (-0.99354, 0.11320, 0); up row ~= (0,0,1) banked.
    // right = up x back = (0*0-1*0.11320, 1*(-0.99354)-0*0, 0) —
    // compute numerically instead of trusting the arithmetic.
    const double yaw = 96.0 * M_PI / 180.0;
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    const double back[3] = {-sy * 1.0, -cy * 1.0, 0.0};
    const double up[3] = {0.0, 0.0, 1.0};
    // right = up x back; down = -(back x right).
    const double right[3] = {up[1] * back[2] - up[2] * back[1],
                             up[2] * back[0] - up[0] * back[2],
                             up[0] * back[1] - up[1] * back[0]};
    const double bcrossr[3] = {back[1] * right[2] - back[2] * right[1],
                               back[2] * right[0] - back[0] * right[2],
                               back[0] * right[1] - back[1] * right[0]};
    const float rows[3][4] = {
        {float(right[0]), float(right[1]), float(right[2]), 0},
        {float(-bcrossr[0]), float(-bcrossr[1]), float(-bcrossr[2]), 0},
        {float(back[0]), float(back[1]), float(back[2]), 0}};
    const BasisCols b = mdkCameraBasisToGodot(rows);
    // Godot camera -Z must equal the converted MDK forward. From
    // back = (-sinYaw*cosP, -cosYaw*cosP, sinP): forward = -back =
    // (sinYaw*cosP, cosYaw*cosP, -sinP) -> godot
    // (-cosYaw*cosP, -sinP, -sinYaw*cosP). At pitch 0:
    // (-cos96, 0, -sin96).
    const double fx = -cy, fz = -sy;
    // Basis column Z is back = -forward -> forward = -colZ.
    CHECK(near(-b.cols[2][0], fx, 1e-5) && near(-b.cols[2][1], 0.0) &&
          near(-b.cols[2][2], fz, 1e-5));
    // Column Y (up) = -converted down ~= +Y for a level camera.
    CHECK(near(b.cols[1][1], 1.0, 1e-4));
    const double det =
        b.cols[0][0] * (b.cols[1][1] * b.cols[2][2] -
                        b.cols[1][2] * b.cols[2][1]) -
        b.cols[0][1] * (b.cols[1][0] * b.cols[2][2] -
                        b.cols[1][2] * b.cols[2][0]) +
        b.cols[0][2] * (b.cols[1][0] * b.cols[2][1] -
                        b.cols[1][1] * b.cols[2][0]);
    CHECK(near(det, 1.0, 1e-5));
  }

  // ---- player yaw basis: upright presentation columns ----
  {
    // yaw 0: MDK forward +X -> Godot -Z; MDK right (0,-1,0) -> +X;
    // up +Z -> +Y. The whole basis is the identity.
    const BasisCols b0 = mdkYawToGodotBasisDeg(0.0f);
    CHECK(near(b0.cols[0][0], 1) && near(b0.cols[0][2], 0) &&
          near(b0.cols[2][0], 0) && near(b0.cols[2][2], 1));
    // yaw 90: MDK forward +Y -> Godot -X, so -colZ == (-1,0,0).
    const BasisCols b90 = mdkYawToGodotBasisDeg(90.0f);
    CHECK(near(-b90.cols[2][0], -1, 1e-6) &&
          near(-b90.cols[2][2], 0, 1e-6));
    // right = MDK (sin90,-cos90,0)=(1,0,0) -> Godot (0,0,-1).
    CHECK(near(b90.cols[0][2], -1, 1e-6) &&
          near(b90.cols[0][0], 0, 1e-6));
    // LEVEL3 spawn yaw 96: forward = (-sin96, 0, -cos96);
    // right = (cos96, 0, -sin96); det +1.
    const double t = 96.0 * M_PI / 180.0;
    const BasisCols b96 = mdkYawToGodotBasisDeg(96.0f);
    CHECK(near(-b96.cols[2][0], -std::sin(t), 1e-6) &&
          near(-b96.cols[2][2], -std::cos(t), 1e-6));
    CHECK(near(b96.cols[0][0], std::cos(t), 1e-6) &&
          near(b96.cols[0][2], -std::sin(t), 1e-6));
    const double det =
        b96.cols[0][0] * (b96.cols[1][1] * b96.cols[2][2] -
                          b96.cols[1][2] * b96.cols[2][1]) -
        b96.cols[0][1] * (b96.cols[1][0] * b96.cols[2][2] -
                          b96.cols[1][2] * b96.cols[2][0]) +
        b96.cols[0][2] * (b96.cols[1][0] * b96.cols[2][1] -
                          b96.cols[1][1] * b96.cols[2][0]);
    CHECK(near(det, 1.0, 1e-6));
    // The basis equals a pure +Y rotation: column Y stays +Y.
    CHECK(b96.cols[1][0] == 0 && b96.cols[1][1] == 1 &&
          b96.cols[1][2] == 0);
  }

  // ---- player collision AABB conversion ----
  {
    // 0x540c30..44 at the spawn pos (-4,0,190): x/y +-1.25, z..z+4.25.
    const float box[6] = {-5.25f, -1.25f, 190.0f,
                          -2.75f, 1.25f, 194.25f};
    const Aabb3 a = mdkBoxToGodot(box);
    // godot min = (-maxy, minz, -maxx) = (-1.25, 190, 2.75);
    // godot max = (-miny, maxz, -minx) = (1.25, 194.25, 5.25).
    CHECK(near(a.min.x, -1.25) && near(a.min.y, 190.0) &&
          near(a.min.z, 2.75));
    CHECK(near(a.max.x, 1.25) && near(a.max.y, 194.25) &&
          near(a.max.z, 5.25));
    // Footprint 2.5x2.5, height 4.25; pos (0,190,4) sits inside.
    CHECK(near(a.max.x - a.min.x, 2.5) &&
          near(a.max.y - a.min.y, 4.25) &&
          near(a.max.z - a.min.z, 2.5));
  }

  // ---- projection: normal-viewport fov/aspect goldens ----
  {
    // scaleY = 1/(zoom*(H/W)*0.5) with zoom=2.4, H/W=360/600:
    // = 1/0.72 = 1.38889; scaleX = 1/(2.4*0.5) = 0.83333.
    const double scaleX = 1.0 / (2.4 * 0.5);
    const double scaleY = 1.0 / (2.4 * (360.0 / 600.0) * 0.5);
    const double fov = mdkCameraFovYDeg(scaleY, 360.0f, 180.4f);
    CHECK(near(fov, 71.36, 0.05));
    const double aspect = mdkCameraAspect(scaleX, scaleY, 600.0f,
                                          360.0f, 299.95f, 180.4f);
    CHECK(near(aspect, 1.6709, 1e-3));
    // Degenerate inputs -> 0 (caller falls back).
    CHECK(mdkCameraFovYDeg(0.0f, 360.0f, 180.4f) == 0.0f);
    CHECK(mdkCameraAspect(scaleX, 0.0f, 600, 360, 299.95f, 180.4f) ==
          0.0f);
  }

  if (gFailures == 0) {
    std::printf("%d checks, 0 failures\n", gChecks);
    return 0;
  }
  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return 1;
}
