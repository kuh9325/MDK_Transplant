#include "core/indexed_image.h"

#include "core/framebuffer.h"

namespace mdk {

void blitIndexedImage(const IndexedImage& img, IndexedFramebuffer& fb,
                      Palette& palette) {
  // Hardening: decoders guarantee pixels == stride*height, but a
  // hand-assembled image must not read out of bounds.
  if (img.width <= 0 || img.height <= 0 || img.stride < img.width ||
      img.pixels.size() <
          static_cast<std::size_t>(img.stride) * img.height) {
    return;
  }
  if (img.hasPalette) {
    for (int i = 0; i < Palette::size(); ++i) {
      const IndexedImage::Rgb c = img.palette[i];
      palette.set(i, {c.r, c.g, c.b, 255});
    }
  }

  if (img.width <= fb.width() && img.height <= fb.height()) {
    const int ox = (fb.width() - img.width) / 2;
    const int oy = (fb.height() - img.height) / 2;
    for (int y = 0; y < img.height; ++y) {
      for (int x = 0; x < img.width; ++x) {
        fb.put(ox + x, oy + y, img.pixels[y * img.stride + x]);
      }
    }
    return;
  }

  // Larger than the work surface: uniform nearest-neighbor downscale,
  // centered. Integer math only — the tighter axis is the one with the
  // smaller ratio: width-bound iff fbW*imgH <= fbH*imgW.
  const bool widthBound =
      static_cast<std::int64_t>(fb.width()) * img.height <=
      static_cast<std::int64_t>(fb.height()) * img.width;
  const int dw = widthBound
                     ? fb.width()
                     : static_cast<int>(static_cast<std::int64_t>(img.width) *
                                        fb.height() / img.height);
  const int dh = widthBound
                     ? static_cast<int>(static_cast<std::int64_t>(img.height) *
                                        fb.width() / img.width)
                     : fb.height();
  const int ox = (fb.width() - dw) / 2;
  const int oy = (fb.height() - dh) / 2;
  for (int y = 0; y < dh; ++y) {
    const int sy = static_cast<int>(static_cast<std::int64_t>(y) *
                                    img.height / dh);
    for (int x = 0; x < dw; ++x) {
      const int sx = static_cast<int>(static_cast<std::int64_t>(x) *
                                      img.width / dw);
      fb.put(ox + x, oy + y, img.pixels[sy * img.stride + sx]);
    }
  }
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes, std::uint64_t h) {
  for (const std::byte b : bytes) {
    h ^= static_cast<std::uint8_t>(b);
    h *= 0x100000001b3ull;
  }
  return h;
}

std::uint64_t imageDigest(const IndexedImage& img) {
  std::uint64_t h = fnv1a64(std::as_bytes(std::span(img.pixels)));
  if (img.hasPalette) {
    h = fnv1a64(std::as_bytes(std::span(img.palette)), h);
  }
  return h;
}

} // namespace mdk
