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

} // namespace mdk

#endif // MDK_CORE_FRONTEND_MENU_H
