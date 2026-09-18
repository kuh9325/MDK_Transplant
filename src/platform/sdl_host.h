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

#include <cstdint>
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

  // --interactive-frontend --selftest support: one scripted input step
  // per call, invoked before pumpEvents with the current frame index.
  // Frames outside the script push nothing. `rootOnly` keeps the
  // Phase 4E root-only script so the pre-transition regression
  // snapshot stays reproducible.
  // Root script (Phase 4E, also frames 0-3 of the full script):
  //   0: DOWN-arrow tap (down+up same frame)  -> next item once
  //   1: mouse motion (0,-41)                 -> arrow to y=139 band 3
  //   2: left button down                     -> hit-test + activate
  //   3: left button up                       -> re-arm the latch
  // Phase 4F two-screen continuation:
  //   4: DOWN tap      -> options sel 8->0 wrap
  //   5: DOWN tap      -> options sel 0->1
  //   6: motion +162   -> arrow to y=301 band 7 (Display)
  //   7: button down   -> hit-test + activate -> Display action
  //   8: button up     -> re-arm the latch
  //   9: ESC tap       -> Back -> return to root (FUN_00420d68)
  void pushFrontendSelfTestStep(std::uint64_t frameIndex, bool rootOnly);

  // --selftest-gameplay-input support (Phase 5A): one scripted
  // keyboard/mouse step per call, invoked before pumpEvents with the
  // current frame index. The script exercises the real SDL->DIK->
  // internal seam plus the mouse delta/wheel/button path; the
  // application consumes each frame through consumeGameplayInput and
  // verifies the semantic results (expectations are computed from
  // the loaded bindings, so the same script covers factory and
  // --settings-file-supplied configurations):
  //   0: LEFT-arrow down        -> turn level starts
  //   1: (held)                 -> level repeats (no edge needed)
  //   2: LEFT-arrow up          -> level clears
  //   3: SPACE down             -> sniper edge pulse (factory)
  //   4: (held)                 -> edge does not repeat
  //   5: SPACE up               -> re-arm
  //   6: '1' tap                -> hidden weapon hotkey slot 0
  //   7: '5' tap                -> hidden weapon hotkey slot 4
  //   8: X + LEFT down          -> SideStep modifier -> strafe -1
  //   9: X + LEFT up            -> release both
  //  10: motion dx +320         -> mouse axis 'A' normalized turn
  //  11: wheel +1 (dz 120)      -> zoom accumulator charge/decay
  //  12: button A down          -> mask-decode Fire (+ loaded bits)
  //  13: button A up + C down   -> sniper synthetic edge
  //  14: (C held)               -> synthetic edge does not repeat
  //  15: button C up            -> re-arm the snipe latch
  //  16: settle frame           -> all controls idle
  void pushGameplaySelfTestStep(std::uint64_t frameIndex);

  // Install an SDL event filter that drops all REAL key/button/motion
  // events — injected events carry a sentinel device ID and pass
  // through. Without this the physical mouse/keyboard race the script
  // (real motion deltas coalesce against or replace injected ones).
  // Selftest-only; call before the frame loop starts.
  void isolateHardwareInputForSelftest();

  std::function<void()> onQuit;
  std::function<void(int w, int h)> onDrawableSizeChanged;

  void shutdown();

private:
  SDL_Window* window_ = nullptr;
  bool initialized_ = false;
};

} // namespace mdk

#endif // MDK_PLATFORM_SDL_HOST_H
