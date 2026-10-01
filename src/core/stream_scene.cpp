// stream_scene.cpp — Phase 19A.1A: mode-5 cinematic math/camera layer.
//
// This file implements ONLY the helper layer declared in stream_scene.h
// (the 0x42xxxx/0x46xxxx native helper ports). Pool/lifecycle, the
// script-facing updaters, and the draw pipeline are later phases.
//
// All formulas are instruction-level ports of the captured disassembly
// (analysis-private/logs/p19a_asm1/4/5.txt); quirks are preserved and
// labeled at the point of use. x87 note, same convention as
// player_camera.cpp: the original computes each FLD/FMUL/FADD chain in
// 80-bit and rounds once at the FSTP f32 store; this port accumulates
// each chain in double and casts at the same store points. Where the
// native stores an f32 intermediate mid-chain (e9c4's scale, dc68's
// frac) the port rounds to float there as well.
//
// Matrix convention (OBSERVED): row-major 3x4, translation in elements
// 3/7/11; a point transforms as out = M * v (row-dot) + t.

#include "core/stream_scene.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <new>

#include "core/enemy_runtime.h"

namespace mdk {
namespace {

// DAT_00497924 — the stored f64 deg->rad constant read by FUN_00437f98.
constexpr double kDegToRad = std::bit_cast<double>(0x3f91df46a2529d35ULL);

// cameraAt (de28) writes the view-config globals every call; they are
// compile-time constants in the port. OBSERVED values:
//   0x540b58/0x540b64 = 0x4019999a = 2.4f      (zoom / projection dist)
//   0x540b68/0x540b6c = 600 x 360              (viewport)
//   0x540b70/0x540b74 = 300 x 180              (viewport center)
//   0x540b78/0x540b7c = 0,0                    (viewport origin)
//   0x497100 = 0.5f, 0x497104 = 360.0f,
//   0x497108 = 0x3ada740e = 1/600f, 0x497110 = 0.5 (f64)
//   0x540bf8 = 1.0f                            (row-2 scale, unscaled)
constexpr float kCamZoom = 2.4f;
constexpr float kCamHalf = 0.5f;
constexpr float kCamHeight = 360.0f;
constexpr float kCamInvWidth = 0.0016666667f;  // 1/600 — raw 0x3ada740e
constexpr double kCamHalf64 = 0.5;
static_assert(std::bit_cast<std::uint32_t>(kCamInvWidth) == 0x3ada740e);
static_assert(std::bit_cast<std::uint32_t>(kCamZoom) == 0x4019999a);

// DAT_004970d8 — wallProbe's plane margin (f64).
constexpr double kWallMargin = -1.5;

// The inlined a x b product sequence shared by dcf4 and de28: six f80
// products, FXCH/FSUBP tree, three f32 stores.
void cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = static_cast<float>(static_cast<double>(a[1]) * b[2] -
                              static_cast<double>(a[2]) * b[1]);
  out[1] = static_cast<float>(static_cast<double>(a[2]) * b[0] -
                              static_cast<double>(a[0]) * b[2]);
  out[2] = static_cast<float>(static_cast<double>(a[0]) * b[1] -
                              static_cast<double>(a[1]) * b[0]);
}

// ---------------------------------------------------------------------------
// Phase 19A.1B — pool/lifecycle link helpers.
//
// BUILD_A chains the 0x4f0740 pool through the collision record's +0x00
// word — CollisionObject::next in this port. `col` is DynamicObject's
// first member, so a stored &next->col addresses the owning record
// itself: the round-trip below is the whole container-of indirection,
// kept in one place (std::launder covers the in-place record resets).
// ---------------------------------------------------------------------------
DynamicObject* streamNext(const DynamicObject* o) {
  if (!o || !o->col.next) return nullptr;
  return std::launder(reinterpret_cast<DynamicObject*>(
      const_cast<CollisionObject*>(o->col.next)));
}

void streamSetNext(DynamicObject* o, DynamicObject* next) {
  o->col.next = next ? &next->col : nullptr;
}

// Scatter a row-major 3x4 into the record's +0xac..+0xd8 block — the
// native writes the 12 floats contiguously, which maps to col.xform[9]
// with translation elements 3/7/11 landing on col.origin[0..2]
// (+0xb8/+0xc8/+0xd8).
void streamStoreMatrix(DynamicObject& o, const float m[12]) {
  o.col.xform[0] = m[0];
  o.col.xform[1] = m[1];
  o.col.xform[2] = m[2];
  o.col.origin[0] = m[3];
  o.col.xform[3] = m[4];
  o.col.xform[4] = m[5];
  o.col.xform[5] = m[6];
  o.col.origin[1] = m[7];
  o.col.xform[6] = m[8];
  o.col.xform[7] = m[9];
  o.col.xform[8] = m[10];
  o.col.origin[2] = m[11];
}

// f64 constants read by FUN_0042c578 (OBSERVED image bytes):
//   0x496fb8 = 2^-12  debris lateral-offset rand range
//   0x496fc0 = 10.0   +0x20 frac scale (c578)
//   0x496fc8 = 2^-13  scale/+0x34 rand range
//   0x496fd0 = 4.0    scale base
//   0x496fd8 = 10.0   +0x20 frac scale (c6f0 — same value, own slot)
constexpr double kDebrisRandRange = 0x1p-12;
constexpr double kFracScaleDebris = 10.0;
constexpr double kSpawnRandRange = 0x1p-13;
constexpr double kDebrisScaleBase = 4.0;
constexpr double kFracScaleMarker = 10.0;

} // namespace

// FUN_0047d59a — saves the x87 control word, sets RC=11 (truncate),
// FRNDINT, restores. Truncation toward zero, not round-nearest.
float StreamScene::streamFrndInt(float v) {
  return std::trunc(v);
}

// FUN_00437f98 — rad = f32(deg) * f64(0x497924); first out pointer gets
// sin, the second cos.
void StreamScene::sincos37f98(float deg, float* s, float* c) {
  const double rad = static_cast<double>(deg) * kDegToRad;
  *s = static_cast<float>(std::sin(rad));
  *c = static_cast<float>(std::cos(rad));
}

// FUN_0046b180 — 3x4 euler matrix. Positional args map to the native
// stack slots arg8/arg_c/arg_10 (sin/cos pairs sA,cA / sB,cB / sC,cC).
// SCALING QUIRK (OBSERVED): out[3]/out[7] = t*S but out[11] = t[2]
// raw — the final FMUL is absent at 0x46b25e..0x46b261.
void StreamScene::euler6b180(float a3, float a2, float a1, float s,
                             const float t[3], float out[12]) {
  float sA, cA, sB, cB, sC, cC;
  sincos37f98(a3, &sA, &cA);
  sincos37f98(a2, &sB, &cB);
  sincos37f98(a1, &sC, &cC);
  const double S = s;
  out[0] = static_cast<float>(static_cast<double>(cB) * cC * S);
  out[1] = static_cast<float>(-(static_cast<double>(cB) * sC) * S);
  out[2] = static_cast<float>(static_cast<double>(sB) * S);
  out[3] = static_cast<float>(static_cast<double>(t[0]) * S);
  out[4] = static_cast<float>((static_cast<double>(sA) * sB * cC +
                               static_cast<double>(cA) * sC) * S);
  out[5] = static_cast<float>((static_cast<double>(cA) * cC -
                               static_cast<double>(sA) * sB * sC) * S);
  out[6] = static_cast<float>(-(static_cast<double>(sA) * cB) * S);
  out[7] = static_cast<float>(static_cast<double>(t[1]) * S);
  out[8] = static_cast<float>((static_cast<double>(sA) * sC -
                               static_cast<double>(cA) * sB * cC) * S);
  out[9] = static_cast<float>((static_cast<double>(cA) * sB * sC +
                               static_cast<double>(sA) * cC) * S);
  out[10] = static_cast<float>(static_cast<double>(cA) * cB * S);
  out[11] = t[2];  // OBSERVED: unscaled — no FMUL in the original.
}

// FUN_0046b2f8 — the traversal object's euler builder, 3x4 form.
// Positional args map arg8/arg_c/arg_10 -> sA/sB/sC (pitch/bank/yaw in
// the native call sites). Translation is stored raw via MOV — NEVER
// scaled (OBSERVED: integer copies at 0x46b371/0x46b3b2/0x46b3d6).
void StreamScene::euler6b2f8(float a1, float a2, float a3, float s,
                             const float t[3], float out[12]) {
  float sA, cA, sB, cB, sC, cC;
  sincos37f98(a1, &sA, &cA);
  sincos37f98(a2, &sB, &cB);
  sincos37f98(a3, &sC, &cC);
  const double S = s;
  out[0] = static_cast<float>(static_cast<double>(cB) * cC * S);
  out[1] = static_cast<float>(-(static_cast<double>(sA) * sB * cC +
                               static_cast<double>(cA) * sC) * S);
  out[2] = static_cast<float>((static_cast<double>(sA) * sC -
                               static_cast<double>(cA) * sB * cC) * S);
  out[3] = t[0];
  out[4] = static_cast<float>(static_cast<double>(cB) * sC * S);
  out[5] = static_cast<float>((static_cast<double>(cA) * cC -
                               static_cast<double>(sA) * sB * sC) * S);
  out[6] = static_cast<float>(-(static_cast<double>(cA) * sB * sC +
                               static_cast<double>(sA) * cC) * S);
  out[7] = t[1];
  out[8] = static_cast<float>(static_cast<double>(sB) * S);
  out[9] = static_cast<float>(static_cast<double>(sA) * cB * S);
  out[10] = static_cast<float>(static_cast<double>(cA) * cB * S);
  out[11] = t[2];
}

// FUN_0046aeb0 — out = A o B for row-major 3x4 transforms. The native's
// accumulation order differs by column (OBSERVED): column 0 sums
// (row1*col, row0*col, row2*col); columns 1..3 sum (row0, row1, row2);
// column 3 then adds A's own translation element.
void StreamScene::compose6aeb0(const float A[12], const float B[12],
                               float out[12]) {
  out[0] = static_cast<float>(static_cast<double>(A[1]) * B[4] +
                              static_cast<double>(A[0]) * B[0] +
                              static_cast<double>(A[2]) * B[8]);
  out[1] = static_cast<float>(static_cast<double>(A[1]) * B[5] +
                              static_cast<double>(A[0]) * B[1] +
                              static_cast<double>(A[2]) * B[9]);
  out[2] = static_cast<float>(static_cast<double>(A[1]) * B[6] +
                              static_cast<double>(A[0]) * B[2] +
                              static_cast<double>(A[2]) * B[10]);
  out[3] = static_cast<float>(static_cast<double>(A[1]) * B[7] +
                              static_cast<double>(A[0]) * B[3] +
                              static_cast<double>(A[2]) * B[11] +
                              static_cast<double>(A[3]));
  out[4] = static_cast<float>(static_cast<double>(A[5]) * B[4] +
                              static_cast<double>(A[4]) * B[0] +
                              static_cast<double>(A[6]) * B[8]);
  out[5] = static_cast<float>(static_cast<double>(A[4]) * B[1] +
                              static_cast<double>(A[5]) * B[5] +
                              static_cast<double>(A[6]) * B[9]);
  out[6] = static_cast<float>(static_cast<double>(A[4]) * B[2] +
                              static_cast<double>(A[5]) * B[6] +
                              static_cast<double>(A[6]) * B[10]);
  out[7] = static_cast<float>(static_cast<double>(A[4]) * B[3] +
                              static_cast<double>(A[5]) * B[7] +
                              static_cast<double>(A[6]) * B[11] +
                              static_cast<double>(A[7]));
  out[8] = static_cast<float>(static_cast<double>(A[9]) * B[4] +
                              static_cast<double>(A[8]) * B[0] +
                              static_cast<double>(A[10]) * B[8]);
  out[9] = static_cast<float>(static_cast<double>(A[8]) * B[1] +
                              static_cast<double>(A[9]) * B[5] +
                              static_cast<double>(A[10]) * B[9]);
  out[10] = static_cast<float>(static_cast<double>(A[8]) * B[2] +
                               static_cast<double>(A[9]) * B[6] +
                               static_cast<double>(A[10]) * B[10]);
  out[11] = static_cast<float>(static_cast<double>(A[8]) * B[3] +
                               static_cast<double>(A[9]) * B[7] +
                               static_cast<double>(A[10]) * B[11] +
                               static_cast<double>(A[11]));
}

// FUN_0046afe4 — out = v transformed by row-major M (row dot + col-3
// translation). Each element: ((v0*Mc0 + v1*Mc1) + v2*Mc2) + Mc3.
void StreamScene::point6afe4(const float v[3], const float M[12],
                             float out[3]) {
  out[0] = static_cast<float>(static_cast<double>(v[0]) * M[0] +
                              static_cast<double>(v[1]) * M[1] +
                              static_cast<double>(v[2]) * M[2] +
                              static_cast<double>(M[3]));
  out[1] = static_cast<float>(static_cast<double>(v[0]) * M[4] +
                              static_cast<double>(v[1]) * M[5] +
                              static_cast<double>(v[2]) * M[6] +
                              static_cast<double>(M[7]));
  out[2] = static_cast<float>(static_cast<double>(v[0]) * M[8] +
                              static_cast<double>(v[1]) * M[9] +
                              static_cast<double>(v[2]) * M[10] +
                              static_cast<double>(M[11]));
}

// FUN_0042e9c4 — normalize v in place. len2 is accumulated in f80 and
// stored f64; the <=0 guard covers zero AND unordered(NaN->normalize
// path is NOT taken only when len2 > 0... observed: 0 < len2 required).
// Scale is stored f32 before being applied (single round), so plain
// float products match the original bit-for-bit here.
void StreamScene::normalizeE9c4(float v[3]) {
  const double len2 = static_cast<double>(v[1]) * v[1] +
                      static_cast<double>(v[0]) * v[0] +
                      static_cast<double>(v[2]) * v[2];
  float scale = 1.0f;
  if (len2 > 0.0)
    scale = static_cast<float>(1.0 / std::sqrt(len2));
  v[0] = v[0] * scale;
  v[1] = v[1] * scale;
  v[2] = v[2] * scale;
}

// FUN_0042e978 — normalizes the three COLUMNS of M in place
// (col c = {M[c], M[c+4], M[c+8]}); translation column untouched.
// Unlike e9c4 there is NO zero guard and the 1/sqrt scale is kept in
// f80 (never f32-rounded) — the port keeps it in double.
void StreamScene::normalizeColsE978(float M[12]) {
  for (int c = 0; c != 3; ++c) {
    const double len2 = static_cast<double>(M[c + 4]) * M[c + 4] +
                        static_cast<double>(M[c]) * M[c] +
                        static_cast<double>(M[c + 8]) * M[c + 8];
    const double scale = 1.0 / std::sqrt(len2);
    M[c] = static_cast<float>(static_cast<double>(M[c]) * scale);
    M[c + 4] = static_cast<float>(static_cast<double>(M[c + 4]) * scale);
    M[c + 8] = static_cast<float>(static_cast<double>(M[c + 8]) * scale);
  }
}

// FUN_0042dc68 — path position on the 32-entry node ring. n =
// FRNDINT-truncated t (toward zero); i0 = n & 0x1f, i1 = (n+1) & 0x1f;
// frac = t - n is stored f32 then used for the lerp of the node
// matrices' translation elements (3/7/11). Negative t wraps via the
// mask (e.g. t=-1.x -> slot 31) and extrapolates on a negative frac —
// both preserved.
void StreamScene::pathPos(float out[3], float t) const {
  const int n = static_cast<int>(streamFrndInt(t));
  const float* m0 = nodeMat_[n & 0x1f];
  const float* m1 = nodeMat_[(n + 1) & 0x1f];
  const float frac = t - static_cast<float>(n);
  out[0] = static_cast<float>((static_cast<double>(m1[3]) - m0[3]) * frac +
                              static_cast<double>(m0[3]));
  out[1] = static_cast<float>((static_cast<double>(m1[7]) - m0[7]) * frac +
                              static_cast<double>(m0[7]));
  out[2] = static_cast<float>((static_cast<double>(m1[11]) - m0[11]) * frac +
                              static_cast<double>(m0[11]));
}

// FUN_0042dcf4 — path-aligned 3x4 frame. Columns are
// {side, dir, side x dir, pathPos(t0)} where dir = pathPos(t1) -
// pathPos(t0) normalized, side = dir x (-camView_ up row) normalized,
// and the third column is the s x d recompute (raw, unnormalized —
// OBSERVED: no third normalizeE9c4 call). The up row read is the
// CURRENT camView_[4..6] — the previous frame's camera up (temporal
// feedback, OBSERVED).
void StreamScene::pathFrame(float out[12], float t0, float t1) const {
  float p0[3], p1[3], d[3], nu[3], s[3];
  pathPos(p0, t0);
  pathPos(p1, t1);
  d[0] = static_cast<float>(static_cast<double>(p1[0]) - p0[0]);
  d[1] = static_cast<float>(static_cast<double>(p1[1]) - p0[1]);
  d[2] = static_cast<float>(static_cast<double>(p1[2]) - p0[2]);
  normalizeE9c4(d);
  nu[0] = -camView_[4];
  nu[1] = -camView_[5];
  nu[2] = -camView_[6];
  cross3(d, nu, s);
  normalizeE9c4(s);
  cross3(s, d, nu);
  out[0] = s[0];
  out[4] = s[1];
  out[8] = s[2];
  out[1] = d[0];
  out[5] = d[1];
  out[9] = d[2];
  out[2] = nu[0];
  out[6] = nu[1];
  out[10] = nu[2];
  out[3] = p0[0];
  out[7] = p0[1];
  out[11] = p0[2];
}

// FUN_0042de28 — mode-5 camera build. eye = arg0 (native ECX, copied to
// camPos_); look = pathPos(t). dir = normalize(look - eye);
// side = normalize(upRef_ x dir); upRef_ = dir x side (writeback —
// OBSERVED, the temporal-up global is overwritten in place). camView_
// rows are [side | up' | dir] with t = -(row . eye); camProj_ is the
// same matrix with rows 0/1 scaled by projScale_[0]/[1] (row 2 scale
// is written 1.0f = unscaled). projScale_ =
//   1/(zoom*0.5f), 1/(zoom*360f*(1/600)f*0.5)
// with the constants above (600x360 viewport, zoom 2.4).
void StreamScene::cameraAt(const float eye[3], float t) {
  float look[3], d[3], s[3], up[3];
  pathPos(look, t);
  d[0] = static_cast<float>(static_cast<double>(look[0]) - eye[0]);
  d[1] = static_cast<float>(static_cast<double>(look[1]) - eye[1]);
  d[2] = static_cast<float>(static_cast<double>(look[2]) - eye[2]);
  normalizeE9c4(d);
  cross3(upRef_, d, s);
  normalizeE9c4(s);
  cross3(d, s, up);
  upRef_[0] = up[0];
  upRef_[1] = up[1];
  upRef_[2] = up[2];
  projScale_[0] = static_cast<float>(
      1.0 / (static_cast<double>(kCamZoom) * kCamHalf));
  projScale_[1] = static_cast<float>(
      1.0 / (static_cast<double>(kCamZoom) * kCamHeight * kCamInvWidth *
             kCamHalf64));
  camPos_[0] = eye[0];
  camPos_[1] = eye[1];
  camPos_[2] = eye[2];
  camView_[0] = s[0];
  camView_[1] = s[1];
  camView_[2] = s[2];
  camView_[3] = static_cast<float>(
      -(static_cast<double>(s[1]) * eye[1] +
        static_cast<double>(s[0]) * eye[0] +
        static_cast<double>(s[2]) * eye[2]));
  camView_[4] = up[0];
  camView_[5] = up[1];
  camView_[6] = up[2];
  camView_[7] = static_cast<float>(
      -(static_cast<double>(up[1]) * eye[1] +
        static_cast<double>(up[0]) * eye[0] +
        static_cast<double>(up[2]) * eye[2]));
  camView_[8] = d[0];
  camView_[9] = d[1];
  camView_[10] = d[2];
  camView_[11] = static_cast<float>(
      -(static_cast<double>(d[1]) * eye[1] +
        static_cast<double>(d[0]) * eye[0] +
        static_cast<double>(d[2]) * eye[2]));
  camProj_[0] = camView_[0] * projScale_[0];
  camProj_[1] = camView_[1] * projScale_[0];
  camProj_[2] = camView_[2] * projScale_[0];
  camProj_[3] = camView_[3] * projScale_[0];
  camProj_[4] = camView_[4] * projScale_[1];
  camProj_[5] = camView_[5] * projScale_[1];
  camProj_[6] = camView_[6] * projScale_[1];
  camProj_[7] = camView_[7] * projScale_[1];
  camProj_[8] = camView_[8];
  camProj_[9] = camView_[9];
  camProj_[10] = camView_[10];
  camProj_[11] = camView_[11];
}

// FUN_0042d97c — first-hit wall probe against the 32-plane set of slot
// (trunc(t) & 0x1f). dist = n.pos + d + (-1.5), compared on the
// unrounded sum while the DIVIDEND is the f32-rounded dist (OBSERVED:
// FST f32 then FCOMPP on the f80 residue, FDIVR reloads the f32).
// Return dist / -(n . (pathPos(t) - pos)) on the first dist <= 0 plane;
// -1 on miss. Parallel rays (n . delta == 0) divide to +-inf.
float StreamScene::wallProbe(const float pos[3], float t) const {
  const int n = static_cast<int>(streamFrndInt(t));
  const float* planes = planes_[n & 0x1f];
  float c[3];
  pathPos(c, t);
  const float dx = static_cast<float>(static_cast<double>(c[0]) - pos[0]);
  const float dy = static_cast<float>(static_cast<double>(c[1]) - pos[1]);
  const float dz = static_cast<float>(static_cast<double>(c[2]) - pos[2]);
  for (int i = 0; i != kStreamPlanes; ++i) {
    const float* pl = planes + i * 4;
    const double distSum = static_cast<double>(pl[1]) * pos[1] +
                           static_cast<double>(pl[0]) * pos[0] +
                           static_cast<double>(pl[2]) * pos[2] +
                           static_cast<double>(pl[3]) + kWallMargin;
    const float dist = static_cast<float>(distSum);
    if (distSum <= 0.0) {
      const double nd = static_cast<double>(pl[1]) * dy +
                        static_cast<double>(pl[0]) * dx +
                        static_cast<double>(pl[2]) * dz;
      return static_cast<float>(dist / -nd);
    }
  }
  return -1.0f;
}

// ---------------------------------------------------------------------------
// Phase 19A.1B — the cinematic object pool / lifecycle / buckets.
// ---------------------------------------------------------------------------
//
// FUN_0042b270 head (pool foundation, OBSERVED instruction-level):
//   FUN_0047d20a(0x4f0740, 0, 400*0x32e) wipes all 400 records, then a
//   forward loop stores +0x00 = next-record for pool[0..398] leaving
//   pool[399].next = 0 — the freelist head 0x540ed0 receives &pool[0],
//   so the initial pop order is lowest-index-first. The 32 bucket heads
//   at 0x4ed6b8 are zeroed by the same wipe region covering them.
// Port: the memset maps to per-record value reset (destroy +
// placement-new) — DynamicObject owns std::string/std::vector members
// and a surface-record list that a raw memset would leak.
StreamScene::StreamScene() { poolReset(); }

void StreamScene::poolReset() {
  for (auto& o : pool_) {
    o.~DynamicObject();
    new (&o) DynamicObject();
  }
  for (int i = 0; i + 1 != kStreamPoolSize; ++i)
    streamSetNext(&pool_[i], &pool_[i + 1]);
  freelist_ = pool_.data();
  for (auto& head : buckets_) head = nullptr;
}

// FUN_0042bdc4 — pop the freelist head; on empty -> FUN_00408eb0("No
// Aliens available in stream!") counted + DAT_0054148e = 1, return 0.
// Else wipe the record (0x32e — here a full in-place value reset),
// head-insert into buckets[bucket & 0x1f] through +0x00, store +0x5c =
// t (the path position — zBias), +0x04 = (u16)(bucket & 0x1f)
// (enemyIndex), then FUN_0042dc68 initializes +0x10..0x18 = pathPos(t).
// +0x06 stays 0 — stream objects are never "named" (unlike the
// traversal sweep's reapUnnamed).
DynamicObject* StreamScene::alloc(int bucket, float t) {
  if (!freelist_) {
    ++poolErrorCalls_;
    quit_ = true;  // DAT_0054148e = 1
    return nullptr;
  }
  DynamicObject* o = freelist_;
  freelist_ = streamNext(o);  // pop: read +0x00 before the wipe
  o->~DynamicObject();
  new (o) DynamicObject();
  const int b = bucket & 0x1f;
  streamSetNext(o, buckets_[b]);
  buckets_[b] = o;
  o->zBias = t;                                       // +0x5c
  o->enemyIndex = static_cast<std::uint16_t>(b);      // +0x04
  float p[3];
  pathPos(p, t);                                      // FUN_0042dc68
  o->setPosition(p[0], p[1], p[2]);                   // +0x10..0x18
  return o;
}

// FUN_0042c7b4 — unlink o from buckets_[enemyIndex & 0x1f] and push it
// onto the freelist head. The native walk keeps a slot pointer that is
// the bucket head slot first, then each node's +0x00 — here prev/head
// cover the two cases. Miss (null slot or end-of-chain) ->
// FUN_00408eb0("Alien not in list to be freed") counted, record
// untouched. On success: *slot = o->next; o->next = freelist;
// freelist = o; then the +0x0c model-record gate — nonzero ->
// FUN_00403880 release + +0x0c = 0. The record BODY is NOT wiped by
// reap (stale fields persist until the next alloc wipe — OBSERVED).
void StreamScene::reap(DynamicObject& o) {
  const int b = o.enemyIndex & 0x1f;
  DynamicObject* prev = nullptr;
  DynamicObject* cur = buckets_[b];
  while (cur && cur != &o) {
    prev = cur;
    cur = streamNext(cur);
  }
  if (!cur) {
    ++poolErrorCalls_;  // "Alien not in list to be freed"
    return;
  }
  if (prev)
    streamSetNext(prev, streamNext(&o));
  else
    buckets_[b] = streamNext(&o);
  streamSetNext(&o, freelist_);
  freelist_ = &o;
  if (o.col.elements) {          // +0x0c gate -> FUN_00403880
    o.model = RuntimeModel{};    //   release the deep-copied record
    o.elemSet = CollisionElementSet{};
    o.col.elements = nullptr;    //   +0x0c = 0
  }
}

// FUN_0042da40 — move o's bucket tag toward `target` inside
// [winLo_, winHi_): outside -> return false (no touch). nb = target &
// 0x1f; the stored tag ob = (u16)+0x04 is compared unmasked — equal ->
// return true with the list untouched. Else unlink o from buckets[ob]
// (head store or predecessor +0x00 rewrite), head-insert into
// buckets[nb], +0x04 = (u16)nb, return true. The native indexes
// buckets_[ob] with the unmasked tag — a tag >= 32 means the record was
// never bucketed; the native reads neighboring globals and fails the
// search, so the port returns false directly (bounds kept).
bool StreamScene::migrate(DynamicObject& o, int target) {
  if (target < winLo_ || target >= winHi_) return false;
  const int nb = target & 0x1f;
  const int ob = o.enemyIndex;
  if (nb == ob) return true;
  if (ob >= kStreamSegs) return false;
  DynamicObject* cur = buckets_[ob];
  if (cur == &o) {
    buckets_[ob] = streamNext(&o);
  } else {
    while (cur && streamNext(cur) != &o) cur = streamNext(cur);
    if (!cur) return false;
    streamSetNext(cur, streamNext(&o));
  }
  streamSetNext(&o, buckets_[nb]);
  buckets_[nb] = &o;
  o.enemyIndex = static_cast<std::uint16_t>(nb);  // +0x04
  return true;
}

// FUN_0042c578 — debris spawn. n = trunc(t); o = alloc(t, n); +0x0c =
// 0 (sprite — no model record). Lateral offset: vecHdr non-null copies
// its 3 f32s into +0x1c..+0x24 (y immediately overwritten below); null
// draws two rands — (r - 0x4000) * 2^-12 into +0x1c and +0x24, y = 0.
// +0x20 = frac(t) * 10 in both cases, so the composed local
// translation is {v.x, frac*10, v.z}. +0xac = nodeMat[n & 0x1f] o
// {I | local}. +0x58 = rand * 2^-13 + 4.0. +0x108 = lightTag
// (0x4edacc). +0x34: FLDZ/FCOMP(speed) — speed > 0 ->
// rand*2^-13 + speed; speed < 0 -> speed - rand*2^-13; == 0 -> 0 (no
// rand drawn). OBSERVED quirk: FCOMP unordered sets CF -> NaN speed
// takes the >0 branch.
DynamicObject* StreamScene::spawnDebris(float t, const void* vecHdr,
                                        float speed) {
  const int n = static_cast<int>(streamFrndInt(t));
  DynamicObject* o = alloc(n, t);
  if (!o) return nullptr;  // native writes +0x0c unconditionally on a
                          // null record (AV); the quit flag already
                          // halted the scene — the port stops here
  o->col.elements = nullptr;  // +0x0c = 0
  if (vecHdr) {
    const float* v = static_cast<const float*>(vecHdr);
    o->field1c[0] = v[0];
    o->field1c[1] = v[1];
    o->field1c[2] = v[2];
  } else {
    o->field1c[0] = static_cast<float>(
        (static_cast<std::int32_t>(enemyRandNext(rng_)) - 0x4000) *
        kDebrisRandRange);
    o->field1c[2] = static_cast<float>(
        (static_cast<std::int32_t>(enemyRandNext(rng_)) - 0x4000) *
        kDebrisRandRange);
  }
  const float frac20 = static_cast<float>(
      (static_cast<double>(t) - static_cast<double>(n)) *
      kFracScaleDebris);
  o->field1c[1] = frac20;  // +0x20
  float local[12] = {};
  local[0] = local[5] = local[10] = 1.0f;
  local[3] = o->field1c[0];
  local[7] = o->field1c[1];
  local[11] = o->field1c[2];
  float m[12];
  compose6aeb0(nodeMat_[n & 0x1f], local, m);
  streamStoreMatrix(*o, m);
  o->col.scale = static_cast<float>(                       // +0x58
      static_cast<double>(enemyRandNext(rng_)) * kSpawnRandRange +
      kDebrisScaleBase);
  o->field108 = reinterpret_cast<const void*>(             // +0x108 =
      static_cast<std::intptr_t>(assets_.lightTag));       //   lightTag
  if (speed > 0.0f || std::isnan(speed))
    o->field34 = static_cast<float>(                       // +0x34 (>0)
        static_cast<double>(enemyRandNext(rng_)) * kSpawnRandRange +
        static_cast<double>(speed));
  else if (speed < 0.0f)
    o->field34 = static_cast<float>(                       // +0x34 (<0)
        static_cast<double>(speed) -
        static_cast<double>(enemyRandNext(rng_)) * kSpawnRandRange);
  else
    o->field34 = 0.0f;                                     // +0x34 (=0)
  return o;
}

// FUN_0042c6f0 — marker spawn. n = trunc(t); o = alloc(t, n); +0x0c =
// 0; +0x1c/+0x24 = 0; +0x20 = frac(t) * 10; +0xac = nodeMat[n & 0x1f]
// o {I | 0, frac*10, 0}; +0x58 = 0x42100000 (36.0f); +0x34 = 0;
// +0x108 = planetTag[0] (0x4eda8c).
DynamicObject* StreamScene::spawnMarker(float t) {
  const int n = static_cast<int>(streamFrndInt(t));
  DynamicObject* o = alloc(n, t);
  if (!o) return nullptr;  // same null-alloc boundary as spawnDebris
  o->col.elements = nullptr;   // +0x0c = 0
  o->field1c[0] = 0.0f;        // +0x1c
  o->field1c[2] = 0.0f;        // +0x24
  o->field1c[1] = static_cast<float>(                      // +0x20
      (static_cast<double>(t) - static_cast<double>(n)) *
      kFracScaleMarker);
  float local[12] = {};
  local[0] = local[5] = local[10] = 1.0f;
  local[3] = o->field1c[0];
  local[7] = o->field1c[1];
  local[11] = o->field1c[2];
  float m[12];
  compose6aeb0(nodeMat_[n & 0x1f], local, m);
  streamStoreMatrix(*o, m);
  o->col.scale = std::bit_cast<float>(0x42100000u);        // +0x58 = 36.0
  o->field34 = 0.0f;                                       // +0x34
  o->field108 = reinterpret_cast<const void*>(             // +0x108 =
      static_cast<std::intptr_t>(assets_.planetTag[0]));   // planetTag[0]
  return o;
}

// Test hooks — O(n) chain walks over the fixed pool (diagnostic only).
int StreamScene::bucketHead(int b) const {
  return objIndex(buckets_[b & 0x1f]);
}

int StreamScene::poolFreeCount() const {
  int n = 0;
  for (DynamicObject* o = freelist_; o; o = streamNext(o)) ++n;
  return n;
}

int StreamScene::poolBucketCount(int b) const {
  int n = 0;
  for (DynamicObject* o = buckets_[b & 0x1f]; o; o = streamNext(o)) ++n;
  return n;
}

int StreamScene::poolLinkIndex(const DynamicObject& o) const {
  if (!o.col.next) return -1;
  const auto* base = reinterpret_cast<const char*>(pool_.data());
  const auto* p = reinterpret_cast<const char*>(o.col.next);
  if (p < base || p >= base + sizeof(pool_)) return -1;
  return static_cast<int>((p - base) / sizeof(DynamicObject));
}

} // namespace mdk
