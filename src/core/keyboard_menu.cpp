#include "core/keyboard_menu.h"

#include "core/framebuffer.h"
#include "core/frontend_menu.h" // FrontendMenuInput
#include "core/frontend_palette.h"
#include "core/fti_font.h"
#include "core/fti_sprite.h"

#include <bit>
#include <cmath>

namespace mdk {

namespace {

// OBSERVED 128-byte extended-DIKey translation table at 0x49bbf0 —
// indexed by (DIK offset & 0x7f) for offsets > 0x7f; unmapped entries
// yield 0x7f, itself a capturable internal code.
constexpr std::array<std::uint8_t, kKeyboardCodeCount> kExtendedDik = {
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x60, 0x61, 0x7f, 0x7f,  // 0x1c/1d: 9c/9d
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x63, 0x7f, 0x64,  // 0x35/37: b5/b7
    0x65, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,  // 0x38: b8
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x66,  // 0x47: c7
    0x67, 0x68, 0x7f, 0x69, 0x7f, 0x6a, 0x7f, 0x6b,  // c8 c9 cb cd cf
    0x6c, 0x6d, 0x6e, 0x6f, 0x7f, 0x7f, 0x7f, 0x7f,  // d0 d1 d2 d3
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x70,  // 0x6f: ef
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
};

// OBSERVED key-glyph tables — internal code -> FONTSML glyph byte.
// English QWERTY @0x49aaa8:
constexpr std::array<std::uint8_t, kKeyboardCodeCount> kGlyphEn = {
    0x00, 0x01, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x30, 0x2d, 0x3d, 0x02, 0x03,
    0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,
    0x4f, 0x50, 0x5b, 0x5d, 0x04, 0x05, 0x41, 0x53,
    0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0x3b,
    0x27, 0x60, 0x06, 0x00, 0x5a, 0x58, 0x43, 0x56,
    0x42, 0x4e, 0x4d, 0x2c, 0x2e, 0x2f, 0x00, 0x2a,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
    0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
    0x1f, 0x80, 0x81, 0x82, 0x00, 0x00, 0x00, 0x83,
    0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x85, 0x00, 0x86, 0x87, 0x86, 0x00, 0x88, 0x89,
    0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91,
    0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
// French AZERTY @0x49ab28:
constexpr std::array<std::uint8_t, kKeyboardCodeCount> kGlyphFr = {
    0x00, 0x01, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x30, 0x29, 0x3d, 0x02, 0x03,
    0x41, 0x5a, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,
    0x4f, 0x50, 0x5e, 0x24, 0x04, 0x05, 0x51, 0x53,
    0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0x4d,
    0x25, 0x32, 0x06, 0x2a, 0x57, 0x58, 0x43, 0x56,
    0x42, 0x4e, 0x2c, 0x3b, 0x3a, 0x21, 0x61, 0x2a,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
    0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
    0x1f, 0x80, 0x81, 0x82, 0x62, 0x63, 0x3c, 0x83,
    0x84, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
    0x85, 0x6c, 0x86, 0x87, 0x86, 0x6d, 0x88, 0x89,
    0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91,
    0x92, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x3f, 0x3f,
};
// German QWERTZ @0x49aba8:
constexpr std::array<std::uint8_t, kKeyboardCodeCount> kGlyphDe = {
    0x00, 0x01, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x30, 0x5c, 0x27, 0x02, 0x03,
    0x51, 0x57, 0x45, 0x52, 0x54, 0x5a, 0x55, 0x49,
    0x4f, 0x50, 0xdc, 0x2b, 0x04, 0x05, 0x41, 0x53,
    0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0xd6,
    0xc4, 0x5e, 0x06, 0x23, 0x59, 0x58, 0x43, 0x56,
    0x42, 0x4e, 0x4d, 0x2c, 0x2e, 0x2d, 0x61, 0x2a,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
    0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
    0x1f, 0x80, 0x81, 0x82, 0x62, 0x63, 0x3c, 0x83,
    0x84, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
    0x85, 0x6c, 0x86, 0x87, 0x86, 0x6d, 0x88, 0x89,
    0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91,
    0x92, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x3f, 0x3f,
};

} // namespace

int internalKeyFromDik(int dik) {
  // FUN_0046b688 (OBSERVED): DIK offsets <= 0x7f index the bitmap
  // directly; > 0x7f translate through table_49bbf0[DIK & 0x7f].
  if (dik < 0) return -1;
  return dik <= 0x7f ? dik : kExtendedDik[dik & 0x7f];
}

int keyboardFirstEdgeBit(const KeyboardEdgeBitmap& edge) {
  // FUN_00419168 (OBSERVED): word 0 scanned first for the lowest
  // set bit, then words 1-3 with +0x20/+0x40/+0x60 offsets; an
  // empty bitmap returns 0 (code 0 can't be produced by the device
  // path, so 0 doubles as "no key").
  for (int w = 0; w < 4; ++w) {
    if (edge[w] != 0) {
      return w * 32 + std::countr_zero(edge[w]);
    }
  }
  return 0;
}

int keyboardPollCapture(const KeyboardEdgeBitmap& edge) {
  // FUN_0041925c (OBSERVED): the first edge bit, then the
  // right-modifier folds — 0x36 (RSHIFT) -> 0x2a, 0x61 (RCTRL's
  // extended code) -> 0x1d, 0x65 (RALT's extended code) -> 0x38.
  const int bit = keyboardFirstEdgeBit(edge);
  if (bit == 0x36) return 0x2a;
  if (bit == 0x61) return 0x1d;
  if (bit == 0x65) return 0x38;
  return bit;
}

const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTableEn() {
  return kGlyphEn;
}
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTableFr() {
  return kGlyphFr;
}
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTableDe() {
  return kGlyphDe;
}
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTable(char langTag) {
  // FUN_0041f068 (OBSERVED): LANG's first byte 'F' -> 0x49ab28,
  // 'G' -> 0x49aba8, anything else (incl. the soft-resolver's 0 on
  // a missing LANG record) -> 0x49aaa8.
  if (langTag == 'F') return kGlyphFr;
  if (langTag == 'G') return kGlyphDe;
  return kGlyphEn;
}

std::array<int, kKeyboardGlobalCount> keyboardGlobalsFromSettings(
    const FrontendSettings& s) {
  // Start from the factory block — the ten hidden hotkey slots
  // (g14..g23) always boot at 2..11 — then overlay the 19 persisted
  // slots in settings-table order (entries 69-87).
  auto g = kKeyboardDefaults;
  const int v[kKeyboardSettingSlots] = {
      s.keyLeft,   s.keyRight,   s.keyUp,       s.keyDown,
      s.keyJump,   s.keySide,    s.keyFire,     s.keySniper,
      s.keyTurbo,  s.keySturbo,  s.keyLookUp,   s.keyLookDown,
      s.keyZoomIn, s.keyZoomOut, s.keyItemNext, s.keyItemPrev,
      s.keyItemUse,s.keySideL,   s.keySideR};
  for (int i = 0; i < kKeyboardSettingSlots; ++i) {
    g[kKeyboardSlotToGlobal[i]] = v[i];
  }
  return g;
}

void keyboardSettingsFromGlobals(
    FrontendSettings& s,
    const std::array<int, kKeyboardGlobalCount>& g) {
  s.keyLeft = g[kKeyboardSlotToGlobal[0]];
  s.keyRight = g[kKeyboardSlotToGlobal[1]];
  s.keyUp = g[kKeyboardSlotToGlobal[2]];
  s.keyDown = g[kKeyboardSlotToGlobal[3]];
  s.keyJump = g[kKeyboardSlotToGlobal[4]];
  s.keySide = g[kKeyboardSlotToGlobal[5]];
  s.keyFire = g[kKeyboardSlotToGlobal[6]];
  s.keySniper = g[kKeyboardSlotToGlobal[7]];
  s.keyTurbo = g[kKeyboardSlotToGlobal[8]];
  s.keySturbo = g[kKeyboardSlotToGlobal[9]];
  s.keyLookUp = g[kKeyboardSlotToGlobal[10]];
  s.keyLookDown = g[kKeyboardSlotToGlobal[11]];
  s.keyZoomIn = g[kKeyboardSlotToGlobal[12]];
  s.keyZoomOut = g[kKeyboardSlotToGlobal[13]];
  s.keyItemNext = g[kKeyboardSlotToGlobal[14]];
  s.keyItemPrev = g[kKeyboardSlotToGlobal[15]];
  s.keyItemUse = g[kKeyboardSlotToGlobal[16]];
  s.keySideL = g[kKeyboardSlotToGlobal[17]];
  s.keySideR = g[kKeyboardSlotToGlobal[18]];
}

KeyboardMenuController::KeyboardMenuController(
    const FrontendMachineState& s,
    const std::array<int, kKeyboardGlobalCount>& keys,
    bool settingsDirty)
    : m_(s), keys_(keys), settingsDirty_(settingsDirty) {
  // FUN_0041f030 (OBSERVED): DAT_00541493 = 5 handled by the flow;
  // DAT_0054bca8 = 0 and DAT_0054bcac = 0x14 are the member
  // initializers (capture clear, selection on the KM_QUIT row).
  // Everything else carries over untouched.
}

void KeyboardMenuController::update(const FrontendMenuInput& in) {
  endedEarly_ = false;

  // FUN_004187e0 accumulate + DAT_00541518 += DAT_0049b6e8 — the
  // main-loop prologue that runs before the mode dispatch, identical
  // to the other screens.
  frontendMouseAccumulate(m_.mouseX, in.mouseDx, kFrontendMouseMaxX);
  frontendMouseAccumulate(m_.mouseY, in.mouseDy, kFrontendMouseMaxY);
  m_.tick += m_.timing.frameStep;

  if (capture_) {
    // Capture state (0x41f194-0x41f513): DIK_ESCAPE edge FIRST —
    // clears the flag and redraws (ESC can never be captured).
    if (in.cancelEdge) {
      capture_ = false;
      return;  // draw follows
    }
    // FUN_0041925c raw-key poll: key==0 -> redraw; key!=0 -> store
    // through the selection's global pointer when it differs, then
    // clear capture and redraw. OBSERVED: no duplicate checks —
    // the code stores verbatim even when already bound elsewhere.
    const int key = keyboardPollCapture(in.rawKeyEdge);
    if (key != 0) {
      if (selection_ >= 0 && selection_ < kKeyboardBindingRows) {
        int* target = &keys_[kKeyboardRowToGlobal[selection_]];
        if (key != *target) {
          settingsDirty_ = true;  // DAT_00541486 = 1
          *target = key;
        }
      }
      // (selection outside 0-18: just clears the flag — OBSERVED.)
      capture_ = false;
    }
    return;  // draw follows
  }

  // 1. prev query = FUN_004237b4 (UP) OR FUN_004238bc (LEFT) —
  // LEFT is queried only when UP didn't fire. sel-1 wraps <0 -> 0x14.
  bool prev = frontendRepeatQuery(m_.tick, in.prevHeld, m_.prevDeadline);
  if (!prev) {
    prev = frontendRepeatQuery(m_.tick, in.leftHeld, m_.leftDeadline);
  }
  if (prev) {
    selection_ -= 1;
    if (selection_ < 0) {
      selection_ = kKeyboardRowTotal - 1;  // 0x14
    }
  }

  // 2. next query = FUN_00423838 (DOWN) OR FUN_00423940 (RIGHT) —
  // RIGHT only when DOWN didn't fire. sel+1 wraps >=0x15 -> 0.
  bool next = frontendRepeatQuery(m_.tick, in.nextHeld, m_.nextDeadline);
  if (!next) {
    next = frontendRepeatQuery(m_.tick, in.rightHeld, m_.rightDeadline);
  }
  if (next) {
    selection_ += 1;
    if (selection_ >= kKeyboardRowTotal) {  // 0x15
      selection_ = 0;
    }
  }

  // 3. Mouse hit-test (0x41f561-0x41f69d): the same three-global gate
  // (dx/dy/buttons — DAT_0054b64c is not part of it), the clamp to
  // (590,350) mutates the shared position globals inside the gate.
  // y>=64: band = trunc((y-50)/30); band>10 -> no select; x>=320
  // shifts +10 (right column); 0<=band<19 -> sel = band. OBSERVED
  // quirk: the left column's band domain reaches 10, so y=350 x<320
  // selects row 10 — a right-column row.
  // y<64: y in [2,18) -> 19 (KM_RESET), [18,34) -> 20 (KM_QUIT);
  // y<2 and y in [34,64) select nothing.
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    if (m_.mouseX > kFrontendHitClampX) m_.mouseX = kFrontendHitClampX;
    if (m_.mouseY > kFrontendHitClampY) m_.mouseY = kFrontendHitClampY;
    const int y = m_.mouseY;
    if (y >= kKeyboardHitRowBandTop) {
      const int band = (y - kKeyboardHitBandBase) / kKeyboardHitBandSize;
      if (band <= kKeyboardHitBandMax) {
        int row = band;
        if (row >= 0 && m_.mouseX >= kKeyboardHitSplitX) {
          row += 10;
        }
        if (row >= 0 && row < kKeyboardBindingRows) {
          selection_ = row;
        }
      }
    } else if (y >= kKeyboardHitResetLo && y < kKeyboardHitResetHi) {
      selection_ = kKeyboardResetRow;
    } else if (y >= kKeyboardHitResetHi && y < kKeyboardHitQuitHi) {
      selection_ = kKeyboardQuitRow;
    }
  }

  // 4. DIK_ESCAPE edge (DAT_0054b570) -> DAT_00541493 = 0x0b + RET —
  // the frame ends before the draw with no other side effect.
  if (in.cancelEdge) {
    action_ = KeyboardAction::Back;
    endedEarly_ = true;
    return;
  }

  // 5. Activate query (FUN_00423764): Enter edge OR any-button
  // down-edge while the latch is armed (re-arms on all-release).
  // sel <19 -> DAT_0054bca8 = 1 (capture); sel ==19 ->
  // FUN_00425db0 + dirty = ECX = 1; sel >=20 -> mode 0x0b + RET.
  if (in.mouseButtons == 0) {
    m_.buttonLatch = true;
  }
  bool fire = in.confirmEdge;
  if (!fire && m_.buttonLatch && in.mouseButtons != 0) {
    fire = true;
  }
  if (fire) {
    m_.buttonLatch = false;
    if (selection_ < kKeyboardResetRow) {
      capture_ = true;
    } else if (selection_ == kKeyboardResetRow) {
      keys_ = kKeyboardDefaults;  // the 29-dword mirror copy
      settingsDirty_ = true;
    } else {
      action_ = KeyboardAction::Back;
      endedEarly_ = true;
      return;
    }
  }
  // 6. Draw block + present + FUN_0042fe78 timing — renderer + endFrame.
}

bool KeyboardMenuController::advanceBlink() {
  // FUN_00414b28: DAT_0049a770 = floor(acc + DAT_0049b6f0); the
  // draw phase is bit 3 of the new accumulator.
  m_.markerAcc =
      static_cast<int>(std::floor(m_.markerAcc + m_.timing.smoothed));
  return (m_.markerAcc & 0x8) != 0;
}

void KeyboardMenuController::endFrame(double dtMs) {
  frontendTimingUpdate(m_.timing, dtMs);
}

KeyboardAction KeyboardMenuController::consumeAction() {
  const KeyboardAction a = action_;
  action_ = KeyboardAction::None;
  return a;
}

namespace {

// The state the draw pass needs — spec values for the static frame,
// live controller values for the dynamic frame.
struct KbFrameState {
  int selection;
  bool capture;
  const std::array<int, kKeyboardGlobalCount>* keys;
  char langTag;
  int brightness;
  int arrowX;
  int arrowY;
};

// The blink accumulator lives outside KbFrameState — the static spec
// freezes it; the dynamic pass advances the controller's global.
struct KbBlink {
  int acc;
  float smoothed;
  KeyboardMenuController* ctl;  // non-null -> advanceBlink() path
};

// FUN_00416a20 (OBSERVED): inclusive hollow rectangle outline.
void kbRectOutline(IndexedFramebuffer& fb, int x0, int y0, int x1,
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

// FUN_00414b28 (OBSERVED): the blinking double-outline bracket —
  // clamps x0>=2, x1<=597, y0>=0, y1<=357; acc advances
// floor(acc+smoothed) per call; bit 3 picks which color leads.
void kbBlinkBracket(IndexedFramebuffer& fb, int x0, int y0, int x1,
                    int y1, KbBlink& blink) {
  if (x0 < kKeyboardBlinkMinX) x0 = kKeyboardBlinkMinX;
  if (x1 > kKeyboardBlinkMaxX) x1 = kKeyboardBlinkMaxX;
  if (y0 < 0) y0 = 0;
  if (y1 > kKeyboardBlinkMaxY) y1 = kKeyboardBlinkMaxY;
  bool phase;
  if (blink.ctl) {
    phase = blink.ctl->advanceBlink();
  } else {
    blink.acc = static_cast<int>(std::floor(blink.acc + blink.smoothed));
    phase = (blink.acc & 0x8) != 0;
  }
  const std::uint8_t c1 = phase ? kKeyboardBlinkA : kKeyboardBlinkB;
  const std::uint8_t c2 = phase ? kKeyboardBlinkB : kKeyboardBlinkA;
  kbRectOutline(fb, x0 - 1, y0 + 1, x1 + 1, y1 + 1, c1);
  kbRectOutline(fb, x0 - 2, y0, x1 + 2, y1 + 2, c2);
}

// FUN_00414dd4 (OBSERVED): FONTSML draw; flag=1 runs the
// FUN_00414b28 marker around (penStart, penY-top)-(penEnd,
// penY+bottom). Multi-char default top=14 bottom=2; a single mapped
// char (the key glyph) uses its glyph's own top/bottom.
void kbDrawFlagged(const FtiFont& font, std::string_view text,
                   IndexedFramebuffer& fb, int x, int penY,
                   bool flagged, KbBlink& blink) {
  const int end =
      drawFtiText(font, text, fb, x, penY, kFtiFontSmlMissingAdvance);
  if (!flagged) return;
  int top = kKeyboardTextTop, bottom = kKeyboardTextBottom;
  if (text.size() == 1) {
    if (const auto* g = font.glyphFor(
            static_cast<std::uint8_t>(text[0]))) {
      top = g->top;
      bottom = g->bottom;
    }
  }
  kbBlinkBracket(fb, x, penY - top, end, penY + bottom, blink);
}

// FUN_00414f1c (OBSERVED): centered text at x = trunc((600-w)/2) —
// the original's (v - sign) >> 1 is trunc toward zero, NOT the
// mouse screen's SAR floor-halving. flag EAX passes through to
// FUN_00414dd4.
void kbDrawCentered(const FtiFont& font, std::string_view text,
                    IndexedFramebuffer& fb, int penY, bool flagged,
                    KbBlink& blink) {
  const int w = measureFtiText(font, text, kFtiFontSmlMissingAdvance);
  const int x = (600 - w) / 2;  // trunc toward zero (OBSERVED)
  kbDrawFlagged(font, text, fb, x, penY, flagged, blink);
}

// FUN_0041f068 (OBSERVED): one binding row — label at
// x = 310*(row/10)+10, y = 30*(row%10)+64, flag =
// (!capture && row==selection); the key glyph at x = 310*col+210,
// flag = (capture && row==selection); the glyph byte comes from the
// LANG-selected table (resolved per row in the original).
void kbDrawRow(const FtiFont& font, IndexedFramebuffer& fb,
               std::string_view label, std::uint8_t glyphByte, int row,
               int selection, bool capture, KbBlink& blink) {
  const int col = row / 10;   // trunc division (OBSERVED IDIV)
  const int sub = row % 10;
  const int y = sub * kKeyboardRowStepY + kKeyboardRowY0;
  kbDrawFlagged(font, label, fb, col * kKeyboardColStepX + kKeyboardLabelX,
                y, !capture && row == selection, blink);
  const char gbuf[1] = {static_cast<char>(glyphByte)};
  kbDrawFlagged(font, std::string_view(gbuf, 1), fb,
                col * kKeyboardColStepX + kKeyboardGlyphX, y,
                capture && row == selection, blink);
}

bool drawKeyboardFrame(IndexedFramebuffer& fb, Palette& palette,
                       const FtiFont& fontSml,
                       const FtiSpriteFrame& arrow,
                       const KeyboardMenuLabels& labels,
                       std::span<const std::byte> sysPalHead,
                       const KbFrameState& st, KbBlink& blink,
                       std::string* err) {
  if (fb.width() != 600 || fb.height() != 360) {
    if (err) *err = "keyboard frame: framebuffer is not 600x360";
    return false;
  }
  if (sysPalHead.size() < 192) {
    if (err) *err = "keyboard frame: SYS_PAL head needs 192 bytes";
    return false;
  }
  for (const auto& l : labels.rows) {
    if (l.empty()) {
      if (err) *err = "keyboard frame: empty KM_* label";
      return false;
    }
  }
  if (labels.reset.empty() || labels.quit.empty() ||
      labels.doit.empty()) {
    if (err) *err = "keyboard frame: empty KM_RESET/QUIT/DOIT label";
    return false;
  }

  // The screen performs no palette upload of its own — the bound
  // palette is the inherited options composition (SYS_PAL head +
  // zeroed tail + the DAT_0054147e lift), re-bound per frame.
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

  // FUN_00415658 — clear(0).
  fb.clear(0);

  // Rows 0-18 — FUN_0041f068 call order, two columns. The original
  // resolves the LANG record per row; the resolved tag is stable so
  // the table is selected once per frame here (same bytes drawn).
  const auto& glyphs = keyboardGlyphTable(st.langTag);
  for (int r = 0; r < kKeyboardBindingRows; ++r) {
    // NATIVE bounds hardening: the original indexes table[keyVal]
    // with the full dword — values outside 0..127 read OOB there
    // (UNKNOWN edge; the port's parse bounds the domain to 0..127,
    // so this mask only covers an unreachable case).
    const int keyVal = (*st.keys)[kKeyboardRowToGlobal[r]] & 0x7f;
    kbDrawRow(fontSml, fb, labels.rows[r], glyphs[keyVal], r,
              st.selection, st.capture, blink);
  }

  // KM_RESET / KM_QUIT — centered FUN_00414f1c rows at y=16/32.
  kbDrawCentered(fontSml, labels.reset, fb, kKeyboardResetY,
                 st.selection == kKeyboardResetRow, blink);
  kbDrawCentered(fontSml, labels.quit, fb, kKeyboardQuitY,
                 st.selection == kKeyboardQuitRow, blink);

  // KM_DOIT — capture prompt only, flag 0.
  if (st.capture) {
    kbDrawCentered(fontSml, labels.doit, fb, kKeyboardDoitY,
                   false, blink);
  }

  // ARROW via FUN_004236c0 at the logical mouse — drawn even in
  // capture mode.
  blitFtiSpriteFrame(arrow, fb, st.arrowX, st.arrowY);
  return true;
}

} // namespace

bool renderKeyboardMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                             const FtiFont& fontSml,
                             const FtiSpriteFrame& arrow,
                             const KeyboardMenuLabels& labels,
                             std::span<const std::byte> sysPalHead,
                             const KeyboardMenuSpec& spec,
                             std::string* err) {
  KbFrameState st{spec.selection, spec.capture, &spec.keys,
                  spec.langTag, spec.brightness, spec.arrowX,
                  spec.arrowY};
  // The static spec freezes DAT_0049a770 at 0 and DAT_0049b6f0 at
  // its power-on 1.0 — the first flagged draw advances to 1,
  // bit-3 clear.
  KbBlink blink{0, 1.0f, nullptr};
  return drawKeyboardFrame(fb, palette, fontSml, arrow, labels,
                           sysPalHead, st, blink, err);
}

bool renderKeyboardMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                               const FtiFont& fontSml,
                               const FtiSpriteFrame& arrow,
                               const KeyboardMenuLabels& labels,
                               std::span<const std::byte> sysPalHead,
                               KeyboardMenuController& ctl,
                               int brightness, std::string* err) {
  KbFrameState st{ctl.selection(), ctl.capture(), &ctl.keyGlobals(),
                  labels.langTag, brightness, ctl.mouseX(), ctl.mouseY()};
  KbBlink blink{0, ctl.smoothedDelta(), &ctl};
  return drawKeyboardFrame(fb, palette, fontSml, arrow, labels,
                           sysPalHead, st, blink, err);
}

} // namespace mdk
