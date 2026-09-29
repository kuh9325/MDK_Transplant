// Phase 18A — saved-games list overlay (sub-mode 1).
//
// EVIDENCE (instruction-level, original binary —
// docs/reverse-engineering/EXECUTABLE_MAP.md + Phase 18A analysis):
//
//   FUN_004202cc — entry/setup. Builds the SAVES directory listing,
//   stores a compact list of up-to-six displayed rows of stem records
//   (9 bytes each), keeps DAT_0049ac28 = selection and
//   DAT_0049ac30 = count, and writes sub-mode 1 through the
//   DAT_00541490 top-byte transition machinery. The restore value
//   (0x54b838-style saved byte) is 0 for the F3/menu arms.
//
//   FUN_004206d0 — the per-frame list handler: prev/next repeat
//   queries (shared FUN_004237b4/FUN_00423838 deadlines), PgUp/PgDn
//   page steps of 13, Home/End, a typed-character first-letter jump,
//   mouse hit band (0x66 < y < 0x137, row = ((y-0x67)>>4) + topRow,
//   clamped), the mouse-tracked edge scroll (sel-1 when hovering the
//   top boundary, sel+1 past 0x136), confirm (FUN_00423764 latch —
//   Enter edge or left-click edge), and Esc cancel. Selection clamps
//   — it does NOT wrap. topRow is re-adjusted after navigation:
//   `if (top+13 <= sel) top = sel-12; if (sel < top) top = sel`.
//
//   FUN_00428144 — lazy header inspection, re-run when the selected
//   row changes (cache key DAT_0054bd2c). Result valid flag lives at
//   DAT_0049ac40; a confirm on an INVALID entry exits the list
//   instead of loading (OBSERVED quirk).
//
//   Slot metadata: the stem is the filename truncated at the first
//   space or period, max 8 characters (the 9-byte record). The
//   preview is: full save (GAME.modeField >= 1000) -> the 64x45 THMB
//   pixels; header-only save -> the per-level LOAD<level>.LBB still;
//   invalid/corrupt -> the "corrupt/empty" message records.
//
//   The confirm action loads SAVES\<stem>.SAV through the same
//   FUN_00427f94 path as Continue; cancel returns to the armed
//   restore sub-mode (0).
//
#ifndef MDK_CORE_SAVE_SLOT_LIST_H
#define MDK_CORE_SAVE_SLOT_LIST_H

#include "core/frontend_machines.h"
#include "core/frontend_menu.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdk {

// OBSERVED list geometry (FUN_004206d0).
inline constexpr int kSaveListPageStep = 13;     // PgUp/PgDn + window size
inline constexpr int kSaveListMaxName = 8;       // 9-byte record + NUL
inline constexpr int kSaveListBandTop = 0x66;    // mouse band exclusive
inline constexpr int kSaveListBandBottom = 0x137;
inline constexpr int kSaveListRowBase = 0x67;    // (y-0x67)>>4 = row
inline constexpr int kSaveListRowStep = 16;      // 0x10
// Hit-test clamps (shared FUN_004187e0 surface clamps).
inline constexpr int kSaveListHitClampX = 590;
inline constexpr int kSaveListHitClampY = 350;

// OBSERVED per-slot summary (FUN_00428144 header inspect + the GAME
// packet fields documented in docs/reverse-engineering/SAVE_FORMAT.md).
struct SaveSlotSummary {
  std::string name;        // stem, <= 8 chars, truncated at ' '/'.'
  bool valid = false;      // FUN_00428144 parse result (DAT_0049ac40)
  bool fullSave = false;   // GAME.modeField >= 1000 -> real THMB preview
  int levelId = 0;         // GAME+0x04 (header-only -> LOAD<level>.LBB)
  int modeField = 0;       // GAME+0x00 raw mode field
  int health = 0;          // GAME+0x0c
  int deathCount = 0;      // GAME+0x10
};

// Truncate a filename the way FUN_004202cc's copy loop does: stop at
// the first space or period, cap at 8 characters.
std::string saveListStem(std::string_view filename);

// Header inspector seam: given a truncated stem, return the parsed
// summary (or nullopt when the file cannot be opened — reported as an
// invalid entry, the FUN_00428144 failure path).
using SaveSlotInspector =
    std::function<std::optional<SaveSlotSummary>(std::string_view stem)>;

enum class SaveListAction {
  None = 0,
  Load,   // confirm on a valid entry — load SAVES\<stem>.SAV
  Exit,   // Esc, or confirm on an INVALID entry (OBSERVED quirk)
};

class SaveSlotListController {
public:
  // `stems` are the directory enumeration results (already truncated
  // via saveListStem by the caller, matching FUN_004202cc). `sel`
  // is the carried-in DAT_0049ac28 (kept if in range, else 0).
  SaveSlotListController(std::vector<std::string> stems,
                         SaveSlotInspector inspect, int sel);

  int count() const { return static_cast<int>(stems_.size()); }
  int selection() const { return sel_; }       // DAT_0049ac28
  int topRow() const { return topRow_; }       // DAT_0049ac2c
  bool mouseTrack() const { return mouseTrack_; } // DAT_0054bd28
  // The lazily-inspected selected entry (DAT_0049ac40 validity flag
  // included). Null when the list is empty.
  const SaveSlotSummary* selected() const;
  const std::vector<std::string>& stems() const { return stems_; }

  SaveListAction pendingAction() const { return action_; }
  SaveListAction consumeAction();
  // Stem of the entry a Load action refers to.
  const std::string& actionStem() const { return actionStem_; }

  // Per-frame update. `sh` is the shared input-machine block — the
  // list uses the SAME repeat deadlines, tick, mouse position and
  // button latch as the root menu (process globals in the original).
  void update(FrontendMachineState& sh, const FrontendMenuInput& in);

private:
  void inspectSelected();

  std::vector<std::string> stems_;
  SaveSlotInspector inspect_;
  int sel_ = 0;            // DAT_0049ac28
  int topRow_ = 0;         // DAT_0049ac2c
  bool mouseTrack_ = false; // DAT_0054bd28 — set once mouse input seen
  int inspectedSel_ = -1;  // DAT_0054bd2c
  SaveSlotSummary inspected_;
  bool inspectedValid_ = false;
  SaveListAction action_ = SaveListAction::None;
  std::string actionStem_;
};

}  // namespace mdk

#endif  // MDK_CORE_SAVE_SLOT_LIST_H
