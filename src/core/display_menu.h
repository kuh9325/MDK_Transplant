// Phase 4H — Display options child screen (FUN_0041d1e0, front-end
// mode 7) — the first real child screen of the options sub-menu.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4H and
// analysis-private/logs/phase4h_funcs.txt, disasm_41d020.txt):
//
//   Entry (FUN_0041d020, OBSERVED — reached from Options row 7 via
//   0x4210ce under the LEFT, RIGHT, or activate query; all three
//   dispatch tables bind row 7 to this entry):
//     DAT_00541493 = 7            display mode
//     DAT_0054b834 = 2            entry selection = the DSP_QUIT row
//     dlut <- tagged 0x300 buffer (DAT_0049aa70); FUN_0046d614
//             flattens the ACTIVE palette into it — the saved copy
//             the exit restores
//     slut <- tagged 0x300 buffer (DAT_0049aa74); head copied from
//             dlut[0:0xc0], tail filled with 4 x 48-entry ramps:
//             gray 64-111, red 112-159, green 160-207, blue 208-255,
//             intensity = i*255/47
//     FUN_00413b40(slut) — SYS_PAL head (DAT_00540820) + slut tail
//             uploaded via FUN_0046d208 (brightness lift applied)
//     NO mouse/tick/ramp/timing reset — the shared globals carry over
//
//   Frame (FUN_0041d1e0, OBSERVED): the same shared query helpers as
//   the options screen, in the same order:
//     prev -> next -> mouse gate -> Esc -> LEFT -> RIGHT -> activate
//     -> draw.
//     UP decrements DAT_0054b834 (wraps <0 -> 2); DOWN increments
//     (wraps >=3 -> 0). The mouse hit-test gate is the same
//     three-global check as Options; band = trunc((mouseY - 5) / 36)
//     constrained to rows 0..2. DAT_0054b570 (Esc) -> FUN_0041d144.
//     LEFT/RIGHT/activate per row:
//       row 0: LEFT brightness -1 (wraps <0 -> 7); RIGHT/activate
//              brightness +1 (wraps >=8 -> 0); each mutation latches
//              DAT_00541486 and re-uploads slut via FUN_0046d208
//              (the new lift shows immediately)
//       row 1: any of the three toggles DAT_00541482 + dirty
//       row 2: LEFT/RIGHT are no-ops (fall through to the next
//              query); activate -> FUN_0041d144 (exit)
//     LEFT and RIGHT NEVER end the frame — the original falls through
//     to the next query after every row's mutation. Esc and
//     row-2 activate RET before the draw block (frameEndedEarly).
//
//   Draw (OBSERVED): FUN_00415658 clear(0); sprintf(buf, DSP_BRGT,
//   brightness) drawn at y=31; DSP_DETH or DSP_DETL (by
//   DAT_00541482) at y=67; DSP_QUIT at y=103 — all through
//   FUN_00423b88 (FONTBIG, centered on 600, ramp key (-1,y) — the
//   same helper the options rows use). FUN_0041cf80 then draws the
//   four 48-cell swatch bands (FUN_00416aa8 inclusive rectfill:
//   cells x=60+10i..69+10i, y=200+32b..231+32b, color index
//   64+48b+i), then the ARROW at the raw logical mouse position.
//
//   Exit (FUN_0041d144, OBSERVED): DAT_00541493 = 0x0b (back to the
//   options screen), FUN_0046d208(0,0x100,dlut) re-uploads the
//   palette saved at entry, both tagged buffers are freed. The
//   options selection _DAT_0054bd34 is untouched — it stays 7 (the
//   Display row). Mouse/ramp/timing all carry back. The dirty flag
//   persists for the eventual options exit (FUN_00420d68 handles
//   persistence — display exit does NOT persist).
//
#ifndef MDK_CORE_DISPLAY_MENU_H
#define MDK_CORE_DISPLAY_MENU_H

#include "core/frontend_machines.h"

#include <span>
#include <string>
#include <string_view>

namespace mdk {

class IndexedFramebuffer;
class Palette;
struct FtiFont;
struct FtiSpriteFrame;
struct FrontendMenuInput;

// OBSERVED display item table (FUN_0041d1e0 draw block).
inline constexpr int kDisplayItemCount = 3;       // rows 0..2
inline constexpr int kDisplayItemY0 = 31;         // 0x1f
inline constexpr int kDisplayItemStep = 36;       // 0x24
inline constexpr int kDisplayEntrySelection = 2;  // DAT_0054b834 = 2
// OBSERVED hit-test constants: band = trunc((mouseY - 5) / 36).
inline constexpr int kDisplayHitBandBase = 5;     // 0x5
inline constexpr int kDisplayHitBandSize = 36;    // 0x24
// Same centered-x/FONTBIG/ramp machinery as the options rows
// (FUN_00423b88 -> FUN_0041518c -> FUN_00423a24 keyed (-1, y)).
inline constexpr int kDisplayCenterWidth = 600;
inline constexpr int kDisplayRampKeyX = -1;
// OBSERVED brightness domain (DAT_0054147e): LEFT wraps <0 -> 7,
// RIGHT/activate wrap >=8 -> 0.
inline constexpr int kDisplayBrightnessMax = 7;
// OBSERVED swatch grid (FUN_0041cf80 -> FUN_00416aa8 inclusive fill).
inline constexpr int kDisplaySwatchBands = 4;
inline constexpr int kDisplaySwatchCells = 48;
inline constexpr int kDisplaySwatchX0 = 60;       // 0x3c
inline constexpr int kDisplaySwatchX1 = 69;       // 0x45 (inclusive)
inline constexpr int kDisplaySwatchXStep = 10;    // 0xa
inline constexpr int kDisplaySwatchY0 = 200;      // 0xc8
inline constexpr int kDisplaySwatchY1 = 231;      // 0xe7 (inclusive)
inline constexpr int kDisplaySwatchYStep = 32;    // 0x20
inline constexpr int kDisplaySwatchPalBase = 64;  // 0x40
inline constexpr int kDisplaySwatchPalStride = 48;// 0x30
// OBSERVED ramp intensity: entry i of a band = i*255/47 per channel.
inline constexpr int kDisplaySwatchRampMax = 47;

// FTI record names (OBSERVED at 0x495924..0x495950 in MDK95.EXE).
// DSP_BRGT's text is itself the row-0 printf format ("Brightness %d").
inline constexpr const char* kDisplayBrightnessRecord = "DSP_BRGT";
inline constexpr const char* kDisplayDetailHighRecord = "DSP_DETH";
inline constexpr const char* kDisplayDetailLowRecord = "DSP_DETL";
inline constexpr const char* kDisplayQuitRecord = "DSP_QUIT";

// Resolved label strings for one frame. `brightnessFmt` is the
// DSP_BRGT record text — drawn through sprintf with the brightness
// value exactly like the original's FUN_0047d2e9 call.
struct DisplayMenuLabels {
  std::string_view brightnessFmt;  // DSP_BRGT — a printf format
  std::string_view detailHigh;     // DSP_DETH
  std::string_view detailLow;      // DSP_DETL
  std::string_view quit;           // DSP_QUIT
};

// The frozen frame state for the static preview (OBSERVED entry
// register values): selection 2 at scale 1.0, the rest 0.65,
// canonical post-config brightness 0 / ForcePCorrect FALSE, ARROW
// at the (unchanged) logical mouse position.
struct DisplayMenuSpec {
  int selection = kDisplayEntrySelection;
  int brightness = 0;       // DAT_0054147e canonical 0 (mirror @0x49b272)
  bool forcePCorrect = false;  // DAT_00541482 canonical FALSE (@0x49b276)
  int arrowX = 300;         // logical mouse — NOT reset on entry
  int arrowY = 180;
};

// Semantic outputs of FUN_0041d1e0 — the only terminal action is the
// exit (FUN_0041d144 -> mode 0x0b), reached by Esc or by activating
// row 2. Setting mutations apply in place and are never emitted.
enum class DisplayAction {
  None = 0,
  Back,  // -> FUN_0041d144: restore saved palette, mode 0x0b
};

// The reconstructed controller — FUN_0041d1e0 input/selection block.
// Same frame protocol as the options controller:
//     update(input)   — FUN_004187e0 accumulate + tick advance +
//                       FUN_0041d1e0 queries/mutations
//     itemScale(...)  — FUN_00423a24 keyed (-1, y), called by the
//                       renderer once per drawn row in draw order
//     endFrame(dtMs)  — FUN_0042fe78/FUN_0042fcd0 timing update
class DisplayMenuController {
public:
  // Mirrors FUN_0041d020: selection 2; everything else carries over
  // from the shared machine state `s` (mouse, tick, deadlines, latch,
  // ramp, timing). `brightness` is DAT_0054147e, `forcePCorrect` is
  // DAT_00541482 — process globals seeded from the post-config state;
  // `settingsDirty` is DAT_00541486 (carried over from the parent
  // options screen — one shared global in the original).
  DisplayMenuController(const FrontendMachineState& s, int brightness,
                        bool forcePCorrect, bool settingsDirty = false);

  int selection() const { return selection_; }     // DAT_0054b834
  int brightness() const { return brightness_; }   // DAT_0054147e
  bool forcePCorrect() const { return forcePCorrect_; }  // DAT_00541482
  bool settingsDirty() const { return settingsDirty_; }  // DAT_00541486
  int mouseX() const { return m_.mouseX; }
  int mouseY() const { return m_.mouseY; }
  int tick() const { return m_.tick; }
  float rampAccumulator() const { return m_.ramp.acc; }
  float smoothedDelta() const { return m_.timing.smoothed; }

  const FrontendMachineState& machineState() const { return m_; }

  // Per-frame update in the original order:
  //   prev -> next -> mouse hit-test -> Esc -> LEFT -> RIGHT ->
  //   activate. LEFT/RIGHT mutate in place and ALWAYS continue to the
  //   next query (the original's branches jump to the next query, not
  //   the epilogue); Esc and row-2 activate end the frame before the
  //   draw block.
  void update(const FrontendMenuInput& in);

  // FUN_00423a24 keyed (-1, itemY) — called per drawn row in draw
  // order by the renderer.
  float itemScale(int itemY, bool selFlag);

  // FUN_0042fe78 timing update — same body as the options endFrame.
  void endFrame(double dtMs);

  // Pending semantic action from the last dispatch.
  DisplayAction pendingAction() const { return action_; }
  DisplayAction consumeAction();

  // OBSERVED frame-termination flag: the Esc branch and row-2
  // activate end in RET before the draw block and timing update —
  // a dispatched frame draws nothing and does not advance the timing
  // machine. Mutations never set this.
  bool frameEndedEarly() const { return endedEarly_; }

private:
  FrontendMachineState m_;   // the shared globals block
  int selection_;            // DAT_0054b834
  int brightness_;           // DAT_0054147e — mutated by row 0
  bool forcePCorrect_;       // DAT_00541482 — toggled by row 1
  bool settingsDirty_;       // DAT_00541486
  DisplayAction action_ = DisplayAction::None;
  bool endedEarly_ = false;
};

// Compose the proven static frame in the original draw order:
// palette bind (SYS_PAL head + 4 ramps + brightness lift) ->
// clear(0) -> rows (y 31/67/103, centered on 600, scale 1.0
// selected / 0.65 otherwise) -> swatch grid -> ARROW at the mouse.
// `sysPalHead` must be the 192-byte SYS_PAL record head — the same
// resident palette head the options screen binds.
// Returns false with `err` on contract violations.
bool renderDisplayMenuFrame(IndexedFramebuffer& fb, Palette& palette,
                            const FtiFont& fontBig,
                            const FtiSpriteFrame& arrow,
                            const DisplayMenuLabels& labels,
                            std::span<const std::byte> sysPalHead,
                            const DisplayMenuSpec& spec,
                            std::string* err);

// Same composition driven by the live controller: per-row scale from
// the FUN_00423a24 ramp (keyed -1,y, called in draw order), the
// controller's brightness/ForcePCorrect/selection, ARROW at the
// controller's logical mouse.
bool renderDisplayMenuDynamic(IndexedFramebuffer& fb, Palette& palette,
                              const FtiFont& fontBig,
                              const FtiSpriteFrame& arrow,
                              const DisplayMenuLabels& labels,
                              std::span<const std::byte> sysPalHead,
                              DisplayMenuController& ctl,
                              std::string* err);

} // namespace mdk

#endif // MDK_CORE_DISPLAY_MENU_H
