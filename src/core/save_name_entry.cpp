#include "core/save_name_entry.h"

#include <cctype>
#include <utility>

namespace mdk {

// The charset test in FUN_00422dec (OBSERVED): the Watcom
// character-class table at DAT_0049bd54 (indexed c+1) passes
// alphabetic (0xc0 bits = upper|lower) and digit (0x20) characters
// straight through; anything else must appear in the explicit
// allowlist string at DAT_00495f88 = "_$".
static bool saveNameCharOk(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (std::isalnum(u)) return true;
  return c == '_' || c == '$';
}

SaveNameEntryController::SaveNameEntryController(
    std::string prefill, bool confirmPhase, bool headerOnly,
    SaveNameWriter writer)
    : name_(std::move(prefill)), cursor_(0),
      confirmPhase_(confirmPhase), headerOnly_(headerOnly),
      writer_(std::move(writer)) {
  if (name_.size() > kSaveNameMaxLen) {
    name_.resize(kSaveNameMaxLen);
  }
  // FUN_00422bc0 arm: DAT_0054bda0 = strlen(DAT_0049ac6c).
  cursor_ = static_cast<int>(name_.size());
}

SaveNameAction SaveNameEntryController::consumeAction() {
  const SaveNameAction a = action_;
  action_ = SaveNameAction::None;
  return a;
}

void SaveNameEntryController::update(FrontendMachineState& sh,
                                     const FrontendMenuInput& in,
                                     int /*tick*/) {
  action_ = SaveNameAction::None;
  writeFailed_ = false;

  // Esc (DAT_0054b570) cancels from either phase — checked first.
  if (in.cancelEdge) {
    action_ = SaveNameAction::Cancel;
    return;
  }

  // The per-frame mouse accumulate (FUN_004187e0) ran at the loop
  // head — the caller owns it. The handler applies the 590/350
  // hit-test clamps to the live position (OBSERVED).
  if (sh.mouseX > kFrontendHitClampX) sh.mouseX = kFrontendHitClampX;
  if (sh.mouseY > kFrontendHitClampY) sh.mouseY = kFrontendHitClampY;

  if (confirmPhase_) {
    // "SAVE CURRENT POSITION?" — 2-item vertical list (SV_ASK prompt
    // with ABORT2/ABORT3 yes/no records). DAT_0054bda8 wraps 0<->1.
    if (frontendRepeatQuery(sh.tick, in.prevHeld, sh.prevDeadline)) {
      confirmSel_ = (confirmSel_ - 1 < 0) ? 1 : confirmSel_ - 1;
    }
    if (frontendRepeatQuery(sh.tick, in.nextHeld, sh.nextDeadline)) {
      confirmSel_ = (confirmSel_ + 1 > 1) ? 0 : confirmSel_ + 1;
    }
    if (in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0) {
      const int band =
          (sh.mouseY - kSaveNameConfirmBandY) / kSaveNameConfirmBandStep;
      if (band >= 0 && band < kSaveNameConfirmItems) {
        confirmSel_ = band;
      }
    }
    if (frontendConfirmQuery(sh, in.confirmEdge, in.mouseButtons)) {
      if (confirmSel_ == 0) {
        confirmPhase_ = false;  // DAT_0054bd9c = 1 -> typing phase
      } else {
        action_ = SaveNameAction::Cancel;
      }
    }
    return;
  }

  // --- typing phase -------------------------------------------------
  if (in.leftEdge && cursor_ > 0) {
    --cursor_;  // DAT_0054b550
  }
  if (in.rightEdge) {  // DAT_0054b554 — cursor = min(strlen, cursor+1)
    const int len = static_cast<int>(name_.size());
    if (cursor_ < len) ++cursor_;
  }
  if (in.nameBackspaceEdge && cursor_ > 0) {
    // Shift name[cursor..] one cell left into cursor-1, then cursor--.
    name_.erase(static_cast<std::size_t>(cursor_ - 1), 1);
    --cursor_;
  }
  if (in.nameDeleteEdge && cursor_ < static_cast<int>(name_.size())) {
    // Delete at cursor (needs name[cursor] != NUL — OBSERVED gate).
    name_.erase(static_cast<std::size_t>(cursor_), 1);
  }
  if (in.nameHomeEdge) {
    cursor_ = 0;
  }
  if (in.nameEndEdge) {
    cursor_ = static_cast<int>(name_.size());
  }
  const char c = in.typedChar;
  if (c != 0 && saveNameCharOk(c) && cursor_ < kSaveNameMaxLen) {
    // OBSERVED: overwrite at the cursor — no insert shift; when the
    // target cell was NUL the byte after is re-terminated first.
    if (cursor_ == static_cast<int>(name_.size())) {
      name_.push_back('\0');  // name[cursor+1] = 0 then overwrite
    }
    name_[static_cast<std::size_t>(cursor_)] = c;
    if (name_.back() == '\0') name_.pop_back();
    ++cursor_;
  }

  // Enter (raw DAT_0054b574 edge — not the latched FUN_00423764):
  // requires cursor_ >= 1 (OBSERVED quirk — the check is on the
  // cursor position, not the name length). Write success tears the
  // dialog down; failure keeps it open (error box shown by writer).
  if (in.confirmEdge && cursor_ >= 1) {
    const bool ok = writer_ ? writer_(name_, headerOnly_) : true;
    if (ok) {
      action_ = SaveNameAction::Commit;
    } else {
      writeFailed_ = true;
    }
  }
}

}  // namespace mdk
