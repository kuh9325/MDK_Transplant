#include "core/frontend_flow.h"

namespace mdk {

FrontendFlowController::FrontendFlowController(
    bool savesExist, const FrontendSettings& initial,
    SettingsPersistSink sink)
    : root_(savesExist), skill_(initial.skill),
      brightness_(initial.brightness),
      forcePCorrect_(initial.forcePCorrect),
      soundFx_(initial.soundFx),
      soundMusic_(initial.soundMusic),
      mouseOn_(initial.mouseOn),
      mouseYRevBits_(initial.mouseYReversed),
      axesMap_(initial.mouseWAxesMap),
      mouseButtMap_{initial.mouseWButtMapA, initial.mouseWButtMapB,
                    initial.mouseWButtMapC, initial.mouseWButtMapD},
      mouseScales_{initial.mouseWXScale, initial.mouseWYScale,
                   initial.mouseWZScale},
      baseSettings_(initial),
      persistSink_(std::move(sink)) {}

void FrontendFlowController::update(const FrontendMenuInput& in) {
  if (screen_ == FrontendScreen::Options) {
    options_->update(in);
  } else if (screen_ == FrontendScreen::Display) {
    display_->update(in);
  } else if (screen_ == FrontendScreen::Sound) {
    sound_->update(in);
  } else if (screen_ == FrontendScreen::Mouse) {
    mouse_->update(in);
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
  if (!options_ || screen_ != FrontendScreen::Options) {
    return OptionsAction::None;
  }
  if (options_->pendingAction() == OptionsAction::Back) {
    options_->consumeAction();
    returnToRoot();
    return OptionsAction::None;  // consumed by the transition
  }
  if (options_->pendingAction() == OptionsAction::Display) {
    options_->consumeAction();
    enterDisplay();
    return OptionsAction::None;  // consumed by the transition
  }
  if (options_->pendingAction() == OptionsAction::Sound) {
    options_->consumeAction();
    enterSound();
    return OptionsAction::None;  // consumed by the transition
  }
  if (options_->pendingAction() == OptionsAction::Mouse) {
    options_->consumeAction();
    enterMouse();
    return OptionsAction::None;  // consumed by the transition
  }
  return options_->consumeAction();
}

DisplayAction FrontendFlowController::consumeDisplayAction() {
  if (!display_ || screen_ != FrontendScreen::Display) {
    return DisplayAction::None;
  }
  if (display_->pendingAction() == DisplayAction::Back) {
    display_->consumeAction();
    returnToOptions();
    return DisplayAction::None;  // consumed by the transition
  }
  return display_->consumeAction();
}

SoundAction FrontendFlowController::consumeSoundAction() {
  if (!sound_ || screen_ != FrontendScreen::Sound) {
    return SoundAction::None;
  }
  if (sound_->pendingAction() == SoundAction::Back) {
    sound_->consumeAction();
    returnToOptionsFromSound();
    return SoundAction::None;  // consumed by the transition
  }
  return sound_->consumeAction();
}

MouseAction FrontendFlowController::consumeMouseAction() {
  if (!mouse_ || screen_ != FrontendScreen::Mouse) {
    return MouseAction::None;
  }
  if (mouse_->pendingAction() == MouseAction::Back) {
    mouse_->consumeAction();
    returnToOptionsFromMouse();
    return MouseAction::None;  // consumed by the transition
  }
  return mouse_->consumeAction();
}

std::vector<SoundAudioEvent> FrontendFlowController::drainAudioEvents() {
  std::vector<SoundAudioEvent> out = std::move(audioEvents_);
  audioEvents_.clear();
  if (sound_) {
    auto ev = sound_->drainAudioEvents();
    out.insert(out.end(), ev.begin(), ev.end());
  }
  return out;
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
    // of the sink's result, mirroring the original exactly. The
    // emit starts from baseSettings_ so entries the screens never
    // touched (the mouse D set, map strings, scales) round-trip
    // instead of silently reverting (Phase 4J).
    if (persistSink_) {
      FrontendSettings s = baseSettings_;
      s.skill = skill_;
      s.brightness = brightness_;
      s.forcePCorrect = forcePCorrect_;
      s.soundFx = soundFx_;
      s.soundMusic = soundMusic_;
      s.mouseOn = mouseOn_;
      s.mouseYReversed = mouseYRevBits_;
      s.mouseWAxesMap = axesMap_;
      s.mouseWButtMapA = mouseButtMap_[0];
      s.mouseWButtMapB = mouseButtMap_[1];
      s.mouseWButtMapC = mouseButtMap_[2];
      s.mouseWButtMapD = mouseButtMap_[3];
      s.mouseWXScale = mouseScales_[0];
      s.mouseWYScale = mouseScales_[1];
      s.mouseWZScale = mouseScales_[2];
      persistSink_(s);
    }
    settingsDirty_ = false;
  }
  root_.setMachineState(options_->machineState());
  options_.reset();
  screen_ = FrontendScreen::Root;
}

// FUN_0041d020 (OBSERVED, disasm_41d020.txt / phase4h_funcs.txt):
// DAT_00541493 = 7, DAT_0054b834 = 2 — plus the palette work (dlut
// saves the active palette, slut = SYS_PAL head + 4x48 ramps
// uploaded; the renderer binds that composition every frame, so the
// controller only carries the state). The options controller stays
// alive underneath: the original's options globals are never
// re-initialized on the way back. The shared input-machine globals
// carry over untouched — no reset anywhere in the entry.
void FrontendFlowController::enterDisplay() {
  display_.emplace(options_->machineState(), brightness_,
                   forcePCorrect_, options_->settingsDirty());
  screen_ = FrontendScreen::Display;
}

// FUN_0041d144 (OBSERVED): DAT_00541493 = 0x0b — the options screen
// resumes with _DAT_0054bd34 still 7 (the Display row). The saved
// dlut palette is re-uploaded (the renderer rebinds the options
// palette every frame anyway). The child's machine state and the
// shared dirty flag (DAT_00541486 — one global) transfer back;
// DAT_0054147e/DAT_00541482 keep their mutated values in the flow.
// NO persist here — FUN_00420d68 handles it on the options exit.
void FrontendFlowController::returnToOptions() {
  brightness_ = display_->brightness();
  forcePCorrect_ = display_->forcePCorrect();
  options_->setMachineState(display_->machineState());
  options_->setSettingsDirty(display_->settingsDirty());
  display_.reset();
  screen_ = FrontendScreen::Options;
}

// FUN_0042322c (OBSERVED, decomp_snd.txt / disasm_42322c.txt):
// DAT_00541493 = 2; FUN_0041d774 stops the ambient MAINSONG; the
// MISC\MDKSOUND.SNI blob loads and OPTSONG/OPTBUTT resolve;
// FUN_00402388(OPTSONG, 0) starts the screen's song;
// DAT_0054bdb8 = 3. DAT_0054bdbc is NOT written — the sound
// selection is a process global (BSS 0 on the first entry,
// retained later). The shared input-machine globals carry over
// untouched, and the options controller stays alive underneath
// exactly like the Display entry.
void FrontendFlowController::enterSound() {
  sound_.emplace(options_->machineState(), soundSelection_, soundFx_,
                 soundMusic_, options_->settingsDirty());
  screen_ = FrontendScreen::Sound;
}

// FUN_00423280 (OBSERVED, decomp_snd3.txt): DAT_00541493 = 0x0b —
// the options screen resumes with _DAT_0054bd34 still 1 (the
// Sound row). FUN_0040210c stops OPTSONG; FUN_00428b34 releases
// the SNI records; DAT_00541492 == 0 in the front-end, so
// FUN_0041d720 restarts the ambient MAINSONG. The child's machine
// state, its selection (the process global DAT_0054bdbc), the
// volume globals, and the shared dirty flag all carry back; the
// exit frame's audio events move to the flow queue before the
// controller dies. NO persist here — FUN_00420d68 handles it on
// the options exit.
void FrontendFlowController::returnToOptionsFromSound() {
  soundSelection_ = sound_->selection();
  soundFx_ = sound_->soundFx();
  soundMusic_ = sound_->soundMusic();
  options_->setMachineState(sound_->machineState());
  options_->setSettingsDirty(sound_->settingsDirty());
  auto ev = sound_->drainAudioEvents();
  audioEvents_.insert(audioEvents_.end(), ev.begin(), ev.end());
  sound_.reset();
  screen_ = FrontendScreen::Options;
}

// FUN_00421664 (OBSERVED, disasm_421664.txt): DAT_00541493 = 4,
// DAT_0054bd40 = 0 (selection reset), DAT_0054bd38 = 0 (grid
// column reset), DAT_0054bd3c = 3 / DAT_0054bd44 = 4. No palette
// work — the screen inherits the options composition. The options
// controller stays alive underneath exactly like the Display/
// Sound entries; the shared input-machine globals carry over
// untouched.
void FrontendFlowController::enterMouse() {
  mouse_.emplace(options_->machineState(), mouseOn_, mouseYRevBits_,
                 axesMap_, mouseButtMap_, mouseScales_,
                 options_->settingsDirty());
  screen_ = FrontendScreen::Mouse;
}

// FUN_004217e8 inline exit (OBSERVED): Esc (checked first) or
// row-3 activate writes DAT_00541493 = 0x0b and RETs — the options
// screen resumes with _DAT_0054bd34 still 3 (the Mouse row). No
// resource release, no palette restore. The child's machine state,
// the mutated W-set globals, and the shared dirty flag all carry
// back. NO persist here — FUN_00420d68 handles it on the options
// exit.
void FrontendFlowController::returnToOptionsFromMouse() {
  mouseOn_ = mouse_->mouseOn();
  mouseYRevBits_ = mouse_->mouseYReversedBits();
  axesMap_ = mouse_->axesMap();
  mouseButtMap_ = mouse_->buttMap();
  mouseScales_ = mouse_->scales();
  options_->setMachineState(mouse_->machineState());
  options_->setSettingsDirty(mouse_->settingsDirty());
  mouse_.reset();
  screen_ = FrontendScreen::Options;
}

} // namespace mdk
