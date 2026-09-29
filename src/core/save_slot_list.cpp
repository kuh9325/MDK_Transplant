#include "core/save_slot_list.h"

#include <cctype>
#include <utility>

namespace mdk {

std::string saveListStem(std::string_view filename) {
  // FUN_004202cc name copy loop (OBSERVED): stop at the first space
  // or period; keep at most 8 characters (the 9-byte record + NUL).
  std::string out;
  for (const char c : filename) {
    if (c == ' ' || c == '.' ||
        out.size() >= static_cast<std::size_t>(kSaveListMaxName)) {
      break;
    }
    out.push_back(c);
  }
  return out;
}

SaveSlotListController::SaveSlotListController(
    std::vector<std::string> stems, SaveSlotInspector inspect, int sel)
    : stems_(std::move(stems)), inspect_(std::move(inspect)) {
  // DAT_0049ac28 carries across list re-entries; FUN_004202cc keeps
  // it when still inside the new count, else resets to 0.
  sel_ = (sel >= 0 && sel < count()) ? sel : 0;
  inspectedSel_ = -1;
  inspectSelected();
}

SaveListAction SaveSlotListController::consumeAction() {
  const SaveListAction a = action_;
  action_ = SaveListAction::None;
  return a;
}

const SaveSlotSummary* SaveSlotListController::selected() const {
  return (sel_ >= 0 && sel_ < count()) ? &inspected_ : nullptr;
}

void SaveSlotListController::inspectSelected() {
  // FUN_00428144: header inspect runs lazily when the selected row
  // changes; DAT_0049ac40 is the validity flag.
  if (sel_ < 0 || sel_ >= count()) {
    inspectedSel_ = sel_;
    inspected_ = {};
    inspectedValid_ = false;
    return;
  }
  if (sel_ == inspectedSel_) {
    return;
  }
  inspected_ = {};
  inspected_.name = stems_[sel_];
  if (inspect_) {
    if (auto s = inspect_(stems_[sel_])) {
      inspected_ = std::move(*s);
      inspected_.name = stems_[sel_];
      inspectedValid_ = inspected_.valid;
    }
  }
  inspected_.valid = inspectedValid_;
  inspectedSel_ = sel_;
}

void SaveSlotListController::update(FrontendMachineState& sh,
                                    const FrontendMenuInput& in) {
  action_ = SaveListAction::None;
  actionStem_.clear();

  const int n = count();

  // The per-frame mouse accumulate (FUN_004187e0) ran at the loop
  // head — the caller owns it. The handler then applies the
  // 590/350 hit-test clamps to the live position (OBSERVED).
  if (sh.mouseX > kSaveListHitClampX) sh.mouseX = kSaveListHitClampX;
  if (sh.mouseY > kSaveListHitClampY) sh.mouseY = kSaveListHitClampY;

  // --- navigation (DAT_0049ac28 clamps; no wrap) -------------------
  if (frontendRepeatQuery(sh.tick, in.prevHeld, sh.prevDeadline)) {
    if (sel_ > 0) --sel_;
  }
  if (frontendRepeatQuery(sh.tick, in.nextHeld, sh.nextDeadline)) {
    if (sel_ < n - 1) ++sel_;
  }
  if (in.homeEdge) {
    sel_ = 0;
  }
  if (in.endEdge) {
    sel_ = n - 1;
  }
  if (in.pageUpEdge) {
    sel_ -= kSaveListPageStep;
    if (sel_ < 0) sel_ = 0;
  }
  if (in.pageDownEdge && n > 0) {
    sel_ += kSaveListPageStep;
    if (sel_ >= n) sel_ = n - 1;
  }

  // Typed-character first-letter jump (OBSERVED): walks entries
  // 0..count-2 comparing the first byte against the translated char;
  // stops at the first entry whose first byte >= typed.
  if (in.typedChar != 0 && n > 0) {
    const unsigned char t =
        static_cast<unsigned char>(std::toupper(in.typedChar));
    sel_ = 0;
    for (int i = 0; i < n - 1; ++i) {
      const unsigned char first = static_cast<unsigned char>(
          std::toupper(stems_[i].empty() ? '\0' : stems_[i][0]));
      if (first >= t) break;
      sel_ = i + 1;
    }
  }

  // Mouse band + edge scroll (OBSERVED FUN_004206d0). Any mouse
  // activity arms the DAT_0054bd28 tracker for the edge scroll.
  const bool mouseMoved =
      in.mouseDx != 0 || in.mouseDy != 0 || in.mouseButtons != 0;
  if (mouseMoved) {
    mouseTrack_ = true;
  }
  if (mouseMoved && n > 0 &&
      sh.mouseY > kSaveListBandTop && sh.mouseY < kSaveListBandBottom) {
    int sel = ((sh.mouseY - kSaveListRowBase) / kSaveListRowStep) +
              topRow_;
    if (sel >= n) sel = n - 1;
    sel_ = sel;
  }
  if (mouseTrack_ && n > 0) {
    if (sel_ >= 1 && sh.mouseY <= kSaveListBandTop) {
      --sel_;  // hover at/above the top band edge scrolls up
    } else if (sel_ < n - 1 && sh.mouseY > kSaveListBandBottom - 1) {
      ++sel_;  // hover past the bottom band edge scrolls down
    }
  }

  // Window follow (OBSERVED): keep sel inside [top, top+12].
  if (topRow_ + kSaveListPageStep <= sel_) {
    topRow_ = sel_ - (kSaveListPageStep - 1);
  }
  if (sel_ < topRow_) {
    topRow_ = sel_;
  }

  inspectSelected();

  // --- confirm / cancel -------------------------------------------
  // FUN_00423764: Enter edge OR the left-button edge latch.
  const bool confirm =
      frontendConfirmQuery(sh, in.confirmEdge, in.mouseButtons);
  if (confirm) {
    mouseTrack_ = false;
    if (n > 0 && inspectedValid_) {
      action_ = SaveListAction::Load;
      actionStem_ = stems_[sel_];
    } else {
      // OBSERVED quirk: activating an invalid/empty slot exits the
      // list rather than staying.
      action_ = SaveListAction::Exit;
    }
    return;
  }
  if (in.cancelEdge) {
    action_ = SaveListAction::Exit;
  }
}

}  // namespace mdk
