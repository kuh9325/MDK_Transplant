// Monotonic timing for the main loop.
//
// ORIGINAL ENGINE OBSERVATION: the original accumulates a per-frame delta
// (DAT_0049b6e8) fed by timeGetTime (Phase 2B). Exact original timing
// constants and integration semantics are UNKNOWN — this clock deliberately
// does not reproduce them.
//
// NATIVE PORT PROJECT DECISION: std::chrono::steady_clock, frame counter,
// and delta-time only. No fixed-step gameplay timestep yet.

#ifndef MDK_CORE_CLOCK_H
#define MDK_CORE_CLOCK_H

#include <chrono>
#include <cstdint>

namespace mdk {

struct FrameTick {
  std::uint64_t index = 0;   // completed frames before this tick
  double dtSeconds = 0.0;    // time since previous tick
  double elapsedSeconds = 0.0; // time since clock start
};

class Clock {
public:
  Clock();

  // Advance the frame counter and produce the tick for the new frame.
  FrameTick tick();

  std::uint64_t frameCount() const { return frame_; }

private:
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point last_;
  std::uint64_t frame_ = 0;
};

} // namespace mdk

#endif // MDK_CORE_CLOCK_H
