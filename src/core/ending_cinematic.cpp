#include "core/ending_cinematic.h"

#include <cstring>

namespace mdk {

bool EndingCinematic::open(std::span<const std::byte> flicBytes,
                           std::string* detail) {
  events_.clear();
  mark_ = 0;
  hold_ = 0.0;
  flcDone_ = false;
  return dec_.open(flicBytes, detail);
}

double EndingCinematic::rampT() const {
  // HYPOTHESIS ramp shape — FUN_0047b384's internals are uncaptured.
  // Brighten 0xd2..0xe8 -> 1, hold through 0xe9, fade back over
  // 0xe9..0x104 (the mark intervals are OBSERVED).
  if (mark_ <= 0xd1) return 0.0;
  if (mark_ <= 0xe9) {
    return (mark_ - 0xd1) / static_cast<double>(0xe9 - 0xd1);
  }
  if (mark_ <= 0x104) {
    return 1.0 - (mark_ - 0xe9) / static_cast<double>(0x104 - 0xe9);
  }
  return 0.0;
}

EndingStage EndingCinematic::step(double tickDelta) {
  if (flcDone_) return EndingStage::kDone;
  if (dec_.decoded() >= dec_.frameCount()) {
    // FUN_004140f4's nonzero edge — the FLIC stage is finished; the
    // caller runs the FUN_0047b674 MVE boundary.
    flcDone_ = true;
    return EndingStage::kMveBoundary;
  }

  // --- the 0x54f3b4 mark script (OBSERVED order) ---------------
  if (mark_ == 0xd2) {
    // FUN_0046d614 — restore the file palette + clear the hold.
    hold_ = 0.0;
  }
  const bool ramp =
      (mark_ > 0xd1 && mark_ < 0xe9) || (mark_ > 0xe8 && mark_ < 0x105);
  if (mark_ == 0xe9) {
    // The ~30-tick hold: arm 0x54f3b0 = 0x1e, run the ramp once,
    // then count down by the frame delta — while >0 this call ends
    // at the limiter (no decode, no present, mark_ holds).
    if (hold_ == 0.0) hold_ = 30.0;
    hold_ -= tickDelta;   // _DAT_0049b6e8 — OBSERVED 1 per tick
    if (hold_ > 0.0) return EndingStage::kHolding;
    hold_ = 0.0;
  }
  (void)ramp;   // the ramp runs each marked frame — rampT() above

  // --- sound marks (FUN_004022b8 play-once / FUN_0040210c stop) --
  switch (mark_) {
    case 0x01: events_.push_back({EndingEvent::kPlayOnce, "DOGSHIP"});  break;
    case 0x81: events_.push_back({EndingEvent::kPlayOnce, "DROP"});     break;
    case 0x85: events_.push_back({EndingEvent::kPlayOnce, "FLYBY"});    break;
    case 0xba:
    case 0xc4: events_.push_back({EndingEvent::kPlayOnce, "EXPLODE1"}); break;
    case 0xbc: events_.push_back({EndingEvent::kStop, "DOGSHIP"});      break;
    case 0xc2: events_.push_back({EndingEvent::kPlayOnce, "ENDEXP"});   break;
    default: break;
  }

  // --- FUN_00414158 decode -> framebuffer copy -> present -------
  std::string err;
  if (!dec_.nextFrame(&err)) {
    flcDone_ = true;
    return EndingStage::kMveBoundary;
  }
  if (dec_.paletteDirty()) {
    events_.push_back({EndingEvent::kPaletteDirty, nullptr});
  }
  ++mark_;
  return EndingStage::kPlaying;
}

std::array<std::uint8_t, 768> EndingCinematic::effectivePalette()
    const {
  const double t = rampT();
  std::array<std::uint8_t, 768> out = dec_.palette();
  if (t <= 0.0) return out;
  for (auto& c : out) {
    c = static_cast<std::uint8_t>(
        static_cast<int>(c) +
        static_cast<int>((255.0 - static_cast<int>(c)) * t + 0.5));
  }
  return out;
}

std::vector<EndingEvent> EndingCinematic::drainEvents() {
  std::vector<EndingEvent> out = std::move(events_);
  events_.clear();
  return out;
}

}  // namespace mdk
