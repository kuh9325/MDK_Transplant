#include "core/object_animation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mdk {
namespace {

constexpr float kFrameDt = 1.0f / 30.0f;   // DAT_0049b6f4 (OBSERVED .rdata)

std::uint16_t rd16(const std::uint8_t* p) {
  std::uint16_t v; std::memcpy(&v, p, 2); return v;
}
std::uint32_t rd32(const std::uint8_t* p) {
  std::uint32_t v; std::memcpy(&v, p, 4); return v;
}
float rdf32(const std::uint8_t* p) {
  float f; std::memcpy(&f, p, 4); return f;
}
std::int16_t rdi16(const std::uint8_t* p) {
  return static_cast<std::int16_t>(rd16(p));
}

// FUN_0046b048 — out = M . v (3x3 rotation only, no translation; the
// original matrix is a 3x4 row-major record whose per-row translation
// entries are skipped — equivalent to our xform[9]+origin[3] split).
void rotVec(const float m[9], const float v[3], float out[3]) {
  out[0] = v[0] * m[0] + v[1] * m[1] + v[2] * m[2];
  out[1] = v[0] * m[3] + v[1] * m[4] + v[2] * m[5];
  out[2] = v[0] * m[6] + v[1] * m[7] + v[2] * m[8];
}

// FUN_00459d54 — min/max over `count` f32 triples -> {min,max}. Same
// helper as dynamic_objects.cpp's parser path (kept local to avoid
// exporting a second copy through the header).
void boundsFromPoints(const float* pts, std::size_t count,
                      float out[6]) {
  out[0] = out[1] = out[2] = 1e30f;
  out[3] = out[4] = out[5] = -1e30f;
  for (std::size_t i = 0; i < count; ++i)
    for (int k = 0; k < 3; ++k) {
      const float v = pts[i * 3 + k];
      if (v < out[k]) out[k] = v;
      if (v > out[3 + k]) out[3 + k] = v;
    }
}

// FUN_0042fa50 — C-string equality (the 12-byte name fields are
// NUL-terminated within the field for all observed records).
bool nameEq(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

} // namespace

// ---------------------------------------------------------------------------
// ObjectAnimView
// ---------------------------------------------------------------------------

const std::uint8_t* ObjectAnimView::bytes(std::size_t off,
                                          std::size_t n) const {
  if (!rec) { ok = false; return nullptr; }
  if (limit) {
    const std::size_t avail =
        static_cast<std::size_t>(limit - rec);
    if (off > avail || n > avail - off) { ok = false; return nullptr; }
  }
  return rec + off;
}

float ObjectAnimView::rate() const {
  const std::uint8_t* p = bytes(0, 4);
  return p ? rdf32(p) : 0.0f;
}
std::uint32_t ObjectAnimView::channelCount() const {
  const std::uint8_t* p = bytes(4, 4);
  return p ? rd32(p) : 0;
}
std::uint32_t ObjectAnimView::frameCount() const {
  const std::uint8_t* p = bytes(8, 4);
  return p ? rd32(p) : 0;
}
const std::uint8_t* ObjectAnimView::channel(std::uint32_t i) const {
  const std::uint8_t* t = bytes(0x0c + i * 4, 4);
  if (!t) return nullptr;
  const std::uint32_t off = rd32(t);
  // Channel records are addressed as rec + 4 + chanOff[i] (OBSERVED).
  return bytes(4 + off, 4) ? rec + 4 + off : nullptr;
}
const float* ObjectAnimView::rootKey(std::uint32_t frame) const {
  const std::uint32_t n = channelCount();
  const std::size_t off = 0x0c + 4ull * n + 12ull * frame;
  const std::uint8_t* p = bytes(off, 12);
  return p ? reinterpret_cast<const float*>(p) : nullptr;
}
std::uint32_t ObjectAnimView::refCount() const {
  const std::uint32_t n = channelCount();
  const std::uint32_t f = frameCount();
  const std::uint8_t* p = bytes(0x0c + 4ull * n + 12ull * f, 4);
  return p ? rd32(p) : 0;
}
const float* ObjectAnimView::refKey(std::uint32_t slot,
                                    std::uint32_t frame) const {
  const std::uint32_t n = channelCount();
  const std::uint32_t f = frameCount();
  const std::size_t off = 0x10 + 4ull * n + 12ull * f +
                          12ull * (static_cast<std::uint64_t>(slot) * f +
                                   frame);
  const std::uint8_t* p = bytes(off, 12);
  return p ? reinterpret_cast<const float*>(p) : nullptr;
}

const char* animChannelName(const std::uint8_t* ch) {
  return reinterpret_cast<const char*>(ch);
}
std::uint32_t animChannelVertCount(const std::uint8_t* ch) {
  return rd32(ch + 0x0c);
}
float animChannelScale(const std::uint8_t* ch) {
  return rdf32(ch + 0x10);
}
bool animChannelIsRigid(const std::uint8_t* ch) {
  return (rd32(ch + 0x10) & 0x7fffffffu) == 0;   // |scale| == 0
}

// ---------------------------------------------------------------------------
// FUN_00455c48 — rigid channel: decode the frame's 12xi16 fixed-point
// 3x4 transform and apply it to the channel base pose.
//   i16 m[12] = {r00,r01,r02,tx, r10,r11,r12,ty, r20,r21,r22,tz}
//   rotation entries scaled by 1/(0x8000>>rotShift) (i16 / divisor);
//   translation entries by 1/(0x8000>>transShift).
// ---------------------------------------------------------------------------
namespace {

void applyRigidChannel(const std::uint8_t* ch, std::size_t vc,
                       int frame, const ObjectAnimView& anim,
                       float* verts) {
  const std::uint8_t* meta = anim.bytes(
      (ch - anim.rec) + 0x14, 2);
  if (!meta) return;
  const float rotDiv =
      static_cast<float>(0x8000 >> meta[0]);      // +0x14 shift
  const float trnDiv =
      static_cast<float>(0x8000 >> meta[1]);      // +0x15 shift
  const std::size_t baseOff = (ch - anim.rec) + 0x16;
  const float* base = reinterpret_cast<const float*>(
      anim.bytes(baseOff, vc * 12));
  const std::uint8_t* kp = anim.bytes(
      baseOff + vc * 12 + static_cast<std::size_t>(frame) * 24, 24);
  if (!base || !kp) return;
  float m[12];
  for (int i = 0; i < 12; ++i) {
    const float div = (i % 4 == 3) ? trnDiv : rotDiv;
    m[i] = static_cast<float>(rdi16(kp + i * 2)) / div;
  }
  for (std::size_t v = 0; v < vc; ++v) {
    const float x = base[v * 3 + 0], y = base[v * 3 + 1],
                z = base[v * 3 + 2];
    // Row-major 3x4: out_i = sum_j m[i*4+j]*in_j + m[i*4+3].
    verts[v * 3 + 0] =
        x * m[0] + y * m[1] + z * m[2] + m[3];
    verts[v * 3 + 1] =
        x * m[4] + y * m[5] + z * m[6] + m[7];
    verts[v * 3 + 2] =
        x * m[8] + y * m[9] + z * m[10] + m[11];
  }
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_00455890 — apply `steps` single-frame advances.
// ---------------------------------------------------------------------------
void objectAnimApply(DynamicObject& o, const ObjectAnimView& anim,
                     int steps) {
  if (!anim.rec || steps <= 0) return;
  const int fc = static_cast<int>(anim.frameCount());
  if (fc <= 0) return;
  for (int s = 0; s < steps; ++s) {
    // Idle gate — evaluated per step (0x455904 and again at 0x455a86):
    // +0x118 >= 0 && +0xe4 == +0x118 resyncs the accumulator.
    if (o.animLatch >= 0 && o.animFrame == o.animLatch) {
      o.animAcc = static_cast<float>(o.animFrame);
      return;
    }
    ++o.animFrame;
    const bool wrapped = o.animFrame >= fc;
    if (wrapped) o.animFrame = 0;
    const int f = o.animFrame;
    // Root-motion impulse (skipped at frame 0 unless the step wrapped;
    // suppressed entirely while +0x14b bit7 is set).
    if ((f > 0 || wrapped) && (o.col.flags14b & 0x80u) == 0) {
      if (const float* k = anim.rootKey(static_cast<std::uint32_t>(f))) {
        float w[3];
        rotVec(o.col.xform, k, w);
        for (int i = 0; i < 3; ++i)
          o.animImpulse[i] += w[i] / kFrameDt;
      }
    }
    // Reference points — refKey[slot][frame] -> model +0x24 slots,
    // capped at 8.
    const std::uint32_t rc = std::min(anim.refCount(), 8u);
    for (std::uint32_t i = 0; i < rc; ++i) {
      if (const float* rk =
              anim.refKey(i, static_cast<std::uint32_t>(f)))
        std::memcpy(o.model.refPoints[i], rk, 12);
    }
    // Per-element channel apply.
    for (std::size_t e = 0; e < o.model.elems.size(); ++e) {
      const std::string ename = o.model.elemName(e);
      const std::uint8_t* ch = nullptr;
      for (std::uint32_t c = 0; c < anim.channelCount(); ++c) {
        const std::uint8_t* cand = anim.channel(c);
        if (cand && nameEq(animChannelName(cand), ename.c_str())) {
          ch = cand;
          break;
        }
      }
      if (!ch) continue;
      std::vector<float>& ev = o.model.elemVerts[e];
      const std::size_t vc = ev.size() / 3;
      if (vc == 0) continue;
      float* verts = ev.data();
      if (animChannelIsRigid(ch)) {
        applyRigidChannel(ch, vc, f, anim, verts);
      } else if (f == 0) {
        // Frame 0 = absolute base pose copy.
        const std::uint8_t* base =
            anim.bytes((ch - anim.rec) + 0x14, vc * 12);
        if (!base) continue;
        std::memcpy(verts, base, vc * 12);
      } else {
        // Scan tagged delta keys: {i16 tag; i8 delta[3*vc]} — apply
        // only the key whose tag == frame.
        const float scale = animChannelScale(ch);
        std::size_t koff = (ch - anim.rec) + 0x14 + vc * 12;
        std::int16_t tag = -1;
        while (true) {
          const std::uint8_t* tp = anim.bytes(koff, 2);
          if (!tp) { tag = -1; break; }
          tag = rdi16(tp);
          koff += 2;
          if (tag < 0 || f <= tag) break;
          koff += vc * 3;
        }
        if (tag == f) {
          const std::uint8_t* d = anim.bytes(koff, vc * 3);
          if (!d) continue;
          for (std::size_t i = 0; i < vc * 3; ++i)
            verts[i] += static_cast<float>(
                static_cast<std::int8_t>(d[i])) * scale;
        }
      }
      boundsFromPoints(verts, vc, o.model.elems[e].localAabb);
    }
  }
}

// ---------------------------------------------------------------------------
// FUN_004555bc — the per-tick driver.
// ---------------------------------------------------------------------------
void objectAnimTick(DynamicObject& o, const std::uint8_t* recLimit) {
  // +0x04 == -1 -> FUN_00455500 (classless timing path). The original
  // resolves a timing record through FUN_0041a5ec (source UNKNOWN —
  // bounded seam): it advances +0xdc by +0xe0 * DT (no rate field),
  // sets +0xe4 = FRNDINT(+0xdc), and wraps/clamps by the record's
  // frame bound. We substitute the bound anim record's frameCount
  // when present (HYPOTHESIS for the wrap bound) and hold at 0
  // otherwise; the vertex applier is never invoked — OBSERVED, the
  // +0x04==-1 path does not run FUN_00455890.
  if (o.enemyIndex == 0xffff) {
    o.animAcc += o.animRate * kFrameDt;
    o.animFrame = static_cast<std::int16_t>(lroundf(o.animAcc));
    ObjectAnimView av{reinterpret_cast<const std::uint8_t*>(o.animRec),
                      recLimit};
    const int fc =
        o.animRec ? static_cast<int>(av.frameCount()) : 0;
    if (fc <= 0) {                       // unresolved record -> hold 0
      o.animFrame = 0;
      o.animAcc = 0.0f;
      return;
    }
    while (o.animFrame >= fc) {          // wrap by the record bound
      o.animFrame = static_cast<std::int16_t>(o.animFrame - fc);
      o.animAcc -= static_cast<float>(fc);
    }
    if (o.animFrame < 0) { o.animFrame = 0; o.animAcc = 0.0f; }
    return;
  }

  // Idle gate: +0x118 >= 0 && +0xe4 == +0x118, or the 0xff00 done
  // latch — resync the accumulator to the applied frame and hold.
  if ((o.animLatch >= 0 && o.animFrame == o.animLatch) ||
      static_cast<std::uint16_t>(o.animLatch) == 0xff00u) {
    o.animAcc = static_cast<float>(o.animFrame);
    return;
  }
  if (!o.animRec) return;
  ObjectAnimView anim{
      reinterpret_cast<const std::uint8_t*>(o.animRec), recLimit};
  const float rate = anim.rate();
  const int fc = static_cast<int>(anim.frameCount());
  if (!anim.ok || fc <= 0) return;

  // Sound marker consume (OBSERVED): when the accumulator crosses the
  // +0x144 mark the original emits the +0x140 name once and clears it.
  // Audio is a documented seam — the name text and the consume-on-
  // cross state transition are preserved.
  const float step = rate * o.animRate * kFrameDt;
  if (!o.animSoundName.empty() &&
      o.animAcc < static_cast<float>(o.animSoundMark) &&
      static_cast<float>(o.animSoundMark) <= o.animAcc + step) {
    o.animSoundName.clear();             // would emit here (seam)
  }

  o.animAcc += step;

  // Target clamp: +0x118 >= 0 && +0xe4 < +0x118 && FRNDINT(+0xdc) >=
  // +0x118  ->  +0xdc = +0x118 (the accumulator caps AT the target;
  // OBSERVED >= at 0x4556e9).
  if (o.animLatch >= 0 && o.animFrame < o.animLatch &&
      lroundf(o.animAcc) >= o.animLatch)
    o.animAcc = static_cast<float>(o.animLatch);

  const bool loop = (o.col.flags148 & 0x8u) != 0;   // +0x148 bit3
  int steps;
  if (o.animAcc >= static_cast<float>(fc - 1)) {
    if (loop) {
      // OBSERVED order: steps from the PRE-wrap accumulator.
      steps = static_cast<int>(lroundf(o.animAcc)) - o.animFrame;
      if (o.animAcc >= static_cast<float>(fc))
        o.animAcc -= static_cast<float>(fc);
    } else {
      o.animAcc = static_cast<float>(fc - 1);
      steps = static_cast<int>(lroundf(o.animAcc)) - o.animFrame;
    }
  } else {
    steps = static_cast<int>(lroundf(o.animAcc)) - o.animFrame;
  }

  objectAnimApply(o, anim, steps);

  // Done latch: reached the last frame of a non-looping anim that was
  // not aimed at a +0x118 target.
  if (o.animFrame == fc - 1 && !loop && o.animFrame != o.animLatch)
    o.animLatch = static_cast<std::int16_t>(0xff00);
}

} // namespace mdk
