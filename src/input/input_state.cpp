#include "input/input_state.h"

namespace mdk {

void InputState::beginFrame() {
  keyEvents_.clear();
  buttonEvents_.clear();
  mouseDx_ = mouseDy_ = 0;
  wheelX_ = wheelY_ = 0;
  wheelIntX_ = wheelIntY_ = 0;
}

void InputState::key(int scancode, bool down, bool repeat) {
  keyEvents_.push_back({scancode, down, repeat});
  if (down) {
    pressed_.insert(scancode);
  } else {
    pressed_.erase(scancode);
  }
}

void InputState::mouseMotion(float dx, float dy) {
  mouseDx_ += dx;
  mouseDy_ += dy;
}

void InputState::mouseButton(std::uint8_t button, bool down) {
  buttonEvents_.push_back({button, down});
  if (down) {
    buttons_.insert(button);
  } else {
    buttons_.erase(button);
  }
}

void InputState::mouseWheel(float x, float y, std::int32_t intX,
                            std::int32_t intY) {
  wheelX_ += x;
  wheelY_ += y;
  wheelIntX_ += intX;
  wheelIntY_ += intY;
}

} // namespace mdk
