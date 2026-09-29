// Phase 18A — the BUILD_A frontend/menu shell: the DAT_00541492
// (primary mode) + DAT_00541493 (sub-mode) machine from the main loop
// FUN_0040103c, wrapped around the existing FrontendFlowController
// (root menu + options subtree) and the Phase-18A overlay
// controllers.
//
// EVIDENCE (instruction-level, original binary — analysis-private
// logs decomp_103c.txt, p18_saves.txt, p14_dis_22bc0.txt,
// p14_dis_103c.txt; jump table dumped at 0x401010):
//
//   Loop head (per frame): FUN_0046ceac folds device input into the
//   DAT_0054b5xx edge globals + DAT_0054b63x mouse state;
//   FUN_004187e0 accumulates the logical mouse; the deferred-
//   transition queue DAT_005414d0 pumps (FUN_004090fc) before the
//   tick advances (DAT_00541518 += DAT_0049b6e8).
//
//   Pause (DAT_00499a08 / DAT_0054151c "running"):
//     arm     — running && !paused && (DAT_0054b630 || DAT_0054b598)
//               && mode != 0 && mode != 8 && sub == 0
//               -> paused = 1 + FUN_00402510 (pause sounds)
//     unpause — paused && running && (DAT_0054b630 || DAT_0054b598 ||
//               DAT_0054b570) && sub != 6 -> paused = 0, Esc cleared,
//               FUN_00424a44 + FUN_00402590 (resume sounds)
//     while paused the loop renders FUN_00424950 instead of the mode
//     dispatch — no overlay arm, no tick-driven UI.
//
//   Esc arm (inside !paused): DAT_0054b570 && sub == 0 &&
//   mode != 6 && !DAT_0054152c -> FUN_004030a8 (abort console).
//   NOT gated on mode — it fires at the frontend menu too.
//
//   Utility arm: mode != 0 && running && DAT_005414f0 &&
//   DAT_0054b584 -> FUN_00428340 (frame-capture hotkey — host seam).
//
//   Overlay arm chain (sub == 0 && mode != 0, first match wins —
//   OBSERVED priority F1 > F2 > F3 > F10 > F11 > F12):
//     F1  DAT_0054b5d4 -> FUN_0041d540  (help, sub 10)
//     F2  DAT_0054b5d8 -> FUN_00422bc0  (save gate -> sub 8)
//     F3  DAT_0054b5dc -> FUN_004202cc  (save list, sub 1)
//     F10 DAT_0054b5f8 -> FUN_004030a8  (abort, sub 9)
//     F11 DAT_0054b5fc -> FUN_0041d194  (brightness 0..7 wrap; +mode 3
//                                      extra FUN_0046c92c when its
//                                      palette gate passes — host fx)
//     F12 DAT_0054b600 -> FUN_00420cf0  (options, sub 11)
//
//   Sub-mode dispatch (jump table at 0x401010 — OBSERVED map):
//     1  FUN_004206d0  saved-games list      (frontend/save core)
//     2  FUN_004233d8  sound options child   (flow)
//     3  FUN_0041fc08  joystick options      (LEGACY — not ported)
//     4  FUN_004217e8  mouse options child   (flow)
//     5  FUN_0041f18c  keyboard child        (flow)
//     6  FUN_004224bc  performance screen    (LEGACY — not ported)
//     7  FUN_0041d1e0  display options child (flow)
//     8  FUN_00422dec  save name entry       (frontend/save core)
//     9  FUN_00403264  abort console         (frontend core)
//     10 FUN_0041d630  help screen           (frontend core)
//     11 FUN_00420eac  options               (flow)
//   sub == 0 falls through to the primary-mode dispatch:
//     mode 2 FUN_004103d8 freefall, 3 FUN_00436100/4371bc traversal,
//     5 FUN_0042c8b0 tally, 6 FUN_004296f0 briefing, 7 level handoff,
//     8 FUN_0047b06c cinematic, mode == 1 (EDX sentinel compare) ->
//     FUN_00418e04 static-noise transition, everything else ->
//     FUN_0041dc90 the frontend menu. Each overlay dispatch is
//     followed by FUN_004026f8 (frame-output hook — presentation).
//
//   Frontend entry FUN_0041d85c(arg):
//     always: mode = 0, DAT_0054bc94/0x54bca4/0x54bc98 = 0,
//     DAT_0054bc98 = FUN_00428290 (LASTGAME.SAV exists) -> root
//     selection = !exists, DAT_0049aa98 > 0 -> 0, FUN_0041d81c,
//     FUN_0041d720 (MAINSONG start);
//     arg == 0 (fresh): FUN_0041d7b4 — the MDKOPT resource reload
//       (boot, abort-return, save-load failure all use this);
//     arg != 0 (returning): DAT_0049aa7c = 1 transition pending +
//       DAT_0049aa84/0x49aa88 counters + DAT_0054152c = 1 (suppress
//       the Esc-abort arm on entry — FUN_0041ebf4 clears it when the
//       entry transition completes) — intro-finish / mode-8-complete /
//       freefall-fail paths.
//
//   New Game (root sel 1): FUN_0041dbd4 teardown + FUN_0041b630 →
//   the mode-6 briefing/load entry — emitted as StartNewGame for the
//   campaign host (already closed).
//
//   Continue (root sel 0, gated on FUN_00428290): builds
//   SAVES\LASTGAME.SAV and calls FUN_00427f94 — emitted as
//   ContinueLastGame; the host runs the load and reports
//   notifyLoadResult(): failure re-enters the frontend fresh (the
//   original's FUN_0041d85c call after its error dialog).
//
//   Ending return (mode 8 complete): FUN_0047b06c calls
//   FUN_0041d85c(nonzero) — the returning-entry path; the host calls
//   enterFrontend(returning=true).
//
#ifndef MDK_CORE_FRONTEND_SHELL_H
#define MDK_CORE_FRONTEND_SHELL_H

#include "core/abort_console.h"
#include "core/frontend_flow.h"
#include "core/mode_dispatch.h"
#include "core/frontend_machines.h"
#include "core/frontend_menu.h"
#include "core/frontend_settings.h"
#include "core/save_name_entry.h"
#include "core/save_slot_list.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdk {

// OBSERVED sub-mode ids — the 0x401010 jump table order.
enum FrontendSub : int {
  kSubPrimary = 0,
  kSubSaveList = 1,
  kSubSound = 2,
  kSubJoystick = 3,   // FUN_0041fc08 — legacy, not ported
  kSubMouse = 4,
  kSubKeyboard = 5,
  kSubPerf = 6,       // FUN_004224bc — legacy, not ported
  kSubDisplay = 7,
  kSubSaveName = 8,
  kSubAbort = 9,
  kSubHelp = 10,
  kSubOptions = 11,
};

// Semantic requests the shell raises for the host (gameplay/save/
// campaign systems are owned outside the frontend).
enum class FrontendRequest {
  None = 0,
  Quit,              // DAT_0054148e = 1 (root Quit / abort-yes at menu)
  StartNewGame,      // root sel 1: FUN_0041dbd4 + FUN_0041b630 -> mode 6
  ContinueLastGame,  // root sel 0: load SAVES\LASTGAME.SAV
  LoadSave,          // save-list confirm: requestName() = stem
  WriteSaveDone,     // name-entry commit: requestName()/HeaderOnly set
  AbortToFrontend,   // abort YES in-game: host frees the mode, then
                     // calls enterFrontend(fresh)
  ResumeTraversal,   // abort NO in mode 3 -> FUN_004348d4 unfreeze
  CycleBrightness,   // F11: brightness = (brightness+1) & 7 (+upload)
  CaptureUtility,    // 0x54b584 edge + DAT_005414f0 gate -> FUN_00428340
  OpenLegacyScreen,  // options row for sub 3 (joystick) / 6 (perf):
                     // emitted for diagnostics; not ported
};

// Host/environment seams the shell consumes.
struct FrontendShellSeams {
  // FUN_00428290 — SAVES\LASTGAME.SAV existence gate (Continue).
  std::function<bool()> lastGameExists;
  // FUN_004202cc enumeration — raw save filename list for SAVES\.
  // The shell truncates each to the <=8-char stem itself.
  std::function<std::vector<std::string>()> enumerateSaves;
  // FUN_00428144 — header inspect for one stem.
  SaveSlotInspector inspectSlot;
  // FUN_00422d84 -> FUN_00427ed4 — perform the save write.
  SaveNameWriter writeSave;
  // MISC\MDKS_%03d.GIF probe for the attract slideshow.
  FrontendMenuController::AttractSlideProbe slideProbe;
};

// Presentation/audio boundary events (the original's resource, song
// and palette calls — surfaces for the host to perform).
enum class FrontendFx {
  PauseSounds,            // FUN_00402510
  ResumeSounds,           // FUN_00402590
  ResumeSoundsAlt,        // FUN_004025d0 (abort-yes path)
  MenuSongStart,          // FUN_0041d720 (MAINSONG in FUN_0041d85c)
  LoadFrontendResources,  // FUN_0041d7b4 — fresh entry resource reload
  TransitionArmed,        // DAT_0049aa7c = 1 — returning-entry noise
  AbortDialogResources,   // FUN_0041c420(0x2e) + palette save/upload
  SaveNameThumbnailGrab,  // FUN_00427e8c(0x49f010) at arm time
};

class FrontendShell {
public:
  explicit FrontendShell(FrontendShellSeams seams);
  // Convenience ctor for tests/diagnostics: LASTGAME presence passed
  // directly, other seams empty.
  explicit FrontendShell(bool lastGameExists);

  int primaryMode() const { return primaryMode_; }  // DAT_00541492
  int subMode() const { return subMode_; }          // DAT_00541493
  bool quitRequested() const { return quit_; }      // DAT_0054148e
  bool paused() const { return paused_; }           // DAT_00499a08
  int transitionByte() const { return transitionByte_; } // 0x541490>>24
  // DAT_0054152c — suppress the Esc-abort arm. Set by the
  // FUN_0041d85c returning-entry path (arg != 0); the original clears
  // it inside FUN_0041ebf4 when the entry transition finishes
  // (transition playback is host-side — the host calls
  // setSuppressEscAbort(false) at that point).
  bool suppressEscAbort() const { return suppressEscAbort_; }
  // DAT_005414fc — global idle counter (ticks up while no input edge
  // or mouse activity is seen and the game is not paused).
  int idleTicks() const { return idleTicks_; }
  // Set when the last update() dispatched a mode-1 noise-transition
  // frame (FUN_00418e04 — host draws the noise; the shell only
  // records that the dispatch ran).
  bool noiseFrameRan() const { return noiseFrame_; }

  // --- host-driven global state ------------------------------------
  void setPrimaryMode(int mode) { primaryMode_ = mode; }
  void setRunning(bool r) { running_ = r; }         // DAT_0054151c
  void setSuppressEscAbort(bool s) { suppressEscAbort_ = s; }
  void setLevelIndex(int idx) { levelIndex_ = idx; } // DAT_00541498
  void setUtilityKeyEnabled(bool e) { utilityEnabled_ = e; } // 0x5414f0
  // Manual-save gate inputs (FUN_00422bc0 arg==0 checks):
  void setSaveBlockFlags(bool blockA,      // DAT_00540e9c
                         bool blockB,      // DAT_00540d9c
                         bool xStrike);    // X_STRIKE tag in obj lists
  // Derived OBSERVED gate state (diagnostics): the manual-save arm
  // conditions without the entity-list scan applied twice.
  bool saveGateOpen() const;

  // FUN_0041d85c — enter the frontend. `returning` selects the
  // arg != 0 path (transition armed + Esc suppression); fresh entry
  // (arg == 0) emits LoadFrontendResources.
  void enterFrontend(bool returning);

  // The inter-level autosave arm — FUN_00422bc0(arg != 0): bypasses
  // the manual-save gate entirely, confirm phase first, header-only
  // write, name prefilled "<levelIndex+1>".
  void armAutosave();

  // Load result callback for ContinueLastGame / LoadSave requests:
  // on failure the original shows its error dialog then re-enters
  // the frontend (FUN_0041d85c).
  void notifyLoadResult(bool ok);

  // --- per-frame ----------------------------------------------------
  void update(const FrontendMenuInput& in);
  void endFrame(double dtMs);

  // --- screens (valid while the matching sub-mode is active) -------
  FrontendFlowController& flow() { return *flow_; }
  const FrontendFlowController& flow() const { return *flow_; }
  SaveSlotListController* saveList() { return saveList_.get(); }
  const SaveSlotListController* saveList() const {
    return saveList_.get();
  }
  SaveNameEntryController* saveName() { return saveName_.get(); }
  const SaveNameEntryController* saveName() const {
    return saveName_.get();
  }
  AbortConsoleController* abortConsole() { return abortConsole_.get(); }
  const AbortConsoleController* abortConsole() const {
    return abortConsole_.get();
  }
  bool helpOpen() const { return subMode_ == kSubHelp; }
  const FrontendMachineState& sharedMachine() const { return shared_; }

  // Requests queue in emission order — a single frame can produce
  // more than one (e.g. the save-name commit raises WriteSaveDone
  // and the teardown's ResumeTraversal in the same teardown chain).
  FrontendRequest pendingRequest() const {
    return requests_.empty() ? FrontendRequest::None
                             : requests_.front().req;
  }
  FrontendRequest consumeRequest();
  const std::string& requestName() const;
  bool requestHeaderOnly() const {
    return !requests_.empty() && requests_.front().headerOnly;
  }

  std::vector<FrontendFx> drainFx();

private:
  void loopHead_(const FrontendMenuInput& in);  // FUN_004187e0 + tick
  void utilityCheck_(const FrontendMenuInput& in);  // 0x40112c
  void armAbort(const FrontendMenuInput& in);   // FUN_004030a8
  void armHelp();                               // FUN_0041d540
  void armSaveList();                           // FUN_004202cc
  void armSaveName(bool autoSave,
                   const FrontendMenuInput& in);  // FUN_00422bc0 tail
  void abortYes();                              // FUN_004031b8
  void abortNo();                               // FUN_0040316c
  void syncSubFromFlow();                 // flow screen -> sub id
  void onFlowActions();                   // consume flow actions
  void teardownOverlay();                 // sub = saved byte
  void resumeAfterOverlay_();  // FUN_00402590 + FUN_004348d4 gate
  void emitRequest(FrontendRequest req, std::string name = {},
                   bool headerOnly = false);

  FrontendShellSeams seams_;

  int primaryMode_ = mode::observed::frontend;
  int subMode_ = kSubPrimary;
  int transitionByte_ = kSubPrimary; // DAT_00541490>>24 mirror
  int savedByte_ = kSubPrimary;      // DAT_0054b838/0x54bda4 restore
  bool quit_ = false;
  bool paused_ = false;              // DAT_00499a08
  bool running_ = false;             // DAT_0054151c
  bool suppressEscAbort_ = false;    // DAT_0054152c
  int idleTicks_ = 0;                // DAT_005414fc
  int levelIndex_ = 0;               // DAT_00541498
  bool utilityEnabled_ = false;      // DAT_005414f0
  bool saveBlockA_ = false;          // DAT_00540e9c
  bool saveBlockB_ = false;          // DAT_00540d9c
  bool xStrikeActive_ = false;       // X_STRIKE object scan
  int lastSaveSel_ = 0;              // DAT_0049ac28 carried across
  bool saveNameAutoSave_ = false;    // DAT_0054bdb0 (the raw arm arg)

  FrontendMachineState shared_;      // the shared globals block
  std::unique_ptr<FrontendFlowController> flow_;
  std::unique_ptr<SaveSlotListController> saveList_;
  std::unique_ptr<SaveNameEntryController> saveName_;
  std::unique_ptr<AbortConsoleController> abortConsole_;

  struct PendingRequest {
    FrontendRequest req = FrontendRequest::None;
    std::string name;        // save stem / legacy-sub id when relevant
    bool headerOnly = false; // WriteSaveDone: the autosave flag
  };
  std::deque<PendingRequest> requests_;
  std::vector<FrontendFx> fx_;
  bool noiseFrame_ = false;   // last update dispatched FUN_00418e04
  bool flowRan_ = false;      // a flow screen ran this update
  bool overlayRan_ = false;   // an overlay screen ran this update
};

}  // namespace mdk

#endif  // MDK_CORE_FRONTEND_SHELL_H
