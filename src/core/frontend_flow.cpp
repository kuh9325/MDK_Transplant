#include "core/frontend_flow.h"

namespace mdk {

FrontendFlowController::FrontendFlowController(
    bool savesExist, const FrontendSettings& initial,
    SettingsPersistSink sink)
    : root_(savesExist), skill_(initial.skill),
      persistSink_(std::move(sink)) {}

void FrontendFlowController::update(const FrontendMenuInput& in) {
  if (screen_ == FrontendScreen::Options) {
    options_->update(in);
  } else {
    root_.update(in);
  }
}

FrontendAction FrontendFlowController::consumeRootAction() {
  if (screen_ == FrontendScreen::Root &&
      root_.pendingAction() == FrontendAction::OpenOptions) {
    root_.consumeAction();
    enterOptions();
    return FrontendAction::None;  // consumed by the transition
  }
  return root_.consumeAction();
}

OptionsAction FrontendFlowController::consumeOptionsAction() {
  if (!options_) {
    return OptionsAction::None;
  }
  if (options_->pendingAction() == OptionsAction::Back) {
    options_->consumeAction();
    returnToRoot();
    return OptionsAction::None;  // consumed by the transition
  }
  return options_->consumeAction();
}

// FUN_00420cf0 (OBSERVED): mode 0x0b, _DAT_0054bd34 = 8 — plus the
// palette swap (saved via svlut / DAT_00540820 uploaded, handled by
// the renderers binding SYS_PAL vs the MDKOPT palette). The shared
// input-machine globals carry over untouched.
void FrontendFlowController::enterOptions() {
  options_.emplace(root_.machineState(), devHidden_, skill_,
                   settingsDirty_);
  screen_ = FrontendScreen::Options;
}

// FUN_00420d68 (OBSERVED, disasm_20d68.txt): DAT_00541493 = 0 first,
// then `TEST DAT_00541486` — set: FUN_004260ac persists the changed
// settings (`Skill = %d` iff != factory 1) and the flag clears
// unconditionally right after the call (0x420dbc — even when the
// write fails, since the writer's fopen-failure path returns
// silently); clear: skip the write entirely. The DAT_00541492 mode
// checks and svlut palette restore follow. Root selection/mouse/ramp
// all carry back; DAT_0054147a keeps its mutated value — it is a
// process global, not per-screen state.
void FrontendFlowController::returnToRoot() {
  skill_ = options_->skill();
  if (options_->settingsDirty()) {
    // FUN_004260ac — Phase 4G native-owned persistence seam. The
    // sink sees the post-config settings (delta serialization is
    // its business); the flag clears after the attempt regardless
    // of the sink's result, mirroring the original exactly.
    if (persistSink_) {
      FrontendSettings s;
      s.skill = skill_;
      persistSink_(s);
    }
    settingsDirty_ = false;
  }
  root_.setMachineState(options_->machineState());
  options_.reset();
  screen_ = FrontendScreen::Root;
}

} // namespace mdk
