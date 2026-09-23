// object_path.cpp — Phase 11B/G5-RE implementation. See the header
// for provenance. All behavior below is OBSERVED from MDK95.EXE
// BUILD_A decompilation unless marked otherwise.
#include "object_path.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "dynamic_objects.h"

namespace mdk {
namespace {

// DAT_0049b6f0 — path frame step (1.0).
constexpr float kPathStep = 1.0f;
// DAT_0049b6f4 — the object tick dt (1/30).
constexpr float kDt = 0.03333334f;
// DAT_00497f10 / DAT_00497f18 — speed-lane deadband around +0x302.
constexpr double kLaneDeadband = 5.0;
// DAT_00497f20 — double 0.5 (lateral-pull minimum frame; displacement
// threshold scale).
constexpr double kHalf = 0.5;
// DAT_00497f28 — double -0.5 (lateral probe looks half a frame back).
constexpr double kNegHalf = -0.5;
// DAT_00497f30 / DAT_00497f34 — f32 ±360 yaw wrap for +0x100 bias.
constexpr float kWrap360 = 360.0f;
constexpr float kWrapNeg360 = -360.0f;

// FUN_0047d59a — x87 FRNDINT (round-to-nearest-even under the default
// FPU control word). The port uses lroundf elsewhere; both differ only
// on exact .5 ties which the path frame never produces in practice.
int frndint(float v) { return static_cast<int>(std::lround(v)); }

// FUN_00437f30 — norm360(deg(atan2(dy,dx))); (0,0) -> 0.
float bearingDeg(float dy, float dx) {
  if (dx == 0.0f && dy == 0.0f) return 0.0f;
  float deg = static_cast<float>(
      std::atan2(static_cast<double>(dy), static_cast<double>(dx)) *
      (180.0 / 3.14159265358979323846));
  if (deg < 0.0f) deg += kWrap360;
  return deg;
}

// FUN_00437f98 — deg -> {sin,cos}.
void sincosDeg(float deg, float* sinOut, float* cosOut) {
  const double rad = static_cast<double>(deg) *
                     (3.14159265358979323846 / 180.0);
  *sinOut = static_cast<float>(std::sin(rad));
  *cosOut = static_cast<float>(std::cos(rad));
}

const std::int32_t* pathWords(const void* rec) {
  return static_cast<const std::int32_t*>(rec);
}

// Entry i's base dword index within the record (count is dword 0;
// entry[i] = dwords [1 + i*10 .. 10 + i*10]).
int entryBase(int i) { return 1 + i * 10; }

float entryFloat(const std::int32_t* w, int i, int field) {
  float f;
  std::memcpy(&f, w + entryBase(i) + field, 4);
  return f;
}

std::int32_t entryFrame(const std::int32_t* w, int i) {
  return w[entryBase(i)];
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_00456bc8 — cubic Hermite sampler.
// ---------------------------------------------------------------------------
// Segment search: i starts at count-2 and walks DOWN while
// entry[i].frame >= f — the chosen segment is the highest i in
// [0, count-2] with frame[i] < f (or 0). e1 = entry[i+1]. The
// position polynomial is unit-interval Hermite with the stored
// tangents used raw (NOT scaled by the frame span):
//   p(t) = p0 + T0 t + (3(p1-p0) - 2T0 - T1) t^2 + (T0 + T1 - 2(p1-p0)) t^3
// where T0 = tanOut[i], T1 = tanIn[i+1], t = (f - f0)/(f1 - f0).
// No clamping: t outside [0,1] extrapolates along the segment curve.
bool pathSample(const void* rec, float frame, float out[3]) {
  if (!rec) return false;
  const std::int32_t* w = pathWords(rec);
  const std::int32_t count = w[0];
  // Port bound: the original indexes entry[count-1]/entry[seg+1]
  // unconditionally — records with fewer than two keys are nonsense.
  if (count < 2) return false;

  int seg = count - 2;
  if (seg > 0) {
    while (seg > 0 && static_cast<float>(entryFrame(w, seg)) >= frame) {
      --seg;
    }
  }
  if (seg < 0) seg = 0;
  const int i0 = seg, i1 = seg + 1;

  const float t = (frame - static_cast<float>(entryFrame(w, i0))) /
                  static_cast<float>(entryFrame(w, i1) - entryFrame(w, i0));
  for (int c = 0; c < 3; ++c) {
    // pos at +0x04..0x0c (fields 1..3); tanIn at +0x10..0x18 (5..7);
    // tanOut at +0x1c..0x24 (8..10) — but +0x24 of entry i0 is
    // entryBase+10 which overflows the 0x28 stride... the original
    // indexes entry[i0] fields by dword index i0*10 + (1+k) so
    // field k of entry i = in_EAX[i*10 + 1 + k] with k in 0..9.
    const float p0 = entryFloat(w, i0, 1 + c);
    const float p1 = entryFloat(w, i1, 1 + c);
    const float t1 = entryFloat(w, i1, 5 + c);   // tanIn of the next key
    const float t0 = entryFloat(w, i0, 8 + c);   // tanOut of this key
    const float d = p1 - p0;
    out[c] = (((t0 + t1 - d * 2.0f) * t + (d * 3.0f - t0 * 2.0f - t1)) *
                  t +
              t0) *
                 t +
             p0;
  }
  return true;
}

float pathFirstFrame(const void* rec) {
  if (!rec) return 0.0f;
  const std::int32_t* w = pathWords(rec);
  if (w[0] < 1) return 0.0f;
  return static_cast<float>(entryFrame(w, 0));
}

float pathLastFrame(const void* rec) {
  if (!rec) return 0.0f;
  const std::int32_t* w = pathWords(rec);
  if (w[0] < 1) return 0.0f;
  return static_cast<float>(entryFrame(w, w[0] - 1));
}

// Entry[i].frame — the cmd-0x80 release midpoint reads entry[1]/entry[2]
// directly (+0x2c/+0x54 for the fixed 0x28 stride). Out-of-range -> 0.
float pathEntryFrame(const void* rec, int i) {
  if (!rec) return 0.0f;
  const std::int32_t* w = pathWords(rec);
  if (i < 0 || i >= w[0]) return 0.0f;
  return static_cast<float>(entryFrame(w, i));
}

// ---------------------------------------------------------------------------
// FUN_00457264 — bind-time snap.
// ---------------------------------------------------------------------------
void objectPathSnap(DynamicObject& o) {
  // OBSERVED (0x45726a..0x4572a0): pos = sample(+0xf0); pos += +0xf4.
  float pt[3] = {0, 0, 0};
  pathSample(o.fieldEC, o.fieldF0, pt);
  o.pos[0] = pt[0] + o.fieldF4[0];
  o.pos[1] = pt[1] + o.fieldF4[1];
  o.pos[2] = pt[2] + o.fieldF4[2];
}

// ---------------------------------------------------------------------------
// FUN_00456d28 — per-frame path follower.
// ---------------------------------------------------------------------------
void objectPathFollow(DynamicObject& o, const float playerPos[3],
                      float playerYawDeg) {
  const void* path = o.fieldEC;
  if (!path) return;
  const std::int32_t* w = pathWords(path);
  const std::int32_t count = w[0];

  const float startX = o.pos[0], startY = o.pos[1], startZ = o.pos[2];

  // Sync-cursor gate (0x456d53..0x456d7c): +0xe6 >= 0 and equal to
  // FRNDINT(+0xf0) -> the object has reached the script waypoint; the
  // whole follower (advance + sample + yaw) halts.
  if (o.fieldE6 >= 0 && o.fieldE6 == frndint(o.fieldF0)) return;

  // --- speed-lane advance (0x456fdd) — +0x14b&0x10 -----------------
  if (o.col.flags14b & 0x10) {
    float s, c;
    sincosDeg(playerYawDeg, &s, &c);
    const float d = (o.pos[0] - playerPos[0]) * c +
                    (o.pos[1] - playerPos[1]) * s;
    // Lanes by approach distance vs the +0x302 setpoint (±5).
    float target;
    const double dd =
        static_cast<double>(std::bit_cast<float>(o.field302));
    if (dd + kLaneDeadband < static_cast<double>(d)) {
      target = std::bit_cast<float>(o.field306);  // far lane
    } else if (dd - kLaneDeadband > static_cast<double>(d)) {
      target = std::bit_cast<float>(o.field30e);  // near lane
    } else {
      target = std::bit_cast<float>(o.field30a);  // mid lane
    }
    // Ease +0xe8 toward the lane target at (+0x30e - +0x306)*dt*0.5.
    const float rate = (std::bit_cast<float>(o.field30e) -
                        std::bit_cast<float>(o.field306)) *
                       kDt * 0.5f;
    if (o.fieldE8 > target) {
      o.fieldE8 -= rate;
      if (o.fieldE8 < target) o.fieldE8 = target;
    } else {
      o.fieldE8 += rate;
      if (o.fieldE8 > target) o.fieldE8 = target;
    }
  }

  // --- frame advance (0x456d8f / 0x457142) --------------------------
  const float step = kPathStep * o.fieldE8;
  const bool forward = (step > 0.0f);
  if (forward) {
    if (o.fieldE6 >= 0) {
      const float cursor = static_cast<float>(o.fieldE6);
      // 0x456dc8/0x456dd4: advance freely unless the step would cross
      // the cursor — then land exactly on it.
      if (!(o.fieldF0 > cursor) && !(o.fieldF0 + step < cursor)) {
        o.fieldF0 = cursor;
      } else {
        o.fieldF0 += step;
      }
    } else {
      o.fieldF0 += step;
    }
    // wrap/release (0x456df2): bound = entry[count-1].frame - 1, and
    // the release path DECs once more -> release bound endFrame-2.
    if (count > 0) {
      const float last = static_cast<float>(entryFrame(w, count - 1) - 1);
      if (o.col.flags149 & 0x4) {
        if (o.fieldF0 >= last - 1.0f) {   // release: unbind + hold end-2
          o.fieldEC = nullptr;
          o.fieldF0 = last - 1.0f;
        }
      } else if (o.fieldF0 >= last) {
        o.fieldF0 -= last;                // wrap
      }
    }
  } else {
    if (o.fieldE6 >= 0) {
      const float cursor = static_cast<float>(o.fieldE6);
      if (!(o.fieldF0 < cursor) && !(o.fieldF0 + step > cursor)) {
        o.fieldF0 = cursor;
      } else {
        o.fieldF0 += step;
      }
    } else {
      o.fieldF0 += step;
    }
    if (count > 0) {
      const float last = static_cast<float>(entryFrame(w, count - 1) - 1);
      if (o.col.flags149 & 0x4) {
        if (o.fieldF0 < 0.0f) {
          o.fieldEC = nullptr;
          o.fieldF0 = 0.0f;
        }
      } else if (o.fieldF0 < 0.0f) {
        o.fieldF0 += last;
      }
    }
  }

  // --- sample + apply (0x456e3f) -----------------------------------
  // The original samples via the cached path pointer even when the
  // wrap/release branch just cleared +0xec — keep `path` here.
  pathSample(path, o.fieldF0, o.pos);
  o.pos[0] += o.fieldF4[0];
  o.pos[1] += o.fieldF4[1];
  const bool ghost = (o.col.flags14b & 0x8) != 0;
  if (ghost) {
    o.pos[2] = startZ;                    // 0x457208 — z preserved
  } else {
    o.pos[2] += o.fieldF4[2];
  }

  // --- lateral pull (0x456e83): +0x104 != 0 && +0xf0 > 0.5 ---------
  // Samples the path at f0-0.5 and f0 to get the local direction and
  // shifts the position by +0x104 * (-sin, +cos)(bearing).
  if (std::bit_cast<std::uint32_t>(o.field104) & 0x7fffffffu) {
    if (static_cast<double>(o.fieldF0) > kHalf) {
      float p1[3], p2[3];
      pathSample(path, static_cast<float>(
                   static_cast<double>(o.fieldF0) + kNegHalf), p1);
      pathSample(path, o.fieldF0, p2);
      const float b = bearingDeg(p2[1] - p1[1], p2[0] - p1[0]);
      float sb, cb;
      sincosDeg(b, &sb, &cb);
      o.pos[0] -= o.field104 * sb;
      o.pos[1] += o.field104 * cb;
    }
  }

  // --- face-travel yaw (0x456f2d): skipped when +0x14a&1 -----------
  if (!(o.col.flags14a & 0x1)) {
    const float dx = o.pos[0] - startX;
    const float dy = o.pos[1] - startY;
    // 0x456f58: engage only when |dx|+|dy| >= dt * 0.5.
    if (std::fabs(dx) + std::fabs(dy) >=
        static_cast<float>(kDt * kHalf)) {
      float yaw = bearingDeg(dy, dx);
      if (std::bit_cast<std::uint32_t>(o.field100) & 0x7fffffffu) {
        yaw += o.field100;
        if (yaw >= kWrap360) yaw += kWrapNeg360;
        if (yaw < 0.0f) yaw += kWrap360;
      }
      o.yawDeg = yaw;
    }
  }

  // --- ghost tail (0x457216): path displacement becomes impulse ----
  // invDt = 1/dt (FLD dt; FLD1; FDIVRP). The XY displacement is
  // accumulated into +0x294/+0x298 scaled by 30 and the position is
  // restored — the path drives velocity, not position.
  if (ghost) {
    const float invDt = 1.0f / kDt;
    o.animImpulse[0] += (o.pos[0] - startX) * invDt;
    o.animImpulse[1] += (o.pos[1] - startY) * invDt;
    o.pos[0] = startX;
    o.pos[1] = startY;
    o.pos[2] = startZ;
  }
}

} // namespace mdk
