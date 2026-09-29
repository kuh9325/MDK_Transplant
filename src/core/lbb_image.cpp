#include "core/lbb_image.h"

#include "core/data_root.h"

#include <cstdio>
#include <string>

namespace mdk {

std::optional<IndexedImage> decodeLbbImage(
    std::span<const std::uint8_t> bytes) {
  constexpr std::size_t kPal = compat::kPaletteEntries * 3;  // 768
  if (bytes.size() < kPal + 4) return std::nullopt;
  const int w = bytes[kPal] | (bytes[kPal + 1] << 8);
  const int h = bytes[kPal + 2] | (bytes[kPal + 3] << 8);
  if (w <= 0 || h <= 0 || w > 600 || h > 360) return std::nullopt;
  const std::size_t px = static_cast<std::size_t>(w) * h;
  if (bytes.size() != kPal + 4 + px) return std::nullopt;

  IndexedImage img;
  img.width = w;
  img.height = h;
  img.stride = w;
  img.pixels.assign(bytes.begin() + (kPal + 4), bytes.end());
  img.hasPalette = true;
  for (int i = 0; i < compat::kPaletteEntries; ++i) {
    img.palette[i].r = bytes[i * 3 + 0];
    img.palette[i].g = bytes[i * 3 + 1];
    img.palette[i].b = bytes[i * 3 + 2];
  }
  return img;
}

std::optional<IndexedImage> loadSaveListLbb(const DataRoot& root,
                                            int levelId) {
  if (levelId < 0 || levelId >= kSaveListLbbCount) return std::nullopt;
  char name[24];
  std::snprintf(name, sizeof(name), "MISC/LOAD_%d.LBB",
                kSaveListLbbLevelIds[levelId]);
  const auto bytes = root.readFile(name, 768 + 4 + 600 * 360, nullptr);
  if (!bytes.has_value()) return std::nullopt;
  return decodeLbbImage(
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(bytes->data()),
          bytes->size()));
}

}  // namespace mdk
