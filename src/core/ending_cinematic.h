// Phase 19D — the mode-8 ending pump: FUN_0047b06c body +
// FUN_0047b0fc init + FUN_0047b3f4 frame pump (OBSERVED —
// analysis-private/logs/p19a_mode8.txt).
//
// FUN_0047b0fc: open+stream `MISC\FLIC\MDKEND.FLC` (the original
// streams it under a 350-px progress bar — the port's host may draw
// its own progress), resolve the FINISH.BNI WAV handles
// (EXPLODE1/DROP/FLYBY/ENDEXP/DOGSHIP), init the FLIC ctx, alloc the
// 600-wide buffer.
//
// FUN_0047b3f4 frame pump — per host tick:
//   * frame-mark counter 0x54f3b4 (starts 0, ++ per presented frame
//     — i.e. the index of the frame about to decode) drives the
//     script: 1=play DOGSHIP, 0x81=DROP, 0x85=FLYBY, 0xba/0xc4=
//     EXPLODE1 (FUN_004022b8 play-once), 0xbc=stop DOGSHIP
//     (FUN_0040210c), 0xc2=ENDEXP.
//   * mark==0xd2 -> FUN_0046d614 palette restore + hold clear;
//     0xd1<mark<0xe9 -> FUN_0047b384 ramp step (brighten);
//     mark==0xe9 -> arm 0x54f3b0=0x1e, ramp once, then count down by
//     the frame delta and SKIP decode/present while >0 (~30-tick
//     hold); 0xe8<mark<0x105 -> ramp step (the down side).
//   * FUN_00414158 decode -> copy decode buffer -> 0x541650
//     framebuffer -> FUN_0046c86c present -> FUN_0042fb68 limiter.
//   * FUN_004140f4's nonzero return = the stage-complete edge (stream
//     exhausted) -> return kDone; the caller then runs the
//     FUN_0047b674 MVE boundary + FUN_0041d85c frontend.
//
// FUN_0047b384's internals are NOT captured — the ramp shape here
// (white blend 0..1 over marks 0xd2..0xe8, hold at 0xe9, 1..0 over
// 0xe9..0x104) is HYPOTHESIS chosen to match the documented
// "brighten ramp / ~30-tick hold / ramp down" description.

#ifndef MDK_CORE_ENDING_CINEMATIC_H
#define MDK_CORE_ENDING_CINEMATIC_H

#include "core/flic_decoder.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdk {

class DataRoot;

// The host-facing stage states. kMveBoundary is the FUN_0047b674
// seam — the FLIC stage finished; the Interplay MVE stage is the
// host's (or a later port's) responsibility before the frontend.
enum class EndingStage {
  kLoading,      // FUN_0047b0fc stream-load in progress
  kPlaying,      // FUN_0047b3f4 frame pump
  kHolding,      // mark 0xe9's 30-tick hold (no decode this tick)
  kMveBoundary,  // FLIC done — FUN_0047b674's MDKBZK.MVE slot
  kDone,         // FUN_0041d85c — exit to frontend
};

// One host-facing event (the FINISH.BNI plays the original fires at
// marks; paletteDirty flags the COLOR-chunk palette upload).
struct EndingEvent {
  enum Kind {
    kPlayOnce,      // FUN_004022b8 — one-shot play
    kStop,          // FUN_0040210c — stop
    kPaletteDirty,  // the FLIC COLOR chunk ran this frame
  };
  Kind kind;
  const char* name;   // FINISH.BNI record name for sounds
};

class EndingCinematic {
public:
  // FUN_0047b0fc — read+open MDKEND.FLC. `flicBytes` must outlive
  // the object (the decoder aliases it). `progress` 0..1 tracks the
  // simulated 350-px progress-bar advance for the host.
  bool open(std::span<const std::byte> flicBytes, std::string* detail);

  // FUN_0047b3f4 — one pump call per limiter tick. `tickDelta` is
  // the 0x49b6e8 frame delta in TICK units (OBSERVED: it reads 1 per
  // limiter window — the 0xe9 hold counter therefore counts 30
  // ticks, ~1 s at the file's 33 ms cadence).
  EndingStage step(double tickDelta);

  int mark() const { return mark_; }          // 0x54f3b4
  double rampT() const;                        // 0..1 white blend
  bool paletteDirty() const { return dec_.paletteDirty(); }
  const FlicDecoder& decoder() const { return dec_; }
  std::span<const std::byte> pixels() const {
    return dec_.pixels();
  }
  // The effective presented palette — file palette white-blended by
  // rampT() (HYPOTHESIS ramp shape, see header).
  std::array<std::uint8_t, 768> effectivePalette() const;

  std::vector<EndingEvent> drainEvents();

private:
  FlicDecoder dec_;
  std::vector<EndingEvent> events_;
  int mark_ = 0;             // 0x54f3b4
  double hold_ = 0.0;        // 0x54f3b0 — the 0x1e-unit hold counter
  bool flcDone_ = false;
};

}  // namespace mdk

#endif  // MDK_CORE_ENDING_CINEMATIC_H
