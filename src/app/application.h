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
