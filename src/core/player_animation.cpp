// Phase 16A — Kurt/player sprite animation. See player_animation.h for
// the evidence block. Implemented as a direct transcription of
// FUN_00461954's dispatch (analysis-private disassembly) — every state
// handler writes the same globals in the same order.

#include "core/player_animation.h"

#include <cmath>
#include <cstring>
#include <span>

#include "core/binary_reader.h"
#include "core/bni_directory.h"
#include "core/enemy_runtime.h" // enemyRandBelow = FUN_00401ed4
#include "core/sni_directory.h"
#include "core/traversal_runtime.h"

namespace mdk {

namespace {

// ---------------------------------------------------------------------------
// Sprite-table accessors — {u32 count, u32 ofs[N], frames}; ofs[i] is
// relative to the count field (payload+4 image).
// ---------------------------------------------------------------------------

std::uint32_t le32(const std::byte* p) {
  return static_cast<std::uint32_t>(p[0]) |
         static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 |
         static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint32_t recCount(const std::byte* rec) {
  return rec ? le32(rec) : 0;
}

const std::byte* recFrame(const std::byte* rec, int index) {
  if (!rec) return nullptr;
  return rec + le32(rec + 4 + index * 4);
}

// FUN_0047d59a — FRNDINT under round-to-nearest, then FISTP.
inline int roundNearest(double v) {
  return static_cast<int>(std::lrint(v));
}

// ---------------------------------------------------------------------------
// FUN_00464278 — integer frame advance with wrap. First frame
// (animPrev != locoState) resets cb4 + the shadow to 0.
// ---------------------------------------------------------------------------

const std::byte* wrapAdvance(const std::byte* rec, int dir,
                             TraversalRuntime& rt, int& shadow,
                             int frameStep) {
  if (rt.animPrev != rt.locoState) {
    rt.animFrame = 0;
    shadow = 0;
    return recFrame(rec, rt.animFrame);
  }
  if (dir > 0) {
    rt.animFrame += frameStep;
    if (rt.animFrame >= static_cast<int>(recCount(rec)))
      rt.animFrame = 0;
  } else {
    rt.animFrame -= frameStep;
    if (rt.animFrame < 0)
      rt.animFrame = static_cast<int>(recCount(rec)) - 1;
  }
  return recFrame(rec, rt.animFrame);
}

// ---------------------------------------------------------------------------
// FUN_00464308 — movement-speed driven float phase. rate(x = 1.5*v):
//   v > 0:   v <= 2/3 ? 0.75x + 0.25 : 0.25x + 0.75
//   v <= 0:  v >= -2/3 ? 0.75x - 0.25 : 0.25x - 0.75
// cb8 += smoothed * rate; single-step wraps at [0, count);
// cb4 = rint(cb8). First frame resets cb8/shadow (not cb4 — the
// rint(cb8)=0 write at the tail lands it anyway).
// ---------------------------------------------------------------------------

const std::byte* speedAdvance(const std::byte* rec, TraversalRuntime& rt,
                              int& shadow, float smoothed) {
  if (rt.animPrev != rt.locoState) {
    shadow = 0;
    rt.animPhase = 0.0f;
  } else {
    const double v = rt.motion.moveVel; // 0x540d48
    const double x = 1.5 * v;
    const double rate =
        (v > 0.0) ? ((v <= 2.0 / 3.0) ? 0.75 * x + 0.25 : 0.25 * x + 0.75)
                  : ((v >= -2.0 / 3.0) ? 0.75 * x - 0.25
                                       : 0.25 * x - 0.75);
    rt.animPhase += smoothed * static_cast<float>(rate);
    const float count = static_cast<float>(recCount(rec));
    if (rt.animPhase >= count) rt.animPhase -= count;
    if (rt.animPhase < 0.0f) rt.animPhase += count;
  }
  rt.animFrame = roundNearest(rt.animPhase);
  return recFrame(rec, rt.animFrame);
}

// ---------------------------------------------------------------------------
// Constant tables (OBSERVED .rdata):
//   0x49b928/0x49b968 — mantle root-motion profiles (z lift / xy push),
//   consumed as successive deltas tab[j+1]-tab[j] for j=(cb4+1)/2.
//   0x49ba24/0x49ba68 — jump-frame vertVel thresholds.
// ---------------------------------------------------------------------------

constexpr float kMantleZ[16] = {
    0.374f, 0.326f, 0.292f, 0.311f, 0.677f, 1.263f, 2.754f, 4.172f,
    4.903f, 5.343f, 5.711f, 6.001f, 6.212f, 6.345f, 6.406f, 6.417f};
constexpr float kMantleXy[16] = {
    0.685f, 0.383f, 0.167f, 0.254f, 0.635f, 1.053f, 0.847f, 0.514f,
    0.17f, -0.125f, -0.501f, -0.825f, -1.066f, -1.221f, -1.293f, -1.305f};
constexpr float kJumpThrStand[16] = {
    25.0f, 26.0f, 27.0f, 20.0f, 15.0f, 13.0f, 9.0f, 6.6f,
    4.0f, 0.2f, -3.0f, -6.3f, -10.0f, -14.0f, -17.0f, -20.0f};
constexpr float kJumpThrMove[12] = {
    25.0f, 26.0f, 27.0f, 20.0f, 15.0f, 13.0f, 9.0f, 6.6f, 4.0f, 0.2f,
    -3.0f, -6.3f};

} // namespace

std::uint32_t playerAnimRecCount(const std::byte* rec) {
  return recCount(rec);
}
const std::byte* playerAnimRecFrame(const std::byte* rec, int index) {
  return recFrame(rec, index);
}

bool playerAnimFrameCrossed(int trig, int dir, int prev, int curr) {
  if (dir > 0) {
    if (prev < curr) return trig > prev && trig <= curr;
    if (prev == curr) return false;
    return trig > prev || trig <= curr;   // wrap
  }
  if (prev > curr) return trig >= curr && trig < prev;
  if (prev == curr) return false;
  return trig < prev || trig >= curr;     // wrap
}

// ---------------------------------------------------------------------------
// FUN_0046445c — name binding.
// ---------------------------------------------------------------------------

namespace {

const char* const kBniNames[] = {
    "K_BANG", "K_BFLIP", "K_CHUTE", "K_CHUTEC", "K_CRASHL", "K_FALL",
    "K_FLOATC", "K_HANG", "K_IDLE", "K_JUMP", "K_LAND", "K_LOOKD",
    "K_LOOKU", "K_MUZZF", "K_RJMP", "K_RUN", "K_RUNFIR", "K_SHOT",
    "K_SIDE", "K_SPWEP", "K_STILL", "K_TAKEOF", "K_TRN45"};
const char* const kSniNames[] = {"K_BSLIDE", "K_FSLIDE", "K_SLIDE",
                                 "K_SLIP",   "K_SURF",   "K_SURFJ"};

const std::byte* sniLookup(const std::vector<std::byte>& file,
                           const char* name) {
  if (file.empty()) return nullptr;
  const SniDirectory dir =
      inspectSniDirectory(std::span<const std::byte>(file));
  if (dir.status != SniDirectoryStatus::kOk) return nullptr;
  const std::size_t nlen = std::strlen(name);
  for (const SniEntry& e : dir.entries) {
    // FUN_0042891c's bank filter — record byte +0x0D bit 0x80 (the
    // K_ records all carry it via the 0xffffffff sentinel flag).
    if ((e.fieldAt0x0C & 0x8000u) == 0) continue;
    // 12-byte bounded field compare against a C-string name.
    std::size_t i = 0;
    for (; i < kSniNameFieldSize && i <= nlen; ++i) {
      const std::byte b = e.nameField[i];
      const char c = (i < nlen) ? name[i] : '\0';
      if (b != static_cast<std::byte>(c)) break;
      if (c == '\0') {
        // FUN_004289a0 -> imageBase + storedOffset + 4.
        const std::uint64_t off = e.payloadFileOffset() + 4;
        if (off > file.size()) return nullptr;
        return file.data() + off;
      }
    }
  }
  return nullptr;
}

} // namespace

void playerAnimBindTables(TraversalRuntime& rt) {
  PlayerAnimTables& t = rt.animTables;
  t = PlayerAnimTables{};
  if (!rt.level.travsprtBytes.empty()) {
    const BniDirectory bd = inspectBniDirectory(
        std::span<const std::byte>(rt.level.travsprtBytes));
    if (bd.status == BniDirectoryStatus::kOk) {
      const std::byte** dst[] = {
          &t.bang,  &t.bFlip,  &t.chute, &t.chuteC, &t.crashL,
          &t.fall,  &t.floatC, &t.hang,  &t.idle,   &t.jump,
          &t.land,  &t.lookD,  &t.lookU, &t.muzzF,  &t.rjmp,
          &t.run,   &t.runFir, &t.shot,  &t.side,   &t.spewP,
          &t.still, &t.takeOf, &t.trn45};
      for (std::size_t i = 0; i < 23; ++i) {
        const BniRecord* r = findBniRecord(bd, kBniNames[i]);
        if (!r) continue;
        // FUN_004039ec — record payload + 4.
        *dst[i] =
            rt.level.travsprtBytes.data() + r->payloadFileOffset + 4;
      }
    }
  }
  const std::byte** dst[] = {&t.bSlide, &t.fSlide, &t.slide,
                             &t.slip,   &t.surf,   &t.surfJ};
  for (std::size_t i = 0; i < 6; ++i) {
    *dst[i] = sniLookup(rt.level.sniBytes, kSniNames[i]);
  }
}

// ---------------------------------------------------------------------------
// Trace identity + table names.
// ---------------------------------------------------------------------------

const char* playerAnimTableName(int tableIndex) {
  static const char* const names[] = {
      "K_BANG",  "K_BFLIP", "K_BSLIDE", "K_CHUTE", "K_CHUTEC",
      "K_CRASHL","K_FALL",  "K_FLOATC", "K_FSLIDE","K_HANG",
      "K_IDLE",  "K_JUMP",  "K_LAND",   "K_LOOKD", "K_LOOKU",
      "K_MUZZF", "K_RJMP",  "K_RUN",    "K_RUNFIR","K_SHOT",
      "K_SIDE",  "K_SLIDE", "K_SLIP",   "K_SPWEP", "K_STILL",
      "K_SURF",  "K_SURFJ", "K_TAKEOF", "K_TRN45"};
  if (tableIndex < 0 || tableIndex >= 29) return "?";
  return names[tableIndex];
}

bool playerAnimFrameIdentity(const TraversalRuntime& rt,
                             const std::byte* frame,
                             int* tableIndex, int* frameIndex) {
  if (!frame) return false;
  const std::byte* const* tabs[] = {
      &rt.animTables.bang,   &rt.animTables.bFlip,
      &rt.animTables.bSlide, &rt.animTables.chute,
      &rt.animTables.chuteC, &rt.animTables.crashL,
      &rt.animTables.fall,   &rt.animTables.floatC,
      &rt.animTables.fSlide, &rt.animTables.hang,
      &rt.animTables.idle,   &rt.animTables.jump,
      &rt.animTables.land,   &rt.animTables.lookD,
      &rt.animTables.lookU,  &rt.animTables.muzzF,
      &rt.animTables.rjmp,   &rt.animTables.run,
      &rt.animTables.runFir, &rt.animTables.shot,
      &rt.animTables.side,   &rt.animTables.slide,
      &rt.animTables.slip,   &rt.animTables.spewP,
      &rt.animTables.still,  &rt.animTables.surf,
      &rt.animTables.surfJ,  &rt.animTables.takeOf,
      &rt.animTables.trn45};
  for (int ti = 0; ti < 29; ++ti) {
    const std::byte* rec = *tabs[ti];
    if (!rec) continue;
    const int n = static_cast<int>(recCount(rec));
    for (int i = 0; i < n; ++i) {
      if (recFrame(rec, i) == frame) {
        if (tableIndex) *tableIndex = ti;
        if (frameIndex) *frameIndex = i;
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// FUN_00461954 — the machine.
// ---------------------------------------------------------------------------

void playerAnimTick(TraversalRuntime& rt,
                    const PlayerAnimEnvironment& env) {
  const PlayerAnimTables& t = rt.animTables;
  const int step = env.frameStep;

  // FUN_00431300's job-registration gate + FUN_00461954's head gate.
  const bool callGate =
      (rt.flagC9c == 0 || rt.transitionPhase == 0) &&
      (rt.cs.excludeObj == nullptr || (rt.mountClass & 0x20) == 0);
  if (!callGate) {
    rt.animDrawn = false;
    return;
  }
  const bool headGate = rt.flag4999d0 && rt.flag541548;

  int shadow = rt.animFrame;             // [EBP-0x2c]
  const int nextFrame = shadow + step;   // 0x461b46 (0x385/0x3ea path)

  // The muzzle overlay pattern: gated by fieldC74 + the frame-counter
  // parity bit; ba1c advances by rand(3)+1 mod 4; offsets jitter by
  // rand(5) plus a per-state bias.
  const auto muzzle = [&](int biasX, int biasY) {
    if (rt.fieldC74 == 0 || (rt.frameCounter & 1) == 0) return;
    rt.animMuzzIdx =
        (enemyRandBelow(rt.rngState, 3) + rt.animMuzzIdx + 1) & 3;
    rt.animOverlayFrame = recFrame(t.muzzF, rt.animMuzzIdx);
    rt.animOfsX = enemyRandBelow(rt.rngState, 5) + biasX;
    rt.animOfsY = enemyRandBelow(rt.rngState, 5) + biasY;
  };
  const auto setMain = [&](const std::byte* rec, int idx) {
    rt.animMainFrame = recFrame(rec, idx);
  };
  const bool grounded = (rt.vert.contactFlags & 1) != 0;
  const bool vertZero = rt.vert.vertVel == 0.0f;
  const float arenaScalar = env.arenaScalar;

  if (!headGate) {
    rt.animOverlayFrame = nullptr;       // cb18 cleared every frame
    const int cac = rt.locoState;
    const bool enter = (rt.animPrev != cac);

    if (cac < 0x64 || cac == 0x322 ||
        (cac >= 0x66 && cac < 0xc8 && cac != 0x65) ||
        (cac > 0xc9 && cac < 0x12c) ||
        (cac > 0x12c && cac < 0x190) ||
        (cac > 0x190 && cac < 0x1f4) ||
        (cac > 0x1f4 && cac < 0x258) ||
        (cac > 0x259 && cac < 0x2bc) ||
        (cac > 0x2bf && cac < 0x320) ||
        (cac > 0x32a && cac < 0x384) ||
        (cac > 0x385 && cac < 0x3e8) ||
        cac > 0x3ea) {
      // FUN_00408eb0("Unknown Damp Animation", 0); cbc = callee EDX
      // — 0 when the diag log is closed (the common path).
      ++rt.seams.animDiagCalls;
      rt.eventPriority = 0;
    } else if (cac == 0x64 || cac == 0x65) {
      // still / idle — cb4 += step, clamp count-1, release.
      const std::byte* rec = (cac == 0x64) ? t.still : t.idle;
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        const int last = static_cast<int>(recCount(rec)) - 1;
        if (last <= rt.animFrame) {
          rt.animFrame = last;
          rt.eventPriority = 0;
        }
      }
      setMain(rec, rt.animFrame);
    } else if (cac == 0xc8) {
      // land — cb4 += step, clamp count-1 on overshoot, release.
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        const int count = static_cast<int>(recCount(t.land));
        if (rt.animFrame >= count) {
          rt.animFrame = count - 1;
          rt.eventPriority = 0;
        }
      }
      setMain(t.land, rt.animFrame);
    } else if (cac == 0xc9) {
      // surf sustain — eyeHeight pinned 4.5, loops [0, count-2].
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        if (static_cast<int>(recCount(t.surf)) - 1 <= rt.animFrame)
          rt.animFrame = 0;
      }
      rt.camera.eyeHeight = 4.5f;
      setMain(t.surf, rt.animFrame);
      muzzle(-42, 12);
    } else if (cac == 0x12c) {
      // shot — loops [0, count-2].
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        if (static_cast<int>(recCount(t.shot)) - 1 <= rt.animFrame)
          rt.animFrame = 0;
      }
      setMain(t.shot, rt.animFrame);
    } else if (cac == 0x190) {
      // turn — proportional yaw index every frame (no enter check).
      const int count = static_cast<int>(recCount(t.trn45));
      int idx = roundNearest(
          static_cast<double>(rt.motion.yawDeg) * (1.0 / 180.0) *
          static_cast<double>(count));
      idx %= count;
      if (idx < 0) idx += count;
      rt.animFrame = idx;
      shadow = idx;
      muzzle(0, 0);
      setMain(t.trn45, rt.animFrame);
    } else if (cac == 0x1f4) {
      // strafe — cb4 += step, wrap to 0 at count.
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        if (rt.animFrame >= static_cast<int>(recCount(t.side)))
          rt.animFrame = 0;
      }
      setMain(t.side, rt.animFrame);
      muzzle(0, 0);
    } else if (cac == 0x258) {
      // run — speed-driven phase + footstep crossings 0 / 0xd.
      rt.animMainFrame =
          speedAdvance(t.run, rt, shadow, env.smoothed);
      const int dir = rt.motion.moveDirLatch;
      if (playerAnimFrameCrossed(0, dir, shadow, rt.animFrame)) {
        ++rt.seams.animSoundCalls;       // FUN_004022b8(0x54c60c/14)
      }
      if (playerAnimFrameCrossed(0xd, dir, shadow, rt.animFrame)) {
        ++rt.seams.animSoundCalls;       // FUN_004022b8(0x54c610/18)
        rt.animFootAlt = (rt.animFootAlt == 0) ? 1 : 0;
      }
    } else if (cac == 0x259) {
      // run-fire — wrap advance (dir = moveDirLatch) + footsteps
      // at frames 4 / 0x11.
      rt.animMainFrame =
          wrapAdvance(t.runFir, rt.motion.moveDirLatch, rt, shadow,
                      step);
      const int dir = rt.motion.moveDirLatch;
      if (playerAnimFrameCrossed(4, dir, shadow, rt.animFrame)) {
        ++rt.seams.animSoundCalls;
      }
      if (playerAnimFrameCrossed(0x11, dir, shadow, rt.animFrame)) {
        ++rt.seams.animSoundCalls;
        rt.animFootAlt = (rt.animFootAlt == 0) ? 1 : 0;
      }
    } else if (cac == 0x2bc) {
      // fall — vertVel==0 && grounded -> land; else wrap loop.
      bool landed = false;
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else if (vertZero && grounded) {
        ++rt.seams.animSoundCalls;       // FUN_004022b8(0x54c608)
        rt.animFrame = 0;
        rt.locoState = 0xc8;
        rt.eventPriority = 2;
        setMain(t.land, 0);
        if (env.jumpHeld == 0) rt.vert.jumpLatch = 0; // c90 clear
        landed = true;
      } else {
        rt.animFrame += step;
        if (rt.animFrame >= static_cast<int>(recCount(t.fall)))
          rt.animFrame -= static_cast<int>(recCount(t.fall));
      }
      if (!landed) setMain(t.fall, rt.animFrame);
      muzzle(40, 6);
    } else if (cac == 0x2bd) {
      // chute — deploy frames 0..4, then sustain (ping-pong on
      // K_CHUTEC while c80) or release-clamp on K_CHUTE.
      if (enter) {
        ++rt.seams.animSoundCalls;       // FUN_004022b8(0x54c5fc)
        rt.animFrame = 0; shadow = 0;
        setMain(t.chute, rt.animFrame);
      } else if (rt.animFrame < 4) {
        rt.animFrame += step;
        if (rt.animFrame > 4) rt.animFrame = 4;
        setMain(t.chute, rt.animFrame);
      } else if (rt.vert.jumpSustain != 0) {
        rt.animFrame += step;
        const int cc = static_cast<int>(recCount(t.chuteC));
        if (rt.animFrame >= 2 * cc + 2) rt.animFrame = 4;
        const int idx =
            (rt.animFrame < cc + 4) ? rt.animFrame - 4
                                    : 2 * cc + 2 - rt.animFrame;
        setMain(t.chuteC, idx);
        ++rt.seams.animSoundCalls;       // FUN_00402388(0x54c604)
        muzzle(20, 0);
      } else {
        // FUN_00402658(0x54c604) "still playing" query — the audio
        // seam models the sustained loop (the refire pair has no
        // gameplay feedback). UNKNOWN: exact query polarity.
        ++rt.seams.animSoundCalls;       // the query itself
        ++rt.seams.animSoundCalls;       // FUN_0040210c(0x54c604)
        ++rt.seams.animSoundCalls;       // FUN_004022b8(0x54c600)
        rt.animFrame += step;
        const int last = static_cast<int>(recCount(t.chute)) - 1;
        if (rt.animFrame >= last) {
          rt.animFrame = last;
          rt.eventPriority = 0;
        }
        setMain(t.chute, rt.animFrame);
      }
    } else if (cac == 0x2be || cac == 0x2bf) {
      // standing / moving jump — vertVel-keyed frame advance, then
      // the shared endpoint: grounded-catch -> land; state changed ->
      // tail; else muzzle + frame.
      const bool standing = (cac == 0x2be);
      const std::byte* rec = standing ? t.jump : t.rjmp;
      const float* thr = standing ? kJumpThrStand : kJumpThrMove;
      const int lo = standing ? 0xa : 7, hi = standing ? 0x10 : 0xc;
      const int ofsSkip = standing ? 0 : 4;  // K_RJMP's +0x14 ofs slot
      if (!enter) {
        if (rt.animFrame < lo) {
          ++rt.animFrame;
        } else {
          while (rt.animFrame < hi &&
                 rt.vert.vertVel <= thr[rt.animFrame])
            ++rt.animFrame;
        }
        if (rt.animFrame == hi) {
          // threshold table exhausted -> fall release (cbc=7).
          rt.animFrame = 0;
          rt.locoState = 0x2bc;
          rt.eventPriority = 7;
          setMain(t.fall, 0);
        }
        // 0x462aaf — c84 += 1 while vertVel <= 0 (the descent
        // charge feeds the movement channel on landing).
        if (rt.vert.vertVel <= 0.0f) rt.motion.airCharge += 1.0f;
      } else {
        rt.animFrame = 0; shadow = 0;
      }
      // 0x462102 / 0x4621ca — the shared endpoint (runs on enter
      // and after the transition too — a same-frame land is real).
      if (vertZero && grounded) {
        ++rt.seams.animSoundCalls;       // FUN_004022b8(0x54c608)
        rt.locoState = 0xc8;
        rt.eventPriority = 2;
        rt.animFrame = 0;
        setMain(t.land, 0);
      } else if (rt.locoState == cac) {
        muzzle(0, standing ? 0 : -10);
        setMain(rec, rt.animFrame + ofsSkip);
      }
    } else if (cac == 0x320) {
      // mantle — half-rate hang frames + root-motion nudge while
      // c7c != 1; end: cb4 = 2*count-1, release, c7c = 0.
      if (enter) {
        ++rt.seams.animSoundCalls;       // FUN_0040210c(0x54c604)
        rt.animFrame = 0; shadow = 0;
      } else if (rt.vert.vertSkip != 1) {
        for (int i = 0; i < step; ++i) {
          ++rt.animFrame;
          const int j = (rt.animFrame + 1) >> 1;
          if (j < 0xf) {
            const double k = 0.7083333333333334 * 0.5;
            const double dz = kMantleZ[j + 1] - kMantleZ[j];
            const double dxy = kMantleXy[j + 1] - kMantleXy[j];
            const double rad =
                static_cast<double>(rt.motion.yawDeg) *
                (3.141592653589793 / 180.0);
            rt.cs.pos[2] += static_cast<float>(dz * k);
            rt.cs.pos[0] -=
                static_cast<float>(dxy * k * std::cos(rad));
            rt.cs.pos[1] -=
                static_cast<float>(dxy * k * std::sin(rad));
            rt.vert.posX = rt.cs.pos[0];
            rt.vert.posY = rt.cs.pos[1];
            rt.vert.posZ = rt.cs.pos[2];
          }
        }
      }
      {
        const int limit = 2 * static_cast<int>(recCount(t.hang)) - 1;
        if (limit <= rt.animFrame) {
          rt.animFrame = limit;
          rt.eventPriority = 0;
          rt.vert.vertSkip = 0;          // c7c clear on release
        }
        setMain(t.hang, rt.animFrame >> 1);
      }
    } else if (cac == 0x321) {
      // surf-jump — plays to the midpoint then parks until contact
      // (e4c); the contact edge either releases early (->0xc9) or
      // unpins into the second half which then plays out. The frame
      // select still runs on the transition frame (falls through).
      if (enter) {
        rt.animFrame = 0; shadow = 0;
        setMain(t.surfJ, 0);
      } else {
        const int count = static_cast<int>(recCount(t.surfJ));
        const int half = count / 2;
        if (rt.animFrame <= half) {
          rt.animFrame += step;
          if (rt.animFrame > half) rt.animFrame = half;
        } else {
          rt.animFrame += step;
          if (rt.animFrame >= count) {
            rt.animFrame = count - 1;
            rt.locoState = 0xc9;
            rt.eventPriority = 2;
            rt.camera.eyeHeight = 4.5f;
          }
        }
        // 0x462f99 — the shared frame select + contact gate.
        if (half > rt.animFrame)
          rt.camera.eyeHeight =
              static_cast<float>(4.5 - rt.animFrame * 0.2);
        else {
          rt.camera.eyeHeight += env.deltaSec;
          if (rt.camera.eyeHeight > 4.5f)
            rt.camera.eyeHeight = 4.5f;
        }
        if (rt.vert.contactObj != 0) {
          if (rt.animFrame <= half - 2) {
            rt.locoState = 0xc9;
            rt.eventPriority = 2;
            rt.camera.eyeHeight = 4.5f;
          } else if (rt.animFrame <= half) {
            rt.animFrame = half + 1;
          }
        }
        setMain(t.surfJ, rt.animFrame);
      }
    } else if (cac == 0x323) {
      // scope-in — the shared body re-runs every frame.
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else rt.animFrame += step;
      ++rt.seams.hudEventCalls;          // FUN_00402388(0x54c5d4)
      rt.scopeHudOffset = roundNearest(
          static_cast<double>(rt.scopeScale) *
              static_cast<double>(rt.camera.eyeHeight) + 29.0);
      rt.camera.pullback = 0.0f;
      rt.camera.eyeHeight = 4.0f;
      if (rt.transitionPhase == 0) {
        ++rt.seams.hudEventCalls;        // FUN_00401ffc
        rt.transitionPhase = 1;
      }
      setMain(t.still, 0);
    } else if (cac == 0x324) {
      // look up/down — proportional pitch index (recomputed every
      // frame); pitch == 0 self-releases.
      const float pitch = rt.look.lookPitchOffset; // 0x540d58
      if (pitch < 0.0f) {
        rt.animFrame = roundNearest(
            static_cast<double>(pitch) /
            (-60.0 - static_cast<double>(arenaScalar)) *
            (static_cast<int>(recCount(t.lookU)) - 1));
        setMain(t.lookU, rt.animFrame);
      } else if (pitch == 0.0f) {
        rt.animFrame = 0;
        rt.eventPriority = 0;
        setMain(t.lookU, 0);
      } else {
        rt.animFrame = roundNearest(
            static_cast<double>(pitch) /
            (90.0 - static_cast<double>(arenaScalar)) *
            (static_cast<int>(recCount(t.lookD)) - 1));
        setMain(t.lookD, rt.animFrame);
      }
    } else if (cac == 0x325) {
      // action — K_SPWEP hold-last + the frame-8 interact trigger.
      if (enter) {
        rt.invHudTimer = 60;             // 0x541558
        rt.animFrame = 0; shadow = 0;
      } else {
        rt.animFrame += step;
        if (playerAnimFrameCrossed(8, 1, shadow, rt.animFrame)) {
          rt.invHudTimer = 60;
          ++rt.seams.animActionCalls;    // FUN_0046a190 interact
        }
        const int last = static_cast<int>(recCount(t.spewP)) - 1;
        if (last <= rt.animFrame) {
          rt.animFrame = last;
          rt.eventPriority = 0;
        }
      }
      setMain(t.spewP, rt.animFrame);
    } else if (cac == 0x326) {
      // hard landing/crash — half-rate; pins at sub-frame 8 while
      // falling without contact (the brace pose).
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        if (shadow <= 8 && rt.animFrame >= 8 &&
            rt.vert.contactObj == 0 && rt.vert.vertVel <= 0.0f &&
            rt.vert.jumpSustain == 0)
          rt.animFrame = 8;
        const int limit =
            2 * static_cast<int>(recCount(t.crashL));
        if (limit <= rt.animFrame) {
          rt.animFrame = limit - 1;
          rt.eventPriority = 0;
        }
      }
      setMain(t.crashL, rt.animFrame >> 1);
    } else if (cac == 0x327) {
      // slip — +1 per frame (NOT frameStep), chains into 0x328.
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        ++rt.animFrame;
        if (rt.animFrame >= static_cast<int>(recCount(t.slip))) {
          rt.animFrame = 0;
          rt.locoState = 0x328;
        }
      }
      setMain(rt.locoState == 0x328 ? t.slide : t.slip,
              rt.animFrame);
    } else if (cac == 0x328 || cac == 0x329) {
      // slide / forward-slide — wrap loops.
      const std::byte* rec = (cac == 0x328) ? t.slide : t.fSlide;
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        rt.animFrame += step;
        if (rt.animFrame >= static_cast<int>(recCount(rec)))
          rt.animFrame -= static_cast<int>(recCount(rec));
      }
      setMain(rec, rt.animFrame);
    } else if (cac == 0x32a) {
      // back-slide — +1 per frame, holds last (no release).
      if (enter) { rt.animFrame = 0; shadow = 0; }
      else {
        ++rt.animFrame;
        const int last = static_cast<int>(recCount(t.bSlide)) - 1;
        if (rt.animFrame > last) rt.animFrame = last;
      }
      setMain(t.bSlide, rt.animFrame);
    } else if (cac == 0x384) {
      // unscope — overlay release + camera restore every frame.
      if (enter) {
        rt.animFrame = 0; shadow = 0;
        rt.seams.hudEventCalls += 2;     // FUN_0040210c + 0x402388
      } else {
        ++rt.seams.scopeOverlayCalls;    // FUN_00416700
        rt.scopeAnimLatch = 0;
        rt.eventPriority = 0;
        rt.animFrame += step;
      }
      rt.camera.pullback = 8.0f;
      rt.camera.eyeHeight = 4.5f;
      rt.scopeHudOffset = -101;
      setMain(t.still, 0);
    } else if (cac == 0x385) {
      // tumble — K_BANG then K_BFLIP across one shared counter;
      // bFlip entry clears e44/e48.
      if (enter) {
        ++rt.seams.animSoundCalls;       // FUN_0040210c(0x54c5dc)
        rt.animFrame = 0; shadow = 0;
      } else {
        const int bang = static_cast<int>(recCount(t.bang));
        const int limit = bang +
            static_cast<int>(recCount(t.bFlip)) - 1;
        rt.animFrame = nextFrame;
        if (rt.animFrame >= limit) {
          rt.animFrame = limit;
          rt.eventPriority = 0;
        }
      }
      const int bang = static_cast<int>(recCount(t.bang));
      if (rt.animFrame < bang) {
        setMain(t.bang, rt.animFrame);
      } else {
        rt.animE44 = 0;
        rt.animE48 = 0;
        setMain(t.bFlip, rt.animFrame - bang);
      }
    } else if (cac == 0x3e8) {
      // scripted takeoff — wrap advance; the wrap edge chains 0x3e9.
      rt.animMainFrame =
          wrapAdvance(t.takeOf, 1, rt, shadow, step);
      if (shadow > rt.animFrame) {
        rt.animFrame = 0;
        rt.locoState = 0x3e9;
        rt.fieldDa0 = 1;
      }
    } else if (cac == 0x3e9) {
      // scripted float — wrap loop (external release via events).
      rt.animMainFrame =
          wrapAdvance(t.floatC, 1, rt, shadow, step);
    } else {
      // 0x3ea — terminal: K_BANG to last frame, hold (no release).
      if (enter) {
        ++rt.seams.animSoundCalls;       // FUN_0040210c(0x54c5dc)
        rt.animFrame = 0; shadow = 0;
      } else {
        rt.animFrame = nextFrame;
        const int last = static_cast<int>(recCount(t.bang)) - 1;
        if (rt.animFrame > last) rt.animFrame = last;
      }
      setMain(t.bang, rt.animFrame);
    }

    // 0x4619c4 — the first-frame latch reads the POST-handler state.
    rt.animPrev = rt.locoState;
  }

  // 0x4619ce..0x461a14 — the draw gate: the original hands the frame
  // pair + jitter offsets to FUN_00409724 when (0x5414d4 &&
  // (!0x49b740 || (c9c && ca0>=2))). The blit itself is deferred;
  // the visibility decision is surfaced on animDrawn.
  rt.animDrawn = (rt.hudActive != 0) &&
      (rt.flag49b740 == 0 ||
       (rt.flagC9c != 0 && rt.transitionPhase >= 2));
  if (rt.reticleAux != 0) ++rt.seams.reticleBlockCalls; // 0x4372d4
}

} // namespace mdk
