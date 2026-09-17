#include "core/frontend_menu.h"

#include "core/compat.h"
#include "core/framebuffer.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/indexed_image.h"

#include <cstring>

namespace mdk {

FrontendMenuSpec frontendMenuSpec(bool savesExist) {
  FrontendMenuSpec spec;
  // FUN_0041d85c: DAT_0049aa78 = !savesExist -> saves: sel 0
  // ("Continue"), no saves: sel 1 ("New Game").
  const int selected = savesExist ? 0 : 1;
  if (savesExist) {
    for (int i = 0; i < kFrontendOptCount; ++i) {
      spec.items.push_back(
          {i, kFrontendItemY0 + kFrontendItemStep * i, i == selected});
    }
  } else {
    // No-saves branch: OPT1..OPT4 at y = 31,67,103,139.
    for (int i = 1; i < kFrontendOptCount; ++i) {
      spec.items.push_back(
          {i, kFrontendItemY0 + kFrontendItemStep * (i - 1),
           i == selected});
    }
  }
  return spec;
}

bool renderFrontendMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                             const IndexedImage& backdrop,
                             const FtiFont& fontBig,
                             const FtiSpriteFrame& arrow,
                             std::span<const std::string_view> optStrings,
                             const FrontendMenuSpec& spec,
                             std::string* err) {
  auto fail = [&](const char* msg) {
    if (err) {
      *err = msg;
    }
    return false;
  };

  if (backdrop.width != fb.width() || backdrop.height != fb.height() ||
      backdrop.stride != backdrop.width) {
    return fail("frontend menu: backdrop is not the 600x360 work size");
  }
  if (optStrings.size() < kFrontendOptCount) {
    return fail("frontend menu: OPT0..OPT4 strings not resolved");
  }

  // Step 1 (OBSERVED): memcpy(fb, MDKOPT pixels, 0x34bc0) — the image
  // is exactly the work-buffer size so this is a flat copy.
  std::memcpy(fb.pixels(), backdrop.pixels.data(),
              static_cast<std::size_t>(fb.width()) * fb.height());

  // Palette upload (FUN_00413b40 -> FUN_0046d208 head+tail: the full
  // 256-entry embedded palette).
  if (backdrop.hasPalette) {
    for (int i = 0; i < palette.size(); ++i) {
      const auto& c = backdrop.palette[static_cast<std::size_t>(i)];
      palette.set(i, {c.r, c.g, c.b, 255});
    }
  }

  // Step 2: items. x_arg = maxW/2 — integer signed division per the
  // original SAR/SUB/SAR pattern; maxW = largest UNSCALED measure
  // over the drawn items (FUN_00414be8, missing-glyph advance 6).
  int maxW = 0;
  for (const auto& item : spec.items) {
    const auto& text = optStrings[item.optIndex];
    if (text.empty()) {
      return fail("frontend menu: empty OPT string");
    }
    const int w = measureFtiText(fontBig, text,
                                 kFtiFontBigMissingAdvance);
    if (w > maxW) {
      maxW = w;
    }
  }
  const int xArg = maxW / 2;  // integer, trunc toward zero (SAR)

  for (const auto& item : spec.items) {
    const auto& text = optStrings[item.optIndex];
    const float scale = item.selected ? kFrontendScaleSelected
                                      : kFrontendScaleUnselected;
    const int w = measureFtiText(fontBig, text,
                                 kFtiFontBigMissingAdvance);
    // FUN_00423b38: x = trunc(x_arg - w*scale*0.5) — x87 truncation
    // toward zero on the f32 scale value.
    const int x = static_cast<int>(
        static_cast<double>(xArg) -
        static_cast<double>(w) * static_cast<double>(scale) * 0.5);
    drawFtiTextScaled(fontBig, text, fb, x, item.y, scale,
                      kFtiFontBigMissingAdvance);
  }

  // Step 3: ARROW at the raw mouse position (FUN_004236c0 —
  // hotspot applied inside the blit).
  blitFtiSpriteFrame(arrow, fb, spec.arrowX, spec.arrowY);
  return true;
}

// ---------------------------------------------------------------------------
// Phase 4E — FUN_0041dc90 interactive controller.

FrontendMenuController::FrontendMenuController(bool savesExist)
    : savesExist_(savesExist), selection_(savesExist ? 0 : 1),
      mouseX_(kFrontendMouseResetX), mouseY_(kFrontendMouseResetY) {}

FrontendAction FrontendMenuController::consumeAction() {
  const FrontendAction a = action_;
  action_ = FrontendAction::None;
  return a;
}

// FUN_004237b4 / FUN_00423838 — the repeat-aware direction queries.
// Identical bodies with per-key deadline state (DAT_0049ac84/88).
// `tick_` is DAT_00541518 (advanced once per frame before the queries).
bool FrontendMenuController::repeatQuery(bool held,
                                         int& deadline) const {
  const int old = deadline;
  const bool fired = held && tick_ > old;
  if (!held) {
    deadline = 0;
  } else if (old == 0) {
    deadline = tick_ + kFrontendRepeatFirstDelay;  // press: tick+30
  } else if (tick_ > old) {
    deadline = tick_ + kFrontendRepeatPeriod;      // repeat: tick+3
  } else if (tick_ + kFrontendRepeatWindow < old) {
    deadline = 0;  // deadline anomalously far ahead -> reset state
  }
  // (On fire the original also calls FUN_00423734 -> SND_PUSH; audio
  // deferred — recorded in ENGINE_RECONSTRUCTION Phase 4E.)
  return fired;
}

void FrontendMenuController::update(const FrontendMenuInput& in) {
  // FUN_004187e0: accumulate raw deltas and clamp to the 600x360 work
  // surface. The accumulate is skipped when the delta is zero.
  if (in.mouseDx != 0) {
    mouseX_ += in.mouseDx;
    if (mouseX_ > kFrontendMouseMaxX) mouseX_ = kFrontendMouseMaxX;
    if (mouseX_ < 0) mouseX_ = 0;
  }
  if (in.mouseDy != 0) {
    mouseY_ += in.mouseDy;
    if (mouseY_ > kFrontendMouseMaxY) mouseY_ = kFrontendMouseMaxY;
    if (mouseY_ < 0) mouseY_ = 0;
  }

  // Main loop: DAT_00541518 += DAT_0049b6e8 before the mode handler.
  tick_ += frameStep_;

  // DAT_0049aa98 is the item-list state; the stable root list is 0.
  // On any selection change the original resets DAT_0049aaa4 to 0, or
  // 999.0f when list state is 1 (transition — unreachable here).
  const float idleReset = (listState() == 1) ? 999.0f : 0.0f;

  // 1. prev query (FUN_004237b4 — DIK_UP held).
  if (repeatQuery(in.prevHeld, prevDeadline_)) {
    idleSeconds_ = idleReset;
    selection_ -= 1;
    if (selection_ < 0 || (selection_ == 0 && !savesExist_)) {
      selection_ = kFrontendOptCount - 1;  // wraps to bottom (4)
    }
  }

  // 2. next query (FUN_00423838 — DIK_DOWN held).
  if (repeatQuery(in.nextHeld, nextDeadline_)) {
    idleSeconds_ = idleReset;
    selection_ += 1;
    if (selection_ >= kFrontendOptCount) {
      selection_ = savesExist_ ? 0 : 1;    // wraps to top
    }
  }

  // 3. Mouse hit-test — gated on ANY mouse input this frame
  // (dx | dy | buttons). The controller clamps the persistent position
  // to (590,350) inside the gate — a second, tighter clamp than the
  // accumulate path's (599,359).
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    if (mouseX_ > kFrontendHitClampX) mouseX_ = kFrontendHitClampX;
    if (mouseY_ > kFrontendHitClampY) mouseY_ = kFrontendHitClampY;
    // band = trunc((y - 5) / 36) — x86 IDIV semantics; C++ integer
    // division truncates toward zero identically.
    int band = (mouseY_ - kFrontendHitBandBase) / kFrontendHitBandSize;
    if (!savesExist_) {
      band += 1;  // hidden OPT0 keeps index 0; mouse bands map to 1..4
    }
    if (band >= (savesExist_ ? 0 : 1) && band < kFrontendOptCount &&
        band != selection_) {
      selection_ = band;
      idleSeconds_ = idleReset;
    }
  }

  // 4. Activate query (FUN_00423764): Enter edge, or any-button
  // down-edge while the latch is armed. Latch re-arms when all buttons
  // are released.
  if (in.mouseButtons == 0) {
    buttonLatch_ = true;
  }
  bool fire = in.confirmEdge;
  if (!fire && buttonLatch_ && in.mouseButtons != 0) {
    fire = true;
  }
  if (fire) {
    buttonLatch_ = false;
    idleSeconds_ = idleReset;
    // Dispatch (FUN_0041dc90 branch block) — semantic actions only;
    // every branch but Options also runs FUN_0041dbd4 cleanup and the
    // quit branch sets DAT_0054148e. Downstream systems are deferred.
    switch (selection_) {
    case 0:
      action_ = savesExist_ ? FrontendAction::ContinueGame
                            : FrontendAction::Quit;  // unreachable guard
      break;
    case 1:
      action_ = FrontendAction::NewGame;
      break;
    case 2:
      action_ = FrontendAction::SavedGame;
      break;
    case 3:
      action_ = FrontendAction::OpenOptions;
      break;
    default:
      action_ = FrontendAction::Quit;
      break;
    }
  }

  // 5. Idle/attract timer: DAT_0049aaa4 += DAT_0049b6f4 each frame.
  idleSeconds_ += deltaSec_;

  // 6. DIK_RIGHT press edge (DAT_0054b554) with list state >= 0 forces
  // the attract trigger (FUN_0041ef74 path) — emitted as a semantic
  // event; the slideshow itself is deferred. An activation dispatched
  // earlier in the same frame already leaves the menu, so it wins.
  if (in.attractEdge && listState() >= 0 &&
      action_ == FrontendAction::None) {
    action_ = FrontendAction::EnterAttract;
  }
}

float FrontendMenuController::itemScale(int centerX, int itemY,
                                        bool selFlag) {
  // FUN_00423a24 verbatim. The (centerX,itemY) pair is the item key.
  if (selFlag) {
    if (centerX != rampCurX_ || itemY != rampCurY_) {
      // Selection moved: previous key <- old current, acc restarts.
      rampPrevX_ = rampCurX_;
      rampPrevY_ = rampCurY_;
      rampAcc_ = 0.0f;
      rampCurX_ = centerX;
      rampCurY_ = itemY;
    } else {
      rampAcc_ += smoothed_;  // += DAT_0049b6f0 once per frame
    }
  }
  const float slope = kFrontendRampSlopeA * kFrontendRampSlopeB;  // 0.07
  if (centerX == rampCurX_ && itemY == rampCurY_) {
    if (rampAcc_ >= kFrontendRampLimit) {
      return kFrontendScaleSelected;   // 1.0
    }
    return kFrontendScaleUnselected + rampAcc_ * slope;  // 0.65 + acc*0.07
  }
  if (centerX == rampPrevX_ && itemY == rampPrevY_) {
    if (rampAcc_ >= kFrontendRampLimit) {
      return kFrontendScaleUnselected; // 0.65
    }
    return kFrontendScaleSelected - rampAcc_ * slope;    // 1.0 - acc*0.07
  }
  return kFrontendScaleUnselected;
}

void FrontendMenuController::endFrame(double dtMs) {
  // FUN_0042fcd0 — the raw delta is measured between the real
  // millisecond clock and the virtual clock DAT_0049b700, which chases
  // real time at rawDelta*(25/3) ms per frame. The original domain is
  // integer milliseconds throughout.
  realMs_ += dtMs;
  const int nowMs = static_cast<int>(realMs_);
  if (!timingStarted_) {
    // First call (DAT_0049b700 == 0): FUN_0042fb30 inits the struct
    // and the virtual clock is set to now — no delta is computed.
    virtualMs_ = nowMs;
    timingStarted_ = true;
    return;
  }
  if (nowMs < virtualMs_) {
    virtualMs_ = nowMs;  // 0x42fd55 — clock-backwards resync
  }
  // rawDelta = (nowMs - virtualMs) * 120 / 1000 — integer math,
  // truncating division (delta*8*16 - delta*8 = delta*120, DIV 1000).
  const int rawDelta = (nowMs - virtualMs_) * 120 / 1000;

  // FUN_0042fdc8 — timing struct @0x49b6e4.
  const float frameUnits = static_cast<float>(rawDelta) * 0.25f;
  smoothed_ = smoothed_ * 0.75f + frameUnits * 0.25f;
  deltaSec_ = smoothed_ * (1.0f / 30.0f);
  stepAccum_ += rawDelta;
  int step = stepAccum_ >> 2;
  stepAccum_ &= 3;
  if (step < 1) {
    step = 1;
    stepAccum_ = 0;
  } else if (smoothed_ > 4.0f || step > 4) {
    step = 4;
    smoothed_ = 4.0f;
    deltaSec_ = smoothed_ * (1.0f / 30.0f);
    stepAccum_ = 0;
  }
  frameStep_ = step;

  // virtual = trunc(virtual + rawDelta * 25/3)  [d[0x4971e0] = 8.3333]
  virtualMs_ =
      static_cast<int>(virtualMs_ + rawDelta * (25.0 / 3.0));
}

bool renderFrontendMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                               const IndexedImage& backdrop,
                               const FtiFont& fontBig,
                               const FtiSpriteFrame& arrow,
                               std::span<const std::string_view> optStrings,
                               FrontendMenuController& ctl,
                               std::string* err) {
  auto fail = [&](const char* msg) {
    if (err) {
      *err = msg;
    }
    return false;
  };
  if (backdrop.width != fb.width() || backdrop.height != fb.height() ||
      backdrop.stride != backdrop.width) {
    return fail("frontend menu: backdrop is not the 600x360 work size");
  }
  if (optStrings.size() < kFrontendOptCount) {
    return fail("frontend menu: OPT0..OPT4 strings not resolved");
  }

  // Same composition as renderFrontendMenuFrame: backdrop memcpy,
  // palette upload, then items in draw order.
  std::memcpy(fb.pixels(), backdrop.pixels.data(),
              static_cast<std::size_t>(fb.width()) * fb.height());
  if (backdrop.hasPalette) {
    for (int i = 0; i < palette.size(); ++i) {
      const auto& c = backdrop.palette[static_cast<std::size_t>(i)];
      palette.set(i, {c.r, c.g, c.b, 255});
    }
  }

  // Item layout (OBSERVED): saves -> indices 0..4 at y=31+36i;
  // no saves -> indices 1..4 at y=31+36(i-1) (global indices preserved).
  const int first = ctl.savesExist() ? 0 : 1;
  int maxW = 0;
  for (int i = first; i < kFrontendOptCount; ++i) {
    const auto& text = optStrings[i];
    if (text.empty()) {
      return fail("frontend menu: empty OPT string");
    }
    const int w = measureFtiText(fontBig, text,
                                 kFtiFontBigMissingAdvance);
    if (w > maxW) {
      maxW = w;
    }
  }
  const int xArg = maxW / 2;

  for (int i = first; i < kFrontendOptCount; ++i) {
    const auto& text = optStrings[i];
    const int y = kFrontendItemY0 +
                  kFrontendItemStep * (ctl.savesExist() ? i : i - 1);
    const float scale = ctl.itemScale(xArg, y, i == ctl.selection());
    const int w = measureFtiText(fontBig, text,
                                 kFtiFontBigMissingAdvance);
    const int x = static_cast<int>(
        static_cast<double>(xArg) -
        static_cast<double>(w) * static_cast<double>(scale) * 0.5);
    drawFtiTextScaled(fontBig, text, fb, x, y, scale,
                      kFtiFontBigMissingAdvance);
  }

  // ARROW at the logical mouse position (FUN_004236c0).
  blitFtiSpriteFrame(arrow, fb, ctl.mouseX(), ctl.mouseY());
  return true;
}

} // namespace mdk
