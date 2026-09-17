#include "core/clock.h"

namespace mdk {

Clock::Clock()
    : start_(std::chrono::steady_clock::now()), last_(start_) {}

FrameTick Clock::tick() {
  const auto now = std::chrono::steady_clock::now();
  FrameTick t;
  t.index = frame_++;
  t.dtSeconds = std::chrono::duration<double>(now - last_).count();
  t.elapsedSeconds = std::chrono::duration<double>(now - start_).count();
  last_ = now;
  return t;
}

} // namespace mdk
