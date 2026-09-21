// Phase 5K — normal traversal camera pose and view matrix.
// FUN_004301e0 tail (0x43042b..0x4309dd) + FUN_00431100 (overhead).
// See player_camera.h for the evidence chain.
//
// x87 NOTE: the original evaluates FLD/FMUL/FADD chains in 80-bit
// and rounds once per FSTP f32 store. This port accumulates each
// chain in double and rounds at the same store points — products
// of two f32s are exact either way; the residual difference on the
// multi-term sums is below f32 round-trip noise.

#include "core/player_camera.h"

#include "core/collision_query.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mdk {

namespace {

// FUN_00437f98 — sin/cos of a degree angle. rad = deg * kDegToRad
// (f64 product, OBSERVED stored constant 0x497924 — 4 ULP below the
// correctly-rounded pi/180), FSIN/FCOS on that product, f32 stores.
// Signature: (deg, &sin, &cos).
constexpr double kDegToRad =
    std::bit_cast<double>(0x3f91df46a2529d35ULL);

void cameraTrigDeg(float deg, float& s, float& c) {
  const double rad = static_cast<double>(deg) * kDegToRad;
  s = static_cast<float>(std::sin(rad));
  c = static_cast<float>(std::cos(rad));
}

// OBSERVED constants (MDK95.EXE BUILD_A constant pool):
//   0x49726c = -20.0f  — pitch threshold for the D pullback scale
//   0x497270 = 100.0f / 0x497274 = 0.0125f — D = (pitch+100)*pb*.0125
//   0x497278 = 5.0f    — (1-cosP) rise term on the pitch>0 branch
//   0x497280 = 0.2 f64 — shake scale
//   0x497288 = 0.5f    — scaleX denominator factor (zoom*0.5)
//   0x49728c = 360.0f / 0x497290 = 0.0016666667f (1/600) — normal
//   0x497294 = 280.0f / 0x497298 = 0.0026041667f (1/384) — alt
//   0x497228 = 0.5 f64 — the shared *0.5 on the scaleY numerator
//   0x4972a0 = 3.0 f64 — portal-test eye offset (+z)
constexpr float kPitchDThreshold = -20.0f;
constexpr float kPitchDOffset = 100.0f;
constexpr float kPitchDScale = 0.0125f;
constexpr float kRiseGain = 5.0f;
constexpr double kShakeScale = 0.2;
constexpr float kHalfF = 0.5f;
constexpr float kNormH = 360.0f;
constexpr float kNormInvW = 0.0016666667f;
constexpr float kAltH = 280.0f;
constexpr float kAltInvW = 0.0026041667f;
// Phase 5M — FUN_00430bf8 / FUN_0042b0c0 constants (OBSERVED,
// MDK95.EXE BUILD_A):
//   0x4972a8 = 5.5 f64   — eye lift above player pos for the sweep
//   0x49b780 = {0.1f,0.1f,0.1f} — camera sweep half-extents
//   0x4972b0 = +4.0f / 0x4972b4 = -4.0f — grounding-probe Z range
//   0x4972b8 = 0.5 f64   — perpendicular retry factor
//   0x3f400000 = 0.75f   — the shared FUN_004630d4 horizontal scale
//   0x496e0c = 0.0016666667f (1/600) — nudge tick -> row0 factor
constexpr double kObstructEyeLift = 5.5;
constexpr float kObstructExt[3] = {0.1f, 0.1f, 0.1f};
constexpr float kProbeUp = 4.0f;
constexpr float kProbeDown = -4.0f;
constexpr double kRetryHalf = 0.5;
constexpr float kObstructApplyScale = 0.75f;
constexpr float kNudgeTimeScale = 0.0016666667f;

// 0x430548..0x430594 — the projection scalar triple, shared by both
// matrix writers. scaleZ is the caller's (-1 normal / +1 overhead).
void cameraScales(const PlayerCameraState& st, bool altAspect,
                  float& sx, float& sy) {
  // bf0 = 1/(zoom*0.5): zoom*0.5 is the only f32 store (local_3c).
  const float hz = st.zoom * kHalfF;
  sx = static_cast<float>(1.0 / static_cast<double>(hz));
  // bf4 = 1/(zoom*(H/W)*0.5): the numerator chain stays on the FPU
  // stack (no f32 store until the reciprocal).
  const float h = altAspect ? kAltH : kNormH;
  const float iw = altAspect ? kAltInvW : kNormInvW;
  const double v = static_cast<double>(st.zoom) * h * iw * 0.5;
  sy = static_cast<float>(1.0 / v);
}

} // namespace

PlayerCameraFrame updatePlayerCamera(PlayerCameraEnvironment& env,
                                     PlayerCameraState& st) {
  PlayerCameraFrame fr;
  PlayerCameraPose& p = st.pose;
  const float px = env.playerPos[0], py = env.playerPos[1],
              pz = env.playerPos[2];

  // 0x4303d1..0x43042b region trig (rewritten here for the position
  // branch — the view tail already produced the angles): sin/cos of
  // viewYaw (locals -0x1c/-0x7c) and pitch (locals -0x84/-0x70).
  float sinYaw, cosYaw, sinP, cosP;
  cameraTrigDeg(env.viewYawDeg, sinYaw, cosYaw);
  cameraTrigDeg(env.effPitchDeg, sinP, cosP);
  p.sinPitch = sinP;  // 0x540be4 — written by the prefix; re-stated
  p.cosPitch = cosP;  // 0x540be8 — here for the pose snapshot
  p.back[0] = -sinYaw * cosP;  // 0x540b34..0x540b3c — prefix writes
  p.back[1] = -cosYaw * cosP;
  p.back[2] = sinP;

  // 0x430437..0x4304af / 0x430ad2..0x430b6d — the two position
  // branches on the effective-pitch sign (FCOMP; pitch <= 0 takes
  // the 0x430ad2 path). The height term is shared.
  const float height = st.eyeHeight - st.heightOffset; // local_48
  if (env.effPitchDeg > 0.0f) {
    const double rise = (1.0 - static_cast<double>(cosP)) * kRiseGain;
    p.pos[0] = static_cast<float>(
        static_cast<double>(px) -
            static_cast<double>(sinYaw) * st.pullback * cosP +
            rise * sinYaw);
    p.pos[1] = static_cast<float>(
        static_cast<double>(py) -
            static_cast<double>(cosYaw) * st.pullback * cosP +
            rise * cosYaw);
    p.pos[2] = static_cast<float>(
        static_cast<double>(pz) + height +
            static_cast<double>(st.pullback) * sinP);
  } else {
    float d = st.pullback;
    if (env.effPitchDeg < kPitchDThreshold)
      d = (env.effPitchDeg + kPitchDOffset) * st.pullback *
          kPitchDScale;
    // The original carries a second, always-zero trig term here
    // (local_40 zeroed by XOR EDX,EDX before the call) — omitted.
    p.pos[0] = static_cast<float>(
        static_cast<double>(px) -
            static_cast<double>(sinYaw) * d * cosP);
    p.pos[1] = static_cast<float>(
        static_cast<double>(py) -
            static_cast<double>(cosYaw) * d * cosP);
    p.pos[2] = static_cast<float>(
        static_cast<double>(pz) + height +
            static_cast<double>(d) * sinP);
  }

  // 0x4304b5..0x4304e7 — camera shake: |0x540ce4| != 0 adds
  // 0x540cf8*0.2 / 0x540cfc*0.2 (f64 scale, single f32 store).
  if (st.shakeMag != 0.0f) {
    p.pos[0] = static_cast<float>(static_cast<double>(p.pos[0]) +
                                  static_cast<double>(st.shakeX) *
                                      kShakeScale);
    p.pos[1] = static_cast<float>(static_cast<double>(p.pos[1]) +
                                  static_cast<double>(st.shakeY) *
                                      kShakeScale);
  }

  // 0x4304ed..0x430542 — banked up vector (bank = b4c + b60):
  //   up.x = sinYaw*cosB*sinP + cosYaw*sinB
  //   up.y = cosYaw*cosB*sinP - sinYaw*sinB
  //   up.z = cosB*cosP
  float sinB, cosB;
  cameraTrigDeg(env.bankDeg, sinB, cosB);
  p.up[0] = static_cast<float>(
      static_cast<double>(sinYaw) * cosB * sinP +
      static_cast<double>(cosYaw) * sinB);
  p.up[1] = static_cast<float>(
      static_cast<double>(cosYaw) * cosB * sinP -
      static_cast<double>(sinYaw) * sinB);
  p.up[2] = static_cast<float>(static_cast<double>(cosB) * cosP);

  // 0x430594..0x4306a0 — right = up x back (f32 stores into
  // locals -0x60/-0x64/-0x68), down = -(back x right) (locals
  // -0x3c/-0x40/-0x44, negated in place via XOR 0x80).
  const float back[3] = {p.back[0], p.back[1], p.back[2]};
  const float right[3] = {
      static_cast<float>(static_cast<double>(p.up[1]) * back[2] -
                         static_cast<double>(p.up[2]) * back[1]),
      static_cast<float>(static_cast<double>(p.up[2]) * back[0] -
                         static_cast<double>(p.up[0]) * back[2]),
      static_cast<float>(static_cast<double>(p.up[0]) * back[1] -
                         static_cast<double>(p.up[1]) * back[0])};
  const float down[3] = {
      static_cast<float>(-(static_cast<double>(back[1]) * right[2] -
                          static_cast<double>(back[2]) * right[1])),
      static_cast<float>(-(static_cast<double>(back[2]) * right[0] -
                          static_cast<double>(back[0]) * right[2])),
      static_cast<float>(-(static_cast<double>(back[0]) * right[1] -
                          static_cast<double>(back[1]) * right[0]))};

  // Projection scalars — scaleZ = -1.0 on the normal path
  // (0x4305f7: MOV 0xbf800000).
  cameraScales(st, env.altAspect, p.scaleX, p.scaleY);
  p.scaleZ = -1.0f;

  // 0x4306a0..0x4306bb — the FUN_00430bf8 obstruction call sits
  // HERE in the original order: after basis compute, before the
  // matrix commit, gated on 0x49b710 != 0 && |0x540d58| == +-0.
  // It may move BOTH camPos and the player position (env.playerPos
  // is in/out for that reason).
  if (st.obstructionEnabled && !env.lookActive) {
    fr.obstructionSeam = true;
    if (env.collision) applyCameraObstruction(env, st);
  }

  // 0x4306c0..0x430816 — M1 (0x540b80): projection-folded
  // world->camera. Row order right/down/back; translations
  // t = scale * (-(row . cam)), each dot accumulated in the
  // original's order (row.y*cy + row.x*cx + row.z*cz for the
  // first two rows; back row uses the local_-0x54/-0x50 spill —
  // same sum). The unscaled t values stay live for M2.
  const double tR = -(static_cast<double>(right[1]) * p.pos[1] +
                      static_cast<double>(right[0]) * p.pos[0] +
                      static_cast<double>(right[2]) * p.pos[2]);
  const double tD = -(static_cast<double>(down[1]) * p.pos[1] +
                      static_cast<double>(down[0]) * p.pos[0] +
                      static_cast<double>(down[2]) * p.pos[2]);
  const double tB = -(static_cast<double>(back[1]) * p.pos[1] +
                      static_cast<double>(back[0]) * p.pos[0] +
                      static_cast<double>(back[2]) * p.pos[2]);
  p.view[0][0] = p.scaleX * right[0];
  p.view[0][1] = p.scaleX * right[1];
  p.view[0][2] = p.scaleX * right[2];
  p.view[0][3] = static_cast<float>(static_cast<double>(p.scaleX) * tR);
  p.view[1][0] = p.scaleY * down[0];
  p.view[1][1] = p.scaleY * down[1];
  p.view[1][2] = p.scaleY * down[2];
  p.view[1][3] = static_cast<float>(static_cast<double>(p.scaleY) * tD);
  p.view[2][0] = p.scaleZ * back[0];
  p.view[2][1] = p.scaleZ * back[1];
  p.view[2][2] = p.scaleZ * back[2];
  p.view[2][3] = static_cast<float>(static_cast<double>(p.scaleZ) * tB);

  // 0x43081c..0x430912 — M2 (0x540bb0): rows basis*|basis| with
  // t = (-(row . cam))*|basis|. The sqrt lengths never leave the
  // FPU stack in the original (no f32 store).
  const double lr = std::sqrt(static_cast<double>(right[1]) * right[1] +
                              static_cast<double>(right[0]) * right[0] +
                              static_cast<double>(right[2]) * right[2]);
  const double ld = std::sqrt(static_cast<double>(down[1]) * down[1] +
                              static_cast<double>(down[0]) * down[0] +
                              static_cast<double>(down[2]) * down[2]);
  const double lb = std::sqrt(static_cast<double>(back[1]) * back[1] +
                              static_cast<double>(back[0]) * back[0] +
                              static_cast<double>(back[2]) * back[2]);
  p.basis[0][0] = static_cast<float>(static_cast<double>(right[0]) * lr);
  p.basis[0][1] = static_cast<float>(static_cast<double>(right[1]) * lr);
  p.basis[0][2] = static_cast<float>(static_cast<double>(right[2]) * lr);
  p.basis[0][3] = static_cast<float>(tR * lr);
  p.basis[1][0] = static_cast<float>(static_cast<double>(down[0]) * ld);
  p.basis[1][1] = static_cast<float>(static_cast<double>(down[1]) * ld);
  p.basis[1][2] = static_cast<float>(static_cast<double>(down[2]) * ld);
  p.basis[1][3] = static_cast<float>(tD * ld);
  p.basis[2][0] = static_cast<float>(static_cast<double>(back[0]) * lb);
  p.basis[2][1] = static_cast<float>(static_cast<double>(back[1]) * lb);
  p.basis[2][2] = static_cast<float>(static_cast<double>(back[2]) * lb);
  p.basis[2][3] = static_cast<float>(tB * lb);

  // 0x43091e..0x430979 — view-config write. Sniper rect is stored
  // when c9c != 0 && ca0 != 0 (the sniper POSE itself is Phase 5L —
  // this port still runs the normal basis above, matching the
  // original's shared block; only the rect/scale pair differs).
  if (env.sniperViewport) {
    p.modeZoom = 1.0f;
    p.viewW = 384;  p.viewH = 280;
    p.viewCX = 299; p.viewCY = 219;
    p.viewOX = 107; p.viewOY = 79;
  } else {
    p.modeZoom = 2.4f;
    p.viewW = 600;  p.viewH = 360;
    p.viewCX = 300; p.viewCY = 180;
    p.viewOX = 0;   p.viewOY = 0;
  }
  // 0x43097f..0x4309dd — 0x49b714 clear + eye->camPos portal test +
  // partner compare: caller-owned (needs the arena table).
  return fr;
}

void updatePlayerCameraOverhead(const PlayerCameraEnvironment& env,
                                PlayerCameraState& st) {
  PlayerCameraPose& p = st.pose;
  const float px = env.playerPos[0], py = env.playerPos[1],
              pz = env.playerPos[2];

  // 0x43110a..0x43116f — same scale triple as the normal block but
  // scaleZ = +1.0 (0x431169: MOV 0x3f800000), then trig on the RAW
  // locomotion yaw 0x540c2c (not 0x540b50).
  cameraScales(st, env.altAspect, p.scaleX, p.scaleY);
  p.scaleZ = 1.0f;
  float sinYaw, cosYaw;
  cameraTrigDeg(env.yawDeg, sinYaw, cosYaw);

  // camPos = (px, py, pz + 0x49b74c); rows of M2 are the original's
  // precomputed locals stored verbatim (all |row| == 1):
  //   right = (sinYaw, -cosYaw, 0), down = (-cosYaw, -sinYaw, 0),
  //   back = (0, 0, -1). tA = -(right . cam) = py*cosYaw - px*sinYaw;
  //   tB = -(down . cam) = px*cosYaw + py*sinYaw; tC = camZ.
  const float camZ = static_cast<float>(static_cast<double>(pz) +
                                        st.overheadHeight);
  const float ncy = -cosYaw, nsy = -sinYaw;
  const float tA = static_cast<float>(static_cast<double>(py) * cosYaw -
                                      static_cast<double>(px) * sinYaw);
  const float tB = static_cast<float>(static_cast<double>(px) * cosYaw +
                                      static_cast<double>(py) * sinYaw);
  p.view[0][0] = p.scaleX * sinYaw;
  p.view[0][1] = p.scaleX * ncy;
  p.view[0][2] = 0.0f;
  p.view[0][3] = static_cast<float>(static_cast<double>(p.scaleX) * tA);
  p.view[1][0] = p.scaleY * ncy;
  p.view[1][1] = p.scaleY * nsy;
  p.view[1][2] = 0.0f;
  p.view[1][3] = static_cast<float>(static_cast<double>(p.scaleY) * tB);
  // ba8 = scaleZ * -1.0 (0x4972e0 f64); bac = scaleZ * camZ.
  p.view[2][0] = 0.0f;
  p.view[2][1] = 0.0f;
  p.view[2][2] = static_cast<float>(static_cast<double>(p.scaleZ) * -1.0);
  p.view[2][3] = static_cast<float>(static_cast<double>(p.scaleZ) * camZ);
  p.basis[0][0] = sinYaw; p.basis[0][1] = ncy;
  p.basis[0][2] = 0.0f;   p.basis[0][3] = tA;
  p.basis[1][0] = ncy;    p.basis[1][1] = nsy;
  p.basis[1][2] = 0.0f;   p.basis[1][3] = tB;
  p.basis[2][0] = 0.0f;   p.basis[2][1] = 0.0f;
  p.basis[2][2] = -1.0f;  p.basis[2][3] = camZ;
  p.pos[0] = px;
  p.pos[1] = py;
  p.pos[2] = camZ;
  // OBSERVED: no view-config write, no portal tail, no b714 touch,
  // no back/up/trig-cache write — stale fields persist.
}

void applyCameraObstruction(PlayerCameraEnvironment& env,
                            PlayerCameraState& st) {
  CollisionState& cs = *env.collision;
  PlayerCameraPose& p = st.pose;
  float* camPos = p.pos;  // [EBP-0x24] — the 0x540b28 block
  if (!cs.arena) return;  // the original derefs c48 unconditionally;
                          // a null arena cannot occur in-game.

  // --- static pass (0x430c05..0x430ca9) -----------------------------------
  // eye = playerPos + (0,0,5.5); segment eye -> camPos; ext 0.1;
  // flag=0 (contact-only, no slide), scale=0.
  float eye[3] = {cs.pos[0], cs.pos[1], cs.pos[2]};
  eye[2] =
      static_cast<float>(static_cast<double>(eye[2]) + kObstructEyeLift);
  float hitPos[3];
  const CollisionNode* node = nullptr;
  const CollisionPoly* hit =
      collisionSweep(cs, eye, camPos, 0, *cs.arena, kObstructExt, 0.0f,
                     hitPos, &node);
  // Carrier re-query — only when 0x540ca4 != 0 && 0x540d3c == 0.
  if (!hit && cs.carrier && !cs.carrierBusy) {
    hit = collisionSweep(cs, eye, camPos, 0, *cs.carrier, kObstructExt,
                         0.0f, hitPos, &node);
  }

  if (hit) {
    // FUN_004301bc — 2D XY distance hitPos <-> camPos (Z ignored).
    const float ddx = hitPos[0] - camPos[0];
    const float ddy = hitPos[1] - camPos[1];
    const float dist = static_cast<float>(
        std::sqrt(static_cast<double>(ddx) * ddx +
                  static_cast<double>(ddy) * ddy));
    float nx = node->nx, ny = node->ny;
    // eye is rebuilt from the live player pos (+5.5 z) — the same
    // values here since no apply has run yet (OBSERVED re-copy).
    eye[0] = cs.pos[0];
    eye[1] = cs.pos[1];
    eye[2] = static_cast<float>(static_cast<double>(cs.pos[2]) +
                                kObstructEyeLift);
    // Plane distance of the eye vs the hit node's split plane —
    // accumulated in the original's order (y*ny+d) + (x*nx+z*nz).
    const float pd = static_cast<float>(
        (static_cast<double>(eye[1]) * node->ny + node->d) +
        (static_cast<double>(eye[0]) * nx +
         static_cast<double>(eye[2]) * node->nz));
    if (pd < 0.0f) {
      nx = -nx;
      ny = -ny;
    }
    float dx = static_cast<float>(static_cast<double>(dist) * nx);
    float dy = static_cast<float>(static_cast<double>(dist) * ny);

    bool apply = true;
    if (env.contactToken) {
      // 0x540e4c != 0 — the push lands only if a FUN_00418c60 stab of
      // the +-4.0 vertical segment at the displaced XY hits the
      // PRIMARY arena (always c48, even after a carrier hit).
      float candA[3] = {cs.pos[0] + dx, cs.pos[1] + dy,
                        cs.pos[2] + kProbeUp};
      float candB[3] = {candA[0], candA[1], cs.pos[2] + kProbeDown};
      float scratch[3];
      apply = collisionStab(*cs.arena, candA, candB, scratch) != nullptr;
      if (!apply) {
        // Retry 1 — rotate the displacement by the perpendicular:
        // dx += 0.5*dist*ny, dy -= 0.5*dist*nx (h in f64, f32 stores).
        const double h = static_cast<double>(dist) * kRetryHalf;
        const float dx0 = dx, dy0 = dy;
        dx = static_cast<float>(static_cast<double>(dx0) + h * ny);
        dy = static_cast<float>(static_cast<double>(dy0) - h * nx);
        candA[0] = candB[0] = cs.pos[0] + dx;
        candA[1] = candB[1] = cs.pos[1] + dy;
        apply =
            collisionStab(*cs.arena, candA, candB, scratch) != nullptr;
        if (!apply) {
          // Retry 2 — the mirrored perpendicular.
          dx = static_cast<float>(static_cast<double>(dx0) - h * ny);
          dy = static_cast<float>(static_cast<double>(dy0) + h * nx);
          candA[0] = candB[0] = cs.pos[0] + dx;
          candA[1] = candB[1] = cs.pos[1] + dy;
          apply = collisionStab(*cs.arena, candA, candB, scratch) !=
                  nullptr;
        }
      }
      // 0x431034 — when all three grounding probes miss, the original
      // returns here: no displacement AND no object pass.
      if (!apply) return;
    }
    if (apply) {
      // 0x430d57 — FUN_004630d4(dx, dy, 0, 0.75, 0, 0): the PLAYER is
      // pushed; the camera then follows the APPLIED delta.
      const float snap[3] = {cs.pos[0], cs.pos[1], cs.pos[2]};
      collisionApply(cs, dx, dy, 0.0f, kObstructApplyScale, nullptr,
                     nullptr);
      for (int i = 0; i < 3; ++i)
        camPos[i] = camPos[i] + (cs.pos[i] - snap[i]);
    }
  }

  // --- object pass (0x430db2..0x430e8b) -----------------------------------
  // 0x540c68 gate. eye rebuilt from the live player pos (+5.5 z);
  // segTarget starts at the live camPos (0x540b28 global read).
  if (cs.arenaValid) {
    eye[0] = cs.pos[0];
    eye[1] = cs.pos[1];
    eye[2] = static_cast<float>(static_cast<double>(cs.pos[2]) +
                                kObstructEyeLift);
    float target[3] = {camPos[0], camPos[1], camPos[2]};
    for (const CollisionObject* obj = cs.arena->objects; obj;
         obj = obj->next) {
      if (!obj->named || !obj->model || (obj->flags148 & 0x810) != 0 ||
          (obj->flags14b & 0x01) == 0)
        continue;
      if (!collisionSegAabbOverlap(eye, target, obj->aabb, kObstructExt))
        continue;
      const CollisionElementSet* set = obj->elements;
      if (!set) continue;
      for (int e = 0; e < set->count; ++e) {
        float clamp[3];
        if (collisionSegAabbResolve(eye, target, set->elems[e].aabb,
                                    clamp, nullptr) == 1)
          std::memcpy(target, clamp, sizeof(target));
      }
    }
    // The tail apply runs unconditionally — even a zero delta (the
    // object list may be empty or no element clipped the target).
    const float snap[3] = {cs.pos[0], cs.pos[1], cs.pos[2]};
    collisionApply(cs, target[0] - camPos[0], target[1] - camPos[1],
                   0.0f, kObstructApplyScale, nullptr, nullptr);
    for (int i = 0; i < 3; ++i)
      camPos[i] = camPos[i] + (cs.pos[i] - snap[i]);
  }
  // Keep the env channel coherent with the authoritative store
  // (0x540bfc == cs.pos) for the runtime's copy-back.
  for (int i = 0; i < 3; ++i) env.playerPos[i] = cs.pos[i];
}

void cameraNudge(int arg, PlayerCameraState& st) {
  PlayerCameraPose& p = st.pose;
  // 0x42b0c9..0x42b10a — pos += M2row0 * (arg * 0x49b570); the
  // product arg*shift stays on the FPU, each add stores f32.
  const double s = static_cast<double>(arg) * st.nudgeShift;
  for (int i = 0; i < 3; ++i)
    p.pos[i] = static_cast<float>(
        static_cast<double>(p.basis[0][i]) * s +
        static_cast<double>(p.pos[i]));
  // 0x42b110..0x42b12f — tick = FRNDINT(arg * 0x49b574) under the
  // truncation control word (FUN_0047d59a): toward zero, not
  // round-nearest. The int store at 0x49b578 persists across the
  // caller's block restore.
  const int tick =
      static_cast<int>(static_cast<double>(arg) * st.nudgeScale);
  st.nudgeTick = tick;
  const double r = static_cast<double>(tick) * kNudgeTimeScale;
  // 0x42b131..0x42b16d — M1 row0 += M1 row2 * r (the dc cb byte pair
  // at 0x42b163 is FMUL ST(3),ST(0): row0[2] += row2[2]*r too).
  for (int i = 0; i < 3; ++i)
    p.view[0][i] = static_cast<float>(
        static_cast<double>(p.view[2][i]) * r +
        static_cast<double>(p.view[0][i]));
  // 0x42b173..0x42b1ef — all three translations refolded against the
  // UPDATED row0/camPos, original accumulation order
  // (row[1]*cy + row[0]*cx) + row[2]*cz, negated, f32 store.
  for (int row = 0; row < 3; ++row)
    p.view[row][3] = static_cast<float>(-(
        static_cast<double>(p.view[row][1]) * p.pos[1] +
        static_cast<double>(p.view[row][0]) * p.pos[0] +
        static_cast<double>(p.view[row][2]) * p.pos[2]));
}

void cameraNudgeApplyMode(PlayerCameraState& st, int mode) {
  // Jump table 0x42b1fc: [0]=0x42b21b (12.0), [1]=0x42b229 (20.0),
  // [2]=0x42b237 (15.0), [3]=0x42b229 (20.0); >3 -> 20.0.
  st.nudgeScale = (mode == 0) ? 12.0f : (mode == 2) ? 15.0f : 20.0f;
}

} // namespace mdk
