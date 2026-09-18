#include "core/frontend_menu.h"

#include "core/compat.h"
#include "core/framebuffer.h"
#include "core/frontend_palette.h"
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
  // 256-entry embedded palette, then the DAT_0054147e brightness
  // lift that every FUN_0046d208 upload applies).
  if (backdrop.hasPalette) {
    for (int i = 0; i < palette.size(); ++i) {
      const auto& c = backdrop.palette[static_cast<std::size_t>(i)];
      palette.set(i, {c.r, c.g, c.b, 255});
    }
  }
  applyFrontendBrightness(palette, spec.brightness);

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

FrontendMachineState FrontendMenuController::machineState() const {
  FrontendMachineState s;
  s.mouseX = mouseX_;
  s.mouseY = mouseY_;
  s.tick = tick_;
  s.prevDeadline = prevDeadline_;
  s.nextDeadline = nextDeadline_;
  s.leftDeadline = leftDeadline_;
  s.rightDeadline = rightDeadline_;
  s.buttonLatch = buttonLatch_;
  s.ramp = ramp_;
  s.timing = timing_;
  return s;
}

void FrontendMenuController::setMachineState(
    const FrontendMachineState& s) {
  mouseX_ = s.mouseX;
  mouseY_ = s.mouseY;
  tick_ = s.tick;
  prevDeadline_ = s.prevDeadline;
  nextDeadline_ = s.nextDeadline;
  leftDeadline_ = s.leftDeadline;
  rightDeadline_ = s.rightDeadline;
  buttonLatch_ = s.buttonLatch;
  ramp_ = s.ramp;
  timing_ = s.timing;
}

void FrontendMenuController::update(const FrontendMenuInput& in) {
  endedEarly_ = false;

  // FUN_004187e0: accumulate raw deltas and clamp to the 600x360 work
  // surface. The accumulate is skipped when the delta is zero.
  frontendMouseAccumulate(mouseX_, in.mouseDx, kFrontendMouseMaxX);
  frontendMouseAccumulate(mouseY_, in.mouseDy, kFrontendMouseMaxY);

  // Main loop: DAT_00541518 += DAT_0049b6e8 before the mode handler.
  tick_ += timing_.frameStep;

  // DAT_0049aa98 is the item-list state; the stable root list is 0.
  // On any selection change the original resets DAT_0049aaa4 to 0, or
  // 999.0f when list state is 1 (transition — unreachable here).
  const float idleReset = (listState() == 1) ? 999.0f : 0.0f;

  // 1. prev query (FUN_004237b4 — DIK_UP held).
  if (frontendRepeatQuery(tick_, in.prevHeld, prevDeadline_)) {
    idleSeconds_ = idleReset;
    selection_ -= 1;
    if (selection_ < 0 || (selection_ == 0 && !savesExist_)) {
      selection_ = kFrontendOptCount - 1;  // wraps to bottom (4)
    }
  }

  // 2. next query (FUN_00423838 — DIK_DOWN held).
  if (frontendRepeatQuery(tick_, in.nextHeld, nextDeadline_)) {
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
    // OBSERVED: every branch RETs immediately — the idle accumulate,
    // attract check, draw block, and timing update are all skipped on
    // the dispatch frame.
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
    endedEarly_ = true;
    return;
  }

  // 5. Idle/attract timer: DAT_0049aaa4 += DAT_0049b6f4 each frame.
  idleSeconds_ += timing_.deltaSec;

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
  // FUN_00423a24 verbatim (shared with the options sub-menu — see
  // frontend_machines.h). The (centerX,itemY) pair is the item key.
  return frontendRampScale(ramp_, centerX, itemY, selFlag,
                           timing_.smoothed);
}

void FrontendMenuController::endFrame(double dtMs) {
  // FUN_0042fb68/FUN_0042fcd0 timing update (shared helper — see
  // frontend_machines.h).
  frontendTimingUpdate(timing_, dtMs);
}

bool renderFrontendMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                               const IndexedImage& backdrop,
                               const FtiFont& fontBig,
                               const FtiSpriteFrame& arrow,
                               std::span<const std::string_view> optStrings,
                               FrontendMenuController& ctl,
                               int brightness,
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
  // Same FUN_0046d208 upload lift as the static path (DAT_0054147e).
  applyFrontendBrightness(palette, brightness);

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
