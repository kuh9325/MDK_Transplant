// Phase 5A — gameplay input consumption (see header for the evidence
// chain). This file reproduces FUN_00419370's configured-binding ->
// action-flag translation and FUN_00406f14's per-frame merge into the
// 0x4ce6e0..0x4ce7ac control block. All constants are OBSERVED
// doubles/floats from the original's .rdata (see
// docs/GAMEPLAY_RECONSTRUCTION.md §"rate constants").

#include "core/gameplay_input.h"

#include "core/frontend_settings.h"

#include <cmath>

namespace mdk {
namespace {

// The 29-dword keyboard block indices -> semantic slots (OBSERVED —
// the FUN_00419370 call sequence; the visible 19 use the row->global
// pointer table Phase 4K documented).
enum KeySlot {
  kSlotLeft = 0,  kSlotRight = 1,  kSlotUp = 2,     kSlotDown = 3,
  kSlotJump = 4,  kSlotSide = 5,   kSlotFire = 6,   kSlotSniper = 7,
  kSlotTurbo = 8, kSlotSetTurbo = 9,
  kSlotLookUp = 10, kSlotLookDown = 11,
  kSlotZoomIn = 12, kSlotZoomOut = 13,
  // 14..23 — the ten hidden direct-weapon hotkeys.
  kSlotItemNext = 24, kSlotItemPrev = 25, kSlotItemUse = 26,
  kSlotSideL = 27, kSlotSideR = 28,
};

// Internal key codes the helpers fold (OBSERVED — FUN_0041925c and
// the FUN_004192cc/00419320 special cases).
constexpr int kKeyLShift = 0x2a, kKeyRShift = 0x36;
constexpr int kKeyLCtrl = 0x1d,  kKeyRCtrl = 0x61;
constexpr int kKeyLAlt = 0x38,   kKeyRAlt = 0x65;
constexpr int kKeyTab = 15;

std::uint32_t levelBit(const RawGameplayInput& raw, int code) {
  return raw.keyLevel[code >> 5] & (1u << (code & 31));
}
std::uint32_t edgeBit(const RawGameplayInput& raw, int code) {
  return raw.keyEdge[code >> 5] & (1u << (code & 31));
}

// OBSERVED constants — .rdata doubles unless noted.
constexpr double kTurnNorm = 0.9;          // 0x4944d8
constexpr double kFour = 4.0;              // 0x4944e0
constexpr double kTurnTurbo = 1.3;         // 0x4944e8
constexpr double kSix = 6.0;               // 0x4944f0
constexpr double kThreeQuarter = 0.75;     // 0x4944f8
constexpr double kYawNorm = 0.4;           // 0x494500
constexpr double kYawTurbo = 0.6;          // 0x494508
constexpr float kYaw45 = 45.0f;            // 0x494510 (f32)
constexpr double kThird = 1.0 / 3.0;       // 0x494518
constexpr double kTen = 10.0;              // 0x494520
constexpr double kStrafeNorm = 1.0 / 22.5; // 0x494528
constexpr double kTwoThirds = 2.0 / 3.0;   // 0x494530
constexpr double kStrafeTurbo = 4.0 / 45.0;// 0x494538
constexpr double kFourThirds = 4.0 / 3.0;  // 0x494540
constexpr double kHalf = 0.5;              // 0x494548
constexpr double kFivePct = 0.05;          // 0x494550
constexpr float kMoveRateA = 35.0f;        // 0x494558 (f32) — selected
constexpr float kMoveRateB = 15.0f;        // 0x49455c (f32)    by sign
constexpr double kDebugMove = 20.0 / 3.0;  // 0x494560
constexpr float kRateClampHi = 4.0f;       // 0x494568 (f32)
constexpr float kRateClampLo = -4.0f;      // 0x49456c (f32)
constexpr double kDeadzone = 0.2;          // 0x494570
constexpr float kZoomVelSlow = 0.01f;      // 0x3c23d70a (sign per acc)
constexpr float kZoomVelFast = 0.15f;      // 0x3e19999a (sign per acc)

float clampRate(float v) {
  if (v > kRateClampHi) return kRateClampHi;
  if (v < kRateClampLo) return kRateClampLo;
  return v;
}

// The button-mask decode shared by the joystick (16 buttons) and
// mouse (4 buttons) loops — the 16 JOY_B* action bits.
struct MaskMerge {
  std::uint32_t snipeRequest = 0;  // local_30
  bool turbo = false;              // mask bit15
  // Axis pre-seeds the mask bits write (the kbd/joy merge may
  // overwrite them later, exactly like the original locals).
  float strafe = 0;                // local_2c pre-seed
  float move = 0;                  // local_28 pre-seed
};

void applyButtonMask(std::uint32_t mask, MaskMerge& m,
                     GameplayInputFrame& f,
                     GameplayInputState& state) {
  if (mask & kBtnFire) f.fire = 1;
  if (mask & kBtnSniper) m.snipeRequest = 1;
  if (mask & kBtnJump) f.jump = 1;
  if (mask & kBtnLookUp) f.lookUp = 1;
  if (mask & kBtnLookDown) f.lookDown = 1;
  if (mask & kBtnItemUse) f.itemUse = 1;
  if (mask & kBtnItemNext) f.itemNext = 1;
  if (mask & kBtnItemPrev) f.itemPrev = 1;
  if (mask & kBtnZoomIn) state.zoomAccumulator += 1;
  if (mask & kBtnZoomOut) state.zoomAccumulator -= 1;
  if (mask & kBtnStrafeLeft) m.strafe = -1.0f;
  if (mask & kBtnStrafeRight) m.strafe = 1.0f;
  if (mask & kBtnMoveFwd) m.move = -1.0f;
  if (mask & kBtnMoveBack) m.move = 1.0f;
  if (mask & kBtnTurbo) m.turbo = true;
}

} // namespace

GameplayInputBindings gameplayBindingsFromSettings(
    const FrontendSettings& s) {
  GameplayInputBindings b;
  b.keys = keyboardGlobalsFromSettings(s);
  b.mouseAxesMap = s.mouseWAxesMap;
  b.mouseButtMask = {s.mouseWButtMapA, s.mouseWButtMapB,
                     s.mouseWButtMapC, s.mouseWButtMapD};
  b.mouseScale = {s.mouseWXScale, s.mouseWYScale, s.mouseWZScale};
  b.mouseOn = s.mouseOn;
  b.mouseYReversedBits = s.mouseYReversed;
  return b;
}

std::uint32_t gameplayKeyLevel(const RawGameplayInput& raw, int code) {
  // FUN_004192cc — the right-modifier fold lives in the query, so a
  // binding on the left code sees the right key too.
  if (code == kKeyLShift) {
    return levelBit(raw, kKeyLShift) | levelBit(raw, kKeyRShift);
  }
  if (code == kKeyLCtrl) {
    return levelBit(raw, kKeyLCtrl) | levelBit(raw, kKeyRCtrl);
  }
  if (code == kKeyLAlt) {
    return levelBit(raw, kKeyLAlt) | levelBit(raw, kKeyRAlt);
  }
  return levelBit(raw, code);
}

std::uint32_t gameplayKeyEdge(const RawGameplayInput& raw, int code) {
  // FUN_00419320 — same fold on the edge bitmap.
  if (code == kKeyLShift) {
    return edgeBit(raw, kKeyLShift) | edgeBit(raw, kKeyRShift);
  }
  if (code == kKeyLCtrl) {
    return edgeBit(raw, kKeyLCtrl) | edgeBit(raw, kKeyRCtrl);
  }
  if (code == kKeyLAlt) {
    return edgeBit(raw, kKeyLAlt) | edgeBit(raw, kKeyRAlt);
  }
  return edgeBit(raw, code);
}

GameplayInputFrame consumeGameplayInput(
    const RawGameplayInput& raw,
    const GameplayInputBindings& bind,
    const GameplayInputEnvironment& env,
    GameplayInputState& state) {
  GameplayInputFrame f;

  // ---- FUN_00419370: configured bindings -> action flags ----------
  const auto lv = [&](int slot) {
    return gameplayKeyLevel(raw, bind.keys[slot]);
  };
  const auto ed = [&](int slot) {
    return gameplayKeyEdge(raw, bind.keys[slot]);
  };

  // Flags keep the original's raw masked values (a folded-modifier
  // query returns the literal bit mask, e.g. 0x400400 — consumers
  // test != 0; no normalization happens in the original either).
  const std::uint32_t keyLeft = lv(kSlotLeft);
  const std::uint32_t keyRight = lv(kSlotRight);
  const std::uint32_t keyUp = lv(kSlotUp);
  const std::uint32_t keyDown = lv(kSlotDown);
  const std::uint32_t keyJump = lv(kSlotJump);
  const std::uint32_t keySide = lv(kSlotSide);
  const std::uint32_t keyFire = lv(kSlotFire);
  const std::uint32_t keySnipe = ed(kSlotSniper);
  const std::uint32_t keyTurbo = lv(kSlotTurbo);
  const std::uint32_t keySetTurbo = ed(kSlotSetTurbo);
  const std::uint32_t keyLookUp = lv(kSlotLookUp);
  const std::uint32_t keyLookDown = lv(kSlotLookDown);
  const std::uint32_t keyZoomIn = lv(kSlotZoomIn);
  const std::uint32_t keyZoomOut = lv(kSlotZoomOut);
  // 0x54b688/0x54b68c (the zoom EDGE copies) exist in the original's
  // flag block but have no BUILD_A reader — not reproduced.
  const std::uint32_t keyItemNext = ed(kSlotItemNext);
  const std::uint32_t keyItemPrev = ed(kSlotItemPrev);
  const std::uint32_t keyItemUse = ed(kSlotItemUse);
  const std::uint32_t keySideL = lv(kSlotSideL);
  const std::uint32_t keySideR = lv(kSlotSideR);

  // ---- FUN_00406f14, in original order -----------------------------

  // STURB edge toggles the latch first.
  if (keySetTurbo != 0) {
    state.setTurboLatch = (state.setTurboLatch == 0) ? 1u : 0u;
  }

  // Flag copies: keyboard flags seed the control block (raw values).
  f.jump = keyJump;
  f.sniperPulse = keySnipe;
  f.fire = keyFire;
  f.itemUse = keyItemUse;
  f.itemNext = keyItemNext;
  f.itemPrev = keyItemPrev;
  for (int i = 0; i < kGameplayWeaponHotkeyCount; ++i) {
    f.weaponSelect[i] = ed(kGameplayWeaponSlotBase + i);
  }
  f.lookUp = keyLookUp;
  f.lookDown = keyLookDown;
  if (keyZoomIn != 0) state.zoomAccumulator += 1;
  if (keyZoomOut != 0) state.zoomAccumulator -= 1;

  // SideStep modifier (iVar11): keyboard SIDE level, then the button
  // masks may OR their bit10 in.
  std::uint32_t sideStep = keySide;

  // Button-mask decode — joystick buttons 0..15 then mouse 0..3, in
  // the original's loop order. One button may set several action
  // bits; pressed buttons accumulate in order (later buttons can
  // overwrite the axis pre-seeds — faithful to the sequential
  // mask writes).
  MaskMerge m;
  if (bind.joyOn) {
    for (int i = 0; i < 16; ++i) {
      if (raw.joyButtons & (1u << i)) {
        const std::uint32_t mask = bind.joyButtMask[i];
        applyButtonMask(mask, m, f, state);
        if (mask & kBtnSideStep) sideStep = 1;
      }
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (raw.mouseButtons & (1u << i)) {
      const std::uint32_t mask = bind.mouseButtMask[i];
      applyButtonMask(mask, m, f, state);
      if (mask & kBtnSideStep) sideStep = 1;
    }
  }

  // Synthetic snipe edge for the button path: a held snipe button
  // pulses only on the request's rising edge (0x499f50 latch).
  if (m.snipeRequest != 0 && state.sniperButtonLatch == 0) {
    f.sniperPulse = 1;
  }
  state.sniperButtonLatch = m.snipeRequest;

  // ---- axis merge ---------------------------------------------------
  const bool turboAny =
      keyTurbo != 0 || state.setTurboLatch != 0 || m.turbo;

  // Turn axis (local_34): LEFT/RIGHT levels unless the SideStep
  // modifier reroutes them; else the first nonzero joy A/D axis
  // (which is itself gated by sideStep == 0 inside the scan).
  float turnAxis = 0.0f;
  bool turnTurbo = turboAny;
  if (keyLeft != 0 && sideStep == 0) {
    turnAxis = -1.0f;
  } else if (keyRight != 0 && sideStep == 0) {
    turnAxis = 1.0f;
  } else if (bind.joyOn) {
    int i = 0;
    for (char c : bind.joyAxesMap) {
      if (c == '\0' || i >= 6) break;
      if ((c == 'A' || c == 'D') && sideStep == 0 &&
          raw.joyAxes[i] != 0.0f) {
        turnAxis = raw.joyAxes[i];
        if (c == 'D') turnAxis = -turnAxis;
        if (bind.joyType != 0) turnTurbo = true;
        break;
      }
      ++i;
    }
  }

  // Move axes: local_38 (axis) + local_28 (digital). The mask
  // pre-seeds local_28; keyboard overwrites both; a joy axis
  // replaces local_28 only when strictly larger in magnitude.
  float moveAxis = 0.0f;          // local_38
  float moveDigital = m.move;     // local_28 (mask pre-seed)
  bool moveTurbo = turboAny;
  if (keyUp != 0) {
    moveAxis = -1.0f;
    moveDigital = moveAxis;
  } else if (keyDown != 0) {
    moveAxis = 1.0f;
    moveDigital = 1.0f;
  } else if (bind.joyOn) {
    int i = 0;
    for (char c : bind.joyAxesMap) {
      if (c == '\0' || i >= 6) break;
      if ((c == 'B' || c == 'E') && raw.joyAxes[i] != 0.0f) {
        moveAxis = raw.joyAxes[i];
        if (c == 'E') moveAxis = -moveAxis;
        if (bind.joyType != 0) moveTurbo = true;
        if (std::fabs(moveDigital) < std::fabs(moveAxis)) {
          moveDigital = moveAxis;
        }
        break;
      }
      ++i;
    }
  }

  // Strafe axis (local_2c): mask pre-seed, then (LEFT|RIGHT under
  // SideStep) or SIDEL/SIDER overwrite, else first matching joy axis
  // (C/F always; A/D route here when the modifier is held).
  float strafeAxis = m.strafe;    // mask pre-seed
  bool strafeTurbo = turboAny;
  if ((keyLeft != 0 && sideStep != 0) || keySideL != 0) {
    strafeAxis = -1.0f;
  } else if ((keyRight != 0 && sideStep != 0) || keySideR != 0) {
    strafeAxis = 1.0f;
  } else if (bind.joyOn) {
    int i = 0;
    for (char c : bind.joyAxesMap) {
      if (c == '\0' || i >= 6) break;
      const bool match = (c == 'C' || c == 'F') ||
                         ((c == 'A' || c == 'D') && sideStep != 0);
      if (match && raw.joyAxes[i] != 0.0f) {
        strafeAxis = raw.joyAxes[i];
        if (c == 'F' || c == 'D') strafeAxis = -strafeAxis;
        if (bind.joyType != 0) strafeTurbo = true;
        break;
      }
      ++i;
    }
  }

  // Combined lateral axis (fVar8): larger magnitude of strafe/turn.
  float yawAxis = strafeAxis;
  if (std::fabs(strafeAxis) < std::fabs(turnAxis)) {
    yawAxis = turnAxis;
  }

  // ---- mouse G/H (sniper-zoom) scan ---------------------------------
  // MouseOn gates BOTH this and the A..F merge. Letters index axes
  // 0..2; the scan stops at NUL, index 3, or the FIRST G/H letter —
  // an OBSERVED early-out (later axes' letters never process).
  if (bind.mouseOn) {
    const int n = (int)bind.mouseAxesMap.size();
    for (int i = 0; i < 3 && i < n; ++i) {
      const char c = bind.mouseAxesMap[i];
      if (c == '\0') break;
      if (c == 'G' || c == 'H') {
        const std::int32_t delta =
            i == 0 ? raw.mouseDx : i == 1 ? raw.mouseDy : raw.mouseDz;
        if (delta != 0) {
          // OBSERVED: rint(delta/scale + sign(delta)*1) — the +-1
          // bias makes any nonzero delta produce at least one tick.
          const double scaled = (double)delta / (double)bind.mouseScale[i] +
                                (delta > 0 ? 1.0 : -1.0);
          const int ticks = (int)std::nearbyint(scaled);
          state.zoomAccumulator += (c == 'G' ? ticks : -ticks);
          if (state.zoomAccumulator > 8) state.zoomAccumulator = 8;
          if (state.zoomAccumulator < -8) state.zoomAccumulator = -8;
        }
        break;
      }
    }
  }

  // ---- rate products (keyboard/joy axes) ----------------------------
  if (turnAxis != 0.0f) {
    const double norm = turnTurbo ? kTurnTurbo : kTurnNorm;
    const double fast = turnTurbo ? kSix : kFour;
    f.turnNorm = (float)(turnAxis * norm);
    f.turnFast = (float)(turnAxis * fast);
    // OBSERVED quirk: the 0.75-scaled pair always uses the NON-turbo
    // constants even when turbo is active.
    f.turnNorm75 = (float)(turnAxis * kTurnNorm * kThreeQuarter);
    f.turnFast75 = (float)(turnAxis * kFour * kThreeQuarter);
  }
  if (yawAxis != 0.0f) {
    const bool t = turnTurbo || strafeTurbo;
    f.yawNorm = (float)(yawAxis * (t ? kYawTurbo : kYawNorm));
    f.yawFast = (float)(yawAxis * (t ? kSix : kFour));
    f.yawNeg45 = (float)(-yawAxis * (double)kYaw45);
    f.yaw4 = (float)(yawAxis * kFour);
    f.yawThird = (float)(yawAxis * kThird);
    f.yaw10 = (float)(yawAxis * kTen);
  }
  if (strafeAxis != 0.0f) {
    f.strafeNorm =
        (float)(strafeAxis * (strafeTurbo ? kStrafeTurbo : kStrafeNorm));
    f.strafeFast =
        (float)(strafeAxis * (strafeTurbo ? kFourThirds : kTwoThirds));
  }
  if (moveDigital != 0.0f) {
    if (moveTurbo) {
      f.moveNorm = (float)(moveAxis * kYawTurbo);
      f.moveFast = (float)(moveAxis * kSix);
      f.moveVelBoosted = -moveDigital;
      f.moveVel = (float)(f.moveVelBoosted * kStrafeTurbo);
      if (env.moveBoostGate == 0) {
        f.moveVelBoosted = (float)(f.moveVelBoosted * kFourThirds);
      }
    } else {
      f.moveNorm = (float)(moveAxis * kYawNorm);
      f.moveFast = (float)(moveAxis * kFour);
      f.moveVel = (float)(-moveDigital * kStrafeNorm);
      f.moveVelBoosted = (float)(-moveDigital * kTwoThirds);
    }
    const float speed = -moveDigital;
    f.moveHalfSlow = (float)(speed * kStrafeNorm * kHalf);
    f.moveHalfFast = (float)(speed * kTwoThirds * kHalf);
    f.move5pct = (float)(speed * kFivePct);
    f.moveThird = (float)(moveAxis * kThird);
    f.move10 = (float)(moveAxis * kTen);
    f.moveSpeed = speed * (moveAxis < 0.0f ? kMoveRateB : kMoveRateA);
    if (env.debugMoveBoost != 0 && levelBit(raw, kKeyTab) != 0) {
      f.moveVel = (float)(-moveDigital * kDebugMove);
      f.moveVelBoosted = f.moveVel;
    }
  }

  // ---- mouse A..F axis merge -----------------------------------------
  // Per-axis letter -> delta/scale (sign-flipped for the negative
  // letters), deadzone |v| < 0.2 -> 0, then dt-normalized clamp +-4.
  float mTurn = 0.0f, mMove = 0.0f, mStrafe = 0.0f;  // local_20/1c/24
  if (bind.mouseOn) {
    const int n = (int)bind.mouseAxesMap.size();
    for (int i = 0; i < 3 && i < n; ++i) {
      const char c = bind.mouseAxesMap[i];
      if (c == '\0') break;
      const std::int32_t delta =
          i == 0 ? raw.mouseDx : i == 1 ? raw.mouseDy : raw.mouseDz;
      if (delta == 0) continue;
      const bool strafeRoute = (c == 'C' || c == 'F') ||
                               ((c == 'A' || c == 'D') && sideStep != 0);
      const bool turnRoute = (c == 'A' || c == 'D') && sideStep == 0;
      const bool moveRoute = (c == 'B' || c == 'E');
      if (!strafeRoute && !turnRoute && !moveRoute) continue;
      float v = (float)delta / bind.mouseScale[i];
      if (c == 'D' || c == 'E' || c == 'F') v = -v;
      if (std::fabs(v) < kDeadzone) v = 0.0f;
      if (turnRoute) mTurn = v;
      else if (moveRoute) mMove = v;
      else mStrafe = v;
    }
  }
  const float mLateral =  // local_3c — larger |.| of strafe/turn
      std::fabs(mStrafe) < std::fabs(mTurn) ? mTurn : mStrafe;

  // Mouse axis -> rate overrides (the dt-normalized +-4-clamped path).
  if (mTurn != 0.0f) {
    const float v = clampRate(mTurn / env.frameStep);
    f.turnFast = (float)(v * kSix * kHalf);
    f.turnNorm = f.turnFast / env.frameStep;
    f.turnNorm75 = f.turnNorm * (float)kThreeQuarter;
    f.turnFast75 = (float)(kThreeQuarter * (double)f.turnFast);
    f.mouseTurnActive = 1;
  }
  if (mLateral != 0.0f) {
    const float v = clampRate(mLateral / env.frameStep);
    f.yaw4 = (float)(v * kFour);
    f.yawNeg45 = (float)(-v * (double)kYaw45);
  }
  if (mStrafe != 0.0f) {
    const float v = clampRate(mStrafe / env.frameStep);
    f.strafeFast = (float)(v * kFourThirds * kHalf);
    f.strafeNorm = f.strafeFast / env.frameStep;
  }
  if (mMove != 0.0f) {
    const float v = clampRate(mMove / env.frameStep);
    const float boosted =
        env.moveBoostGate == 0 ? (float)(-v * kFourThirds) : -v;
    f.moveVelBoosted = boosted * (float)kHalf;
    f.moveHalfFast = (float)(-v * kTwoThirds * kHalf * kHalf);
    f.move5pct = (float)(-v * kFivePct * kHalf);
    // OBSERVED quirk: this field uses the DIGITAL move axis (local_28
    // — 0 for pure-mouse input), not the mouse-normalized v.
    f.moveSpeed = (-moveDigital * (v >= 0.0f ? kMoveRateA : kMoveRateB)) *
                  (float)kHalf;
    f.moveVel = f.moveVelBoosted * (float)(1.0 / (double)env.frameStep);
    f.moveHalfSlow =
        (float)(1.0 / (double)env.frameStep) * f.moveHalfFast;
  }

  // ---- zoom accumulator tail -----------------------------------------
  // Charges while |acc| >= 1 emit the zoom velocities; the acc
  // decays by the raw frame delta toward 0 (OBSERVED int math).
  if (state.zoomAccumulator >= 1) {
    f.zoomVel = -kZoomVelSlow;
    f.zoomVelFast = -kZoomVelFast;
    state.zoomAccumulator -= env.frameDt;
    if (state.zoomAccumulator < 0) state.zoomAccumulator = 0;
  } else if (state.zoomAccumulator <= -1) {
    f.zoomVel = kZoomVelSlow;
    f.zoomVelFast = kZoomVelFast;
    state.zoomAccumulator += env.frameDt;
    if (state.zoomAccumulator > 0) state.zoomAccumulator = 0;
  }

  // ---- state echo + pass-through -------------------------------------
  f.zoomAccumulator = state.zoomAccumulator;
  f.sideStepHeld = sideStep != 0;
  f.turboLatched = state.setTurboLatch != 0;
  f.turnAxis = turnAxis;
  f.moveAxis = moveAxis;
  f.moveDigital = moveDigital;
  f.strafeAxis = strafeAxis;
  f.yawAxis = yawAxis;
  f.mouseDx = raw.mouseDx;
  f.mouseDy = raw.mouseDy;
  f.mouseDz = raw.mouseDz;
  f.mouseButtons = raw.mouseButtons;
  f.mouseOn = bind.mouseOn;
  f.mouseYReversed = bind.mouseYReversedBits != 0;
  return f;
}

} // namespace mdk
