// Decoded original indexed visual — the Phase 4A seam between a
// resource payload decoder and the indexed framebuffer.
//
// PROJECT DECISIONS:
//   * platform-neutral: no SDL, no Metal, no DataRoot ownership —
//     a decoder hands this to whoever presents it;
//   * pixels stay INDEXED: the originals are 8bpp and palette
//     resolution happens at present time through core/framebuffer.h;
//   * the palette rides with the image only when the resource embeds
//     one (hasPalette). Resources whose colors come from a separate
//     palette record decode with hasPalette == false and must be
//     paired by their consumer — never by invented colors;
//   * no proprietary bytes are compiled in: everything here is
//     produced at runtime from user-supplied files.
//
#ifndef MDK_CORE_INDEXED_IMAGE_H
#define MDK_CORE_INDEXED_IMAGE_H

#include "core/compat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mdk {

class IndexedFramebuffer;
class Palette;

// One decoded 8-bit indexed image.
struct IndexedImage {
  struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
  };

  int width = 0;
  int height = 0;
  int stride = 0;  // stored bytes per row (== width for every
                   // payload layout decoded so far)

  // Row-major, top-down, stride*height index bytes.
  std::vector<std::uint8_t> pixels;

  bool hasPalette = false;
  std::array<Rgb, compat::kPaletteEntries> palette{};
};

// Blit an indexed image into the work framebuffer and, when the image
// carries a palette, into the runtime palette (alpha forced opaque).
//
// Placement (NATIVE PORT DECISION — the original consumers blit
// verbatim into the work buffer; presentation placement is the
// presenter's concern, not the resource's):
//   * image fits: centered at 1:1 logical pixels, buffer untouched
//     outside the image rectangle;
//   * image larger than the buffer: uniform nearest-neighbor fit
//     (aspect-preserving), centered — evidence-neutral, no smoothing.
void blitIndexedImage(const IndexedImage& img, IndexedFramebuffer& fb,
                      Palette& palette);

// FNV-1a 64 over bytes — deterministic content checksum for
// logs/tests (not a security primitive).
std::uint64_t fnv1a64(std::span<const std::byte> bytes,
                      std::uint64_t h = 0xcbf29ce484222325ull);

// Digest over the decoded image (pixels, then palette when present).
std::uint64_t imageDigest(const IndexedImage& img);

} // namespace mdk

#endif // MDK_CORE_INDEXED_IMAGE_H
