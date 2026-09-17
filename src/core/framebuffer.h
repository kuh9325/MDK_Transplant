// Indexed software framebuffer + palette.
//
// ORIGINAL ENGINE OBSERVATION: the original software path renders into an
// ~600x360 8bpp indexed working buffer (DAT_00541650) which the platform
// present path then copies to the display surface (Phase 2B). Indexed +
// 256-entry palette matches the observed 8bpp display mode.
//
// NATIVE PORT PROJECT DECISION: one indexed buffer + one 256-entry RGBA
// palette, expanded on CPU to BGRA8 at present time. The original rasterizer
// is NOT reproduced here; this is only the storage/presentation boundary.

#ifndef MDK_CORE_FRAMEBUFFER_H
#define MDK_CORE_FRAMEBUFFER_H

#include "core/compat.h"

#include <array>
#include <cstdint>
#include <vector>

namespace mdk {

class Palette {
public:
  struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
  };

  void set(int index, Color c) { entries_.at(index) = c; }
  Color get(int index) const { return entries_.at(index); }

  static constexpr int size() { return compat::kPaletteEntries; }

private:
  std::array<Color, compat::kPaletteEntries> entries_{};
};

class IndexedFramebuffer {
public:
  IndexedFramebuffer(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }
  std::size_t pixelCount() const { return pixels_.size(); }

  std::uint8_t* pixels() { return pixels_.data(); }
  const std::uint8_t* pixels() const { return pixels_.data(); }
  std::size_t stride() const { return static_cast<std::size_t>(width_); }

  void clear(std::uint8_t index);
  void put(int x, int y, std::uint8_t index);
  std::uint8_t at(int x, int y) const;

private:
  int width_;
  int height_;
  std::vector<std::uint8_t> pixels_;
};

// Expand indexed pixels through the palette into packed BGRA8
// (little-endian byte order B,G,R,A — matches MTLPixelFormatBGRA8Unorm).
// out must have room for fb.pixelCount() * 4 bytes.
void expandToBGRA(const IndexedFramebuffer& fb, const Palette& palette,
                  std::uint8_t* out);

} // namespace mdk

#endif // MDK_CORE_FRAMEBUFFER_H
