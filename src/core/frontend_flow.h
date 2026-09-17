// Phase 4F — two-screen front-end flow controller.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4F):
//
//   Root -> Options (FUN_0041dc90 selection 3 dispatch):
//     FUN_00420cf0 — writes DAT_00541493 = 0x0b and _DAT_0054bd34 = 8,
//     saves the active palette into the "svlut" buffer (FUN_0046d614),
//     uploads DAT_00540820 (FUN_0046d208). No mouse/tick/ramp reset —
//     the shared globals continue on the new screen.
//
//   Options -> Root (FUN_00420eac selection 8 / DIK_ESCAPE):
//     FUN_00420d68 — persists settings when the dirty flag
//     (DAT_00541486) is set (FUN_004260ac — deferred), calls
//     FUN_00402590 to resume the front-end list, restores the saved
//     palette (FUN_0046d208 of svlut), releases svlut, and writes
//     DAT_00541493 = 0 (root mode). Again no input-state reset.
//
//   This controller owns the two reconstructed screens and performs
//   exactly those two transitions. It is deliberately NOT a generic
//   UI router — no hierarchy, no widget system.
//
#ifndef MDK_CORE_FRONTEND_FLOW_H
#define MDK_CORE_FRONTEND_FLOW_H

#include "core/frontend_menu.h"
#include "core/options_menu.h"

#include <optional>

namespace mdk {

enum class FrontendScreen {
  Root,     // FUN_0041dc90 — mode 0
  Options,  // FUN_00420eac — mode 0x0b
};

class FrontendFlowController {
public:
  // Mirrors the front-end entry (FUN_0041d85c): the root controller
  // starts in its proven entry state; the options global display
  // fields start at their canonical values (skill 0, -mapok clear).
  explicit FrontendFlowController(bool savesExist);

  FrontendScreen screen() const { return screen_; }
  bool inOptions() const { return screen_ == FrontendScreen::Options; }

  FrontendMenuController& root() { return root_; }
  const FrontendMenuController& root() const { return root_; }
  // Valid only while screen() == Options.
  OptionsMenuController& options() { return *options_; }
  const OptionsMenuController& options() const { return *options_; }

  // One frame: route the neutral input to the active controller.
  void update(const FrontendMenuInput& in);

  // Consume the active screen's semantic action. Transition-driving
  // actions are handled internally and reported as consumed:
  //   root    OpenOptions -> FUN_00420cf0 (enters options)
  //   options Back        -> FUN_00420d68 (returns to root)
  // All other actions pass through to the caller unchanged (semantic
  // events only — downstream systems deferred).
  FrontendAction consumeRootAction();
  OptionsAction consumeOptionsAction();

  // The options display globals (DAT_0054147a / DAT_005414f4) live
  // across entries — they belong to the flow, not one controller.
  int skill() const { return skill_; }
  bool devHidden() const { return devHidden_; }

private:
  void enterOptions();   // FUN_00420cf0
  void returnToRoot();   // FUN_00420d68

  FrontendMenuController root_;
  std::optional<OptionsMenuController> options_;
  FrontendScreen screen_ = FrontendScreen::Root;
  int skill_ = 0;          // DAT_0054147a — canonical 0 ("Skill - Easy")
  bool devHidden_ = false; // DAT_005414f4 — canonical 0 (no -mapok)
};

} // namespace mdk

#endif // MDK_CORE_FRONTEND_FLOW_H
