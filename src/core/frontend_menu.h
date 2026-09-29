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

#include "core/frontend_machines.h"

#include <array>
#include <cstdint>
#include <functional>
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
// OBSERVED mouse reset (FUN_00418798 — startup, before front-end).
inline constexpr int kFrontendMouseResetX = 300;
inline constexpr int kFrontendMouseResetY = 180;
// OBSERVED hit-test constants (FUN_0041dc90: band = trunc((y-5)/36)).
inline constexpr int kFrontendHitBandBase = 5;
inline constexpr int kFrontendHitBandSize = 36;    // 0x24
// OBSERVED attract idle thresholds (FUN_0041dc90 tail; doubles at
// 0x495a20/0x495a28/0x495a30, seconds on DAT_0049aaa4).
inline constexpr float kAttractDelayMenu = 5.0f;   // state 0 -> slide 1
inline constexpr float kAttractDelayFirst = 4.0f;  // state 1 -> slide 2
inline constexpr float kAttractDelaySlide = 2.0f;  // state n -> slide n+1

struct FrontendMenuItem {
  int optIndex = 0;   // which OPT record (0..4)
  int y = 0;          // pen row on the framebuffer
  bool selected = false;
};

// The frozen frame state (OBSERVED entry-state register values).
struct FrontendMenuSpec {
  std::vector<FrontendMenuItem> items;
  int brightness = 0;   // DAT_0054147e — the FUN_0046d208 upload
                        // lift; canonical 0 at front-end entry,
                        // nonzero after a Display-screen mutation
                        // returns through Options (Phase 4H)
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
  EnterAttract,  // attract/slideshow advance fired this frame
                 // (FUN_0041ef74): DIK_RIGHT edge (DAT_0054b554)
                 // while state >= 0, or the idle timers hitting the
                 // 5.0/4.0/2.0 s thresholds (0x495a20/28/30). Query
                 // attractState() for the resulting slide index.
};

// Platform-neutral per-frame input for the controller. The platform layer
// translates device state into these semantic fields:
//   prevHeld / nextHeld : DIK_UP / DIK_DOWN *level* with original
//                         "held-or-pressed-this-poll" semantics (keymap
//                         bit 103 / 108 of the original key bitmap).
//   confirmEdge         : DIK_RETURN press *edge* (keymap bit 28 new-
//                         press); fires once per physical press.
//   attractEdge         : DIK_RIGHT press edge (keymap bit 106).
//                         Root menu only.
//   leftHeld / rightHeld: DIK_LEFT / DIK_RIGHT levels (keymap bits
//                         100 / 102). Options sub-menu only — the root
//                         menu never queries them.
//   cancelEdge          : DIK_ESCAPE press edge (keymap bit 1,
//                         DAT_0054b570). Options sub-menu only.
//   mouseDx / mouseDy   : raw per-frame mouse deltas (DAT_0054b644/48;
//                         no sensitivity scaling in the original).
//   mouseDz             : third DIMOUSESTATE axis delta
//                         (DAT_0054b64c — the wheel; the mouse
//                         child's Z-axis indicator reads it).
//   mouseButtons        : 4-bit nibble, bit i = button i+1 held
//                         (bit0 left, bit1 right, bit2 middle, bit3 btn4;
//                         the original polls a DIMOUSESTATE and packs
//                         (b&0x80)>>7|6|5|4).
//   rawKeyEdge          : the four 32-bit new-press edge dwords
//                         (DAT_0049a8e8..f4) in the ORIGINAL internal
//                         key-code domain — bit i = internal code i
//                         saw a press edge this poll (FUN_00419370's
//                         `edge = latch & ~prev`). Only the keyboard
//                         child's capture poll (FUN_0041925c) reads
//                         it; every other screen ignores it.
//
// Phase 18A adds the modal-overlay inputs (the original polls the same
// global arrays for every screen — these fields are simply the edge
// dwords the overlay frames read). Root/options ignore them all.
//   pageUp/pageDown     : DAT_0054b5b4/0x54b5b8 gated queries (PgUp/
//                         PgDn — save list page steps of 13).
//   homeEdge/endEdge    : DAT_0054b5bc/0x54b5c0 (list Home/End).
//   keyYEdge/keyNEdge   : DAT_0054b5ac/0x54b594 (abort-console Y/N).
//   f1..f12 edges       : the overlay-arm chain globals
//                         (0x54b5d4 F1 help, 0x54b5d8 F2 save,
//                         0x54b5dc F3 load, 0x54b5f8 F10 abort,
//                         0x54b5fc F11 brightness, 0x54b600 F12
//                         options) — armed only while a non-frontend
//                         primary mode runs with sub-mode 0.
//   pauseEdge           : DAT_0054b630 — the in-game pause toggle.
//   pauseAltEdge        : DAT_0054b598 — the second pause key the main
//                         loop ORs with pauseEdge.
//   utilityEdge         : DAT_0054b584 — the capture-utility binding
//                         edge (the DAT_005414f0 gate is shell-side).
//   typedChar           : the keycharmap-translated character the
//                         overlay frames consume (FUN_0041925c ->
//                         FUN_00419294); 0 = none this frame.
//   leftEdge/rightEdge  : DIK_LEFT/RIGHT press edges
//                         (DAT_0054b550/0x54b554) — cursor motion in
//                         the save name entry; rightEdge is the same
//                         global as attractEdge (the root menu reads
//                         it for the attract advance).
//   nameBackspaceEdge   : internal key 0x0e — delete char before the
//                         cursor (block shift).
//   nameDeleteEdge      : internal key 0x6f — delete char at cursor.
//   nameHomeEdge        : internal key 0x66 — cursor to start
//                         (0x54bda0 = 0).
//   nameEndEdge         : internal key 0x6b — cursor to end
//                         (0x54bda0 = strlen).
struct FrontendMenuInput {
  bool prevHeld = false;
  bool nextHeld = false;
  bool confirmEdge = false;
  bool attractEdge = false;
  bool leftHeld = false;
  bool rightHeld = false;
  bool cancelEdge = false;
  int mouseDx = 0;
  int mouseDy = 0;
  int mouseDz = 0;
  std::uint8_t mouseButtons = 0;
  std::array<std::uint32_t, 4> rawKeyEdge{};
  bool pageUpEdge = false;
  bool pageDownEdge = false;
  bool homeEdge = false;
  bool endEdge = false;
  bool keyYEdge = false;
  bool keyNEdge = false;
  bool f1Edge = false;
  bool f2Edge = false;
  bool f3Edge = false;
  bool f10Edge = false;
  bool f11Edge = false;
  bool f12Edge = false;
  bool pauseEdge = false;
  bool pauseAltEdge = false;
  bool utilityEdge = false;
  char typedChar = 0;
  bool leftEdge = false;
  bool rightEdge = false;
  bool nameBackspaceEdge = false;
  bool nameDeleteEdge = false;
  bool nameHomeEdge = false;
  bool nameEndEdge = false;
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
  // DAT_0049aa98 — the attract/slideshow state: -1 = disabled (the
  // first slide, MDKS_001.GIF, failed to load and the state stays -1
  // even across FUN_0041d85c re-init, which only resets values > 0);
  // 0 = menu idle; 1 = MDKS_001 on screen (menu strings hidden);
  // n >= 2 = MDKS_<n> on screen (menu strings drawn over it).
  int listState() const { return attractState_; }
  int attractState() const { return attractState_; }
  // The slide pixel payload (DAT_0049aaa0) is bound when the provider
  // reported a loadable 600x360 MDKS_<state>.GIF; cleared on failure.
  bool attractSlideActive() const { return slideActive_; }
  // DAT_0049aa98 == 1 hides the menu item strings (the FUN_0041dc90
  // draw block draws strings only when the state is not 1).
  bool menuStringsHidden() const { return attractState_ == 1; }
  float rampAccumulator() const { return ramp_.acc; }  // DAT_0054bdd8
  float smoothedDelta() const { return timing_.smoothed; }  // DAT_0049b6f0
  float deltaSeconds() const { return timing_.deltaSec; }   // DAT_0049b6f4

  // The shared input-machine block (Phase 4F): mouse position, tick,
  // repeat deadlines, button latch, ramp machine, timing struct — the
  // globals that persist unchanged across the root -> options
  // (FUN_00420cf0) and options -> root (FUN_00420d68) transitions.
  // `left/rightDeadline` are serialized but never queried at root
  // (the root menu has no LEFT/RIGHT handler).
  FrontendMachineState machineState() const;
  void setMachineState(const FrontendMachineState& s);

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

  // OBSERVED frame-termination flag: every activation-dispatch branch
  // of FUN_0041dc90 ends in RET before the idle-accumulate, attract
  // check, draw block, and timing update — a dispatched frame draws
  // nothing and does not advance the timing machine. The attract
  // trigger is NOT a dispatch: FUN_0041ef74 runs, then the draw block
  // still executes. True only for the frame in which update() hit an
  // activation branch.
  bool frameEndedEarly() const { return endedEarly_; }

  // Slide provider seam (Phase 18A): the host supplies
  //   bool probe(int slideNo)
  // answering "MISC\MDKS_<slideNo:03>.GIF exists and decodes to a
  // 600x360 image" — the FUN_0041b004 + FUN_00416e98 chain inside
  // FUN_0041ef74. The provider may also preload the slide pixels for
  // the renderer; the core only consumes the yes/no (and keeps
  // `slideActive_` for the draw override). Default provider reports
  // no slides: the first advance goes straight to state -1.
  using AttractSlideProbe = std::function<bool(int slideNo)>;
  void setAttractSlideProbe(AttractSlideProbe p);
  // FUN_0041d85c resets DAT_0049aa98 only when it is > 0 — a -1
  // (first slide missing) survives every frontend re-entry.
  void resetAttractIfPositive();
  // FUN_0041d85c re-entry on an existing controller: selection and
  // the savesExist gate re-derive, pending actions clear, and the
  // attract state resets only when > 0. The shared machine block
  // (mouse/tick/deadlines/latch/ramp/timing) and the slide provider
  // persist — the original does not touch them here.
  void resetForEntry(bool savesExist);
  // The FUN_004140f4 blend gate inside the attract check: while a
  // screen blend/transition resource (DAT_0049aa8c) is active and not
  // finished, the idle timer is held at 0 and the advance suppressed.
  // The shell/host raises this while an entry transition runs.
  void setAttractBlendActive(bool b) { attractBlend_ = b; }

private:
  void attractAdvance();  // FUN_0041ef74 state advance + load attempt
  bool savesExist_;
  int selection_;        // DAT_0049aa78
  int mouseX_;           // DAT_0054b634
  int mouseY_;           // DAT_0054b638
  int tick_ = 0;         // DAT_00541518
  float idleSeconds_ = 0.0f; // DAT_0049aaa4
  int attractState_ = 0;     // DAT_0049aa98
  bool slideActive_ = false; // DAT_0049aaa0 bound (slide pixels valid)
  bool attractBlend_ = false; // DAT_0049aa8c blend gate
  AttractSlideProbe slideProbe_; // MISC\MDKS_%03d.GIF load probe
  // Repeat deadlines (DAT_0049ac84 up, DAT_0049ac88 down).
  int prevDeadline_ = 0;
  int nextDeadline_ = 0;
  // LEFT/RIGHT repeat deadlines (DAT_0049ac8c/90) — serialized members
  // of the shared global block; the root menu never queries them, but
  // they must survive the options round-trip unchanged.
  int leftDeadline_ = 0;
  int rightDeadline_ = 0;
  // Mouse-button edge latch (DAT_0049ac80).
  bool buttonLatch_ = false;
  // Ramp machine (DAT_0054bdc8..bdd8): current/prev item keys + acc.
  FrontendRampState ramp_;
  // Timing struct fields (FUN_0042fb30 init values).
  FrontendTimingState timing_;
  FrontendAction action_ = FrontendAction::None;
  // Whether the last update() hit an activation-dispatch RET — see
  // frameEndedEarly().
  bool endedEarly_ = false;
};

// Compose one live frame from the controller state — identical
// composition to renderFrontendMenuFrame, except each item's scale comes
// from the controller's FUN_00423a24 ramp machine (called in draw order)
// and the arrow sits at the controller's logical mouse position.
// `ctl` is non-const because the scale machine mutates during the pass.
// `brightness` is DAT_0054147e — the FUN_0046d208 upload lift applied
// to the bound palette (a process global mutated by the Display
// screen; 0 on the pre-Phase-4H paths).
bool renderFrontendMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                               const IndexedImage& backdrop,
                               const FtiFont& fontBig,
                               const FtiSpriteFrame& arrow,
                               std::span<const std::string_view> optStrings,
                               FrontendMenuController& ctl,
                               int brightness,
                               std::string* err);

// The strings block of renderFrontendMenuDynamic (the FUN_0041dc90
// item loop, OBSERVED): OPT items at y = 31 + 36i, each scaled by the
// controller's ramp machine in draw order — with NO backdrop copy and
// NO palette upload. The attract path uses this when a slide is bound
// and attractState != 1: the original draws the same strings over the
// slide pixels (0x49aaa0 replaces the framebuffer before the block).
// The ARROW cursor draw stays with the caller (it runs in both the
// state==1 and state>=2 tails).
bool drawFrontendMenuItems(IndexedFramebuffer& fb,
                           const FtiFont& fontBig,
                           std::span<const std::string_view> optStrings,
                           FrontendMenuController& ctl,
                           std::string* err);

} // namespace mdk

#endif // MDK_CORE_FRONTEND_MENU_H
