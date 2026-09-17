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
  // --interactive-frontend: run the reconstructed front-end flow —
  // Phase 4E root menu plus the Phase 4F options sub-menu transition
  // (OpenOptions enters the real OM_* screen; Back/Esc returns).
  // Requires --data-path. Downstream actions (gameplay, saves, child
  // options screens, settings mutations, audio) stay deferred.
  bool interactiveFrontend = false;
  // --frontend-root-only (test-only): run the Phase 4E root controller
  // alone so OpenOptions stays a deferred semantic action — keeps the
  // pre-transition regression snapshot reproducible.
  bool frontendRootOnly = false;
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
