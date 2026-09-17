// Phase 4F — shared front-end input-machine primitives.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phases 4E/4F):
//
//   The front-end root menu (FUN_0041dc90) and the options sub-menu
//   (FUN_00420eac) run the SAME original helpers over the SAME global
//   state block:
//
//     FUN_004187e0   logical mouse accumulate
//                    (DAT_0054b634/38 += raw deltas, clamp 0..599/0..359,
//                    accumulate skipped when the delta is zero)
//     FUN_004237b4   previous/up repeat query  (DAT_0049ac84 deadline)
//     FUN_00423838   next/down repeat query    (DAT_0049ac88)
//     FUN_004238bc   left repeat query         (DAT_0049ac8c)
//     FUN_00423940   right repeat query        (DAT_0049ac90)
//                    — identical bodies; first repeat deadline tick+30,
//                    then tick+3; staleness window +100 resets
//     FUN_00423764   activate query (DIK_RETURN edge DAT_0054b574 OR
//                    any-button down-edge while DAT_0049ac80 is armed;
//                    latch re-arms when all buttons released)
//     FUN_00423a24   selection scale ramp machine
//                    (DAT_0054bdc8/BDCC current key,
//                    DAT_0054bdd0/BDD4 previous key, DAT_0054bdd8 acc;
//                    selected grows 0.65 -> 1.0 at acc*0.07, previous
//                    decays 1.0 -> 0.65, acc clamped at 5.0)
//     FUN_0042fcd0   frame timing update (DAT_0049b6e4 struct;
//                    reached via FUN_0042fb68 at root and FUN_0042fe78
//                    in the options handler — same body)
//
//   Because the original shares ONE set of globals across the two
//   screens, the machine state below is serialized between the root and
//   options controllers on every transition — nothing resets at the
//   FUN_00420cf0 entry except the documented fields (selection = 8).
//
//   Phase 4E proved these bodies inside the root controller; the code
//   is lifted verbatim so both screens share one implementation and the
//   Phase 4E regression tests keep pinning it.
//
#ifndef MDK_CORE_FRONTEND_MACHINES_H
#define MDK_CORE_FRONTEND_MACHINES_H

namespace mdk {

// OBSERVED coordinate clamps (FUN_004187e0 accumulate / FUN_0041dc90 +
// FUN_00420eac hit-test gates — the same constants in both handlers).
inline constexpr int kFrontendMouseMaxX = 599;     // 0x257
inline constexpr int kFrontendMouseMaxY = 359;     // 0x167
inline constexpr int kFrontendHitClampX = 590;     // 0x24e
inline constexpr int kFrontendHitClampY = 350;     // 0x15e
// OBSERVED scale-ramp constants (FUN_00423a24 + dumped data):
//   endpoints 1.0 / 0.65; acc in [0,5]; slope = 0.35*0.2 = 0.07.
inline constexpr float kFrontendRampLimit = 5.0f;    // d[0x496018]
inline constexpr float kFrontendRampSlopeA = 0.35f;  // d[0x49601c]
inline constexpr float kFrontendRampSlopeB = 0.2f;   // d[0x496024]
// OBSERVED key-repeat timing (FUN_004237b4/838/8bc/940, DAT_00541518
// units): first deadline tick+30, repeat deadline tick+3, staleness
// window +100.
inline constexpr int kFrontendRepeatFirstDelay = 30;  // 0x1e
inline constexpr int kFrontendRepeatPeriod = 3;
inline constexpr int kFrontendRepeatWindow = 100;     // 0x64
// OBSERVED stable scale endpoints (FUN_00423a24).
inline constexpr float kFrontendScaleSelected = 1.0f;
inline constexpr float kFrontendScaleUnselected = 0.65f;

// FUN_004187e0 — accumulate one raw delta axis and clamp to the
// 600x360 work surface. The original skips the accumulate entirely
// when the delta is zero.
inline void frontendMouseAccumulate(int& pos, int delta, int max) {
  if (delta == 0) {
    return;
  }
  pos += delta;
  if (pos > max) pos = max;
  if (pos < 0) pos = 0;
}

// FUN_004237b4/838/8bc/940 — the repeat-aware direction query.
// Identical bodies with per-key deadline state. `tick` is
// DAT_00541518 (advanced once per frame before the queries).
inline bool frontendRepeatQuery(int tick, bool held, int& deadline) {
  const int old = deadline;
  const bool fired = held && tick > old;
  if (!held) {
    deadline = 0;
  } else if (old == 0) {
    deadline = tick + kFrontendRepeatFirstDelay;  // press: tick+30
  } else if (tick > old) {
    deadline = tick + kFrontendRepeatPeriod;      // repeat: tick+3
  } else if (tick + kFrontendRepeatWindow < old) {
    deadline = 0;  // deadline anomalously far ahead -> reset state
  }
  // (On fire the original also calls FUN_00423734 -> SND_PUSH; audio
  // deferred — recorded in ENGINE_RECONSTRUCTION Phase 4E.)
  return fired;
}

// Ramp machine state (DAT_0054bdc8..bdd8): current/previous item keys
// plus the accumulator. The item key is the (keyX, keyY) pair the draw
// helper passes — (x_arg, y) at root, (-1, y) in the options sub-menu.
struct FrontendRampState {
  int curX = 0, curY = 0;
  int prevX = 0, prevY = 0;
  float acc = 0.0f;
};

// FUN_00423a24 verbatim — scale for one item during the draw pass.
// `keyX`/`keyY` are the item key; `selFlag` is (item == selection);
// `smoothed` is DAT_0049b6f0 (frame-unit delta). Must be called once
// per drawn item in draw order — the call mutates the machine
// (transition bookkeeping and the once-per-frame accumulator advance).
inline float frontendRampScale(FrontendRampState& r, int keyX, int keyY,
                               bool selFlag, float smoothed) {
  if (selFlag) {
    if (keyX != r.curX || keyY != r.curY) {
      // Selection moved: previous key <- old current, acc restarts.
      r.prevX = r.curX;
      r.prevY = r.curY;
      r.acc = 0.0f;
      r.curX = keyX;
      r.curY = keyY;
    } else {
      r.acc += smoothed;  // += DAT_0049b6f0 once per frame
    }
  }
  const float slope = kFrontendRampSlopeA * kFrontendRampSlopeB;  // 0.07
  if (keyX == r.curX && keyY == r.curY) {
    if (r.acc >= kFrontendRampLimit) {
      return kFrontendScaleSelected;   // 1.0
    }
    return kFrontendScaleUnselected + r.acc * slope;  // 0.65 + acc*0.07
  }
  if (keyX == r.prevX && keyY == r.prevY) {
    if (r.acc >= kFrontendRampLimit) {
      return kFrontendScaleUnselected; // 0.65
    }
    return kFrontendScaleSelected - r.acc * slope;    // 1.0 - acc*0.07
  }
  return kFrontendScaleUnselected;
}

// Frame-timing struct (FUN_0042fb30 init values; DAT_0049b6e4 block).
struct FrontendTimingState {
  int frameStep = 1;        // DAT_0049b6e8
  int stepAccum = 0;        // DAT_0049b6f8
  float smoothed = 1.0f;    // DAT_0049b6f0
  float deltaSec = 1.0f / 30.0f; // DAT_0049b6f4
  int virtualMs = 0;        // DAT_0049b700 (integer-ms virtual clock)
  double realMs = 0.0;      // accumulated real clock (integer-ms domain)
  bool started = false;
};

// FUN_0042fcd0 — the raw delta is measured between the real
// millisecond clock and the virtual clock DAT_0049b700, which chases
// real time at rawDelta*(25/3) ms per frame. The original domain is
// integer milliseconds throughout.
inline void frontendTimingUpdate(FrontendTimingState& t, double dtMs) {
  t.realMs += dtMs;
  const int nowMs = static_cast<int>(t.realMs);
  if (!t.started) {
    // First call (DAT_0049b700 == 0): FUN_0042fb30 inits the struct
    // and the virtual clock is set to now — no delta is computed.
    t.virtualMs = nowMs;
    t.started = true;
    return;
  }
  if (nowMs < t.virtualMs) {
    t.virtualMs = nowMs;  // 0x42fd55 — clock-backwards resync
  }
  // rawDelta = (nowMs - virtualMs) * 120 / 1000 — integer math,
  // truncating division (delta*8*16 - delta*8 = delta*120, DIV 1000).
  const int rawDelta = (nowMs - t.virtualMs) * 120 / 1000;

  // FUN_0042fdc8 — timing struct @0x49b6e4.
  const float frameUnits = static_cast<float>(rawDelta) * 0.25f;
  t.smoothed = t.smoothed * 0.75f + frameUnits * 0.25f;
  t.deltaSec = t.smoothed * (1.0f / 30.0f);
  t.stepAccum += rawDelta;
  int step = t.stepAccum >> 2;
  t.stepAccum &= 3;
  if (step < 1) {
    step = 1;
    t.stepAccum = 0;
  } else if (t.smoothed > 4.0f || step > 4) {
    step = 4;
    t.smoothed = 4.0f;
    t.deltaSec = t.smoothed * (1.0f / 30.0f);
    t.stepAccum = 0;
  }
  t.frameStep = step;

  // virtual = trunc(virtual + rawDelta * 25/3)  [d[0x4971e0] = 8.3333]
  t.virtualMs =
      static_cast<int>(t.virtualMs + rawDelta * (25.0 / 3.0));
}

// The complete shared input-machine block serialized across the
// root <-> options transition. Every field is an original global that
// FUN_00420cf0 (options entry) and FUN_00420d68 (options exit) leave
// untouched — the machine state simply continues on the new screen.
struct FrontendMachineState {
  int mouseX = 300;      // DAT_0054b634
  int mouseY = 180;      // DAT_0054b638
  int tick = 0;          // DAT_00541518
  int prevDeadline = 0;  // DAT_0049ac84 (UP)
  int nextDeadline = 0;  // DAT_0049ac88 (DOWN)
  int leftDeadline = 0;  // DAT_0049ac8c (LEFT — queried by options only)
  int rightDeadline = 0; // DAT_0049ac90 (RIGHT — options only)
  bool buttonLatch = false;  // DAT_0049ac80
  FrontendRampState ramp;    // DAT_0054bdc8..bdd8
  FrontendTimingState timing; // DAT_0049b6e4 struct
};

} // namespace mdk

#endif // MDK_CORE_FRONTEND_MACHINES_H
