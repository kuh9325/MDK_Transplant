// Phase 7 (G1) — frontend conversion-layer tests. These run native
// (no Godot runtime): they pin the pure math in mdk_math.h — the
// same scalar pipeline the GDExtension wrapper (mdk_convert.h)
// feeds into Godot Basis/Transform3D — against the proven spike
// goldens for the LEVEL3 HMO_1 spawn camera.
//
// Canonical mapping (docs/GODOT_INTEGRATION_AUDIT.md):
//   godot = (-mdk.y, mdk.z, -mdk.x)   — proper rotation, det +1.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/dynamic_objects.h"
#include "core/stream_scene.h"
#include "gif_decode.h"
#include "mdk_math.h"
#include "mdk_objid.h"
#include "stream_presenter.h"

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

  // ================================================================
  // Phase 9 (G3) — object transform conversion (P*M*P^T) + objids
  // ================================================================
  //
  // MDK object transform:  world = M * local + org   (row-major m[9])
  // Godot object transform: out  = B * P(local) + P(org)
  // Invariant under test:   B*P(v)+P(o) == P(M*v+o)   for every v.
  {
    auto mdkApply = [](const float m[9], const float o[3],
                       const Vec3& v, Vec3& out) {
      out.x = m[0] * v.x + m[1] * v.y + m[2] * v.z + o[0];
      out.y = m[3] * v.x + m[4] * v.y + m[5] * v.z + o[1];
      out.z = m[6] * v.x + m[7] * v.y + m[8] * v.z + o[2];
    };
    auto basisApply = [](const BasisCols& b, const Vec3& v) {
      Vec3 out;
      out.x = b.cols[0][0] * v.x + b.cols[1][0] * v.y +
              b.cols[2][0] * v.z;
      out.y = b.cols[0][1] * v.x + b.cols[1][1] * v.y +
              b.cols[2][1] * v.z;
      out.z = b.cols[0][2] * v.x + b.cols[1][2] * v.y +
              b.cols[2][2] * v.z;
      return out;
    };
    auto vecNear = [](const Vec3& a, const Vec3& b, double eps) {
      return near(a.x, b.x, eps) && near(a.y, b.y, eps) &&
             near(a.z, b.z, eps);
    };
    // Verify B*P(v)+P(o) == P(M*v+o) over a spread of sample points.
    auto checkEquivalence = [&](const float m[9], const float o[3],
                                const char* tag) {
      const MdkTransform t = mdkTransformToGodot(m, {o[0], o[1], o[2]});
      const Vec3 pts[5] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                           {0, 0, 1}, {3.5f, -2.25f, 7.0f}};
      bool ok = true;
      for (const Vec3& v : pts) {
        Vec3 mw;
        mdkApply(m, o, v, mw);
        const Vec3 expect = mdkVecToGodot(mw.x, mw.y, mw.z);
        const Vec3 pv = mdkVecToGodot(v.x, v.y, v.z);
        Vec3 got = basisApply(t.basis, pv);
        got.x += t.origin.x;
        got.y += t.origin.y;
        got.z += t.origin.z;
        if (!vecNear(expect, got, 1e-4)) {
          std::fprintf(stderr, "  %s: point (%g,%g,%g) -> "
              "expect (%g,%g,%g) got (%g,%g,%g)\n", tag,
              v.x, v.y, v.z, expect.x, expect.y, expect.z,
              got.x, got.y, got.z);
          ok = false;
        }
      }
      CHECK(ok);
      return t;
    };
    auto det3 = [](const BasisCols& b) {
      return b.cols[0][0] * (b.cols[1][1] * b.cols[2][2] -
                             b.cols[1][2] * b.cols[2][1]) -
             b.cols[0][1] * (b.cols[1][0] * b.cols[2][2] -
                             b.cols[1][2] * b.cols[2][0]) +
             b.cols[0][2] * (b.cols[1][0] * b.cols[2][1] -
                             b.cols[1][1] * b.cols[2][0]);
    };

    // identity + translation.
    {
      const float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      const float o[3] = {10.0f, -4.0f, 7.5f};
      const MdkTransform t = checkEquivalence(m, o, "identity");
      CHECK(near(t.basis.cols[0][0], 1) &&
            near(t.basis.cols[1][1], 1) &&
            near(t.basis.cols[2][2], 1));
      CHECK(near(t.origin.x, 4.0) && near(t.origin.y, 7.5) &&
            near(t.origin.z, -10.0));
      CHECK(near(det3(t.basis), 1.0));
    }
    // MDK yaw 90 (forward +X -> +Y): row-major Rz(-90)? The
    // proven object matrix maps +X->+Y, +Y->-X, +Z->+Z.
    {
      const float m[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
      const float o[3] = {0, 0, 0};
      const MdkTransform t = checkEquivalence(m, o, "yaw90");
      CHECK(near(det3(t.basis), 1.0));
      // Godot: a +90 MDK yaw maps local forward (P(+X)=(0,0,-1))
      // to P(+Y)=(-1,0,0) — verified inside checkEquivalence.
    }
    // pitch/bank combo — a nontrivial orthonormal MDK rotation.
    {
      // Rot about MDK +Z (up) by 30 deg then about +X by 20 deg,
      // written out numerically.
      const double a = 30.0 * M_PI / 180.0, b = 20.0 * M_PI / 180.0;
      const float ca = float(std::cos(a)), sa = float(std::sin(a));
      const float cb = float(std::cos(b)), sb = float(std::sin(b));
      // Rz(a)*Rx(b) row-major.
      const float m[9] = {ca, -sa * cb, sa * sb,
                          sa, ca * cb, -ca * sb,
                          0, sb, cb};
      const float o[3] = {-2.0f, 3.0f, 1.0f};
      const MdkTransform t = checkEquivalence(m, o, "pitch/bank");
      CHECK(near(det3(t.basis), 1.0, 1e-4));
    }
    // non-unit scale — det = product of scale factors.
    {
      const float m[9] = {2, 0, 0, 0, 3, 0, 0, 0, 0.5f};
      const float o[3] = {1, 2, 3};
      const MdkTransform t = checkEquivalence(m, o, "scale");
      CHECK(near(det3(t.basis), 3.0));
    }
    // raw arbitrary matrix (the mover raw-matrix path) — general
    // linear map with shear; equivalence is the only contract.
    {
      const float m[9] = {1.0f, 0.3f, 0.0f,
                          0.2f, 1.0f, 0.1f,
                          0.0f, 0.4f, 1.0f};
      const float o[3] = {5.0f, -6.0f, 0.25f};
      checkEquivalence(m, o, "raw/shear");
    }
    // the mover raw-matrix path's zBias origin offset — origin is
    // consumed verbatim (pos + zBias folded by core upstream).
    {
      const float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      const float o[3] = {0, 0, 42.0f};
      const MdkTransform t = checkEquivalence(m, o, "zbias-org");
      CHECK(near(t.origin.y, 42.0));
    }
  }

  // ---- opaque object IDs (mdk_objid.h) ----
  {
    MdkObjectIds ids;
    int storage[4];   // stand-in "objects" — addresses only
    // Pass 1: two live objects mint ids 1,2 in order.
    ids.beginPass();
    ids.markLive(&storage[0]);
    ids.markLive(&storage[1]);
    const std::uint64_t id0 = ids.idFor(&storage[0], 111);
    const std::uint64_t id1 = ids.idFor(&storage[1], 222);
    ids.endPass();
    CHECK(id0 == 1 && id1 == 2 && id0 != id1);
    // Pass 2: same objects -> same ids (frame/transfer stability —
    // the key stays the address regardless of arena list moves).
    ids.beginPass();
    ids.markLive(&storage[0]);
    ids.markLive(&storage[1]);
    CHECK(ids.idFor(&storage[0], 111) == id0);
    CHECK(ids.idFor(&storage[1], 222) == id1);
    ids.endPass();
    CHECK(ids.find(id0) == &storage[0]);
    // Pass 3: storage[1] freed (not marked); its id dies, never
    // reissues.
    ids.beginPass();
    ids.markLive(&storage[0]);
    ids.idFor(&storage[0], 111);
    ids.endPass();
    CHECK(ids.find(id1) == nullptr);
    CHECK(ids.find(id0) == &storage[0]);
    // Address reuse by a DIFFERENT fingerprint -> fresh id (a stale
    // id can never alias a new object).
    ids.beginPass();
    ids.markLive(&storage[1]);   // allocator "reused" the slot
    const std::uint64_t id2 = ids.idFor(&storage[1], 999);
    ids.endPass();
    CHECK(id2 != id1 && ids.find(id2) == &storage[1]);
    CHECK(ids.find(id1) == nullptr);
    // Same-fingerprint reuse keeps the id (indistinguishable from
    // a re-spawn of the same record — documented edge).
    ids.beginPass();
    ids.markLive(&storage[2]);
    const std::uint64_t id3 = ids.idFor(&storage[2], 555);
    ids.endPass();
    ids.beginPass();
    ids.markLive(&storage[2]);
    CHECK(ids.idFor(&storage[2], 555) == id3);
    ids.endPass();
    // Full prune: nothing live -> map empties; next mint continues
    // the counter (no id reuse ever).
    ids.beginPass();
    ids.endPass();
    CHECK(ids.find(id0) == nullptr && ids.find(id2) == nullptr &&
          ids.find(id3) == nullptr);
    ids.beginPass();
    ids.markLive(&storage[3]);
    const std::uint64_t id4 = ids.idFor(&storage[3], 111);
    ids.endPass();
    CHECK(id4 != id0 && id4 != id1 && id4 != id2 && id4 != id3);
  }

  // ---- real DynamicArena::transfer keeps the opaque id ----
  {
    // FUN_004574d0 splices the storage node across arenas — the
    // DynamicObject address is stable, so an address-keyed opaque id
    // survives the move with no re-mint and no duplicate.
    mdk::DynamicArena a, b;
    a.name = "SRC";
    b.name = "DST";
    mdk::DynamicObject& o = a.allocFront();
    o.enemyIndex = 30;
    o.spawnId = 9;
    mdk::DynamicObject& p = a.allocFront();
    p.enemyIndex = 31;
    p.spawnId = 0;

    MdkObjectIds ids;
    ids.beginPass();
    ids.markLive(&o);
    ids.markLive(&p);
    const std::uint64_t oid = ids.idFor(&o, 0xA1);
    const std::uint64_t pid = ids.idFor(&p, 0xB2);
    ids.endPass();

    a.transfer(o, b);
    CHECK(o.arena == &b);
    CHECK(b.col.objects == &o.col);      // pushed front onto dst
    CHECK(o.col.next == nullptr);
    CHECK(a.col.objects == &p.col);      // src list relinked
    CHECK(p.col.next == nullptr);
    bool oStillInA = false;
    for (const auto& up : a.storage) oStillInA |= (up.get() == &o);
    CHECK(!oStillInA);
    bool oInB = false;
    for (const auto& up : b.storage) oInB |= (up.get() == &o);
    CHECK(oInB);

    // Next pass over the post-transfer view: same address -> same id;
    // no second id ever points at o.
    ids.beginPass();
    ids.markLive(&o);
    ids.markLive(&p);
    CHECK(ids.idFor(&o, 0xA1) == oid);
    CHECK(ids.idFor(&p, 0xB2) == pid);
    ids.endPass();
    CHECK(ids.find(oid) == &o && ids.find(pid) == &p);
  }

  // ================================================================
  // Phase 18B.2B — attract-slide GIF decode (corpus shape: GIF87a/89a,
  // one image, GCT only, non-interlaced, exactly 600x360)
  // ================================================================
  {
    // Minimal GIF builder: header + LSD + 256-entry GCT + one image
    // descriptor + LZW(min 8) sub-blocks + trailer. `codes` are the
    // LZW code stream; packing mirrors the decoder's width growth
    // (add one dict entry per code after the epoch's first; bump
    // width when next == 1<<width).
    auto packLzw = [](const std::vector<int>& codes) {
      std::vector<std::uint8_t> out;
      std::uint32_t acc = 0;
      int accBits = 0;
      int codeSize = 9, next = 258;
      bool havePrev = false;
      auto emit = [&](int c) {
        acc |= std::uint32_t(c) << accBits;
        accBits += codeSize;
        while (accBits >= 8) {
          out.push_back(std::uint8_t(acc & 0xff));
          acc >>= 8;
          accBits -= 8;
        }
        if (c == 256) {   // clear
          next = 258; codeSize = 9; havePrev = false;
        } else if (!havePrev) {
          havePrev = true;
        } else if (++next == (1 << codeSize) && codeSize < 12) {
          ++codeSize;
        }
      };
      for (int c : codes) emit(c);
      if (accBits > 0) out.push_back(std::uint8_t(acc & 0xff));
      return out;
    };
    auto buildGif = [&](const std::vector<int>& codes, int w, int h,
                        int ipacked, bool trailer, bool gct) {
      std::vector<std::uint8_t> g;
      auto put = [&](std::initializer_list<int> bs) {
        for (int v : bs) g.push_back(std::uint8_t(v));
      };
      put({'G', 'I', 'F', '8', '9', 'a'});
      put({w & 0xff, w >> 8, h & 0xff, h >> 8,
           gct ? 0xf7 : 0x00, 0, 0});
      if (gct) {
        for (int i = 0; i < 768; ++i) g.push_back(std::uint8_t(i));
      }
      put({0x2c, 0, 0, 0, 0, w & 0xff, w >> 8, h & 0xff, h >> 8,
           ipacked});
      g.push_back(8);   // LZW min code size
      const auto stream = packLzw(codes);
      for (std::size_t p = 0; p < stream.size(); p += 255) {
        const int n = int(std::min<std::size_t>(255,
                                                stream.size() - p));
        g.push_back(std::uint8_t(n));
        g.insert(g.end(), stream.begin() + p, stream.begin() + p + n);
      }
      g.push_back(0);   // block terminator
      if (trailer) g.push_back(0x3b);
      return g;
    };
    // Literal-only code stream: clear + k literals, repeated.
    auto literalStream = [](int pixels, int litBase,
                            std::vector<std::uint8_t>* expected) {
      std::vector<int> codes;
      expected->clear();
      int emitted = 0;
      while (emitted < pixels) {
        codes.push_back(256);                    // clear
        const int n = std::min(200, pixels - emitted);
        for (int i = 0; i < n; ++i) {
          const int lit = (litBase + emitted + i) & 0xff;
          codes.push_back(lit);
          expected->push_back(std::uint8_t(lit));
        }
        emitted += n;
      }
      codes.push_back(257);                      // eoi
      return codes;
    };

    // Valid still: literals-only 600x360, palette echo.
    {
      std::vector<std::uint8_t> expected;
      const auto g = buildGif(literalStream(600 * 360, 3, &expected),
                              600, 360, 0, true, true);
      const auto img = mdkbridge::decodeGifImage(g);
      CHECK(img && img->hasPalette);
      CHECK(img->width == 600 && img->height == 360);
      CHECK(img->pixels == expected);
      CHECK(img->palette[0].r == 0 && img->palette[0].g == 1 &&
            img->palette[0].b == 2);
      CHECK(img->palette[255].r == 765 % 256);
    }

    // Dictionary + KwKwK codes: epoch = clear, lits 0..255 (dict
    // [258+k] = (k,k+1)), then dict codes 258..511 (emit pairs),
    // then 513 == next (KwKwK -> prev string + its first char),
    // then literals to fill.
    {
      std::vector<int> codes = {256};
      std::vector<std::uint8_t> expected;
      for (int i = 0; i <= 255; ++i) {
        codes.push_back(i);
        expected.push_back(std::uint8_t(i));
      }
      for (int k = 0; k <= 253; ++k) {          // codes 258..511
        codes.push_back(258 + k);
        expected.push_back(std::uint8_t(k));
        expected.push_back(std::uint8_t(k + 1));
      }
      // next is now 767 -> emitting 767 is the KwKwK case:
      // prev string [253,254] + its first char.
      codes.push_back(767);
      expected.push_back(253);
      expected.push_back(254);
      expected.push_back(253);
      // Fill to 216000 with literals (fresh epoch each 200 codes).
      while (expected.size() < 600 * 360) {
        codes.push_back(256);
        const int n =
            std::min<int>(200, int(600 * 360 - expected.size()));
        for (int i = 0; i < n; ++i) {
          codes.push_back(7);
          expected.push_back(7);
        }
      }
      codes.push_back(257);
      const auto g = buildGif(codes, 600, 360, 0, true, true);
      const auto img = mdkbridge::decodeGifImage(g);
      CHECK(img && img->pixels == expected);
    }

    // Rejects — every non-corpus shape.
    {
      std::vector<std::uint8_t> expected;
      const auto ok = buildGif(literalStream(600 * 360, 0, &expected),
                               600, 360, 0, true, true);
      auto bad = ok; bad[0] = 'X';
      CHECK(!mdkbridge::decodeGifImage(bad));          // bad sig
      CHECK(!mdkbridge::decodeGifImage(
          {ok.data(), ok.size() - 1}));                // truncated tail
      CHECK(!mdkbridge::decodeGifImage({}));
      // Wrong logical dims.
      CHECK(!mdkbridge::decodeGifImage(
          buildGif(literalStream(320 * 200, 0, &expected),
                   320, 200, 0, true, true)));
      // Interlaced / local-CT image descriptors.
      CHECK(!mdkbridge::decodeGifImage(
          buildGif(literalStream(600 * 360, 0, &expected),
                   600, 360, 0x40, true, true)));
      CHECK(!mdkbridge::decodeGifImage(
          buildGif(literalStream(600 * 360, 0, &expected),
                   600, 360, 0x80, true, true)));
      // No color table at all.
      CHECK(!mdkbridge::decodeGifImage(
          buildGif(literalStream(600 * 360, 0, &expected),
                   600, 360, 0, true, false)));
      // Missing trailer.
      CHECK(!mdkbridge::decodeGifImage(
          buildGif(literalStream(600 * 360, 0, &expected),
                   600, 360, 0, false, true)));
      // A second image descriptor after the first (animation).
      {
        std::vector<std::uint8_t> multi;
        const std::size_t hdrEnd = 13 + 768;   // header+LSD+GCT
        multi.insert(multi.end(), ok.begin(), ok.begin() + hdrEnd);
        // image 1 = ok's descriptor+data .. up to (not incl.) trailer
        multi.insert(multi.end(), ok.begin() + hdrEnd,
                     ok.end() - 1);
        // image 2 = another descriptor (just the descriptor bytes is
        // enough to trip the multi-image reject — decode fails on
        // the second 0x2c before reading its data)
        multi.insert(multi.end(), {0x2c, 0, 0, 0, 0,
                                   600 & 0xff, 600 >> 8,
                                   360 & 0xff, 360 >> 8, 0, 8});
        multi.push_back(0);
        multi.push_back(0x3b);
        CHECK(!mdkbridge::decodeGifImage(multi));
      }
      // Truncated image data (stream ends mid-LZW).
      {
        auto shorty = buildGif(literalStream(600 * 360, 0, &expected),
                               600, 360, 0, true, true);
        shorty.resize(shorty.size() / 2);
        CHECK(!mdkbridge::decodeGifImage(shorty));
      }
      // Sub-rect descriptor (offset origin) — not the corpus shape.
      {
        std::vector<std::uint8_t> g;
        auto put = [&](std::initializer_list<int> bs) {
          for (int v : bs) g.push_back(std::uint8_t(v));
        };
        put({'G', 'I', 'F', '8', '7', 'a'});
        put({600 & 0xff, 600 >> 8, 360 & 0xff, 360 >> 8, 0xf7, 0, 0});
        for (int i = 0; i < 768; ++i) g.push_back(std::uint8_t(i));
        // image at offset (4,4) — legal GIF, outside the corpus.
        put({0x2c, 4, 0, 4, 0, 592 & 0xff, 592 >> 8,
             352 & 0xff, 352 >> 8, 0});
        g.push_back(8);
        const auto s = packLzw(literalStream(592 * 352, 0, &expected));
        for (std::size_t p = 0; p < s.size(); p += 255) {
          const int n = int(std::min<std::size_t>(255, s.size() - p));
          g.push_back(std::uint8_t(n));
          g.insert(g.end(), s.begin() + p, s.begin() + p + n);
        }
        g.push_back(0);
        g.push_back(0x3b);
        CHECK(!mdkbridge::decodeGifImage(g));
      }
    }
  }

  // ---- Phase 19B.1 — StreamPresenter synthetic tests --------------
  // Every fixture is synthetic: indexed images, a 768B DAC image, and
  // two-glyph FTI fonts. No proprietary data. The events are the
  // core's StreamEvent PODs built field-by-field — the same surface
  // the bridge drains.
  {
    using mdk::StreamEvent;
    mdkbridge::StreamPresenter pres;

    auto mkEv = [](StreamEvent::Kind k) {
      StreamEvent e;
      e.kind = k;
      return e;
    };

    // --- palette application (kPaletteSet) -------------------------
    // A DAC image the core would hand the host: index i -> (i,255-i,
    // i*2). The presenter copies it verbatim — the transform math
    // stays in core (paletteRamp is CLOSED there).
    std::uint8_t dac[768];
    for (int i = 0; i < 256; ++i) {
      dac[i * 3 + 0] = std::uint8_t(i);
      dac[i * 3 + 1] = std::uint8_t(255 - i);
      dac[i * 3 + 2] = std::uint8_t(i * 2);
    }
    {
      StreamEvent e = mkEv(StreamEvent::kPaletteSet);
      pres.consume(e, dac);
      const auto& d = pres.diag();
      CHECK(d.paletteSets == 1);
      CHECK(d.paletteHash ==
            mdk::fnv1a64(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(dac), 768)));
      const auto& pal = pres.palette();
      CHECK(pal.get(7).r == 7 && pal.get(7).g == 248 &&
            pal.get(7).b == 14 && pal.get(7).a == 255);
      // Palette update does not touch the indexed framebuffer.
      const auto& fb0 = pres.framebuffer();
      bool fbClean = true;
      for (std::size_t i = 0; i < fb0.pixelCount(); ++i)
        fbClean &= fb0.pixels()[i] == 0;
      CHECK(fbClean);
    }

    // --- toroidal backdrop blit (kBackdropBlit) --------------------
    // dst(x,y) = src((x+U) mod 600, (y+V) mod 360) — the two-piece
    // e684 copy. Synthetic image: pixel (x,y) holds (x^y)&0xff.
    {
      mdk::IndexedImage bg;
      bg.width = 600;
      bg.height = 360;
      bg.stride = 600;
      bg.pixels.resize(600 * 360);
      for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 600; ++x)
          bg.pixels[std::size_t(y) * 600 + x] =
              std::uint8_t((x ^ y) & 0xff);
      pres.bindImage(10, bg);
      StreamEvent e = mkEv(StreamEvent::kBackdropBlit);
      e.tag = 10;
      e.f[0] = 137.0f;              // U
      e.f[1] = 201.0f;              // V
      pres.consume(e, dac);
      const auto& fb = pres.framebuffer();
      auto at = [&](int x, int y) { return fb.at(x, y); };
      CHECK(at(0, 0) == std::uint8_t((137 ^ 201) & 0xff));
      CHECK(at(463, 0) == std::uint8_t((0 ^ 201) & 0xff));   // wrap x
      CHECK(at(0, 159) == std::uint8_t((137 ^ 0) & 0xff));   // wrap y
      CHECK(at(599, 359) ==
            std::uint8_t(((599 + 137) % 600 ^
                          (359 + 201) % 360) & 0xff));
      CHECK(pres.diag().backdropBlits == 1);
    }

    // --- sprite draw: center anchor, 8.8 size, pen-0 key, order ----
    {
      // 8x8 image, pen 0x80; a hole at (3,3) stays transparent.
      mdk::IndexedImage spr;
      spr.width = 8;
      spr.height = 8;
      spr.stride = 8;
      spr.pixels.assign(64, 0x80);
      spr.pixels[3 * 8 + 3] = 0;
      pres.bindImage(20, spr);

      // Lay a marker region first so the pen-0 hole is observable.
      for (int y = 50; y < 80; ++y)
        for (int x = 90; x < 115; ++x)
          pres.framebuffer().put(x, y, 0x11);

      // size 0x100 = 1:1 — effW = 8*256>>8 = 8, center-anchored.
      StreamEvent e = mkEv(StreamEvent::kSpriteDraw);
      e.tag = 20;
      e.f[0] = 100.0f;
      e.f[1] = 60.0f;
      e.f[2] = 256.0f;
      pres.consume(e, dac);
      const auto& fb = pres.framebuffer();
      CHECK(fb.at(100, 60) == 0x80);          // covered by the sprite
      CHECK(fb.at(96, 56) == 0x80);           // top-left = c-4
      CHECK(fb.at(103, 63) == 0x80);          // bottom-right = c+3
      CHECK(fb.at(95, 56) == 0x11);           // left of the sprite
      CHECK(fb.at(104, 63) == 0x11);          // right of it
      CHECK(fb.at(99, 59) == 0x11);           // (3,3) hole: pen 0 skips
      CHECK(pres.diag().sprites == 1 &&
            pres.diag().spriteMisses == 0);

      // Painter order: a second sprite drawn over the same area wins
      // (consume order is the core's far-first emission order).
      mdk::IndexedImage spr2 = spr;
      spr2.pixels.assign(64, 0x90);
      pres.bindImage(21, spr2);
      e.tag = 21;
      pres.consume(e, dac);
      CHECK(fb.at(100, 60) == 0x90);

      // 2x scale: size 0x200 -> effW = 8*512>>8 = 16, center holds.
      StreamEvent e2 = mkEv(StreamEvent::kSpriteDraw);
      e2.tag = 21;
      e2.f[0] = 300.0f;
      e2.f[1] = 200.0f;
      e2.f[2] = 512.0f;
      pres.consume(e2, dac);
      CHECK(fb.at(300, 200) == 0x90);
      CHECK(fb.at(292, 192) == 0x90);         // c-8 corner
      CHECK(fb.at(307, 207) == 0x90);         // c+7 corner
      CHECK(fb.at(291, 192) != 0x90);

      // Unbound tag -> counted miss, no draw.
      StreamEvent e3 = mkEv(StreamEvent::kSpriteDraw);
      e3.tag = 999;
      e3.f[0] = 50.0f;
      e3.f[1] = 50.0f;
      e3.f[2] = 256.0f;
      const int spritesBefore = pres.diag().sprites;
      pres.consume(e3, dac);
      CHECK(pres.diag().sprites == spritesBefore + 1 &&
            pres.diag().spriteMisses >= 1);
    }

    // --- 19B.1A: sprite outcome classification + miss census ------
    {
      pres.reset();
      using SR = mdkbridge::StreamSpriteResult;
      using CK = mdkbridge::StreamSpriteMissKey;
      auto mkImg = [](int w, int h, std::uint8_t pen) {
        mdk::IndexedImage im;
        im.width = w;
        im.height = h;
        im.stride = w;
        im.pixels.assign(std::size_t(w) * h, pen);
        return im;
      };
      auto censusHas = [&](const mdkbridge::StreamPresenter& p,
                           SR cls, int tag, int w, int h,
                           int count) {
        const auto it = p.diag().spriteCensus.find(
            CK{int(cls), tag, w, h});
        return it != p.diag().spriteCensus.end() &&
               it->second.count == count;
      };

      // LIGHT-shaped source (the debris family): 64x64, opaque.
      // Correct tag resolution -> kDrew (not a miss, no census row).
      pres.bindImage(11, mkImg(64, 64, 0x55));
      {
        StreamEvent e = mkEv(StreamEvent::kSpriteDraw);
        e.tag = 11;
        e.f[0] = 300.0f; e.f[1] = 180.0f; e.f[2] = 256.0f;
        pres.consume(e, dac);
        CHECK(pres.diag().spriteDrawn == 1 &&
              pres.diag().spriteMisses == 0);
        CHECK(pres.framebuffer().at(300, 180) == 0x55);
        CHECK(pres.framebuffer().at(268, 148) == 0x55);  // 64px box
        CHECK(pres.framebuffer().at(267, 148) == 0);
        CHECK(pres.diag().spriteCensus.empty());
      }

      // PLANET-shaped source (the marker family): 128x128, opaque.
      pres.bindImage(4, mkImg(128, 128, 0x66));
      {
        StreamEvent e = mkEv(StreamEvent::kSpriteDraw);
        e.tag = 4;
        e.f[0] = 300.0f; e.f[1] = 180.0f; e.f[2] = 128.0f;
        pres.consume(e, dac);   // 0.5x -> 64x64 footprint
        CHECK(pres.diag().spriteDrawn == 2);
        CHECK(pres.framebuffer().at(268, 148) == 0x66);
        CHECK(pres.framebuffer().at(267, 148) == 0);     // past edge
      }

      // Source dims are the bound image's own: a non-square source
      // scales each axis independently (srcW!=srcH -> non-square
      // footprint at a square dst size).
      pres.bindImage(40, mkImg(16, 8, 0x77));
      {
        StreamEvent e = mkEv(StreamEvent::kSpriteDraw);
        e.tag = 40;
        e.f[0] = 100.0f; e.f[1] = 40.0f; e.f[2] = 256.0f;  // 16x8
        pres.consume(e, dac);
        CHECK(pres.diag().spriteDrawn == 3);
        CHECK(pres.framebuffer().at(92, 36) == 0x77);   // c-(8,4)
        CHECK(pres.framebuffer().at(107, 43) == 0x77);  // c+(7,3)
        CHECK(pres.framebuffer().at(91, 36) == 0);      // 16 wide
        CHECK(pres.framebuffer().at(92, 35) == 0);      // 8 tall
      }

      // Copy-safe retained lifetime: mutating the caller's image
      // after bind must not affect the retained table copy.
      {
        mdk::IndexedImage mut = mkImg(4, 4, 0x21);
        pres.bindImage(50, mut);
        mut.pixels.assign(16, 0x99);   // caller-side mutation
        StreamEvent e = mkEv(StreamEvent::kSpriteDraw);
        e.tag = 50;
        e.f[0] = 500.0f; e.f[1] = 60.0f; e.f[2] = 256.0f;
        pres.consume(e, dac);
        CHECK(pres.framebuffer().at(500, 60) == 0x21);
      }

      // Faithful no-draw classes — none are resource misses.
      {
        const int drawn0 = pres.diag().spriteDrawn;

        // Zero-size: dst size 3 on a 64px source -> effW/H = 0.
        StreamEvent e = mkEv(StreamEvent::kSpriteDraw);
        e.tag = 11;
        e.f[0] = 300.0f; e.f[1] = 180.0f; e.f[2] = 3.0f;
        pres.consume(e, dac);
        CHECK(pres.diag().spriteZeroSize == 1 &&
              pres.diag().spriteMisses == 0);
        CHECK(censusHas(pres, SR::kZeroSize, 11, 64, 64, 1));

        // Clipped: dest rect entirely right of the view.
        e.f[0] = 2000.0f; e.f[1] = 180.0f; e.f[2] = 256.0f;
        pres.consume(e, dac);
        CHECK(pres.diag().spriteClipped == 1 &&
              pres.diag().spriteMisses == 0);

        // Transparent: bound, onscreen, every source byte pen 0 —
        // the raster runs but commits nothing.
        pres.bindImage(60, mkImg(8, 8, 0x00));
        e.tag = 60;
        e.f[0] = 300.0f; e.f[1] = 60.0f; e.f[2] = 256.0f;
        pres.consume(e, dac);
        CHECK(pres.diag().spriteTransparent == 1 &&
              pres.diag().spriteMisses == 0);
        CHECK(censusHas(pres, SR::kTransparent, 60, 8, 8, 1));

        // TRUE misses:
        // resource — the tag was never bound.
        e.tag = 999;
        e.f[0] = 300.0f; e.f[1] = 60.0f; e.f[2] = 256.0f;
        pres.consume(e, dac);
        CHECK(pres.diag().spriteMissRes == 1 &&
              pres.diag().spriteMisses == 1);
        CHECK(censusHas(pres, SR::kMissResource, 999, 0, 0, 1));

        // metadata — bound under a tag but the image is unusable
        // (empty pixels / nonpos dims).
        mdk::IndexedImage bad;
        bad.width = 8;
        bad.height = 8;                 // dims claim 8x8, no pixels
        pres.bindImage(70, bad);
        e.tag = 70;
        pres.consume(e, dac);
        CHECK(pres.diag().spriteMissMeta == 1 &&
              pres.diag().spriteMisses == 2);
        CHECK(censusHas(pres, SR::kMissMetadata, 70, 8, 8, 1));

        // The census accumulates: a repeat of the same bucket grows
        // count and widens the size range, never duplicates rows.
        e.tag = 11;
        e.f[0] = 300.0f; e.f[1] = 180.0f; e.f[2] = 1.0f;
        pres.consume(e, dac);
        {
          const auto it = pres.diag().spriteCensus.find(
              CK{int(SR::kZeroSize), 11, 64, 64});
          CHECK(it != pres.diag().spriteCensus.end() &&
                it->second.count == 2 &&
                it->second.sizeMin == 1 && it->second.sizeMax == 3);
        }
        CHECK(pres.diag().spriteDrawn == drawn0);
        // sprites == drawn + misses + faithful non-draws.
        const auto& dd = pres.diag();
        CHECK(dd.spriteDrawn + dd.spriteMisses + dd.spriteZeroSize +
                  dd.spriteClipped + dd.spriteTransparent ==
              dd.sprites);
      }
    }

    // --- HUD subrect blit (kHudBlit) -------------------------------
    {
      pres.reset();
      // 80x4 digit strip — cell d starts at byte offset d*8.
      mdk::IndexedImage strip;
      strip.width = 80;
      strip.height = 4;
      strip.stride = 80;
      strip.pixels.assign(320, 0x00);
      for (int d = 0; d < 10; ++d)
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 8; ++c)
            strip.pixels[std::size_t(r) * 80 + d * 8 + c] =
                std::uint8_t(0x40 + d);
      pres.bindImage(30, strip);
      StreamEvent e = mkEv(StreamEvent::kHudBlit);
      e.tag = 30;
      e.aux = 5 * 8;                          // srcOff = digit 5 cell
      e.f[0] = 20.0f;                         // dstX
      e.f[1] = 30.0f;                         // dstY
      e.f[2] = 8.0f;                          // w
      e.f[3] = 4.0f;                          // h
      e.f[4] = 80.0f;                         // srcStride (strip width)
      e.f[5] = 0.0f;                          // transparent key
      pres.consume(e, dac);
      const auto& fb = pres.framebuffer();
      CHECK(fb.at(20, 30) == 0x45);
      CHECK(fb.at(27, 33) == 0x45);
      CHECK(fb.at(28, 30) == 0);              // past cell w
      CHECK(pres.diag().hudBlits == 1 &&
            pres.diag().hudMisses == 0);

      // Pen-key skip: a 0 byte in the cell stays transparent — the
      // second blit leaves the first blit's 0x45 in place (a non-keyed
      // copy would overwrite it with 0).
      mdk::IndexedImage s2 = strip;
      s2.pixels[1 * 80 + 5 * 8 + 0] = 0;
      pres.bindImage(30, s2);
      pres.consume(e, dac);
      CHECK(fb.at(20, 31) == 0x45);           // keyed byte skipped
    }

    // --- TELETYPE (kTeletypeDraw) ----------------------------------
    {
      pres.reset();
      // Synthetic fonts: FONTBIG 'A' = 8x13 solid pen 0x77 (advance
      // = width); FONTSML 'z' = 4x6 pen 0x33. Everything else falls
      // back to the missing-glyph advance (6 / 4).
      mdk::FtiFont fontBig{}, fontSml{};
      fontBig.glyphs.resize(256);
      fontSml.glyphs.resize(256);
      {
        mdk::FtiGlyph g;
        g.code = 'A';
        g.top = 10;
        g.bottom = 2;                         // rows = 13
        g.width = 8;
        g.pixels.assign(8 * 13, 0x77);
        fontBig.glyphs['A'] = g;
        fontBig.mappedCount = 1;
        fontBig.firstMapped = fontBig.lastMapped = 'A';
      }
      {
        mdk::FtiGlyph g;
        g.code = 'z';
        g.top = 4;
        g.bottom = 1;                         // rows = 6
        g.width = 4;
        g.pixels.assign(4 * 6, 0x33);
        fontSml.glyphs['z'] = g;
        fontSml.mappedCount = 1;
        fontSml.firstMapped = fontSml.lastMapped = 'z';
      }
      pres.bindFonts(fontBig, fontSml);

      // Renderer 0, short line: FONTBIG centered — "AA" measures 16,
      // x = (600-16)/2 = 292; pen y = 100 puts glyph rows 90..102.
      StreamEvent e = mkEv(StreamEvent::kTeletypeDraw);
      e.tag = 0;
      e.aux = 100;
      e.f[0] = 0.0f;
      e.name = "AA";
      pres.consume(e, dac);
      const auto& fb = pres.framebuffer();
      CHECK(fb.at(292, 90) == 0x77);          // centered origin
      CHECK(fb.at(299, 102) == 0x77);         // glyph 0 bottom-right
      CHECK(fb.at(300, 90) == 0x77);          // glyph 1 starts
      CHECK(fb.at(291, 90) == 0);
      CHECK(pres.diag().teletypeDraws == 1);

      // Renderer 0 fallback: unmapped bytes measure 6 each in
      // FONTBIG — 120 * 6 = 720 >= 600 -> FONTSML draws (advance 4,
      // 'z' mapped): 119*4 + 4 = 480 wide, x = 60.
      pres.reset();
      pres.bindFonts(fontBig, fontSml);
      e.name = std::string(120, 'z');
      pres.consume(e, dac);
      CHECK(fb.at(60, 100 - 4) == 0x33);
      CHECK(fb.at(59, 100 - 4) == 0);

      // Renderer 1 (scaled): "A" at scale 2.0 -> w = trunc(8*2)=16,
      // x = trunc((600-16)/2) = 292; glyphTopY = trunc(100-10*2) = 80
      // and the glyph scales to 16 px wide.
      pres.reset();
      pres.bindFonts(fontBig, fontSml);
      e.tag = 1;
      e.f[0] = 2.0f;
      e.name = "A";
      pres.consume(e, dac);
      CHECK(fb.at(292, 80) == 0x77);
      CHECK(fb.at(307, 80) == 0x77);          // 16-px wide glyph
      CHECK(fb.at(308, 80) == 0);
      CHECK(fb.at(291, 80) == 0);
    }

    // --- present boundary + digests --------------------------------
    {
      pres.reset();
      // Model/ribbon carry-through: counted, framebuffer untouched.
      StreamEvent m = mkEv(StreamEvent::kModelDraw);
      StreamEvent r = mkEv(StreamEvent::kRibbonTri);
      pres.consume(m, dac);
      pres.consume(r, dac);
      CHECK(pres.diag().modelsDeferred == 1 &&
            pres.diag().ribbonsDeferred == 1);
      bool clean = true;
      for (std::size_t i = 0; i < pres.framebuffer().pixelCount();
           ++i)
        clean &= pres.framebuffer().pixels()[i] == 0;
      CHECK(clean && !pres.framePending());

      StreamEvent p = mkEv(StreamEvent::kPresent);
      pres.consume(p, dac);
      CHECK(pres.diag().presented == 1);
      CHECK(pres.framePending());
      const std::uint64_t h0 = pres.diag().fbHash;
      pres.clearFramePending();
      CHECK(!pres.framePending());
      // A non-present event does not advance the fb digest.
      StreamEvent m2 = mkEv(StreamEvent::kModelDraw);
      pres.consume(m2, dac);
      CHECK(pres.diag().fbHash == h0);
      // Mutate the fb then present again — the digest tracks it.
      pres.framebuffer().put(0, 0, 0x55);
      pres.consume(p, dac);
      CHECK(pres.diag().presented == 2);
      CHECK(pres.diag().fbHash != h0);
    }

    // --- terminal fill (kExitMode) ----------------------------------
    {
      pres.reset();
      // The core memsets paletteDac before the event — the host sees
      // the uniform 768B table. White exit (alive, non-final).
      std::uint8_t fillPal[768];
      std::memset(fillPal, 0xff, sizeof fillPal);
      StreamEvent e = mkEv(StreamEvent::kExitMode);
      e.aux = 0xff;
      pres.consume(e, fillPal);
      CHECK(pres.diag().terminalFills == 1);
      CHECK(pres.diag().terminalFill == 0xff);
      CHECK(pres.palette().get(0).r == 255 &&
            pres.palette().get(0).g == 255 &&
            pres.palette().get(0).b == 255);
      CHECK(pres.framePending());   // terminal frame still uploads

      // Black exit (final/dead course).
      pres.reset();
      std::memset(fillPal, 0x00, sizeof fillPal);
      e.aux = 0x00;
      pres.consume(e, fillPal);
      CHECK(pres.diag().terminalFill == 0x00);
      CHECK(pres.palette().get(200).r == 0 &&
            pres.palette().get(200).g == 0 &&
            pres.palette().get(200).b == 0);
    }
  }

  if (gFailures == 0) {
    std::printf("%d checks, 0 failures\n", gChecks);
    return 0;
  }
  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  return 1;
}
