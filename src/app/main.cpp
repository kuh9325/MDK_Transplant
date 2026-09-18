#include "app/application.h"

#include <cstdio>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
      "MDK-Native — macOS native skeleton (Phase 3A)\n"
      "usage: %s [options]\n"
      "  --data-path DIR       register a read-only original-data root\n"
      "  --preview-resource FILE RECORD\n"
      "                        decode + present one original BNI visual\n"
      "                        resource (Phase 4A; needs --data-path)\n"
      "  --preview-font FILE RECORD [TEXT]\n"
      "                        decode + present one original FTI font\n"
      "                        record — glyph atlas, or TEXT drawn with\n"
      "                        the proven advance rule (Phase 4C)\n"
      "  --preview-sprite FILE RECORD\n"
      "                        decode + present one original FTI sprite\n"
      "                        record over a checkerboard (Phase 4D)\n"
      "  --preview-options     compose the static front-end root-menu\n"
      "                        frame (Phase 4D; needs --data-path)\n"
      "  --preview-options-submenu\n"
      "                        compose the static options sub-menu frame\n"
      "                        (Phase 4F; needs --data-path)\n"
      "  --preview-display-submenu\n"
      "                        compose the static Display child frame\n"
      "                        (Phase 4H; needs --data-path)\n"
      "  --preview-sound-submenu\n"
      "                        compose the static Sound child frame\n"
      "                        (Phase 4I; needs --data-path)\n"
      "  --preview-mouse-submenu\n"
      "                        compose the static Mouse child frame\n"
      "                        (Phase 4J; needs --data-path)\n"
      "  --preview-keyboard-submenu\n"
      "                        compose the static Keyboard child frame\n"
      "                        (Phase 4K; needs --data-path)\n"
      "  --interactive-frontend\n"
      "                        run the reconstructed six-screen\n"
      "                        front-end flow — root menu + options\n"
      "                        sub-menu + Display + Sound + Mouse +\n"
      "                        Keyboard children\n"
      "                        (Phase 4E/4F/4H/4I/4J/4K; needs\n"
      "                        --data-path)\n"
      "  --frontend-root-only  test-only: keep OpenOptions deferred so\n"
      "                        the Phase 4E root regression snapshot stays\n"
      "                        reproducible (needs --interactive-frontend)\n"
      "  --settings-file FILE  native-owned frontend settings location\n"
      "                        (Phase 4G Skill persistence; MUST be\n"
      "                        outside the read-only data root)\n"
      "  --dump-ppm FILE       write last presented frame as PPM (debug)\n"
      "  --frames N            quit after N frames (smoke-test hook)\n"
      "  --selftest            inject synthetic input events and verify\n"
      "  --selftest-gameplay-input\n"
      "                        inject the deterministic Phase 5A\n"
      "                        keyboard/mouse script and verify the\n"
      "                        semantic gameplay-input frame each frame\n"
      "                        (optionally honors --settings-file)\n"
      "  --selftest-player-motion\n"
      "                        inject the deterministic Phase 5B\n"
      "                        movement script and verify the local\n"
      "                        player-motion state each frame\n"
      "                        (optionally honors --settings-file)\n"
      "  --no-relative-mouse   do not capture the mouse\n"
      "  -h, --help            this message\n",
      argv0);
}

} // namespace

int main(int argc, char** argv) {
  mdk::AppConfig cfg;
  std::string error;
  bool showHelp = false;
  if (!mdk::parseArgs(argc, argv, cfg, error, showHelp)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    usage(argv[0]);
    return 2;
  }
  if (showHelp) {
    usage(argv[0]);
    return 0;
  }
  mdk::Application app(std::move(cfg));
  return app.run();
}
