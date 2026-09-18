#include "core/mouse_menu.h"

#include "core/framebuffer.h"
#include "core/frontend_menu.h" // FrontendMenuInput
#include "core/frontend_palette.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"

#include <bit>
#include <cmath>
#include <limits>

namespace mdk {

MouseMenuController::MouseMenuController(
    const FrontendMachineState& s, bool mouseOn,
    std::uint32_t mouseYReversedBits, std::string_view axesMap,
    const std::array<std::uint32_t, kMouseButtonCount>& buttMap,
    const std::array<float, kMouseAxisCount>& scales,
    bool settingsDirty)
    : m_(s), mouseOn_(mouseOn), mouseYRevBits_(mouseYReversedBits),
      axesMap_(axesMap), buttMap_(buttMap), scales_(scales),
      settingsDirty_(settingsDirty) {
  // FUN_00421664 (OBSERVED): DAT_00541493 = 4 handled by the flow;
  // DAT_0054bd40/38 reset to 0 (member initializers); DAT_0054bd3c
  // = 3 / DAT_0054bd44 = 4 are the compile-time constants.
}

char MouseMenuController::axisMapChar(int axis) const {
  // The original reads the byte at DAT_005413de+i, advancing the
  // cursor only while bytes are nonzero — a NUL or short map makes
  // later axes re-read the same NUL slot (displayed as JOY_A0).
  // (0x421b82-0x421b91 — cursor stays on the NUL.)
  if (axis < 0) return kMouseAxisOff;
  for (int i = 0; i < kMouseAxisCount && i <= axis; ++i) {
    if (i >= static_cast<int>(axesMap_.size()) || axesMap_[i] == '\0') {
      return kMouseAxisOff;
    }
  }
  return axesMap_[axis];
}

float MouseMenuController::axisValue(int axis) const {
  // FILD(delta)/FDIV(scale) then FCOMP against 1.0/-1.0 — the JNC
  // sequence makes NaN and -inf land -1.0, +inf land 1.0.
  if (axis < 0 || axis >= kMouseAxisCount) return -1.0f;
  const float v = static_cast<float>(lastDelta_[axis]) / scales_[axis];
  if (v > 1.0f) return 1.0f;
  if (!(v >= -1.0f)) return -1.0f;  // NaN -> -1.0 like the original
  return v;
}

namespace {

// FISTP of an out-of-int-range float yields 0x80000000 (INT_MIN);
// the original's CMP clamps then send it to -50 (OBSERVED — a
// zero scale therefore pins the marker at -50, never +50).
int testMarkerComponent(int delta, float scale) {
  const float q = (kMouseTestScale * static_cast<float>(delta)) / scale;
  int v;
  if (!std::isfinite(q) || q >= 2147483648.0f || q < -2147483648.0f) {
    v = std::numeric_limits<int>::min();
  } else {
    v = static_cast<int>(std::floor(q));
  }
  if (v > kMouseTestClamp) return kMouseTestClamp;
  if (v < -kMouseTestClamp) return -kMouseTestClamp;
  return v;
}

} // namespace

int MouseMenuController::testOffsetX() const {
  return testMarkerComponent(lastDelta_[0], scales_[0]);
}

int MouseMenuController::testOffsetY() const {
  return testMarkerComponent(lastDelta_[1], scales_[1]);
}

void MouseMenuController::update(const FrontendMenuInput& in) {
  endedEarly_ = false;

  // FUN_004187e0 accumulate + DAT_00541518 += DAT_0049b6e8 — the
  // shared main-loop prologue, identical to the options screen.
  frontendMouseAccumulate(m_.mouseX, in.mouseDx, kFrontendMouseMaxX);
  frontendMouseAccumulate(m_.mouseY, in.mouseDy, kFrontendMouseMaxY);
  m_.tick += m_.timing.frameStep;

  // The draw reads the raw per-frame delta/button globals —
  // captured before any query can exit the frame.
  lastDelta_ = {in.mouseDx, in.mouseDy, in.mouseDz};
  lastButtons_ = in.mouseButtons;

  // 0. DIK_ESCAPE FIRST (DAT_0054b570 raw level, 0x4217f3):
  // DAT_00541493 = 0x0b + RET — the ONLY check before prev; the
  // frame ends before the draw with no other side effect.
  if (in.cancelEdge) {
    action_ = MouseAction::Back;
    endedEarly_ = true;
    return;
  }

  // 1. prev query (FUN_004237b4): sel-1, wraps <0 -> bd3c+0x13-1
  // (=22) at 0x421c30.
  if (frontendRepeatQuery(m_.tick, in.prevHeld, m_.prevDeadline)) {
    selection_ -= 1;
    if (selection_ < 0) {
      selection_ = kMouseRowTotal - 1;
    }
  }

  // 2. next query (FUN_00423838): sel+1, wraps >=bd3c+0x13 (=23)
  // -> 0 (0x42184d).
  if (frontendRepeatQuery(m_.tick, in.nextHeld, m_.nextDeadline)) {
    selection_ += 1;
    if (selection_ >= kMouseRowTotal) {
      selection_ = 0;
    }
  }

  // 3. Mouse hit-test (0x421854-0x421ce4): the same three-global
  // gate (dx/dy/buttons — DAT_0054b64c is not part of it), clamp
  // to (590,350) inside. Left column x<250: trunc((y-2)/16) ->
  // rows 0-3, else trunc((y-259)/16) -> sel band2+4 (axis rows);
  // right column: trunc((y-33)/16) -> sel band+7 AND
  // col = clamp(trunc((x-396)/16), 0, cols-1) — but only when the
  // band is valid.
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    if (m_.mouseX > kFrontendHitClampX) m_.mouseX = kFrontendHitClampX;
    if (m_.mouseY > kFrontendHitClampY) m_.mouseY = kFrontendHitClampY;
    if (m_.mouseX < kMouseHitSplitX) {
      const int band =
          (m_.mouseY - kMouseHitLeftBase) / kMouseHitBandSize;
      if (band >= 0 && band < 4) {
        selection_ = band;
      } else {
        const int band2 =
            (m_.mouseY - kMouseHitAxisBase) / kMouseHitBandSize;
        if (band2 >= 0 && band2 < kMouseAxisCount) {
          selection_ = band2 + 4;
        }
      }
    } else {
      const int band =
          (m_.mouseY - kMouseHitGridBase) / kMouseHitBandSize;
      if (band >= 0 && band < kMouseGridRows) {
        selection_ = band + kMouseGridSelBase;
        int col = (m_.mouseX - kMouseHitColBase) / kMouseHitBandSize;
        if (col >= kMouseButtonCount) col = kMouseButtonCount - 1;
        if (col < 0) col = 0;
        column_ = col;
      }
    }
  }

  // 4. LEFT query (FUN_004238bc): rows 1/2 toggle their setting
  // (0x421ceb/0x421d17 — dirty=1 both ways); rows 4-6 cycle the
  // axis letter -1 (FUN_004216a0); rows 7-22 move the column left,
  // wrapping <0 -> cols-1 (0x421d53). Rows 0/3 fall through.
  if (frontendRepeatQuery(m_.tick, in.leftHeld, m_.leftDeadline)) {
    if (selection_ == 1) {
      mouseOn_ = !mouseOn_;
      settingsDirty_ = true;
    } else if (selection_ == 2) {
      mouseYRevBits_ = mouseYRevBits_ != 0 ? 0u : 1u;
      settingsDirty_ = true;
    }
    if (selection_ >= 4 && selection_ < kMouseGridSelBase) {
      cycleAxisLetter(selection_ - 4, -1);
    }
    if (selection_ >= kMouseGridSelBase &&
        selection_ < kMouseRowTotal) {
      column_ -= 1;
      if (column_ < 0) column_ = kMouseButtonCount - 1;
    }
  }

  // 5. RIGHT query (FUN_00423940): same row 1/2 toggles
  // (0x421d65/0x421d91); rows 4-6 cycle +1; rows 7-22 move the
  // column right, wrapping >=cols -> 0 (0x421d3? — 0x4219d3).
  if (frontendRepeatQuery(m_.tick, in.rightHeld, m_.rightDeadline)) {
    if (selection_ == 1) {
      mouseOn_ = !mouseOn_;
      settingsDirty_ = true;
    } else if (selection_ == 2) {
      mouseYRevBits_ = mouseYRevBits_ != 0 ? 0u : 1u;
      settingsDirty_ = true;
    }
    if (selection_ >= 4 && selection_ < kMouseGridSelBase) {
      cycleAxisLetter(selection_ - 4, +1);
    }
    if (selection_ >= kMouseGridSelBase &&
        selection_ < kMouseRowTotal) {
      column_ += 1;
      if (column_ >= kMouseButtonCount) column_ = 0;
    }
  }

  // 6. Activate query (FUN_00423764): Enter edge or any-button
  // down-edge while the latch is armed. Jump table 0x4217d8:
  // row 0 -> no-op (falls to the range checks, then draw);
  // row 1 -> MouseOn toggle (0x421dbc); row 2 -> MouseYReversed
  // toggle (0x421df4); row 3 -> mode 0x0b + RET (0x4217fc);
  // rows 4-6 -> FUN_004216a0 +1; rows 7-22 -> FUN_00421774.
  if (in.mouseButtons == 0) {
    m_.buttonLatch = true;
  }
  bool fire = in.confirmEdge;
  if (!fire && m_.buttonLatch && in.mouseButtons != 0) {
    fire = true;
  }
  if (fire) {
    m_.buttonLatch = false;
    if (selection_ == 3) {
      action_ = MouseAction::Back;
      endedEarly_ = true;
      return;
    }
    if (selection_ == 1) {
      mouseOn_ = !mouseOn_;
      settingsDirty_ = true;
    } else if (selection_ == 2) {
      mouseYRevBits_ = mouseYRevBits_ != 0 ? 0u : 1u;
      settingsDirty_ = true;
    }
    if (selection_ >= 4 && selection_ < kMouseGridSelBase) {
      cycleAxisLetter(selection_ - 4, +1);
    }
    if (selection_ >= kMouseGridSelBase &&
        selection_ < kMouseRowTotal) {
      toggleButtonBit(selection_ - kMouseGridSelBase, column_);
    }
  }

  // 7. Draw: handled by the renderer (clear -> left rows -> grid ->
  // axis rows -> test indicator -> ARROW -> FUN_0042fe78 timing
  // in endFrame()).
}

// FUN_004216a0 (OBSERVED): cycle the map byte at index `axis` by
// `delta` (+1/-1) inside the '0'+'A'..'H' domain. If the map is
// shorter than the index the original resets the whole map to the
// literal "0" and re-runs — which LOOPS FOREVER when the index
// still exceeds the repaired length; the port applies the repair
// once and skips the cycle (NATIVE hardening of a proven hang).
void MouseMenuController::cycleAxisLetter(int axis, int delta) {
  if (axis < 0) return;
  if (axis >= static_cast<int>(axesMap_.size())) {
    axesMap_ = kMouseAxesMapRepair;
    if (axis >= static_cast<int>(axesMap_.size())) return;
  }
  char c = axesMap_[axis];
  char next;
  if (delta < 0) {
    if (c == kMouseAxisOff) {
      next = kMouseAxisLetterMax;               // '0' -> 'H'
    } else {
      next = static_cast<char>(c - 1);
      if (next < kMouseAxisLetterMin || next > kMouseAxisLetterMax) {
        next = kMouseAxisOff;                   // out of domain -> '0'
      }
    }
  } else {
    if (c == kMouseAxisOff) {
      next = kMouseAxisLetterMin;               // '0' -> 'A'
    } else {
      next = static_cast<char>(c + 1);
      if (next < kMouseAxisLetterMin || next > kMouseAxisLetterMax) {
        next = kMouseAxisOff;                   // 'H'+1 -> '0'
      }
    }
  }
  axesMap_[axis] = next;
  settingsDirty_ = true;
}

// FUN_00421774 (OBSERVED): toggle bit `row` of buttMap[col]
// through the exclusive-group table — bit set -> clear it; bit
// clear -> (mask & ~group[row]) | bit. dirty=1 either way.
void MouseMenuController::toggleButtonBit(int row, int col) {
  if (row < 0 || row >= kMouseGridRows || col < 0 ||
      col >= kMouseButtonCount) {
    return;
  }
  const std::uint32_t bit = 1u << row;
  std::uint32_t& mask = buttMap_[col];
  if ((mask & bit) != 0) {
    mask &= ~bit;
  } else {
    mask = (mask & ~kMouseExclusiveGroups[row]) | bit;
  }
  settingsDirty_ = true;
}

bool MouseMenuController::advanceBlink() {
  // FUN_00414b28: DAT_0049a770 = floor(acc + DAT_0049b6f0); the
  // draw phase is bit 3 of the new accumulator.
  m_.markerAcc =
      static_cast<int>(std::floor(m_.markerAcc + m_.timing.smoothed));
  return (m_.markerAcc & 0x8) != 0;
}

void MouseMenuController::endFrame(double dtMs) {
  frontendTimingUpdate(m_.timing, dtMs);
}

MouseAction MouseMenuController::consumeAction() {
  const MouseAction a = action_;
  action_ = MouseAction::None;
  return a;
}

namespace {

// FUN_00416a20 (OBSERVED): inclusive hollow rectangle outline.
void rectOutline(IndexedFramebuffer& fb, int x0, int y0, int x1,
                 int y1, std::uint8_t color) {
  for (int x = x0; x <= x1; ++x) {
    fb.put(x, y0, color);
    fb.put(x, y1, color);
  }
  for (int y = y0; y <= y1; ++y) {
    fb.put(x0, y, color);
    fb.put(x1, y, color);
  }
}

// FUN_00416aa8 (OBSERVED): inclusive solid rectangle fill.
void rectFill(IndexedFramebuffer& fb, int x0, int y0, int x1,
              int y1, std::uint8_t color) {
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      fb.put(x, y, color);
    }
  }
}

// The state the draw pass needs — spec values for the static
// frame, live controller values for the dynamic frame.
struct FrameState {
  int selection;
  int column;
  bool mouseOn;
  bool yReversed;
  std::string_view axesMap;
  const std::array<std::uint32_t, kMouseButtonCount>* buttMap;
  const std::array<float, kMouseAxisCount>* scales;
  const std::array<int, kMouseAxisCount>* deltas;
  std::uint8_t mouseButtons;
  int brightness;
  int arrowX;
  int arrowY;
};

// The blink accumulator lives outside FrameState — static spec
// uses a frozen copy; dynamic advances the controller's global.
struct BlinkState {
  int acc;
  float smoothed;
  MouseMenuController* ctl;  // non-null -> advanceBlink() path
};

// FUN_00414b28 (OBSERVED): the blinking double-outline bracket.
// Clamps x0>=2, x1<=597, y0>=0, y1<=357; acc advances
// floor(acc+smoothed) per call; bit 3 picks which color leads.
void blinkBracket(IndexedFramebuffer& fb, int x0, int y0, int x1,
                  int y1, BlinkState& blink) {
  if (x0 < kMouseBlinkMinX) x0 = kMouseBlinkMinX;
  if (x1 > kMouseBlinkMaxX) x1 = kMouseBlinkMaxX;
  if (y0 < 0) y0 = 0;
  if (y1 > kMouseBlinkMaxY) y1 = kMouseBlinkMaxY;
  bool phase;
  if (blink.ctl) {
    phase = blink.ctl->advanceBlink();
  } else {
    blink.acc = static_cast<int>(std::floor(blink.acc + blink.smoothed));
    phase = (blink.acc & 0x8) != 0;
  }
  const std::uint8_t c1 = phase ? kMouseBlinkA : kMouseBlinkB;
  const std::uint8_t c2 = phase ? kMouseBlinkB : kMouseBlinkA;
  rectOutline(fb, x0 - 1, y0 + 1, x1 + 1, y1 + 1, c1);
  rectOutline(fb, x0 - 2, y0, x1 + 2, y1 + 2, c2);
}

// FUN_00414dd4 (OBSERVED): FONTSML draw; flag=1 runs the
// FUN_00414b28 marker around (penStart, penY-top)-(penEnd,
// penY+bottom). Multi-char default top=14 bottom=2; a single
// mapped char uses its glyph's own top/bottom.
void drawFlaggedText(const FtiFont& font, std::string_view text,
                     IndexedFramebuffer& fb, int x, int penY,
                     bool flagged, BlinkState& blink) {
  const int end =
      drawFtiText(font, text, fb, x, penY, kFtiFontSmlMissingAdvance);
  if (!flagged) return;
  int top = kMouseTextTop, bottom = kMouseTextBottom;
  if (text.size() == 1) {
    if (const auto* g = font.glyphFor(
            static_cast<std::uint8_t>(text[0]))) {
      top = g->top;
      bottom = g->bottom;
    }
  }
  blinkBracket(fb, x, penY - top, end, penY + bottom, blink);
}

// The display letter for axis i — same NUL semantics as the
// controller: the cursor stops at the first NUL, so every later
// axis resolves JOY_A0.
char displayAxisChar(std::string_view axesMap, int axis) {
  for (int i = 0; i <= axis; ++i) {
    if (i >= static_cast<int>(axesMap.size()) || axesMap[i] == '\0') {
      return kMouseAxisOff;
    }
  }
  return axesMap[axis];
}

// Axis letter -> axisNames index: 'A'..'H' -> 1..8, anything else
// -> 0 (JOY_A0 "Off") — the sprintf("JOY_A%c") repair the original
// performs at 0x421b91-0x421b9f.
int axisNameIndex(char letter) {
  if (letter >= kMouseAxisLetterMin && letter <= kMouseAxisLetterMax) {
    return letter - kMouseAxisLetterMin + 1;
  }
  return 0;
}

// FUN_004213e8 (OBSERVED): left row — resolve text, measure, draw
// at x = (300-w)>>1, y = row*16+16, flag = (row == selection).
void drawLeftRow(const FtiFont& font, IndexedFramebuffer& fb,
                 std::string_view text, int row, int selection,
                 BlinkState& blink) {
  const int w = measureFtiText(font, text, kFtiFontSmlMissingAdvance);
  const int x = (kMouseLeftCenterX - w) >> 1;  // SAR floor-halving
  const int y = row * kMouseLeftRowStep + kMouseLeftRowY0;
  drawFlaggedText(font, text, fb, x, y, row == selection, blink);
}

// FUN_00421504 (OBSERVED): one grid row — label right-aligned at
// gridX0-w-8, y = row*16+46; per column: filled bit -> solid 6
// (cellX, y-13)-(cellX+13, y-1); clear bit -> hollow 14
// (cellX+1, y-13)-(cellX+13, y-1); active column -> cursor
// outline (cellX, y-14)-(cellX+14, y), color 14 filled / 6 hollow.
void drawGridRow(const FtiFont& font, IndexedFramebuffer& fb,
                 std::string_view label, int row, std::uint32_t cellMask,
                 int activeCol) {
  const int gridX0 = kMouseGridXBase - kMouseButtonCount * kMouseGridCellW / 2;
  const int y = row * kMouseGridRowStep + kMouseGridY0;
  const int w = measureFtiText(font, label, kFtiFontSmlMissingAdvance);
  drawFtiText(font, label, fb, gridX0 - w - kMouseGridLabelGap, y,
              kFtiFontSmlMissingAdvance);
  for (int c = 0; c < kMouseButtonCount; ++c) {
    const int cellX = gridX0 + c * kMouseGridCellW;
    const bool set = (cellMask & (1u << c)) != 0;
    if (set) {
      rectFill(fb, cellX, y - 13, cellX + 13, y - 1, kMouseCellFill);
    } else {
      rectOutline(fb, cellX + 1, y - 13, cellX + 13, y - 1,
                  kMouseCellOutline);
    }
    if (activeCol == c) {
      rectOutline(fb, cellX, y - 14, cellX + 14, y,
                  set ? kMouseCursorFilled : kMouseCursorHollow);
    }
  }
}

// FUN_00421448 (OBSERVED): one axis row at y = 350-(5-i)*16 —
// caption "JOY_AX%d" at x=60 unflagged, action "JOY_A%c" at x=90
// flagged when selected; hollow bar (10, y-11)-(50, y-3) color 14;
// marker fill (pos-2, y-11)-(pos+2, y-3) color 6, pos =
// floor(clamped*20+30).
void drawAxisRow(const FtiFont& font, IndexedFramebuffer& fb,
                 std::string_view caption, std::string_view action,
                 int axisIndex, float clampedValue, int selection,
                 BlinkState& blink) {
  const int y = kMouseAxisRowY0 + axisIndex * kMouseAxisRowStep;
  drawFtiText(font, caption, fb, kMouseAxisCaptionX, y,
              kFtiFontSmlMissingAdvance);
  drawFlaggedText(font, action, fb, kMouseAxisActionX, y,
                  axisIndex + 4 == selection, blink);
  rectOutline(fb, kMouseAxisBarX0, y - kMouseAxisBarTop,
              kMouseAxisBarX1, y - kMouseAxisBarBottom,
              kMouseBarOutline);
  const int pos = static_cast<int>(std::floor(
      clampedValue * kMouseAxisMarkerScale + kMouseAxisMarkerBias));
  rectFill(fb, pos - kMouseAxisMarkerHalf, y - kMouseAxisBarTop,
           pos + kMouseAxisMarkerHalf, y - kMouseAxisBarBottom,
           kMouseMarkerFill);
}

// FUN_004212d0 (OBSERVED): the test indicator — frame outline
// color 2 at (50,110)-(150,210); box outlines color 3 offset by
// the clamped marker components.
void drawTestIndicator(IndexedFramebuffer& fb, int mx, int my) {
  rectOutline(fb, kMouseTestX0, kMouseTestY0, kMouseTestX1,
              kMouseTestY1, kMouseTestFrameColor);
  rectOutline(fb, kMouseTestCenterX - 3 + mx, kMouseTestCenterY - 3 + my,
              kMouseTestCenterX + 3 + mx, kMouseTestCenterY + 3 + my,
              kMouseBoxColor);
  rectOutline(fb, kMouseTestCenterX - 2 + mx, kMouseTestCenterY - 2 + my,
              kMouseTestCenterX + 2 + mx, kMouseTestCenterY + 2 + my,
              kMouseBoxColor);
}

// clamp(delta/scale, -1, 1) — the FCOMP/JNC semantics: NaN and
// -inf land -1.0.
float clampAxisValue(int delta, float scale) {
  const float v = static_cast<float>(delta) / scale;
  if (v > 1.0f) return 1.0f;
  if (!(v >= -1.0f)) return -1.0f;
  return v;
}

// clamp(FISTP(50*delta/scale), -50, 50) — out-of-range quotient ->
// INT_MIN -> -50.
int testMarker(int delta, float scale) {
  return testMarkerComponent(delta, scale);
}

bool drawMouseFrame(IndexedFramebuffer& fb, Palette& palette,
                    const FtiFont& fontSml, const FtiSpriteFrame& arrow,
                    const MouseMenuLabels& labels,
                    std::span<const std::byte> sysPalHead,
                    const FrameState& st, BlinkState& blink,
                    std::string* err) {
  if (fb.width() != 600 || fb.height() != 360) {
    if (err) *err = "mouse frame: framebuffer is not 600x360";
    return false;
  }
  if (sysPalHead.size() < 192) {
    if (err) *err = "mouse frame: SYS_PAL head needs 192 bytes";
    return false;
  }
  for (const auto* l : {&labels.test, &labels.enabled, &labels.disabled,
                        &labels.reversed, &labels.normal, &labels.quit,
                        &labels.buttons}) {
    if (l->empty()) {
      if (err) *err = "mouse frame: empty label string";
      return false;
    }
  }
  for (const auto& a : labels.actions) {
    if (a.empty()) {
      if (err) *err = "mouse frame: empty action label";
      return false;
    }
  }
  for (const auto& n : labels.axisNames) {
    if (n.empty()) {
      if (err) *err = "mouse frame: empty axis-name label";
      return false;
    }
  }
  for (const auto& c : labels.axisCaptions) {
    if (c.empty()) {
      if (err) *err = "mouse frame: empty axis caption";
      return false;
    }
  }

  // The screen performs no palette upload of its own — the bound
  // palette is the options screen's SYS_PAL composition, which the
  // port binds per frame (head + zeroed tail + the DAT_0054147e
  // lift, OBSERVED to apply to every bound entry incl. index 0).
  for (int i = 0; i < 64; ++i) {
    palette.set(i, {static_cast<std::uint8_t>(sysPalHead[i * 3 + 0]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 1]),
                    static_cast<std::uint8_t>(sysPalHead[i * 3 + 2]),
                    255});
  }
  for (int i = 64; i < Palette::size(); ++i) {
    palette.set(i, {0, 0, 0, 255});
  }
  applyFrontendBrightness(palette, st.brightness);

  fb.clear(0);

  // Left rows 0-3 (FUN_004213e8 call order at 0x421a4d-0x421a9b):
  // row1 label depends on MouseOn, row2 on MouseYReversed.
  drawLeftRow(fontSml, fb, labels.test, 0, st.selection, blink);
  drawLeftRow(fontSml, fb,
              st.mouseOn ? labels.enabled : labels.disabled, 1,
              st.selection, blink);
  drawLeftRow(fontSml, fb,
              st.yReversed ? labels.reversed : labels.normal, 2,
              st.selection, blink);
  drawLeftRow(fontSml, fb, labels.quit, 3, st.selection, blink);

  // Grid header row -1: cellMask is the live button nibble.
  drawGridRow(fontSml, fb, labels.buttons, kMouseGridHeaderRow,
              st.mouseButtons, -1);

  // Grid rows 0-15: cellMask bit c = (buttMap[c] >> row) & 1;
  // activeCol = column only when this row is selected.
  for (int r = 0; r < kMouseGridRows; ++r) {
    std::uint32_t cellMask = 0;
    for (int c = 0; c < kMouseButtonCount; ++c) {
      if (((*st.buttMap)[c] >> r) & 1u) cellMask |= (1u << c);
    }
    const int activeCol =
        st.selection == r + kMouseGridSelBase ? st.column : -1;
    drawGridRow(fontSml, fb, labels.actions[r], r, cellMask, activeCol);
  }

  // Axis rows 0-2 (FUN_00421448): caption + action + bar/marker.
  for (int i = 0; i < kMouseAxisCount; ++i) {
    const char letter = displayAxisChar(st.axesMap, i);
    const auto& action = labels.axisNames[axisNameIndex(letter)];
    const float value = clampAxisValue((*st.deltas)[i], (*st.scales)[i]);
    drawAxisRow(fontSml, fb, labels.axisCaptions[i], action, i, value,
                st.selection, blink);
  }

  // Test indicator (FUN_004212d0).
  const int mx = testMarker((*st.deltas)[0], (*st.scales)[0]);
  const int my = testMarker((*st.deltas)[1], (*st.scales)[1]);
  drawTestIndicator(fb, mx, my);

  blitFtiSpriteFrame(arrow, fb, st.arrowX, st.arrowY);
  return true;
}

} // namespace

bool renderMouseMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                          const FtiFont& fontSml,
                          const FtiSpriteFrame& arrow,
                          const MouseMenuLabels& labels,
                          std::span<const std::byte> sysPalHead,
                          const MouseMenuSpec& spec, std::string* err) {
  FrameState st{spec.selection,
                spec.column,
                spec.mouseOn,
                spec.mouseYReversedBits != 0,
                spec.axesMap,
                &spec.buttMap,
                &spec.scales,
                &spec.deltas,
                spec.mouseButtons,
                spec.brightness,
                spec.arrowX,
                spec.arrowY};
  // The static spec freezes DAT_0049a770 at 0 and DAT_0049b6f0 at
  // its power-on 1.0 — the first flagged draw advances to 1,
  // bit-3 clear.
  BlinkState blink{0, 1.0f, nullptr};
  return drawMouseFrame(fb, palette, fontSml, arrow, labels, sysPalHead,
                        st, blink, err);
}

bool renderMouseMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontSml,
                            const FtiSpriteFrame& arrow,
                            const MouseMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            MouseMenuController& ctl, int brightness,
                            std::string* err) {
  const std::array<int, kMouseAxisCount> deltas{
      ctl.lastDelta(0), ctl.lastDelta(1), ctl.lastDelta(2)};
  FrameState st{ctl.selection(),
                ctl.column(),
                ctl.mouseOn(),
                ctl.mouseYReversed(),
                ctl.axesMap(),
                &ctl.buttMap(),
                &ctl.scales(),
                &deltas,
                ctl.lastButtons(),
                brightness,
                ctl.mouseX(),
                ctl.mouseY()};
  BlinkState blink{0, ctl.smoothedDelta(), &ctl};
  return drawMouseFrame(fb, palette, fontSml, arrow, labels, sysPalHead,
                        st, blink, err);
}

} // namespace mdk
