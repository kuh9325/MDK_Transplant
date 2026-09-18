// Phase 4H — shared front-end palette staging primitive.
//
// EVIDENCE (instruction-level, original binary):
//
//   FUN_0046d208 — THE palette upload: every screen's palette bind
//   goes through it (SYS_PAL at options entry FUN_00420cf0, the
//   slut compose at display entry FUN_0041d020 via FUN_00413b40,
//   the dlut/svlut restores at FUN_0041d144/FUN_00420d68). It copies
//   the source RGB triplets into the BGRX staging table
//   (DAT_0054d7b8) and — when DAT_0054147e ("Brightness") is nonzero —
//   applies a per-channel +16*level lift clamped to 255 into the
//   staging buffer only (0x46d2c3..0x46d344, OBSERVED):
//
//       stage_ch = min(src_ch + (brightness << 4), 0xff)
//
//   The raw source palette is never modified, so repeated uploads
//   cannot compound the lift.
//
//   In the port the indexed framebuffer's palette is the observable
//   upload result, so each screen renderer applies the same lift at
//   bind time — including index 0 (the original lifts it too: a
//   nonzero brightness grays the "black" clear on every screen).
//
#ifndef MDK_CORE_FRONTEND_PALETTE_H
#define MDK_CORE_FRONTEND_PALETTE_H

#include "core/framebuffer.h"

#include <algorithm>

namespace mdk {

// FUN_0046d208 staging semantics (OBSERVED): per-channel
// min(src + brightness*16, 255) applied to every bound entry.
inline void applyFrontendBrightness(Palette& palette, int brightness) {
  const int lift = brightness * 16;
  if (lift == 0) {
    return;
  }
  for (int i = 0; i < Palette::size(); ++i) {
    Palette::Color c = palette.get(i);
    c.r = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(c.r) + lift, 0, 255));
    c.g = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(c.g) + lift, 0, 255));
    c.b = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(c.b) + lift, 0, 255));
    palette.set(i, c);
  }
}

} // namespace mdk

#endif // MDK_CORE_FRONTEND_PALETTE_H
