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
//   Options -> Root (FUN_00420eac selection 8 activate / DIK_ESCAPE —
//   LEFT/RIGHT on row 8 are bound-gated no-ops):
//     FUN_00420d68 — writes DAT_00541493 = 0 (root mode), persists
//     settings when the dirty flag (DAT_00541486) is set — Phase 4G
//     routes that FUN_004260ac call through the native-owned
//     SettingsPersistSink seam below (frontend_settings.h), never
//     into the data root — runs the DAT_00541492-gated
//     FUN_00402590/FUN_004348d4 calls, restores the saved palette
//     (FUN_0046d208 of svlut), and releases svlut. Again no
//     input-state reset.
//
//   This controller owns the two reconstructed screens and performs
//   exactly those two transitions. It is deliberately NOT a generic
//   UI router — no hierarchy, no widget system.
//
#ifndef MDK_CORE_FRONTEND_FLOW_H
#define MDK_CORE_FRONTEND_FLOW_H

#include "core/frontend_menu.h"
#include "core/frontend_settings.h"
#include "core/options_menu.h"

#include <functional>
#include <optional>

namespace mdk {

enum class FrontendScreen {
  Root,     // FUN_0041dc90 — mode 0
  Options,  // FUN_00420eac — mode 0x0b
};

// Phase 4G persistence seam — the native-owned counterpart of
// FUN_004260ac. Invoked by returnToRoot() ONLY when the dirty flag
// (DAT_00541486) was latched, mirroring the original's TEST/JNZ
// gate; the flag then clears unconditionally (0x420dbc — the
// original clears after the call regardless of the write's
// success, and FUN_004260ac itself returns silently when its fopen
// fails). Return value: whether the write succeeded — observable
// by the caller for diagnostics; the flow does not branch on it.
using SettingsPersistSink = std::function<bool(const FrontendSettings&)>;

class FrontendFlowController {
public:
  // Mirrors the front-end entry (FUN_0041d85c): the root controller
  // starts in its proven entry state; the options globals start at
  // the post-config values `initial` carries (the FUN_00425de4
  // defaults copy + config application — canonical BUILD_A result:
  // skill 1, dirty clear; the native -mapok flag is not yet modeled
  // and stays clear). `sink` is the Phase 4G persistence seam; with
  // no sink installed the dirty-flag lifecycle still runs but no
  // write is attempted (like FUN_004260ac's silent fopen failure).
  explicit FrontendFlowController(bool savesExist,
                                  const FrontendSettings& initial = {},
                                  SettingsPersistSink sink = {});

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
  // DAT_00541486 — the shared settings-dirty flag (see returnToRoot).
  bool settingsDirty() const { return settingsDirty_; }

private:
  void enterOptions();   // FUN_00420cf0
  void returnToRoot();   // FUN_00420d68

  FrontendMenuController root_;
  std::optional<OptionsMenuController> options_;
  FrontendScreen screen_ = FrontendScreen::Root;
  // DAT_0054147a — the post-config startup value (`initial`): the
  // FUN_00425de4 defaults copy yields factory 1 ("Skill - Normal"),
  // then the config applies `Skill = n` overrides. Mutations inside
  // the options screen write back here — it is a process global,
  // not per-screen state.
  int skill_;
  bool devHidden_ = false;      // DAT_005414f4 — canonical 0 (no -mapok)
  bool settingsDirty_ = false;  // DAT_00541486 — canonical 0 (inside
                                // the factory-defaults copy block)
  SettingsPersistSink persistSink_;  // FUN_004260ac seam — see above
};

} // namespace mdk

#endif // MDK_CORE_FRONTEND_FLOW_H
