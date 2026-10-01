// stream_scene.cpp — Phase 19A mode-5 cinematic core.
//
// Implements the helper layer (the 0x42xxxx/0x46xxxx math/camera ports),
// pool/lifecycle, init/tunnelExtend/teardown, the actor updater family,
// the FUN_004555bc animator family, the TELETYPE queue service, the
// frame skeleton's counter/drain/completion core (0x541554 pool drain,
// 0x4ed748 latch, 0x4eda9c drain accumulator, exit handoff), the draw
// stage (e684 scroll state, e100 records, 17e20 HUD emit, 2fb68/2fb30
// limiter state) and the ae60 projector install. The intentional
// host boundaries — the framebuffer blits/raster output, palette DAC
// upload, audio dispatch and the present/wait sleeps — are the
// Phase 19B presentation surface.
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

#include <algorithm>
#include <bit>
#include <cstring>
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

// DAT_004edcc0 — the shared class-table record-0 identity that the
// FUN_004555bc dispatch compares an object's +0x0c against (the fuse
// arm). The native value is a fixed global address; the port keeps a
// process-unique sentinel — identity only, never dereferenced.
CollisionElementSet kStreamClassRec0{};

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
  // 2de38..5a — the view-config globals are rewritten every call:
  // 0x540b58/0x540b64 = 2.4f zoom, 0x540b68/0x540b6c = 600x360
  // viewport, 0x540b70/0x540b74 = 300x180 center, 0x540b78/7c = 0.
  // The sprite emitters read 0x540b68 (i32 numerator) and 0x540b58
  // (f32 denominator) — surfaced as fields so the size math is exact.
  spriteSizeNum_ = 600;
  projSizeF_ = kCamZoom;
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

// ===========================================================================
// FUN_0042be4c — tunnelExtend: advance the tunnel window by one segment.
//
// OBSERVED (instruction-level port of analysis-private/logs/p19a_asm2.txt):
//   - cur = winHi & 0x1f, prev = (winHi-1) & 0x1f — the 32-seg ring.
//   - Final-mode end gate FIRST: isFinal && FILD(winHi) > 186.0 -> spawn
//     the end marker once at t = winHi-3 and freeze (early RET — winHi
//     never advances past 187, stale bucket/plane state persists).
//   - When winHi != winLo (a previous segment exists):
//       nodeMat[cur] = nodeMat[prev] o euler6b180(drift[1],drift[2],
//                      drift[0], s=1.0, t={0,10,0}), then e978 column
//                      renormalize;
//       reap every object in buckets_[cur] (ring slot being recycled);
//       (rand & 3) != 0 -> spawnDebris(winHi-1, nullptr, -3.0f).
//   - Ring points ALWAYS regenerate for slot cur: 16 pts, angle 0..337.5
//     stepping f32(+=22.5); per pt two rands — v = {jx*radius*cos, 0,
//     jz*radius*sin} with j = (r-0x4000 + 163840.0) * 2^-...  (the
//     constants 6.103515625e-06 = 1/163840 and 163840.0 bias the rand
//     into [0.9, 1.1)); point6afe4(v, nodeMat[cur]) -> ringPts[cur][i].
//   - Plane/pen block only when a previous segment exists: penVal =
//     trunc(penBase*(1-penT) + penTarget*penT); per i: pens[prev][2i] =
//     (fold((winHi+2i)&0x1f) + penVal) & 0x3f and [2i+1] likewise with
//     +2i+1 (fold x>=16 -> 31-x); planes[prev][2i] built from
//     (cur[i1]-prev[i]) x (prev[i1]-cur[i1]), [2i+1] from
//     (cur[i]-prev[i]) x (cur[i1]-cur[i]) — both normalized (e9c4) and
//     anchored to prev[i] (d = -(n.p), accumulate order y,x,z);
//     then penT += 0.1 — f32(penT+0.1), and penT > 1.0 (or unordered)
//     -> penT=0, penBase=penTarget, penTarget=rand&0x3f.
//   - winHi++ ALWAYS on this path; then drift update gated on the NEW
//     winHi: isFinal && winHi > 168 -> each drift[k] decays 1.0 toward
//     0 (min(d+1,0) / max(d-1,0)); else 3 rands x (r-0x4000)*1/16384.
//     Shared cap: |drift[k]| > driftMax -> drift[k] *= 0.8 (f64).
//   - radius += (r-0x4000)*1/16384 (f64 chain, f32 store), clamped to
//     [radiusMin, radiusMax]; then ae60(0) filler-select seam.
// ===========================================================================
void StreamScene::tunnelExtend() {
  ++seams_.trailUpdate;   // be4c dispatch — path-history append pass
  const int cur = winHi_ & 0x1f;
  const int prev = (winHi_ - 1) & 0x1f;

  // 0x496f70 = 186.0 — the final-mode end gate (winHi is FILDed).
  if (isFinal_ && static_cast<double>(winHi_) > 186.0) {
    if (marker_) return;
    marker_ = spawnMarker(static_cast<float>(winHi_ - 3));
    return;
  }

  const bool havePrev = (winHi_ != winLo_);
  if (havePrev) {
    const float t[3] = {0.0f, 10.0f, 0.0f};  // 0x496f40 pair / +Y step
    float local[12];
    euler6b180(drift_[1], drift_[2], drift_[0], 1.0f, t, local);
    compose6aeb0(nodeMat_[prev], local, nodeMat_[cur]);
    normalizeColsE978(nodeMat_[cur]);
    while (DynamicObject* o = buckets_[cur]) reap(*o);
    if ((enemyRandNext(rng_) & 3) != 0)
      spawnDebris(static_cast<float>(winHi_ - 1), nullptr, -3.0f);
  }

  // Ring regeneration — runs even on the first segment (winHi==winLo).
  float angle = 0.0f;  // [EBP-0x34] f32 accumulator
  float* w = &ringPts_[cur][0];
  for (int i = 0; i != kStreamRingPts; ++i) {
    float sn = 0.0f, cs = 0.0f;
    sincos37f98(angle, &sn, &cs);   // sin -> -0x44(z jitter), cos -> -0x48
    angle = static_cast<float>(static_cast<double>(angle) + 22.5);
    const double jx =
        (static_cast<double>(static_cast<std::int32_t>(
             enemyRandNext(rng_)) - 0x4000) +
         163840.0) * 6.103515625e-06;
    const double jz =
        (static_cast<double>(static_cast<std::int32_t>(
             enemyRandNext(rng_)) - 0x4000) +
         163840.0) * 6.103515625e-06;
    const float v[3] = {
        static_cast<float>(jx * (static_cast<double>(radius_) *
                                 static_cast<double>(cs))),
        0.0f,
        static_cast<float>(jz * (static_cast<double>(radius_) *
                                 static_cast<double>(sn))),
    };
    point6afe4(v, nodeMat_[cur], w);
    w += 3;
  }

  if (havePrev) {
    const float* P = &ringPts_[prev][0];
    const float* C = &ringPts_[cur][0];
    // penVal = trunc(penBase*(1-penT) + penTarget*penT) — f80 chain,
    // FRNDINT (RC=11 trunc) via 47d59a, FISTP.
    const int penVal = static_cast<int>(streamFrndInt(static_cast<float>(
        static_cast<double>(penBase_) * (1.0 - static_cast<double>(penT_)) +
        static_cast<double>(penTarget_) * static_cast<double>(penT_))));
    for (int i = 0; i != kStreamRingPts; ++i) {
      const int i1 = (i + 1) & 0xf;
      int pa = (winHi_ + 2 * i) & 0x1f;
      if (pa >= 16) pa = 31 - pa;
      pens_[prev][2 * i] = static_cast<std::uint8_t>((pa + penVal) & 0x3f);
      int pb = (winHi_ + 2 * i + 1) & 0x1f;
      if (pb >= 16) pb = 31 - pb;
      pens_[prev][2 * i + 1] =
          static_cast<std::uint8_t>((pb + penVal) & 0x3f);

      // plane[2i]: e0 = cur[i1]-prev[i], e1 = prev[i1]-cur[i1] (f32 subs)
      const float e0[3] = {C[i1 * 3] - P[i * 3], C[i1 * 3 + 1] - P[i * 3 + 1],
                           C[i1 * 3 + 2] - P[i * 3 + 2]};
      const float e1[3] = {P[i1 * 3] - C[i1 * 3],
                           P[i1 * 3 + 1] - C[i1 * 3 + 1],
                           P[i1 * 3 + 2] - C[i1 * 3 + 2]};
      float* n0 = &planes_[prev][(2 * i) * 4];
      cross3(e0, e1, n0);
      normalizeE9c4(n0);
      n0[3] = -static_cast<float>(
          static_cast<double>(n0[1]) * P[i * 3 + 1] +
          static_cast<double>(n0[0]) * P[i * 3] +
          static_cast<double>(n0[2]) * P[i * 3 + 2]);

      // plane[2i+1]: e0' = cur[i]-prev[i], e1' = cur[i1]-cur[i]
      const float e2[3] = {C[i * 3] - P[i * 3], C[i * 3 + 1] - P[i * 3 + 1],
                           C[i * 3 + 2] - P[i * 3 + 2]};
      const float e3[3] = {C[i1 * 3] - C[i * 3],
                           C[i1 * 3 + 1] - C[i * 3 + 1],
                           C[i1 * 3 + 2] - C[i * 3 + 2]};
      float* n1 = &planes_[prev][(2 * i + 1) * 4];
      cross3(e2, e3, n1);
      normalizeE9c4(n1);
      n1[3] = -static_cast<float>(
          static_cast<double>(n1[1]) * P[i * 3 + 1] +
          static_cast<double>(n1[0]) * P[i * 3] +
          static_cast<double>(n1[2]) * P[i * 3 + 2]);
    }
    // penT += 0.1; store happens BEFORE the compare (FST f32), and the
    // FCOMPP/JNC pair resets on >1.0 OR unordered (NaN -> CF=1).
    penT_ = static_cast<float>(static_cast<double>(penT_) + 0.1);
    if (!(penT_ <= 1.0f)) {
      penT_ = 0.0f;
      penBase_ = penTarget_;
      penTarget_ = static_cast<int>(enemyRandNext(rng_)) & 0x3f;
    }
  }

  ++winHi_;

  // Drift update — gated on the POST-increment winHi (0x496f88 = 168.0).
  if (isFinal_ && static_cast<double>(winHi_) > 168.0) {
    for (int k = 0; k != 3; ++k)
      drift_[k] = drift_[k] > 0.0f ? (drift_[k] - 1.0f > 0.0f ? drift_[k] - 1.0f
                                                              : 0.0f)
                                   : (drift_[k] + 1.0f <= 0.0f ? drift_[k] + 1.0f
                                                               : 0.0f);
  } else {
    for (int k = 0; k != 3; ++k)
      drift_[k] = static_cast<float>(
          drift_[k] +
          static_cast<double>(static_cast<std::int32_t>(
              enemyRandNext(rng_)) - 0x4000) * 6.103515625e-05);
  }
  for (int k = 0; k != 3; ++k)
    if (std::fabs(drift_[k]) > driftMax_)
      drift_[k] = static_cast<float>(static_cast<double>(drift_[k]) * 0.8);

  radius_ = static_cast<float>(
      static_cast<double>(static_cast<std::int32_t>(enemyRandNext(rng_)) -
                          0x4000) * 6.103515625e-05 +
      static_cast<double>(radius_));
  if (radius_ < radiusMin_)
    radius_ = radiusMin_;
  else if (radius_ > radiusMax_)
    radius_ = radiusMax_;
  fillSelect(0);  // FUN_0046ae60(0) — be4c's tail re-installs the
                  // full-view projector (XOR EAX,EAX; CALL 0x46ae60)
}

// ===========================================================================
// FUN_0042b270 — mode-5 init (the decoded tail after the pool head).
// ===========================================================================
bool StreamScene::init(const StreamAssets& a, int course, int skill,
                       std::uint32_t rng, int health) {
  // --- head: host-side seams (b270..b2a8) ---
  // FUN_0041c86c(10) mode-entry hook; 0x541544 busy-gate spins
  // FUN_0042b20c(3) until clear (host wait — counted, not spun);
  // FUN_0041cf5c TELETYPE queue clear (0x54b800/7fc/7f4/7f8 = 0 —
  // real body now, Phase 19A.2D); 0x46ca84+0x415678 x2 = video
  // clear+present pair.
  ++seams_.resourceBind;
  teletypeClear();
  seams_.resourceBind += 4;  // 46ca84/415678 pair x2

  // --- the 0x4e74b0..0x4edae0 wipe (0x6630) + proto table + pool ---
  // The proto table (0x4edcc0 + 0x88*i slots) is native storage for
  // parsed RuntimeModels; the port binds protos via StreamAssets.
  // Members outside the wipe region (camView_/camPos_/camProj_/
  // upRef_/bgScroll_/frameTick_) keep their values — init rewrites
  // camView/camPos explicitly at the tail.
  winLo_ = 0; winHi_ = 0;
  std::memset(ringPts_, 0, sizeof ringPts_);
  std::memset(planes_, 0, sizeof planes_);
  std::memset(nodeMat_, 0, sizeof nodeMat_);
  std::memset(pens_, 0, sizeof pens_);
  std::memset(buckets_, 0, sizeof buckets_);
  drift_[0] = drift_[1] = drift_[2] = 0.0f;
  radius_ = 0.0f;
  complete_ = 0;
  completionSrc_ = StreamCompletion::kNone;
  penBase_ = 0; penTarget_ = 0;
  penT_ = 0.0f;
  std::memset(palette_, 0, sizeof palette_);
  std::memset(paletteDac_, 0, sizeof paletteDac_);
  drawDue_ = 1;                       // 0x5414d4 — no write inside
                                      // 42b270's span; the dispatcher's
                                      // prior mode inits armed it
                                      // (cold-BSS would be 0 — the
                                      // harness contract takes the
                                      // armed lifecycle value)
  hudBlink_ = 0;                      // 0x49a8dc — BSS at mode entry
  lastMs_ = 0; limSpan_ = 0; limInSeq_ = 0; limTick_ = 0;
  fade_ = 0.0f;                       // wiped, then stored again (b33e)
  hero_ = escort_ = stray_ = twin_ = marker_ = pickup_ = nullptr;
  driftMax_ = 0.0f;
  radiusMin_ = 0.0f; radiusMax_ = 0.0f;
  windHandle_ = -1;
  quit_ = false; exited_ = false; tornDown_ = false;
  lastLookT_ = 0.0f;
  poolReset();
  rng_ = rng;
  course_ = course;
  skill_ = skill;
  health_ = health;

  isFinal_ = (course >= 4) ? 1 : 0;   // CMP course,4 / JL -> 0

  // --- resource binds ---
  // FUN_0041a480("STREAM\STREAM.MTI", cb=0x41c884, &buf) — MTI table.
  // FUN_00403928("STREAM\STREAM.BNI", 0, 0) — BNI slot bind.
  seams_.resourceBind += 2;
  assets_ = a;

  // PAL: 0x240 bytes from record+0xc0 -> palette_[0xc0..0x300); then
  // 0xc0 bytes of the global system palette (0x540820) -> palette_[0].
  // Missing sources leave zeros (native: rec+0xc0 on null -> AV; the
  // port hardens — hosts bind the proven STREAM PAL + SYS_PAL head).
  if (a.palettePal)
    std::copy_n(a.palettePal, 0x240, palette_ + 0xc0);
  if (a.paletteGlobal)
    std::copy_n(a.paletteGlobal, 0xc0, palette_);

  // BG -> 03a00 (bgTag), PLANET -> 03a00 (4 sub-image slots),
  // LIGHT -> 03a00 (lightTag), sounds via 039c8+02e2c name pairs.
  // Order OBSERVED: WIND,HITSIDE,RESCUE,APPLE,HURT1..7 into slots
  // eda58,eda5c,eda7c,eda80,eda60..eda78 (resolution order differs
  // from slot order — RESCUE/APPLE bind before the HURT block).
  // All are pre-resolved host tags in StreamAssets — counted here.
  ++seams_.resourceBind;   // BG/PLANET/LIGHT + sound-slot lookups

  // FUN_00402014 — three-out scene-state query (locals -0x24/-0x28/
  // -0x2c; host-side pairing with teardown's 0x4020b4).
  ++seams_.resourceBind;

  // Proto binds: 039c8(name) -> rec; name strcpy into the 0x88-stride
  // slot; FUN_00428400(rec, slot, 1) parse; FUN_00403490(slot, &rec)
  // activate. Slots: KURT=1 (0x4edd48), BONES=2 (0x4eddd0),
  // PROFSHIP=3 (0x4ede58), slot4 (0x4edee0) = GUNTA final | SWH150
  // non-final. Port: protos arrive pre-parsed through StreamAssets;
  // the deep copy happens per-object at spawn (03720).
  seams_.resourceBind += 4;  // 039c8+28400+03490 per proto

  // FUN_00418688(0x49a828, rec) — HUD/bitmap record table bind
  // (pairs teardown's 0x418704). LIGHT image lands at 0x4edacc.
  ++seams_.resourceBind;

  // Anim-record binds (039c8 into slots): final branch binds
  // edab0=GUNTANIM; non-final binds edab0=SWHANM; both then bind
  // edaa0=BONESANIM, edaa4=KURTANIM, edaa8=FL_HVR, edaac=FL_WAVE.
  seams_.resourceBind += 5;
  // Mode byte 0x541492 = 5 (diagnostic only — this class IS mode 5).

  // --- counter seeds (skill/course, OBSERVED f64 chains) ---
  // EDX = course>>1 (arithmetic SAR — negative course shifts toward
  // -inf, matching signed semantics). skill==0/1/2 seed; any other
  // skill leaves the zeros from the wipe (OBSERVED dead branch).
  const int courseHalf = course >> 1;
  switch (skill) {
    case 0:
      driftMax_ = static_cast<float>(static_cast<double>(course) + 6.0);
      radiusMax_ = static_cast<float>(17.0 - static_cast<double>(courseHalf));
      radiusMin_ = static_cast<float>(10.0 - static_cast<double>(courseHalf));
      break;
    case 1:
      driftMax_ = static_cast<float>(static_cast<double>(course) + 8.0);
      radiusMax_ = static_cast<float>(17.0 - static_cast<double>(course));
      radiusMin_ = static_cast<float>(10.0 - static_cast<double>(courseHalf));
      break;
    case 2:
      driftMax_ = static_cast<float>(10.0 + static_cast<double>(course));
      radiusMax_ = static_cast<float>(13.0 - static_cast<double>(courseHalf));
      radiusMin_ = static_cast<float>(10.0 - static_cast<double>(courseHalf));
      break;
    default:
      break;  // skill>2 or <0: seeds stay 0 (native falls through)
  }

  // --- ring/window seed (b751..b7b7) ---
  winLo_ = 0; winHi_ = 0;
  nodeMat_[0][0] = nodeMat_[0][5] = nodeMat_[0][10] = 1.0f;  // identity
  radius_ = 10.0f;                                          // 0x41200000
  penBase_ = static_cast<int>(enemyRandNext(rng_)) & 0x3f;
  penTarget_ = static_cast<int>(enemyRandNext(rng_)) & 0x3f;
  for (int i = 0; i != 31; ++i) tunnelExtend();  // fill the whole ring

  // --- presentation block (b7be..b9fd): limiter init (0x42fb30),
  // DAC buffer alloc (0x41c884), 768B staging build, 0x406b80 base
  // install, then the 64-step 0x406d84 crossfade ramp — all host-side
  // DAC ops; counted as seams, and the resulting base install is
  // emitted as the kPaletteSet aux=0 event. ---
  limiterInit();           // 0x42fb30(0x49b6e4) — the tick record reset
  ++seams_.resourceBind;   // 0x41c884 — palette DAC buffer alloc
  ++seams_.paletteRamp;    // staging + 06b80 + 64x06d84 ramp
  paletteRamp(0);          // 0x413b40 base install -> paletteDac_
  emit(StreamEvent::kPaletteSet, 0, 0, 1.0f);

  // --- camera reset (ba03..ba40) ---
  camPos_[0] = camPos_[1] = camPos_[2] = 0.0f;   // 540b28/2c/30
  for (int i = 0; i != 12; ++i) camView_[i] = 0.0f;
  camView_[0] = camView_[5] = camView_[10] = 1.0f;  // identity 3x4
  fillSelect(0);         // FUN_0046ae60(0) — init installs mode 0 too

  // --- hero spawn (ba45..bb0f) ---
  // alloc(winLo, f32(winLo + 0.75)); +0xc = 03720(protoKurt deep copy);
  // +0x4c=90, +0x13c=0, +0x54=0 written BEFORE the euler call; the
  // euler reads s=+0x58 which alloc wiped to 0 -> all-zero xform, then
  // +0x58=1.0 after (OBSERVED — the matrix stays zero until the first
  // updater rebuilds it). +0x114=animKurt, +0xe4=-1, +0xdc=-1.0f,
  // +0x118=-1, +0xe0=30.0, +0x34=6.0, +0x28=0, +0x2c=1.0, +0x30=0,
  // +0x148 |= 0x08.
  auto spawnInit = [](DynamicObject& o, const RuntimeModel* proto,
                          const std::uint8_t* anim, float yaw, float f34) {
    if (proto) {
      o.model = deepCopyModel(*proto);          // FUN_00403720 -> +0xc
      o.elemSet = o.model.elementSet();
      o.col.elements = &o.elemSet;
    }
    o.yawDeg = yaw;        // +0x4c
    o.bankDeg = 0.0f;      // +0x13c
    o.pitchDeg = 0.0f;     // +0x54
    float m[12];
    euler6b2f8(o.pitchDeg, o.bankDeg, o.yawDeg, o.col.scale, o.field1c,
               m);  // s=0 -> out is all zeros (OBSERVED order)
    streamStoreMatrix(o, m);
    o.col.scale = 1.0f;    // +0x58
    o.animRec = anim;      // +0x114
    o.animFrame = -1;      // +0xe4
    o.animAcc = -1.0f;     // +0xdc
    o.animLatch = -1;      // +0x118
    o.animRate = 30.0f;    // +0xe0
    o.field34 = f34;       // +0x34
    o.field28 = 0.0f;      // +0x28
    o.field2c = 1.0f;      // +0x2c
    o.field30 = 0.0f;      // +0x30
    o.col.flags148 =
        static_cast<std::uint16_t>(o.col.flags148 | 0x0008u);  // +0x148
  };

  hero_ = alloc(winLo_, static_cast<float>(static_cast<double>(winLo_) +
                                           0.75));
  if (hero_) spawnInit(*hero_, a.protoKurt, a.animKurt, 90.0f, 6.0f);

  // --- escort (final) / pickup (non-final) ---
  // final:  alloc(winLo+5, f32(winLo+5.0))  [0x496f48 = 5.0 f64]
  //         proto = slot4(GUNTA), anim = edab0(GUNTANIM), +0x34 = 6.0
  // other:  alloc(winLo+16, f32(winLo)+16.0f)  [0x496f40 = 16.0 f32!]
  //         proto = slot4(SWH150), anim = edab0(SWHANM), +0x34 = 5.48
  if (isFinal_) {
    escort_ = alloc(winLo_ + 5,
                    static_cast<float>(static_cast<double>(winLo_) + 5.0));
    if (escort_)
      spawnInit(*escort_, a.protoEscort, a.animEscort, 90.0f, 6.0f);
  } else {
    pickup_ = alloc(winLo_ + 16,
                    static_cast<float>(winLo_) + 16.0f);  // f32 add —
                    // 0x496f40 is read FADD float, not the f64 table
    if (pickup_)
      spawnInit(*pickup_, a.protoEscort, a.animEscort, 0.0f,
                std::bit_cast<float>(0x40af5c29u));  // 5.48f
  }

  // --- tail (bbea..bc07): FUN_004022b8(sndWind) -> looped instance
  // handle at 0x4eda84; FUN_0046c86c mode-leave hook. ---
  if (a.sndWind >= 0) {
    windHandle_ = a.sndWind;
    emit(StreamEvent::kPlaySound, a.sndWind, 1, 0.0f);
  }
  ++seams_.resourceBind;  // 0x46c86c tail hook
  return !quit_;
}

// ===========================================================================
// FUN_0042c824 — teardown (bounded lifecycle port).
//
// OBSERVED order: FUN_00418704 (HUD record table — pairs 18688),
// FUN_0041a528 (MTI release — pairs 1a480), FUN_00403944 (BNI slot
// clear — pairs 03928); then a 0x88-stride proto-table scan releasing
// each record (the port's protos are borrowed views — nothing owned);
// then per-bucket: walk each chain and release +0x0c-gated models
// (FUN_00403880) — the pool freelist is rebuilt wholesale; then
// FUN_004020b4(0x4eda84) stops the WIND instance; 0x46c86c tail hook.
// ===========================================================================
void StreamScene::teardown() {
  seams_.resourceFree += 3;  // 18704 + 1a528 + 03944
  // Proto-slot scan: borrowed views — counted only.
  ++seams_.resourceFree;
  // Per-bucket release walk: the native frees +0x0c records in place
  // (no unlink); the port drops the owned copies then pool-resets.
  for (auto*& head : buckets_) {
    for (DynamicObject* o = head; o; o = streamNext(o)) {
      if (o->col.elements) {
        o->model = RuntimeModel{};
        o->elemSet = CollisionElementSet{};
        o->col.elements = nullptr;
      }
    }
    head = nullptr;
  }
  poolReset();
  // FUN_004020b4(eda84) — stop the WIND instance (emitted as an event
  // since the port's audio is host-side).
  if (windHandle_ >= 0) {
    emit(StreamEvent::kStopSound, windHandle_, 0, 0.0f);
    windHandle_ = -1;
  }
  // Scene-state reset — same span the next init's 0x6630 wipe covers.
  winLo_ = 0; winHi_ = 0;
  std::memset(ringPts_, 0, sizeof ringPts_);
  std::memset(planes_, 0, sizeof planes_);
  std::memset(nodeMat_, 0, sizeof nodeMat_);
  std::memset(pens_, 0, sizeof pens_);
  drift_[0] = drift_[1] = drift_[2] = 0.0f;
  radius_ = 0.0f;
  complete_ = 0;
  completionSrc_ = StreamCompletion::kNone;
  penBase_ = 0; penTarget_ = 0;
  penT_ = 0.0f;
  std::memset(palette_, 0, sizeof palette_);
  fade_ = 0.0f;
  hero_ = escort_ = stray_ = twin_ = marker_ = pickup_ = nullptr;
  driftMax_ = 0.0f;
  radiusMin_ = 0.0f; radiusMax_ = 0.0f;
  quit_ = false;
  exited_ = false;
  assets_ = StreamAssets{};
  ++seams_.resourceFree;  // 0x46c86c tail hook
  tornDown_ = true;
}

// ===========================================================================
// Phase 19A.2B — the mode-5 actor updater family, instruction-level
// ports of the captured asm (p19a_asm1.txt / p19a_asm2.txt):
//   hero    FUN_0042d24c   generic  FUN_0042cf6c
//   escort  FUN_0042d034   pickup   FUN_0042d118
//   stray   FUN_0042db0c   twinSync FUN_0042dabc
// animStep = FUN_004555bc, the shared per-object animator dispatch —
// implemented in Phase 19A.2C below (the callsites were always real;
// only the body was deferred).
// ===========================================================================
namespace {
// d57b/d60d sub-block shared by the two hero lateral offsets on the
// dead/twin path: f64 save -> ±6.25*frame-dt step toward 0 -> f32 store
// -> clamp on the f80 residue compare. The two sub-branches differ on
// unordered (the subtract side keeps NaN, the add side clears it) —
// preserved per-branch like the JBE/JNC pairs in the original.
void streamDecayOffset(float& v, float dt) {
  const double save = v;
  if (!(0.0 >= save)) {                       // v > 0 or NaN -> subtract
    const double nv = save - static_cast<double>(dt) * 6.25;
    v = static_cast<float>(nv);
    if (nv < 0.0) v = 0.0f;
  } else {                                    // v <= 0 ordered -> add
    const double nv = save + static_cast<double>(dt) * 6.25;
    v = static_cast<float>(nv);
    if (!(nv <= 0.0)) v = 0.0f;
  }
}
} // namespace

// FUN_0042d24c — the hero updater. Owns winLo progression: the
// winLo+1 boundary crossing migrates hero+twin, increments winLo_ and
// feeds tunnelExtend() — the only updater that touches the window.
void StreamScene::heroUpdate(float dt) {
  ++seams_.heroUpdate;
  DynamicObject& h = *hero_;
  // FUN_00407f2c — the input fold resolves device state into the two
  // +0x4ce758/+0x4ce75c axis globals; step() stages the resolved pair
  // into input_ at the head of the frame.
  const float axis0 = input_.axis0;
  const float axis1 = input_.axis1;
  // d25f — the gate flag (native ECX): twin present -> 0; else health
  // > 0 -> 1. Dead/twin frames still run movement, anim and the
  // transform tail but skip steering, the swim ease, and the whole
  // wall-probe/ricochet block.
  const bool gate = (twin_ == nullptr && health_ > 0);

  // d274 — speed grow: field34 < 6.0 -> += frame dt (f64 add, f32
  // store), overshoot clamps to 6.0 on the f80 residue compare.
  if (!((double)h.field34 >= 6.0)) {
    const double nv = (double)dt + (double)h.field34;
    h.field34 = (float)nv;
    if (nv > 6.0) h.field34 = 6.0f;
  }
  // d2a9 — path advance: zBias += field34 * dt.
  h.zBias = (float)((double)h.field34 * (double)dt + (double)h.zBias);

  // d2bd — window feed: fires when zBias - 0.75 passes winLo+1
  // (unordered falls INTO the block like the native JNC). Migrations
  // land in the new low bucket BEFORE the winLo_++ / tunnelExtend
  // pair — native order.
  if (!((double)(winLo_ + 1) >= (double)h.zBias - 0.75)) {
    migrate(h, winLo_ + 1);               // da40 — return ignored
    if (twin_) migrate(*twin_, winLo_ + 1);
    ++winLo_;                             // 0x4e74b0++
    tunnelExtend();                       // be4c — winHi++
  }

  animStep(h, dt);              // 555bc — animator dispatch

  // d310 — path frame at (zBias, zBias+1); basis for the rebuild.
  float pf[12];
  pathFrame(pf, h.zBias, (float)((double)h.zBias + 1.0));

  if (gate) {
    // d338 — swim ease: vel = vel*0.75 + pathdir*0.25, renormalized;
    // the unit swim vector spreads lateral/vertical drift through the
    // frame's side (pf[0,4,8]) and up (pf[2,6,10]) columns at
    // 100*frame-dt.
    h.field28 = (float)((double)h.field28 * 0.75 + (double)pf[1] * 0.25);
    h.field2c = (float)((double)h.field2c * 0.75 + (double)pf[5] * 0.25);
    h.field30 = (float)((double)h.field30 * 0.75 + (double)pf[9] * 0.25);
    float swim[3] = {h.field28, h.field2c, h.field30};
    normalizeE9c4(swim);
    h.field28 = swim[0]; h.field2c = swim[1]; h.field30 = swim[2];
    const double lat = (double)h.field2c * (double)pf[4] +
                       (double)h.field28 * (double)pf[0] +
                       (double)h.field30 * (double)pf[8];
    h.field1c[0] =
        (float)((double)h.field1c[0] + lat * 100.0 * (double)dt);
    const double ver = (double)h.field28 * (double)pf[2] +
                       (double)h.field2c * (double)pf[6] +
                       (double)h.field30 * (double)pf[10];
    h.field1c[2] =
        (float)((double)h.field1c[2] + ver * 100.0 * (double)dt);
  } else {
    // d57b — dead/twin: both offsets decay to 0 at 6.25*dt.
    streamDecayOffset(h.field1c[0], dt);
    streamDecayOffset(h.field1c[2], dt);
  }

  // d3ee/d638 — yaw steer: axis0 < 0 steers down (floor 45), > 0
  // steers up (cap 135); released or dead recenters to 90 at 180*dt.
  if (gate && axis0 < 0.0f) {
    const double nv = (double)axis0 * (double)dt + (double)h.yawDeg;
    h.yawDeg = (float)nv;
    if (!(nv >= 45.0)) h.yawDeg = 45.0f;   // JNC — NaN clamps
  } else if (gate && !(axis0 <= 0.0f)) {
    const double nv = (double)axis0 * (double)dt + (double)h.yawDeg;
    h.yawDeg = (float)nv;
    if (nv > 135.0) h.yawDeg = 135.0f;     // JBE — NaN keeps
  } else {
    const double save = h.yawDeg;
    if (!(save >= 90.0)) {
      const double nv = save + (double)dt * 180.0;
      h.yawDeg = (float)nv;
      if (nv > 90.0) h.yawDeg = 90.0f;
    } else {
      const double nv = save - (double)dt * 180.0;
      h.yawDeg = (float)nv;
      if (!(nv >= 90.0)) h.yawDeg = 90.0f;
    }
  }
  // d432/d6ee — bank steer: axis1 < 0 down (floor -45), > 0 up (cap
  // +45); released or dead recenters to 0.
  if (gate && axis1 < 0.0f) {
    const double nv = (double)axis1 * (double)dt + (double)h.bankDeg;
    h.bankDeg = (float)nv;
    if (!(nv >= -45.0)) h.bankDeg = -45.0f;
  } else if (gate && !(axis1 <= 0.0f)) {
    const double nv = (double)axis1 * (double)dt + (double)h.bankDeg;
    h.bankDeg = (float)nv;
    if (nv > 45.0) h.bankDeg = 45.0f;
  } else {
    const double save = h.bankDeg;
    if (save < 0.0) {                       // ordered < 0 -> add branch
      const double nv = save + (double)dt * 180.0;
      h.bankDeg = (float)nv;
      if (!(nv <= 0.0)) h.bankDeg = 0.0f;
    } else {                                // >= 0 or NaN -> subtract
      const double nv = save - (double)dt * 180.0;
      h.bankDeg = (float)nv;
      if (nv < 0.0) h.bankDeg = 0.0f;
    }
  }

  // d47f — steer kicks: yaw != 90 (ordered compare — the native JZ
  // skips on equal OR unordered) pumps the lateral offset by
  // cosd(yaw)*25*dt; bank != 0 pumps the vertical by sind(bank)*25*dt.
  if (!std::isnan(h.yawDeg) && h.yawDeg != 90.0f) {
    h.field1c[0] = (float)(std::cos((double)h.yawDeg * kDegToRad) * 25.0 *
                           (double)dt + (double)h.field1c[0]);
  }
  if (!std::isnan(h.bankDeg) && h.bankDeg != 0.0f) {
    h.field1c[2] = (float)(std::sin((double)h.bankDeg * kDegToRad) * 25.0 *
                           (double)dt + (double)h.field1c[2]);
  }

  // d4df — prevPos latch (MOVSD x3), then local euler + compose into
  // the live +0xac block. The transform drives the position — pos :=
  // origin, the opposite direction from the other updaters.
  h.prevPos[0] = h.pos[0];
  h.prevPos[1] = h.pos[1];
  h.prevPos[2] = h.pos[2];
  float localB[12];
  euler6b2f8(h.pitchDeg, h.bankDeg, h.yawDeg, h.col.scale, h.field1c,
             localB);
  float m[12];
  compose6aeb0(pf, localB, m);
  streamStoreMatrix(h, m);
  h.setPosition(h.col.origin[0], h.col.origin[1], h.col.origin[2]);

  if (!gate) return;                        // d557 — dead/twin RET

  // d7b7 — wall probe at the new pos; a miss (wp < 0 ordered) ends
  // the update. On hit the bank/yaw deflect off the offset ratio and
  // the offsets damp by (1-wp); the euler translation column is
  // patched in place and the transform recomposed.
  const float wp = wallProbe(h.pos, h.zBias);
  if (wp < 0.0f) return;                    // JA — unordered falls in
  const double rr = std::sqrt((double)h.field1c[0] * h.field1c[0] +
                              (double)h.field1c[2] * h.field1c[2]);
  const double k = 45.0 / rr;
  const float bank = (float)(-(double)h.field1c[2] * k);
  h.yawDeg = (float)((double)h.field1c[0] * k + 90.0);
  h.bankDeg = bank;
  const double inv = 1.0 - (double)wp;
  h.field1c[0] = (float)((double)h.field1c[0] * inv);
  h.field1c[2] = (float)((double)h.field1c[2] * inv);
  localB[3] = h.field1c[0];
  localB[7] = h.field1c[1];
  localB[11] = h.field1c[2];
  compose6aeb0(pf, localB, m);
  streamStoreMatrix(h, m);
  h.setPosition(h.col.origin[0], h.col.origin[1], h.col.origin[2]);

  // d878 — ricochet audio: HITSIDE (aux=0) then one of the 7 HURT
  // tags at rand(7) (aux = EDX call-residue in the native — the port
  // emits the stable one-shot flag 0).
  emit(StreamEvent::kPlaySound, assets_.sndHitside, 0, 0.0f);
  const int hurt = enemyRandBelow(rng_, 7);
  emit(StreamEvent::kPlaySound, assets_.sndHurt[hurt], 0, 0.0f);

  // d89d — health drain (the JLE shape is kept though gate already
  // guarantees health_ > 0): skill0 -2; skill1 -(rand(2)+2); skill2
  // -(2*rand(2)+4). Death latch: final -> health=0/complete=1/
  // fade=2.0 (the long death ramp); non-final -> health=1.
  if (health_ > 0) {
    switch (skill_) {
      case 0: health_ -= 2; break;
      case 1: health_ -= enemyRandBelow(rng_, 2) + 2; break;
      case 2: health_ -= enemyRandBelow(rng_, 2) * 2 + 4; break;
      default: break;
    }
    if (health_ <= 0) {
      if (isFinal_) {
        // d940..d961: health=0 -> complete=1 -> fade=2.0, in that
        // order; d967 falls through to the d8c8 speed decay (the
        // same-frame continuation — OBSERVED).
        if (!complete_) completionSrc_ = StreamCompletion::kDeath;
        health_ = 0;
        complete_ = 1;
        fade_ = 2.0f;                       // 0x40000000
      } else {
        health_ = 1;
      }
    }
  }
  // d8c8 — ricochet speed decay: *0.9, floor 4.5 on the f80 product.
  const double nv = (double)h.field34 * 0.9;
  h.field34 = (float)nv;
  if (!(nv >= 4.5)) h.field34 = 4.5f;
}

// FUN_0042cf6c — the scripted-prop updater. +0x34 nonzero advances
// zBias and migrates to the trunc bucket (reap on window exit skips
// the frac tail entirely); the tail writes field1c[1] = frac(zBias)*10
// (0x497048), drives pos from field1c through the node frame, clears
// +0x20 and latches the transform origin. OBSERVED: no anim tick —
// cf6c never calls 0x4555bc.
void StreamScene::genericUpdate(DynamicObject& o, float dt) {
  ++seams_.genericUpdate;
  if (o.field34 != 0.0f && !std::isnan(o.field34)) {
    // FLDZ/FCOMPP/JZ — equal OR unordered skips the advance.
    o.zBias = (float)((double)o.field34 * (double)dt + (double)o.zBias);
    const int n = (int)streamFrndInt(o.zBias);
    if (!migrate(o, n)) {                   // da40 — outside [winLo,winHi)
      reap(o);
      return;
    }
  }
  const int n = (int)streamFrndInt(o.zBias);
  o.field1c[1] = (float)(((double)o.zBias - (double)n) * 10.0);
  float p[3];
  point6afe4(o.field1c, nodeMat_[n & 0x1f], p);
  o.setPosition(p[0], p[1], p[2]);
  o.field1c[1] = 0.0f;
  o.col.origin[0] = o.pos[0];
  o.col.origin[1] = o.pos[1];
  o.col.origin[2] = o.pos[2];
}

// FUN_0042d034 — the final-mode escort: unconditional advance
// (field34*dt), migrate to the trunc bucket — on window exit reap +
// clear edab8 — then the anim tick, the live pathFrame rebuild at
// (zBias, zBias+1), and the same frac/pos/origin tail (scale
// 0x497050 = 10.0).
void StreamScene::escortUpdate(DynamicObject& o, float dt) {
  ++seams_.escortUpdate;
  o.zBias = (float)((double)o.field34 * (double)dt + (double)o.zBias);
  const int n0 = (int)streamFrndInt(o.zBias);
  if (!migrate(o, n0)) {
    reap(o);
    escort_ = nullptr;                      // DAT_004edab8 = 0
    return;
  }
  animStep(o, dt);              // 555bc — animator dispatch
  // d082 — the native pathFrame writes the live +0xac block directly.
  float m[12];
  pathFrame(m, o.zBias, (float)((double)o.zBias + 1.0));
  streamStoreMatrix(o, m);
  const int n = (int)streamFrndInt(o.zBias);
  o.field1c[1] = (float)(((double)o.zBias - (double)n) * 10.0);
  float p[3];
  point6afe4(o.field1c, nodeMat_[n & 0x1f], p);
  o.setPosition(p[0], p[1], p[2]);
  o.field1c[1] = 0.0f;
  o.col.origin[0] = o.pos[0];
  o.col.origin[1] = o.pos[1];
  o.col.origin[2] = o.pos[2];
}

// FUN_0042d118 — the non-final pickup: same advance/migrate/rebuild
// tail as the escort (frac scale 0x497058 = 10.0, edac8 cleared on
// migrate-fail), then the hero-distance gate: the f80 3-D distance vs
// the f32 5.0 constant (0x497060), JC on < — within reach it restores
// 541554 to 150 (0x96), plays APPLE with the aux=1 loop flag, reaps
// and clears the slot.
void StreamScene::pickupUpdate(DynamicObject& o, float dt) {
  ++seams_.pickupUpdate;
  o.zBias = (float)((double)o.field34 * (double)dt + (double)o.zBias);
  const int n0 = (int)streamFrndInt(o.zBias);
  if (!migrate(o, n0)) {
    reap(o);
    pickup_ = nullptr;                      // DAT_004edac8 = 0
    return;
  }
  animStep(o, dt);              // 555bc — animator dispatch
  float m[12];
  pathFrame(m, o.zBias, (float)((double)o.zBias + 1.0));
  streamStoreMatrix(o, m);
  const int n = (int)streamFrndInt(o.zBias);
  o.field1c[1] = (float)(((double)o.zBias - (double)n) * 10.0);
  float p[3];
  point6afe4(o.field1c, nodeMat_[n & 0x1f], p);
  o.setPosition(p[0], p[1], p[2]);
  o.field1c[1] = 0.0f;
  o.col.origin[0] = o.pos[0];
  o.col.origin[1] = o.pos[1];
  o.col.origin[2] = o.pos[2];
  if (!hero_) return;                       // edab4 deref is
                                           // unconditional; bounded
  const double dx = (double)o.pos[0] - (double)hero_->pos[0];
  const double dy = (double)o.pos[1] - (double)hero_->pos[1];
  const double dz = (double)o.pos[2] - (double)hero_->pos[2];
  const double dist =
      std::sqrt((dx * dx + dy * dy) + dz * dz);   // FUN_00430160 f80
  if (!(dist >= 5.0)) {                       // FCOMP f32 5.0, JC
    health_ = 0x96;                           // 541554 = 150
    emit(StreamEvent::kPlaySound, assets_.sndApple, 1, 0.0f);
    reap(o);
    pickup_ = nullptr;
  }
}

// FUN_0042db0c — the stray updater (the db0c dispatch slot — the
// companion that trails the hero's path position). field34 != 0
// advances zBias; the catch-up gate clamps the stray to
// hero.zBias + 5.0 and copies the hero speed; migrate to the trunc
// bucket reaps on window exit (no slot clear — OBSERVED); then the
// anim tick, the trailing pathFrame at (zBias-4, zBias-3) with the
// node translation patched back in, the frac/yaw euler, compose and
// the pos := origin tail.
void StreamScene::strayUpdate(DynamicObject& o, float dt) {
  ++seams_.strayUpdate;
  if (o.field34 != 0.0f && !std::isnan(o.field34)) {
    o.zBias = (float)((double)o.field34 * (double)dt + (double)o.zBias);
  }
  if (hero_) {                              // db32 — edab4 read is
    // Catch-up: stray.zBias < hero.zBias + 5.0 (or unordered) snaps
    // forward to the gate and adopts the hero's +0x34 speed.
    const double lead = (double)hero_->zBias + 5.0;
    if (!((double)o.zBias >= lead)) {
      o.zBias = (float)lead;
      o.field34 = hero_->field34;
    }
  }
  const int n = (int)streamFrndInt(o.zBias);
  if (!migrate(o, n)) {
    reap(o);
    return;
  }
  animStep(o, dt);              // 555bc — animator dispatch
  const int n2 = (int)streamFrndInt(o.zBias);
  float pf[12];
  pathFrame(pf, (float)((double)o.zBias - 4.0),
            (float)((double)o.zBias - 3.0));
  // dbd2 — the node translation is patched back over the trailing
  // frame's (the path frame's own column comes from zBias-4/-3).
  pf[3] = nodeMat_[n2 & 0x1f][3];
  pf[7] = nodeMat_[n2 & 0x1f][7];
  pf[11] = nodeMat_[n2 & 0x1f][11];
  const int n3 = (int)streamFrndInt(o.zBias);
  o.field1c[1] = (float)(((double)o.zBias - (double)n3) * 10.0);
  // dc04 — yaw-only euler (a3=a2=0, scale 1.0) over the frac-window
  // translation, then pf o localB into the live +0xac block.
  float localB[12];
  euler6b180(0.0f, 0.0f, o.yawDeg, 1.0f, o.field1c, localB);
  float m[12];
  compose6aeb0(pf, localB, m);
  streamStoreMatrix(o, m);
  o.field1c[1] = 0.0f;
  o.setPosition(o.col.origin[0], o.col.origin[1], o.col.origin[2]);
}

// FUN_0042dabc — rescue-twin mirror, runs after the object walk (the
// per-object dispatch skips the twin at cb78): pos + the 0x30-byte
// +0xac xform block copy from the hero, then the twin's anim tick.
void StreamScene::twinSync() {
  ++seams_.twinSync;
  if (!twin_ || !hero_) return;             // native derefs raw; bounded
  twin_->setPosition(hero_->pos[0], hero_->pos[1], hero_->pos[2]);
  for (int i = 0; i != 9; ++i)
    twin_->col.xform[i] = hero_->col.xform[i];
  for (int i = 0; i != 3; ++i)
    twin_->col.origin[i] = hero_->col.origin[i];
  animStep(*twin_, stepDt_);
}

// ===========================================================================
// Phase 19A.2C — FUN_004555bc, the shared per-object animator dispatch
// (instruction-level port of the captured asm, p19a_helpers5.txt
// 0x4555bc..0x4557ac). Terminal bodies in native order:
//
//   0x4555c9  +0x04 == -1        -> FUN_00455500 classless timing tail
//   0x4555d4  +0x0c == 0x4edcc0  -> the class-table "fuse" arm
//   0x4555e2  latch>=0 && frame==latch | latch==0xff00 -> resync hold
//   0x45561a  +0x114 == 0        -> return (no advance)
//   0x455628  +0x140/+0x144      -> sound-marker consume (one-shot)
//   0x45569f  record advance     -> accumulate, clamp/wrap, 55890 apply
//
// The classless and ordinary bodies live in object_animation.cpp's
// shared objectAnimTickDt (same code traversal/freefall use); the
// class-table fuse is stream-side here because the +0x0c sentinel and
// its bound chain are representable only through the stream bindings.
// ===========================================================================
const CollisionElementSet* StreamScene::classRec0() {
  return &kStreamClassRec0;
}

void StreamScene::logAnimTick(const DynamicObject& o, char body) {
  StreamAnimTick t;
  t.slot = objIndex(&o);
  t.body = body;
  const std::uint8_t* recs[5] = {assets_.animEscort, assets_.animBones,
                                 assets_.animKurt, assets_.animHvr,
                                 assets_.animWave};
  for (int i = 0; i != 5; ++i)
    if (o.animRec == recs[i] && recs[i]) t.rec = i;
  t.frame = o.animFrame;
  t.acc = o.animAcc;
  t.latch = o.animLatch;
  t.flags148 = static_cast<std::uint8_t>(o.col.flags148 & 0xffu);
  animLog_.push_back(t);
}

void StreamScene::animStep(DynamicObject& o, float dt) {
  ++seams_.animCalls;
  // 0x4555c9 — +0x04 == -1 -> FUN_00455500, the classless timing tail
  // (rate-free: +0xdc += +0xe0 * DT, frame = trunc, repeat-wrap by the
  // record bound). objectAnimTickDt owns the shared body.
  if (o.enemyIndex == 0xffff) {
    objectAnimTickDt(o, assets_.animLimit, dt, nullptr);
    ++seams_.animClassless;
    logAnimTick(o, 'C');
    return;
  }
  // 0x4555d4 — +0x0c == DAT_004edcc0 (shared class-table record 0):
  // the "fuse" arm. acc += f0 (DAT_0049b6f0 frame units — the dtSec
  // arg x30), the bound read through rec0's +0x10 chain (port:
  // assets_.animFuseBound carries the decoded dword, SAR >>0x10).
  // flags149 & 0x40 selects loop-hold vs teardown; the non-loop end
  // calls FUN_0045828c — the same in-place wipe objectTeardownNow
  // ports (keep +0x00 link / +0x60 arena).
  if (o.col.elements == &kStreamClassRec0) {
    ++seams_.animFuse;
    const std::int32_t raw =
        static_cast<std::int32_t>(assets_.animFuseBound);
    const int bound = raw >> 0x10;                  // SAR — sign kept
    o.animAcc += dt * 30.0f;                        // +0xdc += f0
    if ((o.col.flags149 & 0x40u) == 0) {
      // 0x4557d8: FCOMP bound vs acc; JBE (<= or unordered) -> 5828c.
      if (!((float)bound > o.animAcc)) {
        ++seams_.animFuseEnd;
        const CollisionObject* next = o.col.next;
        DynamicArena* arena = o.arena;              // +0x60
        o.~DynamicObject();
        new (&o) DynamicObject();
        o.col.next = next;
        o.arena = arena;
        logAnimTick(o, 'T');
        return;
      }
    } else if (!((float)bound > o.animAcc)) {
      // 0x45582d — loop end: frame = bound-1 and hold (JA covers the
      // ordered bound>acc compare; <= or NaN lands here).
      o.animFrame = static_cast<std::int16_t>(bound - 1);
      logAnimTick(o, 'F');
      return;
    }
    // 0x4557e9 — running: frame = FRNDINT(acc) truncated to i16
    // (FISTP dword -> low word store, same mod-2^16 convention).
    o.animFrame = static_cast<std::int16_t>(
        static_cast<std::int32_t>(std::trunc(o.animAcc)));
    logAnimTick(o, 'F');
    return;
  }
  // Ordinary record driver — 0x4555e2..0x455794 inside
  // objectAnimTickDt. The sound-marker arm (+0x140 name, +0x144 mark)
  // consumes inside the advance; the native emits FUN_00402160
  // (mode 0x1000e, vol 0x7fff, rate 1.0, range 50.0, pos=&+0x10) —
  // surfaced here as a name-bound kPlaySound event.
  const bool hadSound = !o.animSoundName.empty();
  const std::string soundName = o.animSoundName;
  const ObjectAnimBody body =
      objectAnimTickDt(o, assets_.animLimit, dt, nullptr);
  char letter = '?';
  switch (body) {
    case ObjectAnimBody::kClassless:  // unreachable — dispatched above
      ++seams_.animClassless; letter = 'C'; break;
    case ObjectAnimBody::kIdleHold:
      ++seams_.animHold; letter = 'H'; break;
    case ObjectAnimBody::kNoRecord:
      ++seams_.animNull; letter = 'N'; break;
    case ObjectAnimBody::kAdvance:
      ++seams_.animAdvance;
      letter = 'A';
      if (hadSound && o.animSoundName.empty()) {
        ++seams_.animSound;
        StreamEvent ev;
        ev.kind = StreamEvent::kPlaySound;
        ev.tag = -1;                  // name-bound (host resolves)
        ev.aux = 0x1000e;             // FUN_00402160 mode word
        ev.f[0] = o.pos[0]; ev.f[1] = o.pos[1]; ev.f[2] = o.pos[2];
        ev.name = soundName;
        events_.push_back(ev);
        letter = 'S';
      }
      break;
  }
  logAnimTick(o, letter);
}
// ---------------------------------------------------------------------------
// Phase 19A.2F — the draw stage (FUN_0042e684 / e100 / 17e20 / 2fb68).
// ---------------------------------------------------------------------------

// FUN_0042fb30 — the tick-record init (0x49b6e4):
//   {+0x00 mul=1.0f, +0x04 t1=1, +0x08 t2=4, +0x0c t3=1.0f,
//    +0x10 t4=f32(1.0f*1/30f), +0x14 t5=0}.
// The frame's 2fb68 normal arm calls it only when 0x49b700==0 (the
// base never having been armed) — init calls it once directly.
void StreamScene::limiterInit() {
  ++seams_.limiter;
  limiter_[0] = 0x3f800000u;                       // mul = 1.0f
  limiter_[3] = 0x3f800000u;                       // t3  = 1.0f
  limiter_[2] = 4;                                 // t2
  limiter_[1] = 1;                                 // t1
  limiter_[4] = std::bit_cast<std::uint32_t>(      // t4 = f32(t3*dt30)
      static_cast<float>(static_cast<double>(1.0f) *
                         static_cast<double>(
                             std::bit_cast<float>(0x3d088889u))));
  limiter_[5] = 0;                                 // t5
}

// FUN_0042fb68 normal arm (0x49b284==0 — demo flags are dead in mode
// 5) -> FUN_0042fcd0(0x49b6e4). OBSERVED semantics:
//   0x5414d4 = 1 unconditionally (drawDue for the NEXT frame).
//   2fcd0: base==0 -> 2fb30(rec) + base=ms, target=ms+34, return
//   (no t2/EMA on the arming call); ms<base -> base=ms; the pace-wait
//   (5414ac && 5414d4 && ms<target) is dead — 0x5414ac is only armed
//   by the 422558 wrapper's own call. t2 = delta*120/1000 (u32 div).
//   2fdc8: mul=f32(t2*0.25); t3=f32(t3*0.75 + 0.25*mul);
//   t4=f32(t3*1/30f); t5+=t2; t1=t5>>2; t5&=3; t1<1 -> {1,0};
//   t3>4.0 || t1>4 -> resync {t1=4, t3=4.0f, t4=f32(4*1/30f), t5=0}.
//   base = trunc(f64(base) + t2*8.333333333333334); target = base+34.
// t4 (0x49b6f4) is the SAME dword the fade stage consumes as its
// frame delta — the port's caller-supplied dtSec models that global.
void StreamScene::limiterRun(std::uint32_t nowMs) {
  ++seams_.limiter;
  drawDue_ = 1;                                    // 42fc9c
  if (limiterBase_ == 0) {                         // 49b700==0 arm
    limiterInit();                                 //   nested 2fb30
    limiterBase_ = nowMs;                          //   49b700 = ms
    limiterTarget_ = nowMs + 34;                   //   49b704 = ms+0x22
    lastMs_ = nowMs;
    return;
  }
  lastMs_ = nowMs;
  if (nowMs < limiterBase_) limiterBase_ = nowMs;  // fd55 back-fix
  // (the 5414ac pace-wait is skipped — dead in mode 5)
  const std::uint32_t delta = nowMs - limiterBase_;
  const std::uint32_t t2 = delta * 120u / 1000u;   // u32 like SHL/SUB+DIV
  limiter_[2] = t2;                                // rec+8
  // 2fdc8 — the EMA/divisor update.
  const float mul = static_cast<float>(            // rec+0 = f32(t2*0.25)
      static_cast<double>(t2) * 0.25);
  limiter_[0] = std::bit_cast<std::uint32_t>(mul);
  const float t3 = static_cast<float>(             // rec+0xc
      static_cast<double>(std::bit_cast<float>(limiter_[3])) * 0.75 +
      0.25 * static_cast<double>(mul));
  limiter_[3] = std::bit_cast<std::uint32_t>(t3);
  limiter_[4] = std::bit_cast<std::uint32_t>(      // rec+0x10
      static_cast<float>(static_cast<double>(t3) *
                         static_cast<double>(
                             std::bit_cast<float>(0x3d088889u))));
  const int t5 = static_cast<int>(limiter_[5]) + static_cast<int>(t2);
  int t1 = t5 >> 2;                                // arithmetic SAR
  int rem = t5 & 3;
  if (t1 < 1) { t1 = 1; rem = 0; }
  else if (static_cast<double>(t3) > 4.0 || t1 > 4) {
    // fe4c — 4-stall resync (EMA over 4.0 or divisor over 4).
    t1 = 4; rem = 0;
    limiter_[3] = 0x40800000u;                     // t3 = 4.0f
    limiter_[4] = std::bit_cast<std::uint32_t>(
        static_cast<float>(4.0 * static_cast<double>(
                               std::bit_cast<float>(0x3d088889u))));
  }
  limiter_[1] = static_cast<std::uint32_t>(t1);
  limiter_[5] = static_cast<std::uint32_t>(rem);
  limiterBase_ = static_cast<std::uint32_t>(       // fild qword ->
      static_cast<std::int64_t>(std::trunc(        //   trunc -> fistp
          static_cast<double>(t2) * 8.333333333333334 +
          static_cast<double>(limiterBase_))));
  limiterTarget_ = limiterBase_ + 34;
}

// FUN_0042e684 — the backdrop scroll. f80 chains, RC=11 truncation
// (47d59a), signed idiv wraps, OBSERVED:
//   accV = camView[6]*prev[10] - camView[10]*prev[6]   (row1.z/row2.z)
//   accU = camView[0]*prev[4]  - camView[4]*prev[0]    (row0.x/row1.x)
//   V(0x49b600) = trunc(V + (accV*0.5)*360.0f)
//   U(0x49b5fc) = trunc(U + (accU*0.5)*600.0f)   — stereo U -= 49b578
//   (541544/541548 unwritten in mode 5 — skipped), then camView is
//   copied to 49b604, then the wraps: U<0 -> 600-(-U%600),
//   U>=600 -> U%600; same for V mod 360 (U==-600 wraps to 600 — the
//   negative arm is written before the bound re-check, OBSERVED).
//   The toroidal blit itself is host-side; the event carries the
//   post-wrap accumulators and the BG tag.
void StreamScene::backdropScroll() {
  ++seams_.backdrop;
  const double accV =
      static_cast<double>(camView_[6]) * camViewPrev_[10] -
      static_cast<double>(camView_[10]) * camViewPrev_[6];
  const double accU =
      static_cast<double>(camView_[0]) * camViewPrev_[4] -
      static_cast<double>(camView_[4]) * camViewPrev_[0];
  int V = static_cast<int>(std::trunc(
      static_cast<double>(bgScroll_[1]) + accV * 0.5 * 360.0));
  int U = static_cast<int>(std::trunc(
      static_cast<double>(bgScroll_[0]) + accU * 0.5 * 600.0));
  std::memcpy(camViewPrev_, camView_, sizeof camViewPrev_);
  if (U < 0) U = 600 - (-U % 600);
  else if (U >= 600) U %= 600;
  if (V < 0) V = 360 - (-V % 360);
  else if (V >= 360) V %= 360;
  bgScroll_[0] = U;
  bgScroll_[1] = V;
  StreamEvent ev;
  ev.kind = StreamEvent::kBackdropBlit;
  ev.tag = assets_.bgTag;
  ev.f[0] = static_cast<float>(U);
  ev.f[1] = static_cast<float>(V);
  events_.push_back(ev);
}

// FUN_0046ae60 — the 0x49bbe8 projector install (OBSERVED asm
// p19a_helpers6_clean.txt): four comparators select the function
// pointer the slot receives —
//   EAX==1 -> 0x46ad58   EAX==2 -> 0x46ad9c   EAX==3 -> 0x46ade0
//   EAX==4 -> 0x46ae1c   else   -> 0x46ad20   (the default arm)
// All five bodies share one shape — sx = f32((x'+z')/z' * Sx + Bx + T),
// sy = f32((y'+z')/z' * Sy + By + T) — differing only in the .rdata
// constants they reference (0x498d84..0x498e2c, image-verified):
//   sel  addr    Sx       Bx       Sy       By
//   0    46ad20  299.95   —        180.40   —       full view
//   1    46ad58  191.95   108.0    139.95   80.0    sub-window
//   2    46ad9c  69.95    72.0     34.95    10.0
//   3    46ade0  69.95    228.0    34.95    —
//   4    46ae1c  69.95    384.0    34.95    10.0
// (T = 0.05 for all five; the no-bias forms skip one FADD — Bx=0/
// By=0 is value-identical). The port models the slot contents as the
// variant index `projectorSel_`; project6b4f8's fill arm dispatches
// on it. Both mode-5 call sites XOR EAX first — sel 0 always.
namespace {
constexpr double kProjFill[5][5] = {  // {Sx, Bx, T, Sy, By}
    {299.95,   0.0, 0.05, 180.40,  0.0},   // 46ad20
    {191.95, 108.0, 0.05, 139.95, 80.0},   // 46ad58
    { 69.95,  72.0, 0.05,  34.95, 10.0},   // 46ad9c
    { 69.95, 228.0, 0.05,  34.95,  0.0},   // 46ade0
    { 69.95, 384.0, 0.05,  34.95, 10.0}};  // 46ae1c
} // namespace

void StreamScene::fillSelect(int mode) {
  ++seams_.fillSelect;
  projectorSel_ = (mode >= 1 && mode <= 4) ? mode : 0;
}

// FUN_0046b4f8 — project v through camProj_ (0x540b80) into the 6-word
// record block {x',y',z',sx,sy,flags}:
//   flags: bit0 y'>z', bit1 y'<-z', bit2 x'>z', bit3 x'<-z',
//          bit4 (0x10) z'<f64 0.05 — near-clip zeroes sx/sy first.
//   z' != 0 -> CALL [0x49bbe8] — the fillSelect-installed variant
//   computes sx/sy (overwriting the near-clip zeros; OBSERVED: the
//   fill runs even when bit4 was set, only z'==0 skips it). The
//   fill-only sibling FUN_0046b5f0 (z'==0 -> zero both, else the
//   same indirect call) serves the model submitter's per-vertex
//   reproject — the host-side raster path consumes the kModelDraw
//   matrix directly, so no port body is needed here.
void StreamScene::project6b4f8(const float v[3], float out[6]) const {
  point6afe4(v, camProj_, out);
  const float xc = out[0], yc = out[1], zc = out[2];
  std::uint32_t flags = 0;
  // b559..b5da — mov (not or) arms: {y'>z' -> 1; -z'<=y' -> 0; else 2},
  // then x'>z' -> |=4 else x'<-z' -> |=8 (the |8 arm is SKIPPED when
  // |4 was taken — 46b580's jbe, so both never set together).
  if (yc > zc) flags = 1;
  else if (-zc <= yc) flags = 0;
  else flags = 2;
  if (xc > zc) flags |= 4;
  else if (xc < -zc) flags |= 8;
  out[3] = out[4] = 0.0f;
  if (zc < 0.05) flags |= 0x10;
  if (zc != 0.0f) {
    const double* k = kProjFill[projectorSel_];
    const double tx =
        (static_cast<double>(xc) + zc) / static_cast<double>(zc);
    const double ty =
        (static_cast<double>(yc) + zc) / static_cast<double>(zc);
    out[3] = static_cast<float>(tx * k[0] + k[1] + k[2]);
    out[4] = static_cast<float>(ty * k[3] + k[4] + k[2]);
  }
  out[5] = std::bit_cast<float>(flags);
}

// FUN_0042e620 — the ribbon band emitter. For bucket slot `seg` the
// native makes 32 calls: a 15-iteration loop over i=0..14 then the
// i=15 wrap segment as an unrolled tail (OBSERVED p19a_asm3.txt — the
// loop bound is the b[15] record address; the wrap is hand-coded):
//   call 2i   = (b[i], a[i+1], b[i+1])  plane[2i]   penBase - pen[2i]
//   call 2i+1 = (b[i], a[i], a[i+1])    plane[2i+1] penBase - pen[2i+1]
// where a = the higher slot's projected ring (bufA) and b = this
// bucket's (bufB); the tail is the same pattern with (i+1) wrapping
// to 0. penBase is the distance-banded ladder from e100 (all values
// <= -1029 — the c860 negative dispatch lands every ribbon tri on the
// 0x412970 LUT-remap filler with offset (-1029-pen)*256 + 0x540b20,
// i.e. band*64 + penByte selects a 256-entry shade row; the port's
// event tag carries the raw pen scalar and the host resolves it).
//
// Each e620 (asm-verified p19a_asm2.txt):
//   dist = eye.y*n.y + eye.x*n.x + eye.z*n.z + d   (f80 order y,x,z,+d)
//   FLDZ/FCOMPP/JBE — dist >= 0 OR unordered (NaN) -> call 0ca00,
//   dist < 0 -> return 1 (backface skip).
// 0ca00 (g1_spanemit.txt): 0x5414d4==0 gate first; then the AND of
// the three verts' +0x14 flag bytes — nonzero is the trivial reject;
// 0x4ce518 = 0x54b724 material-table install (host plumbing — the
// negative-pen dispatch never reads it, not modeled); then the OR —
// 0 means dispatch direct, nonzero takes the clipper. The port emits
// the pre-clip tri with the packed flag bytes; the host owns the
// clip+draw.
void StreamScene::ribbonEmit(std::uint32_t seg,
                             const float a[kStreamRingPts][6],
                             const float b[kStreamRingPts][6],
                             int penBase) {
  const float* pl = &planes_[seg][0];
  const std::uint8_t* pen = &pens_[seg][0];
  const auto e620 = [&](const float v0[6], const float v1[6],
                        const float v2[6], const float* plane, int p) {
    ++seams_.ribbonDraw;
    const double d =
        static_cast<double>(plane[1]) * camPos_[1] +
        static_cast<double>(plane[0]) * camPos_[0] +
        static_cast<double>(plane[2]) * camPos_[2] +
        static_cast<double>(plane[3]);
    if (d < 0.0) { ++seams_.ribbonPlaneCull; return; }
    if (drawDue_ == 0) { ++seams_.ribbonGate; return; }
    const std::uint32_t f0 = std::bit_cast<std::uint32_t>(v0[5]);
    const std::uint32_t f1 = std::bit_cast<std::uint32_t>(v1[5]);
    const std::uint32_t f2 = std::bit_cast<std::uint32_t>(v2[5]);
    if ((f0 & f1 & f2) != 0) { ++seams_.ribbonReject; return; }
    if ((f0 | f1 | f2) != 0) ++seams_.ribbonClip;
    StreamEvent ev;
    ev.kind = StreamEvent::kRibbonTri;
    ev.tag = p;
    ev.aux = static_cast<int>(f0 | (f1 << 8) | (f2 << 16));
    ev.f[0] = v0[0]; ev.f[1] = v0[1]; ev.f[2] = v0[2];
    ev.f[3] = v1[0]; ev.f[4] = v1[1]; ev.f[5] = v1[2];
    ev.f[6] = v2[0]; ev.f[7] = v2[1]; ev.f[8] = v2[2];
    events_.push_back(ev);
    ++seams_.ribbonEmit;
  };
  for (int i = 0; i != 15; ++i) {
    e620(b[i], a[i + 1], b[i + 1], pl + (2 * i) * 4,
         penBase - pen[2 * i]);
    e620(b[i], a[i], a[i + 1], pl + (2 * i + 1) * 4,
         penBase - pen[2 * i + 1]);
  }
  e620(b[15], a[0], b[0], pl + 30 * 4, penBase - pen[30]);
  e620(b[15], a[15], a[0], pl + 31 * 4, penBase - pen[31]);
}

// FUN_0042e100 — the object draw list. OBSERVED structure:
//   cur = (winHi-1)&0x1f; stop = winLo&0x1f — the exit test runs on
//   the MASKED slots BEFORE decrementing, so buckets winHi-2..winLo
//   are processed (the winHi-1 bucket's ring points feed the first
//   ribbon span but its objects are never drawn — OBSERVED quirk) and
//   a window with (winHi-winLo) ≡ 1 (mod 32) exits immediately.
//   Per bucket: project the 16 ring points of slot cur into the near
//   buffer (6b4f8 x16), 32x e620 ribbon emits for the band between
//   the higher slot's ring and cur's (pen base picked by the
//   OBSERVED distance ladder on dist = cur-winLo:
//   >25 -0x545 / 21..25 -0x505 / 16..20 -0x4c5 / 11..15 -0x485 /
//   6..10 -0x445 / <=5 -0x405), then the object chain appends
//   0x30-stride records into a shared 64-entry stack arena (count
//   persists ACROSS buckets):
//     +0x0c model != 0 -> fn=0x455e24, flag=0 — compose
//       camProj o obj+0xac -> obj+0x7c at record-build time
//     +0x0c == 0 -> fn=e55c (obj == marker_) else e49c, flag=1 —
//       rec+8 = proj z' (the sort key source)
//   then one FUN_00409a00 flush per bucket that appended >=1 record:
//   with 0x541500==1 flag-0 records call fn in chain order, flag-1
//   records defer (key = vz*3.0f + 8000.0f, 0x499f88==0 form) and the
//   drain qsorts DESCENDING by key-bit i32 — far-first painter order
//   (comparator 0x40bd2c returns keyB-keyA). The two projected-ring
//   buffers swap at the bucket tail so each ring projects once.
void StreamScene::emitDrawList() {
  ++seams_.drawList;
  std::uint32_t cur = static_cast<std::uint32_t>(winHi_ - 1) & 0x1f;
  const std::uint32_t stop = static_cast<std::uint32_t>(winLo_) & 0x1f;
  if (cur == stop) return;
  const int span = winHi_ - winLo_;
  if (span > seams_.trailMax) seams_.trailMax = span;
  int dist = span - 1;                     // [-0x40], decremented per bucket
  // The projected-ring double buffer: a = the higher slot's records
  // (native [-0x44], first filled with ring[winHi-1]), b = the current
  // bucket's ([-0x38]).
  float bufA[kStreamRingPts][6], bufB[kStreamRingPts][6];
  for (int i = 0; i != kStreamRingPts; ++i)
    project6b4f8(&ringPts_[cur][i * 3], bufA[i]);
  float (*a)[6] = bufA;
  float (*b)[6] = bufB;
  int recCount = 0;                        // shared arena [-0x1c]
  while (cur != stop) {
    const int bucketStart = recCount;      // [-0x48]
    --dist;
    const int penBase = dist > 25 ? -0x545 : dist > 20 ? -0x505
                      : dist > 15 ? -0x4c5 : dist > 10 ? -0x485
                      : dist >  5 ? -0x445 : -0x405;
    cur = (cur - 1) & 0x1f;
    for (int i = 0; i != kStreamRingPts; ++i)
      project6b4f8(&ringPts_[cur][i * 3], b[i]);
    ribbonEmit(cur, a, b, penBase);        // 32x e620 BEFORE the bucket
    // flag-1 records deferred for this bucket's flush.
    struct Pend { float key; const DynamicObject* o; float pr[6]; };
    std::vector<Pend> deferred;
    for (DynamicObject* o = buckets_[cur]; o; o = streamNext(o)) {
      if (recCount >= 64) {                // arena 0xc00/0x30
        ++seams_.drawListOverflow;         // each past-arena append
        continue;
      }
      ++recCount;
      if (o->col.elements) {               // +0x0c != 0 -> model 55e24
        const float m[12] = {
            o->col.xform[0], o->col.xform[1], o->col.xform[2],
            o->col.origin[0],
            o->col.xform[3], o->col.xform[4], o->col.xform[5],
            o->col.origin[1],
            o->col.xform[6], o->col.xform[7], o->col.xform[8],
            o->col.origin[2]};
        StreamEvent ev;
        ev.kind = StreamEvent::kModelDraw;
        ev.aux = objIndex(o);
        compose6aeb0(camProj_, m, ev.f);
        events_.push_back(ev);
      } else {                             // sprite — flag 1 deferred
        Pend p;
        p.o = o;
        project6b4f8(o->col.origin, p.pr);
        p.key = static_cast<float>(static_cast<double>(p.pr[2]) * 3.0 +
                                   8000.0);
        deferred.push_back(p);
      }
    }
    if (recCount > seams_.drawListMax) seams_.drawListMax = recCount;
    if (recCount != bucketStart) {           // e374 — flush iff appended
      ++seams_.drawFlush;                    // 09a00 for this bucket
      // Deferred drain — qsort by key bits descending (i32 compare —
      // OBSERVED far-first; comparator 0x40bd2c returns keyB-keyA).
      std::stable_sort(deferred.begin(), deferred.end(),
                       [](const Pend& x, const Pend& y) {
                         return std::bit_cast<std::int32_t>(x.key) >
                                std::bit_cast<std::int32_t>(y.key);
                       });
      for (const Pend& p : deferred) {
        const float zc = p.pr[2];
        // e49c/e55c re-project + gate z' >= f64 0.05 at EMIT time.
        if (static_cast<double>(zc) < 0.05) continue;
        const DynamicObject& o = *p.o;
        const int sx = static_cast<int>(std::trunc(p.pr[3]));
        const int sy = static_cast<int>(std::trunc(p.pr[4]));
        const int size = static_cast<int>(std::trunc(
            static_cast<double>(spriteSizeNum_) *
                static_cast<double>(o.col.scale) /
                (static_cast<double>(zc) *
                 static_cast<double>(projSizeF_))));
        const bool marker = (&o == marker_);
        StreamEvent ev;
        ev.kind = StreamEvent::kSpriteDraw;
        ev.tag = static_cast<int>(
            reinterpret_cast<std::intptr_t>(o.field108));
        ev.aux = objIndex(&o);
        ev.f[0] = static_cast<float>(sx);
        ev.f[1] = static_cast<float>(sy);
        ev.f[2] = static_cast<float>(size);
        ev.f[3] = zc;
        ev.f[4] = marker ? static_cast<float>(assets_.planetTag[1])
                         : 64.0f;   // tag5/tag4 = 0x40 regular
        ev.f[5] = marker ? static_cast<float>(assets_.planetTag[2])
                         : 64.0f;
        events_.push_back(ev);
      }
    }
    // Bucket tail — bufA<->bufB so the near ring becomes the next
    // bucket's far ring (each ring projects exactly once).
    std::swap(a, b);
  }
}

// FUN_004185fc — one transparent-keyed indexed blit. Event payload is
// the resolved {dst, w, h, src stride, key} — key is always 0 in the
// mode-5 HUD call sites.
void StreamScene::hudBlit(int tag, int srcOff, float x, float y,
                          float w, float h, float srcStride) {
  ++seams_.hudBlit;
  StreamEvent ev;
  ev.kind = StreamEvent::kHudBlit;
  ev.tag = tag;
  ev.aux = srcOff;
  ev.f[0] = x;
  ev.f[1] = y;
  ev.f[2] = w;
  ev.f[3] = h;
  ev.f[4] = srcStride;
  ev.f[5] = 0.0f;
  events_.push_back(ev);
}

// FUN_00417e20 — the mode-5 HUD. The 0x5414a0/a4/a8 scroll globals are
// set to 1000.0f at dispatcher entry (0x401506) and have no mode-5
// writers, so the (1-a0/a4)*2pi / (1-a8/a4)*2pi envelope collapses to
// zero and the resync arm never fires — the SC_STAT icon draws at its
// fixed corner position: x = 600 - w - 0x10, y = 360 - h - 0xa
// (OBSERVED). Blink phase 0x49a8dc = (49a8dc + limiter t1) & 0x1f
// AFTER the icon, before the digit gate. Digit gate: 0x540e10 <= 0 in
// mode 5 (mount timer) so digits draw when health > 20 or blink <= 15
// — the FUN_004181c0 printer: value >= 1000 -> 999; base x -12/-8/-4
// for 3/2/1 digits; divisors 100/10/1; each cell 8px wide, blit img =
// strip + digit*8, dims {8, digitH}, src stride = strip rec w.
void StreamScene::emitHud() {
  hudBlit(assets_.hudIconTag, 0,
          static_cast<float>(600 - assets_.hudIconW - 16),
          static_cast<float>(360 - assets_.hudIconH - 10),
          static_cast<float>(assets_.hudIconW),
          static_cast<float>(assets_.hudIconH),
          static_cast<float>(assets_.hudIconW));
  hudBlink_ = (hudBlink_ + static_cast<int>(limiter_[1])) & 0x1f;
  if (!(health_ > 0x14 || hudBlink_ <= 15)) return;
  // 4181c0 — health digits under the icon.
  int v = health_;
  int div;
  float cx;
  const float cy = static_cast<float>(
      360 - (assets_.hudIconH + 10) +
      ((assets_.hudIconH - assets_.hudDigitH) >> 1));
  const float bx = static_cast<float>(
      600 - (assets_.hudIconW + 16) + (assets_.hudIconW >> 1));
  if (v >= 1000) { v = 999; cx = bx - 12; div = 100; }
  else if (v >= 100) { cx = bx - 12; div = 100; }
  else if (v >= 10) { cx = bx - 8; div = 10; }
  else { cx = bx - 4; div = 1; }
  for (;;) {
    if (v < 0) break;
    const int digit = v / div;        // idiv — signed
    if (div == 1) v = -1;             // OBSERVED loop-exit quirk
    else { v -= digit * div; div /= 10; }
    hudBlit(assets_.hudDigitTag, digit * 8, cx, cy, 8.0f,
            static_cast<float>(assets_.hudDigitH),
            static_cast<float>(assets_.hudDigitW));
    cx += 8.0f;
  }
}

// ===========================================================================
// TELETYPE queue service — Phase 19A.2D. FUN_0041cf5c (clear),
// FUN_0041cad0 (post), FUN_0041cb44 (per-frame service). All OBSERVED
// asm (p19a_teletype.asm, verified vs MDK95.EXE). The service is
// engine-global (traversal 0x410920, mode-5 0x42cc90/b8, mode-6 paths
// all reach it); in mode 5 it runs once per draw block with
// EAX=[0x5414d4] (frame-due = draw enable; the 0x541544 stereo branch
// re-enters it for the second eye — unwritten in BUILD_A).
//
// It is a 4-deep ring of {f32 rate, u32 flags, char* str} feeding a
// timed two-line status display — NOT a bytecode VM: no opcode
// dispatch, no PC, no object/camera operands exist in this boundary
// (OBSERVED). The only per-entry program state is the live str cursor
// the consume loop mutates; the only escape is the literal "\n"
// two-byte sequence. The STATS.BNI record *named* TELETYPE is a RIFF
// WAVE (the mode-6 briefing typing sfx) — unrelated data, no script.
// ===========================================================================
void StreamScene::teletypeClear() {      // FUN_0041cf5c
  ++seams_.teletype;
  // OBSERVED write order: qWrite, qRead, charTimer, holdTimer. The
  // line buffers, entryFlags, curLine and the queue payloads are left
  // alone — stale bytes stay addressable by later draws.
  ttSetU32(kTtQWrite, 0);
  ttSetU32(kTtQRead, 0);
  ttSetF32(kTtChar, 0.0f);
  ttSetF32(kTtHold, 0.0f);
}

std::uint8_t StreamScene::ttStrByte(std::uint32_t cursor) const {
  const unsigned slot = cursor >> 20, off = cursor & 0xfffffu;
  // A cursor whose slot escapes the table is the corrupt-pointer case
  // (native chases the wild char* until a 0) — the port reads the
  // terminator, which is also where this arm ends up at end-of-text.
  if (slot >= ttText_.size()) return 0;
  const std::string& t = ttText_[slot];
  if (off > t.size()) return 0;
  return static_cast<std::uint8_t>(t.c_str()[off]);  // off==size -> 0
}

std::string StreamScene::ttLineStr(int line) const {
  // C-string at ttMem_ + line*36 — the native draw reads until NUL
  // with no line bound (a 36-byte-full line keeps going into line1 /
  // the scalars / the queue); the port bounds at the arena end.
  const std::size_t base = static_cast<unsigned>(line) * 36u;
  std::string s;
  for (std::size_t i = base; i < ttMem_.size() && ttMem_[i]; ++i)
    s.push_back(static_cast<char>(ttMem_[i]));
  return s;
}

void StreamScene::ttDraw(int renderer, int line, int y, float scale) {
  // FUN_00414d2c (renderer 0 — EAX?,EDX=y,EBX=str) and FUN_0041518c
  // (renderer 1 — EAX=0,EDX=y,EBX=str,+stack f32 scale).
  StreamEvent ev;
  ev.kind = StreamEvent::kTeletypeDraw;
  ev.tag = renderer;
  ev.aux = y;
  ev.f[0] = scale;
  ev.name = ttLineStr(line);
  events_.push_back(ev);
  ++seams_.teletypeDraw;
  ++ttTickDraws_;
}

bool StreamScene::teletypePost(const char* text, std::uint32_t flags,
                               float rate) {
  // FUN_0041cad0 — EAX=name, EDX=flags, [EBP+8]=rate; RET 0x4.
  ++seams_.teletypePost;
  std::uint32_t slot;
  if (flags & 2u) {                         // TEST CL,0x2 -> 0x41cb2e
    const std::uint32_t rd = (ttU32(kTtQRead) - 1u) & 3u;
    ttSetU32(kTtQRead, rd);   // committed BEFORE the resolve — a failed
    slot = rd;                // front-push keeps the decrement (quirk)
  } else {
    slot = ttU32(kTtQWrite);
  }
  const int rec = kTtQueue + static_cast<int>(slot) * 0x0c;
  // FUN_00414890 resolve -> queue[slot].str — the native stores the
  // pointer even when the lookup returns NULL, then bails (no
  // qWrite advance, no flags/rate write).
  ttText_[slot] = text ? text : "";
  ttSetU32(rec + 8, slot << 20);            // str cursor := slot base
  if (!text) return false;                  // 0x41cb25 — resolve miss
  ttSetU32(rec + 4, flags);
  ttSetF32(rec + 0, rate);
  if (!(flags & 2u))
    ttSetU32(kTtQWrite, (ttU32(kTtQWrite) + 1u) & 3u);
  return true;
}

void StreamScene::teletypeService(int drawEnable) {
  // FUN_0041cb44 — EAX = drawEnable (the 0x5414d4 frame-due flag).
  ++seams_.teletypeService;
  StreamTtTick tick{};
  ttTickDraws_ = 0;
  ttTickOv_ = 0;
  const float dt30 = std::bit_cast<float>(0x3d088889u);   // 0x49b6f4
  const std::uint32_t qr = ttU32(kTtQRead);
  // EBX = pending entry ptr — queue non-empty iff qRead != qWrite.
  const int entry = qr != ttU32(kTtQWrite) ? static_cast<int>(qr) : -1;
  // [EBP-0x24] localRate: empty queue -> dt; pending -> f32(dt*2.0)
  // (FLD f32; FMUL f64 0x4958e0; FSTP f32) — pending work makes the
  // CURRENT message expire twice as fast.
  const float localRate =
      entry >= 0
          ? static_cast<float>(static_cast<double>(dt30) * 2.0)
          : dt30;
  // x87 FLDZ;FCOMPP;JZ — the zero arm is taken on ==0 OR unordered, so
  // a NaN charTimer (line-overflow scribble) lands in the zero arm.
  auto x87Zero = [](float v) { return !(v > 0.0f) && !(v < 0.0f); };
  // Same shape vs a constant — FCOMP f64 + JZ: equal OR unordered.
  auto x87Eq = [](float v, float c) { return !(v > c) && !(v < c); };
  // The suppress gate: 0x4999d0 && 0x541548 — both nonzero skips the
  // timer update (draws already emitted). 0x541548 is never written
  // in BUILD_A so the gate is dead on real data.
  const bool suppressed = flag4999d0_ != 0 && flag541548_ != 0;

  if (x87Zero(ttF32(kTtChar))) {            // 0x41cd9b arm
    // holdTimer zero test is INTEGER — f32 bits & 0x7fffffff == 0,
    // i.e. only +/-0.0 (a NaN holdTimer takes the page-out arm).
    if ((ttU32(kTtHold) & 0x7fffffffu) != 0) {
      // ---- page-out (0x41cdba): hold counts down to zero ----
      tick.phase = 'P';
      if (drawEnable) {
        const float s2 = static_cast<float>(
            static_cast<double>(ttF32(kTtHold)) * 2.0);
        if (ttU32(kTtLine) == 1) {
          ttDraw(1, 0, 0x78, s2);                       // 0x41cdd5
        } else {                                      // 0x41ce27
          const float t = s2 * 15.0f;                 // 0x4958e8
          ttDraw(1, 0, static_cast<int>(
                           streamFrndInt(120.0f - t)),// 0x4958ec - t
                 s2);
          ttDraw(1, 1,
                 static_cast<int>(streamFrndInt(t + 120.0f)), s2);
        }
      }
      if (!suppressed) {                              // 0x41cdf6
        const float h = ttF32(kTtHold) - dt30;        // FSUBR
        ttSetF32(kTtHold, h);
        if (h < 0.0f) ttSetF32(kTtHold, 0.0f);        // JA -> 0
      }                                              // (NaN kept)
    } else if (entry >= 0) {
      // ---- entry load + consume (0x41ce98..0x41cf4d) ----
      tick.phase = 'C';
      const int rec = kTtQueue + entry * 0x0c;
      ttSetF32(kTtChar, ttF32(rec));                  // charTimer=rate
      ttSetU32(kTtFlags, ttU32(rec + 4));             // entryFlags
      ttSetU32(kTtHold, 0);                           // holdTimer=0
      ttSetU32(kTtLine, 0);                           // curLine=0
      int col = 0;                                    // EDX
      const std::uint32_t cur0 = ttU32(rec + 8);      // cursor start
      for (;;) {
        const std::uint32_t cur = ttU32(rec + 8);     // str cursor
        ttSetU32(rec + 8, cur + 1);                   // ptr++ first
        const std::uint8_t c = ttStrByte(cur);        // then read old
        if (c == 0) break;                            // -> 0x41cf26
        if (c == '\\' &&
            ttStrByte(cur + 1) == 'n') {              // "\n" escape
          ttSetU32(rec + 8, cur + 2);                 // eat the 'n'
          const unsigned pos =
              ttU32(kTtLine) * 36u + static_cast<unsigned>(col);
          if (ttU32(kTtLine) == 1) {                  // 0x41cf1e
            ttPutByte(pos, 0);   // line1 NUL -> falls into TERM below
            break;
          }
          ttPutByte(pos, 0);                          // line0 NUL
          ttSetU32(kTtLine, ttU32(kTtLine) + 1);      // curLine++
          col = 0;
          continue;
        }
        ttPutByte(ttU32(kTtLine) * 36u + static_cast<unsigned>(col),
                  c);                                 // 0x41ceca
        ++col;
      }
      // 0x41cf26 TERM: current line NUL, curLine++, qRead++ mod 4.
      ttPutByte(ttU32(kTtLine) * 36u + static_cast<unsigned>(col), 0);
      ttSetU32(kTtLine, ttU32(kTtLine) + 1);
      ttSetU32(kTtQRead, (ttU32(kTtQRead) + 1u) & 3u);
      tick.chars = static_cast<int>((ttU32(rec + 8) - cur0) & 0xfffffu);
    }
    // else — queue empty and timers dead: idle RET (0x41cbeb).
  } else if ((ttU32(kTtFlags) & 1u) && !x87Eq(ttF32(kTtHold), 0.5f)) {
    // ---- slide-in (0x41cc1a): entryFlags&1, hold ramps to 0.5 ----
    tick.phase = 'N';
    const double holdF = static_cast<double>(ttF32(kTtHold));
    if (drawEnable) {
      const float s2 = static_cast<float>(holdF * 2.0);
      if (ttU32(kTtLine) == 1) {
        ttDraw(1, 0, 0x78, s2);                       // 0x41cc4c
      } else {                                      // 0x41cca2
        const float t = s2 * 15.0f;
        ttDraw(1, 0, static_cast<int>(streamFrndInt(120.0f - t)), s2);
        ttDraw(1, 1, static_cast<int>(streamFrndInt(t + 120.0f)), s2);
      }
    }
    if (!suppressed) {                              // 0x41cc6d
      const float h = dt30 + ttF32(kTtHold);          // FADD
      ttSetF32(kTtHold, h);
      if (h > 0.5f) ttSetF32(kTtHold, 0.5f);          // else JBE keeps
    }                                                // (NaN kept)
  } else {
    // ---- steady hold (0x41cb8e): flags&1==0 OR hold x87==0.5 ----
    tick.phase = 'S';
    if (drawEnable) {
      if (flag541548_ == 0) {                         // 0x41cd30+
        if (ttU32(kTtLine) == 1) {
          ttDraw(0, 0, 0x78, 0.0f);                   // 414d2c plain
        } else {
          ttDraw(0, 0, 0x69, 0.0f);
          ttDraw(0, 1, 0x87, 0.0f);
        }
      } else {                                       // 0x41cb9f+
        if (ttU32(kTtLine) == 1) {
          ttDraw(1, 0, 0x78, 1.0f);
        } else {                                     // 0x41cd0b
          ttDraw(1, 0, 0x69, 1.0f);
          ttDraw(1, 1, 0x87, 1.0f);
        }
      }
    }
    if (!suppressed) {                              // 0x41cbcf
      const float t = ttF32(kTtChar) - localRate;     // FSUB
      ttSetF32(kTtChar, t);
      if (t < 0.0f) ttSetF32(kTtChar, 0.0f);          // JA -> 0
    }                                                // (NaN kept)
  }

  tick.qRead = static_cast<int>(ttU32(kTtQRead));
  tick.qWrite = static_cast<int>(ttU32(kTtQWrite));
  tick.curLine = static_cast<int>(ttU32(kTtLine));
  tick.flags = ttU32(kTtFlags);
  tick.charTimer = ttF32(kTtChar);
  tick.holdTimer = ttF32(kTtHold);
  tick.cursor = entry >= 0
                    ? ttU32(kTtQueue + entry * 0x0c + 8)
                    : 0;
  tick.draws = ttTickDraws_;
  tick.overflow = ttTickOv_;
  ttLog_.push_back(tick);
}

// The fade stage's 768-byte palette transforms — the four OBSERVED
// arms at 0x42cd2b/0x42ce9f/0x42ce24/0x42cda3 plus the base install.
// Source = palette_ (0x4ed758); result lands in paletteDac_ — the
// local stack buffer the native memsets/transforms then hands to
// FUN_0046d208. All multiplies are f80 chains; truncations are the
// RC=11 47d59a FRNDINT and stores take the LOW BYTE of the i32 —
// including the >255 wrap (mode 1/2/3) and the mode-3 R channel's
// fade*256 low-byte (OBSERVED: at fade==1.0 R wraps 256->0).
//   0: base install (0x413b40) — palette_ verbatim
//   1: isFinal black ramp  — buf[i] = u8(trunc(pal[i]*fade))
//   2: non-final ramp      — buf[i] = u8(trunc(pal[i]*fade +
//                                               (fade-1)*255.0))
//                            (fsubrp gives fade-1.0; NEGATIVE at
//                            fade<1 — the low-byte wrap is OBSERVED)
//   3: death, fade<=1      — R=u8(trunc(f64(fade)*256.0)) for ALL
//                            entries; G/B = u8(trunc(pal*fade))
//   4: death, fade>1       — R = min(255, pal+k) where
//                            k = trunc((2-f64(fade))*255.0) (signed
//                            add, low-byte store on <=0xff);
//                            G/B = raw palette bytes
void StreamScene::paletteRamp(int mode) {
  const double fd = static_cast<double>(fade_);
  switch (mode) {
    case 0:
      std::memcpy(paletteDac_, palette_, sizeof paletteDac_);
      break;
    case 1:
      for (int i = 0; i != 768; ++i)
        paletteDac_[i] = static_cast<std::uint8_t>(static_cast<int>(
            std::trunc(static_cast<double>(palette_[i]) * fd)));
      break;
    case 2:
      for (int i = 0; i != 768; ++i)
        paletteDac_[i] = static_cast<std::uint8_t>(static_cast<int>(
            std::trunc(static_cast<double>(palette_[i]) * fd +
                       (fd - 1.0) * 255.0)));
      break;
    case 3: {
      const std::uint8_t r = static_cast<std::uint8_t>(
          static_cast<int>(std::trunc(fd * 256.0)));
      for (int i = 0; i != 768; i += 3) {
        paletteDac_[i] = r;
        paletteDac_[i + 1] = static_cast<std::uint8_t>(
            static_cast<int>(std::trunc(
                static_cast<double>(palette_[i + 1]) * fd)));
        paletteDac_[i + 2] = static_cast<std::uint8_t>(
            static_cast<int>(std::trunc(
                static_cast<double>(palette_[i + 2]) * fd)));
      }
      break;
    }
    case 4: {
      const int k = static_cast<int>(std::trunc((2.0 - fd) * 255.0));
      for (int i = 0; i != 768; i += 3) {
        const int r = static_cast<int>(palette_[i]) + k;
        paletteDac_[i] = r > 0xff ? 0xff
                                  : static_cast<std::uint8_t>(r);
        paletteDac_[i + 1] = palette_[i + 1];
        paletteDac_[i + 2] = palette_[i + 2];
      }
      break;
    }
    default:
      break;
  }
}

// cc60..ccc7 — gated on 0x5414d4 (video enable) with a 0x541544 stereo
// variant that re-runs e684/e100/1cb44/17e20 for the second eye;
// 0x541544 has no writers in BUILD_A so only the single pass is
// modeled, and the headless port always emits — the host decides
// display.
void StreamScene::emitFrameDraw() {
  backdropScroll();    // FUN_0042e684 — scroll accumulators + blit
  emitDrawList();      // FUN_0042e100 — records + per-bucket flush
  // FUN_0041cb44 — TELETYPE service; arg = the 0x5414d4 frame-due flag
  // (nonzero inside this gate — OBSERVED 0x42cc8b/ccb3).
  teletypeService(drawDue_);
  emitHud();           // FUN_00417e20 — icon + health digits
  emit(StreamEvent::kPresent, 0, 0, 0.0f);   // 0x46c86c present+vsync
}

// ===========================================================================
// FUN_0042c8b0 — frame skeleton. Stages in OBSERVED order; the actor
// updater family (Phase 19A.2B), the FUN_004555bc animator family
// (Phase 19A.2C), the draw stage (19A.2F), the ribbon engine (19A.2G)
// and the limiter state machine are all implemented — the counted
// remainder is host/presentation surface only (see StreamSeams).
// ===========================================================================
bool StreamScene::step(const StreamInput& in, float dtSec) {
  stepLog_.clear();
  animLog_.clear();
  ttLog_.clear();
  // Post-exit latch: the native frame fn returns 1 to the dispatcher,
  // which tears the mode down — it is never re-entered. Model that
  // contract so a second step() can't emit a duplicate kExitMode.
  if (exited_) return false;
  ++frameTick_;                          // 0x49b5a4++
  stepLog_.push_back(StreamStage::kTick);
  // Frame context for the updaters — the native resolves the input
  // axes into globals inside heroUpdate's 407f2c fold and reads the
  // 0x49b6f4 frame delta directly; the port stages both here.
  input_ = in;
  stepDt_ = dtSec;

  // --- rescue-twin gate + completion (c8d1..ca82) ---
  // Entered iff health>0 && (winLo>177 || health==1) — the health==1
  // re-entry comes from cb0b (CMP ECX,1 / JZ back to c8ee).
  if (health_ > 0 && (winLo_ > 177 || health_ == 1)) {
    stepLog_.push_back(StreamStage::kTwinGate);
    if (!twin_ && !isFinal_ && hero_) {
      twin_ = alloc(hero_->enemyIndex, hero_->zBias);
      if (twin_) {
        if (assets_.protoBones) {        // +0xc = 03720(BONES slot)
          twin_->model = deepCopyModel(*assets_.protoBones);
          twin_->elemSet = twin_->model.elementSet();
          twin_->col.elements = &twin_->elemSet;
        }
        // +0xac..+0xdc raw copy — the 12-float interleaved
        // xform[9]/origin[3] block (MOVSD.REP, byte-exact in the
        // original; the port stores xform+origin contiguously).
        for (int i = 0; i != 9; ++i) twin_->col.xform[i] = hero_->col.xform[i];
        for (int i = 0; i != 3; ++i) twin_->col.origin[i] = hero_->col.origin[i];
        twin_->setPosition(hero_->pos[0], hero_->pos[1], hero_->pos[2]);
        twin_->yawDeg = hero_->yawDeg;     // +0x4c
        twin_->bankDeg = hero_->bankDeg;   // +0x13c
        twin_->pitchDeg = hero_->pitchDeg; // +0x54
        twin_->col.scale = 1.0f;           // +0x58
        hero_->col.flags148 =              // hero hides (bit3 clear)
            static_cast<std::uint16_t>(hero_->col.flags148 & 0xfff7u);
        twin_->animRec = assets_.animBones;   // +0x114 (edaa0)
        hero_->animRec = assets_.animBones;   // hero re-binds too
        twin_->animFrame = hero_->animFrame = -1;
        twin_->animAcc = hero_->animAcc = 0.0f;
        twin_->animLatch = hero_->animLatch = -2;
        twin_->animRate = hero_->animRate = 30.0f;
        emit(StreamEvent::kPlaySound, assets_.sndRescue, 0, 0.0f);
      }
    }
    // Completion gates (ca42..ca82): rescue anim past frame 0x50, or
    // final-mode winHi >= 186 (0x497008 — JC skips on <186). Two
    // DISTINCT write sites — kept separate like the native; the port
    // additionally records which one latched first (diagnostic only).
    if (twin_ && twin_->animFrame > 0x50) {
      if (!complete_) completionSrc_ = StreamCompletion::kHero;
      complete_ = 1;                    // 0x42ca55
    }
    if (isFinal_ && static_cast<double>(winHi_) >= 186.0) {
      if (!complete_) completionSrc_ = StreamCompletion::kWindow;
      complete_ = 1;                    // 0x42ca79
    }
  }

  // --- fade stage (ca83..cd14) ---
  stepLog_.push_back(StreamStage::kFade);
  auto emitPal = [this](int aux) {
    paletteRamp(aux);   // the 768B transform the native hands 46d208
    StreamEvent ev;
    ev.kind = StreamEvent::kPaletteSet;
    ev.aux = aux;
    ev.f[0] = fade_;
    events_.push_back(ev);
  };
  // 0x49b6f4 = 0x3d088889 = 1/30f — the fixed frame-dt global the
  // native uses for the fade ramp (not the caller's dt).
  const float kDt30 = std::bit_cast<float>(0x3d088889u);
  if (!(fade_ == 1.0f && !complete_)) {
    if (complete_) {
      // caa3 — fade-out: fade -= 1/30 (f32 FSUB + FST store, NOT the
      // caller's dt); fade >= 0 (or unordered) -> transform and
      // continue; fade < 0 -> clamp, terminal fill, exit.
      fade_ = static_cast<float>(static_cast<double>(fade_) -
                                 static_cast<double>(kDt30));
      if (!(fade_ < 0.0f)) {
        emitPal(health_ <= 0 ? (fade_ <= 1.0f ? 3 : 4)
                             : (isFinal_ ? 1 : 2));
      } else {
        fade_ = 0.0f;
        // cad8 / ccd8..ccf5: the terminal path memsets the 768B
        // palette buffer 0x00 (isFinal or health<=0) else 0xff, hands
        // it to 46d208, and returns 1 (mode exit) — before the draw
        // block and limiter.
        const int fill = (isFinal_ || health_ <= 0) ? 0x00 : 0xff;
        std::memset(paletteDac_, fill, sizeof paletteDac_);
        StreamEvent ev;
        ev.kind = StreamEvent::kExitMode;
        ev.aux = fill;
        events_.push_back(ev);
        stepLog_.push_back(StreamStage::kExit);
        exited_ = true;
        return false;
      }
    } else {
      // ccfa — fade-in: fade += 1/30 (same f32 constant); fade >= 1.0
      // -> clamp + install base palette (413b40); else emit the
      // per-fade transform.
      fade_ = static_cast<float>(static_cast<double>(fade_) +
                                 static_cast<double>(kDt30));
      if (fade_ >= 1.0f) {
        fade_ = 1.0f;
        emitPal(0);                    // install base palette
      } else {
        emitPal(health_ <= 0 ? (fade_ <= 1.0f ? 3 : 4)
                             : (isFinal_ ? 1 : 2));
      }
    }
  }

  // --- object walk (cb26..cb87) ---
  // Slot scan winLo..winHi-1; per object the +0x11c u16 stamp gates
  // re-entry (sign-extended i16 compare vs the i32 tick — OBSERVED:
  // the native reads [+0x11a]>>16, so stamps >= 0x8000 alias negative
  // and never equal a positive tick — preserved via int16_t).
  stepLog_.push_back(StreamStage::kObjectWalk);
  for (int s = winLo_; s < winHi_; ++s) {
    DynamicObject* o = buckets_[s & 0x1f];
    while (o) {
      DynamicObject* next = streamNext(o);  // read pre-dispatch (cb53)
      if (static_cast<std::int16_t>(
              static_cast<std::uint16_t>(o->behaviorByte)) != frameTick_) {
        o->behaviorByte =
            (o->behaviorByte & ~0xffff) | (frameTick_ & 0xffff);
        if (o == hero_)
          heroUpdate(dtSec);             // d24c
        else if (o == stray_)
          strayUpdate(*o, dtSec);        // db0c (never bound in 19A.2A)
        else if (o == escort_)
          escortUpdate(*o, dtSec);       // d034
        else if (o == pickup_)
          pickupUpdate(*o, dtSec);       // d118
        else if (o == twin_)
          ;                              // cb78 — twinSync owns it
        else
          genericUpdate(*o, dtSec);      // cf6c
      }
      o = next;
    }
  }

  // --- twin sync (cb89) ---
  stepLog_.push_back(StreamStage::kTwinSync);
  if (twin_) twinSync();                 // dabc

  // --- camera (cb97..cc51) ---
  stepLog_.push_back(StreamStage::kCamera);
  if (hero_) {
    float base[3], ahead[3], eye[3];
    pathPos(base, static_cast<float>(static_cast<double>(hero_->zBias) -
                                     0.75));          // 0x497028
    pathPos(ahead, static_cast<float>(static_cast<double>(hero_->zBias) +
                                      2.0));          // 0x497018
    for (int i = 0; i != 3; ++i) {
      ahead[i] = static_cast<float>(
          (static_cast<double>(hero_->pos[i]) -
           static_cast<double>(ahead[i])) * 1.375 +
          static_cast<double>(ahead[i]));             // 0x497030
      eye[i] = static_cast<float>(
          static_cast<double>(base[i]) * 0.4 +
          static_cast<double>(ahead[i]) * 0.6);       // 497038/497040
    }
    lastLookT_ = static_cast<float>(static_cast<double>(hero_->zBias) + 2.0);
    cameraAt(eye, lastLookT_);
  }
  ++seams_.listener;                     // 4026f8(camView_) — audio

  // --- draw + limiter (cc60..ccc7) ---
  // 0x5414d4 gate (cc60): zero skips the WHOLE draw block — backdrop,
  // draw list, teletype, HUD AND present — but the limiter still runs
  // (je 0x42ccc7). The normal-arm limiter re-arms it each frame.
  if (drawDue_) {
    stepLog_.push_back(StreamStage::kDraw);
    emitFrameDraw();
  }
  stepLog_.push_back(StreamStage::kLimiter);
  limiterRun(in.nowMs);                  // 42fb68 — frame limiter
  return true;
}

// ===========================================================================
StreamSnapshot StreamScene::snapshot() const {
  StreamSnapshot s;
  s.winLo = winLo_;
  s.winHi = winHi_;
  s.complete = complete_;
  s.completionSrc = completionSrc_;    // diagnostic-only — kept out of
                                       // the stateHash mix on purpose
  s.isFinal = isFinal_;
  s.health = health_;
  s.fade = fade_;
  s.radius = radius_;
  for (int i = 0; i != 3; ++i) s.drift[i] = drift_[i];
  s.penBase = penBase_;
  s.penTarget = penTarget_;
  s.penT = penT_;
  s.driftMax = driftMax_;
  s.radiusMin = radiusMin_;
  s.radiusMax = radiusMax_;
  s.heroIdx = objIndex(hero_);
  s.escortIdx = objIndex(escort_);
  s.markerIdx = objIndex(marker_);
  s.pickupIdx = objIndex(pickup_);
  s.twinIdx = objIndex(twin_);
  if (hero_) {
    s.heroPathT = hero_->zBias;
    s.heroRoll = hero_->yawDeg;
    s.heroPitch = hero_->bankDeg;
    s.heroSpeed = hero_->field34;
    s.heroOfsX = hero_->field28;
    s.heroOfsY = hero_->field2c;
  }
  s.twinAnimFrame = twin_ ? twin_->animFrame : -1;
  for (int i = 0; i != 3; ++i) {
    s.eye[i] = camPos_[i];
    s.upRef[i] = upRef_[i];
  }
  s.lookT = lastLookT_;
  for (int i = 0; i != 12; ++i) s.camView[i] = camView_[i];
  s.bgScroll[0] = static_cast<float>(bgScroll_[0]);
  s.bgScroll[1] = static_cast<float>(bgScroll_[1]);
  // Phase 19A.2F draw-stage globals (diagnostic).
  s.drawDue = drawDue_;
  s.hudBlink = hudBlink_;
  s.projectorSel = projectorSel_;             // 19A.2H0 — not hashed
  {
    std::uint64_t ph = 0xcbf29ce484222325ull;
    for (std::uint8_t b : paletteDac_)
      ph = (ph ^ b) * 0x100000001b3ull;
    s.paletteDacHash = ph;
  }
  for (int i = 0; i != 6; ++i) s.limiter[i] = limiter_[i];
  s.limiterBase = limiterBase_;
  s.limiterTarget = limiterTarget_;
  s.freeObjects = poolFreeCount();
  s.liveObjects = kStreamPoolSize - s.freeObjects;
  // TELETYPE service state (0x54b7a4 block) — surfaced, not hashed
  // into stateHash (canonical digests stay put); ttHash is the
  // service's own FNV-1a over the whole arena.
  s.ttQRead = ttU32(kTtQRead);
  s.ttQWrite = ttU32(kTtQWrite);
  s.ttCurLine = ttU32(kTtLine);
  s.ttEntryFlags = ttU32(kTtFlags);
  s.ttCharTimer = ttF32(kTtChar);
  s.ttHoldTimer = ttF32(kTtHold);
  std::uint64_t th = 0xcbf29ce484222325ull;
  for (std::uint8_t b : ttMem_)
    th = (th ^ b) * 0x100000001b3ull;
  s.ttHash = th;
  // FNV-1a over the deterministic sim fields (init+step+tunnel state).
  std::uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i != n; ++i)
      h = (h ^ b[i]) * 0x100000001b3ull;
  };
  mix(&winLo_, sizeof winLo_);
  mix(&winHi_, sizeof winHi_);
  mix(&complete_, sizeof complete_);
  mix(&isFinal_, sizeof isFinal_);
  mix(&health_, sizeof health_);
  mix(&fade_, sizeof fade_);
  mix(&radius_, sizeof radius_);
  mix(drift_, sizeof drift_);
  mix(&penBase_, sizeof penBase_);
  mix(&penTarget_, sizeof penTarget_);
  mix(&penT_, sizeof penT_);
  mix(&driftMax_, sizeof driftMax_);
  mix(&radiusMin_, sizeof radiusMin_);
  mix(&radiusMax_, sizeof radiusMax_);
  mix(ringPts_, sizeof ringPts_);
  mix(planes_, sizeof planes_);
  mix(nodeMat_, sizeof nodeMat_);
  mix(pens_, sizeof pens_);
  mix(palette_, sizeof palette_);
  mix(&s.heroIdx, sizeof s.heroIdx);
  mix(&s.escortIdx, sizeof s.escortIdx);
  mix(&s.markerIdx, sizeof s.markerIdx);
  mix(&s.pickupIdx, sizeof s.pickupIdx);
  mix(&s.twinIdx, sizeof s.twinIdx);
  s.stateHash = h;
  return s;
}

} // namespace mdk
