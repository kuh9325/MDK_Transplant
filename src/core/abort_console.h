// Phase 18A — the abort console overlay (sub-mode 9).
//
// EVIDENCE (instruction-level, original binary):
//
//   FUN_004030a8 — arm (Esc in the main loop, or the F10 binding
//   edge). Guard: returns early only when DAT_00541514 != 0 while in
//   the frontend (mode 0) — 0x541514 is always 0 in the normal
//   startup path, so the console arms even at the frontend menu.
//   Arm effects: DAT_004a1e28 = 0 (selection starts on YES),
//   DAT_00541493 = 9, FUN_00402510 pause-sounds (unconditional),
//   the ABORT record group loads (DAT_00494280 / 0x499a14), palette
//   save + upload (FUN_0046d614/FUN_0046d208).
//
//   FUN_00403264 — the per-frame handler: 2-item vertical list;
//   prev/next repeat queries wrap DAT_004a1e28 0<->1; the mouse band
//   is trunc((y - 0x95) / 0x24) accepted in [0,2) (same geometry as
//   the save confirm); FUN_00423764 confirm dispatches item 0 to
//   FUN_004031b8 (yes) and item 1 to FUN_0040316c (no). DAT_0054b5ac
//   (the Y key edge) jumps straight to yes; DAT_0054b594 (N edge)
//   OR DAT_0054b570 (Esc edge) to no. The draw is the three
//   ABORT1/2/3 records + the ARROW at the mouse position.
//
//   FUN_004031b8 (yes) — resume sounds (FUN_004025d0), dialog
//   teardown (FUN_00403124(0)), sub-mode 0, then:
//     mode 0 (frontend): FUN_0041dbd4 cleanup + DAT_0054148e = 1
//                        — aborting AT THE MENU quits the game;
//     else: per-mode teardown (mode 1 -> FUN_00418de4, 2 ->
//           FUN_0040fa68, 3 -> FUN_004371bc, 5 -> FUN_0042c824,
//           6 -> FUN_004295c4, 8 -> FUN_0047b0d8) and
//           FUN_0041d85c(0) — a FRESH frontend entry (the mode
//           teardown freed shared resources, so the frontend must
//           reload: not the arg-1 returning path).
//
//   FUN_0040316c (no) — sub-mode 0, resume sounds (FUN_00402590),
//   teardown, then:
//     mode 0: FUN_0041dbd4 + FUN_0041d85c(0) — canceling at the
//             frontend re-enters it fresh;
//     mode 3: FUN_004348d4 — the traversal resume (unfreeze);
//     other modes: nothing further — sub-mode 0 already resumed
//             the mode.
//
#ifndef MDK_CORE_ABORT_CONSOLE_H
#define MDK_CORE_ABORT_CONSOLE_H

#include "core/frontend_machines.h"
#include "core/frontend_menu.h"

namespace mdk {

// OBSERVED geometry — identical to the save-name confirm phase.
inline constexpr int kAbortBandY = 0x95;
inline constexpr int kAbortBandStep = 0x24;
inline constexpr int kAbortItemCount = 2;

enum class AbortAction {
  None = 0,
  Yes,    // item 0 confirm or the Y edge -> FUN_004031b8
  No,     // item 1 confirm, the N edge, or Esc -> FUN_0040316c
};

class AbortConsoleController {
public:
  AbortConsoleController() = default;  // arm resets DAT_004a1e28 = 0

  int selection() const { return sel_; }  // DAT_004a1e28 (0 = yes)

  AbortAction pendingAction() const { return action_; }
  AbortAction consumeAction();

  void update(FrontendMachineState& sh, const FrontendMenuInput& in);

private:
  int sel_ = 0;
  AbortAction action_ = AbortAction::None;
};

}  // namespace mdk

#endif  // MDK_CORE_ABORT_CONSOLE_H
