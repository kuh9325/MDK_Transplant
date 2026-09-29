#include "core/thmb_capture.h"

#include "core/framebuffer.h"

#include <algorithm>
#include <cstring>

namespace mdk {

void captureThumbnail(const IndexedFramebuffer& fb,
                      std::span<const std::uint8_t> palette768,
                      std::uint8_t* out) {
  // Palette snapshot first (FUN_0046d614 writes record[0..767]).
  const std::size_t n =
      std::min<std::size_t>(palette768.size(),
                            compat::kPaletteEntries * 3);
  std::memcpy(out, palette768.data(), n);
  if (n < compat::kPaletteEntries * 3) {
    std::memset(out + n, 0, compat::kPaletteEntries * 3 - n);
  }

  const std::uint8_t* src = fb.pixels();
  const int w = fb.width(), h = fb.height();
  std::uint8_t* dst = out + compat::kPaletteEntries * 3;
  for (int r = 0; r < kThmbHeight; ++r) {
    const int sy = r * kThmbSamplePitch;
    for (int c = 0; c < kThmbWidth; ++c) {
      const int sx = kThmbSrcOffsetX + c * kThmbSamplePitch;
      dst[r * kThmbWidth + c] =
          (sy < h && sx < w) ? src[sy * w + sx] : 0;
    }
  }
}

}  // namespace mdk
