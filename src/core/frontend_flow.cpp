#include "core/frontend_flow.h"

namespace mdk {

FrontendFlowController::FrontendFlowController(bool savesExist)
    : root_(savesExist) {}

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
  options_.emplace(root_.machineState(), devHidden_, skill_);
  screen_ = FrontendScreen::Options;
}

// FUN_00420d68 (OBSERVED): mode 0 — restore the saved palette (the
// root renderer re-binds MDKOPT's embedded palette), persist settings
// only when DAT_00541486 is set (deferred — Phase 4F never mutates),
// re-enter the front-end. Root selection/idle/ramp all persist.
void FrontendFlowController::returnToRoot() {
  root_.setMachineState(options_->machineState());
  options_.reset();
  screen_ = FrontendScreen::Root;
}

} // namespace mdk
