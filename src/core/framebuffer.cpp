#include "core/framebuffer.h"

#include <algorithm>
#include <cassert>

namespace mdk {

IndexedFramebuffer::IndexedFramebuffer(int width, int height)
    : width_(width), height_(height),
      pixels_(static_cast<std::size_t>(width) * height, 0) {
  assert(width > 0 && height > 0);
}

void IndexedFramebuffer::clear(std::uint8_t index) {
  std::fill(pixels_.begin(), pixels_.end(), index);
}

void IndexedFramebuffer::put(int x, int y, std::uint8_t index) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) {
    return;
  }
  pixels_[static_cast<std::size_t>(y) * stride() + x] = index;
}

std::uint8_t IndexedFramebuffer::at(int x, int y) const {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) {
    return 0;
  }
  return pixels_[static_cast<std::size_t>(y) * stride() + x];
}

void expandToBGRA(const IndexedFramebuffer& fb, const Palette& palette,
                  std::uint8_t* out) {
  const std::size_t n = fb.pixelCount();
  for (std::size_t i = 0; i < n; ++i) {
    const Palette::Color c = palette.get(fb.pixels()[i]);
    out[i * 4 + 0] = c.b;
    out[i * 4 + 1] = c.g;
    out[i * 4 + 2] = c.r;
    out[i * 4 + 3] = c.a;
  }
}

} // namespace mdk
