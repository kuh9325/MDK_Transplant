#include "core/abort_console.h"

namespace mdk {

AbortAction AbortConsoleController::consumeAction() {
  const AbortAction a = action_;
  action_ = AbortAction::None;
  return a;
}

void AbortConsoleController::update(FrontendMachineState& sh,
                                    const FrontendMenuInput& in) {
  action_ = AbortAction::None;

  // Prev/next repeat wrap DAT_004a1e28 0<->1 (FUN_004237b4/838).
  if (frontendRepeatQuery(sh.tick, in.prevHeld, sh.prevDeadline)) {
    sel_ = (sel_ - 1 < 0) ? 1 : sel_ - 1;
  }
  if (frontendRepeatQuery(sh.tick, in.nextHeld, sh.nextDeadline)) {
    sel_ = (sel_ + 1 > 1) ? 0 : sel_ + 1;
  }

  // The per-frame mouse accumulate (FUN_004187e0) ran at the loop
  // head — the caller owns it. This handler applies the 590/350
  // hit-test clamps to the live position, then the OBSERVED mouse
  // band trunc((y - 0x95) / 0x24) accepted in [0,2).
  if (sh.mouseX > kFrontendHitClampX) sh.mouseX = kFrontendHitClampX;
  if (sh.mouseY > kFrontendHitClampY) sh.mouseY = kFrontendHitClampY;
  if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
    const int band = (sh.mouseY - kAbortBandY) / kAbortBandStep;
    if (band >= 0 && band < kAbortItemCount) {
      sel_ = band;
    }
  }

  // FUN_00423764 confirm: item 0 -> yes, item 1 -> no.
  if (frontendConfirmQuery(sh, in.confirmEdge, in.mouseButtons)) {
    action_ = (sel_ == 0) ? AbortAction::Yes : AbortAction::No;
    return;
  }
  // Direct keys: Y edge -> yes; N edge or Esc -> no.
  if (in.keyYEdge) {
    action_ = AbortAction::Yes;
  } else if (in.keyNEdge || in.cancelEdge) {
    action_ = AbortAction::No;
  }
}

}  // namespace mdk
