// Phase 4D — static front-end menu composition.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4D and
// analysis-private/logs/phase4d-evidence.md):
//
//   Target frame = the front-end root menu drawn by FUN_0041dc90
//   (dispatched from the main loop when DAT_00541493==0 &&
//   DAT_00541492!=0). This is the ONLY screen that uses the MDKOPT
//   backdrop — the options sub-menu handler FUN_00420eac (state 0x0b)
//   clears the framebuffer and draws OM_* labels instead.
//
//   Stable entry state (FUN_0041d85c "enter front-end" +
//   FUN_00418798 one-time mouse reset, both OBSERVED):
//     DAT_0049aa7c = 0    no transition
//     DAT_0049aaa0 = 0    no override backdrop
//     DAT_0049aa8c = 0    no blend buffer
//     DAT_0049aa98 = 0    item list = OPT0..OPT4
//     DAT_0054bc98 = savesExist (FUN_00428290 — BUILD_A has
//                    SAVES/1.SAV + 2.SAV, so 1)
//     DAT_0049aa78 = !savesExist  -> selection 0 ("Continue")
//     mouse (DAT_0054b634/38) = (300,180); the mouse-moved flag
//                    DAT_0054b644 gates the (mouseY-5)/36 hit-test, so
//                    with no input the selection stays 0
//
//   Draw order (FUN_0041dc90, OBSERVED):
//     1. memcpy(fb, _DAT_0054bca0, 0x34bc0)  — _DAT_0054bca0 = the
//        resolved MDKOPT pixel payload (OPTIONS.BNI), written by
//        FUN_0041d7b4
//     2. for each visible item i: FUN_00423b38(selected, x_arg, y_i, text)
//        y_i = 31 + 36*i   (0x1f + 0x24*i)
//        x_arg = maxW/2 — INTEGER signed division (SAR pattern),
//        maxW = largest UNSCALED FUN_00414be8 measure over the drawn
//        items (all five when saves exist; OPT1..OPT4 otherwise)
//        scale = 1.0 for the selected item, 0.65f otherwise
//        (FUN_00423a24 ramp endpoints — transitions frozen)
//        finalX = trunc(x_arg - measure*scale*0.5)   [x87 trunc RC=11]
//        draw = FUN_00414f64 (FONTBIG; marker flag EAX = 0)
//     3. FUN_004236c0(mouseX, mouseY)  — ARROW sprite at the raw
//        mouse position (hotspot 0,0)
//     4. palette: MDKOPT embedded palette (FUN_00413b40 uploads
//        head 64 + tail 192 at resolve; FUN_00416700 fades toward it —
//        frozen at the endpoint)
//
//   This module freezes all of that to one static frame. No input,
//   no animation, no transitions, no audio.
//
// Phase 4E adds the interactive controller below: the same composition
// driven by the reconstructed FUN_0041dc90 input/selection/scale state.
//
#ifndef MDK_CORE_FRONTEND_MENU_H
#define MDK_CORE_FRONTEND_MENU_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mdk {

class IndexedFramebuffer;
class Palette;
struct FtiFont;
struct FtiSpriteFrame;
struct IndexedImage;

// OBSERVED layout constants (FUN_0041dc90 item block).
inline constexpr int kFrontendItemY0 = 31;      // 0x1f
inline constexpr int kFrontendItemStep = 36;    // 0x24
inline constexpr int kFrontendOptCount = 5;     // OPT0..OPT4
// OBSERVED stable scale endpoints (FUN_00423a24).
inline constexpr float kFrontendScaleSelected = 1.0f;
inline constexpr float kFrontendScaleUnselected = 0.65f;
// OBSERVED mouse reset (FUN_00418798 — startup, before front-end).
inline constexpr int kFrontendMouseResetX = 300;
inline constexpr int kFrontendMouseResetY = 180;
// OBSERVED coordinate clamps (FUN_004187e0 accumulate / FUN_0041dc90
// hit-test gate).
inline constexpr int kFrontendMouseMaxX = 599;     // 0x257
inline constexpr int kFrontendMouseMaxY = 359;     // 0x167
inline constexpr int kFrontendHitClampX = 590;     // 0x24e
inline constexpr int kFrontendHitClampY = 350;     // 0x15e
// OBSERVED hit-test constants (FUN_0041dc90: band = trunc((y-5)/36)).
inline constexpr int kFrontendHitBandBase = 5;
inline constexpr int kFrontendHitBandSize = 36;    // 0x24
// OBSERVED scale-ramp constants (FUN_00423a24 + dumped data):
//   endpoints 1.0 / 0.65; acc in [0,5]; slope = 0.35*0.2 = 0.07.
inline constexpr float kFrontendRampLimit = 5.0f;    // d[0x496018]
inline constexpr float kFrontendRampSlopeA = 0.35f;  // d[0x49601c]
inline constexpr float kFrontendRampSlopeB = 0.2f;   // d[0x496024]
// OBSERVED key-repeat timing (FUN_004237b4/838, DAT_00541518 units):
// first deadline tick+30, repeat deadline tick+3, staleness window +100.
inline constexpr int kFrontendRepeatFirstDelay = 30;  // 0x1e
inline constexpr int kFrontendRepeatPeriod = 3;
inline constexpr int kFrontendRepeatWindow = 100;     // 0x64

struct FrontendMenuItem {
  int optIndex = 0;   // which OPT record (0..4)
  int y = 0;          // pen row on the framebuffer
  bool selected = false;
};

// The frozen frame state (OBSERVED entry-state register values).
struct FrontendMenuSpec {
  std::vector<FrontendMenuItem> items;
  int arrowX = kFrontendMouseResetX;
  int arrowY = kFrontendMouseResetY;
};

// Item list for the proven entry state:
//   savesExist  -> OPT0..OPT4 at y 31,67,103,139,175; selection 0
//   !savesExist -> OPT1..OPT4 at y 31,67,103,139;    selection 1
FrontendMenuSpec frontendMenuSpec(bool savesExist);

// Compose the proven stable frame into `fb` in the original order:
// backdrop pixels memcpy -> centered scaled FONTBIG items -> ARROW.
// `backdrop` must be exactly the 600x360 MDKOPT image; its embedded
// palette is bound to `palette` (the FUN_00413b40 upload).
// `optStrings[i]` must be the resolved OPTi record payload
// (a NUL-terminated ASCII string). Returns false with `err` on
// contract violations (wrong backdrop size, missing strings, missing
// arrow frame 0).
bool renderFrontendMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                             const IndexedImage& backdrop,
                             const FtiFont& fontBig,
                             const FtiSpriteFrame& arrow,
                             std::span<const std::string_view> optStrings,
                             const FrontendMenuSpec& spec,
                             std::string* err);

// ---------------------------------------------------------------------------
// Phase 4E — interactive front-end root-menu controller.
//
// Reconstruction of the FUN_0041dc90 input/selection/scale state machine
// for the stable root list (DAT_0049aa98 == 0). Every rule below is
// OBSERVED at instruction level in MDK95.EXE — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4E.
//
// Semantic activation outputs. The original dispatch calls FUN_0041dbd4
// plus a downstream action (or sets the quit flag); Phase 4E emits these
// events only — gameplay, save I/O, the options sub-menu, and audio all
// remain deferred.
enum class FrontendAction {
  None = 0,
  ContinueGame,  // sel 0 with saves: FUN_00415658 + save-load path
  NewGame,       // sel 1: FUN_0041dbd4 + FUN_0041b630
  SavedGame,     // sel 2: FUN_0041dbd4 + FUN_004202cc
  OpenOptions,   // sel 3: FUN_00420cf0 (sub-menu itself is Phase 4F)
  Quit,          // sel 4 (or sel 0 without saves): DAT_0054148e=1
  EnterAttract,  // DIK_RIGHT edge (DAT_0054b554): attract/slideshow
                 // trigger FUN_0041ef74 — deferred, emitted only
};

// Platform-neutral per-frame input for the controller. The platform layer
// translates device state into these semantic fields:
//   prevHeld / nextHeld : DIK_UP / DIK_DOWN *level* with original
//                         "held-or-pressed-this-poll" semantics (keymap
//                         bit 103 / 108 of the original key bitmap).
//   confirmEdge         : DIK_RETURN press *edge* (keymap bit 28 new-
//                         press); fires once per physical press.
//   attractEdge         : DIK_RIGHT press edge (keymap bit 106).
//   mouseDx / mouseDy   : raw per-frame mouse deltas (DAT_0054b644/48;
//                         no sensitivity scaling in the original).
//   mouseButtons        : 4-bit nibble, bit i = button i+1 held
//                         (bit0 left, bit1 right, bit2 middle, bit3 btn4;
//                         the original polls a DIMOUSESTATE and packs
//                         (b&0x80)>>7|6|5|4).
struct FrontendMenuInput {
  bool prevHeld = false;
  bool nextHeld = false;
  bool confirmEdge = false;
  bool attractEdge = false;
  int mouseDx = 0;
  int mouseDy = 0;
  std::uint8_t mouseButtons = 0;
};

// The reconstructed controller. Frame protocol mirrors the original
// main loop (FUN_0040103c):
//     update(input)   — FUN_004187e0 accumulate + tick advance +
//                       FUN_0041dc90 input/selection/idle block
//     itemScale(...)  — FUN_00423a24, called by the renderer once per
//                       drawn item in draw order
//     endFrame(dtMs)  — FUN_0042fb68/FUN_0042fcd0 timing update
// All cross-frame state lives here so the update is deterministic under
// synthetic input.
class FrontendMenuController {
public:
  // Mirrors FUN_0041d85c: savesExist -> selection 0, else selection 1
  // (OPT0 "Continue" hidden but keeps its numeric index). Mouse resets
  // to (300,180) per FUN_00418798. Ramp/timing registers start at their
  // observed power-on values.
  explicit FrontendMenuController(bool savesExist);

  bool savesExist() const { return savesExist_; }
  int selection() const { return selection_; }         // DAT_0049aa78
  int mouseX() const { return mouseX_; }               // DAT_0054b634
  int mouseY() const { return mouseY_; }               // DAT_0054b638
  int tick() const { return tick_; }                   // DAT_00541518
  float idleSeconds() const { return idleSeconds_; }   // DAT_0049aaa4
  int listState() const { return 0; }                  // DAT_0049aa98
  float rampAccumulator() const { return rampAcc_; }   // DAT_0054bdd8
  float smoothedDelta() const { return smoothed_; }    // DAT_0049b6f0
  float deltaSeconds() const { return deltaSec_; }     // DAT_0049b6f4

  // Per-frame update: mouse accumulate (FUN_004187e0), tick advance,
  // then the FUN_0041dc90 input block in original order:
  //   prev query -> next query -> mouse hit-test -> activate query ->
  //   idle timer -> attract edge.
  void update(const FrontendMenuInput& in);

  // FUN_00423a24 — scale for one item during the draw pass. `centerX` is
  // the shared x_arg (maxW/2); `itemY` the item's pen row; `selFlag` is
  // (item == selection). Must be called once per drawn item in draw
  // order — the call mutates the ramp machine (transition bookkeeping
  // and the once-per-frame accumulator advance).
  float itemScale(int centerX, int itemY, bool selFlag);

  // FUN_0042fb68/FUN_0042fcd0 timing update. `dtMs` is the real frame
  // delta in milliseconds. Called once per frame after rendering.
  void endFrame(double dtMs);

  // Pending semantic action from the last activate/attract dispatch.
  FrontendAction pendingAction() const { return action_; }
  FrontendAction consumeAction();

private:
  bool repeatQuery(bool held, int& deadline) const;  // FUN_004237b4/838

  bool savesExist_;
  int selection_;        // DAT_0049aa78
  int mouseX_;           // DAT_0054b634
  int mouseY_;           // DAT_0054b638
  int tick_ = 0;         // DAT_00541518
  float idleSeconds_ = 0.0f; // DAT_0049aaa4
  // Repeat deadlines (DAT_0049ac84 up, DAT_0049ac88 down).
  int prevDeadline_ = 0;
  int nextDeadline_ = 0;
  // Mouse-button edge latch (DAT_0049ac80).
  bool buttonLatch_ = false;
  // Ramp machine (DAT_0054bdc8..bdd8): current/prev item keys + acc.
  int rampCurX_ = 0, rampCurY_ = 0;
  int rampPrevX_ = 0, rampPrevY_ = 0;
  float rampAcc_ = 0.0f;
  // Timing struct fields (FUN_0042fb30 init values).
  int frameStep_ = 1;        // DAT_0049b6e8
  int stepAccum_ = 0;        // DAT_0049b6f8
  float smoothed_ = 1.0f;    // DAT_0049b6f0
  float deltaSec_ = 1.0f / 30.0f; // DAT_0049b6f4
  int virtualMs_ = 0;        // DAT_0049b700 (integer-ms virtual clock)
  double realMs_ = 0.0;      // accumulated real clock (integer-ms domain)
  bool timingStarted_ = false;
  FrontendAction action_ = FrontendAction::None;
};

// Compose one live frame from the controller state — identical
// composition to renderFrontendMenuFrame, except each item's scale comes
// from the controller's FUN_00423a24 ramp machine (called in draw order)
// and the arrow sits at the controller's logical mouse position.
// `ctl` is non-const because the scale machine mutates during the pass.
bool renderFrontendMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                               const IndexedImage& backdrop,
                               const FtiFont& fontBig,
                               const FtiSpriteFrame& arrow,
                               std::span<const std::string_view> optStrings,
                               FrontendMenuController& ctl,
                               std::string* err);

} // namespace mdk

#endif // MDK_CORE_FRONTEND_MENU_H
