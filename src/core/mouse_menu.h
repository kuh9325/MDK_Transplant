// Phase 4J — Mouse options child screen (FUN_004217e8, front-end
// mode 0x04) — the third real child screen of the options sub-menu,
// and the first that writes the input-mapping settings consumed by
// the later gameplay-input path.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4J and
// analysis-private/logs/disasm_4217e8.txt, disasm_4j_helpers.txt,
// disasm_4j_marker.txt, disasm_4j_font3.txt):
//
//   Entry (FUN_00421664, OBSERVED — reached from Options row 3 via
//   the activate/LEFT/RIGHT dispatch tables):
//     DAT_00541493 = 0x04       mouse mode
//     DAT_0054bd40 = 0          selection — reset at entry
//     DAT_0054bd38 = 0          grid column — reset at entry
//     DAT_0054bd3c = 3          axis count
//     DAT_0054bd44 = 4          button count
//     NO mouse/tick/ramp/timing reset — the shared globals carry
//     over from the parent options screen
//
//   Frame (FUN_004217e8, OBSERVED): the same shared query helpers
//   as the options screen, but Esc is checked FIRST (before prev):
//     Esc (DAT_0054b570 raw level) -> DAT_00541493 = 0x0b + RET —
//       silent exit, ends the frame before the draw
//     prev  -> sel-1, wraps <0 -> DAT_0054bd3c+0x13-1 (=22)
//     next  -> sel+1, wraps >=DAT_0054bd3c+0x13 (=23) -> 0
//     mouse gate (same three-global check DAT_0054b644/648/640 —
//       DAT_0054b64c is NOT in the gate; clamp (590,350) inside):
//       x < 250:
//         band = trunc((y-2)/16);  0<=band<4    -> sel = band
//         else band2 = trunc((y-259)/16); 0<=band2<axes -> sel = band2+4
//       x >= 250 (only when the band is valid):
//         band = trunc((y-33)/16); 0<=band<16 -> sel = band+7
//         col  = clamp(trunc((x-396)/16), 0, cols-1)
//     LEFT  -> row 1: MouseOn 0<->1 + dirty; row 2: MouseYReversed
//              0<->1 + dirty; rows 4-6: axis letter -1
//              (FUN_004216a0); rows 7-22: col-1, wraps <0 -> cols-1
//     RIGHT -> same row 1/2 toggles; rows 4-6: letter +1;
//              rows 7-22: col+1, wraps >=cols -> 0
//     activate -> row 0: no-op (falls to draw)
//                 row 1: MouseOn toggle + dirty
//                 row 2: MouseYReversed toggle + dirty
//                 row 3: DAT_00541493 = 0x0b + RET — exit to options
//                 rows 4-6: axis letter +1 (FUN_004216a0)
//                 rows 7-22: FUN_00421774 — toggle bit (sel-7) of
//                        MouseWButtMap[col] through the 16-entry
//                        exclusive-group table at 0x499f0c
//     draw block -> FUN_0046c86c present -> FUN_0042fe78 timing.
//   OBSERVED: rows 0 and 3 take no LEFT/RIGHT mutation (they fall
//   through the toggle checks); the column wrap is bidirectional.
//
//   Axis-letter domain (FUN_004216a0, OBSERVED): '0' plus 'A'..'H'
//   — '0' is the valid "Off" mapping (JOY_A0). +1: '0'->'A', ...
//   'H'->'0'; -1: '0'->'H', ..., 'A'->'0'; an out-of-domain byte
//   repairs to '0'. If the map string is shorter than the axis
//   index the whole map is reset to the literal "0" and the check
//   re-runs — for a map that stays too short the original LOOPS
//   FOREVER; the port applies the repair once and skips the cycle
//   (documented NATIVE hardening of a proven hang).
//
//   Button-toggle exclusivity (FUN_00421774 + table @0x499f0c):
//   bit set -> clear just that bit; bit clear ->
//   mask = (mask & ~group[row]) | bit; dirty=1 either way.
//
//   Draw (OBSERVED — all FONTSML; no FONTBIG, no FUN_00423a24
//   ramp calls on this screen):
//     FUN_00415658 clear(0)
//     FUN_004213e8 rows 0-3 at y = row*16+16 — record text drawn
//       at x = (300-w)>>1 (SAR floor-halving around x=150), flag =
//       (row == sel): row0 JOY_TEST, row1 M_ENA/M_DIS by MouseOn,
//       row2 M_REV/M_NORM by MouseYReversed, row3 JOY_QUIT
//     FUN_00421504 grid: header row -1 ("JOY_B", cellMask =
//       DAT_0054b640 live buttons) + 16 rows ("JOY_B%c" A..P) —
//       label right-aligned at gridX0-w-8 (gridX0 = 428-cols*8 =
//       396), row y = row*16+46; per column c at cellX = 396+16c:
//       bit set -> solid fill color 6 (cellX, y-13)-(cellX+13, y-1);
//       bit clear -> hollow outline color 14 (cellX+1, y-13)-
//       (cellX+13, y-1); active column -> cursor outline
//       (cellX, y-14)-(cellX+14, y) color 14 on filled / 6 on hollow
//       cells; cellMask for row r = bit i of MouseWButtMap[i]>>r
//     FUN_00421448 axis rows i at y = 350-(5-i)*16 = 270,286,302 —
//       "JOY_AX%d" caption at x=60 flag=0, "JOY_A%c" action name at
//       x=90 flag=(i+4==sel), hollow bar outline color 14
//       (10, y-11)-(50, y-3), marker fill color 6
//       (pos-2, y-11)-(pos+2, y-3) where pos =
//       floor(clamp(delta_i/scale_i, -1, 1)*20+30) — FCOMP/JNC
//       clamps NaN to -1.0
//     FUN_004212d0 test indicator — frame outline color 2
//       (50,110)-(150,210); box outlines color 3:
//       (mx+97, my+157)-(mx+103, my+163) and
//       (mx+98, my+158)-(mx+102, my+162) where m =
//       clamp(FISTP(50*delta/scale), -50, 50) — FISTP of an
//       out-of-int-range quotient yields INT_MIN -> -50
//     ARROW via FUN_004236c0 at the logical mouse
//
//   Selection marker (OBSERVED — FUN_00414dd4 flag=1 ->
//   FUN_00414b28): a blinking double-outline bracket around the
//   text rect (penStart, penY-top)-(penEnd, penY+bottom); multi-
//   char default top=14 bottom=2, single-char uses the glyph's
//   own extents. Colors 1/2 swap on bit 3 of DAT_0049a770,
//   advanced floor(acc + DAT_0049b6f0) per call. Flagged draws:
//   the selected left row (rows 0-3) and the selected axis action
//   (rows 4-6). Grid rows draw NO flagged text — the cell cursor
//   is their indicator and the accumulator freezes there.
//
//   Exit (OBSERVED): inline in FUN_004217e8 — DAT_00541493 = 0x0b
//   + RET. The options selection _DAT_0054bd34 is untouched ->
//   resumes 3. The shared dirty flag carries back; persistence
//   waits for the eventual options exit (FUN_00420d68).
//
//   Settings globals (OBSERVED — settings-table dump + BUILD_A
//   MDK.CFG): the screen mutates only the W set:
//     DAT_005413de MouseWAxesMap  type-3 char[4] "ABG" (49)
//     DAT_005413be..ca MouseWButtMapA..D  type-0 dwords {1,4,2,0}
//                                    (53-56)
//     DAT_005413e6..ee MouseWX/Y/ZScale   type-1 floats {16,16,50}
//                                    (61-63)
//     DAT_00541472 MouseOn        type-2 bool, factory TRUE (67)
//     DAT_00541476 MouseYReversed type-1 float slot; the screen
//                                writes raw int32 0/1 into it
//                                (1.4013e-45 denormal — OBSERVED
//                                quirk, reproduced as raw bits) (68)
//   Every mutation latches the shared dirty flag DAT_00541486.
//
#ifndef MDK_CORE_MOUSE_MENU_H
#define MDK_CORE_MOUSE_MENU_H

#include "core/frontend_machines.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mdk {

class IndexedFramebuffer;
class Palette;
struct FtiFont;
struct FtiSpriteFrame;
struct FrontendMenuInput;

// OBSERVED screen geometry / state constants.
inline constexpr int kMouseRowTotal = 23;      // DAT_0054bd3c + 0x13
inline constexpr int kMouseAxisCount = 3;      // DAT_0054bd3c
inline constexpr int kMouseButtonCount = 4;    // DAT_0054bd44
inline constexpr int kMouseGridRows = 16;      // JOY_BA..JOY_BP
inline constexpr int kMouseGridSelBase = 7;    // sel 7..22 = grid rows
// Left-column rows 0-3 draw at y = row*16+16 (FUN_004213e8), text
// at x = (300-w)>>1.
inline constexpr int kMouseLeftRowY0 = 16;
inline constexpr int kMouseLeftRowStep = 16;
inline constexpr int kMouseLeftCenterX = 300;  // 0x12c
// Axis rows draw at y = 350-(5-i)*16 = 270,286,302.
inline constexpr int kMouseAxisRowY0 = 270;
inline constexpr int kMouseAxisRowStep = 16;
// Grid geometry (FUN_00421504): row y = row*16+46 (header row -1
// at y=30); gridX0 = 428 - cols*8 = 396 for 4 columns; cells 16px.
inline constexpr int kMouseGridY0 = 46;
inline constexpr int kMouseGridRowStep = 16;
inline constexpr int kMouseGridHeaderRow = -1;
inline constexpr int kMouseGridXBase = 428;    // 0x1ac
inline constexpr int kMouseGridCellW = 16;
inline constexpr int kMouseGridLabelGap = 8;
// Hit-test constants (OBSERVED): left column x<250 has two bands —
// rows 0-3 via trunc((y-2)/16), axis rows via trunc((y-259)/16);
// the right column x>=250 maps trunc((y-33)/16) to sel = band+7
// and clamps col = trunc((x-396)/16) into [0, cols-1] — only when
// the band is valid.
inline constexpr int kMouseHitSplitX = 250;    // 0xfa
inline constexpr int kMouseHitLeftBase = 2;
inline constexpr int kMouseHitAxisBase = 259;  // 0x103
inline constexpr int kMouseHitGridBase = 33;   // 0x21
inline constexpr int kMouseHitBandSize = 16;   // 0x10
inline constexpr int kMouseHitColBase = 396;   // gridX0
// Axis indicator (FUN_00421448): caption x=60, action x=90,
// hollow bar (10, y-11)-(50, y-3), marker = floor(clamped*20+30),
// fill (pos-2, y-11)-(pos+2, y-3) color 6.
inline constexpr int kMouseAxisCaptionX = 60;
inline constexpr int kMouseAxisActionX = 90;
inline constexpr int kMouseAxisBarX0 = 10;
inline constexpr int kMouseAxisBarX1 = 50;
inline constexpr int kMouseAxisBarTop = 11;
inline constexpr int kMouseAxisBarBottom = 3;
inline constexpr float kMouseAxisMarkerScale = 20.0f; // d[0x495d0c]
inline constexpr float kMouseAxisMarkerBias = 30.0f;  // d[0x495d10]
inline constexpr int kMouseAxisMarkerHalf = 2;        // pos±2
// Test indicator (FUN_004212d0): frame outline color 2
// (50,110)-(150,210); box outlines color 3 — 7x7 (97..103,
// 157..163) and 5x5 (98..102, 158..162) around (100,160) offset by
// m = clamp(FISTP(50*delta/scale), -50, 50).
inline constexpr int kMouseTestX0 = 50;
inline constexpr int kMouseTestY0 = 110;
inline constexpr int kMouseTestX1 = 150;
inline constexpr int kMouseTestY1 = 210;
inline constexpr int kMouseTestCenterX = 100;
inline constexpr int kMouseTestCenterY = 160;
inline constexpr int kMouseTestClamp = 50;
inline constexpr int kMouseTestScale = 50;
// Colors (OBSERVED immediates).
inline constexpr std::uint8_t kMouseTestFrameColor = 2;
inline constexpr std::uint8_t kMouseBoxColor = 3;
inline constexpr std::uint8_t kMouseCellFill = 6;
inline constexpr std::uint8_t kMouseCellOutline = 14;
inline constexpr std::uint8_t kMouseCursorHollow = 6;
inline constexpr std::uint8_t kMouseCursorFilled = 14;
inline constexpr std::uint8_t kMouseBarOutline = 14;
inline constexpr std::uint8_t kMouseMarkerFill = 6;
inline constexpr std::uint8_t kMouseBlinkA = 1;
inline constexpr std::uint8_t kMouseBlinkB = 2;
// Blink-bracket clamps (FUN_00414b28) and default text extents
// (FUN_00414dd4 multi-char path: top=14, bottom=2).
inline constexpr int kMouseBlinkMaxX = 597;   // 0x255
inline constexpr int kMouseBlinkMaxY = 357;   // 0x165
inline constexpr int kMouseBlinkMinX = 2;
inline constexpr int kMouseTextTop = 14;
inline constexpr int kMouseTextBottom = 2;
// Axis letter domain (FUN_004216a0): '0' ("Off") + 'A'..'H'.
inline constexpr char kMouseAxisOff = '0';
inline constexpr char kMouseAxisLetterMin = 'A';
inline constexpr char kMouseAxisLetterMax = 'H';
// The 16-entry exclusive-group table at 0x499f0c (OBSERVED dwords).
inline constexpr std::array<std::uint32_t, kMouseGridRows>
    kMouseExclusiveGroups = {0x2,    0x1,    0x0,    0x7c18,
                             0x7c18, 0xe0,   0x3e0,  0x3e0,
                             0x3c0,  0x3c0,  0x1c18, 0x1c18,
                             0x1c18, 0x6018, 0x6018, 0x0};
// OBSERVED factory defaults (mirror bytes): axes "ABG", button
// masks {1,4,2,0}, scales {16,16,50}, MouseOn TRUE, MouseYReversed
// float-bits 0.
inline constexpr const char* kMouseAxesMapFactory = "ABG";
inline constexpr const char* kMouseAxesMapRepair = "0"; // d[0x495d14]
inline constexpr std::array<std::uint32_t, kMouseButtonCount>
    kMouseButtMapFactory = {1, 4, 2, 0};
inline constexpr std::array<float, kMouseAxisCount>
    kMouseScalesFactory = {16.0f, 16.0f, 50.0f};

// Resource names (OBSERVED literals in the FUN_004217e8 body).
inline constexpr const char* kMouseTestRecord = "JOY_TEST";
inline constexpr const char* kMouseEnabledRecord = "M_ENA";
inline constexpr const char* kMouseDisabledRecord = "M_DIS";
inline constexpr const char* kMouseReversedRecord = "M_REV";
inline constexpr const char* kMouseNormalRecord = "M_NORM";
inline constexpr const char* kMouseQuitRecord = "JOY_QUIT";
inline constexpr const char* kMouseButtonsRecord = "JOY_B";
// Grid rows resolve "JOY_B%c" with 'A'+row -> JOY_BA..JOY_BP;
// axis rows resolve "JOY_A%c" with the map letter -> JOY_A0 for
// '0'/invalid, JOY_AA..JOY_AH for 'A'..'H'; captions resolve
// "JOY_AX%d" with the axis index -> JOY_AX0..JOY_AX2.

// Resolved label strings for one frame.
struct MouseMenuLabels {
  std::string_view test;       // JOY_TEST
  std::string_view enabled;    // M_ENA
  std::string_view disabled;   // M_DIS
  std::string_view reversed;   // M_REV
  std::string_view normal;     // M_NORM
  std::string_view quit;       // JOY_QUIT
  std::string_view buttons;    // JOY_B
  std::array<std::string_view, kMouseGridRows> actions;   // BA..BP
  std::array<std::string_view, 9> axisNames;  // A0 + AA..AH
  std::array<std::string_view, kMouseAxisCount> axisCaptions; // AX0..2
};

// The frozen frame state for the static preview (OBSERVED entry
// register values): selection/column reset to 0 at entry, factory
// mappings, centered test marker, ARROW at the carried mouse.
struct MouseMenuSpec {
  int selection = 0;          // DAT_0054bd40 — reset at entry
  int column = 0;             // DAT_0054bd38 — reset at entry
  bool mouseOn = true;        // DAT_00541472 (factory TRUE)
  std::uint32_t mouseYReversedBits = 0;  // DAT_00541476 raw bits
  std::string_view axesMap = kMouseAxesMapFactory;  // 0x5413de (W)
  std::array<std::uint32_t, kMouseButtonCount> buttMap =
      kMouseButtMapFactory;                        // 0x5413be..ca (W)
  std::array<float, kMouseAxisCount> scales = kMouseScalesFactory;
  std::array<int, kMouseAxisCount> deltas = {0, 0, 0};  // raw deltas
  std::uint8_t mouseButtons = 0;   // DAT_0054b640 header cells
  int brightness = 0;            // DAT_0054147e — the upload lift
  int arrowX = 300;              // logical mouse — NOT reset on entry
  int arrowY = 180;
};

// Semantic outputs of FUN_004217e8 — the only terminal action is
// the exit (mode 0x0b + RET), reached by Esc (checked first) or by
// activating row 3 (JOY_QUIT).
enum class MouseAction {
  None = 0,
  Back,  // -> DAT_00541493 = 0x0b: return to the options screen
};

// The reconstructed controller — FUN_004217e8 input/selection
// block plus the FUN_00421664 entry semantics. Same frame protocol
// as the other children:
//     update(input)   — FUN_004187e0 accumulate + tick advance +
//                       FUN_004217e8 queries/mutations
//     endFrame(dtMs)  — FUN_0042fe78/FUN_0042fcd0 timing update
// (The screen makes no FUN_00423a24 ramp calls — the selection
// indicators are the blink bracket and the cell cursor.)
class MouseMenuController {
public:
  // Mirrors FUN_00421664: selection and column reset to 0; the
  // settings globals are borrowed process globals (the W set the
  // screen mutates); `settingsDirty` is DAT_00541486 carried from
  // the parent options screen. Everything else carries over from
  // the shared machine state `s` untouched.
  MouseMenuController(
      const FrontendMachineState& s, bool mouseOn,
      std::uint32_t mouseYReversedBits, std::string_view axesMap,
      const std::array<std::uint32_t, kMouseButtonCount>& buttMap,
      const std::array<float, kMouseAxisCount>& scales,
      bool settingsDirty = false);

  int selection() const { return selection_; }   // DAT_0054bd40
  int column() const { return column_; }         // DAT_0054bd38
  bool mouseOn() const { return mouseOn_; }      // DAT_00541472
  // The raw dword bits of the type-1 float slot DAT_00541476 — the
  // screen toggles them as int 0/1 (the denormal quirk).
  std::uint32_t mouseYReversedBits() const { return mouseYRevBits_; }
  bool mouseYReversed() const { return mouseYRevBits_ != 0; }
  const std::string& axesMap() const { return axesMap_; }  // 0x5413de
  std::uint32_t buttMask(int col) const { return buttMap_[col]; }
  const std::array<std::uint32_t, kMouseButtonCount>& buttMap() const {
    return buttMap_;
  }
  float scale(int axis) const { return scales_[axis]; }
  const std::array<float, kMouseAxisCount>& scales() const {
    return scales_;
  }
  bool settingsDirty() const { return settingsDirty_; }  // 0x541486
  int mouseX() const { return m_.mouseX; }
  int mouseY() const { return m_.mouseY; }
  int tick() const { return m_.tick; }
  float smoothedDelta() const { return m_.timing.smoothed; }
  // DAT_0049a770 — the blink-bracket accumulator (a process global
  // carried inside the shared machine state; advances only when a
  // flagged FONTSML draw runs FUN_00414b28).
  int markerAccumulator() const { return m_.markerAcc; }

  const FrontendMachineState& machineState() const { return m_; }

  // The last frame's raw per-axis deltas and button nibble — the
  // same globals the original draws the test indicator and the
  // grid header from (DAT_0054b644/648/64c, DAT_0054b640).
  int lastDelta(int axis) const { return lastDelta_[axis]; }
  std::uint8_t lastButtons() const { return lastButtons_; }

  // The display letter for axis i (OBSERVED): the raw map byte —
  // the renderer folds it into the JOY_A%c record name ('0' and
  // anything outside 'A'..'H' resolve JOY_A0 = "Off").
  char axisMapChar(int axis) const;

  // Axis indicator value (OBSERVED): clamp(delta_i / scale_i,
  // -1.0, 1.0) — FLD/FST float math; a zero/NaN scale lands -1.0
  // through the original's FCOMP/JNC sequence.
  float axisValue(int axis) const;

  // Test-indicator marker offsets (OBSERVED):
  // clamp(FISTP(50*delta/scale), -50, 50) — FISTP of an
  // out-of-range quotient yields INT_MIN -> -50.
  int testOffsetX() const;
  int testOffsetY() const;

  // Per-frame update in the original order:
  //   Esc -> prev -> next -> mouse hit-test -> LEFT -> RIGHT ->
  //   activate. Esc and row-3 activate end the frame before the
  //   draw (RET); mutations continue to the next query.
  void update(const FrontendMenuInput& in);

  // FUN_00414b28 — advances DAT_0049a770 by
  // floor(acc + DAT_0049b6f0) and reports the post-advance bit-3
  // (the blink phase for this call). Called once per flagged
  // FONTSML draw by the renderer.
  bool advanceBlink();

  // FUN_0042fe78 timing update — same body as the options endFrame.
  void endFrame(double dtMs);

  // Pending semantic action from the last dispatch.
  MouseAction pendingAction() const { return action_; }
  MouseAction consumeAction();

  // OBSERVED frame-termination flag: the Esc branch and row-3
  // activate end in RET before the draw block and timing update.
  bool frameEndedEarly() const { return endedEarly_; }

private:
  FrontendMachineState m_;   // the shared globals block
  int selection_ = 0;        // DAT_0054bd40 — reset at entry
  int column_ = 0;           // DAT_0054bd38 — reset at entry
  bool mouseOn_;             // DAT_00541472
  std::uint32_t mouseYRevBits_;  // DAT_00541476 (float slot, int bits)
  std::string axesMap_;      // DAT_005413de (W axes map)
  std::array<std::uint32_t, kMouseButtonCount> buttMap_;  // 0x5413be..
  std::array<float, kMouseAxisCount> scales_;             // 0x5413e6..
  std::array<int, kMouseAxisCount> lastDelta_{0, 0, 0};  // 0x54b644..
  std::uint8_t lastButtons_ = 0;  // DAT_0054b640
  bool settingsDirty_;       // DAT_00541486
  MouseAction action_ = MouseAction::None;
  bool endedEarly_ = false;

  // FUN_004216a0 — cycle the axis-map letter at `axis` by delta
  // (+1/-1) inside the '0'+'A'..'H' domain, with the "0"-literal
  // repair for short maps (bounded — see the class comment).
  void cycleAxisLetter(int axis, int delta);
  // FUN_00421774 — toggle bit `row` of buttMap[col] through the
  // exclusive-group table.
  void toggleButtonBit(int row, int col);
};

// Compose the proven static frame in the original draw order:
// palette bind (SYS_PAL head + zeroed tail + brightness lift —
// the screen performs NO palette upload of its own; it inherits
// the options screen's composition, which the port binds per
// frame) -> clear(0) -> left rows 0-3 -> grid header + 16 rows ->
// axis rows -> test indicator -> ARROW. `sysPalHead` must be the
// 192-byte SYS_PAL record head.
// Returns false with `err` on contract violations.
bool renderMouseMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                          const FtiFont& fontSml,
                          const FtiSpriteFrame& arrow,
                          const MouseMenuLabels& labels,
                          std::span<const std::byte> sysPalHead,
                          const MouseMenuSpec& spec,
                          std::string* err);

// Same composition driven by the live controller: the controller's
// selection/column/settings/deltas drive the draw, the blink
// bracket advances DAT_0049a770 once per flagged draw, ARROW at
// the controller's logical mouse. `brightness` is DAT_0054147e —
// the upload lift (a process global).
bool renderMouseMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontSml,
                            const FtiSpriteFrame& arrow,
                            const MouseMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            MouseMenuController& ctl, int brightness,
                            std::string* err);

} // namespace mdk

#endif // MDK_CORE_MOUSE_MENU_H
