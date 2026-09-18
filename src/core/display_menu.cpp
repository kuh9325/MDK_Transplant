#include "core/display_menu.h"

#include "core/framebuffer.h"
#include "core/frontend_menu.h"
#include "core/frontend_palette.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"

#include <cstdio>

namespace mdk {

DisplayMenuController::DisplayMenuController(
    const FrontendMachineState& s, int brightness, bool forcePCorrect,
    bool settingsDirty)
    : m_(s), selection_(kDisplayEntrySelection),
      brightness_(brightness), forcePCorrect_(forcePCorrect),
      settingsDirty_(settingsDirty) {}

DisplayAction DisplayMenuController::consumeAction() {
  const DisplayAction a = action_;
  action_ = DisplayAction::None;
  return a;
}

void DisplayMenuController::update(const FrontendMenuInput& in) {
  endedEarly_ = false;

  // FUN_004187e0 accumulate + DAT_00541518 += DAT_0049b6e8 — the
  // shared main-loop prologue, identical to the options screen.
  frontendMouseAccumulate(m_.mouseX, in.mouseDx, kFrontendMouseMaxX);
  frontendMouseAccumulate(m_.mouseY, in.mouseDy, kFrontendMouseMaxY);
  m_.tick += m_.timing.frameStep;

  // 1. prev query (FUN_004237b4): dec, wraps <0 -> 2 (0x41d43b).
  if (frontendRepeatQuery(m_.tick, in.prevHeld, m_.prevDeadline)) {
    selection_ -= 1;
    if (selection_ < 0) {
      selection_ = kDisplayItemCount - 1;
    }
  }

  // 2. next query (FUN_00423838): inc, wraps >=3 -> 0 (0x41d224).
  if (frontendRepeatQuery(m_.tick, in.nextHeld, m_.nextDeadline)) {
    selection_ += 1;
    if (selection_ >= kDisplayItemCount) {
      selection_ = 0;
    }
  }

  // 3. Mouse hit-test — the same three-global gate as Options
  // (DAT_0054b644/648/640); the controller clamps to (590,350)
  // inside the gate. band = trunc((mouseY - 5) / 36) — x86 IDIV
  // semantics; a valid band is assigned unconditionally.
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    if (m_.mouseX > kFrontendHitClampX) m_.mouseX = kFrontendHitClampX;
    if (m_.mouseY > kFrontendHitClampY) m_.mouseY = kFrontendHitClampY;
    const int band =
        (m_.mouseY - kDisplayHitBandBase) / kDisplayHitBandSize;
    if (band > -1 && band < kDisplayItemCount) {
      selection_ = band;
    }
  }

  // 4. DIK_ESCAPE (DAT_0054b570) -> FUN_0041d144 + RET (0x41d469):
  // restore the saved palette, mode 0x0b. Frame ends before the draw.
  if (in.cancelEdge) {
    action_ = DisplayAction::Back;
    endedEarly_ = true;
    return;
  }

  // 5. LEFT query (FUN_004238bc): row 0 -> brightness -1 wrapping
  // <0 -> 7 (0x41d478); row 1 -> toggle DAT_00541482 (0x41d487);
  // row 2 -> nothing (0x41d48a `jnz` falls to the RIGHT query).
  // EVERY branch continues to the RIGHT query — LEFT never ends
  // the frame.
  if (frontendRepeatQuery(m_.tick, in.leftHeld, m_.leftDeadline)) {
    if (selection_ == 0) {
      brightness_ -= 1;
      if (brightness_ < 0) {
        brightness_ = kDisplayBrightnessMax;
      }
      settingsDirty_ = true;
      // (The original re-uploads slut via FUN_0046d208 here — the
      // renderer binds the lifted palette every frame, so the new
      // level shows immediately.)
    } else if (selection_ == 1) {
      forcePCorrect_ = !forcePCorrect_;
      settingsDirty_ = true;
    }
  }

  // 6. RIGHT query (FUN_00423940): row 0 -> brightness +1 wrapping
  // >=8 -> 0 (0x41d306); row 1 -> toggle (0x41d4c5); row 2 falls
  // through to the activate query.
  if (frontendRepeatQuery(m_.tick, in.rightHeld, m_.rightDeadline)) {
    if (selection_ == 0) {
      brightness_ += 1;
      if (brightness_ > kDisplayBrightnessMax) {
        brightness_ = 0;
      }
      settingsDirty_ = true;
    } else if (selection_ == 1) {
      forcePCorrect_ = !forcePCorrect_;
      settingsDirty_ = true;
    }
  }

  // 7. Activate query (FUN_00423764): Enter edge, or any-button
  // down-edge while the latch is armed. Row 0 -> brightness +1
  // (0x41d33f — same inc/wrap as RIGHT); row 1 -> toggle (0x41d4f2);
  // both fall through to the draw. Row 2 -> FUN_0041d144 + RET.
  if (in.mouseButtons == 0) {
    m_.buttonLatch = true;
  }
  bool fire = in.confirmEdge;
  if (!fire && m_.buttonLatch && in.mouseButtons != 0) {
    fire = true;
  }
  if (fire) {
    m_.buttonLatch = false;
    if (selection_ == 0) {
      brightness_ += 1;
      if (brightness_ > kDisplayBrightnessMax) {
        brightness_ = 0;
      }
      settingsDirty_ = true;
    } else if (selection_ == 1) {
      forcePCorrect_ = !forcePCorrect_;
      settingsDirty_ = true;
    } else {
      action_ = DisplayAction::Back;
      endedEarly_ = true;
      return;
    }
  }

  // 8. Draw: handled by the renderer (palette bind -> clear -> rows
  // -> swatch grid -> ARROW -> FUN_0042fe78 timing in endFrame()).
}

float DisplayMenuController::itemScale(int itemY, bool selFlag) {
  // FUN_00423a24 keyed (-1, itemY) — FUN_00423b88 passes EDX=-1.
  return frontendRampScale(m_.ramp, kDisplayRampKeyX, itemY, selFlag,
                           m_.timing.smoothed);
}

void DisplayMenuController::endFrame(double dtMs) {
  // FUN_0042fe78 -> FUN_0042fcd0 — the shared timing update.
  frontendTimingUpdate(m_.timing, dtMs);
}

// ---------------------------------------------------------------------------

static bool displayMenuFail(std::string* err, const char* msg) {
  if (err) {
    *err = msg;
  }
  return false;
}

// Shared draw pass for both renderers: palette bind, clear, rows,
// swatch grid, arrow. `scaleFor(row)` yields the row's scale (static
// spec endpoints or the live ramp machine).
static bool drawDisplayFrame(IndexedFramebuffer& fb, Palette& palette,
                             const FtiFont& fontBig,
                             const FtiSpriteFrame& arrow,
                             const DisplayMenuLabels& labels,
                             std::span<const std::byte> sysPalHead,
                             int brightness, bool forcePCorrect,
                             int arrowX, int arrowY,
                             float (*scaleFor)(void*, int),
                             void* scaleCtx, std::string* err) {
  if (fb.width() != kDisplayCenterWidth || fb.height() != 360) {
    return displayMenuFail(
        err, "display menu: framebuffer is not the 600x360 work size");
  }
  if (sysPalHead.size() < 192) {
    return displayMenuFail(
        err, "display menu: SYS_PAL head needs 192 bytes");
  }
  if (labels.brightnessFmt.empty() || labels.detailHigh.empty() ||
      labels.detailLow.empty() || labels.quit.empty()) {
    return displayMenuFail(
        err, "display menu: empty DSP_* label for a drawn row");
  }

  // Palette upload (FUN_0041d020 -> FUN_00413b40): SYS_PAL head
  // entries 0-63 (DAT_00540820 — the slut head copy is dead: the
  // helper always rebinds the resident SYS_PAL head), then the slut
  // tail: four 48-entry ramps (gray 64-111, red 112-159, green
  // 160-207, blue 208-255), intensity = i*255/47. The FUN_0046d208
  // brightness lift applies to every bound entry at upload.
  for (int i = 0; i < 64; ++i) {
    palette.set(i, {static_cast<std::uint8_t>(sysPalHead[i * 3 + 0]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 1]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 2]),
                    255});
  }
  for (int band = 0; band < kDisplaySwatchBands; ++band) {
    for (int i = 0; i < kDisplaySwatchCells; ++i) {
      const std::uint8_t v = static_cast<std::uint8_t>(
          i * 255 / kDisplaySwatchRampMax);
      Palette::Color c{0, 0, 0, 255};
      switch (band) {
      case 0: c = {v, v, v, 255}; break;  // gray
      case 1: c = {v, 0, 0, 255}; break;  // red
      case 2: c = {0, v, 0, 255}; break;  // green
      default: c = {0, 0, v, 255}; break; // blue
      }
      palette.set(kDisplaySwatchPalBase +
                      band * kDisplaySwatchPalStride + i,
                  c);
    }
  }
  applyFrontendBrightness(palette, brightness);

  // Step 1 (OBSERVED): FUN_00415658 -> FUN_0047d20a zero-fill — the
  // whole framebuffer clears to index 0. No backdrop on this screen.
  fb.clear(0);

  // Step 2: the three rows via FUN_00423b88 (FONTBIG, centered on
  // 600, scale from the (-1, y)-keyed ramp).
  std::string_view texts[kDisplayItemCount];
  // Row 0: sprintf(buf, DSP_BRGT, brightness) — the record text is
  // the format string (FUN_0047d2e9 vsprintf). Bounded into a local
  // scratch (the original's is a stack buffer).
  char fmt[96];
  std::snprintf(fmt, sizeof(fmt), "%.*s",
                static_cast<int>(labels.brightnessFmt.size()),
                labels.brightnessFmt.data());
  char row0[96];
  std::snprintf(row0, sizeof(row0), fmt, brightness);
  texts[0] = row0;
  texts[1] = forcePCorrect ? labels.detailHigh : labels.detailLow;
  texts[2] = labels.quit;
  for (int i = 0; i < kDisplayItemCount; ++i) {
    const int y = kDisplayItemY0 + kDisplayItemStep * i;
    const float scale = scaleFor(scaleCtx, i);
    const int w = measureFtiText(fontBig, texts[i],
                                 kFtiFontBigMissingAdvance);
    const int x = static_cast<int>(
        (static_cast<double>(kDisplayCenterWidth) -
         static_cast<double>(w) * static_cast<double>(scale)) *
        0.5);
    drawFtiTextScaled(fontBig, texts[i], fb, x, y, scale,
                      kFtiFontBigMissingAdvance);
  }

  // Step 3 (OBSERVED): FUN_0041cf80 swatch grid — four bands of 48
  // cells, FUN_00416aa8 inclusive rectfill per cell:
  //   x = 60+10i .. 69+10i (10px), y = 200+32b .. 231+32b (32px),
  //   color index = 64 + 48*band + i.
  for (int band = 0; band < kDisplaySwatchBands; ++band) {
    const std::uint8_t base = static_cast<std::uint8_t>(
        kDisplaySwatchPalBase + band * kDisplaySwatchPalStride);
    const int y0 = kDisplaySwatchY0 + band * kDisplaySwatchYStep;
    const int y1 = kDisplaySwatchY1 + band * kDisplaySwatchYStep;
    for (int i = 0; i < kDisplaySwatchCells; ++i) {
      const int x0 = kDisplaySwatchX0 + i * kDisplaySwatchXStep;
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x0 + (kDisplaySwatchX1 - kDisplaySwatchX0);
             ++x) {
          fb.put(x, y, static_cast<std::uint8_t>(base + i));
        }
      }
    }
  }

  // Step 4: ARROW at the raw logical mouse position (FUN_004236c0).
  blitFtiSpriteFrame(arrow, fb, arrowX, arrowY);
  return true;
}

bool renderDisplayMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontBig,
                            const FtiSpriteFrame& arrow,
                            const DisplayMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            const DisplayMenuSpec& spec,
                            std::string* err) {
  struct Ctx {
    int selection;
  } ctx{spec.selection};
  auto scaleFor = [](void* p, int i) -> float {
    const auto* c = static_cast<const Ctx*>(p);
    return i == c->selection ? kFrontendScaleSelected
                             : kFrontendScaleUnselected;
  };
  return drawDisplayFrame(fb, palette, fontBig, arrow, labels,
                          sysPalHead, spec.brightness,
                          spec.forcePCorrect, spec.arrowX, spec.arrowY,
                          scaleFor, &ctx, err);
}

bool renderDisplayMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                              const FtiFont& fontBig,
                              const FtiSpriteFrame& arrow,
                              const DisplayMenuLabels& labels,
                              std::span<const std::byte> sysPalHead,
                              DisplayMenuController& ctl,
                              std::string* err) {
  struct Ctx {
    DisplayMenuController* ctl;
  } ctx{&ctl};
  auto scaleFor = [](void* p, int i) -> float {
    auto* c = static_cast<Ctx*>(p);
    const int y = kDisplayItemY0 + kDisplayItemStep * i;
    // FUN_00423b88 -> FUN_00423a24 keyed (-1, y); called once per
    // drawn row in draw order.
    return c->ctl->itemScale(y, i == c->ctl->selection());
  };
  return drawDisplayFrame(fb, palette, fontBig, arrow, labels,
                          sysPalHead, ctl.brightness(),
                          ctl.forcePCorrect(), ctl.mouseX(),
                          ctl.mouseY(), scaleFor, &ctx, err);
}

} // namespace mdk
