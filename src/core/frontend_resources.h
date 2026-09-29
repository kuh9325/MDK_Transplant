// Phase 18B.2A — the decoded original resources the frontend/menu
// screens need, shared by the native app (src/app/application.cpp)
// and the Godot bridge presenter.
//
// All bindings resolve through the proven original paths
// (MISC/OPTIONS.BNI record MDKOPT + the MISC/MDKFONT.FTI record
// names the original's resolve calls produce). The overlay/dialog
// string records (SV_*/ABORT*/HELP_*/PAUSED/SVBAD/OPTSTRT) are the
// same OBSERVED NUL-terminated payload shape as OPT*/OM_*.
//
// EVIDENCE: the draw-side layouts these records feed are OBSERVED
// in the per-screen module headers (frontend_menu.h, options_menu.h,
// display_menu.h, sound_menu.h, mouse_menu.h, keyboard_menu.h,
// save_slot_list.h, save_name_entry.h, abort_console.h).
//
#ifndef MDK_CORE_FRONTEND_RESOURCES_H
#define MDK_CORE_FRONTEND_RESOURCES_H

#include "core/fti_font.h"
#include "core/fti_sprite.h"
#include "core/indexed_image.h"
#include "core/keyboard_menu.h"
#include "core/mouse_menu.h"
#include "core/options_menu.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace mdk {

class DataRoot;

struct FrontendResources {
  IndexedImage backdrop;            // MISC/OPTIONS.BNI record MDKOPT
  FtiFont fontBig;                  // MISC/MDKFONT.FTI record FONTBIG
  FtiFont fontSml;                  // record FONTSML (sound end labels)
  FtiSprite arrow;                  // record ARROW (frame 0 used)
  std::vector<std::string> optStrings;  // OPT0..OPT4 C strings
  // Phase 4F options sub-menu (FUN_00420eac): the OM_* row labels
  // (records ARE NUL-terminated strings — OBSERVED) and the resident
  // system-palette head the options palette upload uses.
  std::array<std::string, kOptionsItemCount> omStrings;
  std::array<std::string, 3> omSkill;    // OM_SK_0/1/2 skill variants
  // Phase 4H display child (FUN_0041d1e0): the DSP_* row records —
  // DSP_BRGT is a printf format ("Brightness %d"), the rest plain
  // strings (OBSERVED NUL-terminated, same as OM_*).
  std::string dspBrightness;             // DSP_BRGT
  std::string dspDetailHigh;             // DSP_DETH
  std::string dspDetailLow;              // DSP_DETL
  std::string dspQuit;                   // DSP_QUIT
  // Phase 4I sound child (FUN_004233d8): the SND_* records — same
  // NUL-terminated string shape. SND_SET ("Setup Device") exists in
  // the FTI but is never resolved by the proven frame (vestigial —
  // not loaded).
  std::string sndTitle;                  // SND_TITL
  std::string sndInfo;                   // SND_INFO
  std::string sndEffects;                // SND_FX
  std::string sndMusic;                  // SND_MUSI
  std::string sndDone;                   // SND_DONE
  std::string sndEnd100;                 // SND_100
  std::string sndEnd0;                   // SND_0
  // Phase 4J mouse child (FUN_004217e8): the JOY_*/M_* records —
  // same NUL-terminated string shape, all drawn FONTSML.
  std::string mouseTest;                 // JOY_TEST
  std::string mouseEnabled;              // M_ENA
  std::string mouseDisabled;             // M_DIS
  std::string mouseReversed;             // M_REV
  std::string mouseNormal;               // M_NORM
  std::string mouseQuit;                 // JOY_QUIT
  std::string mouseButtons;              // JOY_B (grid header)
  std::array<std::string, kMouseGridRows> mouseActions;   // JOY_BA..BP
  std::array<std::string, 9> mouseAxisNames; // JOY_A0 + JOY_AA..AH
  std::array<std::string, kMouseAxisCount> mouseAxisCaps; // JOY_AX0..2
  // Phase 4K keyboard child (FUN_0041f18c): the KM_* records —
  // same NUL-terminated string shape, all drawn FONTSML — plus the
  // LANG record's first byte (the glyph-table selector; a missing
  // LANG soft-resolves to English exactly like FUN_00414930's 0).
  std::array<std::string, kKeyboardBindingRows> kbRows;   // KM_* rows
  std::string kbReset;                 // KM_RESET
  std::string kbQuit;                  // KM_QUIT
  std::string kbDoit;                  // KM_DOIT
  char kbLangTag = 0;                  // LANG record first byte
  // Phase 18B.2A — the overlay/dialog records the sub-mode 1/8/9/10
  // screens resolve (OBSERVED NUL-terminated strings, same shape):
  std::string optStrt;               // OPTSTRT  "Starting New Game..."
  std::string svOpt1;                // SVOPT1   "Select Saved Game\nESC to Quit"
  std::string svOpt2;                // SVOPT2   "Restoring %s"
  std::string svOpt3;                // SVOPT3   "No Saved Games Found"
  std::string svTitle;               // SV_TITLE "Name for Saved Game"
  std::string svAsk;                 // SV_ASK   "Save Game?"
  std::string svBad;                 // SVBAD    "Invalid/Corrupt File"
  std::string svFail;                // SV_FAIL  "Can't save to\n%s\nCD Drive?"
  std::string paused;                // PAUSED   "Game Paused"
  std::string abort1;                // ABORT1   "Really Quit?"
  std::string abort2;                // ABORT2   "Yes"
  std::string abort3;                // ABORT3   "No"
  std::string helpTop;               // HELP_TOP "MDK Help Screen"
  std::string helpBot;               // HELP_BOT "Items marked with * are remappable"
  std::array<std::string, 18> helpLines;  // HELP_01..HELP_18
  std::array<std::byte, 192> sysPalHead{};  // SYS_PAL record head
  bool savesExist = false;          // FUN_00428290 SAVES/*.SAV probe
};

// Load and decode every front-end resource. Fills `err` -> false on
// failure (missing files/records, decode errors).
bool loadFrontendResources(DataRoot& root, FrontendResources& res,
                           std::string* err);

}  // namespace mdk

#endif  // MDK_CORE_FRONTEND_RESOURCES_H
