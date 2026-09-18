// Phase 4K — Keyboard options child screen (FUN_0041f18c, front-end
// mode 0x05) — the fourth reconstructed child screen of the options
// sub-menu, and the one that writes the keyboard bindings consumed
// by the later gameplay-input path.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4K and
// analysis-private/logs/disasm_4k_keyboard.txt, disasm_4k_poll.txt,
// disasm_4k_poll2.txt, disasm_4k_poll3.txt, disasm_4k_input.txt,
// disasm_4k_dinput.txt, disasm_4k_defaults.txt, disasm_4k_helpers.txt,
// dump_4k_keynames.txt, dump_4k_defaults.txt, dump_keytab.txt,
// dump_4k_settings.txt):
//
//   Entry (FUN_0041f030, OBSERVED — reached from Options row 4 via
//   the activate/LEFT/RIGHT dispatch tables):
//     DAT_00541493 = 0x05       keyboard mode
//     DAT_0054bca8 = 0          capture flag cleared
//     DAT_0054bcac = 0x14       selection = 20 — the KM_QUIT row,
//                             NOT a row count and NOT reset to 0
//     NO mouse/tick/ramp/timing reset — the shared globals carry
//     over from the parent options screen
//
//   Frame (FUN_0041f18c, OBSERVED — after the shared accumulate +
//   tick prologue):
//     capture != 0:
//       Esc edge (DAT_0054b570) -> DAT_0054bca8 = 0 -> draw —
//         ESC can never be captured (checked before the poll)
//       else FUN_0041925c raw-key poll -> key==0 -> draw
//         key!=0 -> selection-mapped target dword:
//           key != *target -> DAT_00541486 = 1; *target = key
//           (same-value rebind writes NOTHING — no dirty, no store)
//         DAT_0054bca8 = 0 -> draw
//       ALL other queries skipped — nav, mouse, activate dead
//     capture == 0 (normal state):
//       prev query = FUN_004237b4 (UP) OR FUN_004238bc (LEFT) —
//         two independent repeat machines, either fires ->
//         sel-1, wraps <0 -> 0x14 (20)
//       next query = FUN_00423838 (DOWN) OR FUN_00423940 (RIGHT) —
//         sel+1, wraps >=0x15 (21) -> 0
//       mouse gate (same three-global check DAT_0054b644/648/640 —
//         DAT_0054b64c NOT in the gate; clamp (590,350) inside):
//         y >= 64:   band = trunc((y-50)/30); band>10 -> no select;
//                    band>=0 && x>=320 -> band+=10;
//                    0<=band<19 -> sel = band
//         y < 64:    y in [2,18)  -> sel = 19 (KM_RESET)
//                    y in [18,34) -> sel = 20 (KM_QUIT)
//                    (y<2 and y in [34,64) select nothing)
//       Esc edge -> DAT_00541493 = 0x0b + RET — silent exit, ends
//         the frame before the draw
//       activate query (FUN_00423764):
//         sel < 19  -> DAT_0054bca8 = 1 (capture begins) -> draw
//         sel == 19 -> FUN_00425db0 (29-dword mirror copy resets
//                      all bindings incl. the hidden hotkey slots)
//                      then DAT_00541486 = ECX = 1 (dirty latched)
//         sel >= 20 -> DAT_00541493 = 0x0b + RET — exit to options
//       OBSERVED: NO duplicate-key handling anywhere — the captured
//       code stores verbatim even when already bound elsewhere (the
//       factory defaults themselves bind 'A' to both LookUp and
//       ZoomIn and 'Z' to both LookDown and ZoomOut).
//     draw block -> FUN_0046c86c present -> FUN_0042fe78 timing.
//
//   Internal key-code domain (OBSERVED — NOT SDL, NOT Windows VK):
//     DIK offset <= 0x7f -> internal code = the DIK itself
//     DIK offset >  0x7f -> internal code = table_49bbf0[DIK & 0x7f]
//                           (unmapped extended keys -> 0x7f, itself a
//                           capturable code)
//     Mappings: KP_ENTER 0x9c->96, RCTRL 0x9d->97, KP_/ 0xb5->99,
//     SYSRQ 0xb7->100, RALT 0xb8->101, HOME 0xc7->102, UP 0xc8->103,
//     PGUP 0xc9->104, LEFT 0xcb->105, RIGHT 0xcd->106, END 0xcf->107,
//     DOWN 0xd0->108, PGDN 0xd1->109, INS 0xd2->110, DEL 0xd3->111,
//     0xef->112.
//     FUN_0041925c (capture poll, OBSERVED): FUN_00419168 returns the
//     lowest set bit of the four 32-bit new-press edge dwords
//     (DAT_0049a8e8..f4; 0 when empty — code 0 can't be produced), then
//     the right-modifier folds: 0x36->0x2a, 0x61->0x1d, 0x65->0x38.
//
//   Draw (FUN_0041f068 per binding row + the frame tail, OBSERVED —
//   all FONTSML via FUN_00414dd4/FUN_00414f1c; no FONTBIG, no ramp):
//     FUN_00415658 clear(0)
//     rows 0-18, two columns: label at x = 310*(row/10)+10,
//       y = 30*(row%10)+64, flag = (!capture && row==sel);
//       key glyph at x = 310*(row/10)+210, same y, flag =
//       (capture && row==sel) — the key name highlights during
//       capture instead of the label
//     glyph = table[keyVal] through the language table: LANG record
//       resolved per row (FUN_00414930 soft resolver — missing -> 0);
//       first byte 'F' -> 0x49ab28 (AZERTY), 'G' -> 0x49aba8
//       (QWERTZ), anything else -> 0x49aaa8 (QWERTY). Glyph byte 0
//       draws nothing (the unbound/blank case).
//     KM_RESET centered (600-w)/2 at y=16, flag = (sel==19)
//     KM_QUIT  centered (600-w)/2 at y=32, flag = (sel==20)
//     KM_DOIT  centered (600-w)/2 at y=354, flag=0 — capture only
//     ARROW via FUN_004236c0 at the logical mouse — drawn even in
//       capture mode
//
//   Settings globals (OBSERVED — settings-table dump entries 69-87,
//   all type-0 dword slots; the 29-dword block DAT_005413fe..0x54146e
//   also carries ten hidden weapon-hotkey slots g14..g23 at
//   0x541436..0x54145a, factory 2..11, reset by FUN_00425db0 but
//   never shown or persisted):
//     row 0  KM_LEFT   -> g0  KeyLeft     (69)  factory 105
//     row 1  KM_RIGHT  -> g1  KeyRight    (70)  factory 106
//     row 2  KM_UP     -> g2  KeyUp       (71)  factory 103
//     row 3  KM_DOWN   -> g3  KeyDown     (72)  factory 108
//     row 4  KM_JUMP   -> g4  KeyJump     (73)  factory 56  (LALT)
//     row 5  KM_SIDEL  -> g27 KeySideL    (86)  factory 51  (',')
//     row 6  KM_SIDE   -> g5  KeySide     (74)  factory 45  ('X')
//     row 7  KM_SIDER  -> g28 KeySideR    (87)  factory 52  ('.')
//     row 8  KM_SNIPE  -> g7  KeySniper   (76)  factory 57  (SPACE)
//     row 9  KM_FIRE   -> g6  KeyFire     (75)  factory 29  (LCTRL)
//     row 10 KM_TURBO  -> g8  KeyTurbo    (77)  factory 42  (LSHIFT)
//     row 11 KM_STURB  -> g9  KeySturbo   (78)  factory 58  (CAPS)
//     row 12 KM_LKUP   -> g10 KeyLookUp   (79)  factory 30  ('A')
//     row 13 KM_LKDWN  -> g11 KeyLookDown (80)  factory 44  ('Z')
//     row 14 KM_ZOOMI  -> g12 KeyZoomIn   (81)  factory 30  ('A')
//     row 15 KM_ZOOMO  -> g13 KeyZoomOut  (82)  factory 44  ('Z')
//     row 16 KM_INEXT  -> g24 KeyItemNext (83)  factory 27  (']')
//     row 17 KM_IPREV  -> g25 KeyItemPrev (84)  factory 26  ('[')
//     row 18 KM_IUSE   -> g26 KeyItemUse  (85)  factory 28  (RETURN)
//     row 19 KM_RESET  -> FUN_00425db0 (not a binding)
//     row 20 KM_QUIT   -> exit (not a binding)
//
//   Exit (OBSERVED): inline — Esc or the KM_QUIT row writes
//   DAT_00541493 = 0x0b and RETs (FUN_0041f058 is the same write as
//   a callable helper). The options selection _DAT_0054bd34 is
//   untouched -> resumes 4. The shared dirty flag carries back;
//   persistence waits for the eventual options exit (FUN_00420d68).
//
#ifndef MDK_CORE_KEYBOARD_MENU_H
#define MDK_CORE_KEYBOARD_MENU_H

#include "core/frontend_machines.h"
#include "core/frontend_settings.h"

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

// OBSERVED screen constants (FUN_0041f030/FUN_0041f18c).
inline constexpr int kKeyboardBindingRows = 19;  // rows 0-18
inline constexpr int kKeyboardResetRow = 19;     // "Set Defaults"
inline constexpr int kKeyboardQuitRow = 20;      // KM_QUIT
inline constexpr int kKeyboardRowTotal = 21;     // wrap bound 0x15
// DAT_0054bcac = 0x14 at FUN_0041f030 — the entry selection is the
// KM_QUIT row, NOT a row count and NOT a zero reset.
inline constexpr int kKeyboardEntrySelection = 20;
// The 29-dword block DAT_005413fe..0x54146e — 19 settings slots plus
// ten hidden hotkey slots (g14..g23 = 0x541436..0x54145a) that
// FUN_00425db0 resets inside the same copy.
inline constexpr int kKeyboardGlobalCount = 29;
inline constexpr int kKeyboardSettingSlots = 19;  // table entries 69-87
// Internal key-code domain: four 32-bit bitfields = codes 0..127.
inline constexpr int kKeyboardCodeCount = 128;

// Row -> index inside the 29-dword global block (the capture
// switch's literal global pointers, OBSERVED — the draw order and
// the settings order are deliberately different).
inline constexpr std::array<int, kKeyboardBindingRows>
    kKeyboardRowToGlobal = {0, 1, 2, 3, 4, 27, 5, 28, 7, 6,
                            8, 9, 10, 11, 12, 13, 24, 25, 26};

// Settings slot (entries 69-87 in table order) -> global index.
inline constexpr std::array<int, kKeyboardSettingSlots>
    kKeyboardSlotToGlobal = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                             10, 11, 12, 13, 24, 25, 26, 27, 28};

// OBSERVED factory defaults — the FUN_00425db0 mirror block at
// 0x49b1f2, all 29 dwords in global order (g14..g23 = the hidden
// number-row hotkeys 2..11). 'A'(30) and 'Z'(44) legitimately bind
// two actions each.
inline constexpr std::array<int, kKeyboardGlobalCount>
    kKeyboardDefaults = {105, 106, 103, 108, 56,  45,  29,  57, 42, 58,
                         30,  44,  30,  44,   2,   3,   4,   5,  6,  7,
                         8,   9,   10,  11,  27,  26,  28,  51, 52};

// Settings-table key names in table order (entries 69-87, OBSERVED).
inline constexpr std::array<const char*, kKeyboardSettingSlots>
    kKeyboardSettingNames = {
        "KeyLeft",   "KeyRight",  "KeyUp",       "KeyDown",
        "KeyJump",   "KeySide",   "KeyFire",     "KeySniper",
        "KeyTurbo",  "KeySturbo", "KeyLookUp",   "KeyLookDown",
        "KeyZoomIn", "KeyZoomOut","KeyItemNext", "KeyItemPrev",
        "KeyItemUse","KeySideL",  "KeySideR"};

// KM_* record names in draw order (OBSERVED literals in the
// FUN_0041f18c draw block).
inline constexpr std::array<const char*, kKeyboardBindingRows>
    kKeyboardRowRecords = {"KM_LEFT", "KM_RIGHT", "KM_UP",   "KM_DOWN",
                           "KM_JUMP", "KM_SIDEL", "KM_SIDE", "KM_SIDER",
                           "KM_SNIPE","KM_FIRE",  "KM_TURBO","KM_STURB",
                           "KM_LKUP", "KM_LKDWN", "KM_ZOOMI","KM_ZOOMO",
                           "KM_INEXT","KM_IPREV", "KM_IUSE"};
inline constexpr const char* kKeyboardResetRecord = "KM_RESET";
inline constexpr const char* kKeyboardQuitRecord = "KM_QUIT";
inline constexpr const char* kKeyboardDoitRecord = "KM_DOIT";
inline constexpr const char* kKeyboardLangRecord = "LANG";

// Draw geometry (OBSERVED immediates).
inline constexpr int kKeyboardColStepX = 310;   // 310*(row/10)
inline constexpr int kKeyboardLabelX = 10;      // +0x0a
inline constexpr int kKeyboardGlyphX = 210;     // +0xd2
inline constexpr int kKeyboardRowY0 = 64;       // +0x40
inline constexpr int kKeyboardRowStepY = 30;    // 30*(row%10)
inline constexpr int kKeyboardResetY = 16;      // 0x10
inline constexpr int kKeyboardQuitY = 32;       // 0x20
inline constexpr int kKeyboardDoitY = 354;      // 0x162
// Hit-test constants (OBSERVED): row bands need y>=64;
// band = trunc((y-50)/30) valid <=10; x>=320 shifts +10 (right
// column); y in [2,18) -> 19, [18,34) -> 20.
inline constexpr int kKeyboardHitRowBandTop = 64;    // 0x40
inline constexpr int kKeyboardHitBandBase = 50;      // 0x32
inline constexpr int kKeyboardHitBandSize = 30;      // 0x1e
inline constexpr int kKeyboardHitBandMax = 10;       // 0x0a
inline constexpr int kKeyboardHitSplitX = 320;       // 0x140
inline constexpr int kKeyboardHitResetLo = 2;
inline constexpr int kKeyboardHitResetHi = 18;       // 0x12
inline constexpr int kKeyboardHitQuitHi = 34;        // 0x22
// Blink-bracket clamps (FUN_00414b28) and default text extents
// (FUN_00414dd4 multi-char path: top=14, bottom=2) — same helpers
// as the mouse screen.
inline constexpr int kKeyboardBlinkMaxX = 597;   // 0x255
inline constexpr int kKeyboardBlinkMaxY = 357;   // 0x165
inline constexpr int kKeyboardBlinkMinX = 2;
inline constexpr int kKeyboardTextTop = 14;
inline constexpr int kKeyboardTextBottom = 2;
inline constexpr std::uint8_t kKeyboardBlinkA = 1;
inline constexpr std::uint8_t kKeyboardBlinkB = 2;

// ---------------------------------------------------------------------------
// Internal key-code domain helpers (the four-dword bitfield domain —
// deliberately NOT collapsed into an enum: the original stores and
// tests raw codes 0..127).
using KeyboardEdgeBitmap = std::array<std::uint32_t, 4>;

// FUN_0046b688's DIK -> internal rule (OBSERVED): offset <= 0x7f
// uses the DIK directly; > 0x7f maps through the 128-byte table at
// 0x49bbf0 (unmapped -> 0x7f). `dik` is the raw DirectInput offset —
// producing it from a platform key event is the platform layer's
// business (see application.cpp's scancode -> DIK seam); this module
// deliberately knows nothing about SDL.
int internalKeyFromDik(int dik);

// FUN_00419168 (OBSERVED): lowest set bit across the four 32-bit
// new-press edge dwords — 0..31 of word 0, 32..63 of word 1, 64..95
// of word 2, 96..127 of word 3. Returns 0 when the bitmap is empty
// (code 0 can't be produced by the device path, so 0 doubles as
// "no key").
int keyboardFirstEdgeBit(const KeyboardEdgeBitmap& edge);

// FUN_0041925c (OBSERVED): the capture poll — first edge bit, then
// the right-modifier folds 0x36->0x2a (RSHIFT->LSHIFT),
// 0x61->0x1d (RCTRL->LCTRL), 0x65->0x38 (RALT->LALT).
int keyboardPollCapture(const KeyboardEdgeBitmap& edge);

// The three 128-byte key-glyph tables (OBSERVED dumps): English
// QWERTY @0x49aaa8, French AZERTY @0x49ab28, German QWERTZ
// @0x49aba8 — internal code -> FONTSML glyph byte (0 = blank).
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTableEn();
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTableFr();
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTableDe();
// FUN_0041f068's selection (OBSERVED): LANG's first byte 'F' -> FR,
// 'G' -> DE, anything else (incl. a missing/empty LANG) -> EN.
const std::array<std::uint8_t, kKeyboardCodeCount>&
    keyboardGlyphTable(char langTag);

// Map between the persisted 19 settings fields and the 29-dword
// global block. From-settings: start from the factory block (the
// hidden slots always boot at factory), overlay the 19 slots.
// To-settings: read the 19 slots back (hidden slots never persist).
std::array<int, kKeyboardGlobalCount> keyboardGlobalsFromSettings(
    const FrontendSettings& s);
void keyboardSettingsFromGlobals(
    FrontendSettings& s,
    const std::array<int, kKeyboardGlobalCount>& g);

// Resolved label strings for one frame.
struct KeyboardMenuLabels {
  std::array<std::string_view, kKeyboardBindingRows> rows;  // KM_*
  std::string_view reset;  // KM_RESET
  std::string_view quit;   // KM_QUIT
  std::string_view doit;   // KM_DOIT
  // The LANG record's first byte — 'E' canonical; 0 selects the
  // English table exactly like the original's NULL resolve.
  char langTag = 'E';
};

// The frozen frame state for the static preview (OBSERVED entry
// register values): selection 20 (the KM_QUIT row), capture clear,
// factory 29-dword block, ARROW at the carried mouse.
struct KeyboardMenuSpec {
  int selection = kKeyboardEntrySelection;  // DAT_0054bcac = 0x14
  bool capture = false;                     // DAT_0054bca8 = 0
  std::array<int, kKeyboardGlobalCount> keys = kKeyboardDefaults;
  char langTag = 'E';      // LANG record first byte
  int brightness = 0;      // DAT_0054147e — the upload lift
  int arrowX = 300;        // logical mouse — NOT reset on entry
  int arrowY = 180;
};

// Semantic outputs of FUN_0041f18c — the only terminal action is
// the exit (mode 0x0b + RET), reached by Esc in normal state or by
// activating the KM_QUIT row.
enum class KeyboardAction {
  None = 0,
  Back,  // -> DAT_00541493 = 0x0b: return to the options screen
};

// The reconstructed controller — FUN_0041f18c input/selection/
// capture block plus the FUN_0041f030 entry semantics. Same frame
// protocol as the other children:
//     update(input)   — FUN_004187e0 accumulate + tick advance +
//                       FUN_0041f18c queries/capture/dispatch
//     endFrame(dtMs)  — FUN_0042fe78/FUN_0042fcd0 timing update
// (The screen makes no FUN_00423a24 ramp calls — the selection
// indicator is the FUN_00414b28 blink bracket.)
class KeyboardMenuController {
public:
  // Mirrors FUN_0041f030: capture cleared and selection = 0x14 (the
  // KM_QUIT row) at entry; `keys` borrows the 29-dword settings
  // block (process globals — the flow owns them); `settingsDirty`
  // is DAT_00541486 carried from the parent options screen.
  // Everything else carries over from the shared machine state `s`
  // untouched.
  KeyboardMenuController(
      const FrontendMachineState& s,
      const std::array<int, kKeyboardGlobalCount>& keys,
      bool settingsDirty = false);

  int selection() const { return selection_; }  // DAT_0054bcac
  bool capture() const { return capture_; }     // DAT_0054bca8
  // The live 29-dword block — binding rows read/write through
  // kKeyboardRowToGlobal; hidden slots ride along for the reset.
  const std::array<int, kKeyboardGlobalCount>& keyGlobals() const {
    return keys_;
  }
  int keyAt(int globalIndex) const { return keys_[globalIndex]; }
  int keyForRow(int row) const {
    return row >= 0 && row < kKeyboardBindingRows
               ? keys_[kKeyboardRowToGlobal[row]]
               : 0;
  }
  bool settingsDirty() const { return settingsDirty_; }  // 0x541486
  int mouseX() const { return m_.mouseX; }
  int mouseY() const { return m_.mouseY; }
  int tick() const { return m_.tick; }
  float smoothedDelta() const { return m_.timing.smoothed; }
  // DAT_0049a770 — the blink-bracket accumulator (a process global
  // carried inside the shared machine state; advances once per
  // flagged FONTSML draw).
  int markerAccumulator() const { return m_.markerAcc; }

  const FrontendMachineState& machineState() const { return m_; }

  // Per-frame update in the original order:
  //   capture ? (Esc-cancel -> draw | poll -> maybe commit -> draw)
  //   : (UP|LEFT) -> (DOWN|RIGHT) -> mouse hit-test -> Esc ->
  //     activate (capture-entry | defaults reset | quit-exit).
  // Esc-exit and the quit-row dispatch end the frame before the
  // draw (RET); every other path reaches the draw block.
  void update(const FrontendMenuInput& in);

  // FUN_00414b28 — advances DAT_0049a770 by
  // floor(acc + DAT_0049b6f0) and reports the post-advance bit-3
  // (the blink phase for this call). Called once per flagged
  // FONTSML draw by the renderer.
  bool advanceBlink();

  // FUN_0042fe78 timing update — same body as the options endFrame.
  void endFrame(double dtMs);

  // Pending semantic action from the last dispatch.
  KeyboardAction pendingAction() const { return action_; }
  KeyboardAction consumeAction();

  // OBSERVED frame-termination flag: the normal-state Esc branch
  // and the sel>=20 activate end in RET before the draw block and
  // timing update. Capture cancels/commits/entries all draw.
  bool frameEndedEarly() const { return endedEarly_; }

private:
  FrontendMachineState m_;   // the shared globals block
  int selection_ = kKeyboardEntrySelection;  // DAT_0054bcac = 0x14
  bool capture_ = false;     // DAT_0054bca8
  std::array<int, kKeyboardGlobalCount> keys_;  // 0x5413fe..0x54146e
  bool settingsDirty_;       // DAT_00541486
  KeyboardAction action_ = KeyboardAction::None;
  bool endedEarly_ = false;
};

// Compose the proven static frame in the original draw order:
// palette bind (SYS_PAL head + zeroed tail + brightness lift —
// the screen performs NO palette upload of its own; it inherits
// the options screen's composition, which the port binds per
// frame) -> clear(0) -> 19 two-column binding rows -> KM_RESET ->
// KM_QUIT -> (KM_DOIT) -> ARROW. `sysPalHead` must be the 192-byte
// SYS_PAL record head. Returns false with `err` on contract
// violations.
bool renderKeyboardMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                             const FtiFont& fontSml,
                             const FtiSpriteFrame& arrow,
                             const KeyboardMenuLabels& labels,
                             std::span<const std::byte> sysPalHead,
                             const KeyboardMenuSpec& spec,
                             std::string* err);

// Same composition driven by the live controller: the controller's
// selection/capture/binding state drives the draw, the blink
// bracket advances DAT_0049a770 once per flagged draw, ARROW at
// the controller's logical mouse. `brightness` is DAT_0054147e —
// the upload lift (a process global).
bool renderKeyboardMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                               const FtiFont& fontSml,
                               const FtiSpriteFrame& arrow,
                               const KeyboardMenuLabels& labels,
                               std::span<const std::byte> sysPalHead,
                               KeyboardMenuController& ctl,
                               int brightness, std::string* err);

} // namespace mdk

#endif // MDK_CORE_KEYBOARD_MENU_H
