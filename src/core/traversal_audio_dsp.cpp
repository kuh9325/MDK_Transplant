#include "core/traversal_audio_dsp.h"

#include <cmath>

namespace mdk {

namespace {

// OBSERVED image constants (see the header for the address map).
constexpr double kDopplerSpeed = 1100.0;   // 0x494198 / 0x4941d8
constexpr float kDopplerNum = 30.0f;       // 0x494190 / 0x4941d0
constexpr double kDopplerLo = 0.25;        // 0x4941a0 / 0x4941e0
constexpr double kDopplerHi = 3.0;         // 0x4941a8 / 0x4941e8
constexpr float kPanScale = 32767.0f;      // 0x4941b0
constexpr double kMinDist = 20.0;          // 0x4941b8
constexpr double kMaxDist = 250.0;         // 0x4941c0
constexpr double kFalloff = 1.0 / 230.0;   // 0x4941c8
constexpr double kConeInner2 = 4.0;        // 0x4941f0 — lat^2 gate
constexpr double kConeRadius = 2.0;        // 0x4941f8
constexpr double kAxialNum = 400.0 * 0.75; // 0x494208 * 0x494210 = 300
constexpr double kConeEdge = 1.3;          // 0x494218
constexpr double kVolClamp = 32767.0;      // 0x494220
constexpr double kMbScale = 2500.0 / 32767.0; // 0x498f50
constexpr int kMbBias = 0x9c4;             // 2500

// Source position select — inst+0xa (mode bits 16..23): bit0 baked
// +0x18, bit1 the live-pos pointer (modeled: caller already refreshed
// v.pos from the owner), bit2 the matrix-offset form (not emitted by
// any ported callsite — treated as baked, documented seam), else
// (0,0,0).
void sourcePos(const TraversalAudioVoice& v,
               float& px, float& py, float& pz) {
  const std::uint32_t sel = (v.mode >> 16) & 0x7;
  if (sel == 0) {
    px = py = pz = 0.0f;
  } else {
    px = v.pos[0];
    py = v.pos[1];
    pz = v.pos[2];
  }
}

// The doppler block shared by both updaters (mode&8, prevDist>=0):
// shift = 1 + (prevDist - dist)*30/(frame*1100), clamp [0.25, 3.0].
void doppler_(TraversalAudioVoice& v, const TraversalAudioListener& l,
              float dist, TraversalAudioPush& out) {
  double shift = static_cast<double>(v.prevDist - dist) *
                 static_cast<double>(kDopplerNum) /
                 (static_cast<double>(l.frame) * kDopplerSpeed);
  shift += 1.0;
  if (shift < kDopplerLo) shift = kDopplerLo;
  else if (shift > kDopplerHi) shift = kDopplerHi;
  // FILD recRate; FMUL shift; FMUL rate; FISTP — x87 trunc (ftol).
  v.freqHz = static_cast<int>(
      static_cast<double>(v.recRateHz) * shift *
      static_cast<double>(v.rate));
  out.freq = true;
}

// FUN_0040282c — the 2D updater (normal traversal).
void update2d_(TraversalAudioVoice& v, const TraversalAudioListener& l,
               bool prevOk, TraversalAudioPush& out) {
  if ((v.mode & 0xe) == 0) return;
  float px, py, pz;
  sourcePos(v, px, py, pz);
  const float rx = l.m[0][0] * px + l.m[0][1] * py + l.m[0][2] * pz +
                   l.m[0][3];
  const float ry = l.m[1][0] * px + l.m[1][1] * py + l.m[1][2] * pz +
                   l.m[1][3];
  const float rz = l.m[2][0] * px + l.m[2][1] * py + l.m[2][2] * pz +
                   l.m[2][3];
  const double d2 = static_cast<double>(rx) * rx +
                    static_cast<double>(ry) * ry +
                    static_cast<double>(rz) * rz;
  // FLDZ/FCOMP -> JNC keeps the pre-stored value: d2<=0 -> dist 1.0.
  const float dist = (d2 > 0.0) ? static_cast<float>(std::sqrt(d2))
                                : 1.0f;
  if (prevOk && (v.mode & 0x8)) doppler_(v, l, dist, out);
  if (v.mode & 0x4) {
    const float inv = 1.0f / dist;
    const float nx = rx * inv;
    const float nz = rz * inv;
    v.pan = static_cast<int>((nz * l.m[2][0] - nx * l.m[2][1]) *
                             kPanScale);
    if (prevOk) out.pan = true;
  }
  if (v.mode & 0x2) {
    if (dist < static_cast<float>(kMinDist)) {
      v.effVol = static_cast<int>(v.baseVol);
    } else if (dist > static_cast<float>(kMaxDist)) {
      v.effVol = 0;
    } else {
      v.effVol = static_cast<int>(
          static_cast<double>(v.baseVol) *
          (kMaxDist - static_cast<double>(dist)) * kFalloff);
    }
    if (prevOk) out.vol = true;
  }
  v.prevDist = dist;
}

// FUN_00402b00 — the 3D updater (sniper scope). No pan channel exists
// in this updater (OBSERVED — the mode&4 block is absent).
void update3d_(TraversalAudioVoice& v, const TraversalAudioListener& l,
               bool prevOk, TraversalAudioPush& out) {
  if ((v.mode & 0xe) == 0) return;
  float px, py, pz;
  sourcePos(v, px, py, pz);
  const float rx = l.m[0][0] * px + l.m[0][1] * py + l.m[0][2] * pz +
                   l.m[0][3];
  const float ry = l.m[1][0] * px + l.m[1][1] * py + l.m[1][2] * pz +
                   l.m[1][3];
  const float rz = l.m[2][0] * px + l.m[2][1] * py + l.m[2][2] * pz +
                   l.m[2][3];
  const double d2 = static_cast<double>(rx) * rx +
                    static_cast<double>(ry) * ry +
                    static_cast<double>(rz) * rz;
  const float dist = (d2 > 0.0) ? static_cast<float>(std::sqrt(d2))
                                : 0.0f;
  if (prevOk && (v.mode & 0x8)) doppler_(v, l, dist, out);
  if (v.mode & 0x2) {
    double latX = rx, latY = ry;
    const double az = rz;
    const double lat2 = static_cast<double>(latX) * latX +
                        static_cast<double>(latY) * latY;
    if (lat2 > kConeInner2) {
      // Outside the inner cone — keep only the excess beyond the
      // radius-2 core, then fold the scope-aperture aspect/zoom.
      const double lat = std::sqrt(lat2);
      const double excess = 1.0 - kConeRadius / lat;
      latX *= excess * (2.0 / static_cast<double>(l.zoom));
      latY *= excess *
              (2.0 * 384.0 / (280.0 * static_cast<double>(l.zoom)));
    } else {
      latX = 0.0;
      latY = 0.0;
    }
    if (az > 0.0) {
      v.effVol = 0;   // behind the listener plane
    } else {
      double axial = (kAxialNum / static_cast<double>(l.zoom)) /
                     std::fabs(az);
      if (axial > 1.0) axial = 1.0;
      double cone = kConeEdge -
                    std::sqrt(latX * latX + latY * latY) /
                        std::fabs(az);
      double vf = cone * axial;
      if (vf < 0.0) vf = 0.0;
      double vol = static_cast<double>(v.baseVol) * vf;
      if (vol > kVolClamp) vol = kVolClamp;
      v.effVol = static_cast<int>(vol);
    }
    if (prevOk) out.vol = true;
  }
  v.prevDist = dist;
}

} // namespace

void traversalAudioVoiceInit(TraversalAudioVoice& v, std::uint32_t mode,
                             const float pos[3], int vol, float rate,
                             float range, int recRateHz) {
  v.mode = mode;
  if (pos) {
    v.pos[0] = pos[0];
    v.pos[1] = pos[1];
    v.pos[2] = pos[2];
  }
  v.baseVol = static_cast<float>(vol);   // inst+0x30 = stack vol arg
  v.rate = rate;                          // inst+0x34
  v.range = range;                        // inst+0x38
  // inst+0x3c: (mode&1) ? vol : 0 — positional callsites all carry
  // bit0 clear, so the buffer starts at the silent seed and the
  // updater's first push (tick 2) writes the real level.
  v.effVol = (mode & 1) ? vol : 0;
  v.prevDist = -1.0f;                     // inst+0x24 sentinel
  v.pan = 0;                              // inst+0x40
  v.recRateHz = recRateHz;
  // inst+0x44 = trunc(recRate * rate) — FILD/FMUL/FISTP.
  v.freqHz = static_cast<int>(static_cast<double>(recRateHz) *
                              static_cast<double>(rate));
}

TraversalAudioPush traversalAudioVoiceTick(
    TraversalAudioVoice& v, const TraversalAudioListener& l) {
  TraversalAudioPush out;
  // FUN_004026f8's per-node block: the +0x9 latch test reads the
  // PRE-tick prevDist — the -1.0 sentinel's first update computes
  // fields without pushing; a latched instance pushes once, then
  // loses its updater bits and the latch clears.
  const bool latch = (v.mode & 0x100u) != 0;
  const bool prevOk = v.prevDist >= 0.0f;
  if (l.mode3d) {
    update3d_(v, l, prevOk, out);
  } else {
    update2d_(v, l, prevOk, out);
  }
  if (latch && prevOk) {
    const std::uint32_t b0 = v.mode & 0xffu;
    if (b0 & 0xe) {
      v.mode = (v.mode & ~0xffu) | ((b0 & 0xf0u) | 0x1u);
    }
    v.mode ^= 0x100u;
  }
  return out;
}

bool traversalAudioVoiceActive(const TraversalAudioVoice& v) {
  return (v.mode & 0xe) != 0;
}

int traversalAudioScaledVol(int vol, int pct) {
  return vol * pct / 100;   // IMUL + IDIV — C truncating division
}

int traversalAudioMilliBel(int scaledVol) {
  // FILD vol; FMUL (2500/32767); FISTP; SUB 0x9c4 — trunc + bias.
  return static_cast<int>(static_cast<double>(scaledVol) * kMbScale) -
         kMbBias;
}

float traversalAudioVolDb(int scaledVol) {
  return static_cast<float>(traversalAudioMilliBel(scaledVol)) / 100.f;
}

int traversalAudioDsPan(int pan32767) {
  return (pan32767 * 10000) >> 15;   // IMUL + SAR — arithmetic shift
}

float traversalAudioPanUnit(int pan32767) {
  return static_cast<float>(traversalAudioDsPan(pan32767)) / 10000.0f;
}

} // namespace mdk
