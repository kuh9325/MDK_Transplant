#include "core/frontend_shell.h"

#include <cmath>
#include <utility>

namespace mdk {

namespace {

// Sub-modes the FrontendFlowController owns (options + children and
// the primary-dispatch root). Overlay subs (1/8/9/10) are armed by
// onFlowActions/arm calls and must not be remapped by syncSubFromFlow.
bool isFlowSub(int s) {
  return s == kSubPrimary || s == kSubSound || s == kSubMouse ||
         s == kSubKeyboard || s == kSubDisplay || s == kSubOptions;
}

// The dispatch else-branch: everything outside {1,2,3,5,6,7,8} runs
// FUN_0041dc90 — mode 0 is the normal frontend, mode 4 and any other
// unlisted value land here too (OBSERVED fallthrough).
bool isFrontendMenuMode(int m) {
  return m != mode::observed::noiseTransition &&
         m != mode::observed::transition &&
         m != mode::observed::traversal && m != mode::observed::stats &&
         m != mode::observed::levelLoad &&
         m != mode::observed::traverseContinue &&
         m != mode::observed::cinematic;
}

}  // namespace

FrontendShell::FrontendShell(FrontendShellSeams seams)
    : seams_(std::move(seams)) {
  enterFrontend(false);  // boot = the FUN_0041d85c(0) fresh entry
}

FrontendShell::FrontendShell(bool lastGameExists) {
  seams_.lastGameExists = [lastGameExists] { return lastGameExists; };
  enterFrontend(false);
}

void FrontendShell::emitRequest(FrontendRequest req, std::string name,
                                bool headerOnly) {
  requests_.push_back({req, std::move(name), headerOnly});
}

FrontendRequest FrontendShell::consumeRequest() {
  if (requests_.empty()) {
    return FrontendRequest::None;
  }
  const FrontendRequest r = requests_.front().req;
  requests_.pop_front();
  return r;
}

const std::string& FrontendShell::requestName() const {
  static const std::string empty;
  return requests_.empty() ? empty : requests_.front().name;
}

std::vector<FrontendFx> FrontendShell::drainFx() {
  std::vector<FrontendFx> out;
  out.swap(fx_);
  return out;
}

bool FrontendShell::advanceMarkerBlink() {
  // FUN_00414b28's accumulator step on the shared global
  // (DAT_0049a770): floor(acc + smoothed), bit 3 is the phase.
  shared_.markerAcc = static_cast<int>(
      std::floor(shared_.markerAcc + shared_.timing.smoothed));
  return (shared_.markerAcc & 0x8) != 0;
}

void FrontendShell::setSaveBlockFlags(bool a, bool b, bool xStrike) {
  saveBlockA_ = a;
  saveBlockB_ = b;
  xStrikeActive_ = xStrike;
}

bool FrontendShell::saveGateOpen() const {
  // FUN_00422bc0 arg == 0 gate (OBSERVED): traversal mode, no
  // overlay, both block flags clear, no live X_STRIKE object.
  return primaryMode_ == mode::observed::traversal &&
         subMode_ == kSubPrimary && !saveBlockA_ && !saveBlockB_ &&
         !xStrikeActive_;
}

void FrontendShell::enterFrontend(bool returning) {
  // FUN_0041d85c (OBSERVED): mode = 0, the continue flag re-derives
  // from FUN_00428290 (LASTGAME.SAV), sub-mode clears, MAINSONG
  // starts, and the arg selects fresh vs returning resources.
  primaryMode_ = mode::observed::frontend;
  subMode_ = kSubPrimary;
  transitionByte_ = kSubPrimary;
  savedByte_ = kSubPrimary;
  saveList_.reset();
  saveName_.reset();
  abortConsole_.reset();
  quit_ = false;

  const bool saves = seams_.lastGameExists && seams_.lastGameExists();
  if (!flow_) {
    flow_ = std::make_unique<FrontendFlowController>(saves);
    if (seams_.slideProbe) {
      flow_->root().setAttractSlideProbe(seams_.slideProbe);
    }
  } else {
    flow_->enterFrontend(saves);
  }

  fx_.push_back(FrontendFx::MenuSongStart);  // FUN_0041d720
  if (!returning) {
    fx_.push_back(FrontendFx::LoadFrontendResources);  // FUN_0041d7b4
  } else {
    // arg != 0: DAT_0049aa7c = 1 + DAT_0054152c = 1 (Esc suppression).
    fx_.push_back(FrontendFx::TransitionArmed);
    suppressEscAbort_ = true;
  }
  // The shared block is the active screen's globals — resync the
  // mirror after the entry reset.
  shared_ = flow_->activeMachineState();
}

void FrontendShell::armAutosave() {
  // FUN_00422bc0 with a nonzero arg: bypasses the manual gate. Called
  // by the host outside update() — no pending mouse deltas for the
  // arm's FUN_004187e0 refresh to re-apply.
  const FrontendMenuInput empty{};
  armSaveName(true, empty);
}

void FrontendShell::notifyLoadResult(bool ok) {
  if (ok) {
    // 0x42099d (OBSERVED): load success -> DAT_00541493 = 0 (the mode
    // was already rewritten by FUN_00427f94 — the host mirrors that
    // with setPrimaryMode).
    subMode_ = kSubPrimary;
    transitionByte_ = kSubPrimary;
  } else {
    // 0x420a88 (OBSERVED): FUN_00427f94 returned 0 — its error dialog
    // is host-side — then FUN_0041d85c(0): fresh frontend re-entry.
    enterFrontend(false);
  }
}

// The loop head (0x4010a3-0x4010e9): FUN_004187e0 accumulates the
// logical mouse (clamp 599/359) and DAT_00541518 advances by
// DAT_0049b6e8. Runs every frame — before the pause block, the arm
// chain, and the dispatch.
void FrontendShell::loopHead_(const FrontendMenuInput& in) {
  frontendMouseAccumulate(shared_.mouseX, in.mouseDx,
                          kFrontendMouseMaxX);
  frontendMouseAccumulate(shared_.mouseY, in.mouseDy,
                          kFrontendMouseMaxY);
  shared_.tick += shared_.timing.frameStep;
}

// 0x40112c (OBSERVED): mode != 0 && running && DAT_005414f0 &&
// DAT_0054b584 -> FUN_00428340 — runs even on paused frames.
void FrontendShell::utilityCheck_(const FrontendMenuInput& in) {
  if (primaryMode_ != mode::observed::frontend && running_ &&
      utilityEnabled_ && in.utilityEdge) {
    emitRequest(FrontendRequest::CaptureUtility);  // FUN_00428340
  }
}

// The shared resume chain used by the overlay teardowns
// (FUN_0041d57c / FUN_00420d68 / FUN_00422d1c / FUN_0042056c):
// mode != 0 && the restored sub == 0 -> FUN_00402590 resume sounds,
// and mode == 3 also FUN_004348d4 (traversal unfreeze).
void FrontendShell::resumeAfterOverlay_() {
  if (primaryMode_ != mode::observed::frontend &&
      subMode_ == kSubPrimary) {
    fx_.push_back(FrontendFx::ResumeSounds);
    if (primaryMode_ == mode::observed::traversal) {
      emitRequest(FrontendRequest::ResumeTraversal);
    }
  }
}

void FrontendShell::armAbort(const FrontendMenuInput& in) {
  // FUN_004030a8 (OBSERVED): the DAT_00541514 && mode == 0 gate is
  // inert in the normal startup path (the flag is always 0), so the
  // console arms everywhere it is invoked. DAT_004a1e28 = 0 (the
  // selection resets to the YES item — the controller default),
  // DAT_00541493 = 9 directly, FUN_00402510 pause sounds
  // unconditionally, FUN_004187e0 re-applies the still-pending mouse
  // deltas (the OBSERVED arm-frame double-accumulate), then the
  // dialog resource load + palette work.
  abortConsole_ = std::make_unique<AbortConsoleController>();
  savedByte_ = subMode_;
  transitionByte_ = kSubAbort;
  subMode_ = kSubAbort;
  fx_.push_back(FrontendFx::PauseSounds);  // FUN_00402510 — no gate
  frontendMouseAccumulate(shared_.mouseX, in.mouseDx,
                          kFrontendMouseMaxX);
  frontendMouseAccumulate(shared_.mouseY, in.mouseDy,
                          kFrontendMouseMaxY);
  fx_.push_back(FrontendFx::AbortDialogResources);
}

void FrontendShell::armHelp() {
  // FUN_0041d540 (OBSERVED): FUN_00402510 pause sounds only when
  // mode != 0 && the PRE-arm sub == 0; DAT_0054b838 = the old
  // sub-mode; DAT_00541493 = 10 directly.
  if (primaryMode_ != mode::observed::frontend &&
      subMode_ == kSubPrimary) {
    fx_.push_back(FrontendFx::PauseSounds);
  }
  savedByte_ = subMode_;
  transitionByte_ = kSubHelp;
  subMode_ = kSubHelp;
}

void FrontendShell::armSaveList() {
  // FUN_004202cc (OBSERVED): FUN_00402510 when mode != 0 (no sub
  // check — the arm is only reached under sub == 0 anyway);
  // DAT_00541493 = 1 directly; SAVES\ enumeration keeps the stem up
  // to the first ' '/'.', <= 8 chars.
  std::vector<std::string> stems;
  if (seams_.enumerateSaves) {
    for (const auto& name : seams_.enumerateSaves()) {
      stems.push_back(saveListStem(name));
    }
  }
  saveList_ = std::make_unique<SaveSlotListController>(
      std::move(stems), seams_.inspectSlot, lastSaveSel_);
  savedByte_ = subMode_;
  transitionByte_ = kSubSaveList;
  subMode_ = kSubSaveList;
  if (primaryMode_ != mode::observed::frontend) {
    fx_.push_back(FrontendFx::PauseSounds);  // FUN_00402510
  }
}

void FrontendShell::armSaveName(bool autoSave,
                                const FrontendMenuInput& in) {
  if (!autoSave && !saveGateOpen()) {
    return;  // FUN_00422bc0 arg == 0 gate failed — silent no-op.
  }
  // Arm effects (OBSERVED): DAT_0054bdac = 0, DAT_0054bdb0 =
  // DAT_0054bdb4 = the raw arg (nonzero -> header-only write);
  // FUN_00402510 when mode != 0 && sub == 0; DAT_0054bda4 = the old
  // sub-mode; DAT_00541493 = 8; DAT_0054151c = 0 (world frozen);
  // autosave prefill "<levelIndex+1>" via sprintf("%d"); cursor =
  // strlen(name); FUN_004187e0 double-accumulates the pending deltas;
  // FUN_00427e8c grabs the thumbnail; DAT_0054bd9c = (arg == 0) —
  // the flag runs the "SAVE CURRENT POSITION?" confirm phase while
  // it is ZERO, so the AUTOSAVE asks first and the manual (F2) path
  // goes straight to typing; DAT_0054bda8 = 0.
  const std::string prefill =
      autoSave ? std::to_string(levelIndex_ + 1) : std::string();
  saveName_ = std::make_unique<SaveNameEntryController>(
      prefill, /*confirmPhase=*/autoSave, /*headerOnly=*/autoSave,
      seams_.writeSave);
  saveNameAutoSave_ = autoSave;
  savedByte_ = subMode_;
  transitionByte_ = kSubSaveName;
  subMode_ = kSubSaveName;
  running_ = false;  // DAT_0054151c = 0
  if (primaryMode_ != mode::observed::frontend &&
      savedByte_ == kSubPrimary) {
    fx_.push_back(FrontendFx::PauseSounds);  // FUN_00402510
  }
  frontendMouseAccumulate(shared_.mouseX, in.mouseDx,
                          kFrontendMouseMaxX);
  frontendMouseAccumulate(shared_.mouseY, in.mouseDy,
                          kFrontendMouseMaxY);
  fx_.push_back(FrontendFx::SaveNameThumbnailGrab);  // FUN_00427e8c
}

void FrontendShell::abortYes() {
  // FUN_004031b8 (OBSERVED): FUN_004025d0 resume-sounds variant,
  // FUN_00403124 dialog release, DAT_00541493 = 0, then by mode:
  abortConsole_.reset();
  subMode_ = kSubPrimary;
  transitionByte_ = kSubPrimary;
  fx_.push_back(FrontendFx::ResumeSoundsAlt);  // FUN_004025d0
  if (primaryMode_ == mode::observed::frontend) {
    // mode == 0: FUN_0041dbd4 + DAT_0054148e -> QUIT.
    quit_ = true;
    emitRequest(FrontendRequest::Quit);
  } else {
    // mode != 0: the original runs the per-mode teardown table
    // (FUN_00418de4 / FUN_0040fa68 / FUN_004371bc / FUN_0042c824 /
    // FUN_004295c4 / FUN_0047b0d8 — host-side) then FUN_0041d85c.
    emitRequest(FrontendRequest::AbortToFrontend);
  }
}

void FrontendShell::abortNo() {
  // FUN_0040316c (OBSERVED): DAT_00541493 = 0 first, FUN_00402590
  // resume sounds UNCONDITIONALLY, FUN_00403124 release, then:
  abortConsole_.reset();
  subMode_ = kSubPrimary;
  transitionByte_ = kSubPrimary;
  fx_.push_back(FrontendFx::ResumeSounds);  // FUN_00402590 — no gate
  if (primaryMode_ == mode::observed::frontend) {
    enterFrontend(false);  // FUN_0041dbd4 + FUN_0041d85c — fresh entry
  } else if (primaryMode_ == mode::observed::traversal) {
    emitRequest(FrontendRequest::ResumeTraversal);  // FUN_004348d4
  }
  // other modes: nothing further — sub = 0 already resumed them.
}

void FrontendShell::teardownOverlay() {
  // Shared overlay teardown: the sub-mode restores the arm's saved
  // byte (DAT_0054b838 / DAT_0054bda4), the shared machine block
  // re-enters the resumed flow screen, and the mode != 0 && sub == 0
  // paths run the resume chain (FUN_00402590 / FUN_004348d4).
  subMode_ = savedByte_;
  transitionByte_ = savedByte_;
  if (subMode_ == kSubPrimary) {
    flow_->root().setMachineState(shared_);
  } else if (subMode_ == kSubOptions && flow_->inOptions()) {
    flow_->options().setMachineState(shared_);
  }
  resumeAfterOverlay_();
}

void FrontendShell::syncSubFromFlow() {
  const int prev = subMode_;
  switch (flow_->screen()) {
  case FrontendScreen::Root:     subMode_ = kSubPrimary;  break;
  case FrontendScreen::Options:  subMode_ = kSubOptions;  break;
  case FrontendScreen::Display:  subMode_ = kSubDisplay;  break;
  case FrontendScreen::Sound:    subMode_ = kSubSound;    break;
  case FrontendScreen::Mouse:    subMode_ = kSubMouse;    break;
  case FrontendScreen::Keyboard: subMode_ = kSubKeyboard; break;
  }
  transitionByte_ = subMode_;
  // Options teardown (FUN_00420d68, OBSERVED): leaving the options
  // subtree back to sub 0 over a non-frontend mode runs the same
  // resume chain as the overlay teardowns (FUN_00402590 + the mode-3
  // FUN_004348d4 unfreeze).
  if (prev != kSubPrimary && subMode_ == kSubPrimary) {
    resumeAfterOverlay_();
  }
}

void FrontendShell::onFlowActions() {
  const FrontendAction a = flow_->consumeRootAction();
  switch (a) {
  case FrontendAction::NewGame:
    // FUN_0041dbd4 + FUN_0041b630 -> mode 6 briefing/load entry.
    emitRequest(FrontendRequest::StartNewGame);
    break;
  case FrontendAction::ContinueGame:
    // FUN_00415658 + SAVES\LASTGAME.SAV + FUN_00427f94.
    emitRequest(FrontendRequest::ContinueLastGame);
    break;
  case FrontendAction::SavedGame:
    // sel 2 -> FUN_0041dbd4 + FUN_004202cc (frontend context — no
    // sound pause). Snapshot the shared block first: the list borrows
    // the same globals.
    shared_ = flow_->activeMachineState();
    armSaveList();
    break;
  case FrontendAction::Quit:
    quit_ = true;  // DAT_0054148e = 1
    emitRequest(FrontendRequest::Quit);
    break;
  case FrontendAction::EnterAttract:
  case FrontendAction::None:
  case FrontendAction::OpenOptions:
    break;  // internal / diagnostics-only
  }

  // Options rows the flow does not own: Help arms the sub-10 overlay;
  // Joystick/Performance are legacy screens (sub 3/6 — not ported).
  const OptionsAction oa = flow_->consumeOptionsAction();
  switch (oa) {
  case OptionsAction::Help:
    shared_ = flow_->activeMachineState();
    armHelp();
    break;
  case OptionsAction::Joystick:
    emitRequest(FrontendRequest::OpenLegacyScreen, "3");
    break;
  case OptionsAction::Performance:
    emitRequest(FrontendRequest::OpenLegacyScreen, "6");
    break;
  default:
    break;
  }
  // Drain the child screens' transition-consuming actions so their
  // internal returns (Back -> options etc.) fire — the actions
  // themselves are semantics the shell does not reinterpret.
  flow_->consumeDisplayAction();
  flow_->consumeSoundAction();
  flow_->consumeMouseAction();
  flow_->consumeKeyboardAction();
}

void FrontendShell::update(const FrontendMenuInput& in) {
  flowRan_ = false;
  overlayRan_ = false;
  noiseFrame_ = false;

  // The loop head (FUN_004187e0 accumulate + DAT_00541518 tick
  // advance) runs unconditionally every frame in the original. The
  // flow screens perform it inside their update(); for every other
  // dispatch path — and for paused/arming frames that reach no screen
  // — the shell performs it via loopHead_().

  // --- pause block (0x4010ef-0x401221, OBSERVED) -------------------
  bool escConsumed = false;
  if (paused_) {
    if (running_ && (in.pauseEdge || in.pauseAltEdge || in.cancelEdge) &&
        subMode_ != kSubPerf) {
      // DAT_0054b570 cleared -> the Esc does not re-arm abort below.
      paused_ = false;
      escConsumed = in.cancelEdge;
      fx_.push_back(FrontendFx::ResumeSounds);  // 424a44 + 402590
    } else {
      loopHead_(in);
      utilityCheck_(in);  // the 0x40112c check runs while paused
      return;             // FUN_00424950 renders; no dispatch
    }
  } else if (running_ && (in.pauseEdge || in.pauseAltEdge) &&
             primaryMode_ != mode::observed::frontend &&
             primaryMode_ != mode::observed::cinematic &&
             subMode_ == kSubPrimary) {
    loopHead_(in);
    utilityCheck_(in);
    paused_ = true;  // DAT_00499a08 = 1 — the arming frame draws the
    fx_.push_back(FrontendFx::PauseSounds);  // pause screen, no dispatch
    return;
  }

  // --- DAT_005414fc global idle counter (0x401234, inside !paused) --
  // The original ORs the edge dword 0x54b57c, the held-key bitmaps
  // 0x54b534/38/3c, and the mouse deltas — any activity resets it to
  // 0, else it accumulates DAT_0049b6e8 (frameStep).
  const bool anyActivity =
      in.prevHeld || in.nextHeld || in.confirmEdge || in.leftEdge ||
      in.rightEdge || in.leftHeld || in.rightHeld || in.cancelEdge ||
      in.attractEdge || in.pageUpEdge || in.pageDownEdge ||
      in.homeEdge || in.endEdge || in.keyYEdge || in.keyNEdge ||
      in.f1Edge || in.f2Edge || in.f3Edge || in.f10Edge ||
      in.f11Edge || in.f12Edge || in.pauseEdge || in.pauseAltEdge ||
      in.utilityEdge || in.nameBackspaceEdge || in.nameDeleteEdge ||
      in.nameHomeEdge || in.nameEndEdge ||
      in.typedChar != 0 || in.mouseDx != 0 || in.mouseDy != 0 ||
      in.mouseDz != 0 || in.mouseButtons != 0 || in.rawKeyEdge[0] ||
      in.rawKeyEdge[1] || in.rawKeyEdge[2] || in.rawKeyEdge[3];
  idleTicks_ = anyActivity ? 0 : idleTicks_ + shared_.timing.frameStep;

  // --- capture-utility hotkey (0x40112c gates, OBSERVED) ----------
  utilityCheck_(in);

  // --- Esc -> abort console arm (0x401267 — not mode-gated) --------
  if (in.cancelEdge && !escConsumed && subMode_ == kSubPrimary &&
      primaryMode_ != mode::observed::levelLoad && !suppressEscAbort_) {
    if (isFrontendMenuMode(primaryMode_)) {
      // The live shared block is the root screen's state — snapshot
      // it so the overlay borrows the same globals it would share.
      shared_ = flow_->activeMachineState();
    }
    armAbort(in);
  }

  // --- overlay arm chain (sub == 0 && mode != 0; OBSERVED priority
  //     F1 > F2 > F3 > F10 > F11 > F12 — first match wins) -----------
  if (subMode_ == kSubPrimary &&
      primaryMode_ != mode::observed::frontend) {
    if (in.f1Edge) {
      armHelp();
    } else if (in.f2Edge) {
      armSaveName(/*autoSave=*/false, in);  // gated — may no-op
    } else if (in.f3Edge) {
      armSaveList();
    } else if (in.f10Edge) {
      armAbort(in);
    } else if (in.f11Edge) {
      emitRequest(FrontendRequest::CycleBrightness);  // FUN_0041d194
    } else if (in.f12Edge) {
      // FUN_00420cf0 — options over whatever mode is running.
      flow_->enterOptionsSubtree(shared_);
      subMode_ = kSubOptions;
      transitionByte_ = kSubOptions;
    }
  }

  // --- dispatch (CH re-read after the arms — a just-armed overlay
  //     runs its first frame this same iteration, OBSERVED) ---------
  switch (subMode_) {
  case kSubSaveList:
    if (saveList_) {
      loopHead_(in);
      saveList_->update(shared_, in);
      overlayRan_ = true;
      switch (saveList_->pendingAction()) {
      case SaveListAction::Load:
        // 0x42097e-0x420997 (OBSERVED): the mode teardowns run
        // host-side, then FUN_0042056c(0) — list cleanup only (sub
        // stays 1 through the load call), then FUN_00427f94. Success
        // writes sub = 0 (mode already set by the load); failure
        // runs FUN_0041d85c(0). The load is host-deferred here:
        // notifyLoadResult() resolves it.
        lastSaveSel_ = saveList_->selection();
        emitRequest(FrontendRequest::LoadSave,
                    saveList_->actionStem());
        saveList_.reset();  // FUN_0042056c(0) cleanup
        break;
      case SaveListAction::Exit:
        // Esc/cancel path at 0x420a74: FUN_0042056c(1) — cleanup,
        // then mode == 0 -> FUN_0041d85c(0) fresh re-entry;
        // mode != 0 -> resume chain + sub = 0.
        lastSaveSel_ = saveList_->selection();
        saveList_.reset();
        if (primaryMode_ == mode::observed::frontend) {
          enterFrontend(false);
        } else {
          subMode_ = kSubPrimary;
          transitionByte_ = kSubPrimary;
          resumeAfterOverlay_();
        }
        break;
      case SaveListAction::None:
        break;
      }
    }
    break;
  case kSubSaveName:
    if (saveName_) {
      loopHead_(in);
      saveName_->update(shared_, in, shared_.tick);
      overlayRan_ = true;
      const SaveNameAction na = saveName_->pendingAction();
      if (na != SaveNameAction::None) {
        if (na == SaveNameAction::Commit) {
          emitRequest(FrontendRequest::WriteSaveDone,
                      saveName_->name(),
                      saveName_->headerOnly());
        }
        saveName_.reset();
        // FUN_00422d1c (OBSERVED): DAT_00541493 = DAT_0054bda4,
        // DAT_0054151c = 1, the mode != 0 && sub == 0 resume chain
        // (FUN_00402590 + mode-3 FUN_004348d4), and one extra frame
        // flip (FUN_004167a0) when the autosave flag DAT_0054bdb0
        // was nonzero — presentation, not modeled.
        teardownOverlay();
        running_ = true;
      }
    }
    break;
  case kSubAbort:
    if (abortConsole_) {
      loopHead_(in);
      abortConsole_->update(shared_, in);
      overlayRan_ = true;
      const AbortAction aa = abortConsole_->pendingAction();
      if (aa == AbortAction::Yes) {
        abortYes();
      } else if (aa == AbortAction::No) {
        abortNo();
      }
    }
    break;
  case kSubHelp:
    // FUN_0041d630 (OBSERVED): Esc exits outright; otherwise the four
    // repeat queries (up/down/left/right) plus the latched confirm
    // query exit — the frame draws the HELP records only while all of
    // them are clear. FUN_0041d57c teardown restores the saved byte
    // and runs the resume chain.
    loopHead_(in);
    overlayRan_ = true;
    {
      const bool nav =
          frontendRepeatQuery(shared_.tick, in.prevHeld,
                              shared_.prevDeadline) ||
          frontendRepeatQuery(shared_.tick, in.nextHeld,
                              shared_.nextDeadline) ||
          frontendRepeatQuery(shared_.tick, in.leftHeld,
                              shared_.leftDeadline) ||
          frontendRepeatQuery(shared_.tick, in.rightHeld,
                              shared_.rightDeadline);
      if (in.cancelEdge ||
          frontendConfirmQuery(shared_, in.confirmEdge,
                               in.mouseButtons) ||
          nav) {
        teardownOverlay();
      }
    }
    break;
  // The flow-owned sub-modes — options and its children dispatch
  // their screens regardless of the primary mode (F12 options runs
  // over traversal exactly like it runs over the frontend).
  case kSubSound:
  case kSubMouse:
  case kSubKeyboard:
  case kSubDisplay:
  case kSubOptions:
    if (flow_->inOptions()) {
      flow_->options().setMachineState(shared_);
    }
    flow_->update(in);
    flowRan_ = true;
    // onFlowActions performs the action-triggered transitions and
    // overlay arms (the original's frame functions write
    // DAT_00541493 directly). The screen->sub sync then applies only
    // while the flow still owns the sub-mode.
    onFlowActions();
    if (isFlowSub(subMode_)) {
      syncSubFromFlow();
    }
    shared_ = flow_->activeMachineState();
    break;
  default: {
    // sub == 0 falls through to the primary-mode dispatch at
    // 0x401486; the unported legacy screens (3 joystick, 6 perf) and
    // any out-of-range sub get no dispatch here — the loop head
    // still ran.
    if (subMode_ != kSubPrimary) {
      loopHead_(in);
      break;
    }
    if (primaryMode_ == mode::observed::noiseTransition) {
      loopHead_(in);
      noiseFrame_ = true;  // FUN_00418e04 — host draws the noise
    } else if (isFrontendMenuMode(primaryMode_)) {
      // The flow screens do their own accumulate+tick — the loop head
      // for these frames runs inside their update(). Keep the shared
      // mirror injected into the ACTIVE screen when it has a setter
      // (child screens carry their own copies — nothing outside the
      // flow reads the mirror while they run).
      if (flow_->inOptions()) {
        flow_->options().setMachineState(shared_);
      } else if (flow_->screen() == FrontendScreen::Root) {
        flow_->root().setMachineState(shared_);
      }
      flow_->update(in);
      flowRan_ = true;
      onFlowActions();
      if (isFlowSub(subMode_)) {
        syncSubFromFlow();
      }
      shared_ = flow_->activeMachineState();
    } else {
      // Gameplay modes 2/3/5/6/7/8 — the host dispatches them; the
      // loop head still ran.
      loopHead_(in);
    }
    break;
  }
  }
}

void FrontendShell::endFrame(double dtMs) {
  // The loop-tail timing update runs inside the active screen's frame
  // (FUN_0042fe78 family). Gameplay/noise frames time themselves.
  if (flowRan_) {
    flow_->endFrame(dtMs);
    shared_ = flow_->activeMachineState();
  } else if (overlayRan_) {
    frontendTimingUpdate(shared_.timing, dtMs);
  }
}

}  // namespace mdk
