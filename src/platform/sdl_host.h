// SDL3 platform host: process init, window, event pump.
//
// ORIGINAL ENGINE OBSERVATION: the Win95 build owns a PeekMessage pump +
// DirectInput device polls per frame; DOS uses VESA/INT9 directly
// (Phase 2B). The shared engine above the seam consumes platform-produced
// input state. This class is the native equivalent of that seam.
//
// NATIVE PORT PROJECT DECISION: SDL3 supplies windowing, event delivery,
// and relative-mouse capture; Metal presentation attaches to the SDL
// window via SDL_Metal_CreateView.

#ifndef MDK_PLATFORM_SDL_HOST_H
#define MDK_PLATFORM_SDL_HOST_H

#include <functional>
#include <string>

struct SDL_Window;

namespace mdk {

class InputState;

class SdlHost {
public:
  SdlHost() = default;
  ~SdlHost();

  SdlHost(const SdlHost&) = delete;
  SdlHost& operator=(const SdlHost&) = delete;

  // SDL_Init(VIDEO|EVENTS). Logs and returns false on failure.
  bool init();

  // Create the resizable, Metal-capable, high-pixel-density window.
  bool createWindow(int width, int height, const char* title);

  // Pump the SDL event queue once: translate input events into `input`
  // and invoke the registered callbacks for window/quit events.
  void pumpEvents(InputState& input);

  void setRelativeMouse(bool enabled);
  bool relativeMouse() const;

  SDL_Window* window() const { return window_; }
  void windowSizeInPixels(int* w, int* h) const;
  void setTitle(const std::string& title);

  // --selftest support: push synthetic key/motion/button/wheel events into
  // the SDL queue so the event->InputState path can be validated without
  // macOS Accessibility permission. verifySelfTestInput checks that the
  // injected events arrived intact.
  void injectSelfTestEvents();
  bool verifySelfTestInput(const InputState& input) const;

  std::function<void()> onQuit;
  std::function<void(int w, int h)> onDrawableSizeChanged;

  void shutdown();

private:
  SDL_Window* window_ = nullptr;
  bool initialized_ = false;
};

} // namespace mdk

#endif // MDK_PLATFORM_SDL_HOST_H
