// Phase 18A — save name-entry / confirm overlay (sub-mode 8).
//
// EVIDENCE (instruction-level, original binary):
//
//   FUN_00422bc0 — the arm (F2 manual path and the inter-level
//   autosave callers). Gate (arg ESI == 0, manual): primary mode == 3
//   AND sub-mode == 0 AND DAT_00540e9c == 0 AND DAT_00540d9c == 0 AND
//   no live object carries the X_STRIKE tag (two entity lists are
//   walked; a hit aborts the arm silently). Any nonzero arg bypasses
//   every check — the autosave callers rely on that.
//
//   Arm effects (OBSERVED): DAT_0054bda4 = DAT_00541490 >> 24 (saved
//   restore byte), DAT_00541493 = 8, DAT_0054151c = 0 (the "running"
//   flag — freezes the world), DAT_0054bdb0/0x54bdb4 = the raw arg
//   (0 manual / nonzero auto; FUN_00422d84 treats nonzero as the
//   header-only write flag), FUN_00402510 pause-sounds when the arm
//   ran in a non-frontend mode with sub-mode 0, the thumbnail grab
//   FUN_00427e8c(0x49f010) runs AT ARM TIME (the THMB pixels are
//   captured when the dialog opens, not at write), and:
//     manual: DAT_0049ac6c keeps its previous bytes (no prefill),
//             DAT_0054bd9c = 1 (straight to typing)
//     auto:   sprintf(DAT_0049ac6c, "%d", levelIndex + 1),
//             DAT_0054bd9c = 0 ("SAVE CURRENT POSITION?" confirm
//             phase first), DAT_0054bda8 = 0
//     both:   DAT_0054bda0 = strlen(DAT_0049ac6c) — cursor at end
//
//   FUN_00422dec — the per-frame handler. Esc (DAT_0054b570) cancels
//   from either phase. Confirm phase (0x54bd9c == 0): 2-item vertical
//   list, prev/next repeat wrap the DAT_0054bda8 index, the mouse
//   band is trunc((y - 0x95) / 0x24) accepted in [0,2), FUN_00423764
//   confirm: item 0 -> typing phase, item 1 -> cancel teardown.
//   Typing phase (0x54bd9c == 1):
//     left edge (0x54b550)  — cursor-- (floor 0)
//     right edge (0x54b554) — cursor = min(strlen, cursor + 1)
//     key 0x0e (backspace)  — shift name[cursor..] left into
//                             cursor-1, then cursor-- (needs > 0)
//     key 0x6f (delete)     — shift name[cursor+1..] left at cursor
//                             (needs name[cursor] != NUL)
//     key 0x66 (home)       — cursor = 0
//     key 0x6b (end)        — cursor = strlen
//     translated char       — charset check then OVERWRITE at cursor
//                             (no insert shift): if the cell was NUL
//                             the byte after is re-terminated; then
//                             cursor++ — capped at cursor < 8.
//     Enter edge (0x54b574) — fires only when cursor >= 1 (not when
//                             the name is merely nonempty — cursor 0
//                             with text blocks Enter, OBSERVED quirk);
//                             FUN_00422d84 writes the save; success
//                             tears down, failure (FUN_00427ed4 == 0,
//                             error box shown) keeps the dialog open.
//   Teardown restores DAT_00541493 = DAT_0054bda4.
//
//   Cursor blink: drawn when (DAT_00541518 & 8) != 0 — the shared
//   tick; the controller is handed the shell tick each frame.
//
#ifndef MDK_CORE_SAVE_NAME_ENTRY_H
#define MDK_CORE_SAVE_NAME_ENTRY_H

#include "core/frontend_machines.h"
#include "core/frontend_menu.h"

#include <array>
#include <functional>
#include <string>
#include <string_view>

namespace mdk {

// OBSERVED constants.
inline constexpr int kSaveNameMaxLen = 8;         // DAT_0049ac6c cap
inline constexpr int kSaveNameConfirmBandY = 0x95;
inline constexpr int kSaveNameConfirmBandStep = 0x24;
inline constexpr int kSaveNameConfirmItems = 2;
inline constexpr int kSaveNameCursorBlinkMask = 8; // tick & 8

enum class SaveNameAction {
  None = 0,
  Commit,  // Enter + cursor >= 1 + write succeeded — teardown
  Cancel,  // Esc, or the confirm-phase "no" item — teardown
};

// The write seam (FUN_00422d84 -> FUN_00427ed4): the host performs the
// actual SAVES\<name>.SAV write; return value = the original's write
// success flag. `headerOnly` is DAT_0054bdb4 != 0 (auto arm).
using SaveNameWriter =
    std::function<bool(std::string_view name, bool headerOnly)>;

class SaveNameEntryController {
public:
  // `prefill` is the initial DAT_0049ac6c contents ("" for manual,
  // "<levelIndex+1>" for the autosave arm — computed by the caller).
  // `confirmPhase` = DAT_0054bd9c == 0 arm (autosave asks first).
  // `headerOnly` = DAT_0054bdb4 != 0.
  SaveNameEntryController(std::string prefill, bool confirmPhase,
                          bool headerOnly, SaveNameWriter writer);

  const std::string& name() const { return name_; }  // DAT_0049ac6c
  int cursor() const { return cursor_; }             // DAT_0054bda0
  int confirmSelection() const { return confirmSel_; } // 0x54bda8
  bool confirmPhase() const { return confirmPhase_; } // 0x54bd9c==0
  bool headerOnly() const { return headerOnly_; }
  bool cursorBlink(int tick) const { return (tick & kSaveNameCursorBlinkMask) != 0; }

  SaveNameAction pendingAction() const { return action_; }
  SaveNameAction consumeAction();
  bool writeFailed() const { return writeFailed_; }  // last Enter attempt failed

  // `tick` is the shared DAT_00541518 tick (blink phase).
  void update(FrontendMachineState& sh, const FrontendMenuInput& in,
              int tick);

private:
  std::string name_;          // DAT_0049ac6c (fixed 9-byte record)
  int cursor_ = 0;            // DAT_0054bda0
  int confirmSel_ = 0;        // DAT_0054bda8
  bool confirmPhase_ = false; // runs while DAT_0054bd9c == 0
  bool headerOnly_ = false;   // DAT_0054bdb4
  SaveNameWriter writer_;
  bool writeFailed_ = false;
  SaveNameAction action_ = SaveNameAction::None;
};

}  // namespace mdk

#endif  // MDK_CORE_SAVE_NAME_ENTRY_H
