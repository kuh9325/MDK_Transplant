#include "core/options_menu.h"

#include "core/framebuffer.h"
#include "core/frontend_menu.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"

namespace mdk {

OptionsMenuController::OptionsMenuController(const FrontendMachineState& s,
                                             bool devHidden, int skill,
                                             bool settingsDirty)
    : m_(s), selection_(kOptionsEntrySelection), skill_(skill),
      settingsDirty_(settingsDirty), devHidden_(devHidden) {}

OptionsAction OptionsMenuController::consumeAction() {
  const OptionsAction a = action_;
  action_ = OptionsAction::None;
  return a;
}

// The shared FUN_00420eac dispatch, once per direction query. Every
// terminal item emits its semantic action and returns true (the
// original's branch calls the child-screen entry and RETs). Rows 2,3,4
// activated while DAT_005414f4 is set are a no-op in the original —
// they still return true here (bare epilogue at 0x420fd3: frame ends,
// no draw). Row 6 mutates DAT_0054147a in place and latches the dirty
// flag, then falls through (returns false). Row 8 dispatches only
// under the activate query — the LEFT/RIGHT tables bound at
// `cmp eax,7; ja`, so LEFT/RIGHT on OM_QUIT fall through too.
bool OptionsMenuController::dispatch(Query q) {
  switch (selection_) {
  case 0: action_ = OptionsAction::Help; return true;
  case 1: action_ = OptionsAction::Sound; return true;
  case 2:
    if (!devHidden_) action_ = OptionsAction::Joystick;
    return true;
  case 3:
    if (!devHidden_) action_ = OptionsAction::Mouse;
    return true;
  case 4:
    if (!devHidden_) action_ = OptionsAction::Keyboard;
    return true;
  case 5: action_ = OptionsAction::Performance; return true;
  case 6:
    // 0x421085 (LEFT): `dec; jl -> =2`. 0x421131 (RIGHT) / 0x4211cb
    // (activate): `inc; cmp 3; jge -> =0`. All three set
    // DAT_00541486=1 and fall through.
    if (q == Query::Left) {
      skill_ -= 1;
      if (skill_ < 0) skill_ = 2;
    } else {
      skill_ += 1;
      if (skill_ > 2) skill_ = 0;
    }
    settingsDirty_ = true;
    action_ = q == Query::Left ? OptionsAction::SkillCyclePrev
                               : OptionsAction::SkillCycleNext;
    return false;
  case 7: action_ = OptionsAction::Display; return true;
  case 8:
    if (q == Query::Activate) {
      action_ = OptionsAction::Back;  // 0x42101b -> FUN_00420d68
      return true;
    }
    return false;  // LEFT/RIGHT bound >7 -> next query (OBSERVED)
  default:
    return false;  // out of range: `ja` -> next query / draw
  }
}

void OptionsMenuController::update(const FrontendMenuInput& in) {
  endedEarly_ = false;

  // FUN_004187e0 accumulate — same shared helper as the root menu.
  frontendMouseAccumulate(m_.mouseX, in.mouseDx, kFrontendMouseMaxX);
  frontendMouseAccumulate(m_.mouseY, in.mouseDy, kFrontendMouseMaxY);

  // DAT_00541518 += DAT_0049b6e8 before the mode handler.
  m_.tick += m_.timing.frameStep;

  // (DAT_00541538 != 0 delegates the whole frame to the Sound screen
  // entry — child screens are deferred, so the flag stays 0 here.)

  // 1. prev query (FUN_004237b4 — DIK_UP held).
  if (frontendRepeatQuery(m_.tick, in.prevHeld, m_.prevDeadline)) {
    selection_ -= 1;
    if (selection_ < 0) {
      selection_ = kOptionsItemCount - 1;  // wraps to 8
    }
    if (devHidden_ && selection_ == 4) {
      selection_ = 1;   // skip hidden rows 2-4 (OBSERVED)
    }
  }

  // 2. next query (FUN_00423838 — DIK_DOWN held).
  if (frontendRepeatQuery(m_.tick, in.nextHeld, m_.nextDeadline)) {
    selection_ += 1;
    if (selection_ >= kOptionsItemCount) {
      selection_ = 0;   // wraps to 0
    }
    if (devHidden_ && selection_ == 2) {
      selection_ = 5;   // skip hidden rows 2-4 (OBSERVED)
    }
  }

  // 3. Mouse hit-test — gated on ANY mouse input this frame; the
  // controller clamps the persistent position to (590,350) inside the
  // gate. band = trunc((y - 23) / 36) — x86 IDIV semantics (C++ integer
  // division truncates toward zero identically). OBSERVED: the band is
  // assigned unconditionally when valid; the "1 < band < 5" clause is
  // a GUARD that reverts to the current selection when the hidden
  // rows are disabled out (DAT_005414f4 != 0).
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    if (m_.mouseX > kFrontendHitClampX) m_.mouseX = kFrontendHitClampX;
    if (m_.mouseY > kFrontendHitClampY) m_.mouseY = kFrontendHitClampY;
    const int band =
        (m_.mouseY - kOptionsHitBandBase) / kOptionsHitBandSize;
    if (band > -1 && band < kOptionsItemCount &&
        !(devHidden_ && band > 1 && band < 5)) {
      selection_ = band;
    }
  }

  // 4. DIK_ESCAPE edge (DAT_0054b570) — jumps to the same case as
  // activating row 8: FUN_00420d68 (leave options, restore the saved
  // palette, re-enter the front-end root). Emitted as Back. OBSERVED:
  // the branch RETs — draw and timing update skipped this frame.
  if (in.cancelEdge) {
    action_ = OptionsAction::Back;
    endedEarly_ = true;
    return;
  }

  // 5. LEFT query (FUN_004238bc — DIK_LEFT held). On skill row 6 this
  // cycles the skill DOWN (DAT_0054147a -1, wraps 0->2) and falls
  // through to the RIGHT query; on row 8 it is a silent fall-through
  // (`cmp eax,7; ja` at 0x420fbe); on rows 0-5,7 it dispatches and the
  // frame ends.
  if (frontendRepeatQuery(m_.tick, in.leftHeld, m_.leftDeadline) &&
      dispatch(Query::Left)) {
    endedEarly_ = true;
    return;
  }

  // 6. RIGHT query (FUN_00423940 — DIK_RIGHT held). On row 6 this
  // cycles the skill UP (+1, wraps 2->0) and falls through to the
  // activate query; row 8 falls through as well (`ja` at 0x4210b2);
  // otherwise it dispatches and ends the frame.
  if (frontendRepeatQuery(m_.tick, in.rightHeld, m_.rightDeadline) &&
      dispatch(Query::Right)) {
    endedEarly_ = true;
    return;
  }

  // 7. Activate query (FUN_00423764): Enter edge, or any-button
  // down-edge while the latch is armed. Latch re-arms when all
  // buttons are released. On row 6 this cycles skill UP and falls
  // through to the draw block; otherwise it dispatches.
  if (in.mouseButtons == 0) {
    m_.buttonLatch = true;
  }
  bool fire = in.confirmEdge;
  if (!fire && m_.buttonLatch && in.mouseButtons != 0) {
    fire = true;
  }
  if (fire) {
    m_.buttonLatch = false;
    // OBSERVED: a terminal dispatch RETs before the draw block; the
    // row-6 skill cycle falls through to the draw instead.
    endedEarly_ = dispatch(Query::Activate);
  }

  // 8. Draw: handled by the renderer (clear -> labels -> ARROW ->
  // present -> FUN_0042fe78 timing update in endFrame()).
}

float OptionsMenuController::itemScale(int itemY, bool selFlag) {
  // FUN_00423a24 keyed (-1, itemY) — FUN_00423b88 passes EDX=-1.
  return frontendRampScale(m_.ramp, kOptionsRampKeyX, itemY, selFlag,
                           m_.timing.smoothed);
}

void OptionsMenuController::endFrame(double dtMs) {
  // FUN_0042fe78 -> FUN_0042fcd0 — the shared timing update.
  frontendTimingUpdate(m_.timing, dtMs);
}

// ---------------------------------------------------------------------------

static bool optionsMenuFail(std::string* err, const char* msg) {
  if (err) {
    *err = msg;
  }
  return false;
}

// Shared draw pass for both renderers: palette bind, clear, rows,
// arrow. `scaleFor(i)` yields the row's scale (static spec endpoints
// or the live ramp machine).
static bool drawOptionsFrame(IndexedFramebuffer& fb, Palette& palette,
                             const FtiFont& fontBig,
                             const FtiSpriteFrame& arrow,
                             const OptionsMenuLabels& labels,
                             std::span<const std::byte> sysPalHead,
                             bool devHidden, int arrowX, int arrowY,
                             float (*scaleFor)(void*, int),
                             void* scaleCtx, std::string* err) {
  if (fb.width() != kOptionsCenterWidth || fb.height() != 360) {
    return optionsMenuFail(
        err, "options menu: framebuffer is not the 600x360 work size");
  }
  if (sysPalHead.size() < 192) {
    return optionsMenuFail(
        err, "options menu: SYS_PAL head needs 192 bytes");
  }

  // Palette upload (FUN_0046d208(0,0x100,DAT_00540820) at entry):
  // SYS_PAL head entries 0-63 + zeroed tail 64-255 — see header docs.
  for (int i = 0; i < 64; ++i) {
    palette.set(i, {static_cast<std::uint8_t>(sysPalHead[i * 3 + 0]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 1]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 2]),
                    255});
  }
  for (int i = 64; i < palette.size(); ++i) {
    palette.set(i, {0, 0, 0, 255});
  }

  // Step 1 (OBSERVED): FUN_00415658 -> FUN_0047d20a zero-fill — the
  // whole framebuffer clears to index 0 (SYS_PAL[0] = black). No
  // backdrop is drawn on this screen.
  fb.clear(0);

  // Step 2: rows 0..8 in draw order (FUN_00420df8 each): skip rows
  // 2,3,4 when DAT_005414f4 is set; the index space is NOT compacted.
  for (int i = 0; i < kOptionsItemCount; ++i) {
    if (devHidden && i >= 2 && i <= 4) {
      continue;
    }
    const std::string_view text = labels.items[i];
    if (text.empty()) {
      return optionsMenuFail(
          err, "options menu: empty OM_* label for a drawn row");
    }
    const int y = kOptionsItemY0 + kOptionsItemStep * i;
    const float scale = scaleFor(scaleCtx, i);
    // FUN_0041518c: x = trunc((600 - w*scale) * 0.5) — x87 truncation
    // toward zero; measure via FUN_00414be8 (missing advance 6).
    const int w =
        measureFtiText(fontBig, text, kFtiFontBigMissingAdvance);
    const int x = static_cast<int>(
        (static_cast<double>(kOptionsCenterWidth) -
         static_cast<double>(w) * static_cast<double>(scale)) *
        0.5);
    drawFtiTextScaled(fontBig, text, fb, x, y, scale,
                      kFtiFontBigMissingAdvance);
  }

  // Step 3: ARROW at the raw logical mouse position (FUN_004236c0).
  blitFtiSpriteFrame(arrow, fb, arrowX, arrowY);
  return true;
}

bool renderOptionsMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontBig,
                            const FtiSpriteFrame& arrow,
                            const OptionsMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            const OptionsMenuSpec& spec,
                            std::string* err) {
  struct Ctx {
    int selection;
  } ctx{spec.selection};
  auto scaleFor = [](void* p, int i) -> float {
    const auto* c = static_cast<const Ctx*>(p);
    return i == c->selection ? kFrontendScaleSelected
                             : kFrontendScaleUnselected;
  };
  return drawOptionsFrame(fb, palette, fontBig, arrow, labels,
                          sysPalHead, spec.devHidden, spec.arrowX,
                          spec.arrowY, scaleFor, &ctx, err);
}

bool renderOptionsMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                              const FtiFont& fontBig,
                              const FtiSpriteFrame& arrow,
                              const OptionsMenuLabels& labels,
                              std::span<const std::byte> sysPalHead,
                              OptionsMenuController& ctl,
                              std::string* err) {
  struct Ctx {
    OptionsMenuController* ctl;
  } ctx{&ctl};
  auto scaleFor = [](void* p, int i) -> float {
    auto* c = static_cast<Ctx*>(p);
    const int y = kOptionsItemY0 + kOptionsItemStep * i;
    // FUN_00420df8 -> FUN_00423b88 -> FUN_00423a24 keyed (-1, y);
    // called once per drawn row in draw order.
    return c->ctl->itemScale(y, i == c->ctl->selection());
  };
  return drawOptionsFrame(fb, palette, fontBig, arrow, labels,
                          sysPalHead, ctl.devHidden(), ctl.mouseX(),
                          ctl.mouseY(), scaleFor, &ctx, err);
}

} // namespace mdk
