// Neutral per-frame input state.
//
// ORIGINAL ENGINE OBSERVATION: the original polls keyboard+mouse every
// frame (DInput device objects on Win95, INT9/port-60h scancodes on DOS)
// and produces platform-produced input state for shared engine code
// (Phase 2B). DOS MDKDOS.EXE exposes no wheel/Z-axis binding (user-
// observed, RUNTIME_ORACLE.md §2A.2).
//
// NATIVE PORT PROJECT DECISIONS:
//   - Key identity is the platform scancode number (SDL scancode values on
//     the SDL platform seam). Game-action mapping is a later-phase concern.
//   - Mouse wheel data is preserved (both float and integer ticks) even
//     though the DOS build cannot consume it — the native port does not
//     throw information away. It is NOT bound to any game action yet.
//   - Relative mouse motion is accumulated per frame.

#ifndef MDK_INPUT_STATE_H
#define MDK_INPUT_STATE_H

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace mdk {

struct KeyEvent {
  int scancode = 0;   // platform scancode numbering (SDL on this seam)
  bool down = false;
  bool repeat = false;
};

struct ButtonEvent {
  std::uint8_t button = 0;
  bool down = false;
};

class InputState {
public:
  // Called once at the top of each frame before event collection.
  void beginFrame();

  // Recorders — invoked by the platform layer only.
  void key(int scancode, bool down, bool repeat);
  void mouseMotion(float dx, float dy);
  void mouseButton(std::uint8_t button, bool down);
  void mouseWheel(float x, float y, std::int32_t intX, std::int32_t intY);

  // Queries — consumed by application/mode code.
  bool keyDown(int scancode) const { return pressed_.contains(scancode); }
  const std::vector<KeyEvent>& keyEvents() const { return keyEvents_; }
  const std::vector<ButtonEvent>& buttonEvents() const { return buttonEvents_; }

  float mouseDx() const { return mouseDx_; }
  float mouseDy() const { return mouseDy_; }
  bool mouseButtonDown(std::uint8_t button) const {
    return buttons_.contains(button);
  }

  float wheelX() const { return wheelX_; }
  float wheelY() const { return wheelY_; }
  std::int32_t wheelTicksX() const { return wheelIntX_; }
  std::int32_t wheelTicksY() const { return wheelIntY_; }

private:
  std::unordered_set<int> pressed_;
  std::unordered_set<int> buttons_;
  std::vector<KeyEvent> keyEvents_;
  std::vector<ButtonEvent> buttonEvents_;
  float mouseDx_ = 0, mouseDy_ = 0;
  float wheelX_ = 0, wheelY_ = 0;
  std::int32_t wheelIntX_ = 0, wheelIntY_ = 0;
};

} // namespace mdk

#endif // MDK_INPUT_STATE_H
