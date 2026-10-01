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

} // namespace mdk
