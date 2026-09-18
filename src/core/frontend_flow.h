// Phase 4F — two-screen front-end flow controller.
//
// EVIDENCE (instruction-level, original binary — see
// docs/ENGINE_RECONSTRUCTION.md Phase 4F):
//
//   Root -> Options (FUN_0041dc90 selection 3 dispatch):
//     FUN_00420cf0 — writes DAT_00541493 = 0x0b and _DAT_0054bd34 = 8,
//     saves the active palette into the "svlut" buffer (FUN_0046d614),
//     uploads DAT_00540820 (FUN_0046d208). No mouse/tick/ramp reset —
//     the shared globals continue on the new screen.
//
//   Options -> Root (FUN_00420eac selection 8 activate / DIK_ESCAPE —
//   LEFT/RIGHT on row 8 are bound-gated no-ops):
//     FUN_00420d68 — writes DAT_00541493 = 0 (root mode), persists
//     settings when the dirty flag (DAT_00541486) is set — Phase 4G
//     routes that FUN_004260ac call through the native-owned
//     SettingsPersistSink seam below (frontend_settings.h), never
//     into the data root — runs the DAT_00541492-gated
//     FUN_00402590/FUN_004348d4 calls, restores the saved palette
//     (FUN_0046d208 of svlut), and releases svlut. Again no
//     input-state reset.
//
//   Options -> Display (FUN_00420eac row 7 — all three dispatch
//   tables bind it to 0x4210ce, OBSERVED):
//     FUN_0041d020 — writes DAT_00541493 = 7 and DAT_0054b834 = 2,
//     saves the active palette into "dlut", composes "slut"
//     (SYS_PAL head + 4x48 ramps) and uploads it. No input-state
//     reset. The options screen stays alive underneath — its
//     selection (7) and machine state are shared globals, not
//     per-screen copies.
//
//   Display -> Options (FUN_0041d1e0 Esc / row-2 activate):
//     FUN_0041d144 — writes DAT_00541493 = 0x0b, re-uploads the
//     saved dlut palette, frees both buffers. Options resumes with
//     selection 7; the shared dirty flag carries the child's
//     mutations back (DAT_00541486 is one global — the options
//     exit persists it later). No persist happens here.
//
//   Options -> Sound (FUN_00420eac row 1 — all three dispatch
//   tables bind it to 0x420fdd, OBSERVED):
//     FUN_0042322c — writes DAT_00541493 = 2, stops the ambient
//     MAINSONG (FUN_0041d774), loads MISC\MDKSOUND.SNI, resolves
//     OPTSONG/OPTBUTT, starts OPTSONG, sets DAT_0054bdb8 = 3.
//     DAT_0054bdbc (the sound selection) is NOT reset — a process
//     global that survives entries. No input-state reset.
//
//   Sound -> Options (FUN_004233d8 Esc / row-2 activate):
//     FUN_00423280 — writes DAT_00541493 = 0x0b, stops OPTSONG
//     (FUN_0040210c), releases the SNI (FUN_00428b34), restarts
//     MAINSONG (FUN_0041d720 — DAT_00541492 == 0 in the front-end).
//     Options resumes with _DAT_0054bd34 still 1; the shared
//     dirty flag carries the volume mutations back. No persist
//     happens here either.
//
//   This controller owns the four reconstructed screens and
//   performs exactly those transitions. It is deliberately NOT a
//   generic UI router — no hierarchy, no widget system.
//
#ifndef MDK_CORE_FRONTEND_FLOW_H
#define MDK_CORE_FRONTEND_FLOW_H

#include "core/display_menu.h"
#include "core/frontend_menu.h"
#include "core/frontend_settings.h"
#include "core/options_menu.h"
#include "core/sound_menu.h"

#include <functional>
#include <optional>
#include <vector>

namespace mdk {

enum class FrontendScreen {
  Root,     // FUN_0041dc90 — mode 0
  Options,  // FUN_00420eac — mode 0x0b
  Display,  // FUN_0041d1e0 — mode 7 (Phase 4H)
  Sound,    // FUN_004233d8 — mode 2 (Phase 4I)
};

// Phase 4G persistence seam — the native-owned counterpart of
// FUN_004260ac. Invoked by returnToRoot() ONLY when the dirty flag
// (DAT_00541486) was latched, mirroring the original's TEST/JNZ
// gate; the flag then clears unconditionally (0x420dbc — the
// original clears after the call regardless of the write's
// success, and FUN_004260ac itself returns silently when its fopen
// fails). Return value: whether the write succeeded — observable
// by the caller for diagnostics; the flow does not branch on it.
using SettingsPersistSink = std::function<bool(const FrontendSettings&)>;

class FrontendFlowController {
public:
  // Mirrors the front-end entry (FUN_0041d85c): the root controller
  // starts in its proven entry state; the options globals start at
  // the post-config values `initial` carries (the FUN_00425de4
  // defaults copy + config application — canonical BUILD_A result:
  // skill 1, dirty clear; the native -mapok flag is not yet modeled
  // and stays clear). `sink` is the Phase 4G persistence seam; with
  // no sink installed the dirty-flag lifecycle still runs but no
  // write is attempted (like FUN_004260ac's silent fopen failure).
  explicit FrontendFlowController(bool savesExist,
                                  const FrontendSettings& initial = {},
                                  SettingsPersistSink sink = {});

  FrontendScreen screen() const { return screen_; }
  bool inOptions() const { return screen_ == FrontendScreen::Options; }

  FrontendMenuController& root() { return root_; }
  const FrontendMenuController& root() const { return root_; }
  // Valid only while screen() == Options (the options globals stay
  // alive underneath the Display child — FUN_0041d144 returns to
  // mode 0x0b without re-running FUN_00420cf0, so _DAT_0054bd34 is
  // still 7 on resume).
  OptionsMenuController& options() { return *options_; }
  const OptionsMenuController& options() const { return *options_; }
  // Valid only while screen() == Display.
  DisplayMenuController& display() { return *display_; }
  const DisplayMenuController& display() const { return *display_; }
  // Valid only while screen() == Sound.
  SoundMenuController& sound() { return *sound_; }
  const SoundMenuController& sound() const { return *sound_; }

  // One frame: route the neutral input to the active controller.
  void update(const FrontendMenuInput& in);

  // Consume the active screen's semantic action. Transition-driving
  // actions are handled internally and reported as consumed:
  //   root    OpenOptions -> FUN_00420cf0 (enters options)
  //   options Back        -> FUN_00420d68 (returns to root)
  //   options Display     -> FUN_0041d020 (enters the display child)
  //   options Sound       -> FUN_0042322c (enters the sound child)
  //   display Back        -> FUN_0041d144 (returns to options)
  //   sound   Back        -> FUN_00423280 (returns to options)
  // All other actions pass through to the caller unchanged (semantic
  // events only — downstream systems deferred).
  FrontendAction consumeRootAction();
  OptionsAction consumeOptionsAction();
  DisplayAction consumeDisplayAction();
  SoundAction consumeSoundAction();

  // The front-end settings globals (DAT_0054147a / DAT_005414f4 /
  // DAT_0054147e / DAT_00541482 / DAT_00541308 / DAT_0054130c) live
  // across entries — they belong to the flow, not one controller.
  int skill() const { return skill_; }
  bool devHidden() const { return devHidden_; }
  int brightness() const { return brightness_; }
  bool forcePCorrect() const { return forcePCorrect_; }
  int soundFx() const { return soundFx_; }       // DAT_00541308
  int soundMusic() const { return soundMusic_; } // DAT_0054130c
  // DAT_00541486 — the shared settings-dirty flag (see returnToRoot).
  bool settingsDirty() const { return settingsDirty_; }

  // Phase 4I semantic audio events — the flow-level queue for the
  // proven OPTSONG/OPTBUTT/ambient-song triggers. The sound
  // controller's per-frame events move here (the original's audio
  // system is global — the exit's SongStop/AmbientSongStart
  // survive the controller's destruction). Callers drain once per
  // frame; no audio backend consumes them in Phase 4I.
  std::vector<SoundAudioEvent> drainAudioEvents();

private:
  void enterOptions();      // FUN_00420cf0
  void returnToRoot();      // FUN_00420d68
  void enterDisplay();      // FUN_0041d020
  void returnToOptions();   // FUN_0041d144
  void enterSound();        // FUN_0042322c
  void returnToOptionsFromSound();  // FUN_00423280

  FrontendMenuController root_;
  std::optional<OptionsMenuController> options_;
  std::optional<DisplayMenuController> display_;
  std::optional<SoundMenuController> sound_;
  FrontendScreen screen_ = FrontendScreen::Root;
  // DAT_0054147a — the post-config startup value (`initial`): the
  // FUN_00425de4 defaults copy yields factory 1 ("Skill - Normal"),
  // then the config applies `Skill = n` overrides. Mutations inside
  // the options screen write back here — it is a process global,
  // not per-screen state.
  int skill_;
  bool devHidden_ = false;      // DAT_005414f4 — canonical 0 (no -mapok)
  // DAT_0054147e / DAT_00541482 — the post-config Brightness (0..7)
  // and ForcePCorrect values. Same process-global lifetime as
  // skill_: the Display screen mutates them, they flow back here on
  // FUN_0041d144, and FUN_00420d68 persists them.
  int brightness_;
  bool forcePCorrect_;
  // DAT_00541308 / DAT_0054130c — the post-config SoundFX/SoundMusic
  // volumes [0,100]. Same process-global lifetime: the Sound screen
  // mutates them, they flow back here on FUN_00423280, and
  // FUN_00420d68 persists them.
  int soundFx_;
  int soundMusic_;
  // DAT_0054bdbc — the sound-screen selection. FUN_0042322c does
  // NOT reset it: BSS-zero on the first entry, retained across
  // later entries (FUN_00423280 leaves it wherever the frame
  // handler put it).
  int soundSelection_ = 0;
  bool settingsDirty_ = false;  // DAT_00541486 — canonical 0 (inside
                                // the factory-defaults copy block)
  SettingsPersistSink persistSink_;  // FUN_004260ac seam — see above
  // Sound-screen audio events collected at controller destruction
  // (the exit frame's SongStop/AmbientSongStart) — merged into
  // drainAudioEvents() with whatever the live controller holds.
  std::vector<SoundAudioEvent> audioEvents_;
};

} // namespace mdk

#endif // MDK_CORE_FRONTEND_FLOW_H
