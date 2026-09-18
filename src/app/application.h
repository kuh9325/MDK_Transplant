// Application shell: lifecycle + main loop.
//
// Loop shape mirrors the observed original structure (Phase 2B): pump
// platform events -> tick/input -> mode dispatch -> render -> present,
// until the quit flag is set. Nothing here is gameplay.

#ifndef MDK_APP_APPLICATION_H
#define MDK_APP_APPLICATION_H

#include <filesystem>
#include <optional>
#include <string>

namespace mdk {

struct AppConfig {
  std::optional<std::filesystem::path> dataPath; // --data-path (read-only)
  std::optional<std::filesystem::path> dumpPpm; // --dump-ppm (debug hook)
  // --preview-resource FILE RECORD: decode one proven original visual
  // resource (Phase 4A: paletted BNI bitmaps) and present it through
  // the normal indexed framebuffer + Metal path. Requires --data-path.
  std::optional<std::string> previewFile;
  std::optional<std::string> previewRecord;
  // --preview-font FILE RECORD [TEXT]: decode one proven FTI font
  // record (Phase 4C: FONTSML/FONTBIG glyph layout) and present its
  // glyphs through the indexed framebuffer. Requires --data-path.
  // Without TEXT an atlas of every mapped glyph is drawn; with TEXT
  // the byte string is drawn once using the proven advance rule.
  std::optional<std::string> fontPreviewFile;
  std::optional<std::string> fontPreviewRecord;
  std::optional<std::string> fontPreviewText;
  // --preview-sprite FILE RECORD: decode one proven FTI sprite record
  // (Phase 4D: the ARROW cursor format — frame table + command stream)
  // and draw it over a synthetic checkerboard so transparency is
  // inspectable. Requires --data-path. QA aid only.
  std::optional<std::string> spritePreviewFile;
  std::optional<std::string> spritePreviewRecord;
  // --preview-options: compose the one proven static front-end frame
  // (Phase 4D): MDKOPT backdrop + OPT0..OPT4 scaled centered labels +
  // ARROW at the reset mouse position, all from original resources.
  // Requires --data-path. Static — no input, animation, or audio.
  bool optionsPreview = false;
  // --preview-options-submenu: compose the static options sub-menu
  // frame (Phase 4F): cleared framebuffer + OM_* scaled centered
  // labels (selection 8) + ARROW at the entry mouse position, under
  // the resident system palette. Requires --data-path.
  bool optionsSubmenuPreview = false;
  // --preview-display-submenu: compose the static Display child
  // screen frame (Phase 4H): cleared framebuffer + DSP_* scaled
  // centered rows (entry selection 2 = Quit) + the 4x48 swatch grid
  // + ARROW, under the composed display palette (SYS_PAL head +
  // ramps). Requires --data-path.
  bool displaySubmenuPreview = false;
  // --preview-sound-submenu: compose the static Sound child screen
  // frame (Phase 4I): cleared framebuffer + SND_TITL/SND_INFO
  // centered rows + the two volume rows (scaled labels, inclusive
  // bars at factory 70/100, FONTSML endpoints) + SND_DONE + ARROW
  // under the inherited options palette. Requires --data-path.
  bool soundSubmenuPreview = false;
  // --preview-mouse-submenu: compose the static Mouse child screen
  // frame (Phase 4J): cleared framebuffer + the four left rows
  // (JOY_TEST/M_ENA/M_NORM/JOY_QUIT at factory state) + the 4x16
  // button-map grid + three axis bars with centered markers + the
  // test indicator + ARROW — all FONTSML, under the inherited
  // options palette. Requires --data-path.
  bool mouseSubmenuPreview = false;
  // --preview-keyboard-submenu: compose the static Keyboard child
  // screen frame (Phase 4K): cleared framebuffer + the two-column
  // 19-row KM_* grid (factory bindings, LANG-selected glyphs) +
  // centered KM_RESET/KM_QUIT (entry selection 20 = the KM_QUIT
  // row) + ARROW — all FONTSML under the inherited options
  // palette. Requires --data-path.
  bool keyboardSubmenuPreview = false;
  // --interactive-frontend: run the reconstructed front-end flow —
  // Phase 4E root menu plus the Phase 4F options sub-menu transition
  // (OpenOptions enters the real OM_* screen; Back/Esc returns) plus
  // the Phase 4H Display child (options row 7 enters it; Esc/Quit
  // returns to options), the Phase 4I Sound child (options row 1,
  // Esc/Done resumes at row 1), the Phase 4J Mouse child (options
  // row 3, Esc/Quit resumes at row 3), and the Phase 4K Keyboard
  // child (options row 4, Esc/KM_QUIT resumes at row 4). Requires
  // --data-path. Downstream actions (gameplay, saves, other child
  // screens, real audio playback) stay deferred.
  bool interactiveFrontend = false;
  // --frontend-root-only (test-only): run the Phase 4E root controller
  // alone so OpenOptions stays a deferred semantic action — keeps the
  // pre-transition regression snapshot reproducible.
  bool frontendRootOnly = false;
  // --settings-file FILE (Phase 4G): the native-owned frontend
  // settings location — loaded at startup (FUN_00425de4 analogue)
  // and written when the options exit persists a dirty Skill
  // (FUN_004260ac analogue). MUST be outside the read-only
  // DataRoot — the port never writes into original data.
  std::optional<std::filesystem::path> settingsFile;
  std::uint64_t frames = 0;   // --frames N: quit after N frames (0 = run)
  bool selftest = false;      // --selftest: inject synthetic input events
  bool relativeMouse = true;  // --no-relative-mouse to disable
  int windowWidth = 960;      // logical points; drawable may be 2x (Retina)
  int windowHeight = 720;     // 4:3 default
};

class Application {
public:
  explicit Application(AppConfig cfg);
  int run();

private:
  AppConfig cfg_;
  bool selftestOk_ = true;
};

// Parse argv into AppConfig. Returns false (and fills error) on bad args
// or --help (help is not an error state but stops startup).
bool parseArgs(int argc, char** argv, AppConfig& cfg, std::string& error,
               bool& showHelp);

} // namespace mdk

#endif // MDK_APP_APPLICATION_H
