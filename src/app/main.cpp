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
      "  --interactive-frontend\n"
      "                        run the reconstructed root-menu selection\n"
      "                        controller — original-style keyboard/mouse\n"
      "                        input, scale ramp, semantic actions\n"
      "                        (Phase 4E; needs --data-path)\n"
      "  --dump-ppm FILE       write last presented frame as PPM (debug)\n"
      "  --frames N            quit after N frames (smoke-test hook)\n"
      "  --selftest            inject synthetic input events and verify\n"
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
